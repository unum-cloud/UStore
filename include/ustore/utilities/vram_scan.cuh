/**
 *  @file include/ustore/utilities/vram_scan.cuh
 *  @author Ash Vardanian
 *  @date October 31, 2026
 *  @brief The device steps a kernel scans with: what a block scan kit answers, and a chain that
 *      hands every block of a launch the total of the blocks before it, so one launch compacts what
 *      it flags without a second pass.
 *
 *  A kernel template takes its block scan as a kit, which its class's unit names: one per GPU
 *  generation, in @c vram_nvidia_scan.cuh and @c vram_amd_scan.hip, each over StringZilla's block
 *  scan of its generation's lane steps. The chain uses device atomics and no warp intrinsic.
 *
 *  A chain is one 64-bit slot the host zeroes before the launch. Blocks take tickets in the order
 *  they start, and each waits for the ticket before its own to chain its total, adds its own, and
 *  only then writes; a ticket is only ever held by a running block, so the wait always ends. The
 *  last block leaves the grand total in the slot, which the host downloads.
 *
 *  Apple devices have no 64-bit atomics, so there the host runs the select of
 *  @c vram_kernels_apple.metal instead, whose launches this header names: a count per tile, one
 *  threadgroup scanning the counts, and a write of every flagged index at its rank, in item order
 *  as the chain's.
 */

#pragma once

#include <cstdint> // `std::uint64_t`

#if !defined(__CUDACC__) && !defined(__HIP__)
#include "ustore/utilities/vram_device.hpp" // `launch`, `metal_executor_t`, `status_t`
#endif

namespace unum::ustore::utilities {

/** Blocks one chain hands tickets to at most, which bounds how long the last one waits. */
inline constexpr std::uint64_t chain_blocks_max_k = 1024;

/** Where a chain's slot keeps the tickets handed out while its launch runs. */
inline constexpr unsigned chain_tickets_shift_k = 54;

/** Where a chain's slot keeps the blocks chained, above the total they reached. */
inline constexpr unsigned chain_chained_shift_k = 44;

#if defined(__CUDACC__) || defined(__HIP__)

/** This block's ticket, the part of the input it works on; every thread calls it, and it ends
 *  in a barrier. */
__device__ inline std::uint64_t chain_ticket(std::uint64_t* slot) noexcept {
    __shared__ std::uint64_t ticket;
    if (threadIdx.x == 0)
        ticket = static_cast<std::uint64_t>(
                     atomicAdd(reinterpret_cast<unsigned long long*>(slot), 1ull << chain_tickets_shift_k)) >>
                 chain_tickets_shift_k;
    __syncthreads();
    std::uint64_t const taken = ticket;
    // A second call in the same block rewrites the slot, so no thread may still be reading it then
    __syncthreads();
    return taken;
}

/** The total of the blocks before ticket @p ticket of @p tickets, once each of them chained,
 *  publishing this block's own @p total for the ones after; every thread calls it, and it ends
 *  in a barrier. */
__device__ inline std::uint64_t chain_offset(std::uint64_t* slot, std::uint64_t ticket, std::uint64_t tickets,
                                             std::uint64_t total) noexcept {
    __shared__ std::uint64_t offset;
    if (threadIdx.x == 0) {
        unsigned long long* const chain = reinterpret_cast<unsigned long long*>(slot);
        unsigned long long chained;
        do chained = *reinterpret_cast<unsigned long long volatile*>(chain);
        while ((chained >> chain_chained_shift_k & (chain_blocks_max_k - 1)) != ticket);
        offset = static_cast<std::uint64_t>(chained & ((1ull << chain_chained_shift_k) - 1));
        // Every ticket is taken and every earlier block chained, so the last block's
        // write is final.
        if (ticket + 1 == tickets) *slot = offset + total;
        else atomicAdd(chain, (1ull << chain_chained_shift_k) + total);
    }
    __syncthreads();
    std::uint64_t const before = offset;
    // A second call in the same block rewrites the slot, so no thread may still be reading it then
    __syncthreads();
    return before;
}

/** What one thread's value scans to across its block: the sum of the values of the threads before
 *  it, and of all. */
struct block_scanned_t {
    std::uint64_t before = 0;
    std::uint64_t total = 0;
};

#else

/** Flags the select reads a byte each, as the merge ranks and the fold write them. */
inline constexpr std::uint32_t select_byte_flags_k = 8;

/**
 *  @brief Queues the first two passes of the select over the @p count byte @p flags on
 *      @p executor, which leave in @p counts the items flagged before every one of the @p tiles
 *      tiles of @p tile items, a multiple of 256, and the total after the last.
 *
 *  Every tile counts in 32 bits and the scan widens the counts to 64-bit offsets, so @p counts
 *  holds @p tiles + 1 words on the device.
 *
 *  @par Statuses:
 *  - @c status_code_t::device_k The bundle holds no select, or the device refused a launch.
 */
inline status_t count_flagged(metal_executor_t& executor, std::uint8_t const* flags, std::uint64_t count,
                              std::uint32_t tile, std::uint32_t tiles, std::uint64_t* counts) noexcept {
    char const* const passes[] = {"ustore_select_count_apple", "ustore_select_scan_apple"};
    unsigned const groups[] = {tiles, 1u};
    // The select's scans assume threadgroups of 256 threads
    for (std::size_t pass = 0; pass != 2; ++pass)
        if (status_t const launched = launch(executor, executor.kernels()->function(passes[pass]), groups[pass], 256u,
                                             0u, flags, count, tile, tiles, select_byte_flags_k, counts,
                                             static_cast<std::uint32_t*>(nullptr));
            launched.failed())
            return launched;
    return {};
}

/**
 *  @brief Queues the select over the @p count byte @p flags on @p executor: the passes of
 *      @c count_flagged, then a list of the index of every flagged item into @p selected in item
 *      order, which holds @p count words on the device.
 *
 *  @par Statuses:
 *  - @c status_code_t::device_k The bundle holds no select, or the device refused a launch.
 */
inline status_t select_flagged(metal_executor_t& executor, std::uint8_t const* flags, std::uint32_t count,
                               std::uint32_t tile, std::uint32_t tiles, std::uint64_t* counts,
                               std::uint32_t* selected) noexcept {
    if (status_t const counted = count_flagged(executor, flags, count, tile, tiles, counts); counted.failed())
        return counted;
    return launch(executor, executor.kernels()->function("ustore_select_write_apple"), tiles, 256u, 0u, flags,
                  std::uint64_t {count}, tile, tiles, select_byte_flags_k, counts, selected);
}

#endif

} // namespace unum::ustore::utilities
