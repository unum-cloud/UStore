/**
 *  @file include/ustore/blobs/nvme_transactions.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The transaction machinery every NVMe database composes: the deltas and read sets one
 *      transaction carries, the registry that orders and conflict-checks the transactions of one
 *      store, and the core of one transaction.
 *
 *  Transactions are optimistic. Writes go to deltas of the transaction's own and reach the store
 *  only at the commit, so a rollback costs nothing and a failed stage leaves the changes where a
 *  caller can still log them. A transaction pins the stamp the ledger last published, and every
 *  watch is scoped from that stamp or later, never from one a commit drew before landing it. There
 *  is no table of conflicts: every write lands in a collection under its commit's stamp, so a
 *  stage asks the store whether a commit newer than the watch wrote the key, and the store answers
 *  from its memory runs and from the runs that carry the key. A written key's watch lives beside
 *  its write, the read set holds the keys read or tracked, and the one exclusion is the ledger's
 *  leader word, which a staged transaction holds from @c stage until @c commit. Every collection a
 *  commit writes is sealed into a memory run before its stamp is drawn, so a commit lands whole or
 *  not at all.
 */

#pragma once

#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <algorithm> // `std::min`
#include <concepts>  // `std::same_as`
#include <limits>    // `std::numeric_limits`
#include <memory>    // `std::unique_ptr`
#include <new>       // `std::nothrow`, placement `new`
#include <span>      // `std::span`
#include <utility>   // `std::move`, `std::swap`

#include <smashtable/basic_hash_table.hpp> // `st::basic_hash_table`, `st::mapping`, `st::assume_reserved`
#include <smashtable/basic_vector.hpp>     // `st::vector`
#include <smashtable/shared.hpp>           // `st::walk_control_t`

#include "ustore/blobs/nvme_compaction.hpp"     // `compactor_ref_t`, `range_tombstone`
#include "ustore/blobs/nvme_key_slot.hpp"       // `lookup_result_t`, `stored_value_size_t`, `transaction_stamp_t`
#include "ustore/blobs/nvme_memory_run.hpp"     // `delta_view_t`
#include "ustore/blobs/nvme_published_runs.hpp" // `commit_ledger`, `stamp_pin`, `commit_request_t`, `memory_shape_t`
#include "ustore/blobs/nvme_store.hpp"          // `sealed_commit`
#include "ustore/blobs/nvme_tree.hpp"         // `transaction_config_t`, `lsm_tree_run_record_t`, `compaction_outcome_t`
#include "ustore/utilities/folds.hpp"         // `fold_concept`, `fold_delta_pair`, `delta_kind_t`
#include "ustore/utilities/nvme_channels.hpp" // `io_channel_t`
#include "ustore/utilities/nvme_mounts.hpp"   // `resources_shared_ptr`
#include "ustore/utilities/nvme_ring.hpp"     // `notifier`
#include "ustore/utilities/ram_buffers.hpp"   // `local_arena_t`, `arena_allocator`, `constrained_allocator`
#include "ustore/utilities/types.hpp"         // `status_t`, `status_code_t`, `byte_t`, `bytes_span_t`, `as_bytes_view`

namespace unum::ustore::blobs {

using utilities::constrained_allocator;
using utilities::notifier;
using utilities::resources_shared_ptr;

#pragma region Transaction Deltas

/**
 *  @brief The uncommitted deltas of one transaction, keyed by stored key inside each
 *      collection it wrote.
 *  @note Owned by one transaction and touched by its thread alone, so nothing here is atomic.
 */
template <typename stored_key_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_ = st::native_row_kit_t>
class transaction_deltas {
  public:
#pragma region Types

    using stored_key_t = stored_key_type_;
    using stored_value_offset_in_buffer_t = std::size_t;

    /** The value widths one collection admits, of which exactly one is ever set. */
    struct value_widths_t {

        /** The width every value of a fixed-width collection has, zero for a variable-width one. */
        std::size_t value_size;

        /** The width no value of a variable-width collection exceeds, zero for
         *  a fixed-width one. */
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

    /** Where one written value sits in the insertions buffer, what the delta does, and from when
     *  the transaction watches the key for another commit writing it. */
    struct stored_value_place_t {

        /** The first byte of the value, zero for a removal and for a value of no bytes. */
        stored_value_offset_t offset;

        /** The bytes of the value. */
        stored_value_size_t size;

        /** What the delta does to the key. */
        delta_kind_t kind;

        /** The stamp a commit writing the key after conflicts,
         *  or @c invalid_transaction_stamp_k when unwatched. */
        transaction_stamp_t watch_stamp;
    };

    static_assert(sizeof(stored_value_place_t) == 24, "A delta's kind takes the padding before its watch, not a word");

    /** The keys one range removal hides. */
    using range_t = range_tombstone<stored_key_t>;

    /** The ranges one collection's deltas remove, in the order the transaction removed them. */
    using ranges_t = st::vector<range_t, constrained_allocator<range_t, atomic_reference_>>;

    /** The deltas of one collection. */
    struct collection_deltas_t {
        using stored_values_t =
            st::basic_hash_table<st::mapping<stored_key_t, stored_value_place_t>, stored_key_hash_t, st::equal_to_t,
                                 constrained_allocator<std::byte, atomic_reference_>, row_kit_type_>;

        /** The value widths the collection admits, which every write is checked against. */
        value_widths_t widths;

        /** Where each written key's value sits. */
        stored_values_t stored_values;

        /** The bytes those values occupy. */
        std::size_t elements_size_bytes;

        /** The ranges removed, each landing as one range tombstone with the commit. */
        ranges_t ranges;
    };

    using collections_t =
        st::basic_hash_table<st::mapping<collection_id_t, collection_deltas_t>, collection_id_hash_t, st::equal_to_t,
                             constrained_allocator<std::byte, atomic_reference_>, row_kit_type_>;

#pragma endregion Types

#pragma region Public Members

    /** The deltas of every written collection. */
    collections_t collections_by_id;

    /** The bytes every written value of every collection sits in, sized by the first write that
     *  lands bytes, as a landed commit of one collection takes it over. */
    st::vector<std::byte, constrained_allocator<std::byte, atomic_reference_>> insertions_buffer;

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

    /** The bytes a landed commit takes over with the buffer: every written byte when one collection
     *  holds them and they fill half the buffer or more, so a run holds at most twice the bytes its
     *  memory bounds count, or none, which the seal copies. */
    [[nodiscard]] std::span<std::byte const> lendable_bytes() const noexcept {
        bool const lends = collections_by_id.size() == 1 && 2 * next_inserted_offset >= insertions_buffer.size();
        return lends ? std::span<std::byte const> {insertions_buffer}.first(next_inserted_offset)
                     : std::span<std::byte const> {};
    }
    inline std::size_t count() const noexcept { return elements_count_; }

    /** The keys written inside @p id. */
    inline std::size_t count(collection_id_t id) const noexcept;

    /** Hands every written collection's summary to @p callback, without staging a list of them. */
    template <typename callback_type_>
    inline void for_each_collection(callback_type_&& callback) const noexcept;

    /** What the deltas did to @p stored_key of @p id: wrote, removed it or its range, or none. */
    inline lookup_result_t lookup(collection_id_t id, stored_key_t stored_key) const noexcept;

    inline status_t stored_value_size(collection_id_t id, stored_key_t stored_key,
                                      stored_value_size_t& size) const noexcept;

    inline status_t select(collection_id_t id, stored_key_t stored_key, stored_value_span_t stored_value,
                           stored_value_size_t& size) const noexcept;

    template <typename stored_value_location_type_>
    inline lookup_result_t find(collection_id_t id, stored_key_t stored_key,
                                stored_value_location_type_& location) const noexcept;
    template <typename stored_value_location_type_>
    inline void select(stored_value_location_type_ const& location, stored_value_span_t stored_value,
                       notifier<atomic_reference_, waiter_type_> notifier) const noexcept;

    /** Writes @p stored_value under @p stored_key of @p id, whose values are @p widths
     *  wide, watched from @p watch_stamp, which only lowers the stamp an earlier delta of
     *  the key took. */
    inline status_t write(collection_id_t id, value_widths_t widths, stored_key_t stored_key,
                          stored_value_view_t stored_value,
                          transaction_stamp_t watch_stamp = invalid_transaction_stamp_k) noexcept;

    /** Writes a removal of @p stored_key of @p id, watched from @p watch_stamp as @c write is. */
    inline status_t remove(collection_id_t id, value_widths_t widths, stored_key_t stored_key,
                           transaction_stamp_t watch_stamp = invalid_transaction_stamp_k) noexcept;

    /** Removes every key of @p id from @p start up to @p end, exclusive, as one range, dropping
     *  every delta the deltas hold inside it, as the range lands with them and hides only older
     *  versions; a later write survives. */
    inline status_t remove_range(collection_id_t id, value_widths_t widths, stored_key_t start,
                                 stored_key_t end) noexcept;

    /** The ranges the deltas remove from @p id, in the order they were removed. */
    inline std::span<range_t const> ranges(collection_id_t id) const noexcept;

    /** Writes @p change, a change delta of @p fold, under @p stored_key of @p id, folded
     *  after whatever the deltas hold for the key, keeping that delta's watch and taking none
     *  of its own. */
    template <utilities::fold_concept fold_type_>
    inline status_t change(collection_id_t id, value_widths_t widths, stored_key_t stored_key,
                           stored_value_view_t change, fold_type_ const& fold) noexcept;

    /** Fills @p held with what the deltas hold for @p stored_key of @p id, its bytes viewing the
     *  insertions buffer, answering whether they hold anything. */
    inline bool delta(collection_id_t id, stored_key_t stored_key, delta_view_t& held) const noexcept;

    /** Moves every watched delta's watch to @p watch_stamp, for a transaction started over with
     *  its deltas kept. */
    inline void rescope_watches(transaction_stamp_t watch_stamp) noexcept;

    /** Hands @p callback every delta as <tt>callback(id, stored_key, place)</tt>, in no order. */
    template <typename callback_type_>
    inline void for_each_write(callback_type_&& callback) const noexcept;

    /** Hands @p callback every delta of @p id as <tt>callback(stored_key, delta)</tt>, its bytes viewing
     *  the insertions buffer, in no order. */
    template <typename callback_type_>
    inline void for_each_delta(collection_id_t id, callback_type_&& callback) const noexcept;

#pragma endregion Interface Methods

#pragma region Batch Methods

    template <typename stored_value_location_type_>
    inline status_t find(collection_id_t id, std::span<stored_key_t const> stored_keys,
                         std::span<stored_value_location_type_> locations,
                         missing_locations_t missing_locations = missing_locations_t::invalidate_k) const;
    template <typename stored_value_location_type_>
    inline void select(std::span<stored_value_location_type_ const> locations, bytes_span_t buffer,
                       notifier<atomic_reference_, waiter_type_> notifier) const;

#pragma endregion Batch Methods

#pragma region Validators

    inline status_t check_stored_value(value_widths_t const& widths, stored_key_t stored_key,
                                       stored_value_size_t stored_value_size) const noexcept;

    inline bool can_fit(std::size_t stored_values_count, std::size_t stored_values_sum_size) const noexcept;

    inline void clear() noexcept;

#pragma endregion Validators

  private:
#pragma region Collection Reach

    /** Finds the deltas of @p id, starting them at @p widths when the collection is new. */
    inline status_t reach_collection_(collection_id_t id, value_widths_t widths,
                                      collection_deltas_t*& collection) noexcept;

#pragma endregion Collection Reach

#pragma region Private Members

    /** The budget the deltas of every collection are charged to. */
    resources_shared_ptr<atomic_reference_, waiter_type_> resources_;

    /** The bytes @c insertions_buffer holds once sized. */
    std::size_t capacity_bytes_ = 0;

    /** The keys every collection together admits. */
    std::size_t elements_max_count_ = 0;

    /** The keys every collection together holds. */
    std::size_t elements_count_ = 0;

#pragma endregion Private Members
};

template <typename stored_key_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_>
inline transaction_deltas<stored_key_type_, atomic_reference_, waiter_type_, row_kit_type_>::transaction_deltas(
    std::size_t capacity_bytes, std::size_t elements_max_count,
    resources_shared_ptr<atomic_reference_, waiter_type_> resources) noexcept
    : collections_by_id({}, {}, constrained_allocator<std::byte, atomic_reference_> {resources->memory_constraint}),
      insertions_buffer(constrained_allocator<std::byte, atomic_reference_> {resources->memory_constraint}),
      next_inserted_offset(0), resources_(resources), capacity_bytes_(capacity_bytes),
      elements_max_count_(elements_max_count), elements_count_(0) {}

template <typename stored_key_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_>
inline transaction_deltas<stored_key_type_, atomic_reference_, waiter_type_, row_kit_type_>::transaction_deltas(
    transaction_deltas&& other) noexcept
    : collections_by_id(std::move(other.collections_by_id)), insertions_buffer(std::move(other.insertions_buffer)),
      next_inserted_offset(other.next_inserted_offset), resources_(std::move(other.resources_)),
      capacity_bytes_(other.capacity_bytes_), elements_max_count_(other.elements_max_count_),
      elements_count_(other.elements_count_) {}

template <typename stored_key_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_>
inline transaction_deltas<stored_key_type_, atomic_reference_, waiter_type_, row_kit_type_>&
transaction_deltas<stored_key_type_, atomic_reference_, waiter_type_, row_kit_type_>::operator=(
    transaction_deltas&& other) noexcept {
    std::swap(collections_by_id, other.collections_by_id);
    std::swap(insertions_buffer, other.insertions_buffer);
    std::swap(next_inserted_offset, other.next_inserted_offset);
    std::swap(resources_, other.resources_);
    std::swap(capacity_bytes_, other.capacity_bytes_);
    std::swap(elements_max_count_, other.elements_max_count_);
    std::swap(elements_count_, other.elements_count_);
    return *this;
}

template <typename stored_key_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_>
inline std::size_t transaction_deltas<stored_key_type_, atomic_reference_, waiter_type_, row_kit_type_>::count(
    collection_id_t id) const noexcept {
    auto iterator = collections_by_id.find(id);
    return iterator == collections_by_id.end() ? 0 : iterator.value().stored_values.size();
}

template <typename stored_key_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_>
template <typename callback_type_>
inline void transaction_deltas<stored_key_type_, atomic_reference_, waiter_type_, row_kit_type_>::for_each_collection(
    callback_type_&& callback) const noexcept {
    [[maybe_unused]] st::status_t const walked = collections_by_id.for_each([&](auto const& entry) noexcept {
        callback(
            collection_info_t {entry.key(), entry.value().stored_values.size(), entry.value().elements_size_bytes});
    });
}

template <typename stored_key_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_>
inline lookup_result_t transaction_deltas<stored_key_type_, atomic_reference_, waiter_type_, row_kit_type_>::lookup(
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
    if (result != lookup_result_t::missing_k) return result;
    for (range_t const& range : iterator.value().ranges)
        if (range.covers(stored_key)) return lookup_result_t::removed_k;
    return result;
}

template <typename stored_key_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_>
inline status_t transaction_deltas<stored_key_type_, atomic_reference_, waiter_type_, row_kit_type_>::stored_value_size(
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

template <typename stored_key_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_>
inline status_t transaction_deltas<stored_key_type_, atomic_reference_, waiter_type_, row_kit_type_>::select(
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

template <typename stored_key_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_>
template <typename stored_value_location_type_>
inline lookup_result_t transaction_deltas<stored_key_type_, atomic_reference_, waiter_type_, row_kit_type_>::find(
    collection_id_t id, stored_key_t stored_key, stored_value_location_type_& location) const noexcept {

    location.extent.size_bytes = 0;
    location.extent.offset = 0;
    location.extent.kind = delta_kind_t::remove_k;
    location.extent.recipient_id = reinterpret_cast<std::uint64_t>(this);
    location.extent.recipient_state_id = 0;

    auto result = lookup_result_t::missing_k;
    auto iterator = collections_by_id.find(id);
    if (iterator == collections_by_id.end()) return result;

    iterator.value().stored_values.probe_to_find(stored_key, [&](auto const& entry) noexcept {
        auto const& stored_value_place = entry.value();
        location.extent.offset = stored_value_place.offset;
        location.extent.size_bytes = stored_value_place.size;
        location.extent.kind = stored_value_place.kind;
        constexpr lookup_result_t results[2] = {lookup_result_t::written_k, lookup_result_t::removed_k};
        result = results[location.extent.kind == delta_kind_t::remove_k];
    });

    return result;
}

template <typename stored_key_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_>
template <typename stored_value_location_type_>
inline void transaction_deltas<stored_key_type_, atomic_reference_, waiter_type_, row_kit_type_>::select(
    stored_value_location_type_ const& location, stored_value_span_t stored_value,
    notifier<atomic_reference_, waiter_type_> notifier) const noexcept {

    if (!location) {
        notifier.fail({status_code_t::invalid_argument_k, "A stored value location naming nothing"});
        return;
    }
    if (stored_value.size() < location.aligned_buffer_size()) {
        notifier.fail({status_code_t::invalid_argument_k, "A buffer shorter than the stored value"});
        return;
    }
    if (location.extent.recipient_id != reinterpret_cast<std::uint64_t>(this)) {
        notifier.fail({status_code_t::invalid_argument_k, "A stored value location of other deltas"});
        return;
    }
    if (location.extent.recipient_state_id != 0) {
        notifier.fail({status_code_t::invalid_argument_k, "A stored value location of another buffer state"});
        return;
    }

    if (location.extent.size_bytes > 0) {
        auto inserted = as_bytes_view(insertions_buffer).subspan(location.extent.offset, location.extent.size_bytes);
        std::memcpy(stored_value.data() + location.buffer_padding(), inserted.data(), inserted.size());
    }
    notifier.did_one();
}

template <typename stored_key_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_>
inline status_t transaction_deltas<stored_key_type_, atomic_reference_, waiter_type_, row_kit_type_>::reach_collection_(
    collection_id_t id, value_widths_t widths, collection_deltas_t*& collection) noexcept {

    auto iterator = collections_by_id.find(id);
    if (iterator == collections_by_id.end()) {
        if ((widths.value_size > 0) & (widths.value_max_size > 0))
            return status_t {status_code_t::invalid_argument_k, "Invalid config"};

        collection_deltas_t fresh {
            widths,
            {{}, {}, constrained_allocator<std::byte, atomic_reference_> {resources_->memory_constraint}},
            0,
            ranges_t {constrained_allocator<range_t, atomic_reference_> {resources_->memory_constraint}}};
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

template <typename stored_key_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_>
inline status_t transaction_deltas<stored_key_type_, atomic_reference_, waiter_type_, row_kit_type_>::write(
    collection_id_t id, value_widths_t widths, stored_key_t stored_key, stored_value_view_t stored_value,
    transaction_stamp_t watch_stamp) noexcept {

    collection_deltas_t* collection = nullptr;
    auto status = reach_collection_(id, widths, collection);
    if (status.failed()) return status;

    status = check_stored_value(collection->widths, stored_key, stored_value.size());
    if (status.failed()) return status;

    if (!can_fit(1, stored_value.size())) [[unlikely]]
        return status_t {status_code_t::capacity_exhausted_k,
                         "The transaction holds transaction_max_deltas or transaction_max_bytes"};

    // The room for the key is taken before a byte of the value lands, so a refusal writes nothing.
    if (collection->stored_values.reserve_more(1) == collection_deltas_t::stored_values_t::reserve_result_t::failed_k)
        return status_t {status_code_t::out_of_memory_k, "No room for another key"};

    if (!stored_value.empty() && insertions_buffer.size() != capacity_bytes_ &&
        insertions_buffer.resize(st::assume_uninitialized, capacity_bytes_) != st::status_t::success_k)
        return status_t {status_code_t::out_of_memory_k, "No room for the bytes of the transaction"};
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

template <typename stored_key_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_>
inline status_t transaction_deltas<stored_key_type_, atomic_reference_, waiter_type_, row_kit_type_>::remove(
    collection_id_t id, value_widths_t widths, stored_key_t stored_key, transaction_stamp_t watch_stamp) noexcept {

    collection_deltas_t* collection = nullptr;
    auto status = reach_collection_(id, widths, collection);
    if (status.failed()) return status;

    if (!can_fit(1, 0)) [[unlikely]]
        return status_t {status_code_t::capacity_exhausted_k,
                         "The transaction holds transaction_max_deltas or transaction_max_bytes"};

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

template <typename stored_key_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_>
inline status_t transaction_deltas<stored_key_type_, atomic_reference_, waiter_type_, row_kit_type_>::remove_range(
    collection_id_t id, value_widths_t widths, stored_key_t start, stored_key_t end) noexcept {

    if (!(start < end)) return status_t {status_code_t::invalid_argument_k, "A range ends at or before its start"};
    collection_deltas_t* collection = nullptr;
    auto status = reach_collection_(id, widths, collection);
    if (status.failed()) return status;

    if (!can_fit(1, 0)) [[unlikely]]
        return status_t {status_code_t::capacity_exhausted_k,
                         "The transaction holds transaction_max_deltas or transaction_max_bytes"};
    if (collection->ranges.reserve(collection->ranges.size() + 1) != st::status_t::success_k)
        return status_t {status_code_t::out_of_memory_k, "No room for another range"};

    range_t const range {start, end};
    st::vector<stored_key_t, constrained_allocator<stored_key_t, atomic_reference_>> inside {
        constrained_allocator<stored_key_t, atomic_reference_> {resources_->memory_constraint}};
    status_t listed;
    [[maybe_unused]] st::status_t const walked = collection->stored_values.for_each([&](auto const& entry) noexcept {
        if (listed.succeeded() && range.covers(entry.key()))
            listed = utilities::growth_status(inside.push_back(stored_key_t {entry.key()}));
    });
    if (listed.failed()) return listed;
    for (stored_key_t const& stored_key : inside) {
        collection->stored_values.probe_to_find(
            stored_key, [&](auto const& entry) noexcept { collection->elements_size_bytes -= entry.value().size; });
        [[maybe_unused]] bool const erased = collection->stored_values.erase(stored_key);
        --elements_count_;
    }
    [[maybe_unused]] st::status_t const placed = collection->ranges.push_back(st::assume_reserved, range_t {range});
    ++elements_count_;
    return status_t {};
}

template <typename stored_key_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_>
inline std::span<
    typename transaction_deltas<stored_key_type_, atomic_reference_, waiter_type_, row_kit_type_>::range_t const>
transaction_deltas<stored_key_type_, atomic_reference_, waiter_type_, row_kit_type_>::ranges(
    collection_id_t id) const noexcept {
    auto iterator = collections_by_id.find(id);
    if (iterator == collections_by_id.end()) return {};
    return std::span<range_t const> {iterator.value().ranges};
}

template <typename stored_key_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_>
template <utilities::fold_concept fold_type_>
inline status_t transaction_deltas<stored_key_type_, atomic_reference_, waiter_type_, row_kit_type_>::change(
    collection_id_t id, value_widths_t widths, stored_key_t stored_key, stored_value_view_t change,
    fold_type_ const& fold) noexcept {

    collection_deltas_t* collection = nullptr;
    auto status = reach_collection_(id, widths, collection);
    if (status.failed()) return status;
    if (stored_key == limit<stored_key_t>::max()) return status_t {status_code_t::malformed_k, "Invalid stored key"};
    if (collection->stored_values.reserve_more(1) == collection_deltas_t::stored_values_t::reserve_result_t::failed_k)
        return status_t {status_code_t::out_of_memory_k, "No room for another key"};

    // A change composes with a held change and applies to a held write or removal, so one delta
    // per key reaches the seal, and a lone one is composed too, so every held chain reads in the
    // fold's own order
    utilities::local_arena_t scratch;
    utilities::arena_allocator<byte_t> arena = scratch.allocator();
    delta_view_t const newer {delta_kind_t::change_k, change};
    delta_view_t older;
    bool const held = delta(id, stored_key, older);
    // A key inside a range the deltas remove takes the change over that removal.
    bool const removed_in_range = !held && lookup(id, stored_key) == lookup_result_t::removed_k;
    if (removed_in_range) older = delta_view_t {delta_kind_t::remove_k, {}};
    delta_view_t folded {delta_kind_t::change_k, {}};
    if (held || removed_in_range) {
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
        return status_t {status_code_t::capacity_exhausted_k,
                         "The transaction holds transaction_max_deltas or transaction_max_bytes"};
    if (!folded.bytes.empty() && insertions_buffer.size() != capacity_bytes_ &&
        insertions_buffer.resize(st::assume_uninitialized, capacity_bytes_) != st::status_t::success_k)
        return status_t {status_code_t::out_of_memory_k, "No room for the bytes of the transaction"};
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

template <typename stored_key_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_>
inline bool transaction_deltas<stored_key_type_, atomic_reference_, waiter_type_, row_kit_type_>::delta(
    collection_id_t id, stored_key_t stored_key, delta_view_t& held) const noexcept {
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

template <typename stored_key_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_>
inline void transaction_deltas<stored_key_type_, atomic_reference_, waiter_type_, row_kit_type_>::rescope_watches(
    transaction_stamp_t watch_stamp) noexcept {
    [[maybe_unused]] st::status_t const walked = collections_by_id.for_each([&](auto& collection) noexcept {
        [[maybe_unused]] st::status_t const rescoped = collection.value_ref().stored_values.for_each(
            [&](auto& entry) noexcept {
                stored_value_place_t& place = entry.value_ref();
                if (place.watch_stamp != invalid_transaction_stamp_k) place.watch_stamp = watch_stamp;
            });
    });
}

template <typename stored_key_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_>
template <typename callback_type_>
inline void transaction_deltas<stored_key_type_, atomic_reference_, waiter_type_, row_kit_type_>::for_each_write(
    callback_type_&& callback) const noexcept {
    [[maybe_unused]] st::status_t const walked = collections_by_id.for_each([&](auto const& collection) noexcept {
        [[maybe_unused]] st::status_t const listed = collection.value().stored_values.for_each(
            [&](auto const& entry) noexcept { callback(collection.key(), entry.key(), entry.value()); });
    });
}

template <typename stored_key_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_>
template <typename callback_type_>
inline void transaction_deltas<stored_key_type_, atomic_reference_, waiter_type_, row_kit_type_>::for_each_delta(
    collection_id_t id, callback_type_&& callback) const noexcept {
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

template <typename stored_key_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_>
template <typename stored_value_location_type_>
inline status_t transaction_deltas<stored_key_type_, atomic_reference_, waiter_type_, row_kit_type_>::find(
    collection_id_t id, std::span<stored_key_t const> stored_keys, std::span<stored_value_location_type_> locations,
    missing_locations_t missing_locations) const {

    if (locations.size() < stored_keys.size())
        return status_t {status_code_t::invalid_argument_k, "Buffer is too small"};

    if (missing_locations == missing_locations_t::invalidate_k)
        for (std::size_t index = 0; index < stored_keys.size(); ++index)
            find<stored_value_location_type_>(id, stored_keys[index], locations[index]);
    else
        for (std::size_t index = 0; index < stored_keys.size(); ++index) {
            stored_value_location_type_ location;
            auto const found = find<stored_value_location_type_>(id, stored_keys[index], location);
            if (found != lookup_result_t::missing_k) locations[index] = location;
        }

    return status_t {};
}

template <typename stored_key_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_>
template <typename stored_value_location_type_>
inline void transaction_deltas<stored_key_type_, atomic_reference_, waiter_type_, row_kit_type_>::select(
    std::span<stored_value_location_type_ const> locations, bytes_span_t buffer,
    notifier<atomic_reference_, waiter_type_> notifier) const {

    for (std::size_t index = 0, offset = 0; (index < locations.size()) & !notifier.has_failed(); ++index) {
        auto const& location = locations[index];
        auto is_recipient = location.extent.recipient_id == reinterpret_cast<std::uint64_t>(this);
        if (is_recipient) {
            if (location.extent.size_bytes > 0) {
                auto stored_value = buffer.subspan(offset, location.aligned_buffer_size());
                select<stored_value_location_type_>(location, stored_value, notifier);
            }
            else notifier.did_one();
        }
        offset += location.aligned_buffer_size();
    }
}

template <typename stored_key_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_>
inline status_t
transaction_deltas<stored_key_type_, atomic_reference_, waiter_type_, row_kit_type_>::check_stored_value(
    value_widths_t const& widths, stored_key_t stored_key, stored_value_size_t stored_value_size) const noexcept {

    bool valid_key = stored_key != limit<stored_key_t>::max();
    bool valid_fixed_width_stored_value = (widths.value_size > 0) & (stored_value_size == widths.value_size);
    bool valid_variable_width_stored_value = stored_value_size <= widths.value_max_size;
    bool valid = valid_key & (valid_fixed_width_stored_value ^ valid_variable_width_stored_value);
    return valid ? status_t {} : status_t {status_code_t::malformed_k, "Invalid stored value"};
}

template <typename stored_key_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_>
inline bool transaction_deltas<stored_key_type_, atomic_reference_, waiter_type_, row_kit_type_>::can_fit(
    std::size_t stored_values_count, std::size_t stored_values_sum_size) const noexcept {

    auto used_memory = next_inserted_offset;
    bool has_element_place = elements_count_ + stored_values_count <= elements_max_count_;
    // A place names its offset in 32 bits, which every value of the buffer stays within.
    bool has_enough_space = (used_memory + stored_values_sum_size <= capacity_bytes_) &
                            (used_memory + stored_values_sum_size <= std::numeric_limits<stored_value_offset_t>::max());
    return has_element_place & has_enough_space;
}

template <typename stored_key_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_>
inline void transaction_deltas<stored_key_type_, atomic_reference_, waiter_type_, row_kit_type_>::clear() noexcept {
    collections_by_id.clear();
    next_inserted_offset = 0;
    elements_count_ = 0;
}

#pragma endregion Transaction Deltas

#pragma region Read Sets

/** When a transaction checks its watched keys for conflicts. */
enum class watch_schedule_t : std::uint8_t {

    /** Ignores key collisions. */
    never_k,

    /** Checks in place, at the moment of the call. */
    now_k,

    /** Checks between the transaction start and its commit. */
    on_commit_k,

    /** Checks between the operation and the commit. */
    on_commit_since_operation_k,

    /** Schedule used when none is specified. */
    default_k = on_commit_k,
};

/** Why a transaction watches a key it did not write, which a second watch of the same key widens; a
 *  written key's watch lives beside its write instead. */
enum class watch_purpose_t : std::uint8_t {

    /** The transaction read the key. */
    read_k = 0x01,

    /** The transaction relies on the key standing as it found it, which a removal or a whole
     *  write since breaks and a change delta folded onto it leaves alone. */
    existence_k = 0x02,

    /** The transaction tracks the key without reading or writing it. */
    track_k = 0x04,

    /** Every purpose, which a count with no filter asks for. */
    all_k = read_k | existence_k | track_k,
};

/** The purposes of either watch. */
constexpr watch_purpose_t operator|(watch_purpose_t first, watch_purpose_t second) noexcept {
    return static_cast<watch_purpose_t>(static_cast<std::uint8_t>(first) | static_cast<std::uint8_t>(second));
}

/** The purposes both watches share. */
constexpr watch_purpose_t operator&(watch_purpose_t first, watch_purpose_t second) noexcept {
    return static_cast<watch_purpose_t>(static_cast<std::uint8_t>(first) & static_cast<std::uint8_t>(second));
}

/**
 *  @brief The keys one transaction reads or tracks, each with the stamp it was first watched
 *      from and why.
 *
 *  Watching the same key twice keeps the earlier stamp and widens the purpose.
 */
template <typename stored_key_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_ = st::native_row_kit_t>
class read_set {
  public:
    using stored_key_t = stored_key_type_;

    /** From which stamp a key was watched, and why. */
    struct watch_t {

        /** The earliest stamp any watch of this key was scoped from. */
        transaction_stamp_t watch_stamp;

        /** Every purpose the watches of this key were taken for. */
        watch_purpose_t purpose;
    };

    using stored_keys_t = st::basic_hash_table<st::mapping<stored_key_t, watch_t>, stored_key_hash_t, st::equal_to_t,
                                               constrained_allocator<std::byte, atomic_reference_>, row_kit_type_>;

    /** A set charging @p resources for the keys it holds. */
    explicit read_set(resources_shared_ptr<atomic_reference_, waiter_type_> const& resources) noexcept
        : stored_keys({}, {}, constrained_allocator<std::byte, atomic_reference_> {resources->memory_constraint}) {}

    /** Whether the transaction watches nothing. */
    bool empty() const noexcept { return stored_keys.empty(); }

    /** The watched keys taken for every purpose in @p purpose. */
    std::size_t count(watch_purpose_t purpose = watch_purpose_t::all_k) const noexcept {
        if (purpose == watch_purpose_t::all_k) return stored_keys.size();
        std::size_t matching = 0;
        [[maybe_unused]] st::status_t const walked = stored_keys.for_each(
            [&](auto const& entry) noexcept { matching += (entry.value().purpose & purpose) == purpose; });
        return matching;
    }

    /** Makes room for @p count more keys, so a following @c add takes no allocation. */
    status_t reserve_more(std::size_t count) noexcept {
        if (stored_keys.reserve_more(count) == stored_keys_t::reserve_result_t::failed_k)
            return {status_code_t::out_of_memory_k, "No room for the keys a transaction watches"};
        return {};
    }

    /**
     *  @brief Watches @p stored_key from @p watch_stamp for @p purpose, in
     *      room @c reserve_more took.
     *  @param[in] stored_key The key the transaction watches.
     *  @param[in] watch_stamp The stamp this watch is scoped from, which only lowers a key's stamp.
     *  @param[in] purpose Why this watch was taken, which only widens a key's purpose.
     */
    void add(stored_key_t stored_key, transaction_stamp_t watch_stamp, watch_purpose_t purpose,
             st::assume_reserved_t) noexcept {
        auto insert = [&](auto& unused_slot) noexcept {
            new (&unused_slot.key_ref()) stored_key_t {stored_key};
            new (&unused_slot.value_ref()) watch_t {watch_stamp, purpose};
        };
        auto update = [&](auto& equal_slot) noexcept {
            watch_t& watch = equal_slot.value_ref();
            watch.watch_stamp = std::min(watch_stamp, watch.watch_stamp);
            watch.purpose = watch.purpose | purpose;
        };
        [[maybe_unused]] auto const stored = stored_keys.probe_to_upsert(stored_key, insert, update,
                                                                         st::assume_reserved);
    }

    /** Drops every watch, for a transaction starting over. */
    void clear() noexcept { stored_keys.clear(); }

    /** The watched keys, charged to the budget the set was made with. */
    stored_keys_t stored_keys;
};

/** The keys one transaction watches, kept apart per collection so two collections never collide. */
template <typename stored_key_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_ = st::native_row_kit_t>
class grouped_read_set {
  public:
    using stored_key_t = stored_key_type_;
    using read_set_t = read_set<stored_key_t, atomic_reference_, waiter_type_, row_kit_type_>;
    using groups_t =
        st::basic_hash_table<st::mapping<collection_id_t, read_set_t>, collection_id_hash_t, st::equal_to_t,
                             constrained_allocator<std::byte, atomic_reference_>, row_kit_type_>;

    /** A set charging @p resources for the keys of every collection it watches. */
    explicit grouped_read_set(resources_shared_ptr<atomic_reference_, waiter_type_> resources) noexcept
        : resources(resources),
          groups({}, {}, constrained_allocator<std::byte, atomic_reference_> {resources->memory_constraint}) {}

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
    std::size_t count(collection_id_t id, watch_purpose_t purpose = watch_purpose_t::all_k) const noexcept {
        auto iterator = groups.find(id);
        return iterator == groups.end() ? 0 : iterator.value().count(purpose);
    }

    /** Makes room for @p count more keys of @p id, so a following @c add takes no allocation. */
    status_t reserve_more(collection_id_t id, std::size_t count) noexcept {
        auto iterator = groups.find(id);
        if (iterator == groups.end()) {
            if (groups.reserve_more(1) == groups_t::reserve_result_t::failed_k)
                return {status_code_t::out_of_memory_k, "No room for another watched collection"};
            groups.emplace(id, read_set_t {resources}, st::assume_reserved);
            iterator = groups.find(id);
            if (iterator == groups.end())
                return {status_code_t::out_of_memory_k, "No room for another watched collection"};
        }
        return iterator.value_ref().reserve_more(count);
    }

    /** Watches @p stored_key of @p id from @p watch_stamp, in room @c reserve_more took. */
    void add(collection_id_t id, stored_key_t stored_key, transaction_stamp_t watch_stamp, watch_purpose_t purpose,
             st::assume_reserved_t) noexcept {
        auto iterator = groups.find(id);
        if (iterator == groups.end()) return;
        iterator.value_ref().add(stored_key, watch_stamp, purpose, st::assume_reserved);
    }

    /** Drops every watch of every collection, for a transaction starting over. */
    void clear() noexcept { groups.clear(); }

    /** The budget every watched collection's keys are charged to. */
    resources_shared_ptr<atomic_reference_, waiter_type_> resources;

    /** One watched-key set per collection the transaction touched. */
    groups_t groups;
};

#pragma endregion Read Sets

#pragma region Transaction State

/**
 *  @brief What one transaction writes and watches in collections whose store keys them otherwise
 *      than the database, kept by that store's own code and joined to the transaction, which lands
 *      it with its own writes.
 *
 *  Its owner seals it on the committing thread before the transaction stages or commits. The thread
 *  holding the ledger's leader word then prepares it beside the transaction's own collections, and
 *  publishes it under the drawn stamp, or forgets what it prepared for a commit it refuses and for
 *  a stage let go.
 */
struct transaction_share_t {

    /** The share joined before this one, or null. */
    transaction_share_t* next = nullptr;

    /** What every step is handed, owned by whoever joined the share. */
    void* context = nullptr;

    /** Refuses the commit with @c write_conflict_k for a watch a commit since violated, or lays out
     *  every view the share publishes, leaving none laid out when it refuses. */
    status_t (*prepare)(void* context) noexcept = nullptr;

    /** Publishes what @c prepare laid out under @p stamp. */
    void (*publish)(void* context, transaction_stamp_t stamp) noexcept = nullptr;

    /** Drops what @c prepare laid out. */
    void (*forget)(void* context) noexcept = nullptr;
};

/** Everything one transaction carries between its start and its commit. */
template <typename stored_key_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_ = st::native_row_kit_t>
struct transaction_state {
    using stored_key_t = stored_key_type_;
    using transaction_deltas_t = transaction_deltas<stored_key_t, atomic_reference_, waiter_type_, row_kit_type_>;
    using read_set_t = grouped_read_set<stored_key_t, atomic_reference_, waiter_type_, row_kit_type_>;

    /** One range of keys a transaction walked, which any commit landing inside it
     *  since @c watch_stamp conflicts with, the keys it never found included. */
    struct range_watch_t {
        collection_id_t collection = collection_invalid_id_k;
        stored_key_t lower = stored_key_t {};
        stored_key_t upper = stored_key_t {};
        transaction_stamp_t watch_stamp = invalid_transaction_stamp_k;
    };

    /** The ranges a transaction walked, in the order it walked them. */
    using range_watches_t = st::vector<range_watch_t, constrained_allocator<range_watch_t, atomic_reference_>>;

    /** One written collection's deltas, sealed by the committer and landed by the leader. */
    struct sealed_collection_t {

        /** The collection written, which the leader looks up again. */
        collection_id_t id = collection_invalid_id_k;

        /** Its deltas and the run they become. */
        sealed_commit<stored_key_t, atomic_reference_, row_kit_type_> commit;

        /** The memory-run record the run lands in, claimed before the leader word. */
        memory_run_claim_t record;
    };

    /** Every written collection's sealed deltas, between a stage or a commit and its landing. */
    using sealed_collections_t =
        st::vector<sealed_collection_t, constrained_allocator<sealed_collection_t, atomic_reference_>>;

    inline transaction_state(transaction_deltas_t&& written,
                             resources_shared_ptr<atomic_reference_, waiter_type_> resources) noexcept
        : deltas(std::move(written)), read_set(resources),
          ranges(constrained_allocator<range_watch_t, atomic_reference_> {resources->memory_constraint}),
          sealed(constrained_allocator<sealed_collection_t, atomic_reference_> {resources->memory_constraint}) {}

    /** Whether the transaction has neither written nor watched anything. */
    inline bool empty() const noexcept { return deltas.empty() && read_set.empty() && ranges.empty(); }

    /** Drops both the writes and the watches, for a rollback or a renewal. */
    inline void clear() noexcept {
        deltas.clear();
        read_set.clear();
        ranges.clear();
        sealed.clear();
    }

    /** The deltas the transaction has not committed yet, filed under the collection
     *  each belongs to. */
    transaction_deltas_t deltas;

    /** The keys the transaction watches for conflicts. */
    read_set_t read_set;

    /** The ranges the transaction watches for conflicts, phantoms included. */
    range_watches_t ranges;

    /** The writes sealed per collection, filled by a stage or a commit and emptied once it lands
     *  or is refused. */
    sealed_collections_t sealed;

    /** The shares joined to the transaction, newest first, which outlive it and
     *  which nothing clears. */
    transaction_share_t* shares = nullptr;
};

#pragma endregion Transaction State

#pragma region Store Interface

/**
 *  @brief What a database asks of the store one collection lives in: which collections it keeps,
 *      its mount, its reads, the steps of a commit, its maintenance and the runs a schema names.
 *
 *  A scan walks the store through the @c nvme_store_cursor its key slot and fold name.
 */
template <typename store_type_>
concept nvme_store_concept = requires(
    store_type_& store, store_type_ const& viewed, typename store_type_::volume_t& volume,
    collection_config_t const& config, typename store_type_::constraint_t& constraint,
    typename store_type_::ledger_t& ledger, io_channel_t& channel, lsm_tree_publication_hook_t const& published,
    compactor_ref_t const& compactor, std::span<lsm_tree_run_record_t const> runs,
    st::vector<lsm_tree_run_record_t>& listed, typename store_type_::run_pool_t& run_pool,
    typename store_type_::ingest_runs_t& ingest_runs, typename store_type_::stored_key_t stored_key,
    std::span<byte_t> into, stored_value_size_t& size, typename store_type_::sealed_commit_t& sealed,
    memory_run_claim_t& claimed, transaction_stamp_t stamp, std::size_t thread_index) {
    typename store_type_::key_slot_t;
    typename store_type_::fold_t;
    {
        store_type_::mount(volume, config, constraint, ledger, run_pool)
    } noexcept -> std::same_as<expected<store_type_>>;
    { store.restore(channel, runs) } noexcept -> std::same_as<status_t>;
    { store.truncate_above(channel, channel, stamp, published) } noexcept -> std::same_as<status_t>;
    { store.destroy() } noexcept;
    { record_lsm_tree_runs(viewed.tree(), listed) } noexcept -> std::same_as<status_t>;
    { viewed.population_count() } noexcept -> std::same_as<std::uint64_t>;
    { viewed.size_bytes() } noexcept -> std::same_as<std::uint64_t>;
    { viewed.value_max_size() } noexcept -> std::same_as<stored_value_size_t>;
    { viewed.find(stored_key, size, stamp, thread_index) } noexcept -> std::same_as<lookup_result_t>;
    { store.select(channel, stored_key, into, size, stamp, thread_index) } noexcept -> std::same_as<status_t>;
    { viewed.written_since(stored_key, stamp) } noexcept -> std::same_as<bool>;
    { viewed.range_written_since(stored_key, stored_key, stamp) } noexcept -> std::same_as<bool>;
    { viewed.memory_pressure(sealed) } noexcept -> std::same_as<memory_pressure_t>;
    { store.try_claim(claimed) } noexcept -> std::same_as<status_t>;
    { store.give_back(claimed) } noexcept;
    { viewed.vacated() } noexcept -> std::same_as<std::uint32_t const&>;
    { store.prepare(sealed, std::move(claimed)) } noexcept;
    { store.publish(sealed, stamp) } noexcept;
    { store.forget_prepared() } noexcept;
    { viewed.memory_shape() } noexcept -> std::same_as<memory_shape_t>;
    { viewed.maintenance_due() } noexcept -> std::same_as<bool>;
    { store.merge_memory_runs() } noexcept -> std::same_as<status_t>;
    { viewed.flush_due() } noexcept -> std::same_as<bool>;
    { store.flush(channel) } noexcept -> std::same_as<status_t>;
    { viewed.needs_compaction() } noexcept -> std::same_as<bool>;
    { store.compact(channel, channel, compactor, published) } noexcept -> std::same_as<expected<compaction_outcome_t>>;
    { store.begin_ingest() } noexcept -> std::same_as<typename store_type_::ingest_runs_t>;
    { store.publish_ingest(channel, ingest_runs) } noexcept -> std::same_as<expected<transaction_stamp_t>>;
    { store.drop_ingest(ingest_runs) } noexcept;
};

/**
 *  @brief What a registry needs from the store beneath it to stage and to commit, one collection
 *      at a time.
 *
 *  A committer seals each written collection on its own thread, making room first when the
 *  collection's memory runs are past their bounds. The thread holding the ledger's leader word then
 *  prepares each one, publishes it under the drawn stamp, or forgets what it prepared for a commit
 *  it refuses. Once its commit lands, the committer maintains every collection it wrote. The
 *  conflict checks read published views. A store of one collection answers every call for that
 *  collection, whichever ID names it.
 */
template <typename store_type_, typename stored_key_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, typename row_kit_type_ = st::native_row_kit_t>
concept transaction_store_concept = requires(
    store_type_& store, collection_id_t collection_id, stored_key_type_ stored_key,
    transaction_deltas<stored_key_type_, atomic_reference_, waiter_type_, row_kit_type_> const& deltas,
    sealed_commit<stored_key_type_, atomic_reference_, row_kit_type_>& sealed, memory_run_claim_t& claimed,
    transaction_stamp_t stamp) {
    { store.ledger() } -> std::same_as<commit_ledger<atomic_reference_, waiter_type_>&>;
    { store.written_since(collection_id, stored_key, stamp) } -> std::same_as<bool>;
    { store.range_written_since(collection_id, stored_key, stored_key, stamp) } -> std::same_as<bool>;
    { store.replaced_since(collection_id, stored_key, stamp) } -> std::same_as<bool>;
    { store.seal_collection(collection_id, deltas, sealed) } -> std::same_as<status_t>;
    { store.claim_memory_record(collection_id, claimed) } -> std::same_as<status_t>;
    { store.prepare_collection(collection_id, sealed, std::move(claimed)) } -> std::same_as<status_t>;
    { store.publish_collection(collection_id, sealed, stamp) } -> std::same_as<void>;
    { store.forget_collection(collection_id) } -> std::same_as<void>;
    { store.maintain(collection_id) } -> std::same_as<status_t>;
};

#pragma endregion Store Interface

#pragma region Transaction Registry

template <typename stored_key_type_, typename registry_type_, template <typename> typename atomic_reference_,
          typename waiter_type_>
class transaction_core;

/**
 *  @brief The transactions of one store, the stamps they commit under and the conflicts
 *      that stop them.
 *  @tparam stored_key_type_ The key every collection of the store is addressed by.
 *  @tparam store_type_ The store the commits land in, which must
 *      satisfy @c transaction_store_concept.
 *
 *  Nothing here is shared state of its own: the stamps come from the store's ledger, and a conflict
 *  is a key the store says was written since the watch, read from the memory runs and the runs that
 *  carry the key.
 */
template <typename stored_key_type_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, typename row_kit_type_ = st::native_row_kit_t>
class transaction_registry {
  public:
#pragma region Types

    using stored_key_t = stored_key_type_;
    using store_t = store_type_;
    using transaction_state_t = transaction_state<stored_key_t, atomic_reference_, waiter_type_, row_kit_type_>;
    using transaction_deltas_t = typename transaction_state_t::transaction_deltas_t;
    using collection_info_t = typename transaction_deltas_t::collection_info_t;

#pragma endregion Types

#pragma region Construction

    /** A registry committing into @p store, which outlives it. */
    inline transaction_registry(store_t& store,
                                resources_shared_ptr<atomic_reference_, waiter_type_> resources) noexcept
        : store_(&store), resources_(std::move(resources)) {}

    inline transaction_registry(transaction_registry&&) = delete;
    inline transaction_registry(transaction_registry const&) = delete;
    inline transaction_registry& operator=(transaction_registry const&) = delete;
    inline transaction_registry& operator=(transaction_registry&&) = delete;

#pragma endregion Construction

#pragma region Interface Methods

    /** A transaction over every collection of the store, driven through the front
     *  end @p transaction_type_, with the delta bounds of @p config. */
    template <typename transaction_type_>
    inline expected<std::unique_ptr<transaction_type_>> create_transaction(transaction_config_t const& config) noexcept;

#pragma endregion Interface Methods

  private:
    template <typename, typename, template <typename> typename peer_atomic_reference_, typename>
    friend class transaction_core;

#pragma region Commit Stages

    /** One transaction's commit, as the sequencer hands it to its two steps. */
    struct commit_t {

        /** The registry committing. */
        transaction_registry* owner = nullptr;

        /** The transaction's writes, watches and sealed deltas. */
        transaction_state_t* data = nullptr;

        /** The stamp every watch of the transaction is scoped from or past. */
        transaction_stamp_t start_stamp = invalid_transaction_stamp_k;
    };

    inline status_t stage_(transaction_state_t& data, transaction_stamp_t start_stamp,
                           transaction_stamp_t& stage_stamp) noexcept;
    inline transaction_stamp_t commit_staged_(transaction_state_t& data) noexcept;
    inline expected<transaction_stamp_t> commit_(transaction_state_t& data, transaction_stamp_t start_stamp) noexcept;
    inline void discard_(transaction_state_t& data) noexcept;
    inline status_t seal_(transaction_state_t& data) noexcept;
    inline status_t prepare_(transaction_state_t& data, transaction_stamp_t start_stamp) noexcept;
    inline void forget_(transaction_state_t& data, std::size_t prepared) noexcept;
    inline void maintain_(transaction_state_t& data) noexcept;
    static inline status_t prepare_commit_(void* context) noexcept;
    static inline void publish_commit_(void* context, transaction_stamp_t stamp) noexcept;

    /** Pins @p pin at the newest published stamp, which a transaction starts at and reads from. */
    inline transaction_stamp_t request_start_(stamp_pin<atomic_reference_, waiter_type_>& pin) noexcept {
        return store_->ledger().pin_reader(pin);
    }

    /** The stamp a watch taken now is scoped from: every commit at or below it has landed. */
    inline transaction_stamp_t current_stamp_() const noexcept { return store_->ledger().published(); }

#pragma endregion Commit Stages

#pragma region Conflict Check

    inline status_t watch_now_(transaction_stamp_t start_stamp, collection_id_t group_id,
                               std::span<stored_key_t const> stored_keys) noexcept;
    inline status_t watch_existence_now_(transaction_stamp_t start_stamp, collection_id_t group_id,
                                         std::span<stored_key_t const> stored_keys) noexcept;
    inline bool conflicting_(transaction_state_t const& data) noexcept;

#pragma endregion Conflict Check

#pragma region Private Members

    /** The store every commit of this registry lands in, owned by whoever named it. */
    store_t* store_;

    /** The budget every transaction's writes and watches are charged to. */
    resources_shared_ptr<atomic_reference_, waiter_type_> resources_;

#pragma endregion Private Members
};

#pragma region Interface Methods

template <typename stored_key_type_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, typename row_kit_type_>
template <typename transaction_type_>
inline expected<std::unique_ptr<transaction_type_>>
transaction_registry<stored_key_type_, store_type_, atomic_reference_, waiter_type_, row_kit_type_>::create_transaction(
    transaction_config_t const& config) noexcept {

    transaction_deltas_t deltas(config.max_bytes, config.max_deltas, resources_);
    std::unique_ptr<transaction_type_> transaction {new (std::nothrow)
                                                        transaction_type_ {*this, std::move(deltas), resources_}};
    if (!transaction) return status_t {status_code_t::out_of_memory_k, "No room for another transaction"};
    return transaction;
}

#pragma endregion Interface Methods

#pragma region Commit Stages

template <typename stored_key_type_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, typename row_kit_type_>
inline status_t transaction_registry<stored_key_type_, store_type_, atomic_reference_, waiter_type_,
                                     row_kit_type_>::stage_(transaction_state_t& data, transaction_stamp_t start_stamp,
                                                            transaction_stamp_t& stage_stamp) noexcept {

    // Sealed and claimed before the leader word is taken, so the sort, the copy and any flush hold
    // up no other committer.
    if (status_t const sealed = seal_(data); sealed.failed()) return sealed;
    store_->ledger().lead();
    stage_stamp = current_stamp_();
    status_t const prepared = prepare_(data, start_stamp);
    if (prepared.succeeded()) return status_t {};
    store_->ledger().stop_leading();
    data.sealed.clear();
    return prepared;
}

template <typename stored_key_type_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, typename row_kit_type_>
inline transaction_stamp_t transaction_registry<stored_key_type_, store_type_, atomic_reference_, waiter_type_,
                                                row_kit_type_>::commit_staged_(transaction_state_t& data) noexcept {

    // The stage prepared every collection's view, so from the stamp on nothing can fail.
    commit_t committing {this, &data, invalid_transaction_stamp_k};
    commit_request_t request;
    request.context = &committing;
    request.publish = &publish_commit_;
    transaction_stamp_t const commit_stamp = store_->ledger().commit_leading(request);
    maintain_(data);
    data.sealed.clear();
    return commit_stamp;
}

template <typename stored_key_type_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, typename row_kit_type_>
inline expected<transaction_stamp_t>
transaction_registry<stored_key_type_, store_type_, atomic_reference_, waiter_type_, row_kit_type_>::commit_(
    transaction_state_t& data, transaction_stamp_t start_stamp) noexcept {

    if (status_t const sealed = seal_(data); sealed.failed()) return sealed;
    // Even a commit that wrote nothing draws a stamp, so every commit has a place of its own
    // in the order.
    commit_t committing {this, &data, start_stamp};
    commit_request_t request;
    request.context = &committing;
    request.prepare = &prepare_commit_;
    request.publish = &publish_commit_;
    expected<transaction_stamp_t> committed = store_->ledger().submit(request);
    if (committed) maintain_(data);
    data.sealed.clear();
    return committed;
}

template <typename stored_key_type_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, typename row_kit_type_>
inline void transaction_registry<stored_key_type_, store_type_, atomic_reference_, waiter_type_,
                                 row_kit_type_>::discard_(transaction_state_t& data) noexcept {
    forget_(data, data.sealed.size());
    for (transaction_share_t* share = data.shares; share != nullptr; share = share->next) share->forget(share->context);
    store_->ledger().stop_leading();
    data.sealed.clear();
}

/** Seals every written collection's deltas on the committing thread, one sealed
 *  entry per collection. */
template <typename stored_key_type_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, typename row_kit_type_>
inline status_t transaction_registry<stored_key_type_, store_type_, atomic_reference_, waiter_type_,
                                     row_kit_type_>::seal_(transaction_state_t& data) noexcept {
    data.sealed.clear();
    if (data.sealed.reserve(data.deltas.collections_by_id.size()) != st::status_t::success_k)
        return status_t {status_code_t::out_of_memory_k, "No room to seal a commit"};
    status_t status = status_t {};
    data.deltas.for_each_collection([&](collection_info_t const& collection) noexcept {
        if (status.failed()) return;
        typename transaction_state_t::sealed_collection_t sealed;
        sealed.id = collection.id;
        status = store_->seal_collection(collection.id, data.deltas, sealed.commit);
        // Claimed before the leader word, waiting on a flush when every record is out, so the
        // commit cannot be refused one; a flush never needs a record, so holding this claim while
        // waiting for the next cannot deadlock.
        if (status.succeeded() && !sealed.commit.empty())
            status = store_->claim_memory_record(collection.id, sealed.record);
        if (status.succeeded()) [[maybe_unused]]
            st::status_t const kept = data.sealed.push_back(st::assume_reserved, std::move(sealed));
    });
    if (status.failed()) data.sealed.clear();
    return status;
}

/** Checks the watches against the views published so far and lays out every written collection's
 *  view, on the thread holding the leader word; a refusal forgets every view it laid out. */
template <typename stored_key_type_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, typename row_kit_type_>
inline status_t transaction_registry<stored_key_type_, store_type_, atomic_reference_, waiter_type_,
                                     row_kit_type_>::prepare_(transaction_state_t& data,
                                                              transaction_stamp_t start_stamp) noexcept {

    // Every watch is scoped from the start or later, so nothing published since the start leaves
    // nothing to check.
    if (current_stamp_() != start_stamp && conflicting_(data))
        return status_t {status_code_t::write_conflict_k, "Keys conflict"};
    for (std::size_t index = 0; index != data.sealed.size(); ++index)
        if (status_t const prepared = store_->prepare_collection(data.sealed[index].id, data.sealed[index].commit,
                                                                 std::move(data.sealed[index].record));
            prepared.failed()) {
            forget_(data, index);
            return prepared;
        }
    // A share checks its own watches, as only its store reads its keys, and forgets nothing
    // once it refuses.
    for (transaction_share_t* share = data.shares; share != nullptr; share = share->next)
        if (status_t const prepared = share->prepare(share->context); prepared.failed()) {
            forget_(data, data.sealed.size());
            for (transaction_share_t* undone = data.shares; undone != share; undone = undone->next)
                undone->forget(undone->context);
            return prepared;
        }
    return status_t {};
}

template <typename stored_key_type_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, typename row_kit_type_>
inline void transaction_registry<stored_key_type_, store_type_, atomic_reference_, waiter_type_,
                                 row_kit_type_>::forget_(transaction_state_t& data, std::size_t prepared) noexcept {
    for (std::size_t index = 0; index != prepared; ++index) store_->forget_collection(data.sealed[index].id);
}

/** Merges, flushes and compacts whatever the landed commit left due in the collections it wrote; a
 *  failure there leaves the commit landed, and the next commit making room meets it again. */
template <typename stored_key_type_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, typename row_kit_type_>
inline void transaction_registry<stored_key_type_, store_type_, atomic_reference_, waiter_type_,
                                 row_kit_type_>::maintain_(transaction_state_t& data) noexcept {
    for (auto const& sealed : data.sealed) [[maybe_unused]]
        status_t const maintained = store_->maintain(sealed.id);
}

template <typename stored_key_type_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, typename row_kit_type_>
inline status_t transaction_registry<stored_key_type_, store_type_, atomic_reference_, waiter_type_,
                                     row_kit_type_>::prepare_commit_(void* context) noexcept {
    commit_t const& committing = *static_cast<commit_t const*>(context);
    return committing.owner->prepare_(*committing.data, committing.start_stamp);
}

template <typename stored_key_type_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, typename row_kit_type_>
inline void transaction_registry<stored_key_type_, store_type_, atomic_reference_, waiter_type_,
                                 row_kit_type_>::publish_commit_(void* context, transaction_stamp_t stamp) noexcept {
    commit_t const& committing = *static_cast<commit_t const*>(context);
    for (auto& sealed : committing.data->sealed) {
        sealed.commit.adopt(committing.data->deltas.insertions_buffer);
        committing.owner->store_->publish_collection(sealed.id, sealed.commit, stamp);
    }
    for (transaction_share_t* share = committing.data->shares; share != nullptr; share = share->next)
        share->publish(share->context, stamp);
}

#pragma endregion Commit Stages

#pragma region Conflict Check

template <typename stored_key_type_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, typename row_kit_type_>
inline status_t transaction_registry<stored_key_type_, store_type_, atomic_reference_, waiter_type_,
                                     row_kit_type_>::watch_now_(transaction_stamp_t start_stamp,
                                                                collection_id_t group_id,
                                                                std::span<stored_key_t const> stored_keys) noexcept {

    bool written = false;
    for (std::size_t index = 0; index != stored_keys.size() && !written; ++index)
        written = store_->written_since(group_id, stored_keys[index], start_stamp);
    if (written) return status_t {status_code_t::write_conflict_k, "Keys conflict"};
    return status_t {};
}

/** Refuses with @c write_conflict_k when a commit since @p start_stamp removed or wholly wrote
 *  any of @p stored_keys, which an existence watch checking in place asks. */
template <typename stored_key_type_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, typename row_kit_type_>
inline status_t
transaction_registry<stored_key_type_, store_type_, atomic_reference_, waiter_type_,
                     row_kit_type_>::watch_existence_now_(transaction_stamp_t start_stamp, collection_id_t group_id,
                                                          std::span<stored_key_t const> stored_keys) noexcept {
    for (stored_key_t const& stored_key : stored_keys)
        if (store_->replaced_since(group_id, stored_key, start_stamp))
            return status_t {status_code_t::write_conflict_k, "A key was removed or replaced since the start"};
    return status_t {};
}

template <typename stored_key_type_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, typename row_kit_type_>
inline bool transaction_registry<stored_key_type_, store_type_, atomic_reference_, waiter_type_,
                                 row_kit_type_>::conflicting_(transaction_state_t const& data) noexcept {

    // First-committer-wins over the watched writes, then every key read or tracked.
    bool conflict = false;
    data.deltas.for_each_write(
        [&](collection_id_t collection_id, stored_key_t const& stored_key, auto const& place) noexcept {
            if (!conflict && place.watch_stamp != invalid_transaction_stamp_k)
                conflict = store_->written_since(collection_id, stored_key, place.watch_stamp);
        });
    if (conflict) return true;
    [[maybe_unused]] st::status_t const walked = data.read_set.groups.for_each(
        [&](auto const& group) noexcept -> st::walk_control_t {
            collection_id_t const group_id = group.key();
            [[maybe_unused]] st::status_t const watched = group.value().stored_keys.for_each(
                [&](auto const& watch) noexcept -> st::walk_control_t {
                    // A watch for existence alone lets change deltas through; any other purpose
                    // sees every write.
                    conflict = watch.value().purpose == watch_purpose_t::existence_k
                                   ? store_->replaced_since(group_id, watch.key(), watch.value().watch_stamp)
                                   : store_->written_since(group_id, watch.key(), watch.value().watch_stamp);
                    return conflict ? st::walk_control_t::halt_k : st::walk_control_t::resume_k;
                });
            return conflict ? st::walk_control_t::halt_k : st::walk_control_t::resume_k;
        });
    if (conflict) return true;
    for (auto const& range : data.ranges)
        if (store_->range_written_since(range.collection, range.lower, range.upper, range.watch_stamp)) return true;
    return false;
}

#pragma endregion Conflict Check

#pragma endregion Transaction Registry

#pragma region Transaction Core

/**
 *  @brief What every transaction of a store is made of: its writes and watches, the stamps it pins
 *      and the two-phase lifecycle its registry drives, which a front end wraps with the lookups
 *      and writes of its own key shape.
 *  @tparam stored_key_type_ The key every collection of the store is addressed by.
 *  @tparam registry_type_ The registry that orders this transaction against every other of
 *      the same store.
 *
 *  Nothing here throws: a mutation that outgrows the deltas answers @c capacity_exhausted_k,
 *  and the caller may commit what it has and start over. The core is reusable after @c commit,
 *  after @c rollback and after @c renew.
 */
template <typename stored_key_type_, typename registry_type_, template <typename> typename atomic_reference_,
          typename waiter_type_>
class transaction_core {
  public:

#pragma region Types

    /** The key every collection of the store is addressed by. */
    using stored_key_t = stored_key_type_;

    /** The registry that orders this transaction against every other of the same store. */
    using registry_t = registry_type_;

    /** The store every committed write of this transaction lands in. */
    using store_t = typename registry_t::store_t;

    /** The uncommitted writes and the watched keys this transaction carries. */
    using state_t = typename registry_t::transaction_state_t;

    /** The uncommitted deltas alone, filed under the collection they belong to. */
    using transaction_deltas_t = typename state_t::transaction_deltas_t;

    /** Whether this transaction is filling its deltas or holding the ledger's leader word. */
    enum class phase_t : std::uint8_t {

        /** The transaction takes reads and writes, and holds nothing of the store. */
        started_k,

        /** The transaction has checked its watches, laid out its views and holds the
         *  ledger's leader word. */
        staged_k,
    };

#pragma endregion Types

#pragma region Construction

    transaction_core(transaction_core const&) = delete;
    transaction_core(transaction_core&&) = delete;
    transaction_core& operator=(transaction_core const&) = delete;
    transaction_core& operator=(transaction_core&&) = delete;

    /**
     *  @brief A transaction started at the newest stamp @p registry has published, which it pins
     *      until it dies.
     *  @param[in,out] registry The registry that orders and conflict-checks it, outliving it.
     *  @param[in] deltas The deltas it fills, already sized to its configuration.
     *  @param[in] resources The budget its watched keys are charged to.
     */
    transaction_core(registry_t& registry, transaction_deltas_t&& deltas,
                     resources_shared_ptr<atomic_reference_, waiter_type_> resources) noexcept
        : registry_(registry), data_(std::move(deltas), std::move(resources)) {
        start_stamp_ = registry_.request_start_(pin_);
    }

    /** Hands the leader word back when a stage still holds it, then gives the start stamp back. */
    ~transaction_core() noexcept {
        if (phase_ == phase_t::staged_k) registry_.discard_(data_);
    }

#pragma endregion Construction

#pragma region Lifecycle

    /**
     *  @brief Seals every write, takes the ledger's leader word, checks every watched key and lays
     *      out every view the commit publishes, without applying anything, which reserves this
     *      commit's place in the next batch.
     *
     *  A failure keeps the changes, so a caller can still log what it could not stage, and a
     *  success holds the leader word until @c commit or @c rollback hands it back; readers never
     *  wait for it, later committers do.
     */
    status_t stage() noexcept {
        if (phase_ != phase_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        transaction_stamp_t staged_stamp = invalid_transaction_stamp_k;
        status_t const staged = registry_.stage_(data_, start_stamp_, staged_stamp);
        if (staged.succeeded()) {
            last_stage_stamp_ = staged_stamp;
            phase_ = phase_t::staged_k;
        }
        return staged;
    }

    /**
     *  @brief Lands every write of this transaction, through the ledger's sequencer unless a stage
     *      already holds the leader word, and returns once its stamp is published.
     *
     *  @note Nothing is held on return, and only a success clears the changes and the watches and
     *      starts the transaction again.
     */
    status_t commit() noexcept {
        if (phase_ == phase_t::staged_k) last_try_to_commit_stamp_ = registry_.commit_staged_(data_);
        else {
            auto [committed, commit_status] = registry_.commit_(data_, start_stamp_);
            if (commit_status.failed()) {
                last_try_to_commit_stamp_ = registry_.current_stamp_();
                return commit_status;
            }
            last_try_to_commit_stamp_ = committed;
        }
        restart_();
        return {};
    }

    /** Drops every write and every watch, and starts the transaction over from this moment. */
    void rollback() noexcept {
        if (phase_ == phase_t::staged_k) registry_.discard_(data_);
        phase_ = phase_t::started_k;
        data_.clear();
        operation_pin_.release();
        start_stamp_ = registry_.request_start_(pin_);
    }

    /**
     *  @brief Hands back the leader word a stage took, keeping every write and every watch, so the
     *      next stage or commit checks them all again.
     *
     *  @note A staged transaction holds the ledger's leader word, which every other committer of
     *      the ledger waits for, so a caller that only wants a stage's verdict lets it go at once.
     */
    void unstage() noexcept {
        if (phase_ == phase_t::staged_k) registry_.discard_(data_);
        phase_ = phase_t::started_k;
    }

    /**
     *  @brief Starts over from this moment while keeping the writes, whose watches move to the new
     *      start, which only a transaction that read nothing may.
     *  @return @c invalid_argument_k when the transaction has read anything, whose
     *      watches cannot move.
     */
    status_t renew() noexcept {
        if (phase_ != phase_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        if (data_.read_set.count(watch_purpose_t::read_k) != 0)
            return {status_code_t::invalid_argument_k, "That transaction has read something"};
        start_stamp_ = registry_.request_start_(pin_);
        data_.deltas.rescope_watches(start_stamp_);
        return {};
    }

    /** Joins @p share, which every stage and commit from now on lays out and lands with this
     *  transaction's own writes, and which must outlive the transaction. */
    void join(transaction_share_t& share) noexcept {
        share.next = data_.shares;
        data_.shares = &share;
    }

#pragma endregion Lifecycle

#pragma region Watches

    /** Watches @p stored_keys of @p collection_id for conflicts, without reading or writing any of
     *  them, which a commit and a rollback both drop. */
    template <watch_schedule_t watch_schedule_>
    status_t watch_more(collection_id_t collection_id, std::span<stored_key_t const> stored_keys) noexcept {
        if (phase_ != phase_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        if (status_t const prepared = watch_now_or_prepare<watch_schedule_>(collection_id, stored_keys);
            prepared.failed())
            return prepared;
        watch_later<watch_schedule_>(collection_id, stored_keys, watch_stamp_for<watch_schedule_>(),
                                     watch_purpose_t::track_k);
        return {};
    }

    /** Watches @p stored_keys of @p collection_id for being removed or wholly written, letting
     *  change deltas through, which an edit folding onto a key that must still stand takes. */
    template <watch_schedule_t watch_schedule_>
    status_t watch_existence(collection_id_t collection_id, std::span<stored_key_t const> stored_keys) noexcept {
        if (phase_ != phase_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        if constexpr (watch_schedule_ == watch_schedule_t::now_k)
            return registry_.watch_existence_now_(start_stamp_, collection_id, stored_keys);
        else {
            if (status_t const prepared = watch_now_or_prepare<watch_schedule_>(collection_id, stored_keys);
                prepared.failed())
                return prepared;
            watch_later<watch_schedule_>(collection_id, stored_keys, watch_stamp_for<watch_schedule_>(),
                                         watch_purpose_t::existence_k);
            return {};
        }
    }

    /** Watches every key of @p collection_id from @p lower to @p upper inclusive, the absent
     *  ones too, which a scan this transaction ran over them does so a commit inserting into
     *  the range conflicts. */
    template <watch_schedule_t watch_schedule_>
    status_t watch_range(collection_id_t collection_id, stored_key_t lower, stored_key_t upper) noexcept {
        if (phase_ != phase_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        if constexpr (watch_schedule_ == watch_schedule_t::never_k) return {};
        else if constexpr (watch_schedule_ == watch_schedule_t::now_k) {
            if (store().range_written_since(collection_id, lower, upper, start_stamp_))
                return {status_code_t::write_conflict_k, "A commit landed in that range since the start"};
            return {};
        }
        else {
            if (data_.ranges.push_back(typename state_t::range_watch_t {
                    collection_id, lower, upper, watch_stamp_for<watch_schedule_>()}) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A transaction could not watch another range"};
            return {};
        }
    }

    /** Checks @p stored_keys of @p collection_id right away, or makes the room a later watch
     *  of them needs. */
    template <watch_schedule_t watch_schedule_>
    status_t watch_now_or_prepare(collection_id_t collection_id, std::span<stored_key_t const> stored_keys) noexcept {
        if constexpr (watch_schedule_ == watch_schedule_t::now_k)
            return registry_.watch_now_(start_stamp_, collection_id, stored_keys);
        else if constexpr (watch_schedule_ == watch_schedule_t::on_commit_k ||
                           watch_schedule_ == watch_schedule_t::on_commit_since_operation_k)
            return data_.read_set.reserve_more(collection_id, stored_keys.size());
        else return {};
    }

    /** Records every key of @p stored_keys as watched from @p from for @p purpose, in room the
     *  step above took. */
    template <watch_schedule_t watch_schedule_>
    void watch_later(collection_id_t collection_id, std::span<stored_key_t const> stored_keys, transaction_stamp_t from,
                     watch_purpose_t purpose) noexcept {
        if constexpr (watch_schedule_ == watch_schedule_t::on_commit_k ||
                      watch_schedule_ == watch_schedule_t::on_commit_since_operation_k)
            for (stored_key_t const& stored_key : stored_keys)
                data_.read_set.add(collection_id, stored_key, from, purpose, st::assume_reserved);
    }

    /** Refuses a write of @p stored_key right away when @p watch_schedule_ checks in place and a
     *  commit since the start wrote it; every other schedule leaves the check to the stage. */
    template <watch_schedule_t watch_schedule_>
    status_t check_write_now(collection_id_t collection_id, stored_key_t stored_key) noexcept {
        if constexpr (watch_schedule_ == watch_schedule_t::now_k)
            return registry_.watch_now_(start_stamp_, collection_id, std::span<stored_key_t const> {&stored_key, 1});
        else return {};
    }

    /** The stamp a watch taken now is scoped from, which the schedule decides: under a watch scoped
     *  from the operation, the stamp the last read pinned, or the published one before any read. */
    template <watch_schedule_t watch_schedule_>
    [[nodiscard]] transaction_stamp_t watch_stamp_for() const noexcept {
        if constexpr (watch_schedule_ == watch_schedule_t::on_commit_k) return start_stamp_;
        else if constexpr (watch_schedule_ == watch_schedule_t::on_commit_since_operation_k)
            return operation_pin_.held() ? operation_pin_.stamp() : registry_.current_stamp_();
        else return invalid_transaction_stamp_k;
    }

    /** The stamp a read under @p watch_schedule_ answers at: the start, but for a watch scoped from
     *  the read itself, the newest published stamp, pinned so its versions outlive a flush or a
     *  merge until the next read. */
    template <watch_schedule_t watch_schedule_>
    [[nodiscard]] transaction_stamp_t read_stamp_for() noexcept {
        if constexpr (watch_schedule_ == watch_schedule_t::on_commit_since_operation_k)
            return registry_.request_start_(operation_pin_);
        else return start_stamp_;
    }

#pragma endregion Watches

#pragma region Stamps

    /** The stamp this transaction was started or restarted at, which scopes every
     *  watch it takes. */
    [[nodiscard]] transaction_stamp_t start_stamp() const noexcept { return start_stamp_; }

    /** The stamp this transaction last staged at, or @c invalid_transaction_stamp_k while
     *  it never has. */
    [[nodiscard]] transaction_stamp_t last_stage_stamp() const noexcept { return last_stage_stamp_; }

    /** The stamp this transaction last committed at, or @c invalid_transaction_stamp_k while
     *  it never has. */
    [[nodiscard]] transaction_stamp_t last_commit_stamp() const noexcept { return last_commit_stamp_; }

    /** The stamp this transaction last tried to commit at, whether or not that commit landed. */
    [[nodiscard]] transaction_stamp_t last_try_to_commit_stamp() const noexcept { return last_try_to_commit_stamp_; }

#pragma endregion Stamps

#pragma region Access

    /** Whether this transaction is filling its deltas rather than holding the
     *  ledger's leader word. */
    [[nodiscard]] bool started() const noexcept { return phase_ == phase_t::started_k; }

    /** The writes and the watches this transaction carries, which a read of it also fills. */
    [[nodiscard]] state_t& state() noexcept { return data_; }
    [[nodiscard]] state_t const& state() const noexcept { return data_; }

    /** The store this transaction reads and commits into, which the registry beside it names. */
    [[nodiscard]] store_t& store() const noexcept { return *registry_.store_; }

#pragma endregion Access

  private:

#pragma region Private Methods

    /** Clears what a landed commit carried and starts again at the stamp it published. */
    void restart_() noexcept {
        phase_ = phase_t::started_k;
        last_stage_stamp_ = last_try_to_commit_stamp_;
        last_commit_stamp_ = last_try_to_commit_stamp_;
        data_.clear();
        operation_pin_.release();
        start_stamp_ = registry_.request_start_(pin_);
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The registry that orders this transaction and checks its watches, outliving it. */
    registry_t& registry_;

    /** The writes and the watches this transaction carries. */
    state_t data_;

    /** The start stamp, pinned so the history this transaction reads at stays kept. */
    stamp_pin<atomic_reference_, waiter_type_> pin_;

    /** The stamp the last read scoped from itself answered at, pinned until the next such read
     *  or a restart. */
    stamp_pin<atomic_reference_, waiter_type_> operation_pin_;

    /** The stamp this transaction was started or restarted at. */
    transaction_stamp_t start_stamp_ = invalid_transaction_stamp_k;

    /** The stamp this transaction last staged at. */
    transaction_stamp_t last_stage_stamp_ = invalid_transaction_stamp_k;

    /** The stamp this transaction last committed at. */
    transaction_stamp_t last_commit_stamp_ = invalid_transaction_stamp_k;

    /** The stamp this transaction last tried to commit at. */
    transaction_stamp_t last_try_to_commit_stamp_ = invalid_transaction_stamp_k;

    /** Whether this transaction is filling its deltas or holding the ledger's leader word. */
    phase_t phase_ = phase_t::started_k;

#pragma endregion Private Members
};

#pragma endregion Transaction Core

} // namespace unum::ustore::blobs
