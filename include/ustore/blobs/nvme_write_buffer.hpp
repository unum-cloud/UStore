/**
 *  @file include/ustore/blobs/nvme_write_buffer.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief In-memory write buffer of stored values keyed by stored key.
 */

#pragma once

#include <cstddef> // `std::byte`, `std::size_t`

#include <algorithm> // `std::min`, `std::max`
#include <limits>    // `std::numeric_limits`
#include <new>       // placement `new`

#include <smashtable/basic_hash_table.hpp> // `st::hash_map`, `st::assume_reserved`
#include <smashtable/basic_vector.hpp>     // `st::basic_vector`

#include "ustore/blobs/nvme_key_slot.hpp"   // `limit`, `stored_value_size_t`, `delta_kind_t`
#include "ustore/utilities/nvme_ring.hpp"   // `constrained_allocator`
#include "ustore/utilities/nvme_volume.hpp" // `resources_shared_ptr`
#include "ustore/utilities/types.hpp"       // the `fu` and `st` aliases

namespace unum::ustore::blobs {

using utilities::constrained_allocator;
using utilities::notifier;
using utilities::resources_shared_ptr;

/**
 *  @brief The writes of one collection not yet sealed, keyed by stored key, charged to one database's budget.
 *  @tparam atomic_reference_ The reference the budget owns its word through.
 *  @tparam waiter_type_ What a core waiting on a batch's counter does between polls.
 */
template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
class write_buffer {
  public:
#pragma region Types

    /** The resources the buffer charges and keeps alive. */
    using resources_ptr_t = resources_shared_ptr<atomic_reference_, waiter_type_>;

    /** The handle a batched select reports each part through. */
    using notifier_t = notifier<atomic_reference_, waiter_type_>;

    /** Offset into @c stored_values_buffer, 64-bit even though files stay under 4GB, since concurrent
     *  writers may push @c next_stored_value_offset_ past the 32-bit range before the capacity check. */
    using stored_value_offset_in_buffer_t = std::size_t;

    /** Where one written value sits, or that the key was removed, and from when a transaction watches the key for
     *  another commit writing it, @c invalid_transaction_stamp_k when it does not. */
    struct stored_value_place_t {
        stored_value_offset_in_buffer_t offset;
        stored_value_size_t size;
        delta_kind_t kind = delta_kind_t::write_k;
        transaction_stamp_t watch_stamp = invalid_transaction_stamp_k;
    };

    using stored_key_t = stored_key_;
    using stored_values_hash_map_t = st::hash_map<stored_key_t, stored_value_place_t, stored_key_hash_t, st::equal_to_t,
                                                  constrained_allocator<std::byte, atomic_reference_>>;

#pragma endregion Types

#pragma region Public Members
    stored_values_hash_map_t stored_values;
    st::basic_vector<std::byte, constrained_allocator<std::byte, atomic_reference_>> stored_values_buffer;
    stored_value_offset_in_buffer_t next_stored_value_offset_ = 0;
#pragma endregion Public Members

#pragma region Construction
    inline write_buffer(std::size_t capacity_bytes, std::size_t elements_max_count, std::size_t concurrency_limit,
                        std::size_t value_size, std::size_t value_max_size, resources_ptr_t resources);
    inline write_buffer(write_buffer const& other);
    inline write_buffer(write_buffer&& other) noexcept;

    inline write_buffer& operator=(write_buffer const& other);
    inline write_buffer& operator=(write_buffer&& other) noexcept;
#pragma endregion Construction

#pragma region Interface Methods
    inline bool empty() const noexcept { return stored_values.empty(); }
    inline std::size_t count() const noexcept { return stored_values.size(); }
    inline std::size_t capacity() const noexcept { return capacity_bytes_; }
    inline std::size_t stored_value_max_size() const noexcept { return std::max(value_size_, value_max_size_); }

    inline lookup_result_t status(stored_key_t stored_key) const noexcept;

    inline status_t stored_value_size(stored_key_t stored_key, stored_value_size_t& size) const noexcept;

    inline status_t select(stored_key_t stored_key, stored_value_span_t stored_value,
                           stored_value_size_t& size) const noexcept;

    template <typename stored_value_location_>
    inline lookup_result_t find(stored_key_t stored_key, stored_value_location_& location) const noexcept;
    template <typename stored_value_location_>
    inline void select(stored_value_location_ const& location, stored_value_span_t stored_value,
                       notifier_t notifier) const noexcept;

    /** Writes @p stored_value under @p stored_key, watched from @p watch_stamp, which only lowers the stamp an
     *  earlier write of the key took. */
    inline status_t add(stored_key_t stored_key, stored_value_view_t stored_value,
                        transaction_stamp_t watch_stamp = invalid_transaction_stamp_k) noexcept;

    /** Writes a removal of @p stored_key, watched from @p watch_stamp as @c add is. */
    inline status_t mark_as_removed(stored_key_t stored_key,
                                    transaction_stamp_t watch_stamp = invalid_transaction_stamp_k) noexcept;

    /** Moves every watched write's watch to @p watch_stamp, for a transaction started over with its writes kept. */
    inline void rescope_watches(transaction_stamp_t watch_stamp) noexcept;
#pragma endregion Interface Methods

#pragma region Batch Methods
    template <typename stored_value_location_>
    inline status_t find(std::span<stored_key_t const> stored_keys, std::span<stored_value_location_> locations,
                         missing_locations_t missing_locations = missing_locations_t::invalidate_k) const;
    template <typename stored_value_location_>
    inline void select(std::span<stored_value_location_ const> locations, bytes_span_t buffer,
                       notifier_t notifier) const;
#pragma endregion Batch Methods

#pragma region Validators
    inline status_t check_stored_values(std::span<stored_key_t const> stored_keys,
                                        std::span<stored_value_size_t const> sizes) const noexcept;
    inline status_t check_stored_values(write_buffer const& other) const noexcept;
    inline status_t check_stored_value(stored_key_t stored_key, stored_value_size_t stored_value_size) const noexcept;

    inline bool can_fit(std::size_t stored_values_count, std::size_t stored_values_sum_size) const noexcept;
#pragma endregion Validators

#pragma region Unsafe Methods
    inline status_t unsafe_add(write_buffer const& other) noexcept;
    inline void unsafe_clear() noexcept;
    inline void unsafe_drop_stored_values() noexcept;
#pragma endregion Unsafe Methods

#pragma region Private Members
  private:

    /** The earlier of @p watch_stamp and the watch an earlier write of @p stored_key took. */
    inline transaction_stamp_t earlier_watch_(stored_key_t stored_key, transaction_stamp_t watch_stamp) const noexcept;

    resources_ptr_t resources_;
    std::size_t capacity_bytes_ = 0;
    std::size_t elements_max_count_ = 0;
    std::size_t concurrency_limit_ = 0;
    std::size_t value_size_ = 0;
    std::size_t value_max_size_ = 0;
#pragma endregion Private Members
};

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline write_buffer<stored_key_, atomic_reference_, waiter_type_>::write_buffer(
    std::size_t capacity_bytes, std::size_t elements_max_count, std::size_t concurrency_limit, std::size_t value_size,
    std::size_t value_max_size, resources_ptr_t resources)
    : stored_values({}, {}, constrained_allocator<std::byte, atomic_reference_> {resources->memory_constraint}),
      stored_values_buffer(constrained_allocator<std::byte, atomic_reference_> {resources->memory_constraint}),
      next_stored_value_offset_(0), resources_(resources), capacity_bytes_(capacity_bytes),
      elements_max_count_(elements_max_count), concurrency_limit_(concurrency_limit), value_size_(value_size),
      value_max_size_(value_max_size) {
    assert(!(value_size > 0 && value_max_size > 0) && "Only one allowed");
    // A budget that refuses the bytes leaves the buffer empty, and `can_fit` then refuses every write.
    if (st::failed(stored_values_buffer.resize(capacity_bytes)) ||
        stored_values.reserve(elements_max_count) == stored_values_hash_map_t::reserve_result_t::failed_k)
        capacity_bytes_ = 0;
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline write_buffer<stored_key_, atomic_reference_, waiter_type_>::write_buffer(write_buffer const& other)
    : stored_values({}, {}, constrained_allocator<std::byte, atomic_reference_> {other.resources_->memory_constraint}),
      stored_values_buffer(constrained_allocator<std::byte, atomic_reference_> {other.resources_->memory_constraint}),
      next_stored_value_offset_(0), resources_(other.resources_), capacity_bytes_(other.capacity_bytes_),
      elements_max_count_(other.elements_max_count_), concurrency_limit_(other.concurrency_limit_),
      value_size_(other.value_size_), value_max_size_(other.value_max_size_) {
    if (st::failed(stored_values_buffer.resize(other.capacity_bytes_)) ||
        stored_values.reserve(other.elements_max_count_) == stored_values_hash_map_t::reserve_result_t::failed_k) {
        capacity_bytes_ = 0;
        return;
    }
    // A buffer of the source's own capacity always fits its contents, so the copy cannot be refused.
    [[maybe_unused]] status_t const copied = unsafe_add(other);
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline write_buffer<stored_key_, atomic_reference_, waiter_type_>::write_buffer(write_buffer&& other) noexcept
    : stored_values(std::move(other.stored_values)), stored_values_buffer(std::move(other.stored_values_buffer)),
      next_stored_value_offset_(std::move(other.next_stored_value_offset_)), resources_(std::move(other.resources_)),
      capacity_bytes_(std::move(other.capacity_bytes_)), elements_max_count_(std::move(other.elements_max_count_)),
      concurrency_limit_(std::move(other.concurrency_limit_)), value_size_(std::move(other.value_size_)),
      value_max_size_(std::move(other.value_max_size_)) {}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline write_buffer<stored_key_, atomic_reference_, waiter_type_>&
write_buffer<stored_key_, atomic_reference_, waiter_type_>::operator=(write_buffer const& other) {

    stored_values = stored_values_hash_map_t(
        {}, {}, constrained_allocator<std::byte, atomic_reference_> {resources_->memory_constraint});
    stored_values_buffer = st::basic_vector<std::byte, constrained_allocator<std::byte, atomic_reference_>>(
        constrained_allocator<std::byte, atomic_reference_> {resources_->memory_constraint});
    resources_ = other.resources_;
    next_stored_value_offset_ = 0;

    bool const reserved = !st::failed(stored_values_buffer.resize(other.capacity_bytes_)) &&
                          stored_values.reserve(other.elements_max_count_) !=
                              stored_values_hash_map_t::reserve_result_t::failed_k;
    capacity_bytes_ = reserved ? other.capacity_bytes_ : 0;
    elements_max_count_ = other.elements_max_count_;
    concurrency_limit_ = other.concurrency_limit_;
    value_size_ = other.value_size_;
    value_max_size_ = other.value_max_size_;

    // A buffer of the source's own capacity always fits its contents, so the copy cannot be refused.
    [[maybe_unused]] status_t const copied = unsafe_add(other);
    return *this;
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline write_buffer<stored_key_, atomic_reference_, waiter_type_>&
write_buffer<stored_key_, atomic_reference_, waiter_type_>::operator=(write_buffer&& other) noexcept {
    std::swap(stored_values, other.stored_values);
    std::swap(stored_values_buffer, other.stored_values_buffer);
    std::swap(next_stored_value_offset_, other.next_stored_value_offset_);
    std::swap(resources_, other.resources_);
    std::swap(capacity_bytes_, other.capacity_bytes_);
    std::swap(elements_max_count_, other.elements_max_count_);
    std::swap(concurrency_limit_, other.concurrency_limit_);
    std::swap(value_size_, other.value_size_);
    std::swap(value_max_size_, other.value_max_size_);
    return *this;
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline lookup_result_t write_buffer<stored_key_, atomic_reference_, waiter_type_>::status(
    stored_key_t stored_key) const noexcept {

    auto result = lookup_result_t::missing_k;
    stored_values.probe_to_find(
        stored_key,
        [&result](auto const& entry) {
            constexpr lookup_result_t results[2] = {
                lookup_result_t::written_k,
                lookup_result_t::removed_k,
            };
            result = results[entry.value().kind == delta_kind_t::remove_k];
        },
        st::assume_reserved);

    return result;
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline status_t write_buffer<stored_key_, atomic_reference_, waiter_type_>::stored_value_size(
    stored_key_t stored_key, stored_value_size_t& size) const noexcept {

    auto status = status_t {status_code_t::not_found_k, "Object not exists"};
    stored_values.probe_to_find(
        stored_key,
        [&](auto const& entry) {
            if (entry.value().kind != delta_kind_t::remove_k) size = entry.value().size;
            status = status_t {};
        },
        st::assume_reserved);

    return status;
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline status_t write_buffer<stored_key_, atomic_reference_, waiter_type_>::select(
    stored_key_t stored_key, stored_value_span_t stored_value, stored_value_size_t& size) const noexcept {

    status_t status = status_t {status_code_t::not_found_k, "Object not exists"};
    stored_values.probe_to_find(
        stored_key,
        [&](auto const& entry) {
            auto const& stored_value_place = entry.value();
            if (stored_value_place.kind == delta_kind_t::remove_k) {
                status = status_t {status_code_t::not_found_k, "Object not exists"};
                return;
            }
            if (stored_value_place.size == 0) {
                size = 0;
                status = status_t {};
                return;
            }
            if (stored_value.size() < stored_value_place.size) {
                status = status_t {status_code_t::invalid_argument_k, "Buffer is too small"};
                return;
            }

            auto span = as_bytes_view(stored_values_buffer).subspan(stored_value_place.offset, stored_value_place.size);
            std::memcpy(stored_value.data(), span.data(), span.size());
            size = span.size();
            status = status_t {};
        },
        st::assume_reserved);

    return status;
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
template <typename stored_value_location_>
inline lookup_result_t write_buffer<stored_key_, atomic_reference_, waiter_type_>::find(
    stored_key_t stored_key, stored_value_location_& location) const noexcept {

    location.size_bytes = 0;
    location.offset = 0;
    location.kind = delta_kind_t::remove_k;
    location.recipient_id = reinterpret_cast<u64_t>(this);
    location.recipient_state_id = 0;

    auto result = lookup_result_t::missing_k;
    stored_values.probe_to_find(
        stored_key,
        [&](auto const& entry) {
            auto const& stored_value_place = entry.value();
            location.offset = static_cast<stored_value_offset_t>(stored_value_place.offset);
            location.size_bytes = stored_value_place.size;
            location.kind = stored_value_place.kind;
            constexpr lookup_result_t results[2] = {lookup_result_t::written_k, lookup_result_t::removed_k};
            result = results[stored_value_place.kind == delta_kind_t::remove_k];
        },
        st::assume_reserved);

    return result;
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
template <typename stored_value_location_>
inline void write_buffer<stored_key_, atomic_reference_, waiter_type_>::select(stored_value_location_ const& location,
                                                                               stored_value_span_t stored_value,
                                                                               notifier_t notifier) const noexcept {
    if (!location) {
        notifier.fail({status_code_t::invalid_argument_k, "A stored value location naming nothing"});
        return;
    }
    if (stored_value.size() < location.aligned_buffer_size()) {
        notifier.fail({status_code_t::invalid_argument_k, "A buffer shorter than the stored value"});
        return;
    }

    if (location.recipient_id != reinterpret_cast<u64_t>(this)) {
        notifier.fail({status_code_t::invalid_argument_k, "A stored value location of another write buffer"});
        return;
    }
    if (location.recipient_state_id != 0) {
        notifier.fail({status_code_t::invalid_argument_k, "A stored value location of another buffer state"});
        return;
    }

    if (location.size_bytes > 0) {
        auto span = as_bytes_view(stored_values_buffer).subspan(location.offset, location.size_bytes);
        std::memcpy(stored_value.data() + location.buffer_padding(), span.data(), span.size());
    }
    notifier.did_one();
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline status_t write_buffer<stored_key_, atomic_reference_, waiter_type_>::add(
    stored_key_t stored_key, stored_value_view_t stored_value, transaction_stamp_t watch_stamp) noexcept {

    auto status = check_stored_value(stored_key, stored_value.size());
    if (status.failed()) return status;

    if (!can_fit(1, stored_value.size())) [[unlikely]]
        return status_t {status_code_t::capacity_exhausted_k, "Cache is full"};

    stored_value_offset_in_buffer_t stored_value_offset = 0;
    if (!stored_value.empty()) {
        stored_value_offset = next_stored_value_offset_;
        next_stored_value_offset_ += stored_value.size();
        std::memcpy(stored_values_buffer.data() + stored_value_offset, stored_value.data(), stored_value.size());
    }
    stored_values.emplace(stored_key,
                          stored_value_place_t {stored_value_offset, stored_value_size_t(stored_value.size()),
                                                delta_kind_t::write_k, earlier_watch_(stored_key, watch_stamp)},
                          st::assume_reserved);

    return status_t {};
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline status_t write_buffer<stored_key_, atomic_reference_, waiter_type_>::mark_as_removed(
    stored_key_t stored_key, transaction_stamp_t watch_stamp) noexcept {

    if (!can_fit(1, 0)) [[unlikely]]
        return status_t {status_code_t::capacity_exhausted_k, "Cache is full"};

    stored_values.emplace(stored_key,
                          stored_value_place_t {0, 0, delta_kind_t::remove_k, earlier_watch_(stored_key, watch_stamp)},
                          st::assume_reserved);
    return status_t {};
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline void write_buffer<stored_key_, atomic_reference_, waiter_type_>::rescope_watches(
    transaction_stamp_t watch_stamp) noexcept {
    [[maybe_unused]] st::status_t const walked = stored_values.for_each([&](auto& entry) noexcept {
        stored_value_place_t& place = entry.value_ref();
        if (place.watch_stamp != invalid_transaction_stamp_k) place.watch_stamp = watch_stamp;
    });
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline transaction_stamp_t write_buffer<stored_key_, atomic_reference_, waiter_type_>::earlier_watch_(
    stored_key_t stored_key, transaction_stamp_t watch_stamp) const noexcept {
    stored_values.probe_to_find(stored_key, [&](auto const& entry) noexcept {
        watch_stamp = std::min(watch_stamp, entry.value().watch_stamp);
    });
    return watch_stamp;
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
template <typename stored_value_location_>
inline status_t write_buffer<stored_key_, atomic_reference_, waiter_type_>::find(
    std::span<stored_key_t const> stored_keys, std::span<stored_value_location_> locations,
    missing_locations_t missing_locations) const {

    if (locations.size() < stored_keys.size())
        return status_t {status_code_t::invalid_argument_k, "Buffer is too small"};

    if (missing_locations == missing_locations_t::invalidate_k) {
        for (std::size_t index = 0; index < stored_keys.size(); ++index) {
            auto& location = locations[index];
            find<stored_value_location_>(stored_keys[index], location);
        }
    }
    else {
        for (std::size_t index = 0; index < stored_keys.size(); ++index) {
            stored_value_location_ location;
            auto status = find<stored_value_location_>(stored_keys[index], location);
            if (status != lookup_result_t::missing_k) locations[index] = location;
        }
    }

    return status_t {};
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
template <typename stored_value_location_>
inline void write_buffer<stored_key_, atomic_reference_, waiter_type_>::select(
    std::span<stored_value_location_ const> locations, bytes_span_t buffer, notifier_t notifier) const {

    for (std::size_t index = 0, offset = 0; (index < locations.size()) & !notifier.has_failed(); ++index) {
        auto const& location = locations[index];
        auto is_recipient = location.recipient_id == reinterpret_cast<u64_t>(this);
        if (is_recipient) {
            if (location.size_bytes > 0) {
                auto stored_value = buffer.subspan(offset, location.aligned_buffer_size());
                select<stored_value_location_>(location, stored_value, notifier);
            }
            else notifier.did_one();
        }
        offset += location.aligned_buffer_size();
    }
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline status_t write_buffer<stored_key_, atomic_reference_, waiter_type_>::check_stored_values(
    std::span<stored_key_t const> stored_keys, std::span<stored_value_size_t const> sizes) const noexcept {

    status_t status = status_t {status_code_t::not_found_k, "It is empty"};
    bool break_loop = false;
    for (std::size_t index = 0; (index < stored_keys.size()) & !break_loop; ++index) {
        status = check_stored_value(stored_keys[index], sizes[index]);
        break_loop = status.failed();
    }

    return status;
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline status_t write_buffer<stored_key_, atomic_reference_, waiter_type_>::check_stored_values(
    write_buffer const& other) const noexcept {

    status_t status = status_t {status_code_t::not_found_k, "It is empty"};
    [[maybe_unused]] st::status_t const walked = other.stored_values.for_each([&](auto const& entry) noexcept {
        status = check_stored_value(entry.key(), entry.value().size);
        return status.failed() ? st::walk_control_t::halt_k : st::walk_control_t::resume_k;
    });
    return status;
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline status_t write_buffer<stored_key_, atomic_reference_, waiter_type_>::check_stored_value(
    stored_key_t stored_key, stored_value_size_t stored_value_size) const noexcept {

    bool valid_key = stored_key != limit<stored_key_t>::max();
    bool valid_fixed_width_stored_value = (value_size_ > 0) & (stored_value_size == value_size_);
    bool valid_variable_width_stored_value = stored_value_size <= value_max_size_;
    bool valid = valid_key & (valid_fixed_width_stored_value ^ valid_variable_width_stored_value);
    return valid ? status_t {} : status_t {status_code_t::malformed_k, "Invalid stored value"};
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline bool write_buffer<stored_key_, atomic_reference_, waiter_type_>::can_fit(
    std::size_t stored_values_count, std::size_t stored_values_sum_size) const noexcept {
    auto used_memory = next_stored_value_offset_;
    bool has_element_place = stored_values.size() + stored_values_count <= elements_max_count_;
    // A location names its offset in 32 bits, which every value of the buffer stays within.
    bool has_enough_space = (used_memory + stored_values_sum_size <= capacity_bytes_) &
                            (used_memory + stored_values_sum_size <= std::numeric_limits<stored_value_offset_t>::max());
    return has_element_place & has_enough_space;
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline status_t write_buffer<stored_key_, atomic_reference_, waiter_type_>::unsafe_add(
    write_buffer const& other) noexcept {

    if (!can_fit(other.stored_values.size(), other.next_stored_value_offset_)) [[unlikely]]
        return status_t {status_code_t::capacity_exhausted_k, "Cache is full"};

    auto status = check_stored_values(other);
    if (status.failed()) return status;

    [[maybe_unused]] auto const walked = other.stored_values.for_each([&](auto const& other_entry) {
        auto const& stored_value_place = other_entry.value();
        if (stored_value_place.size != 0) {
            auto other_stored_value =
                as_bytes_view(other.stored_values_buffer).subspan(stored_value_place.offset, stored_value_place.size);
            std::memcpy(stored_values_buffer.data() + next_stored_value_offset_, other_stored_value.data(),
                        other_stored_value.size());
            stored_values.emplace(
                other_entry.key(),
                stored_value_place_t {next_stored_value_offset_, stored_value_place.size, stored_value_place.kind},
                st::assume_reserved);

            next_stored_value_offset_ += stored_value_place.size;
        }
        else
            stored_values.emplace(other_entry.key(), stored_value_place_t {0, 0, stored_value_place.kind},
                                  st::assume_reserved);
    });

    return status_t {};
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline void write_buffer<stored_key_, atomic_reference_, waiter_type_>::unsafe_clear() noexcept {
    stored_values.clear();
    next_stored_value_offset_ = 0;
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline void write_buffer<stored_key_, atomic_reference_, waiter_type_>::unsafe_drop_stored_values() noexcept {

    // Only a cache of variable-width stored values accepts an empty stored value
    assert(check_stored_value(limit<stored_key_t>::min(), 0).succeeded() && "Not supported");

    [[maybe_unused]] st::status_t const walked = stored_values.for_each([&](auto const& entry) noexcept {
        auto& stored_value_place = entry.value_ref();
        stored_value_place.offset = 0;
        stored_value_place.size = 0;
        return st::walk_control_t::resume_k;
    });
    next_stored_value_offset_ = 0;
}

} // namespace unum::ustore::blobs
