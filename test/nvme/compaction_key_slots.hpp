/**
 *  @file test/nvme/compaction_key_slots.hpp
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief The key slots the compaction suite merges, shared by its host translation unit and its device one.
 */
#pragma once

#include <cstdint> // `std::uint8_t`, `std::uint32_t`

#include <limits> // `std::numeric_limits`

#include "ustore/blobs/nvme_key_slot.hpp" // `limit`

namespace unum::ustore::test {

using blobs::limit;

/** Location of an tombstoned variable-size key slot. */
inline constexpr std::uint32_t tombstone_location_k = std::numeric_limits<std::uint32_t>::max();

/** Location of an empty variable-size key slot. */
inline constexpr std::uint32_t empty_location_k = tombstone_location_k - 1;

/** Stored values of up to eight bytes inside the key slot, laid out as UStoreNVMe's inline 8-byte key slots are. */
template <typename stored_key_type_>
struct inline_key_slot {
    using stored_key_t = stored_key_type_;

    stored_key_t stored_key = limit<stored_key_t>::max();
    std::uint32_t size = 0;
    std::uint8_t data[8] = {};

    constexpr bool is_tombstone() const noexcept { return size == 0; }
};

/** Stored values of one width in the stored values section, laid out as UStoreNVMe's fixed-size key slots are. */
template <typename stored_key_type_>
struct fixed_width_key_slot {
    using stored_key_t = stored_key_type_;

    stored_key_t stored_key = limit<stored_key_t>::max();
    std::uint32_t size = 0;
    std::uint32_t location = 0;

    constexpr bool is_tombstone() const noexcept { return size == 0; }
    constexpr bool is_stored_value_empty() const noexcept { return size == 0; }
};

/** Stored values of any width in the stored values section, laid out as UStoreNVMe's variable-size key slots are. */
template <typename stored_key_type_>
struct variable_width_key_slot {
    using stored_key_t = stored_key_type_;

    stored_key_t stored_key = limit<stored_key_t>::max();
    std::uint32_t size = 0;
    std::uint32_t location = 0;

    constexpr bool is_tombstone() const noexcept { return location == tombstone_location_k; }
    constexpr bool is_stored_value_empty() const noexcept {
        return (location == tombstone_location_k) | (location == empty_location_k);
    }
};

} // namespace unum::ustore::test
