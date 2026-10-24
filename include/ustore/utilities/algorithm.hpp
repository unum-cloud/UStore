/**
 *  @file include/ustore/utilities/algorithm.hpp
 *  @author Ash Vardanian
 *  @date January 11, 2023
 *  @brief Searches over sorted batches, 128-bit sorted merges and intersections, reservoir sampling of keys over the
 *      iterators of ordered engines, and inserts and erases in buffers of trivially copyable elements.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstring> // `std::memcpy`, `std::memmove`

#include <algorithm>   // `std::ranges::lower_bound`, `std::min`
#include <chrono>      // `std::chrono::steady_clock`
#include <concepts>    // `std::same_as`, `std::convertible_to`
#include <random>      // `std::minstd_rand`, `std::uniform_int_distribution`
#include <ranges>      // `std::ranges::random_access_range`
#include <span>        // `std::span`
#include <type_traits> // `std::is_trivially_copyable_v`

#include "ustore/utilities/types.hpp" // `status_t`, `ustore_key_t`

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

#pragma region Sorted Keys

/** Orders two 16-byte keys as their bytes, the order every engine stores UUIDs in, in one 128-bit comparison. */
inline int compare_128(void const* first, void const* second) noexcept { return std::memcmp(first, second, 16); }

/** Orders two keys as their bytes. */
inline int compare_128(ustore_key_t const& first, ustore_key_t const& second) noexcept {
    return compare_128(static_cast<void const*>(&first), static_cast<void const*>(&second));
}

/**
 *  @brief A forward walk over sorted 16-byte keys, one distinct key at a time, which can leap ahead to a key.
 *
 *  A walk that reads storage as it goes answers a status from every move, so a failed read ends the merge using it.
 */
template <typename cursor_type_>
concept sorted_128_cursor_concept = requires(cursor_type_& cursor, cursor_type_ const& constant,
                                             ustore_key_t const& key) {
    { constant.valid() } -> std::same_as<bool>;
    { constant.key() } -> std::convertible_to<ustore_key_t>;
    { cursor.next() } -> std::same_as<status_t>;
    { cursor.seek(key) } -> std::same_as<status_t>;
};

/** Walks a sorted span of keys one distinct key at a time, galloping forward on a seek. */
class sorted_128_cursor {
  public:
    sorted_128_cursor() noexcept = default;

    /** A walk over @p keys, sorted bytewise and possibly repeating, which must outlive the walk. */
    explicit sorted_128_cursor(std::span<ustore_key_t const> keys) noexcept : keys_(keys) {}

    /** Whether the walk still stands on a key. */
    bool valid() const noexcept { return offset_ < keys_.size(); }

    /** The key the walk stands on, while it is valid. */
    ustore_key_t key() const noexcept { return keys_[offset_]; }

    /** Steps past every repeat of the current key. */
    status_t next() noexcept {
        ustore_key_t const current = keys_[offset_];
        do ++offset_;
        while (offset_ < keys_.size() && compare_128(keys_[offset_], current) == 0);
        return {};
    }

    /** Leaps to the first key not ordering before @p wanted, never backwards. */
    status_t seek(ustore_key_t const& wanted) noexcept {
        if (!valid() || compare_128(keys_[offset_], wanted) >= 0) return {};
        // Doubling steps find a window in logarithmic time of the distance, then halving finds the key in it
        std::size_t below = offset_, step = 1;
        std::size_t above = offset_ + step;
        while (above < keys_.size() && compare_128(keys_[above], wanted) < 0) {
            below = above;
            step *= 2;
            above = offset_ + step;
        }
        above = std::min(above, keys_.size());
        while (above - below > 1) {
            std::size_t const middle = below + (above - below) / 2;
            if (compare_128(keys_[middle], wanted) < 0) below = middle;
            else above = middle;
        }
        offset_ = above;
        return {};
    }

  private:
    std::span<ustore_key_t const> keys_;
    std::size_t offset_ = 0;
};

static_assert(sorted_128_cursor_concept<sorted_128_cursor>);

/**
 *  @brief Calls @p callback with every key both walks hold, once each and in order, leapfrogging the walk that lags.
 *  @param[in] callback Takes the shared key and answers a status, where a failure stops the walk and is returned.
 */
template <sorted_128_cursor_concept first_cursor_, sorted_128_cursor_concept second_cursor_, typename callback_type_>
status_t intersect_sorted_128(first_cursor_& first, second_cursor_& second, callback_type_&& callback) noexcept {
    while (first.valid() && second.valid()) {
        ustore_key_t const first_key = first.key(), second_key = second.key();
        int const order = compare_128(first_key, second_key);
        status_t status;
        if (order < 0) status = first.seek(second_key);
        else if (order > 0) status = second.seek(first_key);
        else {
            status = callback(first_key);
            if (status.succeeded()) status = first.next();
            if (status.succeeded()) status = second.next();
        }
        if (status.failed()) return status;
    }
    return {};
}

/** Writes the distinct keys two sorted spans share into @p output, which fits the shorter span, and answers how
 *  many. */
inline std::size_t intersect_sorted_128(std::span<ustore_key_t const> first, std::span<ustore_key_t const> second,
                                        ustore_key_t* output) noexcept {
    sorted_128_cursor first_cursor(first), second_cursor(second);
    std::size_t count = 0;
    [[maybe_unused]] status_t const status = intersect_sorted_128(first_cursor, second_cursor,
                                                                  [&](ustore_key_t const& key) noexcept {
                                                                      output[count++] = key;
                                                                      return status_t {};
                                                                  });
    return count;
}

/**
 *  @brief Walks the union of two sorted spans of keys once, calling @p callback with every distinct key and whether
 *      each span holds it, which serves unions, differences and symmetric differences alike.
 *  @param[in] callback Takes the key, then whether the first and the second span hold it, and answers a status, where
 *      a failure stops the walk and is returned.
 */
template <typename callback_type_>
status_t merge_sorted_128(std::span<ustore_key_t const> first, std::span<ustore_key_t const> second,
                          callback_type_&& callback) noexcept {
    sorted_128_cursor first_cursor(first), second_cursor(second);
    while (first_cursor.valid() || second_cursor.valid()) {
        int const order = !first_cursor.valid()    ? 1
                          : !second_cursor.valid() ? -1
                                                   : compare_128(first_cursor.key(), second_cursor.key());
        ustore_key_t const key = order <= 0 ? first_cursor.key() : second_cursor.key();
        if (status_t const status = callback(key, order <= 0, order >= 0); status.failed()) return status;
        status_t const first_moved = order <= 0 ? first_cursor.next() : status_t {};
        status_t const second_moved = order >= 0 ? second_cursor.next() : status_t {};
        if (first_moved.failed() || second_moved.failed()) return first_moved.failed() ? first_moved : second_moved;
    }
    return {};
}

#pragma endregion Sorted Keys

} // namespace unum::ustore::utilities
