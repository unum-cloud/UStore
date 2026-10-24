/**
 *  @file src/nvme/pages_calls.hpp
 *  @author Ash Vardanian
 *  @date October 20, 2026
 *  @brief The page edits of the C ABI over the pages stores of `include/ustore/pages/nvme_store.hpp`, for the NVMe
 *      engine's table.
 *
 *  `src/nvme/variants.hpp` includes this file right before its table, as these calls build on its writes and its
 *  catalog lookups. An edit call addressing only pages collections runs through the `*_in_page_edits` calls of
 *  `src/pages.cpp`, which read the pages unwatched, check and apply the edits, and hand the edits of every page to
 *  @c commit_page_edits as one chain. A chain that imports or removes the page decides it whole, so the page it leaves
 *  is written, or removed, and any other chain is appended as one @c page_fold change record, watching nothing, so two
 *  commits editing one page both land. A call addressing any other collection, the main one included, rewrites its
 *  pages through the `*_in_page_values` calls, as RAM does. Reads stay with `pages_read_in_page_values`, which reads
 *  through the engine's own blobs reads, folding the records.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <span> // `std::span`

#include "ustore/pages.h" // `ustore_pages_import_t`, `ustore_pages_insert_t`

#include "ustore/pages/page_fold.hpp"       // `page_fold`
#include "ustore/utilities/ram_buffers.hpp" // `arena_allocator`

namespace unum::ustore::nvme {

using ustore::pages::page_fold;

/** The fold every change record of a pages collection is written for. */
inline constexpr page_fold pages_fold_k {};

/** Implements @c page_edits_commit_t over a database of @p cpu_class_, committing the chains @p edited lists in one
 *  transaction, the caller's or one of their own flushed when @p durability asks. */
template <cpu_class_t cpu_class_>
status_t commit_page_edits(ustore_database_t database, std::span<page_edits_t const> edited,
                           ustore_durability_t durability, view_t const& view) noexcept {
    database_state<cpu_class_>& state = *handle_cast<database_state<cpu_class_>>(database);
    std::size_t bytes = 0;
    for (page_edits_t const& page : edited) bytes += page.chain.size();
    return serve_write(
        state, view, durability, edited.size(), bytes, watch_schedule_t::never_k,
        [&](call_access<cpu_class_> const& access) noexcept -> status_t {
            for (page_edits_t const& page : edited) {
                collection_id_t const collection = page.collection;
                stored_key_t const key = stored_of(page.key.uuid);
                delta_view_t const change {delta_kind_t::change_k, page.chain};
                if (!pages_fold_k.closes(change)) {
                    if (status_t const staged = access.transaction->change(collection, key, page.chain, pages_fold_k);
                        staged.failed())
                        return staged;
                    continue;
                }
                arena_allocator<byte_t> arena {access.scratch};
                auto [tree, tree_status] = pages_fold_k.apply({}, std::span<delta_view_t const> {&change, 1}, arena);
                if (tree_status.failed() && tree_status != status_code_t::not_found_k) return tree_status;
                value_view_t const value = tree_status.succeeded() ? value_view_t {tree} : value_view_t {};
                if (status_t const written = write_value(access, collection, key, value); written.failed())
                    return written;
            }
            return {};
        });
}

/** Whether every task of @p arguments names a pages collection, which keeps a page as its edits. */
template <cpu_class_t cpu_class_, typename arguments_type_>
bool keeps_page_edits(arguments_type_ const& arguments, view_t const& view) noexcept {
    return holds_only(state_of<cpu_class_>(arguments), view, strided(arguments.collections), arguments.count,
                      ustore_modality_pages_k);
}

/** Implements @c ustore_pages_import, writing every imported page whole. */
template <cpu_class_t cpu_class_>
status_t pages_import(ustore_pages_import_t const& arguments, strings_arg_t const& values,
                      view_t const& view) noexcept {
    if (!keeps_page_edits<cpu_class_>(arguments, view)) return pages_import_in_page_values(arguments, values, view);
    return pages_import_in_page_edits(arguments, values, view, &commit_page_edits<cpu_class_>);
}

/** Implements @c ustore_pages_insert, appending the insertions of every page. */
template <cpu_class_t cpu_class_>
status_t pages_insert(ustore_pages_insert_t const& arguments, strings_arg_t const& values,
                      view_t const& view) noexcept {
    if (!keeps_page_edits<cpu_class_>(arguments, view)) return pages_insert_in_page_values(arguments, values, view);
    return pages_insert_in_page_edits(arguments, values, view, &commit_page_edits<cpu_class_>);
}

/** Implements @c ustore_pages_remove, appending the removals of every page, or removing the pages Nil names. */
template <cpu_class_t cpu_class_>
status_t pages_remove(ustore_pages_remove_t const& arguments, view_t const& view) noexcept {
    if (!keeps_page_edits<cpu_class_>(arguments, view)) return pages_remove_in_page_values(arguments, view);
    return pages_remove_in_page_edits(arguments, view, &commit_page_edits<cpu_class_>);
}

/** Implements @c ustore_pages_move, appending the moves of every page. */
template <cpu_class_t cpu_class_>
status_t pages_move(ustore_pages_move_t const& arguments, view_t const& view) noexcept {
    if (!keeps_page_edits<cpu_class_>(arguments, view)) return pages_move_in_page_values(arguments, view);
    return pages_move_in_page_edits(arguments, view, &commit_page_edits<cpu_class_>);
}

/** Implements @c ustore_pages_splice, appending the splices of every page. */
template <cpu_class_t cpu_class_>
status_t pages_splice(ustore_pages_splice_t const& arguments, strings_arg_t const& values,
                      view_t const& view) noexcept {
    if (!keeps_page_edits<cpu_class_>(arguments, view)) return pages_splice_in_page_values(arguments, values, view);
    return pages_splice_in_page_edits(arguments, values, view, &commit_page_edits<cpu_class_>);
}

/** Implements @c ustore_pages_set_attributes, appending the attributes set on every page. */
template <cpu_class_t cpu_class_>
status_t pages_set_attributes(ustore_pages_set_attributes_t const& arguments, strings_arg_t const& fields,
                              strings_arg_t const& values, view_t const& view) noexcept {
    if (!keeps_page_edits<cpu_class_>(arguments, view))
        return pages_set_attributes_in_page_values(arguments, fields, values, view);
    return pages_set_attributes_in_page_edits(arguments, fields, values, view, &commit_page_edits<cpu_class_>);
}

/** Implements @c ustore_pages_remove_attributes, appending the attributes removed from every page. */
template <cpu_class_t cpu_class_>
status_t pages_remove_attributes(ustore_pages_remove_attributes_t const& arguments, strings_arg_t const& fields,
                                 view_t const& view) noexcept {
    if (!keeps_page_edits<cpu_class_>(arguments, view))
        return pages_remove_attributes_in_page_values(arguments, fields, view);
    return pages_remove_attributes_in_page_edits(arguments, fields, view, &commit_page_edits<cpu_class_>);
}

} // namespace unum::ustore::nvme
