/**
 *  @file src_oracles/leveldb/blobs.hpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief The blobs calls of the C ABI over the key ranges of `src_oracles/leveldb/database.hpp`.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#include <leveldb/db.h>
#include <leveldb/write_batch.h>

#include "ustore/ranges.hpp" // `place_batch_t`, `strided`

#include "ustore/utilities/algorithm.hpp" // `reservoir_sample_iterator`

#include "database.hpp"           // `database_t`, `resolve_collections`, `collection_walk_t`, `get_each`, `write_many`
#include "utilities/batches.hpp"  // `strings_arg_t`, `validate_write`, `places_of`
#include "utilities/outputs.hpp"  // `read_outputs_t`, `scan_outputs_t`, `measure_outputs_t`
#include "utilities/progress.hpp" // `progress_t`, `progress_cancelled_k`

namespace unum::ustore::oracles::leveldb {

#pragma region Reads and Writes

status_t blobs_write(ustore_blobs_write_t const& arguments, strings_arg_t const& values, view_t const& view) {
    database_t& database = *handle_cast<database_t>(arguments.database);
    place_batch_t const places = places_of(arguments);

    status_t const validated = validate_write(view.transaction, places, values, arguments.durability, view.watch);
    if (validated.failed() || !arguments.count) return validated;
    status_t const resolved = resolve_collections(database, places.collections_begin, arguments.count,
                                                  entry_layout_t::uuids_to_bytes_k);
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
                                                  entry_layout_t::uuids_to_bytes_k);
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
                                                  entry_layout_t::uuids_to_bytes_k);
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
    status_t const resolved = resolve_collections(database, collections, arguments.count,
                                                  entry_layout_t::uuids_to_bytes_k);
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
    status_t const resolved = resolve_collections(database, collections, arguments.count,
                                                  entry_layout_t::uuids_to_bytes_k);
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
    status_t const resolved = resolve_collections(database, collections, arguments.count,
                                                  entry_layout_t::uuids_to_bytes_k);
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

        ustore_collection_id_t const collection = collections[task_index];
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

} // namespace unum::ustore::oracles::leveldb
