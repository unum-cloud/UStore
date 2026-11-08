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
    transaction_stamp_t const stamp = read_stamp_of(access);
    transaction_stamp_t const watch_stamp = watch_stamp_of(access);
    return visit_strings(access, collection, [&](auto const& store) noexcept -> status_t {
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
                present = store.find(text, bytes, stamp, access.thread_index) == lookup_result_t::written_k;
            }
            if (present) outputs.push_present(value);
            else outputs.push_missing();
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
    call_access<cpu_class_> const access = access_of(state, view);
    for (std::size_t first = 0; first != arguments.count;) {
        std::size_t const last = run_end(collections, first, arguments.count);
        if (status_t const read = read_strings(access, collections[first], texts, first, last, outputs); read.failed())
            return read;
        first = last;
    }
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
    progress_t progress {arguments.progress, arguments.progress_state, arguments.count};
    call_access<cpu_class_> const access = access_of(state, view);
    for (std::size_t task = 0; task != arguments.count; ++task) {
        strings_window_t const window {
            .prefix = prefixes ? string_of(prefixes[task]) : std::string_view {},
            .start = starts ? string_of(starts[task]) : std::string_view {},
            .end = ends ? string_of(ends[task]) : std::string_view {},
            .direction = arguments.direction,
        };
        outputs.open_task(task);
        if (status_t const scanned = scan_strings(access, collections[task], window, arguments.wanted, outputs);
            scanned.failed())
            return scanned;
        if (!progress.advance(1)) return progress_cancelled_k;
    }
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
    call_access<cpu_class_> const access = access_of(state, view);
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
    call_access<cpu_class_> const access = access_of(state, view);
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
