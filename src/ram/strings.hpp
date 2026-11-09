/**
 *  @file src/ram/strings.hpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief The strings calls of the C ABI over the strings stores of <tt>src/ram/database.hpp</tt>,
 *      for the in-memory engine's tables.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <string_view> // `std::string_view`
#include <type_traits> // `std::is_same_v`

#include "ustore/ranges.hpp" // `strided`, `strided_iterator`
#include "ustore/strings.h"

#include "ustore/strings/ram_store.hpp" // `string_pair_t`

#include "database.hpp"           // `database_state`, `with_source`, `serve_write`, `read_strings`, `scan_strings`
#include "utilities/batches.hpp"  // `strings_arg_t`, `validate_choices`, `validate_direction`, `samples_of`
#include "utilities/outputs.hpp"  // `strings_read_outputs_t`, `strings_scan_outputs_t`, `measure_outputs_t`
#include "utilities/progress.hpp" // `progress_t`, `progress_cancelled_k`
#include "utilities/slices.hpp"   // `for_task_slices`

namespace unum::ustore::ram {

#pragma region Strings Calls

/** Hands @p stage every run of @p count tasks on a strings collection through @p source, with the part of the
 *  transaction over its store, refusing a collection that holds no strings. */
template <typename source_type_, typename stage_type_>
store_status_t stage_string_runs(source_type_& source, strided_iterator<ustore_collection_id_t const> collections,
                                 std::size_t count, status_t& outcome, stage_type_&& stage) noexcept {
    return for_each_run(source, collections, count, outcome,
                        [&]<typename entries_type_>(entries_type_& entries, std::size_t first,
                                                    std::size_t end) noexcept -> store_status_t {
                            using entry_t = entry_of<entries_type_>;
                            if constexpr (!std::is_same_v<entry_t, string_pair_t>) return refused_modality(outcome);
                            else {
                                auto part = source.part(entries);
                                if (!part) return part.status();
                                return stage(**part, first, end);
                            }
                        });
}

/** Implements @c ustore_strings_write, committing a batch outside a transaction atomically. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_strings_write_t const& arguments,
               strings_arg_t const& texts, view_t const& view) noexcept {
    if (status_t const status = validate_choices(view.transaction, arguments.durability, view.watch); status.failed())
        return status;
    if (!arguments.count) return {};
    strided_iterator<ustore_key_t const> const values = strided(arguments.values);
    if (!texts || !values) return {status_code_t::invalid_argument_k, "Need the strings and their values"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    return serve_write(state, arguments.durability, view, [&](auto& source, status_t& outcome) noexcept {
        return stage_string_runs(source, collections, arguments.count, outcome,
                                 [&](auto& part, std::size_t first, std::size_t end) noexcept {
                                     return stage_strings(part, texts, values, first, end);
                                 });
    });
}

/** Implements @c ustore_strings_remove, committing a batch outside a transaction atomically, where a string already
 *  gone stays gone, and every string of a range goes when the batch gives end strings. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_strings_remove_t const& arguments,
               strings_arg_t const& texts, strings_arg_t const& end_texts, view_t const& view) noexcept {
    if (status_t const status = validate_choices(view.transaction, arguments.durability, view.watch); status.failed())
        return status;
    if (!arguments.count) return {};
    if (!texts) return {status_code_t::invalid_argument_k, "No strings were provided"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    return serve_write(state, arguments.durability, view, [&](auto& source, status_t& outcome) noexcept {
        return stage_string_runs(source, collections, arguments.count, outcome,
                                 [&](auto& part, std::size_t first, std::size_t end) noexcept {
                                     if (end_texts)
                                         return stage_string_range_removals(part, texts, end_texts, first, end);
                                     return stage_string_removals(part, texts, first, end, source.watch_mode);
                                 });
    });
}

/** Implements @c ustore_strings_read, writing the key every string maps onto into the caller's arena. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_strings_read_t const& arguments,
               strings_arg_t const& texts, view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!texts) return {status_code_t::invalid_argument_k, "No strings were provided"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    status_t outcome;
    auto [outputs, outputs_status] = strings_read_outputs_t::make(arguments);
    if (outputs_status.failed()) return outputs_status;

    store_status_t const read = with_source(state, view, [&]<typename source_type_>(source_type_& source) noexcept {
        auto read_slice = [&](std::size_t first_task, std::size_t length, view_t const&) noexcept -> status_t {
            status_t sliced;
            store_status_t const runs = for_each_run(
                source, collections + static_cast<std::ptrdiff_t>(first_task), length, sliced,
                [&]<typename entries_type_>(entries_type_& entries, std::size_t first,
                                            std::size_t end) noexcept -> store_status_t {
                    using entry_t = entry_of<entries_type_>;
                    if constexpr (!std::is_same_v<entry_t, string_pair_t>) return refused_modality(sliced);
                    else {
                        auto store = source.view(entries);
                        if (!store) return store.status();
                        return read_strings(*store, texts, first_task + first, first_task + end, outputs);
                    }
                });
            return reported(runs, sliced);
        };
        // A transaction's reads join its watch set, so only a reader at one stamp splits the batch
        if constexpr (std::is_same_v<source_type_, reader_source<cpu_class_, isolation_>>)
            outcome = for_task_slices(view, arguments.count, read_slice);
        else outcome = read_slice(0, arguments.count, view);
        return store_status_t::success_k;
    });
    if (status_t const status = reported(read, outcome); status.failed()) return status;
    outputs.finish(arguments);
    return {};
}

/** Implements @c ustore_strings_scan as ordered walks of each collection's strings either way. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_strings_scan_t const& arguments,
               strings_arg_t const& prefixes, strings_arg_t const& starts, strings_arg_t const& ends,
               view_t const& view) noexcept {
    if (status_t const status = validate_direction(arguments.direction); status.failed() || !arguments.count)
        return status;
    if (!arguments.offsets || !arguments.strings)
        return {status_code_t::invalid_argument_k, "Need outputs for the offsets and the strings"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    auto [outputs, outputs_status] = strings_scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    st::vector<strings_scan_slice_t, arena_allocator<strings_scan_slice_t>> slices {{view.scratch}};
    if (st::failed(slices.resize(slice_slots(view))))
        return {status_code_t::out_of_memory_k, "Failed to allocate the slices of a scan"};
    progress_t progress {arguments.progress, arguments.progress_state, arguments.count};

    status_t outcome;
    store_status_t const walked = with_source(state, view, [&]<typename source_type_>(source_type_& source) noexcept {
        auto scan_slice = [&](std::size_t first_task, std::size_t length, view_t const& slice) noexcept -> status_t {
            strings_scan_slice_t& sliced = slices[slice.thread_index];
            sliced = {first_task, length, outputs.slice_in(slice.scratch)};
            status_t slice_outcome;
            store_status_t const runs = for_each_run(
                source, collections + static_cast<std::ptrdiff_t>(first_task), length, slice_outcome,
                [&]<typename entries_type_>(entries_type_& entries, std::size_t first,
                                            std::size_t end) noexcept -> store_status_t {
                    using entry_t = entry_of<entries_type_>;
                    if constexpr (!std::is_same_v<entry_t, string_pair_t>) return refused_modality(slice_outcome);
                    else {
                        auto store = source.view(entries);
                        if (!store) return store.status();
                        for (std::size_t task = first_task + first; task != first_task + end; ++task) {
                            strings_window_t const window {
                                .prefix = prefixes ? string_of(prefixes[task]) : std::string_view {},
                                .start = starts ? string_of(starts[task]) : std::string_view {},
                                .end = ends ? string_of(ends[task]) : std::string_view {},
                                .direction = arguments.direction,
                            };
                            sliced.outputs.open_task(task);
                            store_status_t const scanned = scan_strings(*store, window, arguments.wanted, slice.scratch,
                                                                        sliced.outputs, slice_outcome);
                            if (st::failed(scanned) || slice_outcome.failed()) return scanned;
                            if (progress.advance(1)) continue;
                            slice_outcome = progress_cancelled_k;
                            break;
                        }
                        return store_status_t::success_k;
                    }
                });
            return reported(runs, slice_outcome);
        };
        // A transaction's reads join its watch set, so only a reader at one stamp splits the batch
        if constexpr (std::is_same_v<source_type_, reader_source<cpu_class_, isolation_>>)
            outcome = for_task_slices(view, arguments.count, scan_slice);
        else outcome = scan_slice(0, arguments.count, view);
        return store_status_t::success_k;
    });
    if (status_t const status = reported(walked, outcome); status.failed()) return status;
    for (strings_scan_slice_t const& slice : slices)
        if (status_t const adopted = outputs.adopt(slice.outputs, slice.first, slice.length); adopted.failed())
            return adopted;
    return outputs.finish(arguments);
}

/** Implements @c ustore_strings_sample as a reservoir filled walking each collection's strings. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_strings_sample_t const& arguments,
               view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!arguments.offsets || !arguments.strings)
        return {status_code_t::invalid_argument_k, "Need outputs for the offsets and the strings"};
    sample_batch_t const samples = samples_of(arguments);
    auto [outputs, outputs_status] = strings_scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    sample_generator_t generator = sample_generator_t::seeded(&outputs);

    status_t outcome;
    store_status_t const walked = with_source(state, view, [&](auto& source) noexcept {
        return for_each_run(source, samples.collections, samples.size(), outcome,
                            [&]<typename entries_type_>(entries_type_& entries, std::size_t first,
                                                        std::size_t end) noexcept -> store_status_t {
                                using entry_t = entry_of<entries_type_>;
                                if constexpr (!std::is_same_v<entry_t, string_pair_t>) return refused_modality(outcome);
                                else {
                                    auto store = source.view(entries);
                                    if (!store) return store.status();
                                    for (std::size_t task = first; task != end; ++task) {
                                        outputs.open_task(task);
                                        store_status_t const sampled = sample_strings(
                                            *store, samples[task].limit, generator, view.scratch, outputs, outcome);
                                        if (st::failed(sampled) || outcome.failed()) return sampled;
                                    }
                                    return store_status_t::success_k;
                                }
                            });
    });
    if (status_t const status = reported(walked, outcome); status.failed()) return status;
    return outputs.finish(arguments);
}

/** Implements @c ustore_strings_measure, exact for strings and their keys at the view it reads. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_strings_measure_t const& arguments,
               strings_arg_t const& starts, strings_arg_t const& ends, view_t const& view) noexcept {
    if (status_t const status = validate_direction(arguments.direction); status.failed() || !arguments.count)
        return status;
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    auto [outputs, outputs_status] = measure_outputs_t::make(arguments);
    if (outputs_status.failed()) return outputs_status;

    status_t outcome;
    store_status_t const walked = with_source(state, view, [&](auto& source) noexcept {
        return for_each_run(source, collections, arguments.count, outcome,
                            [&]<typename entries_type_>(entries_type_& entries, std::size_t first,
                                                        std::size_t end) noexcept -> store_status_t {
                                using entry_t = entry_of<entries_type_>;
                                if constexpr (!std::is_same_v<entry_t, string_pair_t>) return refused_modality(outcome);
                                else {
                                    auto store = source.view(entries);
                                    if (!store) return store.status();
                                    for (std::size_t task = first; task != end; ++task) {
                                        strings_window_t const window {
                                            .start = starts ? string_of(starts[task]) : std::string_view {},
                                            .end = ends ? string_of(ends[task]) : std::string_view {},
                                            .direction = arguments.direction,
                                        };
                                        window_measure_t measured;
                                        store_status_t const measuring = measure_strings(*store, window, measured);
                                        if (st::failed(measuring)) return measuring;
                                        outputs.assign(task, measured.cardinality, measured.value_bytes,
                                                       measured.space_bytes);
                                    }
                                    return store_status_t::success_k;
                                }
                            });
    });
    if (status_t const status = reported(walked, outcome); status.failed()) return status;
    outputs.finish(arguments);
    return {};
}

#pragma endregion Strings Calls

#pragma region Table

template <cpu_class_t cpu_class_, isolation_t isolation_>
strings_verbs<home_t::host_k> const ram_variant<cpu_class_, isolation_>::strings_verbs_k {
    .write = &serve_database<cpu_class_, isolation_, ustore_strings_write_t, strings_arg_t>,
    .remove = &serve_database<cpu_class_, isolation_, ustore_strings_remove_t, strings_arg_t, strings_arg_t>,
    .read = &serve_database<cpu_class_, isolation_, ustore_strings_read_t, strings_arg_t>,
    .scan = &serve_database<cpu_class_, isolation_, ustore_strings_scan_t, strings_arg_t, strings_arg_t, strings_arg_t>,
    .sample = &serve_database<cpu_class_, isolation_, ustore_strings_sample_t>,
    .measure = &serve_database<cpu_class_, isolation_, ustore_strings_measure_t, strings_arg_t, strings_arg_t>,
};

#pragma endregion Table

} // namespace unum::ustore::ram
