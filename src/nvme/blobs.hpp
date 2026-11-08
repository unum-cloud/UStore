/**
 *  @file src/nvme/blobs.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The blobs calls of the C ABI over the stores of <tt>src/nvme/database.hpp</tt>, for the NVMe engine's table.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <algorithm> // `std::max`, `std::min`
#include <span>      // `std::span`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/blobs.h"
#include "ustore/ranges.hpp" // `place_batch_t`, `strided`

#include "ustore/utilities/ram_buffers.hpp" // `arena_allocator`, `growth_status`

#include "database.hpp"           // `database_state`, `call_access`, `resolve_runs`, `serve_write`, `walk`
#include "utilities/batches.hpp"  // `places_of`, `validate_write`, `samples_of`, `sample_generator_t`
#include "utilities/outputs.hpp"  // `read_outputs_t`, `task_window_t`, `scan_outputs_t`, `measure_outputs_t`
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

/** Removes whole values through the store of each collection, or every key of each range as one
 *  range tombstone when the call names end keys, committing a batch outside a transaction
 *  atomically, for a caller whose resolver checked the collections. */
template <cpu_class_t cpu_class_>
status_t remove_values(ustore_blobs_remove_t const& arguments, view_t const& view) noexcept {
    place_batch_t const places = places_of(arguments);
    if (status_t const status = validate_remove(view.transaction, places, arguments.durability, view.watch);
        status.failed() || !arguments.count)
        return status;
    strided_iterator<ustore_key_t const> const end_keys = strided(arguments.end_keys);
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    return serve_write(state, view, arguments.durability, places.size(), 1, watch_schedule_t::never_k,
                       [&](call_access<cpu_class_> const& access) noexcept -> status_t {
                           for (std::size_t index = 0; index != places.size(); ++index) {
                               place_t const place = places[index];
                               stored_key_t const key = stored_of(place.key.uuid);
                               status_t const removed =
                                   end_keys
                                       ? remove_key_range(access, place.collection, key,
                                                          stored_of(end_keys[static_cast<std::ptrdiff_t>(index)].uuid))
                                       : write_value(access, place.collection, key, value_view_t {});
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

/**
 *  @brief Reads the windows @p outputs asks of the values at @p places at the stamp of @p access,
 *      outside a transaction, straight into the arena @p outputs exports, as one batched read of
 *      every run of keys of one collection, a missing key reading as a missing value.
 *
 *  Sizes and bytes come from the one stamp the call pinned, so every window has its room in one
 *  block of the arena, and a read of no values reads only the sizes.
 */
template <cpu_class_t cpu_class_>
status_t read_values_batched(call_access<cpu_class_> const& access, ustore_blobs_read_t const& arguments,
                             place_batch_t const& places, read_outputs_t& outputs) noexcept {
    basic_database_of<cpu_class_>& store = *access.state.store;
    std::size_t const count = places.size();
    auto [keys, keys_status] = arena_allocate<stored_key_t>(access.scratch, count);
    if (keys_status.failed()) return keys_status;
    auto [windows, windows_status] = arena_allocate<task_window_t>(access.scratch, count);
    if (windows_status.failed()) return windows_status;
    auto [present, present_status] = arena_allocate<bool>(access.scratch, count);
    if (present_status.failed()) return present_status;
    std::size_t total = 0;
    for (std::size_t index = 0; index != count; ++index) {
        place_t const place = places[index];
        keys[index] = stored_of(place.key.uuid);
        stored_value_size_t size = 0;
        status_t const measured = store.stored_value_size(place.collection, keys[index], size, access.stamp,
                                                          access.thread_index);
        if (measured.failed() && measured != status_code_t::not_found_k) return measured;
        present[index] = measured.succeeded();
        windows[index] = present[index] ? outputs.window(index, size) : task_window_t {};
        total += windows[index].length;
    }

    if (arguments.values) {
        auto [room, room_status] = arena_allocate<byte_t>(outputs.arena(), total);
        if (room_status.failed()) return room_status;
        auto [rooms, rooms_status] = arena_allocate<byte_t*>(access.scratch, count);
        if (rooms_status.failed()) return rooms_status;
        auto [results, results_status] = arena_allocate<lookup_result_t>(access.scratch, count);
        if (results_status.failed()) return results_status;
        for (std::size_t index = 0, at = 0; index != count; at += windows[index].length, ++index)
            rooms[index] = room.data() + at;
        for (std::size_t first = 0; first != count;) {
            collection_id_t const collection = places[first].collection;
            std::size_t last = first + 1;
            while (last != count && places[last].collection == collection) ++last;
            // Only the bytes of each window land, at their place in its room
            auto sink = [&, first](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
                task_window_t const window = windows[first + request];
                std::uint64_t const begin = std::max<std::uint64_t>(offset, window.offset);
                std::uint64_t const end = std::min<std::uint64_t>(offset + bytes.size(), window.offset + window.length);
                if (begin < end)
                    std::memcpy(rooms[first + request] + (begin - window.offset), bytes.data() + (begin - offset),
                                static_cast<std::size_t>(end - begin));
            };
            if (status_t const read = store.select(collection, keys.subspan(first, last - first),
                                                   results.subspan(first, last - first), sink, access.stamp,
                                                   access.thread_index);
                read.failed())
                return read;
            first = last;
        }
        outputs.adopt(room);
    }
    for (std::size_t index = 0; index != count; ++index)
        if (present[index]) outputs.append_adopted(windows[index].length);
        else outputs.append_missing();
    return {};
}

/** Reads values through the store of each collection, folded as its modality folds them, for a
 *  caller whose resolver checked the collections: outside a transaction as one batched read
 *  straight into the arena, and through one key by key, each copied into the scratch and into the
 *  arena at the end. */
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
        if (status_t const read = read_values_batched(access, arguments, places, outputs); read.failed()) return read;
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

/** Scans keys as ordered walks of the store of each collection between the bounds of each
 *  task, in its direction, or run by run for an unordered scan outside a transaction, a
 *  graph's being its vertices, watching what a transaction hands out, for a caller whose
 *  resolver checked the collections. A descending walk passes every key of its range, as the
 *  runs walk ascending alone. */
template <cpu_class_t cpu_class_>
status_t scan_values(ustore_blobs_scan_t const& arguments, view_t const& view) noexcept {
    if (status_t const status = validate_scan(arguments); status.failed() || !arguments.count) return status;
    scan_batch_t const scans = scans_of(arguments);
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
        stored_key_t const lower = stored_of(scan.keys.lower.uuid);
        stored_key_t const upper = stored_of(scan.keys.upper.uuid);
        bool const descending = scan.direction == ustore_scan_descending_k;
        // An unordered scan walks each run alone outside a transaction, whose own deltas merge only in order
        status_t collected;
        if (scan.limit != 0 && arguments.mode == ustore_scan_unordered_k && !access.transaction)
            collected = collect_keys_unordered(access, scan.collection, lower, upper, scan.limit, found);
        if (scan.limit != 0 && (arguments.mode != ustore_scan_unordered_k || access.transaction ||
                                collected == status_code_t::contended_k)) {
            found.clear();
            collected = descending ? collect_keys_descending(access, scan.collection, lower, upper, scan.limit, found)
                                   : collect_keys(access, scan.collection, lower, upper, scan.limit, found);
        }
        if (collected.failed()) return collected;
        for (std::size_t index = 0; index != found.size(); ++index)
            if (status_t const pushed = outputs.push(key_of(found[index])); pushed.failed()) return pushed;
        if (status_t const watched = watch_keys(access, scan.collection, std::span<stored_key_t const> {found});
            watched.failed())
            return watched;
        // A full page covered the keys up to its last in its direction, a short one the whole range
        bool const full = found.size() == scan.limit && !found.empty();
        stored_key_t const watched_lower = full && descending ? found[found.size() - 1] : lower;
        stored_key_t const watched_upper = full && !descending ? found[found.size() - 1] : upper;
        if (scan.limit != 0)
            if (status_t const watched = watch_range(access, scan.collection, watched_lower, watched_upper);
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

/** Measures the keys and value bytes of each task's range, exact at the view it reads, through the
 *  store of each collection, for a caller whose resolver checked the collections. */
template <cpu_class_t cpu_class_>
status_t measure_values(ustore_blobs_measure_t const& arguments, view_t const& view) noexcept {
    if (status_t const direction = validate_direction(arguments.direction); direction.failed() || !arguments.count)
        return direction;
    scan_batch_t const measures = measures_of(arguments);
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    auto [outputs, outputs_status] = measure_outputs_t::make(arguments);
    if (outputs_status.failed()) return outputs_status;
    call_access<cpu_class_> const access = access_of(state, view);
    for (std::size_t task = 0; task != measures.size(); ++task) {
        scan_task_t const measure = measures[task];
        std::size_t cardinality = 0;
        std::size_t value_bytes = 0;
        stored_key_t const lower = stored_of(measure.keys.lower.uuid);
        stored_key_t const upper = stored_of(measure.keys.upper.uuid);
        if (measure.limit != 0) {
            status_t const walked = walk(access, measure.collection, lower,
                                         [&](stored_key_t key, stored_value_size_t size) noexcept {
                                             if (upper < key) return false;
                                             ++cardinality;
                                             value_bytes += size;
                                             return true;
                                         });
            if (walked.failed()) return walked;
            if (status_t const watched = watch_range(access, measure.collection, lower, upper); watched.failed())
                return watched;
        }
        outputs.assign(task, cardinality, value_bytes, value_bytes + cardinality * sizeof(stored_key_t));
    }
    outputs.finish(arguments);
    return {};
}

/** Implements @c ustore_blobs_measure over the collections holding blobs. */
template <cpu_class_t cpu_class_>
status_t blobs_measure(ustore_blobs_measure_t const& arguments, view_t const& view) noexcept {
    if (status_t const resolved = resolve_modality<cpu_class_>(arguments.database, arguments.collections,
                                                               arguments.count, ustore_modality_blobs_k, view);
        resolved.failed())
        return resolved;
    return measure_values<cpu_class_>(arguments, view);
}

#pragma endregion Blob Calls

} // namespace unum::ustore::nvme
