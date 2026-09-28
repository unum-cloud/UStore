/**
 *  @file src_oracles/rocksdb/engine.cpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief Embedded persistent key-value store on RocksDB, with two-phase transactions and snapshots.
 *
 *  Named collections are column families, and keys are fixed-width integers under a custom comparator.
 *  Tables stay block-based, since the plain-table format lacks non-prefix seeks that scans need.
 *
 *  Every write reaches RocksDB's log, which stays in memory until a flushed write or commit, a full buffer or a clean
 *  close writes it out, so a crash recovers a consistent prefix of the commits across every collection. Transactions
 *  buffer their writes and watched reads and lock nothing until they stage, when RocksDB's pessimistic transaction
 *  locks and validates every key against the snapshot its first read or write took, and prepares it into the log.
 */

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::strlen`

#include <algorithm>    // `std::sort`, `std::unique`
#include <array>        // `std::array`
#include <atomic>       // `std::atomic`
#include <compare>      // `std::strong_ordering`
#include <filesystem>   // `std::filesystem::remove`
#include <iterator>     // `std::back_inserter`
#include <limits>       // `std::numeric_limits`
#include <memory>       // `std::unique_ptr`
#include <mutex>        // `std::lock_guard`
#include <shared_mutex> // `std::shared_lock`, `std::shared_mutex`
#include <new>          // `std::nothrow`
#include <span>         // `std::span`
#include <string>       // `std::string`, `std::to_string`, the path and name type RocksDB takes
#include <string_view>  // `std::string_view`
#include <system_error> // `std::error_code`
#include <utility>      // `std::pair`
#include <vector>       // `std::vector`, the descriptor, handle and file lists RocksDB takes

#include <fmt/format.h> // `fmt::format_to`, `fmt::format_to_n`
#include <rocksdb/db.h>
#include <rocksdb/sst_file_writer.h>
#include <rocksdb/utilities/options_util.h>
#include <rocksdb/utilities/transaction.h>
#include <rocksdb/utilities/transaction_db.h>
#include <rocksdb/utilities/write_batch_with_index.h>
#include <rocksdb/version.h>

#include <smashtable/basic_hash_table.hpp>
#include <smashtable/basic_vector.hpp>

#include <ustore/db.h>
#include <ustore/ranges_args.hpp> // `places_arg_t`

#include "engine.hpp"                 // `engine_t`, `handle_t`, `handle_cast`
#include "helpers/blocking_files.hpp" // `directory_exists`
#include "helpers/full_scan.hpp"      // `reservoir_sample_iterator`
#include "helpers/linked_array.hpp"   // `growing_tape_t`, `uninitialized_array`
#include "helpers/linked_memory.hpp"  // `linked_memory_lock_t`

namespace unum::ustore::rocksdb {

#pragma region Database

/** The open RocksDB database. */
using native_t = ::rocksdb::TransactionDB;

/** Status RocksDB answers. */
using native_status_t = ::rocksdb::Status;

/** A column family, which is how named collections are stored. */
using column_t = ::rocksdb::ColumnFamilyHandle;

/** A column family a caller created, what it holds and the name it carries, empty for none. */
struct family_t {
    column_t* column = nullptr;
    ustore_modality_t modality = ustore_modality_any_k;
    std::string name;
};

/** The column families callers created, by the identifier of their collection, which the database destroys before it
 *  closes. Every family keeps RocksDB's bytewise order, which is the order of 16-byte keys and of strings alike. */
using columns_t = st::hash_map<ustore_collection_id_t, family_t, std::hash<ustore_collection_id_t>>;

/** The name of the column family holding the names of collections, which no identifier reaches. */
constexpr char const* names_column_name_k = "ustore.names";

/** The spelling of every modality in the order of @c ustore_modality_t, which leads the name of a family holding it. */
constexpr std::array<std::string_view, 6> modality_names_k {"any", "blobs", "docs", "graph", "strings", "pages"};

/** Reads what a family holds and the collection it stores from its name, `<modality>.<identifier>`, answering false
 *  for a name this engine never gives. */
bool read_family_name(std::string_view name, ustore_modality_t& modality, ustore_collection_id_t& id) noexcept {
    std::size_t const dot = name.find('.');
    if (dot == std::string_view::npos) return false;
    auto [uuid, uuid_status] = uuid_from_text(name.substr(dot + 1));
    if (uuid_status.failed()) return false;
    id.uuid = uuid;
    for (std::size_t index = 0; index != modality_names_k.size(); ++index)
        if (name.substr(0, dot) == modality_names_k[index]) {
            modality = static_cast<ustore_modality_t>(index);
            return true;
        }
    return false;
}

/** Milliseconds a write outside any transaction waits for a key a staged transaction holds before it conflicts, where
 *  RocksDB would wait a whole second. */
constexpr std::int64_t head_write_lock_timeout_ms_k = 16;

/** The calls this engine serves, which stamp every handle it hands out. */
extern engine_t const engine_k;

/** The open database and its column families. */
struct database_t : handle_t {

    /** The column families callers created, which leave out the main and the names ones. */
    columns_t columns;

    /** Shared by every call for as long as it runs, exclusive for a create, a drop or a reset. */
    std::shared_mutex columns_mutex;

    /** The default column family, which holds the main collection. */
    column_t* main = nullptr;

    /** The column family holding the names of collections, keyed by the names themselves. */
    column_t* names = nullptr;

    /** The open database. */
    std::unique_ptr<native_t> native;

    /** Snapshots handed out and not yet freed, which a reset refuses to run under. */
    std::atomic<std::size_t> open_snapshots = 0;

    /** Writes the log's buffer out and closes, whether the database opened fully or failed halfway. A snapshot the
     *  caller never freed makes `Close` refuse, which is the caller's error, so the database goes regardless. */
    ~database_t() noexcept {
        if (!native) return;
        [[maybe_unused]] native_status_t const flushed = native->FlushWAL(true);
        for (auto const& [id, family] : columns) native->DestroyColumnFamilyHandle(family.column);
        if (main) native->DestroyColumnFamilyHandle(main);
        if (names) native->DestroyColumnFamilyHandle(names);
        [[maybe_unused]] native_status_t const closed = native->Close();
    }
};

/** A snapshot handle, pinning one RocksDB snapshot of its database until freed. */
struct snapshot_t : handle_t {

    /** The database the snapshot was taken of, which releases it. */
    database_t* database = nullptr;

    /** The RocksDB snapshot. */
    ::rocksdb::Snapshot const* native = nullptr;
};

/** A key a transaction read while watching it, locked and validated when the transaction stages. */
struct watched_key_t {
    ustore_collection_id_t collection = ustore_collection_main_k;
    ustore_key_t key {};

    friend std::strong_ordering operator<=>(watched_key_t const&, watched_key_t const&) noexcept = default;
};

/** How far a transaction got towards its commit. */
enum class staging_t : std::uint8_t {

    /** Buffering writes and watched reads, holding no locks. */
    pending_k,

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
struct transaction_t : handle_t {

    /** RocksDB's transaction, holding the snapshot from its first read or write and the locks once staged. */
    std::unique_ptr<::rocksdb::Transaction> native;

    /** Writes not yet replayed, which reads in the transaction see before the database. */
    ::rocksdb::WriteBatchWithIndex batch {::rocksdb::BytewiseComparator(), 0, true};

    /** Keys read while watching them. */
    st::basic_vector<watched_key_t> watched;

    /** Strings read while watching them, with the strings collections holding them. */
    std::vector<std::pair<ustore_collection_id_t, std::string>> watched_strings;

    /** Collections holding buffered writes, each replayed from the batch's index at stage. */
    st::basic_vector<ustore_collection_id_t> written;

    /** How far the transaction got. */
    staging_t staging = staging_t::pending_k;
};

inline ::rocksdb::Slice to_slice(ustore_key_t const& key) noexcept {
    return {reinterpret_cast<char const*>(&key), sizeof(ustore_key_t)};
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

/** Buffers one write of @p place into @p transaction, noting its collection for the replay at stage. */
native_status_t buffer_write(transaction_t& transaction, column_t* column, ustore_collection_id_t collection,
                             ::rocksdb::Slice key, value_view_t content) {
    [[maybe_unused]] ::rocksdb::Snapshot const* const snapshot = snapshot_of(transaction);
    native_status_t const status = !content ? transaction.batch.Delete(column, key)
                                            : transaction.batch.Put(column, key, to_slice(content));
    if (!status.ok()) return status;
    for (ustore_collection_id_t const written : transaction.written)
        if (written == collection) return status;
    if (st::failed(transaction.written.emplace_back(collection))) return native_status_t::MemoryLimit();
    return status;
}

/** Exports a failed status into @p error, answering whether it failed. */
bool export_error(native_status_t const& status, ustore_error_t* error) noexcept {
    if (status.ok()) return false;

    if (status.IsNoSpace()) *error = {ustore_error_capacity_exhausted_k, "RocksDB ran out of disk space"};
    else if (status.IsCorruption()) *error = {ustore_error_malformed_k, "RocksDB found corrupted data"};
    else if (status.IsIOError()) *error = {ustore_error_io_k, "RocksDB failed an I/O operation"};
    else if (status.IsInvalidArgument()) *error = {ustore_error_invalid_argument_k, "RocksDB rejected an argument"};
    else if (status.IsNotFound()) *error = {ustore_error_not_found_k, "RocksDB found no such entry"};
    else if (status.IsNotSupported()) *error = {ustore_error_unsupported_k, "RocksDB does not support the operation"};
    else if (status.IsBusy() || status.IsTryAgain() || status.IsTimedOut())
        *error = {ustore_error_write_conflict_k, "A concurrent write changed a key the transaction wrote"};
    else if (status.IsMemoryLimit()) *error = {ustore_error_out_of_memory_k, "RocksDB reached its memory limit"};
    else if (status.IsLockLimit()) *error = {ustore_error_capacity_exhausted_k, "RocksDB ran out of lock slots"};
    else if (status.IsTxnNotPrepared())
        *error = {ustore_error_invalid_argument_k, "The transaction was not prepared before its commit"};
    else *error = {ustore_error_internal_k, "RocksDB failed"};
    return true;
}

/** The family @p collection addresses, or null for any identifier no caller-created family carries. The caller holds
 *  @c columns_mutex, which keeps the family alive for as long as it holds it. */
column_t* column_of(database_t& database, ustore_collection_id_t collection) noexcept {
    if (collection == ustore_collection_main_k) return database.main;
    auto const found = database.columns.find(collection);
    return found != database.columns.end() ? (*found).mapped.column : nullptr;
}

/** Refuses @p collection unless a family keyed as @p keying holds it, answering whether it refused. The caller holds
 *  @c columns_mutex. */
bool refuse_collection(database_t& database, ustore_collection_id_t collection, keying_t keying,
                       ustore_error_t* error) noexcept {
    keying_t held = keying_t::uuids_k;
    if (collection != ustore_collection_main_k) {
        auto const found = database.columns.find(collection);
        if (found == database.columns.end()) {
            *error = {ustore_error_not_found_k, "No such collection"};
            return true;
        }
        held = keying_of((*found).mapped.modality);
    }
    if (held == keying) return false;
    *error = {ustore_error_invalid_argument_k,
              keying == keying_t::strings_k ? "The collection holds no strings" : "The collection holds strings"};
    return true;
}

/** Refuses a batch addressing a collection no family keyed as @p keying holds, answering whether it refused. The caller
 *  holds @c columns_mutex, so every collection it accepts stays addressable until the call returns. */
bool refuse_collections(database_t& database, strided_iterator<ustore_collection_id_t const> collections,
                        std::size_t count, keying_t keying, ustore_error_t* error) noexcept {
    if (!collections) return refuse_collection(database, ustore_collection_main_k, keying, error);
    for (std::size_t index = 0; index != count; ++index) {
        bool const repeated = index != 0 && collections[index] == collections[index - 1];
        if (!repeated && refuse_collection(database, collections[index], keying, error)) return true;
    }
    return false;
}

/** Removes every pair of @p column in one synchronous batch, or empties every value for @c ustore_drop_vals_k. */
native_status_t clear_column(database_t& database, column_t* column, ustore_drop_mode_t mode) {
    ::rocksdb::WriteBatch batch;
    std::unique_ptr<::rocksdb::Iterator> iterator {database.native->NewIterator(::rocksdb::ReadOptions(), column)};
    native_status_t status;
    for (iterator->SeekToFirst(); status.ok() && iterator->Valid(); iterator->Next())
        status = mode == ustore_drop_vals_k ? batch.Put(column, iterator->key(), ::rocksdb::Slice())
                                            : batch.Delete(column, iterator->key());
    if (status.ok()) status = iterator->status();
    if (!status.ok()) return status;

    // Clearing is not transactional, so it takes no locks behind a staged transaction
    ::rocksdb::WriteOptions write_options;
    write_options.sync = true;
    ::rocksdb::TransactionDBWriteOptimizations optimizations;
    optimizations.skip_concurrency_control = true;
    return database.native->Write(write_options, optimizations, &batch);
}

/** Points @p options at @p snapshot, leaving them at the head for a call naming none. */
void use_snapshot(ustore_snapshot_t snapshot, ::rocksdb::ReadOptions& options) noexcept {
    if (snapshot) options.snapshot = handle_cast<snapshot_t>(snapshot)->native;
}

#pragma endregion Database

#pragma region Reads and Writes

/** An upper bound on the bytes a batch record spends beyond its key and value. */
constexpr std::size_t record_overhead_k = 16;

void write_one(database_t& database, transaction_t* transaction, places_arg_t const& places,
               contents_arg_t const& contents, ustore_options_t const options, ustore_error_t* error) {

    // Syncing writes the log's buffer out first, so a flushed write keeps every write before it too
    ::rocksdb::WriteOptions write_options;
    write_options.sync = options & ustore_option_write_flush_k;

    auto place = places[0];
    auto content = contents[0];
    column_t* const column = column_of(database, place.collection);
    auto const key = to_slice(place.key);
    native_status_t status;

    if (transaction) status = buffer_write(*transaction, column, place.collection, key, content);
    else
        status = !content //
                     ? database.native->Delete(write_options, column, key)
                     : database.native->Put(write_options, column, key, to_slice(content));

    export_error(status, error);
}

void write_many(database_t& database, transaction_t* transaction, places_arg_t const& places,
                contents_arg_t const& contents, ustore_options_t const options, ustore_error_t* error) {

    ::rocksdb::WriteOptions write_options;
    write_options.sync = options & ustore_option_write_flush_k;

    if (transaction) {
        for (std::size_t task_index = 0; task_index != places.size(); ++task_index) {
            auto place = places[task_index];
            auto content = contents[task_index];
            column_t* const column = column_of(database, place.collection);
            if (export_error(buffer_write(*transaction, column, place.collection, to_slice(place.key), content), error))
                return;
        }
        return;
    }

    // Reserving the whole batch spares its buffer from regrowing, and the spare record covers the header
    std::size_t reserved_bytes = record_overhead_k;
    for (std::size_t task_index = 0; task_index != places.size(); ++task_index)
        reserved_bytes += record_overhead_k + sizeof(ustore_key_t) + contents[task_index].size();

    ::rocksdb::WriteBatch batch {reserved_bytes};
    for (std::size_t task_index = 0; task_index != places.size(); ++task_index) {
        auto place = places[task_index];
        auto content = contents[task_index];
        column_t* const column = column_of(database, place.collection);
        auto const key = to_slice(place.key);
        native_status_t const status = !content ? batch.Delete(column, key) : batch.Put(column, key, to_slice(content));
        if (export_error(status, error)) return;
    }

    native_status_t const status = database.native->Write(write_options, &batch);
    export_error(status, error);
}

/** Records @p place among the keys @p transaction validates when it stages, unless @p options say not to watch it. */
native_status_t watch(transaction_t& transaction, place_t const& place, ustore_options_t const options) noexcept {
    if (options & ustore_option_transaction_dont_watch_k) return native_status_t::OK();
    if (st::failed(transaction.watched.emplace_back(watched_key_t {place.collection, place.key})))
        return native_status_t::MemoryLimit();
    return native_status_t::OK();
}

/** Reads one place into @p value, from the transaction's writes before the database when a transaction reads it. */
native_status_t get_one(database_t& database, transaction_t* transaction, ::rocksdb::ReadOptions const& read_options,
                        place_t const& place, ustore_options_t const options, ::rocksdb::PinnableSlice& value) {

    column_t* const column = column_of(database, place.collection);
    auto const key = to_slice(place.key);
    if (!transaction) return database.native->Get(read_options, column, key, &value);

    native_status_t const watched = watch(*transaction, place, options);
    if (!watched.ok()) return watched;
    return transaction->batch.GetFromBatchAndDB(database.native.get(), read_options, column, key, &value);
}

/** Whether the keys of @p places ascend, which spares a batched read its own sort. */
bool keys_ascend(places_arg_t const& places) noexcept {
    for (std::size_t task_index = 1; task_index < places.size(); ++task_index)
        if (places[task_index].key < places[task_index - 1].key) return false;
    return true;
}

/** Reads every place in one batch, leaving each value pinned in @p values and its outcome in @p statuses. */
void get_many(database_t& database, transaction_t* transaction, ::rocksdb::ReadOptions const& read_options,
              places_arg_t const& places, ustore_options_t const options, ::rocksdb::PinnableSlice* values,
              native_status_t* statuses, ustore_error_t* error) {

    std::size_t const count = places.size();
    bool const same_collection = places.same_collection();

    // A transaction's writes are merged into one collection at a time, so a mixed batch reads key by key
    if (transaction && !same_collection) {
        for (std::size_t task_index = 0; task_index != count; ++task_index)
            statuses[task_index] = get_one(database, transaction, read_options, places[task_index], options,
                                           values[task_index]);
        return;
    }

    std::unique_ptr<::rocksdb::Slice[]> keys {new (std::nothrow)::rocksdb::Slice[count]};
    if (!keys) {
        *error = {ustore_error_out_of_memory_k, "Failed to allocate the batch"};
        return;
    }
    for (std::size_t task_index = 0; task_index != count; ++task_index)
        keys[task_index] = to_slice(places[task_index].key);

    if (same_collection) {
        column_t* const column = column_of(database, places[0].collection);
        bool const sorted = keys_ascend(places);
        if (!transaction)
            return database.native->MultiGet(read_options, column, count, keys.get(), values, statuses, sorted);
        for (std::size_t task_index = 0; task_index != count; ++task_index)
            if (export_error(watch(*transaction, places[task_index], options), error)) return;
        transaction->batch.MultiGetFromBatchAndDB(database.native.get(), read_options, column, count, keys.get(),
                                                  values, statuses, sorted);
        return;
    }

    std::unique_ptr<column_t*[]> columns {new (std::nothrow) column_t*[count]};
    if (!columns) {
        *error = {ustore_error_out_of_memory_k, "Failed to allocate the batch"};
        return;
    }
    for (std::size_t task_index = 0; task_index != count; ++task_index)
        columns[task_index] = column_of(database, places[task_index].collection);
    database.native->MultiGet(read_options, count, columns.get(), keys.get(), values, statuses);
}

/** Exports what each read found, copying every found value into one arena tape exactly once. */
void export_values(ustore_read_t& arguments, linked_memory_lock_t& arena, ::rocksdb::PinnableSlice const* values,
                   native_status_t const* statuses, std::size_t const count) noexcept {

    std::size_t tape_bytes = 0;
    for (std::size_t task_index = 0; task_index != count; ++task_index) {
        if (statuses[task_index].IsNotFound()) continue;
        if (export_error(statuses[task_index], arguments.error)) return;
        tape_bytes += values[task_index].size();
    }

    auto offsets = arena.alloc_or_dummy(count + 1, arguments.error, arguments.offsets);
    if (arguments.error->status) return;
    auto lengths = arena.alloc_or_dummy(count, arguments.error, arguments.lengths);
    if (arguments.error->status) return;
    auto presences = arena.alloc_or_dummy(count, arguments.error, arguments.presences);
    if (arguments.error->status) return;

    bool const needs_export = arguments.values != nullptr;
    byte_t* const tape = needs_export ? arena.alloc<byte_t>(tape_bytes, arguments.error).data() : nullptr;
    if (arguments.error->status) return;

    std::size_t offset = 0;
    for (std::size_t task_index = 0; task_index != count; ++task_index) {
        bool const found = statuses[task_index].ok();
        std::size_t const length = found ? values[task_index].size() : 0;
        presences[task_index] = found;
        lengths[task_index] = found ? static_cast<ustore_length_t>(length) : ustore_length_missing_k;
        offsets[task_index] = static_cast<ustore_length_t>(offset);
        if (!needs_export || !length) continue;
        std::memcpy(tape + offset, values[task_index].data(), length);
        offset += length;
    }
    offsets[count] = static_cast<ustore_length_t>(offset);

    if (needs_export) *arguments.values = reinterpret_cast<ustore_bytes_ptr_t>(tape);
}

/** The transaction a read goes through, pointing @p read_options at the snapshot the transaction reads, or null for a
 *  read of the head or of a snapshot, which the C ABI never lets name a transaction as well. */
transaction_t* reading_transaction(ustore_transaction_t handle, ::rocksdb::ReadOptions& read_options) noexcept {
    if (!handle) return nullptr;
    transaction_t* const transaction = handle_cast<transaction_t>(handle);
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

/** Writes what @p iterator walks into an SST file at @p file_path, then moves that file into @p column of @p target. */
native_status_t export_column(::rocksdb::Iterator& iterator, ::rocksdb::Options const& options,
                              std::string const& file_path, ::rocksdb::DB& target, column_t* column) {

    iterator.SeekToFirst();
    if (!iterator.Valid()) return iterator.status();

    // An SST file skips the memtable and the log, and ingestion moves it into place without copying
    ::rocksdb::SstFileWriter writer {::rocksdb::EnvOptions(), options, column};
    native_status_t status = writer.Open(file_path);
    for (; status.ok() && iterator.Valid(); iterator.Next()) status = writer.Put(iterator.key(), iterator.value());
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

/** Removes what a failed export of @p families left at @p path, answering whether nothing remains: first the SST
 *  files, which RocksDB does not recognize as its own, then the database around them. */
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

void database_init(ustore_database_init_rocksdb_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need an output for the database"};
        return;
    }
    if (!arguments.directory) {
        *arguments.error = {ustore_error_invalid_argument_k, "A database needs a directory"};
        return;
    }
    if (arguments.data_directories_count && !arguments.data_directories) {
        *arguments.error = {ustore_error_invalid_argument_k, "Counted data directories are null"};
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

    // Recovery depends on the options file: https://github.com/facebook/rocksdb/wiki/RocksDB-Options-File
    ::rocksdb::Options options;
    options.compression = ::rocksdb::kNoCompression;
    std::vector<::rocksdb::ColumnFamilyDescriptor> column_descriptors;
    std::string const root(arguments.directory);
    native_status_t status = ::rocksdb::LoadLatestOptions(::rocksdb::ConfigOptions(), root, &options,
                                                          &column_descriptors);
    if (!status.ok() && !status.IsNotFound()) {
        *arguments.error = {ustore_error_io_k, "Failed to recover the RocksDB state"};
        return;
    }

    if (column_descriptors.empty())
        column_descriptors.push_back({::rocksdb::kDefaultColumnFamilyName, ::rocksdb::ColumnFamilyOptions()});
    // A family no caller could have created refuses the open
    for (auto& descriptor : column_descriptors) {
        ustore_modality_t modality = ustore_modality_any_k;
        ustore_collection_id_t id {};
        bool const known = descriptor.name == ::rocksdb::kDefaultColumnFamilyName ||
                           descriptor.name == names_column_name_k || read_family_name(descriptor.name, modality, id);
        if (!known) {
            *arguments.error = {ustore_error_malformed_k, "The database holds a column family of another layout"};
            return;
        }
    }

    // An export persists `error_if_exists`, which would refuse every reopening of it
    options.create_if_missing = true;
    options.error_if_exists = false;
    // Every write reaches the log, which stays in memory until a flushed write, a full buffer or a clean close; set
    // after the options files, so a database created before keeps no other setting
    options.manual_wal_flush = true;
    for (ustore_data_directory_t const& directory :
         std::span {arguments.data_directories, static_cast<std::size_t>(arguments.data_directories_count)}) {
        if (!directory.path) {
            *arguments.error = {ustore_error_invalid_argument_k, "A data directory needs a path"};
            return;
        }
        options.db_paths.push_back(
            {std::string(directory.path), directory.max_bytes ? directory.max_bytes : UINT64_MAX});
    }

    std::unique_ptr<database_t> database {new (std::nothrow) database_t};
    if (!database) {
        *arguments.error = {ustore_error_out_of_memory_k, "Failed to allocate the database"};
        return;
    }

    // A write outside any transaction waits this long for a key a staged transaction holds, and then conflicts
    ::rocksdb::TransactionDBOptions transaction_options;
    transaction_options.default_lock_timeout = head_write_lock_timeout_ms_k;
    native_t* native = nullptr;
    std::vector<column_t*> handles;
    status = native_t::Open(options, transaction_options, root, column_descriptors, &handles, &native);
    if (export_error(status, arguments.error)) return;
    database->native.reset(native);
    // The database owns every handle the moment it opens, so a failure below still closes them
    bool recorded = true;
    for (column_t* const handle : handles) {
        ustore_modality_t modality = ustore_modality_any_k;
        ustore_collection_id_t id {};
        if (handle->GetName() == ::rocksdb::kDefaultColumnFamilyName) database->main = handle;
        else if (handle->GetName() == names_column_name_k) database->names = handle;
        else if (read_family_name(handle->GetName(), modality, id) &&
                 st::succeeded(database->columns.upsert(
                     st::mapping<ustore_collection_id_t, family_t> {id, family_t {handle, modality}})))
            continue;
        else {
            recorded = false;
            native->DestroyColumnFamilyHandle(handle);
        }
    }
    if (!recorded) {
        *arguments.error = {ustore_error_out_of_memory_k, "Failed to record the collections"};
        return;
    }

    // No call resumes a transaction a crash left prepared, so each is rolled back, releasing the log it pins
    std::vector<::rocksdb::Transaction*> prepared;
    database->native->GetAllPreparedTransactions(&prepared);
    native_status_t rolled_back;
    for (::rocksdb::Transaction* const recovered : prepared) {
        native_status_t const rollback = recovered->Rollback();
        if (rolled_back.ok()) rolled_back = rollback;
        delete recovered;
    }
    if (export_error(rolled_back, arguments.error)) return;

    // The names live in a family of their own, keyed by the names, created the first time a database opens
    if (!database->names) {
        ::rocksdb::ColumnFamilyOptions names_options;
        names_options.comparator = ::rocksdb::BytewiseComparator();
        status = database->native->CreateColumnFamily(names_options, names_column_name_k, &database->names);
        if (export_error(status, arguments.error)) return;
    }

    // Every family remembers the name it carries, so a listing walks no names and a drop removes its own at once
    std::unique_ptr<::rocksdb::Iterator> names {
        database->native->NewIterator(::rocksdb::ReadOptions(), database->names)};
    for (names->SeekToFirst(); names->Valid(); names->Next()) {
        ustore_collection_id_t id = ustore_collection_main_k;
        ustore_modality_t modality = ustore_modality_any_k;
        if (!read_name_record(names->value(), id, modality)) {
            *arguments.error = {ustore_error_malformed_k, "A name holds no collection record"};
            return;
        }
        auto const found = database->columns.find(id);
        if (found != database->columns.end()) (*found).mapped.name = names->key().ToString();
    }
    if (export_error(names->status(), arguments.error)) return;
    names.reset();
    database->engine = &engine_k;
    *arguments.db = c_handle<ustore_database_t>(database.release());
}

void database_capabilities(ustore_database_capabilities_t& arguments) {
    *arguments.capabilities = ustore_capabilities_t(ustore_supports_transactions_k | ustore_supports_collections_k |
                                                    ustore_supports_snapshots_k);
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
    database.open_snapshots.fetch_add(1, std::memory_order_relaxed);
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
    ::rocksdb::ReadOptions snapshot_options;
    snapshot_options.fill_cache = false;
    use_snapshot(arguments.snapshot, snapshot_options);

    ::rocksdb::Options options;
    options.compression = ::rocksdb::kNoCompression;
    options.create_if_missing = true;
    options.error_if_exists = true;

    std::string const path(arguments.path);
    std::unique_ptr<::rocksdb::DB> exported;
    native_status_t status = ::rocksdb::DB::Open(options, path, &exported);
    if (export_error(status, arguments.error)) return;

    // Every family travels under its own name and comparator, the names among them
    std::string file_path;
    std::shared_lock lock {database.columns_mutex};
    std::vector<column_t*> families {database.main, database.names};
    for (auto const& [id, family] : database.columns) families.push_back(family.column);
    for (column_t* const column : families) {
        column_t* target = exported->DefaultColumnFamily();
        if (column != database.main) {
            ::rocksdb::ColumnFamilyOptions column_options;
            column_options.comparator = column->GetComparator();
            status = exported->CreateColumnFamily(column_options, column->GetName(), &target);
        }
        if (!status.ok()) break;

        column_file_path(file_path, path, column->GetID());
        std::unique_ptr<::rocksdb::Iterator> iterator {database.native->NewIterator(snapshot_options, column)};
        status = iterator ? export_column(*iterator, options, file_path, *exported, target)
                          : native_status_t::Aborted();
        if (target != exported->DefaultColumnFamily()) exported->DestroyColumnFamilyHandle(target);
        if (!status.ok()) break;
    }

    exported.reset();
    if (!export_error(status, arguments.error)) return;
    // A failed export removes what it wrote, and says so when it cannot, since the path then refuses every retry
    if (!remove_partial_export(path, options, families))
        arguments.error->message = "Failed to export the snapshot, and to remove the files it left behind";
}

#pragma endregion Snapshots

#pragma region Reads and Writes

void write(ustore_write_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    database_t& database = *handle_cast<database_t>(arguments.db);
    transaction_t* const transaction = handle_cast<transaction_t>(arguments.transaction);
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
    std::shared_lock lock {database.columns_mutex};
    if (refuse_collections(database, collections, arguments.tasks_count, keying_t::uuids_k, arguments.error)) return;
    if (transaction && transaction->staging != staging_t::pending_k) {
        *arguments.error = {ustore_error_invalid_argument_k, "A staged transaction takes no more writes"};
        return;
    }

    if (arguments.tasks_count == 1)
        write_one(database, transaction, places, contents, arguments.options, arguments.error);
    else write_many(database, transaction, places, contents, arguments.options, arguments.error);
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
    std::shared_lock lock {database.columns_mutex};
    if (refuse_collections(database, collections, arguments.tasks_count, keying_t::uuids_k, arguments.error)) return;

    ::rocksdb::ReadOptions read_options;
    use_snapshot(arguments.snapshot, read_options);
    transaction_t* const transaction = reading_transaction(arguments.transaction, read_options);
    bool const watches = !(arguments.options & ustore_option_transaction_dont_watch_k);
    if (transaction && watches && transaction->staging != staging_t::pending_k) {
        *arguments.error = {ustore_error_invalid_argument_k, "A staged transaction watches no more reads"};
        return;
    }

    std::size_t const count = places.count;
    if (count == 1) {
        ::rocksdb::PinnableSlice value;
        native_status_t const status = get_one(database, transaction, read_options, places[0], arguments.options,
                                               value);
        return export_values(arguments, arena, &value, &status, count);
    }

    // Values stay pinned until the tape is sized, so each is copied once, straight from its block
    std::unique_ptr<::rocksdb::PinnableSlice[]> values {new (std::nothrow)::rocksdb::PinnableSlice[count]};
    std::unique_ptr<native_status_t[]> statuses {new (std::nothrow) native_status_t[count]};
    if (!values || !statuses) {
        *arguments.error = {ustore_error_out_of_memory_k, "Failed to allocate the batch"};
        return;
    }

    get_many(database, transaction, read_options, places, arguments.options, values.get(), statuses.get(),
             arguments.error);
    if (arguments.error->status) return;
    export_values(arguments, arena, values.get(), statuses.get(), count);
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
    scans_arg_t tasks {collections, start_keys, limits, arguments.tasks_count};

    validate_scan(arguments.transaction, tasks, arguments.options, arguments.error);
    if (arguments.error->status) return;
    std::shared_lock lock {database.columns_mutex};
    if (refuse_collections(database, collections, arguments.tasks_count, keying_t::uuids_k, arguments.error)) return;

    auto offsets = arena.alloc_or_dummy(tasks.count + 1, arguments.error, arguments.offsets);
    if (arguments.error->status) return;
    auto counts = arena.alloc_or_dummy(tasks.count, arguments.error, arguments.counts);
    if (arguments.error->status) return;

    auto const total_keys = reduce_n(tasks.limits, tasks.count, 0ul);
    auto keys_output = *arguments.keys = arena.alloc<ustore_key_t>(total_keys, arguments.error).data();
    if (arguments.error->status) return;

    ::rocksdb::ReadOptions read_options;
    read_options.fill_cache = false;
    use_snapshot(arguments.snapshot, read_options);
    transaction_t* const transaction = reading_transaction(arguments.transaction, read_options);

    // Consecutive tasks over one collection share an iterator, since `Seek` repositions it anywhere
    column_t* iterated_column = nullptr;
    std::unique_ptr<::rocksdb::Iterator> iterator;

    for (ustore_size_t task_index = 0; task_index != arguments.tasks_count; ++task_index) {
        scan_t task = tasks[task_index];
        column_t* const column = column_of(database, task.collection);
        if (!iterator || column != iterated_column) {
            iterator.reset(new_iterator(database, transaction, read_options, column));
            if (!iterator) {
                *arguments.error = {ustore_error_out_of_memory_k, "Failed to create a RocksDB iterator"};
                return;
            }
            iterated_column = column;
        }

        offsets[task_index] = keys_output - *arguments.keys;

        ustore_size_t matched_count = 0;
        iterator->Seek(to_slice(task.min_key));
        while (iterator->Valid() && matched_count != task.limit) {
            std::memcpy(keys_output, iterator->key().data(), sizeof(ustore_key_t));
            ++keys_output;
            ++matched_count;
            iterator->Next();
        }
        if (export_error(iterator->status(), arguments.error)) return;

        counts[task_index] = matched_count;
    }

    offsets[tasks.size()] = keys_output - *arguments.keys;
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
    std::shared_lock lock {database.columns_mutex};
    if (refuse_collections(database, collections, arguments.tasks_count, keying_t::uuids_k, arguments.error)) return;

    auto offsets = arena.alloc_or_dummy(samples.count + 1, arguments.error, arguments.offsets);
    if (arguments.error->status) return;
    auto counts = arena.alloc_or_dummy(samples.count, arguments.error, arguments.counts);
    if (arguments.error->status) return;

    auto const total_keys = reduce_n(samples.limits, samples.count, 0ul);
    auto keys_output = *arguments.keys = arena.alloc<ustore_key_t>(total_keys, arguments.error).data();
    if (arguments.error->status) return;

    ::rocksdb::ReadOptions read_options;
    read_options.fill_cache = false;
    use_snapshot(arguments.snapshot, read_options);
    transaction_t* const transaction = reading_transaction(arguments.transaction, read_options);

    column_t* iterated_column = nullptr;
    std::unique_ptr<::rocksdb::Iterator> iterator;

    for (std::size_t task_index = 0; task_index != samples.count; ++task_index) {
        sample_arg_t task = samples[task_index];
        column_t* const column = column_of(database, task.collection);
        offsets[task_index] = keys_output - *arguments.keys;

        if (!iterator || column != iterated_column) {
            iterator.reset(new_iterator(database, transaction, read_options, column));
            if (!iterator) {
                *arguments.error = {ustore_error_out_of_memory_k, "Failed to create a RocksDB iterator"};
                return;
            }
            iterated_column = column;
        }

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
    strided_iterator<ustore_collection_id_t const> collections {arguments.collections, arguments.collections_stride};
    std::shared_lock lock {database.columns_mutex};
    if (refuse_collections(database, collections, arguments.tasks_count, keying_t::uuids_k, arguments.error)) return;
    strided_iterator<ustore_key_t const> start_keys {arguments.start_keys, arguments.start_keys_stride};
    strided_iterator<ustore_key_t const> end_keys {arguments.end_keys, arguments.end_keys_stride};
    ::rocksdb::SizeApproximationOptions size_options;

    // Properties describe a whole collection, so consecutive tasks over one read them once
    column_t* measured_column = nullptr;
    std::uint64_t keys_count = 0;
    std::uint64_t sst_files_size = 0;

    for (ustore_size_t task_index = 0; task_index != arguments.tasks_count; ++task_index) {
        column_t* const column = column_of(database, collections ? collections[task_index] : ustore_collection_main_k);
        ustore_key_t const min_key = start_keys[task_index];
        ustore_key_t const max_key = end_keys[task_index];
        ::rocksdb::Range const range(to_slice(min_key), to_slice(max_key));

        std::uint64_t approximate_size = 0;
        native_status_t const status = database.native->GetApproximateSizes(size_options, column, &range, 1,
                                                                            &approximate_size);
        if (export_error(status, arguments.error)) return;

        if (column != measured_column) {
            keys_count = 0;
            sst_files_size = 0;
            database.native->GetIntProperty(column, "rocksdb.estimate-num-keys", &keys_count);
            database.native->GetIntProperty(column, "rocksdb.total-sst-files-size", &sst_files_size);
            measured_column = column;
        }

        min_cardinalities[task_index] = static_cast<ustore_size_t>(0);
        max_cardinalities[task_index] = static_cast<ustore_size_t>(keys_count);
        min_value_bytes[task_index] = static_cast<ustore_size_t>(0);
        max_value_bytes[task_index] = std::numeric_limits<ustore_size_t>::max();
        min_space_usages[task_index] = approximate_size;
        max_space_usages[task_index] = sst_files_size;
    }
}

#pragma endregion Reads and Writes

#pragma region Collections

/** Creates an empty family holding @p modality under @p name and records it, answering its identifier through @p id
 *  and whether it did. The caller holds @c columns_mutex exclusively. */
bool create_family(database_t& database, ustore_modality_t modality, std::string_view name, ustore_collection_id_t& id,
                   ustore_error_t* error) {
    if (static_cast<std::size_t>(modality) >= modality_names_k.size()) {
        *error = {ustore_error_invalid_argument_k, "Unknown modality"};
        return false;
    }
    if (!mint_collection_id(id)) {
        *error = {ustore_error_internal_k, "The system gave no entropy for a collection identifier"};
        return false;
    }

    // A family is named after what it holds and the collection it stores, so an open recovers both
    std::string family_name {modality_names_k[modality]};
    family_name.push_back('.');
    family_name.resize(family_name.size() + uuid_text_length_k);
    uuid_to_text(id.uuid, std::span<char>(family_name).last<uuid_text_length_k>());
    column_t* column = nullptr;
    if (export_error(database.native->CreateColumnFamily(::rocksdb::ColumnFamilyOptions(), family_name, &column),
                     error))
        return false;

    // A family that cannot be recorded could never be addressed, so it is dropped again
    if (st::failed(database.columns.upsert(
            st::mapping<ustore_collection_id_t, family_t> {id, family_t {column, modality, std::string(name)}}))) {
        [[maybe_unused]] native_status_t const dropped = database.native->DropColumnFamily(column);
        database.native->DestroyColumnFamilyHandle(column);
        *error = {ustore_error_out_of_memory_k, "Failed to record the collection"};
        return false;
    }
    return true;
}

/** Drops the family @p id addresses, forgetting first and durably the name it carries, answering whether it did. The
 *  caller holds @c columns_mutex exclusively. */
bool drop_family(database_t& database, ustore_collection_id_t id, ustore_error_t* error) {
    auto const found = database.columns.find(id);
    if (found == database.columns.end()) {
        *error = {ustore_error_not_found_k, "No such collection"};
        return false;
    }
    family_t const& family = (*found).mapped;
    column_t* const column = family.column;

    // A crash between the two leaves an unnamed family behind, never a name addressing a family that is gone
    ::rocksdb::WriteOptions synced;
    synced.sync = true;
    if (!family.name.empty() && export_error(database.native->Delete(synced, database.names, family.name), error))
        return false;

    if (export_error(database.native->DropColumnFamily(column), error)) return false;
    [[maybe_unused]] bool const erased = database.columns.erase(id);
    database.native->DestroyColumnFamilyHandle(column);
    return true;
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
    std::string_view const name = string_of(arguments.name, arguments.name_length);
    if (arguments.name && name.empty()) {
        *arguments.error = {ustore_error_invalid_argument_k, "A name is never empty, as the main collection has none"};
        return;
    }

    // Creators take the families exclusively, so no other one can take the name between the lookup and the record
    database_t& database = *handle_cast<database_t>(arguments.db);
    std::lock_guard lock {database.columns_mutex};
    if (!name.empty()) {
        std::string record;
        native_status_t const looked_up = database.native->Get(::rocksdb::ReadOptions(), database.names, name, &record);
        if (looked_up.ok()) {
            *arguments.error = {ustore_error_already_exists_k, "A collection already carries the name"};
            return;
        }
        if (!looked_up.IsNotFound() && export_error(looked_up, arguments.error)) return;
    }
    ustore_collection_id_t id {};
    if (!create_family(database, arguments.modality, name, id, arguments.error)) return;

    // The family's name carries the identifier, so a name resolved after a restart still addresses it
    if (!name.empty()) {
        ::rocksdb::WriteOptions synced;
        synced.sync = true;
        name_record_t const named {id, arguments.modality};
        if (export_error(database.native->Put(synced, database.names, name, named.view()), arguments.error)) {
            // A family no name addresses would be unreachable, so it goes, while the error that stopped the name stays
            ustore_error_t dropped {};
            drop_family(database, id, &dropped);
            return;
        }
    }
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
        std::lock_guard lock {database.columns_mutex};
        drop_family(database, arguments.collection, arguments.error);
        return;
    }

    std::shared_lock lock {database.columns_mutex};
    column_t* const column = column_of(database, arguments.collection);
    if (!column) *arguments.error = {ustore_error_not_found_k, "No such collection"};
    else export_error(clear_column(database, column, arguments.mode), arguments.error);
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
    std::shared_lock lock {database.columns_mutex};
    std::size_t const collections_count = database.columns.size();
    auto ids = arena.alloc_or_dummy(collections_count, arguments.error, arguments.collections);
    auto modalities = arena.alloc_or_dummy(collections_count, arguments.error, arguments.modalities);
    growing_tape_t names {arena};
    if (arguments.error->status) return;
    std::size_t index = 0;
    for (auto const& [id, family] : database.columns) {
        ids[index] = id;
        modalities[index] = family.modality;
        ++index;
        names.push_back(family.name.empty() ? value_view_t {} : value_view_t {family.name}, arguments.error);
        if (arguments.error->status) return;
    }
    *arguments.count = static_cast<ustore_size_t>(collections_count);
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
    ::rocksdb::ReadOptions read_options;
    use_snapshot(arguments.snapshot, read_options);

    std::size_t const count = arguments.tasks_count;
    strings_arg_t const names {{arguments.names, arguments.names_stride},
                               {arguments.names_lengths, arguments.names_lengths_stride}};
    std::span<ustore_collection_id_t> const ids = arena.alloc<ustore_collection_id_t>(count, arguments.error);
    auto modalities = arena.alloc_or_dummy(count, arguments.error, arguments.modalities);
    auto presences = arena.alloc_or_dummy(count, arguments.error, arguments.presences);
    if (arguments.error->status) return;

    std::string record;
    for (std::size_t index = 0; index != count; ++index) {
        std::string_view const name = names[index];
        native_status_t const status = database.native->Get(read_options, database.names, ::rocksdb::Slice(name),
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

/** Serves "clear", removing every pair while keeping the collections and their names. */
void clear_pairs(database_t& database, ustore_database_control_t& arguments) {
    std::shared_lock lock {database.columns_mutex};
    if (export_error(clear_column(database, database.main, ustore_drop_keys_vals_k), arguments.error)) return;
    for (auto const& [id, family] : database.columns)
        if (export_error(clear_column(database, family.column, ustore_drop_keys_vals_k), arguments.error)) return;
}

/** Serves "reset", dropping every collection with its name and clearing the main one, unless a snapshot is open, as
 *  the caller owns it and would read a database it never saw. */
void reset_database(database_t& database, ustore_database_control_t& arguments) {
    if (database.open_snapshots.load(std::memory_order_relaxed) != 0) {
        *arguments.error = {ustore_error_invalid_argument_k, "Free every snapshot before resetting the database"};
        return;
    }

    // The map cannot lose a family while it is walked, so the ones to drop are gathered first
    std::lock_guard lock {database.columns_mutex};
    st::basic_vector<ustore_collection_id_t> dropped;
    if (st::failed(dropped.reserve(database.columns.size()))) {
        *arguments.error = {ustore_error_out_of_memory_k, "Failed to gather the collections"};
        return;
    }
    for (auto const& [id, family] : database.columns) [[maybe_unused]]
        auto const gathered = dropped.push_back(st::assume_reserved, ustore_collection_id_t {id});
    for (ustore_collection_id_t const id : dropped)
        if (!drop_family(database, id, arguments.error)) return;
    if (export_error(clear_column(database, database.main, ustore_drop_keys_vals_k), arguments.error)) return;
    export_error(clear_column(database, database.names, ustore_drop_keys_vals_k), arguments.error);
}

/** Serves "compact", flushing every memtable and compacting every family, the main and the names ones included. */
void compact_database(database_t& database, ustore_database_control_t& arguments) {
    ::rocksdb::FlushOptions const flush_options;
    ::rocksdb::CompactRangeOptions const compact_options;
    std::shared_lock lock {database.columns_mutex};
    std::vector<column_t*> families {database.main, database.names};
    for (auto const& [id, family] : database.columns) families.push_back(family.column);
    for (column_t* const column : families) {
        if (export_error(database.native->Flush(flush_options, column), arguments.error)) return;
        native_status_t const status = database.native->CompactRange(compact_options, column, nullptr, nullptr);
        if (export_error(status, arguments.error)) return;
    }
}

/** Serves "info", naming the engine and the RocksDB release it links. */
void describe_database(database_t&, ustore_database_control_t& arguments) {
    linked_memory_lock_t arena = linked_memory(arguments.arena, ustore_options_default_k, arguments.error);
    if (arguments.error->status) return;

    constexpr std::size_t response_capacity_k = 64;
    char* const response = arena.alloc<char>(response_capacity_k, arguments.error).data();
    if (arguments.error->status) return;
    auto const written = fmt::format_to_n(response, response_capacity_k - 1, R"({{"engine":"rocksdb","version":"{}"}})",
                                          ::rocksdb::GetRocksVersionAsString());
    *written.out = '\0';
    *arguments.response = response;
}

/** Serves "usage", summing the estimated keys, memtable bytes and table file bytes of every collection. */
void report_usage(database_t& database, ustore_database_control_t& arguments) {
    linked_memory_lock_t arena = linked_memory(arguments.arena, ustore_options_default_k, arguments.error);
    if (arguments.error->status) return;

    std::uint64_t keys_count = 0;
    std::uint64_t memory_bytes = 0;
    std::uint64_t disk_bytes = 0;
    std::shared_lock lock {database.columns_mutex};
    std::vector<column_t*> families {database.main, database.names};
    for (auto const& [id, family] : database.columns) families.push_back(family.column);
    for (column_t* const column : families) {
        std::uint64_t property = 0;
        if (database.native->GetIntProperty(column, ::rocksdb::DB::Properties::kEstimateNumKeys, &property))
            keys_count += property;
        if (database.native->GetIntProperty(column, ::rocksdb::DB::Properties::kCurSizeAllMemTables, &property))
            memory_bytes += property;
        if (database.native->GetIntProperty(column, ::rocksdb::DB::Properties::kTotalSstFilesSize, &property))
            disk_bytes += property;
    }

    constexpr std::size_t response_capacity_k = 128;
    char* const response = arena.alloc<char>(response_capacity_k, arguments.error).data();
    if (arguments.error->status) return;
    auto const written = fmt::format_to_n(response, response_capacity_k - 1,
                                          R"({{"keys":{},"memory_bytes":{},"disk_bytes":{}}})", keys_count,
                                          memory_bytes, disk_bytes);
    *written.out = '\0';
    *arguments.response = response;
}

/** One free-form request of @c ustore_database_control. */
struct control_request_t {

    /** The name a caller sends. */
    std::string_view name;

    /** Serves the request. */
    void (*serve)(database_t&, ustore_database_control_t&);
};

/** Every request @c ustore_database_control serves. */
inline constexpr std::array<control_request_t, 5> control_requests_k {{
    {"clear", &clear_pairs},
    {"reset", &reset_database},
    {"compact", &compact_database},
    {"info", &describe_database},
    {"usage", &report_usage},
}};

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
    database_t& database = *handle_cast<database_t>(arguments.db);
    std::string_view const request {arguments.request};
    for (control_request_t const& known : control_requests_k)
        if (known.name == request) return known.serve(database, arguments);
    *arguments.error = {ustore_error_invalid_argument_k,
                        "Unknown request: expected clear, reset, compact, info or usage"};
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
    transaction_t* const transaction = handle_cast<transaction_t>(arguments.transaction);
    if (transaction && transaction->staging != staging_t::pending_k) {
        *arguments.error = {ustore_error_invalid_argument_k, "A staged transaction takes no more writes"};
        return;
    }
    strided_iterator<ustore_collection_id_t const> const collections {arguments.collections,
                                                                      arguments.collections_stride};
    strings_arg_t const texts {{arguments.strings, arguments.strings_stride},
                               {arguments.strings_lengths, arguments.strings_lengths_stride}};
    contents_arg_t const contents {bits_view_t {arguments.values_presences},
                                   {arguments.values_offsets, arguments.values_offsets_stride},
                                   {arguments.values_lengths, arguments.values_lengths_stride},
                                   {arguments.values, arguments.values_stride},
                                   arguments.tasks_count};

    std::shared_lock lock {database.columns_mutex};
    if (refuse_collections(database, collections, arguments.tasks_count, keying_t::strings_k, arguments.error)) return;

    ::rocksdb::WriteBatch batch;
    for (std::size_t index = 0; index != arguments.tasks_count; ++index) {
        ustore_collection_id_t const collection = collections[index];
        column_t* const column = column_of(database, collection);
        std::string_view const text = texts[index];
        ::rocksdb::Slice const key {text};
        value_view_t const content = contents[index];
        native_status_t const status = transaction ? buffer_write(*transaction, column, collection, key, content)
                                       : !content  ? batch.Delete(column, key)
                                                   : batch.Put(column, key, to_slice(content));
        if (export_error(status, arguments.error)) return;
    }
    if (transaction) return;

    // Syncing writes the log's buffer out first, so a flushed write keeps every write before it too
    ::rocksdb::WriteOptions write_options;
    write_options.sync = arguments.options & ustore_option_write_flush_k;
    export_error(database.native->Write(write_options, &batch), arguments.error);
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
    ::rocksdb::ReadOptions read_options;
    use_snapshot(arguments.snapshot, read_options);
    transaction_t* const transaction = reading_transaction(arguments.transaction, read_options);
    bool const watches = transaction && !(arguments.options & ustore_option_transaction_dont_watch_k);

    strided_iterator<ustore_collection_id_t const> const collections {arguments.collections,
                                                                      arguments.collections_stride};
    strings_arg_t const texts {{arguments.strings, arguments.strings_stride},
                               {arguments.strings_lengths, arguments.strings_lengths_stride}};
    std::shared_lock lock {database.columns_mutex};
    if (refuse_collections(database, collections, arguments.tasks_count, keying_t::strings_k, arguments.error)) return;

    growing_tape_t exported {arena};
    exported.reserve(arguments.tasks_count, arguments.error);
    if (arguments.error->status) return;
    ::rocksdb::PinnableSlice value;
    for (std::size_t index = 0; index != arguments.tasks_count; ++index) {
        ustore_collection_id_t const collection = collections[index];
        column_t* const column = column_of(database, collection);
        std::string_view const text = texts[index];
        ::rocksdb::Slice const key {text};
        if (watches) transaction->watched_strings.emplace_back(collection, std::string(text));

        value.Reset();
        native_status_t const status = transaction ? transaction->batch.GetFromBatchAndDB(
                                                         database.native.get(), read_options, column, key, &value)
                                                   : database.native->Get(read_options, column, key, &value);
        if (!status.IsNotFound() && export_error(status, arguments.error)) return;
        value_view_t const found = status.ok() ? value_view_t {reinterpret_cast<byte_t const*>(value.data()),
                                                               static_cast<ustore_length_t>(value.size())}
                                               : value_view_t {};
        exported.push_back(found, arguments.error);
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
    ::rocksdb::ReadOptions read_options;
    read_options.fill_cache = false;
    use_snapshot(arguments.snapshot, read_options);
    transaction_t* const transaction = reading_transaction(arguments.transaction, read_options);

    std::size_t const count = arguments.tasks_count;
    strided_iterator<ustore_collection_id_t const> const collections {arguments.collections,
                                                                      arguments.collections_stride};
    strings_arg_t const prefixes {{arguments.prefixes, arguments.prefixes_stride},
                                  {arguments.prefixes_lengths, arguments.prefixes_lengths_stride}};
    strings_arg_t const starts {{arguments.start_strings, arguments.start_strings_stride},
                                {arguments.start_strings_lengths, arguments.start_strings_lengths_stride}};
    strided_iterator<ustore_length_t const> const limits {arguments.count_limits, arguments.count_limits_stride};
    std::shared_lock lock {database.columns_mutex};
    if (refuse_collections(database, collections, count, keying_t::strings_k, arguments.error)) return;

    auto offsets = arena.alloc_or_dummy(count + 1, arguments.error, arguments.offsets);
    auto counts = arena.alloc_or_dummy(count, arguments.error, arguments.counts);
    if (arguments.error->status) return;
    growing_tape_t found {arena};
    for (std::size_t task = 0; task != count; ++task) {
        std::string_view const prefix = arguments.prefixes ? prefixes[task] : std::string_view {};
        std::string_view const start = arguments.start_strings ? starts[task] : std::string_view {};
        std::string_view const first = start > prefix ? start : prefix;
        std::size_t const found_before = found.lengths().size();
        offsets[task] = static_cast<ustore_length_t>(found_before);

        std::unique_ptr<::rocksdb::Iterator> iterator {
            new_iterator(database, transaction, read_options, column_of(database, collections[task]))};
        for (iterator->Seek(::rocksdb::Slice(first.data(), first.size()));
             iterator->Valid() && found.lengths().size() - found_before != limits[task]; iterator->Next()) {
            std::string_view const text = iterator->key().ToStringView();
            if (!text.starts_with(prefix)) break;
            found.push_back(value_view_t {text}, arguments.error);
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

/** Begins @p transaction afresh, reusing its RocksDB transaction when it has one, and pinning no snapshot until the
 *  transaction's first read or write, so an idle handle holds back no compaction. */
bool begin(database_t& database, transaction_t& transaction, ustore_error_t* error) {
    ::rocksdb::TransactionOptions options;
    // Locks are taken only at stage, where a millisecond covers the lock stripe and a held key is a conflict
    options.lock_timeout = 1;
    ::rocksdb::Transaction* const begun = database.native->BeginTransaction(::rocksdb::WriteOptions(), options,
                                                                            transaction.native.release());
    transaction.native.reset(begun);
    transaction.batch.Clear();
    transaction.watched.clear();
    transaction.watched_strings.clear();
    transaction.written.clear();
    transaction.staging = staging_t::pending_k;
    if (!begun) *error = {ustore_error_internal_k, "Failed to begin a transaction"};
    return begun != nullptr;
}

/**
 *  @brief Stages @p transaction unless it already is: locks and validates every watched read and buffered write
 *      against the snapshot its first read or write took, then prepares the writes into the log.
 *
 *  A refused key rolls back to the save point taken before the first lock, leaving the transaction pending and
 *  retryable as on the reference engine, unless RocksDB's prepare itself failed and left it needing a reset.
 */
bool stage(database_t& database, transaction_t& transaction, ustore_error_t* error) {
    if (transaction.staging == staging_t::staged_k) return true;
    if (transaction.staging == staging_t::broken_k) {
        *error = {ustore_error_invalid_argument_k, "A failed prepare left the transaction needing a reset"};
        return false;
    }

    // A prepare needs a name unique among open transactions only, as the database rolls back every older one it opens
    ::rocksdb::Transaction& native = *transaction.native;
    if (native.GetName().empty() && export_error(native.SetName(std::to_string(native.GetID())), error)) return false;

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
    watched_strings.erase(std::unique(watched_strings.begin(), watched_strings.end()), watched_strings.end());
    for (std::size_t index = 0; status.ok() && index != watched_strings.size(); ++index) {
        column = column_of(database, watched_strings[index].first);
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
        return true;
    }

    // Undoing the stage keeps the transaction retryable, and a transaction that cannot be undone needs a reset
    bool const undone = native.GetState() == ::rocksdb::Transaction::STARTED && native.RollbackToSavePoint().ok();
    if (!undone) transaction.staging = staging_t::broken_k;

    if (status.IsBusy() || status.IsTimedOut())
        *error = reads_validated ? ustore_error_t {ustore_error_write_conflict_k,
                                                   "A concurrent write changed a key the transaction wrote"}
                                 : ustore_error_t {ustore_error_read_conflict_k,
                                                   "A concurrent write changed a key the transaction read"};
    else export_error(status, error);
    return false;
}

void transaction_init(ustore_transaction_init_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    validate_transaction_begin(arguments.transaction, arguments.options, arguments.error);
    if (arguments.error->status) return;

    database_t& database = *handle_cast<database_t>(arguments.db);
    std::unique_ptr<transaction_t> created;
    transaction_t* transaction = handle_cast<transaction_t>(*arguments.transaction);
    if (!transaction) {
        created.reset(new (std::nothrow) transaction_t);
        if (!created) {
            *arguments.error = {ustore_error_out_of_memory_k, "Failed to allocate the transaction"};
            return;
        }
        created->engine = &engine_k;
        transaction = created.get();
    }
    // A prepare left behind would pin the log and come back after a crash
    else if (transaction->staging == staging_t::staged_k) {
        if (export_error(transaction->native->Rollback(), arguments.error)) return;
    }

    if (!begin(database, *transaction, arguments.error)) return;
    if (created) *arguments.transaction = c_handle<ustore_transaction_t>(created.release());
}

void transaction_stage(ustore_transaction_stage_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!arguments.transaction) {
        *arguments.error = {ustore_error_uninitialized_k, "Transaction is uninitialized"};
        return;
    }
    database_t& database = *handle_cast<database_t>(arguments.db);
    transaction_t& transaction = *handle_cast<transaction_t>(arguments.transaction);
    std::shared_lock lock {database.columns_mutex};
    if (!stage(database, transaction, arguments.error)) return;
    // A prepare takes no sequence number, so the latest one orders this stage before the commit that follows it
    if (arguments.stamp) *arguments.stamp = database.native->GetLatestSequenceNumber();
}

void transaction_commit(ustore_transaction_commit_t& arguments) {
    if (!arguments.transaction) return;

    validate_transaction_commit(arguments.transaction, arguments.options, arguments.error);
    if (arguments.error->status) return;

    database_t& database = *handle_cast<database_t>(arguments.db);
    transaction_t& transaction = *handle_cast<transaction_t>(arguments.transaction);
    std::shared_lock lock {database.columns_mutex};
    if (!stage(database, transaction, arguments.error)) return;

    // A flushed commit syncs the log within its own write, so no reader sees it before it is durable, and any other
    // commit waits in the log's buffer until a flushed one, a full buffer or a clean close writes it out
    ::rocksdb::Transaction& native = *transaction.native;
    if (arguments.options & ustore_option_write_flush_k) {
        ::rocksdb::WriteOptions write_options;
        write_options.sync = true;
        native.SetWriteOptions(write_options);
    }
    if (export_error(native.Commit(), arguments.error)) return;
    if (arguments.stamp) *arguments.stamp = native.GetId();

    // The handle carries on as a new transaction, as a reset would leave it
    begin(database, transaction, arguments.error);
}

#pragma endregion Transactions

#pragma region Memory

void transaction_free(ustore_transaction_t handle) noexcept {
    transaction_t* const transaction = handle_cast<transaction_t>(handle);
    // A prepare left behind would pin the log and come back after a crash
    if (transaction->staging == staging_t::staged_k) {
        [[maybe_unused]] native_status_t const rolled_back = transaction->native->Rollback();
    }
    delete transaction;
}

void database_free(ustore_database_t database) noexcept { delete handle_cast<database_t>(database); }

void snapshot_free(ustore_snapshot_t handle) noexcept {
    snapshot_t* const snapshot = handle_cast<snapshot_t>(handle);
    snapshot->database->native->ReleaseSnapshot(snapshot->native);
    snapshot->database->open_snapshots.fetch_sub(1, std::memory_order_relaxed);
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

} // namespace unum::ustore::rocksdb

using namespace unum::ustore;
using namespace unum::ustore::rocksdb;

#if defined(__linux__)

/** Lets RocksDB's POSIX file system open io_uring rings, which it leaves off unless this weak symbol is defined. A
 *  thread whose ring the kernel or a seccomp profile refuses reads through `pread` instead. */
extern "C" bool RocksDbIOUringEnable() { return true; }
#endif

void ustore_database_init_rocksdb(ustore_database_init_rocksdb_t* arguments) {
    guarded<ustore_database_init_rocksdb_t, &database_init>(*arguments);
}
