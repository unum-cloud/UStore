/**
 *  @file src/helpers/algorithm.hpp
 *  @author Ash Vardanian
 *  @date January 11, 2023
 *  @brief Searches over sorted batches that the modalities share, on top of @c std::ranges.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <algorithm> // `std::ranges::lower_bound`
#include <ranges>    // `std::ranges::random_access_range`

namespace unum::ustore {

/** The position of the first element of a sorted range that is not less than @p wanted. */
template <std::ranges::random_access_range range_, typename wanted_>
std::size_t offset_in_sorted(range_ const& sorted, wanted_ const& wanted) noexcept {
    return static_cast<std::size_t>(std::ranges::lower_bound(sorted, wanted) - std::ranges::begin(sorted));
}

} // namespace unum::ustore
