/**
 *  @file src/ram/blobs.hpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief The blobs calls of the C ABI over the stores of <tt>src/ram/database.hpp</tt>, for the
 *      in-memory engine's tables.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <type_traits> // `std::is_same_v`

#include "ustore/blobs.h"
#include "ustore/ranges.hpp" // `place_batch_t`, `strided`

#include "database.hpp"          // `database_state`, `read_values`, `remove_keys`, `scan_keys`, `measure_keys`
#include "utilities/batches.hpp" // `strings_arg_t`, `places_of`

namespace unum::ustore::ram {

#pragma region Blob Calls

/** Implements @c ustore_blobs_read, copying every value into the caller's arena once. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_blobs_read_t const& arguments,
               view_t const& view) noexcept {
    return read_values<value_pair_t>(state, arguments, view);
}

/** Writes @p content under @p key at the newest stamp, straight into the store of the collection @p collections
 *  names first. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
store_status_t write_one(database_state<cpu_class_, isolation_>& state,
                         strided_iterator<ustore_collection_id_t const> collections, ustore_key_t const& key,
                         value_view_t content, status_t& outcome) noexcept {
    claim_t claim;
    reader_source<cpu_class_, isolation_> source {state, state.order.take_snapshot(claim)};
    return for_each_run(source, collections, 1, outcome,
                        [&](auto& entries, std::size_t, std::size_t) noexcept -> store_status_t {
                            using entry_t = entry_of<decltype(entries)>;
                            if constexpr (!std::is_same_v<entry_t, value_pair_t>) return refused_modality(outcome);
                            else return stage_upsert<value_pair_t>(entries, key, content);
                        });
}

/** Implements @c ustore_blobs_write, committing a batch outside a transaction atomically. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_blobs_write_t const& arguments,
               strings_arg_t const& values, view_t const& view) noexcept {
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
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_blobs_remove_t const& arguments,
               view_t const& view) noexcept {
    return remove_keys<value_pair_t>(state, arguments, view);
}

/** Implements @c ustore_blobs_scan as ordered walks of each collection's keys. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_blobs_scan_t const& arguments,
               view_t const& view) noexcept {
    return scan_keys<value_pair_t>(state, arguments, view);
}

/** Implements @c ustore_blobs_sample as a reservoir filled from a walk of each collection's keys. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_blobs_sample_t const& arguments,
               view_t const& view) noexcept {
    return sample_keys<value_pair_t>(state, arguments, view);
}

/** Implements @c ustore_blobs_measure, exact for entries and value bytes at the view it reads. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_blobs_measure_t const& arguments,
               view_t const& view) noexcept {
    return measure_keys<value_pair_t>(state, arguments, view);
}

#pragma endregion Blob Calls

#pragma region Table

template <cpu_class_t cpu_class_, isolation_t isolation_>
blobs_verbs<home_t::host_k> const ram_variant<cpu_class_, isolation_>::blobs_verbs_k {
    .write = &guarded_by<ustore_modality_blobs_k, &resolve_modality<cpu_class_, isolation_>,
                         &serve_database<cpu_class_, isolation_, ustore_blobs_write_t, strings_arg_t>>,
    .remove = &guarded_by<ustore_modality_blobs_k, &resolve_modality<cpu_class_, isolation_>,
                          &serve_database<cpu_class_, isolation_, ustore_blobs_remove_t>>,
    .read = &guarded_by<ustore_modality_blobs_k, &resolve_modality<cpu_class_, isolation_>,
                        &serve_database<cpu_class_, isolation_, ustore_blobs_read_t>>,
    .scan = &guarded_by<ustore_modality_blobs_k, &resolve_modality<cpu_class_, isolation_>,
                        &serve_database<cpu_class_, isolation_, ustore_blobs_scan_t>>,
    .sample = &guarded_by<ustore_modality_blobs_k, &resolve_modality<cpu_class_, isolation_>,
                          &serve_database<cpu_class_, isolation_, ustore_blobs_sample_t>>,
    .measure = &guarded_by<ustore_modality_blobs_k, &resolve_modality<cpu_class_, isolation_>,
                           &serve_database<cpu_class_, isolation_, ustore_blobs_measure_t>>,
};

#pragma endregion Table

} // namespace unum::ustore::ram
