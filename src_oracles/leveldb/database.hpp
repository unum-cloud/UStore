/**
 *  @file src_oracles/leveldb/database.hpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief The database, its keyspace layout, collections and snapshots of the LevelDB oracle, which its calls of every
 *      modality build on.
 */

#pragma once

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

/** The collections system collection, all bytes `0xFF`, which no minted UUIDv7 reaches, as its byte 6 is `0x7X`. */
constexpr ustore_collection_id_t collections_collection_k {
    {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}}};

/** The names system collection, all bytes `0xFF` but the last `0xFE`, which no minted UUIDv7 reaches either. */
constexpr ustore_collection_id_t names_collection_k {
    {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE}}};

/** Bytes every stored key leads with, the UUID of its collection. */
constexpr std::size_t collection_bytes_k = sizeof(ustore_collection_id_t);

/** A stored key of a `struct pairs` or a `struct collections`, which a slice may view for as long as it lives. */
struct uuid_key_t {
    char bytes[collection_bytes_k + sizeof(ustore_uuid_t)];

    [[nodiscard]] ::leveldb::Slice slice() const noexcept { return {bytes, sizeof(bytes)}; }
};

/** The stored key of @p uuid in @p collection: a key of a collection keyed by UUIDs, or a collection the collections
 *  system collection lists. */
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

/** The 16 bytes @p stored holds past its collection's UUID: the key of a pair, or the collection a collections entry
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

/** What the collections system collection records of a collection: `struct collections_entry`. */
struct collections_entry_t {
    ustore_modality_t modality {};

    /** The name the collection carries, empty for none, viewing the bytes the entry was read from. */
    std::string_view name;
};

/** Reads the collections entry @p held, whose name views the same bytes, refusing bytes too short to be one. */
inline expected<collections_entry_t> read_collections_entry(::leveldb::Slice const& held) noexcept {
    if (held.empty())
        return status_t {status_code_t::malformed_k, "The collections entry of a collection is malformed"};
    return collections_entry_t {static_cast<ustore_modality_t>(held[0]), {held.data() + 1, held.size() - 1}};
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

    /** Serializes changes to both system collections, so a name is checked and taken in one step. */
    std::mutex collections_mutex;
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

/** What the collections system collection records of @p collection as @p options read it, whose name views @p held,
 *  refusing a collection it does not list, the reserved ones included. */
expected<collections_entry_t> look_up(database_t& database, ustore_collection_id_t const& collection, std::string& held,
                                      ::leveldb::ReadOptions const& options = {}) {
    uuid_key_t const key = uuid_key(collections_collection_k, collection.uuid);
    native_status_t const status = database.native->Get(options, key.slice(), &held);
    if (status.IsNotFound()) return status_t {status_code_t::not_found_k, "No such collection"};
    if (!status.ok()) return status_of(status);
    return read_collections_entry(held);
}

/** Refuses a batch addressing a collection the collections system collection does not list or one keyed otherwise
 *  than @p layout, looking each run of tasks on one collection up once. */
status_t resolve_collections(database_t& database, strided_iterator<ustore_collection_id_t const> collections,
                             std::size_t count, entry_layout_t layout) {
    std::string held;
    for (std::size_t task_index = 0; task_index != count; ++task_index) {
        if (task_index != 0 && collections[task_index] == collections[task_index - 1]) continue;
        auto [entry, look_up_status] = look_up(database, collections[task_index], held);
        if (look_up_status.failed()) return look_up_status;
        if (entry_layout_of(entry.modality) != layout) return collection_holds_another_modality();
    }
    return {};
}

/** Refuses a batch addressing a collection the collections system collection does not list or one
 *  @c admit_modality refuses for @p modality, looking each run of tasks on one collection up once. */
status_t refuse_modalities(ustore_database_t handle, ustore_collections_view_t collections, ustore_size_t count,
                           ustore_modality_t modality) {
    database_t& database = *handle_cast<database_t>(handle);
    strided_iterator<ustore_collection_id_t const> const tasks = strided(collections);
    std::string held;
    for (std::size_t task_index = 0; task_index != count; ++task_index) {
        if (task_index != 0 && tasks[task_index] == tasks[task_index - 1]) continue;
        auto [entry, look_up_status] = look_up(database, tasks[task_index], held);
        if (look_up_status.failed()) return look_up_status;
        if (status_t const admitted = admit_modality(entry.modality, modality); admitted.failed()) return admitted;
    }
    return {};
}

/** The resolver of this engine, which every checked verb and every layer over blobs runs first. */
status_t resolve_modality(ustore_database_t database, ustore_collections_view_t collections, ustore_size_t count,
                          ustore_modality_t modality, view_t const&) noexcept {
    return guarded<&refuse_modalities>(database, collections, count, modality);
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
    // Every stored key, both system collections included, sorts below one byte past the largest UUID
    std::string const past_last(collection_bytes_k + 1, '\xFF');
    ::leveldb::Range const everything(::leveldb::Slice(), past_last);
    std::uint64_t disk_bytes = 0;
    database.native->GetApproximateSizes(&everything, 1, &disk_bytes);
    *arguments.description = {
        .capabilities = ustore_capabilities_snapshots_k,
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

/** Drops @p collection, forgetting first and durably its collections entry and the name the entry records. The caller
 *  holds @c collections_mutex. */
status_t drop_collection(database_t& database, ustore_collection_id_t const& collection) {
    std::string held;
    auto [entry, look_up_status] = look_up(database, collection, held);
    if (look_up_status.failed()) return look_up_status;

    // A crash between the two leaves an unlisted run behind, never a listed or named collection that is gone
    ::leveldb::WriteBatch forgotten;
    uuid_key_t const listed = uuid_key(collections_collection_k, collection.uuid);
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
    std::string_view const name = string_of(arguments.name);
    if (arguments.name.data && name.empty()) return {status_code_t::invalid_argument_k, "A name is never empty"};

    // The entry and the name reach the log in one batch, so a crash keeps both or neither
    database_t& database = *handle_cast<database_t>(arguments.database);
    std::lock_guard lock {database.collections_mutex};
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

    uuid_key_t const listed = uuid_key(collections_collection_k, id.uuid);
    std::string entry(1, static_cast<char>(arguments.modality));
    entry.append(name);
    ::leveldb::WriteBatch batch;
    batch.Put(listed.slice(), entry);
    if (!name.empty()) batch.Put(name_key, ::leveldb::Slice(reinterpret_cast<char const*>(id.uuid.bytes), sizeof(id)));
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

    database_t& database = *handle_cast<database_t>(arguments.database);
    if (arguments.mode == ustore_drop_collection_k) {
        std::lock_guard lock {database.collections_mutex};
        return drop_collection(database, arguments.collection);
    }
    std::string held;
    auto [entry, look_up_status] = look_up(database, arguments.collection, held);
    if (look_up_status.failed()) return look_up_status;
    if (entry.modality == ustore_modality_strings_k && arguments.mode == ustore_drop_values_k)
        return {status_code_t::invalid_argument_k, "A strings collection's values are keys, which can't be emptied"};
    return status_of(clear_collection(database, arguments.collection, arguments.mode));
}

status_t collection_list(ustore_collection_list_t const& arguments, view_t const& view) {
    if (!arguments.written) return {status_code_t::invalid_argument_k, "Need an output for the collections written"};

    database_t& database = *handle_cast<database_t>(arguments.database);
    ::leveldb::ReadOptions options;
    options.fill_cache = false;
    use_snapshot(view, options);

    // The collections system collection lists every collection, keyed by its ID
    ::leveldb::Slice const collections = prefix_of(collections_collection_k);
    iterator_pointer_t iterator {database.native->NewIterator(options)};
    collection_list_outputs_t outputs {view.scratch};
    for (iterator->Seek(collections); iterator->Valid() && iterator->key().starts_with(collections); iterator->Next()) {
        ::leveldb::Slice const listed = iterator->key();
        auto [entry, read_status] = read_collections_entry(iterator->value());
        if (read_status.failed() || listed.size() != sizeof(uuid_key_t))
            return {status_code_t::malformed_k, "The collections entry of a collection is malformed"};
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

    // A name resolves to its collection's UUID, and the collections system collection then records its modality
    std::string key;
    std::string named;
    std::string held;
    for (std::size_t index = 0; index != arguments.count; ++index) {
        native_status_t const status = database.native->Get(
            options, string_key(names_collection_k, string_of(names[index]), key), &named);
        if (status.IsNotFound()) {
            outputs.assign(index, ustore_collection_id_t {}, ustore_modality_t {}, false);
            continue;
        }
        if (!status.ok()) return status_of(status);
        if (named.size() != sizeof(ustore_collection_id_t))
            return {status_code_t::malformed_k, "The names entry of a collection is malformed"};
        ustore_collection_id_t collection;
        std::memcpy(collection.uuid.bytes, named.data(), sizeof(collection.uuid.bytes));
        auto [entry, look_up_status] = look_up(database, collection, held, options);
        if (look_up_status.failed()) return look_up_status;
        outputs.assign(index, collection, entry.modality, true);
    }
    outputs.finish(arguments);
    return {};
}

/** Clears every collection as the mode asks in one walk of the whole keyspace, in batches the size of a memtable, the
 *  last one synced, where dropping the collections removes both system collections with them, and emptying the
 *  values leaves every strings collection as it is, whose values are keys. */
status_t database_clear(ustore_database_clear_t const& arguments, view_t const&) {
    ustore_drop_mode_t const mode = arguments.mode;
    if (mode != ustore_drop_collection_k && mode != ustore_drop_pairs_k && mode != ustore_drop_values_k)
        return {status_code_t::invalid_argument_k, "Unknown drop mode"};

    database_t& database = *handle_cast<database_t>(arguments.database);
    std::lock_guard lock {database.collections_mutex};
    ::leveldb::WriteOptions buffered;
    ::leveldb::WriteOptions synced;
    synced.sync = true;
    ::leveldb::WriteBatch batch;
    native_status_t status;
    ::leveldb::Slice const collections = prefix_of(collections_collection_k);
    ::leveldb::Slice const names = prefix_of(names_collection_k);
    // The collection the walk is in, looked up once as the walk enters its run
    ustore_collection_id_t collection = collections_collection_k;
    ustore_modality_t modality {};
    std::string held;
    iterator_pointer_t iterator {database.native->NewIterator(::leveldb::ReadOptions())};
    for (iterator->SeekToFirst(); status.ok() && iterator->Valid(); iterator->Next()) {
        ::leveldb::Slice const key = iterator->key();
        if (mode == ustore_drop_collection_k) batch.Delete(key);
        else if (key.starts_with(collections) || key.starts_with(names)) continue;
        else if (mode == ustore_drop_pairs_k) batch.Delete(key);
        else {
            if (!key.starts_with(prefix_of(collection))) {
                std::memcpy(collection.uuid.bytes, key.data(), collection_bytes_k);
                auto [entry, look_up_status] = look_up(database, collection, held);
                if (look_up_status.failed() && look_up_status != status_code_t::not_found_k) return look_up_status;
                modality = look_up_status.failed() ? ustore_modality_t {} : entry.modality;
            }
            if (modality == ustore_modality_strings_k) continue;
            batch.Put(key, ::leveldb::Slice());
        }
        if (batch.ApproximateSize() < database.options.write_buffer_size) continue;
        status = database.native->Write(buffered, &batch);
        batch.Clear();
    }
    if (status.ok()) status = iterator->status();
    if (status.ok()) status = database.native->Write(synced, &batch);
    return status_of(status);
}

/** Compacts the whole keyspace one collection at a time, in key order up to the names and the collections system
 *  collections, each flushing the memtable ahead of it, and reports one unit per collection, as LevelDB measures no
 *  memtable. */
status_t database_compact(ustore_database_compact_t const& arguments, view_t const&) {
    database_t& database = *handle_cast<database_t>(arguments.database);
    // The collections system collection lists every collection in the order of their identifiers, which is key order
    st::basic_vector<ustore_collection_id_t> bounds;
    st::status_t grown = st::status_t::success_k;
    ::leveldb::Slice const collections = prefix_of(collections_collection_k);
    iterator_pointer_t iterator {database.native->NewIterator(::leveldb::ReadOptions())};
    for (iterator->Seek(collections);
         st::succeeded(grown) && iterator->Valid() && iterator->key().starts_with(collections); iterator->Next())
        grown = bounds.emplace_back(ustore_collection_id_t {tail_uuid_of(iterator->key())});
    if (!iterator->status().ok()) return status_of(iterator->status());
    iterator.reset();
    if (st::succeeded(grown)) grown = bounds.emplace_back(names_collection_k);
    if (st::succeeded(grown)) grown = bounds.emplace_back(collections_collection_k);
    if (st::failed(grown)) return growth_status(grown);

    // A run spans from the prefix of its collection to the prefix of the next one, the first from the keyspace's start
    // and the last to its end
    progress_t progress {arguments.progress, arguments.progress_state, bounds.size()};
    for (std::size_t index = 0; index != bounds.size(); ++index) {
        bool const first = index == 0;
        bool const last = index + 1 == bounds.size();
        ::leveldb::Slice const begin = prefix_of(bounds[index]);
        ::leveldb::Slice const end = last ? ::leveldb::Slice() : prefix_of(bounds[index + 1]);
        database.native->CompactRange(first ? nullptr : &begin, last ? nullptr : &end);
        if (!progress.advance(1)) return progress_cancelled_k;
    }
    return {};
}

#pragma endregion Collections

#pragma region Memory

void database_free(ustore_database_t database) noexcept { delete handle_cast<database_t>(database); }

void snapshot_free(ustore_snapshot_t handle) noexcept {
    snapshot_t* const snapshot = handle_cast<snapshot_t>(handle);
    snapshot->database->native->ReleaseSnapshot(snapshot->native);
    delete snapshot;
}

#pragma endregion Memory

} // namespace unum::ustore::oracles::leveldb
