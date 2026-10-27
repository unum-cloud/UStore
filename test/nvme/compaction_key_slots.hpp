/**
 *  @file test/nvme/compaction_key_slots.hpp
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief The key slots the compaction suite merges, shared by its host translation unit and its device one.
 */
#pragma once

#include <cstdint> // `std::uint8_t`, `std::uint32_t`

#include "ustore/blobs/nvme_key_slot.hpp" // `limit`, `value_layout_t`

namespace unum::ustore::test {

using blobs::limit;

/** Stored values of up to eight bytes inside the key slot, laid out as UStoreNVMe's inline 8-byte key slots are. */
template <typename stored_key_type_>
struct inline_key_slot {
    using stored_key_t = stored_key_type_;

    stored_key_t stored_key = limit<stored_key_t>::max();
    std::uint32_t size = 0;
    std::uint8_t data[8] = {};
};

/** Stored values of one width in the stored values section, found by stride, as UStoreNVMe's fixed-width key slots
 *  find theirs. */
template <typename stored_key_type_>
struct fixed_width_key_slot {
    using stored_key_t = stored_key_type_;
    static constexpr blobs::value_layout_t value_layout_k = blobs::value_layout_t::fixed_width_k;

    stored_key_t stored_key = limit<stored_key_t>::max();
};

/** Stored values of any width in the stored values section, found through value offsets, as UStoreNVMe's
 *  variable-width key slots find theirs. */
template <typename stored_key_type_>
struct variable_width_key_slot {
    using stored_key_t = stored_key_type_;
    static constexpr blobs::value_layout_t value_layout_k = blobs::value_layout_t::variable_width_k;

    stored_key_t stored_key = limit<stored_key_t>::max();
};

} // namespace unum::ustore::test
