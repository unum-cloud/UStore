/**
 *  @file include/ustore/utilities/tapes.hpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief The strings a tape is assembled from, the segments a device gathers them in, and the host twin of the
 *      device kernels, so both residencies lay a tape out byte for byte alike.
 *
 *  A tape is the offsets of its strings, one more than their count and the first zero, then their bytes back to back,
 *  as Apache Arrow's @c binary and @c large_binary lay them. On a device one block scans the lengths into the offsets
 *  and cuts the strings into segments, and one block per segment copies its strings; on the host @c assemble_tape does
 *  both in one pass, and its device twin in @c vram_device.hpp queues both launches.
 */

#pragma once

#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <span> // `std::span`

namespace unum::ustore::utilities {

/** One string a tape copies, wherever its bytes live. */
struct tape_source_t {

    /** The first byte, which may be null for an empty string. */
    std::byte const* data = nullptr;

    /** Bytes the string takes. */
    std::uint64_t length = 0;
};

static_assert(sizeof(tape_source_t) == 16, "A tape source is a pointer and a length");

/** A run of consecutive sources one block of a gather copies, starting at the byte of the tape the offsets gave the
 *  first of them. */
struct tape_segment_t {

    /** The first source of the run. */
    std::uint32_t first_source = 0;

    /** Sources the run holds. */
    std::uint32_t sources_count = 0;

    /** Where the first source's bytes start inside the tape. */
    std::uint64_t first_byte = 0;
};

static_assert(sizeof(tape_segment_t) == 16, "A tape segment is two counts and an offset");

/** Threads of every tape kernel's block, eight 32-lane NVIDIA warps or four
 *  64-lane AMD wavefronts. */
inline constexpr unsigned tape_block_threads_k = 256;

/** Sources each block of a device gather copies. */
inline constexpr std::uint32_t tape_sources_per_segment_k = 64;

/**
 *  @brief Lays @p count strings out as a tape, which the device's offsets and gather kernels write between them.
 *  @param[in] sources Answers @c sources(index) with the @c tape_source_t of every string, in tape order.
 *  @param[out] offsets One offset per string and one past the last, the first zero.
 *  @param[out] bytes Room for every byte of every string, which the last offset counts.
 */
template <typename offset_type_, typename sources_type_>
void assemble_tape(std::size_t count, sources_type_ const& sources, std::span<offset_type_> offsets,
                   std::span<std::byte> bytes) noexcept {
    offsets[0] = 0;
    for (std::size_t index = 0; index != count; ++index) {
        tape_source_t const source = sources(index);
        if (source.length)
            std::memcpy(bytes.data() + offsets[index], source.data, static_cast<std::size_t>(source.length));
        offsets[index + 1] = static_cast<offset_type_>(offsets[index] + source.length);
    }
}

/** Fixed-width rows @c stride bytes apart, like the source, target and edge keys of an edge tape,
 *  and the dense run a de-interleave copies them into, one row after another. */
struct strided_rows_t {
    std::byte const* source = nullptr;
    std::uint64_t stride = 0;
    std::uint64_t row_bytes = 0;
    std::uint64_t rows = 0;
    std::byte* dense = nullptr;
};

/** Copies row @p row of @p rows into its dense place, as host and devices de-interleave alike. */
constexpr void densify_row(strided_rows_t const& rows, std::uint64_t row) noexcept {
    for (std::uint64_t byte = 0; byte != rows.row_bytes; ++byte)
        rows.dense[row * rows.row_bytes + byte] = rows.source[row * rows.stride + byte];
}

} // namespace unum::ustore::utilities
