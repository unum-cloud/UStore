/**
 *  @file src/vram/vram_kernels_nvidia_tapes.cu
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief The tape kernels of NVIDIA devices, built device-only into the code object an executor loads at spawn.
 */
#include "ustore/utilities/tapes.cuh" // `tape_offsets_block`, `tape_gather_block`

using namespace unum::ustore::utilities;

/** Threads of every tape kernel's block, eight 32-lane warps. */
constexpr unsigned tape_block_threads_k = 256;

/** Scans the lengths of @p count sources into 32-bit @p offsets and cuts the segments, on one block. */
extern "C" __global__ void __launch_bounds__(tape_block_threads_k)
    ustore_tape_offsets_u32(tape_source_t const* sources, std::uint64_t count, std::uint32_t sources_per_segment,
                            std::uint32_t* offsets, tape_segment_t* segments) {
    tape_offsets_block<std::uint32_t, tape_block_threads_k>(sources, count, sources_per_segment, offsets, segments);
}

/** Scans the lengths of @p count sources into 64-bit @p offsets and cuts the segments, on one block. */
extern "C" __global__ void __launch_bounds__(tape_block_threads_k)
    ustore_tape_offsets_u64(tape_source_t const* sources, std::uint64_t count, std::uint32_t sources_per_segment,
                            std::uint64_t* offsets, tape_segment_t* segments) {
    tape_offsets_block<std::uint64_t, tape_block_threads_k>(sources, count, sources_per_segment, offsets, segments);
}

/** Copies the strings of every segment into @p bytes, one block per segment. */
extern "C" __global__ void __launch_bounds__(tape_block_threads_k)
    ustore_tape_gather(tape_source_t const* sources, tape_segment_t const* segments, std::byte* bytes) {
    tape_gather_block(sources, segments, bytes);
}
