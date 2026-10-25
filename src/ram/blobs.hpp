/**
 *  @file src/ram/blobs.hpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief The blobs calls of the C ABI over the stores of `src/ram/database.hpp`, for the in-memory engine's tables.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <limits>      // `std::numeric_limits`
#include <type_traits> // `std::is_same_v`

#include "ustore/blobs.h"
#include "ustore/ranges.hpp" // `place_batch_t`, `strided`

#include "database.hpp"          // `database_state`, `read_values`, `remove_keys`, `scan_keys`, `sample_keys`
#include "utilities/batches.hpp" // `strings_arg_t`, `places_of`
#include "utilities/outputs.hpp" // `measure_outputs_t`

namespace unum::ustore::ram {

#pragma region Blob Calls

/** Implements @c ustore_blobs_read, copying every value into the caller's arena once. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_blobs_read_t const& arguments, view_t const& view) noexcept {
    return read_values<value_pair_t>(state, arguments, view);
}

/** Writes @p content under @p key at the newest stamp, straight into the store of the collection @p collections
 *  names first. */
template <isolation_t isolation_>
store_status_t write_one(database_state<isolation_>& state, strided_iterator<ustore_collection_id_t const> collections,
                         ustore_key_t const& key, value_view_t content, status_t& outcome) noexcept {
    claim_t claim;
    reader_source<isolation_> source {state, state.order.take_snapshot(claim)};
    return for_each_run(source, collections, 1, outcome,
                        [&](auto& entries, std::size_t, std::size_t) noexcept -> store_status_t {
                            using entry_t = entry_of<decltype(entries)>;
                            if constexpr (!std::is_same_v<entry_t, value_pair_t>) return refused_modality(outcome);
                            else return stage_upsert<value_pair_t>(entries, key, content);
                        });
}

/** Implements @c ustore_blobs_write, committing a batch outside a transaction atomically. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_blobs_write_t const& arguments, strings_arg_t const& values,
               view_t const& view) noexcept {
    place_batch_t const places = places_of(arguments);
    if (status_t const status = validate_write(view.transaction, places, values, arguments.durability, view.watch);
        status.failed() || !arguments.count)
        return status;
    if (view.transaction || places.size() != 1)
        return serve_write(state, arguments.durability, view, [&](auto& source, status_t& outcome) noexcept {
            return stage_runs_of<value_pair_t>(source, places.collections_begin, places.size(), outcome,
                                               [&](auto& part, std::size_t first, std::size_t end) noexcept {
                                                   return stage_writes<value_pair_t>(part, places, values, first, end);
                                               });
        });
    status_t outcome;
    store_status_t const written = write_one(state, places.collections_begin, places[0].key, values[0], outcome);
    if (status_t const status = reported(written, outcome); status.failed()) return status;
    if (arguments.durability == ustore_durability_flushed_k) return persist(state);
    return {};
}

/** Implements @c ustore_blobs_remove, committing a batch outside a transaction atomically, where a key already gone
 *  stays gone. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_blobs_remove_t const& arguments, view_t const& view) noexcept {
    return remove_keys<value_pair_t>(state, arguments, view);
}

/** Implements @c ustore_blobs_scan as ordered walks of each collection's keys. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_blobs_scan_t const& arguments, view_t const& view) noexcept {
    return scan_keys<value_pair_t>(state, arguments, view);
}

/** Implements @c ustore_blobs_sample as a reservoir filled from a walk of each collection's keys. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_blobs_sample_t const& arguments, view_t const& view) noexcept {
    return sample_keys<value_pair_t>(state, arguments, view);
}

/** Records the exact measure of @p task into @p outputs. */
inline void assign_measure(measure_outputs_t& outputs, std::size_t task, window_measure_t measured) noexcept {
    outputs.min_cardinalities[task] = measured.cardinality;
    outputs.max_cardinalities[task] = measured.cardinality;
    outputs.min_value_bytes[task] = measured.value_bytes;
    outputs.max_value_bytes[task] = measured.value_bytes;
    outputs.min_space_usages[task] = measured.space_bytes;
    outputs.max_space_usages[task] = std::numeric_limits<ustore_size_t>::max();
}

/** Implements @c ustore_blobs_measure, exact for entries and value bytes at the view it reads. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_blobs_measure_t const& arguments,
               view_t const& view) noexcept {
    if (!arguments.count) return {};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    strided_iterator<ustore_key_t const> const start_keys = strided(arguments.start_keys);
    strided_iterator<ustore_key_t const> const end_keys = strided(arguments.end_keys);
    auto [outputs, outputs_status] = measure_outputs_t::make(arguments);
    if (outputs_status.failed()) return outputs_status;

    status_t outcome;
    store_status_t const walked = with_source(state, view, [&](auto& source) noexcept {
        return for_each_run(source, collections, arguments.count, outcome,
                            [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
                                using entry_t = entry_of<decltype(entries)>;
                                if constexpr (!std::is_same_v<entry_t, value_pair_t>) return refused_modality(outcome);
                                else {
                                    auto store = source.view(entries);
                                    if (!store) return store.status();
                                    for (std::size_t task = first; task != end; ++task) {
                                        window_measure_t measured;
                                        ustore_key_t const lower = start_keys ? start_keys[task] : smallest_key_k;
                                        ustore_key_t const last = end_keys ? end_keys[task] : largest_key_k;
                                        store_status_t const measuring = measure_window<entry_t>(*store, lower, last,
                                                                                                 measured);
                                        if (st::failed(measuring)) return measuring;
                                        assign_measure(outputs, task, measured);
                                    }
                                    return store_status_t::success_k;
                                }
                            });
    });
    if (status_t const status = reported(walked, outcome); status.failed()) return status;
    outputs.finish(arguments);
    return {};
}

#pragma endregion Blob Calls

} // namespace unum::ustore::ram
