/**
 *  @file src_oracles/rocksdb/blobs.hpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief The blobs calls of the C ABI over the column families of `src_oracles/rocksdb/database.hpp`.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <limits>       // `std::numeric_limits`
#include <memory>       // `std::unique_ptr`
#include <new>          // `std::nothrow`
#include <shared_mutex> // `std::shared_lock`

#include <rocksdb/db.h>

#include "ustore/ranges.hpp" // `place_batch_t`, `strided`

#include "ustore/utilities/algorithm.hpp" // `reservoir_sample_iterator`

#include "database.hpp"           // `database_t`, `transaction_t`, `refuse_collections`, `get_many`, `write_many`
#include "utilities/batches.hpp"  // `strings_arg`, `validate_write`, `places_of`
#include "utilities/outputs.hpp"  // `scan_outputs_t`, `measure_outputs_t`
#include "utilities/progress.hpp" // `progress_t`, `progress_cancelled_k`

namespace unum::ustore::oracles::rocksdb {

#pragma region Reads and Writes

status_t blobs_write(ustore_blobs_write_t const& arguments, strings_arg_t const& values, view_t const& view) {
    database_t& database = *state_of<database_t>(arguments.database);
    transaction_t* const transaction = state_of<transaction_t>(view.transaction);
    place_batch_t const places = places_of(arguments);

    status_t const validated = validate_write(view.transaction, places, values, arguments.durability, view.watch);
    if (validated.failed() || !arguments.count) return validated;
    std::shared_lock lock {database.columns_mutex};
    status_t const refused = refuse_collections(database, places.collections_begin, arguments.count,
                                                entry_layout_t::uuids_to_bytes_k);
    if (refused.failed()) return refused;
    if (status_t const status = refuse_staged_write(transaction); status.failed()) return status;

    if (arguments.count == 1) return write_one(database, transaction, places, values, arguments.durability);
    return write_many(database, transaction, places, values, arguments.durability);
}

status_t blobs_remove(ustore_blobs_remove_t const& arguments, view_t const& view) {
    database_t& database = *state_of<database_t>(arguments.database);
    transaction_t* const transaction = state_of<transaction_t>(view.transaction);
    place_batch_t const places = places_of(arguments);

    status_t const validated = validate_remove(view.transaction, places, arguments.durability, view.watch);
    if (validated.failed() || !arguments.count) return validated;
    std::shared_lock lock {database.columns_mutex};
    status_t const refused = refuse_collections(database, places.collections_begin, arguments.count,
                                                entry_layout_t::uuids_to_bytes_k);
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

    database_t& database = *state_of<database_t>(arguments.database);
    place_batch_t const places = places_of(arguments);
    if (status_t const status = validate_read(places); status.failed()) return status;
    std::shared_lock lock {database.columns_mutex};
    status_t const refused = refuse_collections(database, places.collections_begin, arguments.count,
                                                entry_layout_t::uuids_to_bytes_k);
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
    database_t& database = *state_of<database_t>(arguments.database);
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    scan_batch_t const tasks {collections, strided(arguments.start_keys), arguments.wanted, arguments.count};

    if (status_t const status = validate_scan(arguments); status.failed() || !arguments.count) return status;
    std::shared_lock lock {database.columns_mutex};
    status_t const refused = refuse_collections(database, collections, arguments.count,
                                                entry_layout_t::uuids_to_bytes_k);
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

    database_t& database = *state_of<database_t>(arguments.database);
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    sample_batch_t const samples {collections, arguments.wanted, arguments.count};
    std::shared_lock lock {database.columns_mutex};
    status_t const refused = refuse_collections(database, collections, arguments.count,
                                                entry_layout_t::uuids_to_bytes_k);
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
    database_t& database = *state_of<database_t>(arguments.database);
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    std::shared_lock lock {database.columns_mutex};
    status_t const refused = refuse_collections(database, collections, arguments.count,
                                                entry_layout_t::uuids_to_bytes_k);
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
        column_t* const column = column_of(database, collections[task_index]);
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

} // namespace unum::ustore::oracles::rocksdb
