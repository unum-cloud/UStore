/**
 *  @file src/nvme/compaction.cu
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The compiled part of GPU compaction: binding a compactor to an executor, growing its scratch through it, and
 *      its compaction of every key slot the engine stores, with the graph's device fold for the graph's key slot,
 *      instantiated here so the host TUs of the engine call it.
 */

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uintptr_t`

#include "ustore/blobs/vram_nvidia_compaction.cuh"     // `cuda_compactor_t::compact`
#include "ustore/graph/vram_nvidia_adjacency_fold.cuh" // `vram_nvidia_adjacency_fold_t::compose`

namespace unum::ustore::blobs {

expected<cuda_compactor_t> cuda_compactor_t::make(utilities::cuda_executor_t& executor,
                                                  device_sections_reader_t sections) noexcept {
    return cuda_compactor_t(executor, sections);
}

cuda_compactor_t::~cuda_compactor_t() noexcept {
    if (!scratch_) return;
    sections_.release();
    if (executor_->bind().succeeded()) executor_->free_async(reinterpret_cast<std::uintptr_t>(scratch_));
}

st::status_t cuda_compactor_t::reserve_(std::size_t bytes, std::size_t kept_bytes) noexcept {
    if (bytes <= scratch_bytes_) return st::status_t::success_k;
    expected<std::uintptr_t> grown = executor_->allocate_async(bytes);
    if (!grown) return st::status_t::out_of_memory_heap_k;
    ++device_allocations_;
    if (scratch_) {
        std::uintptr_t const page = utilities::portable_alignment_k;
        std::uintptr_t const into = round_up_to_multiple<std::uintptr_t>(*grown, page);
        std::uintptr_t const from = round_up_to_multiple<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(scratch_),
                                                                         page);
        if (kept_bytes != 0 && executor_->copy_async(into, from, kept_bytes).failed()) {
            executor_->free_async(*grown);
            return st::status_t::unknown_k;
        }
        sections_.release();
        executor_->free_async(reinterpret_cast<std::uintptr_t>(scratch_));
    }
    scratch_ = reinterpret_cast<byte_t*>(*grown);
    scratch_bytes_ = bytes;
    return st::status_t::success_k;
}

} // namespace unum::ustore::blobs

#include "nvme/compaction_variants.hpp" // `views_of`, `variable_width_slot_t`
