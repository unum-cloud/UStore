/**
 *  @file include/ustore/utilities/vram_scan.cuh
 *  @author Ash Vardanian
 *  @date October 31, 2026
 *  @brief The device steps a kernel scans with: a block-wide exclusive scan, and a chain that hands
 *      every block of a launch the total of the blocks before it, so one launch compacts what it
 *      flags without a second pass.
 *
 *  Every step synchronizes through block-shared memory, @c __syncthreads and device atomics
 *  alone, with no warp intrinsic, so one source compiles for 32-lane NVIDIA warps and 64-lane
 *  AMD wavefronts alike.
 *
 *  A chain is one 64-bit slot the host zeroes before the launch. Blocks take tickets in the order
 *  they start, and each waits for the ticket before its own to chain its total, adds its own, and
 *  only then writes; a ticket is only ever held by a running block, so the wait always ends. The
 *  last block leaves the grand total in the slot, which the host downloads.
 */

#pragma once

#include <cstdint> // `std::uint64_t`

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
    return ticket;
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
    return offset;
}

/** What one thread's value scans to across its block: the sum of the values of the threads before
 *  it, and of all. */
struct block_scanned_t {
    std::uint64_t before = 0;
    std::uint64_t total = 0;
};

/** Scans @p value across a block of @p block_threads_ threads; every thread calls it, and it ends
 *  in a barrier. */
template <unsigned block_threads_>
__device__ block_scanned_t block_exclusive_scan(std::uint64_t value) noexcept {
    __shared__ std::uint64_t scanned[block_threads_];
    unsigned const thread = threadIdx.x;
    scanned[thread] = value;
    __syncthreads();
    // An inclusive Hillis-Steele scan, whose every step reads before any thread of
    // the block writes.
    for (unsigned stride = 1; stride < block_threads_; stride *= 2) {
        std::uint64_t const before = thread >= stride ? scanned[thread - stride] : 0;
        __syncthreads();
        scanned[thread] += before;
        __syncthreads();
    }
    block_scanned_t const scan {scanned[thread] - value, scanned[block_threads_ - 1]};
    __syncthreads();
    return scan;
}

#endif

} // namespace unum::ustore::utilities
