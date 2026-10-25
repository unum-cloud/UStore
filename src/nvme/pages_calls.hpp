/**
 *  @file src/nvme/pages_calls.hpp
 *  @author Ash Vardanian
 *  @date October 20, 2026
 *  @brief The page edits of the C ABI over the pages stores of `include/ustore/pages/nvme_store.hpp`, for the NVMe
 *      engine's table.
 *
 *  `src/nvme/variants.hpp` includes this file right before its table, as these calls build on its writes and its
 *  catalog lookups. An edit call addressing any collection but a pages one is refused. The `*_edits` calls of
 *  `src/pages.cpp` read the pages unwatched through the engine's own reads, check and apply the edits, and answer the
 *  edits of every page as one chain, which @c commit_page_edits lands. A chain that imports or removes the page decides
 *  it whole, so the page it leaves is written, or removed, and any other chain is appended as one @c page_fold_t change
 *  delta, watching nothing, so two commits editing one page both land. Reads take `pages_read_over_blobs` over the
 *  engine's own reads of the pages store, which fold the deltas.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <span> // `std::span`

#include "ustore/pages.h" // `ustore_pages_import_t`, `ustore_pages_insert_t`

#include "ustore/pages/page_fold.hpp"       // `page_fold_t`
#include "ustore/utilities/ram_buffers.hpp" // `arena_allocator`

namespace unum::ustore::nvme {

using ustore::pages::page_fold_t;

/** The fold every change delta of a pages collection is written for. */
inline constexpr page_fold_t pages_fold_k {};

/** Refuses a page call of @p arguments addressing any collection but a pages one, then commits the chains @p edits
 *  answers, called as `edits()`, in one transaction, the caller's or one of their own flushed when the call asks. */
template <cpu_class_t cpu_class_, typename arguments_type_, typename edits_type_>
status_t commit_page_edits(arguments_type_ const& arguments, view_t const& view, edits_type_&& edits) noexcept {
    database_state<cpu_class_>& state = state_of<cpu_class_>(arguments);
    if (status_t const resolved = resolve_runs(state, view, strided(arguments.collections), arguments.count,
                                               ustore_modality_pages_k);
        resolved.failed())
        return resolved;
    auto [edited, edits_status] = edits();
    if (edits_status.failed() || edited.empty()) return edits_status;
    std::size_t bytes = 0;
    for (page_edits_t const& page : edited) bytes += page.chain.size();
    return serve_write(
        state, view, arguments.durability, edited.size(), bytes, watch_schedule_t::never_k,
        [&](call_access<cpu_class_> const& access) noexcept -> status_t {
            for (page_edits_t const& page : edited) {
                collection_id_t const collection = page.collection;
                stored_key_t const key = stored_of(page.key.uuid);
                delta_view_t const change {delta_kind_t::change_k, page.chain};
                if (!pages_fold_k.closes(change)) {
                    if (status_t const changed = access.transaction->change(collection, key, page.chain, pages_fold_k);
                        changed.failed())
                        return changed;
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

/** Implements @c ustore_pages_import, writing every imported page whole. */
template <cpu_class_t cpu_class_>
status_t pages_import(ustore_pages_import_t const& arguments, strings_arg_t const& values,
                      view_t const& view) noexcept {
    return commit_page_edits<cpu_class_>(arguments, view, [&]() noexcept {
        return pages_import_edits(nvme_variant<cpu_class_>::values_verbs_k, arguments, values, view);
    });
}

/** Implements @c ustore_pages_insert, appending the insertions of every page. */
template <cpu_class_t cpu_class_>
status_t pages_insert(ustore_pages_insert_t const& arguments, strings_arg_t const& values,
                      view_t const& view) noexcept {
    return commit_page_edits<cpu_class_>(arguments, view, [&]() noexcept {
        return pages_insert_edits(nvme_variant<cpu_class_>::values_verbs_k, arguments, values, view);
    });
}

/** Implements @c ustore_pages_remove, appending the removals of every page, or removing the pages Nil names. */
template <cpu_class_t cpu_class_>
status_t pages_remove(ustore_pages_remove_t const& arguments, view_t const& view) noexcept {
    return commit_page_edits<cpu_class_>(arguments, view, [&]() noexcept {
        return pages_remove_edits(nvme_variant<cpu_class_>::values_verbs_k, arguments, view);
    });
}

/** Implements @c ustore_pages_move, appending the moves of every page. */
template <cpu_class_t cpu_class_>
status_t pages_move(ustore_pages_move_t const& arguments, view_t const& view) noexcept {
    return commit_page_edits<cpu_class_>(arguments, view, [&]() noexcept {
        return pages_move_edits(nvme_variant<cpu_class_>::values_verbs_k, arguments, view);
    });
}

/** Implements @c ustore_pages_splice, appending the splices of every page. */
template <cpu_class_t cpu_class_>
status_t pages_splice(ustore_pages_splice_t const& arguments, strings_arg_t const& values,
                      view_t const& view) noexcept {
    return commit_page_edits<cpu_class_>(arguments, view, [&]() noexcept {
        return pages_splice_edits(nvme_variant<cpu_class_>::values_verbs_k, arguments, values, view);
    });
}

/** Implements @c ustore_pages_set_attributes, appending the attributes set on every page. */
template <cpu_class_t cpu_class_>
status_t pages_set_attributes(ustore_pages_set_attributes_t const& arguments, strings_arg_t const& fields,
                              strings_arg_t const& values, view_t const& view) noexcept {
    return commit_page_edits<cpu_class_>(arguments, view, [&]() noexcept {
        return pages_set_attributes_edits(nvme_variant<cpu_class_>::values_verbs_k, arguments, fields, values, view);
    });
}

/** Implements @c ustore_pages_remove_attributes, appending the attributes removed from every page. */
template <cpu_class_t cpu_class_>
status_t pages_remove_attributes(ustore_pages_remove_attributes_t const& arguments, strings_arg_t const& fields,
                                 view_t const& view) noexcept {
    return commit_page_edits<cpu_class_>(arguments, view, [&]() noexcept {
        return pages_remove_attributes_edits(nvme_variant<cpu_class_>::values_verbs_k, arguments, fields, view);
    });
}

} // namespace unum::ustore::nvme
