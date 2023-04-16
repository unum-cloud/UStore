/**
 * @file engine_leveldb.cpp
 * @author Ashot Vardanian
 *
 * @brief Embedded Persistent Key-Value Store on top of @b LevelDB.
 * Has no support for collections, transactions or any non-CRUD jobs.
 */
#include <mutex>
#include <fstream>

#include <leveldb/db.h>
#include <leveldb/comparator.h>
#include <leveldb/write_batch.h>
#include <leveldb/cache.h> // `NewLRUCache`
#include <nlohmann/json.hpp>

#include "ustore/db.h"
#include "ustore/cpp/ranges_args.hpp"   // `places_arg_t`
#include "helpers/linked_array.hpp"  // `uninitialized_array_gt`
#include "helpers/full_scan.hpp"     // `reservoir_sample_iterator`
#include "helpers/config_loader.hpp" // `config_loader_t`

using namespace unum::ustore;
using namespace unum;

namespace stdfs = std::filesystem;
using json_t = nlohmann::json;

/*********************************************************/
/*****************   Structures & Consts  ****************/
/*********************************************************/

ustore_collection_t const ustore_collection_main_k = 0;
ustore_length_t const ustore_length_missing_k = std::numeric_limits<ustore_length_t>::max();
ustore_key_t const ustore_key_unknown_k = std::numeric_limits<ustore_key_t>::max();
bool const ustore_supports_transactions_k = false;
bool const ustore_supports_named_collections_k = false;
bool const ustore_supports_snapshots_k = true;

using level_native_t = leveldb::DB;
using level_status_t = leveldb::Status;
using level_options_t = leveldb::Options;
using level_iter_uptr_t = std::unique_ptr<leveldb::Iterator>;

struct key_comparator_t final : public leveldb::Comparator {

    inline int Compare(leveldb::Slice const& a, leveldb::Slice const& b) const override {
        auto ai = *reinterpret_cast<ustore_key_t const*>(a.data());
        auto bi = *reinterpret_cast<ustore_key_t const*>(b.data());
        if (ai == bi)
            return 0;
        return ai < bi ? -1 : 1;
    }

    char const* Name() const override { return "Integral"; }

    void FindShortestSeparator(std::string*, leveldb::Slice const&) const override {}

    void FindShortSuccessor(std::string* key) const override {
        auto& int_key = *reinterpret_cast<ustore_key_t*>(key->data());
        ++int_key;
    }
};

static key_comparator_t const key_comparator_k = {};

struct level_snapshot_t {
    leveldb::Snapshot const* snapshot = nullptr;
};

struct level_db_t {
    std::unordered_map<ustore_size_t, level_snapshot_t*> snapshots;
    std::unique_ptr<level_native_t> native;
    std::mutex mutex;
};

/*********************************************************/
/*****************	 C++ Implementation	  ****************/
/*********************************************************/

inline leveldb::Slice to_slice(ustore_key_t const& key) noexcept {
    return {reinterpret_cast<char const*>(&key), sizeof(ustore_key_t)};
}

inline leveldb::Slice to_slice(value_view_t value) noexcept {
    return {reinterpret_cast<const char*>(value.begin()), value.size()};
}

inline std::unique_ptr<std::string> make_value(ustore_error_t* c_error) noexcept {
    std::unique_ptr<std::string> value_uptr;
    try {
        value_uptr = std::make_unique<std::string>();
    }
    catch (...) {
        *c_error = "Fail to allocate value";
    }
    return value_uptr;
}

bool export_error(level_status_t const& status, ustore_error_t* c_error) {
    if (status.ok())
        return false;

    if (status.IsCorruption())
        *c_error = "Failure: DB Corruption";
    else if (status.IsIOError())
        *c_error = "Failure: IO  Error";
    else if (status.IsInvalidArgument())
        *c_error = "Failure: Invalid Argument";
    else
        *c_error = "Failure";
    return true;
}

void ustore_database_init(ustore_database_init_t* c_ptr) {

    ustore_database_init_t& c = *c_ptr;
    try {
        level_options_t options;
        options.comparator = &key_comparator_k;
        options.compression = leveldb::kNoCompression;
        options.create_if_missing = true;

        return_error_if_m(c.config, c.error, args_wrong_k, "Null config specified");
        // Load config
        config_t config;
        auto st = config_loader_t::load_from_json_string(c.config, config);
        return_error_if_m(st, c.error, args_wrong_k, st.message());

        // Root path
        stdfs::path root = config.directory;
        stdfs::file_status root_status = stdfs::status(root);
        return_error_if_m(root_status.type() == stdfs::file_type::directory,
                          c.error,
                          args_wrong_k,
                          "Root isn't a directory");

        // Storage paths
        return_error_if_m(config.data_directories.empty(), c.error, args_wrong_k, "Multi-disk not supported");

        // Engine config
        return_error_if_m(config.engine.config_url.empty(), c.error, args_wrong_k, "Doesn't support URL configs");

        auto fill_options = [](json_t const& js, level_options_t& options) {
            if (js.contains("write_buffer_size"))
                options.write_buffer_size = js["write_buffer_size"];
            if (js.contains("max_file_size"))
                options.max_file_size = js["max_file_size"];
            if (js.contains("max_open_files"))
                options.max_open_files = js["max_open_files"];
            if (js.contains("cache_size"))
                options.block_cache = leveldb::NewLRUCache(js["cache_size"]);
            if (js.contains("create_if_missing"))
                options.create_if_missing = js["create_if_missing"];
            if (js.contains("error_if_exists"))
                options.error_if_exists = js["error_if_exists"];
            if (js.contains("paranoid_checks"))
                options.paranoid_checks = js["paranoid_checks"];
            if (js.contains("compression"))
                if (js["compression"] == "kSnappyCompression" || js["compression"] == "snappy")
                    options.compression = leveldb::kSnappyCompression;
        };

        // Load from file
        if (!config.engine.config_file_path.empty()) {
            std::ifstream ifs(config.engine.config_file_path);
            return_error_if_m(ifs, c.error, args_wrong_k, "Config file not found");
            auto js = json_t::parse(ifs);
            fill_options(js, options);
        }
        // Override with nested
        if (!config.engine.config.empty())
            fill_options(config.engine.config, options);

        level_db_t* db_ptr = new level_db_t;
        level_native_t* native_db = nullptr;
        level_status_t status = leveldb::DB::Open(options, root, &native_db);
        if (!status.ok()) {
            *c.error = "Couldn't open LevelDB";
            return;
        }
        db_ptr->native = std::unique_ptr<level_native_t>(native_db);
        *c.db = db_ptr;
    }
    catch (json_t::type_error const&) {
        *c.error = "Unsupported type in LevelDB configuration key";
    }
    catch (...) {
        *c.error = "Open Failure";
    }
}

void ustore_snapshot_list(ustore_snapshot_list_t* c_ptr) {
    ustore_snapshot_list_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    return_error_if_m(c.count && c.ids, c.error, args_combo_k, "Need outputs!");

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    level_db_t& db = *reinterpret_cast<level_db_t*>(c.db);
    std::lock_guard<std::mutex> locker(db.mutex);
    std::size_t snapshots_count = db.snapshots.size();
    *c.count = static_cast<ustore_size_t>(snapshots_count);

    // For every snapshot we also need to export IDs
    auto ids = arena.alloc_or_dummy(snapshots_count, c.error, c.ids);
    return_if_error_m(c.error);

    std::size_t i = 0;
    for (const auto& [id, _] : db.snapshots)
        ids[i++] = id;
}

void ustore_snapshot_create(ustore_snapshot_create_t* c_ptr) {
    ustore_snapshot_create_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");

    level_db_t& db = *reinterpret_cast<level_db_t*>(c.db);
    std::lock_guard<std::mutex> locker(db.mutex);
    auto it = db.snapshots.find(*c.id);
    if (it != db.snapshots.end())
        return_error_if_m(it->second, c.error, args_wrong_k, "Such snapshot already exists!");

    level_snapshot_t* level_snapshot = nullptr;
    safe_section("Allocating snapshot handle", c.error, [&] { level_snapshot = new level_snapshot_t(); });
    return_if_error_m(c.error);

    level_snapshot->snapshot = db.native->GetSnapshot();
    if (!level_snapshot->snapshot)
        *c.error = "Couldn't get a snapshot!";

    *c.id = reinterpret_cast<std::size_t>(level_snapshot);
    db.snapshots[*c.id] = level_snapshot;
}

void ustore_snapshot_drop(ustore_snapshot_drop_t* c_ptr) {
    if (!c_ptr)
        return;

    ustore_snapshot_drop_t& c = *c_ptr;
    if (!c.id)
        return;

    level_db_t& db = *reinterpret_cast<level_db_t*>(c.db);
    level_snapshot_t& snap = *reinterpret_cast<level_snapshot_t*>(c.id);
    if (!snap.snapshot)
        return;

    db.native->ReleaseSnapshot(snap.snapshot);
    snap.snapshot = nullptr;

    auto id = reinterpret_cast<std::size_t>(c.id);
    db.mutex.lock();
    db.snapshots.erase(id);
    db.mutex.unlock();
}

void write_one( //
    level_db_t& db,
    places_arg_t const& places,
    contents_arg_t const& contents,
    leveldb::WriteOptions const& options,
    ustore_error_t* c_error) {

    auto place = places[0];
    auto content = contents[0];
    auto key = to_slice(place.key);
    level_status_t status =
        !content ? db.native->Delete(options, key) : db.native->Put(options, key, to_slice(content));
    export_error(status, c_error);
}

void write_many( //
    level_db_t& db,
    places_arg_t const& places,
    contents_arg_t const& contents,
    leveldb::WriteOptions const& options,
    ustore_error_t* c_error) {

    leveldb::WriteBatch batch;
    for (std::size_t i = 0; i != places.size(); ++i) {
        auto place = places[i];
        auto content = contents[i];

        auto key = to_slice(place.key);
        if (!content)
            batch.Delete(key);
        else
            batch.Put(key, to_slice(content));
    }

    level_status_t status = db.native->Write(options, &batch);
    export_error(status, c_error);
}

void ustore_write(ustore_write_t* c_ptr) {

    ustore_write_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");

    level_db_t& db = *reinterpret_cast<level_db_t*>(c.db);
    strided_iterator_gt<ustore_collection_t const> collections {c.collections, c.collections_stride};
    strided_iterator_gt<ustore_key_t const> keys {c.keys, c.keys_stride};
    strided_iterator_gt<ustore_bytes_cptr_t const> vals {c.values, c.values_stride};
    strided_iterator_gt<ustore_length_t const> offs {c.offsets, c.offsets_stride};
    strided_iterator_gt<ustore_length_t const> lens {c.lengths, c.lengths_stride};
    bits_view_t presences {c.presences};

    places_arg_t places {collections, keys, {}, c.tasks_count};
    contents_arg_t contents {presences, offs, lens, vals, c.tasks_count};

    validate_write(c.transaction, places, contents, c.options, c.error);
    return_if_error_m(c.error);

    leveldb::WriteOptions options;
    if (c.options & ustore_option_write_flush_k)
        options.sync = true;

    try {
        auto func = c.tasks_count == 1 ? &write_one : &write_many;
        func(db, places, contents, options, c.error);
    }
    catch (...) {
        *c.error = "Write Failure";
    }
}

template <typename value_enumerator_at>
void read_enumerate( //
    level_db_t& db,
    places_arg_t tasks,
    leveldb::ReadOptions const& options,
    std::string& value,
    value_enumerator_at enumerator,
    ustore_error_t* c_error) {

    for (std::size_t i = 0; i != tasks.size(); ++i) {
        place_t place = tasks[i];
        level_status_t status = db.native->Get(options, to_slice(place.key), &value);
        if (!status.IsNotFound()) {
            if (export_error(status, c_error))
                return;
            auto begin = reinterpret_cast<ustore_bytes_cptr_t>(value.data());
            auto length = static_cast<ustore_length_t>(value.size());
            enumerator(i, value_view_t {begin, length});
        }
        else
            enumerator(i, value_view_t {});
    }
}

void ustore_read(ustore_read_t* c_ptr) {

    ustore_read_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    level_db_t& db = *reinterpret_cast<level_db_t*>(c.db);
    level_snapshot_t& snap = *reinterpret_cast<level_snapshot_t*>(c.snapshot);
    strided_iterator_gt<ustore_key_t const> keys {c.keys, c.keys_stride};
    places_arg_t places {{}, keys, {}, c.tasks_count};

    validate_read(c.transaction, places, c.options, c.error);
    return_if_error_m(c.error);

    // 1. Allocate a tape for all the values to be pulled
    auto offs = arena.alloc_or_dummy(places.count + 1, c.error, c.offsets);
    return_if_error_m(c.error);
    auto lens = arena.alloc_or_dummy(places.count, c.error, c.lengths);
    return_if_error_m(c.error);
    auto presences = arena.alloc_or_dummy(places.count, c.error, c.presences);
    return_if_error_m(c.error);
    bool const needs_export = c.values != nullptr;

    uninitialized_array_gt<byte_t> contents(arena);

    // 2. Pull metadata & data in one run, as reading from disk is expensive
    try {
        leveldb::ReadOptions options;
        if (c.snapshot) {
            auto it = db.snapshots.find(c.snapshot);
            return_error_if_m(it != db.snapshots.end(), c.error, args_wrong_k, "The snapshot does'nt exist!");
            options.snapshot = snap.snapshot;
        }

        std::string value_buffer;
        ustore_length_t progress_in_tape = 0;
        auto data_enumerator = [&](std::size_t i, value_view_t value) {
            presences[i] = bool(value);
            lens[i] = value ? value.size() : ustore_length_missing_k;
            offs[i] = contents.size();
            if (needs_export)
                contents.insert(contents.size(), value.begin(), value.end(), c.error);
        };
        read_enumerate(db, places, options, value_buffer, data_enumerator, c.error);
        offs[places.count] = contents.size();
        if (needs_export)
            *c.values = reinterpret_cast<ustore_bytes_ptr_t>(contents.begin());
    }
    catch (...) {
        *c.error = "Read Failure";
    }
}

void ustore_scan(ustore_scan_t* c_ptr) {

    ustore_scan_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    level_db_t& db = *reinterpret_cast<level_db_t*>(c.db);
    level_snapshot_t& snap = *reinterpret_cast<level_snapshot_t*>(c.snapshot);
    strided_iterator_gt<ustore_key_t const> start_keys {c.start_keys, c.start_keys_stride};
    strided_iterator_gt<ustore_length_t const> limits {c.count_limits, c.count_limits_stride};
    scans_arg_t scans {{}, start_keys, limits, c.tasks_count};

    return_if_error_m(c.error);

    // 1. Allocate a tape for all the values to be fetched
    auto offsets = arena.alloc_or_dummy(scans.count + 1, c.error, c.offsets);
    return_if_error_m(c.error);
    auto counts = arena.alloc_or_dummy(scans.count, c.error, c.counts);
    return_if_error_m(c.error);

    auto total_keys = reduce_n(scans.limits, scans.count, 0ul);
    auto keys_output = *c.keys = arena.alloc<ustore_key_t>(total_keys, c.error).begin();
    return_if_error_m(c.error);

    // 2. Fetch the data
    leveldb::ReadOptions options;
    options.fill_cache = false;
    if (c.snapshot) {
        auto it = db.snapshots.find(c.snapshot);
        return_error_if_m(it != db.snapshots.end(), c.error, args_wrong_k, "The snapshot does'nt exist!");
        options.snapshot = snap.snapshot;
    }

    level_iter_uptr_t it;
    try {
        it = level_iter_uptr_t(db.native->NewIterator(options));
    }
    catch (...) {
        *c.error = "Fail To Create Iterator";
        return;
    }
    for (ustore_size_t i = 0; i != c.tasks_count; ++i) {
        scan_t task = scans[i];
        it->Seek(to_slice(task.min_key));
        offsets[i] = keys_output - *c.keys;

        ustore_size_t j = 0;
        while (it->Valid() && j != task.limit) {
            std::memcpy(keys_output, it->key().data(), sizeof(ustore_key_t));
            ++keys_output;
            ++j;
            it->Next();
        }

        counts[i] = j;
    }

    offsets[scans.size()] = keys_output - *c.keys;
}

void ustore_sample(ustore_sample_t* c_ptr) {

    ustore_sample_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    if (!c.tasks_count)
        return;

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    level_db_t& db = *reinterpret_cast<level_db_t*>(c.db);
    level_snapshot_t& snap = *reinterpret_cast<level_snapshot_t*>(c.snapshot);
    strided_iterator_gt<ustore_length_t const> lens {c.count_limits, c.count_limits_stride};
    sample_args_t samples {{}, lens, c.tasks_count};

    // 1. Allocate a tape for all the values to be fetched
    auto offsets = arena.alloc_or_dummy(samples.count + 1, c.error, c.offsets);
    return_if_error_m(c.error);
    auto counts = arena.alloc_or_dummy(samples.count, c.error, c.counts);
    return_if_error_m(c.error);

    auto total_keys = reduce_n(samples.limits, samples.count, 0ul);
    auto keys_output = *c.keys = arena.alloc<ustore_key_t>(total_keys, c.error).begin();
    return_if_error_m(c.error);

    // 2. Fetch the data
    leveldb::ReadOptions options;
    options.fill_cache = false;
    if (c.snapshot) {
        auto it = db.snapshots.find(c.snapshot);
        return_error_if_m(it != db.snapshots.end(), c.error, args_wrong_k, "The snapshot does'nt exist!");
        options.snapshot = snap.snapshot;
    }

    for (std::size_t task_idx = 0; task_idx != samples.count; ++task_idx) {
        sample_arg_t task = samples[task_idx];
        offsets[task_idx] = keys_output - *c.keys;

        level_iter_uptr_t it;
        safe_section("Creating a LevelDB iterator", c.error, [&] {
            it = level_iter_uptr_t(db.native->NewIterator(options));
        });
        return_if_error_m(c.error);

        ptr_range_gt<ustore_key_t> sampled_keys(keys_output, task.limit);
        reservoir_sample_iterator(it, sampled_keys, c.error);

        counts[task_idx] = task.limit;
        keys_output += task.limit;
    }
    offsets[samples.count] = keys_output - *c.keys;
}

void ustore_measure(ustore_measure_t* c_ptr) {

    ustore_measure_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    auto min_cardinalities = arena.alloc_or_dummy(c.tasks_count, c.error, c.min_cardinalities);
    auto max_cardinalities = arena.alloc_or_dummy(c.tasks_count, c.error, c.max_cardinalities);
    auto min_value_bytes = arena.alloc_or_dummy(c.tasks_count, c.error, c.min_value_bytes);
    auto max_value_bytes = arena.alloc_or_dummy(c.tasks_count, c.error, c.max_value_bytes);
    auto min_space_usages = arena.alloc_or_dummy(c.tasks_count, c.error, c.min_space_usages);
    auto max_space_usages = arena.alloc_or_dummy(c.tasks_count, c.error, c.max_space_usages);
    return_if_error_m(c.error);

    level_db_t& db = *reinterpret_cast<level_db_t*>(c.db);
    strided_iterator_gt<ustore_key_t const> start_keys {c.start_keys, c.start_keys_stride};
    strided_iterator_gt<ustore_key_t const> end_keys {c.end_keys, c.end_keys_stride};
    uint64_t approximate_size = 0;
    std::optional<std::string> memory_usage;
    level_status_t status;

    for (ustore_size_t i = 0; i != c.tasks_count; ++i) {
        min_cardinalities[i] = static_cast<ustore_size_t>(0);
        max_cardinalities[i] = static_cast<ustore_size_t>(0);
        min_value_bytes[i] = static_cast<ustore_size_t>(0);
        max_value_bytes[i] = static_cast<ustore_size_t>(0);

        ustore_key_t const min_key = start_keys[i];
        ustore_key_t const max_key = end_keys[i];
        leveldb::Range range(to_slice(min_key), to_slice(max_key));
        try {
            db.native->GetApproximateSizes(&range, 1, &approximate_size);
            min_space_usages[i] = approximate_size;

            memory_usage = "0";
            db.native->GetProperty("leveldb.approximate-memory-usage", &memory_usage.value());
            max_space_usages[i] = std::stoi(memory_usage.value());
        }
        catch (...) {
            *c.error = "Property Read Failure";
        }
    }
}

/*********************************************************/
/*****************	Collections Management	****************/
/*********************************************************/

void ustore_collection_create(ustore_collection_create_t* c_ptr) {

    ustore_collection_create_t& c = *c_ptr;
    auto name_len = c.name ? std::strlen(c.name) : 0;
    return_error_if_m(name_len, c.error, args_wrong_k, "Collections not supported by LevelDB!");
}

void ustore_collection_drop(ustore_collection_drop_t* c_ptr) {

    ustore_collection_drop_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    bool invalidate = c.mode == ustore_drop_keys_vals_handle_k;
    return_error_if_m(c.id == ustore_collection_main_k && !invalidate,
                      c.error,
                      args_combo_k,
                      "Collections not supported by LevelDB!");

    level_db_t& db = *reinterpret_cast<level_db_t*>(c.db);

    leveldb::WriteBatch batch;
    auto it = std::unique_ptr<leveldb::Iterator>(db.native->NewIterator(leveldb::ReadOptions()));

    if (c.mode == ustore_drop_keys_vals_k) {
        for (it->SeekToFirst(); it->Valid(); it->Next())
            batch.Delete(it->key());
    }

    else if (c.mode == ustore_drop_vals_k) {
        for (it->SeekToFirst(); it->Valid(); it->Next())
            batch.Put(it->key(), leveldb::Slice());
    }

    leveldb::WriteOptions options;
    options.sync = true;
    level_status_t status = db.native->Write(options, &batch);
    export_error(status, c.error);
}

void ustore_collection_list(ustore_collection_list_t* c_ptr) {

    ustore_collection_list_t& c = *c_ptr;
    *c.count = 0;
    if (c.ids)
        *c.ids = nullptr;
    if (c.offsets)
        *c.offsets = nullptr;
    if (c.names)
        *c.names = nullptr;
}

void ustore_database_control(ustore_database_control_t* c_ptr) {

    ustore_database_control_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");

    if (!c.request && (*c.error = "Request is NULL!"))
        return;

    *c.response = NULL;
    *c.error = "Controls aren't supported in this implementation!";
}

/*********************************************************/
/*****************		Transactions	  ****************/
/*********************************************************/

void ustore_transaction_init(ustore_transaction_init_t* c_ptr) {

    ustore_transaction_init_t& c = *c_ptr;
    *c.error = "Transactions not supported by LevelDB!";
}

void ustore_transaction_commit(ustore_transaction_commit_t* c_ptr) {

    ustore_transaction_commit_t& c = *c_ptr;
    *c.error = "Transactions not supported by LevelDB!";
}

/*********************************************************/
/*****************	  Memory Management   ****************/
/*********************************************************/

void ustore_arena_free(ustore_arena_t c_arena) {
    clear_linked_memory(c_arena);
}

void ustore_transaction_free(ustore_transaction_t) {
}

void ustore_database_free(ustore_database_t c_db) {
    if (!c_db)
        return;
    level_db_t* db = reinterpret_cast<level_db_t*>(c_db);
    delete db;
}

void ustore_error_free(ustore_error_t) {
}