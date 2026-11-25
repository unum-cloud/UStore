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

#include <algorithm>   // `std::upper_bound`
#include <span>        // `std::span`
#include <type_traits> // `std::is_same_v`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/blobs.h"
#include "ustore/utilities/ranges.hpp" // `place_batch_t`, `strided`

#include "ustore/utilities/ram_buffers.hpp" // `arena_allocator`, `growth_status`

#include "database.hpp"           // `database_state`, `call_access`, `resolve_runs`, `serve_write`, `walk`
#include "utilities/batches.hpp"  // `places_of`, `validate_write`, `samples_of`, `sample_generator_t`
#include "utilities/outputs.hpp"  // `read_outputs_t`, `task_window_t`, `scan_outputs_t`, `measure_outputs_t`
#include "utilities/progress.hpp" // `progress_t`, `progress_cancelled_k`
#include "utilities/slices.hpp"   // `for_task_slices`, `for_tasks`, `slice_slots`

namespace unum::ustore::nvme {

#pragma region Blob Calls

/** Implements @c ustore_blobs_write, committing a batch outside a transaction atomically. */
template <cpu_class_t cpu_class_>
status_t blobs_write(ustore_blobs_write_t const& arguments, strings_arg_t const& values, view_t const& view) noexcept {
    util::place_batch_t const places = util::places_of(arguments);
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
                               util::place_t const place = places[index];
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
    util::place_batch_t const places = util::places_of(arguments);
    if (status_t const status = validate_remove(view.transaction, places, arguments.durability, view.watch);
        status.failed() || !arguments.count)
        return status;
    util::strided_iterator<ustore_key_t const> const end_keys = util::strided(arguments.end_keys);
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    return serve_write(state, view, arguments.durability, places.size(), 1, watch_schedule_t::never_k,
                       [&](call_access<cpu_class_> const& access) noexcept -> status_t {
                           for (std::size_t index = 0; index != places.size(); ++index) {
                               util::place_t const place = places[index];
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
 *      outside a transaction, as one batched read of every run of keys of one collection, a missing
 *      key reading as a missing value.
 *
 *  Every key descends the store once: the select tells each slice a value's size before its bytes,
 *  so the slice carves the window's room in its own thread's scratch, its tape, and lands the bytes
 *  there; one pass then lays the rooms end to end in the arena @p outputs exports, in key order. A
 *  read of no values asks for no bytes. The batch splits over the threads of @p view's executor,
 *  each slice reading on its thread's channel through one reading per collection's run of keys.
 */
template <cpu_class_t cpu_class_>
status_t read_values_batched(call_access<cpu_class_> const& access, view_t const& view,
                             ustore_blobs_read_t const& arguments, util::place_batch_t const& places,
                             util::read_outputs_t& outputs) noexcept {
    basic_database_of<cpu_class_>& store = *access.state.store;
    std::size_t const count = places.size();
    // Slices read runs of neighbours in key order, and outputs land at their tasks in request order
    auto [sorted, sorted_status] = sort_places(places, access.scratch);
    if (sorted_status.failed()) return sorted_status;
    util::place_batch_t const& neighbours = sorted.places;
    std::span<std::size_t const> const tasks = sorted.tasks;
    auto [keys, keys_status] = arena_allocate<stored_key_t>(access.scratch, count);
    if (keys_status.failed()) return keys_status;
    auto [windows, windows_status] = arena_allocate<util::task_window_t>(access.scratch, count);
    if (windows_status.failed()) return windows_status;
    auto [rooms, rooms_status] = arena_allocate<byte_t*>(access.scratch, count);
    if (rooms_status.failed()) return rooms_status;
    auto [results, results_status] = arena_allocate<lookup_result_t>(access.scratch, count);
    if (results_status.failed()) return results_status;
    auto [answers, answers_status] = arena_allocate<lookup_result_t>(access.scratch, count);
    if (answers_status.failed()) return answers_status;
    auto select_slice = [&](std::size_t begin, std::size_t length, std::size_t,
                            view_t const& slice) noexcept -> status_t {
        nvme_queues_ref_t const queues = nvme_queues_of(slice);
        status_t room_status;
        for (std::size_t first = begin; first != begin + length;) {
            std::size_t const last = run_end(neighbours.collections_begin, first, begin + length);
            for (std::size_t index = first; index != last; ++index) {
                keys[index] = stored_of(neighbours[index].key.uuid);
                windows[tasks[index]] = {};
            }
            sized_sink sink {
                .sized =
                    [&, first](std::size_t request, stored_value_size_t size) noexcept {
                        std::size_t const task = tasks[first + request];
                        windows[task] = outputs.window(task, size);
                        if (!arguments.values || windows[task].length == 0 || room_status.failed()) return false;
                        auto [room, status] = arena_allocate<byte_t>(slice.scratch, windows[task].length);
                        room_status = status;
                        rooms[task] = room.data();
                        return status.succeeded();
                    },
                // Only the bytes of each window land, at their place in its room
                .landed =
                    [&, first](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
                        std::size_t const task = tasks[first + request];
                        util::task_window_t const window = windows[task];
                        std::uint64_t const window_begin = fu::max_of<std::uint64_t>(offset, window.offset);
                        std::uint64_t const window_end = fu::min_of<std::uint64_t>(offset + bytes.size(),
                                                                                   window.offset + window.length);
                        if (window_begin < window_end)
                            std::memcpy(rooms[task] + (window_begin - window.offset),
                                        bytes.data() + (window_begin - offset),
                                        static_cast<std::size_t>(window_end - window_begin));
                    }};
            if (status_t const read = with_reading(access, neighbours[first].collection, queues.thread_index,
                                                   [&](collection_reading_of<cpu_class_> const& reading) noexcept {
                                                       return store.select(
                                                           reading, slice.scratch, keys.subspan(first, last - first),
                                                           results.subspan(first, last - first), sink, queues);
                                                   });
                read.failed())
                return read;
            if (room_status.failed()) return room_status;
            first = last;
        }
        return {};
    };
    if (status_t const read = util::for_task_slices(view, count, select_slice); read.failed()) return read;
    for (std::size_t index = 0; index != count; ++index) answers[tasks[index]] = results[index];

    std::size_t total = 0;
    for (std::size_t index = 0; index != count; ++index) {
        if (answers[index] == lookup_result_t::incomplete_k)
            return {status_code_t::io_k, "A run is shorter than its index claims"};
        if (answers[index] == lookup_result_t::written_k) total += windows[index].length;
    }
    if (arguments.values) {
        auto [room, room_status] = arena_allocate<byte_t>(outputs.arena(), total);
        if (room_status.failed()) return room_status;
        for (std::size_t index = 0, at = 0; index != count; ++index) {
            if (answers[index] != lookup_result_t::written_k || windows[index].length == 0) continue;
            std::memcpy(room.data() + at, rooms[index], windows[index].length);
            at += windows[index].length;
        }
        outputs.adopt(room);
    }
    for (std::size_t index = 0; index != count; ++index)
        if (answers[index] == lookup_result_t::written_k) outputs.append_adopted(windows[index].length);
        else outputs.append_missing();
    return {};
}

/** Reads the values @p places name through @p access, which holds one reading for the call:
 *  outside a transaction as one batched read straight into the arena, and through one key by key,
 *  each copied into the scratch and into the arena at the end. */
template <cpu_class_t cpu_class_>
status_t read_values_held(call_access<cpu_class_> const& access, view_t const& view,
                          ustore_blobs_read_t const& arguments, util::place_batch_t const& places) noexcept {
    auto [outputs, outputs_status] = util::read_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    if (!access.transaction) {
        if (status_t const read = read_values_batched(access, view, arguments, places, outputs); read.failed())
            return read;
        return outputs.finish(arguments);
    }
    for (std::size_t index = 0; index != places.size(); ++index) {
        util::place_t const place = places[index];
        auto [value, read_status] = read_value(access, place.collection, stored_of(place.key.uuid));
        if (read_status.failed()) return read_status;
        if (value) outputs.append(value);
        else outputs.append_missing();
    }
    return outputs.finish(arguments);
}

/** Reads values through the store of each collection, folded as its modality folds them, for a
 *  caller whose resolver checked the collections, as @c read_values_held reads them. */
template <cpu_class_t cpu_class_>
status_t read_values(ustore_blobs_read_t const& arguments, view_t const& view) noexcept {
    if (!arguments.count) return {};
    util::place_batch_t const places = util::places_of(arguments);
    if (status_t const status = validate_read(places); status.failed()) return status;
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    call_access<cpu_class_> access = access_of(state, view);
    collection_reading_of<cpu_class_> held;
    if (status_t const one = read_at_one_state(access, places.collections_begin, places.size(), held); one.failed())
        return one;
    return read_values_held(access, view, arguments, places);
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

/** The keys of one collection a transaction's scan watches once the tasks finish: those a task
 *  listed, and the range its walk covered, lowest first. */
struct watched_keys_t {
    std::span<stored_key_t const> keys;
    stored_key_t lower;
    stored_key_t upper;
};

/** Watches through the transaction of @p access, once the tasks finished, what each of @p watched
 *  recorded, in task order, as a transaction's watch set takes one call at a time. */
template <cpu_class_t cpu_class_>
status_t watch_tasks(call_access<cpu_class_> const& access, collection_id_t collection,
                     std::span<watched_keys_t const> watched) noexcept {
    for (watched_keys_t const& task : watched) {
        if (status_t const status = watch_keys(access, collection, task.keys); status.failed()) return status;
        if (task.upper < task.lower) continue;
        if (status_t const status = watch_range(access, collection, task.lower, task.upper); status.failed())
            return status;
    }
    return {};
}

/** Scans keys as ordered walks of the store of each collection between the bounds of each task, in
 *  its direction, or run by run for an unordered scan outside a transaction, a graph's being its
 *  vertices, for a caller whose resolver checked the collections. A descending walk passes every
 *  key of its range, as the runs walk ascending alone. Every task watches nothing while the slices
 *  run; a transaction watches what each listed once they end. */
template <cpu_class_t cpu_class_>
status_t scan_values(ustore_blobs_scan_t const& arguments, view_t const& view) noexcept {
    if (status_t const status = util::validate_scan(arguments); status.failed() || !arguments.count) return status;
    util::scan_batch_t const scans = util::scans_of(arguments);
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    auto [outputs, outputs_status] = util::scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    st::vector<util::scan_slice_t, arena_allocator<util::scan_slice_t>> slices {{view.scratch}};
    if (st::failed(slices.resize(util::slice_slots(view))))
        return {status_code_t::out_of_memory_k, "No room for the scan slices"};
    auto [watched, watched_status] = arena_allocate<watched_keys_t>(view.scratch, scans.size());
    if (watched_status.failed()) return watched_status;
    util::progress_t progress {arguments.progress, arguments.progress_state, arguments.count};
    call_access<cpu_class_> access = access_of(state, view);
    collection_reading_of<cpu_class_> held;
    if (status_t const one = read_at_one_state(access, scans.collections, scans.size(), held); one.failed()) return one;
    auto scan_slice = [&](std::size_t first_task, std::size_t length, std::size_t run,
                          view_t const& slice) noexcept -> status_t {
        util::scan_slice_t& sliced = slices[run];
        sliced = {first_task, length, outputs.slice_in(slice.scratch)};
        call_access<cpu_class_> const sliced_access {state,          access.transaction, watch_schedule_t::never_k,
                                                     slice.scratch,  access.stamp,       nvme_queues_of(slice),
                                                     access.strings, access.reading};
        st::vector<stored_key_t, arena_allocator<stored_key_t>> found {{slice.scratch}};
        for (std::size_t task = first_task; task != first_task + length; ++task) {
            util::scan_task_t const scan = scans[task];
            sliced.outputs.open_task(task);
            found.clear();
            stored_key_t const lower = stored_of(scan.keys.lower.uuid);
            stored_key_t const upper = stored_of(scan.keys.upper.uuid);
            bool const descending = scan.direction == ustore_scan_descending_k;
            // Unordered scans walk each run alone, as only a transaction's deltas need the order
            bool const unordered = arguments.mode == ustore_scan_unordered_k && !sliced_access.transaction;
            status_t collected;
            if (scan.limit != 0)
                collected =
                    unordered ? collect_keys_unordered(sliced_access, scan.collection, lower, upper, scan.limit, found)
                    : descending
                        ? collect_keys_descending(sliced_access, scan.collection, lower, upper, scan.limit, found)
                        : collect_keys(sliced_access, scan.collection, lower, upper, scan.limit, found);
            if (collected.failed()) return collected;
            for (std::size_t index = 0; index != found.size(); ++index)
                if (status_t const pushed = sliced.outputs.push(key_of(found[index])); pushed.failed()) return pushed;
            // A full page covered the keys up to its last one, and a short page the whole range
            bool const full = found.size() == scan.limit && !found.empty();
            watched[task] = {{},
                             full && descending ? found[found.size() - 1] : lower,
                             full && !descending ? found[found.size() - 1] : upper};
            if (scan.limit == 0) watched[task] = {{}, upper, lower};
            if (access.transaction && !found.empty()) {
                auto [kept, kept_status] = util::arena_copy<stored_key_t>(slice.scratch,
                                                                          std::span<stored_key_t const> {found});
                if (kept_status.failed()) return kept_status;
                watched[task].keys = kept;
            }
            if (!progress.advance(1)) return util::progress_cancelled_k;
        }
        return {};
    };
    if (status_t const scanned = util::for_task_slices(view, scans.size(), scan_slice); scanned.failed())
        return scanned;
    if (status_t const finished = outputs.finish(arguments, slices); finished.failed()) return finished;
    for (std::size_t task = 0; access.transaction && task != scans.size(); ++task)
        if (status_t const status = watch_tasks(access, scans[task].collection, {&watched[task], 1}); status.failed())
            return status;
    return {};
}

/** The key a walk in @p direction visits right after @p key, or answers false past the walk's
 *  last key. */
inline bool step_key(stored_key_t& key, scan_direction_t direction) noexcept {
    ustore_key_t const edge = direction == scan_direction_t::descending_k ? util::smallest_key_k : util::largest_key_k;
    ustore_key_t const current = key_of(key);
    if (current == edge) return false;
    key = stored_of(
        (direction == scan_direction_t::descending_k ? util::key_before(current) : util::key_after(current)).uuid);
    return true;
}

/** The key a walk in @p direction visits right before @p key, which must not be its first. */
inline stored_key_t key_before_in(stored_key_t key, scan_direction_t direction) noexcept {
    ustore_key_t const current = key_of(key);
    return stored_of(
        (direction == scan_direction_t::descending_k ? util::key_after(current) : util::key_before(current)).uuid);
}

/** How a task of a cursor's pull ended: past its whole range, or once its batch filled, its
 *  range left. */
enum class task_end_t : std::uint8_t {
    walked_k,
    filled_k,
};

/**
 *  @brief Implements a cursor's pull of the one range of @p arguments into @p sink, the batches of
 *      @c docs_batches_t, split into one task per thread of @p view's executor, which they claim.
 *
 *  The calling thread cuts the range at the key slots of its runs, with no read, into tasks of
 *  about equal entries and value bytes, which together cover about @c wanted entries, and carves
 *  every batch from the entries and bytes each task holds, before the tasks run. Each task walks
 *  its range a round at a time with a level cursor in its thread's scratch, reads the round's
 *  values on its thread's channel at the places the walk found them, and narrows and appends them
 *  to its batch. A walk through a transaction merges its own writes and records what it watches,
 *  which the calling thread watches once the tasks end. The tasks cover consecutive ranges, so the
 *  next pull resumes past the first task that filled, or at the first that never ran.
 */
template <cpu_class_t cpu_class_, typename sink_type_>
status_t scan_values(ustore_blobs_scan_t const& arguments, sink_type_& sink, view_t const& view) noexcept {
    if (status_t const status = util::validate_scan(arguments); status.failed()) return status;
    if (arguments.count != 1) return {status_code_t::invalid_argument_k, "A cursor's pull scans one range"};
    util::scan_batch_t const scans = util::scans_of(arguments);
    util::scan_task_t const scan = scans[0];
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    basic_database_of<cpu_class_>& store = *state.store;
    call_access<cpu_class_> access = access_of(state, view);
    collection_reading_of<cpu_class_> held;
    if (status_t const one = read_at_one_state(access, scans.collections, 1, held); one.failed()) return one;
    sink.resume = exhausted_t {};
    if (scan.limit == 0) return sink.carve({}, {}, view.scratch);

    scan_direction_t const direction = scan.direction == ustore_scan_descending_k ? scan_direction_t::descending_k
                                                                                  : scan_direction_t::ascending_k;
    bool const descending = direction == scan_direction_t::descending_k;
    stored_key_t const lower = stored_of(scan.keys.lower.uuid);
    stored_key_t const upper = stored_of(scan.keys.upper.uuid);
    stored_key_t const from = descending ? upper : lower;
    stored_key_t const bound = descending ? lower : upper;
    // One task per thread, or one where the match's engine serves one call at a time
    std::size_t const capacity = view.executor && !sink.reads_values() ? view.executor->pool.view().threads_count()
                                                                       : std::size_t {1};
    auto [starts, starts_status] = arena_allocate<stored_key_t>(view.scratch, capacity);
    auto [entries, entries_status] = arena_allocate<std::uint64_t>(view.scratch, capacity);
    auto [bytes, bytes_status] = arena_allocate<std::uint64_t>(view.scratch, capacity);
    auto [limits, limits_status] = arena_allocate<std::uint64_t>(view.scratch, capacity);
    auto [ends, ends_status] = arena_allocate<task_end_t>(view.scratch, capacity);
    auto [watched, watched_status] = arena_allocate<watched_keys_t>(view.scratch, capacity);
    if (starts_status.failed() || entries_status.failed() || bytes_status.failed() || limits_status.failed() ||
        ends_status.failed() || watched_status.failed())
        return {status_code_t::out_of_memory_k, "A pull could not hold its tasks"};
    std::optional<stored_key_t> stop;
    std::size_t tasks = 0;
    if (status_t const split = with_reading(access, scan.collection, view.thread_index,
                                            [&](collection_reading_of<cpu_class_> const& reading) noexcept {
                                                auto [cut, cut_status] = store.split(reading, view.scratch, from, bound,
                                                                                     direction, scan.limit, starts,
                                                                                     entries, bytes, stop);
                                                tasks = cut;
                                                return cut_status;
                                            });
        split.failed())
        return split;

    // A transaction's own writes add to the task holding each, a change by the size it folds to
    auto const task_of = [&](stored_key_t const& key) noexcept -> std::size_t {
        bool const past_bound = descending ? key < bound : bound < key;
        bool const past_stop = stop && (descending ? !(*stop < key) : !(key < *stop));
        if ((descending ? from < key : key < from) || past_bound || past_stop) return tasks;
        std::size_t task = 0;
        while (task + 1 != tasks && !(descending ? starts[task + 1] < key : key < starts[task + 1])) ++task;
        return task;
    };
    if (access.transaction) {
        call_access<cpu_class_> const quiet {state,         access.transaction, watch_schedule_t::never_k,
                                             view.scratch,  access.stamp,       access.queues,
                                             access.strings};
        status_t counted;
        access.transaction->for_each_uncommitted(
            scan.collection, [&](stored_key_t key, delta_kind_t kind, stored_value_size_t size) noexcept {
                std::size_t const task = task_of(key);
                if (task == tasks || counted.failed()) return;
                std::uint64_t folded_size = size;
                if (kind == delta_kind_t::change_k) {
                    auto [folded, fold_status] = read_value(quiet, scan.collection, key);
                    counted = fold_status;
                    folded_size = folded.size();
                }
                entries[task] += 1, bytes[task] += folded_size;
            });
        if (counted.failed()) return counted;
    }
    for (std::uint64_t task = 0, left = scan.limit; task != tasks; ++task) {
        limits[task] = fu::min_of(entries[task], left);
        left -= limits[task];
        ends[task] = task_end_t::walked_k;
        watched[task] = {{}, upper, lower};
    }
    if (status_t const carved = sink.carve({limits.data(), tasks}, {bytes.data(), tasks}, view.scratch);
        carved.failed())
        return carved;

    using strings_kit_t = strings_kit_of<cpu_ladder_floor_k>;
    auto run_task = [&](std::size_t task, view_t const& slice) noexcept -> status_t {
        std::size_t const limit = static_cast<std::size_t>(limits[task]);
        if (!limit) return {};
        stored_key_t const task_from = starts[task];
        stored_key_t const task_bound = task + 1 != tasks ? key_before_in(starts[task + 1], direction)
                                        : stop            ? key_before_in(*stop, direction)
                                                          : bound;
        call_access<cpu_class_> const sliced_access {state,          access.transaction, watch_schedule_t::never_k,
                                                     slice.scratch,  access.stamp,       nvme_queues_of(slice),
                                                     access.strings, access.reading};
        auto consume = [&](std::span<stored_key_t const> keys, auto&& read_documents) noexcept -> expected<bool> {
            auto [listed, listed_status] = arena_allocate<ustore_key_t>(slice.scratch, keys.size());
            if (listed_status.failed()) return listed_status;
            for (std::size_t index = 0; index != keys.size(); ++index) listed[index] = key_of(keys[index]);
            candidates_t kept;
            json_documents_t documents;
            std::span<std::uint32_t> document_of;
            if (status_t const narrowed = sink.narrow(std::span<ustore_key_t const> {listed}, read_documents,
                                                      slice.scratch, kept, documents, document_of);
                narrowed.failed())
                return narrowed;
            // Only a task cut below its entries fills with keys of its range left
            std::size_t const room = sink.room(task, limit);
            bool const filled = kept.count > room;
            kept.count = fu::min_of(kept.count, room);
            if (status_t const appended = sink.template append<strings_kit_t>(
                    task, std::span<ustore_key_t const> {listed}, kept, documents, document_of, slice.scratch);
                appended.failed())
                return appended;
            if (filled) ends[task] = task_end_t::filled_k;
            return !filled;
        };

        if (!access.transaction)
            return with_reading(
                sliced_access, scan.collection, slice.thread_index,
                [&](collection_reading_of<cpu_class_> const& reading) noexcept {
                    return store.scan_rounds(
                        reading, slice.scratch, task_from, task_bound, direction,
                        [&](std::span<stored_key_t const> keys, std::span<stored_value_size_t const> sizes,
                            auto&& reader) noexcept -> expected<bool> {
                            auto read_documents = [&](std::span<std::uint32_t const> picked,
                                                      json_documents_t& documents) noexcept -> status_t {
                                auto [indices, indices_status] = arena_allocate<std::size_t>(slice.scratch,
                                                                                             picked.size());
                                auto [offsets, offsets_status] = arena_allocate<std::uint64_t>(slice.scratch,
                                                                                               picked.size() + 1);
                                if (indices_status.failed() || offsets_status.failed())
                                    return {status_code_t::out_of_memory_k, "A task could not lay out its documents"};
                                offsets[0] = 0;
                                for (std::size_t index = 0; index != picked.size(); ++index) {
                                    indices[index] = picked[index];
                                    offsets[index + 1] = offsets[index] + sizes[picked[index]];
                                }
                                auto [tape, tape_status] = arena_allocate<byte_t>(slice.scratch,
                                                                                  offsets[picked.size()]);
                                if (tape_status.failed()) return tape_status;
                                documents = {util::u64_tape(tape.data(), offsets.data()), nullptr, picked.size()};
                                return reader(std::span<std::size_t const> {indices},
                                              [&](std::size_t index, std::uint64_t offset,
                                                  std::span<byte_t const> landed) noexcept {
                                                  if (!landed.empty())
                                                      std::memcpy(tape.data() + offsets[index] + offset, landed.data(),
                                                                  landed.size());
                                              });
                            };
                            return consume(keys, read_documents);
                        },
                        sliced_access.queues);
                });

        // Through a transaction, every round walks with the transaction's own writes merged in, and
        // is watched later
        std::size_t const round_keys = scan_page_keys<stored_key_t>();
        st::vector<stored_key_t, arena_allocator<stored_key_t>> keys {{slice.scratch}};
        st::vector<stored_key_t, arena_allocator<stored_key_t>> walked {{slice.scratch}};
        stored_key_t round_from = task_from;
        for (bool more = true; more;) {
            keys.clear();
            status_t room;
            status_t const walked_status = walk(
                sliced_access, scan.collection, round_from,
                [&](stored_key_t key, stored_value_size_t) noexcept {
                    if (descending ? key < task_bound : task_bound < key) return more = false;
                    room = growth_status(keys.push_back(stored_key_t {key}));
                    return room.succeeded() && keys.size() < round_keys;
                },
                direction);
            if (walked_status.failed()) return walked_status;
            if (room.failed()) return room;
            if (keys.size() < round_keys) more = false;
            if (keys.empty()) break;
            for (stored_key_t const& key : keys)
                if (status_t const kept = growth_status(walked.push_back(stored_key_t {key})); kept.failed())
                    return kept;
            auto read_documents = [&](std::span<std::uint32_t const> picked,
                                      json_documents_t& documents) noexcept -> status_t {
                auto [values, values_status] = arena_allocate<value_view_t>(slice.scratch, picked.size());
                auto [offsets, offsets_status] = arena_allocate<std::uint64_t>(slice.scratch, picked.size() + 1);
                if (values_status.failed() || offsets_status.failed())
                    return {status_code_t::out_of_memory_k, "A task could not lay out its documents"};
                offsets[0] = 0;
                for (std::size_t index = 0; index != picked.size(); ++index) {
                    auto [value, value_status] = read_value(sliced_access, scan.collection, keys[picked[index]]);
                    if (value_status.failed()) return value_status;
                    values[index] = value;
                    offsets[index + 1] = offsets[index] + value.size();
                }
                auto [tape, tape_status] = arena_allocate<byte_t>(slice.scratch, offsets[picked.size()]);
                if (tape_status.failed()) return tape_status;
                for (std::size_t index = 0; index != picked.size(); ++index)
                    if (values[index].size())
                        std::memcpy(tape.data() + offsets[index], values[index].data(), values[index].size());
                documents = {util::u64_tape(tape.data(), offsets.data()), nullptr, picked.size()};
                return {};
            };
            auto [keeps_going, consume_status] = consume(std::span<stored_key_t const> {keys}, read_documents);
            if (consume_status.failed()) return consume_status;
            if (!keeps_going) break;
            round_from = keys.back();
            if (more) more = step_key(round_from, direction);
        }
        // The range walked so far is watched with its keys, so a commit landing a key it did not
        // find conflicts too
        stored_key_t const reached = ends[task] == task_end_t::filled_k && !walked.empty() ? walked.back() : task_bound;
        auto [kept, kept_status] = util::arena_copy<stored_key_t>(slice.scratch,
                                                                  std::span<stored_key_t const> {walked});
        if (kept_status.failed()) return kept_status;
        watched[task] = {kept, descending ? reached : task_from, descending ? task_from : reached};
        return {};
    };
    if (status_t const ran = util::for_tasks(view, tasks, run_task); ran.failed()) return ran;
    if (status_t const status = watch_tasks(access, scan.collection, {watched.data(), tasks}); status.failed())
        return status;

    if (stop) sink.resume = key_of(*stop);
    for (std::size_t task = 0; task != tasks; ++task) {
        // A task the limit cut off resumes the walk, and an empty one has nothing to walk
        if (!limits[task] && entries[task]) {
            sink.resume = key_of(starts[task]);
            break;
        }
        if (ends[task] != task_end_t::filled_k) continue;
        stored_key_t next = stored_of(sink.batches[task].keys[sink.batches[task].rows - 1].uuid);
        if (step_key(next, direction)) sink.resume = key_of(next);
        else sink.resume = exhausted_t {};
        break;
    }
    return {};
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

/** A store keeping its collections under the database's own key, which the blobs calls reach. */
template <typename store_type_>
concept a_database_keyed_store = std::is_same_v<typename store_type_::stored_key_t, stored_key_t>;

/**
 *  @brief Appends to @p kept up to @p wanted distinct present keys of the collection @p read reads
 *      through @p store, drawn uniformly by rank over its runs, memory runs included.
 *
 *  A rank maps onto a run by the prefix counts of the runs' keys, carved in @p scratch, and a draw
 *  is kept only from the key's newest version. The draws stop at as many as there are versions,
 *  counted into @p versions, and a sample that wide takes every version in turn, not at random.
 */
template <typename store_type_, typename keys_type_>
status_t draw_ranks(store_type_ const& store, typename store_type_::reading_t const& read, std::size_t wanted,
                    util::sample_generator_t& generator, ustore_arena_t scratch, keys_type_& kept,
                    std::uint64_t& versions) noexcept {
    counted_runs_t const counted = store.count_runs(read);
    auto [prefix, prefix_status] = arena_allocate<std::uint64_t>(scratch, counted.count + 1);
    if (prefix_status.failed()) return prefix_status;
    prefix[0] = 0;
    for (std::uint64_t run = 0; run != counted.count; ++run)
        prefix[run + 1] = prefix[run] + store.run_population(read, run);
    std::uint64_t const total = prefix[counted.count];
    versions = total;
    bool const every = wanted >= total;
    if (status_t const room = growth_status(kept.reserve(every ? total : wanted)); room.failed()) return room;
    for (std::uint64_t draws = 0; kept.size() < wanted && draws != total; ++draws) {
        std::uint64_t const rank = every ? draws : st::draw_below(generator, total);
        std::uint64_t const run = static_cast<std::uint64_t>(std::upper_bound(prefix.begin(), prefix.end(), rank) -
                                                             prefix.begin() - 1);
        stored_key_t key;
        if (!store.newest_present_key(read, run, rank - prefix[run], key)) continue;
        [[maybe_unused]] st::status_t const pushed = kept.push_back(st::assume_reserved, stored_key_t {key});
        // Duplicates leave once the sample fills, so the draws go on for the keys they displaced
        if (kept.size() != wanted) continue;
        [[maybe_unused]] st::status_t const shrunk = kept.resize(util::sort_and_deduplicate(kept.begin(), kept.end()));
    }
    [[maybe_unused]] st::status_t const shrunk = kept.resize(util::sort_and_deduplicate(kept.begin(), kept.end()));
    return {};
}

/** Samples keys as a reservoir filled from a walk of @p collection: a transaction's view, whose own
 *  writes no rank of the store counts, or the runs whose rank draws ran out short. */
template <cpu_class_t cpu_class_, typename keys_type_>
status_t sample_walked(call_access<cpu_class_> const& access, collection_id_t collection, std::size_t wanted,
                       util::sample_generator_t& generator, keys_type_& kept) noexcept {
    std::size_t seen = 0;
    status_t room;
    status_t const walked = walk(
        access, collection, limit<stored_key_t>::min_k, [&](stored_key_t key, stored_value_size_t) noexcept {
            std::size_t const slot = seen < wanted ? seen : st::draw_below(generator, seen + 1);
            ++seen;
            if (slot >= wanted) return true;
            if (slot != kept.size()) {
                kept[slot] = key;
                return true;
            }
            room = growth_status(kept.push_back(stored_key_t {key}));
            return room.succeeded();
        });
    return walked.failed() ? walked : room;
}

/** Samples into @p kept the present keys of @p collection as @p access reads them outside a
 *  transaction, by rank, carving what the draws track in the scratch of @p access, so a sample
 *  holds as many keys as asked or every present key. */
template <cpu_class_t cpu_class_, typename keys_type_>
status_t sample_ranks(call_access<cpu_class_> const& access, collection_id_t collection, std::size_t wanted,
                      util::sample_generator_t& generator, keys_type_& kept) noexcept {
    basic_database_of<cpu_class_>& store = *access.state.store;
    std::uint64_t versions = 0;
    status_t const drawn = with_reading(
        access, collection, access.queues.thread_index,
        [&](collection_reading_of<cpu_class_> const& reading) noexcept -> status_t {
            return store.visit_reading(
                reading,
                [&]<typename store_type_>(store_type_ const& source,
                                          typename store_type_::reading_t const& read) noexcept -> status_t {
                    if constexpr (a_database_keyed_store<store_type_>)
                        return draw_ranks(source, read, wanted, generator, access.scratch, kept, versions);
                    else return {status_code_t::invalid_argument_k, "That collection is keyed otherwise"};
                });
        });
    if (drawn.failed() || kept.size() == wanted || wanted >= versions) return drawn;
    // Random draws as many as the versions can miss present keys, which a rare walk then finds
    kept.clear();
    return sample_walked(access, collection, wanted, generator, kept);
}

/** Samples keys of the store of each collection, for a caller whose resolver checked the
 *  collections: by rank over the runs, one slice of tasks per thread, or as a reservoir over a walk
 *  of a transaction's view. */
template <cpu_class_t cpu_class_>
status_t sample_values(ustore_blobs_sample_t const& arguments, view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!arguments.offsets || !arguments.keys)
        return {status_code_t::invalid_argument_k, "Need outputs for the offsets and the keys"};
    util::sample_batch_t const samples = util::samples_of(arguments);
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    auto [outputs, outputs_status] = util::scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    st::vector<util::scan_slice_t, arena_allocator<util::scan_slice_t>> slices {{view.scratch}};
    if (st::failed(slices.resize(util::slice_slots(view))))
        return {status_code_t::out_of_memory_k, "No room for the sample slices"};
    call_access<cpu_class_> access = access_of(state, view);
    collection_reading_of<cpu_class_> held;
    if (status_t const one = read_at_one_state(access, samples.collections, samples.size(), held); one.failed())
        return one;
    auto sample_slice = [&](std::size_t first_task, std::size_t length, std::size_t run,
                            view_t const& slice) noexcept -> status_t {
        util::scan_slice_t& sliced = slices[run];
        sliced = {first_task, length, outputs.slice_in(slice.scratch)};
        call_access<cpu_class_> const sliced_access {state,          access.transaction, access.schedule,
                                                     slice.scratch,  access.stamp,       nvme_queues_of(slice),
                                                     access.strings, access.reading};
        util::sample_generator_t generator = util::sample_generator_t::seeded(&sliced);
        st::vector<stored_key_t, arena_allocator<stored_key_t>> kept {{slice.scratch}};
        for (std::size_t task = first_task; task != first_task + length; ++task) {
            util::sample_task_t const sample = samples[task];
            sliced.outputs.open_task(task);
            kept.clear();
            status_t const drawn = sample.limit == 0 ? status_t {}
                                   : sliced_access.transaction
                                       ? sample_walked(sliced_access, sample.collection, sample.limit, generator, kept)
                                       : sample_ranks(sliced_access, sample.collection, sample.limit, generator, kept);
            if (drawn.failed()) return drawn;
            for (std::size_t index = 0; index != kept.size(); ++index)
                if (status_t const pushed = sliced.outputs.push(key_of(kept[index])); pushed.failed()) return pushed;
            if (status_t const watched = watch_keys(sliced_access, sample.collection,
                                                    std::span<stored_key_t const> {kept});
                watched.failed())
                return watched;
        }
        return {};
    };
    // A transaction's reads join its watch set, so only a read at one stamp splits the batch
    status_t const sampled = access.transaction ? sample_slice(0, samples.size(), 0, view)
                                                : util::for_task_slices(view, samples.size(), sample_slice);
    if (sampled.failed()) return sampled;
    return outputs.finish(arguments, slices);
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
    if (status_t const direction = util::validate_direction(arguments.direction);
        direction.failed() || !arguments.count)
        return direction;
    util::scan_batch_t const measures = util::measures_of(arguments);
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    auto [outputs, outputs_status] = util::measure_outputs_t::make(arguments);
    if (outputs_status.failed()) return outputs_status;
    call_access<cpu_class_> access = access_of(state, view);
    collection_reading_of<cpu_class_> held;
    if (status_t const one = read_at_one_state(access, measures.collections, measures.size(), held); one.failed())
        return one;
    for (std::size_t task = 0; task != measures.size(); ++task) {
        util::scan_task_t const measure = measures[task];
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
