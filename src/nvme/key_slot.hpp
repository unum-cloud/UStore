/**
 *  @file src/nvme/key_slot.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Stored key and on-disk location of a stored value, specialized per value layout.
 */
#pragma once

#include <cstdint>

#include "nvme/stored_value.hpp"
#include "nvme/stored_key.hpp" // `limit`
#include "nvme/types.hpp"      // `round_down_to_multiple`, `round_up_to_multiple`

namespace unum::ustore::nvme {

template <typename stored_key_, value_layout_t value_layout_>
struct key_slot {
    using stored_key_t = stored_key_;
    static constexpr value_layout_t value_layout_k = value_layout_;

    stored_key_t stored_key = limit<stored_key_t>::max();
    stored_value_size_t size = 0;
    byte_t data[static_cast<std::uint16_t>(value_layout_k)] = {byte_t(0)};

    constexpr bool is_tombstone() const noexcept { return size == 0; }
    constexpr void make_tombstone() noexcept { size = 0; }
    constexpr bool is_stored_value_empty() const noexcept { return size == 0; }

    stored_value_size_t aligned_buffer_size() const noexcept { return size; }
    stored_value_offset_t aligned_offset_in_file() const noexcept { return 0; }
    stored_value_size_t buffer_padding() const noexcept { return 0; }

    constexpr bool operator==(key_slot const& other) const noexcept { return stored_key == other.stored_key; }
    constexpr bool operator==(stored_key_t other) const noexcept { return stored_key == other; }
    constexpr bool operator<(key_slot const& other) const noexcept { return stored_key < other.stored_key; }
    constexpr bool operator<(stored_key_t other) const noexcept { return stored_key < other; }
};

template <typename stored_key_>
struct key_slot<stored_key_, value_layout_t::fixed_width_k> {
    using stored_key_t = stored_key_;
    static constexpr value_layout_t value_layout_k = value_layout_t::fixed_width_k;

    stored_key_t stored_key = limit<stored_key_t>::max();
    stored_value_size_t size = 0;
    stored_value_offset_t location = 0;

    constexpr bool is_tombstone() const noexcept { return size == 0; }
    constexpr void make_tombstone() noexcept { size = 0; }
    constexpr bool is_stored_value_empty() const noexcept { return size == 0; }

    stored_value_size_t aligned_buffer_size() const noexcept {
        auto padded_size = stored_value_offset_t(size != 0) * (buffer_padding() + size);
        return round_up_to_multiple<stored_value_size_t>(padded_size, buffer_alignment_k);
    }
    stored_value_offset_t aligned_offset_in_file() const noexcept {
        return round_down_to_multiple<stored_value_offset_t>(location, buffer_alignment_k);
    }
    stored_value_size_t buffer_padding() const noexcept { return location - aligned_offset_in_file(); }

    constexpr bool operator==(key_slot const& other) const noexcept { return stored_key == other.stored_key; }
    constexpr bool operator==(stored_key_t other) const noexcept { return stored_key == other; }
    constexpr bool operator<(key_slot const& other) const noexcept { return stored_key < other.stored_key; }
    constexpr bool operator<(stored_key_t other) const noexcept { return stored_key < other; }
};

template <typename stored_key_>
struct key_slot<stored_key_, value_layout_t::variable_width_k> {
    using stored_key_t = stored_key_;
    static constexpr value_layout_t value_layout_k = value_layout_t::variable_width_k;

    stored_key_t stored_key = limit<stored_key_t>::max();
    stored_value_size_t size = 0;
    stored_value_offset_t location = 0;

    constexpr bool is_tombstone() const noexcept { return location == tombstone_offset_k; }
    constexpr void make_tombstone() noexcept {
        size = 0;
        location = tombstone_offset_k;
    }
    constexpr bool is_stored_value_empty() const noexcept {
        return (location == tombstone_offset_k) | (location == empty_stored_value_offset_k);
    }

    stored_value_size_t aligned_buffer_size() const noexcept {
        auto padded_size = stored_value_offset_t(!is_stored_value_empty()) * (buffer_padding() + size);
        return round_up_to_multiple<stored_value_size_t>(padded_size, buffer_alignment_k);
    }
    stored_value_offset_t aligned_offset_in_file() const noexcept {
        return round_down_to_multiple<stored_value_offset_t>(location, buffer_alignment_k);
    }
    stored_value_size_t buffer_padding() const noexcept { return location - aligned_offset_in_file(); }

    constexpr bool operator==(key_slot const& other) const noexcept { return stored_key == other.stored_key; }
    constexpr bool operator==(stored_key_t other) const noexcept { return stored_key == other; }
    constexpr bool operator<(key_slot const& other) const noexcept { return stored_key < other.stored_key; }
    constexpr bool operator<(stored_key_t other) const noexcept { return stored_key < other; }
};

} // namespace unum::ustore::nvme
