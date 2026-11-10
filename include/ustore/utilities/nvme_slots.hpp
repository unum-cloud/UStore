/**
 *  @file include/ustore/utilities/nvme_slots.hpp
 *  @author Ash Vardanian
 *  @date November 4, 2026
 *  @brief The 1 GB slots a raw medium is cut into: their sizes, the owner block opening every slot
 *      a database holds, and the record a slot pool keeps per slot.
 *
 *  A raw block device or SPDK namespace has no filesystem, so a database claims it slot by slot
 *  from its base address. The first logical block of a slot says who holds it; a block of all zeros
 *  means the slot is free, and anything that is not @c slot_owner_magic_k with this database's ID
 *  is foreign and skipped without being identified. Runs begin @c slot_payload_offset_k bytes in,
 *  so 512 B and 4 KB formats both leave the owner block alone.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`, `std::uint64_t`

#include "ustore/utilities/nvme_ring.hpp" // `portable_alignment_k`
#include "ustore/utilities/types.hpp"     // `uuid128_t`

namespace unum::ustore::utilities {

#pragma region Sizes

/** Bytes of one slot, which is also the largest file a run of a directory volume grows to. */
inline constexpr std::uint64_t slot_bytes_k = std::uint64_t {1} << 30;

/** Where a run begins inside its slot, past the owner block of any logical block size. */
inline constexpr std::uint64_t slot_payload_offset_k = portable_alignment_k;

/** Bytes the largest run takes, which is one slot less its owner block. */
inline constexpr std::uint64_t sorted_run_bytes_max_k = slot_bytes_k - slot_payload_offset_k;

/** Runs shorter than this are appended into a shared packed slot; longer ones take a slot
 *  of their own. */
inline constexpr std::uint64_t packed_run_bytes_max_k = slot_bytes_k / 2;

#pragma endregion Sizes

#pragma region Owner Block

/** The eight bytes of @p text read as one word, in the order they sit on the medium. */
[[nodiscard]] constexpr std::uint64_t magic_of(char const (&text)[9]) noexcept {
    std::uint64_t magic = 0;
    for (std::size_t index = 0; index != 8; ++index)
        magic |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(text[index])) << (index * 8);
    return magic;
}

/** The word the owner block of every slot a database holds opens with. */
inline constexpr std::uint64_t slot_owner_magic_k = magic_of("USTORESL");

/** The layout an owner block follows, which a slot of another version is not mounted by. */
inline constexpr std::uint32_t slot_format_version_k = 1;

#pragma pack(push, 1)

/**
 *  @brief The first logical block of every slot a database holds, little-endian.
 *
 *  It is shorter than one logical block and NVMe writes at least one block atomically, so a rewrite
 *  of the manifest slot's owner block is the commit of a slotted volume and needs no checksum.
 */
struct slot_owner_block_t {

    /** @c slot_owner_magic_k, so a held slot is told apart from a free or a foreign one. */
    std::uint64_t magic = 0;

    /** @c slot_format_version_k for this layout. */
    std::uint32_t format_version = 0;

    /** The slot's data directory among the database's, so reordered drives are refused. */
    std::uint32_t medium = 0;

    /** The database holding the slot, minted when the database was first opened. */
    uuid128_t database;

    /** The generation of the manifest copy in force, on the manifest slot alone. */
    std::uint64_t manifest_generation = 0;

    /** Where in the medium that copy begins, one of the manifest slot's two JSON regions. */
    std::uint64_t manifest_offset = 0;

    /** Bytes of that copy, which also ends at its first zero byte. */
    std::uint64_t manifest_bytes = 0;
};

#pragma pack(pop)

static_assert(sizeof(slot_owner_block_t) == 56, "An owner block is 56 packed bytes");

#pragma endregion Owner Block

#pragma region Slot Records

/** What a slot pool keeps per slot of a raw medium, the slot's index being its place
 *  on the medium. */
struct slot_t {

    /** Stepped by every vacate, so an extent kept past its slot's release resolves to nothing. */
    std::uint32_t generation = 0;

    /** Bytes of the runs inside the slot that are still allocated. */
    std::uint32_t live_bytes = 0;

    /** Where the next packed run starts, zero while the slot is free. */
    std::uint32_t tail_bytes = 0;
};

/** One record of a volume's pool, as an extent remembers it: the index and the generation it
 *  was taken at. */
struct placement_handle_t {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;
};

#pragma endregion Slot Records

} // namespace unum::ustore::utilities
