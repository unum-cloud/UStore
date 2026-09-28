/**
 *  @file src/nvme/extent.hpp
 *  @author Ash Vardanian
 *  @date September 18, 2026
 *  @brief A contiguous byte range on one volume, and the alignment arithmetic every transport shares.
 */

#pragma once

#include <cstdint> // `std::uint64_t`

#include <ustore/types.hpp> // `byte_t`

namespace unum::ustore::nvme {

#pragma region Alignment

/**
 *  @brief The alignment every transport accepts, for memory, for offsets and for lengths alike.
 *
 *  A caller that cannot ask its disk what it needs aligns to this and is accepted everywhere:
 *  io_uring on @c O_DIRECT reports @b [512,4096] through @c STATX_DIOALIGN, SPDK needs a page
 *  boundary for any transfer that crosses one, and cuFile wants 4096 on the device pointer, the
 *  file offset and the size. Nothing hardcodes 512, since a disk reports its own three numbers.
 */
inline constexpr std::uint64_t portable_alignment_k = 4096;

/** Whether @p address is a multiple of @p alignment, for the memory a registration hands the driver. */
inline bool is_address_aligned(void const* address, std::uint64_t alignment) noexcept {
    return (reinterpret_cast<std::uintptr_t>(address) & (alignment - 1)) == 0;
}

#pragma endregion Alignment

#pragma region Extent

/**
 *  @brief A contiguous byte range on one volume, counted from the volume's first byte.
 *
 *  One disk serves one volume, so an extent needs no volume identifier: a directory volume
 *  resolves a name to its disk before it builds the extent, and an extent volume addresses the
 *  namespace directly. Both fields are byte counts, never blocks, so a 512-byte and a 4096-byte
 *  format describe the same range the same way.
 */
struct extent_t {

    /** First byte of the range, which a disk requires to be a multiple of its offset alignment. */
    std::uint64_t offset = 0;

    /** Bytes in the range, which a disk requires to be a multiple of its length granularity. */
    std::uint64_t length = 0;

    /** One past the last byte, saturating rather than wrapping on a range that overflows. */
    [[nodiscard]] constexpr std::uint64_t end() const noexcept {
        return offset > UINT64_MAX - length ? UINT64_MAX : offset + length;
    }

    /** Whether the range covers no bytes, which only a flush submits. */
    [[nodiscard]] constexpr bool empty() const noexcept { return length == 0; }

    /** Whether the whole range sits below @p capacity, computed without overflowing. */
    [[nodiscard]] constexpr bool fits_within(std::uint64_t capacity) const noexcept {
        return offset <= capacity && length <= capacity - offset;
    }

    /** Whether @p other sits entirely inside this range. */
    [[nodiscard]] constexpr bool contains(extent_t const& other) const noexcept {
        return other.offset >= offset && other.end() <= end();
    }

    /** Whether the two ranges share at least one byte. */
    [[nodiscard]] constexpr bool overlaps(extent_t const& other) const noexcept {
        return offset < other.end() && other.offset < end();
    }

    /** Whether the offset and the length are both multiples of @p alignment. */
    [[nodiscard]] constexpr bool aligned_to(std::uint64_t alignment) const noexcept {
        return (offset & (alignment - 1)) == 0 && (length & (alignment - 1)) == 0;
    }

    /** The part of this range that sits below @p capacity, empty when it starts past the end. */
    [[nodiscard]] constexpr extent_t clipped_to(std::uint64_t capacity) const noexcept {
        if (offset >= capacity) return extent_t {offset, 0};
        std::uint64_t const reachable = capacity - offset;
        return extent_t {offset, length < reachable ? length : reachable};
    }

    friend constexpr bool operator==(extent_t const& first, extent_t const& second) noexcept {
        return first.offset == second.offset && first.length == second.length;
    }
};

#pragma endregion Extent

} // namespace unum::ustore::nvme
