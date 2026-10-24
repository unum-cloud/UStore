/**
 *  @file include/ustore/docs/nvme_store.hpp
 *  @author Ash Vardanian
 *  @date October 20, 2026
 *  @brief The NVMe store of a docs collection: one collection keyed by document UUID, whose value is the document's
 *      JSON text, folded by @c json_fold.
 *
 *  A whole document is a write, and a modification of a field, a patch or a merge appends a change record of
 *  @c json_fold deltas rather than rewriting the document, so a small edit of a large document writes the edit alone.
 *  Reads and merges fold the records into the document through the fold path every collection shares, and the records
 *  vary in width, so their merges run on the host.
 */

#pragma once

#include "ustore/blobs/nvme_store.hpp"    // `collection`
#include "ustore/blobs/nvme_key_slot.hpp" // `key_slot`, `value_layout_t`
#include "ustore/docs/json_fold.hpp"      // `json_fold`

namespace unum::ustore::docs {

/**
 *  @brief The collection of one docs collection: every document a key of variable-width values, folded by
 *      @c json_fold, which serves the docs modality alone.
 *  @tparam stored_key_type_ The key every document is addressed by.
 *  @tparam volume_type_ The volume every run of the collection lives on.
 *  @tparam atomic_reference_ The reference the published view, the cache and the budget own their words through.
 *  @tparam waiting_policy_type_ What a core waiting on a read or the ledger does, and how it parks.
 */
template <typename stored_key_type_, typename volume_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
using nvme_docs = blobs::collection<blobs::key_slot<stored_key_type_, blobs::value_layout_t::variable_width_k>,
                                    volume_type_, atomic_reference_, waiting_policy_type_, json_fold>;

} // namespace unum::ustore::docs
