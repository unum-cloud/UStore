/**
 *  @file src/nvme/value_cache.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Values of recently read keys, bounded by a count and evicted oldest first.
 *
 *  A hit is served as a view of the bytes the cache already owns rather than as a copy into a buffer
 *  the caller had to size first, so a batched read that finds a key here costs one hash probe and no
 *  allocation at all. The bound is a count rather than a byte total, derived once from the budget and
 *  the width the collection's values take.
 */

#pragma once

#include <cstddef> // `std::byte`, `std::size_t`
#include <cstring> // `std::memcpy`

#include <algorithm> // `std::min`
#include <bit>       // `std::bit_floor`
#include <span>      // `std::span`
#include <utility>   // `std::exchange`, `std::move`, `std::swap`

#include <smashtable/basic_hash_table.hpp> // `st::hash_map`, `st::assume_reserved_t`
#include <smashtable/basic_ring.hpp>       // `st::basic_ring`, `st::ring_eviction_t`

#include <ustore/types.hpp> // `byte_t`

#include "nvme/constrained_allocator.hpp" // `resource_constraint_t`, `constrained_allocator`
#include "nvme/stored_value.hpp"          // `stored_value_size_t`
#include "nvme/types.hpp"                 // `constrained_buffer_t`, `value_layout_t`

namespace unum::ustore::nvme {

#pragma region Cache

/**
 *  @brief Values of recently read keys, bounded by a count and evicted oldest first.
 *
 *  One collection owns one cache, whose bytes are charged to the budget the cache was made with. The
 *  budget is borrowed and must outlive the cache.
 */
template <typename stored_key_type_, value_layout_t value_layout_>
class value_cache {
  public:

#pragma region Types

    /** The key the cache addresses its values by. */
    using stored_key_t = stored_key_type_;

    /** The layout this cache serves, which fixes how its capacity was derived. */
    static constexpr value_layout_t value_layout_k = value_layout_;

#pragma endregion Types

#pragma region Construction

    value_cache() noexcept = default;
    value_cache(value_cache const&) = delete;
    value_cache& operator=(value_cache const&) = delete;

    value_cache(value_cache&& other) noexcept
        : constraint_(std::exchange(other.constraint_, nullptr)), cached_(std::move(other.cached_)),
          oldest_first_(std::move(other.oldest_first_)), capacity_(std::exchange(other.capacity_, 0)) {}

    value_cache& operator=(value_cache&& other) noexcept {
        if (this == &other) return *this;
        std::swap(constraint_, other.constraint_);
        std::swap(cached_, other.cached_);
        std::swap(oldest_first_, other.oldest_first_);
        std::swap(capacity_, other.capacity_);
        return *this;
    }

    /**
     *  @brief A cache holding what @p capacity_bytes of @p constraint afford of values @p value_size wide.
     *  @param[in] capacity_bytes Bytes the cache may take, where zero leaves it holding nothing.
     *  @param[in] value_size Bytes one value takes, which is its widest for a variable-width layout.
     *  @param[inout] constraint The budget the cached bytes are charged to, which must outlive the cache.
     */
    [[nodiscard]] static value_cache of(std::size_t capacity_bytes, stored_value_size_t value_size,
                                        resource_constraint_t& constraint) noexcept {
        value_cache cache;
        cache.constraint_ = &constraint;
        cache.cached_ = cached_t {st::hash<stored_key_t> {}, st::equal_to_t {},
                                  constrained_allocator<std::byte> {constraint}};
        if (capacity_bytes == 0 || value_size == 0) return cache;

        std::size_t const bytes_per_element = bytes_per_element_k + value_size;
        std::size_t const affordable = std::min(capacity_bytes / bytes_per_element, oldest_first_t::capacity_limit_k);
        if (affordable == 0) return cache;

        // The eviction ring takes a power-of-two count of keys and refuses any other, so the budget
        // is rounded down to one rather than silently leaving the cache holding nothing.
        cache.capacity_ = std::bit_floor(affordable);
        auto oldest_first = oldest_first_t::make(cache.capacity_, constrained_allocator<stored_key_t> {constraint});
        if (!oldest_first) {
            cache.capacity_ = 0;
            return cache;
        }
        cache.oldest_first_ = std::move(*oldest_first);
        if (cache.cached_.reserve(cache.capacity_) == cached_t::reserve_result_t::failed_k) cache.capacity_ = 0;
        return cache;
    }

#pragma endregion Construction

#pragma region Lookups

    /** The entries the cache holds at once, zero when it caches nothing. */
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    /** The entries the cache holds right now. */
    [[nodiscard]] std::size_t size() const noexcept { return cached_.size(); }

    /**
     *  @brief Calls @p visitor with the bytes cached under @p stored_key, which the cache still owns.
     *  @return Whether the key was cached, which is the only case the visitor is called in.
     */
    template <typename visitor_type_>
    [[nodiscard]] bool visit(stored_key_t stored_key, visitor_type_&& visitor) const noexcept {
        bool seen = false;
        cached_.probe_to_find(stored_key, [&](auto const& slot) noexcept {
            constrained_buffer_t const& buffer = slot.value();
            visitor(std::span<byte_t const> {buffer});
            seen = true;
        });
        return seen;
    }

    /** Caches @p stored_value under @p stored_key, evicting the oldest entry once the bound is reached. */
    void add(stored_key_t stored_key, std::span<byte_t const> stored_value) noexcept {
        if (capacity_ == 0) return;

        constrained_buffer_t buffer {constrained_allocator<std::byte> {*constraint_}};
        if (st::failed(buffer.resize(stored_value.size()))) return;
        if (!stored_value.empty()) std::memcpy(buffer.data(), stored_value.data(), stored_value.size());

        bool replaced = false;
        cached_.probe_to_find(stored_key, [&](auto& slot) noexcept {
            slot.value_ref() = std::move(buffer);
            replaced = true;
        });
        if (replaced) return;

        stored_key_t evicted {};
        if (oldest_first_.push_evicting(stored_key_t {stored_key}, evicted) == st::ring_eviction_t::evicted_oldest_k) {
            [[maybe_unused]] bool const dropped = cached_.erase(evicted);
        }
        cached_.emplace(stored_key, std::move(buffer), st::assume_reserved_t {});
    }

    /** Drops every cached value, which a flush does because a staged write has replaced them. */
    void clear() noexcept {
        cached_.clear();
        oldest_first_.clear();
    }

#pragma endregion Lookups

  private:

#pragma region Private Types

    /** The cached values, addressed by their stored key. */
    using cached_t = st::hash_map<stored_key_t, constrained_buffer_t, st::hash<stored_key_t>, st::equal_to_t,
                                  constrained_allocator<std::byte>>;

    /** The cached keys in the order they arrived. */
    using oldest_first_t = st::basic_ring<stored_key_t, constrained_allocator<stored_key_t>>;

#pragma endregion Private Types

#pragma region Private Members

    /** Bytes one entry takes beyond its value: the key twice, in the table and in the ring, and the buffer header. */
    static constexpr std::size_t bytes_per_element_k = 2 * sizeof(stored_key_t) + sizeof(constrained_buffer_t);

    /** The budget every cached byte is charged to, null until the cache is made. */
    resource_constraint_t* constraint_ = nullptr;

    /** The cached values, addressed by their stored key. */
    cached_t cached_;

    /** The cached keys in the order they arrived, whose oldest an insertion at the bound evicts. */
    oldest_first_t oldest_first_;

    /** The entries the cache holds at once, zero when it caches nothing. */
    std::size_t capacity_ = 0;

#pragma endregion Private Members
};

#pragma endregion Cache

} // namespace unum::ustore::nvme
