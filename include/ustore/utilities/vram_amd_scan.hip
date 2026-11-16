/**
 *  @file include/ustore/utilities/vram_amd_scan.hip
 *  @author Ash Vardanian
 *  @date November 16, 2026
 *  @brief The block scan kits of AMD generations, which a class unit names beside its kernels.
 *
 *  CDNA3 and CDNA4 scan 64-lane wavefronts through DPP, and CDNA5 scans its 32-lane ones through
 *  its own lane steps, so each lineage keeps its kit.
 */

#pragma once

#include <cstdint> // `std::uint64_t`

#include <stringzilla/strings/cdna3.cuh> // `sz_block_scan_cdna3_`
#include <stringzilla/strings/cdna5.cuh> // `sz_block_scan_cdna5_`

#include "ustore/utilities/vram_scan.cuh" // `block_scanned_t`

namespace unum::ustore::utilities {

/** The block scan of CDNA3 and CDNA4, StringZilla's over the DPP scans of 64-lane wavefronts. */
struct cdna3_scan_t {

    /** Scans @p value across a block of @p block_threads_ threads; every thread calls it from
     *  uniform control flow, and it ends in a barrier. */
    template <unsigned block_threads_>
    static __device__ block_scanned_t block_exclusive(std::uint64_t value) noexcept {
        constexpr unsigned wavefront_lanes = 64;
        static_assert(block_threads_ % wavefront_lanes == 0, "a block is a whole number of wavefronts");
        __shared__ sz_size_t wavefront_totals[block_threads_ / wavefront_lanes];
        sz_size_t total;
        return {sz_block_scan_cdna3_(value, wavefront_totals, &total, block_threads_), total};
    }
};

/** The block scan of CDNA5, StringZilla's over the lane steps of 32-lane wavefronts. */
struct cdna5_scan_t {

    /** Scans @p value across a block of @p block_threads_ threads; every thread calls it from
     *  uniform control flow, and it ends in a barrier. */
    template <unsigned block_threads_>
    static __device__ block_scanned_t block_exclusive(std::uint64_t value) noexcept {
        constexpr unsigned wavefront_lanes = 32;
        static_assert(block_threads_ % wavefront_lanes == 0, "a block is a whole number of wavefronts");
        __shared__ sz_size_t wavefront_totals[block_threads_ / wavefront_lanes];
        sz_size_t total;
        return {sz_block_scan_cdna5_(value, wavefront_totals, &total, block_threads_), total};
    }
};

} // namespace unum::ustore::utilities
