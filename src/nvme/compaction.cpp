/**
 *  @file src/nvme/compaction.cpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The compiled part of GPU compaction: binding a compactor to an executor whose device
 *      loaded the library's kernels, freeing its device runs, and compacting every key slot.
 *
 *  The graph's device fold for the graph's key slot is instantiated here as well, so the host TUs
 *  of the engine call it.
 */

#include "ustore/blobs/vram_nvidia_compaction.cuh"     // `cuda_compactor_t`
#include "ustore/graph/vram_nvidia_adjacency_fold.cuh" // the fold's device definitions

namespace unum::ustore::blobs {

expected<cuda_compactor_t> cuda_compactor_t::make(utilities::cuda_executor_t& executor,
                                                  device_sections_reader_t sections) noexcept {
    if (!executor.kernels())
        return status_t {status_code_t::unsupported_k, "The device loaded no bundle of the library's kernels"};
    return cuda_compactor_t(executor, sections);
}

cuda_compactor_t::~cuda_compactor_t() noexcept {
    if (!blocks_.count) return;
    sections_.release();
    if (executor_->bind().failed()) return;
    utilities::rewind(blocks_, *executor_);
    executor_->free_async(blocks_.blocks[0].address);
}

} // namespace unum::ustore::blobs

#include "nvme/compaction_variants.hpp" // the explicit instantiations of both
