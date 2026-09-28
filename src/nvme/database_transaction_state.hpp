/**
 *  @file src/nvme/database_transaction_state.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Uncommitted writes and watched keys of a database transaction, grouped by collection.
 */
#pragma once

#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <new>     // placement `new`
#include <span>    // `std::span`
#include <utility> // `std::move`, `std::swap`

#include <smashtable/basic_hash_table.hpp> // `st::hash_map`, `st::assume_reserved_t`
#include <smashtable/basic_vector.hpp>     // `st::basic_vector`

#include "nvme/read_set.hpp"     // `read_set`, `transaction_stamp_t`
#include "nvme/write_buffer.hpp" // `write_buffer_status_t`, `stored_value_size_t`

namespace unum::ustore::nvme {

#pragma region Grouped Write Buffer

/**
 *  @brief The uncommitted writes of one transaction, keyed by stored key inside each collection.
 *  @note Owned by one transaction and touched by its thread alone, so nothing here is atomic.
 */
template <typename stored_key_>
class grouped_write_buffer {
  public:
#pragma region Types

    using stored_key_t = stored_key_;
    using group_id_t = collection_id_t;
    using stored_value_offset_in_buffer_t = std::size_t;

    /** The value widths one collection admits, of which exactly one is ever set. */
    struct group_config_t {

        /** The width every value of a fixed-width collection has, zero for a variable-width one. */
        std::size_t value_size;

        /** The width no value of a variable-width collection exceeds, zero for a fixed-width one. */
        std::size_t value_max_size;
    };

    /** What one collection's share of the buffer holds, which a stage sizes a flush against. */
    struct group_info_t {

        /** The collection these writes belong to. */
        group_id_t id;

        /** The keys written inside it. */
        std::size_t elements_count;

        /** The bytes their values occupy. */
        std::size_t elements_size_bytes;
    };

    /** Where one written value sits in the insertions buffer, or that the key was removed. */
    struct stored_value_place_t {

        /** The first byte of the value, @c tombstone_offset_k for a removal and
         *  @c empty_stored_value_offset_k for a value of no bytes. */
        stored_value_offset_in_buffer_t offset;

        /** The bytes of the value. */
        stored_value_size_t size;
    };

    /** One collection's share of the buffer. */
    struct group_t {
        using stored_values_t = st::hash_map<stored_key_t, stored_value_place_t, stored_key_hash_t, st::equal_to_t,
                                             constrained_allocator<std::byte>>;

        /** The value widths the collection admits, which every write is checked against. */
        group_config_t config;

        /** Where each written key's value sits. */
        stored_values_t stored_values;

        /** The bytes those values occupy. */
        std::size_t elements_size_bytes;
    };

    using groups_t =
        st::hash_map<group_id_t, group_t, collection_id_hash_t, st::equal_to_t, constrained_allocator<std::byte>>;

#pragma endregion Types

#pragma region Public Members

    /** One share of the buffer per written collection. */
    groups_t groups_by_id;

    /** The bytes every written value of every collection sits in. */
    st::basic_vector<std::byte, constrained_allocator<std::byte>> insertions_buffer;

    /** The first free byte of @c insertions_buffer. */
    stored_value_offset_in_buffer_t next_inserted_offset = 0;

#pragma endregion Public Members

#pragma region Construction

    inline grouped_write_buffer(std::size_t capacity_bytes, std::size_t elements_max_count,
                                resources_shared_ptr_t resources) noexcept;

    inline grouped_write_buffer(grouped_write_buffer const& other) = delete;
    inline grouped_write_buffer(grouped_write_buffer&& other) noexcept;
    inline grouped_write_buffer& operator=(grouped_write_buffer const& other) = delete;
    inline grouped_write_buffer& operator=(grouped_write_buffer&& other) noexcept;

#pragma endregion Construction

#pragma region Interface Methods

    inline bool empty() const noexcept { return elements_count_ == 0; }
    inline std::size_t count() const noexcept { return elements_count_; }

    /** Hands every written collection's share to @p callback, without staging a list of them. */
    template <typename callback_>
    inline void for_each_group(callback_&& callback) const noexcept;

    inline write_buffer_status_t status(group_id_t id, stored_key_t stored_key) const noexcept;

    inline status_t stored_value_size(group_id_t id, stored_key_t stored_key, stored_value_size_t& size) const noexcept;

    inline status_t select(group_id_t id, stored_key_t stored_key, stored_value_span_t stored_value,
                           stored_value_size_t& size) const noexcept;

    template <typename stored_value_location_>
    inline write_buffer_status_t find(group_id_t id, stored_key_t stored_key,
                                      stored_value_location_& location) const noexcept;
    template <typename stored_value_location_>
    inline void select(stored_value_location_ const& location, stored_value_span_t stored_value,
                       notifier_t notifier) const noexcept;

    template <typename config_provider_>
    inline status_t add(group_id_t id, config_provider_ config_provider, stored_key_t stored_key,
                        stored_value_view_t stored_value) noexcept;
    template <typename config_provider_, typename serializable_stored_value_>
    inline status_t add(group_id_t id, config_provider_ config_provider, stored_key_t stored_key,
                        serializable_stored_value_ const& stored_value) noexcept;
    template <typename config_provider_>
    inline status_t mark_as_removed(group_id_t id, config_provider_ config_provider, stored_key_t stored_key) noexcept;

#pragma endregion Interface Methods

#pragma region Batch Methods

    template <typename stored_value_location_>
    inline status_t find(group_id_t id, std::span<stored_key_t const> stored_keys,
                         std::span<stored_value_location_> locations,
                         missing_locations_t missing_locations = missing_locations_t::invalidate_k) const;
    template <typename stored_value_location_>
    inline void select(std::span<stored_value_location_ const> locations, bytes_span_t buffer,
                       notifier_t notifier) const;

#pragma endregion Batch Methods

#pragma region Validators

    inline status_t check_stored_value(group_config_t const& config, stored_key_t stored_key,
                                       stored_value_size_t stored_value_size) const noexcept;

    inline bool can_fit(std::size_t stored_values_count, std::size_t stored_values_sum_size) const noexcept;

    inline void clear() noexcept;

#pragma endregion Validators

  private:
#pragma region Helper Methods

    /** Finds the share of @p id, making one from @p config_provider when the collection is new. */
    template <typename config_provider_>
    inline status_t reach_group_(group_id_t id, config_provider_ config_provider, group_t*& group) noexcept;

#pragma endregion Helper Methods

#pragma region Private Members

    /** The budget every share of the buffer is charged to. */
    resources_shared_ptr_t resources_;

    /** The bytes @c insertions_buffer holds. */
    std::size_t capacity_bytes_ = 0;

    /** The keys every share together admits. */
    std::size_t elements_max_count_ = 0;

    /** The keys every share together holds. */
    std::size_t elements_count_ = 0;

#pragma endregion Private Members
};

template <typename stored_key_>
inline grouped_write_buffer<stored_key_>::grouped_write_buffer(std::size_t capacity_bytes,
                                                               std::size_t elements_max_count,
                                                               resources_shared_ptr_t resources) noexcept
    : groups_by_id({}, {}, constrained_allocator<std::byte> {resources->memory_constraint}),
      insertions_buffer(constrained_allocator<std::byte> {resources->memory_constraint}), next_inserted_offset(0),
      resources_(resources), capacity_bytes_(capacity_bytes), elements_max_count_(elements_max_count),
      elements_count_(0) {
    // A budget that refuses the bytes leaves no capacity, and `can_fit` then refuses every write.
    if (st::failed(insertions_buffer.resize(capacity_bytes))) capacity_bytes_ = 0;
}

template <typename stored_key_>
inline grouped_write_buffer<stored_key_>::grouped_write_buffer(grouped_write_buffer&& other) noexcept
    : groups_by_id(std::move(other.groups_by_id)), insertions_buffer(std::move(other.insertions_buffer)),
      next_inserted_offset(other.next_inserted_offset), resources_(std::move(other.resources_)),
      capacity_bytes_(other.capacity_bytes_), elements_max_count_(other.elements_max_count_),
      elements_count_(other.elements_count_) {}

template <typename stored_key_>
inline grouped_write_buffer<stored_key_>& grouped_write_buffer<stored_key_>::operator=(
    grouped_write_buffer&& other) noexcept {
    std::swap(groups_by_id, other.groups_by_id);
    std::swap(insertions_buffer, other.insertions_buffer);
    std::swap(next_inserted_offset, other.next_inserted_offset);
    std::swap(resources_, other.resources_);
    std::swap(capacity_bytes_, other.capacity_bytes_);
    std::swap(elements_max_count_, other.elements_max_count_);
    std::swap(elements_count_, other.elements_count_);
    return *this;
}

template <typename stored_key_>
template <typename callback_>
inline void grouped_write_buffer<stored_key_>::for_each_group(callback_&& callback) const noexcept {
    [[maybe_unused]] st::status_t const walked = groups_by_id.for_each([&](auto const& entry) noexcept {
        callback(group_info_t {entry.key(), entry.value().stored_values.size(), entry.value().elements_size_bytes});
    });
}

template <typename stored_key_>
inline write_buffer_status_t grouped_write_buffer<stored_key_>::status(group_id_t id,
                                                                       stored_key_t stored_key) const noexcept {

    auto result = write_buffer_status_t::mismatch_k;
    auto iterator = groups_by_id.find(id);
    if (iterator == groups_by_id.end()) return result;

    iterator.value().stored_values.probe_to_find(stored_key, [&result](auto const& entry) noexcept {
        constexpr write_buffer_status_t results[2] = {
            write_buffer_status_t::found_k,
            write_buffer_status_t::removed_k,
        };
        result = results[entry.value().offset == tombstone_offset_k];
    });

    return result;
}

template <typename stored_key_>
inline status_t grouped_write_buffer<stored_key_>::stored_value_size(group_id_t id, stored_key_t stored_key,
                                                                     stored_value_size_t& size) const noexcept {

    auto status = status_t {status_code_t::not_found_k, "Object not exists"};
    auto iterator = groups_by_id.find(id);
    if (iterator == groups_by_id.end()) return status;

    iterator.value().stored_values.probe_to_find(stored_key, [&](auto const& entry) noexcept {
        if (entry.value().offset != tombstone_offset_k) size = entry.value().size;
        status = status_t {};
    });

    return status;
}

template <typename stored_key_>
inline status_t grouped_write_buffer<stored_key_>::select(group_id_t id, stored_key_t stored_key,
                                                          stored_value_span_t stored_value,
                                                          stored_value_size_t& size) const noexcept {

    status_t status = status_t {status_code_t::not_found_k, "Object not exists"};
    auto iterator = groups_by_id.find(id);
    if (iterator == groups_by_id.end()) return status;

    iterator.value().stored_values.probe_to_find(stored_key, [&](auto const& entry) noexcept {
        auto const& stored_value_place = entry.value();
        if (stored_value_place.offset == tombstone_offset_k) {
            status = status_t {status_code_t::not_found_k, "Object not exists"};
            return;
        }
        if (stored_value_place.offset == empty_stored_value_offset_k) {
            size = 0;
            status = status_t {};
            return;
        }
        if (stored_value.size() < stored_value_place.size) {
            status = status_t {status_code_t::invalid_argument_k, "Buffer is too small"};
            return;
        }

        auto inserted = as_bytes_view(insertions_buffer).subspan(stored_value_place.offset, stored_value_place.size);
        std::memcpy(stored_value.data(), inserted.data(), inserted.size());
        size = inserted.size();
        status = status_t {};
    });

    return status;
}

template <typename stored_key_>
template <typename stored_value_location_>
inline write_buffer_status_t grouped_write_buffer<stored_key_>::find(group_id_t id, stored_key_t stored_key,
                                                                     stored_value_location_& location) const noexcept {

    location.size_bytes = 0;
    location.offset = tombstone_offset_k;
    location.recipient_id = reinterpret_cast<std::uint64_t>(this);
    location.recipient_state_id = 0;

    auto result = write_buffer_status_t::mismatch_k;
    auto iterator = groups_by_id.find(id);
    if (iterator == groups_by_id.end()) return result;

    iterator.value().stored_values.probe_to_find(stored_key, [&](auto const& entry) noexcept {
        auto const& stored_value_place = entry.value();
        location.offset = stored_value_place.offset;
        location.size_bytes = stored_value_place.size;
        constexpr write_buffer_status_t results[2] = {write_buffer_status_t::found_k, write_buffer_status_t::removed_k};
        result = results[stored_value_place.offset == tombstone_offset_k];
    });

    return result;
}

template <typename stored_key_>
template <typename stored_value_location_>
inline void grouped_write_buffer<stored_key_>::select(stored_value_location_ const& location,
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
    if (location.recipient_id != reinterpret_cast<std::uint64_t>(this)) {
        notifier.fail({status_code_t::invalid_argument_k, "A stored value location of another write buffer"});
        return;
    }
    if (location.recipient_state_id != 0) {
        notifier.fail({status_code_t::invalid_argument_k, "A stored value location of another buffer state"});
        return;
    }

    if (location.size_bytes > 0) {
        auto inserted = as_bytes_view(insertions_buffer).subspan(location.offset, location.size_bytes);
        std::memcpy(stored_value.data() + location.buffer_padding(), inserted.data(), inserted.size());
    }
    notifier.did_one();
}

template <typename stored_key_>
template <typename config_provider_>
inline status_t grouped_write_buffer<stored_key_>::reach_group_(group_id_t id, config_provider_ config_provider,
                                                                group_t*& group) noexcept {

    auto iterator = groups_by_id.find(id);
    if (iterator == groups_by_id.end()) {
        group_config_t config {};
        auto status = config_provider(id, config);
        if (status.failed()) return status;
        if ((config.value_size > 0) & (config.value_max_size > 0))
            return status_t {status_code_t::invalid_argument_k, "Invalid config"};

        group_t fresh {config, {{}, {}, constrained_allocator<std::byte> {resources_->memory_constraint}}, 0};
        if (groups_by_id.reserve_more(1) == groups_t::reserve_result_t::failed_k)
            return status_t {status_code_t::out_of_memory_k, "No room for another collection"};
        groups_by_id.emplace(id, std::move(fresh), st::assume_reserved_t {});
        iterator = groups_by_id.find(id);
        if (iterator == groups_by_id.end())
            return status_t {status_code_t::out_of_memory_k, "No room for another collection"};
    }
    group = &iterator.value_ref();
    return status_t {};
}

template <typename stored_key_>
template <typename config_provider_>
inline status_t grouped_write_buffer<stored_key_>::add(group_id_t id, config_provider_ config_provider,
                                                       stored_key_t stored_key,
                                                       stored_value_view_t stored_value) noexcept {

    group_t* group = nullptr;
    auto status = reach_group_(id, config_provider, group);
    if (status.failed()) return status;

    status = check_stored_value(group->config, stored_key, stored_value.size());
    if (status.failed()) return status;

    if (!can_fit(1, stored_value.size())) [[unlikely]]
        return status_t {status_code_t::capacity_exhausted_k, "Cache is full"};

    // The room for the key is taken before a byte of the value lands, so a refusal writes nothing.
    if (group->stored_values.reserve_more(1) == group_t::stored_values_t::reserve_result_t::failed_k)
        return status_t {status_code_t::out_of_memory_k, "No room for another key"};

    auto inserted_offset = static_cast<stored_value_offset_in_buffer_t>(empty_stored_value_offset_k);
    if (!stored_value.empty()) {
        inserted_offset = next_inserted_offset;
        std::memcpy(insertions_buffer.data() + inserted_offset, stored_value.data(), stored_value.size());
        next_inserted_offset += stored_value.size();
    }

    stored_value_place_t const place {inserted_offset, static_cast<stored_value_size_t>(stored_value.size())};
    auto insert = [&](auto& unused_slot) noexcept {
        new (&unused_slot.key_ref()) stored_key_t {stored_key};
        new (&unused_slot.value_ref()) stored_value_place_t {place};
        group->elements_size_bytes += stored_value.size();
        ++elements_count_;
    };
    auto update = [&](auto& equal_slot) noexcept {
        group->elements_size_bytes -= equal_slot.value_ref().size;
        equal_slot.value_ref() = place;
        group->elements_size_bytes += stored_value.size();
    };
    [[maybe_unused]] auto const stored = group->stored_values.probe_to_upsert(stored_key, insert, update,
                                                                              st::assume_reserved_t {});

    return status_t {};
}

template <typename stored_key_>
template <typename config_provider_, typename serializable_stored_value_>
inline status_t grouped_write_buffer<stored_key_>::add(group_id_t id, config_provider_ config_provider,
                                                       stored_key_t stored_key,
                                                       serializable_stored_value_ const& stored_value) noexcept {

    group_t* group = nullptr;
    auto status = reach_group_(id, config_provider, group);
    if (status.failed()) return status;

    auto const serialized_size = static_cast<stored_value_size_t>(nvme::storage_requirements(stored_value));
    status = check_stored_value(group->config, stored_key, serialized_size);
    if (status.failed()) return status;

    if (!can_fit(1, serialized_size)) [[unlikely]]
        return status_t {status_code_t::capacity_exhausted_k, "Cache is full"};

    // The room for the key is taken before a byte of the value lands, so a refusal writes nothing.
    if (group->stored_values.reserve_more(1) == group_t::stored_values_t::reserve_result_t::failed_k)
        return status_t {status_code_t::out_of_memory_k, "No room for another key"};

    status = nvme::save(stored_value, bytes_span_t {insertions_buffer.data() + next_inserted_offset, serialized_size});
    if (status.failed()) return status;

    stored_value_place_t const place {next_inserted_offset, serialized_size};
    next_inserted_offset += serialized_size;

    auto insert = [&](auto& unused_slot) noexcept {
        new (&unused_slot.key_ref()) stored_key_t {stored_key};
        new (&unused_slot.value_ref()) stored_value_place_t {place};
        group->elements_size_bytes += serialized_size;
        ++elements_count_;
    };
    auto update = [&](auto& equal_slot) noexcept {
        group->elements_size_bytes -= equal_slot.value_ref().size;
        equal_slot.value_ref() = place;
        group->elements_size_bytes += serialized_size;
    };
    [[maybe_unused]] auto const stored = group->stored_values.probe_to_upsert(stored_key, insert, update,
                                                                              st::assume_reserved_t {});

    return status_t {};
}

template <typename stored_key_>
template <typename config_provider_>
inline status_t grouped_write_buffer<stored_key_>::mark_as_removed(group_id_t id, config_provider_ config_provider,
                                                                   stored_key_t stored_key) noexcept {

    group_t* group = nullptr;
    auto status = reach_group_(id, config_provider, group);
    if (status.failed()) return status;

    if (!can_fit(1, 0)) [[unlikely]]
        return status_t {status_code_t::capacity_exhausted_k, "Cache is full"};

    if (group->stored_values.reserve_more(1) == group_t::stored_values_t::reserve_result_t::failed_k)
        return status_t {status_code_t::out_of_memory_k, "No room for another key"};

    stored_value_place_t const place {tombstone_offset_k, 0};
    auto insert = [&](auto& unused_slot) noexcept {
        new (&unused_slot.key_ref()) stored_key_t {stored_key};
        new (&unused_slot.value_ref()) stored_value_place_t {place};
        ++elements_count_;
    };
    auto update = [&](auto& equal_slot) noexcept {
        group->elements_size_bytes -= equal_slot.value_ref().size;
        equal_slot.value_ref() = place;
    };
    [[maybe_unused]] auto const stored = group->stored_values.probe_to_upsert(stored_key, insert, update,
                                                                              st::assume_reserved_t {});

    return status_t {};
}

template <typename stored_key_>
template <typename stored_value_location_>
inline status_t grouped_write_buffer<stored_key_>::find(group_id_t id, std::span<stored_key_t const> stored_keys,
                                                        std::span<stored_value_location_> locations,
                                                        missing_locations_t missing_locations) const {

    if (locations.size() < stored_keys.size())
        return status_t {status_code_t::invalid_argument_k, "Buffer is too small"};

    if (missing_locations == missing_locations_t::invalidate_k)
        for (std::size_t index = 0; index < stored_keys.size(); ++index)
            find<stored_value_location_>(id, stored_keys[index], locations[index]);
    else
        for (std::size_t index = 0; index < stored_keys.size(); ++index) {
            stored_value_location_ location;
            auto const found = find<stored_value_location_>(id, stored_keys[index], location);
            if (found != write_buffer_status_t::mismatch_k) locations[index] = location;
        }

    return status_t {};
}

template <typename stored_key_>
template <typename stored_value_location_>
inline void grouped_write_buffer<stored_key_>::select(std::span<stored_value_location_ const> locations,
                                                      bytes_span_t buffer, notifier_t notifier) const {

    for (std::size_t index = 0, offset = 0; (index < locations.size()) & !notifier.has_failed(); ++index) {
        auto const& location = locations[index];
        auto is_recipient = location.recipient_id == reinterpret_cast<std::uint64_t>(this);
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

template <typename stored_key_>
inline status_t grouped_write_buffer<stored_key_>::check_stored_value(
    group_config_t const& config, stored_key_t stored_key, stored_value_size_t stored_value_size) const noexcept {

    bool valid_key = stored_key != limit<stored_key_t>::max();
    bool valid_fixed_width_stored_value = (config.value_size > 0) & (stored_value_size == config.value_size);
    bool valid_variable_width_stored_value = stored_value_size <= config.value_max_size;
    bool valid = valid_key & (valid_fixed_width_stored_value ^ valid_variable_width_stored_value);
    return valid ? status_t {} : status_t {status_code_t::malformed_k, "Invalid stored value"};
}

template <typename stored_key_>
inline bool grouped_write_buffer<stored_key_>::can_fit(std::size_t stored_values_count,
                                                       std::size_t stored_values_sum_size) const noexcept {

    auto used_memory = next_inserted_offset;
    bool has_element_place = elements_count_ + stored_values_count <= elements_max_count_;
    bool has_enough_space = (used_memory + stored_values_sum_size <= capacity_bytes_) &
                            (used_memory + stored_values_sum_size <= empty_stored_value_offset_k);
    return has_element_place & has_enough_space;
}

template <typename stored_key_>
inline void grouped_write_buffer<stored_key_>::clear() noexcept {
    groups_by_id.clear();
    next_inserted_offset = 0;
    elements_count_ = 0;
}

#pragma endregion Grouped Write Buffer

#pragma region Grouped Read Set

/** The keys one transaction watches, kept apart per collection so two collections never collide. */
template <typename stored_key_>
class grouped_read_set {
  public:
    using stored_key_t = stored_key_;
    using group_id_t = collection_id_t;
    using read_set_t = read_set<stored_key_t>;
    using groups_t =
        st::hash_map<group_id_t, read_set_t, collection_id_hash_t, st::equal_to_t, constrained_allocator<std::byte>>;

    /** A set charging @p resources for the keys of every collection it watches. */
    explicit grouped_read_set(resources_shared_ptr_t resources) noexcept
        : resources(resources), groups({}, {}, constrained_allocator<std::byte> {resources->memory_constraint}) {}

    /** Whether the transaction watches nothing anywhere. */
    bool empty() const noexcept { return count() == 0; }

    /** The watched keys of every collection taken for every purpose in @p purpose. */
    std::size_t count(watch_purpose_t purpose = watch_purpose_t::all_k) const noexcept {
        std::size_t matching = 0;
        [[maybe_unused]] st::status_t const walked = groups.for_each(
            [&](auto const& entry) noexcept { matching += entry.value().count(purpose); });
        return matching;
    }

    /** The watched keys of @p id taken for every purpose in @p purpose. */
    std::size_t count(group_id_t id, watch_purpose_t purpose = watch_purpose_t::all_k) const noexcept {
        auto iterator = groups.find(id);
        return iterator == groups.end() ? 0 : iterator.value().count(purpose);
    }

    /** Makes room for @p count more keys of @p id, so a following @c add takes no allocation. */
    status_t reserve_more(group_id_t id, std::size_t count) noexcept {
        auto iterator = groups.find(id);
        if (iterator == groups.end()) {
            if (groups.reserve_more(1) == groups_t::reserve_result_t::failed_k)
                return {status_code_t::out_of_memory_k, "No room for another watched collection"};
            groups.emplace(id, read_set_t {resources}, st::assume_reserved_t {});
            iterator = groups.find(id);
            if (iterator == groups.end())
                return {status_code_t::out_of_memory_k, "No room for another watched collection"};
        }
        return iterator.value_ref().reserve_more(count);
    }

    /** Watches @p stored_key of @p id from @p watch_stamp, in room @c reserve_more took. */
    void add(group_id_t id, stored_key_t stored_key, transaction_stamp_t watch_stamp, watch_purpose_t purpose,
             st::assume_reserved_t) noexcept {
        auto iterator = groups.find(id);
        if (iterator == groups.end()) return;
        iterator.value_ref().add(stored_key, watch_stamp, purpose, st::assume_reserved_t {});
    }

    /** Drops every watch of every collection, for a transaction starting over. */
    void clear() noexcept { groups.clear(); }

    /** The budget every watched collection's keys are charged to. */
    resources_shared_ptr_t resources;

    /** One watched-key set per collection the transaction touched. */
    groups_t groups;
};

#pragma endregion Grouped Read Set

#pragma region Transaction State

/** Everything one database transaction carries between its start and its commit. */
template <typename stored_key_>
struct database_transaction_state {
    using stored_key_t = stored_key_;
    using write_buffer_t = grouped_write_buffer<stored_key_t>;
    using read_set_t = grouped_read_set<stored_key_t>;

    inline database_transaction_state(write_buffer_t&& write_buffer, resources_shared_ptr_t resources) noexcept
        : changes(std::move(write_buffer)), read_set(resources) {}

    /** Whether the transaction has neither written nor watched anything. */
    inline bool empty() const noexcept { return changes.empty() & read_set.empty(); }

    /** Drops both the writes and the watches, for a rollback or a renewal. */
    inline void clear() noexcept {
        changes.clear();
        read_set.clear();
    }

    /** The writes the transaction has not committed yet. */
    write_buffer_t changes;

    /** The keys the transaction watches for conflicts. */
    read_set_t read_set;
};

#pragma endregion Transaction State

} // namespace unum::ustore::nvme
