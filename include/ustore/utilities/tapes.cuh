/**
 *  @file include/ustore/utilities/tapes.cuh
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief The device halves of a tape's assembly: one block scanning the lengths into the offsets and cutting the
 *      strings into segments, and one block per segment copying its strings, as @c assemble_tape lays them on the host.
 *
 *  Both templates synchronize through block-shared memory and @c __syncthreads alone, with no warp intrinsic, CUB or
 *  rocPRIM, so one source compiles for 32-lane NVIDIA warps and 64-lane AMD wavefronts alike. A tape takes two
 *  launches, the offsets on one block and the gather on one block per segment, which wait for nothing in between
 *  but the stream's own order, and allocate nothing.
 */

#pragma once

#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`

#include "ustore/utilities/tapes.hpp" // `tape_source_t`, `tape_segment_t`

namespace unum::ustore::utilities {

/**
 *  @brief Scans the lengths of @p count sources into @p offsets on one block of @p block_threads_ threads, then cuts
 *      the sources into segments of @p sources_per_segment each, every one starting at the offset of its first.
 *  @param[in] sources The strings, in tape order.
 *  @param[out] offsets One offset per string and one past the last, the first zero.
 *  @param[out] segments Room for @p count divided by @p sources_per_segment and rounded up, one block of the gather
 *      each.
 */
template <typename offset_type_, unsigned block_threads_>
__device__ void tape_offsets_block(tape_source_t const* sources, std::uint64_t count, std::uint32_t sources_per_segment,
                                   offset_type_* offsets, tape_segment_t* segments) noexcept {
    __shared__ std::uint64_t scanned[block_threads_];
    __shared__ std::uint64_t carried;
    unsigned const thread = threadIdx.x;
    if (thread == 0) carried = 0;
    __syncthreads();
    for (std::uint64_t base = 0; base < count; base += block_threads_) {
        std::uint64_t const index = base + thread;
        std::uint64_t const length = index < count ? sources[index].length : 0;
        scanned[thread] = length;
        __syncthreads();
        // An inclusive Hillis-Steele scan, whose every step reads before any thread of the block writes.
        for (unsigned stride = 1; stride < block_threads_; stride *= 2) {
            std::uint64_t const before = thread >= stride ? scanned[thread - stride] : 0;
            __syncthreads();
            scanned[thread] += before;
            __syncthreads();
        }
        if (index < count) offsets[index] = static_cast<offset_type_>(carried + scanned[thread] - length);
        __syncthreads();
        if (thread == 0) carried += scanned[block_threads_ - 1];
        __syncthreads();
    }
    if (thread == 0) offsets[count] = static_cast<offset_type_>(carried);
    __syncthreads();
    std::uint64_t const stride = std::uint64_t {block_threads_} * sources_per_segment;
    for (std::uint64_t first = std::uint64_t {thread} * sources_per_segment; first < count; first += stride) {
        std::uint64_t const segment = first / sources_per_segment;
        std::uint64_t const left = count - first;
        tape_segment_t cut;
        cut.first_source = static_cast<std::uint32_t>(first);
        cut.sources_count = static_cast<std::uint32_t>(left < sources_per_segment ? left : sources_per_segment);
        cut.first_byte = static_cast<std::uint64_t>(offsets[first]);
        segments[segment] = cut;
    }
}

/**
 *  @brief Copies the strings of segment @c blockIdx.x of @p segments into @p bytes, every thread of the block taking
 *      every block-width-th byte of each string, so the block's threads write neighbouring bytes together.
 *  @param[in] sources The strings, in tape order.
 *  @param[in] segments The segments @c tape_offsets_block cut, one per block of the launch.
 *  @param[out] bytes The tape's bytes, which the last offset counts.
 */
__device__ inline void tape_gather_block(tape_source_t const* sources, tape_segment_t const* segments,
                                         std::byte* bytes) noexcept {
    tape_segment_t const segment = segments[blockIdx.x];
    std::uint64_t written = segment.first_byte;
    for (std::uint32_t index = 0; index != segment.sources_count; ++index) {
        tape_source_t const source = sources[segment.first_source + index];
        for (std::uint64_t byte = threadIdx.x; byte < source.length; byte += blockDim.x)
            bytes[written + byte] = source.data[byte];
        written += source.length;
    }
}

} // namespace unum::ustore::utilities
