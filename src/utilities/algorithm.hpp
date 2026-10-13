/**
 *  @file src/utilities/algorithm.hpp
 *  @author Ash Vardanian
 *  @date January 11, 2023
 *  @brief Searches over sorted batches, reservoir sampling of keys over the iterators of ordered engines, and inserts
 *      and erases in buffers of trivially copyable elements.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstring> // `std::memcpy`, `std::memmove`

#include <algorithm>   // `std::ranges::lower_bound`
#include <chrono>      // `std::chrono::steady_clock`
#include <random>      // `std::minstd_rand`, `std::uniform_int_distribution`
#include <ranges>      // `std::ranges::random_access_range`
#include <type_traits> // `std::is_trivially_copyable_v`

#include "ustore/status.hpp" // `status_t`
#include "ustore/types.hpp"  // `ustore_key_t`

#include "utilities/outputs.hpp" // `scan_outputs_t`

namespace unum::ustore::utilities {

/** The position of the first element of a sorted range that is not less than @p wanted. */
template <std::ranges::random_access_range range_, typename wanted_>
std::size_t offset_in_sorted(range_ const& sorted, wanted_ const& wanted) noexcept {
    return static_cast<std::size_t>(std::ranges::lower_bound(sorted, wanted) - std::ranges::begin(sorted));
}

/**
 *  Appends up to @p limit keys drawn uniformly over a RocksDB or LevelDB iterator to @p outputs.
 *  @param[inout] iterator Engine iterator, rewound to the first key before sampling.
 *  @param[in] limit The most keys the sample draws, fewer only when the collection holds fewer.
 *  @param[inout] outputs The outputs the drawn keys land on, after the keys they already hold.
 *  @see https://en.wikipedia.org/wiki/Reservoir_sampling
 */
template <typename iterator_>
status_t reservoir_sample_iterator(iterator_&& iterator, std::size_t limit, scan_outputs_t& outputs) {

    auto const seed = static_cast<std::minstd_rand::result_type>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::minstd_rand generator(seed);

    std::size_t const first = outputs.size();
    std::size_t visited = 0;
    for (iterator->SeekToFirst(); visited < limit; ++visited, iterator->Next()) {
        if (!iterator->Valid()) return {};
        ustore_key_t key;
        std::memcpy(&key, iterator->key().data(), sizeof(ustore_key_t));
        if (status_t const status = outputs.push(key); status.failed()) return status;
    }

    for (; iterator->Valid(); ++visited, iterator->Next()) {
        std::size_t const slot = std::uniform_int_distribution<std::size_t>(0, visited)(generator);
        if (slot < limit) std::memcpy(&outputs[first + slot], iterator->key().data(), sizeof(ustore_key_t));
    }
    return {};
}

/** Inserts trivially copyable elements at @p offset of a buffer with spare capacity and returns the new length. */
template <typename element_>
std::size_t trivial_insert( //
    element_* begin, std::size_t old_length, std::size_t offset, element_ const* inserted_begin,
    element_ const* inserted_end) noexcept {

    static_assert(std::is_trivially_copyable_v<element_>);
    auto const inserted_length = static_cast<std::size_t>(inserted_end - inserted_begin);
    auto const following_length = old_length - offset;
    // An empty range may hold null pointers, which `memmove` and `memcpy` must never receive, even for zero bytes
    if (following_length)
        std::memmove(begin + offset + inserted_length, begin + offset, following_length * sizeof(element_));
    if (inserted_length) std::memcpy(begin + offset, inserted_begin, inserted_length * sizeof(element_));
    return old_length + inserted_length;
}

/** Erases trivially copyable elements from a buffer and returns the new length. */
template <typename element_>
std::size_t trivial_erase(element_* begin, std::size_t old_length, std::size_t removed_offset,
                          std::size_t removed_length) noexcept {

    static_assert(std::is_trivially_copyable_v<element_>);
    auto const following_length = old_length - (removed_offset + removed_length);
    std::memmove(begin + removed_offset, begin + removed_offset + removed_length, following_length * sizeof(element_));
    return old_length - removed_length;
}

} // namespace unum::ustore::utilities
