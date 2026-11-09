/**
 *  @file src/nvme/strings.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The strings calls of the C ABI over the strings stores of <tt>src/nvme/database.hpp</tt>,
 *      for the NVMe engine's table.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstring> // `std::memcpy`

#include <span>        // `std::span`
#include <string_view> // `std::string_view`

#include "ustore/ranges.hpp" // `strided`, `strided_iterator`
#include "ustore/strings.h"

#include "ustore/utilities/ram_buffers.hpp" // `arena_allocate`

#include "database.hpp"           // `database_state`, `string_task_t`, `stage_strings`, `run_end`, `scan_strings`
#include "utilities/batches.hpp"  // `strings_arg_t`, `validate_choices`, `validate_direction`, `samples_of`
#include "utilities/outputs.hpp"  // `strings_read_outputs_t`, `strings_scan_outputs_t`, `measure_outputs_t`
#include "utilities/progress.hpp" // `progress_t`, `progress_cancelled_k`
#include "utilities/slices.hpp"   // `for_task_slices`

namespace unum::ustore::nvme {

#pragma region Strings Calls

/** Lays the tasks of a strings write or removal out in the scratch of @p view, writing the keys of @p values, or
 *  removing the strings when @p values view none. */
inline expected<std::span<string_task_t>> string_tasks(view_t const& view,
                                                       strided_iterator<ustore_collection_id_t const> collections,
                                                       std::size_t count, strings_arg_t const& texts,
                                                       strided_iterator<ustore_key_t const> values) noexcept {
    auto [tasks, allocate_status] = arena_allocate<string_task_t>(view.scratch, count);
    if (allocate_status.failed()) return allocate_status;
    for (std::size_t index = 0; index != count; ++index)
        tasks[index] = values ? string_task_t {collections[index], string_of(texts[index]), delta_kind_t::write_k,
                                               values[index].uuid}
                              : string_task_t {collections[index], string_of(texts[index])};
    return tasks;
}

/** Writes or removes the strings of a batch, each one on its own, watched as the call's transaction watches and not
 *  at all outside one, where the last writer wins. */
template <cpu_class_t cpu_class_, typename arguments_type_>
status_t serve_strings_write(arguments_type_ const& arguments, strings_arg_t const& texts,
                             strided_iterator<ustore_key_t const> values, view_t const& view) noexcept {
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    if (status_t const resolved = resolve_runs(state, view, collections, arguments.count, ustore_modality_strings_k);
        resolved.failed())
        return resolved;
    auto [tasks, tasks_status] = string_tasks(view, collections, arguments.count, texts, values);
    if (tasks_status.failed()) return tasks_status;
    std::size_t bytes = 0;
    for (string_task_t const& task : tasks) bytes += task.text.size() + sizeof(task.value);
    return serve_write(state, view, arguments.durability, arguments.count, bytes, watch_schedule_t::never_k,
                       [&](call_access<cpu_class_> const& access) noexcept {
                           return stage_strings(access, std::span<string_task_t const> {tasks});
                       });
}

/** Implements @c ustore_strings_write, committing a batch outside a transaction atomically. */
template <cpu_class_t cpu_class_>
status_t strings_write(ustore_strings_write_t const& arguments, strings_arg_t const& texts,
                       view_t const& view) noexcept {
    if (status_t const status = validate_choices(view.transaction, arguments.durability, view.watch); status.failed())
        return status;
    if (!arguments.count) return {};
    strided_iterator<ustore_key_t const> const values = strided(arguments.values);
    if (!texts || !values) return {status_code_t::invalid_argument_k, "Need the strings and their keys"};
    return serve_strings_write<cpu_class_>(arguments, texts, values, view);
}

/** Implements @c ustore_strings_remove, committing a batch outside a transaction atomically, every
 *  range of strings the call names by end strings as one range tombstone. */
template <cpu_class_t cpu_class_>
status_t strings_remove(ustore_strings_remove_t const& arguments, strings_arg_t const& texts,
                        strings_arg_t const& end_texts, view_t const& view) noexcept {
    if (status_t const status = validate_choices(view.transaction, arguments.durability, view.watch); status.failed())
        return status;
    if (!arguments.count) return {};
    if (!texts) return {status_code_t::invalid_argument_k, "No strings were provided"};
    if (end_texts) {
        strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
        database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
        if (status_t const resolved = resolve_runs(state, view, collections, arguments.count,
                                                   ustore_modality_strings_k);
            resolved.failed())
            return resolved;
        std::size_t bytes = 0;
        for (std::size_t index = 0; index != arguments.count; ++index)
            bytes += string_of(texts[index]).size() + string_of(end_texts[index]).size();
        return serve_write(
            state, view, arguments.durability, arguments.count, bytes, watch_schedule_t::never_k,
            [&](call_access<cpu_class_> const& access) noexcept -> status_t {
                for (std::size_t index = 0; index != arguments.count; ++index)
                    if (status_t const removed = remove_string_range(
                            access, collections[index], string_of(texts[index]), string_of(end_texts[index]));
                        removed.failed())
                        return removed;
                return {};
            });
    }
    return serve_strings_write<cpu_class_>(arguments, texts, strided_iterator<ustore_key_t const> {}, view);
}

/** Reads the keys of the strings @p texts holds from @p first up to @p last, all of @p collection,
 *  into @p outputs as @p access sees them, inside one epoch entry and one lookup of the collection:
 *  a transaction's own string first, then the store's, watched as @p access watches. */
template <cpu_class_t cpu_class_>
status_t read_strings(call_access<cpu_class_> const& access, collection_id_t collection, strings_arg_t const& texts,
                      std::size_t first, std::size_t last, strings_read_outputs_t& outputs) noexcept {
    transaction_stamp_t const watch_stamp = watch_stamp_of(access);
    return visit_strings(access, collection, [&](auto const& store, auto const& reading) noexcept -> status_t {
        for (std::size_t index = first; index != last; ++index) {
            std::string_view const text = string_of(texts[index]);
            ustore_key_t value;
            std::span<byte_t, sizeof(ustore_uuid_t)> const bytes = std::as_writable_bytes(std::span {value.uuid.bytes});
            delta_view_t own_delta;
            bool present = false;
            if (access.strings && access.strings->uncommitted(collection, text, own_delta)) {
                present = own_delta.kind != delta_kind_t::remove_k;
                if (present) std::memcpy(bytes.data(), own_delta.bytes.data(), bytes.size());
            }
            else {
                if (watch_stamp != invalid_transaction_stamp_k)
                    if (status_t const watched = access.strings->watch(collection, text, watch_stamp); watched.failed())
                        return watched;
                present = store.find(reading, text, bytes) == lookup_result_t::written_k;
            }
            if (present) outputs.assign_present(index, value);
            else outputs.assign_missing(index);
        }
        return {};
    });
}

/** Implements @c ustore_strings_read, reading every run of strings of one collection together. */
template <cpu_class_t cpu_class_>
status_t strings_read(ustore_strings_read_t const& arguments, strings_arg_t const& texts, view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!texts) return {status_code_t::invalid_argument_k, "No strings were provided"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    if (status_t const resolved = resolve_runs(state, view, collections, arguments.count, ustore_modality_strings_k);
        resolved.failed())
        return resolved;
    auto [outputs, outputs_status] = strings_read_outputs_t::make(arguments);
    if (outputs_status.failed()) return outputs_status;
    call_access<cpu_class_> access = access_of(state, view);
    collection_reading_of<cpu_class_> held;
    if (status_t const one = read_at_one_state(access, collections, arguments.count, held); one.failed()) return one;
    auto read_slice = [&](std::size_t begin, std::size_t length, view_t const& slice) noexcept -> status_t {
        call_access<cpu_class_> const sliced {state,          access.transaction, access.schedule,
                                              slice.scratch,  access.stamp,       thread_index_of(state, slice),
                                              access.strings, access.reading};
        for (std::size_t first = begin; first != begin + length;) {
            std::size_t const last = run_end(collections, first, begin + length);
            if (status_t const read = read_strings(sliced, collections[first], texts, first, last, outputs);
                read.failed())
                return read;
            first = last;
        }
        return {};
    };
    // A transaction's reads join its watch set, so only a read at one stamp splits the batch
    status_t const read = access.transaction ? read_slice(0, arguments.count, view)
                                             : for_task_slices(view, arguments.count, read_slice);
    if (read.failed()) return read;
    outputs.finish(arguments);
    return {};
}

/** Implements @c ustore_strings_scan as ordered walks of each collection's strings either way. */
template <cpu_class_t cpu_class_>
status_t strings_scan(ustore_strings_scan_t const& arguments, strings_arg_t const& prefixes,
                      strings_arg_t const& starts, strings_arg_t const& ends, view_t const& view) noexcept {
    if (status_t const status = validate_direction(arguments.direction); status.failed() || !arguments.count)
        return status;
    if (!arguments.offsets || !arguments.strings)
        return {status_code_t::invalid_argument_k, "Need outputs for the offsets and the strings"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    if (status_t const resolved = resolve_runs(state, view, collections, arguments.count, ustore_modality_strings_k);
        resolved.failed())
        return resolved;
    auto [outputs, outputs_status] = strings_scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    st::vector<strings_scan_slice_t, arena_allocator<strings_scan_slice_t>> slices {{view.scratch}};
    if (st::failed(slices.resize(slice_slots(view))))
        return {status_code_t::out_of_memory_k, "Failed to allocate the slices of a scan"};
    progress_t progress {arguments.progress, arguments.progress_state, arguments.count};
    call_access<cpu_class_> access = access_of(state, view);
    collection_reading_of<cpu_class_> held;
    if (status_t const one = read_at_one_state(access, collections, arguments.count, held); one.failed()) return one;
    auto scan_slice = [&](std::size_t first_task, std::size_t length, view_t const& slice) noexcept -> status_t {
        strings_scan_slice_t& sliced = slices[slice.thread_index];
        sliced = {first_task, length, outputs.slice_in(slice.scratch)};
        call_access<cpu_class_> const sliced_access {state,          access.transaction, access.schedule,
                                                     slice.scratch,  access.stamp,       thread_index_of(state, slice),
                                                     access.strings, access.reading};
        for (std::size_t task = first_task; task != first_task + length; ++task) {
            strings_window_t const window {
                .prefix = prefixes ? string_of(prefixes[task]) : std::string_view {},
                .start = starts ? string_of(starts[task]) : std::string_view {},
                .end = ends ? string_of(ends[task]) : std::string_view {},
                .direction = arguments.direction,
            };
            sliced.outputs.open_task(task);
            if (status_t const scanned = scan_strings(sliced_access, collections[task], window, arguments.wanted,
                                                      sliced.outputs);
                scanned.failed())
                return scanned;
            if (!progress.advance(1)) return progress_cancelled_k;
        }
        return {};
    };
    // A transaction's reads join its watch set, so only a read at one stamp splits the batch
    status_t const scanned = access.transaction ? scan_slice(0, arguments.count, view)
                                                : for_task_slices(view, arguments.count, scan_slice);
    if (scanned.failed()) return scanned;
    for (strings_scan_slice_t const& slice : slices)
        if (status_t const adopted = outputs.adopt(slice.outputs, slice.first, slice.length); adopted.failed())
            return adopted;
    return outputs.finish(arguments);
}

/** Implements @c ustore_strings_sample as a reservoir filled from a walk of each collection's
 *  strings, every kept one copied, as a walk's strings live only until it moves on. */
template <cpu_class_t cpu_class_>
status_t strings_sample(ustore_strings_sample_t const& arguments, view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!arguments.offsets || !arguments.strings)
        return {status_code_t::invalid_argument_k, "Need outputs for the offsets and the strings"};
    sample_batch_t const samples = samples_of(arguments);
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    if (status_t const resolved = resolve_runs(state, view, samples.collections, arguments.count,
                                               ustore_modality_strings_k);
        resolved.failed())
        return resolved;
    auto [outputs, outputs_status] = strings_scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    sample_generator_t generator = sample_generator_t::seeded(&outputs);
    call_access<cpu_class_> access = access_of(state, view);
    collection_reading_of<cpu_class_> held;
    if (status_t const one = read_at_one_state(access, samples.collections, arguments.count, held); one.failed())
        return one;
    st::vector<kept_string_t> kept;
    for (std::size_t task = 0; task != samples.size(); ++task) {
        sample_task_t const sample = samples[task];
        outputs.open_task(task);
        kept.clear();
        std::size_t seen = 0;
        status_t room;
        if (sample.limit != 0)
            if (status_t const walked = walk_window_strings(
                    access, sample.collection, strings_window_t {},
                    [&](std::string_view text) noexcept {
                        std::size_t const slot = seen < sample.limit ? seen : st::draw_below(generator, seen + 1);
                        ++seen;
                        if (slot >= sample.limit) return true;
                        if (slot == kept.size()) room = growth_status(kept.push_back(kept_string_t {}));
                        if (room.succeeded()) room = keep_string(kept[slot], text);
                        return room.succeeded();
                    });
                walked.failed() || room.failed())
                return walked.failed() ? walked : room;
        transaction_stamp_t const watch_stamp = watch_stamp_of(access);
        for (kept_string_t const& text : kept) {
            if (status_t const pushed = outputs.push(kept_text(text)); pushed.failed()) return pushed;
            if (watch_stamp != invalid_transaction_stamp_k)
                if (status_t const watched = access.strings->watch(sample.collection, kept_text(text), watch_stamp);
                    watched.failed())
                    return watched;
        }
    }
    return outputs.finish(arguments);
}

/** Implements @c ustore_strings_measure, exact for strings and their keys at the view it reads,
 *  each string taking its bytes, its 16-byte key, its 16-byte head and its key offset in a run. */
template <cpu_class_t cpu_class_>
status_t strings_measure(ustore_strings_measure_t const& arguments, strings_arg_t const& starts,
                         strings_arg_t const& ends, view_t const& view) noexcept {
    if (status_t const status = validate_direction(arguments.direction); status.failed() || !arguments.count)
        return status;
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    if (status_t const resolved = resolve_runs(state, view, collections, arguments.count, ustore_modality_strings_k);
        resolved.failed())
        return resolved;
    auto [outputs, outputs_status] = measure_outputs_t::make(arguments);
    if (outputs_status.failed()) return outputs_status;
    call_access<cpu_class_> access = access_of(state, view);
    collection_reading_of<cpu_class_> held;
    if (status_t const one = read_at_one_state(access, collections, arguments.count, held); one.failed()) return one;
    for (std::size_t task = 0; task != arguments.count; ++task) {
        strings_window_t const window {
            .start = starts ? string_of(starts[task]) : std::string_view {},
            .end = ends ? string_of(ends[task]) : std::string_view {},
            .direction = arguments.direction,
        };
        std::size_t cardinality = 0;
        std::size_t string_bytes = 0;
        if (status_t const walked = walk_window_strings(access, collections[task], window,
                                                        [&](std::string_view text) noexcept {
                                                            ++cardinality;
                                                            string_bytes += text.size();
                                                            return true;
                                                        });
            walked.failed())
            return walked;
        if (status_t const watched = watch_window_strings(access, collections[task], window, {}, {},
                                                          page_fill_t::short_k);
            watched.failed())
            return watched;
        std::size_t const value_bytes = cardinality * sizeof(ustore_key_t);
        std::size_t const entry_bytes = sizeof(st::key128_t) + sizeof(std::uint32_t);
        outputs.assign(task, cardinality, value_bytes, string_bytes + value_bytes + cardinality * entry_bytes);
    }
    outputs.finish(arguments);
    return {};
}

#pragma endregion Strings Calls

} // namespace unum::ustore::nvme
