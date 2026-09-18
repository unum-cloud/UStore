/**
 *  @file src/nvme/value_cache.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Value caches of recently selected stored values, keyed by stored key.
 */

#pragma once

#include <cstddef> // `std::byte`, `std::size_t`
#include <cstring> // `std::memcpy`

#include <algorithm> // `std::min`
#include <limits>    // `std::numeric_limits`
#include <utility>   // `std::move`, `std::swap`

#include <smashtable/basic_hash_table.hpp> // `st::hash_map`
#include <smashtable/basic_ring.hpp>       // `st::basic_ring`, `st::ring_eviction_t`

#include "nvme/resources.hpp"    // `resources_shared_ptr_t`
#include "nvme/stored_value.hpp" // `stored_value_span_t`, `stored_value_view_t`
#include "nvme/types.hpp"        // `constrained_buffer_t`, `constrained_allocator`

namespace unum::ustore::nvme {

/**
 *  @brief Stored values of recently selected keys, bounded by a count and evicted oldest first.
 *
 *  One collection owns one cache, whose bytes are charged to the database budget the cache was made
 *  with. The bound is a count rather than a byte total, derived once from the byte budget and the
 *  width the collection's values take, so a lookup and an insertion each touch one hash slot and at
 *  most one ring slot.
 */
template <typename stored_key_, value_layout_t value_layout_>
class value_cache {
  public:
    using stored_key_t = stored_key_;

    /** The layout this cache serves, which fixes how its capacity was derived. */
    static constexpr value_layout_t value_layout_k = value_layout_;

    /**
     *  @brief A cache holding what @p capacity_bytes affords of values @p value_size wide.
     *  @param[in] capacity_bytes Bytes the cache may take, where zero leaves it holding nothing.
     *  @param[in] value_size Bytes one value takes, which is its widest for a variable-width layout.
     *  @param[in] resources The budget the cached bytes are charged to.
     */
    value_cache(std::size_t capacity_bytes, stored_value_size_t value_size,
                resources_shared_ptr_t const& resources) noexcept
        : resources_(resources), cached_(st::default_hash_t {}, st::equal_to_t {},
                                         constrained_allocator<std::byte> {resources->memory_constraint}) {
        if (capacity_bytes == 0 || value_size == 0) return;
        std::size_t const bytes_per_element = bytes_per_element_k + value_size;
        capacity_ = std::min(capacity_bytes / bytes_per_element,
                             static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()));
        if (capacity_ == 0) return;
        auto oldest_first = st::basic_ring<stored_key_t, constrained_allocator<stored_key_t>>::make(
            capacity_, constrained_allocator<stored_key_t> {resources_->memory_constraint});
        if (!oldest_first) {
            capacity_ = 0;
            return;
        }
        oldest_first_ = std::move(*oldest_first);
        if (cached_.reserve(capacity_) == cached_t::reserve_result_t::failed_k) capacity_ = 0;
    }

    value_cache(value_cache const&) = delete;
    value_cache& operator=(value_cache const&) = delete;

    value_cache(value_cache&& other) noexcept
        : resources_(std::move(other.resources_)), cached_(std::move(other.cached_)),
          oldest_first_(std::move(other.oldest_first_)), capacity_(std::exchange(other.capacity_, 0)) {}

    value_cache& operator=(value_cache&& other) noexcept {
        std::swap(resources_, other.resources_);
        std::swap(cached_, other.cached_);
        std::swap(oldest_first_, other.oldest_first_);
        std::swap(capacity_, other.capacity_);
        return *this;
    }

    /**
     *  @brief Copies the value cached under @p stored_key into @p stored_value.
     *  @param[in] stored_key The key to look up.
     *  @param[out] stored_value Receives the cached bytes when they fit.
     *  @param[out] size The bytes written, or zero when nothing was.
     *  @return Whether the key was cached and its value fit.
     */
    bool get(stored_key_t stored_key, stored_value_span_t stored_value, stored_value_size_t& size) const noexcept {
        size = 0;
        bool copied = false;
        cached_.probe_to_find(stored_key, [&](auto const& slot) noexcept {
            constrained_buffer_t const& buffer = slot.value();
            if (stored_value.size() < buffer.size()) return;
            std::memcpy(stored_value.data(), buffer.data(), buffer.size());
            size = static_cast<stored_value_size_t>(buffer.size());
            copied = true;
        });
        return copied;
    }

    /** Caches @p stored_value under @p stored_key, evicting the oldest entry once the bound is reached. */
    void add(stored_key_t stored_key, stored_value_view_t stored_value) noexcept {
        if (capacity_ == 0) return;

        constrained_buffer_t buffer {constrained_allocator<std::byte> {resources_->memory_constraint}};
        if (st::failed(buffer.resize(stored_value.size()))) return;
        std::memcpy(buffer.data(), stored_value.data(), stored_value.size());

        bool replaced = false;
        cached_.probe_to_find(stored_key, [&](auto& slot) noexcept {
            slot.value_ref() = std::move(buffer);
            replaced = true;
        });
        if (replaced) return;

        stored_key_t evicted {};
        if (oldest_first_.push_evicting(stored_key_t {stored_key}, evicted) == st::ring_eviction_t::evicted_oldest_k)
            cached_.erase(evicted);
        cached_.emplace(stored_key, std::move(buffer), st::assume_reserved_t {});
    }

    /** Drops every cached value, for a collection whose files were replaced under it. */
    void unsafe_clear() noexcept {
        cached_.clear();
        oldest_first_.clear();
    }

  private:

    using cached_t = st::hash_map<stored_key_t, constrained_buffer_t, st::default_hash_t, st::equal_to_t,
                                  constrained_allocator<std::byte>>;

    /** Bytes one entry takes beyond its value: the key twice, in the table and in the ring, and the buffer header. */
    static constexpr std::size_t bytes_per_element_k = 2 * sizeof(stored_key_t) + sizeof(constrained_buffer_t);

    /** The budget every cached byte is charged to, shared with the database that made the cache. */
    resources_shared_ptr_t resources_;

    /** The cached values, addressed by their stored key. */
    cached_t cached_;

    /** The cached keys in the order they arrived, whose oldest an insertion at the bound evicts. */
    st::basic_ring<stored_key_t, constrained_allocator<stored_key_t>> oldest_first_;

    /** The entries the cache holds at once, zero when it caches nothing. */
    std::size_t capacity_ = 0;
};

} // namespace unum::ustore::nvme
