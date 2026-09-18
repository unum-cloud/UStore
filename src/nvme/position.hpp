/**
 *  @file src/nvme/position.hpp
 *  @author Ash Vardanian
 *  @date September 17, 2026
 *  @brief Where a sorted search landed, and whether it found the value it was looking for.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <algorithm> // `std::ranges::lower_bound`
#include <ranges>    // `std::ranges::random_access_range`

namespace unum::ustore::nvme {

/**
 *  @brief Where a value sits in a sorted sequence, or where it would be inserted.
 *
 *  One search answers both questions, so an upsert locates its slot and learns whether it
 *  overwrites without searching twice.
 */
struct position_t {

    /** The index of the value, or of the first value ordering after it. */
    std::size_t offset = 0;

    /** Whether the sequence holds the value at @c offset. */
    bool exists = false;
};

/** Where @p wanted sits in the sorted @p range, or where it would be inserted. */
template <std::ranges::random_access_range range_type_, typename wanted_type_>
position_t find_in_sorted(range_type_ const& range, wanted_type_ const& wanted) noexcept {
    auto const first = std::ranges::begin(range);
    auto const last = std::ranges::end(range);
    auto const found = std::ranges::lower_bound(first, last, wanted);
    std::size_t const offset = static_cast<std::size_t>(found - first);
    return {offset, found != last && !(wanted < *found)};
}

} // namespace unum::ustore::nvme
