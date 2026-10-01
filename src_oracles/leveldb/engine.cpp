/**
 *  @file src_oracles/leveldb/engine.cpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief Embedded persistent key-value store on LevelDB, with collections as key ranges and without transactions.
 *
 *  LevelDB keeps one keyspace in bytewise order, so every stored key leads with the UUID of its collection and the
 *  pairs of each collection lie in one contiguous run. The collection's modality shapes the rest of the key, and the
 *  catalog and the names are two more collections under reserved UUIDs:
 *
 *  @code{.c}
 *  struct pairs {              // blobs, docs, graph and any collections, the main one under the nil UUID
 *      uint8_t collection[16]; // offset 0
 *      uint8_t key[16];        // offset 16, 32 bytes in all
 *  };
 *  struct strings {            // strings collections
 *      uint8_t collection[16]; // offset 0
 *      char string[];          // offset 16
 *  };
 *  struct catalog {            // every collection but the main one, valued by a `struct catalog_entry`
 *      uint8_t collection[16]; // offset 0, all bytes 0xFF
 *      uint8_t listed[16];     // offset 16
 *  };
 *  struct catalog_entry {
 *      uint8_t modality;       // offset 0
 *      char name[];            // offset 1, empty for an anonymous collection
 *  };
 *  struct names {              // valued by a `name_record_t`
 *      uint8_t collection[16]; // offset 0, all bytes 0xFF but the last, 0xFE
 *      char name[];            // offset 16
 *  };
 *  @endcode
 *
 *  A block stores the bytes a key shares with its predecessor once, so the leading UUID costs little on disk.
 */

#include <cstddef> // `offsetof`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <charconv>     // `std::from_chars`
#include <limits>       // `std::numeric_limits`
#include <memory>       // `std::unique_ptr`
#include <mutex>        // `std::lock_guard`, `std::mutex`
#include <new>          // `std::nothrow`
#include <span>         // `std::span`
#include <string>       // `std::string`, the path, property and value type LevelDB takes
#include <string_view>  // `std::string_view`
#include <system_error> // `std::errc`

#include <fmt/format.h> // `fmt::format_to_n`
#include <leveldb/db.h>
#include <leveldb/write_batch.h>

#include "ustore/db.h"
#include "ustore/ranges_args.hpp" // `places_arg_t`, `strings_arg_t`
#include "ustore/status.hpp"      // `expected`, `status_t`

#include "engine.hpp"                 // `engine_t`, `handle_t`, `handle_cast`
#include "helpers/blocking_files.hpp" // `directory_exists`
#include "helpers/full_scan.hpp"      // `reservoir_sample_iterator`
#include "helpers/linked_array.hpp"   // `growing_tape_t`, `uninitialized_array`

namespace unum::ustore::leveldb {

#pragma region Database

/** The open LevelDB database. */
using native_t = ::leveldb::DB;

/** Status LevelDB answers. */
using native_status_t = ::leveldb::Status;

/** An owned LevelDB iterator. */
using iterator_pointer_t = std::unique_ptr<::leveldb::Iterator>;

/** The collection holding the catalog, all bytes `0xFF`, which no minted UUIDv7 reaches, as its byte 6 is `0x7X`. */
constexpr ustore_collection_id_t catalog_collection_k {
    {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}}};

/** The collection holding the names of collections, all bytes `0xFF` but the last `0xFE`, which no minted UUIDv7
 *  reaches either. */
constexpr ustore_collection_id_t names_collection_k {
    {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE}}};

/** Bytes every stored key leads with, the UUID of its collection. */
constexpr std::size_t collection_bytes_k = sizeof(ustore_collection_id_t);

/** A stored key of a `struct pairs` or a `struct catalog`, which a slice may view for as long as it lives. */
struct uuid_key_t {
    char bytes[collection_bytes_k + sizeof(ustore_uuid_t)];

    [[nodiscard]] ::leveldb::Slice slice() const noexcept { return {bytes, sizeof(bytes)}; }
};

/** The stored key of @p uuid in @p collection: a key of a collection keyed by UUIDs, or a collection the catalog
 *  lists. */
inline uuid_key_t uuid_key(ustore_collection_id_t const& collection, ustore_uuid_t const& uuid) noexcept {
    uuid_key_t stored;
    std::memcpy(stored.bytes, collection.uuid.bytes, collection_bytes_k);
    std::memcpy(stored.bytes + collection_bytes_k, uuid.bytes, sizeof(uuid.bytes));
    return stored;
}

/** Writes into @p stored the stored key of @p text in @p collection: a string of a strings collection, or a name. */
inline ::leveldb::Slice string_key(ustore_collection_id_t const& collection, std::string_view text,
                                   std::string& stored) {
    stored.assign(reinterpret_cast<char const*>(collection.uuid.bytes), collection_bytes_k);
    stored.append(text);
    return stored;
}

/** The 16 bytes every stored key of @p collection leads with, where a walk of it starts, viewing @p collection
 *  itself. */
inline ::leveldb::Slice prefix_of(ustore_collection_id_t const& collection) noexcept {
    return {reinterpret_cast<char const*>(collection.uuid.bytes), collection_bytes_k};
}

/** Refuses a temporary collection, which would leave the prefix viewing freed bytes. */
::leveldb::Slice prefix_of(ustore_collection_id_t const&&) = delete;

/** The 16 bytes @p stored holds past its collection's UUID: the key of a pair, or the collection a catalog entry
 *  lists. */
inline ustore_uuid_t tail_uuid_of(::leveldb::Slice const& stored) noexcept {
    ustore_uuid_t uuid;
    std::memcpy(uuid.bytes, stored.data() + collection_bytes_k, sizeof(uuid.bytes));
    return uuid;
}

/** What follows the collection's UUID in @p stored: the key or string of a pair. */
inline ::leveldb::Slice tail_of(::leveldb::Slice const& stored) noexcept {
    return {stored.data() + collection_bytes_k, stored.size() - collection_bytes_k};
}

/** What the catalog records of a collection: `struct catalog_entry`. */
struct catalog_entry_t {
    ustore_modality_t modality = ustore_modality_any_k;

    /** The name the collection carries, empty for none, viewing the bytes the entry was read from. */
    std::string_view name;
};

/** Reads the catalog entry @p held, whose name views the same bytes, refusing bytes too short to be one. */
inline expected<catalog_entry_t> read_catalog_entry(::leveldb::Slice const& held) noexcept {
    if (held.empty()) return status_t {status_code_t::malformed_k, "The catalog entry of a collection is malformed"};
    return catalog_entry_t {static_cast<ustore_modality_t>(held[0]), {held.data() + 1, held.size() - 1}};
}

/** Walks the keys of one collection keyed by UUIDs over @p base, handing the reservoir sampler the key of each. */
class collection_walk_t {
    ::leveldb::Iterator& base_;
    ::leveldb::Slice prefix_;

  public:
    collection_walk_t(::leveldb::Iterator& base, ::leveldb::Slice prefix) noexcept : base_(base), prefix_(prefix) {}

    collection_walk_t* operator->() noexcept { return this; }

    void SeekToFirst() { base_.Seek(prefix_); }

    void Next() { base_.Next(); }

    [[nodiscard]] bool Valid() const noexcept { return base_.Valid() && base_.key().starts_with(prefix_); }

    [[nodiscard]] ::leveldb::Slice key() const noexcept { return tail_of(base_.key()); }
};

/** The calls this engine serves, which stamp every handle it hands out. */
extern engine_t const engine_k;

/** The open database and its options. */
struct database_t : handle_t {

    /** Options the database opened with, reused to create exports. */
    ::leveldb::Options options;

    /** The open database. */
    std::unique_ptr<native_t> native;

    /** Serializes changes to the catalog and the names, so a name is checked and taken in one step. */
    std::mutex catalog_mutex;
};

/** A snapshot handle, pinning one LevelDB snapshot of its database until freed. */
struct snapshot_t : handle_t {

    /** The database the snapshot was taken of, which releases it. */
    database_t* database = nullptr;

    /** The LevelDB snapshot. */
    ::leveldb::Snapshot const* native = nullptr;
};

inline ::leveldb::Slice to_slice(value_view_t value) noexcept {
    return {reinterpret_cast<char const*>(value.begin()), value.size()};
}

inline value_view_t to_value(::leveldb::Slice const& slice) noexcept {
    return {reinterpret_cast<ustore_bytes_cptr_t>(slice.data()), static_cast<ustore_length_t>(slice.size())};
}

/** The status LevelDB's @p status stands for, success included. */
status_t status_of(native_status_t const& status) noexcept {
    if (status.ok()) return {};
    if (status.IsCorruption()) return {status_code_t::malformed_k, "LevelDB found corrupted data"};
    if (status.IsIOError()) return {status_code_t::input_output_k, "LevelDB failed an I/O operation"};
    if (status.IsInvalidArgument()) return {status_code_t::invalid_argument_k, "LevelDB rejected an argument"};
    if (status.IsNotFound()) return {status_code_t::not_found_k, "LevelDB found no such entry"};
    if (status.IsNotSupportedError()) return {status_code_t::unsupported_k, "LevelDB does not support the operation"};
    return {status_code_t::internal_k, "LevelDB failed"};
}

/** Exports a failed status into @p error, answering whether it failed. */
bool export_error(native_status_t const& status, ustore_error_t* error) noexcept {
    if (status.ok()) return false;
    *error = status_of(status);
    return true;
}

/** What the catalog records of @p collection, whose name views @p held, refusing a collection the catalog does not
 *  list, the reserved ones included. */
expected<catalog_entry_t> look_up(database_t& database, ustore_collection_id_t const& collection, std::string& held) {
    if (collection == ustore_collection_main_k) return catalog_entry_t {};

    uuid_key_t const key = uuid_key(catalog_collection_k, collection.uuid);
    native_status_t const status = database.native->Get(::leveldb::ReadOptions(), key.slice(), &held);
    if (status.IsNotFound()) return status_t {status_code_t::not_found_k, "No such collection"};
    if (!status.ok()) return status_of(status);
    return read_catalog_entry(held);
}

/** Refuses a batch addressing a collection the catalog does not list or one keyed otherwise than @p keying, looking
 *  each run of tasks on one collection up once. */
status_t resolve_collections(database_t& database, strided_iterator<ustore_collection_id_t const> collections,
                             std::size_t count, keying_t keying) {
    std::string held;
    for (std::size_t task_index = 0; task_index != count; ++task_index) {
        bool const repeated = task_index != 0 && collections && collections[task_index] == collections[task_index - 1];
        if (repeated) continue;
        ustore_collection_id_t const collection = collections ? collections[task_index] : ustore_collection_main_k;
        auto [entry, look_up_status] = look_up(database, collection, held);
        if (look_up_status.failed()) return look_up_status;
        if (keying_of(entry.modality) != keying)
            return {status_code_t::invalid_argument_k,
                    keying == keying_t::strings_k ? "The collection holds no strings" : "The collection holds strings"};
    }
    return {};
}

/** Points @p options at @p snapshot, leaving them at the head for a call naming none. */
void use_snapshot(ustore_snapshot_t snapshot, ::leveldb::ReadOptions& options) noexcept {
    if (snapshot) options.snapshot = handle_cast<snapshot_t>(snapshot)->native;
}

#pragma endregion Database

#pragma region Reads and Writes

/** From this many keys one iterator, sharing a version and its loaded blocks, beats a `Get` that copies each value. */
constexpr std::size_t iterator_read_threshold_k = 16;

void write_one(database_t& database, places_arg_t const& places, contents_arg_t const& contents,
               ::leveldb::WriteOptions const& options, ustore_error_t* error) {

    auto place = places[0];
    auto content = contents[0];
    uuid_key_t const key = uuid_key(place.collection, place.key.uuid);
    native_status_t const status = !content ? database.native->Delete(options, key.slice())
                                            : database.native->Put(options, key.slice(), to_slice(content));
    export_error(status, error);
}

void write_many(database_t& database, places_arg_t const& places, contents_arg_t const& contents,
                ::leveldb::WriteOptions const& options, ustore_error_t* error) {

    ::leveldb::WriteBatch batch;
    for (std::size_t task_index = 0; task_index != places.size(); ++task_index) {
        auto place = places[task_index];
        auto content = contents[task_index];
        uuid_key_t const key = uuid_key(place.collection, place.key.uuid);
        if (!content) batch.Delete(key.slice());
        else batch.Put(key.slice(), to_slice(content));
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
        auto place = places[task_index];
        uuid_key_t const key = uuid_key(place.collection, place.key.uuid);
        native_status_t const status = database.native->Get(options, key.slice(), &value);
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
        auto place = places[task_index];
        uuid_key_t const key = uuid_key(place.collection, place.key.uuid);
        iterator->Seek(key.slice());
        if (iterator->Valid() && iterator->key().compare(key.slice()) == 0) {
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
    *arguments.db = c_handle<ustore_database_t>(database.release());
}

void database_capabilities(ustore_database_capabilities_t& arguments) {
    *arguments.capabilities = ustore_capabilities_t(ustore_supports_collections_k | ustore_supports_snapshots_k);
}

#pragma endregion Lifetime

#pragma region Snapshots

void snapshot_init(ustore_snapshot_init_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!arguments.snapshot) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need an output for the snapshot"};
        return;
    }

    database_t& database = *handle_cast<database_t>(arguments.db);
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
    snapshot->engine = &engine_k;
    snapshot->database = &database;
    *arguments.snapshot = c_handle<ustore_snapshot_t>(snapshot.release());
}

void snapshot_export(ustore_snapshot_export_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    database_t& database = *handle_cast<database_t>(arguments.db);

    if (!arguments.snapshot) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need a snapshot to export"};
        return;
    }
    ::leveldb::ReadOptions scan_options;
    scan_options.fill_cache = false;
    use_snapshot(arguments.snapshot, scan_options);

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

#pragma endregion Snapshots

#pragma region Reads and Writes

void write(ustore_write_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }

    database_t& database = *handle_cast<database_t>(arguments.db);
    strided_iterator<ustore_collection_id_t const> collections {arguments.collections, arguments.collections_stride};
    strided_iterator<ustore_key_t const> keys {arguments.keys, arguments.keys_stride};
    strided_iterator<ustore_bytes_cptr_t const> values {arguments.values, arguments.values_stride};
    strided_iterator<ustore_length_t const> offsets {arguments.offsets, arguments.offsets_stride};
    strided_iterator<ustore_length_t const> lengths {arguments.lengths, arguments.lengths_stride};
    bits_view_t presences {arguments.presences};

    places_arg_t places {collections, keys, {}, arguments.tasks_count};
    contents_arg_t contents {presences, offsets, lengths, values, arguments.tasks_count};

    validate_write(arguments.transaction, places, contents, arguments.options, arguments.error);
    if (arguments.error->status) return;
    if (!arguments.tasks_count) return;
    *arguments.error = resolve_collections(database, collections, arguments.tasks_count, keying_t::uuids_k);
    if (arguments.error->status) return;

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
    strided_iterator<ustore_collection_id_t const> collections {arguments.collections, arguments.collections_stride};
    strided_iterator<ustore_key_t const> keys {arguments.keys, arguments.keys_stride};
    places_arg_t places {collections, keys, {}, arguments.tasks_count};

    validate_read(arguments.transaction, places, arguments.options, arguments.error);
    if (arguments.error->status) return;
    *arguments.error = resolve_collections(database, collections, arguments.tasks_count, keying_t::uuids_k);
    if (arguments.error->status) return;

    ::leveldb::ReadOptions options;
    use_snapshot(arguments.snapshot, options);

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
    strided_iterator<ustore_collection_id_t const> collections {arguments.collections, arguments.collections_stride};
    strided_iterator<ustore_key_t const> start_keys {arguments.start_keys, arguments.start_keys_stride};
    strided_iterator<ustore_length_t const> limits {arguments.count_limits, arguments.count_limits_stride};
    scans_arg_t scans {collections, start_keys, limits, arguments.tasks_count};

    validate_scan(arguments.transaction, scans, arguments.options, arguments.error);
    if (arguments.error->status) return;
    *arguments.error = resolve_collections(database, collections, arguments.tasks_count, keying_t::uuids_k);
    if (arguments.error->status) return;

    auto offsets = arena.alloc_or_dummy(scans.count + 1, arguments.error, arguments.offsets);
    if (arguments.error->status) return;
    auto counts = arena.alloc_or_dummy(scans.count, arguments.error, arguments.counts);
    if (arguments.error->status) return;

    auto const total_keys = reduce_n(scans.limits, scans.count, 0ul);
    auto keys_output = *arguments.keys = arena.alloc<ustore_key_t>(total_keys, arguments.error).data();
    if (arguments.error->status) return;

    ::leveldb::ReadOptions options;
    options.fill_cache = false;
    use_snapshot(arguments.snapshot, options);

    iterator_pointer_t iterator {database.native->NewIterator(options)};
    for (ustore_size_t task_index = 0; task_index != arguments.tasks_count; ++task_index) {
        scan_t task = scans[task_index];
        uuid_key_t const start = uuid_key(task.collection, task.min_key.uuid);
        ::leveldb::Slice const prefix = prefix_of(task.collection);
        iterator->Seek(start.slice());
        offsets[task_index] = keys_output - *arguments.keys;

        ustore_size_t matched_count = 0;
        while (iterator->Valid() && iterator->key().starts_with(prefix) && matched_count != task.limit) {
            *keys_output = ustore_key_t {tail_uuid_of(iterator->key())};
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
    strided_iterator<ustore_collection_id_t const> collections {arguments.collections, arguments.collections_stride};
    strided_iterator<ustore_length_t const> limits {arguments.count_limits, arguments.count_limits_stride};
    sample_args_t samples {collections, limits, arguments.tasks_count};
    *arguments.error = resolve_collections(database, collections, arguments.tasks_count, keying_t::uuids_k);
    if (arguments.error->status) return;

    auto offsets = arena.alloc_or_dummy(samples.count + 1, arguments.error, arguments.offsets);
    if (arguments.error->status) return;
    auto counts = arena.alloc_or_dummy(samples.count, arguments.error, arguments.counts);
    if (arguments.error->status) return;

    auto const total_keys = reduce_n(samples.limits, samples.count, 0ul);
    auto keys_output = *arguments.keys = arena.alloc<ustore_key_t>(total_keys, arguments.error).data();
    if (arguments.error->status) return;

    ::leveldb::ReadOptions options;
    options.fill_cache = false;
    use_snapshot(arguments.snapshot, options);

    iterator_pointer_t iterator {database.native->NewIterator(options)};
    for (std::size_t task_index = 0; task_index != samples.count; ++task_index) {
        sample_arg_t task = samples[task_index];
        offsets[task_index] = keys_output - *arguments.keys;

        ::leveldb::Slice const prefix = prefix_of(task.collection);
        collection_walk_t walk {*iterator, prefix};
        std::size_t const drawn = reservoir_sample_iterator(walk, std::span {keys_output, task.limit});
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
    strided_iterator<ustore_collection_id_t const> collections {arguments.collections, arguments.collections_stride};
    strided_iterator<ustore_key_t const> start_keys {arguments.start_keys, arguments.start_keys_stride};
    strided_iterator<ustore_key_t const> end_keys {arguments.end_keys, arguments.end_keys_stride};
    *arguments.error = resolve_collections(database, collections, arguments.tasks_count, keying_t::uuids_k);
    if (arguments.error->status) return;

    // Memory usage describes the whole database, so one property read serves every task
    std::uint64_t memory_bytes = 0;
    std::string memory_usage;
    bool const reported = database.native->GetProperty("leveldb.approximate-memory-usage", &memory_usage);
    char const* const usage_end = memory_usage.data() + memory_usage.size();
    auto const [parsed_end, parse_error] = std::from_chars(memory_usage.data(), usage_end, memory_bytes);
    if (!reported || parse_error != std::errc {} || parsed_end != usage_end) {
        *arguments.error = {ustore_error_internal_k, "LevelDB reported no readable memory usage"};
        return;
    }

    for (ustore_size_t task_index = 0; task_index != arguments.tasks_count; ++task_index) {
        min_cardinalities[task_index] = static_cast<ustore_size_t>(0);
        max_cardinalities[task_index] = static_cast<ustore_size_t>(0);
        min_value_bytes[task_index] = static_cast<ustore_size_t>(0);
        max_value_bytes[task_index] = static_cast<ustore_size_t>(0);

        ustore_collection_id_t const collection = collections ? collections[task_index] : ustore_collection_main_k;
        uuid_key_t const min_key = uuid_key(collection, start_keys[task_index].uuid);
        uuid_key_t const max_key = uuid_key(collection, end_keys[task_index].uuid);
        ::leveldb::Range const range(min_key.slice(), max_key.slice());
        std::uint64_t approximate_size = 0;
        database.native->GetApproximateSizes(&range, 1, &approximate_size);
        min_space_usages[task_index] = approximate_size;
        max_space_usages[task_index] = memory_bytes;
    }
}

#pragma endregion Reads and Writes

#pragma region Collections

/** Clears the run of @p collection as @p mode asks, in batches the size of a memtable, the last one synced. */
native_status_t clear_collection(database_t& database, ustore_collection_id_t const& collection,
                                 ustore_drop_mode_t mode) {
    ::leveldb::WriteOptions buffered;
    ::leveldb::WriteOptions synced;
    synced.sync = true;
    ::leveldb::WriteBatch batch;
    native_status_t status;
    ::leveldb::Slice const prefix = prefix_of(collection);
    iterator_pointer_t iterator {database.native->NewIterator(::leveldb::ReadOptions())};
    for (iterator->Seek(prefix); status.ok() && iterator->Valid(); iterator->Next()) {
        if (!iterator->key().starts_with(prefix)) break;
        if (mode == ustore_drop_vals_k) batch.Put(iterator->key(), ::leveldb::Slice());
        else batch.Delete(iterator->key());
        if (batch.ApproximateSize() < database.options.write_buffer_size) continue;
        status = database.native->Write(buffered, &batch);
        batch.Clear();
    }
    if (status.ok()) status = iterator->status();
    if (status.ok()) status = database.native->Write(synced, &batch);
    return status;
}

/** Drops @p collection, forgetting first and durably its catalog entry and the name the entry records. The caller
 *  holds @c catalog_mutex. */
status_t drop_collection(database_t& database, ustore_collection_id_t const& collection) {
    std::string held;
    auto [entry, look_up_status] = look_up(database, collection, held);
    if (look_up_status.failed()) return look_up_status;

    // A crash between the two leaves an unlisted run behind, never a listed or named collection that is gone
    ::leveldb::WriteBatch forgotten;
    uuid_key_t const listed = uuid_key(catalog_collection_k, collection.uuid);
    forgotten.Delete(listed.slice());
    std::string name;
    if (!entry.name.empty()) forgotten.Delete(string_key(names_collection_k, entry.name, name));
    ::leveldb::WriteOptions synced;
    synced.sync = true;
    status_t const forget_status = status_of(database.native->Write(synced, &forgotten));
    if (forget_status.failed()) return forget_status;
    return status_of(clear_collection(database, collection, ustore_drop_keys_vals_k));
}

void collection_create(ustore_collection_create_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!arguments.collection) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need an output for the collection"};
        return;
    }
    if (arguments.modality > ustore_modality_pages_k) {
        *arguments.error = {ustore_error_invalid_argument_k, "Unknown modality"};
        return;
    }
    std::string_view const name = string_of(arguments.name, arguments.name_length);
    if (arguments.name && name.empty()) {
        *arguments.error = {ustore_error_invalid_argument_k, "A name is never empty, as the main collection has none"};
        return;
    }

    // The entry and the name reach the log in one batch, so a crash keeps both or neither
    database_t& database = *handle_cast<database_t>(arguments.db);
    std::lock_guard lock {database.catalog_mutex};
    std::string name_key;
    if (!name.empty()) {
        std::string record;
        native_status_t const looked_up = database.native->Get(::leveldb::ReadOptions(),
                                                               string_key(names_collection_k, name, name_key), &record);
        if (looked_up.ok()) {
            *arguments.error = {ustore_error_already_exists_k, "A collection already carries the name"};
            return;
        }
        if (!looked_up.IsNotFound() && export_error(looked_up, arguments.error)) return;
    }
    ustore_collection_id_t id {};
    if (!mint_collection_id(id)) {
        *arguments.error = {ustore_error_internal_k, "The system gave no entropy for a collection identifier"};
        return;
    }

    uuid_key_t const listed = uuid_key(catalog_collection_k, id.uuid);
    std::string entry(1, static_cast<char>(arguments.modality));
    entry.append(name);
    name_record_t const record {id, arguments.modality};
    ::leveldb::WriteBatch batch;
    batch.Put(listed.slice(), entry);
    if (!name.empty()) batch.Put(name_key, ::leveldb::Slice(record.bytes, sizeof(record.bytes)));
    ::leveldb::WriteOptions synced;
    synced.sync = true;
    if (export_error(database.native->Write(synced, &batch), arguments.error)) return;
    *arguments.collection = id;
}

void collection_drop(ustore_collection_drop_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (arguments.mode != ustore_drop_keys_vals_handle_k && arguments.mode != ustore_drop_keys_vals_k &&
        arguments.mode != ustore_drop_vals_k) {
        *arguments.error = {ustore_error_invalid_argument_k, "Unknown drop mode"};
        return;
    }
    if (arguments.collection == ustore_collection_main_k && arguments.mode == ustore_drop_keys_vals_handle_k) {
        *arguments.error = {ustore_error_invalid_argument_k, "The main collection can only be cleared"};
        return;
    }

    database_t& database = *handle_cast<database_t>(arguments.db);
    if (arguments.mode == ustore_drop_keys_vals_handle_k) {
        std::lock_guard lock {database.catalog_mutex};
        *arguments.error = drop_collection(database, arguments.collection);
        return;
    }
    std::string held;
    *arguments.error = look_up(database, arguments.collection, held).status();
    if (arguments.error->status) return;
    export_error(clear_collection(database, arguments.collection, arguments.mode), arguments.error);
}

void collection_list(ustore_collection_list_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!arguments.count) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need an output for the count"};
        return;
    }

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;

    database_t& database = *handle_cast<database_t>(arguments.db);
    ::leveldb::ReadOptions options;
    options.fill_cache = false;
    use_snapshot(arguments.snapshot, options);

    // The catalog lists every collection but the main one, keyed by its ID
    ::leveldb::Slice const catalog = prefix_of(catalog_collection_k);
    iterator_pointer_t iterator {database.native->NewIterator(options)};
    uninitialized_array<ustore_collection_id_t> ids {arena};
    uninitialized_array<ustore_modality_t> modalities {arena};
    growing_tape_t names {arena};
    for (iterator->Seek(catalog); iterator->Valid() && iterator->key().starts_with(catalog); iterator->Next()) {
        ::leveldb::Slice const listed = iterator->key();
        auto [entry, read_status] = read_catalog_entry(iterator->value());
        if (read_status.failed() || listed.size() != sizeof(uuid_key_t)) {
            *arguments.error = {ustore_error_malformed_k, "The catalog entry of a collection is malformed"};
            return;
        }
        ids.push_back(ustore_collection_id_t {tail_uuid_of(listed)}, arguments.error);
        modalities.push_back(entry.modality, arguments.error);
        names.push_back(entry.name.empty() ? value_view_t {} : value_view_t {entry.name}, arguments.error);
        if (arguments.error->status) return;
    }
    if (export_error(iterator->status(), arguments.error)) return;
    *arguments.count = static_cast<ustore_size_t>(ids.size());
    if (arguments.collections) *arguments.collections = ids.data();
    if (arguments.modalities) *arguments.modalities = modalities.data();
    if (arguments.names_offsets) *arguments.names_offsets = names.offsets().begin().get();
    if (arguments.names) *arguments.names = reinterpret_cast<ustore_char_t*>(names.contents().begin().get());
}

void collection_find(ustore_collection_find_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!arguments.tasks_count) return;
    if (!arguments.names || !arguments.collections) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need the names and an output for the identifiers"};
        return;
    }

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;
    database_t& database = *handle_cast<database_t>(arguments.db);
    ::leveldb::ReadOptions options;
    use_snapshot(arguments.snapshot, options);

    std::size_t const count = arguments.tasks_count;
    strings_arg_t const names {{arguments.names, arguments.names_stride},
                               {arguments.names_lengths, arguments.names_lengths_stride}};
    std::span<ustore_collection_id_t> const ids = arena.alloc<ustore_collection_id_t>(count, arguments.error);
    auto modalities = arena.alloc_or_dummy(count, arguments.error, arguments.modalities);
    auto presences = arena.alloc_or_dummy(count, arguments.error, arguments.presences);
    if (arguments.error->status) return;

    std::string key;
    std::string record;
    for (std::size_t index = 0; index != count; ++index) {
        native_status_t const status = database.native->Get(options, string_key(names_collection_k, names[index], key),
                                                            &record);
        ustore_collection_id_t id = ustore_collection_main_k;
        ustore_modality_t modality = ustore_modality_any_k;
        bool const present = status.ok();
        if (!present && !status.IsNotFound() && export_error(status, arguments.error)) return;
        if (present && !read_name_record(record, id, modality)) {
            *arguments.error = {ustore_error_malformed_k, "A name holds no collection record"};
            return;
        }
        ids[index] = id;
        modalities[index] = modality;
        presences[index] = present;
    }
    *arguments.collections = ids.data();
}

void database_control(ustore_database_control_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!arguments.request || !arguments.response) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need a request and an output for the response"};
        return;
    }

    *arguments.response = nullptr;
    if (std::string_view {arguments.request} != "info") {
        *arguments.error = {ustore_error_invalid_argument_k, "Unknown request: expected info"};
        return;
    }

    linked_memory_lock_t arena = linked_memory(arguments.arena, ustore_options_default_k, arguments.error);
    if (arguments.error->status) return;
    constexpr std::size_t response_capacity_k = 64;
    char* const response = arena.alloc<char>(response_capacity_k, arguments.error).data();
    if (arguments.error->status) return;
    auto const written = fmt::format_to_n(response, response_capacity_k - 1,
                                          R"({{"engine":"leveldb","version":"{}.{}"}})", ::leveldb::kMajorVersion,
                                          ::leveldb::kMinorVersion);
    *written.out = '\0';
    *arguments.response = response;
}

#pragma endregion Collections

#pragma region Strings

void strings_write(ustore_strings_write_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!arguments.tasks_count) return;
    if (!arguments.strings) {
        *arguments.error = {ustore_error_invalid_argument_k, "No strings were provided"};
        return;
    }

    database_t& database = *handle_cast<database_t>(arguments.db);
    strided_iterator<ustore_collection_id_t const> const collections {arguments.collections,
                                                                      arguments.collections_stride};
    strings_arg_t const texts {{arguments.strings, arguments.strings_stride},
                               {arguments.strings_lengths, arguments.strings_lengths_stride}};
    contents_arg_t const contents {bits_view_t {arguments.values_presences},
                                   {arguments.values_offsets, arguments.values_offsets_stride},
                                   {arguments.values_lengths, arguments.values_lengths_stride},
                                   {arguments.values, arguments.values_stride},
                                   arguments.tasks_count};
    *arguments.error = resolve_collections(database, collections, arguments.tasks_count, keying_t::strings_k);
    if (arguments.error->status) return;

    ::leveldb::WriteBatch batch;
    std::string key;
    for (std::size_t index = 0; index != arguments.tasks_count; ++index) {
        ustore_collection_id_t const collection = collections ? collections[index] : ustore_collection_main_k;
        value_view_t const content = contents[index];
        string_key(collection, texts[index], key);
        if (!content) batch.Delete(key);
        else batch.Put(key, to_slice(content));
    }
    ::leveldb::WriteOptions options;
    options.sync = arguments.options & ustore_option_write_flush_k;
    export_error(database.native->Write(options, &batch), arguments.error);
}

void strings_read(ustore_strings_read_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!arguments.tasks_count) return;
    if (!arguments.strings) {
        *arguments.error = {ustore_error_invalid_argument_k, "No strings were provided"};
        return;
    }

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;
    database_t& database = *handle_cast<database_t>(arguments.db);
    ::leveldb::ReadOptions options;
    use_snapshot(arguments.snapshot, options);

    strided_iterator<ustore_collection_id_t const> const collections {arguments.collections,
                                                                      arguments.collections_stride};
    strings_arg_t const texts {{arguments.strings, arguments.strings_stride},
                               {arguments.strings_lengths, arguments.strings_lengths_stride}};
    *arguments.error = resolve_collections(database, collections, arguments.tasks_count, keying_t::strings_k);
    if (arguments.error->status) return;

    growing_tape_t exported {arena};
    exported.reserve(arguments.tasks_count, arguments.error);
    if (arguments.error->status) return;
    std::string key;
    std::string value;
    for (std::size_t index = 0; index != arguments.tasks_count; ++index) {
        ustore_collection_id_t const collection = collections ? collections[index] : ustore_collection_main_k;
        native_status_t const status = database.native->Get(options, string_key(collection, texts[index], key), &value);
        if (!status.IsNotFound() && export_error(status, arguments.error)) return;
        exported.push_back(status.ok() ? to_value(value) : value_view_t {}, arguments.error);
        if (arguments.error->status) return;
    }

    if (arguments.presences) *arguments.presences = exported.presences().get();
    if (arguments.offsets) *arguments.offsets = exported.offsets().begin().get();
    if (arguments.lengths) *arguments.lengths = exported.lengths().begin().get();
    if (arguments.values) *arguments.values = reinterpret_cast<ustore_byte_t*>(exported.contents().begin().get());
}

void strings_scan(ustore_strings_scan_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!arguments.tasks_count) return;
    if (!arguments.count_limits || !arguments.strings) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need the limits and an output for the strings"};
        return;
    }

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;
    database_t& database = *handle_cast<database_t>(arguments.db);
    ::leveldb::ReadOptions options;
    options.fill_cache = false;
    use_snapshot(arguments.snapshot, options);

    std::size_t const count = arguments.tasks_count;
    strided_iterator<ustore_collection_id_t const> const collections {arguments.collections,
                                                                      arguments.collections_stride};
    strings_arg_t const prefixes {{arguments.prefixes, arguments.prefixes_stride},
                                  {arguments.prefixes_lengths, arguments.prefixes_lengths_stride}};
    strings_arg_t const starts {{arguments.start_strings, arguments.start_strings_stride},
                                {arguments.start_strings_lengths, arguments.start_strings_lengths_stride}};
    strided_iterator<ustore_length_t const> const limits {arguments.count_limits, arguments.count_limits_stride};
    *arguments.error = resolve_collections(database, collections, count, keying_t::strings_k);
    if (arguments.error->status) return;

    auto offsets = arena.alloc_or_dummy(count + 1, arguments.error, arguments.offsets);
    auto counts = arena.alloc_or_dummy(count, arguments.error, arguments.counts);
    if (arguments.error->status) return;
    growing_tape_t found {arena};
    std::string first;
    std::string bound;
    iterator_pointer_t iterator {database.native->NewIterator(options)};
    for (std::size_t task = 0; task != count; ++task) {
        ustore_collection_id_t const collection = collections ? collections[task] : ustore_collection_main_k;
        std::string_view const prefix = arguments.prefixes ? prefixes[task] : std::string_view {};
        std::string_view const start = arguments.start_strings ? starts[task] : std::string_view {};
        std::size_t const found_before = found.lengths().size();
        offsets[task] = static_cast<ustore_length_t>(found_before);

        // The stored key of every string starting with the prefix starts with the stored key of the prefix
        ::leveldb::Slice const wanted = string_key(collection, prefix, bound);
        for (iterator->Seek(string_key(collection, start > prefix ? start : prefix, first));
             iterator->Valid() && iterator->key().starts_with(wanted) &&
             found.lengths().size() - found_before != limits[task];
             iterator->Next()) {
            ::leveldb::Slice const text = tail_of(iterator->key());
            found.push_back(to_value(text), arguments.error);
            if (arguments.error->status) return;
        }
        if (export_error(iterator->status(), arguments.error)) return;
        counts[task] = static_cast<ustore_length_t>(found.lengths().size() - found_before);
    }
    offsets[count] = static_cast<ustore_length_t>(found.lengths().size());
    if (arguments.strings_offsets) *arguments.strings_offsets = found.offsets().begin().get();
    *arguments.strings = reinterpret_cast<ustore_char_t*>(found.contents().begin().get());
}

#pragma endregion Strings

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

void database_free(ustore_database_t database) noexcept { delete handle_cast<database_t>(database); }

void snapshot_free(ustore_snapshot_t handle) noexcept {
    snapshot_t* const snapshot = handle_cast<snapshot_t>(handle);
    snapshot->database->native->ReleaseSnapshot(snapshot->native);
    delete snapshot;
}

#pragma endregion Memory

engine_t const engine_k {
    .database_capabilities = &guarded<ustore_database_capabilities_t, &database_capabilities>,
    .database_control = &guarded<ustore_database_control_t, &database_control>,
    .snapshot_init = &guarded<ustore_snapshot_init_t, &snapshot_init>,
    .snapshot_export = &guarded<ustore_snapshot_export_t, &snapshot_export>,
    .read = &guarded<ustore_read_t, &read>,
    .write = &guarded<ustore_write_t, &write>,
    .scan = &guarded<ustore_scan_t, &scan>,
    .sample = &guarded<ustore_sample_t, &sample>,
    .measure = &guarded<ustore_measure_t, &measure>,
    .collection_create = &guarded<ustore_collection_create_t, &collection_create>,
    .collection_drop = &guarded<ustore_collection_drop_t, &collection_drop>,
    .collection_list = &guarded<ustore_collection_list_t, &collection_list>,
    .collection_find = &guarded<ustore_collection_find_t, &collection_find>,
    .strings_write = &guarded<ustore_strings_write_t, &strings_write>,
    .strings_read = &guarded<ustore_strings_read_t, &strings_read>,
    .strings_scan = &guarded<ustore_strings_scan_t, &strings_scan>,
    .graph_find_edges = &graph_find_edges_in_vertex_values,
    .graph_upsert_edges = &graph_upsert_edges_in_vertex_values,
    .graph_remove_edges = &graph_remove_edges_in_vertex_values,
    .graph_upsert_vertices = &graph_upsert_vertices_in_vertex_values,
    .graph_remove_vertices = &graph_remove_vertices_in_vertex_values,
    .transaction_init = &guarded<ustore_transaction_init_t, &transaction_init>,
    .transaction_stage = &guarded<ustore_transaction_stage_t, &transaction_stage>,
    .transaction_commit = &guarded<ustore_transaction_commit_t, &transaction_commit>,
    .transaction_free = &transaction_free,
    .snapshot_free = &snapshot_free,
    .database_free = &database_free,
};

} // namespace unum::ustore::leveldb

using namespace unum::ustore;
using namespace unum::ustore::leveldb;

void ustore_database_init_leveldb(ustore_database_init_leveldb_t* arguments) {
    guarded<ustore_database_init_leveldb_t, &database_init>(*arguments);
}
