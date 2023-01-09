/**
 * @file engine_leveldb.cpp
 * @author Ashot Vardanian
 *
 * @brief Embedded Persistent Key-Value Store on top of @b LevelDB.
 * Has no support for collections, transactions or any non-CRUD jobs.
 */
#include <fstream>

#include <leveldb/db.h>
#include <leveldb/comparator.h>
#include <leveldb/write_batch.h>
#include <leveldb/cache.h> // `NewLRUCache`
#include <nlohmann/json.hpp>

#include "ukv/db.h"
#include "ukv/cpp/ranges_args.hpp"  // `places_arg_t`
#include "helpers/linked_array.hpp" // `uninitialized_array_gt`
#include "helpers/full_scan.hpp"    // `reservoir_sample_iterator`

using namespace unum::ukv;
using namespace unum;

namespace stdfs = std::filesystem;
using json_t = nlohmann::json;

/*********************************************************/
/*****************   Structures & Consts  ****************/
/*********************************************************/

ukv_collection_t const ukv_collection_main_k = 0;
ukv_length_t const ukv_length_missing_k = std::numeric_limits<ukv_length_t>::max();
ukv_key_t const ukv_key_unknown_k = std::numeric_limits<ukv_key_t>::max();
bool const ukv_supports_transactions_k = false;
bool const ukv_supports_named_collections_k = false;
bool const ukv_supports_snapshots_k = false;

using level_db_t = leveldb::DB;
using level_status_t = leveldb::Status;
using level_options_t = leveldb::Options;
using level_iter_uptr_t = std::unique_ptr<leveldb::Iterator>;

static constexpr char const* config_name_k = "config_leveldb.json";

struct key_comparator_t final : public leveldb::Comparator {

    inline int Compare(leveldb::Slice const& a, leveldb::Slice const& b) const override {
        auto ai = *reinterpret_cast<ukv_key_t const*>(a.data());
        auto bi = *reinterpret_cast<ukv_key_t const*>(b.data());
        if (ai == bi)
            return 0;
        return ai < bi ? -1 : 1;
    }

    char const* Name() const override { return "Integral"; }

    void FindShortestSeparator(std::string*, leveldb::Slice const&) const override {}

    void FindShortSuccessor(std::string* key) const override {
        auto& int_key = *reinterpret_cast<ukv_key_t*>(key->data());
        ++int_key;
    }
};

struct level_txn_t {
    leveldb::DB* db = nullptr;
    leveldb::Snapshot const* snapshot = nullptr;
};

static key_comparator_t const key_comparator_k = {};

/*********************************************************/
/*****************	 C++ Implementation	  ****************/
/*********************************************************/

inline leveldb::Slice to_slice(ukv_key_t const& key) noexcept {
    return {reinterpret_cast<char const*>(&key), sizeof(ukv_key_t)};
}

inline leveldb::Slice to_slice(value_view_t value) noexcept {
    return {reinterpret_cast<const char*>(value.begin()), value.size()};
}

inline std::unique_ptr<std::string> make_value(ukv_error_t* c_error) noexcept {
    std::unique_ptr<std::string> value_uptr;
    try {
        value_uptr = std::make_unique<std::string>();
    }
    catch (...) {
        *c_error = "Fail to allocate value";
    }
    return value_uptr;
}

bool export_error(level_status_t const& status, ukv_error_t* c_error) {
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

void ukv_database_init(ukv_database_init_t* c_ptr) {

    ukv_database_init_t& c = *c_ptr;
    try {
        level_options_t options;
        options.comparator = &key_comparator_k;
        options.compression = leveldb::kNoCompression;
        options.create_if_missing = true;

        // Check if the directory contains a config
        stdfs::path root = c.config;
        stdfs::file_status root_status = stdfs::status(root);
        return_error_if_m(root_status.type() == stdfs::file_type::directory,
                          c.error,
                          args_wrong_k,
                          "Root isn't a directory");
        stdfs::path config_path = stdfs::path(root) / config_name_k;
        stdfs::file_status config_status = stdfs::status(config_path);
        if (config_status.type() == stdfs::file_type::not_found) {
            log_warning_m(
                "Configuration file is missing under the path %s. "
                "Default will be used\n",
                config_path.c_str());
        }
        else {
            std::ifstream ifs(config_path.c_str());
            json_t js = json_t::parse(ifs);
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
        }

        level_db_t* db_ptr = nullptr;
        level_status_t status = level_db_t::Open(options, c.config, &db_ptr);
        if (!status.ok()) {
            *c.error = "Couldn't open LevelDB";
            return;
        }
        *c.db = db_ptr;
    }
    catch (json_t::type_error const&) {
        *c.error = "Unsupported type in LevelDB configuration key";
    }
    catch (...) {
        *c.error = "Open Failure";
    }
}

void write_one( //
    level_db_t& db,
    places_arg_t const& places,
    contents_arg_t const& contents,
    leveldb::WriteOptions const& options,
    ukv_error_t* c_error) {

    auto place = places[0];
    auto content = contents[0];
    auto key = to_slice(place.key);
    level_status_t status = !content ? db.Delete(options, key) : db.Put(options, key, to_slice(content));
    export_error(status, c_error);
}

void write_many( //
    level_db_t& db,
    places_arg_t const& places,
    contents_arg_t const& contents,
    leveldb::WriteOptions const& options,
    ukv_error_t* c_error) {

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

    level_status_t status = db.Write(options, &batch);
    export_error(status, c_error);
}

void ukv_write(ukv_write_t* c_ptr) {

    ukv_write_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");

    level_db_t& db = *reinterpret_cast<level_db_t*>(c.db);
    strided_iterator_gt<ukv_collection_t const> collections {c.collections, c.collections_stride};
    strided_iterator_gt<ukv_key_t const> keys {c.keys, c.keys_stride};
    strided_iterator_gt<ukv_bytes_cptr_t const> vals {c.values, c.values_stride};
    strided_iterator_gt<ukv_length_t const> offs {c.offsets, c.offsets_stride};
    strided_iterator_gt<ukv_length_t const> lens {c.lengths, c.lengths_stride};
    bits_view_t presences {c.presences};

    places_arg_t places {collections, keys, {}, c.tasks_count};
    contents_arg_t contents {presences, offs, lens, vals, c.tasks_count};

    validate_write(c.transaction, places, contents, c.options, c.error);
    return_if_error_m(c.error);

    leveldb::WriteOptions options;
    if (c.options & ukv_option_write_flush_k)
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
    ukv_error_t* c_error) {

    for (std::size_t i = 0; i != tasks.size(); ++i) {
        place_t place = tasks[i];
        level_status_t status = db.Get(options, to_slice(place.key), &value);
        if (!status.IsNotFound()) {
            if (export_error(status, c_error))
                return;
            auto begin = reinterpret_cast<ukv_bytes_cptr_t>(value.data());
            auto length = static_cast<ukv_length_t>(value.size());
            enumerator(i, value_view_t {begin, length});
        }
        else
            enumerator(i, value_view_t {});
    }
}

void ukv_read(ukv_read_t* c_ptr) {

    ukv_read_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    level_db_t& db = *reinterpret_cast<level_db_t*>(c.db);
    level_txn_t& txn = *reinterpret_cast<level_txn_t*>(c.transaction);
    strided_iterator_gt<ukv_key_t const> keys {c.keys, c.keys_stride};
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
        if (c.transaction)
            options.snapshot = txn.snapshot;

        std::string value_buffer;
        ukv_length_t progress_in_tape = 0;
        auto data_enumerator = [&](std::size_t i, value_view_t value) {
            presences[i] = bool(value);
            lens[i] = value ? value.size() : ukv_length_missing_k;
            offs[i] = contents.size();
            if (needs_export)
                contents.insert(contents.size(), value.begin(), value.end(), c.error);
        };
        read_enumerate(db, places, options, value_buffer, data_enumerator, c.error);
        offs[places.count] = contents.size();
        if (needs_export)
            *c.values = reinterpret_cast<ukv_bytes_ptr_t>(contents.begin());
    }
    catch (...) {
        *c.error = "Read Failure";
    }
}

void ukv_scan(ukv_scan_t* c_ptr) {

    ukv_scan_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    level_db_t& db = *reinterpret_cast<level_db_t*>(c.db);
    level_txn_t& txn = *reinterpret_cast<level_txn_t*>(c.transaction);
    strided_iterator_gt<ukv_key_t const> start_keys {c.start_keys, c.start_keys_stride};
    strided_iterator_gt<ukv_length_t const> limits {c.count_limits, c.count_limits_stride};
    scans_arg_t scans {{}, start_keys, limits, c.tasks_count};

    validate_scan(c.transaction, scans, c.options, c.error);
    return_if_error_m(c.error);

    // 1. Allocate a tape for all the values to be fetched
    auto offsets = arena.alloc_or_dummy(scans.count + 1, c.error, c.offsets);
    return_if_error_m(c.error);
    auto counts = arena.alloc_or_dummy(scans.count, c.error, c.counts);
    return_if_error_m(c.error);

    auto total_keys = reduce_n(scans.limits, scans.count, 0ul);
    auto keys_output = *c.keys = arena.alloc<ukv_key_t>(total_keys, c.error).begin();
    return_if_error_m(c.error);

    // 2. Fetch the data
    leveldb::ReadOptions options;
    options.fill_cache = false;

    if (c.transaction)
        options.snapshot = txn.snapshot;

    level_iter_uptr_t it;
    try {
        it = level_iter_uptr_t(db.NewIterator(options));
    }
    catch (...) {
        *c.error = "Fail To Create Iterator";
        return;
    }
    for (ukv_size_t i = 0; i != c.tasks_count; ++i) {
        scan_t task = scans[i];
        it->Seek(to_slice(task.min_key));
        offsets[i] = keys_output - *c.keys;

        ukv_size_t j = 0;
        while (it->Valid() && j != task.limit) {
            std::memcpy(keys_output, it->key().data(), sizeof(ukv_key_t));
            ++keys_output;
            ++j;
            it->Next();
        }

        counts[i] = j;
    }

    offsets[scans.size()] = keys_output - *c.keys;
}

void ukv_sample(ukv_sample_t* c_ptr) {

    ukv_sample_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    if (!c.tasks_count)
        return;

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    level_db_t& db = *reinterpret_cast<level_db_t*>(c.db);
    level_txn_t& txn = *reinterpret_cast<level_txn_t*>(c.transaction);
    strided_iterator_gt<ukv_length_t const> lens {c.count_limits, c.count_limits_stride};
    sample_args_t samples {{}, lens, c.tasks_count};

    // 1. Allocate a tape for all the values to be fetched
    auto offsets = arena.alloc_or_dummy(samples.count + 1, c.error, c.offsets);
    return_if_error_m(c.error);
    auto counts = arena.alloc_or_dummy(samples.count, c.error, c.counts);
    return_if_error_m(c.error);

    auto total_keys = reduce_n(samples.limits, samples.count, 0ul);
    auto keys_output = *c.keys = arena.alloc<ukv_key_t>(total_keys, c.error).begin();
    return_if_error_m(c.error);

    // 2. Fetch the data
    leveldb::ReadOptions options;
    options.fill_cache = false;

    if (c.transaction)
        options.snapshot = txn.snapshot;

    for (std::size_t task_idx = 0; task_idx != samples.count; ++task_idx) {
        sample_arg_t task = samples[task_idx];
        offsets[task_idx] = keys_output - *c.keys;

        level_iter_uptr_t it;
        safe_section("Creating a LevelDB iterator", c.error, [&] { it = level_iter_uptr_t(db.NewIterator(options)); });
        return_if_error_m(c.error);

        ptr_range_gt<ukv_key_t> sampled_keys(keys_output, task.limit);
        reservoir_sample_iterator(it, sampled_keys, c.error);

        counts[task_idx] = task.limit;
        keys_output += task.limit;
    }
    offsets[samples.count] = keys_output - *c.keys;
}

void ukv_measure(ukv_measure_t* c_ptr) {

    ukv_measure_t& c = *c_ptr;
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
    strided_iterator_gt<ukv_key_t const> start_keys {c.start_keys, c.start_keys_stride};
    strided_iterator_gt<ukv_key_t const> end_keys {c.end_keys, c.end_keys_stride};
    uint64_t approximate_size = 0;
    std::optional<std::string> memory_usage;
    level_status_t status;

    for (ukv_size_t i = 0; i != c.tasks_count; ++i) {
        min_cardinalities[i] = static_cast<ukv_size_t>(0);
        max_cardinalities[i] = static_cast<ukv_size_t>(0);
        min_value_bytes[i] = static_cast<ukv_size_t>(0);
        max_value_bytes[i] = static_cast<ukv_size_t>(0);

        ukv_key_t const min_key = start_keys[i];
        ukv_key_t const max_key = end_keys[i];
        leveldb::Range range(to_slice(min_key), to_slice(max_key));
        try {
            db.GetApproximateSizes(&range, 1, &approximate_size);
            min_space_usages[i] = approximate_size;

            memory_usage = "0";
            db.GetProperty("leveldb.approximate-memory-usage", &memory_usage.value());
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

void ukv_collection_create(ukv_collection_create_t* c_ptr) {

    ukv_collection_create_t& c = *c_ptr;
    auto name_len = c.name ? std::strlen(c.name) : 0;
    return_error_if_m(name_len, c.error, args_wrong_k, "Collections not supported by LevelDB!");
}

void ukv_collection_drop(ukv_collection_drop_t* c_ptr) {

    ukv_collection_drop_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    bool invalidate = c.mode == ukv_drop_keys_vals_handle_k;
    return_error_if_m(c.id == ukv_collection_main_k && !invalidate,
                      c.error,
                      args_combo_k,
                      "Collections not supported by LevelDB!");

    level_db_t& db = *reinterpret_cast<level_db_t*>(c.db);

    leveldb::WriteBatch batch;
    auto it = std::unique_ptr<leveldb::Iterator>(db.NewIterator(leveldb::ReadOptions()));

    if (c.mode == ukv_drop_keys_vals_k) {
        for (it->SeekToFirst(); it->Valid(); it->Next())
            batch.Delete(it->key());
    }

    else if (c.mode == ukv_drop_vals_k) {
        for (it->SeekToFirst(); it->Valid(); it->Next())
            batch.Put(it->key(), leveldb::Slice());
    }

    leveldb::WriteOptions options;
    options.sync = true;
    level_status_t status = db.Write(options, &batch);
    export_error(status, c.error);
}

void ukv_collection_list(ukv_collection_list_t* c_ptr) {

    ukv_collection_list_t& c = *c_ptr;
    *c.count = 0;
    if (c.ids)
        *c.ids = nullptr;
    if (c.offsets)
        *c.offsets = nullptr;
    if (c.names)
        *c.names = nullptr;
}

void ukv_database_control(ukv_database_control_t* c_ptr) {

    ukv_database_control_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");

    if (!c.request && (*c.error = "Request is NULL!"))
        return;

    *c.response = NULL;
    *c.error = "Controls aren't supported in this implementation!";
}

/*********************************************************/
/*****************		Transactions	  ****************/
/*********************************************************/

void ukv_transaction_init(ukv_transaction_init_t* c_ptr) {

    ukv_transaction_init_t& c = *c_ptr;
    *c.error = "Transactions not supported by LevelDB!";

#if 0 // TODO: Persistent Snapshots will be receiving a separate interface.
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    validate_transaction_begin(c.transaction, c.options, c.error);
    return_if_error_m(c.error);
    if (!*c.transaction)
        safe_section("Allocating transaction handle", c.error, [&] { *c.transaction = new level_txn_t(); });
    return_if_error_m(c.error);

    level_db_t& db = *reinterpret_cast<level_db_t*>(c.db);
    level_txn_t& txn = **reinterpret_cast<level_txn_t**>(c.transaction);

    auto wants_just_snapshot = !(c.options & ukv_option_transaction_dont_watch_k);
    if (wants_just_snapshot) {
        txn.snapshot = db.GetSnapshot();
        txn.db = &db;
        if (!txn.snapshot)
            *c.error = "Couldn't start a transaction!";
    }
#endif
}

void ukv_transaction_commit(ukv_transaction_commit_t* c_ptr) {

    ukv_transaction_commit_t& c = *c_ptr;
    *c.error = "Transactions not supported by LevelDB!";
}

/*********************************************************/
/*****************	  Memory Management   ****************/
/*********************************************************/

void ukv_arena_free(ukv_arena_t c_arena) {
    clear_linked_memory(c_arena);
}

void ukv_transaction_free(ukv_transaction_t) {
#if 0 // TODO: Persistent Snapshots will be receiving a separate interface.
    if (!c_txn)
        return;
    level_txn_t& txn = *reinterpret_cast<level_txn_t*>(c_txn);
    level_db_t& db = *txn.db;
    if (txn.snapshot)
        db.ReleaseSnapshot(txn.snapshot);
    txn.db = nullptr;
    txn.snapshot = nullptr;
    delete &txn;
#endif
}

void ukv_database_free(ukv_database_t c_db) {
    if (!c_db)
        return;
    level_db_t* db = reinterpret_cast<level_db_t*>(c_db);
    delete db;
}

void ukv_error_free(ukv_error_t) {
}