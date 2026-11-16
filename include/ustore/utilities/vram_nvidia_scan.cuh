/**
 *  @file include/ustore/utilities/vram_nvidia_scan.cuh
 *  @author Ash Vardanian
 *  @date November 16, 2026
 *  @brief The block scan kits of NVIDIA generations, which a class unit names beside its kernels.
 *
 *  Every NVIDIA class is Ampere or newer, so Ampere's kit is the floor every class takes until a
 *  newer generation scans faster.
 */

#pragma once

#include <cstdint> // `std::uint64_t`

#include <stringzilla/cuda.cuh> // `sz_block_scan_cuda_`

#include "ustore/utilities/vram_scan.cuh" // `block_scanned_t`

namespace unum::ustore::utilities {

/** The block scan of Ampere and newer, StringZilla's over the shuffles of 32-lane warps. */
struct ampere_scan_t {

    /** Scans @p value across a block of @p block_threads_ threads; every thread calls it from
     *  uniform control flow, and it ends in a barrier. */
    template <unsigned block_threads_>
    static __device__ block_scanned_t block_exclusive(std::uint64_t value) noexcept {
        constexpr unsigned warp_lanes = 32;
        static_assert(block_threads_ % warp_lanes == 0, "a block is a whole number of warps");
        __shared__ sz_size_t warp_totals[block_threads_ / warp_lanes];
        sz_size_t total;
        return {sz_block_scan_cuda_(value, warp_totals, &total, block_threads_), total};
    }
};

} // namespace unum::ustore::utilities
