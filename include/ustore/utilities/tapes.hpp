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
 *  both in one pass.
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

} // namespace unum::ustore::utilities
