/**
 *  @file include/ustore/graph/vram_nvidia_adjacency_fold.hpp
 *  @author Ash Vardanian
 *  @date November 1, 2026
 *  @brief The host face of the device fold of graph adjacency, which the tree and the engine pick
 *      for a graph's merges and whose @c compose @c vram_nvidia_adjacency_fold.cuh defines over
 *      the bundles' kernels.
 */

#pragma once

#include <cstdint> // `std::uint64_t`

#include <span> // `std::span`

#include <smashtable/basic_vector.hpp> // `st::vector`
#include <smashtable/shared.hpp>       // `st::status_t`

#include "ustore/utilities/folds.hpp" // `delta_view_t`
#include "ustore/utilities/types.hpp" // `byte_t`

namespace unum::ustore::graph {

/**
 *  @brief The device fold of graph adjacency, composing on one NVIDIA device
 *      what @c adjacency_fold_t composes on the host, for a compaction's chains
 *      of half-edge changes all at once.
 *
 *  Host translation units name it, as the tree and the engine pick it for a
 *  graph's merges; its @c compose launches the bundles' kernels and is
 *  defined in @c vram_nvidia_adjacency_fold.cuh, which the device
 *  compactor's unit includes.
 */
struct vram_nvidia_adjacency_fold_t {

    /**
     *  @brief Composes chain @c c, the changes from @p chain_starts[c] to @p chain_starts[c + 1]
     *      oldest first, into the bytes of @p composed from @p composed_offsets[c] to
     *      @p composed_offsets[c + 1].
     *
     *  The compose runs on the device of @p executor, in device memory carved off @p blocks.
     *
     *  @tparam executor_type_ The executor whose stream the device work queues on, as
     *      in @c utilities::cuda_executor_t.
     *  @tparam blocks_type_ The device runs the compose carves from, as
     *      in @c utilities::device_blocks_t.
     *
     *  @par Statuses:
     *  - @c status_code_t::invalid_argument_k A delta is no change, or carries a ragged or
     *    malformed record, which no compose reads.
     *  - @c status_code_t::out_of_memory_heap_k A buffer could not grow, or a chain passes what
     *    one block sorts on the device.
     */
    template <typename executor_type_, typename blocks_type_>
    st::status_t compose(executor_type_& executor, blocks_type_& blocks, std::span<delta_view_t const> changes,
                         std::span<std::uint64_t const> chain_starts, st::vector<byte_t>& composed,
                         st::vector<std::uint64_t>& composed_offsets) const noexcept;
};

} // namespace unum::ustore::graph
