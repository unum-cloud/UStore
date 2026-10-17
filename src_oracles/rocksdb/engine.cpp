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

#include "ustore/utilities/algorithm.hpp"      // `reservoir_sample_iterator`
#include "ustore/utilities/blocking_files.hpp" // `directory_exists`

#include "engine.hpp"             // `engine_t`, `handle_t`, `handle_cast`
#include "utilities/batches.hpp"  // `strings_arg_t`, `validate_write`, `places_of`
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

    /** Families dropped while a snapshot was open, whose handles stay for the saves of those snapshots, destroyed with
     *  the last snapshot. */
    st::basic_vector<column_t*> retired;

    /** Guards @c retired and the decrements of @c open_snapshots, apart from @c columns_mutex, so a free from inside a
     *  call holding the families never waits on itself. A snapshot counts itself in under the families, which a drop
     *  holds exclusively. */
    std::mutex retired_mutex;

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

/** The transaction database behind @p database, which every call beginning a transaction or clearing a family
 *  reaches only on a database opened read-write, since the router refuses those calls on one opened read-only. */
transactional_t& transactional(database_t& database) noexcept {
    return static_cast<transactional_t&>(*database.native);
}

/** A snapshot handle, pinning one RocksDB snapshot of its database until freed. */
struct snapshot_t : handle_t {

    /** The database the snapshot was taken of, which releases it. */
    database_t* database = nullptr;

    /** The RocksDB snapshot. */
    ::rocksdb::Snapshot const* native = nullptr;

    /** The collections the database held when the snapshot was taken, with families that outlive their drops. */
    columns_t columns;
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
struct transaction_t : handle_t {

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
                             ::rocksdb::Slice key, value_view_t content) {
    native_status_t const status = transaction.batch.Put(column, key, to_slice(content));
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

/** The family @p collection addresses, or null for any identifier no caller-created family carries. The caller holds
 *  @c columns_mutex, which keeps the family alive for as long as it holds it. */
column_t* column_of(database_t& database, ustore_collection_id_t collection) noexcept {
    if (collection == ustore_collection_main_k) return database.main;
    auto const found = database.columns.find(collection);
    return found != database.columns.end() ? (*found).mapped.column : nullptr;
}

/** Refuses @p collection unless a family keyed as @p keying holds it. The caller holds @c columns_mutex. */
status_t refuse_collection(database_t& database, ustore_collection_id_t collection, keying_t keying) noexcept {
    keying_t held = keying_t::uuids_k;
    if (collection != ustore_collection_main_k) {
        auto const found = database.columns.find(collection);
        if (found == database.columns.end()) return {status_code_t::not_found_k, "No such collection"};
        held = keying_of((*found).mapped.modality);
    }
    if (held == keying) return {};
    return {status_code_t::invalid_argument_k,
            keying == keying_t::strings_k ? "The collection holds no strings" : "The collection holds strings"};
}

/** Refuses a batch addressing a collection no family keyed as @p keying holds. The caller holds @c columns_mutex, so
 *  every collection it accepts stays addressable until the call returns. */
status_t refuse_collections(database_t& database, strided_iterator<ustore_collection_id_t const> collections,
                            std::size_t count, keying_t keying) noexcept {
    if (!collections) return refuse_collection(database, ustore_collection_main_k, keying);
    for (std::size_t index = 0; index != count; ++index) {
        bool const repeated = index != 0 && collections[index] == collections[index - 1];
        if (repeated) continue;
        if (status_t const status = refuse_collection(database, collections[index], keying); status.failed())
            return status;
    }
    return {};
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
    if (view.snapshot) options.snapshot = handle_cast<snapshot_t>(view.snapshot)->native;
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
    native_status_t const status = transaction ? buffer_write(*transaction, column, place.collection, key, content)
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
                                                        values[task_index]);
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
    transaction_t* const transaction = handle_cast<transaction_t>(view.transaction);
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

/** Opens the database in the directory @p arguments name into their output: read-write, creating it when missing, or
 *  read-only through RocksDB's `OpenForReadOnly`, as @p access asks. */
template <typename arguments_type_>
status_t open_database(arguments_type_ const& arguments, access_t access) {
    if (!arguments.database) return {status_code_t::invalid_argument_k, "Need an output for the database"};
    *arguments.database = nullptr;
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
    // A family no caller could have created refuses the open
    for (auto& descriptor : column_descriptors) {
        ustore_modality_t modality = ustore_modality_any_k;
        ustore_collection_id_t id {};
        bool const known = descriptor.name == ::rocksdb::kDefaultColumnFamilyName ||
                           descriptor.name == names_column_name_k || read_family_name(descriptor.name, modality, id);
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

    std::unique_ptr<database_t> database {new (std::nothrow) database_t};
    if (!database) return {status_code_t::out_of_memory_k, "Failed to allocate the database"};
    database->access = access;

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
            database->native->DestroyColumnFamilyHandle(handle);
        }
    }
    if (!recorded) return {status_code_t::out_of_memory_k, "Failed to record the collections"};

    if (access == access_t::read_only_k) {
        if (!database->names) return {status_code_t::malformed_k, "The database holds no names of collections"};
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

        // The names live in a family of their own, keyed by the names, created the first time a database opens
        if (!database->names) {
            ::rocksdb::ColumnFamilyOptions names_options;
            names_options.comparator = ::rocksdb::BytewiseComparator();
            status = database->native->CreateColumnFamily(names_options, names_column_name_k, &database->names);
            if (!status.ok()) return status_of(status);
        }
    }

    // Every family remembers the name it carries, so a listing walks no names and a drop removes its own at once
    std::unique_ptr<::rocksdb::Iterator> names {
        database->native->NewIterator(::rocksdb::ReadOptions(), database->names)};
    for (names->SeekToFirst(); names->Valid(); names->Next()) {
        auto [named, named_status] = read_name_record(names->value());
        if (named_status.failed()) return named_status;
        auto const found = database->columns.find(named.id());
        if (found != database->columns.end()) (*found).mapped.name = names->key().ToString();
    }
    if (!names->status().ok()) return status_of(names->status());
    names.reset();
    database->engine = &engine_k;
    *arguments.database = c_handle<ustore_database_t>(database.release());
    return {};
}

status_t database_open(ustore_database_open_rocksdb_t const& arguments, view_t const&) {
    return open_database(arguments, access_t::read_write_k);
}

status_t database_view(ustore_database_view_rocksdb_t const& arguments, view_t const&) {
    return open_database(arguments, access_t::read_only_k);
}

/** Gathers the main family, the names one and every family @p columns lists into @p families. */
status_t gather_families(database_t const& database, columns_t const& columns,
                         st::basic_vector<column_t*>& families) noexcept {
    if (st::failed(families.reserve(columns.size() + 2)))
        return {status_code_t::out_of_memory_k, "Failed to gather the families"};
    [[maybe_unused]] auto const main_gathered = families.emplace_back(st::assume_reserved, database.main);
    [[maybe_unused]] auto const names_gathered = families.emplace_back(st::assume_reserved, database.names);
    for (auto const& [id, family] : columns) [[maybe_unused]]
        auto const gathered = families.emplace_back(st::assume_reserved, family.column);
    return {};
}

/** Reports the engine with the estimated keys of every collection, and the memtable and table file bytes of every
 *  family, the names one included. */
status_t database_describe(ustore_database_describe_t const& arguments, view_t const&) {
    database_t& database = *handle_cast<database_t>(arguments.database);
    ustore_database_description_t& description = *arguments.description;
    description.capabilities = ustore_capabilities_t(
        ustore_capabilities_transactions_k | ustore_capabilities_collections_k | ustore_capabilities_snapshots_k);
    description.engine = ustore_engine_rocksdb_k;
    description.isolation = ustore_isolation_snapshot_k;
    std::shared_lock lock {database.columns_mutex};
    st::basic_vector<column_t*> families;
    if (status_t const status = gather_families(database, database.columns, families); status.failed()) return status;
    for (column_t* const column : families) {
        std::uint64_t property = 0;
        if (column != database.names &&
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

    database_t& database = *handle_cast<database_t>(arguments.database);
    std::unique_ptr<snapshot_t> snapshot {new (std::nothrow) snapshot_t};
    if (!snapshot) return {status_code_t::out_of_memory_k, "Failed to allocate the snapshot handle"};

    // Creators and droppers take the families exclusively, so the recorded ones are the ones the snapshot reads
    std::shared_lock lock {database.columns_mutex};
    for (auto const& [id, family] : database.columns)
        if (st::failed(snapshot->columns.upsert(st::mapping<ustore_collection_id_t, family_t> {id, family})))
            return {status_code_t::out_of_memory_k, "Failed to record the collections of the snapshot"};
    snapshot->native = database.native->GetSnapshot();
    if (!snapshot->native) return {status_code_t::internal_k, "Failed to take a snapshot"};
    snapshot->engine = &engine_k;
    snapshot->database = &database;
    database.open_snapshots.fetch_add(1, std::memory_order_relaxed);
    if (arguments.read_stamp) *arguments.read_stamp = snapshot->native->GetSequenceNumber();
    *arguments.snapshot = c_handle<ustore_snapshot_t>(snapshot.release());
    return {};
}

/** Saves exactly the families the snapshot recorded, those dropped since included through the handles they keep until
 *  it is freed, so it reads no family of the database and takes no lock on them. */
status_t snapshot_save(ustore_snapshot_save_t const& arguments, view_t const& view) {
    database_t& database = *handle_cast<database_t>(database_of(arguments));
    snapshot_t const& snapshot = *handle_cast<snapshot_t>(view.snapshot);
    ::rocksdb::ReadOptions snapshot_options;
    snapshot_options.fill_cache = false;
    use_snapshot(view, snapshot_options);

    // Every family travels under its own name and comparator, the main and the names ones among them
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
        column_t* target = exported->DefaultColumnFamily();
        if (column != database.main) {
            ::rocksdb::ColumnFamilyOptions column_options;
            column_options.comparator = column->GetComparator();
            status = exported->CreateColumnFamily(column_options, column->GetName(), &target);
        }
        if (!status.ok()) break;

        ustore_size_t bytes = 0;
        column_file_path(file_path, path, column->GetID());
        std::unique_ptr<::rocksdb::Iterator> iterator {database.native->NewIterator(snapshot_options, column)};
        status = iterator ? export_column(*iterator, options, file_path, *exported, target, bytes)
                          : native_status_t::Aborted();
        if (target != exported->DefaultColumnFamily()) exported->DestroyColumnFamilyHandle(target);
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

#pragma region Reads and Writes

status_t blobs_write(ustore_blobs_write_t const& arguments, strings_arg_t const& values, view_t const& view) {
    database_t& database = *handle_cast<database_t>(arguments.database);
    transaction_t* const transaction = handle_cast<transaction_t>(view.transaction);
    place_batch_t const places = places_of(arguments);

    status_t const validated = validate_write(view.transaction, places, values, arguments.durability, view.watch);
    if (validated.failed() || !arguments.count) return validated;
    std::shared_lock lock {database.columns_mutex};
    status_t const refused = refuse_collections(database, places.collections_begin, arguments.count, keying_t::uuids_k);
    if (refused.failed()) return refused;
    if (status_t const status = refuse_staged_write(transaction); status.failed()) return status;

    if (arguments.count == 1) return write_one(database, transaction, places, values, arguments.durability);
    return write_many(database, transaction, places, values, arguments.durability);
}

status_t blobs_remove(ustore_blobs_remove_t const& arguments, view_t const& view) {
    database_t& database = *handle_cast<database_t>(arguments.database);
    transaction_t* const transaction = handle_cast<transaction_t>(view.transaction);
    place_batch_t const places = places_of(arguments);

    status_t const validated = validate_remove(view.transaction, places, arguments.durability, view.watch);
    if (validated.failed() || !arguments.count) return validated;
    std::shared_lock lock {database.columns_mutex};
    status_t const refused = refuse_collections(database, places.collections_begin, arguments.count, keying_t::uuids_k);
    if (refused.failed()) return refused;
    if (status_t const status = refuse_staged_write(transaction); status.failed()) return status;

    ::rocksdb::WriteBatch batch;
    for (std::size_t task_index = 0; task_index != places.count; ++task_index) {
        place_t const place = places[task_index];
        column_t* const column = column_of(database, place.collection);
        native_status_t const status = transaction
                                           ? buffer_remove(*transaction, column, place.collection, to_slice(place.key))
                                           : batch.Delete(column, to_slice(place.key));
        if (!status.ok()) return status_of(status);
    }
    if (transaction) return {};
    return status_of(database.native->Write(write_options_for(arguments.durability), &batch));
}

status_t blobs_read(ustore_blobs_read_t const& arguments, view_t const& view) {
    if (!arguments.count) return {};

    database_t& database = *handle_cast<database_t>(arguments.database);
    place_batch_t const places = places_of(arguments);
    if (status_t const status = validate_read(places); status.failed()) return status;
    std::shared_lock lock {database.columns_mutex};
    status_t const refused = refuse_collections(database, places.collections_begin, arguments.count, keying_t::uuids_k);
    if (refused.failed()) return refused;

    ::rocksdb::ReadOptions read_options;
    transaction_t* const transaction = reading_transaction(view, read_options);
    if (status_t const status = refuse_staged_watch(transaction, view.watch); status.failed()) return status;

    std::size_t const count = places.count;
    if (count == 1) {
        ::rocksdb::PinnableSlice value;
        native_status_t const status = get_one(database, transaction, read_options, places[0], view.watch, value);
        return export_values(arguments, &value, &status, view.scratch);
    }

    // Values stay pinned until the outputs copy them, so each is copied once, straight from its block
    std::unique_ptr<::rocksdb::PinnableSlice[]> values {new (std::nothrow)::rocksdb::PinnableSlice[count]};
    std::unique_ptr<native_status_t[]> statuses {new (std::nothrow) native_status_t[count]};
    if (!values || !statuses) return {status_code_t::out_of_memory_k, "Failed to allocate the batch"};

    if (status_t const status = get_many(database, transaction, read_options, places, view.watch, values.get(),
                                         statuses.get());
        status.failed())
        return status;
    return export_values(arguments, values.get(), statuses.get(), view.scratch);
}

status_t blobs_scan(ustore_blobs_scan_t const& arguments, view_t const& view) {
    database_t& database = *handle_cast<database_t>(arguments.database);
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    scan_batch_t const tasks {collections, strided(arguments.start_keys), arguments.wanted, arguments.count};

    if (status_t const status = validate_scan(arguments); status.failed() || !arguments.count) return status;
    std::shared_lock lock {database.columns_mutex};
    status_t const refused = refuse_collections(database, collections, arguments.count, keying_t::uuids_k);
    if (refused.failed()) return refused;

    ::rocksdb::ReadOptions read_options;
    read_options.fill_cache = false;
    transaction_t* const transaction = reading_transaction(view, read_options);

    // Consecutive tasks over one collection share an iterator, since `Seek` repositions it anywhere
    column_t* iterated_column = nullptr;
    std::unique_ptr<::rocksdb::Iterator> iterator;
    auto [outputs, outputs_status] = scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    progress_t progress {arguments.progress, arguments.progress_state, tasks.count};

    for (std::size_t task_index = 0; task_index != tasks.count; ++task_index) {
        scan_task_t const task = tasks[task_index];
        column_t* const column = column_of(database, task.collection);
        if (!iterator || column != iterated_column) {
            iterator.reset(new_iterator(database, transaction, read_options, column));
            if (!iterator) return {status_code_t::out_of_memory_k, "Failed to create a RocksDB iterator"};
            iterated_column = column;
        }

        outputs.open_task(task_index);
        std::size_t const scan_end = outputs.size() + task.limit;
        for (iterator->Seek(to_slice(task.start_key)); iterator->Valid() && outputs.size() != scan_end;
             iterator->Next()) {
            ustore_key_t key;
            std::memcpy(&key, iterator->key().data(), sizeof(ustore_key_t));
            if (status_t const status = outputs.push(key); status.failed()) return status;
        }
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
    std::shared_lock lock {database.columns_mutex};
    status_t const refused = refuse_collections(database, collections, arguments.count, keying_t::uuids_k);
    if (refused.failed()) return refused;

    ::rocksdb::ReadOptions read_options;
    read_options.fill_cache = false;
    transaction_t* const transaction = reading_transaction(view, read_options);

    column_t* iterated_column = nullptr;
    std::unique_ptr<::rocksdb::Iterator> iterator;
    auto [outputs, outputs_status] = scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;

    for (std::size_t task_index = 0; task_index != samples.count; ++task_index) {
        sample_task_t const task = samples[task_index];
        column_t* const column = column_of(database, task.collection);
        outputs.open_task(task_index);

        if (!iterator || column != iterated_column) {
            iterator.reset(new_iterator(database, transaction, read_options, column));
            if (!iterator) return {status_code_t::out_of_memory_k, "Failed to create a RocksDB iterator"};
            iterated_column = column;
        }

        if (status_t const status = reservoir_sample_iterator(iterator, task.limit, outputs); status.failed())
            return status;
        if (!iterator->status().ok()) return status_of(iterator->status());
    }
    return outputs.finish(arguments);
}

status_t blobs_measure(ustore_blobs_measure_t const& arguments, view_t const&) {
    if (!arguments.count) return {};
    database_t& database = *handle_cast<database_t>(arguments.database);
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    std::shared_lock lock {database.columns_mutex};
    status_t const refused = refuse_collections(database, collections, arguments.count, keying_t::uuids_k);
    if (refused.failed()) return refused;
    strided_iterator<ustore_key_t const> const start_keys = strided(arguments.start_keys);
    strided_iterator<ustore_key_t const> const end_keys = strided(arguments.end_keys);
    auto [outputs, outputs_status] = measure_outputs_t::make(arguments);
    if (outputs_status.failed()) return outputs_status;
    ::rocksdb::SizeApproximationOptions size_options;

    // Properties describe a whole collection, so consecutive tasks over one read them once
    column_t* measured_column = nullptr;
    std::uint64_t keys_count = 0;
    std::uint64_t sst_files_size = 0;

    for (std::size_t task_index = 0; task_index != arguments.count; ++task_index) {
        column_t* const column = column_of(database, collections ? collections[task_index] : ustore_collection_main_k);
        ustore_key_t const start_key = start_keys ? start_keys[task_index] : smallest_key_k;
        ustore_key_t const end_key = end_keys ? end_keys[task_index] : largest_key_k;
        ::rocksdb::Range const range(to_slice(start_key), to_slice(end_key));

        std::uint64_t approximate_size = 0;
        native_status_t const status = database.native->GetApproximateSizes(size_options, column, &range, 1,
                                                                            &approximate_size);
        if (!status.ok()) return status_of(status);

        if (column != measured_column) {
            keys_count = 0;
            sst_files_size = 0;
            database.native->GetIntProperty(column, "rocksdb.estimate-num-keys", &keys_count);
            database.native->GetIntProperty(column, "rocksdb.total-sst-files-size", &sst_files_size);
            measured_column = column;
        }

        outputs.min_cardinalities[task_index] = static_cast<ustore_size_t>(0);
        outputs.max_cardinalities[task_index] = static_cast<ustore_size_t>(keys_count);
        outputs.min_value_bytes[task_index] = static_cast<ustore_size_t>(0);
        outputs.max_value_bytes[task_index] = std::numeric_limits<ustore_size_t>::max();
        outputs.min_space_usages[task_index] = approximate_size;
        outputs.max_space_usages[task_index] = sst_files_size;
    }
    outputs.finish(arguments);
    return {};
}

#pragma endregion Reads and Writes

#pragma region Collections

/** Creates an empty family holding @p modality under @p name and records it, answering the identifier of its
 *  collection. The caller holds @c columns_mutex exclusively. */
expected<ustore_collection_id_t> create_family(database_t& database, ustore_modality_t modality,
                                               std::string_view name) {
    if (static_cast<std::size_t>(modality) >= modality_names_k.size())
        return status_t {status_code_t::invalid_argument_k, "Unknown modality"};
    auto [id, mint_status] = mint_collection_id();
    if (mint_status.failed()) return mint_status;

    // A family is named after what it holds and the collection it stores, so an open recovers both
    std::string family_name {modality_names_k[modality]};
    family_name.push_back('.');
    family_name.resize(family_name.size() + uuid_text_length_k);
    uuid_to_text(id.uuid, std::span<char>(family_name).last<uuid_text_length_k>());
    column_t* column = nullptr;
    native_status_t const created = database.native->CreateColumnFamily(::rocksdb::ColumnFamilyOptions(), family_name,
                                                                        &column);
    if (!created.ok()) return status_of(created);

    // A family that cannot be recorded could never be addressed, so it is dropped again
    if (st::failed(database.columns.upsert(
            st::mapping<ustore_collection_id_t, family_t> {id, family_t {column, modality, std::string(name)}}))) {
        [[maybe_unused]] native_status_t const dropped = database.native->DropColumnFamily(column);
        database.native->DestroyColumnFamilyHandle(column);
        return status_t {status_code_t::out_of_memory_k, "Failed to record the collection"};
    }
    return id;
}

/** Drops the family @p id addresses, forgetting first and durably the name it carries. The caller holds
 *  @c columns_mutex exclusively. */
status_t drop_family(database_t& database, ustore_collection_id_t id) {
    auto const found = database.columns.find(id);
    if (found == database.columns.end()) return {status_code_t::not_found_k, "No such collection"};
    family_t const& family = (*found).mapped;
    column_t* const column = family.column;

    // A crash between the two leaves an unnamed family behind, never a name addressing a family that is gone
    ::rocksdb::WriteOptions synced;
    synced.sync = true;
    if (!family.name.empty()) {
        native_status_t const forgotten = database.native->Delete(synced, database.names, family.name);
        if (!forgotten.ok()) return status_of(forgotten);
    }

    // A snapshot open across the drop still saves the family, so its handle waits for the last snapshot
    std::lock_guard retired_lock {database.retired_mutex};
    bool const retiring = database.open_snapshots.load(std::memory_order_relaxed) != 0;
    if (retiring && st::failed(database.retired.reserve(database.retired.size() + 1)))
        return {status_code_t::out_of_memory_k, "Failed to keep the dropped family for the open snapshots"};
    native_status_t const dropped = database.native->DropColumnFamily(column);
    if (!dropped.ok()) return status_of(dropped);
    [[maybe_unused]] bool const erased = database.columns.erase(id);
    if (retiring) [[maybe_unused]]
        auto const kept = database.retired.emplace_back(st::assume_reserved, column);
    else database.native->DestroyColumnFamilyHandle(column);
    return {};
}

status_t collection_create(ustore_collection_create_t const& arguments, view_t const&) {
    if (!arguments.collection) return {status_code_t::invalid_argument_k, "Need an output for the collection"};
    std::string_view const name = string_of(arguments.name);
    if (arguments.name.data && name.empty())
        return {status_code_t::invalid_argument_k, "A name is never empty, as the main collection has none"};

    // Creators take the families exclusively, so no other one can take the name between the lookup and the record
    database_t& database = *handle_cast<database_t>(arguments.database);
    std::lock_guard lock {database.columns_mutex};
    if (!name.empty()) {
        std::string record;
        native_status_t const looked_up = database.native->Get(::rocksdb::ReadOptions(), database.names, name, &record);
        if (looked_up.ok()) return {status_code_t::already_exists_k, "A collection already carries the name"};
        if (!looked_up.IsNotFound()) return status_of(looked_up);
    }
    auto [id, create_status] = create_family(database, arguments.modality, name);
    if (create_status.failed()) return create_status;

    // The family's name carries the identifier, so a name resolved after a restart still addresses it
    if (!name.empty()) {
        ::rocksdb::WriteOptions synced;
        synced.sync = true;
        name_record_t const named {id, arguments.modality};
        native_status_t const recorded = database.native->Put(synced, database.names, name, named.view());
        if (!recorded.ok()) {
            // A family no name addresses would be unreachable, so it goes, while the error that stopped the name stays
            [[maybe_unused]] status_t const dropped = drop_family(database, id);
            return status_of(recorded);
        }
    }
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
        std::lock_guard lock {database.columns_mutex};
        return drop_family(database, arguments.collection);
    }

    std::shared_lock lock {database.columns_mutex};
    column_t* const column = column_of(database, arguments.collection);
    if (!column) return {status_code_t::not_found_k, "No such collection"};
    return status_of(clear_column(database, column, arguments.mode));
}

status_t collection_list(ustore_collection_list_t const& arguments, view_t const& view) {
    if (!arguments.written) return {status_code_t::invalid_argument_k, "Need an output for the collections written"};

    database_t& database = *handle_cast<database_t>(arguments.database);
    std::shared_lock lock {database.columns_mutex};
    columns_t const& listed = view.snapshot ? handle_cast<snapshot_t>(view.snapshot)->columns : database.columns;
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

    database_t& database = *handle_cast<database_t>(arguments.database);
    ::rocksdb::ReadOptions read_options;
    use_snapshot(view, read_options);

    auto [outputs, outputs_status] = collection_find_outputs_t::make(arguments);
    if (outputs_status.failed()) return outputs_status;

    std::string record;
    for (std::size_t index = 0; index != arguments.count; ++index) {
        native_status_t const status = database.native->Get(read_options, database.names,
                                                            ::rocksdb::Slice(string_of(names[index])), &record);
        bool const present = status.ok();
        if (!present && !status.IsNotFound()) return status_of(status);
        auto [named, named_status] = present ? read_name_record(record) : name_record_t {};
        if (named_status.failed()) return named_status;
        outputs.assign(index, named.id(), named.modality(), present);
    }
    outputs.finish(arguments);
    return {};
}

/** Clears every collection as @p mode asks, keeping the collections and their names. */
status_t clear_families(database_t& database, ustore_drop_mode_t mode) {
    std::shared_lock lock {database.columns_mutex};
    native_status_t const cleared = clear_column(database, database.main, mode);
    if (!cleared.ok()) return status_of(cleared);
    for (auto const& [id, family] : database.columns) {
        native_status_t const status = clear_column(database, family.column, mode);
        if (!status.ok()) return status_of(status);
    }
    return {};
}

/** Drops every collection with its name and clears the main one, unless a snapshot is open, as the caller owns it and
 *  would read a database it never saw. */
status_t reset_database(database_t& database) {
    if (database.open_snapshots.load(std::memory_order_relaxed) != 0)
        return {status_code_t::invalid_argument_k, "Free every snapshot before resetting the database"};

    // The map cannot lose a family while it is walked, so the ones to drop are gathered first
    std::lock_guard lock {database.columns_mutex};
    st::basic_vector<ustore_collection_id_t> dropped;
    if (st::failed(dropped.reserve(database.columns.size())))
        return {status_code_t::out_of_memory_k, "Failed to gather the collections"};
    for (auto const& [id, family] : database.columns) [[maybe_unused]]
        auto const gathered = dropped.push_back(st::assume_reserved, ustore_collection_id_t {id});
    for (ustore_collection_id_t const id : dropped)
        if (status_t const status = drop_family(database, id); status.failed()) return status;
    native_status_t const cleared = clear_column(database, database.main, ustore_drop_pairs_k);
    if (!cleared.ok()) return status_of(cleared);
    return status_of(clear_column(database, database.names, ustore_drop_pairs_k));
}

status_t database_clear(ustore_database_clear_t const& arguments, view_t const&) {
    database_t& database = *handle_cast<database_t>(arguments.database);
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

/** Flushes every memtable and compacts every family, the main and the names ones included, reporting the bytes each
 *  family held before. */
status_t database_compact(ustore_database_compact_t const& arguments, view_t const&) {
    database_t& database = *handle_cast<database_t>(arguments.database);
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

#pragma region Strings

status_t strings_write(ustore_strings_write_t const& arguments, strings_arg_t const& texts, strings_arg_t const& values,
                       view_t const& view) {
    if (!arguments.count) return {};
    if (!texts || !values) return {status_code_t::invalid_argument_k, "Need the strings and their values"};

    database_t& database = *handle_cast<database_t>(arguments.database);
    transaction_t* const transaction = handle_cast<transaction_t>(view.transaction);
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);

    std::shared_lock lock {database.columns_mutex};
    status_t const refused = refuse_collections(database, collections, arguments.count, keying_t::strings_k);
    if (refused.failed()) return refused;
    if (status_t const status = refuse_staged_write(transaction); status.failed()) return status;

    ::rocksdb::WriteBatch batch;
    for (std::size_t index = 0; index != arguments.count; ++index) {
        ustore_collection_id_t const collection = collections[index];
        column_t* const column = column_of(database, collection);
        ::rocksdb::Slice const key {string_of(texts[index])};
        value_view_t const content = values[index];
        native_status_t const status = transaction ? buffer_write(*transaction, column, collection, key, content)
                                                   : batch.Put(column, key, to_slice(content));
        if (!status.ok()) return status_of(status);
    }
    if (transaction) return {};
    return status_of(database.native->Write(write_options_for(arguments.durability), &batch));
}

status_t strings_remove(ustore_strings_remove_t const& arguments, strings_arg_t const& texts, view_t const& view) {
    if (!arguments.count) return {};
    if (!texts) return {status_code_t::invalid_argument_k, "No strings were provided"};

    database_t& database = *handle_cast<database_t>(arguments.database);
    transaction_t* const transaction = handle_cast<transaction_t>(view.transaction);
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);

    std::shared_lock lock {database.columns_mutex};
    status_t const refused = refuse_collections(database, collections, arguments.count, keying_t::strings_k);
    if (refused.failed()) return refused;
    if (status_t const status = refuse_staged_write(transaction); status.failed()) return status;

    ::rocksdb::WriteBatch batch;
    for (std::size_t index = 0; index != arguments.count; ++index) {
        ustore_collection_id_t const collection = collections[index];
        column_t* const column = column_of(database, collection);
        ::rocksdb::Slice const key {string_of(texts[index])};
        native_status_t const status = transaction ? buffer_remove(*transaction, column, collection, key)
                                                   : batch.Delete(column, key);
        if (!status.ok()) return status_of(status);
    }
    if (transaction) return {};
    return status_of(database.native->Write(write_options_for(arguments.durability), &batch));
}

status_t strings_read(ustore_strings_read_t const& arguments, strings_arg_t const& texts, view_t const& view) {
    if (!arguments.count) return {};
    if (!texts) return {status_code_t::invalid_argument_k, "No strings were provided"};

    database_t& database = *handle_cast<database_t>(arguments.database);
    ::rocksdb::ReadOptions read_options;
    transaction_t* const transaction = reading_transaction(view, read_options);
    if (status_t const status = refuse_staged_watch(transaction, view.watch); status.failed()) return status;

    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    std::shared_lock lock {database.columns_mutex};
    status_t const refused = refuse_collections(database, collections, arguments.count, keying_t::strings_k);
    if (refused.failed()) return refused;

    // Values stay pinned until the outputs copy them, so each is copied once, straight from its block
    std::size_t const count = arguments.count;
    std::unique_ptr<::rocksdb::PinnableSlice[]> values {new (std::nothrow)::rocksdb::PinnableSlice[count]};
    std::unique_ptr<native_status_t[]> statuses {new (std::nothrow) native_status_t[count]};
    if (!values || !statuses) return {status_code_t::out_of_memory_k, "Failed to allocate the batch"};
    for (std::size_t index = 0; index != count; ++index) {
        ustore_collection_id_t const collection = collections[index];
        column_t* const column = column_of(database, collection);
        std::string_view const text = string_of(texts[index]);
        ::rocksdb::Slice const key {text};
        if (transaction && view.watch == ustore_watch_keys_k &&
            st::failed(transaction->watched_strings.push_back(std::pair {collection, std::string(text)})))
            return {status_code_t::out_of_memory_k, "Failed to watch a string"};
        statuses[index] = transaction ? transaction->batch.GetFromBatchAndDB(database.native.get(), read_options,
                                                                             column, key, &values[index])
                                      : database.native->Get(read_options, column, key, &values[index]);
    }
    return export_values(arguments, values.get(), statuses.get(), view.scratch);
}

status_t strings_scan(ustore_strings_scan_t const& arguments, strings_arg_t const& prefixes,
                      strings_arg_t const& starts, view_t const& view) {
    if (!arguments.count) return {};
    if (!arguments.offsets || !arguments.strings)
        return {status_code_t::invalid_argument_k, "Need outputs for the offsets and the strings"};

    database_t& database = *handle_cast<database_t>(arguments.database);
    ::rocksdb::ReadOptions read_options;
    read_options.fill_cache = false;
    transaction_t* const transaction = reading_transaction(view, read_options);

    std::size_t const count = arguments.count;
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    std::shared_lock lock {database.columns_mutex};
    status_t const refused = refuse_collections(database, collections, count, keying_t::strings_k);
    if (refused.failed()) return refused;

    auto [outputs, outputs_status] = strings_scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    progress_t progress {arguments.progress, arguments.progress_state, count};
    for (std::size_t task = 0; task != count; ++task) {
        std::string_view const prefix = prefixes ? string_of(prefixes[task]) : std::string_view {};
        std::string_view const start = starts ? string_of(starts[task]) : std::string_view {};
        std::string_view const first = start > prefix ? start : prefix;
        std::size_t const scan_end = outputs.size() + arguments.wanted;
        outputs.open_task(task);

        std::unique_ptr<::rocksdb::Iterator> iterator {
            new_iterator(database, transaction, read_options, column_of(database, collections[task]))};
        for (iterator->Seek(::rocksdb::Slice(first.data(), first.size()));
             iterator->Valid() && outputs.size() != scan_end; iterator->Next()) {
            std::string_view const text = iterator->key().ToStringView();
            if (!text.starts_with(prefix)) break;
            if (status_t const status = outputs.push(text); status.failed()) return status;
        }
        if (!iterator->status().ok()) return status_of(iterator->status());
        if (!progress.advance(1)) return progress_cancelled_k;
    }
    return outputs.finish(arguments);
}

#pragma endregion Strings

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

    database_t& database = *handle_cast<database_t>(arguments.database);
    std::unique_ptr<transaction_t> created;
    transaction_t* transaction = handle_cast<transaction_t>(*arguments.transaction);
    if (!transaction) {
        created.reset(new (std::nothrow) transaction_t);
        if (!created) return {status_code_t::out_of_memory_k, "Failed to allocate the transaction"};
        created->engine = &engine_k;
        transaction = created.get();
    }
    // A prepare left behind would pin the log and come back after a crash
    else if (transaction->staging == staging_t::staged_k) {
        native_status_t const rolled_back = transaction->native->Rollback();
        if (!rolled_back.ok()) return status_of(rolled_back);
    }

    if (status_t const status = begin(database, *transaction); status.failed()) return status;
    if (created) *arguments.transaction = c_handle<ustore_transaction_t>(created.release());
    return {};
}

status_t transaction_stage(ustore_transaction_stage_t const& arguments, view_t const&) {
    if (!arguments.transaction) return {status_code_t::uninitialized_k, "Transaction is uninitialized"};
    database_t& database = *handle_cast<database_t>(arguments.database);
    transaction_t& transaction = *handle_cast<transaction_t>(arguments.transaction);
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

    database_t& database = *handle_cast<database_t>(arguments.database);
    transaction_t& transaction = *handle_cast<transaction_t>(arguments.transaction);
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
    transaction_t& transaction = *handle_cast<transaction_t>(arguments.transaction);
    if (transaction.staging == staging_t::broken_k)
        return {status_code_t::invalid_argument_k, "A failed prepare left the transaction needing a reset"};
    if (transaction.staging == staging_t::buffering_k) return {};

    // The successor takes its snapshot while the stage still locks every key it read or wrote, so staging again
    // validates the kept reads and writes against every change since the first stage
    database_t& database = *handle_cast<database_t>(arguments.database);
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
    database_t& database = *snapshot->database;
    database.native->ReleaseSnapshot(snapshot->native);
    delete snapshot;
    std::lock_guard retired_lock {database.retired_mutex};
    if (database.open_snapshots.fetch_sub(1, std::memory_order_relaxed) != 1) return;
    for (column_t* const column : database.retired) database.native->DestroyColumnFamilyHandle(column);
    database.retired.clear();
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
    .transaction_init = &guarded<&transaction_init>,
    .transaction_stage = &guarded<&transaction_stage>,
    .transaction_commit = &guarded<&transaction_commit>,
    .transaction_rollback = &guarded<&transaction_rollback>,
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
    .transaction_free = &transaction_free,
    .snapshot_free = &snapshot_free,
    .database_free = &database_free,
};

} // namespace unum::ustore::oracles::rocksdb

using namespace unum::ustore;
using namespace unum::ustore::oracles::rocksdb;

#if defined(__linux__)

/** Lets RocksDB's POSIX file system open io_uring rings, which it leaves off unless this weak symbol is defined. A
 *  thread whose ring the kernel or a seccomp profile refuses reads through `pread` instead. */
extern "C" bool RocksDbIOUringEnable() { return true; }
#endif

void ustore_database_open_rocksdb(ustore_database_open_rocksdb_t* arguments) {
    route_init(arguments, &guarded<&database_open, ustore_database_open_rocksdb_t, view_t>);
}

void ustore_database_view_rocksdb(ustore_database_view_rocksdb_t* arguments) {
    route_init(arguments, &guarded<&database_view, ustore_database_view_rocksdb_t, view_t>);
}
