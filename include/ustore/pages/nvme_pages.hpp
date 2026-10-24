/**
 *  @file include/ustore/pages/nvme_pages.hpp
 *  @author Ash Vardanian
 *  @date October 20, 2026
 *  @brief The NVMe store of a pages collection: one collection keyed by page UUID, whose value is the page's dense
 *      element tree, folded by @c page_fold.
 *
 *  An import is a write of the whole tree, and every other edit appends a change record of @c page_fold edits rather
 *  than rewriting the page. The edits apply forgivingly, so two commits editing one page both land, and reads and
 *  merges fold the records into the tree through the fold path every collection shares. The records vary in width, so
 *  their merges run on the host.
 */

#pragma once

#include "ustore/blobs/nvme_collection.hpp" // `collection`
#include "ustore/blobs/nvme_key_slot.hpp"   // `key_slot`, `value_layout_t`
#include "ustore/pages/page_fold.hpp"       // `page_fold`

namespace unum::ustore::pages {

/**
 *  @brief The collection of one pages collection: every page a key of variable-width values, folded by
 *      @c page_fold, which serves the pages modality alone.
 *  @tparam stored_key_type_ The key every page is addressed by.
 *  @tparam volume_type_ The volume every run of the collection lives on.
 *  @tparam atomic_reference_ The reference the published view, the cache and the budget own their words through.
 *  @tparam waiting_policy_type_ What a core waiting on a read or the ledger does, and how it parks.
 */
template <typename stored_key_type_, typename volume_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
using nvme_pages = blobs::collection<blobs::key_slot<stored_key_type_, blobs::value_layout_t::variable_width_k>,
                                     volume_type_, atomic_reference_, waiting_policy_type_, page_fold>;

} // namespace unum::ustore::pages
