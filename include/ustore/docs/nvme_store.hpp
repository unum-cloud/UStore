/**
 *  @file include/ustore/docs/nvme_store.hpp
 *  @author Ash Vardanian
 *  @date October 20, 2026
 *  @brief The NVMe store of a docs collection: one store keyed by document UUID, whose value is the document's
 *      JSON text, folded by @c json_fold_t.
 *
 *  A whole document is a write, and a modification of a field, a patch or a merge appends a change
 *  delta of @c json_fold_t edits rather than rewriting the document, so a small edit of a large
 *  document writes the edit alone. Reads and merges fold the deltas into the document through the
 *  fold path every store shares, and the deltas vary in width, so their merges run on the host.
 */

#pragma once

#include "ustore/blobs/nvme_store.hpp"    // `nvme_store`
#include "ustore/blobs/nvme_key_slot.hpp" // `key_slot`, `value_layout_t`
#include "ustore/docs/json_fold.hpp"      // `json_fold_t`

namespace unum::ustore::docs {

/**
 *  @brief The store of one docs collection: every document a key of variable-width values, folded
 *      by @c json_fold_t, which serves the docs modality alone.
 *  @tparam stored_key_type_ The key every document is addressed by.
 *  @tparam volume_type_ The volume every run of the store lives on.
 *  @tparam atomic_reference_ The reference the published view, the cache and the budget own their words through.
 *  @tparam waiter_type_ What a core waiting on a read or the ledger does between attempts, before it parks.
 *  @tparam device_fold_type_ What composes the changes of a merge on a device,
 *      or @c blobs::no_device_fold_t where nothing does.
 */
template <typename stored_key_type_, typename volume_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, typename device_fold_type_>
using nvme_store = blobs::nvme_store<blobs::key_slot<stored_key_type_, blobs::value_layout_t::variable_width_k>,
                                     volume_type_, atomic_reference_, waiter_type_, json_fold_t, device_fold_type_>;

} // namespace unum::ustore::docs
