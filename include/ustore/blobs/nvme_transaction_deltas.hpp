/**
 *  @file include/ustore/blobs/nvme_transaction_deltas.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The uncommitted deltas of one transaction, keyed by stored key inside each collection it wrote.
 */

#pragma once

#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <algorithm> // `std::min`
#include <limits>    // `std::numeric_limits`
#include <new>       // placement `new`
#include <span>      // `std::span`
#include <utility>   // `std::move`, `std::swap`

#include <smashtable/basic_hash_table.hpp> // `st::hash_map`, `st::assume_reserved`
#include <smashtable/basic_vector.hpp>     // `st::basic_vector`

#include "ustore/blobs/nvme_key_slot.hpp"   // `lookup_result_t`, `stored_value_size_t`, `transaction_stamp_t`
#include "ustore/blobs/nvme_memory_run.hpp" // `delta_view_t`
#include "ustore/utilities/folds.hpp"       // `fold_concept`, `fold_delta_pair`, `delta_kind_t`
#include "ustore/utilities/nvme_ring.hpp"   // `constrained_allocator`, `notifier`
#include "ustore/utilities/nvme_volume.hpp" // `resources_shared_ptr`
#include "ustore/utilities/ram_buffers.hpp" // `local_arena_t`, `arena_allocator`
#include "ustore/utilities/types.hpp"       // `status_t`, `status_code_t`, `byte_t`

namespace unum::ustore::blobs {

using utilities::constrained_allocator;
using utilities::notifier;
using utilities::resources_shared_ptr;

#pragma region Transaction Deltas

/**
 *  @brief The uncommitted deltas of one transaction, keyed by stored key inside each collection it wrote.
 *  @note Owned by one transaction and touched by its thread alone, so nothing here is atomic.
 */
template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
class transaction_deltas {
  public:
#pragma region Types

    using stored_key_t = stored_key_;
    using stored_value_offset_in_buffer_t = std::size_t;

    /** The value widths one collection admits, of which exactly one is ever set. */
    struct value_widths_t {

        /** The width every value of a fixed-width collection has, zero for a variable-width one. */
        std::size_t value_size;

        /** The width no value of a variable-width collection exceeds, zero for a fixed-width one. */
        std::size_t value_max_size;
    };

    /** What one collection's deltas hold, which a stage sizes a flush against. */
    struct collection_info_t {

        /** The collection these deltas belong to. */
        collection_id_t id;

        /** The keys written inside it. */
        std::size_t elements_count;

        /** The bytes their values occupy. */
        std::size_t elements_size_bytes;
    };

    /** Where one written value sits in the insertions buffer, what the delta does, and from when the transaction
     *  watches the key for another commit writing it. */
    struct stored_value_place_t {

        /** The first byte of the value, zero for a removal and for a value of no bytes. */
        stored_value_offset_t offset;

        /** The bytes of the value. */
        stored_value_size_t size;

        /** What the delta does to the key. */
        delta_kind_t kind;

        /** The stamp a commit writing the key after conflicts, or @c invalid_transaction_stamp_k when unwatched. */
        transaction_stamp_t watch_stamp;
    };

    static_assert(sizeof(stored_value_place_t) == 24, "A delta's kind takes the padding before its watch, not a word");

    /** The deltas of one collection. */
    struct collection_deltas_t {
        using stored_values_t = st::hash_map<stored_key_t, stored_value_place_t, stored_key_hash_t, st::equal_to_t,
                                             constrained_allocator<std::byte, atomic_reference_>>;

        /** The value widths the collection admits, which every write is checked against. */
        value_widths_t widths;

        /** Where each written key's value sits. */
        stored_values_t stored_values;

        /** The bytes those values occupy. */
        std::size_t elements_size_bytes;
    };

    using collections_t = st::hash_map<collection_id_t, collection_deltas_t, collection_id_hash_t, st::equal_to_t,
                                       constrained_allocator<std::byte, atomic_reference_>>;

#pragma endregion Types

#pragma region Public Members

    /** The deltas of every written collection. */
    collections_t collections_by_id;

    /** The bytes every written value of every collection sits in. */
    st::basic_vector<std::byte, constrained_allocator<std::byte, atomic_reference_>> insertions_buffer;

    /** The first free byte of @c insertions_buffer. */
    stored_value_offset_in_buffer_t next_inserted_offset = 0;

#pragma endregion Public Members

#pragma region Construction

    inline transaction_deltas(std::size_t capacity_bytes, std::size_t elements_max_count,
                              resources_shared_ptr<atomic_reference_, waiter_type_> resources) noexcept;

    inline transaction_deltas(transaction_deltas const& other) = delete;
    inline transaction_deltas(transaction_deltas&& other) noexcept;
    inline transaction_deltas& operator=(transaction_deltas const& other) = delete;
    inline transaction_deltas& operator=(transaction_deltas&& other) noexcept;

#pragma endregion Construction

#pragma region Interface Methods

    inline bool empty() const noexcept { return elements_count_ == 0; }
    inline std::size_t count() const noexcept { return elements_count_; }

    /** The keys written inside @p id. */
    inline std::size_t count(collection_id_t id) const noexcept;

    /** Hands every written collection's summary to @p callback, without staging a list of them. */
    template <typename callback_>
    inline void for_each_collection(callback_&& callback) const noexcept;

    /** What the deltas did to @p stored_key of @p id: wrote it, removed it, or neither. */
    inline lookup_result_t lookup(collection_id_t id, stored_key_t stored_key) const noexcept;

    inline status_t stored_value_size(collection_id_t id, stored_key_t stored_key,
                                      stored_value_size_t& size) const noexcept;

    inline status_t select(collection_id_t id, stored_key_t stored_key, stored_value_span_t stored_value,
                           stored_value_size_t& size) const noexcept;

    template <typename stored_value_location_>
    inline lookup_result_t find(collection_id_t id, stored_key_t stored_key,
                                stored_value_location_& location) const noexcept;
    template <typename stored_value_location_>
    inline void select(stored_value_location_ const& location, stored_value_span_t stored_value,
                       notifier<atomic_reference_, waiter_type_> notifier) const noexcept;

    /** Writes @p stored_value under @p stored_key of @p id, whose values are @p widths wide, watched from
     *  @p watch_stamp, which only lowers the stamp an earlier delta of the key took. */
    inline status_t write(collection_id_t id, value_widths_t widths, stored_key_t stored_key,
                          stored_value_view_t stored_value,
                          transaction_stamp_t watch_stamp = invalid_transaction_stamp_k) noexcept;

    /** Writes a removal of @p stored_key of @p id, watched from @p watch_stamp as @c write is. */
    inline status_t remove(collection_id_t id, value_widths_t widths, stored_key_t stored_key,
                           transaction_stamp_t watch_stamp = invalid_transaction_stamp_k) noexcept;

    /** Writes @p change, a change delta of @p fold, under @p stored_key of @p id, folded after whatever the deltas
     *  hold for the key, keeping that delta's watch and taking none of its own. */
    template <utilities::fold_concept fold_type_>
    inline status_t change(collection_id_t id, value_widths_t widths, stored_key_t stored_key,
                           stored_value_view_t change, fold_type_ const& fold) noexcept;

    /** Fills @p held with what the deltas hold for @p stored_key of @p id, its bytes viewing the insertions buffer,
     *  answering whether they hold anything. */
    inline bool delta(collection_id_t id, stored_key_t stored_key, delta_view_t& held) const noexcept;

    /** Moves every watched delta's watch to @p watch_stamp, for a transaction started over with its deltas kept. */
    inline void rescope_watches(transaction_stamp_t watch_stamp) noexcept;

    /** Hands @p callback every delta as @c callback(id, stored_key, place), in no order. */
    template <typename callback_>
    inline void for_each_write(callback_&& callback) const noexcept;

    /** Hands @p callback every delta of @p id as @c callback(stored_key, delta), its bytes viewing the insertions
     *  buffer, in no order. */
    template <typename callback_>
    inline void for_each_delta(collection_id_t id, callback_&& callback) const noexcept;

#pragma endregion Interface Methods

#pragma region Batch Methods

    template <typename stored_value_location_>
    inline status_t find(collection_id_t id, std::span<stored_key_t const> stored_keys,
                         std::span<stored_value_location_> locations,
                         missing_locations_t missing_locations = missing_locations_t::invalidate_k) const;
    template <typename stored_value_location_>
    inline void select(std::span<stored_value_location_ const> locations, bytes_span_t buffer,
                       notifier<atomic_reference_, waiter_type_> notifier) const;

#pragma endregion Batch Methods

#pragma region Validators

    inline status_t check_stored_value(value_widths_t const& widths, stored_key_t stored_key,
                                       stored_value_size_t stored_value_size) const noexcept;

    inline bool can_fit(std::size_t stored_values_count, std::size_t stored_values_sum_size) const noexcept;

    inline void clear() noexcept;

#pragma endregion Validators

  private:
#pragma region Helper Methods

    /** Finds the deltas of @p id, starting them at @p widths when the collection is new. */
    inline status_t reach_collection_(collection_id_t id, value_widths_t widths,
                                      collection_deltas_t*& collection) noexcept;

#pragma endregion Helper Methods

#pragma region Private Members

    /** The budget the deltas of every collection are charged to. */
    resources_shared_ptr<atomic_reference_, waiter_type_> resources_;

    /** The bytes @c insertions_buffer holds. */
    std::size_t capacity_bytes_ = 0;

    /** The keys every collection together admits. */
    std::size_t elements_max_count_ = 0;

    /** The keys every collection together holds. */
    std::size_t elements_count_ = 0;

#pragma endregion Private Members
};

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline transaction_deltas<stored_key_, atomic_reference_, waiter_type_>::transaction_deltas(
    std::size_t capacity_bytes, std::size_t elements_max_count,
    resources_shared_ptr<atomic_reference_, waiter_type_> resources) noexcept
    : collections_by_id({}, {}, constrained_allocator<std::byte, atomic_reference_> {resources->memory_constraint}),
      insertions_buffer(constrained_allocator<std::byte, atomic_reference_> {resources->memory_constraint}),
      next_inserted_offset(0), resources_(resources), capacity_bytes_(capacity_bytes),
      elements_max_count_(elements_max_count), elements_count_(0) {
    // A budget that refuses the bytes leaves no capacity, and `can_fit` then refuses every write.
    if (st::failed(insertions_buffer.resize(capacity_bytes))) capacity_bytes_ = 0;
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline transaction_deltas<stored_key_, atomic_reference_, waiter_type_>::transaction_deltas(
    transaction_deltas&& other) noexcept
    : collections_by_id(std::move(other.collections_by_id)), insertions_buffer(std::move(other.insertions_buffer)),
      next_inserted_offset(other.next_inserted_offset), resources_(std::move(other.resources_)),
      capacity_bytes_(other.capacity_bytes_), elements_max_count_(other.elements_max_count_),
      elements_count_(other.elements_count_) {}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline transaction_deltas<stored_key_, atomic_reference_, waiter_type_>&
transaction_deltas<stored_key_, atomic_reference_, waiter_type_>::operator=(transaction_deltas&& other) noexcept {
    std::swap(collections_by_id, other.collections_by_id);
    std::swap(insertions_buffer, other.insertions_buffer);
    std::swap(next_inserted_offset, other.next_inserted_offset);
    std::swap(resources_, other.resources_);
    std::swap(capacity_bytes_, other.capacity_bytes_);
    std::swap(elements_max_count_, other.elements_max_count_);
    std::swap(elements_count_, other.elements_count_);
    return *this;
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline std::size_t transaction_deltas<stored_key_, atomic_reference_, waiter_type_>::count(
    collection_id_t id) const noexcept {
    auto iterator = collections_by_id.find(id);
    return iterator == collections_by_id.end() ? 0 : iterator.value().stored_values.size();
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
template <typename callback_>
inline void transaction_deltas<stored_key_, atomic_reference_, waiter_type_>::for_each_collection(
    callback_&& callback) const noexcept {
    [[maybe_unused]] st::status_t const walked = collections_by_id.for_each([&](auto const& entry) noexcept {
        callback(
            collection_info_t {entry.key(), entry.value().stored_values.size(), entry.value().elements_size_bytes});
    });
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline lookup_result_t transaction_deltas<stored_key_, atomic_reference_, waiter_type_>::lookup(
    collection_id_t id, stored_key_t stored_key) const noexcept {

    auto result = lookup_result_t::missing_k;
    auto iterator = collections_by_id.find(id);
    if (iterator == collections_by_id.end()) return result;

    iterator.value().stored_values.probe_to_find(stored_key, [&result](auto const& entry) noexcept {
        constexpr lookup_result_t results[2] = {
            lookup_result_t::written_k,
            lookup_result_t::removed_k,
        };
        result = results[entry.value().kind == delta_kind_t::remove_k];
    });

    return result;
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline status_t transaction_deltas<stored_key_, atomic_reference_, waiter_type_>::stored_value_size(
    collection_id_t id, stored_key_t stored_key, stored_value_size_t& size) const noexcept {

    auto status = status_t {status_code_t::not_found_k, "Object not exists"};
    auto iterator = collections_by_id.find(id);
    if (iterator == collections_by_id.end()) return status;

    iterator.value().stored_values.probe_to_find(stored_key, [&](auto const& entry) noexcept {
        if (entry.value().kind != delta_kind_t::remove_k) size = entry.value().size;
        status = status_t {};
    });

    return status;
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline status_t transaction_deltas<stored_key_, atomic_reference_, waiter_type_>::select(
    collection_id_t id, stored_key_t stored_key, stored_value_span_t stored_value,
    stored_value_size_t& size) const noexcept {

    status_t status = status_t {status_code_t::not_found_k, "Object not exists"};
    auto iterator = collections_by_id.find(id);
    if (iterator == collections_by_id.end()) return status;

    iterator.value().stored_values.probe_to_find(stored_key, [&](auto const& entry) noexcept {
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

        auto inserted = as_bytes_view(insertions_buffer).subspan(stored_value_place.offset, stored_value_place.size);
        std::memcpy(stored_value.data(), inserted.data(), inserted.size());
        size = inserted.size();
        status = status_t {};
    });

    return status;
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
template <typename stored_value_location_>
inline lookup_result_t transaction_deltas<stored_key_, atomic_reference_, waiter_type_>::find(
    collection_id_t id, stored_key_t stored_key, stored_value_location_& location) const noexcept {

    location.size_bytes = 0;
    location.offset = 0;
    location.kind = delta_kind_t::remove_k;
    location.recipient_id = reinterpret_cast<std::uint64_t>(this);
    location.recipient_state_id = 0;

    auto result = lookup_result_t::missing_k;
    auto iterator = collections_by_id.find(id);
    if (iterator == collections_by_id.end()) return result;

    iterator.value().stored_values.probe_to_find(stored_key, [&](auto const& entry) noexcept {
        auto const& stored_value_place = entry.value();
        location.offset = stored_value_place.offset;
        location.size_bytes = stored_value_place.size;
        location.kind = stored_value_place.kind;
        constexpr lookup_result_t results[2] = {lookup_result_t::written_k, lookup_result_t::removed_k};
        result = results[location.kind == delta_kind_t::remove_k];
    });

    return result;
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
template <typename stored_value_location_>
inline void transaction_deltas<stored_key_, atomic_reference_, waiter_type_>::select(
    stored_value_location_ const& location, stored_value_span_t stored_value,
    notifier<atomic_reference_, waiter_type_> notifier) const noexcept {

    if (!location) {
        notifier.fail({status_code_t::invalid_argument_k, "A stored value location naming nothing"});
        return;
    }
    if (stored_value.size() < location.aligned_buffer_size()) {
        notifier.fail({status_code_t::invalid_argument_k, "A buffer shorter than the stored value"});
        return;
    }
    if (location.recipient_id != reinterpret_cast<std::uint64_t>(this)) {
        notifier.fail({status_code_t::invalid_argument_k, "A stored value location of other deltas"});
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

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline status_t transaction_deltas<stored_key_, atomic_reference_, waiter_type_>::reach_collection_(
    collection_id_t id, value_widths_t widths, collection_deltas_t*& collection) noexcept {

    auto iterator = collections_by_id.find(id);
    if (iterator == collections_by_id.end()) {
        if ((widths.value_size > 0) & (widths.value_max_size > 0))
            return status_t {status_code_t::invalid_argument_k, "Invalid config"};

        collection_deltas_t fresh {
            widths,
            {{}, {}, constrained_allocator<std::byte, atomic_reference_> {resources_->memory_constraint}},
            0};
        if (collections_by_id.reserve_more(1) == collections_t::reserve_result_t::failed_k)
            return status_t {status_code_t::out_of_memory_k, "No room for another collection"};
        collections_by_id.emplace(id, std::move(fresh), st::assume_reserved);
        iterator = collections_by_id.find(id);
        if (iterator == collections_by_id.end())
            return status_t {status_code_t::out_of_memory_k, "No room for another collection"};
    }
    collection = &iterator.value_ref();
    return status_t {};
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline status_t transaction_deltas<stored_key_, atomic_reference_, waiter_type_>::write(
    collection_id_t id, value_widths_t widths, stored_key_t stored_key, stored_value_view_t stored_value,
    transaction_stamp_t watch_stamp) noexcept {

    collection_deltas_t* collection = nullptr;
    auto status = reach_collection_(id, widths, collection);
    if (status.failed()) return status;

    status = check_stored_value(collection->widths, stored_key, stored_value.size());
    if (status.failed()) return status;

    if (!can_fit(1, stored_value.size())) [[unlikely]]
        return status_t {status_code_t::capacity_exhausted_k, "Cache is full"};

    // The room for the key is taken before a byte of the value lands, so a refusal writes nothing.
    if (collection->stored_values.reserve_more(1) == collection_deltas_t::stored_values_t::reserve_result_t::failed_k)
        return status_t {status_code_t::out_of_memory_k, "No room for another key"};

    stored_value_offset_t inserted_offset = 0;
    if (!stored_value.empty()) {
        inserted_offset = static_cast<stored_value_offset_t>(next_inserted_offset);
        std::memcpy(insertions_buffer.data() + inserted_offset, stored_value.data(), stored_value.size());
        next_inserted_offset += stored_value.size();
    }

    stored_value_place_t const place {inserted_offset, static_cast<stored_value_size_t>(stored_value.size()),
                                      delta_kind_t::write_k, watch_stamp};
    auto insert = [&](auto& unused_slot) noexcept {
        new (&unused_slot.key_ref()) stored_key_t {stored_key};
        new (&unused_slot.value_ref()) stored_value_place_t {place};
        collection->elements_size_bytes += stored_value.size();
        ++elements_count_;
    };
    auto update = [&](auto& equal_slot) noexcept {
        stored_value_place_t& held = equal_slot.value_ref();
        collection->elements_size_bytes -= held.size;
        transaction_stamp_t const earlier = std::min(held.watch_stamp, watch_stamp);
        held = place;
        held.watch_stamp = earlier;
        collection->elements_size_bytes += stored_value.size();
    };
    [[maybe_unused]] auto const stored = collection->stored_values.probe_to_upsert(stored_key, insert, update,
                                                                                   st::assume_reserved);

    return status_t {};
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline status_t transaction_deltas<stored_key_, atomic_reference_, waiter_type_>::remove(
    collection_id_t id, value_widths_t widths, stored_key_t stored_key, transaction_stamp_t watch_stamp) noexcept {

    collection_deltas_t* collection = nullptr;
    auto status = reach_collection_(id, widths, collection);
    if (status.failed()) return status;

    if (!can_fit(1, 0)) [[unlikely]]
        return status_t {status_code_t::capacity_exhausted_k, "Cache is full"};

    if (collection->stored_values.reserve_more(1) == collection_deltas_t::stored_values_t::reserve_result_t::failed_k)
        return status_t {status_code_t::out_of_memory_k, "No room for another key"};

    stored_value_place_t const place {0, 0, delta_kind_t::remove_k, watch_stamp};
    auto insert = [&](auto& unused_slot) noexcept {
        new (&unused_slot.key_ref()) stored_key_t {stored_key};
        new (&unused_slot.value_ref()) stored_value_place_t {place};
        ++elements_count_;
    };
    auto update = [&](auto& equal_slot) noexcept {
        stored_value_place_t& held = equal_slot.value_ref();
        collection->elements_size_bytes -= held.size;
        transaction_stamp_t const earlier = std::min(held.watch_stamp, watch_stamp);
        held = place;
        held.watch_stamp = earlier;
    };
    [[maybe_unused]] auto const stored = collection->stored_values.probe_to_upsert(stored_key, insert, update,
                                                                                   st::assume_reserved);

    return status_t {};
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
template <utilities::fold_concept fold_type_>
inline status_t transaction_deltas<stored_key_, atomic_reference_, waiter_type_>::change(
    collection_id_t id, value_widths_t widths, stored_key_t stored_key, stored_value_view_t change,
    fold_type_ const& fold) noexcept {

    collection_deltas_t* collection = nullptr;
    auto status = reach_collection_(id, widths, collection);
    if (status.failed()) return status;
    if (stored_key == limit<stored_key_t>::max()) return status_t {status_code_t::malformed_k, "Invalid stored key"};
    if (collection->stored_values.reserve_more(1) == collection_deltas_t::stored_values_t::reserve_result_t::failed_k)
        return status_t {status_code_t::out_of_memory_k, "No room for another key"};

    // A change composes with a held change and applies to a held write or removal, so one delta per key reaches the
    // seal, and a lone one is composed too, so every held chain reads in the fold's own order
    utilities::local_arena_t scratch;
    utilities::arena_allocator<byte_t> arena = scratch.allocator();
    delta_view_t const newer {delta_kind_t::change_k, change};
    delta_view_t older;
    bool const held = delta(id, stored_key, older);
    delta_view_t folded {delta_kind_t::change_k, {}};
    if (held) {
        auto [paired, pair_status] = utilities::fold_delta_pair(fold, newer, older, arena);
        if (pair_status.failed()) return pair_status;
        folded = paired;
    }
    else {
        auto [composed, compose_status] = fold.compose(std::span<delta_view_t const> {&newer, 1}, arena);
        if (compose_status.failed()) return compose_status;
        folded.bytes = composed;
    }

    if (!can_fit(held ? 0 : 1, folded.bytes.size())) [[unlikely]]
        return status_t {status_code_t::capacity_exhausted_k, "Cache is full"};
    stored_value_offset_t inserted_offset = 0;
    if (!folded.bytes.empty()) {
        inserted_offset = static_cast<stored_value_offset_t>(next_inserted_offset);
        std::memcpy(insertions_buffer.data() + inserted_offset, folded.bytes.data(), folded.bytes.size());
        next_inserted_offset += folded.bytes.size();
    }

    stored_value_place_t const place {inserted_offset, static_cast<stored_value_size_t>(folded.bytes.size()),
                                      folded.kind, invalid_transaction_stamp_k};
    auto insert = [&](auto& unused_slot) noexcept {
        new (&unused_slot.key_ref()) stored_key_t {stored_key};
        new (&unused_slot.value_ref()) stored_value_place_t {place};
        collection->elements_size_bytes += place.size;
        ++elements_count_;
    };
    auto update = [&](auto& equal_slot) noexcept {
        stored_value_place_t& kept = equal_slot.value_ref();
        collection->elements_size_bytes -= kept.size;
        transaction_stamp_t const watched = kept.watch_stamp;
        kept = place;
        kept.watch_stamp = watched;
        collection->elements_size_bytes += place.size;
    };
    [[maybe_unused]] auto const stored = collection->stored_values.probe_to_upsert(stored_key, insert, update,
                                                                                   st::assume_reserved);
    return status_t {};
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline bool transaction_deltas<stored_key_, atomic_reference_, waiter_type_>::delta(collection_id_t id,
                                                                                    stored_key_t stored_key,
                                                                                    delta_view_t& held) const noexcept {
    auto iterator = collections_by_id.find(id);
    if (iterator == collections_by_id.end()) return false;
    bool found = false;
    iterator.value().stored_values.probe_to_find(stored_key, [&](auto const& entry) noexcept {
        stored_value_place_t const& place = entry.value();
        held = delta_view_t {place.kind, {}};
        if (place.size != 0) held.bytes = as_bytes_view(insertions_buffer).subspan(place.offset, place.size);
        found = true;
    });
    return found;
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline void transaction_deltas<stored_key_, atomic_reference_, waiter_type_>::rescope_watches(
    transaction_stamp_t watch_stamp) noexcept {
    [[maybe_unused]] st::status_t const walked = collections_by_id.for_each([&](auto& collection) noexcept {
        [[maybe_unused]] st::status_t const rescoped = collection.value_ref().stored_values.for_each(
            [&](auto& entry) noexcept {
                stored_value_place_t& place = entry.value_ref();
                if (place.watch_stamp != invalid_transaction_stamp_k) place.watch_stamp = watch_stamp;
            });
    });
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
template <typename callback_>
inline void transaction_deltas<stored_key_, atomic_reference_, waiter_type_>::for_each_write(
    callback_&& callback) const noexcept {
    [[maybe_unused]] st::status_t const walked = collections_by_id.for_each([&](auto const& collection) noexcept {
        [[maybe_unused]] st::status_t const listed = collection.value().stored_values.for_each(
            [&](auto const& entry) noexcept { callback(collection.key(), entry.key(), entry.value()); });
    });
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
template <typename callback_>
inline void transaction_deltas<stored_key_, atomic_reference_, waiter_type_>::for_each_delta(
    collection_id_t id, callback_&& callback) const noexcept {
    auto iterator = collections_by_id.find(id);
    if (iterator == collections_by_id.end()) return;
    [[maybe_unused]] st::status_t const walked = iterator.value().stored_values.for_each(
        [&](auto const& entry) noexcept {
            stored_value_place_t const& place = entry.value();
            delta_view_t delta {place.kind, {}};
            if (place.size != 0) delta.bytes = as_bytes_view(insertions_buffer).subspan(place.offset, place.size);
            callback(entry.key(), delta);
        });
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
template <typename stored_value_location_>
inline status_t transaction_deltas<stored_key_, atomic_reference_, waiter_type_>::find(
    collection_id_t id, std::span<stored_key_t const> stored_keys, std::span<stored_value_location_> locations,
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
            if (found != lookup_result_t::missing_k) locations[index] = location;
        }

    return status_t {};
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
template <typename stored_value_location_>
inline void transaction_deltas<stored_key_, atomic_reference_, waiter_type_>::select(
    std::span<stored_value_location_ const> locations, bytes_span_t buffer,
    notifier<atomic_reference_, waiter_type_> notifier) const {

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

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline status_t transaction_deltas<stored_key_, atomic_reference_, waiter_type_>::check_stored_value(
    value_widths_t const& widths, stored_key_t stored_key, stored_value_size_t stored_value_size) const noexcept {

    bool valid_key = stored_key != limit<stored_key_t>::max();
    bool valid_fixed_width_stored_value = (widths.value_size > 0) & (stored_value_size == widths.value_size);
    bool valid_variable_width_stored_value = stored_value_size <= widths.value_max_size;
    bool valid = valid_key & (valid_fixed_width_stored_value ^ valid_variable_width_stored_value);
    return valid ? status_t {} : status_t {status_code_t::malformed_k, "Invalid stored value"};
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline bool transaction_deltas<stored_key_, atomic_reference_, waiter_type_>::can_fit(
    std::size_t stored_values_count, std::size_t stored_values_sum_size) const noexcept {

    auto used_memory = next_inserted_offset;
    bool has_element_place = elements_count_ + stored_values_count <= elements_max_count_;
    // A place names its offset in 32 bits, which every value of the buffer stays within.
    bool has_enough_space = (used_memory + stored_values_sum_size <= capacity_bytes_) &
                            (used_memory + stored_values_sum_size <= std::numeric_limits<stored_value_offset_t>::max());
    return has_element_place & has_enough_space;
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
inline void transaction_deltas<stored_key_, atomic_reference_, waiter_type_>::clear() noexcept {
    collections_by_id.clear();
    next_inserted_offset = 0;
    elements_count_ = 0;
}

#pragma endregion Transaction Deltas

} // namespace unum::ustore::blobs
