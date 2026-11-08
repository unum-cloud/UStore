/**
 *  @file include/ustore/pages/nvme_store.hpp
 *  @author Ash Vardanian
 *  @date October 20, 2026
 *  @brief The NVMe store of a pages collection: one store keyed by page UUID, whose value is the page's dense
 *      element tree, folded by @c page_fold_t.
 *
 *  An import is a write of the whole tree, and every other edit appends a change delta of @c page_fold_t edits rather
 *  than rewriting the page. The edits apply forgivingly, so two commits editing one page both land, and reads and
 *  merges fold the deltas into the tree through the fold path every store shares. The deltas vary in width, so their
 *  merges run on the host.
 */

#pragma once

#include "ustore/blobs/nvme_store.hpp"    // `nvme_store`
#include "ustore/blobs/nvme_key_slot.hpp" // `key_slot`, `value_layout_t`
#include "ustore/pages/page_fold.hpp"     // `page_fold_t`

namespace unum::ustore::pages {

/**
 *  @brief The store of one pages collection: every page a key of variable-width values, folded
 *      by @c page_fold_t, which serves the pages modality alone.
 *  @tparam stored_key_type_ The key every page is addressed by.
 *  @tparam volume_type_ The volume every run of the store lives on.
 *  @tparam atomic_reference_ The reference the published view, the cache and the budget own their words through.
 *  @tparam waiter_type_ What a core waiting on a read or the ledger does between attempts, before it parks.
 *  @tparam device_fold_type_ What composes the changes of a merge on a device,
 *      or @c blobs::no_device_fold_t where nothing does.
 *  @tparam row_kit_type_ The SmashTable kit the memory runs' trees search with.
 */
template <typename stored_key_type_, typename volume_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, typename device_fold_type_, typename row_kit_type_>
using nvme_store =
    blobs::nvme_store<blobs::key_slot<stored_key_type_, blobs::value_layout_t::variable_width_k>, volume_type_,
                      atomic_reference_, waiter_type_, page_fold_t, device_fold_type_, row_kit_type_>;

} // namespace unum::ustore::pages
