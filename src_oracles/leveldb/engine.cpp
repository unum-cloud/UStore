/**
 *  @file src_oracles/leveldb/engine.cpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief Embedded persistent key-value store on LevelDB, without named collections or transactions.
 */

#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <charconv> // `std::from_chars`
#include <limits>   // `std::numeric_limits`
#include <memory>   // `std::unique_ptr`
#include <mutex>    // `std::lock_guard`, `std::mutex`
#include <new>      // `std::nothrow`
#include <span>     // `std::span`
#include <string>   // `std::string`, the path, property and value type LevelDB takes

#include <leveldb/comparator.h>
#include <leveldb/db.h>
#include <leveldb/write_batch.h>

#include <smashtable/basic_hash_table.hpp>

#include <ustore/db.h>
#include <ustore/ranges_args.hpp> // `places_arg_t`

#include "engine.hpp"                 // `engine_t`, `handle_t`, `handle_cast`
#include "helpers/blocking_files.hpp" // `directory_exists`
#include "helpers/full_scan.hpp"      // `reservoir_sample_iterator`
#include "helpers/linked_array.hpp"   // `uninitialized_array`

namespace unum::ustore::leveldb {

#pragma region Database

/** The open LevelDB database. */
using native_t = ::leveldb::DB;

/** Status LevelDB answers. */
using native_status_t = ::leveldb::Status;

/** An owned LevelDB iterator. */
using iterator_pointer_t = std::unique_ptr<::leveldb::Iterator>;

/** Reads the key @p slice holds, whose bytes LevelDB leaves unaligned. */
inline ustore_key_t key_of(::leveldb::Slice const& slice) noexcept {
    ustore_key_t key;
    std::memcpy(&key, slice.data(), sizeof(ustore_key_t));
    return key;
}

/** Orders keys as the integers they hold. */
struct key_comparator_t final : public ::leveldb::Comparator {

    int Compare(::leveldb::Slice const& left, ::leveldb::Slice const& right) const override {
        ustore_key_t const left_key = key_of(left);
        ustore_key_t const right_key = key_of(right);
        if (left_key == right_key) return 0;
        return left_key < right_key ? -1 : 1;
    }

    char const* Name() const override { return "Integral"; }

    void FindShortestSeparator(std::string*, ::leveldb::Slice const&) const override {}

    /** Leaves @p key as it is, since LevelDB keeps a successor only when it is physically shorter. */
    void FindShortSuccessor(std::string*) const override {}
};

key_comparator_t const key_comparator_k {};

/** A snapshot handle, whose address is its identifier. */
struct snapshot_t {

    /** The LevelDB snapshot. */
    ::leveldb::Snapshot const* native = nullptr;
};

/** Identifiers of the live snapshots. */
using snapshots_t = st::hash_set<ustore_snapshot_t>;

/** The calls this engine serves, which stamp every handle it hands out. */
extern engine_t const engine_k;

/** The open database, its options and its snapshots. */
struct database_t : handle_t {

    /** Options the database opened with, reused to create exports. */
    ::leveldb::Options options;

    /** Identifiers of the live snapshots. */
    snapshots_t snapshots;

    /** The open database. */
    std::unique_ptr<native_t> native;

    /** Guards the snapshots. */
    std::mutex mutex;
};

inline ::leveldb::Slice to_slice(ustore_key_t const& key) noexcept {
    return {reinterpret_cast<char const*>(&key), sizeof(ustore_key_t)};
}

inline ::leveldb::Slice to_slice(value_view_t value) noexcept {
    return {reinterpret_cast<char const*>(value.begin()), value.size()};
}

inline value_view_t to_value(::leveldb::Slice const& slice) noexcept {
    return {reinterpret_cast<ustore_bytes_cptr_t>(slice.data()), static_cast<ustore_length_t>(slice.size())};
}

/** Exports a failed status into @p error, answering whether it failed. */
bool export_error(native_status_t const& status, ustore_error_t* error) noexcept {
    if (status.ok()) return false;

    if (status.IsCorruption()) *error = {ustore_error_malformed_k, "LevelDB found corrupted data"};
    else if (status.IsIOError()) *error = {ustore_error_io_k, "LevelDB failed an I/O operation"};
    else if (status.IsInvalidArgument()) *error = {ustore_error_invalid_argument_k, "LevelDB rejected an argument"};
    else if (status.IsNotFound()) *error = {ustore_error_not_found_k, "LevelDB found no such entry"};
    else if (status.IsNotSupportedError())
        *error = {ustore_error_unsupported_k, "LevelDB does not support the operation"};
    else *error = {ustore_error_internal_k, "LevelDB failed"};
    return true;
}

/** Refuses tasks addressing any collection but the main one, answering whether it refused. */
bool refuse_other_collections(strided_iterator<ustore_collection_t const> collections, std::size_t count,
                              ustore_error_t* error) noexcept {
    if (!collections) return false;
    for (std::size_t task_index = 0; task_index != count; ++task_index) {
        if (collections[task_index] == ustore_collection_main_k) continue;
        *error = {ustore_error_unsupported_k, "Collections are not supported by LevelDB"};
        return true;
    }
    return false;
}

/** Points @p options at the snapshot @p id, leaving them untouched for the zero identifier. */
void use_snapshot(database_t& database, ustore_snapshot_t id, ::leveldb::ReadOptions& options, ustore_error_t* error) {
    if (!id) return;
    std::lock_guard lock {database.mutex};
    if (database.snapshots.find(id) == database.snapshots.end()) {
        *error = {ustore_error_not_found_k, "The snapshot doesn't exist"};
        return;
    }
    options.snapshot = reinterpret_cast<snapshot_t*>(id)->native;
}

#pragma endregion Database

#pragma region Reads and Writes

/** From this many keys one iterator, sharing a version and its loaded blocks, beats a `Get` that copies each value. */
constexpr std::size_t iterator_read_threshold_k = 16;

void write_one(database_t& database, places_arg_t const& places, contents_arg_t const& contents,
               ::leveldb::WriteOptions const& options, ustore_error_t* error) {

    auto place = places[0];
    auto content = contents[0];
    auto const key = to_slice(place.key);
    native_status_t const status = !content ? database.native->Delete(options, key)
                                            : database.native->Put(options, key, to_slice(content));
    export_error(status, error);
}

void write_many(database_t& database, places_arg_t const& places, contents_arg_t const& contents,
                ::leveldb::WriteOptions const& options, ustore_error_t* error) {

    ::leveldb::WriteBatch batch;
    for (std::size_t task_index = 0; task_index != places.size(); ++task_index) {
        auto place = places[task_index];
        auto content = contents[task_index];
        auto const key = to_slice(place.key);
        if (!content) batch.Delete(key);
        else batch.Put(key, to_slice(content));
    }

    native_status_t const status = database.native->Write(options, &batch);
    export_error(status, error);
}

/** Reads every place with its own `Get`, handing each value to @p enumerator with its index. */
template <typename value_enumerator_type_>
void get_each(database_t& database, places_arg_t const& places, ::leveldb::ReadOptions const& options,
              value_enumerator_type_&& enumerator, ustore_error_t* error) {

    // `Get` writes only into a string, so one buffer serves the whole batch
    std::string value;
    for (std::size_t task_index = 0; task_index != places.size(); ++task_index) {
        native_status_t const status = database.native->Get(options, to_slice(places[task_index].key), &value);
        if (status.IsNotFound()) {
            enumerator(task_index, value_view_t {});
            continue;
        }
        if (export_error(status, error)) return;
        enumerator(task_index, to_value(value));
    }
}

/** Reads every place through one iterator, handing each value to @p enumerator straight from its block. */
template <typename value_enumerator_type_>
void seek_each(database_t& database, places_arg_t const& places, ::leveldb::ReadOptions const& options,
               value_enumerator_type_&& enumerator, ustore_error_t* error) {

    iterator_pointer_t iterator {database.native->NewIterator(options)};
    for (std::size_t task_index = 0; task_index != places.size(); ++task_index) {
        ::leveldb::Slice const key = to_slice(places[task_index].key);
        iterator->Seek(key);
        if (iterator->Valid() && iterator->key().compare(key) == 0) {
            enumerator(task_index, to_value(iterator->value()));
            continue;
        }
        if (export_error(iterator->status(), error)) return;
        enumerator(task_index, value_view_t {});
    }
}

#pragma endregion Reads and Writes

#pragma region Lifetime

void database_init(ustore_database_init_leveldb_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need an output for the database"};
        return;
    }
    if (!arguments.directory) {
        *arguments.error = {ustore_error_invalid_argument_k, "A database needs a directory"};
        return;
    }

    auto [root_exists, directory_exists_status] = directory_exists(arguments.directory);
    if (directory_exists_status.failed()) {
        *arguments.error = directory_exists_status;
        return;
    }
    if (!root_exists) {
        *arguments.error = {ustore_error_not_found_k, "Root isn't a directory"};
        return;
    }

    std::unique_ptr<database_t> database {new (std::nothrow) database_t};
    if (!database) {
        *arguments.error = {ustore_error_out_of_memory_k, "Failed to allocate the database"};
        return;
    }
    database->options.comparator = &key_comparator_k;
    database->options.compression = ::leveldb::kNoCompression;
    database->options.create_if_missing = true;

    native_t* native = nullptr;
    native_status_t const status = native_t::Open(database->options, std::string(arguments.directory), &native);
    if (!status.ok()) {
        *arguments.error = {ustore_error_io_k, "Failed to open LevelDB"};
        return;
    }
    database->native.reset(native);
    database->engine = &engine_k;
    *arguments.db = static_cast<handle_t*>(database.release());
}

void database_capabilities(ustore_database_capabilities_t& arguments) {
    *arguments.capabilities = ustore_supports_snapshots_k;
}

#pragma endregion Lifetime

#pragma region Snapshots

void snapshot_list(ustore_snapshot_list_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!arguments.count || !arguments.ids) {
        *arguments.error = {ustore_error_invalid_argument_k, "Null outputs specified"};
        return;
    }

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;

    database_t& database = *handle_cast<database_t>(arguments.db);
    std::lock_guard lock {database.mutex};
    std::size_t const snapshots_count = database.snapshots.size();
    *arguments.count = static_cast<ustore_size_t>(snapshots_count);

    auto ids = arena.alloc_or_dummy(snapshots_count, arguments.error, arguments.ids);
    if (arguments.error->status) return;

    std::size_t index = 0;
    [[maybe_unused]] auto const walked = database.snapshots.for_each(
        [&](auto const& slot) noexcept { ids[index++] = slot.key(); });
}

void snapshot_create(ustore_snapshot_create_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }

    database_t& database = *handle_cast<database_t>(arguments.db);
    std::lock_guard lock {database.mutex};
    if (database.snapshots.find(*arguments.id) != database.snapshots.end()) {
        *arguments.error = {ustore_error_already_exists_k, "Such snapshot already exists"};
        return;
    }

    std::unique_ptr<snapshot_t> snapshot {new (std::nothrow) snapshot_t};
    if (!snapshot) {
        *arguments.error = {ustore_error_out_of_memory_k, "Failed to allocate the snapshot handle"};
        return;
    }
    snapshot->native = database.native->GetSnapshot();
    if (!snapshot->native) {
        *arguments.error = {ustore_error_internal_k, "Failed to take a snapshot"};
        return;
    }

    auto const id = reinterpret_cast<ustore_snapshot_t>(snapshot.get());
    if (st::failed(database.snapshots.upsert(id))) {
        database.native->ReleaseSnapshot(snapshot->native);
        *arguments.error = {ustore_error_out_of_memory_k, "Failed to register the snapshot"};
        return;
    }
    [[maybe_unused]] snapshot_t* const registered = snapshot.release();
    *arguments.id = id;
}

void snapshot_export(ustore_snapshot_export_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    database_t& database = *handle_cast<database_t>(arguments.db);

    ::leveldb::ReadOptions scan_options;
    scan_options.fill_cache = false;
    use_snapshot(database, arguments.id, scan_options, arguments.error);
    if (arguments.error->status) return;

    native_t* exported_native = nullptr;
    native_status_t status = native_t::Open(database.options, arguments.path, &exported_native);
    if (!status.ok()) {
        *arguments.error = {ustore_error_io_k, "Failed to create the exported database"};
        return;
    }
    std::unique_ptr<native_t> exported {exported_native};

    // Batches the size of a memtable keep writes few without holding the whole export in memory
    ::leveldb::WriteBatch batch;
    iterator_pointer_t iterator {database.native->NewIterator(scan_options)};
    for (iterator->SeekToFirst(); iterator->Valid() && status.ok(); iterator->Next()) {
        batch.Put(iterator->key(), iterator->value());
        if (batch.ApproximateSize() < database.options.write_buffer_size) continue;
        status = exported->Write(::leveldb::WriteOptions(), &batch);
        batch.Clear();
    }
    if (status.ok()) status = iterator->status();
    if (status.ok()) status = exported->Write(::leveldb::WriteOptions(), &batch);
    iterator.reset();
    exported.reset();

    if (!status.ok()) ::leveldb::DestroyDB(arguments.path, database.options);
    export_error(status, arguments.error);
}

void snapshot_drop(ustore_snapshot_drop_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    database_t& database = *handle_cast<database_t>(arguments.db);
    std::lock_guard lock {database.mutex};
    if (!database.snapshots.erase(arguments.id)) {
        *arguments.error = {ustore_error_not_found_k, "The snapshot doesn't exist"};
        return;
    }

    snapshot_t* const snapshot = reinterpret_cast<snapshot_t*>(arguments.id);
    database.native->ReleaseSnapshot(snapshot->native);
    delete snapshot;
}

#pragma endregion Snapshots

#pragma region Reads and Writes

void write(ustore_write_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }

    database_t& database = *handle_cast<database_t>(arguments.db);
    strided_iterator<ustore_collection_t const> collections {arguments.collections, arguments.collections_stride};
    strided_iterator<ustore_key_t const> keys {arguments.keys, arguments.keys_stride};
    strided_iterator<ustore_bytes_cptr_t const> values {arguments.values, arguments.values_stride};
    strided_iterator<ustore_length_t const> offsets {arguments.offsets, arguments.offsets_stride};
    strided_iterator<ustore_length_t const> lengths {arguments.lengths, arguments.lengths_stride};
    bits_view_t presences {arguments.presences};

    places_arg_t places {collections, keys, {}, arguments.tasks_count};
    contents_arg_t contents {presences, offsets, lengths, values, arguments.tasks_count};

    validate_write(arguments.transaction, places, contents, arguments.options, arguments.error);
    if (arguments.error->status) return;
    if (refuse_other_collections(collections, arguments.tasks_count, arguments.error)) return;
    if (!arguments.tasks_count) return;

    ::leveldb::WriteOptions options;
    options.sync = arguments.options & ustore_option_write_flush_k;

    if (arguments.tasks_count == 1) write_one(database, places, contents, options, arguments.error);
    else write_many(database, places, contents, options, arguments.error);
}

void read(ustore_read_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!arguments.tasks_count) return;

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;

    database_t& database = *handle_cast<database_t>(arguments.db);
    strided_iterator<ustore_collection_t const> collections {arguments.collections, arguments.collections_stride};
    strided_iterator<ustore_key_t const> keys {arguments.keys, arguments.keys_stride};
    places_arg_t places {{}, keys, {}, arguments.tasks_count};

    validate_read(arguments.transaction, places, arguments.options, arguments.error);
    if (arguments.error->status) return;
    if (refuse_other_collections(collections, arguments.tasks_count, arguments.error)) return;

    ::leveldb::ReadOptions options;
    use_snapshot(database, arguments.snapshot, options, arguments.error);
    if (arguments.error->status) return;

    auto offsets = arena.alloc_or_dummy(places.count + 1, arguments.error, arguments.offsets);
    if (arguments.error->status) return;
    auto lengths = arena.alloc_or_dummy(places.count, arguments.error, arguments.lengths);
    if (arguments.error->status) return;
    auto presences = arena.alloc_or_dummy(places.count, arguments.error, arguments.presences);
    if (arguments.error->status) return;
    bool const needs_export = arguments.values != nullptr;

    uninitialized_array<byte_t> contents(arena);

    // Metadata and data come in one pass, as reading from disk is expensive
    auto data_enumerator = [&](std::size_t index, value_view_t value) {
        presences[index] = bool(value);
        lengths[index] = value ? value.size() : ustore_length_missing_k;
        offsets[index] = contents.size();
        if (needs_export) contents.insert(contents.size(), value.begin(), value.end(), arguments.error);
    };
    if (places.count < iterator_read_threshold_k) get_each(database, places, options, data_enumerator, arguments.error);
    else seek_each(database, places, options, data_enumerator, arguments.error);
    offsets[places.count] = contents.size();
    if (needs_export) *arguments.values = reinterpret_cast<ustore_bytes_ptr_t>(contents.begin());
}

void scan(ustore_scan_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;

    database_t& database = *handle_cast<database_t>(arguments.db);
    strided_iterator<ustore_collection_t const> collections {arguments.collections, arguments.collections_stride};
    strided_iterator<ustore_key_t const> start_keys {arguments.start_keys, arguments.start_keys_stride};
    strided_iterator<ustore_length_t const> limits {arguments.count_limits, arguments.count_limits_stride};
    scans_arg_t scans {{}, start_keys, limits, arguments.tasks_count};

    validate_scan(arguments.transaction, scans, arguments.options, arguments.error);
    if (arguments.error->status) return;
    if (refuse_other_collections(collections, arguments.tasks_count, arguments.error)) return;

    auto offsets = arena.alloc_or_dummy(scans.count + 1, arguments.error, arguments.offsets);
    if (arguments.error->status) return;
    auto counts = arena.alloc_or_dummy(scans.count, arguments.error, arguments.counts);
    if (arguments.error->status) return;

    auto const total_keys = reduce_n(scans.limits, scans.count, 0ul);
    auto keys_output = *arguments.keys = arena.alloc<ustore_key_t>(total_keys, arguments.error).data();
    if (arguments.error->status) return;

    ::leveldb::ReadOptions options;
    options.fill_cache = false;
    use_snapshot(database, arguments.snapshot, options, arguments.error);
    if (arguments.error->status) return;

    iterator_pointer_t iterator {database.native->NewIterator(options)};
    for (ustore_size_t task_index = 0; task_index != arguments.tasks_count; ++task_index) {
        scan_t task = scans[task_index];
        iterator->Seek(to_slice(task.min_key));
        offsets[task_index] = keys_output - *arguments.keys;

        ustore_size_t matched_count = 0;
        while (iterator->Valid() && matched_count != task.limit) {
            std::memcpy(keys_output, iterator->key().data(), sizeof(ustore_key_t));
            ++keys_output;
            ++matched_count;
            iterator->Next();
        }
        if (export_error(iterator->status(), arguments.error)) return;

        counts[task_index] = matched_count;
    }

    offsets[scans.size()] = keys_output - *arguments.keys;
}

void sample(ustore_sample_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!arguments.tasks_count) return;

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;

    database_t& database = *handle_cast<database_t>(arguments.db);
    strided_iterator<ustore_collection_t const> collections {arguments.collections, arguments.collections_stride};
    strided_iterator<ustore_length_t const> limits {arguments.count_limits, arguments.count_limits_stride};
    sample_args_t samples {{}, limits, arguments.tasks_count};
    if (refuse_other_collections(collections, arguments.tasks_count, arguments.error)) return;

    auto offsets = arena.alloc_or_dummy(samples.count + 1, arguments.error, arguments.offsets);
    if (arguments.error->status) return;
    auto counts = arena.alloc_or_dummy(samples.count, arguments.error, arguments.counts);
    if (arguments.error->status) return;

    auto const total_keys = reduce_n(samples.limits, samples.count, 0ul);
    auto keys_output = *arguments.keys = arena.alloc<ustore_key_t>(total_keys, arguments.error).data();
    if (arguments.error->status) return;

    ::leveldb::ReadOptions options;
    options.fill_cache = false;
    use_snapshot(database, arguments.snapshot, options, arguments.error);
    if (arguments.error->status) return;

    iterator_pointer_t iterator {database.native->NewIterator(options)};
    for (std::size_t task_index = 0; task_index != samples.count; ++task_index) {
        sample_arg_t task = samples[task_index];
        offsets[task_index] = keys_output - *arguments.keys;

        std::size_t const drawn = reservoir_sample_iterator(iterator, std::span {keys_output, task.limit});
        if (export_error(iterator->status(), arguments.error)) return;

        counts[task_index] = static_cast<ustore_length_t>(drawn);
        keys_output += drawn;
    }
    offsets[samples.count] = keys_output - *arguments.keys;
}

void measure(ustore_measure_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;

    auto min_cardinalities = arena.alloc_or_dummy(arguments.tasks_count, arguments.error, arguments.min_cardinalities);
    auto max_cardinalities = arena.alloc_or_dummy(arguments.tasks_count, arguments.error, arguments.max_cardinalities);
    auto min_value_bytes = arena.alloc_or_dummy(arguments.tasks_count, arguments.error, arguments.min_value_bytes);
    auto max_value_bytes = arena.alloc_or_dummy(arguments.tasks_count, arguments.error, arguments.max_value_bytes);
    auto min_space_usages = arena.alloc_or_dummy(arguments.tasks_count, arguments.error, arguments.min_space_usages);
    auto max_space_usages = arena.alloc_or_dummy(arguments.tasks_count, arguments.error, arguments.max_space_usages);
    if (arguments.error->status) return;

    database_t& database = *handle_cast<database_t>(arguments.db);
    strided_iterator<ustore_collection_t const> collections {arguments.collections, arguments.collections_stride};
    strided_iterator<ustore_key_t const> start_keys {arguments.start_keys, arguments.start_keys_stride};
    strided_iterator<ustore_key_t const> end_keys {arguments.end_keys, arguments.end_keys_stride};
    if (refuse_other_collections(collections, arguments.tasks_count, arguments.error)) return;

    // Memory usage describes the whole database, so one property read serves every task
    std::uint64_t memory_bytes = 0;
    std::string memory_usage;
    if (database.native->GetProperty("leveldb.approximate-memory-usage", &memory_usage)) {
        [[maybe_unused]] auto const parsed = std::from_chars(memory_usage.data(),
                                                             memory_usage.data() + memory_usage.size(), memory_bytes);
    }

    for (ustore_size_t task_index = 0; task_index != arguments.tasks_count; ++task_index) {
        min_cardinalities[task_index] = static_cast<ustore_size_t>(0);
        max_cardinalities[task_index] = static_cast<ustore_size_t>(0);
        min_value_bytes[task_index] = static_cast<ustore_size_t>(0);
        max_value_bytes[task_index] = static_cast<ustore_size_t>(0);

        ustore_key_t const min_key = start_keys[task_index];
        ustore_key_t const max_key = end_keys[task_index];
        ::leveldb::Range const range(to_slice(min_key), to_slice(max_key));
        std::uint64_t approximate_size = 0;
        database.native->GetApproximateSizes(&range, 1, &approximate_size);
        min_space_usages[task_index] = approximate_size;
        max_space_usages[task_index] = memory_bytes;
    }
}

#pragma endregion Reads and Writes

#pragma region Collections

void collection_create(ustore_collection_create_t& arguments) {
    *arguments.error = {ustore_error_unsupported_k, "Collections are not supported by LevelDB"};
}

void collection_drop(ustore_collection_drop_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    bool const invalidate = arguments.mode == ustore_drop_keys_vals_handle_k;
    if (arguments.id != ustore_collection_main_k || invalidate) {
        *arguments.error = {ustore_error_unsupported_k, "Collections are not supported by LevelDB"};
        return;
    }

    database_t& database = *handle_cast<database_t>(arguments.db);

    ::leveldb::WriteBatch batch;
    iterator_pointer_t iterator {database.native->NewIterator(::leveldb::ReadOptions())};

    if (arguments.mode == ustore_drop_keys_vals_k) {
        for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) batch.Delete(iterator->key());
    }
    else if (arguments.mode == ustore_drop_vals_k) {
        for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next())
            batch.Put(iterator->key(), ::leveldb::Slice());
    }

    ::leveldb::WriteOptions options;
    options.sync = true;
    native_status_t const status = database.native->Write(options, &batch);
    export_error(status, arguments.error);
}

void collection_list(ustore_collection_list_t& arguments) {
    *arguments.count = 0;
    if (arguments.ids) *arguments.ids = nullptr;
}

void database_control(ustore_database_control_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!arguments.request) {
        *arguments.error = {ustore_error_invalid_argument_k, "Null request specified"};
        return;
    }

    *arguments.response = nullptr;
    *arguments.error = {ustore_error_unsupported_k, "Controls are not supported by this engine"};
}

#pragma endregion Collections

#pragma region Transactions

void transaction_init(ustore_transaction_init_t& arguments) {
    *arguments.error = {ustore_error_unsupported_k, "Transactions are not supported by LevelDB"};
}

void transaction_stage(ustore_transaction_stage_t& arguments) {
    *arguments.error = {ustore_error_unsupported_k, "Transactions are not supported by LevelDB"};
}

void transaction_commit(ustore_transaction_commit_t& arguments) {
    *arguments.error = {ustore_error_unsupported_k, "Transactions are not supported by LevelDB"};
}

#pragma endregion Transactions

#pragma region Memory

void transaction_free(ustore_transaction_t) noexcept {}

void database_free(ustore_database_t handle) noexcept {
    database_t* const database = handle_cast<database_t>(handle);
    // Snapshots a caller never dropped belong to the database, so they go with it
    [[maybe_unused]] auto const released = database->snapshots.for_each([&](auto const& slot) noexcept {
        snapshot_t* const snapshot = reinterpret_cast<snapshot_t*>(slot.key());
        database->native->ReleaseSnapshot(snapshot->native);
        delete snapshot;
    });
    delete database;
}

#pragma endregion Memory

engine_t const engine_k {
    .database_capabilities = &guarded<ustore_database_capabilities_t, &database_capabilities>,
    .database_control = &guarded<ustore_database_control_t, &database_control>,
    .snapshot_list = &guarded<ustore_snapshot_list_t, &snapshot_list>,
    .snapshot_create = &guarded<ustore_snapshot_create_t, &snapshot_create>,
    .snapshot_export = &guarded<ustore_snapshot_export_t, &snapshot_export>,
    .snapshot_drop = &guarded<ustore_snapshot_drop_t, &snapshot_drop>,
    .read = &guarded<ustore_read_t, &read>,
    .write = &guarded<ustore_write_t, &write>,
    .scan = &guarded<ustore_scan_t, &scan>,
    .sample = &guarded<ustore_sample_t, &sample>,
    .measure = &guarded<ustore_measure_t, &measure>,
    .collection_create = &guarded<ustore_collection_create_t, &collection_create>,
    .collection_drop = &guarded<ustore_collection_drop_t, &collection_drop>,
    .collection_list = &guarded<ustore_collection_list_t, &collection_list>,
    .transaction_init = &guarded<ustore_transaction_init_t, &transaction_init>,
    .transaction_stage = &guarded<ustore_transaction_stage_t, &transaction_stage>,
    .transaction_commit = &guarded<ustore_transaction_commit_t, &transaction_commit>,
    .transaction_free = &transaction_free,
    .database_free = &database_free,
};

} // namespace unum::ustore::leveldb

using namespace unum::ustore;
using namespace unum::ustore::leveldb;

void ustore_database_init_leveldb(ustore_database_init_leveldb_t* arguments) {
    guarded<ustore_database_init_leveldb_t, &database_init>(*arguments);
}
