/**
 *  @file src/nvme/blobs.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The blobs calls of the C ABI over the stores of <tt>src/nvme/database.hpp</tt>, for the NVMe engine's table.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <span> // `std::span`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/blobs.h"
#include "ustore/ranges.hpp" // `place_batch_t`, `strided`

#include "ustore/utilities/ram_buffers.hpp" // `arena_allocator`, `growth_status`

#include "database.hpp"           // `database_state`, `call_access`, `resolve_runs`, `serve_write`, `walk`
#include "utilities/batches.hpp"  // `places_of`, `validate_write`, `samples_of`, `sample_generator_t`
#include "utilities/outputs.hpp"  // `read_outputs_t`, `scan_outputs_t`, `measure_outputs_t`
#include "utilities/progress.hpp" // `progress_t`, `progress_cancelled_k`

namespace unum::ustore::nvme {

#pragma region Blob Calls

/** Implements @c ustore_blobs_write, committing a batch outside a transaction atomically. */
template <cpu_class_t cpu_class_>
status_t blobs_write(ustore_blobs_write_t const& arguments, strings_arg_t const& values, view_t const& view) noexcept {
    place_batch_t const places = places_of(arguments);
    if (status_t const status = validate_write(view.transaction, places, values, arguments.durability, view.watch);
        status.failed() || !arguments.count)
        return status;
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    if (status_t const resolved = resolve_runs(state, view, places.collections_begin, places.size(),
                                               ustore_modality_blobs_k);
        resolved.failed())
        return resolved;
    std::size_t bytes = 0;
    for (std::size_t index = 0; index != places.size(); ++index) bytes += values[index].length;
    return serve_write(state, view, arguments.durability, places.size(), bytes, watch_schedule_t::never_k,
                       [&](call_access<cpu_class_> const& access) noexcept -> status_t {
                           for (std::size_t index = 0; index != places.size(); ++index) {
                               place_t const place = places[index];
                               status_t const written = write_value(access, place.collection, stored_of(place.key.uuid),
                                                                    value_view_t {values[index]});
                               if (written.failed()) return written;
                           }
                           return {};
                       });
}

/** Removes whole values through the store of each collection, committing a batch outside a transaction atomically,
 *  for a caller whose resolver checked the collections. */
template <cpu_class_t cpu_class_>
status_t remove_values(ustore_blobs_remove_t const& arguments, view_t const& view) noexcept {
    place_batch_t const places = places_of(arguments);
    if (status_t const status = validate_remove(view.transaction, places, arguments.durability, view.watch);
        status.failed() || !arguments.count)
        return status;
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    return serve_write(state, view, arguments.durability, places.size(), 1, watch_schedule_t::never_k,
                       [&](call_access<cpu_class_> const& access) noexcept -> status_t {
                           for (std::size_t index = 0; index != places.size(); ++index) {
                               place_t const place = places[index];
                               status_t const removed = write_value(access, place.collection, stored_of(place.key.uuid),
                                                                    value_view_t {});
                               if (removed.failed()) return removed;
                           }
                           return {};
                       });
}

/** Implements @c ustore_blobs_remove over the values of blobs collections alone. */
template <cpu_class_t cpu_class_>
status_t blobs_remove(ustore_blobs_remove_t const& arguments, view_t const& view) noexcept {
    if (status_t const resolved = resolve_modality<cpu_class_>(arguments.database, arguments.collections,
                                                               arguments.count, ustore_modality_blobs_k, view);
        resolved.failed())
        return resolved;
    return remove_values<cpu_class_>(arguments, view);
}

/** Reads whole values through the store of each collection, folded as its modality folds them,
 *  copying each value into the scratch once and into the arena at the end, for a caller whose
 *  resolver checked the collections; outside a transaction, every run of keys of one collection is
 *  one batched read of its store. */
template <cpu_class_t cpu_class_>
status_t read_values(ustore_blobs_read_t const& arguments, view_t const& view) noexcept {
    if (!arguments.count) return {};
    place_batch_t const places = places_of(arguments);
    if (status_t const status = validate_read(places); status.failed()) return status;
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    auto [outputs, outputs_status] = read_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    call_access<cpu_class_> const access = access_of(state, view);
    if (!access.transaction) {
        auto [keys, keys_status] = arena_allocate<stored_key_t>(view.scratch, places.size());
        if (keys_status.failed()) return keys_status;
        auto [values, values_status] = arena_allocate<value_view_t>(view.scratch, places.size());
        if (values_status.failed()) return values_status;
        for (std::size_t index = 0; index != places.size(); ++index) keys[index] = stored_of(places[index].key.uuid);
        for (std::size_t first = 0; first != places.size();) {
            collection_id_t const collection = places[first].collection;
            std::size_t last = first + 1;
            while (last != places.size() && places[last].collection == collection) ++last;
            if (status_t const read = read_values_batched(access, collection, keys.subspan(first, last - first),
                                                          values.subspan(first, last - first));
                read.failed())
                return read;
            first = last;
        }
        for (value_view_t const& value : values)
            if (value) outputs.append(value);
            else outputs.append_missing();
        return outputs.finish(arguments);
    }
    for (std::size_t index = 0; index != places.size(); ++index) {
        place_t const place = places[index];
        auto [value, read_status] = read_value(access, place.collection, stored_of(place.key.uuid));
        if (read_status.failed()) return read_status;
        if (value) outputs.append(value);
        else outputs.append_missing();
    }
    return outputs.finish(arguments);
}

/** Implements @c ustore_blobs_read over the values of blobs collections alone. */
template <cpu_class_t cpu_class_>
status_t blobs_read(ustore_blobs_read_t const& arguments, view_t const& view) noexcept {
    if (status_t const resolved = resolve_modality<cpu_class_>(arguments.database, arguments.collections,
                                                               arguments.count, ustore_modality_blobs_k, view);
        resolved.failed())
        return resolved;
    return read_values<cpu_class_>(arguments, view);
}

/** Scans keys as ordered walks of the store of each collection, or run by run for an unordered scan outside a
 *  transaction, a graph's being its vertices, watching what a transaction hands out, for a caller whose resolver
 *  checked the collections. */
template <cpu_class_t cpu_class_>
status_t scan_values(ustore_blobs_scan_t const& arguments, view_t const& view) noexcept {
    if (status_t const status = validate_scan(arguments); status.failed() || !arguments.count) return status;
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    scan_batch_t const scans {collections, strided(arguments.start_keys), arguments.wanted, arguments.count};
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    auto [outputs, outputs_status] = scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    progress_t progress {arguments.progress, arguments.progress_state, arguments.count};
    call_access<cpu_class_> const access = access_of(state, view);
    st::vector<stored_key_t, arena_allocator<stored_key_t>> found {{view.scratch}};
    for (std::size_t task = 0; task != scans.size(); ++task) {
        scan_task_t const scan = scans[task];
        outputs.open_task(task);
        found.clear();
        stored_key_t const from = stored_of(scan.start_key.uuid);
        // An unordered scan walks each run alone outside a transaction, whose own deltas merge only in order
        status_t collected;
        if (scan.limit != 0 && arguments.mode == ustore_scan_unordered_k && !access.transaction)
            collected = collect_keys_unordered(access, scan.collection, from, scan.limit, found);
        if (scan.limit != 0 && (arguments.mode != ustore_scan_unordered_k || access.transaction ||
                                collected == status_code_t::contended_k)) {
            found.clear();
            collected = collect_keys(access, scan.collection, from, scan.limit, found);
        }
        if (collected.failed()) return collected;
        for (std::size_t index = 0; index != found.size(); ++index)
            if (status_t const pushed = outputs.push(key_of(found[index])); pushed.failed()) return pushed;
        if (status_t const watched = watch_keys(access, scan.collection, std::span<stored_key_t const> {found});
            watched.failed())
            return watched;
        // A full page covered the keys up to its last, and a short one every key up to the largest
        stored_key_t const upper = found.size() == scan.limit && !found.empty() ? found[found.size() - 1]
                                                                                : limit<stored_key_t>::max();
        if (scan.limit != 0)
            if (status_t const watched = watch_range(access, scan.collection, stored_of(scan.start_key.uuid), upper);
                watched.failed())
                return watched;
        if (!progress.advance(1)) return progress_cancelled_k;
    }
    return outputs.finish(arguments);
}

/** Implements @c ustore_blobs_scan over the keys of blobs collections alone. */
template <cpu_class_t cpu_class_>
status_t blobs_scan(ustore_blobs_scan_t const& arguments, view_t const& view) noexcept {
    if (status_t const resolved = resolve_modality<cpu_class_>(arguments.database, arguments.collections,
                                                               arguments.count, ustore_modality_blobs_k, view);
        resolved.failed())
        return resolved;
    return scan_values<cpu_class_>(arguments, view);
}

/** Samples keys as a reservoir filled from a walk of the store of each collection, for a caller whose resolver
 *  checked the collections. */
template <cpu_class_t cpu_class_>
status_t sample_values(ustore_blobs_sample_t const& arguments, view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!arguments.offsets || !arguments.keys)
        return {status_code_t::invalid_argument_k, "Need outputs for the offsets and the keys"};
    sample_batch_t const samples = samples_of(arguments);
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    auto [outputs, outputs_status] = scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    sample_generator_t generator = sample_generator_t::seeded(&outputs);
    call_access<cpu_class_> const access = access_of(state, view);
    st::vector<stored_key_t, arena_allocator<stored_key_t>> kept {{view.scratch}};
    for (std::size_t task = 0; task != samples.size(); ++task) {
        sample_task_t const sample = samples[task];
        outputs.open_task(task);
        kept.clear();
        std::size_t seen = 0;
        status_t room;
        if (sample.limit != 0)
            if (status_t const walked = walk(access, sample.collection, limit<stored_key_t>::min(),
                                             [&](stored_key_t key, stored_value_size_t) noexcept {
                                                 std::size_t const slot = seen < sample.limit
                                                                              ? seen
                                                                              : st::draw_below(generator, seen + 1);
                                                 ++seen;
                                                 if (slot >= sample.limit) return true;
                                                 if (slot != kept.size()) {
                                                     kept[slot] = key;
                                                     return true;
                                                 }
                                                 room = growth_status(kept.push_back(stored_key_t {key}));
                                                 return room.succeeded();
                                             });
                walked.failed() || room.failed())
                return walked.failed() ? walked : room;
        for (std::size_t index = 0; index != kept.size(); ++index)
            if (status_t const pushed = outputs.push(key_of(kept[index])); pushed.failed()) return pushed;
        if (status_t const watched = watch_keys(access, sample.collection, std::span<stored_key_t const> {kept});
            watched.failed())
            return watched;
    }
    return outputs.finish(arguments);
}

/** Implements @c ustore_blobs_sample over the collections holding blobs. */
template <cpu_class_t cpu_class_>
status_t blobs_sample(ustore_blobs_sample_t const& arguments, view_t const& view) noexcept {
    if (status_t const resolved = resolve_modality<cpu_class_>(arguments.database, arguments.collections,
                                                               arguments.count, ustore_modality_blobs_k, view);
        resolved.failed())
        return resolved;
    return sample_values<cpu_class_>(arguments, view);
}

/** Implements @c ustore_blobs_measure, exact for keys and value bytes at the view it reads. */
template <cpu_class_t cpu_class_>
status_t blobs_measure(ustore_blobs_measure_t const& arguments, view_t const& view) noexcept {
    if (!arguments.count) return {};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    strided_iterator<ustore_key_t const> const start_keys = strided(arguments.start_keys);
    strided_iterator<ustore_key_t const> const end_keys = strided(arguments.end_keys);
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    if (status_t const resolved = resolve_runs(state, view, collections, arguments.count, ustore_modality_blobs_k);
        resolved.failed())
        return resolved;
    auto [outputs, outputs_status] = measure_outputs_t::make(arguments);
    if (outputs_status.failed()) return outputs_status;
    call_access<cpu_class_> const access = access_of(state, view);
    for (std::size_t task = 0; task != arguments.count; ++task) {
        stored_key_t const lower = stored_of((start_keys ? start_keys[task] : smallest_key_k).uuid);
        stored_key_t const last = stored_of((end_keys ? end_keys[task] : largest_key_k).uuid);
        std::size_t cardinality = 0;
        std::size_t value_bytes = 0;
        status_t const walked = walk(access, collections[task], lower,
                                     [&](stored_key_t key, stored_value_size_t size) noexcept {
                                         if (last < key) return false;
                                         ++cardinality;
                                         value_bytes += size;
                                         return true;
                                     });
        if (walked.failed()) return walked;
        if (status_t const watched = watch_range(access, collections[task], lower, last); watched.failed())
            return watched;
        outputs.assign(task, cardinality, value_bytes, value_bytes + cardinality * sizeof(stored_key_t));
    }
    outputs.finish(arguments);
    return {};
}

#pragma endregion Blob Calls

} // namespace unum::ustore::nvme
