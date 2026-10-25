/**
 *  @file include/ustore/graph/nvme_store.hpp
 *  @author Ash Vardanian
 *  @date December 1, 2022
 *  @brief The NVMe store of a graph collection: one store keyed by vertex, whose value is the vertex's sorted
 *      half-edges, folded by @c adjacency_fold_t.
 *
 *  Every vertex is one key, and its value is the half-edges @c adjacency_fold_t lays out, so a walk of one vertex reads
 *  one value. A write appends 48-byte change deltas to the keys of both endpoints of an edge rather than rewriting
 *  either, so two commits adding edges to one vertex never read each other's writes, and reads and merges fold the
 *  deltas into the value through the fold path every store shares. A removed vertex is a removal of its key.
 *
 *  The store lives in a database beside the blobs stores and draws its stamps from the same ledger, so a commit
 *  writing a graph and any other collection lands whole under one stamp.
 */

#pragma once

#include "ustore/blobs/nvme_store.hpp"     // `nvme_store`
#include "ustore/blobs/nvme_key_slot.hpp"  // `key_slot`, `value_layout_t`
#include "ustore/graph/adjacency_fold.hpp" // `adjacency_fold_t`

namespace unum::ustore::graph {

/**
 *  @brief The store of one graph: every vertex a key of variable-width values, folded by @c adjacency_fold_t, which
 *      serves the graph modality alone.
 *  @tparam stored_key_type_ The key every vertex is addressed by.
 *  @tparam volume_type_ The volume every run of the graph lives on.
 *  @tparam atomic_reference_ The reference the published view, the cache and the budget own their words through.
 *  @tparam waiter_type_ What a core waiting on a read or the ledger does between attempts, before it parks.
 */
template <typename stored_key_type_, typename volume_type_, template <typename> typename atomic_reference_,
          typename waiter_type_>
using nvme_store = blobs::nvme_store<blobs::key_slot<stored_key_type_, blobs::value_layout_t::variable_width_k>,
                                     volume_type_, atomic_reference_, waiter_type_, adjacency_fold_t>;

} // namespace unum::ustore::graph
