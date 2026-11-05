/**
 *  @file src_oracles/rocksdb/blobs.hpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief The blobs calls of the C ABI over the column families of `src_oracles/rocksdb/database.hpp`.
 */

#pragma once

#include <cstddef> // `std::size_t`, `std::ptrdiff_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <memory>       // `std::unique_ptr`
#include <mutex>        // `std::unique_lock`
#include <new>          // `std::nothrow`
#include <shared_mutex> // `std::shared_lock`

#include <rocksdb/db.h>

#include "ustore/ranges.hpp" // `place_batch_t`, `strided`

#include "ustore/utilities/algorithm.hpp" // `reservoir_sample_iterator`

#include "database.hpp"          // `database_t`, `transaction_t`, `seek_towards`, `buffer_remove`, `get_many`
#include "utilities/batches.hpp" // `validate_write`, `places_of`, `scans_of`, `measures_of`
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

/** Removes every key from each start of @p places up to its end in @p end_keys, exclusive, key by
 *  key in one batch at the head, holding @c columns_mutex exclusively so no write lands in a range
 *  between its listing and its removal. */
status_t remove_ranges_at_head(database_t& database, place_batch_t const& places,
                               strided_iterator<ustore_key_t const> end_keys, ustore_durability_t durability) {
    std::unique_lock lock {database.columns_mutex};
    status_t const refused = refuse_collections(database, places.collections_begin, places.count,
                                                entry_layout_t::uuids_to_bytes_k);
    if (refused.failed()) return refused;

    ::rocksdb::WriteBatch batch;
    ::rocksdb::ReadOptions const read_options;
    column_t* iterated_column = nullptr;
    std::unique_ptr<::rocksdb::Iterator> iterator;
    for (std::size_t task_index = 0; task_index != places.count; ++task_index) {
        place_t const place = places[task_index];
        ::rocksdb::Slice const end = to_slice(end_keys[static_cast<std::ptrdiff_t>(task_index)]);
        column_t* const column = column_of(database, place.collection);
        if (!iterator || column != iterated_column) {
            iterator.reset(database.native->NewIterator(read_options, column));
            if (!iterator) return {status_code_t::out_of_memory_k, "Failed to create a RocksDB iterator"};
            iterated_column = column;
        }
        for (iterator->Seek(to_slice(place.key)); iterator->Valid() && iterator->key().compare(end) < 0;
             iterator->Next())
            if (native_status_t const status = batch.Delete(column, iterator->key()); !status.ok())
                return status_of(status);
        if (!iterator->status().ok()) return status_of(iterator->status());
    }
    if (!batch.Count()) return {};
    return status_of(database.native->Write(write_options_for(durability), &batch));
}

/** Removes every key from each start of @p places up to its end in @p end_keys, exclusive, through
 *  @p transaction key by key. The caller holds @c columns_mutex. */
status_t remove_ranges_in(database_t& database, transaction_t& transaction, place_batch_t const& places,
                          strided_iterator<ustore_key_t const> end_keys, view_t const& view) {
    ::rocksdb::ReadOptions read_options;
    read_options.snapshot = snapshot_of(transaction);
    st::vector<ustore_key_t, arena_allocator<ustore_key_t>> listed {{view.scratch}};
    for (std::size_t task_index = 0; task_index != places.count; ++task_index) {
        place_t const place = places[task_index];
        ::rocksdb::Slice const end = to_slice(end_keys[static_cast<std::ptrdiff_t>(task_index)]);
        column_t* const column = column_of(database, place.collection);

        // Changing the batch under its own iterator invalidates it, so the range is listed first
        listed.clear();
        std::unique_ptr<::rocksdb::Iterator> iterator {new_iterator(database, &transaction, read_options, column)};
        if (!iterator) return {status_code_t::out_of_memory_k, "Failed to create a RocksDB iterator"};
        for (iterator->Seek(to_slice(place.key)); iterator->Valid() && iterator->key().compare(end) < 0;
             iterator->Next()) {
            ustore_key_t key;
            std::memcpy(&key, iterator->key().data(), sizeof(ustore_key_t));
            if (status_t const status = growth_status(listed.emplace_back(key)); status.failed()) return status;
        }
        if (!iterator->status().ok()) return status_of(iterator->status());
        for (ustore_key_t const& key : listed) {
            native_status_t const status = buffer_remove(transaction, column, place.collection, to_slice(key));
            if (!status.ok()) return status_of(status);
        }
    }
    return {};
}

status_t blobs_remove(ustore_blobs_remove_t const& arguments, view_t const& view) {
    database_t& database = *state_of<database_t>(arguments.database);
    transaction_t* const transaction = state_of<transaction_t>(view.transaction);
    place_batch_t const places = places_of(arguments);

    status_t const validated = validate_remove(view.transaction, places, arguments.durability, view.watch);
    if (validated.failed() || !arguments.count) return validated;
    strided_iterator<ustore_key_t const> const end_keys = strided(arguments.end_keys);
    if (end_keys && !transaction) return remove_ranges_at_head(database, places, end_keys, arguments.durability);
    std::shared_lock lock {database.columns_mutex};
    status_t const refused = refuse_collections(database, places.collections_begin, arguments.count,
                                                entry_layout_t::uuids_to_bytes_k);
    if (refused.failed()) return refused;
    if (status_t const status = refuse_staged_write(transaction); status.failed()) return status;
    if (end_keys) return remove_ranges_in(database, *transaction, places, end_keys, view);

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
    scan_batch_t const tasks = scans_of(arguments);

    if (status_t const status = validate_scan(arguments); status.failed() || !arguments.count) return status;
    std::shared_lock lock {database.columns_mutex};
    status_t const refused = refuse_collections(database, collections, arguments.count,
                                                entry_layout_t::uuids_to_bytes_k);
    if (refused.failed()) return refused;

    ::rocksdb::ReadOptions read_options;
    read_options.fill_cache = false;
    transaction_t* const transaction = reading_transaction(view, read_options);

    // Consecutive tasks over one collection share an iterator, since a seek repositions it anywhere
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
        for (seek_towards(*iterator, to_slice(task.start_key()), task.direction);
             iterator->Valid() && outputs.size() != scan_end; step_towards(*iterator, task.direction)) {
            ustore_key_t key;
            std::memcpy(&key, iterator->key().data(), sizeof(ustore_key_t));
            if (!task.keys.contains(key)) break;
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

/** Measures every range by walking its keys at the view it reads, exactly, with the space RocksDB
 *  estimates the range takes in its files and memory. */
status_t blobs_measure(ustore_blobs_measure_t const& arguments, view_t const& view) {
    if (status_t const status = validate_direction(arguments.direction); status.failed() || !arguments.count)
        return status;
    database_t& database = *state_of<database_t>(arguments.database);
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    scan_batch_t const tasks = measures_of(arguments);
    std::shared_lock lock {database.columns_mutex};
    status_t const refused = refuse_collections(database, collections, arguments.count,
                                                entry_layout_t::uuids_to_bytes_k);
    if (refused.failed()) return refused;

    ::rocksdb::ReadOptions read_options;
    read_options.fill_cache = false;
    transaction_t* const transaction = reading_transaction(view, read_options);
    auto [outputs, outputs_status] = measure_outputs_t::make(arguments);
    if (outputs_status.failed()) return outputs_status;
    ::rocksdb::SizeApproximationOptions size_options;
    size_options.include_memtables = true;

    column_t* iterated_column = nullptr;
    std::unique_ptr<::rocksdb::Iterator> iterator;
    for (std::size_t task_index = 0; task_index != tasks.count; ++task_index) {
        scan_task_t const task = tasks[task_index];
        column_t* const column = column_of(database, task.collection);
        if (!iterator || column != iterated_column) {
            iterator.reset(new_iterator(database, transaction, read_options, column));
            if (!iterator) return {status_code_t::out_of_memory_k, "Failed to create a RocksDB iterator"};
            iterated_column = column;
        }

        ustore_size_t cardinality = 0;
        ustore_size_t value_bytes = 0;
        std::uint64_t approximate_size = 0;
        if (task.limit) {
            for (iterator->Seek(to_slice(task.keys.lower)); iterator->Valid(); iterator->Next()) {
                ustore_key_t key;
                std::memcpy(&key, iterator->key().data(), sizeof(ustore_key_t));
                if (task.keys.upper < key) break;
                ++cardinality;
                value_bytes += iterator->value().size();
            }
            if (!iterator->status().ok()) return status_of(iterator->status());

            // The least key past the upper bound adds a zero byte, closing the range RocksDB sizes
            char past_upper[sizeof(ustore_key_t) + 1] {};
            std::memcpy(past_upper, &task.keys.upper, sizeof(ustore_key_t));
            ::rocksdb::Range const range(to_slice(task.keys.lower), {past_upper, sizeof(past_upper)});
            native_status_t const status = database.native->GetApproximateSizes(size_options, column, &range, 1,
                                                                                &approximate_size);
            if (!status.ok()) return status_of(status);
        }
        outputs.assign(task_index, cardinality, value_bytes, approximate_size);
    }
    outputs.finish(arguments);
    return {};
}

#pragma endregion Reads and Writes

} // namespace unum::ustore::oracles::rocksdb
