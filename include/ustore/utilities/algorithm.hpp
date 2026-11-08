/**
 *  @file include/ustore/utilities/algorithm.hpp
 *  @author Ash Vardanian
 *  @date January 11, 2023
 *  @brief Searches over sorted batches, 128-bit sorted merges and intersections, reservoir
 *      sampling over engine iterators, inserts and erases in trivially copyable buffers, sorts
 *      that allocate nothing and a loser tree naming the least head among sorted runs.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::memmove`

#include <algorithm>   // `std::ranges::lower_bound`, `std::min`
#include <array>       // `std::array`
#include <chrono>      // `std::chrono::steady_clock`
#include <concepts>    // `std::same_as`, `std::convertible_to`
#include <random>      // `std::minstd_rand`, `std::uniform_int_distribution`
#include <ranges>      // `std::ranges::random_access_range`
#include <span>        // `std::span`
#include <type_traits> // `std::is_trivially_copyable_v`
#include <utility>     // `std::move`, `std::swap`

#include <smashtable/basic_vector.hpp> // `st::vector`
#include <smashtable/row_layout.hpp>   // `st::key128_t`
#include <smashtable/shared.hpp>       // `st::status_t`, `st::default_allocator`, `st::assume_uninitialized`

#include "ustore/utilities/types.hpp" // `status_t`, `ustore_key_t`

namespace unum::ustore::utilities {

/** The position of the first element of a sorted range that is not less than @p wanted. */
template <std::ranges::random_access_range range_type_, typename wanted_type_>
std::size_t offset_in_sorted(range_type_ const& sorted, wanted_type_ const& wanted) noexcept {
    return static_cast<std::size_t>(std::ranges::lower_bound(sorted, wanted) - std::ranges::begin(sorted));
}

/**
 *  Appends up to @p limit keys drawn uniformly over a RocksDB or LevelDB iterator to @p outputs.
 *  @param[in,out] iterator Engine iterator, rewound to the first key before sampling.
 *  @param[in] limit The most keys the sample draws, fewer only when the collection holds fewer.
 *  @param[in,out] outputs The outputs the drawn keys land on, after the keys they already hold, which @c push a key,
 *      answering a status, and address a key with @c operator[].
 *  @see https://en.wikipedia.org/wiki/Reservoir_sampling
 */
template <typename iterator_type_, typename outputs_type_>
status_t reservoir_sample_iterator(iterator_type_&& iterator, std::size_t limit, outputs_type_& outputs) {

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
template <typename element_type_>
std::size_t trivial_insert( //
    element_type_* begin, std::size_t old_length, std::size_t offset, element_type_ const* inserted_begin,
    element_type_ const* inserted_end) noexcept {

    static_assert(std::is_trivially_copyable_v<element_type_>);
    auto const inserted_length = static_cast<std::size_t>(inserted_end - inserted_begin);
    auto const following_length = old_length - offset;
    // An empty range may hold null pointers, which `memmove` and `memcpy` must never receive, even for zero bytes
    if (following_length)
        std::memmove(begin + offset + inserted_length, begin + offset, following_length * sizeof(element_type_));
    if (inserted_length) std::memcpy(begin + offset, inserted_begin, inserted_length * sizeof(element_type_));
    return old_length + inserted_length;
}

/** Erases trivially copyable elements from a buffer and returns the new length. */
template <typename element_type_>
std::size_t trivial_erase(element_type_* begin, std::size_t old_length, std::size_t removed_offset,
                          std::size_t removed_length) noexcept {

    static_assert(std::is_trivially_copyable_v<element_type_>);
    auto const following_length = old_length - (removed_offset + removed_length);
    std::memmove(begin + removed_offset, begin + removed_offset + removed_length,
                 following_length * sizeof(element_type_));
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
class sorted_128_cursor_t {
  public:
    sorted_128_cursor_t() noexcept = default;

    /** A walk over @p keys, sorted bytewise and possibly repeating, which must outlive the walk. */
    explicit sorted_128_cursor_t(std::span<ustore_key_t const> keys) noexcept : keys_(keys) {}

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

static_assert(sorted_128_cursor_concept<sorted_128_cursor_t>);

/**
 *  @brief Calls @p callback with every key both walks hold, once each and in order, leapfrogging the walk that lags.
 *  @param[in,out] first The first walk, advanced as it is read.
 *  @param[in,out] second The second walk, advanced as it is read.
 *  @param[in] callback Takes the shared key and answers a status, where a failure stops the walk and is returned.
 */
template <sorted_128_cursor_concept first_cursor_type_, sorted_128_cursor_concept second_cursor_type_,
          typename callback_type_>
status_t intersect_sorted_128(first_cursor_type_& first, second_cursor_type_& second,
                              callback_type_&& callback) noexcept {
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
    sorted_128_cursor_t first_cursor(first), second_cursor(second);
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
 *  @param[in] first The first sorted span.
 *  @param[in] second The second sorted span.
 *  @param[in] callback Takes the key, then whether the first and the second span hold it, and answers a status, where
 *      a failure stops the walk and is returned.
 */
template <typename callback_type_>
status_t merge_sorted_128(std::span<ustore_key_t const> first, std::span<ustore_key_t const> second,
                          callback_type_&& callback) noexcept {
    sorted_128_cursor_t first_cursor(first), second_cursor(second);
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

/** The byte @p index of @p word, counting from the most significant, as a big-endian
 *  layout stores it. */
constexpr std::size_t word_byte(std::uint64_t word, std::size_t index) noexcept {
    return static_cast<std::size_t>((word >> ((sizeof(std::uint64_t) - 1 - index) * 8)) & 0xFF);
}

/** The 64-bit word @p word of a stored key, counting from the most significant. */
inline std::uint64_t stored_key_word(std::uint64_t stored_key, std::size_t) noexcept { return stored_key; }

/** The 64-bit word @p word of a 16-byte key, the high one first, as its ordering compares them. */
inline std::uint64_t stored_key_word(st::key128_t stored_key, std::size_t word) noexcept {
    return word == 0 ? stored_key.high : stored_key.low;
}

/** The byte @p digit of a stored key, counting from the most significant, so bytes order as
 *  the keys do. */
template <typename stored_key_type_>
std::size_t stored_key_byte(stored_key_type_ const& stored_key, std::size_t digit) noexcept {
    return word_byte(stored_key_word(stored_key, digit / sizeof(std::uint64_t)), digit % sizeof(std::uint64_t));
}

/** Orders @p values from the byte @p digit on, up to @p digits, most significant first. */
template <typename value_type_, typename digit_type_, typename order_type_>
void radix_sort_(std::span<value_type_> values, std::size_t digit, std::size_t digits, digit_type_ const& digit_of,
                 order_type_ const& precedes) noexcept {
    // Below this many values, moving them beats clearing and walking a 256-bucket table.
    constexpr std::size_t insertion_limit_k = 64;
    if (values.size() < insertion_limit_k) {
        for (std::size_t index = 1; index < values.size(); ++index) {
            value_type_ moved = std::move(values[index]);
            std::size_t position = index;
            for (; position != 0 && precedes(moved, values[position - 1]); --position)
                values[position] = std::move(values[position - 1]);
            values[position] = std::move(moved);
        }
        return;
    }
    // One repeated value, as a batch rewriting a key yields, is already in order.
    std::size_t differing = 1;
    while (differing != values.size() && !precedes(values[0], values[differing]) &&
           !precedes(values[differing], values[0]))
        ++differing;
    if (differing == values.size()) return;
    for (; digit != digits; ++digit) {
        std::array<std::size_t, 256> ends {};
        for (value_type_ const& value : values) ++ends[digit_of(value, digit)];
        if (ends[digit_of(values[0], digit)] == values.size()) continue;
        std::array<std::size_t, 256> next;
        for (std::size_t bucket = 0, start = 0; bucket != 256; ++bucket) {
            next[bucket] = start;
            start += ends[bucket];
            ends[bucket] = start;
        }
        // Each swap drops one value into its bucket for good, so the cycles finish in one sweep.
        for (std::size_t bucket = 0; bucket != 256; ++bucket)
            while (next[bucket] != ends[bucket]) {
                std::size_t const home = digit_of(values[next[bucket]], digit);
                if (home == bucket) ++next[bucket];
                else std::swap(values[next[bucket]], values[next[home]++]);
            }
        for (std::size_t bucket = 0, begin = 0; bucket != 256; begin = ends[bucket++])
            if (ends[bucket] - begin > 1)
                radix_sort_(values.subspan(begin, ends[bucket] - begin), digit + 1, digits, digit_of, precedes);
        return;
    }
}

/** Sorts @p values in place by @p digits bytes, most significant first, that @p digit_of reads as
 *  <tt>digit_of(value, digit)</tt>, allocating nothing: an insertion sort under @p precedes below 64
 *  values, otherwise an MSD radix skipping the bytes every value shares. @p precedes must order as
 *  the bytes do; values it calls equal land in no particular order among themselves. */
template <typename value_type_, typename digit_type_, typename order_type_>
void radix_sort(std::span<value_type_> values, std::size_t digits, digit_type_ const& digit_of,
                order_type_ const& precedes) noexcept {
    radix_sort_(values, 0, digits, digit_of, precedes);
}

/** Sorts @p keyed by their @c stored_key in place, allocating nothing; entries of one key land in
 *  no particular order among themselves. */
template <typename keyed_type_>
void sort_by_stored_key(std::span<keyed_type_> keyed) noexcept {
    using stored_key_t = typename keyed_type_::stored_key_t;
    auto const digit_of = [](keyed_type_ const& entry, std::size_t digit) noexcept {
        return stored_key_byte(entry.stored_key, digit);
    };
    auto const precedes = [](keyed_type_ const& first, keyed_type_ const& second) noexcept {
        return first.stored_key < second.stored_key;
    };
    radix_sort(keyed, sizeof(stored_key_t), digit_of, precedes);
}

/** Sifts the last of @p heap up into the heap the values before it form, whose top
 *  no value precedes. */
template <typename value_type_, typename order_type_>
void heap_sift_up(std::span<value_type_> heap, order_type_ const& precedes) noexcept {
    for (std::size_t child = heap.size() - 1; child != 0;) {
        std::size_t const parent = (child - 1) / 2;
        if (!precedes(heap[parent], heap[child])) return;
        std::swap(heap[parent], heap[child]);
        child = parent;
    }
}

/** Sifts the value at @p root of @p heap down below every value it precedes. */
template <typename value_type_, typename order_type_>
void heap_sift_down(std::span<value_type_> heap, std::size_t root, order_type_ const& precedes) noexcept {
    for (std::size_t child = 2 * root + 1; child < heap.size(); root = child, child = 2 * root + 1) {
        if (child + 1 < heap.size() && precedes(heap[child], heap[child + 1])) ++child;
        if (!precedes(heap[root], heap[child])) return;
        std::swap(heap[root], heap[child]);
    }
}

/** Sorts @p values ascending under @p precedes in place through a heap, allocating nothing; values
 *  it calls equal land in no particular order among themselves. */
template <typename value_type_, typename order_type_>
void heap_sort(std::span<value_type_> values, order_type_ const& precedes) noexcept {
    for (std::size_t parent = values.size() / 2; parent-- != 0;) heap_sift_down(values, parent, precedes);
    for (std::size_t size = values.size(); size > 1;) {
        std::swap(values[0], values[--size]);
        heap_sift_down(values.first(size), 0, precedes);
    }
}

/**
 *  @brief Sorts @p values ascending under @p precedes, keeping equal values in their order:
 *      insertion-sorted runs of 16, then merge passes between @p values and @p scratch, ending in
 *      @p values, allocating nothing.
 *  @param[in,out] scratch At least as many values as @p values, each assignable, whose contents
 *      are left moved-from.
 */
template <typename value_type_, typename order_type_>
void merge_sort(std::span<value_type_> values, std::span<value_type_> scratch, order_type_ const& precedes) noexcept {
    // Longer runs spend more comparisons shifting than a merge level would.
    constexpr std::size_t run_k = 16;
    std::size_t const count = values.size();
    value_type_* source = values.data();
    value_type_* target = scratch.data();
    for (std::size_t first = 0; first < count; first += run_k) {
        value_type_* const begin = source + first;
        value_type_* const end = source + std::min(first + run_k, count);
        for (value_type_* next = begin + 1; next < end; ++next) {
            value_type_ moved = std::move(*next);
            value_type_* hole = next;
            for (; hole != begin && precedes(moved, hole[-1]); --hole) *hole = std::move(hole[-1]);
            *hole = std::move(moved);
        }
    }
    for (std::size_t width = run_k; width < count; width *= 2) {
        for (std::size_t first = 0; first < count; first += 2 * width) {
            value_type_* left = source + first;
            value_type_* const middle = source + std::min(first + width, count);
            value_type_* right = middle;
            value_type_* const end = source + std::min(first + 2 * width, count);
            value_type_* written = target + first;
            // The right value goes first only when it strictly precedes, which keeps
            // the merge stable.
            while (left != middle && right != end)
                if (precedes(*right, *left)) *written++ = std::move(*right++);
                else *written++ = std::move(*left++);
            while (left != middle) *written++ = std::move(*left++);
            while (right != end) *written++ = std::move(*right++);
        }
        std::swap(source, target);
    }
    if (source != values.data())
        for (std::size_t index = 0; index != count; ++index) values[index] = std::move(source[index]);
}

/**
 *  @brief Names the least head among the sorted runs one pass merges: a tournament whose inner
 *      nodes keep the run that lost the match played there, so moving the least run's head on
 *      replays only the path above its leaf.
 *
 *  Run @c i plays from leaf @c runs + i of a tree laid out as a heap, whose @c runs - 1 inner nodes
 *  give two children to every node for any count of runs. The caller's order tells whether one
 *  run's head precedes another's, an exhausted run's preceding none. Naming the least head is the
 *  one step a merge kit takes over with a vector minimum across the heads.
 */
template <typename allocator_type_ = st::default_allocator<std::uint32_t>>
class loser_tree {
  public:
    using allocator_t = allocator_type_;

    loser_tree() noexcept = default;

    /** Allocates the inner nodes through @p allocator. */
    explicit loser_tree(allocator_t const& allocator) noexcept : losers_(allocator) {}

    /** Plays every match among the heads of @p runs runs, which @p precedes orders as
     *  <tt>precedes(first, second)</tt>. */
    template <typename order_type_>
    st::status_t reset(std::uint32_t runs, order_type_ const& precedes) noexcept {
        losers_.clear();
        if (st::status_t const grown = losers_.resize(st::assume_uninitialized, runs); grown != st::status_t::success_k)
            return grown;
        least_ = runs != 0 ? play_(1, precedes) : 0;
        return st::status_t::success_k;
    }

    /** The run whose head precedes every other's. */
    [[nodiscard]] std::uint32_t least() const noexcept { return least_; }

    /** Replays the matches above the least run's leaf, once its head moved on. */
    template <typename order_type_>
    void replay(order_type_ const& precedes) noexcept {
        std::uint32_t winner = least_;
        for (std::size_t node = (losers_.size() + winner) / 2; node != 0; node /= 2)
            if (precedes(losers_[node], winner)) std::swap(losers_[node], winner);
        least_ = winner;
    }

  private:
    /** Plays every match below @p node, keeping each loser, and answers the winner. */
    template <typename order_type_>
    std::uint32_t play_(std::size_t node, order_type_ const& precedes) noexcept {
        if (node >= losers_.size()) return static_cast<std::uint32_t>(node - losers_.size());
        std::uint32_t const left = play_(2 * node, precedes);
        std::uint32_t const right = play_(2 * node + 1, precedes);
        bool const right_wins = precedes(right, left);
        losers_[node] = right_wins ? left : right;
        return right_wins ? right : left;
    }

    /** The run that lost the match at every inner node, from node one on. */
    st::vector<std::uint32_t, allocator_t> losers_;

    /** The run whose head precedes every other's. */
    std::uint32_t least_ = 0;
};

#pragma endregion Sorted Keys

} // namespace unum::ustore::utilities
