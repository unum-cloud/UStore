/**
 *  @file include/ustore/utilities/algorithm.hpp
 *  @author Ash Vardanian
 *  @date January 11, 2023
 *  @brief Searches over sorted batches, reservoir sampling of keys over the iterators of ordered engines, inserts
 *      and erases in buffers of trivially copyable elements, and random-access iterators over parallel arrays.
 */

#pragma once

#include <cstddef> // `std::ptrdiff_t`, `std::size_t`
#include <cstring> // `std::memcpy`, `std::memmove`

#include <algorithm>   // `std::ranges::lower_bound`
#include <chrono>      // `std::chrono::steady_clock`
#include <compare>     // `std::strong_ordering`
#include <iterator>    // `std::random_access_iterator_tag`
#include <random>      // `std::minstd_rand`, `std::uniform_int_distribution`
#include <ranges>      // `std::ranges::random_access_range`, `std::ranges::subrange`
#include <type_traits> // `std::is_trivially_copyable_v`

#include "ustore/utilities/types.hpp" // `status_t`, `ustore_key_t`

namespace unum::ustore::utilities {

/** The position of the first element of a sorted range that is not less than @p wanted. */
template <std::ranges::random_access_range range_, typename wanted_>
std::size_t offset_in_sorted(range_ const& sorted, wanted_ const& wanted) noexcept {
    return static_cast<std::size_t>(std::ranges::lower_bound(sorted, wanted) - std::ranges::begin(sorted));
}

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

/**
 *  Appends up to @p limit keys drawn uniformly over a RocksDB or LevelDB iterator to @p outputs.
 *  @param[inout] iterator Engine iterator, rewound to the first key before sampling.
 *  @param[in] limit The most keys the sample draws, fewer only when the collection holds fewer.
 *  @param[inout] outputs The outputs the drawn keys land on, after the keys they already hold, which @c push a key,
 *      answering a status, and address a key with @c operator[].
 *  @see https://en.wikipedia.org/wiki/Reservoir_sampling
 */
template <typename iterator_, typename outputs_type_>
status_t reservoir_sample_iterator(iterator_&& iterator, std::size_t limit, outputs_type_& outputs) {

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

/**
 *  @brief Walks a container that answers an index, handing out the element it builds for that index.
 *
 *  Parallel arrays hold no element to point at, so the iterator returns the container's own value
 *  rather than a reference into it, and the container must outlive every iterator over it.
 */
template <typename container_type_>
class indexed_iterator {
  public:
    using container_t = container_type_;
    using iterator_category = std::random_access_iterator_tag;
    using value_type = typename container_t::value_type;
    using difference_type = std::ptrdiff_t;
    using pointer = value_type*;
    using reference = value_type;

    indexed_iterator() = default;

    /** An iterator over @p container, positioned at @p offset. */
    indexed_iterator(container_t container, std::size_t offset) noexcept : container_(container), offset_(offset) {}

    /** The element the container builds for the current index. */
    value_type operator*() const { return container_[offset_]; }

    /** The element the container builds for the index @p delta places ahead. */
    value_type operator[](difference_type delta) const { return container_[offset_ + static_cast<std::size_t>(delta)]; }

    indexed_iterator& operator++() noexcept {
        ++offset_;
        return *this;
    }

    indexed_iterator operator++(int) noexcept {
        indexed_iterator copy = *this;
        ++offset_;
        return copy;
    }

    indexed_iterator& operator--() noexcept {
        --offset_;
        return *this;
    }

    indexed_iterator operator--(int) noexcept {
        indexed_iterator copy = *this;
        --offset_;
        return copy;
    }

    indexed_iterator& operator+=(difference_type delta) noexcept {
        offset_ += static_cast<std::size_t>(delta);
        return *this;
    }

    indexed_iterator& operator-=(difference_type delta) noexcept {
        offset_ -= static_cast<std::size_t>(delta);
        return *this;
    }

    friend indexed_iterator operator+(indexed_iterator iterator, difference_type delta) noexcept {
        return iterator += delta;
    }

    friend indexed_iterator operator+(difference_type delta, indexed_iterator iterator) noexcept {
        return iterator += delta;
    }

    friend indexed_iterator operator-(indexed_iterator iterator, difference_type delta) noexcept {
        return iterator -= delta;
    }

    friend difference_type operator-(indexed_iterator const& first, indexed_iterator const& second) noexcept {
        return static_cast<difference_type>(first.offset_) - static_cast<difference_type>(second.offset_);
    }

    friend bool operator==(indexed_iterator const& first, indexed_iterator const& second) noexcept {
        return first.offset_ == second.offset_;
    }

    friend std::strong_ordering operator<=>(indexed_iterator const& first, indexed_iterator const& second) noexcept {
        return first.offset_ <=> second.offset_;
    }

    /** The index the iterator sits at. */
    std::size_t offset() const noexcept { return offset_; }

  private:

    /** The container answering every index, owned by whoever the iterator was made from. */
    container_t container_ {};

    /** The index the iterator sits at. */
    std::size_t offset_ = 0;
};

/** A run of elements one container builds, between two indices. */
template <typename container_type_>
using indexed_range = std::ranges::subrange<indexed_iterator<container_type_>>;

} // namespace unum::ustore::utilities
