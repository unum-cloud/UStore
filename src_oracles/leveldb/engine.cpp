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
#include <utility>      // `std::exchange`

#include <leveldb/db.h>
#include <leveldb/write_batch.h>

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "ustore/db.h"
#include "ustore/ranges.hpp" // `place_batch_t`, `strided`
#include "ustore/status.hpp" // `expected`, `status_t`

#include "ustore/utilities/algorithm.hpp"      // `reservoir_sample_iterator`
#include "ustore/utilities/blocking_files.hpp" // `directory_exists`
#include "ustore/utilities/ram_buffers.hpp"    // `arena_allocate`, `growth_status`

#include "engine.hpp"             // `engine_t`, `handle_t`, `handle_cast`
#include "utilities/batches.hpp"  // `strings_arg_t`, `validate_write`, `places_of`
#include "utilities/outputs.hpp"  // `read_outputs_t`, `scan_outputs_t`, `collection_list_outputs_t`
#include "utilities/progress.hpp" // `progress_t`, `progress_cancelled_k`

namespace unum::ustore::oracles::leveldb {
using namespace unum::ustore::utilities;

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

    /** Options the database opened with, reused to create the databases snapshots save. */
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
    return {reinterpret_cast<ustore_byte_t const*>(slice.data()), static_cast<ustore_length_t>(slice.size())};
}

/** The status LevelDB's @p status stands for, success included. */
status_t status_of(native_status_t const& status) noexcept {
    if (status.ok()) return {};
    if (status.IsCorruption()) return {status_code_t::malformed_k, "LevelDB found corrupted data"};
    if (status.IsIOError()) return {status_code_t::io_k, "LevelDB failed an I/O operation"};
    if (status.IsInvalidArgument()) return {status_code_t::invalid_argument_k, "LevelDB rejected an argument"};
    if (status.IsNotFound()) return {status_code_t::not_found_k, "LevelDB found no such entry"};
    if (status.IsNotSupportedError()) return {status_code_t::unsupported_k, "LevelDB does not support the operation"};
    return {status_code_t::internal_k, "LevelDB failed"};
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

/** Points @p options at the snapshot @p view names, leaving them at the head for any other view. */
void use_snapshot(view_t const& view, ::leveldb::ReadOptions& options) noexcept {
    if (view.snapshot) options.snapshot = handle_cast<snapshot_t>(view.snapshot)->native;
}

/** The bytes of memory LevelDB estimates the database holds. */
expected<std::uint64_t> memory_usage(database_t& database) {
    std::uint64_t memory_bytes = 0;
    std::string reported;
    if (!database.native->GetProperty("leveldb.approximate-memory-usage", &reported))
        return status_t {status_code_t::internal_k, "LevelDB reported no memory usage"};
    char const* const reported_end = reported.data() + reported.size();
    auto const [parsed_end, parse_error] = std::from_chars(reported.data(), reported_end, memory_bytes);
    if (parse_error != std::errc {} || parsed_end != reported_end)
        return status_t {status_code_t::internal_k, "LevelDB reported no readable memory usage"};
    return memory_bytes;
}

/** The options of a write that syncs when @p durability asks for a flush. */
::leveldb::WriteOptions write_options_for(ustore_durability_t durability) noexcept {
    ::leveldb::WriteOptions options;
    options.sync = durability == ustore_durability_flushed_k;
    return options;
}

#pragma endregion Database

#pragma region Reads and Writes

/** From this many keys one iterator, sharing a version and its loaded blocks, beats a `Get` that copies each value. */
constexpr std::size_t iterator_read_threshold_k = 16;

status_t write_one(database_t& database, place_batch_t const& places, strings_arg_t const& values,
                   ::leveldb::WriteOptions const& options) {

    place_t const place = places[0];
    uuid_key_t const key = uuid_key(place.collection, place.key.uuid);
    return status_of(database.native->Put(options, key.slice(), to_slice(values[0])));
}

status_t write_many(database_t& database, place_batch_t const& places, strings_arg_t const& values,
                    ::leveldb::WriteOptions const& options) {

    ::leveldb::WriteBatch batch;
    for (std::size_t task_index = 0; task_index != places.size(); ++task_index) {
        place_t const place = places[task_index];
        uuid_key_t const key = uuid_key(place.collection, place.key.uuid);
        batch.Put(key.slice(), to_slice(values[task_index]));
    }
    return status_of(database.native->Write(options, &batch));
}

/** Appends @p value to @p outputs through a copy in @p scratch, since LevelDB hands it out of a buffer the next read
 *  reuses, or a missing value. */
status_t append_staged(read_outputs_t& outputs, value_view_t value, ustore_arena_t scratch) noexcept {
    if (!value) {
        outputs.append_missing();
        return {};
    }
    auto [staged, status] = arena_allocate<byte_t>(scratch, value.size());
    if (status.failed()) return status;
    if (!staged.empty()) std::memcpy(staged.data(), value.data(), value.size());
    outputs.append(value_view_t {staged.data(), value.size()});
    return {};
}

/** Reads every place with its own `Get`, handing each value to @p enumerator with its index. */
template <typename value_enumerator_type_>
status_t get_each(database_t& database, place_batch_t const& places, ::leveldb::ReadOptions const& options,
                  value_enumerator_type_&& enumerator) {

    // `Get` writes only into a string, so one buffer serves the whole batch
    std::string value;
    for (std::size_t task_index = 0; task_index != places.size(); ++task_index) {
        auto place = places[task_index];
        uuid_key_t const key = uuid_key(place.collection, place.key.uuid);
        native_status_t const status = database.native->Get(options, key.slice(), &value);
        if (!status.ok() && !status.IsNotFound()) return status_of(status);
        value_view_t const found = status.ok() ? to_value(value) : value_view_t {};
        if (status_t const enumerated = enumerator(task_index, found); enumerated.failed()) return enumerated;
    }
    return {};
}

/** Reads every place through one iterator, handing each value to @p enumerator straight from its block. */
template <typename value_enumerator_type_>
status_t seek_each(database_t& database, place_batch_t const& places, ::leveldb::ReadOptions const& options,
                   value_enumerator_type_&& enumerator) {

    iterator_pointer_t iterator {database.native->NewIterator(options)};
    for (std::size_t task_index = 0; task_index != places.size(); ++task_index) {
        auto place = places[task_index];
        uuid_key_t const key = uuid_key(place.collection, place.key.uuid);
        iterator->Seek(key.slice());
        bool const present = iterator->Valid() && iterator->key().compare(key.slice()) == 0;
        if (!present && !iterator->status().ok()) return status_of(iterator->status());
        value_view_t const found = present ? to_value(iterator->value()) : value_view_t {};
        if (status_t const enumerated = enumerator(task_index, found); enumerated.failed()) return enumerated;
    }
    return {};
}

#pragma endregion Reads and Writes

#pragma region Lifetime

status_t database_open(ustore_database_open_leveldb_t const& arguments, view_t const&) {
    if (!arguments.database) return {status_code_t::invalid_argument_k, "Need an output for the database"};
    *arguments.database = nullptr;
    if (!arguments.directory.length) return {status_code_t::invalid_argument_k, "A database needs a directory"};

    std::string const root(string_of(arguments.directory));
    auto [root_exists, directory_exists_status] = directory_exists(root.c_str());
    if (directory_exists_status.failed()) return directory_exists_status;
    if (!root_exists) return {status_code_t::not_found_k, "Root isn't a directory"};

    std::unique_ptr<database_t> database {new (std::nothrow) database_t};
    if (!database) return {status_code_t::out_of_memory_k, "Failed to allocate the database"};
    database->options.compression = ::leveldb::kNoCompression;
    database->options.create_if_missing = true;

    native_t* native = nullptr;
    native_status_t const status = native_t::Open(database->options, root, &native);
    if (!status.ok()) return {status_code_t::io_k, "Failed to open LevelDB"};
    database->native.reset(native);
    database->engine = &engine_k;
    *arguments.database = c_handle<ustore_database_t>(database.release());
    return {};
}

/** Reports the engine with LevelDB's estimates of the memory it holds and of the bytes its files take, and no count of
 *  the keys, which LevelDB keeps none of. */
status_t database_describe(ustore_database_describe_t const& arguments, view_t const&) {
    database_t& database = *handle_cast<database_t>(arguments.database);
    auto [memory_bytes, memory_status] = memory_usage(database);
    if (memory_status.failed()) return memory_status;
    // Every stored key, the reserved catalog and names included, sorts below one byte past the largest UUID
    std::string const past_last(collection_bytes_k + 1, '\xFF');
    ::leveldb::Range const everything(::leveldb::Slice(), past_last);
    std::uint64_t disk_bytes = 0;
    database.native->GetApproximateSizes(&everything, 1, &disk_bytes);
    *arguments.description = {
        .capabilities = ustore_capabilities_t(ustore_capabilities_collections_k | ustore_capabilities_snapshots_k),
        .engine = ustore_engine_leveldb_k,
        .isolation = ustore_isolation_snapshot_k,
        .memory_bytes = memory_bytes,
        .disk_bytes = disk_bytes,
    };
    return {};
}

#pragma endregion Lifetime

#pragma region Snapshots

status_t snapshot_init(ustore_snapshot_init_t const& arguments, view_t const&) {
    if (!arguments.snapshot) return {status_code_t::invalid_argument_k, "Need an output for the snapshot"};

    database_t& database = *handle_cast<database_t>(arguments.database);
    std::unique_ptr<snapshot_t> snapshot {new (std::nothrow) snapshot_t};
    if (!snapshot) return {status_code_t::out_of_memory_k, "Failed to allocate the snapshot handle"};
    snapshot->native = database.native->GetSnapshot();
    if (!snapshot->native) return {status_code_t::internal_k, "Failed to take a snapshot"};
    snapshot->engine = &engine_k;
    snapshot->database = &database;
    // LevelDB keeps a snapshot's sequence number behind its private interface
    if (arguments.read_stamp) *arguments.read_stamp = 0;
    *arguments.snapshot = c_handle<ustore_snapshot_t>(snapshot.release());
    return {};
}

status_t snapshot_save(ustore_snapshot_save_t const& arguments, view_t const& view) {
    database_t& database = *handle_cast<database_t>(database_of(arguments));
    ::leveldb::ReadOptions scan_options;
    scan_options.fill_cache = false;
    use_snapshot(view, scan_options);
    iterator_pointer_t iterator {database.native->NewIterator(scan_options)};

    // The total costs one more walk of the keyspace, which only a callback needs
    ustore_size_t total = 0;
    for (iterator->SeekToFirst(); arguments.progress && iterator->Valid(); iterator->Next())
        total += iterator->key().size() + iterator->value().size();
    if (!iterator->status().ok()) return status_of(iterator->status());
    progress_t progress {arguments.progress, arguments.progress_state, total};

    std::string const path(string_of(arguments.path));
    native_t* saved_native = nullptr;
    native_status_t status = native_t::Open(database.options, path, &saved_native);
    if (!status.ok()) return {status_code_t::io_k, "Failed to create the saved database"};
    std::unique_ptr<native_t> saved {saved_native};

    // Batches the size of a memtable keep writes few without holding the whole copy in memory
    ::leveldb::WriteBatch batch;
    ustore_size_t batched = 0;
    bool carries_on = true;
    for (iterator->SeekToFirst(); iterator->Valid() && status.ok() && carries_on; iterator->Next()) {
        batch.Put(iterator->key(), iterator->value());
        batched += iterator->key().size() + iterator->value().size();
        if (batch.ApproximateSize() < database.options.write_buffer_size) continue;
        status = saved->Write(::leveldb::WriteOptions(), &batch);
        carries_on = progress.advance(std::exchange(batched, 0));
        batch.Clear();
    }
    if (status.ok()) status = iterator->status();
    if (status.ok() && carries_on) status = saved->Write(::leveldb::WriteOptions(), &batch);
    if (status.ok() && carries_on) carries_on = progress.advance(batched);
    iterator.reset();
    saved.reset();

    if (status.ok() && carries_on) return {};
    ::leveldb::DestroyDB(path, database.options);
    return carries_on ? status_of(status) : progress_cancelled_k;
}

#pragma endregion Snapshots

#pragma region Reads and Writes

status_t blobs_write(ustore_blobs_write_t const& arguments, strings_arg_t const& values, view_t const& view) {
    database_t& database = *handle_cast<database_t>(arguments.database);
    place_batch_t const places = places_of(arguments);

    status_t const validated = validate_write(view.transaction, places, values, arguments.durability, view.watch);
    if (validated.failed() || !arguments.count) return validated;
    status_t const resolved = resolve_collections(database, places.collections_begin, arguments.count,
                                                  keying_t::uuids_k);
    if (resolved.failed()) return resolved;

    ::leveldb::WriteOptions const options = write_options_for(arguments.durability);
    if (arguments.count == 1) return write_one(database, places, values, options);
    return write_many(database, places, values, options);
}

status_t blobs_remove(ustore_blobs_remove_t const& arguments, view_t const& view) {
    database_t& database = *handle_cast<database_t>(arguments.database);
    place_batch_t const places = places_of(arguments);

    status_t const validated = validate_remove(view.transaction, places, arguments.durability, view.watch);
    if (validated.failed() || !arguments.count) return validated;
    status_t const resolved = resolve_collections(database, places.collections_begin, arguments.count,
                                                  keying_t::uuids_k);
    if (resolved.failed()) return resolved;

    ::leveldb::WriteBatch batch;
    for (std::size_t task_index = 0; task_index != places.count; ++task_index) {
        place_t const place = places[task_index];
        batch.Delete(uuid_key(place.collection, place.key.uuid).slice());
    }
    return status_of(database.native->Write(write_options_for(arguments.durability), &batch));
}

status_t blobs_read(ustore_blobs_read_t const& arguments, view_t const& view) {
    if (!arguments.count) return {};

    database_t& database = *handle_cast<database_t>(arguments.database);
    place_batch_t const places = places_of(arguments);

    if (status_t const status = validate_read(places); status.failed()) return status;
    status_t const resolved = resolve_collections(database, places.collections_begin, arguments.count,
                                                  keying_t::uuids_k);
    if (resolved.failed()) return resolved;

    ::leveldb::ReadOptions options;
    use_snapshot(view, options);

    auto [outputs, outputs_status] = read_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;

    // Metadata and data come in one pass, as reading from disk is expensive
    auto data_enumerator = [&](std::size_t, value_view_t value) noexcept -> status_t {
        return append_staged(outputs, value, view.scratch);
    };
    status_t const status = places.count < iterator_read_threshold_k
                                ? get_each(database, places, options, data_enumerator)
                                : seek_each(database, places, options, data_enumerator);
    if (status.failed()) return status;
    return outputs.finish(arguments);
}

status_t blobs_scan(ustore_blobs_scan_t const& arguments, view_t const& view) {
    database_t& database = *handle_cast<database_t>(arguments.database);
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    scan_batch_t const scans {collections, strided(arguments.start_keys), arguments.wanted, arguments.count};

    if (status_t const status = validate_scan(arguments); status.failed() || !arguments.count) return status;
    status_t const resolved = resolve_collections(database, collections, arguments.count, keying_t::uuids_k);
    if (resolved.failed()) return resolved;

    ::leveldb::ReadOptions options;
    options.fill_cache = false;
    use_snapshot(view, options);

    iterator_pointer_t iterator {database.native->NewIterator(options)};
    auto [outputs, outputs_status] = scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    progress_t progress {arguments.progress, arguments.progress_state, scans.count};
    for (std::size_t task_index = 0; task_index != scans.count; ++task_index) {
        scan_task_t const task = scans[task_index];
        uuid_key_t const start = uuid_key(task.collection, task.start_key.uuid);
        ::leveldb::Slice const prefix = prefix_of(task.collection);
        outputs.open_task(task_index);
        std::size_t const scan_end = outputs.size() + task.limit;
        for (iterator->Seek(start.slice());
             iterator->Valid() && iterator->key().starts_with(prefix) && outputs.size() != scan_end; iterator->Next())
            if (status_t const status = outputs.push(ustore_key_t {tail_uuid_of(iterator->key())}); status.failed())
                return status;
        if (!iterator->status().ok()) return status_of(iterator->status());
        if (!progress.advance(1)) return progress_cancelled_k;
    }
    return outputs.finish(arguments);
}

status_t blobs_sample(ustore_blobs_sample_t const& arguments, view_t const& view) {
    if (!arguments.count) return {};
    if (!arguments.offsets || !arguments.keys)
        return {status_code_t::invalid_argument_k, "Need outputs for the offsets and the keys"};

    database_t& database = *handle_cast<database_t>(arguments.database);
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    sample_batch_t const samples {collections, arguments.wanted, arguments.count};
    status_t const resolved = resolve_collections(database, collections, arguments.count, keying_t::uuids_k);
    if (resolved.failed()) return resolved;

    ::leveldb::ReadOptions options;
    options.fill_cache = false;
    use_snapshot(view, options);

    iterator_pointer_t iterator {database.native->NewIterator(options)};
    auto [outputs, outputs_status] = scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    for (std::size_t task_index = 0; task_index != samples.count; ++task_index) {
        sample_task_t const task = samples[task_index];
        outputs.open_task(task_index);

        ::leveldb::Slice const prefix = prefix_of(task.collection);
        collection_walk_t walk {*iterator, prefix};
        if (status_t const status = reservoir_sample_iterator(walk, task.limit, outputs); status.failed())
            return status;
        if (!iterator->status().ok()) return status_of(iterator->status());
    }
    return outputs.finish(arguments);
}

status_t blobs_measure(ustore_blobs_measure_t const& arguments, view_t const&) {
    if (!arguments.count) return {};
    database_t& database = *handle_cast<database_t>(arguments.database);
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    strided_iterator<ustore_key_t const> const start_keys = strided(arguments.start_keys);
    strided_iterator<ustore_key_t const> const end_keys = strided(arguments.end_keys);
    status_t const resolved = resolve_collections(database, collections, arguments.count, keying_t::uuids_k);
    if (resolved.failed()) return resolved;
    auto [outputs, outputs_status] = measure_outputs_t::make(arguments);
    if (outputs_status.failed()) return outputs_status;

    // Memory usage describes the whole database, so one property read serves every task
    auto [memory_bytes, memory_status] = memory_usage(database);
    if (memory_status.failed()) return memory_status;

    for (std::size_t task_index = 0; task_index != arguments.count; ++task_index) {
        outputs.min_cardinalities[task_index] = static_cast<ustore_size_t>(0);
        outputs.max_cardinalities[task_index] = static_cast<ustore_size_t>(0);
        outputs.min_value_bytes[task_index] = static_cast<ustore_size_t>(0);
        outputs.max_value_bytes[task_index] = static_cast<ustore_size_t>(0);

        ustore_collection_id_t const collection = collections ? collections[task_index] : ustore_collection_main_k;
        uuid_key_t const start_key = uuid_key(collection, (start_keys ? start_keys[task_index] : smallest_key_k).uuid);
        uuid_key_t const end_key = uuid_key(collection, (end_keys ? end_keys[task_index] : largest_key_k).uuid);
        ::leveldb::Range const range(start_key.slice(), end_key.slice());
        std::uint64_t approximate_size = 0;
        database.native->GetApproximateSizes(&range, 1, &approximate_size);
        outputs.min_space_usages[task_index] = approximate_size;
        outputs.max_space_usages[task_index] = memory_bytes;
    }
    outputs.finish(arguments);
    return {};
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
        if (mode == ustore_drop_values_k) batch.Put(iterator->key(), ::leveldb::Slice());
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
    return status_of(clear_collection(database, collection, ustore_drop_pairs_k));
}

status_t collection_create(ustore_collection_create_t const& arguments, view_t const&) {
    if (!arguments.collection) return {status_code_t::invalid_argument_k, "Need an output for the collection"};
    if (arguments.modality > ustore_modality_pages_k) return {status_code_t::invalid_argument_k, "Unknown modality"};
    std::string_view const name = string_of(arguments.name);
    if (arguments.name.data && name.empty())
        return {status_code_t::invalid_argument_k, "A name is never empty, as the main collection has none"};

    // The entry and the name reach the log in one batch, so a crash keeps both or neither
    database_t& database = *handle_cast<database_t>(arguments.database);
    std::lock_guard lock {database.catalog_mutex};
    std::string name_key;
    if (!name.empty()) {
        std::string record;
        native_status_t const looked_up = database.native->Get(::leveldb::ReadOptions(),
                                                               string_key(names_collection_k, name, name_key), &record);
        if (looked_up.ok()) return {status_code_t::already_exists_k, "A collection already carries the name"};
        if (!looked_up.IsNotFound()) return status_of(looked_up);
    }
    auto [id, mint_status] = mint_collection_id();
    if (mint_status.failed()) return mint_status;

    uuid_key_t const listed = uuid_key(catalog_collection_k, id.uuid);
    std::string entry(1, static_cast<char>(arguments.modality));
    entry.append(name);
    name_record_t const record {id, arguments.modality};
    ::leveldb::WriteBatch batch;
    batch.Put(listed.slice(), entry);
    if (!name.empty()) batch.Put(name_key, ::leveldb::Slice(record.bytes, sizeof(record.bytes)));
    ::leveldb::WriteOptions synced;
    synced.sync = true;
    if (status_t const status = status_of(database.native->Write(synced, &batch)); status.failed()) return status;
    *arguments.collection = id;
    return {};
}

status_t collection_drop(ustore_collection_drop_t const& arguments, view_t const&) {
    if (arguments.mode != ustore_drop_collection_k && arguments.mode != ustore_drop_pairs_k &&
        arguments.mode != ustore_drop_values_k)
        return {status_code_t::invalid_argument_k, "Unknown drop mode"};
    if (arguments.collection == ustore_collection_main_k && arguments.mode == ustore_drop_collection_k)
        return {status_code_t::invalid_argument_k, "The main collection can only be cleared"};

    database_t& database = *handle_cast<database_t>(arguments.database);
    if (arguments.mode == ustore_drop_collection_k) {
        std::lock_guard lock {database.catalog_mutex};
        return drop_collection(database, arguments.collection);
    }
    std::string held;
    if (status_t const status = look_up(database, arguments.collection, held).status(); status.failed()) return status;
    return status_of(clear_collection(database, arguments.collection, arguments.mode));
}

status_t collection_list(ustore_collection_list_t const& arguments, view_t const& view) {
    if (!arguments.written) return {status_code_t::invalid_argument_k, "Need an output for the collections written"};

    database_t& database = *handle_cast<database_t>(arguments.database);
    ::leveldb::ReadOptions options;
    options.fill_cache = false;
    use_snapshot(view, options);

    // The catalog lists every collection but the main one, keyed by its ID
    ::leveldb::Slice const catalog = prefix_of(catalog_collection_k);
    iterator_pointer_t iterator {database.native->NewIterator(options)};
    collection_list_outputs_t outputs {view.scratch};
    for (iterator->Seek(catalog); iterator->Valid() && iterator->key().starts_with(catalog); iterator->Next()) {
        ::leveldb::Slice const listed = iterator->key();
        auto [entry, read_status] = read_catalog_entry(iterator->value());
        if (read_status.failed() || listed.size() != sizeof(uuid_key_t))
            return {status_code_t::malformed_k, "The catalog entry of a collection is malformed"};
        value_view_t const name = entry.name.empty() ? value_view_t {} : value_view_t {entry.name};
        if (status_t const status = outputs.push(ustore_collection_id_t {tail_uuid_of(listed)}, entry.modality, name);
            status.failed())
            return status;
    }
    if (!iterator->status().ok()) return status_of(iterator->status());
    return outputs.finish(arguments);
}

status_t collection_find(ustore_collection_find_t const& arguments, strings_arg_t const& names, view_t const& view) {
    if (!arguments.count) return {};
    if (!names || !arguments.collections)
        return {status_code_t::invalid_argument_k, "Need the names and an output for the identifiers"};

    database_t& database = *handle_cast<database_t>(arguments.database);
    ::leveldb::ReadOptions options;
    use_snapshot(view, options);

    auto [outputs, outputs_status] = collection_find_outputs_t::make(arguments);
    if (outputs_status.failed()) return outputs_status;

    std::string key;
    std::string record;
    for (std::size_t index = 0; index != arguments.count; ++index) {
        native_status_t const status = database.native->Get(
            options, string_key(names_collection_k, string_of(names[index]), key), &record);
        bool const present = status.ok();
        if (!present && !status.IsNotFound()) return status_of(status);
        auto [named, named_status] = present ? read_name_record(record) : name_record_t {};
        if (named_status.failed()) return named_status;
        outputs.assign(index, named.id(), named.modality(), present);
    }
    outputs.finish(arguments);
    return {};
}

/** Clears every collection as the mode asks in one walk of the whole keyspace, in batches the size of a memtable, the
 *  last one synced, where dropping the collections removes the catalog and the names with them. */
status_t database_clear(ustore_database_clear_t const& arguments, view_t const&) {
    ustore_drop_mode_t const mode = arguments.mode;
    if (mode != ustore_drop_collection_k && mode != ustore_drop_pairs_k && mode != ustore_drop_values_k)
        return {status_code_t::invalid_argument_k, "Unknown drop mode"};

    database_t& database = *handle_cast<database_t>(arguments.database);
    std::lock_guard lock {database.catalog_mutex};
    ::leveldb::WriteOptions buffered;
    ::leveldb::WriteOptions synced;
    synced.sync = true;
    ::leveldb::WriteBatch batch;
    native_status_t status;
    ::leveldb::Slice const catalog = prefix_of(catalog_collection_k);
    ::leveldb::Slice const names = prefix_of(names_collection_k);
    iterator_pointer_t iterator {database.native->NewIterator(::leveldb::ReadOptions())};
    for (iterator->SeekToFirst(); status.ok() && iterator->Valid(); iterator->Next()) {
        ::leveldb::Slice const key = iterator->key();
        if (mode == ustore_drop_collection_k) batch.Delete(key);
        else if (key.starts_with(catalog) || key.starts_with(names)) continue;
        else if (mode == ustore_drop_values_k) batch.Put(key, ::leveldb::Slice());
        else batch.Delete(key);
        if (batch.ApproximateSize() < database.options.write_buffer_size) continue;
        status = database.native->Write(buffered, &batch);
        batch.Clear();
    }
    if (status.ok()) status = iterator->status();
    if (status.ok()) status = database.native->Write(synced, &batch);
    return status_of(status);
}

/** Compacts the whole keyspace one collection at a time, in key order from the main one to the names and the catalog,
 *  each flushing the memtable ahead of it, and reports one unit per collection, as LevelDB measures no memtable. */
status_t database_compact(ustore_database_compact_t const& arguments, view_t const&) {
    database_t& database = *handle_cast<database_t>(arguments.database);
    // The catalog lists every collection but the main one in the order of their identifiers, which is key order
    st::basic_vector<ustore_collection_id_t> bounds;
    st::status_t grown = bounds.emplace_back(ustore_collection_main_k);
    ::leveldb::Slice const catalog = prefix_of(catalog_collection_k);
    iterator_pointer_t iterator {database.native->NewIterator(::leveldb::ReadOptions())};
    for (iterator->Seek(catalog); st::succeeded(grown) && iterator->Valid() && iterator->key().starts_with(catalog);
         iterator->Next())
        grown = bounds.emplace_back(ustore_collection_id_t {tail_uuid_of(iterator->key())});
    if (!iterator->status().ok()) return status_of(iterator->status());
    iterator.reset();
    if (st::succeeded(grown)) grown = bounds.emplace_back(names_collection_k);
    if (st::succeeded(grown)) grown = bounds.emplace_back(catalog_collection_k);
    if (st::failed(grown)) return growth_status(grown);

    // A run spans from the prefix of its collection to the prefix of the next one, and the last to the keyspace's end
    progress_t progress {arguments.progress, arguments.progress_state, bounds.size()};
    for (std::size_t index = 0; index != bounds.size(); ++index) {
        bool const last = index + 1 == bounds.size();
        ::leveldb::Slice const begin = prefix_of(bounds[index]);
        ::leveldb::Slice const end = last ? ::leveldb::Slice() : prefix_of(bounds[index + 1]);
        database.native->CompactRange(&begin, last ? nullptr : &end);
        if (!progress.advance(1)) return progress_cancelled_k;
    }
    return {};
}

#pragma endregion Collections

#pragma region Strings

status_t strings_write(ustore_strings_write_t const& arguments, strings_arg_t const& texts, strings_arg_t const& values,
                       view_t const&) {
    if (!arguments.count) return {};
    if (!texts || !values) return {status_code_t::invalid_argument_k, "Need the strings and their values"};

    database_t& database = *handle_cast<database_t>(arguments.database);
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    status_t const resolved = resolve_collections(database, collections, arguments.count, keying_t::strings_k);
    if (resolved.failed()) return resolved;

    ::leveldb::WriteBatch batch;
    std::string key;
    for (std::size_t index = 0; index != arguments.count; ++index) {
        ustore_collection_id_t const collection = collections ? collections[index] : ustore_collection_main_k;
        batch.Put(string_key(collection, string_of(texts[index]), key), to_slice(values[index]));
    }
    return status_of(database.native->Write(write_options_for(arguments.durability), &batch));
}

status_t strings_remove(ustore_strings_remove_t const& arguments, strings_arg_t const& texts, view_t const&) {
    if (!arguments.count) return {};
    if (!texts) return {status_code_t::invalid_argument_k, "No strings were provided"};

    database_t& database = *handle_cast<database_t>(arguments.database);
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    status_t const resolved = resolve_collections(database, collections, arguments.count, keying_t::strings_k);
    if (resolved.failed()) return resolved;

    ::leveldb::WriteBatch batch;
    std::string key;
    for (std::size_t index = 0; index != arguments.count; ++index) {
        ustore_collection_id_t const collection = collections ? collections[index] : ustore_collection_main_k;
        batch.Delete(string_key(collection, string_of(texts[index]), key));
    }
    return status_of(database.native->Write(write_options_for(arguments.durability), &batch));
}

status_t strings_read(ustore_strings_read_t const& arguments, strings_arg_t const& texts, view_t const& view) {
    if (!arguments.count) return {};
    if (!texts) return {status_code_t::invalid_argument_k, "No strings were provided"};

    database_t& database = *handle_cast<database_t>(arguments.database);
    ::leveldb::ReadOptions options;
    use_snapshot(view, options);

    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    status_t const resolved = resolve_collections(database, collections, arguments.count, keying_t::strings_k);
    if (resolved.failed()) return resolved;

    auto [outputs, outputs_status] = read_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    std::string key;
    std::string value;
    for (std::size_t index = 0; index != arguments.count; ++index) {
        ustore_collection_id_t const collection = collections ? collections[index] : ustore_collection_main_k;
        native_status_t const status = database.native->Get(
            options, string_key(collection, string_of(texts[index]), key), &value);
        if (!status.ok() && !status.IsNotFound()) return status_of(status);
        value_view_t const found = status.ok() ? to_value(value) : value_view_t {};
        if (status_t const appended = append_staged(outputs, found, view.scratch); appended.failed()) return appended;
    }
    return outputs.finish(arguments);
}

status_t strings_scan(ustore_strings_scan_t const& arguments, strings_arg_t const& prefixes,
                      strings_arg_t const& starts, view_t const& view) {
    if (!arguments.count) return {};
    if (!arguments.offsets || !arguments.strings)
        return {status_code_t::invalid_argument_k, "Need outputs for the offsets and the strings"};

    database_t& database = *handle_cast<database_t>(arguments.database);
    ::leveldb::ReadOptions options;
    options.fill_cache = false;
    use_snapshot(view, options);

    std::size_t const count = arguments.count;
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    status_t const resolved = resolve_collections(database, collections, count, keying_t::strings_k);
    if (resolved.failed()) return resolved;

    auto [outputs, outputs_status] = strings_scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    progress_t progress {arguments.progress, arguments.progress_state, count};
    std::string first;
    std::string bound;
    iterator_pointer_t iterator {database.native->NewIterator(options)};
    for (std::size_t task = 0; task != count; ++task) {
        ustore_collection_id_t const collection = collections ? collections[task] : ustore_collection_main_k;
        std::string_view const prefix = prefixes ? string_of(prefixes[task]) : std::string_view {};
        std::string_view const start = starts ? string_of(starts[task]) : std::string_view {};
        std::size_t const scan_end = outputs.size() + arguments.wanted;
        outputs.open_task(task);

        // The stored key of every string starting with the prefix starts with the stored key of the prefix
        ::leveldb::Slice const wanted = string_key(collection, prefix, bound);
        for (iterator->Seek(string_key(collection, start > prefix ? start : prefix, first));
             iterator->Valid() && iterator->key().starts_with(wanted) && outputs.size() != scan_end; iterator->Next())
            if (status_t const status = outputs.push(to_value(tail_of(iterator->key()))); status.failed())
                return status;
        if (!iterator->status().ok()) return status_of(iterator->status());
        if (!progress.advance(1)) return progress_cancelled_k;
    }
    return outputs.finish(arguments);
}

#pragma endregion Strings

#pragma region Memory

void database_free(ustore_database_t database) noexcept { delete handle_cast<database_t>(database); }

void snapshot_free(ustore_snapshot_t handle) noexcept {
    snapshot_t* const snapshot = handle_cast<snapshot_t>(handle);
    snapshot->database->native->ReleaseSnapshot(snapshot->native);
    delete snapshot;
}

#pragma endregion Memory

engine_t const engine_k {
    .database_describe = &guarded<&database_describe>,
    .database_clear = &guarded<&database_clear>,
    .database_compact = &guarded<&database_compact>,
    .snapshot_init = &guarded<&snapshot_init>,
    .snapshot_save = &guarded<&snapshot_save>,
    .collection_list = &guarded<&collection_list>,
    .collection_create = &guarded<&collection_create>,
    .collection_find = &guarded<&collection_find>,
    .collection_drop = &guarded<&collection_drop>,
    .blobs_write = &guarded<&blobs_write>,
    .blobs_remove = &guarded<&blobs_remove>,
    .blobs_read = &guarded<&blobs_read>,
    .blobs_scan = &guarded<&blobs_scan>,
    .blobs_sample = &guarded<&blobs_sample>,
    .blobs_measure = &guarded<&blobs_measure>,
    .docs_write = &docs_write_in_json_values,
    .docs_read = &docs_read_in_json_values,
    .docs_gist = &docs_gist_in_json_values,
    .docs_gather = &docs_gather_in_json_values,
    .graph_read_edges = &graph_read_edges_in_vertex_values,
    .graph_write_edges = &graph_write_edges_in_vertex_values,
    .graph_remove_edges = &graph_remove_edges_in_vertex_values,
    .graph_write_vertices = &graph_write_vertices_in_vertex_values,
    .graph_remove_vertices = &graph_remove_vertices_in_vertex_values,
    .strings_write = &guarded<&strings_write>,
    .strings_remove = &guarded<&strings_remove>,
    .strings_read = &guarded<&strings_read>,
    .strings_scan = &guarded<&strings_scan>,
    .snapshot_free = &snapshot_free,
    .database_free = &database_free,
};

} // namespace unum::ustore::oracles::leveldb

using namespace unum::ustore;
using namespace unum::ustore::oracles::leveldb;

void ustore_database_open_leveldb(ustore_database_open_leveldb_t* arguments) {
    route_init(arguments, &guarded<&database_open, ustore_database_open_leveldb_t, view_t>);
}
