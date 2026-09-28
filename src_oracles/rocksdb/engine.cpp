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
#include <charconv>     // `std::to_chars`
#include <compare>      // `std::strong_ordering`
#include <filesystem>   // `std::filesystem::remove`
#include <limits>       // `std::numeric_limits`
#include <memory>       // `std::unique_ptr`
#include <mutex>        // `std::lock_guard`, `std::mutex`, `std::unique_lock`
#include <shared_mutex> // `std::shared_lock`, `std::shared_mutex`
#include <new>          // `std::nothrow`
#include <span>         // `std::span`
#include <string>       // `std::string`, `std::to_string`, the path and name type RocksDB takes
#include <string_view>  // `std::string_view`
#include <system_error> // `std::error_code`
#include <vector>       // `std::vector`, the descriptor, handle and file lists RocksDB takes

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
#include "helpers/linked_memory.hpp"  // `linked_memory_lock_t`

namespace unum::ustore::rocksdb {

#pragma region Database

/** The open RocksDB database. */
using native_t = ::rocksdb::TransactionDB;

/** Status RocksDB answers. */
using native_status_t = ::rocksdb::Status;

/** A column family, which is how named collections are stored. */
using column_t = ::rocksdb::ColumnFamilyHandle;

/** Reads the key @p slice holds, whose bytes RocksDB leaves unaligned. */
inline ustore_key_t key_of(::rocksdb::Slice const& slice) noexcept {
    ustore_key_t key;
    std::memcpy(&key, slice.data(), sizeof(ustore_key_t));
    return key;
}

/** Orders keys as the integers they hold. */
struct key_comparator_t final : public ::rocksdb::Comparator {

    int Compare(::rocksdb::Slice const& left, ::rocksdb::Slice const& right) const override {
        ustore_key_t const left_key = key_of(left);
        ustore_key_t const right_key = key_of(right);
        if (left_key == right_key) return 0;
        return left_key < right_key ? -1 : 1;
    }

    char const* Name() const override { return "i64"; }

    void FindShortestSeparator(std::string*, ::rocksdb::Slice const&) const override {}

    void FindShortSuccessor(std::string*) const override {}

    bool CanKeysWithDifferentByteContentsBeEqual() const override { return false; }

    bool IsSameLengthImmediateSuccessor(::rocksdb::Slice const& first, ::rocksdb::Slice const& second) const override {
        return key_of(first) + 1 == key_of(second);
    }
};

key_comparator_t const key_comparator_k {};

/** A snapshot handle, whose address is its identifier. */
struct snapshot_t {

    /** The RocksDB snapshot. */
    ::rocksdb::Snapshot const* native = nullptr;
};

/** Identifiers of the live snapshots. */
using snapshots_t = st::hash_set<ustore_snapshot_t>;

/** Column family handles by identifier, which the database destroys before it closes. */
using columns_t = st::hash_map<ustore_collection_t, column_t*>;

/** The name of the column family behind @c ustore_collection_names_k. */
constexpr char const* names_column_name_k = "ustore.names";

/** Milliseconds a write outside any transaction waits for a key a staged transaction holds before it conflicts, where
 *  RocksDB would wait a whole second. */
constexpr std::int64_t head_write_lock_timeout_ms_k = 16;

/** The calls this engine serves, which stamp every handle it hands out. */
extern engine_t const engine_k;

/** The open database, its column families and its snapshots. */
struct database_t : handle_t {

    /** Every column family by identifier, the default one included. */
    columns_t columns;

    /** Shared by every lookup, exclusive for a create, a drop or a reset. */
    std::shared_mutex columns_mutex;

    /** The column family behind @c ustore_collection_names_k, opened with the database. */
    column_t* names = nullptr;

    /** Identifiers of the live snapshots. */
    snapshots_t snapshots;

    /** The open database. */
    std::unique_ptr<native_t> native;

    /** Guards the snapshots. */
    std::mutex mutex;

    /** Releases the snapshots callers never dropped, as `Close` refuses while one is held, then writes the log's
     *  buffer out and closes, whether the database opened fully or failed halfway. */
    ~database_t() noexcept {
        if (!native) return;
        [[maybe_unused]] auto const released = snapshots.for_each([&](auto const& slot) noexcept {
            snapshot_t* const snapshot = reinterpret_cast<snapshot_t*>(slot.key());
            native->ReleaseSnapshot(snapshot->native);
            delete snapshot;
        });
        [[maybe_unused]] native_status_t const flushed = native->FlushWAL(true);
        for (auto const& [id, column] : columns) native->DestroyColumnFamilyHandle(column);
        [[maybe_unused]] native_status_t const closed = native->Close();
    }
};

/** A key a transaction read while watching it, locked and validated when the transaction stages. */
struct watched_key_t {
    ustore_collection_t collection = ustore_collection_main_k;
    ustore_key_t key = 0;

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
    ::rocksdb::WriteBatchWithIndex batch {&key_comparator_k, 0, true};

    /** Keys read while watching them. */
    st::basic_vector<watched_key_t> watched;

    /** Collections holding buffered writes, each replayed from the batch's index at stage. */
    st::basic_vector<ustore_collection_t> written;

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
native_status_t buffer_write(transaction_t& transaction, column_t* column, place_t const& place, value_view_t content) {
    [[maybe_unused]] ::rocksdb::Snapshot const* const snapshot = snapshot_of(transaction);
    native_status_t const status = !content ? transaction.batch.Delete(column, to_slice(place.key))
                                            : transaction.batch.Put(column, to_slice(place.key), to_slice(content));
    if (!status.ok()) return status;
    for (ustore_collection_t const written : transaction.written)
        if (written == place.collection) return status;
    if (st::failed(transaction.written.emplace_back(place.collection))) return native_status_t::MemoryLimit();
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

/** The column family @p collection addresses, or null when no family carries that identifier. */
column_t* column_of(database_t& database, ustore_collection_t collection) noexcept {
    if (collection == ustore_collection_main_k) return database.native->DefaultColumnFamily();
    if (collection == ustore_collection_names_k) return database.names;
    std::shared_lock lock {database.columns_mutex};
    auto const found = database.columns.find(collection);
    return found != database.columns.end() ? (*found).mapped : nullptr;
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

/** Points @p options at the snapshot @p id, leaving them untouched for the zero identifier. */
void use_snapshot(database_t& database, ustore_snapshot_t id, ::rocksdb::ReadOptions& options, ustore_error_t* error) {
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

    if (transaction) status = buffer_write(*transaction, column, place, content);
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
            if (export_error(buffer_write(*transaction, column, place, content), error)) return;
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

/**
 *  @brief The transaction a read goes through, pointing @p read_options at the snapshot the transaction reads.
 *
 *  A read naming a snapshot as well reads that snapshot alone and watches nothing, as on the reference engine, so it
 *  answers null then.
 */
transaction_t* reading_transaction(ustore_transaction_t handle, ::rocksdb::ReadOptions& read_options) noexcept {
    if (!handle || read_options.snapshot) return nullptr;
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
    char digits[std::numeric_limits<std::size_t>::digits10 + 1];
    char const* const digits_end = std::to_chars(digits, digits + sizeof(digits), id).ptr;
    file_path.assign(path)
        .append("/column-")
        .append(digits, static_cast<std::size_t>(digits_end - digits))
        .append(".sst");
}

/** Removes what a failed export of @p columns left at @p path, answering whether nothing remains: first the SST
 *  files, which RocksDB does not recognize as its own, then the database around them. */
bool remove_partial_export(std::string const& path, ::rocksdb::Options const& options, columns_t const& columns) {
    bool removed = true;
    std::string file_path;
    for (auto const& [id, column] : columns) {
        column_file_path(file_path, path, id);
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
    for (auto& descriptor : column_descriptors) descriptor.options.comparator = &key_comparator_k;

    // An export persists `error_if_exists`, which would refuse every reopening of it
    options.create_if_missing = true;
    options.error_if_exists = false;
    options.comparator = &key_comparator_k;
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
        if (st::failed(
                database->columns.upsert(st::mapping<ustore_collection_t, column_t*> {handle->GetID(), handle}))) {
            recorded = false;
            native->DestroyColumnFamilyHandle(handle);
        }
        else if (handle->GetName() == names_column_name_k) database->names = handle;
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

    // The reserved names collection is a column family like any other, created the first time a database opens
    if (!database->names) {
        ::rocksdb::ColumnFamilyOptions names_options;
        names_options.comparator = &key_comparator_k;
        status = database->native->CreateColumnFamily(names_options, names_column_name_k, &database->names);
        if (!status.ok()) {
            *arguments.error = {ustore_error_io_k, "Failed to open the names collection"};
            return;
        }
        column_t* const names = database->names;
        if (st::failed(database->columns.upsert(st::mapping<ustore_collection_t, column_t*> {names->GetID(), names}))) {
            database->native->DestroyColumnFamilyHandle(database->names);
            *arguments.error = {ustore_error_out_of_memory_k, "Failed to record the names collection"};
            return;
        }
    }
    database->engine = &engine_k;
    *arguments.db = static_cast<handle_t*>(database.release());
}

void database_capabilities(ustore_database_capabilities_t& arguments) {
    *arguments.capabilities = ustore_capabilities_t(ustore_supports_transactions_k | ustore_supports_collections_k |
                                                    ustore_supports_snapshots_k);
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

    ::rocksdb::ReadOptions snapshot_options;
    snapshot_options.fill_cache = false;
    use_snapshot(database, arguments.id, snapshot_options, arguments.error);
    if (arguments.error->status) return;
    if (!snapshot_options.snapshot) {
        *arguments.error = {ustore_error_not_found_k, "The snapshot doesn't exist"};
        return;
    }

    ::rocksdb::Options options;
    options.compression = ::rocksdb::kNoCompression;
    options.comparator = &key_comparator_k;
    options.create_if_missing = true;
    options.error_if_exists = true;

    std::string const path(arguments.path);
    std::unique_ptr<::rocksdb::DB> exported;
    native_status_t status = ::rocksdb::DB::Open(options, path, &exported);
    if (export_error(status, arguments.error)) return;

    ::rocksdb::ColumnFamilyOptions column_options;
    column_options.comparator = &key_comparator_k;
    std::string file_path;

    std::shared_lock lock {database.columns_mutex};
    for (auto const& [id, column] : database.columns) {
        column_t* target = exported->DefaultColumnFamily();
        if (column->GetName() != ::rocksdb::kDefaultColumnFamilyName)
            status = exported->CreateColumnFamily(column_options, column->GetName(), &target);
        if (!status.ok()) break;

        column_file_path(file_path, path, id);
        std::unique_ptr<::rocksdb::Iterator> iterator {database.native->NewIterator(snapshot_options, column)};
        status = iterator ? export_column(*iterator, options, file_path, *exported, target)
                          : native_status_t::Aborted();
        if (target != exported->DefaultColumnFamily()) exported->DestroyColumnFamilyHandle(target);
        if (!status.ok()) break;
    }

    exported.reset();
    if (!export_error(status, arguments.error)) return;
    // A failed export removes what it wrote, and says so when it cannot, since the path then refuses every retry
    if (!remove_partial_export(path, options, database.columns))
        arguments.error->message = "Failed to export the snapshot, and to remove the files it left behind";
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
    transaction_t* const transaction = handle_cast<transaction_t>(arguments.transaction);
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
    if (!arguments.tasks_count) return;
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

    strided_iterator<ustore_collection_t const> collections {arguments.collections, arguments.collections_stride};
    strided_iterator<ustore_key_t const> keys {arguments.keys, arguments.keys_stride};
    places_arg_t places {collections, keys, {}, arguments.tasks_count};
    validate_read(arguments.transaction, places, arguments.options, arguments.error);
    if (arguments.error->status) return;

    ::rocksdb::ReadOptions read_options;
    use_snapshot(database, arguments.snapshot, read_options, arguments.error);
    if (arguments.error->status) return;
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
    strided_iterator<ustore_collection_t const> collections {arguments.collections, arguments.collections_stride};
    strided_iterator<ustore_key_t const> start_keys {arguments.start_keys, arguments.start_keys_stride};
    strided_iterator<ustore_length_t const> limits {arguments.count_limits, arguments.count_limits_stride};
    scans_arg_t tasks {collections, start_keys, limits, arguments.tasks_count};

    validate_scan(arguments.transaction, tasks, arguments.options, arguments.error);
    if (arguments.error->status) return;

    auto offsets = arena.alloc_or_dummy(tasks.count + 1, arguments.error, arguments.offsets);
    if (arguments.error->status) return;
    auto counts = arena.alloc_or_dummy(tasks.count, arguments.error, arguments.counts);
    if (arguments.error->status) return;

    auto const total_keys = reduce_n(tasks.limits, tasks.count, 0ul);
    auto keys_output = *arguments.keys = arena.alloc<ustore_key_t>(total_keys, arguments.error).data();
    if (arguments.error->status) return;

    ::rocksdb::ReadOptions read_options;
    read_options.fill_cache = false;
    use_snapshot(database, arguments.snapshot, read_options, arguments.error);
    if (arguments.error->status) return;
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
    strided_iterator<ustore_collection_t const> collections {arguments.collections, arguments.collections_stride};
    strided_iterator<ustore_length_t const> limits {arguments.count_limits, arguments.count_limits_stride};
    sample_args_t samples {collections, limits, arguments.tasks_count};

    auto offsets = arena.alloc_or_dummy(samples.count + 1, arguments.error, arguments.offsets);
    if (arguments.error->status) return;
    auto counts = arena.alloc_or_dummy(samples.count, arguments.error, arguments.counts);
    if (arguments.error->status) return;

    auto const total_keys = reduce_n(samples.limits, samples.count, 0ul);
    auto keys_output = *arguments.keys = arena.alloc<ustore_key_t>(total_keys, arguments.error).data();
    if (arguments.error->status) return;

    ::rocksdb::ReadOptions read_options;
    read_options.fill_cache = false;
    use_snapshot(database, arguments.snapshot, read_options, arguments.error);
    if (arguments.error->status) return;
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
    strided_iterator<ustore_collection_t const> collections {arguments.collections, arguments.collections_stride};
    strided_iterator<ustore_key_t const> start_keys {arguments.start_keys, arguments.start_keys_stride};
    strided_iterator<ustore_key_t const> end_keys {arguments.end_keys, arguments.end_keys_stride};
    ::rocksdb::SizeApproximationOptions size_options;

    // Properties describe a whole collection, so consecutive tasks over one read them once
    column_t* measured_column = nullptr;
    std::uint64_t keys_count = 0;
    std::uint64_t sst_files_size = 0;

    for (ustore_size_t task_index = 0; task_index != arguments.tasks_count; ++task_index) {
        column_t* const column = column_of(database, collections[task_index]);
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

void collection_create(ustore_collection_create_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!arguments.id) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need an output for the collection"};
        return;
    }

    database_t& database = *handle_cast<database_t>(arguments.db);
    std::lock_guard lock {database.columns_mutex};

    // A column family is addressed by a name of its own, so one is minted from the first unused number
    char minted[32] = "c";
    for (std::size_t serial = 0;; ++serial) {
        auto const written = std::to_chars(minted + 1, minted + sizeof(minted) - 1, serial);
        *written.ptr = '\0';
        bool taken = false;
        for (auto const& [id, column] : database.columns) taken = taken || column->GetName() == minted;
        if (!taken) break;
    }

    column_t* column = nullptr;
    ::rocksdb::ColumnFamilyOptions column_options;
    column_options.comparator = &key_comparator_k;
    native_status_t const status = database.native->CreateColumnFamily(column_options, minted, &column);
    if (export_error(status, arguments.error)) return;

    // A family that cannot be recorded could never be addressed, so it is dropped again
    if (st::failed(database.columns.upsert(st::mapping<ustore_collection_t, column_t*> {column->GetID(), column}))) {
        [[maybe_unused]] native_status_t const dropped = database.native->DropColumnFamily(column);
        database.native->DestroyColumnFamilyHandle(column);
        *arguments.error = {ustore_error_out_of_memory_k, "Failed to record the collection"};
        return;
    }

    // RocksDB persists the identifier of a column family, so a name resolved after a restart still addresses it
    *arguments.id = column->GetID();
}

void collection_drop(ustore_collection_drop_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }

    bool const invalidate = arguments.mode == ustore_drop_keys_vals_handle_k;
    bool const reserved = arguments.id == ustore_collection_main_k || arguments.id == ustore_collection_names_k;
    if (reserved && invalidate) {
        *arguments.error = {ustore_error_invalid_argument_k, "This collection can only be cleared"};
        return;
    }

    database_t& database = *handle_cast<database_t>(arguments.db);
    if (!invalidate) {
        column_t* const column = column_of(database, arguments.id);
        if (!column) *arguments.error = {ustore_error_not_found_k, "No such collection"};
        else export_error(clear_column(database, column, arguments.mode), arguments.error);
        return;
    }

    std::lock_guard lock {database.columns_mutex};
    auto const found = database.columns.find(arguments.id);
    if (found == database.columns.end()) {
        *arguments.error = {ustore_error_not_found_k, "No such collection"};
        return;
    }
    column_t* const column = (*found).mapped;
    if (export_error(database.native->DropColumnFamily(column), arguments.error)) return;
    [[maybe_unused]] bool const erased = database.columns.erase(arguments.id);
    database.native->DestroyColumnFamilyHandle(column);
}

void collection_list(ustore_collection_list_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!arguments.count) {
        *arguments.error = {ustore_error_invalid_argument_k, "Null outputs specified"};
        return;
    }

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;

    database_t& database = *handle_cast<database_t>(arguments.db);

    // Neither the default column family nor the reserved names collection is a collection the caller made
    auto const listed = [&](column_t* const column) noexcept {
        return column != database.names && column->GetName() != ::rocksdb::kDefaultColumnFamilyName;
    };

    std::shared_lock lock {database.columns_mutex};
    std::size_t collections_count = 0;
    for (auto const& [id, column] : database.columns) collections_count += listed(column);
    *arguments.count = static_cast<ustore_size_t>(collections_count);

    auto ids = arena.alloc_or_dummy(collections_count, arguments.error, arguments.ids);
    if (arguments.error->status) return;

    std::size_t index = 0;
    for (auto const& [id, column] : database.columns) {
        if (!listed(column)) continue;
        ids[index] = id;
        ++index;
    }
}

/** Serves "clear", removing every pair while keeping the collections and their names. */
void clear_pairs(database_t& database, ustore_database_control_t& arguments) {
    std::shared_lock lock {database.columns_mutex};
    for (auto const& [id, column] : database.columns) {
        if (column == database.names) continue;
        if (export_error(clear_column(database, column, ustore_drop_keys_vals_k), arguments.error)) return;
    }
}

/** Serves "reset", releasing every snapshot, dropping every collection and clearing the reserved ones. */
void reset_database(database_t& database, ustore_database_control_t& arguments) {
    {
        std::lock_guard lock {database.mutex};
        [[maybe_unused]] auto const released = database.snapshots.for_each([&](auto const& slot) noexcept {
            snapshot_t* const snapshot = reinterpret_cast<snapshot_t*>(slot.key());
            database.native->ReleaseSnapshot(snapshot->native);
            delete snapshot;
        });
        database.snapshots.clear();
    }

    // The map cannot lose a family while it is walked, so the ones to drop are gathered first
    std::lock_guard lock {database.columns_mutex};
    st::basic_vector<ustore_collection_t> dropped;
    if (st::failed(dropped.reserve(database.columns.size()))) {
        *arguments.error = {ustore_error_out_of_memory_k, "Failed to gather the collections"};
        return;
    }
    for (auto const& [id, column] : database.columns) {
        bool const reserved = column == database.names || column->GetName() == ::rocksdb::kDefaultColumnFamilyName;
        if (reserved) continue;
        [[maybe_unused]] auto const gathered = dropped.push_back(st::assume_reserved, ustore_collection_t {id});
    }
    for (ustore_collection_t const id : dropped) {
        column_t* const column = database.columns.at(id);
        if (export_error(database.native->DropColumnFamily(column), arguments.error)) return;
        [[maybe_unused]] bool const erased = database.columns.erase(id);
        database.native->DestroyColumnFamilyHandle(column);
    }
    column_t* const main = database.native->DefaultColumnFamily();
    if (export_error(clear_column(database, main, ustore_drop_keys_vals_k), arguments.error)) return;
    export_error(clear_column(database, database.names, ustore_drop_keys_vals_k), arguments.error);
}

/** Serves "compact", flushing every memtable and compacting every collection. */
void compact_database(database_t& database, ustore_database_control_t& arguments) {
    ::rocksdb::FlushOptions const flush_options;
    ::rocksdb::CompactRangeOptions const compact_options;
    std::shared_lock lock {database.columns_mutex};
    for (auto const& [id, column] : database.columns) {
        if (export_error(database.native->Flush(flush_options, column), arguments.error)) return;
        native_status_t const status = database.native->CompactRange(compact_options, column, nullptr, nullptr);
        if (export_error(status, arguments.error)) return;
    }
}

/** Writes @p text at @p cursor and answers where it ends. */
inline char* append_text(char* cursor, std::string_view text) noexcept {
    std::memcpy(cursor, text.data(), text.size());
    return cursor + text.size();
}

/** Serves "info", naming the engine and the RocksDB release it links. */
void describe_database(database_t&, ustore_database_control_t& arguments) {
    linked_memory_lock_t arena = linked_memory(arguments.arena, ustore_options_default_k, arguments.error);
    if (arguments.error->status) return;

    std::string const version = ::rocksdb::GetRocksVersionAsString();
    constexpr std::string_view prefix_k = R"({"engine":"rocksdb","version":")";
    constexpr std::string_view suffix_k = R"("})";
    char* const response =
        arena.alloc<char>(prefix_k.size() + version.size() + suffix_k.size() + 1, arguments.error).data();
    if (arguments.error->status) return;

    char* const end = append_text(append_text(append_text(response, prefix_k), version), suffix_k);
    *end = '\0';
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
    for (auto const& [id, column] : database.columns) {
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
    char* const response_end = response + response_capacity_k;

    char* cursor = append_text(response, R"({"keys":)");
    cursor = std::to_chars(cursor, response_end, keys_count).ptr;
    cursor = append_text(cursor, R"(,"memory_bytes":)");
    cursor = std::to_chars(cursor, response_end, memory_bytes).ptr;
    cursor = append_text(cursor, R"(,"disk_bytes":)");
    cursor = std::to_chars(cursor, response_end, disk_bytes).ptr;
    cursor = append_text(cursor, "}");
    *cursor = '\0';
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
    if (created) *arguments.transaction = static_cast<handle_t*>(created.release());
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
    if (!stage(database, transaction, arguments.error)) return;
    // A prepare takes no sequence number, so the latest one orders this stage before the commit that follows it
    if (arguments.sequence_number) *arguments.sequence_number = database.native->GetLatestSequenceNumber();
}

void transaction_commit(ustore_transaction_commit_t& arguments) {
    if (!arguments.transaction) return;

    validate_transaction_commit(arguments.transaction, arguments.options, arguments.error);
    if (arguments.error->status) return;

    database_t& database = *handle_cast<database_t>(arguments.db);
    transaction_t& transaction = *handle_cast<transaction_t>(arguments.transaction);
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
    if (arguments.sequence_number) *arguments.sequence_number = native.GetId();

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
