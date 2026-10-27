/**
 *  @file src/ram/strings.hpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief The strings calls of the C ABI over the strings stores of `src/ram/database.hpp`, for the in-memory engine's
 *      tables.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <string_view> // `std::string_view`
#include <type_traits> // `std::is_same_v`

#include "ustore/ranges.hpp" // `strided`, `strided_iterator`
#include "ustore/strings.h"

#include "database.hpp"           // `database_state`, `with_source`, `serve_write`, `read_strings`, `scan_strings`
#include "utilities/batches.hpp"  // `strings_arg_t`
#include "utilities/outputs.hpp"  // `strings_read_outputs_t`, `strings_scan_outputs_t`
#include "utilities/progress.hpp" // `progress_t`, `progress_cancelled_k`

namespace unum::ustore::ram {

#pragma region Strings Calls

/** Hands @p stage every run of @p count tasks on a strings collection through @p source, with the part of the
 *  transaction over its store, refusing a collection that holds no strings. */
template <typename source_type_, typename stage_type_>
store_status_t stage_string_runs(source_type_& source, strided_iterator<ustore_collection_id_t const> collections,
                                 std::size_t count, status_t& outcome, stage_type_&& stage) noexcept {
    return for_each_run(source, collections, count, outcome,
                        [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
                            using entry_t = entry_of<decltype(entries)>;
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
 *  gone stays gone. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_strings_remove_t const& arguments,
               strings_arg_t const& texts, view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!texts) return {status_code_t::invalid_argument_k, "No strings were provided"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    return serve_write(state, arguments.durability, view, [&](auto& source, status_t& outcome) noexcept {
        return stage_string_runs(source, collections, arguments.count, outcome,
                                 [&](auto& part, std::size_t first, std::size_t end) noexcept {
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

    store_status_t const read = with_source(state, view, [&](auto& source) noexcept {
        return for_each_run(source, collections, arguments.count, outcome,
                            [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
                                using entry_t = entry_of<decltype(entries)>;
                                if constexpr (!std::is_same_v<entry_t, string_pair_t>) return refused_modality(outcome);
                                else {
                                    auto store = source.view(entries);
                                    if (!store) return store.status();
                                    return read_strings(*store, texts, first, end, outputs);
                                }
                            });
    });
    if (status_t const status = reported(read, outcome); status.failed()) return status;
    outputs.finish(arguments);
    return {};
}

/** Implements @c ustore_strings_scan as ordered walks of each collection's strings. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_strings_scan_t const& arguments,
               strings_arg_t const& prefixes, strings_arg_t const& starts, view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!arguments.offsets || !arguments.strings)
        return {status_code_t::invalid_argument_k, "Need outputs for the offsets and the strings"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    auto [outputs, outputs_status] = strings_scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    progress_t progress {arguments.progress, arguments.progress_state, arguments.count};

    status_t outcome;
    store_status_t const walked = with_source(state, view, [&](auto& source) noexcept {
        return for_each_run(
            source, collections, arguments.count, outcome,
            [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
                using entry_t = entry_of<decltype(entries)>;
                if constexpr (!std::is_same_v<entry_t, string_pair_t>) return refused_modality(outcome);
                else {
                    auto store = source.view(entries);
                    if (!store) return store.status();
                    for (std::size_t task = first; task != end && outcome.succeeded(); ++task) {
                        std::string_view const prefix = prefixes ? string_of(prefixes[task]) : std::string_view {};
                        std::string_view const start = starts ? string_of(starts[task]) : std::string_view {};
                        outputs.open_task(task);
                        store_status_t const scanned = scan_strings(*store, prefix, start, arguments.wanted, outputs,
                                                                    outcome);
                        if (st::failed(scanned) || outcome.failed()) return scanned;
                        if (!progress.advance(1)) outcome = progress_cancelled_k;
                    }
                    return store_status_t::success_k;
                }
            });
    });
    if (status_t const status = reported(walked, outcome); status.failed()) return status;
    return outputs.finish(arguments);
}

#pragma endregion Strings Calls

#pragma region Table

template <cpu_class_t cpu_class_, isolation_t isolation_>
strings_verbs<home_t::host_k> const ram_variant<cpu_class_, isolation_>::strings_verbs_k {
    .write = &serve_database<cpu_class_, isolation_, ustore_strings_write_t, strings_arg_t>,
    .remove = &serve_database<cpu_class_, isolation_, ustore_strings_remove_t, strings_arg_t>,
    .read = &serve_database<cpu_class_, isolation_, ustore_strings_read_t, strings_arg_t>,
    .scan = &serve_database<cpu_class_, isolation_, ustore_strings_scan_t, strings_arg_t, strings_arg_t>,
};

#pragma endregion Table

} // namespace unum::ustore::ram
