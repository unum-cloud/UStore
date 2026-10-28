/**
 *  @file src_oracles/rocksdb/database.hpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief The database, collections, transactions and snapshots of the RocksDB oracle, which its calls of every
 *      modality build on.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::strlen`

#include <algorithm>    // `std::sort`, `std::unique`
#include <atomic>       // `std::atomic`
#include <compare>      // `std::strong_ordering`
#include <filesystem>   // `std::filesystem::remove`
#include <iterator>     // `std::back_inserter`
#include <limits>       // `std::numeric_limits`
#include <memory>       // `std::unique_ptr`
#include <mutex>        // `std::lock_guard`, `std::mutex`
#include <numeric>      // `std::accumulate`
#include <shared_mutex> // `std::shared_lock`, `std::shared_mutex`
#include <new>          // `std::nothrow`
#include <span>         // `std::span`
#include <string>       // `std::string`, `std::to_string`, the path and name type RocksDB takes
#include <string_view>  // `std::string_view`
#include <system_error> // `std::error_code`
#include <utility>      // `std::pair`
#include <vector>       // `std::vector`, the descriptor, handle and file lists RocksDB takes

#include <fmt/format.h> // `fmt::format_to`
#include <rocksdb/db.h>
#include <rocksdb/sst_file_writer.h>
#include <rocksdb/utilities/options_util.h>
#include <rocksdb/utilities/transaction.h>
#include <rocksdb/utilities/transaction_db.h>
#include <rocksdb/utilities/write_batch_with_index.h>

#include <smashtable/basic_hash_table.hpp>
#include <smashtable/basic_vector.hpp>

#include "ustore/db.h"
#include "ustore/ranges.hpp" // `place_batch_t`, `strided`

#include "ustore/utilities/blocking_files.hpp" // `directory_exists`

#include "engine.hpp"             // `engine_t`, `ustore_*_handle_t`, `state_of`
#include "utilities/batches.hpp"  // `strings_arg`, `validate_write`, `places_of`
#include "utilities/outputs.hpp"  // `read_outputs_t`, `scan_outputs_t`, `collection_list_outputs_t`
#include "utilities/progress.hpp" // `progress_t`, `progress_cancelled_k`

namespace unum::ustore::oracles::rocksdb {
using namespace unum::ustore::utilities;

#pragma region Database

/** The open RocksDB database, read-write or read-only. */
using native_t = ::rocksdb::DB;

/** The open RocksDB database when it opened read-write, which transactions begin on. */
using transactional_t = ::rocksdb::TransactionDB;

/** Status RocksDB answers. */
using native_status_t = ::rocksdb::Status;

/** A column family, which stores one collection or one system collection. */
using column_t = ::rocksdb::ColumnFamilyHandle;

/** The column family of a collection a caller created, with the modality and the name its collections entry records,
 *  the name empty for none. */
struct family_t {
    column_t* column = nullptr;
    ustore_modality_t modality {};
    std::string name;
};

/** The column families of the collections callers created, by the identifier of their collection, which the database
 *  destroys before it closes. */
using columns_t = st::hash_map<ustore_collection_id_t, family_t, std::hash<ustore_collection_id_t>>;

/** The name of the column family of the names system collection, mapping every name onto its collection. */
constexpr char const* names_column_name_k = "_names";

/** The name of the column family of the collections system collection, mapping every collection onto its modality
 *  and its name. */
constexpr char const* collections_column_name_k = "_collections";

/** Reads the collection a column family stores from its name, the collection's UUID text, answering false for a name
 *  this engine gives no collection. */
bool read_column_name(std::string_view name, ustore_collection_id_t& id) noexcept {
    if (name.size() != uuid_text_length_k) return false;
    auto [uuid, uuid_status] = uuid_from_text(name);
    if (uuid_status.failed()) return false;
    id.uuid = uuid;
    return true;
}

/** Milliseconds a write outside any transaction waits for a key a staged transaction holds before it conflicts, where
 *  RocksDB would wait a whole second. */
constexpr std::int64_t head_write_lock_timeout_ms_k = 16;

/** The calls this engine serves, which stamp every handle it hands out. */
extern engine_t const engine_k;

/** The open database and its column families. */
struct database_t {

    /** The column families of the collections callers created, which leave out the system ones. */
    columns_t columns;

    /** Shared by every call for as long as it runs, exclusive for a create, a drop or a reset. */
    std::shared_mutex columns_mutex;

    /** The column family of the names system collection. */
    column_t* names = nullptr;

    /** The column family of the collections system collection. */
    column_t* collections = nullptr;

    /** The open database. */
    std::unique_ptr<native_t> native;

    /** Snapshots handed out and not yet freed, which a reset refuses to run under. */
    std::atomic<std::size_t> open_snapshots = 0;

    /** Column families dropped while a snapshot was open, whose handles stay for the saves of those snapshots,
     *  destroyed with the last snapshot. */
    st::basic_vector<column_t*> retired;

    /** Guards @c retired and the decrements of @c open_snapshots, apart from @c columns_mutex, so a free from inside a
     *  call holding the column families never waits on itself. A snapshot counts itself in under the column
     *  families, which a drop holds exclusively. */
    std::mutex retired_mutex;

    /** Writes the log's buffer out and closes, whether the database opened fully or failed halfway. A snapshot the
     *  caller never freed makes `Close` refuse, which is the caller's error, so the database goes regardless. */
    ~database_t() noexcept {
        if (!native) return;
        [[maybe_unused]] native_status_t const flushed = native->FlushWAL(true);
        for (auto const& [id, family] : columns) native->DestroyColumnFamilyHandle(family.column);
        if (names) native->DestroyColumnFamilyHandle(names);
        if (collections) native->DestroyColumnFamilyHandle(collections);
        [[maybe_unused]] native_status_t const closed = native->Close();
    }
};

/** The transaction database behind @p database, which every call beginning a transaction or clearing a column family
 *  reaches only on a database opened read-write, since the router refuses those calls on one opened read-only. */
transactional_t& transactional(database_t& database) noexcept {
    return static_cast<transactional_t&>(*database.native);
}

/** What a snapshot holds, behind its snapshot handle, pinning one RocksDB snapshot of its database until freed. */
struct snapshot_t {

    /** The database the snapshot was taken of, which releases it. */
    database_t* database = nullptr;

    /** The RocksDB snapshot. */
    ::rocksdb::Snapshot const* native = nullptr;

    /** The collections the database held when the snapshot was taken, with column families that outlive their drops. */
    columns_t columns;
};

/** A key a transaction read while watching it, locked and validated when the transaction stages. */
struct watched_key_t {
    ustore_collection_id_t collection {};
    ustore_key_t key {};

    friend std::strong_ordering operator<=>(watched_key_t const&, watched_key_t const&) noexcept = default;
};

/** How far a transaction got towards its commit. */
enum class staging_t : std::uint8_t {

    /** Buffering writes and watched reads, holding no locks. */
    buffering_k,

    /** Prepared into the log, with every key it touched locked and validated. */
    staged_k,

    /** A prepare failed after RocksDB left the transaction unusable, so only a reset brings it back. */
    broken_k,
};

/**
 *  @brief A transaction whose conflicts surface at stage or commit, as they do on the reference engine.
 *
 *  Writes buffer in @c batch and watched reads in @c watched, so nothing is locked while the transaction runs, and
 *  a write outside it never waits. Staging replays both into RocksDB's pessimistic transaction, whose snapshot is the
 *  one the transaction's first read or write took.
 */
struct transaction_t {

    /** RocksDB's transaction, holding the snapshot from its first read or write and the locks once staged. */
    std::unique_ptr<::rocksdb::Transaction> native;

    /** Writes not yet replayed, which reads in the transaction see before the database. */
    ::rocksdb::WriteBatchWithIndex batch {::rocksdb::BytewiseComparator(), 0, true};

    /** Keys read while watching them. */
    st::basic_vector<watched_key_t> watched;

    /** Strings read while watching them, with the strings collections holding them. */
    st::basic_vector<std::pair<ustore_collection_id_t, std::string>> watched_strings;

    /** Collections holding buffered writes, each replayed from the batch's index at stage. */
    st::basic_vector<ustore_collection_id_t> written;

    /** How far the transaction got. */
    staging_t staging = staging_t::buffering_k;
};

inline ::rocksdb::Slice to_slice(ustore_key_t const& key) noexcept {
    return {reinterpret_cast<char const*>(&key), sizeof(ustore_key_t)};
}

inline ::rocksdb::Slice to_slice(ustore_collection_id_t const& id) noexcept {
    return {reinterpret_cast<char const*>(&id), sizeof(ustore_collection_id_t)};
}

inline ::rocksdb::Slice to_slice(value_view_t value) noexcept {
    return {reinterpret_cast<char const*>(value.begin()), value.size()};
}

/** The snapshot @p transaction reads and validates against, which its first read or write takes. */
::rocksdb::Snapshot const* snapshot_of(transaction_t& transaction) noexcept {
    ::rocksdb::Transaction& native = *transaction.native;
    if (!native.GetSnapshot()) native.SetSnapshot();
    return native.GetSnapshot();
}

/** Takes the snapshot of @p transaction on its first write, and notes @p collection for the replay at stage. */
native_status_t note_written(transaction_t& transaction, ustore_collection_id_t collection) {
    [[maybe_unused]] ::rocksdb::Snapshot const* const snapshot = snapshot_of(transaction);
    for (ustore_collection_id_t const written : transaction.written)
        if (written == collection) return native_status_t::OK();
    if (st::failed(transaction.written.emplace_back(collection))) return native_status_t::MemoryLimit();
    return native_status_t::OK();
}

/** Buffers one write of @p key into @p transaction, noting its collection for the replay at stage. */
native_status_t buffer_write(transaction_t& transaction, column_t* column, ustore_collection_id_t collection,
                             ::rocksdb::Slice key, ::rocksdb::Slice value) {
    native_status_t const status = transaction.batch.Put(column, key, value);
    return status.ok() ? note_written(transaction, collection) : status;
}

/** Buffers one removal of @p key into @p transaction, noting its collection for the replay at stage. */
native_status_t buffer_remove(transaction_t& transaction, column_t* column, ustore_collection_id_t collection,
                              ::rocksdb::Slice key) {
    native_status_t const status = transaction.batch.Delete(column, key);
    return status.ok() ? note_written(transaction, collection) : status;
}

/** The status RocksDB's @p status stands for, success included. */
status_t status_of(native_status_t const& status) noexcept {
    if (status.ok()) return {};
    if (status.IsNoSpace()) return {status_code_t::capacity_exhausted_k, "RocksDB ran out of disk space"};
    if (status.IsCorruption()) return {status_code_t::malformed_k, "RocksDB found corrupted data"};
    if (status.IsIOError()) return {status_code_t::io_k, "RocksDB failed an I/O operation"};
    if (status.IsInvalidArgument()) return {status_code_t::invalid_argument_k, "RocksDB rejected an argument"};
    if (status.IsNotFound()) return {status_code_t::not_found_k, "RocksDB found no such entry"};
    if (status.IsNotSupported()) return {status_code_t::unsupported_k, "RocksDB does not support the operation"};
    if (status.IsBusy() || status.IsTryAgain() || status.IsTimedOut())
        return {status_code_t::write_conflict_k, "A concurrent write changed a key the transaction wrote"};
    if (status.IsMemoryLimit()) return {status_code_t::out_of_memory_k, "RocksDB reached its memory limit"};
    if (status.IsLockLimit()) return {status_code_t::capacity_exhausted_k, "RocksDB ran out of lock slots"};
    if (status.IsTxnNotPrepared())
        return {status_code_t::invalid_argument_k, "The transaction was not prepared before its commit"};
    return {status_code_t::internal_k, "RocksDB failed"};
}

/** The column family @p collection addresses, or null for any identifier no caller-created column family carries.
 *  The caller holds @c columns_mutex, which keeps the column family alive for as long as it holds it. */
column_t* column_of(database_t& database, ustore_collection_id_t collection) noexcept {
    auto const found = database.columns.find(collection);
    return found != database.columns.end() ? (*found).mapped.column : nullptr;
}

/** Refuses @p collection unless a column family keyed as @p layout holds it. The caller holds @c columns_mutex. */
status_t refuse_collection(database_t& database, ustore_collection_id_t collection, entry_layout_t layout) noexcept {
    auto const found = database.columns.find(collection);
    if (found == database.columns.end()) return {status_code_t::not_found_k, "No such collection"};
    if (entry_layout_of((*found).mapped.modality) == layout) return {};
    return collection_holds_another_modality();
}

/** Refuses a batch addressing a collection no column family keyed as @p layout holds. The caller holds
 *  @c columns_mutex, so every collection it accepts stays addressable until the call returns. */
status_t refuse_collections(database_t& database, strided_iterator<ustore_collection_id_t const> collections,
                            std::size_t count, entry_layout_t layout) noexcept {
    for (std::size_t index = 0; index != count; ++index) {
        bool const repeated = index != 0 && collections[index] == collections[index - 1];
        if (repeated) continue;
        if (status_t const status = refuse_collection(database, collections[index], layout); status.failed())
            return status;
    }
    return {};
}

/** Refuses a batch addressing a collection no caller created or one @c admit_modality refuses for @p modality,
 *  looking each run of tasks on one collection up once. */
status_t refuse_modalities(ustore_database_t handle, ustore_collections_view_t collections, ustore_size_t count,
                           ustore_modality_t modality) {
    database_t& database = *state_of<database_t>(handle);
    strided_iterator<ustore_collection_id_t const> const tasks = strided(collections);
    std::shared_lock lock {database.columns_mutex};
    for (std::size_t index = 0; index != count; ++index) {
        if (index != 0 && tasks[index] == tasks[index - 1]) continue;
        auto const found = database.columns.find(tasks[index]);
        if (found == database.columns.end()) return {status_code_t::not_found_k, "No such collection"};
        if (status_t const admitted = admit_modality((*found).mapped.modality, modality); admitted.failed())
            return admitted;
    }
    return {};
}

/** The resolver of this engine, which every checked verb and every layer over blobs runs first. */
status_t resolve_modality(ustore_database_t database, ustore_collections_view_t collections, ustore_size_t count,
                          ustore_modality_t modality, view_t const&) noexcept {
    return guarded<&refuse_modalities>(database, collections, count, modality);
}

/** Refuses a write into @p transaction once it staged. */
status_t refuse_staged_write(transaction_t const* transaction) noexcept {
    if (!transaction || transaction->staging == staging_t::buffering_k) return {};
    return {status_code_t::invalid_argument_k, "A staged transaction takes no more writes"};
}

/** Refuses a read watching its keys through @p transaction once it staged. */
status_t refuse_staged_watch(transaction_t const* transaction, ustore_watch_t mode) noexcept {
    if (!transaction || mode != ustore_watch_keys_k || transaction->staging == staging_t::buffering_k) return {};
    return {status_code_t::invalid_argument_k, "A staged transaction watches no more reads"};
}

/** Removes every pair of @p column in one synchronous batch, or empties every value for @c ustore_drop_values_k. */
native_status_t clear_column(database_t& database, column_t* column, ustore_drop_mode_t mode) {
    ::rocksdb::WriteBatch batch;
    std::unique_ptr<::rocksdb::Iterator> iterator {database.native->NewIterator(::rocksdb::ReadOptions(), column)};
    native_status_t status;
    for (iterator->SeekToFirst(); status.ok() && iterator->Valid(); iterator->Next())
        status = mode == ustore_drop_values_k ? batch.Put(column, iterator->key(), ::rocksdb::Slice())
                                              : batch.Delete(column, iterator->key());
    if (status.ok()) status = iterator->status();
    if (!status.ok()) return status;

    // Clearing is not transactional, so it takes no locks behind a staged transaction
    ::rocksdb::WriteOptions write_options;
    write_options.sync = true;
    ::rocksdb::TransactionDBWriteOptimizations optimizations;
    optimizations.skip_concurrency_control = true;
    return transactional(database).Write(write_options, optimizations, &batch);
}

/** Points @p options at the snapshot @p view names, leaving them at the head for any other view. */
void use_snapshot(view_t const& view, ::rocksdb::ReadOptions& options) noexcept {
    if (view.snapshot) options.snapshot = state_of<snapshot_t>(view.snapshot)->native;
}

#pragma endregion Database

#pragma region Reads and Writes

/** An upper bound on the bytes a batch record spends beyond its key and value. */
constexpr std::size_t record_overhead_k = 16;

/** The options of a write or a commit, syncing when @p durability asks for a flush, which writes the log's buffer out
 *  first, so a flushed write keeps every write before it too. */
::rocksdb::WriteOptions write_options_for(ustore_durability_t durability) noexcept {
    ::rocksdb::WriteOptions write_options;
    write_options.sync = durability == ustore_durability_flushed_k;
    return write_options;
}

status_t write_one(database_t& database, transaction_t* transaction, place_batch_t const& places,
                   strings_arg_t const& values, ustore_durability_t durability) {

    place_t const place = places[0];
    value_view_t const content = values[0];
    column_t* const column = column_of(database, place.collection);
    ::rocksdb::Slice const key = to_slice(place.key);
    native_status_t const status = transaction
                                       ? buffer_write(*transaction, column, place.collection, key, to_slice(content))
                                       : database.native->Put(write_options_for(durability), column, key,
                                                              to_slice(content));
    return status_of(status);
}

status_t write_many(database_t& database, transaction_t* transaction, place_batch_t const& places,
                    strings_arg_t const& values, ustore_durability_t durability) {

    if (transaction) {
        for (std::size_t task_index = 0; task_index != places.size(); ++task_index) {
            place_t const place = places[task_index];
            column_t* const column = column_of(database, place.collection);
            native_status_t const status = buffer_write(*transaction, column, place.collection, to_slice(place.key),
                                                        to_slice(values[task_index]));
            if (!status.ok()) return status_of(status);
        }
        return {};
    }

    // Reserving the whole batch spares its buffer from regrowing, and the spare record covers the header
    std::size_t reserved_bytes = record_overhead_k;
    for (std::size_t task_index = 0; task_index != places.size(); ++task_index)
        reserved_bytes += record_overhead_k + sizeof(ustore_key_t) + values[task_index].length;

    ::rocksdb::WriteBatch batch {reserved_bytes};
    for (std::size_t task_index = 0; task_index != places.size(); ++task_index) {
        place_t const place = places[task_index];
        column_t* const column = column_of(database, place.collection);
        native_status_t const status = batch.Put(column, to_slice(place.key), to_slice(values[task_index]));
        if (!status.ok()) return status_of(status);
    }

    return status_of(database.native->Write(write_options_for(durability), &batch));
}

/** Records @p place among the keys @p transaction validates when it stages, unless @p mode says not to watch it. */
native_status_t watch(transaction_t& transaction, place_t const& place, ustore_watch_t mode) noexcept {
    if (mode != ustore_watch_keys_k) return native_status_t::OK();
    if (st::failed(transaction.watched.emplace_back(watched_key_t {place.collection, place.key})))
        return native_status_t::MemoryLimit();
    return native_status_t::OK();
}

/** Reads one place into @p value, from the transaction's writes before the database when a transaction reads it. */
native_status_t get_one(database_t& database, transaction_t* transaction, ::rocksdb::ReadOptions const& read_options,
                        place_t const& place, ustore_watch_t mode, ::rocksdb::PinnableSlice& value) {

    column_t* const column = column_of(database, place.collection);
    ::rocksdb::Slice const key = to_slice(place.key);
    if (!transaction) return database.native->Get(read_options, column, key, &value);

    native_status_t const watched = watch(*transaction, place, mode);
    if (!watched.ok()) return watched;
    return transaction->batch.GetFromBatchAndDB(database.native.get(), read_options, column, key, &value);
}

/** Whether the keys of @p places ascend, which spares a batched read its own sort. */
bool keys_ascend(place_batch_t const& places) noexcept {
    for (std::size_t task_index = 1; task_index < places.size(); ++task_index)
        if (places[task_index].key < places[task_index - 1].key) return false;
    return true;
}

/** Reads every place in one batch, leaving each value pinned in @p values and its status in @p statuses. */
status_t get_many(database_t& database, transaction_t* transaction, ::rocksdb::ReadOptions const& read_options,
                  place_batch_t const& places, ustore_watch_t mode, ::rocksdb::PinnableSlice* values,
                  native_status_t* statuses) {

    std::size_t const count = places.size();
    bool const same_collection = places.same_collection();

    // A transaction's writes are merged into one collection at a time, so a mixed batch reads key by key
    if (transaction && !same_collection) {
        for (std::size_t task_index = 0; task_index != count; ++task_index)
            statuses[task_index] = get_one(database, transaction, read_options, places[task_index], mode,
                                           values[task_index]);
        return {};
    }

    std::unique_ptr<::rocksdb::Slice[]> keys {new (std::nothrow)::rocksdb::Slice[count]};
    if (!keys) return {status_code_t::out_of_memory_k, "Failed to allocate the batch"};
    for (std::size_t task_index = 0; task_index != count; ++task_index)
        keys[task_index] = to_slice(places[task_index].key);

    if (same_collection) {
        column_t* const column = column_of(database, places[0].collection);
        bool const sorted = keys_ascend(places);
        if (!transaction) {
            database.native->MultiGet(read_options, column, count, keys.get(), values, statuses, sorted);
            return {};
        }
        for (std::size_t task_index = 0; task_index != count; ++task_index) {
            native_status_t const watched = watch(*transaction, places[task_index], mode);
            if (!watched.ok()) return status_of(watched);
        }
        transaction->batch.MultiGetFromBatchAndDB(database.native.get(), read_options, column, count, keys.get(),
                                                  values, statuses, sorted);
        return {};
    }

    std::unique_ptr<column_t*[]> columns {new (std::nothrow) column_t*[count]};
    if (!columns) return {status_code_t::out_of_memory_k, "Failed to allocate the batch"};
    for (std::size_t task_index = 0; task_index != count; ++task_index)
        columns[task_index] = column_of(database, places[task_index].collection);
    database.native->MultiGet(read_options, count, columns.get(), keys.get(), values, statuses);
    return {};
}

/** Exports what each read of @p arguments found into the columns of its arena, each pinned value copied exactly
 *  once, staging only their views in @p scratch. */
template <typename arguments_type_>
status_t export_values(arguments_type_ const& arguments, ::rocksdb::PinnableSlice const* values,
                       native_status_t const* statuses, ustore_arena_t scratch) noexcept {
    auto [outputs, outputs_status] = read_outputs_t::make(arguments, scratch);
    if (outputs_status.failed()) return outputs_status;
    for (std::size_t task_index = 0; task_index != arguments.count; ++task_index) {
        if (statuses[task_index].IsNotFound()) outputs.append_missing();
        else if (statuses[task_index].ok()) outputs.append(values[task_index].ToStringView());
        else return status_of(statuses[task_index]);
    }
    return outputs.finish(arguments);
}

/** The transaction a read goes through, or null for a view of the head or of a snapshot, pointing @p read_options at
 *  the snapshot the view reads. */
transaction_t* reading_transaction(view_t const& view, ::rocksdb::ReadOptions& read_options) noexcept {
    use_snapshot(view, read_options);
    if (!view.transaction) return nullptr;
    transaction_t* const transaction = state_of<transaction_t>(view.transaction);
    read_options.snapshot = snapshot_of(*transaction);
    return transaction;
}

/** An iterator over @p column, which merges the writes @p transaction buffered over the database when it has one. */
::rocksdb::Iterator* new_iterator(database_t& database, transaction_t* transaction,
                                  ::rocksdb::ReadOptions const& read_options, column_t* column) {
    ::rocksdb::Iterator* const base = database.native->NewIterator(read_options, column);
    if (!transaction || !base) return base;
    return transaction->batch.NewIteratorWithBase(column, base, &read_options);
}

#pragma endregion Reads and Writes

#pragma region Snapshots

/** Adds to @p bytes the bytes of the keys and values @p iterator walks. */
native_status_t tally_column(::rocksdb::Iterator& iterator, ustore_size_t& bytes) {
    for (iterator.SeekToFirst(); iterator.Valid(); iterator.Next())
        bytes += iterator.key().size() + iterator.value().size();
    return iterator.status();
}

/** Writes what @p iterator walks into an SST file at @p file_path, then moves that file into @p column of @p target,
 *  adding the bytes of the keys and values to @p bytes. */
native_status_t export_column(::rocksdb::Iterator& iterator, ::rocksdb::Options const& options,
                              std::string const& file_path, ::rocksdb::DB& target, column_t* column,
                              ustore_size_t& bytes) {

    iterator.SeekToFirst();
    if (!iterator.Valid()) return iterator.status();

    // An SST file skips the memtable and the log, and ingestion moves it into place without copying
    ::rocksdb::SstFileWriter writer {::rocksdb::EnvOptions(), options, column};
    native_status_t status = writer.Open(file_path);
    for (; status.ok() && iterator.Valid(); iterator.Next()) {
        status = writer.Put(iterator.key(), iterator.value());
        bytes += iterator.key().size() + iterator.value().size();
    }
    if (status.ok()) status = iterator.status();
    if (status.ok()) status = writer.Finish();
    if (status.ok()) {
        ::rocksdb::IngestExternalFileOptions ingest_options;
        ingest_options.move_files = true;
        status = target.IngestExternalFile(column, {file_path}, ingest_options);
    }
    return status;
}

/** Points @p file_path at the SST file an export at @p path writes for the column family @p id, named apart from the
 *  numbered files RocksDB owns in the same directory. */
void column_file_path(std::string& file_path, std::string const& path, std::size_t id) {
    file_path.clear();
    fmt::format_to(std::back_inserter(file_path), "{}/column-{}.sst", path, id);
}

/** Removes what a failed export of the column families @p families left at @p path, answering whether nothing
 *  remains: first the SST files, which RocksDB does not recognize as its own, then the database around them. */
bool remove_partial_export(std::string const& path, ::rocksdb::Options const& options,
                           std::span<column_t* const> families) {
    bool removed = true;
    std::string file_path;
    for (column_t* const family : families) {
        column_file_path(file_path, path, family->GetID());
        // A file that was never written, or was already moved in by an ingestion, is no error
        std::error_code error;
        std::filesystem::remove(file_path, error);
        removed = removed && !error;
    }
    return ::rocksdb::DestroyDB(path, options).ok() && removed;
}

#pragma endregion Snapshots

#pragma region Lifetime

/** Opens the database in the directory @p arguments name into their output: read-write, creating it when missing, or
 *  read-only through RocksDB's `OpenForReadOnly`, as @p access asks. */
template <typename arguments_type_>
status_t open_database(arguments_type_ const& arguments, access_t access) {
    if (!arguments.database) return {status_code_t::invalid_argument_k, "Need an output for the database"};
    if (!arguments.directory.length) return {status_code_t::invalid_argument_k, "A database needs a directory"};
    if (arguments.data_directories_count && !arguments.data_directories)
        return {status_code_t::invalid_argument_k, "Counted data directories are null"};

    std::string const root(string_of(arguments.directory));
    auto [root_exists, directory_exists_status] = directory_exists(root.c_str());
    if (directory_exists_status.failed()) return directory_exists_status;
    if (!root_exists) return {status_code_t::not_found_k, "Root isn't a directory"};

    // Recovery depends on the options file: https://github.com/facebook/rocksdb/wiki/RocksDB-Options-File
    ::rocksdb::Options options;
    options.compression = ::rocksdb::kNoCompression;
    std::vector<::rocksdb::ColumnFamilyDescriptor> column_descriptors;
    native_status_t status = ::rocksdb::LoadLatestOptions(::rocksdb::ConfigOptions(), root, &options,
                                                          &column_descriptors);
    if (!status.ok() && !status.IsNotFound()) return {status_code_t::io_k, "Failed to recover the RocksDB state"};

    if (column_descriptors.empty())
        column_descriptors.push_back({::rocksdb::kDefaultColumnFamilyName, ::rocksdb::ColumnFamilyOptions()});
    // A column family this engine names neither after a collection nor after a system collection refuses the open
    for (auto const& descriptor : column_descriptors) {
        ustore_collection_id_t id {};
        bool const known = descriptor.name == ::rocksdb::kDefaultColumnFamilyName ||
                           descriptor.name == names_column_name_k || descriptor.name == collections_column_name_k ||
                           read_column_name(descriptor.name, id);
        if (!known) return {status_code_t::malformed_k, "The database holds a column family of another layout"};
    }

    // An export persists `error_if_exists`, which would refuse every reopening of it
    options.create_if_missing = true;
    options.error_if_exists = false;
    // Every write reaches the log, which stays in memory until a flushed write, a full buffer or a clean close; set
    // after the options files, so a database created before keeps no other setting
    options.manual_wal_flush = true;
    for (ustore_data_directory_t const& directory :
         std::span {arguments.data_directories, static_cast<std::size_t>(arguments.data_directories_count)}) {
        if (!directory.path.length) return {status_code_t::invalid_argument_k, "A data directory needs a path"};
        options.db_paths.push_back(
            {std::string(string_of(directory.path)), directory.max_bytes ? directory.max_bytes : UINT64_MAX});
    }

    std::unique_ptr<ustore_database_handle_t> handle {new (std::nothrow) ustore_database_handle_t {}};
    std::unique_ptr<database_t> database {handle ? new (std::nothrow) database_t : nullptr};
    if (!database) return {status_code_t::out_of_memory_k, "Failed to allocate the database"};

    std::vector<column_t*> handles;
    if (access == access_t::read_only_k) {
        status = native_t::OpenForReadOnly(options, root, column_descriptors, &handles, &database->native);
        if (!status.ok()) return status_of(status);
    }
    else {
        // A write outside any transaction waits this long for a key a staged transaction holds, and then conflicts
        ::rocksdb::TransactionDBOptions transaction_options;
        transaction_options.default_lock_timeout = head_write_lock_timeout_ms_k;
        transactional_t* native = nullptr;
        status = transactional_t::Open(options, transaction_options, root, column_descriptors, &handles, &native);
        if (!status.ok()) return status_of(status);
        database->native.reset(native);
    }
    // The database owns every handle the moment it opens, so a failure below still closes them
    bool recorded = true;
    for (column_t* const handle : handles) {
        ustore_collection_id_t id {};
        std::string const& column_name = handle->GetName();
        if (column_name == ::rocksdb::kDefaultColumnFamilyName) database->native->DestroyColumnFamilyHandle(handle);
        else if (column_name == names_column_name_k) database->names = handle;
        else if (column_name == collections_column_name_k) database->collections = handle;
        else if (!read_column_name(column_name, id) ||
                 st::failed(
                     database->columns.upsert(st::mapping<ustore_collection_id_t, family_t> {id, family_t {handle}}))) {
            recorded = false;
            database->native->DestroyColumnFamilyHandle(handle);
        }
    }
    if (!recorded) return {status_code_t::out_of_memory_k, "Failed to record the collections"};

    if (access == access_t::read_only_k) {
        if (!database->names || !database->collections)
            return {status_code_t::malformed_k, "The database holds no system collections"};
    }
    else {
        // No call resumes a transaction a crash left prepared, so each is rolled back, releasing the log it pins
        std::vector<::rocksdb::Transaction*> prepared;
        transactional(*database).GetAllPreparedTransactions(&prepared);
        native_status_t rolled_back;
        for (::rocksdb::Transaction* const recovered : prepared) {
            native_status_t const rollback = recovered->Rollback();
            if (rolled_back.ok()) rolled_back = rollback;
            delete recovered;
        }
        if (!rolled_back.ok()) return status_of(rolled_back);

        // Each system collection is a column family of its own, created the first time a database opens
        if (!database->names)
            status = database->native->CreateColumnFamily(::rocksdb::ColumnFamilyOptions(), names_column_name_k,
                                                          &database->names);
        if (status.ok() && !database->collections)
            status = database->native->CreateColumnFamily(::rocksdb::ColumnFamilyOptions(), collections_column_name_k,
                                                          &database->collections);
        if (!status.ok()) return status_of(status);
    }

    // Every collection's modality and name are read into memory once, so a call resolves them without a lookup
    std::size_t described = 0;
    std::unique_ptr<::rocksdb::Iterator> entries {
        database->native->NewIterator(::rocksdb::ReadOptions(), database->collections)};
    for (entries->SeekToFirst(); entries->Valid(); entries->Next()) {
        ::rocksdb::Slice const key = entries->key();
        ::rocksdb::Slice const value = entries->value();
        ustore_collection_id_t id;
        bool const well_formed = key.size() == sizeof(ustore_collection_id_t) && !value.empty() &&
                                 static_cast<std::uint8_t>(value[0]) >= ustore_modality_blobs_k &&
                                 static_cast<std::uint8_t>(value[0]) <= ustore_modality_pages_k;
        if (!well_formed) return {status_code_t::malformed_k, "The database holds a malformed collections entry"};
        std::memcpy(&id, key.data(), sizeof(ustore_collection_id_t));
        auto const found = database->columns.find(id);
        if (found == database->columns.end())
            return {status_code_t::malformed_k, "The database records a collection with no column family"};
        family_t& family = (*found).mapped;
        family.modality = static_cast<ustore_modality_t>(value[0]);
        family.name.assign(value.data() + 1, value.size() - 1);
        ++described;
    }
    if (!entries->status().ok()) return status_of(entries->status());
    entries.reset();

    // A column family no collections entry records is the leftover of a create or a drop that crashed halfway
    if (described != database->columns.size()) {
        if (access == access_t::read_only_k)
            return {status_code_t::malformed_k, "The database holds a column family a crashed create or drop left"};
        st::basic_vector<ustore_collection_id_t> leftovers;
        std::string entry;
        for (auto const& [id, family] : database->columns) {
            status = database->native->Get(::rocksdb::ReadOptions(), database->collections, to_slice(id), &entry);
            if (status.IsNotFound() && st::failed(leftovers.push_back(ustore_collection_id_t {id})))
                return {status_code_t::out_of_memory_k, "Failed to gather the leftover column families"};
            if (!status.ok() && !status.IsNotFound()) return status_of(status);
        }
        for (ustore_collection_id_t const id : leftovers) {
            column_t* const column = (*database->columns.find(id)).mapped.column;
            status = database->native->DropColumnFamily(column);
            database->native->DestroyColumnFamilyHandle(column);
            [[maybe_unused]] bool const erased = database->columns.erase(id);
            if (!status.ok()) return status_of(status);
        }
    }
    handle->engine = &engine_k, handle->state = database.release(), handle->access = access;
    *arguments.database = handle.release();
    return {};
}

status_t database_open(ustore_database_open_rocksdb_t const& arguments, cpu_ceiling_t, view_t const&) {
    return open_database(arguments, access_t::read_write_k);
}

status_t database_view(ustore_database_view_rocksdb_t const& arguments, cpu_ceiling_t, view_t const&) {
    return open_database(arguments, access_t::read_only_k);
}

/** Gathers the column families of both system collections and of every collection @p columns lists into
 *  @p families. */
status_t gather_families(database_t const& database, columns_t const& columns,
                         st::basic_vector<column_t*>& families) noexcept {
    if (st::failed(families.reserve(columns.size() + 2)))
        return {status_code_t::out_of_memory_k, "Failed to gather the column families"};
    [[maybe_unused]] auto const names_gathered = families.emplace_back(st::assume_reserved, database.names);
    [[maybe_unused]] auto const collections_gathered = families.emplace_back(st::assume_reserved, database.collections);
    for (auto const& [id, family] : columns) [[maybe_unused]]
        auto const gathered = families.emplace_back(st::assume_reserved, family.column);
    return {};
}

/** Reports the engine with the estimated keys of every collection, and the memtable and table file bytes of every
 *  column family, the system ones included. */
status_t database_describe(ustore_database_describe_t const& arguments, view_t const&) {
    database_t& database = *state_of<database_t>(arguments.database);
    ustore_database_description_t& description = *arguments.description;
    description.capabilities = ustore_capabilities_t(ustore_capabilities_transactions_k |
                                                     ustore_capabilities_snapshots_k);
    description.engine = ustore_engine_rocksdb_k;
    description.isolation = ustore_isolation_snapshot_k;
    std::shared_lock lock {database.columns_mutex};
    st::basic_vector<column_t*> families;
    if (status_t const status = gather_families(database, database.columns, families); status.failed()) return status;
    for (column_t* const column : families) {
        std::uint64_t property = 0;
        if (column != database.names && column != database.collections &&
            database.native->GetIntProperty(column, ::rocksdb::DB::Properties::kEstimateNumKeys, &property))
            description.keys += property;
        if (database.native->GetIntProperty(column, ::rocksdb::DB::Properties::kCurSizeAllMemTables, &property))
            description.memory_bytes += property;
        if (database.native->GetIntProperty(column, ::rocksdb::DB::Properties::kTotalSstFilesSize, &property))
            description.disk_bytes += property;
    }
    return {};
}

#pragma endregion Lifetime

#pragma region Snapshots

status_t snapshot_init(ustore_snapshot_init_t const& arguments, view_t const&) {
    if (!arguments.snapshot) return {status_code_t::invalid_argument_k, "Need an output for the snapshot"};

    database_t& database = *state_of<database_t>(arguments.database);
    std::unique_ptr<ustore_snapshot_handle_t> handle {new (std::nothrow) ustore_snapshot_handle_t {}};
    std::unique_ptr<snapshot_t> snapshot {handle ? new (std::nothrow) snapshot_t : nullptr};
    if (!snapshot) return {status_code_t::out_of_memory_k, "Failed to allocate the snapshot handle"};

    // Creators and droppers take the column families exclusively, so the recorded ones are the ones the snapshot reads
    std::shared_lock lock {database.columns_mutex};
    for (auto const& [id, family] : database.columns)
        if (st::failed(snapshot->columns.upsert(st::mapping<ustore_collection_id_t, family_t> {id, family})))
            return {status_code_t::out_of_memory_k, "Failed to record the collections of the snapshot"};
    snapshot->native = database.native->GetSnapshot();
    if (!snapshot->native) return {status_code_t::internal_k, "Failed to take a snapshot"};
    snapshot->database = &database;
    database.open_snapshots.fetch_add(1, std::memory_order_relaxed);
    if (arguments.read_stamp) *arguments.read_stamp = snapshot->native->GetSequenceNumber();
    handle->engine = &engine_k, handle->state = snapshot.release();
    *arguments.snapshot = handle.release();
    return {};
}

/** Saves exactly the column families the snapshot recorded, those dropped since included through the handles they
 *  keep until it is freed, so it reads no column family of the database and takes no lock on them. */
status_t snapshot_save(ustore_snapshot_save_t const& arguments, view_t const& view) {
    database_t& database = *state_of<database_t>(database_of(arguments));
    snapshot_t const& snapshot = *state_of<snapshot_t>(view.snapshot);
    ::rocksdb::ReadOptions snapshot_options;
    snapshot_options.fill_cache = false;
    use_snapshot(view, snapshot_options);

    // Every column family travels under its own name and comparator, the system ones among them, so the saved database
    // opens with the collections entries that describe its column families
    st::basic_vector<column_t*> families;
    if (status_t const status = gather_families(database, snapshot.columns, families); status.failed()) return status;
    ustore_size_t total = 0;
    for (std::size_t index = 0; arguments.progress && index != families.size(); ++index) {
        std::unique_ptr<::rocksdb::Iterator> iterator {database.native->NewIterator(snapshot_options, families[index])};
        native_status_t const tallied = iterator ? tally_column(*iterator, total) : native_status_t::Aborted();
        if (!tallied.ok()) return status_of(tallied);
    }
    progress_t progress {arguments.progress, arguments.progress_state, total};

    ::rocksdb::Options options;
    options.compression = ::rocksdb::kNoCompression;
    options.create_if_missing = true;
    options.error_if_exists = true;

    std::string const path(string_of(arguments.path));
    std::unique_ptr<::rocksdb::DB> exported;
    native_status_t status = ::rocksdb::DB::Open(options, path, &exported);
    if (!status.ok()) return status_of(status);

    std::string file_path;
    status_t failure;
    for (column_t* const column : families) {
        column_t* target = nullptr;
        ::rocksdb::ColumnFamilyOptions column_options;
        column_options.comparator = column->GetComparator();
        status = exported->CreateColumnFamily(column_options, column->GetName(), &target);
        if (!status.ok()) break;

        ustore_size_t bytes = 0;
        column_file_path(file_path, path, column->GetID());
        std::unique_ptr<::rocksdb::Iterator> iterator {database.native->NewIterator(snapshot_options, column)};
        status = iterator ? export_column(*iterator, options, file_path, *exported, target, bytes)
                          : native_status_t::Aborted();
        exported->DestroyColumnFamilyHandle(target);
        if (!status.ok()) break;
        if (!progress.advance(bytes)) {
            failure = progress_cancelled_k;
            break;
        }
    }

    exported.reset();
    if (status.ok() && failure.succeeded()) return {};
    // A failed save removes what it wrote, and says so when it cannot, since the path then refuses every retry
    if (failure.succeeded()) failure = status_of(status);
    if (!remove_partial_export(path, options, families))
        failure.message = "Failed to save the snapshot, and to remove the files it left behind";
    return failure;
}

#pragma endregion Snapshots

#pragma region Collections

/** Creates an empty column family for a collection holding @p modality under @p name, then puts both of its system
 *  entries in one batch, answering the identifier of the collection. The caller holds @c columns_mutex exclusively. */
expected<ustore_collection_id_t> create_family(database_t& database, ustore_modality_t modality,
                                               std::string_view name) {
    auto [id, mint_status] = mint_collection_id();
    if (mint_status.failed()) return mint_status;

    std::string column_name(uuid_text_length_k, '\0');
    uuid_to_text(id.uuid, std::span<char>(column_name).first<uuid_text_length_k>());
    column_t* column = nullptr;
    native_status_t status = database.native->CreateColumnFamily(::rocksdb::ColumnFamilyOptions(), column_name,
                                                                 &column);
    if (!status.ok()) return status_of(status);
    bool const recorded = st::succeeded(database.columns.upsert(
        st::mapping<ustore_collection_id_t, family_t> {id, family_t {column, modality, std::string(name)}}));
    if (!recorded) status = native_status_t::MemoryLimit();

    // RocksDB applies one batch atomically across column families, so no name outlives its collections entry
    std::string entry;
    entry.push_back(static_cast<char>(modality));
    entry.append(name);
    ::rocksdb::WriteBatch batch;
    if (status.ok()) status = batch.Put(database.collections, to_slice(id), entry);
    if (status.ok() && !name.empty()) status = batch.Put(database.names, name, to_slice(id));
    if (status.ok()) status = database.native->Write(write_options_for(ustore_durability_flushed_k), &batch);
    if (status.ok()) return id;

    // A column family no collections entry records could never be addressed, so it is dropped again
    if (recorded) [[maybe_unused]]
        bool const erased = database.columns.erase(id);
    [[maybe_unused]] native_status_t const dropped = database.native->DropColumnFamily(column);
    database.native->DestroyColumnFamilyHandle(column);
    return status_of(status);
}

/** Drops the column family @p id addresses, deleting first and durably both of its system entries in one batch. The
 *  caller holds @c columns_mutex exclusively. */
status_t drop_family(database_t& database, ustore_collection_id_t id) {
    auto const found = database.columns.find(id);
    if (found == database.columns.end()) return {status_code_t::not_found_k, "No such collection"};
    family_t const& family = (*found).mapped;
    column_t* const column = family.column;

    // A snapshot open across the drop still saves the column family, so its handle waits for the last snapshot
    std::lock_guard retired_lock {database.retired_mutex};
    bool const retiring = database.open_snapshots.load(std::memory_order_relaxed) != 0;
    if (retiring && st::failed(database.retired.reserve(database.retired.size() + 1)))
        return {status_code_t::out_of_memory_k, "Failed to keep the dropped column family for the open snapshots"};

    ::rocksdb::WriteBatch batch;
    native_status_t status = batch.Delete(database.collections, to_slice(id));
    if (status.ok() && !family.name.empty()) status = batch.Delete(database.names, family.name);
    if (status.ok()) status = database.native->Write(write_options_for(ustore_durability_flushed_k), &batch);
    if (!status.ok()) return status_of(status);

    // A column family the drop fails to remove keeps no collections entry, so the next read-write open drops it
    [[maybe_unused]] native_status_t const dropped = database.native->DropColumnFamily(column);
    [[maybe_unused]] bool const erased = database.columns.erase(id);
    if (retiring) [[maybe_unused]]
        auto const kept = database.retired.emplace_back(st::assume_reserved, column);
    else database.native->DestroyColumnFamilyHandle(column);
    return {};
}

status_t collection_create(ustore_collection_create_t const& arguments, view_t const&) {
    if (!arguments.collection) return {status_code_t::invalid_argument_k, "Need an output for the collection"};
    std::string_view const name = string_of(arguments.name);
    if (arguments.name.data && name.empty()) return {status_code_t::invalid_argument_k, "A name is never empty"};

    // Creators take the column families exclusively, so no other one takes the name between the lookup and the batch
    database_t& database = *state_of<database_t>(arguments.database);
    std::lock_guard lock {database.columns_mutex};
    if (!name.empty()) {
        std::string entry;
        native_status_t const looked_up = database.native->Get(::rocksdb::ReadOptions(), database.names, name, &entry);
        if (looked_up.ok()) return {status_code_t::already_exists_k, "A collection already carries the name"};
        if (!looked_up.IsNotFound()) return status_of(looked_up);
    }
    auto [id, create_status] = create_family(database, arguments.modality, name);
    if (create_status.failed()) return create_status;
    *arguments.collection = id;
    return {};
}

status_t collection_drop(ustore_collection_drop_t const& arguments, view_t const&) {
    if (arguments.mode != ustore_drop_collection_k && arguments.mode != ustore_drop_pairs_k &&
        arguments.mode != ustore_drop_values_k)
        return {status_code_t::invalid_argument_k, "Unknown drop mode"};

    database_t& database = *state_of<database_t>(arguments.database);
    if (arguments.mode == ustore_drop_collection_k) {
        std::lock_guard lock {database.columns_mutex};
        return drop_family(database, arguments.collection);
    }

    // Every value of a strings collection is a key, so none empties
    std::shared_lock lock {database.columns_mutex};
    if (arguments.mode == ustore_drop_values_k)
        if (status_t const refused = refuse_collection(database, arguments.collection,
                                                       entry_layout_t::uuids_to_bytes_k);
            refused.failed())
            return refused;
    column_t* const column = column_of(database, arguments.collection);
    if (!column) return {status_code_t::not_found_k, "No such collection"};
    return status_of(clear_column(database, column, arguments.mode));
}

status_t collection_list(ustore_collection_list_t const& arguments, view_t const& view) {
    if (!arguments.written) return {status_code_t::invalid_argument_k, "Need an output for the collections written"};

    database_t& database = *state_of<database_t>(arguments.database);
    std::shared_lock lock {database.columns_mutex};
    columns_t const& listed = view.snapshot ? state_of<snapshot_t>(view.snapshot)->columns : database.columns;
    collection_list_outputs_t outputs {view.scratch};
    for (auto const& [id, family] : listed) {
        value_view_t const name = family.name.empty() ? value_view_t {} : value_view_t {family.name};
        if (status_t const status = outputs.push(id, family.modality, name); status.failed()) return status;
    }
    return outputs.finish(arguments);
}

status_t collection_find(ustore_collection_find_t const& arguments, strings_arg_t const& names, view_t const& view) {
    if (!arguments.count) return {};
    if (!names || !arguments.collections)
        return {status_code_t::invalid_argument_k, "Need the names and an output for the identifiers"};

    database_t& database = *state_of<database_t>(arguments.database);
    ::rocksdb::ReadOptions read_options;
    use_snapshot(view, read_options);

    auto [outputs, outputs_status] = collection_find_outputs_t::make(arguments);
    if (outputs_status.failed()) return outputs_status;

    // A snapshot's names resolve into the collections it recorded, and the head's into the database's own
    std::shared_lock lock {database.columns_mutex};
    columns_t const& found = view.snapshot ? state_of<snapshot_t>(view.snapshot)->columns : database.columns;
    std::string entry;
    for (std::size_t index = 0; index != arguments.count; ++index) {
        native_status_t const status = database.native->Get(read_options, database.names,
                                                            ::rocksdb::Slice(string_of(names[index])), &entry);
        if (status.IsNotFound()) {
            outputs.assign(index, ustore_collection_id_t {}, ustore_modality_t {}, false);
            continue;
        }
        if (!status.ok()) return status_of(status);
        if (entry.size() != sizeof(ustore_collection_id_t))
            return {status_code_t::malformed_k, "The database holds a malformed names entry"};
        ustore_collection_id_t id;
        std::memcpy(&id, entry.data(), sizeof(ustore_collection_id_t));
        auto const family = found.find(id);
        if (family == found.end()) return {status_code_t::malformed_k, "A name addresses no collection"};
        outputs.assign(index, id, (*family).mapped.modality, true);
    }
    outputs.finish(arguments);
    return {};
}

/** Clears every collection as @p mode asks, keeping the collections and their names, where emptying values skips
 *  every strings collection, whose values are keys. */
status_t clear_families(database_t& database, ustore_drop_mode_t mode) {
    std::shared_lock lock {database.columns_mutex};
    for (auto const& [id, family] : database.columns) {
        bool const keeps_values = mode == ustore_drop_values_k &&
                                  entry_layout_of(family.modality) == entry_layout_t::strings_to_uuids_k;
        if (keeps_values) continue;
        native_status_t const status = clear_column(database, family.column, mode);
        if (!status.ok()) return status_of(status);
    }
    return {};
}

/** Drops every collection with both of its system entries, unless a snapshot is open, as the caller owns it and would
 *  read a database it never saw. */
status_t reset_database(database_t& database) {
    if (database.open_snapshots.load(std::memory_order_relaxed) != 0)
        return {status_code_t::invalid_argument_k, "Free every snapshot before resetting the database"};

    // The map cannot lose a column family while it is walked, so the ones to drop are gathered first
    std::lock_guard lock {database.columns_mutex};
    st::basic_vector<ustore_collection_id_t> dropped;
    if (st::failed(dropped.reserve(database.columns.size())))
        return {status_code_t::out_of_memory_k, "Failed to gather the collections"};
    for (auto const& [id, family] : database.columns) [[maybe_unused]]
        auto const gathered = dropped.push_back(st::assume_reserved, ustore_collection_id_t {id});
    for (ustore_collection_id_t const id : dropped)
        if (status_t const status = drop_family(database, id); status.failed()) return status;
    return {};
}

status_t database_clear(ustore_database_clear_t const& arguments, view_t const&) {
    database_t& database = *state_of<database_t>(arguments.database);
    switch (arguments.mode) {
    case ustore_drop_collection_k: return reset_database(database);
    case ustore_drop_pairs_k:
    case ustore_drop_values_k: return clear_families(database, arguments.mode);
    default: return {status_code_t::invalid_argument_k, "Unknown drop mode"};
    }
}

/** The bytes RocksDB estimates @p column holds in its memtables and table files, which a compaction rewrites. */
ustore_size_t column_bytes(database_t& database, column_t* column) {
    std::uint64_t memory_bytes = 0;
    std::uint64_t disk_bytes = 0;
    database.native->GetIntProperty(column, ::rocksdb::DB::Properties::kCurSizeAllMemTables, &memory_bytes);
    database.native->GetIntProperty(column, ::rocksdb::DB::Properties::kTotalSstFilesSize, &disk_bytes);
    return memory_bytes + disk_bytes;
}

/** Flushes every memtable and compacts every column family, the main and the system ones included, reporting the bytes
 *  each column family held before. */
status_t database_compact(ustore_database_compact_t const& arguments, view_t const&) {
    database_t& database = *state_of<database_t>(arguments.database);
    ::rocksdb::FlushOptions const flush_options;
    ::rocksdb::CompactRangeOptions const compact_options;
    std::shared_lock lock {database.columns_mutex};
    st::basic_vector<column_t*> families;
    if (status_t const status = gather_families(database, database.columns, families); status.failed()) return status;
    st::basic_vector<ustore_size_t> bytes;
    if (st::failed(bytes.resize(families.size())))
        return {status_code_t::out_of_memory_k, "Failed to measure the families"};
    for (std::size_t index = 0; index != families.size(); ++index)
        bytes[index] = column_bytes(database, families[index]);
    progress_t progress {arguments.progress, arguments.progress_state,
                         std::accumulate(bytes.begin(), bytes.end(), ustore_size_t {0})};
    for (std::size_t index = 0; index != families.size(); ++index) {
        native_status_t const flushed = database.native->Flush(flush_options, families[index]);
        if (!flushed.ok()) return status_of(flushed);
        native_status_t const compacted = database.native->CompactRange(compact_options, families[index], nullptr,
                                                                        nullptr);
        if (!compacted.ok()) return status_of(compacted);
        if (!progress.advance(bytes[index])) return progress_cancelled_k;
    }
    return {};
}

#pragma endregion Collections

#pragma region Transactions

/** Begins a RocksDB transaction, reinitializing @p reused unless it is null, and pinning no snapshot until the
 *  transaction's first read or write, so an idle handle holds back no compaction. */
::rocksdb::Transaction* begin_native(database_t& database, ::rocksdb::Transaction* reused) {
    ::rocksdb::TransactionOptions options;
    // Locks are taken only at stage, where a millisecond covers the lock stripe and a held key is a conflict
    options.lock_timeout = 1;
    return transactional(database).BeginTransaction(::rocksdb::WriteOptions(), options, reused);
}

/** Begins @p transaction afresh, reusing its RocksDB transaction and forgetting its writes and reads. */
status_t begin(database_t& database, transaction_t& transaction) {
    transaction.native.reset(begin_native(database, transaction.native.release()));
    transaction.batch.Clear();
    transaction.watched.clear();
    transaction.watched_strings.clear();
    transaction.written.clear();
    transaction.staging = staging_t::buffering_k;
    if (!transaction.native) return {status_code_t::internal_k, "Failed to begin a transaction"};
    return {};
}

/**
 *  @brief Stages @p transaction unless it already is: locks and validates every watched read and buffered write
 *      against the snapshot its first read or write took, then prepares the writes into the log.
 *
 *  A refused key rolls back to the save point taken before the first lock, leaving the transaction buffering and
 *  retryable as on the reference engine, unless RocksDB's prepare itself failed and left it needing a reset.
 */
status_t stage(database_t& database, transaction_t& transaction) {
    if (transaction.staging == staging_t::staged_k) return {};
    if (transaction.staging == staging_t::broken_k)
        return {status_code_t::invalid_argument_k, "A failed prepare left the transaction needing a reset"};

    // A prepare needs a name unique among open transactions only, as the database rolls back every older one it opens
    ::rocksdb::Transaction& native = *transaction.native;
    if (native.GetName().empty()) {
        native_status_t const named = native.SetName(std::to_string(native.GetID()));
        if (!named.ok()) return status_of(named);
    }

    // Sorted keys lock in one order across transactions and group by collection, and a key read twice locks once
    st::basic_vector<watched_key_t>& watched = transaction.watched;
    std::sort(watched.begin(), watched.end());
    std::size_t const distinct = static_cast<std::size_t>(std::unique(watched.begin(), watched.end()) -
                                                          watched.begin());
    watched.erase(distinct, watched.size() - distinct);

    native.SetSavePoint();
    ::rocksdb::ReadOptions const read_options;
    native_status_t status;
    column_t* column = nullptr;
    for (std::size_t index = 0; status.ok() && index != watched.size(); ++index) {
        if (index == 0 || watched[index - 1].collection != watched[index].collection)
            column = column_of(database, watched[index].collection);
        status = column ? native.GetForUpdate(read_options, column, to_slice(watched[index].key),
                                              static_cast<std::string*>(nullptr), false, true)
                        : native_status_t::NotFound("The collection was dropped");
    }
    auto& watched_strings = transaction.watched_strings;
    std::sort(watched_strings.begin(), watched_strings.end());
    std::size_t const distinct_strings = static_cast<std::size_t>(
        std::unique(watched_strings.begin(), watched_strings.end()) - watched_strings.begin());
    watched_strings.erase(distinct_strings, watched_strings.size() - distinct_strings);
    for (std::size_t index = 0; status.ok() && index != watched_strings.size(); ++index) {
        ustore_collection_id_t const collection = watched_strings[index].first;
        column = column_of(database, collection);
        status = column ? native.GetForUpdate(read_options, column, watched_strings[index].second,
                                              static_cast<std::string*>(nullptr), false, true)
                        : native_status_t::NotFound("The collection was dropped");
    }
    bool const reads_validated = status.ok();

    // The index holds each key's latest write once, in key order, so a key written twice replays and locks once
    for (std::size_t index = 0; status.ok() && index != transaction.written.size(); ++index) {
        column = column_of(database, transaction.written[index]);
        if (!column) {
            status = native_status_t::NotFound("The collection was dropped");
            break;
        }
        std::unique_ptr<::rocksdb::WBWIIterator> entries {transaction.batch.NewIterator(column)};
        for (entries->SeekToFirst(); status.ok() && entries->Valid(); entries->Next()) {
            ::rocksdb::WriteEntry const entry = entries->Entry();
            status = entry.type == ::rocksdb::kDeleteRecord ? native.Delete(column, entry.key)
                                                            : native.Put(column, entry.key, entry.value);
        }
        if (status.ok()) status = entries->status();
    }
    if (status.ok()) status = native.Prepare();
    if (status.ok()) {
        transaction.staging = staging_t::staged_k;
        return {};
    }

    // Undoing the stage keeps the transaction retryable, and a transaction that cannot be undone needs a reset
    bool const undone = native.GetState() == ::rocksdb::Transaction::STARTED && native.RollbackToSavePoint().ok();
    if (!undone) transaction.staging = staging_t::broken_k;

    if (!status.IsBusy() && !status.IsTimedOut()) return status_of(status);
    if (reads_validated)
        return {status_code_t::write_conflict_k, "A concurrent write changed a key the transaction wrote"};
    return {status_code_t::read_conflict_k, "A concurrent write changed a key the transaction read"};
}

status_t transaction_init(ustore_transaction_init_t const& arguments, view_t const&) {
    if (status_t const status = validate_transaction_begin(arguments.transaction); status.failed()) return status;

    database_t& database = *state_of<database_t>(arguments.database);
    std::unique_ptr<ustore_transaction_handle_t> handle;
    std::unique_ptr<transaction_t> created;
    transaction_t* transaction = state_of<transaction_t>(*arguments.transaction);
    if (!transaction) {
        handle.reset(new (std::nothrow) ustore_transaction_handle_t {});
        created.reset(handle ? new (std::nothrow) transaction_t : nullptr);
        if (!created) return {status_code_t::out_of_memory_k, "Failed to allocate the transaction"};
        transaction = created.get();
    }
    // A prepare left behind would pin the log and come back after a crash
    else if (transaction->staging == staging_t::staged_k) {
        native_status_t const rolled_back = transaction->native->Rollback();
        if (!rolled_back.ok()) return status_of(rolled_back);
    }

    if (status_t const status = begin(database, *transaction); status.failed()) return status;
    if (created) {
        handle->engine = &engine_k, handle->state = created.release();
        *arguments.transaction = handle.release();
    }
    return {};
}

status_t transaction_stage(ustore_transaction_stage_t const& arguments, view_t const&) {
    if (!arguments.transaction) return {status_code_t::uninitialized_k, "Transaction is uninitialized"};
    database_t& database = *state_of<database_t>(arguments.database);
    transaction_t& transaction = *state_of<transaction_t>(arguments.transaction);
    std::shared_lock lock {database.columns_mutex};
    // A transaction that neither read nor wrote takes its snapshot here, which its commit then validates against
    ::rocksdb::Snapshot const* const snapshot = snapshot_of(transaction);
    if (status_t const status = stage(database, transaction); status.failed()) return status;
    if (arguments.read_stamp) *arguments.read_stamp = snapshot->GetSequenceNumber();
    return {};
}

status_t transaction_commit(ustore_transaction_commit_t const& arguments, view_t const&) {
    status_t const validated = validate_transaction_commit(arguments.transaction, arguments.durability);
    if (validated.failed()) return validated;

    database_t& database = *state_of<database_t>(arguments.database);
    transaction_t& transaction = *state_of<transaction_t>(arguments.transaction);
    std::shared_lock lock {database.columns_mutex};
    if (status_t const status = stage(database, transaction); status.failed()) return status;

    // A flushed commit syncs the log within its own write, so no reader sees it before it is durable, and any other
    // commit waits in the log's buffer until a flushed one, a full buffer or a clean close writes it out
    ::rocksdb::Transaction& native = *transaction.native;
    native.SetWriteOptions(write_options_for(arguments.durability));
    native_status_t const committed = native.Commit();
    if (!committed.ok()) return status_of(committed);
    if (arguments.commit_stamp) *arguments.commit_stamp = native.GetId();

    // The handle carries on as a new transaction, as a reset would leave it
    return begin(database, transaction);
}

status_t transaction_rollback(ustore_transaction_rollback_t const& arguments, view_t const&) {
    if (!arguments.transaction) return {status_code_t::uninitialized_k, "Transaction is uninitialized"};
    transaction_t& transaction = *state_of<transaction_t>(arguments.transaction);
    if (transaction.staging == staging_t::broken_k)
        return {status_code_t::invalid_argument_k, "A failed prepare left the transaction needing a reset"};
    if (transaction.staging == staging_t::buffering_k) return {};

    // The successor takes its snapshot while the stage still locks every key it read or wrote, so staging again
    // validates the kept reads and writes against every change since the first stage
    database_t& database = *state_of<database_t>(arguments.database);
    std::unique_ptr<::rocksdb::Transaction> successor {begin_native(database, nullptr)};
    if (!successor) return {status_code_t::internal_k, "Failed to begin a transaction"};
    successor->SetSnapshot();
    native_status_t const rolled_back = transaction.native->Rollback();
    if (!rolled_back.ok()) return status_of(rolled_back);
    transaction.native = std::move(successor);
    transaction.staging = staging_t::buffering_k;
    return {};
}

#pragma endregion Transactions

#pragma region Memory

void transaction_free(ustore_transaction_t handle) noexcept {
    transaction_t* const transaction = state_of<transaction_t>(handle);
    // A prepare left behind would pin the log and come back after a crash
    if (transaction->staging == staging_t::staged_k) {
        [[maybe_unused]] native_status_t const rolled_back = transaction->native->Rollback();
    }
    delete transaction;
}

void database_free(ustore_database_t database) noexcept { delete state_of<database_t>(database); }

void snapshot_free(ustore_snapshot_t handle) noexcept {
    snapshot_t* const snapshot = state_of<snapshot_t>(handle);
    database_t& database = *snapshot->database;
    database.native->ReleaseSnapshot(snapshot->native);
    delete snapshot;
    std::lock_guard retired_lock {database.retired_mutex};
    if (database.open_snapshots.fetch_sub(1, std::memory_order_relaxed) != 1) return;
    for (column_t* const column : database.retired) database.native->DestroyColumnFamilyHandle(column);
    database.retired.clear();
}

#pragma endregion Memory

} // namespace unum::ustore::oracles::rocksdb
