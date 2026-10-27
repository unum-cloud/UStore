/**
 *  @file src/ram/pages.hpp
 *  @author Ash Vardanian
 *  @date October 25, 2026
 *  @brief The pages calls of the C ABI over the pages stores of <tt>src/ram/database.hpp</tt>, for
 *      the in-memory engine's tables.
 *
 *  Every page is its dense element tree, filed under its key in the pages store of its collection.
 *  An edit call reads the trees of the pages it touches, watched, applies its tasks by
 *  <tt>ustore/pages/page_edits.hpp</tt> through the paths of <tt>src/blobs_layers/pages.hpp</tt>,
 *  and writes every new tree back or erases the pages it removed, inside one SmashTable
 *  transaction: the caller's, or one of the call's own, retried on conflicts. A read renders the
 *  trees as the view sees them. A call naming a collection of another modality is refused.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <span> // `std::span`

#include "ustore/pages.h"    // `ustore_pages_import_t`, `ustore_pages_read_t`
#include "ustore/ranges.hpp" // `place_batch_t`, `values_view_t`, `strided_iterator`

#include "blobs_layers/pages.hpp" // `touched_page_t`, `serve_import`, `serve_insert`, `read_pages`

#include "database.hpp" // `database_state`, `with_source`, `read_into_scratch`, `stage_runs_of`, `serve_read_modify_write`

namespace unum::ustore::ram {

#pragma region Page Calls

/** Reads the trees of the pages at @p places through @p source watching as @p view asks, appending
 *  them to its scratch, and recording into @p failure the failure of the stores. */
template <typename source_type_>
expected<values_view_t> read_page_trees(source_type_& source, place_batch_t const& places, view_t const& view,
                                        store_status_t& failure) noexcept {
    ustore_octet_t const* presences = nullptr;
    ustore_strings_view_t trees {};
    status_t const status = read_into_scratch<page_pair_t>(source, places, view, presences, trees, failure);
    if (status.failed()) return status;
    return values_view_t {places.count, presences, trees};
}

/** Lands an edit call on the pages stores of a transaction, reading the pages watched, as a
 *  transaction watches the pages it rewrites whatever the caller asked of the writes, and staging
 *  every new tree, recording into @c failure the first failure of the stores, which a commit
 *  retries when it is a conflict. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
struct pages_in_store {
    static constexpr ustore_watch_t reads_watch_k = ustore_watch_keys_k;

    transaction_source<cpu_class_, isolation_>& source;
    store_status_t& failure;

    /** Reads the trees of the pages at @p places through @p view. */
    expected<values_view_t> read(place_batch_t const& places, view_t const& view) const noexcept {
        return read_page_trees(source, places, view, failure);
    }

    /** Stages the tree of every page of @p pages that exists, and erases those that stopped existing. */
    template <typename arguments_type_>
    status_t operator()(arguments_type_ const&, std::span<touched_page_t> pages, view_t const& view) const noexcept {
        page_allocator_t allocator {view.scratch};
        strided_iterator<touched_page_t const> const touched {pages.data(), sizeof(touched_page_t)};
        status_t outcome;
        store_status_t const staged = stage_runs_of<page_pair_t>(
            source, touched.members(&touched_page_t::place).members(&collection_key_t::collection), pages.size(),
            outcome, [&](auto& part, std::size_t first, std::size_t end) noexcept -> store_status_t {
                for (std::size_t index = first; index != end; ++index) {
                    touched_page_t& page = pages[index];
                    if (!page.state.exists) {
                        if (!page.existed) continue;
                        store_status_t const erased = part.erase(page.place.key);
                        if (st::failed(erased)) return erased;
                        continue;
                    }
                    auto [tree, tree_status] = page.state.tree.encode(allocator);
                    if (tree_status.failed()) {
                        outcome = tree_status;
                        return store_status_t::invalid_argument_k;
                    }
                    store_status_t const written = stage_upsert<page_pair_t>(part, page.place.key, value_view_t {tree});
                    if (st::failed(written)) return written;
                }
                return store_status_t::success_k;
            });
        if (outcome.failed()) return outcome;
        if (st::failed(staged)) failure = staged;
        return translate(staged);
    }
};

/** Serves an edit call of @p arguments through @p serve_edits, called as <tt>serve_edits(land)</tt>
 *  with the lander of the pages stores, inside one transaction. */
template <cpu_class_t cpu_class_, isolation_t isolation_, typename arguments_type_, typename serve_edits_type_>
status_t edit_in_store(database_state<cpu_class_, isolation_>& state, arguments_type_ const& arguments,
                       view_t const& view, serve_edits_type_&& serve_edits) noexcept {
    if (!arguments.count) return {};
    return serve_read_modify_write(
        state, arguments.durability, view,
        [&](transaction_source<cpu_class_, isolation_>& source, store_status_t& failure) noexcept {
            return serve_edits(pages_in_store<cpu_class_, isolation_> {source, failure});
        });
}

/** Implements @c ustore_pages_import, writing every imported page whole. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_pages_import_t const& arguments,
               strings_arg_t const& values, view_t const& view) noexcept {
    return edit_in_store(state, arguments, view, [&](pages_in_store<cpu_class_, isolation_> const& land) noexcept {
        return serve_import(arguments, values, view, land);
    });
}

/** Implements @c ustore_pages_insert. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_pages_insert_t const& arguments,
               strings_arg_t const& values, view_t const& view) noexcept {
    return edit_in_store(state, arguments, view, [&](pages_in_store<cpu_class_, isolation_> const& land) noexcept {
        return serve_insert(arguments, values, view, land);
    });
}

/** Implements @c ustore_pages_remove, where a Nil element removes the whole page. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_pages_remove_t const& arguments,
               view_t const& view) noexcept {
    return edit_in_store(state, arguments, view, [&](pages_in_store<cpu_class_, isolation_> const& land) noexcept {
        return serve_remove(arguments, view, land);
    });
}

/** Implements @c ustore_pages_move. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_pages_move_t const& arguments,
               view_t const& view) noexcept {
    return edit_in_store(state, arguments, view, [&](pages_in_store<cpu_class_, isolation_> const& land) noexcept {
        return serve_move(arguments, view, land);
    });
}

/** Implements @c ustore_pages_splice. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_pages_splice_t const& arguments,
               strings_arg_t const& values, view_t const& view) noexcept {
    return edit_in_store(state, arguments, view, [&](pages_in_store<cpu_class_, isolation_> const& land) noexcept {
        return serve_splice(arguments, values, view, land);
    });
}

/** Implements @c ustore_pages_set_attributes. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_pages_set_attributes_t const& arguments,
               strings_arg_t const& fields, strings_arg_t const& values, view_t const& view) noexcept {
    return edit_in_store(state, arguments, view, [&](pages_in_store<cpu_class_, isolation_> const& land) noexcept {
        return serve_set_attributes(arguments, fields, values, view, land);
    });
}

/** Implements @c ustore_pages_remove_attributes. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_pages_remove_attributes_t const& arguments,
               strings_arg_t const& fields, view_t const& view) noexcept {
    return edit_in_store(state, arguments, view, [&](pages_in_store<cpu_class_, isolation_> const& land) noexcept {
        return serve_remove_attributes(arguments, fields, view, land);
    });
}

/** Implements @c ustore_pages_read, rendering every element its tasks name. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_pages_read_t const& arguments,
               view_t const& view) noexcept {
    return with_source(state, view, [&]<typename source_type_>(source_type_& source) noexcept -> status_t {
        store_status_t failure = store_status_t::success_k;
        return read_pages(arguments, view,
                          [&]() noexcept { return read_page_trees(source, places_of(arguments), view, failure); });
    });
}

/** Implements @c ustore_pages_scan as ordered walks of each collection's keys. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_pages_scan_t const& arguments,
               view_t const& view) noexcept {
    return scan_keys<page_pair_t>(state, arguments, view);
}

#pragma endregion Page Calls

#pragma region Table

template <cpu_class_t cpu_class_, isolation_t isolation_>
pages_verbs<home_t::host_k> const ram_variant<cpu_class_, isolation_>::pages_verbs_k {
    .import = &serve_database<cpu_class_, isolation_, ustore_pages_import_t, strings_arg_t>,
    .insert = &serve_database<cpu_class_, isolation_, ustore_pages_insert_t, strings_arg_t>,
    .remove = &serve_database<cpu_class_, isolation_, ustore_pages_remove_t>,
    .move = &serve_database<cpu_class_, isolation_, ustore_pages_move_t>,
    .splice = &serve_database<cpu_class_, isolation_, ustore_pages_splice_t, strings_arg_t>,
    .set_attributes =
        &serve_database<cpu_class_, isolation_, ustore_pages_set_attributes_t, strings_arg_t, strings_arg_t>,
    .remove_attributes = &serve_database<cpu_class_, isolation_, ustore_pages_remove_attributes_t, strings_arg_t>,
    .read = &serve_database<cpu_class_, isolation_, ustore_pages_read_t>,
    .scan = &serve_database<cpu_class_, isolation_, ustore_pages_scan_t>,
};

#pragma endregion Table

} // namespace unum::ustore::ram
