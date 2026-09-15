/**
 *  @file src/rocksdb/engine.cpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief Embedded persistent key-value store on RocksDB, with optimistic transactions and snapshots.
 *
 *  Named collections are column families, and keys are fixed-width integers under a custom comparator.
 *  Tables stay block-based, since the plain-table format lacks non-prefix seeks that scans need.
 */

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::strlen`

#include <charconv> // `std::to_chars`
#include <limits>   // `std::numeric_limits`
#include <memory>   // `std::unique_ptr`
#include <mutex>    // `std::lock_guard`, `std::mutex`, `std::unique_lock`
#include <new>      // `std::bad_alloc`, `std::nothrow`
#include <string>   // `std::string`, the path and name type RocksDB takes
#include <vector>   // `std::vector`, the descriptor, handle and file lists RocksDB takes

#include <rocksdb/db.h>
#include <rocksdb/sst_file_writer.h>
#include <rocksdb/utilities/optimistic_transaction_db.h>
#include <rocksdb/utilities/options_util.h>
#include <rocksdb/utilities/transaction.h>

#include <smashtable/basic_hash_table.hpp>
#include <smashtable/basic_vector.hpp>

#include <ustore/db.h>
#include <ustore/ranges_args.hpp> // `places_arg_t`

#include "helpers/config_loader.hpp" // `load_config`
#include "helpers/full_scan.hpp"     // `reservoir_sample_iterator`
#include "helpers/linked_memory.hpp" // `linked_memory_lock_t`

ustore_collection_t const ustore_collection_main_k = 0;
ustore_length_t const ustore_length_missing_k = std::numeric_limits<ustore_length_t>::max();
ustore_key_t const ustore_key_unknown_k = std::numeric_limits<ustore_key_t>::max();

namespace unum::ustore::rocksdb {

#pragma region Database

/** The open RocksDB database. */
using native_t = ::rocksdb::OptimisticTransactionDB;

/** Status RocksDB answers. */
using native_status_t = ::rocksdb::Status;

/** A RocksDB transaction. */
using transaction_t = ::rocksdb::Transaction;

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
using snapshots_t = ashvardanian::smashtable::hash_set<ustore_snapshot_t>;

/** Column family handles, which the database destroys before it closes. */
using columns_t = ashvardanian::smashtable::basic_vector<column_t*>;

/** The open database, its column families and its snapshots. */
struct database_t {

    /** Handles of every column family, the default one included. */
    columns_t columns;

    /** Identifiers of the live snapshots. */
    snapshots_t snapshots;

    /** The open database. */
    std::unique_ptr<native_t> native;

    /** Guards the snapshots and reading the sequence number of a commit. */
    std::mutex mutex;
};

inline ::rocksdb::Slice to_slice(ustore_key_t const& key) noexcept {
    return {reinterpret_cast<char const*>(&key), sizeof(ustore_key_t)};
}

inline ::rocksdb::Slice to_slice(value_view_t value) noexcept {
    return {reinterpret_cast<char const*>(value.begin()), value.size()};
}

/** Exports a failed status into @p error, answering whether it failed. */
bool export_error(native_status_t const& status, ustore_error_t* error) noexcept {
    if (status.ok()) return false;

    if (status.IsCorruption()) *error = "Failure: DB Corruption";
    else if (status.IsIOError()) *error = "Failure: IO  Error";
    else if (status.IsInvalidArgument()) *error = "Failure: Invalid Argument";
    else *error = "Failure";
    return true;
}

column_t* column_of(database_t& database, ustore_collection_t collection) noexcept {
    return collection == ustore_collection_main_k ? database.native->DefaultColumnFamily()
                                                  : reinterpret_cast<column_t*>(collection);
}

/** Points @p options at the snapshot @p id, leaving them untouched for the zero identifier. */
void use_snapshot(database_t& database, ustore_snapshot_t id, ::rocksdb::ReadOptions& options, ustore_error_t* error) {
    if (!id) return;
    std::lock_guard lock {database.mutex};
    if (database.snapshots.find(id) == database.snapshots.end()) {
        *error = "The snapshot doesn't exist!";
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

    bool const safe = options & ustore_option_write_flush_k;
    bool const watch = !(options & ustore_option_transaction_dont_watch_k);

    ::rocksdb::WriteOptions write_options;
    write_options.sync = safe;
    write_options.disableWAL = !safe;

    auto place = places[0];
    auto content = contents[0];
    column_t* const column = column_of(database, place.collection);
    auto const key = to_slice(place.key);
    native_status_t status;

    if (transaction)
        status = !content //
                     ? watch ? transaction->Delete(column, key) : transaction->DeleteUntracked(column, key)
                     : watch ? transaction->Put(column, key, to_slice(content))
                             : transaction->PutUntracked(column, key, to_slice(content));
    else
        status = !content //
                     ? database.native->Delete(write_options, column, key)
                     : database.native->Put(write_options, column, key, to_slice(content));

    export_error(status, error);
}

void write_many(database_t& database, transaction_t* transaction, places_arg_t const& places,
                contents_arg_t const& contents, ustore_options_t const options, ustore_error_t* error) {

    bool const safe = options & ustore_option_write_flush_k;
    bool const watch = !(options & ustore_option_transaction_dont_watch_k);

    ::rocksdb::WriteOptions write_options;
    write_options.sync = safe;
    write_options.disableWAL = !safe;

    if (transaction) {
        for (std::size_t task_index = 0; task_index != places.size(); ++task_index) {
            auto place = places[task_index];
            auto content = contents[task_index];
            column_t* const column = column_of(database, place.collection);
            auto const key = to_slice(place.key);
            native_status_t const status = !content ? watch ? transaction->Delete(column, key)
                                                            : transaction->DeleteUntracked(column, key)
                                           : watch  ? transaction->Put(column, key, to_slice(content))
                                                    : transaction->PutUntracked(column, key, to_slice(content));
            if (export_error(status, error)) return;
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

/** Reads one place into @p value, tracking its key for conflicts unless @p options say not to watch it. */
native_status_t get_one(database_t& database, transaction_t* transaction, ::rocksdb::ReadOptions const& read_options,
                        place_t const& place, ustore_options_t const options, ::rocksdb::PinnableSlice& value) {

    column_t* const column = column_of(database, place.collection);
    auto const key = to_slice(place.key);
    if (!transaction) return database.native->Get(read_options, column, key, &value);

    bool const watch = !(options & ustore_option_transaction_dont_watch_k);
    return watch ? transaction->GetForUpdate(read_options, column, key, &value)
                 : transaction->Get(read_options, column, key, &value);
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
    bool const watch = !(options & ustore_option_transaction_dont_watch_k);
    bool const same_collection = places.same_collection();

    // `MultiGetForUpdate` takes only vectors and a transaction batches one collection, both reading key by key
    if (transaction && (watch || !same_collection)) {
        for (std::size_t task_index = 0; task_index != count; ++task_index)
            statuses[task_index] = get_one(database, transaction, read_options, places[task_index], options,
                                           values[task_index]);
        return;
    }

    std::unique_ptr<::rocksdb::Slice[]> keys {new (std::nothrow)::rocksdb::Slice[count]};
    if (!keys) {
        *error = "Failed to allocate the batch";
        return;
    }
    for (std::size_t task_index = 0; task_index != count; ++task_index)
        keys[task_index] = to_slice(places[task_index].key);

    if (same_collection) {
        column_t* const column = column_of(database, places[0].collection);
        bool const sorted = keys_ascend(places);
        if (transaction) transaction->MultiGet(read_options, column, count, keys.get(), values, statuses, sorted);
        else database.native->MultiGet(read_options, column, count, keys.get(), values, statuses, sorted);
        return;
    }

    std::unique_ptr<column_t*[]> columns {new (std::nothrow) column_t*[count]};
    if (!columns) {
        *error = "Failed to allocate the batch";
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
    if (*arguments.error) return;
    auto lengths = arena.alloc_or_dummy(count, arguments.error, arguments.lengths);
    if (*arguments.error) return;
    auto presences = arena.alloc_or_dummy(count, arguments.error, arguments.presences);
    if (*arguments.error) return;

    bool const needs_export = arguments.values != nullptr;
    byte_t* const tape = needs_export ? arena.alloc<byte_t>(tape_bytes, arguments.error).begin() : nullptr;
    if (*arguments.error) return;

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
    if (!status.ok()) { [[maybe_unused]] native_status_t const removed = options.env->DeleteFile(file_path); }
    return status;
}

#pragma endregion Snapshots

#pragma region Lifetime

void database_init(ustore_database_init_t& arguments) {
    if (!arguments.config) {
        *arguments.error = "Null config specified";
        return;
    }

    config_t config;
    auto loaded = load_config(arguments.config, config);
    if (!loaded) {
        *arguments.error = loaded.message();
        return;
    }

    if (!is_directory(config.directory.data())) {
        *arguments.error = "Root isn't a directory";
        return;
    }
    if (!config.engine.config_url.empty()) {
        *arguments.error = "Doesn't support URL configs";
        return;
    }

    // Recovery depends on the options file: https://github.com/facebook/rocksdb/wiki/RocksDB-Options-File
    ::rocksdb::Options options;
    options.compression = ::rocksdb::kNoCompression;
    ::rocksdb::ColumnFamilyOptions column_options;
    std::vector<::rocksdb::ColumnFamilyDescriptor> column_descriptors;
    ::rocksdb::ConfigOptions config_options;
    native_status_t status;

    if (!config.engine.config_file_path.empty()) {
        status = ::rocksdb::LoadOptionsFromFile(config_options, std::string(config.engine.config_file_path), &options,
                                                &column_descriptors);
        if (!status.ok()) {
            *arguments.error = "Couldn't parse RocksDB config";
            return;
        }
    }

    yyjson_val* const database_json = yyjson_obj_get(config.engine.config, "DBOptions");
    yyjson_val* const column_json = yyjson_obj_get(config.engine.config, "CFOptions");
    bool const valid = //
        read_json_member(database_json, "create_if_missing", options.create_if_missing) &&
        read_json_member(database_json, "writable_file_max_buffer_size", options.writable_file_max_buffer_size) &&
        read_json_member(database_json, "max_open_files", options.max_open_files) &&
        read_json_member(database_json, "max_file_opening_threads", options.max_file_opening_threads) &&
        read_json_member(column_json, "max_write_buffer_number", column_options.max_write_buffer_number) &&
        read_json_member(column_json, "write_buffer_size", column_options.write_buffer_size) &&
        read_json_member(column_json, "target_file_size_base", column_options.target_file_size_base) &&
        read_json_member(column_json, "max_compaction_bytes", column_options.max_compaction_bytes) &&
        read_json_member(column_json, "level_compaction_dynamic_level_bytes",
                         column_options.level_compaction_dynamic_level_bytes) &&
        read_json_member(column_json, "level0_stop_writes_trigger", column_options.level0_stop_writes_trigger) &&
        read_json_member(column_json, "target_file_size_multiplier", column_options.target_file_size_multiplier) &&
        read_json_member(column_json, "max_bytes_for_level_multiplier", column_options.max_bytes_for_level_multiplier);
    if (!valid) {
        *arguments.error = "Unsupported type in RocksDB configuration key";
        return;
    }

    std::string const root(config.directory);
    status = ::rocksdb::LoadLatestOptions(config_options, root, &options, &column_descriptors);
    if (!status.ok() && !status.IsNotFound()) {
        *arguments.error = "Recovering RocksDB state";
        return;
    }

    if (column_descriptors.empty()) column_descriptors.push_back({::rocksdb::kDefaultColumnFamilyName, column_options});
    for (auto& descriptor : column_descriptors) descriptor.options.comparator = &key_comparator_k;

    // An export persists `error_if_exists`, which would refuse every reopening of it
    options.create_if_missing = true;
    options.error_if_exists = false;
    options.comparator = &key_comparator_k;
    for (disk_config_t const& disk : config.data_directories)
        options.db_paths.push_back({std::string(disk.path), disk.max_size});

    std::unique_ptr<database_t> database {new (std::nothrow) database_t};
    if (!database) {
        *arguments.error = "Failed to allocate the database";
        return;
    }

    native_t* native = nullptr;
    std::vector<column_t*> handles;
    ::rocksdb::OptimisticTransactionDBOptions transaction_options;
    status = native_t::Open(options, transaction_options, root, column_descriptors, &handles, &native);
    if (!status.ok()) {
        *arguments.error = "Opening RocksDB with options";
        return;
    }
    database->native.reset(native);

    if (ashvardanian::smashtable::failed(database->columns.reserve(handles.size()))) {
        for (column_t* const handle : handles) database->native->DestroyColumnFamilyHandle(handle);
        *arguments.error = "Failed to allocate the collection handles";
        return;
    }
    for (column_t* const handle : handles) {
        [[maybe_unused]] auto const kept = database->columns.emplace_back(handle);
    }
    *arguments.db = database.release();
}

void get_metadata(ustore_get_metadata_t& arguments) {
    *arguments.metadata = ustore_metadata_t(ustore_supports_transactions_k | ustore_supports_named_collections_k |
                                            ustore_supports_snapshots_k);
}

#pragma endregion Lifetime

#pragma region Snapshots

void snapshot_list(ustore_snapshot_list_t& arguments) {
    if (!arguments.db) {
        *arguments.error = "DataBase is uninitialized";
        return;
    }
    if (!arguments.count || !arguments.ids) {
        *arguments.error = "Need outputs!";
        return;
    }

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (*arguments.error) return;

    database_t& database = *reinterpret_cast<database_t*>(arguments.db);
    std::lock_guard lock {database.mutex};
    std::size_t const snapshots_count = database.snapshots.size();
    *arguments.count = static_cast<ustore_size_t>(snapshots_count);

    auto ids = arena.alloc_or_dummy(snapshots_count, arguments.error, arguments.ids);
    if (*arguments.error) return;

    std::size_t index = 0;
    [[maybe_unused]] auto const walked = database.snapshots.for_each(
        [&](auto const& slot) noexcept { ids[index++] = slot.key(); });
}

void snapshot_create(ustore_snapshot_create_t& arguments) {
    if (!arguments.db) {
        *arguments.error = "DataBase is uninitialized";
        return;
    }

    database_t& database = *reinterpret_cast<database_t*>(arguments.db);
    std::lock_guard lock {database.mutex};
    if (database.snapshots.find(*arguments.id) != database.snapshots.end()) {
        *arguments.error = "Such snapshot already exists!";
        return;
    }

    std::unique_ptr<snapshot_t> snapshot {new (std::nothrow) snapshot_t};
    if (!snapshot) {
        *arguments.error = "Allocating snapshot handle";
        return;
    }
    snapshot->native = database.native->GetSnapshot();
    if (!snapshot->native) {
        *arguments.error = "Couldn't get a snapshot!";
        return;
    }

    auto const id = reinterpret_cast<ustore_snapshot_t>(snapshot.get());
    if (ashvardanian::smashtable::failed(database.snapshots.upsert(id))) {
        database.native->ReleaseSnapshot(snapshot->native);
        *arguments.error = "Failed to register the snapshot";
        return;
    }
    [[maybe_unused]] snapshot_t* const registered = snapshot.release();
    *arguments.id = id;
}

void snapshot_export(ustore_snapshot_export_t& arguments) {
    if (!arguments.db) {
        *arguments.error = "DataBase is uninitialized";
        return;
    }
    database_t& database = *reinterpret_cast<database_t*>(arguments.db);

    ::rocksdb::ReadOptions snapshot_options;
    snapshot_options.fill_cache = false;
    use_snapshot(database, arguments.id, snapshot_options, arguments.error);
    if (*arguments.error) return;
    if (!snapshot_options.snapshot) {
        *arguments.error = "The snapshot doesn't exist!";
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
    char digits[std::numeric_limits<std::size_t>::digits10 + 1];

    for (std::size_t column_index = 0; status.ok() && column_index != database.columns.size(); ++column_index) {
        column_t* const column = database.columns[column_index];
        column_t* target = exported->DefaultColumnFamily();
        if (column->GetName() != ::rocksdb::kDefaultColumnFamilyName)
            status = exported->CreateColumnFamily(column_options, column->GetName(), &target);
        if (!status.ok()) break;

        // File names stay apart from the numbered ones RocksDB owns in the same directory
        char const* const digits_end = std::to_chars(digits, digits + sizeof(digits), column_index).ptr;
        file_path.assign(path)
            .append("/column-")
            .append(digits, static_cast<std::size_t>(digits_end - digits))
            .append(".sst");

        std::unique_ptr<::rocksdb::Iterator> iterator {database.native->NewIterator(snapshot_options, column)};
        status = iterator ? export_column(*iterator, options, file_path, *exported, target)
                          : native_status_t::Aborted();
        if (target != exported->DefaultColumnFamily()) exported->DestroyColumnFamilyHandle(target);
    }

    exported.reset();
    if (!status.ok()) { [[maybe_unused]] native_status_t const destroyed = ::rocksdb::DestroyDB(path, options); }
    export_error(status, arguments.error);
}

void snapshot_drop(ustore_snapshot_drop_t& arguments) {
    if (!arguments.id) return;

    database_t& database = *reinterpret_cast<database_t*>(arguments.db);
    std::lock_guard lock {database.mutex};
    if (!database.snapshots.erase(arguments.id)) return;

    snapshot_t* const snapshot = reinterpret_cast<snapshot_t*>(arguments.id);
    database.native->ReleaseSnapshot(snapshot->native);
    delete snapshot;
}

#pragma endregion Snapshots

#pragma region Reads and Writes

void write(ustore_write_t& arguments) {
    if (!arguments.db) {
        *arguments.error = "DataBase is uninitialized";
        return;
    }
    database_t& database = *reinterpret_cast<database_t*>(arguments.db);
    transaction_t* const transaction = reinterpret_cast<transaction_t*>(arguments.transaction);
    strided_iterator<ustore_collection_t const> collections {arguments.collections, arguments.collections_stride};
    strided_iterator<ustore_key_t const> keys {arguments.keys, arguments.keys_stride};
    strided_iterator<ustore_bytes_cptr_t const> values {arguments.values, arguments.values_stride};
    strided_iterator<ustore_length_t const> offsets {arguments.offsets, arguments.offsets_stride};
    strided_iterator<ustore_length_t const> lengths {arguments.lengths, arguments.lengths_stride};
    bits_view_t presences {arguments.presences};

    places_arg_t places {collections, keys, {}, arguments.tasks_count};
    contents_arg_t contents {presences, offsets, lengths, values, arguments.tasks_count};

    validate_write(arguments.transaction, places, contents, arguments.options, arguments.error);
    if (*arguments.error) return;
    if (!arguments.tasks_count) return;

    if (arguments.tasks_count == 1)
        write_one(database, transaction, places, contents, arguments.options, arguments.error);
    else write_many(database, transaction, places, contents, arguments.options, arguments.error);
}

void read(ustore_read_t& arguments) {
    if (!arguments.db) {
        *arguments.error = "DataBase is uninitialized";
        return;
    }
    if (!arguments.tasks_count) return;

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (*arguments.error) return;

    database_t& database = *reinterpret_cast<database_t*>(arguments.db);
    transaction_t* const transaction = reinterpret_cast<transaction_t*>(arguments.transaction);

    strided_iterator<ustore_collection_t const> collections {arguments.collections, arguments.collections_stride};
    strided_iterator<ustore_key_t const> keys {arguments.keys, arguments.keys_stride};
    places_arg_t places {collections, keys, {}, arguments.tasks_count};
    validate_read(arguments.transaction, places, arguments.options, arguments.error);
    if (*arguments.error) return;

    ::rocksdb::ReadOptions read_options;
    use_snapshot(database, arguments.snapshot, read_options, arguments.error);
    if (*arguments.error) return;

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
        *arguments.error = "Failed to allocate the batch";
        return;
    }

    get_many(database, transaction, read_options, places, arguments.options, values.get(), statuses.get(),
             arguments.error);
    if (*arguments.error) return;
    export_values(arguments, arena, values.get(), statuses.get(), count);
}

void scan(ustore_scan_t& arguments) {
    if (!arguments.db) {
        *arguments.error = "DataBase is uninitialized";
        return;
    }

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (*arguments.error) return;

    database_t& database = *reinterpret_cast<database_t*>(arguments.db);
    transaction_t* const transaction = reinterpret_cast<transaction_t*>(arguments.transaction);
    strided_iterator<ustore_collection_t const> collections {arguments.collections, arguments.collections_stride};
    strided_iterator<ustore_key_t const> start_keys {arguments.start_keys, arguments.start_keys_stride};
    strided_iterator<ustore_length_t const> limits {arguments.count_limits, arguments.count_limits_stride};
    scans_arg_t tasks {collections, start_keys, limits, arguments.tasks_count};

    validate_scan(arguments.transaction, tasks, arguments.options, arguments.error);
    if (*arguments.error) return;

    auto offsets = arena.alloc_or_dummy(tasks.count + 1, arguments.error, arguments.offsets);
    if (*arguments.error) return;
    auto counts = arena.alloc_or_dummy(tasks.count, arguments.error, arguments.counts);
    if (*arguments.error) return;

    auto const total_keys = reduce_n(tasks.limits, tasks.count, 0ul);
    auto keys_output = *arguments.keys = arena.alloc<ustore_key_t>(total_keys, arguments.error).begin();
    if (*arguments.error) return;

    ::rocksdb::ReadOptions read_options;
    read_options.fill_cache = false;
    use_snapshot(database, arguments.snapshot, read_options, arguments.error);
    if (*arguments.error) return;

    // Consecutive tasks over one collection share an iterator, since `Seek` repositions it anywhere
    column_t* iterated_column = nullptr;
    std::unique_ptr<::rocksdb::Iterator> iterator;

    for (ustore_size_t task_index = 0; task_index != arguments.tasks_count; ++task_index) {
        scan_t task = tasks[task_index];
        column_t* const column = column_of(database, task.collection);
        if (!iterator || column != iterated_column) {
            iterator.reset(transaction ? transaction->GetIterator(read_options, column)
                                       : database.native->NewIterator(read_options, column));
            if (!iterator) {
                *arguments.error = "Creating a RocksDB iterator";
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
        *arguments.error = "DataBase is uninitialized";
        return;
    }
    if (!arguments.tasks_count) return;

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (*arguments.error) return;

    database_t& database = *reinterpret_cast<database_t*>(arguments.db);
    transaction_t* const transaction = reinterpret_cast<transaction_t*>(arguments.transaction);
    strided_iterator<ustore_collection_t const> collections {arguments.collections, arguments.collections_stride};
    strided_iterator<ustore_length_t const> limits {arguments.count_limits, arguments.count_limits_stride};
    sample_args_t samples {collections, limits, arguments.tasks_count};

    auto offsets = arena.alloc_or_dummy(samples.count + 1, arguments.error, arguments.offsets);
    if (*arguments.error) return;
    auto counts = arena.alloc_or_dummy(samples.count, arguments.error, arguments.counts);
    if (*arguments.error) return;

    auto const total_keys = reduce_n(samples.limits, samples.count, 0ul);
    auto keys_output = *arguments.keys = arena.alloc<ustore_key_t>(total_keys, arguments.error).begin();
    if (*arguments.error) return;

    ::rocksdb::ReadOptions read_options;
    read_options.fill_cache = false;
    use_snapshot(database, arguments.snapshot, read_options, arguments.error);
    if (*arguments.error) return;

    column_t* iterated_column = nullptr;
    std::unique_ptr<::rocksdb::Iterator> iterator;

    for (std::size_t task_index = 0; task_index != samples.count; ++task_index) {
        sample_arg_t task = samples[task_index];
        column_t* const column = column_of(database, task.collection);
        offsets[task_index] = keys_output - *arguments.keys;

        if (!iterator || column != iterated_column) {
            iterator.reset(transaction ? transaction->GetIterator(read_options, column)
                                       : database.native->NewIterator(read_options, column));
            if (!iterator) {
                *arguments.error = "Creating a RocksDB iterator";
                return;
            }
            iterated_column = column;
        }

        ptr_range<ustore_key_t> sampled_keys(keys_output, task.limit);
        reservoir_sample_iterator(iterator, sampled_keys, arguments.error);

        counts[task_index] = task.limit;
        keys_output += task.limit;
    }
    offsets[samples.count] = keys_output - *arguments.keys;
}

void measure(ustore_measure_t& arguments) {
    if (!arguments.db) {
        *arguments.error = "DataBase is uninitialized";
        return;
    }

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (*arguments.error) return;

    auto min_cardinalities = arena.alloc_or_dummy(arguments.tasks_count, arguments.error, arguments.min_cardinalities);
    auto max_cardinalities = arena.alloc_or_dummy(arguments.tasks_count, arguments.error, arguments.max_cardinalities);
    auto min_value_bytes = arena.alloc_or_dummy(arguments.tasks_count, arguments.error, arguments.min_value_bytes);
    auto max_value_bytes = arena.alloc_or_dummy(arguments.tasks_count, arguments.error, arguments.max_value_bytes);
    auto min_space_usages = arena.alloc_or_dummy(arguments.tasks_count, arguments.error, arguments.min_space_usages);
    auto max_space_usages = arena.alloc_or_dummy(arguments.tasks_count, arguments.error, arguments.max_space_usages);
    if (*arguments.error) return;

    database_t& database = *reinterpret_cast<database_t*>(arguments.db);
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
    std::size_t const name_length = arguments.name ? std::strlen(arguments.name) : 0;
    if (!name_length) {
        *arguments.error = "Default collection is always present";
        return;
    }
    if (!arguments.db) {
        *arguments.error = "DataBase is uninitialized";
        return;
    }

    database_t& database = *reinterpret_cast<database_t*>(arguments.db);
    for (column_t* const existing : database.columns)
        if (existing && existing->GetName() == arguments.name) {
            *arguments.error = "Such collection already exists!";
            return;
        }

    // Room for the handle comes first, so a created column family is never left unrecorded
    bool const reserved = !ashvardanian::smashtable::failed(database.columns.reserve(database.columns.size() + 1));
    if (!reserved) {
        *arguments.error = "Failed to allocate a collection handle";
        return;
    }

    column_t* column = nullptr;
    ::rocksdb::ColumnFamilyOptions column_options;
    column_options.comparator = &key_comparator_k;
    native_status_t const status = database.native->CreateColumnFamily(column_options, arguments.name, &column);
    if (export_error(status, arguments.error)) return;

    [[maybe_unused]] auto const recorded = database.columns.emplace_back(column);
    *arguments.id = reinterpret_cast<ustore_collection_t>(column);
}

void collection_drop(ustore_collection_drop_t& arguments) {
    if (!arguments.db) {
        *arguments.error = "DataBase is uninitialized";
        return;
    }

    bool const invalidate = arguments.mode == ustore_drop_keys_vals_handle_k;
    if (arguments.id == ustore_collection_main_k && invalidate) {
        *arguments.error = "Default collection can't be invalidated.";
        return;
    }

    database_t& database = *reinterpret_cast<database_t*>(arguments.db);
    column_t* column = database.native->DefaultColumnFamily();
    std::size_t position = database.columns.size();
    if (arguments.id != ustore_collection_main_k) {
        column = reinterpret_cast<column_t*>(arguments.id);
        position = 0;
        while (position != database.columns.size() && database.columns[position] != column) ++position;
        if (position == database.columns.size()) {
            *arguments.error = "No such collection";
            return;
        }
    }

    if (arguments.mode == ustore_drop_keys_vals_handle_k) {
        native_status_t const status = database.native->DropColumnFamily(column);
        if (export_error(status, arguments.error)) return;
        database.native->DestroyColumnFamilyHandle(column);
        database.columns[position] = database.columns[database.columns.size() - 1];
        database.columns.pop_back();
        return;
    }

    ::rocksdb::WriteOptions write_options;
    write_options.sync = true;
    ::rocksdb::WriteBatch batch;
    std::unique_ptr<::rocksdb::Iterator> iterator {database.native->NewIterator(::rocksdb::ReadOptions(), column)};
    for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) {
        native_status_t const staged = arguments.mode == ustore_drop_vals_k
                                           ? batch.Put(column, iterator->key(), ::rocksdb::Slice())
                                           : batch.Delete(column, iterator->key());
        if (export_error(staged, arguments.error)) return;
    }
    native_status_t const status = database.native->Write(write_options, &batch);
    export_error(status, arguments.error);
}

void collection_list(ustore_collection_list_t& arguments) {
    if (!arguments.db) {
        *arguments.error = "DataBase is uninitialized";
        return;
    }
    if (!arguments.count || !arguments.names) {
        *arguments.error = "Need names and outputs!";
        return;
    }

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (*arguments.error) return;

    database_t& database = *reinterpret_cast<database_t*>(arguments.db);
    std::size_t const collections_count = database.columns.size() - 1;
    *arguments.count = static_cast<ustore_size_t>(collections_count);

    // Every name is null-terminated
    std::size_t strings_length = 0;
    for (column_t* const column : database.columns) strings_length += column->GetName().size() + 1;

    auto names = arena.alloc<char>(strings_length, arguments.error).begin();
    *arguments.names = names;
    if (*arguments.error) return;

    auto ids = arena.alloc_or_dummy(collections_count, arguments.error, arguments.ids);
    if (*arguments.error) return;
    auto offsets = arena.alloc_or_dummy(collections_count + 1, arguments.error, arguments.offsets);
    if (*arguments.error) return;

    std::size_t index = 0;
    for (column_t* const column : database.columns) {
        std::string const& name = column->GetName();
        if (name == ::rocksdb::kDefaultColumnFamilyName) continue;

        std::memcpy(names, name.data(), name.size());
        names[name.size()] = '\0';
        ids[index] = reinterpret_cast<ustore_collection_t>(column);
        offsets[index] = static_cast<ustore_length_t>(names - *arguments.names);
        names += name.size() + 1;
        ++index;
    }
    offsets[index] = static_cast<ustore_length_t>(names - *arguments.names);
}

void database_control(ustore_database_control_t& arguments) {
    *arguments.response = nullptr;
    *arguments.error = "Controls aren't supported in this implementation!";
}

#pragma endregion Collections

#pragma region Transactions

void transaction_init(ustore_transaction_init_t& arguments) {
    if (!arguments.db) {
        *arguments.error = "DataBase is uninitialized";
        return;
    }
    validate_transaction_begin(arguments.transaction, arguments.options, arguments.error);
    if (*arguments.error) return;

    bool const safe = arguments.options & ustore_option_write_flush_k;
    database_t& database = *reinterpret_cast<database_t*>(arguments.db);
    transaction_t* const reused = reinterpret_cast<transaction_t*>(*arguments.transaction);

    ::rocksdb::OptimisticTransactionOptions transaction_options;
    transaction_options.set_snapshot = false;
    ::rocksdb::WriteOptions write_options;
    write_options.sync = safe;
    write_options.disableWAL = !safe;

    transaction_t* const begun = database.native->BeginTransaction(write_options, transaction_options, reused);
    if (!begun) {
        *arguments.error = "Couldn't start a transaction!";
        return;
    }
    *arguments.transaction = begun;
}

void transaction_stage(ustore_transaction_stage_t& arguments) {
    if (!arguments.transaction) {
        *arguments.error = "Transaction is uninitialized";
        return;
    }
    // `OptimisticTransaction::Prepare` refuses two-phase commit, and conflicts are checked only inside `Commit`
    *arguments.error = "Staging isn't supported by RocksDB optimistic transactions";
}

void transaction_commit(ustore_transaction_commit_t& arguments) {
    if (!arguments.transaction) return;

    validate_transaction_commit(arguments.transaction, arguments.options, arguments.error);
    if (*arguments.error) return;

    database_t& database = *reinterpret_cast<database_t*>(arguments.db);
    transaction_t& transaction = *reinterpret_cast<transaction_t*>(arguments.transaction);

    // The sequence number is read under the mutex so it belongs to this commit
    std::unique_lock lock {database.mutex, std::defer_lock};
    if (arguments.sequence_number) lock.lock();
    native_status_t const status = transaction.Commit();
    if (export_error(status, arguments.error)) return;
    if (arguments.sequence_number) *arguments.sequence_number = database.native->GetLatestSequenceNumber();
}

#pragma endregion Transactions

} // namespace unum::ustore::rocksdb

using namespace unum::ustore;
using namespace unum::ustore::rocksdb;

#pragma region Exception Boundaries

void ustore_database_init(ustore_database_init_t* arguments) {
    try {
        database_init(*arguments);
    }
    catch (std::bad_alloc const&) {
        *arguments->error = "Out of memory";
    }
    catch (...) {
        *arguments->error = "Unexpected engine failure";
    }
}

void ustore_get_metadata(ustore_get_metadata_t* arguments) {
    try {
        get_metadata(*arguments);
    }
    catch (std::bad_alloc const&) {
        *arguments->error = "Out of memory";
    }
    catch (...) {
        *arguments->error = "Unexpected engine failure";
    }
}

void ustore_snapshot_list(ustore_snapshot_list_t* arguments) {
    try {
        snapshot_list(*arguments);
    }
    catch (std::bad_alloc const&) {
        *arguments->error = "Out of memory";
    }
    catch (...) {
        *arguments->error = "Unexpected engine failure";
    }
}

void ustore_snapshot_create(ustore_snapshot_create_t* arguments) {
    try {
        snapshot_create(*arguments);
    }
    catch (std::bad_alloc const&) {
        *arguments->error = "Out of memory";
    }
    catch (...) {
        *arguments->error = "Unexpected engine failure";
    }
}

void ustore_snapshot_export(ustore_snapshot_export_t* arguments) {
    try {
        snapshot_export(*arguments);
    }
    catch (std::bad_alloc const&) {
        *arguments->error = "Out of memory";
    }
    catch (...) {
        *arguments->error = "Unexpected engine failure";
    }
}

void ustore_snapshot_drop(ustore_snapshot_drop_t* arguments) {
    if (!arguments) return;
    try {
        snapshot_drop(*arguments);
    }
    catch (std::bad_alloc const&) {
        *arguments->error = "Out of memory";
    }
    catch (...) {
        *arguments->error = "Unexpected engine failure";
    }
}

void ustore_write(ustore_write_t* arguments) {
    try {
        write(*arguments);
    }
    catch (std::bad_alloc const&) {
        *arguments->error = "Out of memory";
    }
    catch (...) {
        *arguments->error = "Unexpected engine failure";
    }
}

void ustore_read(ustore_read_t* arguments) {
    try {
        read(*arguments);
    }
    catch (std::bad_alloc const&) {
        *arguments->error = "Out of memory";
    }
    catch (...) {
        *arguments->error = "Unexpected engine failure";
    }
}

void ustore_scan(ustore_scan_t* arguments) {
    try {
        scan(*arguments);
    }
    catch (std::bad_alloc const&) {
        *arguments->error = "Out of memory";
    }
    catch (...) {
        *arguments->error = "Unexpected engine failure";
    }
}

void ustore_sample(ustore_sample_t* arguments) {
    try {
        sample(*arguments);
    }
    catch (std::bad_alloc const&) {
        *arguments->error = "Out of memory";
    }
    catch (...) {
        *arguments->error = "Unexpected engine failure";
    }
}

void ustore_measure(ustore_measure_t* arguments) {
    try {
        measure(*arguments);
    }
    catch (std::bad_alloc const&) {
        *arguments->error = "Out of memory";
    }
    catch (...) {
        *arguments->error = "Unexpected engine failure";
    }
}

void ustore_collection_create(ustore_collection_create_t* arguments) {
    try {
        collection_create(*arguments);
    }
    catch (std::bad_alloc const&) {
        *arguments->error = "Out of memory";
    }
    catch (...) {
        *arguments->error = "Unexpected engine failure";
    }
}

void ustore_collection_drop(ustore_collection_drop_t* arguments) {
    try {
        collection_drop(*arguments);
    }
    catch (std::bad_alloc const&) {
        *arguments->error = "Out of memory";
    }
    catch (...) {
        *arguments->error = "Unexpected engine failure";
    }
}

void ustore_collection_list(ustore_collection_list_t* arguments) {
    try {
        collection_list(*arguments);
    }
    catch (std::bad_alloc const&) {
        *arguments->error = "Out of memory";
    }
    catch (...) {
        *arguments->error = "Unexpected engine failure";
    }
}

void ustore_database_control(ustore_database_control_t* arguments) {
    try {
        database_control(*arguments);
    }
    catch (std::bad_alloc const&) {
        *arguments->error = "Out of memory";
    }
    catch (...) {
        *arguments->error = "Unexpected engine failure";
    }
}

void ustore_transaction_init(ustore_transaction_init_t* arguments) {
    try {
        transaction_init(*arguments);
    }
    catch (std::bad_alloc const&) {
        *arguments->error = "Out of memory";
    }
    catch (...) {
        *arguments->error = "Unexpected engine failure";
    }
}

void ustore_transaction_stage(ustore_transaction_stage_t* arguments) {
    try {
        transaction_stage(*arguments);
    }
    catch (std::bad_alloc const&) {
        *arguments->error = "Out of memory";
    }
    catch (...) {
        *arguments->error = "Unexpected engine failure";
    }
}

void ustore_transaction_commit(ustore_transaction_commit_t* arguments) {
    try {
        transaction_commit(*arguments);
    }
    catch (std::bad_alloc const&) {
        *arguments->error = "Out of memory";
    }
    catch (...) {
        *arguments->error = "Unexpected engine failure";
    }
}

#pragma endregion Exception Boundaries

#pragma region Memory

void ustore_arena_free(ustore_arena_t arena) { clear_linked_memory(arena); }

void ustore_transaction_free(ustore_transaction_t transaction) { delete reinterpret_cast<transaction_t*>(transaction); }

void ustore_database_free(ustore_database_t database_handle) {
    if (!database_handle) return;
    database_t* const database = reinterpret_cast<database_t*>(database_handle);
    for (column_t* const column : database->columns) database->native->DestroyColumnFamilyHandle(column);
    database->native.reset();
    delete database;
}

void ustore_error_free(ustore_error_t const) {}

#pragma endregion Memory
