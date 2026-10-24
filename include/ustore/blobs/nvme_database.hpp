/**
 *  @file include/ustore/blobs/nvme_database.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Databases of named collections and single-collection databases, with their snapshots, their optimistic
 *      transactions, and the registries and read sets that order and check those transactions.
 *
 *  A database is a catalog and nothing else: it resolves a name to a collection, hands that collection
 *  a channel, and persists the mapping as one named blob of a volume so a reopen finds the same names.
 *  Every byte of data belongs to the collection below it, and every conflict belongs to the registry
 *  beside it; what is left is placement, naming and the order the two are changed in. One claim guards every
 *  call that changes the catalog, a tree or a blob, since flushes, compactions and catalog saves are serialized by
 *  it; commits land through the ledger's sequencer and take it only to pay for the maintenance they made due. A read
 *  takes no claim: a collection publishes immutable views a reader walks inside its ledger's epochs, the entries are
 *  pinned on the heap, and the catalog is one more immutable view published the same way and freed by whoever
 *  replaces it, once its readers left.
 *
 *  A single-collection database is the smallest thing that can be committed to: one collection, the
 *  registry that orders the commits landing in it, and two blobs on the volume - the schema a reopen
 *  checks itself against, and whatever bytes the owner keeps beside the data.
 *  Reads take the caller's channel, so any number of threads read at once; flushes, compactions and blob writes
 *  take the two channels the database was opened with, under its write path, which one thread holds at a time.
 *
 *  A snapshot copies no run and moves no byte: it pins the stamp the ledger last published, which every merge
 *  keeps the versions of, and reads through whatever view each collection publishes. Every commit ordered at or
 *  before that stamp is inside the snapshot and every later one is not.
 *
 *  Transactions are optimistic. Writes go to a write buffer of the transaction's own and reach the collections
 *  only at the commit, so a rollback costs nothing and a failed stage leaves the changes where a caller can still
 *  log them. A transaction pins the stamp the ledger last published, and every watch is scoped from that stamp or
 *  later, never from one a commit drew before landing it. There is no table of conflicts: every write lands in a
 *  collection under its commit's stamp, so a stage asks each collection whether a commit newer than the watch
 *  wrote the key, and the collection answers from its memory runs and from the runs that carry the key. A written
 *  key's watch lives beside its write, @c read_set holds the keys read or tracked, and the one exclusion is the
 *  ledger's leader word, which a staged transaction holds from @c stage until @c commit. Every collection a commit
 *  writes is sealed into a memory run before its stamp is drawn, so a commit lands whole or not at all.
 */

#pragma once

#include <cassert> // `assert`
#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::int32_t`, `std::uint16_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::memset`

#include <algorithm>   // `std::copy`, `std::min`
#include <array>       // `std::array`
#include <atomic>      // `std::atomic_thread_fence`, `std::memory_order`
#include <concepts>    // `std::same_as`
#include <limits>      // `std::numeric_limits`
#include <memory>      // `std::unique_ptr`
#include <new>         // `std::nothrow`, placement `new`
#include <span>        // `std::span`
#include <type_traits> // `std::remove_reference_t`
#include <utility>     // `std::exchange`, `std::forward`, `std::move`, `std::swap`
#include <variant>     // `std::variant`, `std::visit`, `std::get_if`

#include <smashtable/basic_hash_table.hpp> // `st::hash_map`, `st::assume_reserved`
#include <smashtable/basic_vector.hpp>     // `st::basic_vector`

#include "ustore/blobs/nvme_collection.hpp"   // `collection`, `collection_scanner`, `keyed_delta`
#include "ustore/blobs/nvme_key_slot.hpp"     // `key_slot`, `stored_value_size_t`, `collection_id_t`
#include "ustore/blobs/nvme_tree.hpp"         // `lsm_tree_run_record_t`, `database_config_t`
#include "ustore/blobs/nvme_write_buffer.hpp" // `write_buffer`, `write_buffer_status_t`
#include "ustore/utilities/nvme_channels.hpp" // `io_channel_t`, `basic_io_channel_pool`, `pooled_buffer_t`
#include "ustore/utilities/nvme_ring.hpp"     // `extent_t`, `uuid_t`, `constrained_allocator`
#include "ustore/utilities/nvme_volume.hpp"   // `basic_volume_router`, `resources_shared_ptr`
#include "ustore/utilities/types.hpp"         // `status_t`, `status_code_t`, `expected`, `byte_t`

namespace unum::ustore::blobs {

using utilities::basic_io_channel_pool;
using utilities::basic_volume_router;
using utilities::volume_id_t;

#pragma region Read Set

#pragma region Watches

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

/** Why a transaction watches a key it did not write, which a second watch of the same key widens; a written key's
 *  watch lives beside its write instead. */
enum class watch_purpose_t : std::uint8_t {

    /** The transaction read the key. */
    read_k = 0x01,

    /** The transaction tracks the key without reading or writing it. */
    track_k = 0x04,

    /** Every purpose, which a count with no filter asks for. */
    all_k = read_k | track_k,
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
 *  @brief The keys one transaction reads or tracks, each with the stamp it was first watched from and why.
 *
 *  Watching the same key twice keeps the earlier stamp and widens the purpose.
 */
template <typename stored_key_, template <typename> typename atomic_reference_, typename waiting_policy_type_>
class read_set {
  public:
    using stored_key_t = stored_key_;

    /** From which stamp a key was watched, and why. */
    struct watch_t {

        /** The earliest stamp any watch of this key was scoped from. */
        transaction_stamp_t watch_stamp;

        /** Every purpose the watches of this key were taken for. */
        watch_purpose_t purpose;
    };

    using stored_keys_t = st::hash_map<stored_key_t, watch_t, stored_key_hash_t, st::equal_to_t,
                                       constrained_allocator<std::byte, atomic_reference_>>;

    /** A set charging @p resources for the keys it holds. */
    explicit read_set(resources_shared_ptr<atomic_reference_, waiting_policy_type_> const& resources) noexcept
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
     *  @brief Watches @p stored_key from @p watch_stamp for @p purpose, in room @c reserve_more took.
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

#pragma endregion Watches

#pragma endregion Read Set

#pragma region Database Transaction State

#pragma region Grouped Write Buffer

/**
 *  @brief The uncommitted writes of one transaction, keyed by stored key inside each collection.
 *  @note Owned by one transaction and touched by its thread alone, so nothing here is atomic.
 */
template <typename stored_key_, template <typename> typename atomic_reference_, typename waiting_policy_type_>
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

    /** Where one written value sits in the insertions buffer, what the write does, and from when the transaction
     *  watches the key for another commit writing it. */
    struct stored_value_place_t {

        /** The first byte of the value, zero for a removal and for a value of no bytes. */
        stored_value_offset_t offset;

        /** The bytes of the value. */
        stored_value_size_t size;

        /** What the write does to the key. */
        delta_kind_t kind;

        /** The stamp a commit writing the key after conflicts, or @c invalid_transaction_stamp_k when unwatched. */
        transaction_stamp_t watch_stamp;
    };

    static_assert(sizeof(stored_value_place_t) == 24, "A write's kind takes the padding before its watch, not a word");

    /** One collection's share of the buffer. */
    struct group_t {
        using stored_values_t = st::hash_map<stored_key_t, stored_value_place_t, stored_key_hash_t, st::equal_to_t,
                                             constrained_allocator<std::byte, atomic_reference_>>;

        /** The value widths the collection admits, which every write is checked against. */
        group_config_t config;

        /** Where each written key's value sits. */
        stored_values_t stored_values;

        /** The bytes those values occupy. */
        std::size_t elements_size_bytes;
    };

    using groups_t = st::hash_map<group_id_t, group_t, collection_id_hash_t, st::equal_to_t,
                                  constrained_allocator<std::byte, atomic_reference_>>;

#pragma endregion Types

#pragma region Public Members

    /** One share of the buffer per written collection. */
    groups_t groups_by_id;

    /** The bytes every written value of every collection sits in. */
    st::basic_vector<std::byte, constrained_allocator<std::byte, atomic_reference_>> insertions_buffer;

    /** The first free byte of @c insertions_buffer. */
    stored_value_offset_in_buffer_t next_inserted_offset = 0;

#pragma endregion Public Members

#pragma region Construction

    inline grouped_write_buffer(std::size_t capacity_bytes, std::size_t elements_max_count,
                                resources_shared_ptr<atomic_reference_, waiting_policy_type_> resources) noexcept;

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
                       notifier<atomic_reference_, waiting_policy_type_> notifier) const noexcept;

    /** Writes @p stored_value under @p stored_key of @p id, watched from @p watch_stamp, which only lowers the stamp
     *  an earlier write of the key took. */
    template <typename config_provider_>
    inline status_t add(group_id_t id, config_provider_ config_provider, stored_key_t stored_key,
                        stored_value_view_t stored_value,
                        transaction_stamp_t watch_stamp = invalid_transaction_stamp_k) noexcept;

    /** Writes a removal of @p stored_key of @p id, watched from @p watch_stamp as @c add is. */
    template <typename config_provider_>
    inline status_t mark_as_removed(group_id_t id, config_provider_ config_provider, stored_key_t stored_key,
                                    transaction_stamp_t watch_stamp = invalid_transaction_stamp_k) noexcept;

    /** Writes @p change, a change record of @p fold, under @p stored_key of @p id, folded after whatever the buffer
     *  holds for the key, keeping that write's watch and taking none of its own. */
    template <typename config_provider_, utilities::fold_concept fold_type_>
    inline status_t add_change(group_id_t id, config_provider_ config_provider, stored_key_t stored_key,
                               stored_value_view_t change, fold_type_ const& fold) noexcept;

    /** Fills @p held with what the buffer holds for @p stored_key of @p id, its bytes viewing the buffer, answering
     *  whether it holds anything. */
    inline bool delta(group_id_t id, stored_key_t stored_key, delta_view_t& held) const noexcept;

    /** Moves every watched write's watch to @p watch_stamp, for a transaction started over with its writes kept. */
    inline void rescope_watches(transaction_stamp_t watch_stamp) noexcept;

    /** Hands @p callback every write as @c callback(id, stored_key, place), in no order. */
    template <typename callback_>
    inline void for_each_write(callback_&& callback) const noexcept;

#pragma endregion Interface Methods

#pragma region Batch Methods

    template <typename stored_value_location_>
    inline status_t find(group_id_t id, std::span<stored_key_t const> stored_keys,
                         std::span<stored_value_location_> locations,
                         missing_locations_t missing_locations = missing_locations_t::invalidate_k) const;
    template <typename stored_value_location_>
    inline void select(std::span<stored_value_location_ const> locations, bytes_span_t buffer,
                       notifier<atomic_reference_, waiting_policy_type_> notifier) const;

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
    resources_shared_ptr<atomic_reference_, waiting_policy_type_> resources_;

    /** The bytes @c insertions_buffer holds. */
    std::size_t capacity_bytes_ = 0;

    /** The keys every share together admits. */
    std::size_t elements_max_count_ = 0;

    /** The keys every share together holds. */
    std::size_t elements_count_ = 0;

#pragma endregion Private Members
};

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiting_policy_type_>
inline grouped_write_buffer<stored_key_, atomic_reference_, waiting_policy_type_>::grouped_write_buffer(
    std::size_t capacity_bytes, std::size_t elements_max_count,
    resources_shared_ptr<atomic_reference_, waiting_policy_type_> resources) noexcept
    : groups_by_id({}, {}, constrained_allocator<std::byte, atomic_reference_> {resources->memory_constraint}),
      insertions_buffer(constrained_allocator<std::byte, atomic_reference_> {resources->memory_constraint}),
      next_inserted_offset(0), resources_(resources), capacity_bytes_(capacity_bytes),
      elements_max_count_(elements_max_count), elements_count_(0) {
    // A budget that refuses the bytes leaves no capacity, and `can_fit` then refuses every write.
    if (st::failed(insertions_buffer.resize(capacity_bytes))) capacity_bytes_ = 0;
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiting_policy_type_>
inline grouped_write_buffer<stored_key_, atomic_reference_, waiting_policy_type_>::grouped_write_buffer(
    grouped_write_buffer&& other) noexcept
    : groups_by_id(std::move(other.groups_by_id)), insertions_buffer(std::move(other.insertions_buffer)),
      next_inserted_offset(other.next_inserted_offset), resources_(std::move(other.resources_)),
      capacity_bytes_(other.capacity_bytes_), elements_max_count_(other.elements_max_count_),
      elements_count_(other.elements_count_) {}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiting_policy_type_>
inline grouped_write_buffer<stored_key_, atomic_reference_, waiting_policy_type_>&
grouped_write_buffer<stored_key_, atomic_reference_, waiting_policy_type_>::operator=(
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

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiting_policy_type_>
template <typename callback_>
inline void grouped_write_buffer<stored_key_, atomic_reference_, waiting_policy_type_>::for_each_group(
    callback_&& callback) const noexcept {
    [[maybe_unused]] st::status_t const walked = groups_by_id.for_each([&](auto const& entry) noexcept {
        callback(group_info_t {entry.key(), entry.value().stored_values.size(), entry.value().elements_size_bytes});
    });
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiting_policy_type_>
inline write_buffer_status_t grouped_write_buffer<stored_key_, atomic_reference_, waiting_policy_type_>::status(
    group_id_t id, stored_key_t stored_key) const noexcept {

    auto result = write_buffer_status_t::mismatch_k;
    auto iterator = groups_by_id.find(id);
    if (iterator == groups_by_id.end()) return result;

    iterator.value().stored_values.probe_to_find(stored_key, [&result](auto const& entry) noexcept {
        constexpr write_buffer_status_t results[2] = {
            write_buffer_status_t::found_k,
            write_buffer_status_t::removed_k,
        };
        result = results[entry.value().kind == delta_kind_t::remove_k];
    });

    return result;
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiting_policy_type_>
inline status_t grouped_write_buffer<stored_key_, atomic_reference_, waiting_policy_type_>::stored_value_size(
    group_id_t id, stored_key_t stored_key, stored_value_size_t& size) const noexcept {

    auto status = status_t {status_code_t::not_found_k, "Object not exists"};
    auto iterator = groups_by_id.find(id);
    if (iterator == groups_by_id.end()) return status;

    iterator.value().stored_values.probe_to_find(stored_key, [&](auto const& entry) noexcept {
        if (entry.value().kind != delta_kind_t::remove_k) size = entry.value().size;
        status = status_t {};
    });

    return status;
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiting_policy_type_>
inline status_t grouped_write_buffer<stored_key_, atomic_reference_, waiting_policy_type_>::select(
    group_id_t id, stored_key_t stored_key, stored_value_span_t stored_value,
    stored_value_size_t& size) const noexcept {

    status_t status = status_t {status_code_t::not_found_k, "Object not exists"};
    auto iterator = groups_by_id.find(id);
    if (iterator == groups_by_id.end()) return status;

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

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiting_policy_type_>
template <typename stored_value_location_>
inline write_buffer_status_t grouped_write_buffer<stored_key_, atomic_reference_, waiting_policy_type_>::find(
    group_id_t id, stored_key_t stored_key, stored_value_location_& location) const noexcept {

    location.size_bytes = 0;
    location.offset = 0;
    location.kind = delta_kind_t::remove_k;
    location.recipient_id = reinterpret_cast<std::uint64_t>(this);
    location.recipient_state_id = 0;

    auto result = write_buffer_status_t::mismatch_k;
    auto iterator = groups_by_id.find(id);
    if (iterator == groups_by_id.end()) return result;

    iterator.value().stored_values.probe_to_find(stored_key, [&](auto const& entry) noexcept {
        auto const& stored_value_place = entry.value();
        location.offset = stored_value_place.offset;
        location.size_bytes = stored_value_place.size;
        location.kind = stored_value_place.kind;
        constexpr write_buffer_status_t results[2] = {write_buffer_status_t::found_k, write_buffer_status_t::removed_k};
        result = results[location.kind == delta_kind_t::remove_k];
    });

    return result;
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiting_policy_type_>
template <typename stored_value_location_>
inline void grouped_write_buffer<stored_key_, atomic_reference_, waiting_policy_type_>::select(
    stored_value_location_ const& location, stored_value_span_t stored_value,
    notifier<atomic_reference_, waiting_policy_type_> notifier) const noexcept {

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

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiting_policy_type_>
template <typename config_provider_>
inline status_t grouped_write_buffer<stored_key_, atomic_reference_, waiting_policy_type_>::reach_group_(
    group_id_t id, config_provider_ config_provider, group_t*& group) noexcept {

    auto iterator = groups_by_id.find(id);
    if (iterator == groups_by_id.end()) {
        group_config_t config {};
        auto status = config_provider(id, config);
        if (status.failed()) return status;
        if ((config.value_size > 0) & (config.value_max_size > 0))
            return status_t {status_code_t::invalid_argument_k, "Invalid config"};

        group_t fresh {config,
                       {{}, {}, constrained_allocator<std::byte, atomic_reference_> {resources_->memory_constraint}},
                       0};
        if (groups_by_id.reserve_more(1) == groups_t::reserve_result_t::failed_k)
            return status_t {status_code_t::out_of_memory_k, "No room for another collection"};
        groups_by_id.emplace(id, std::move(fresh), st::assume_reserved);
        iterator = groups_by_id.find(id);
        if (iterator == groups_by_id.end())
            return status_t {status_code_t::out_of_memory_k, "No room for another collection"};
    }
    group = &iterator.value_ref();
    return status_t {};
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiting_policy_type_>
template <typename config_provider_>
inline status_t grouped_write_buffer<stored_key_, atomic_reference_, waiting_policy_type_>::add(
    group_id_t id, config_provider_ config_provider, stored_key_t stored_key, stored_value_view_t stored_value,
    transaction_stamp_t watch_stamp) noexcept {

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
        group->elements_size_bytes += stored_value.size();
        ++elements_count_;
    };
    auto update = [&](auto& equal_slot) noexcept {
        stored_value_place_t& held = equal_slot.value_ref();
        group->elements_size_bytes -= held.size;
        transaction_stamp_t const earlier = std::min(held.watch_stamp, watch_stamp);
        held = place;
        held.watch_stamp = earlier;
        group->elements_size_bytes += stored_value.size();
    };
    [[maybe_unused]] auto const stored = group->stored_values.probe_to_upsert(stored_key, insert, update,
                                                                              st::assume_reserved);

    return status_t {};
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiting_policy_type_>
template <typename config_provider_>
inline status_t grouped_write_buffer<stored_key_, atomic_reference_, waiting_policy_type_>::mark_as_removed(
    group_id_t id, config_provider_ config_provider, stored_key_t stored_key,
    transaction_stamp_t watch_stamp) noexcept {

    group_t* group = nullptr;
    auto status = reach_group_(id, config_provider, group);
    if (status.failed()) return status;

    if (!can_fit(1, 0)) [[unlikely]]
        return status_t {status_code_t::capacity_exhausted_k, "Cache is full"};

    if (group->stored_values.reserve_more(1) == group_t::stored_values_t::reserve_result_t::failed_k)
        return status_t {status_code_t::out_of_memory_k, "No room for another key"};

    stored_value_place_t const place {0, 0, delta_kind_t::remove_k, watch_stamp};
    auto insert = [&](auto& unused_slot) noexcept {
        new (&unused_slot.key_ref()) stored_key_t {stored_key};
        new (&unused_slot.value_ref()) stored_value_place_t {place};
        ++elements_count_;
    };
    auto update = [&](auto& equal_slot) noexcept {
        stored_value_place_t& held = equal_slot.value_ref();
        group->elements_size_bytes -= held.size;
        transaction_stamp_t const earlier = std::min(held.watch_stamp, watch_stamp);
        held = place;
        held.watch_stamp = earlier;
    };
    [[maybe_unused]] auto const stored = group->stored_values.probe_to_upsert(stored_key, insert, update,
                                                                              st::assume_reserved);

    return status_t {};
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiting_policy_type_>
template <typename config_provider_, utilities::fold_concept fold_type_>
inline status_t grouped_write_buffer<stored_key_, atomic_reference_, waiting_policy_type_>::add_change(
    group_id_t id, config_provider_ config_provider, stored_key_t stored_key, stored_value_view_t change,
    fold_type_ const& fold) noexcept {

    group_t* group = nullptr;
    auto status = reach_group_(id, config_provider, group);
    if (status.failed()) return status;
    if (stored_key == limit<stored_key_t>::max()) return status_t {status_code_t::malformed_k, "Invalid stored key"};
    if (group->stored_values.reserve_more(1) == group_t::stored_values_t::reserve_result_t::failed_k)
        return status_t {status_code_t::out_of_memory_k, "No room for another key"};

    // A change composes with a buffered change and applies to a buffered write or removal, so one record per key
    // reaches the seal, and a lone one is composed too, so every buffered chain reads in the fold's own order
    utilities::local_arena_t scratch;
    utilities::arena_allocator<byte_t> arena = scratch.allocator();
    delta_view_t const newer {delta_kind_t::change_k, change};
    delta_view_t older;
    bool const held = delta(id, stored_key, older);
    delta_view_t folded {delta_kind_t::change_k, {}};
    if (held) {
        auto [paired, pair_status] = utilities::fold_pair(fold, newer, older, arena);
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
        group->elements_size_bytes += place.size;
        ++elements_count_;
    };
    auto update = [&](auto& equal_slot) noexcept {
        stored_value_place_t& kept = equal_slot.value_ref();
        group->elements_size_bytes -= kept.size;
        transaction_stamp_t const watched = kept.watch_stamp;
        kept = place;
        kept.watch_stamp = watched;
        group->elements_size_bytes += place.size;
    };
    [[maybe_unused]] auto const stored = group->stored_values.probe_to_upsert(stored_key, insert, update,
                                                                              st::assume_reserved);
    return status_t {};
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiting_policy_type_>
inline bool grouped_write_buffer<stored_key_, atomic_reference_, waiting_policy_type_>::delta(
    group_id_t id, stored_key_t stored_key, delta_view_t& held) const noexcept {
    auto iterator = groups_by_id.find(id);
    if (iterator == groups_by_id.end()) return false;
    bool found = false;
    iterator.value().stored_values.probe_to_find(stored_key, [&](auto const& entry) noexcept {
        stored_value_place_t const& place = entry.value();
        held = delta_view_t {place.kind, {}};
        if (place.size != 0) held.bytes = as_bytes_view(insertions_buffer).subspan(place.offset, place.size);
        found = true;
    });
    return found;
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiting_policy_type_>
inline void grouped_write_buffer<stored_key_, atomic_reference_, waiting_policy_type_>::rescope_watches(
    transaction_stamp_t watch_stamp) noexcept {
    [[maybe_unused]] st::status_t const walked = groups_by_id.for_each([&](auto& group) noexcept {
        [[maybe_unused]] st::status_t const rescoped = group.value_ref().stored_values.for_each(
            [&](auto& entry) noexcept {
                stored_value_place_t& place = entry.value_ref();
                if (place.watch_stamp != invalid_transaction_stamp_k) place.watch_stamp = watch_stamp;
            });
    });
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiting_policy_type_>
template <typename callback_>
inline void grouped_write_buffer<stored_key_, atomic_reference_, waiting_policy_type_>::for_each_write(
    callback_&& callback) const noexcept {
    [[maybe_unused]] st::status_t const walked = groups_by_id.for_each([&](auto const& group) noexcept {
        [[maybe_unused]] st::status_t const listed = group.value().stored_values.for_each(
            [&](auto const& entry) noexcept { callback(group.key(), entry.key(), entry.value()); });
    });
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiting_policy_type_>
template <typename stored_value_location_>
inline status_t grouped_write_buffer<stored_key_, atomic_reference_, waiting_policy_type_>::find(
    group_id_t id, std::span<stored_key_t const> stored_keys, std::span<stored_value_location_> locations,
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

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiting_policy_type_>
template <typename stored_value_location_>
inline void grouped_write_buffer<stored_key_, atomic_reference_, waiting_policy_type_>::select(
    std::span<stored_value_location_ const> locations, bytes_span_t buffer,
    notifier<atomic_reference_, waiting_policy_type_> notifier) const {

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

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiting_policy_type_>
inline status_t grouped_write_buffer<stored_key_, atomic_reference_, waiting_policy_type_>::check_stored_value(
    group_config_t const& config, stored_key_t stored_key, stored_value_size_t stored_value_size) const noexcept {

    bool valid_key = stored_key != limit<stored_key_t>::max();
    bool valid_fixed_width_stored_value = (config.value_size > 0) & (stored_value_size == config.value_size);
    bool valid_variable_width_stored_value = stored_value_size <= config.value_max_size;
    bool valid = valid_key & (valid_fixed_width_stored_value ^ valid_variable_width_stored_value);
    return valid ? status_t {} : status_t {status_code_t::malformed_k, "Invalid stored value"};
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiting_policy_type_>
inline bool grouped_write_buffer<stored_key_, atomic_reference_, waiting_policy_type_>::can_fit(
    std::size_t stored_values_count, std::size_t stored_values_sum_size) const noexcept {

    auto used_memory = next_inserted_offset;
    bool has_element_place = elements_count_ + stored_values_count <= elements_max_count_;
    // A place names its offset in 32 bits, which every value of the buffer stays within.
    bool has_enough_space = (used_memory + stored_values_sum_size <= capacity_bytes_) &
                            (used_memory + stored_values_sum_size <= std::numeric_limits<stored_value_offset_t>::max());
    return has_element_place & has_enough_space;
}

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiting_policy_type_>
inline void grouped_write_buffer<stored_key_, atomic_reference_, waiting_policy_type_>::clear() noexcept {
    groups_by_id.clear();
    next_inserted_offset = 0;
    elements_count_ = 0;
}

#pragma endregion Grouped Write Buffer

#pragma region Grouped Read Set

/** The keys one transaction watches, kept apart per collection so two collections never collide. */
template <typename stored_key_, template <typename> typename atomic_reference_, typename waiting_policy_type_>
class grouped_read_set {
  public:
    using stored_key_t = stored_key_;
    using group_id_t = collection_id_t;
    using read_set_t = read_set<stored_key_t, atomic_reference_, waiting_policy_type_>;
    using groups_t = st::hash_map<group_id_t, read_set_t, collection_id_hash_t, st::equal_to_t,
                                  constrained_allocator<std::byte, atomic_reference_>>;

    /** A set charging @p resources for the keys of every collection it watches. */
    explicit grouped_read_set(resources_shared_ptr<atomic_reference_, waiting_policy_type_> resources) noexcept
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
            groups.emplace(id, read_set_t {resources}, st::assume_reserved);
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
        iterator.value_ref().add(stored_key, watch_stamp, purpose, st::assume_reserved);
    }

    /** Drops every watch of every collection, for a transaction starting over. */
    void clear() noexcept { groups.clear(); }

    /** The budget every watched collection's keys are charged to. */
    resources_shared_ptr<atomic_reference_, waiting_policy_type_> resources;

    /** One watched-key set per collection the transaction touched. */
    groups_t groups;
};

#pragma endregion Grouped Read Set

#pragma region Transaction State

/**
 *  @brief What one transaction writes and watches in collections whose store keys them otherwise than the database,
 *      kept by that store's own code and joined to the transaction, which lands it with its own writes.
 *
 *  Its owner seals it on the committing thread before the transaction stages or commits. The thread holding the
 *  ledger's leader word then prepares it beside the transaction's own collections, and publishes it under the drawn
 *  stamp, or forgets what it prepared for a commit it refuses and for a stage let go.
 */
struct transaction_share_t {

    /** The share joined before this one, or null. */
    transaction_share_t* next = nullptr;

    /** What every step is handed, owned by whoever joined the share. */
    void* context = nullptr;

    /** Refuses the commit with @c write_conflict_k for a watch a commit since violated, or lays out every view the
     *  share publishes, leaving none laid out when it refuses. */
    status_t (*prepare)(void* context) noexcept = nullptr;

    /** Publishes what @c prepare laid out under @p stamp. */
    void (*publish)(void* context, transaction_stamp_t stamp) noexcept = nullptr;

    /** Drops what @c prepare laid out. */
    void (*forget)(void* context) noexcept = nullptr;
};

/** Everything one database transaction carries between its start and its commit. */
template <typename stored_key_, template <typename> typename atomic_reference_, typename waiting_policy_type_>
struct database_transaction_state {
    using stored_key_t = stored_key_;
    using write_buffer_t = grouped_write_buffer<stored_key_t, atomic_reference_, waiting_policy_type_>;
    using read_set_t = grouped_read_set<stored_key_t, atomic_reference_, waiting_policy_type_>;

    /** One range of keys a transaction walked, which any commit landing inside it since @c watch_stamp conflicts
     *  with, the keys it never found included. */
    struct range_watch_t {
        collection_id_t collection = collection_invalid_id_k;
        stored_key_t lower = stored_key_t {};
        stored_key_t upper = stored_key_t {};
        transaction_stamp_t watch_stamp = invalid_transaction_stamp_k;
    };

    /** The ranges a transaction walked, in the order it walked them. */
    using range_watches_t = st::basic_vector<range_watch_t, constrained_allocator<range_watch_t, atomic_reference_>>;

    /** One written collection's records, sealed by the committer and landed by the leader. */
    struct sealed_collection_t {

        /** The collection written, which the leader looks up again. */
        collection_id_t id = collection_invalid_id_k;

        /** Its records and the run they become. */
        sealed_commit<stored_key_t, atomic_reference_> commit;
    };

    /** Every written collection's sealed records, between a stage or a commit and its landing. */
    using sealed_collections_t =
        st::basic_vector<sealed_collection_t, constrained_allocator<sealed_collection_t, atomic_reference_>>;

    inline database_transaction_state(write_buffer_t&& write_buffer,
                                      resources_shared_ptr<atomic_reference_, waiting_policy_type_> resources) noexcept
        : changes(std::move(write_buffer)), read_set(resources),
          ranges(constrained_allocator<range_watch_t, atomic_reference_> {resources->memory_constraint}),
          sealed(constrained_allocator<sealed_collection_t, atomic_reference_> {resources->memory_constraint}) {}

    /** Whether the transaction has neither written nor watched anything. */
    inline bool empty() const noexcept { return changes.empty() && read_set.empty() && ranges.empty(); }

    /** Drops both the writes and the watches, for a rollback or a renewal. */
    inline void clear() noexcept {
        changes.clear();
        read_set.clear();
        ranges.clear();
        sealed.clear();
    }

    /** The writes the transaction has not committed yet. */
    write_buffer_t changes;

    /** The keys the transaction watches for conflicts. */
    read_set_t read_set;

    /** The ranges the transaction watches for conflicts, phantoms included. */
    range_watches_t ranges;

    /** The writes sealed per collection, filled by a stage or a commit and emptied once it lands or is refused. */
    sealed_collections_t sealed;

    /** The shares joined to the transaction, newest first, which outlive it and which nothing clears. */
    transaction_share_t* shares = nullptr;
};

#pragma endregion Transaction State

#pragma endregion Database Transaction State

#pragma region Database Transaction Registry

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
class database_transaction;

#pragma region Store Interface

/**
 *  @brief What a registry needs from the store beneath it to stage and to commit.
 *
 *  A committer seals each written collection on its own thread, making room first when the collection's memory runs
 *  are past their bounds. The thread holding the ledger's leader word then prepares each one, publishes it under the
 *  drawn stamp, or forgets what it prepared for a commit it refuses. Once its commit lands, the committer maintains
 *  every collection it wrote. The conflict checks read published views. The
 *  read accessors a transaction reaches the store through forward from members that instantiate when a caller uses
 *  them, so a store serving reads of one shape alone still satisfies this.
 */
template <typename store_type_, typename stored_key_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
concept transaction_store_concept = requires(
    store_type_& store, collection_id_t group_id, stored_key_type_ stored_key,
    grouped_write_buffer<stored_key_type_, atomic_reference_, waiting_policy_type_> const& changes,
    sealed_commit<stored_key_type_, atomic_reference_>& sealed, transaction_stamp_t stamp) {
    { store.ledger() } -> std::same_as<commit_ledger<atomic_reference_, waiting_policy_type_>&>;
    { store.written_since(group_id, stored_key, stamp) } -> std::same_as<bool>;
    { store.range_written_since(group_id, stored_key, stored_key, stamp) } -> std::same_as<bool>;
    { store.seal_collection(group_id, changes, sealed) } -> std::same_as<status_t>;
    { store.prepare_collection(group_id, sealed) } -> std::same_as<status_t>;
    { store.publish_collection(group_id, sealed, stamp) } -> std::same_as<void>;
    { store.forget_collection(group_id) } -> std::same_as<void>;
    { store.maintain(group_id) } -> std::same_as<status_t>;
};

#pragma endregion Store Interface

/**
 *  @brief The transactions of one database, the stamps they commit under and the conflicts that stop them.
 *  @tparam stored_key_ The key every collection of the store is addressed by.
 *  @tparam store_type_ The store the commits land in, which must satisfy @c transaction_store_concept.
 *
 *  Nothing here is shared state of its own: the stamps come from the store's ledger, and a conflict is a key
 *  the store says was written since the watch, read from the memory runs and the runs that carry the key.
 */
template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
class database_transaction_registry {
  public:
#pragma region Types

    using stored_key_t = stored_key_;
    using store_t = store_type_;
    using transaction_t = database_transaction<stored_key_, store_type_, atomic_reference_, waiting_policy_type_>;
    using transaction_state_t = database_transaction_state<stored_key_t, atomic_reference_, waiting_policy_type_>;
    using write_buffer_t = typename transaction_state_t::write_buffer_t;
    using group_info_t = typename write_buffer_t::group_info_t;
    using transaction_unique_ptr_t = std::unique_ptr<transaction_t>;
    using return_transaction_t = expected<transaction_unique_ptr_t>;

#pragma endregion Types

#pragma region Construction

    /** A registry committing into @p store, which outlives it. */
    inline database_transaction_registry(
        store_t& store, resources_shared_ptr<atomic_reference_, waiting_policy_type_> resources) noexcept
        : store_(&store), resources_(std::move(resources)) {}

    inline database_transaction_registry(database_transaction_registry&&) = delete;
    inline database_transaction_registry(database_transaction_registry const&) = delete;
    inline database_transaction_registry& operator=(database_transaction_registry const&) = delete;
    inline database_transaction_registry& operator=(database_transaction_registry&&) = delete;

#pragma endregion Construction

#pragma region Interface Methods

    /** A transaction over every collection of the store, with the write-buffer bounds of @p config. */
    inline return_transaction_t create_transaction(transaction_config_t const& config) noexcept;

#pragma endregion Interface Methods

  private:
#pragma region Transactions

    friend store_t;
    friend transaction_t;

    /** One transaction's commit, as the sequencer hands it to its two steps. */
    struct commit_t {

        /** The registry committing. */
        database_transaction_registry* owner = nullptr;

        /** The transaction's writes, watches and sealed records. */
        transaction_state_t* data = nullptr;

        /** The stamp every watch of the transaction is scoped from or past. */
        transaction_stamp_t start_stamp = invalid_transaction_stamp_k;
    };

    inline status_t stage_(transaction_state_t& data, transaction_stamp_t start_stamp,
                           transaction_stamp_t& stage_stamp) noexcept;
    inline transaction_stamp_t commit_staged_(transaction_state_t& data) noexcept;
    inline expected<transaction_stamp_t> commit_(transaction_state_t& data, transaction_stamp_t start_stamp) noexcept;
    inline void discard_(transaction_state_t& data) noexcept;

    /** Pins @p pin at the newest published stamp, which a transaction starts at and reads from. */
    inline transaction_stamp_t request_start_(stamp_pin<atomic_reference_, waiting_policy_type_>& pin) noexcept {
        return store_->ledger().pin_reader(pin);
    }

    /** The stamp a watch taken now is scoped from: every commit at or below it has landed. */
    inline transaction_stamp_t current_stamp_() const noexcept { return store_->ledger().published(); }

    inline status_t watch_now_(transaction_stamp_t start_stamp, collection_id_t group_id,
                               std::span<stored_key_t const> stored_keys) noexcept;

#pragma endregion Transactions

#pragma region Helper Methods

    inline bool conflicting_(transaction_state_t const& data) noexcept;
    inline status_t seal_(transaction_state_t& data) noexcept;
    inline status_t prepare_(transaction_state_t& data, transaction_stamp_t start_stamp) noexcept;
    inline void forget_(transaction_state_t& data, std::size_t prepared) noexcept;
    inline void maintain_(transaction_state_t& data) noexcept;
    static inline status_t prepare_commit_(void* context) noexcept;
    static inline void publish_commit_(void* context, transaction_stamp_t stamp) noexcept;

#pragma endregion Helper Methods

#pragma region Store Accessor Methods

    inline bool has_collection_(auto const& id) noexcept { return store_->has_collection(id); }
    inline auto list_collections_() noexcept { return store_->list_collections(); }
    inline bool contains_(auto const& id, stored_key_t stored_key, transaction_stamp_t stamp,
                          std::size_t thread_index) noexcept {
        return store_->contains(id, stored_key, stamp, thread_index);
    }
    inline status_t stored_value_size_(auto const& id, stored_key_t stored_key, stored_value_size_t& size,
                                       transaction_stamp_t stamp, std::size_t thread_index) noexcept {
        return store_->stored_value_size(id, stored_key, size, stamp, thread_index);
    }
    inline stored_value_size_t buffer_size_for_any_stored_value_(auto const& id) noexcept {
        return store_->buffer_size_for_any_stored_value(id);
    }
    inline status_t select_(auto const& id, stored_key_t stored_key, stored_value_span_t stored_value,
                            stored_value_size_t& size, transaction_stamp_t stamp, std::size_t thread_index) noexcept {
        return store_->select(id, stored_key, stored_value, size, stamp, thread_index);
    }

    /** Whether a commit since @p stamp landed in @p id from @p lower to @p upper, checked now against the published
     *  view. */
    [[nodiscard]] inline bool range_written_now_(collection_id_t id, stored_key_t lower, stored_key_t upper,
                                                 transaction_stamp_t stamp) noexcept {
        return store_->range_written_since(id, lower, upper, stamp);
    }

#pragma endregion Store Accessor Methods

#pragma region Private Members

    /** The store every commit of this registry lands in, owned by whoever named it. */
    store_t* store_;

    /** The budget every transaction's writes and watches are charged to. */
    resources_shared_ptr<atomic_reference_, waiting_policy_type_> resources_;

#pragma endregion Private Members
};

#pragma region Interface Methods

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
inline typename database_transaction_registry<stored_key_, store_type_, atomic_reference_,
                                              waiting_policy_type_>::return_transaction_t
database_transaction_registry<stored_key_, store_type_, atomic_reference_, waiting_policy_type_>::create_transaction(
    transaction_config_t const& config) noexcept {

    write_buffer_t changes(config.write_buffer_max_bytes, config.write_buffer_max_elements, resources_);
    transaction_unique_ptr_t transaction {new (std::nothrow) transaction_t {*this, std::move(changes), resources_}};
    if (!transaction) return status_t {status_code_t::out_of_memory_k, "No room for another transaction"};
    return transaction;
}

#pragma endregion Interface Methods

#pragma region Transactions

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
inline status_t database_transaction_registry<stored_key_, store_type_, atomic_reference_,
                                              waiting_policy_type_>::stage_(transaction_state_t& data,
                                                                            transaction_stamp_t start_stamp,
                                                                            transaction_stamp_t& stage_stamp) noexcept {

    // Sealed before the leader word is taken, so the sort, the copy and any flush hold up no other committer.
    if (status_t const sealed = seal_(data); sealed.failed()) return sealed;
    store_->ledger().lead();
    stage_stamp = current_stamp_();
    if (status_t const prepared = prepare_(data, start_stamp); prepared.failed()) {
        store_->ledger().stop_leading();
        data.sealed.clear();
        return prepared;
    }
    return status_t {};
}

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
inline transaction_stamp_t
database_transaction_registry<stored_key_, store_type_, atomic_reference_, waiting_policy_type_>::commit_staged_(
    transaction_state_t& data) noexcept {

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

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
inline expected<transaction_stamp_t>
database_transaction_registry<stored_key_, store_type_, atomic_reference_, waiting_policy_type_>::commit_(
    transaction_state_t& data, transaction_stamp_t start_stamp) noexcept {

    if (status_t const sealed = seal_(data); sealed.failed()) return sealed;
    // Even a commit that wrote nothing draws a stamp, so every commit has a place of its own in the order.
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

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
inline void database_transaction_registry<stored_key_, store_type_, atomic_reference_, waiting_policy_type_>::discard_(
    transaction_state_t& data) noexcept {
    forget_(data, data.sealed.size());
    for (transaction_share_t* share = data.shares; share != nullptr; share = share->next) share->forget(share->context);
    store_->ledger().stop_leading();
    data.sealed.clear();
}

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
inline status_t
database_transaction_registry<stored_key_, store_type_, atomic_reference_, waiting_policy_type_>::watch_now_(
    transaction_stamp_t start_stamp, collection_id_t group_id, std::span<stored_key_t const> stored_keys) noexcept {

    bool written = false;
    for (std::size_t index = 0; index != stored_keys.size() && !written; ++index)
        written = store_->written_since(group_id, stored_keys[index], start_stamp);
    if (written) return status_t {status_code_t::write_conflict_k, "Keys conflict"};
    return status_t {};
}

#pragma endregion Transactions

#pragma region Helper Methods

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
inline bool
database_transaction_registry<stored_key_, store_type_, atomic_reference_, waiting_policy_type_>::conflicting_(
    transaction_state_t const& data) noexcept {

    // First-committer-wins over the watched writes, then every key read or tracked.
    bool conflict = false;
    data.changes.for_each_write(
        [&](collection_id_t group_id, stored_key_t const& stored_key, auto const& place) noexcept {
            if (!conflict && place.watch_stamp != invalid_transaction_stamp_k)
                conflict = store_->written_since(group_id, stored_key, place.watch_stamp);
        });
    if (conflict) return true;
    [[maybe_unused]] st::status_t const walked = data.read_set.groups.for_each(
        [&](auto const& group) noexcept -> st::walk_control_t {
            collection_id_t const group_id = group.key();
            [[maybe_unused]] st::status_t const watched = group.value().stored_keys.for_each(
                [&](auto const& watch) noexcept -> st::walk_control_t {
                    conflict = store_->written_since(group_id, watch.key(), watch.value().watch_stamp);
                    return conflict ? st::walk_control_t::halt_k : st::walk_control_t::resume_k;
                });
            return conflict ? st::walk_control_t::halt_k : st::walk_control_t::resume_k;
        });
    if (conflict) return true;
    for (auto const& range : data.ranges)
        if (store_->range_written_since(range.collection, range.lower, range.upper, range.watch_stamp)) return true;
    return false;
}

/** Seals every written collection's records on the committing thread, one sealed entry per collection. */
template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
inline status_t database_transaction_registry<stored_key_, store_type_, atomic_reference_, waiting_policy_type_>::seal_(
    transaction_state_t& data) noexcept {
    data.sealed.clear();
    if (data.sealed.reserve(data.changes.groups_by_id.size()) != st::status_t::success_k)
        return status_t {status_code_t::out_of_memory_k, "No room to seal a commit"};
    status_t status = status_t {};
    data.changes.for_each_group([&](group_info_t const& group) noexcept {
        if (status.failed()) return;
        typename transaction_state_t::sealed_collection_t sealed;
        sealed.id = group.id;
        status = store_->seal_collection(group.id, data.changes, sealed.commit);
        if (status.succeeded()) [[maybe_unused]]
            st::status_t const kept = data.sealed.push_back(st::assume_reserved, std::move(sealed));
    });
    if (status.failed()) data.sealed.clear();
    return status;
}

/** Checks the watches against the views published so far and lays out every written collection's view, on the
 *  thread holding the leader word; a refusal forgets every view it laid out. */
template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
inline status_t
database_transaction_registry<stored_key_, store_type_, atomic_reference_, waiting_policy_type_>::prepare_(
    transaction_state_t& data, transaction_stamp_t start_stamp) noexcept {

    // Every watch is scoped from the start or later, so nothing published since the start leaves nothing to check.
    if (current_stamp_() != start_stamp && conflicting_(data))
        return status_t {status_code_t::write_conflict_k, "Keys conflict"};
    for (std::size_t index = 0; index != data.sealed.size(); ++index)
        if (status_t const prepared = store_->prepare_collection(data.sealed[index].id, data.sealed[index].commit);
            prepared.failed()) {
            forget_(data, index);
            return prepared;
        }
    // A share checks its own watches, as only its store reads its keys, and forgets nothing once it refuses.
    for (transaction_share_t* share = data.shares; share != nullptr; share = share->next)
        if (status_t const prepared = share->prepare(share->context); prepared.failed()) {
            forget_(data, data.sealed.size());
            for (transaction_share_t* undone = data.shares; undone != share; undone = undone->next)
                undone->forget(undone->context);
            return prepared;
        }
    return status_t {};
}

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
inline void database_transaction_registry<stored_key_, store_type_, atomic_reference_, waiting_policy_type_>::forget_(
    transaction_state_t& data, std::size_t prepared) noexcept {
    for (std::size_t index = 0; index != prepared; ++index) store_->forget_collection(data.sealed[index].id);
}

/** Merges, flushes and compacts whatever the landed commit left due in the collections it wrote; a failure there
 *  leaves the commit landed, and the next commit making room meets it again. */
template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
inline void database_transaction_registry<stored_key_, store_type_, atomic_reference_, waiting_policy_type_>::maintain_(
    transaction_state_t& data) noexcept {
    for (auto const& sealed : data.sealed) [[maybe_unused]]
        status_t const maintained = store_->maintain(sealed.id);
}

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
inline status_t database_transaction_registry<stored_key_, store_type_, atomic_reference_,
                                              waiting_policy_type_>::prepare_commit_(void* context) noexcept {
    commit_t const& committing = *static_cast<commit_t const*>(context);
    return committing.owner->prepare_(*committing.data, committing.start_stamp);
}

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
inline void database_transaction_registry<stored_key_, store_type_, atomic_reference_,
                                          waiting_policy_type_>::publish_commit_(void* context,
                                                                                 transaction_stamp_t stamp) noexcept {
    commit_t const& committing = *static_cast<commit_t const*>(context);
    for (auto& sealed : committing.data->sealed)
        committing.owner->store_->publish_collection(sealed.id, sealed.commit, stamp);
    for (transaction_share_t* share = committing.data->shares; share != nullptr; share = share->next)
        share->publish(share->context, stamp);
}

#pragma endregion Helper Methods

#pragma endregion Database Transaction Registry

#pragma region Database Snapshot

#pragma region Snapshot

/**
 *  @brief One moment of a database: a stamp pinned in its ledger, which every read through the snapshot answers at.
 *  @tparam store_type_ The database read, which must outlive the snapshot.
 *
 *  A snapshot holds no view and no level set, so it never delays a publication: every merge keeps the versions a
 *  pinned stamp reads, and every read re-enters the collection it names through the view published then.
 */
template <typename store_type_>
class database_snapshot {
  public:

    /** The database read. */
    using store_t = store_type_;

    /** The key every collection of the database is addressed by. */
    using stored_key_t = typename store_t::stored_key_t;

    /** A collection named by its ID or by its name, which is how every call addresses one. */
    using any_collection_id_t = typename store_t::any_collection_id_t;

    database_snapshot() noexcept = default;
    database_snapshot(database_snapshot const&) = delete;
    database_snapshot& operator=(database_snapshot const&) = delete;
    database_snapshot(database_snapshot&&) noexcept = default;
    database_snapshot& operator=(database_snapshot&&) noexcept = default;

    /** The newest stamp @p store published, pinned in its ledger until the snapshot dies. */
    [[nodiscard]] static database_snapshot of(store_t& store) noexcept {
        database_snapshot taken;
        taken.store_ = &store;
        taken.stamp_ = store.ledger().pin_reader(taken.pin_);
        return taken;
    }

    /** The stamp every read through this snapshot answers at. */
    [[nodiscard]] transaction_stamp_t stamp() const noexcept { return stamp_; }

    /** Whether @p id names a collection of the database now. */
    [[nodiscard]] bool contains(any_collection_id_t const& id) noexcept { return store_->has_collection(id); }

    /** What @p id held for @p stored_key at the snapshot's stamp, and how many bytes its value took. */
    [[nodiscard]] select_result_t find(any_collection_id_t const& id, stored_key_t stored_key,
                                       stored_value_size_t& size) noexcept {
        return store_->find(id, stored_key, size, stamp_);
    }

    /** Copies the value @p id held for @p stored_key at the snapshot's stamp into @p into, filing it in the value
     *  cache when it paid I/O for it. */
    status_t select(any_collection_id_t const& id, stored_key_t stored_key, std::span<byte_t> into,
                    stored_value_size_t& size) noexcept {
        return store_->select(id, stored_key, into, size, stamp_);
    }

  private:

    /** The database read, borrowed and outliving the snapshot. */
    store_t* store_ = nullptr;

    /** The pin that keeps every version at the stamp. */
    typename store_t::pin_t pin_;

    /** The stamp pinned. */
    transaction_stamp_t stamp_ = unwritten_stamp_k;
};

#pragma endregion Snapshot

#pragma endregion Database Snapshot

#pragma region Database Transaction

#pragma region Transaction

/**
 *  @brief A group of writes and reads over several collections that succeed or fail together.
 *  @tparam stored_key_ The key every collection of the store is addressed by.
 *  @tparam store_type_ The store the writes land in, which must satisfy @c transaction_store_concept.
 *
 *  Nothing here throws: a mutation that outgrows the write buffer answers @c capacity_exhausted_k, and
 *  the caller may commit what it has and start over. The object is reusable after @c commit, after
 *  @c rollback and after @c renew.
 */
template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
class database_transaction {
  public:

#pragma region Types

    /** The key every collection of the store is addressed by. */
    using stored_key_t = stored_key_;

    /** The store every committed write of this transaction lands in. */
    using store_t = store_type_;

    /** The registry that orders this transaction against every other of the same store. */
    using registry_t = database_transaction_registry<stored_key_t, store_t, atomic_reference_, waiting_policy_type_>;

    /** The uncommitted writes and the watched keys this transaction carries. */
    using state_t = database_transaction_state<stored_key_t, atomic_reference_, waiting_policy_type_>;

    /** The uncommitted writes alone, grouped by the collection they belong to. */
    using write_buffer_t = typename state_t::write_buffer_t;

    /** A collection named by its ID or by its name, which is how every call addresses one. */
    using any_collection_id_t = typename store_t::any_collection_id_t;

#pragma endregion Types

#pragma region Construction

    database_transaction(database_transaction const&) = delete;
    database_transaction(database_transaction&&) = delete;
    database_transaction& operator=(database_transaction const&) = delete;
    database_transaction& operator=(database_transaction&&) = delete;

    /**
     *  @brief A transaction started at the newest stamp @p manager has published, which it pins until it dies.
     *  @param[inout] manager The registry that orders and conflict-checks it, outliving it.
     *  @param[in] changes The write buffer it fills, already sized to its configuration.
     *  @param[in] resources The budget its watched keys are charged to.
     */
    database_transaction(registry_t& manager, write_buffer_t&& changes,
                         resources_shared_ptr<atomic_reference_, waiting_policy_type_> resources) noexcept
        : manager_(manager), data_(std::move(changes), std::move(resources)) {
        start_stamp_ = manager_.request_start_(pin_);
    }

    /** Hands the leader word back when a stage still holds it, then gives the start stamp back. */
    ~database_transaction() noexcept {
        if (phase_ == phase_t::staged_k) manager_.discard_(data_);
    }

#pragma endregion Construction

#pragma region Lifecycle

    /**
     *  @brief Seals every write, takes the ledger's leader word, checks every watched key and lays out every view
     *      the commit publishes, without applying anything, which reserves this commit's place in the next batch.
     *
     *  A failure keeps the changes, so a caller can still log what it could not stage, and a success holds the
     *  leader word until @c commit or @c rollback hands it back; readers never wait for it, later committers do.
     */
    status_t stage() noexcept {
        if (phase_ != phase_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        transaction_stamp_t staged_stamp = invalid_transaction_stamp_k;
        status_t const staged = manager_.stage_(data_, start_stamp_, staged_stamp);
        if (staged.succeeded()) {
            last_stage_stamp_ = staged_stamp;
            phase_ = phase_t::staged_k;
        }
        return staged;
    }

    /**
     *  @brief Lands every write of this transaction, through the ledger's sequencer unless a stage already holds the
     *      leader word, and returns once its stamp is published.
     *
     *  @note Nothing is held on return, and only a success clears the changes and the watches and starts the
     *      transaction again.
     */
    status_t commit() noexcept {
        if (phase_ == phase_t::staged_k) last_try_to_commit_stamp_ = manager_.commit_staged_(data_);
        else {
            auto [committed, commit_status] = manager_.commit_(data_, start_stamp_);
            if (commit_status.failed()) {
                last_try_to_commit_stamp_ = manager_.current_stamp_();
                return commit_status;
            }
            last_try_to_commit_stamp_ = committed;
        }
        restart_();
        return {};
    }

    /** Drops every write and every watch, and starts the transaction over from this moment. */
    void rollback() noexcept {
        if (phase_ == phase_t::staged_k) manager_.discard_(data_);
        phase_ = phase_t::started_k;
        data_.clear();
        start_stamp_ = manager_.request_start_(pin_);
    }

    /**
     *  @brief Hands back the leader word a stage took, keeping every write and every watch, so the next stage or
     *      commit checks them all again.
     *
     *  @note A staged transaction holds the ledger's leader word, which every other committer of the ledger waits
     *      for, so a caller that only wants a stage's verdict lets it go at once.
     */
    void unstage() noexcept {
        if (phase_ == phase_t::staged_k) manager_.discard_(data_);
        phase_ = phase_t::started_k;
    }

    /**
     *  @brief Starts over from this moment while keeping the writes, whose watches move to the new start, which only
     *      a transaction that read nothing may.
     *  @return @c invalid_argument_k when the transaction has read anything, whose watches cannot move.
     */
    status_t renew() noexcept {
        if (phase_ != phase_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        if (data_.read_set.count(watch_purpose_t::read_k) != 0)
            return {status_code_t::invalid_argument_k, "That transaction has read something"};
        start_stamp_ = manager_.request_start_(pin_);
        data_.changes.rescope_watches(start_stamp_);
        return {};
    }

    /** Joins @p share, which every stage and commit from now on lays out and lands with this transaction's own writes,
     *  and which must outlive the transaction. */
    void join(transaction_share_t& share) noexcept {
        share.next = data_.shares;
        data_.shares = &share;
    }

    /**
     *  @brief Watches @p stored_keys of @p id for conflicts, without reading or writing any of them.
     *  @param[in] id The collection the keys belong to, named by its ID or by its name.
     *  @param[in] stored_keys The keys to watch, which are dropped by a commit and by a rollback.
     */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t watch_more(any_collection_id_t const& id, std::span<stored_key_t const> stored_keys) noexcept {
        if (phase_ != phase_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        collection_id_t const collection_id = resolve_(id).id;
        if (collection_id == collection_invalid_id_k)
            return {status_code_t::not_found_k, "The store names no such collection"};

        if (status_t const prepared = watch_now_or_prepare_for_later_<watch_schedule_>(collection_id, stored_keys);
            prepared.failed())
            return prepared;
        watch_later_<watch_schedule_>(collection_id, stored_keys, watch_stamp_for_<watch_schedule_>(),
                                      watch_purpose_t::track_k);
        return {};
    }

    /** Watches every key of @p id from @p lower to @p upper inclusive, the absent ones too, which a scan this
     *  transaction ran over them does so a commit inserting into the range conflicts. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t watch_range(any_collection_id_t const& id, stored_key_t lower, stored_key_t upper) noexcept {
        if (phase_ != phase_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        if constexpr (watch_schedule_ == watch_schedule_t::never_k) return {};
        collection_id_t const collection_id = resolve_(id).id;
        if (collection_id == collection_invalid_id_k)
            return {status_code_t::not_found_k, "The store names no such collection"};
        if constexpr (watch_schedule_ == watch_schedule_t::now_k) {
            if (manager_.range_written_now_(collection_id, lower, upper, start_stamp_))
                return {status_code_t::write_conflict_k, "A commit landed in that range since the start"};
            return {};
        }
        else {
            if (data_.ranges.push_back(typename state_t::range_watch_t {
                    collection_id, lower, upper, watch_stamp_for_<watch_schedule_>()}) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A transaction could not watch another range"};
            return {};
        }
    }

#pragma endregion Lifecycle

#pragma region Catalog

    /** Whether the store names the collection @p id. */
    [[nodiscard]] bool has_collection(any_collection_id_t const& id) noexcept {
        if (!idle_()) return false;
        return manager_.has_collection_(id);
    }

    /** The ID @p id resolves to, or @c collection_invalid_id_k when it names nothing. */
    [[nodiscard]] collection_id_t collection_id(any_collection_id_t const& id) noexcept {
        if (!idle_()) return collection_invalid_id_k;
        return resolve_(id).id;
    }

    /** The name and the number of every collection of the store. */
    [[nodiscard]] st::basic_vector<collection_ids_t> list_collections() noexcept {
        if (!idle_()) return {};
        return manager_.list_collections_();
    }

    /** Bytes the largest value @p id admits takes, which is what a caller sizes a read buffer by. */
    [[nodiscard]] stored_value_size_t buffer_size_for_any_stored_value(any_collection_id_t const& id) noexcept {
        if (!idle_()) return 0;
        return manager_.buffer_size_for_any_stored_value_(id);
    }

    /** Whether @p count more writes of @p bytes still fit in this transaction's own buffer. */
    [[nodiscard]] bool has_available_space_for(std::size_t count, std::size_t bytes) const noexcept {
        return data_.changes.can_fit(count, bytes);
    }

#pragma endregion Catalog

#pragma region Writes

    /** Writes @p stored_value under @p stored_key of @p id into this transaction's own buffer, watching the key for
     *  another commit writing it as @p watch_schedule_ says. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t insert(any_collection_id_t const& id, stored_key_t stored_key, stored_value_view_t stored_value) noexcept {
        if (phase_ != phase_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        resolved_collection_t const named = resolve_(id);
        collection_id_t const collection_id = named.id;
        if (collection_id == collection_invalid_id_k)
            return {status_code_t::not_found_k, "The store names no such collection"};
        if (status_t const checked = check_write_now_<watch_schedule_>(collection_id, stored_key); checked.failed())
            return checked;
        return data_.changes.add(collection_id, group_config_of_(named.config), stored_key, stored_value,
                                 watch_stamp_for_<watch_schedule_>());
    }

    /** Writes a removal of @p stored_key of @p id into this transaction's own buffer, watched as @c insert is. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t remove(any_collection_id_t const& id, stored_key_t stored_key) noexcept {
        if (phase_ != phase_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        resolved_collection_t const named = resolve_(id);
        collection_id_t const collection_id = named.id;
        if (collection_id == collection_invalid_id_k)
            return {status_code_t::not_found_k, "The store names no such collection"};
        if (status_t const checked = check_write_now_<watch_schedule_>(collection_id, stored_key); checked.failed())
            return checked;
        return data_.changes.mark_as_removed(collection_id, group_config_of_(named.config), stored_key,
                                             watch_stamp_for_<watch_schedule_>());
    }

    /**
     *  @brief Writes @p record, a change record of @p fold, under @p stored_key of @p id into this transaction's own
     *      buffer, folded after whatever the buffer holds for the key.
     *
     *  A change reads nothing, so it watches nothing, and two transactions changing one key both commit; a caller
     *  whose change depends on what it read watches that key itself.
     */
    template <utilities::fold_concept fold_type_>
    status_t change(any_collection_id_t const& id, stored_key_t stored_key, stored_value_view_t record,
                    fold_type_ const& fold) noexcept {
        if (phase_ != phase_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        resolved_collection_t const named = resolve_(id);
        if (named.id == collection_invalid_id_k)
            return {status_code_t::not_found_k, "The store names no such collection"};
        return data_.changes.add_change(named.id, group_config_of_(named.config), stored_key, record, fold);
    }

#pragma endregion Writes

#pragma region Lookups

    /** Whether this transaction or the store carries @p stored_key of @p id, executor thread @p thread_index reading
     *  the store on its own channel and epoch slot, as every lookup below does. */
    [[nodiscard]] bool contains(any_collection_id_t const& id, stored_key_t stored_key,
                                std::size_t thread_index = outside_executor_k) noexcept {
        return contains_uncommitted(id, stored_key) || contains_committed(id, stored_key, thread_index);
    }

    /** Bytes the value of @p stored_key takes, as this transaction would see it. */
    status_t stored_value_size(any_collection_id_t const& id, stored_key_t stored_key, stored_value_size_t& size,
                               std::size_t thread_index = outside_executor_k) noexcept {
        status_t const staged = stored_value_size_uncommitted(id, stored_key, size);
        if (staged.succeeded() || !(staged == status_code_t::not_found_k)) return staged;
        return stored_value_size_committed(id, stored_key, size, thread_index);
    }

    /** Copies the value of @p stored_key into @p into, taking this transaction's own write first. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t select(any_collection_id_t const& id, stored_key_t stored_key, stored_value_span_t into,
                    stored_value_size_t& size, std::size_t thread_index = outside_executor_k) noexcept {
        status_t const staged = select_uncommitted(id, stored_key, into, size);
        if (staged.succeeded()) return staged;
        return select_committed<watch_schedule_>(id, stored_key, into, size, thread_index);
    }

    /** What this transaction's own buffer did to @p stored_key of @p id: wrote it, removed it, or neither. */
    [[nodiscard]] write_buffer_status_t uncommitted(collection_id_t collection_id,
                                                    stored_key_t stored_key) const noexcept {
        return data_.changes.status(collection_id, stored_key);
    }

    /** Fills @p held with what this transaction's own buffer did to @p stored_key of @p collection_id, a write, a
     *  removal or a change, answering whether it did anything. */
    [[nodiscard]] bool uncommitted_delta(collection_id_t collection_id, stored_key_t stored_key,
                                         delta_view_t& held) const noexcept {
        return data_.changes.delta(collection_id, stored_key, held);
    }

    /** Hands @p callback every key this transaction wrote into @p collection_id, in no order, as
     *  @c callback(stored_key, kind, size), the kind telling a write from a removal and a change, which a reader
     *  folds over the committed value. */
    template <typename callback_type_>
    void for_each_uncommitted(collection_id_t collection_id, callback_type_&& callback) const noexcept {
        auto const group = data_.changes.groups_by_id.find(collection_id);
        if (group == data_.changes.groups_by_id.end()) return;
        [[maybe_unused]] st::status_t const walked = group.value().stored_values.for_each(
            [&](auto const& entry) noexcept {
                stored_value_size_t const size = entry.value().kind == delta_kind_t::remove_k ? 0 : entry.value().size;
                callback(entry.key(), entry.value().kind, size);
            });
    }

    /** Whether this transaction's own buffer carries @p stored_key of @p id. */
    [[nodiscard]] bool contains_uncommitted(any_collection_id_t const& id, stored_key_t stored_key) noexcept {
        if (!idle_()) return false;
        collection_id_t const collection_id = resolve_(id).id;
        if (collection_id == collection_invalid_id_k) return false;
        return data_.changes.status(collection_id, stored_key) == write_buffer_status_t::found_k;
    }

    /** Bytes the value this transaction wrote for @p stored_key takes. */
    status_t stored_value_size_uncommitted(any_collection_id_t const& id, stored_key_t stored_key,
                                           stored_value_size_t& size) noexcept {
        if (!idle_()) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        collection_id_t const collection_id = resolve_(id).id;
        if (collection_id == collection_invalid_id_k)
            return {status_code_t::not_found_k, "The store names no such collection"};
        return data_.changes.stored_value_size(collection_id, stored_key, size);
    }

    /** Copies the value this transaction wrote for @p stored_key into @p into. */
    status_t select_uncommitted(any_collection_id_t const& id, stored_key_t stored_key, stored_value_span_t into,
                                stored_value_size_t& size) noexcept {
        if (!idle_()) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        collection_id_t const collection_id = resolve_(id).id;
        if (collection_id == collection_invalid_id_k)
            return {status_code_t::not_found_k, "The store names no such collection"};
        return data_.changes.select(collection_id, stored_key, into, size);
    }

    /** Whether the store carries @p stored_key of @p id, ignoring what this transaction wrote. */
    [[nodiscard]] bool contains_committed(any_collection_id_t const& id, stored_key_t stored_key,
                                          std::size_t thread_index = outside_executor_k) noexcept {
        if (!idle_()) return false;
        return manager_.contains_(id, stored_key, start_stamp_, thread_index);
    }

    /** Bytes the store held for @p stored_key of @p id when this transaction started, ignoring what it wrote. */
    status_t stored_value_size_committed(any_collection_id_t const& id, stored_key_t stored_key,
                                         stored_value_size_t& size,
                                         std::size_t thread_index = outside_executor_k) noexcept {
        if (!idle_()) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        return manager_.stored_value_size_(id, stored_key, size, start_stamp_, thread_index);
    }

    /**
     *  @brief Reads @p stored_key of @p id out of the store and watches it for conflicts.
     *  @param[in] id The collection to read from, named by its ID or by its name.
     *  @param[in] stored_key The key to read.
     *  @param[out] into Where the value lands, which need not be aligned and need not be registered.
     *  @param[out] size Bytes written, which is zero for every failure.
     *  @param[in] thread_index The executor thread reading, which takes its own channel and epoch slot.
     *
     *  @note The read runs before the watch is taken, so a key the store refuses is never watched.
     */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t select_committed(any_collection_id_t const& id, stored_key_t stored_key, stored_value_span_t into,
                              stored_value_size_t& size, std::size_t thread_index = outside_executor_k) noexcept {
        if (phase_ != phase_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        collection_id_t const collection_id = resolve_(id).id;
        if (collection_id == collection_invalid_id_k)
            return {status_code_t::not_found_k, "The store names no such collection"};

        transaction_stamp_t const from = watch_stamp_for_<watch_schedule_>();
        transaction_stamp_t const at = read_stamp_for_<watch_schedule_>();
        if (status_t const read = manager_.select_(collection_id, stored_key, into, size, at, thread_index);
            read.failed())
            return read;

        std::array<stored_key_t, 1> const alone {stored_key};
        std::span<stored_key_t const> const watched {alone};
        if (status_t const prepared = watch_now_or_prepare_for_later_<watch_schedule_>(collection_id, watched);
            prepared.failed())
            return prepared;
        watch_later_<watch_schedule_>(collection_id, watched, from, watch_purpose_t::read_k);
        return {};
    }

#pragma endregion Lookups

#pragma region Stamps

    /** The stamp this transaction was started or restarted at, which scopes every watch it takes. */
    [[nodiscard]] transaction_stamp_t start_stamp() const noexcept { return start_stamp_; }

    /** The stamp this transaction last staged at, or @c invalid_transaction_stamp_k while it never has. */
    [[nodiscard]] transaction_stamp_t last_stage_stamp() const noexcept { return last_stage_stamp_; }

    /** The stamp this transaction last committed at, or @c invalid_transaction_stamp_k while it never has. */
    [[nodiscard]] transaction_stamp_t last_commit_stamp() const noexcept { return last_commit_stamp_; }

    /** The stamp this transaction last tried to commit at, whether or not that commit landed. */
    [[nodiscard]] transaction_stamp_t last_try_to_commit_stamp() const noexcept { return last_try_to_commit_stamp_; }

#pragma endregion Stamps

  private:

#pragma region Private Types

    /** Whether this transaction is filling its buffer or holding the ledger's leader word. */
    enum class phase_t : std::uint8_t {

        /** The transaction takes reads and writes, and holds nothing of the store. */
        started_k,

        /** The transaction has checked its watches, laid out its views and holds the ledger's leader word. */
        staged_k,
    };

    /** The value widths one collection admits, which every write of it is checked against. */
    using group_config_t = typename write_buffer_t::group_config_t;

    /** What one lookup of a collection answered, which the next lookup of the same one reuses. */
    struct resolved_collection_t {

        /** The ID the collection answers to, invalid when the store names no such collection. */
        collection_id_t id = collection_invalid_id_k;

        /** The value widths that collection admits, which every write of it is checked against. */
        group_config_t config {};

        /** The catalog version the answer was read at, which a create and a drop both move past. */
        std::uint64_t catalog_version = 0;
    };

#pragma endregion Private Types

#pragma region Private Methods

    /** Whether this transaction is filling its buffer rather than holding the ledger's leader word. */
    [[nodiscard]] bool idle_() const noexcept { return phase_ == phase_t::started_k; }

    /** The store this transaction reads and commits into, which the registry beside it names. */
    [[nodiscard]] store_t& store_() const noexcept { return *manager_.store_; }

    /**
     *  @brief What @p id resolves to, taken from the last answer while the catalog has not moved.
     *  @param[in] id The collection to resolve, named by its ID or by its name.
     *  @return Its ID and its widths, the ID invalid when the store names no such collection.
     *
     *  @note One catalog lookup answers both, and a run of calls over one collection takes one lookup
     *      rather than one per key; a create or a drop moves the version and the next call reads again.
     */
    [[nodiscard]] resolved_collection_t resolve_(any_collection_id_t const& id) noexcept {
        std::uint64_t const version = store_().catalog_version();
        collection_id_t const* const identified = std::get_if<collection_id_t>(&id);
        if (identified != nullptr && resolved_.id != collection_invalid_id_k && *identified == resolved_.id &&
            version == resolved_.catalog_version)
            return resolved_;

        collection_id_t collection_id = collection_invalid_id_k;
        collection_config_t shape;
        if (store_().resolve_collection(id, collection_id, shape).failed()) return resolved_collection_t {};
        resolved_.id = collection_id;
        resolved_.config = group_config_t {shape.lsm_tree.lsm_level_0.sorted_run.value_size,
                                           shape.lsm_tree.lsm_level_0.sorted_run.value_max_size};
        resolved_.catalog_version = version;
        return resolved_;
    }

    /** Answers the write buffer with @p widths, which the caller resolved in the same lookup. */
    [[nodiscard]] static auto group_config_of_(group_config_t widths) noexcept {
        return [widths](collection_id_t, group_config_t& config) noexcept {
            config = widths;
            return status_t {};
        };
    }

    /** The stamp a watch taken now is scoped from, which the schedule alone decides. */
    template <watch_schedule_t watch_schedule_>
    [[nodiscard]] transaction_stamp_t watch_stamp_for_() const noexcept {
        if constexpr (watch_schedule_ == watch_schedule_t::on_commit_k) return start_stamp_;
        else if constexpr (watch_schedule_ == watch_schedule_t::on_commit_since_operation_k)
            return manager_.current_stamp_();
        else return invalid_transaction_stamp_k;
    }

    /** The stamp a read under @p watch_schedule_ answers at: the start, but for a watch scoped from the read itself. */
    template <watch_schedule_t watch_schedule_>
    [[nodiscard]] transaction_stamp_t read_stamp_for_() const noexcept {
        if constexpr (watch_schedule_ == watch_schedule_t::on_commit_since_operation_k)
            return manager_.current_stamp_();
        else return start_stamp_;
    }

    /** Checks @p stored_keys right away, or makes the room a later watch of them needs. */
    template <watch_schedule_t watch_schedule_>
    status_t watch_now_or_prepare_for_later_(collection_id_t collection_id,
                                             std::span<stored_key_t const> stored_keys) noexcept {
        if constexpr (watch_schedule_ == watch_schedule_t::now_k)
            return manager_.watch_now_(start_stamp_, collection_id, stored_keys);
        else if constexpr (watch_schedule_ == watch_schedule_t::on_commit_k ||
                           watch_schedule_ == watch_schedule_t::on_commit_since_operation_k)
            return data_.read_set.reserve_more(collection_id, stored_keys.size());
        else return {};
    }

    /** Records every key of @p stored_keys as watched from @p from, in room the step above took. */
    template <watch_schedule_t watch_schedule_>
    void watch_later_(collection_id_t collection_id, std::span<stored_key_t const> stored_keys,
                      transaction_stamp_t from, watch_purpose_t purpose) noexcept {
        if constexpr (watch_schedule_ == watch_schedule_t::on_commit_k ||
                      watch_schedule_ == watch_schedule_t::on_commit_since_operation_k)
            for (stored_key_t const& stored_key : stored_keys)
                data_.read_set.add(collection_id, stored_key, from, purpose, st::assume_reserved);
    }

    /** Refuses a write of @p stored_key right away when @p watch_schedule_ checks in place and a commit since the
     *  start wrote it; every other schedule leaves the check to the stage. */
    template <watch_schedule_t watch_schedule_>
    status_t check_write_now_(collection_id_t collection_id, stored_key_t stored_key) noexcept {
        if constexpr (watch_schedule_ == watch_schedule_t::now_k)
            return manager_.watch_now_(start_stamp_, collection_id, std::span<stored_key_t const> {&stored_key, 1});
        else return {};
    }

    /** Clears what a landed commit carried and starts again at the stamp it published. */
    void restart_() noexcept {
        phase_ = phase_t::started_k;
        last_stage_stamp_ = last_try_to_commit_stamp_;
        last_commit_stamp_ = last_try_to_commit_stamp_;
        data_.clear();
        start_stamp_ = manager_.request_start_(pin_);
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The registry that orders this transaction and checks its watches, outliving it. */
    registry_t& manager_;

    /** The writes and the watches this transaction carries, which a read of it also fills. */
    state_t data_;

    /** The collection the last lookup answered, which the next lookup of the same one reuses. */
    resolved_collection_t resolved_;

    /** The start stamp, pinned so the history this transaction reads at stays kept. */
    stamp_pin<atomic_reference_, waiting_policy_type_> pin_;

    /** The stamp this transaction was started or restarted at. */
    transaction_stamp_t start_stamp_ = invalid_transaction_stamp_k;

    /** The stamp this transaction last staged at. */
    transaction_stamp_t last_stage_stamp_ = invalid_transaction_stamp_k;

    /** The stamp this transaction last committed at. */
    transaction_stamp_t last_commit_stamp_ = invalid_transaction_stamp_k;

    /** The stamp this transaction last tried to commit at. */
    transaction_stamp_t last_try_to_commit_stamp_ = invalid_transaction_stamp_k;

    /** Whether this transaction is filling its buffer or holding the ledger's leader word. */
    phase_t phase_ = phase_t::started_k;

#pragma endregion Private Members
};

#pragma endregion Transaction

#pragma endregion Database Transaction

#pragma region Database

#pragma region Schema

/** The layout the catalog blob follows, which a database of another version refuses to open. */
inline constexpr std::uint64_t database_schema_version_k = 2;

/** @p text as the bytes a volume names a blob by, without its terminator. */
template <std::size_t length_>
[[nodiscard]] inline blob_name_t blob_name_of(char const (&text)[length_]) noexcept {
    return blob_name_t {reinterpret_cast<byte_t const*>(text), length_ - 1};
}

/**
 *  @brief What every copy of a catalog carries besides its collections: which copy is newer and what it promises.
 *
 *  A catalog is saved to its two copies in turn, so replacing one never leaves a mount without the other.
 */
struct catalog_stamps_t {

    /** How many times the catalog has been saved, of which the higher copy is the one a mount reads. */
    std::uint64_t generation = 0;

    /** Every commit at or below this is wholly in the runs the copy names, and a mount drops every one above. */
    transaction_stamp_t durable = unwritten_stamp_k;
};

/** Which of the two copies of a catalog the save of @p generation replaces. */
[[nodiscard]] constexpr std::size_t catalog_copy_of(std::uint64_t generation) noexcept {
    return static_cast<std::size_t>(generation % 2);
}

/** The name copy @p copy of the catalog of a database is stored under, on whichever volume placement gave it. */
[[nodiscard]] inline blob_name_t database_schema_name(std::size_t copy) noexcept {
    return copy == 0 ? blob_name_of("ustore.database.schema.0") : blob_name_of("ustore.database.schema.1");
}

/** The name the caller's own bytes are stored under, which the engine never reads. */
[[nodiscard]] inline blob_name_t database_user_data_name() noexcept { return blob_name_of("ustore.database.user"); }

/** Bytes the longest collection name takes, which a record holds inline rather than pointing at. */
inline constexpr std::size_t collection_name_max_bytes_k = 64;

/**
 *  @brief One collection as the catalog blob names it, which is all a mount needs to open it again.
 *
 *  Every field is stored as it lies, so the image is the object representation and no field of it is
 *  ever read through a pointer the layout did not align.
 */
struct collection_record_t {

    /** The UUID the catalog, every commit and the C ABI address the collection by. */
    collection_id_t id = collection_invalid_id_k;

    /** How fast the bytes a level admits grow with its number. */
    std::uint64_t level_enlarge_factor = 0;

    /** Bytes the value cache of the collection holds. */
    std::uint64_t cache_limit = 0;

    /** Bytes the write buffer of the collection holds at once. */
    std::uint64_t write_buffer_max_bytes = 0;

    /** Writes the write buffer of the collection holds at once. */
    std::uint64_t write_buffer_max_elements = 0;

    /** Runs of this collection in the image's run list, which follow those of the records before it. */
    std::uint64_t run_count = 0;

    /** Bytes every value takes, zero for a layout whose values differ in width. */
    std::uint32_t value_size = 0;

    /** Bytes the largest value takes, zero for a fixed-width layout. */
    std::uint32_t value_max_size = 0;

    /** The volume the collection's runs live on, which a reopen binds it to again. */
    std::uint16_t volume = 0;

    /** Bytes of @c name_storage the name occupies. */
    std::uint16_t name_bytes = 0;

    /** The @c ustore_modality_t the collection holds, which with its value layout picks the store a mount opens. */
    std::uint16_t modality = 0;

    /** The name a caller reaches the collection by, held inline and never terminated. */
    std::array<char, collection_name_max_bytes_k> name_storage {};

    /** The name this record carries, as a view into its own bytes. */
    [[nodiscard]] string_view_t name() const noexcept { return string_view_t {name_storage.data(), name_bytes}; }

    /** The collection this record describes, as the collection layer takes its bounds. */
    [[nodiscard]] collection_config_t config() const noexcept {
        collection_config_t config;
        config.cache_limit = static_cast<std::size_t>(cache_limit);
        config.write_buffer_max_bytes = static_cast<std::size_t>(write_buffer_max_bytes);
        config.write_buffer_max_elements = static_cast<std::size_t>(write_buffer_max_elements);
        config.lsm_tree.level_enlarge_factor = static_cast<std::size_t>(level_enlarge_factor);
        config.lsm_tree.lsm_level_0.sorted_run.value_size = static_cast<stored_value_size_t>(value_size);
        config.lsm_tree.lsm_level_0.sorted_run.value_max_size = static_cast<stored_value_size_t>(value_max_size);
        return config;
    }
};

#pragma endregion Schema

#pragma region Database

/**
 *  @brief The claim a database's maintenance, catalog and blob writes run under, which callers park on rather than
 *      spin on, so a caller parked on one holder wakes once that holder lets it go, whoever takes it next.
 *
 *  The claim is one word kept as ForkUnion's generation clock: @b odd while held and @b even while free, stepped only
 *  by the take that finds it even and by its holder's release, so every generation advances it by two. A take sets
 *  the held bit, which leaves an odd word as it was, and answers by the parity it read; a release adds one.
 *
 *  A committer finding it held returns at once rather than waiting, and every holder looks at the views again after
 *  it lets go. The fence after each release pairs with the fence before each attempt, so either the committer takes
 *  the claim, or the holder that refused it reads the views the committer published.
 *
 *  @tparam atomic_reference_ The reference the claim's word is owned through.
 *  @tparam waiting_policy_type_ How a caller finding the claim held parks until its holder lets go.
 */
template <template <typename> typename atomic_reference_, typename waiting_policy_type_>
class parking_claim {
  public:

    /** Takes the claim, parked on each generation it finds held until that holder lets it go. */
    void take() noexcept {
        atomic_reference_<std::uint32_t> const epoch {epoch_};
        // Only the held bit of the old word is tested, so x86 sets it with `lock bts` and reloads the word to park on.
        while (epoch.fetch_or(1, std::memory_order_acquire) % 2 != 0) {
            std::uint32_t const seen = epoch.load(std::memory_order_relaxed);
            if (seen % 2 != 0) waiting_policy_type_ {}.wait_until_changed(&epoch_, seen);
        }
    }

    /** Takes the claim if no caller holds it, and answers whether it did. */
    [[nodiscard]] bool take_if_free() noexcept {
        // Pairs with the fence after a release: either this reads the release, or that holder reads what came before.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return atomic_reference_<std::uint32_t> {epoch_}.fetch_or(1, std::memory_order_acquire) % 2 == 0;
    }

    /** Parks until the caller holding the claim now lets it go, or returns at once while nobody holds it. */
    void wait_until_released() noexcept {
        atomic_reference_<std::uint32_t> const epoch {epoch_};
        std::uint32_t const seen = epoch.load(std::memory_order_acquire);
        if (seen % 2 == 0) return;
        // The wait reloads relaxed; this reload acquires, so a woken caller reads what the holder wrote at once.
        do waiting_policy_type_ {}.wait_until_changed(&epoch_, seen);
        while (epoch.load(std::memory_order_acquire) == seen);
    }

    /** Hands the claim back, which only its holder does, and wakes every caller parked on it; the holder looks at the
     *  views again only after this. */
    void release() noexcept {
        atomic_reference_<std::uint32_t> {epoch_}.add(1, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        waiting_policy_type_ {}.notify_waiters(&epoch_);
    }

  private:

    /** The claim's generation: odd while held, even while free, stepped by whoever takes or lets it go. */
    alignas(fu::default_alignment_k) std::uint32_t epoch_ = 0;
};

/**
 *  @brief What a database asks of the store one collection lives in: which collections it keeps, its mount, its
 *      reads, the steps of a commit, its maintenance and the runs a catalog names.
 *
 *  A scan walks the store through the @c collection_scanner its key slot and fold name.
 */
template <typename store_type_>
concept nvme_store = requires(
    store_type_& store, store_type_ const& viewed, ustore_modality_t modality, value_layout_t layout,
    typename store_type_::volume_t& volume, collection_config_t const& config,
    typename store_type_::constraint_t& constraint, typename store_type_::ledger_t& ledger, io_channel_t& channel,
    lsm_tree_publication_hook_t const& published, std::span<lsm_tree_run_record_t const> runs,
    st::basic_vector<lsm_tree_run_record_t>& listed, typename store_type_::stored_key_t stored_key,
    std::span<typename store_type_::stored_key_t const> stored_keys,
    std::span<keyed_delta<typename store_type_::stored_key_t>> deltas, std::span<byte_t const> stored_values,
    std::span<stored_value_size_t const> sizes, std::span<byte_t> into, stored_value_size_t& size,
    typename store_type_::sealed_commit_t& sealed, transaction_stamp_t stamp, std::size_t thread_index) {
    typename store_type_::key_slot_t;
    typename store_type_::fold_t;
    { store_type_::serves(modality, layout) } noexcept -> std::same_as<bool>;
    { store_type_::mount(volume, config, constraint, ledger) } noexcept -> std::same_as<expected<store_type_>>;
    { store.restore(channel, runs) } noexcept -> std::same_as<status_t>;
    { store.truncate_above(channel, channel, stamp, published) } noexcept -> std::same_as<status_t>;
    { store.destroy(channel) } noexcept -> std::same_as<status_t>;
    { record_lsm_tree_runs(viewed.tree(), listed) } noexcept -> std::same_as<status_t>;
    { viewed.population_count() } noexcept -> std::same_as<std::uint64_t>;
    { viewed.size_bytes() } noexcept -> std::same_as<std::uint64_t>;
    { viewed.value_max_size() } noexcept -> std::same_as<stored_value_size_t>;
    { viewed.find(stored_key, size, stamp, thread_index) } noexcept -> std::same_as<select_result_t>;
    { store.select(channel, stored_key, into, size, stamp, thread_index) } noexcept -> std::same_as<status_t>;
    { viewed.written_since(stored_key, stamp) } noexcept -> std::same_as<bool>;
    { viewed.range_written_since(stored_key, stored_key, stamp) } noexcept -> std::same_as<bool>;
    { viewed.seal(deltas) } noexcept -> std::same_as<expected<typename store_type_::sealed_commit_t>>;
    {
        viewed.seal_inserts(stored_keys, stored_values, sizes)
    } noexcept -> std::same_as<expected<typename store_type_::sealed_commit_t>>;
    { viewed.seal_erasures(stored_keys) } noexcept -> std::same_as<expected<typename store_type_::sealed_commit_t>>;
    { viewed.memory_pressure(sealed) } noexcept -> std::same_as<memory_pressure_t>;
    { store.prepare(sealed) } noexcept -> std::same_as<status_t>;
    { store.publish(sealed, stamp) } noexcept;
    { store.forget_prepared() } noexcept;
    { viewed.memory_shape() } noexcept -> std::same_as<memory_shape_t>;
    { viewed.maintenance_due() } noexcept -> std::same_as<bool>;
    { store.merge_memory_runs() } noexcept -> std::same_as<status_t>;
    { viewed.flush_due() } noexcept -> std::same_as<bool>;
    { store.flush(channel) } noexcept -> std::same_as<status_t>;
    { viewed.needs_compaction() } noexcept -> std::same_as<bool>;
    { store.compact(channel, channel, published) } noexcept -> std::same_as<expected<compaction_outcome_t>>;
};

/**
 *  @brief Named collections of one database, the catalog that resolves them and the transactions they share.
 *  @tparam stored_key_ The key every collection of this database is addressed by.
 *  @tparam volume_type_ The volume every run of every collection lives on.
 *  @tparam stores_ The stores a collection may live in, of which a create and a mount take the first that serves its
 *      modality and value layout.
 *
 *  The router and the channels are borrowed and must outlive the database, and so must the budget the
 *  collections charge their memory to. A database is pinned rather than movable, because its registry
 *  and every live transaction name it.
 */
template <typename stored_key_, typename volume_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_, nvme_store... stores_>
class basic_database {
  public:

#pragma region Types

    /** The key every collection of this database is addressed by. */
    using stored_key_t = stored_key_;

    /** The volume every run of every collection lives on. */
    using volume_t = volume_type_;

    /** The transport those volumes move bytes through. */
    using disk_t = std::remove_reference_t<decltype(std::declval<volume_t&>().disk())>;

    /** The volumes of this database and the placement that spreads collections over them. */
    using router_t = basic_volume_router<volume_t>;

    /** The queue pairs every call that moves bytes borrows one of. */
    using channel_pool_t = basic_io_channel_pool<disk_t, atomic_reference_>;

    /** One borrowed queue pair, drained and given back when it dies. */
    using channel_guard_t = typename channel_pool_t::guard_t;

    /** A collection named by its ID or by its name, which is how every call addresses one. */
    using any_collection_id_t = std::variant<collection_id_t, collection_alphanumeric_id_t>;

    /** The transactions of this database, the order they commit in and the conflicts that stop them. */
    using registry_t =
        database_transaction_registry<stored_key_t, basic_database, atomic_reference_, waiting_policy_type_>;

    /** One transaction over the collections of this database. */
    using transaction_t = database_transaction<stored_key_t, basic_database, atomic_reference_, waiting_policy_type_>;

    /** A live transaction, owned by whoever asked for it and retired when it dies. */
    using transaction_unique_ptr_t = std::unique_ptr<transaction_t>;

    /** One pinned stamp, at which the whole database answers. */
    using snapshot_t = database_snapshot<basic_database>;

    /** One stamp a snapshot, a transaction or a scan holds against the ledger. */
    using pin_t = stamp_pin<atomic_reference_, waiting_policy_type_>;

    /** One collection, held by the store its modality and value layout picked when it was created. */
    using any_collection_t = std::variant<stores_...>;

    /** One entry of the catalog: what a collection is called, how it is shaped, and the collection itself. */
    struct collection_entry_t {

        /** The UUID the catalog, every commit and the C ABI address this collection by. */
        collection_id_t id = collection_invalid_id_k;

        /** The name a caller reaches this collection by. */
        string_t name;

        /** What this collection holds, which with its value layout picked its store. */
        ustore_modality_t modality = ustore_modality_any_k;

        /** The bounds this collection was mounted with. */
        collection_config_t config {};

        /** The volume this collection's runs live on. */
        volume_id_t volume = volume_id_t::unmounted_k;

        /** The collection itself, pinned here so a snapshot may name it. */
        any_collection_t held;
    };

#pragma endregion Types

#pragma region Construction

    basic_database(basic_database const&) = delete;
    basic_database(basic_database&&) = delete;
    basic_database& operator=(basic_database const&) = delete;
    basic_database& operator=(basic_database&&) = delete;

    /** A database charging @p resources for every collection it opens, which @c mount then fills. */
    explicit basic_database(resources_shared_ptr<atomic_reference_, waiting_policy_type_> resources) noexcept
        : resources_(std::move(resources)), transactions_(*this, resources_) {}

    ~basic_database() noexcept { [[maybe_unused]] status_t const closed = unmount(); }

    /**
     *  @brief Opens every collection @p router already names, or starts an empty catalog.
     *  @param[inout] router The volumes every run lands on, borrowed and outliving the database.
     *  @param[inout] channels The queue pairs every call borrows one of, borrowed and outliving the database.
     *  @param[in] config The write-buffer bounds a transaction takes when its caller names none.
     */
    status_t mount(router_t& router, channel_pool_t& channels, database_config_t const& config) noexcept {
        if (router_ != nullptr) return {status_code_t::already_exists_k, "This database has already mounted"};
        router_ = &router;
        channels_ = &channels;
        config_ = config;
        status_t const opened = open_catalog_();
        if (opened.failed()) {
            // A catalog this database could not open stays as it lies, never saved again without what it missed.
            collections_.clear();
            router_ = nullptr;
            channels_ = nullptr;
        }
        return opened;
    }

    /**
     *  @brief Flushes every collection, rewrites the catalog and lets every collection go.
     *
     *  @note The router and the channels are still needed here, so this runs before the mount underneath
     *      is torn down; the destructor calls it for a caller that did not.
     */
    status_t unmount() noexcept {
        if (router_ == nullptr) return {};
        status_t first;
        {
            expected<claimed_channel_t> claimed = claim_channel_();
            if (!claimed) first = claimed.status();
            else {
                for (std::size_t index = 0; index != collections_.size(); ++index)
                    if (status_t const flushed = std::visit(
                            [&](auto& held) noexcept { return held.flush(claimed->channel()); },
                            collections_[index]->held);
                        flushed.failed() && first.succeeded())
                        first = flushed;
                if (status_t const saved = save_schema_(claimed->channel()); saved.failed() && first.succeeded())
                    first = saved;
            }
            // The entries outlive the publication that stops naming them, which waits out every reader of the catalog
            // it replaced.
            st::basic_vector<entry_pointer_t> released = std::move(collections_);
            [[maybe_unused]] status_t const emptied = publish_catalog_();
        }
        router_ = nullptr;
        channels_ = nullptr;
        return first;
    }

#pragma endregion Construction

#pragma region Catalog

    /** Collections this database names right now. */
    [[nodiscard]] std::size_t size() noexcept {
        reader_t const inside = ledger_.epochs().enter();
        return catalog_entries_().size();
    }

    /** Whether @p id names a collection of this database. */
    [[nodiscard]] bool has_collection(any_collection_id_t const& id) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        return entry_at_in_(catalog_entries_(), id) != nullptr;
    }

    /** The ID @p id resolves to, or @c collection_invalid_id_k when it names nothing. */
    [[nodiscard]] collection_id_t collection_id(any_collection_id_t const& id) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        collection_entry_t const* const found = entry_at_in_(catalog_entries_(), id);
        return found ? found->id : collection_invalid_id_k;
    }

    /** The name @p collection_id answers to, which is empty when it names nothing. */
    [[nodiscard]] string_t collection_name(collection_id_t collection_id) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        collection_entry_t const* const found = entry_with_id_in_(catalog_entries_(), collection_id);
        string_t name;
        if (found) { [[maybe_unused]] sz::status_t const copied = name.assign(found->name.view()); }
        return name;
    }

    /** The bounds @p collection_id was created with, which a transaction checks every write against. */
    status_t collection_config(collection_id_t collection_id, collection_config_t& config) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        collection_entry_t const* const found = entry_with_id_in_(catalog_entries_(), collection_id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        config = found->config;
        return {};
    }

    /** What @p collection_id holds, which a copy of it is created with. */
    status_t collection_modality(collection_id_t collection_id, ustore_modality_t& modality) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        collection_entry_t const* const found = entry_with_id_in_(catalog_entries_(), collection_id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        modality = found->modality;
        return {};
    }

    /**
     *  @brief The ID @p id answers to and the bounds it was created with, read in one lookup.
     *  @param[in] id The collection to resolve, named by its ID or by its name.
     *  @param[out] collection_id The ID it answers to, invalid when the database names none.
     *  @param[out] config The bounds it was created with, untouched when the database names none.
     */
    status_t resolve_collection(any_collection_id_t const& id, collection_id_t& collection_id,
                                collection_config_t& config) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        collection_entry_t const* const found = entry_at_in_(catalog_entries_(), id);
        collection_id = found ? found->id : collection_invalid_id_k;
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        config = found->config;
        return {};
    }

    /** How many times the catalog has been saved, whose @c catalog_copy_of names the copy a mount reads. */
    [[nodiscard]] std::uint64_t catalog_generation() const noexcept { return generation_; }

    /** How many times the catalog has been published, which a memo of a lookup is checked against. */
    [[nodiscard]] std::uint64_t catalog_version() const noexcept {
        return atomic_reference_<std::uint64_t const> {catalog_version_}.load(std::memory_order_acquire);
    }

    /** The name and the number of every collection, which is short of the full list only when memory runs out. */
    [[nodiscard]] st::basic_vector<collection_ids_t> list_collections() noexcept {
        reader_t const inside = ledger_.epochs().enter();
        std::span<collection_entry_t* const> const entries = catalog_entries_();
        st::basic_vector<collection_ids_t> listed;
        if (listed.reserve(entries.size()) != st::status_t::success_k) return listed;
        for (collection_entry_t const* entry : entries) {
            collection_ids_t named;
            if (sz::failed(named.name.assign(entry->name.view()))) break;
            named.id = entry->id;
            if (listed.push_back(std::move(named)) != st::status_t::success_k) break;
        }
        return listed;
    }

    /**
     *  @brief Catalogs a new collection @p name under @p collection_id, shaped by @p config.
     *  @param[in] collection_id The UUID the caller minted, which addresses the collection for as long as it lives.
     *  @param[in] name The name callers reach the collection by, which no other collection may hold.
     *  @param[in] modality What the collection holds, which with its value layout picks its store.
     *  @param[in] config The write-buffer bounds, the cache bound and the shape of the tree below.
     *
     *  @note The value layout follows from @c config's value size, so a width of eight bytes is stored
     *      inside the key slots and a width of none is a collection of values that differ.
     */
    status_t create_collection(collection_id_t collection_id, string_view_t name, ustore_modality_t modality,
                               collection_config_t const& config) noexcept {
        if (name.empty()) return status_t {status_code_t::invalid_argument_k, "A collection is named by a byte"};
        if (name.size() > collection_name_max_bytes_k)
            return status_t {status_code_t::invalid_argument_k, "A collection name is shorter than that"};
        if (collection_id == collection_invalid_id_k)
            return status_t {status_code_t::invalid_argument_k, "That ID names no collection"};
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        if (entry_named_in_(catalog_entries_(), name) != nullptr ||
            entry_with_id_in_(catalog_entries_(), collection_id) != nullptr)
            return status_t {status_code_t::already_exists_k, "The database already names that collection"};

        auto [placed, optimal_volume_status] = router_->optimal_volume(1);
        if (optimal_volume_status.failed()) return optimal_volume_status;

        entry_pointer_t entry {new (std::nothrow) collection_entry_t {}};
        if (!entry) return status_t {status_code_t::out_of_memory_k, "No room for another collection"};
        entry->id = collection_id;
        if (sz::failed(entry->name.assign(name)))
            return status_t {status_code_t::out_of_memory_k, "No room for a collection's name"};
        entry->modality = modality;
        entry->config = config;
        entry->volume = placed;
        if (status_t const mounted = mount_collection_(*entry); mounted.failed()) return mounted;

        if (collections_.push_back(std::move(entry)) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room for another collection"};
        if (status_t const saved = save_schema_(claimed->channel()); saved.failed()) {
            collections_.erase(collections_.size() - 1);
            return saved;
        }
        if (status_t const publication = publish_catalog_(); publication.failed()) {
            collections_.erase(collections_.size() - 1);
            return publication;
        }
        return {};
    }

    /** Returns every run of @p id to its volume and forgets the name, which destroys the data. */
    status_t drop_collection(any_collection_id_t const& id) noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        std::size_t const position = position_at_(id);
        if (position == collections_.size())
            return {status_code_t::not_found_k, "The database names no such collection"};

        // The catalog stops naming the runs before a single one of them is handed back. The publication takes the
        // leader word, which keeps a staged or combining commit from publishing into the collection meanwhile, and a
        // request still pending finds it gone; it returns once no reader is left in a view of the collection.
        entry_pointer_t doomed = std::move(collections_[position]);
        collections_.erase(position);
        status_t const publication = publish_catalog_();
        if (publication.failed()) {
            [[maybe_unused]] st::status_t const returned = collections_.push_back(std::move(doomed));
            return publication;
        }
        if (status_t const saved = save_schema_(claimed->channel()); saved.failed()) {
            [[maybe_unused]] st::status_t const returned = collections_.push_back(std::move(doomed));
            [[maybe_unused]] status_t const restored = publish_catalog_();
            return saved;
        }
        return std::visit([&](auto& held) noexcept { return held.destroy(claimed->channel()); }, doomed->held);
    }

#pragma endregion Catalog

#pragma region Shape

    /** Records the memory runs hold plus key slots every level of every collection holds. */
    [[nodiscard]] std::uint64_t population_count() noexcept {
        reader_t const inside = ledger_.epochs().enter();
        std::uint64_t counted = 0;
        for (collection_entry_t const* entry : catalog_entries_())
            counted += std::visit([](auto const& collection) noexcept { return collection.population_count(); },
                                  entry->held);
        return counted;
    }

    /** Bytes every run of every collection takes on its volume. */
    [[nodiscard]] std::uint64_t size_bytes() noexcept {
        reader_t const inside = ledger_.epochs().enter();
        std::uint64_t counted = 0;
        for (collection_entry_t const* entry : catalog_entries_())
            counted += std::visit([](auto const& collection) noexcept { return collection.size_bytes(); }, entry->held);
        return counted;
    }

    /** Bytes the largest value @p id admits takes, which is what a caller sizes a read buffer by. */
    [[nodiscard]] stored_value_size_t buffer_size_for_any_stored_value(any_collection_id_t const& id) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        collection_entry_t const* const found = entry_at_in_(catalog_entries_(), id);
        if (!found) return 0;
        return std::visit([](auto const& collection) noexcept { return collection.value_max_size(); }, found->held);
    }

#pragma endregion Shape

#pragma region Lookups

    /** What @p id carries for @p stored_key as a reader pinned at @p stamp sees it, and the bytes its value takes;
     *  a collection the database does not name carries nothing. Executor thread @p thread_index reads on its own
     *  epoch slot, as every lookup below does. */
    [[nodiscard]] select_result_t find(any_collection_id_t const& id, stored_key_t stored_key,
                                       stored_value_size_t& size, transaction_stamp_t stamp = head_stamp_k,
                                       std::size_t thread_index = outside_executor_k) noexcept {
        size = 0;
        reader_t const inside = ledger_.epochs().enter(thread_index);
        collection_entry_t const* const found = entry_at_in_(catalog_entries_(), id);
        if (!found) return select_result_t::nothing_k;
        return std::visit(
            [&]<typename store_type_>(store_type_ const& collection) noexcept {
                if constexpr (keyed_by_database<store_type_>)
                    return collection.find(stored_key, size, stamp, thread_index);
                else return select_result_t::nothing_k;
            },
            found->held);
    }

    /** Whether @p id carries @p stored_key with a value rather than a tombstone, as a reader pinned at @p stamp
     *  sees it, a chain of changes always folding into one. */
    [[nodiscard]] bool contains(any_collection_id_t const& id, stored_key_t stored_key,
                                transaction_stamp_t stamp = head_stamp_k,
                                std::size_t thread_index = outside_executor_k) noexcept {
        stored_value_size_t size = 0;
        select_result_t const found = find(id, stored_key, size, stamp, thread_index);
        return found == select_result_t::success_k || found == select_result_t::incomplete_k;
    }

    /** Bytes the value of @p stored_key takes in @p id as a reader pinned at @p stamp sees it, or @c not_found_k for a
     *  miss and an erasure; a chain of changes reaching values apart from their key slots is folded through a channel
     *  to be measured. */
    status_t stored_value_size(any_collection_id_t const& id, stored_key_t stored_key, stored_value_size_t& size,
                               transaction_stamp_t stamp = head_stamp_k,
                               std::size_t thread_index = outside_executor_k) noexcept {
        select_result_t const found = find(id, stored_key, size, stamp, thread_index);
        if (found == select_result_t::success_k) return {};
        if (found != select_result_t::incomplete_k) return {status_code_t::not_found_k, "That collection misses it"};
        std::uint64_t measured = 0;
        select_result_t folded = select_result_t::nothing_k;
        auto const count = [&](std::size_t, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
            measured = std::max<std::uint64_t>(measured, offset + bytes.size());
        };
        if (status_t const read = select(id, std::span<stored_key_t const> {&stored_key, 1},
                                         std::span<select_result_t> {&folded, 1}, count, stamp, thread_index);
            read.failed())
            return read;
        if (folded != select_result_t::success_k) return {status_code_t::not_found_k, "That collection misses it"};
        size = static_cast<stored_value_size_t>(measured);
        return {};
    }

    /**
     *  @brief Copies the value @p id holds for @p stored_key into @p into, which needs only its bytes.
     *  @param[in] id The collection to read from, named by its ID or by its name.
     *  @param[in] stored_key The key to read.
     *  @param[out] into Where the value lands, which need not be aligned and need not be registered.
     *  @param[out] size Bytes written, which is zero for every failure.
     *  @param[in] stamp The stamp a reader is pinned at, which sees the newest version at or below it.
     *  @param[in] thread_index The executor thread reading, which takes its own channel and epoch slot.
     */
    status_t select(any_collection_id_t const& id, stored_key_t stored_key, std::span<byte_t> into,
                    stored_value_size_t& size, transaction_stamp_t stamp = head_stamp_k,
                    std::size_t thread_index = outside_executor_k) noexcept {
        size = 0;
        reader_t const inside = ledger_.epochs().enter(thread_index);
        collection_entry_t* const found = entry_at_in_(catalog_entries_(), id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        auto [channel, acquire_status] = channels_->acquire(thread_index);
        if (acquire_status.failed()) return acquire_status;
        return std::visit(
            [&]<typename store_type_>(store_type_& collection) noexcept -> status_t {
                if constexpr (keyed_by_database<store_type_>)
                    return collection.select(channel.channel(), stored_key, into, size, stamp, thread_index);
                else return keyed_otherwise_();
            },
            found->held);
    }

    /**
     *  @brief Reads the values of @p stored_keys out of @p id, refilling the channel as completions retire.
     *  @param[in] id The collection to read from, named by its ID or by its name.
     *  @param[in] stored_keys The keys to read, in any order and with any number of misses among them.
     *  @param[out] results What every key answered, which separates a miss from an erasure.
     *  @param[in] sink Called as @c sink(request, offset, bytes) for every key that locates a value.
     *  @param[in] stamp The stamp a reader is pinned at, which sees the newest version at or below it.
     *  @param[in] thread_index The executor thread reading, which takes its own channel and epoch slot.
     */
    template <typename sink_type_>
    status_t select(any_collection_id_t const& id, std::span<stored_key_t const> stored_keys,
                    std::span<select_result_t> results, sink_type_&& sink, transaction_stamp_t stamp = head_stamp_k,
                    std::size_t thread_index = outside_executor_k) noexcept {
        reader_t const inside = ledger_.epochs().enter(thread_index);
        collection_entry_t const* const found = entry_at_in_(catalog_entries_(), id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        auto [channel, acquire_status] = channels_->acquire(thread_index);
        if (acquire_status.failed()) return acquire_status;
        return std::visit(
            [&]<typename store_type_>(store_type_ const& collection) noexcept -> status_t {
                if constexpr (keyed_by_database<store_type_>)
                    return collection.select(channel.channel(), stored_keys, results, sink, stamp, thread_index);
                else return keyed_otherwise_();
            },
            found->held);
    }

    /**
     *  @brief Hands @p visitor every key of @p id from @p from up in ascending order, memory runs included.
     *  @param[in] id The collection to walk, named by its ID or by its name.
     *  @param[in] from The smallest key the walk reports, which need not be in the collection.
     *  @param[in] visitor Called as @c visitor(stored_key, state, size) for the newest version of every key at or
     *      below @p stamp, erasures included, answering whether the walk carries on.
     *  @param[in] stamp The stamp a reader is pinned at, which sees the newest version at or below it.
     *  @param[in] thread_index The executor thread walking, which announces on its own epoch slot.
     *
     *  @note The walk reads one page of keys at a time inside the epochs, finding the collection again for each, and
     *      hands a page to @p visitor outside them, so a commit waits for one page at most; a collection dropped
     *      meanwhile ends the walk with @c not_found_k.
     */
    template <typename visitor_type_>
    status_t scan(any_collection_id_t const& id, stored_key_t from, visitor_type_&& visitor,
                  transaction_stamp_t stamp = head_stamp_k, std::size_t thread_index = outside_executor_k) noexcept {
        // A head walk pins the stamp it starts at, so every page reads the same commits.
        stamp_pin<atomic_reference_, waiting_policy_type_> pin;
        if (stamp == head_stamp_k) stamp = ledger_.pin_reader(pin);
        std::array<scan_entry<stored_key_t>, scan_page_keys_k<stored_key_t>> page;
        for (;;) {
            std::size_t count = 0;
            bool follows = false;
            {
                reader_t const inside = ledger_.epochs().enter(thread_index);
                collection_entry_t const* const found = entry_at_in_(catalog_entries_(), id);
                if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
                status_t const read = std::visit(
                    [&]<typename store_type_>(store_type_ const& collection) noexcept -> status_t {
                        if constexpr (keyed_by_database<store_type_>) {
                            using scanner_t =
                                collection_scanner<typename store_type_::key_slot_t, volume_t, atomic_reference_,
                                                   waiting_policy_type_, typename store_type_::fold_t>;
                            auto [scanner, over_status] = scanner_t::over(collection, from, stamp, thread_index);
                            if (over_status.failed()) return over_status;
                            std::span<scan_entry<stored_key_t> const> const entries = scanner.page();
                            std::copy(entries.begin(), entries.end(), page.begin());
                            count = entries.size();
                            follows = scanner.follows(from);
                            return {};
                        }
                        else return keyed_otherwise_();
                    },
                    found->held);
                if (read.failed()) return read;
            }
            for (std::size_t index = 0; index != count; ++index) {
                scan_entry<stored_key_t>& entry = page[index];
                // A chain of changes the scanner could not fold without a channel is folded here, outside the epochs
                if (entry.state == select_result_t::incomplete_k) {
                    status_t const measured = stored_value_size(id, entry.stored_key, entry.size, stamp, thread_index);
                    if (measured.failed() && !(measured == status_code_t::not_found_k)) return measured;
                    entry.state = measured.succeeded() ? select_result_t::success_k : select_result_t::erased_k;
                }
                if (!visitor(entry.stored_key, entry.state, entry.size)) return {};
            }
            if (!follows) return {};
        }
    }

#pragma endregion Lookups

#pragma region Writes

    /** Writes @p stored_value under @p stored_key of @p id as a commit of its own, flushing first when the memory runs
     *  are past their bounds. */
    status_t insert(any_collection_id_t const& id, stored_key_t stored_key, stored_value_view_t stored_value) noexcept {
        return commit_alone_(id, [&](auto const& collection) noexcept {
            keyed_delta<stored_key_t> delta {stored_key, delta_view_t {delta_kind_t::write_k, stored_value}};
            return collection.seal(std::span<keyed_delta<stored_key_t>> {&delta, 1});
        });
    }

    /** Removes @p stored_key of @p id as a commit of its own, flushing first when the memory runs are past their
     *  bounds. */
    status_t remove(any_collection_id_t const& id, stored_key_t stored_key) noexcept {
        return commit_alone_(id, [&](auto const& collection) noexcept {
            keyed_delta<stored_key_t> delta {stored_key, delta_view_t {delta_kind_t::remove_k, {}}};
            return collection.seal(std::span<keyed_delta<stored_key_t>> {&delta, 1});
        });
    }

    /**
     *  @brief Writes one value per key into @p id as one commit of its own, flushing first when they do not all fit.
     *  @param[in] id The collection to write to, named by its ID or by its name.
     *  @param[in] stored_keys The keys to write, in any order and with repeats taking the last value.
     *  @param[in] stored_values The values back to back, in the order their keys name them.
     *  @param[in] sizes Bytes of every value, one per key, which must add up to @p stored_values.
     */
    status_t insert(any_collection_id_t const& id, std::span<stored_key_t const> stored_keys,
                    std::span<byte_t const> stored_values, std::span<stored_value_size_t const> sizes) noexcept {
        return commit_alone_(id, [&](auto const& collection) noexcept {
            return collection.seal_inserts(stored_keys, stored_values, sizes);
        });
    }

    /** Removes every key of @p stored_keys in @p id, as one commit of its own. */
    status_t remove(any_collection_id_t const& id, std::span<stored_key_t const> stored_keys) noexcept {
        return commit_alone_(id,
                             [&](auto const& collection) noexcept { return collection.seal_erasures(stored_keys); });
    }

#pragma endregion Writes

#pragma region Maintenance

    /** Writes the memory runs of every collection as one run of its level zero. */
    status_t flush() noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        status_t first;
        for (std::size_t index = 0; index != collections_.size(); ++index)
            if (status_t const flushed = std::visit([&](auto& held) noexcept { return held.flush(claimed->channel()); },
                                                    collections_[index]->held);
                flushed.failed() && first.succeeded())
                first = flushed;
        // Every run the flushes wrote is sealed by now, so the catalog may name them.
        if (status_t const saved = save_schema_(claimed->channel()); saved.failed() && first.succeeded()) first = saved;
        return first;
    }

    /** Whether any collection has a level over its bounds, which @c compact then merges. */
    [[nodiscard]] bool needs_compaction() noexcept {
        reader_t const inside = ledger_.epochs().enter();
        for (collection_entry_t const* entry : catalog_entries_())
            if (std::visit([](auto const& collection) noexcept { return collection.needs_compaction(); }, entry->held))
                return true;
        return false;
    }

    /** Merges one overfull level of every collection that has one, which needs two channels at once. */
    status_t compact() noexcept {
        expected<claimed_channel_t> reading = claim_channel_();
        if (!reading) return reading.status();
        auto [writing, acquire_status] = channels_->acquire();
        if (acquire_status.failed()) return acquire_status;

        schema_write_t recording {this, &reading->channel()};
        lsm_tree_publication_hook_t published;
        published.notify = &record_schema_;
        published.context = &recording;

        status_t first;
        for (std::size_t index = 0; index != collections_.size(); ++index) {
            auto [merged, compact_status] = std::visit(
                [&](auto& held) noexcept { return held.compact(reading->channel(), writing.channel(), published); },
                collections_[index]->held);
            if (compact_status.failed() && first.succeeded()) first = compact_status;
        }
        return first;
    }

    /**
     *  @brief Once @p collection_id is due, merges its memory runs by generation, and flushes and compacts them once
     *      they hold their bounds, which every commit runs for the collections it wrote once it lands.
     *
     *  Returns at once while another caller holds the claim, since that holder looks at every collection again after
     *  it lets go, so no commit waits behind another's maintenance.
     *  @note Called outside the ledger's epochs.
     */
    status_t maintain(collection_id_t collection_id) noexcept {
        {
            reader_t const inside = ledger_.epochs().enter();
            collection_entry_t const* const found = entry_with_id_in_(catalog_entries_(), collection_id);
            if (!found ||
                !std::visit([](auto const& collection) noexcept { return collection.maintenance_due(); }, found->held))
                return {};
        }
        claim_guard_t const held = claim_guard_t::taken_if_free(*this);
        if (!held.holding()) return {};
        std::size_t const position = position_at_(any_collection_id_t {collection_id});
        if (position == collections_.size()) return {};
        return maintain_collection_(*collections_[position]);
    }

    /** How many memory runs @p id's collection holds and how many commits they hold, or none for a missing one. */
    [[nodiscard]] memory_shape_t memory_shape(any_collection_id_t const& id) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        collection_entry_t const* const found = entry_at_in_(catalog_entries_(), id);
        if (!found) return {};
        return std::visit([](auto const& collection) noexcept { return collection.memory_shape(); }, found->held);
    }

    /** Pins the newest published stamp, at which every read through the snapshot answers, and flushes nothing. */
    expected<snapshot_t> take_snapshot() noexcept { return snapshot_t::of(*this); }

#pragma endregion Maintenance

#pragma region Transactions

    /** A transaction over every collection, with the write-buffer bounds of @p config. */
    expected<transaction_unique_ptr_t> create_transaction(transaction_config_t const& config) noexcept {
        return transactions_.create_transaction(config);
    }

    /** A transaction with the write-buffer bounds this database was mounted with. */
    expected<transaction_unique_ptr_t> create_transaction() noexcept {
        return create_transaction(config_.default_transaction);
    }

    /** The order every commit of this database draws its stamp from, and the stamps its readers pin. */
    [[nodiscard]] commit_ledger<atomic_reference_, waiting_policy_type_>& ledger() noexcept { return ledger_; }

#pragma endregion Transactions

#pragma region Write Path

    /** Whether a commit newer than @p from wrote @p stored_key of @p collection_id, read from the published view; a
     *  collection gone since counts as written. */
    [[nodiscard]] bool written_since(collection_id_t collection_id, stored_key_t stored_key,
                                     transaction_stamp_t from) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        collection_entry_t const* const found = entry_with_id_in_(catalog_entries_(), collection_id);
        if (!found) return true;
        return std::visit(
            [&]<typename store_type_>(store_type_ const& collection) noexcept {
                if constexpr (keyed_by_database<store_type_>) return collection.written_since(stored_key, from);
                else return true;
            },
            found->held);
    }

    /** Whether a commit newer than @p from wrote any key of @p collection_id from @p lower to @p upper inclusive,
     *  read from the published view; a collection gone since counts as written. */
    [[nodiscard]] bool range_written_since(collection_id_t collection_id, stored_key_t lower, stored_key_t upper,
                                           transaction_stamp_t from) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        collection_entry_t const* const found = entry_with_id_in_(catalog_entries_(), collection_id);
        if (!found) return true;
        return std::visit(
            [&]<typename store_type_>(store_type_ const& collection) noexcept {
                if constexpr (keyed_by_database<store_type_>) return collection.range_written_since(lower, upper, from);
                else return true;
            },
            found->held);
    }

    /**
     *  @brief Seals what @p changes wrote to @p collection_id into @p sealed on the committing thread, making room
     *      first when the collection's memory runs are past their bounds.
     *  @return @c write_conflict_k for a collection gone since the transaction wrote it.
     */
    status_t seal_collection(collection_id_t collection_id,
                             grouped_write_buffer<stored_key_t, atomic_reference_, waiting_policy_type_> const& changes,
                             sealed_commit<stored_key_t, atomic_reference_>& sealed) noexcept {
        auto const group = changes.groups_by_id.find(collection_id);
        if (group == changes.groups_by_id.end()) return {};
        st::basic_vector<keyed_delta<stored_key_t>, constrained_allocator<keyed_delta<stored_key_t>, atomic_reference_>>
            deltas {
                constrained_allocator<keyed_delta<stored_key_t>, atomic_reference_> {resources_->memory_constraint}};
        if (deltas.reserve(group.value().stored_values.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to hand a commit over"};
        [[maybe_unused]] st::status_t const walked = group.value().stored_values.for_each(
            [&](auto const& entry) noexcept {
                auto const& place = entry.value();
                delta_view_t delta {place.kind, {}};
                if (place.size != 0)
                    delta.bytes = as_bytes_view(changes.insertions_buffer).subspan(place.offset, place.size);
                [[maybe_unused]] st::status_t const placed = deltas.push_back(
                    st::assume_reserved, keyed_delta<stored_key_t> {entry.key(), delta});
            });
        return seal_into_(collection_id, sealed, [&](auto const& collection) noexcept {
            return collection.seal(std::span<keyed_delta<stored_key_t>> {deltas});
        });
    }

    /**
     *  @brief Lays out the view @p sealed's run is published in, on the thread holding the leader word.
     *  @return @c write_conflict_k for a collection gone since the transaction wrote it.
     */
    status_t prepare_collection(collection_id_t collection_id,
                                sealed_commit<stored_key_t, atomic_reference_>& sealed) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        collection_entry_t* const found = entry_with_id_in_(catalog_entries_(), collection_id);
        if (!found) return {status_code_t::write_conflict_k, "A collection the transaction wrote is gone"};
        return std::visit(
            [&]<typename store_type_>(store_type_& collection) noexcept -> status_t {
                if constexpr (keyed_by_database<store_type_>) return collection.prepare(sealed);
                else return keyed_otherwise_();
            },
            found->held);
    }

    /** Publishes @p sealed under @p stamp in the view @c prepare_collection laid out, before the stamp itself is,
     *  on the thread holding the leader word, which a drop waits for. */
    void publish_collection(collection_id_t collection_id, sealed_commit<stored_key_t, atomic_reference_>& sealed,
                            transaction_stamp_t stamp) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        collection_entry_t* const found = entry_with_id_in_(catalog_entries_(), collection_id);
        std::visit(
            [&]<typename store_type_>(store_type_& collection) noexcept {
                if constexpr (keyed_by_database<store_type_>) collection.publish(sealed, stamp);
            },
            found->held);
    }

    /** Forgets the view @c prepare_collection laid out for a commit the leader refuses. */
    void forget_collection(collection_id_t collection_id) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        if (collection_entry_t* const found = entry_with_id_in_(catalog_entries_(), collection_id))
            std::visit([](auto& collection) noexcept { collection.forget_prepared(); }, found->held);
    }

    /** Calls @p visitor with the store @p id's collection lives in, inside the ledger's epochs, which keep the store
     *  alive until it returns, and answers what it answers, or @c not_found_k for a collection the database names not;
     *  @p visitor takes every store this database holds and answers a status. */
    template <typename visitor_type_>
    status_t visit_store(any_collection_id_t const& id, visitor_type_&& visitor,
                         std::size_t thread_index = outside_executor_k) noexcept {
        reader_t const inside = ledger_.epochs().enter(thread_index);
        collection_entry_t* const found = entry_at_in_(catalog_entries_(), id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        return std::visit(std::forward<visitor_type_>(visitor), found->held);
    }

    /** Flushes @p collection_id's memory runs while @p sealed, which a store keying its collections otherwise sealed
     *  for a transaction's share, would leave them past twice their bounds, as the database's own commits do. */
    template <typename sealed_type_>
    status_t make_room(collection_id_t collection_id, sealed_type_ const& sealed) noexcept {
        return make_room_(collection_id, sealed);
    }

    /** The queue pairs every call that moves bytes borrows one of, a read of a store keying its collections otherwise
     *  included. */
    [[nodiscard]] channel_pool_t& channels() noexcept { return *channels_; }

#pragma endregion Write Path

#pragma region User Data

    /** Bytes the caller's own blob takes, which is zero when it has saved none. */
    [[nodiscard]] std::uint64_t user_data_size_bytes() noexcept {
        [[maybe_unused]] claim_guard_t const held {*this};
        expected<volume_blob_t> const found = router_->find(database_user_data_name());
        return found ? found->length : 0;
    }

    /** Replaces the caller's own blob with @p bytes, which the engine never reads. */
    status_t save_user_data(bytes_view_t bytes) noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        return write_blob_(claimed->channel(), database_user_data_name(), blob_kind_t::user_data_k, bytes);
    }

    /** Reads the caller's own blob into @p into, which must hold @c user_data_size_bytes of it. */
    status_t load_user_data(bytes_span_t into) noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        return read_blob_(claimed->channel(), database_user_data_name(), into);
    }

    /** Forgets the caller's own blob and returns its bytes to the volume that held it. */
    status_t clear_user_data() noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        return router_->drop(claimed->channel(), database_user_data_name());
    }

#pragma endregion User Data

  private:

#pragma region Private Types

    /** One catalog entry, heap-pinned so a snapshot and a live transaction may name it. */
    using entry_pointer_t = std::unique_ptr<collection_entry_t>;

    /**
     *  @brief One published catalog, naming every entry the database held when it was laid out.
     *
     *  A lookup that reads nothing but a name, an ID or a shape needs no claim: every entry is pinned on the heap, so
     *  a create and a drop publish only a new list naming them, and free the one it replaces once every reader that
     *  loaded it has left.
     */
    struct catalog_view_t {

        /** The entries, in the order @c collections_ held them. */
        st::basic_vector<collection_entry_t*> entries;

        /** Its room in the ledger's list of what the leader unpublished, once a publication replaces it. */
        unpublished_t unpublished;
    };

    /** One reader inside the ledger's epochs, announced until it dies. */
    using reader_t = typename epoch_domain<atomic_reference_, waiting_policy_type_>::reader_t;

    /**
     *  @brief The database's one claim, held for the life of the guard.
     *
     *  It orders whatever rewrites the catalog, a tree or a blob: creates, drops, the maintenance commits pay for, the
     *  explicit flushes and compactions, and the caller's own blob. No read takes it. A commit takes it once it landed,
     *  or returns at once when another caller has it, since every holder looks at the views again after it lets go.
     */
    class claim_guard_t {
      public:

        claim_guard_t() noexcept = default;
        claim_guard_t(claim_guard_t const&) = delete;
        claim_guard_t& operator=(claim_guard_t const&) = delete;

        /** Takes @p owner's claim, parked while another caller holds it. */
        explicit claim_guard_t(basic_database& owner) noexcept : owner_(&owner) { owner.claim_.take(); }

        /** Takes @p owner's claim if no caller holds it, or answers a guard holding nothing. */
        [[nodiscard]] static claim_guard_t taken_if_free(basic_database& owner) noexcept {
            claim_guard_t guard;
            if (owner.claim_.take_if_free()) guard.owner_ = &owner;
            return guard;
        }

        claim_guard_t(claim_guard_t&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}

        claim_guard_t& operator=(claim_guard_t&& other) noexcept {
            if (this == &other) return *this;
            release_();
            owner_ = std::exchange(other.owner_, nullptr);
            return *this;
        }

        ~claim_guard_t() noexcept { release_(); }

        /** Whether this guard holds the claim. */
        [[nodiscard]] bool holding() const noexcept { return owner_ != nullptr; }

      private:

        /** Hands the claim back, then maintains whatever a commit left due meanwhile. */
        void release_() noexcept {
            if (owner_) std::exchange(owner_, nullptr)->release_claim_();
        }

        /** The database whose claim this guard holds, null once moved from or never taken. */
        basic_database* owner_ = nullptr;
    };

    /** The claim and the channel one call of this database runs under, released in that order. */
    class claimed_channel_t {
      public:

        claimed_channel_t(claim_guard_t&& claim, channel_guard_t&& channel) noexcept
            : claim_(std::move(claim)), channel_(std::move(channel)) {}

        claimed_channel_t(claimed_channel_t&&) noexcept = default;
        claimed_channel_t& operator=(claimed_channel_t&&) noexcept = default;

        /** The channel, which only the thread holding this is on. */
        [[nodiscard]] io_channel_t& channel() const noexcept { return channel_.channel(); }

      private:

        /** The database's claim, given back after the channel. */
        claim_guard_t claim_;

        /** The borrowed queue pair, drained and given back first. */
        channel_guard_t channel_;
    };

#pragma endregion Private Types

#pragma region Private Methods

    /** Whether @p store_type_ keeps its collections under this database's key, so the calls naming one reach it. */
    template <typename store_type_>
    static constexpr bool keyed_by_database = std::is_same_v<typename store_type_::stored_key_t, stored_key_t>;

    /** What a call naming a key of this database answers for a collection keyed otherwise, which only the calls of
     *  its own store reach. */
    static status_t keyed_otherwise_() noexcept {
        return {status_code_t::invalid_argument_k, "That collection is keyed otherwise"};
    }

    /**
     *  @brief The entries of the catalog published last, empty before the mount publishes one.
     *  @note A caller inside the ledger's epochs walks them until it leaves; one holding the claim reads them in step
     *      with @c collections_, since only a claim holder publishes.
     */
    [[nodiscard]] std::span<collection_entry_t* const> catalog_entries_() const noexcept {
        catalog_view_t const* const published = catalog_view_.load();
        if (!published) return {};
        return published->entries;
    }

    /**
     *  @brief Lays out a catalog naming every entry of @c collections_, publishes it under the leader word, and frees
     *      the one it replaces as it lets the word go, once every reader that could hold it has left.
     *  @note Only a caller holding the claim and inside no epoch publishes, so an entry the new catalog no longer names
     *      may be destroyed as soon as this returns.
     */
    status_t publish_catalog_() noexcept {
        std::unique_ptr<catalog_view_t> next {new (std::nothrow) catalog_view_t {}};
        if (!next || next->entries.reserve(collections_.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to publish the catalog"};
        for (entry_pointer_t const& entry : collections_) [[maybe_unused]]
            st::status_t const named = next->entries.push_back(st::assume_reserved, entry.get());
        ledger_.lead();
        atomic_reference_<std::uint64_t> const version {catalog_version_};
        version.store(version.load(std::memory_order_relaxed) + 1, std::memory_order_release);
        catalog_view_.replace(*next);
        if (catalog_view_t* const older = std::exchange(catalog_, std::move(next)).release())
            ledger_.unpublish(older->unpublished, older, &destroy_catalog_);
        ledger_.stop_leading();
        return {};
    }

    static void destroy_catalog_(void* catalog) noexcept { delete static_cast<catalog_view_t*>(catalog); }

    /** The entry @p collection_id names in @p entries, or null when they name none. */
    [[nodiscard]] static collection_entry_t* entry_with_id_in_(std::span<collection_entry_t* const> entries,
                                                               collection_id_t collection_id) noexcept {
        for (collection_entry_t* entry : entries)
            if (entry->id == collection_id) return entry;
        return nullptr;
    }

    /** The entry @p name answers to in @p entries, or null when they name none. */
    [[nodiscard]] static collection_entry_t* entry_named_in_(std::span<collection_entry_t* const> entries,
                                                             string_view_t name) noexcept {
        for (collection_entry_t* entry : entries)
            if (entry->name.view() == name) return entry;
        return nullptr;
    }

    /** The entry @p id names in @p entries, or null when they name none. */
    [[nodiscard]] static collection_entry_t* entry_at_in_(std::span<collection_entry_t* const> entries,
                                                          any_collection_id_t const& id) noexcept {
        if (collection_id_t const* const identified = std::get_if<collection_id_t>(&id))
            return entry_with_id_in_(entries, *identified);
        return entry_named_in_(entries, std::get<collection_alphanumeric_id_t>(id).view());
    }

    /** One head write's commit, as the sequencer hands it to its two steps. */
    struct head_commit_t {

        /** The database committed to. */
        basic_database* store = nullptr;

        /** The collection written, which the leader looks up again. */
        collection_id_t id = collection_invalid_id_k;

        /** Its records and the run they become. */
        sealed_commit<stored_key_t, atomic_reference_> sealed;
    };

    static status_t prepare_head_(void* context) noexcept {
        head_commit_t& head = *static_cast<head_commit_t*>(context);
        return head.store->prepare_collection(head.id, head.sealed);
    }

    static void publish_head_(void* context, transaction_stamp_t stamp) noexcept {
        head_commit_t& head = *static_cast<head_commit_t*>(context);
        head.store->publish_collection(head.id, head.sealed, stamp);
    }

    /** Lands what @p sealer seals out of @p id's collection as a commit of its own, through the sequencer. */
    template <typename sealer_type_>
    status_t commit_alone_(any_collection_id_t const& id, sealer_type_&& sealer) noexcept {
        head_commit_t head;
        head.store = this;
        {
            reader_t const inside = ledger_.epochs().enter();
            collection_entry_t const* const found = entry_at_in_(catalog_entries_(), id);
            if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
            head.id = found->id;
        }
        if (status_t const sealed = seal_into_(head.id, head.sealed, sealer); sealed.failed()) return sealed;
        if (!head.sealed.run) return {};
        commit_request_t request;
        request.context = &head;
        request.prepare = &prepare_head_;
        request.publish = &publish_head_;
        if (status_t const committed = ledger_.submit(request).status(); committed.failed()) return committed;
        // The commit has landed, so a failure to maintain is met again by the next commit making room.
        [[maybe_unused]] status_t const maintained = maintain(head.id);
        return {};
    }

    /** Seals with @p sealer into @p sealed out of @p collection_id's collection, then makes room while its memory
     *  runs are past twice their bounds, inside the ledger's epochs only while sealing. */
    template <typename sealer_type_>
    status_t seal_into_(collection_id_t collection_id, sealed_commit<stored_key_t, atomic_reference_>& sealed,
                        sealer_type_&& sealer) noexcept {
        {
            reader_t const inside = ledger_.epochs().enter();
            collection_entry_t const* const found = entry_with_id_in_(catalog_entries_(), collection_id);
            if (!found) return {status_code_t::write_conflict_k, "A collection the transaction wrote is gone"};
            status_t const status = std::visit(
                [&]<typename store_type_>(store_type_ const& collection) noexcept -> status_t {
                    if constexpr (keyed_by_database<store_type_>) {
                        auto [taken, seal_status] = sealer(collection);
                        if (seal_status.failed()) return seal_status;
                        sealed = std::move(taken);
                        return {};
                    }
                    else return keyed_otherwise_();
                },
                found->held);
            if (status.failed()) return status;
        }
        return make_room_(collection_id, sealed);
    }

    /**
     *  @brief Flushes @p collection_id's memory runs while @p sealed, which the collection's store sealed, would leave
     *      them past twice their bounds, holding no epoch and parking rather than spinning while another caller holds
     *      the claim.
     *  @return Success once the commit fits, or why a flush could not make room.
     */
    template <typename sealed_type_>
    status_t make_room_(collection_id_t collection_id, sealed_type_ const& sealed) noexcept {
        if (!sealed.run) return {};
        for (;;) {
            {
                reader_t const inside = ledger_.epochs().enter();
                collection_entry_t const* const found = entry_with_id_in_(catalog_entries_(), collection_id);
                if (!found) return {status_code_t::write_conflict_k, "A collection the transaction wrote is gone"};
                memory_pressure_t const pressure = std::visit(
                    [&](auto const& collection) noexcept {
                        if constexpr (requires { collection.memory_pressure(sealed); })
                            return collection.memory_pressure(sealed);
                        else return memory_pressure_t::fits_k;
                    },
                    found->held);
                if (pressure != memory_pressure_t::stalled_k) return {};
            }
            claim_guard_t const held = claim_guard_t::taken_if_free(*this);
            if (!held.holding()) {
                // Whoever holds the claim may be flushing this very collection, so the pressure is read again after.
                claim_.wait_until_released();
                continue;
            }
            std::size_t const position = position_at_(any_collection_id_t {collection_id});
            if (position == collections_.size())
                return {status_code_t::write_conflict_k, "A collection the transaction wrote is gone"};
            if (status_t const flushed = std::visit(
                    [&](auto& collection) noexcept { return flush_and_compact_(collection); },
                    collections_[position]->held);
                flushed.failed())
                return flushed;
        }
    }

    /** Flushes @p collection's memory runs, saves the catalog naming the run, and compacts the tree for as long as the
     *  flush left a level over its bounds, holding the claim. */
    template <typename collection_type_>
    status_t flush_and_compact_(collection_type_& collection) noexcept {
        auto [reading, reading_status] = channels_->acquire();
        if (reading_status.failed()) return reading_status;
        if (status_t const flushed = collection.flush(reading.channel()); flushed.failed()) return flushed;
        if (status_t const saved = save_schema_(reading.channel()); saved.failed()) return saved;
        if (!collection.needs_compaction()) return {};
        auto [writing, writing_status] = channels_->acquire();
        if (writing_status.failed()) return writing_status;
        schema_write_t recording {this, &reading.channel()};
        lsm_tree_publication_hook_t published;
        published.notify = &record_schema_;
        published.context = &recording;
        while (collection.needs_compaction()) {
            auto [outcome, compact_status] = collection.compact(reading.channel(), writing.channel(), published);
            if (compact_status.failed()) return compact_status;
            if (outcome == compaction_outcome_t::nothing_to_compact_k) break;
        }
        return {};
    }

    /** Merges @p entry's memory runs by generation, and flushes and compacts them once they hold their bounds, holding
     *  the claim. */
    status_t maintain_collection_(collection_entry_t& entry) noexcept {
        return std::visit(
            [&](auto& collection) noexcept -> status_t {
                if (status_t const merged = collection.merge_memory_runs(); merged.failed()) return merged;
                if (!collection.flush_due()) return {};
                return flush_and_compact_(collection);
            },
            entry.held);
    }

    /** Maintains every collection, holding the claim; answers the first failure, after trying every collection. */
    status_t maintain_due_() noexcept {
        status_t first;
        for (entry_pointer_t const& entry : collections_)
            if (status_t const maintained = maintain_collection_(*entry); maintained.failed() && first.succeeded())
                first = maintained;
        return first;
    }

    /** Whether any collection the catalog names is due for a merge or a flush, read from the published views. */
    [[nodiscard]] bool maintenance_due_() noexcept {
        reader_t const inside = ledger_.epochs().enter();
        for (collection_entry_t const* entry : catalog_entries_())
            if (std::visit([](auto const& collection) noexcept { return collection.maintenance_due(); }, entry->held))
                return true;
        return false;
    }

    /**
     *  @brief Hands the claim back, then looks once more at every collection and, if one is due and the claim is
     *      still free, takes it back to maintain them, so a commit that found the claim held left its maintenance to
     *      this caller.
     *
     *  A caller that finds the claim taken again stops, since the new holder looks once more after it lets go, and a
     *  failed maintenance is met again by the next commit.
     */
    void release_claim_() noexcept {
        claim_.release();
        // One look covers a commit that found the claim held; work due after it waits for its collection's next commit.
        if (!maintenance_due_() || !claim_.take_if_free()) return;
        [[maybe_unused]] status_t const maintained = maintain_due_();
        claim_.release();
    }

    /** Takes the claim and then a shared channel, which every call that moves bytes runs under. */
    expected<claimed_channel_t> claim_channel_() noexcept {
        claim_guard_t claim {*this};
        auto [guard, acquire_status] = channels_->acquire();
        if (acquire_status.failed()) return acquire_status;
        return claimed_channel_t {std::move(claim), std::move(guard)};
    }

    /** Where @p id sits in the catalog, or the catalog's size when it names nothing. */
    [[nodiscard]] std::size_t position_at_(any_collection_id_t const& id) const noexcept {
        if (collection_id_t const* const identified = std::get_if<collection_id_t>(&id)) {
            for (std::size_t index = 0; index != collections_.size(); ++index)
                if (collections_[index]->id == *identified) return index;
            return collections_.size();
        }
        collection_alphanumeric_id_t const& named = std::get<collection_alphanumeric_id_t>(id);
        for (std::size_t index = 0; index != collections_.size(); ++index)
            if (collections_[index]->name == named) return index;
        return collections_.size();
    }

    /** Mounts @p entry's collection on its volume in the first of @c stores_ that serves its modality in the value
     *  layout its value size picks, refusing a collection no store serves. */
    status_t mount_collection_(collection_entry_t& entry) noexcept {
        value_layout_t const layout = resolve_value_layout(entry.config.lsm_tree.lsm_level_0.sorted_run.value_size);
        status_t mounted {status_code_t::unsupported_k, "No store serves a collection of that modality and layout"};
        bool picked = false;
        auto rung = [&]<typename store_type_>(std::type_identity<store_type_>) noexcept {
            if (picked || !store_type_::serves(entry.modality, layout)) return;
            picked = true;
            auto [store, mount_status] = store_type_::mount(router_->volume(entry.volume), entry.config,
                                                            resources_->memory_constraint, ledger_);
            mounted = mount_status;
            if (mount_status.succeeded()) entry.held.template emplace<store_type_>(std::move(store));
        };
        (rung(std::type_identity<stores_> {}), ...);
        return mounted;
    }

    /**
     *  @brief Replaces the blob @p name with @p bytes, sealing it only once they are durable.
     *  @param[inout] channel The channel the bytes are staged and flushed on, held by the calling thread.
     *  @param[in] name The bytes the blob is named by, which no other blob of this database holds.
     *  @param[in] kind What the blob is, which is how a later mount enumerates it.
     *  @param[in] bytes The whole image, which must fit in one block of @p channel.
     */
    status_t write_blob_(io_channel_t& channel, blob_name_t name, blob_kind_t kind,
                         std::span<byte_t const> bytes) noexcept {
        if (bytes.empty()) return {status_code_t::invalid_argument_k, "A named blob carries at least one byte"};
        std::uint64_t const written = round_up_to_multiple(bytes.size(), portable_alignment_k);
        if (written > channel.requests().buffer_bytes())
            return {status_code_t::capacity_exhausted_k, "That blob is longer than one block of a channel"};

        [[maybe_unused]] status_t const forgotten = router_->drop(channel, name);
        auto [reserved, create_status] = router_->create(name, kind, bytes.size());
        if (create_status.failed()) return create_status;
        if (reserved.extent.length < written)
            return {status_code_t::capacity_exhausted_k, "A volume reserved less than one page for that blob"};

        volume_t& volume = router_->volume(reserved.volume);
        basic_write_pipeline<disk_t, waiting_policy_type_> writer {volume.disk(), channel, reserved.medium};
        auto [block, acquire_buffer_status] = writer.acquire_buffer();
        if (acquire_buffer_status.failed()) return acquire_buffer_status;
        std::span<byte_t> const room = block.bytes();
        std::memset(room.data(), 0, static_cast<std::size_t>(written));
        std::memcpy(room.data(), bytes.data(), bytes.size());
        if (status_t const queued = writer.enqueue_write(std::move(block), reserved.slice(0, written)); queued.failed())
            return queued;
        if (status_t const durable = writer.make_durable(); durable.failed()) return durable;
        return volume.seal(channel, reserved);
    }

    /** Reads the whole blob @p name into @p into, which must hold its content length. */
    status_t read_blob_(io_channel_t& channel, blob_name_t name, std::span<byte_t> into) const noexcept {
        auto [found, find_status] = router_->find(name);
        if (find_status.failed()) return find_status;
        if (into.size() < found.length) return {status_code_t::invalid_argument_k, "A buffer shorter than the blob"};
        std::uint64_t const read = round_up_to_multiple(found.length, portable_alignment_k);
        if (read > channel.requests().buffer_bytes())
            return {status_code_t::capacity_exhausted_k, "That blob is longer than one block of a channel"};

        volume_t& volume = router_->volume(found.volume);
        basic_read_pipeline<disk_t> reader {volume.disk(), channel, found.medium};
        if (status_t const offered = reader.enqueue_read(found.slice(0, read), 0); offered.failed()) return offered;
        if (status_t const flushed = reader.submit(); flushed.failed()) return flushed;
        status_t carried;
        auto sink = [&](completed_buffer_t const& completed) noexcept {
            if (status_t const whole = require_completed_in_full(completed); whole.failed()) {
                carried = whole;
                return;
            }
            std::memcpy(into.data(), completed.bytes.data(), static_cast<std::size_t>(found.length));
        };
        if (status_t const polled = reader.poll_all_completions(sink); polled.failed()) return polled;
        return carried;
    }

    /**
     *  @brief Writes the catalog out over its older copy, which every change to it does.
     *
     *  The durable stamp is read before the runs are listed: every commit at or below it is in some run already, and
     *  a run a flush adds in between only holds commits above it, which a mount of this copy drops.
     */
    status_t save_schema_(io_channel_t& channel) noexcept {
        catalog_stamps_t stamps;
        stamps.durable = ledger_.durable_stamp();
        stamps.generation = generation_ + 1;
        st::basic_vector<collection_record_t> records;
        st::basic_vector<lsm_tree_run_record_t> runs;
        if (records.reserve(collections_.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to lay the catalog out"};
        for (std::size_t index = 0; index != collections_.size(); ++index) {
            collection_entry_t const& entry = *collections_[index];
            collection_record_t record;
            record.id = entry.id;
            std::size_t const named_before = runs.size();
            if (status_t const named = std::visit(
                    [&](auto const& held) noexcept { return record_lsm_tree_runs(held.tree(), runs); }, entry.held);
                named.failed())
                return named;
            record.run_count = runs.size() - named_before;
            if (entry.name.size() > collection_name_max_bytes_k)
                return {status_code_t::capacity_exhausted_k, "A collection name outgrew the catalog's record"};
            std::memcpy(record.name_storage.data(), entry.name.data(), entry.name.size());
            record.name_bytes = static_cast<std::uint16_t>(entry.name.size());
            record.volume = static_cast<std::uint16_t>(entry.volume);
            record.modality = static_cast<std::uint16_t>(entry.modality);
            record.value_size = entry.config.lsm_tree.lsm_level_0.sorted_run.value_size;
            record.value_max_size = entry.config.lsm_tree.lsm_level_0.sorted_run.value_max_size;
            record.level_enlarge_factor = entry.config.lsm_tree.level_enlarge_factor;
            record.cache_limit = entry.config.cache_limit;
            record.write_buffer_max_bytes = entry.config.write_buffer_max_bytes;
            record.write_buffer_max_elements = entry.config.write_buffer_max_elements;
            if (records.push_back(std::move(record)) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "No room to lay the catalog out"};
        }

        std::uint64_t const version = database_schema_version_k;
        st::basic_vector<byte_t> image;
        if (image.resize(fields_storage_requirements(version, stamps, records, runs)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to lay the catalog out"};
        if (status_t const laid = save_fields(bytes_span_t {image}, version, stamps, records, runs); laid.failed())
            return laid;
        if (status_t const written = write_blob_(channel, database_schema_name(catalog_copy_of(stamps.generation)),
                                                 blob_kind_t::schema_k, std::span<byte_t const> {image});
            written.failed())
            return written;
        generation_ = stamps.generation;
        return {};
    }

    /** Reads copy @p copy of the catalog into its fields, or leaves @p stamps at generation zero when it is absent. */
    status_t read_schema_copy_(io_channel_t& channel, std::size_t copy, catalog_stamps_t& stamps,
                               st::basic_vector<collection_record_t>& records,
                               st::basic_vector<lsm_tree_run_record_t>& runs) const noexcept {
        auto [found, find_status] = router_->find(database_schema_name(copy));
        if (find_status.failed()) return find_status == status_code_t::not_found_k ? status_t {} : find_status;

        st::basic_vector<byte_t> image;
        if (image.resize(static_cast<std::size_t>(found.length)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to read the catalog into"};
        if (status_t const read = read_blob_(channel, database_schema_name(copy), std::span<byte_t> {image});
            read.failed())
            return read;

        std::uint64_t version = 0;
        if (status_t const parsed = load_fields(bytes_view_t {image}, version, stamps, records, runs); parsed.failed())
            return parsed;
        if (version != database_schema_version_k)
            return {status_code_t::unsupported_k, "The catalog follows another schema version"};
        if (stamps.generation == 0) return {status_code_t::malformed_k, "A saved catalog counts at least one save"};
        return {};
    }

    /** Opens every collection the catalog names, drops the versions a crash tore and publishes the catalog, which a
     *  mount does once, leaving nothing published when it fails. */
    status_t open_catalog_() noexcept {
        // Every executor thread with a channel of its own reads on an epoch slot of its own too.
        if (status_t const reserved = ledger_.epochs().reserve_threads(channels_->executor_channels());
            reserved.failed())
            return reserved;
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        if (status_t const loaded = load_schema_(claimed->channel()); loaded.failed()) return loaded;

        // The stamps continue above the durable stamp, and every version above it is a commit a crash tore, which
        // is dropped before anything draws a stamp; resuming first is what lets the truncation's catalog promise it.
        if (status_t const resumed = ledger_.resume(loaded_durable_); resumed.failed()) return resumed;
        auto [writing, acquire_status] = channels_->acquire();
        if (acquire_status.failed()) return acquire_status;
        schema_write_t recording {this, &claimed->channel()};
        lsm_tree_publication_hook_t published;
        published.notify = &record_schema_;
        published.context = &recording;
        for (std::size_t index = 0; index != collections_.size(); ++index)
            if (status_t const truncated = std::visit(
                    [&](auto& held) noexcept {
                        return held.truncate_above(claimed->channel(), writing.channel(), loaded_durable_, published);
                    },
                    collections_[index]->held);
                truncated.failed())
                return truncated;
        return publish_catalog_();
    }

    /** Opens every collection the schema blob names, which is what a mount of an older database does. */
    status_t load_schema_(io_channel_t& channel) noexcept {
        // A copy is sealed only once durable, so whichever copies a crash left are whole, and the newer one wins.
        std::array<catalog_stamps_t, 2> stamps {};
        std::array<st::basic_vector<collection_record_t>, 2> records;
        std::array<st::basic_vector<lsm_tree_run_record_t>, 2> runs_of;
        for (std::size_t copy = 0; copy != 2; ++copy)
            if (status_t const read = read_schema_copy_(channel, copy, stamps[copy], records[copy], runs_of[copy]);
                read.failed())
                return read;
        std::size_t const newer = stamps[1].generation > stamps[0].generation ? 1 : 0;
        if (stamps[newer].generation == 0) return {};
        generation_ = stamps[newer].generation;
        loaded_durable_ = stamps[newer].durable;
        return adopt_all_(channel, records[newer], runs_of[newer]);
    }

    /** Opens every collection @p records names over the runs @p runs carries for them, in order. */
    status_t adopt_all_(io_channel_t& channel, st::basic_vector<collection_record_t> const& records,
                        st::basic_vector<lsm_tree_run_record_t> const& runs) noexcept {
        if (collections_.reserve(records.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room for the collections the catalog names"};
        std::size_t named = 0;
        for (std::size_t index = 0; index != records.size(); ++index) {
            if (records[index].run_count > runs.size() - named)
                return {status_code_t::malformed_k, "The catalog names more runs than it carries"};
            std::size_t const count = static_cast<std::size_t>(records[index].run_count);
            if (status_t const opened = adopt_(channel, records[index],
                                               std::span<lsm_tree_run_record_t const> {runs.data() + named, count});
                opened.failed())
                return opened;
            named += count;
        }
        if (named != runs.size()) return {status_code_t::malformed_k, "The catalog carries runs no collection claims"};
        return {};
    }

    /** Opens the collection @p record describes over the runs @p runs names, and takes it into the catalog. */
    status_t adopt_(io_channel_t& channel, collection_record_t const& record,
                    std::span<lsm_tree_run_record_t const> runs) noexcept {
        if (static_cast<std::size_t>(record.volume) >= router_->size())
            return {status_code_t::malformed_k, "The catalog names a volume this mount did not open"};

        entry_pointer_t entry {new (std::nothrow) collection_entry_t {}};
        if (!entry) return {status_code_t::out_of_memory_k, "No room for another collection"};
        entry->id = record.id;
        if (sz::failed(entry->name.assign(record.name())))
            return {status_code_t::out_of_memory_k, "No room for a collection's name"};
        entry->modality = static_cast<ustore_modality_t>(record.modality);
        entry->config = record.config();
        entry->volume = static_cast<volume_id_t>(record.volume);
        if (status_t const mounted = mount_collection_(*entry); mounted.failed()) return mounted;
        if (status_t const restored = std::visit([&](auto& held) noexcept { return held.restore(channel, runs); },
                                                 entry->held);
            restored.failed())
            return restored;
        if (collections_.push_back(std::move(entry)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room for another collection"};
        return {};
    }

    /** What a compaction's hook is handed, which is the database and the channel it records through. */
    struct schema_write_t {

        /** The database whose catalog the hook writes out. */
        basic_database* store = nullptr;

        /** The channel the schema blob is staged and sealed on, held by the compacting thread. */
        io_channel_t* channel = nullptr;
    };

    /** Writes the catalog out for a compaction that has published, before it drops what it consumed. */
    static status_t record_schema_(void* context) noexcept {
        schema_write_t const& writing = *static_cast<schema_write_t const*>(context);
        return writing.store->save_schema_(*writing.channel);
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The budget every collection and every transaction of this database charges. */
    resources_shared_ptr<atomic_reference_, waiting_policy_type_> resources_;

    /** The stamps every commit of this database draws, a transaction's or a head write's, and the stamps pinned. */
    commit_ledger<atomic_reference_, waiting_policy_type_> ledger_;

    /** The transactions of this database, which names this store for as long as it lives. */
    registry_t transactions_;

    /** How many times the catalog has been saved, which names the copy the next save replaces. */
    std::uint64_t generation_ = 0;

    /** The durable stamp of the catalog the mount read, above which it dropped every version. */
    transaction_stamp_t loaded_durable_ = unwritten_stamp_k;

    /** The volumes every run lands on, null until the database is mounted. */
    router_t* router_ = nullptr;

    /** The queue pairs every call that moves bytes takes one of, null until the database is mounted. */
    channel_pool_t* channels_ = nullptr;

    /** The write-buffer bounds a transaction takes when its caller names none. */
    database_config_t config_ {};

    /** Every collection the catalog names, pinned so a snapshot may point into one. */
    st::basic_vector<entry_pointer_t> collections_;

    /** The catalog readers load, which @c catalog_ owns. */
    published_view<catalog_view_t, atomic_reference_> catalog_view_;

    /** The catalog published last, owned until the publication replacing it frees it. */
    std::unique_ptr<catalog_view_t> catalog_;

    /** How many catalogs have been published, which is what lets a caller memoize a lookup. */
    std::uint64_t catalog_version_ = 0;

    /** Held by whichever call changes the catalog, a tree or a blob, a commit paying for its maintenance included. */
    parking_claim<atomic_reference_, waiting_policy_type_> claim_;

#pragma endregion Private Members
};

#pragma endregion Database

#pragma endregion Database

#pragma region Single Collection Database Transaction State

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiting_policy_type_>
struct single_collection_database_transaction_state {
    using stored_key_t = stored_key_;
    using write_buffer_t = write_buffer<stored_key_t, atomic_reference_, waiting_policy_type_>;
    using read_set_t = read_set<stored_key_t, atomic_reference_, waiting_policy_type_>;

    inline single_collection_database_transaction_state(
        write_buffer_t&& write_buffer, resources_shared_ptr<atomic_reference_, waiting_policy_type_> resources) noexcept
        : changes(std::forward<write_buffer_t>(write_buffer)), read_set(resources) {}

    inline bool empty() const noexcept { return changes.empty() && read_set.empty(); }

    inline void clear() {
        changes.unsafe_clear();
        read_set.clear();
        sealed = sealed_commit<stored_key_t, atomic_reference_> {};
    }

    write_buffer_t changes;
    read_set_t read_set;

    /** The writes sealed by a stage or a commit, emptied once it lands or is refused. */
    sealed_commit<stored_key_t, atomic_reference_> sealed;
};

#pragma endregion Single Collection Database Transaction State

#pragma region Single Collection Database Transaction Registry

template <typename stored_key_, value_layout_t value_layout_>
class single_collection_database;

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
class single_collection_database_transaction;

#pragma region Store Interface

/**
 *  @brief What a registry needs from the single collection beneath it to stage and to commit.
 *
 *  A whole write buffer travels across this boundary rather than one key at a time, because the sole
 *  collection seals the batch in one call and has nothing to group it by. The committer seals on its own thread;
 *  the thread holding the ledger's leader word prepares, publishes, or forgets what it prepared; and the committer
 *  maintains the collection once its commit lands.
 */
template <typename store_type_, typename write_buffer_type_, typename stored_key_type_,
          template <typename> typename atomic_reference_, typename waiting_policy_type_>
concept single_collection_transaction_store_concept = requires(
    store_type_& store, store_type_ const& constant, write_buffer_type_ const& changes,
    sealed_commit<stored_key_type_, atomic_reference_>& sealed, stored_key_type_ stored_key,
    transaction_stamp_t stamp) {
    { store.ledger() } -> std::same_as<commit_ledger<atomic_reference_, waiting_policy_type_>&>;
    { constant.written_since(stored_key, stamp) } -> std::same_as<bool>;
    { store.seal_write_buffer(changes, sealed) } -> std::same_as<status_t>;
    { store.prepare_sealed(sealed) } -> std::same_as<status_t>;
    { store.publish_sealed(sealed, stamp) } -> std::same_as<void>;
    { store.forget_sealed() } -> std::same_as<void>;
    { store.maintain() } -> std::same_as<status_t>;
};

#pragma endregion Store Interface

/**
 *  @brief The transactions of one single-collection database, the stamps they commit under and their conflicts.
 *  @tparam stored_key_ The key the collection is addressed by.
 *  @tparam store_type_ The collection the commits land in, satisfying @c single_collection_transaction_store_concept.
 */
template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
class single_collection_database_transaction_registry {
  public:
#pragma region Types

    using stored_key_t = stored_key_;
    using store_t = store_type_;
    using transaction_t =
        single_collection_database_transaction<stored_key_, store_type_, atomic_reference_, waiting_policy_type_>;
    using transaction_state_t =
        single_collection_database_transaction_state<stored_key_t, atomic_reference_, waiting_policy_type_>;
    using write_buffer_t = typename transaction_state_t::write_buffer_t;
    using transaction_unique_ptr_t = std::unique_ptr<transaction_t>;
    using return_transaction_t = expected<transaction_unique_ptr_t>;

#pragma endregion Types

#pragma region Construction

    /** A registry committing into @p store, which outlives it. */
    inline single_collection_database_transaction_registry(
        store_t& store, resources_shared_ptr<atomic_reference_, waiting_policy_type_> resources) noexcept
        : store_(&store), resources_(std::move(resources)) {}

    inline single_collection_database_transaction_registry(single_collection_database_transaction_registry&&) = delete;
    inline single_collection_database_transaction_registry(single_collection_database_transaction_registry const&) =
        delete;
    inline single_collection_database_transaction_registry& operator=(
        single_collection_database_transaction_registry const&) = delete;
    inline single_collection_database_transaction_registry& operator=(
        single_collection_database_transaction_registry&&) = delete;

#pragma endregion Construction

#pragma region Interface Methods

    /** A transaction whose write buffer @p config bounds and admits values of the widths the collection does. */
    inline return_transaction_t create_transaction(transaction_config_t const& config, std::size_t value_size,
                                                   std::size_t value_max_size) noexcept;

#pragma endregion Interface Methods

  private:
#pragma region Transactions

    friend store_t;
    friend transaction_t;

    /** One transaction's commit, as the sequencer hands it to its two steps. */
    struct commit_t {

        /** The registry committing. */
        single_collection_database_transaction_registry* owner = nullptr;

        /** The transaction's writes, watches and sealed records. */
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
    inline void publish_(transaction_state_t& data, transaction_stamp_t stamp) noexcept;
    static inline status_t prepare_commit_(void* context) noexcept;
    static inline void publish_commit_(void* context, transaction_stamp_t stamp) noexcept;

    /** Pins @p pin at the newest published stamp, which a transaction starts at and reads from. */
    inline transaction_stamp_t request_start_(stamp_pin<atomic_reference_, waiting_policy_type_>& pin) noexcept {
        return store_->ledger().pin_reader(pin);
    }

    /** The stamp a watch taken now is scoped from: every commit at or below it has landed. */
    inline transaction_stamp_t current_stamp_() const noexcept { return store_->ledger().published(); }

    inline status_t watch_now_(transaction_stamp_t start_stamp,
                               std::span<stored_key_t const> stored_keys) const noexcept;

#pragma endregion Transactions

#pragma region Helper Methods

    inline bool conflicting_(transaction_state_t const& data) const noexcept;

#pragma endregion Helper Methods

#pragma region Private Members

    /** The collection every commit of this registry lands in, owned by whoever named it. */
    store_t* store_;

    /** The budget every transaction's writes and watches are charged to. */
    resources_shared_ptr<atomic_reference_, waiting_policy_type_> resources_;

#pragma endregion Private Members
};

#pragma region Interface Methods

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
inline typename single_collection_database_transaction_registry<stored_key_, store_type_, atomic_reference_,
                                                                waiting_policy_type_>::return_transaction_t
single_collection_database_transaction_registry<stored_key_, store_type_, atomic_reference_, waiting_policy_type_>::
    create_transaction(transaction_config_t const& config, std::size_t value_size,
                       std::size_t value_max_size) noexcept {

    write_buffer_t changes(config.write_buffer_max_bytes, config.write_buffer_max_elements, 1, value_size,
                           value_max_size, resources_);
    transaction_unique_ptr_t transaction {new (std::nothrow) transaction_t {*this, std::move(changes), resources_}};
    if (!transaction) return status_t {status_code_t::out_of_memory_k, "No room for another transaction"};
    return transaction;
}

#pragma endregion Interface Methods

#pragma region Transactions

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
inline status_t single_collection_database_transaction_registry<
    stored_key_, store_type_, atomic_reference_,
    waiting_policy_type_>::stage_(transaction_state_t& data, transaction_stamp_t start_stamp,
                                  transaction_stamp_t& stage_stamp) noexcept {

    // Sealed before the leader word is taken, so the sort, the copy and any flush hold up no other committer.
    if (status_t const sealed = seal_(data); sealed.failed()) return sealed;
    store_->ledger().lead();
    stage_stamp = current_stamp_();
    if (status_t const prepared = prepare_(data, start_stamp); prepared.failed()) {
        store_->ledger().stop_leading();
        data.sealed = sealed_commit<stored_key_t, atomic_reference_> {};
        return prepared;
    }
    return status_t {};
}

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
inline transaction_stamp_t single_collection_database_transaction_registry<
    stored_key_, store_type_, atomic_reference_, waiting_policy_type_>::commit_staged_(transaction_state_t&
                                                                                           data) noexcept {

    // The stage prepared the collection's view, so from the stamp on nothing can fail.
    commit_t committing {this, &data, invalid_transaction_stamp_k};
    commit_request_t request;
    request.context = &committing;
    request.publish = &publish_commit_;
    transaction_stamp_t const commit_stamp = store_->ledger().commit_leading(request);
    // The commit has landed, so a failure to maintain is met again by the next commit making room.
    [[maybe_unused]] status_t const maintained = store_->maintain();
    data.sealed = sealed_commit<stored_key_t, atomic_reference_> {};
    return commit_stamp;
}

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
inline expected<transaction_stamp_t> single_collection_database_transaction_registry<
    stored_key_, store_type_, atomic_reference_,
    waiting_policy_type_>::commit_(transaction_state_t& data, transaction_stamp_t start_stamp) noexcept {

    if (status_t const sealed = seal_(data); sealed.failed()) return sealed;
    // Even a commit that wrote nothing draws a stamp, so every commit has a place of its own in the order.
    commit_t committing {this, &data, start_stamp};
    commit_request_t request;
    request.context = &committing;
    request.prepare = &prepare_commit_;
    request.publish = &publish_commit_;
    expected<transaction_stamp_t> committed = store_->ledger().submit(request);
    // The commit has landed, so a failure to maintain is met again by the next commit making room.
    if (committed) [[maybe_unused]]
        status_t const maintained = store_->maintain();
    data.sealed = sealed_commit<stored_key_t, atomic_reference_> {};
    return committed;
}

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
inline void single_collection_database_transaction_registry<
    stored_key_, store_type_, atomic_reference_, waiting_policy_type_>::discard_(transaction_state_t& data) noexcept {
    if (data.sealed.run) store_->forget_sealed();
    store_->ledger().stop_leading();
    data.sealed = sealed_commit<stored_key_t, atomic_reference_> {};
}

/** Seals the transaction's writes on the committing thread, making room first when the memory runs are full. */
template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
inline status_t single_collection_database_transaction_registry<
    stored_key_, store_type_, atomic_reference_, waiting_policy_type_>::seal_(transaction_state_t& data) noexcept {
    data.sealed = sealed_commit<stored_key_t, atomic_reference_> {};
    if (data.changes.empty()) return status_t {};
    return store_->seal_write_buffer(data.changes, data.sealed);
}

/** Checks the watches against the views published so far and lays out the collection's view, on the thread holding
 *  the leader word; a refusal forgets the view. */
template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
inline status_t single_collection_database_transaction_registry<
    stored_key_, store_type_, atomic_reference_,
    waiting_policy_type_>::prepare_(transaction_state_t& data, transaction_stamp_t start_stamp) noexcept {
    // Every watch is scoped from the start or later, so nothing published since the start leaves nothing to check.
    if (current_stamp_() != start_stamp && conflicting_(data))
        return status_t {status_code_t::write_conflict_k, "Keys conflict"};
    if (!data.sealed.run) return status_t {};
    return store_->prepare_sealed(data.sealed);
}

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
inline void single_collection_database_transaction_registry<
    stored_key_, store_type_, atomic_reference_, waiting_policy_type_>::publish_(transaction_state_t& data,
                                                                                 transaction_stamp_t stamp) noexcept {
    if (data.sealed.run) store_->publish_sealed(data.sealed, stamp);
}

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
inline status_t single_collection_database_transaction_registry<
    stored_key_, store_type_, atomic_reference_, waiting_policy_type_>::prepare_commit_(void* context) noexcept {
    commit_t const& committing = *static_cast<commit_t const*>(context);
    return committing.owner->prepare_(*committing.data, committing.start_stamp);
}

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
inline void single_collection_database_transaction_registry<stored_key_, store_type_, atomic_reference_,
                                                            waiting_policy_type_>::publish_commit_(void* context,
                                                                                                   transaction_stamp_t
                                                                                                       stamp) noexcept {
    commit_t const& committing = *static_cast<commit_t const*>(context);
    committing.owner->publish_(*committing.data, stamp);
}

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
inline status_t single_collection_database_transaction_registry<
    stored_key_, store_type_, atomic_reference_, waiting_policy_type_>::watch_now_(transaction_stamp_t start_stamp,
                                                                                   std::span<stored_key_t const>
                                                                                       stored_keys) const noexcept {

    bool written = false;
    for (std::size_t index = 0; index != stored_keys.size() && !written; ++index)
        written = store_->written_since(stored_keys[index], start_stamp);
    if (written) return status_t {status_code_t::write_conflict_k, "Keys conflict"};
    return status_t {};
}

#pragma endregion Transactions

#pragma region Helper Methods

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
inline bool single_collection_database_transaction_registry<
    stored_key_, store_type_, atomic_reference_, waiting_policy_type_>::conflicting_(transaction_state_t const& data)
    const noexcept {

    // First-committer-wins over the watched writes, then every key read or tracked.
    bool conflict = false;
    [[maybe_unused]] st::status_t const written = data.changes.stored_values.for_each(
        [&](auto const& entry) noexcept -> st::walk_control_t {
            transaction_stamp_t const from = entry.value().watch_stamp;
            conflict = from != invalid_transaction_stamp_k && store_->written_since(entry.key(), from);
            return conflict ? st::walk_control_t::halt_k : st::walk_control_t::resume_k;
        });
    if (conflict) return true;
    [[maybe_unused]] st::status_t const watched = data.read_set.stored_keys.for_each(
        [&](auto const& watch) noexcept -> st::walk_control_t {
            conflict = store_->written_since(watch.key(), watch.value().watch_stamp);
            return conflict ? st::walk_control_t::halt_k : st::walk_control_t::resume_k;
        });
    return conflict;
}

#pragma endregion Helper Methods

#pragma endregion Single Collection Database Transaction Registry

#pragma region Single Collection Database Snapshot

#pragma region Snapshot

/**
 *  @brief One moment of a single-collection database: a stamp pinned in its ledger, which every read through the
 *      snapshot answers at.
 *  @tparam store_type_ The database read, which must outlive the snapshot.
 *
 *  Move-only, because the pin belongs to exactly one owner. It holds no view and no level set, so it never delays a
 *  publication: every merge keeps the versions a pinned stamp reads.
 */
template <typename store_type_>
class single_collection_database_snapshot {
  public:

    /** The database read. */
    using store_t = store_type_;

    /** The key the database is addressed by. */
    using stored_key_t = typename store_t::stored_key_t;

    single_collection_database_snapshot() noexcept = default;
    single_collection_database_snapshot(single_collection_database_snapshot const&) = delete;
    single_collection_database_snapshot& operator=(single_collection_database_snapshot const&) = delete;
    single_collection_database_snapshot(single_collection_database_snapshot&&) noexcept = default;
    single_collection_database_snapshot& operator=(single_collection_database_snapshot&&) noexcept = default;

    /** The newest stamp @p store published, pinned in its ledger until the snapshot dies. */
    [[nodiscard]] static single_collection_database_snapshot of(store_t& store) noexcept {
        single_collection_database_snapshot taken;
        taken.store_ = &store;
        taken.stamp_ = store.ledger().pin_reader(taken.pin_);
        return taken;
    }

    /** The stamp every read through this snapshot answers at, which every commit inside it is at or below. */
    [[nodiscard]] transaction_stamp_t stamp() const noexcept { return stamp_; }

    /**
     *  @brief Whether the database carried @p stored_key at the snapshot's stamp, and how many bytes its value took.
     *  @param[in] stored_key The key to look for.
     *  @param[out] size Bytes the value takes, which is zero for anything but @c success_k.
     */
    [[nodiscard]] select_result_t find(stored_key_t stored_key, stored_value_size_t& size) const noexcept {
        return store_->find(stored_key, size, stamp_);
    }

    /**
     *  @brief Copies the value of @p stored_key into @p into, which needs only the value's own bytes.
     *  @param[inout] channel The channel the read is submitted and completed on, held by the calling thread.
     *  @param[in] stored_key The key to read.
     *  @param[out] into Where the value lands, which need not be aligned and need not be registered.
     *  @param[out] size Bytes written, which is zero for every failure.
     */
    status_t select(io_channel_t& channel, stored_key_t stored_key, std::span<byte_t> into,
                    stored_value_size_t& size) noexcept {
        return store_->select(channel, stored_key, into, size, stamp_);
    }

    /**
     *  @brief Reads the values of @p stored_keys, refilling the channel as completions retire.
     *  @param[inout] channel The channel the reads are submitted and completed on, held by the calling thread.
     *  @param[in] stored_keys The keys to read, in any order and with any number of misses among them.
     *  @param[out] results What every key answered, which separates a miss from an erasure.
     *  @param[in] sink Called as @c sink(request, offset, bytes) for every key that locates a value.
     */
    template <typename sink_type_>
    status_t select(io_channel_t& channel, std::span<stored_key_t const> stored_keys,
                    std::span<select_result_t> results, sink_type_&& sink) const noexcept {
        return store_->select(channel, stored_keys, results, std::forward<sink_type_>(sink), stamp_);
    }

  private:

    /** The database read, borrowed and outliving the snapshot. */
    store_t* store_ = nullptr;

    /** The pin that keeps every version at the stamp. */
    typename store_t::pin_t pin_;

    /** The stamp pinned. */
    transaction_stamp_t stamp_ = unwritten_stamp_k;
};

#pragma endregion Snapshot

#pragma endregion Single Collection Database Snapshot

#pragma region Single Collection Database Transaction

#pragma region Transaction

/**
 *  @brief An indivisible group of reads and writes over one single-collection database.
 *
 *  Reusable after a @c commit and after a @c rollback, both of which start it again from the moment
 *  they ran. Nothing throws: every modifying call answers a status, and a caller reading
 *  @c status_code_t::write_conflict_k rolls back and does the work again.
 */
template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_>
class single_collection_database_transaction {
  public:

#pragma region Types

    /** The key the database is addressed by. */
    using stored_key_t = stored_key_;

    /** The database every commit of this transaction lands in. */
    using store_t = store_type_;

    /** The registry that orders this transaction against every other one of the database. */
    using registry_t = single_collection_database_transaction_registry<stored_key_, store_type_, atomic_reference_,
                                                                       waiting_policy_type_>;

    /** The uncommitted changes and the watched keys of this transaction. */
    using transaction_state_t =
        single_collection_database_transaction_state<stored_key_t, atomic_reference_, waiting_policy_type_>;

    /** The write buffer the uncommitted changes live in. */
    using write_buffer_t = typename transaction_state_t::write_buffer_t;

#pragma endregion Types

#pragma region Construction

    single_collection_database_transaction(single_collection_database_transaction const&) = delete;
    single_collection_database_transaction& operator=(single_collection_database_transaction const&) = delete;
    single_collection_database_transaction(single_collection_database_transaction&&) = delete;
    single_collection_database_transaction& operator=(single_collection_database_transaction&&) = delete;

    /**
     *  @brief A transaction started at the newest stamp @p manager has published, which it pins until it dies.
     *  @param[inout] manager The registry this transaction stages and commits through, outliving it.
     *  @param[in] changes The write buffer the uncommitted writes land in, moved in and owned from here on.
     *  @param[in] resources The budget the watched keys are charged to.
     */
    single_collection_database_transaction(
        registry_t& manager, write_buffer_t&& changes,
        resources_shared_ptr<atomic_reference_, waiting_policy_type_> resources) noexcept
        : manager_(manager), data_(std::move(changes), std::move(resources)) {
        start_stamp_ = manager_.request_start_(pin_);
        state_ = state_t::started_k;
    }

    /** Hands the leader word back when a stage still holds it, then gives the start stamp back. */
    ~single_collection_database_transaction() noexcept {
        if (state_ == state_t::staged_k) manager_.discard_(data_);
    }

#pragma endregion Construction

#pragma region Two Phase Commit

    /**
     *  @brief Seals the changes, takes the ledger's leader word, checks the watched keys and lays out the view the
     *      commit publishes, leaving the commit itself for later.
     *
     *  A failure keeps the changes exactly as they were, so a caller can log what it could not commit,
     *  and hands the leader word back, so the transaction is running again rather than staged.
     */
    status_t stage() noexcept {
        if (state_ != state_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is not running"};
        transaction_stamp_t staged_stamp = invalid_transaction_stamp_k;
        if (status_t const staged = manager_.stage_(data_, start_stamp_, staged_stamp); staged.failed()) return staged;
        last_stage_stamp_ = staged_stamp;
        state_ = state_t::staged_k;
        return {};
    }

    /**
     *  @brief Lands every change, through the ledger's sequencer unless a stage already holds the leader word, and
     *      starts the transaction again once its stamp is published.
     *
     *  Nothing is held on return; a failure keeps the changes for logging and a @c rollback drops them.
     */
    status_t commit() noexcept {
        if (state_ == state_t::invalid_k) return {status_code_t::invalid_argument_k, "That transaction is not running"};
        if (state_ == state_t::staged_k) last_try_to_commit_stamp_ = manager_.commit_staged_(data_);
        else {
            auto [committed, commit_status] = manager_.commit_(data_, start_stamp_);
            if (commit_status.failed()) {
                last_try_to_commit_stamp_ = manager_.current_stamp_();
                return commit_status;
            }
            last_try_to_commit_stamp_ = committed;
        }
        restart_();
        return {};
    }

    /** Drops every change and every watch, and starts the transaction again from this moment. */
    void rollback() noexcept {
        if (state_ == state_t::staged_k) manager_.discard_(data_);
        data_.clear();
        start_stamp_ = manager_.request_start_(pin_);
        state_ = state_t::started_k;
    }

    /**
     *  @brief Starts the transaction again from this moment, keeping its changes, whose watches move to the new
     *      start.
     *
     *  Legal only for a transaction that has read nothing, since a read taken before the new start
     *  would be checked against a stamp it never ran at.
     */
    status_t renew() noexcept {
        if (state_ != state_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        if (data_.read_set.count(watch_purpose_t::read_k) != 0)
            return {status_code_t::invalid_argument_k, "That transaction has read something"};
        start_stamp_ = manager_.request_start_(pin_);
        data_.changes.rescope_watches(start_stamp_);
        return {};
    }

#pragma endregion Two Phase Commit

#pragma region Writes

    /** Whether @p count more values of @p bytes together still fit in this transaction's write buffer. */
    [[nodiscard]] bool has_available_space_for(std::size_t count, std::size_t bytes) const noexcept {
        return state_ == state_t::started_k && data_.changes.can_fit(count, bytes);
    }

    /** Writes @p stored_value under @p stored_key, which reaches the database only at the commit, watching the key
     *  for another commit writing it as @p watch_schedule_ says. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t insert(stored_key_t stored_key, std::span<byte_t const> stored_value) noexcept {
        if (state_ != state_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        if (!manager_.store_->admits(stored_value.size()))
            return {status_code_t::malformed_k, "This database admits no value of that size"};
        if (status_t const checked = check_write_now_<watch_schedule_>(stored_key); checked.failed()) return checked;
        return data_.changes.add(stored_key, stored_value, watch_stamp_for_<watch_schedule_>());
    }

    /** Erases @p stored_key, which reaches the database as a tombstone only at the commit, watched as @c insert is. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t erase(stored_key_t stored_key) noexcept {
        if (state_ != state_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        if (status_t const checked = check_write_now_<watch_schedule_>(stored_key); checked.failed()) return checked;
        return data_.changes.mark_as_removed(stored_key, watch_stamp_for_<watch_schedule_>());
    }

    /** Watches @p stored_keys for conflicts until the next commit or rollback, without reading them. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t watch_more(std::span<stored_key_t const> stored_keys) noexcept {
        if (state_ != state_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        if (status_t const prepared = watch_now_or_prepare_<watch_schedule_>(stored_keys); prepared.failed())
            return prepared;
        watch_later_<watch_schedule_>(stored_keys, watch_stamp_for_<watch_schedule_>(), watch_purpose_t::track_k);
        return {};
    }

#pragma endregion Writes

#pragma region Lookups

    /**
     *  @brief Whether anything carries @p stored_key, watching the database when it is what answered.
     *  @param[in] stored_key The key to look for.
     *  @param[out] size Bytes the value takes, which is zero for anything but @c success_k.
     */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    [[nodiscard]] select_result_t find(stored_key_t stored_key, stored_value_size_t& size) const noexcept {
        size = 0;
        if (state_ != state_t::started_k) return select_result_t::nothing_k;
        select_result_t const staged = find_uncommitted(stored_key, size);
        if (staged != select_result_t::nothing_k) return staged;

        std::span<stored_key_t const> const only {&stored_key, 1};
        transaction_stamp_t const watch_from = watch_stamp_for_<watch_schedule_>();
        if (watch_now_or_prepare_<watch_schedule_>(only).failed()) return select_result_t::nothing_k;
        select_result_t const carried = manager_.store_->find(stored_key, size, read_stamp_for_<watch_schedule_>());
        watch_later_<watch_schedule_>(only, watch_from, watch_purpose_t::read_k);
        return carried;
    }

    /** What this transaction's own write buffer holds under @p stored_key, without touching the database. */
    [[nodiscard]] select_result_t find_uncommitted(stored_key_t stored_key, stored_value_size_t& size) const noexcept {
        size = 0;
        stored_value_size_t held = 0;
        select_result_t const staged = visit_uncommitted_(stored_key, [&](std::span<byte_t const> bytes) noexcept {
            held = static_cast<stored_value_size_t>(bytes.size());
        });
        size = held;
        return staged;
    }

    /**
     *  @brief Copies the value of @p stored_key into @p into, from this transaction or from the database.
     *  @param[inout] channel The channel a read of the database is submitted and completed on, held by the caller.
     *  @param[in] stored_key The key to read.
     *  @param[out] into Where the value lands, which need not be aligned and need not be registered.
     *  @param[out] size Bytes written, which is zero for every failure.
     */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t select(io_channel_t& channel, stored_key_t stored_key, std::span<byte_t> into,
                    stored_value_size_t& size) noexcept {
        if (state_ != state_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        size = 0;
        bool refused = false;
        select_result_t const staged = visit_uncommitted_(stored_key, [&](std::span<byte_t const> bytes) noexcept {
            if (into.size() < bytes.size()) {
                refused = true;
                return;
            }
            if (!bytes.empty()) std::memcpy(into.data(), bytes.data(), bytes.size());
            size = static_cast<stored_value_size_t>(bytes.size());
        });
        if (refused) return {status_code_t::invalid_argument_k, "A buffer shorter than the value"};
        if (staged == select_result_t::erased_k)
            return {status_code_t::not_found_k, "This transaction erased that key"};
        if (staged == select_result_t::success_k) return {};

        // The watch is scoped from before the read, so a commit racing this read is refused by it.
        std::span<stored_key_t const> const only {&stored_key, 1};
        transaction_stamp_t const watch_from = watch_stamp_for_<watch_schedule_>();
        if (status_t const prepared = watch_now_or_prepare_<watch_schedule_>(only); prepared.failed()) return prepared;
        transaction_stamp_t const at = read_stamp_for_<watch_schedule_>();
        status_t const read = manager_.store_->select(channel, stored_key, into, size, at);
        watch_later_<watch_schedule_>(only, watch_from, watch_purpose_t::read_k);
        return read;
    }

    /**
     *  @brief Reads the values of @p stored_keys, this transaction's own writes ahead of the database.
     *  @param[inout] channel The channel a read of the database is submitted and completed on, held by the caller.
     *  @param[in] stored_keys The keys to read, in any order and with any number of misses among them.
     *  @param[out] results What every key answered, which separates a miss from an erasure.
     *  @param[in] sink Called as @c sink(request, offset, bytes) for every key that locates a value.
     *
     *  @note A value this transaction wrote arrives as one call carrying all of it, while one the
     *      database answers arrives as one call per block, exactly as a read of the database does.
     */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k, typename sink_type_>
    status_t select(io_channel_t& channel, std::span<stored_key_t const> stored_keys,
                    std::span<select_result_t> results, sink_type_&& sink) const noexcept {
        if (state_ != state_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        if (results.size() != stored_keys.size())
            return {status_code_t::invalid_argument_k, "A batched select answers one result per key"};

        st::basic_vector<stored_key_t> deferred_keys;
        st::basic_vector<std::size_t> deferred_requests;
        if (deferred_keys.reserve(stored_keys.size()) != st::status_t::success_k ||
            deferred_requests.reserve(stored_keys.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A transaction could not track the keys of a batch"};

        for (std::size_t request = 0; request != stored_keys.size(); ++request) {
            select_result_t const staged = visit_uncommitted_(
                stored_keys[request],
                [&](std::span<byte_t const> bytes) noexcept { sink(request, std::uint64_t {0}, bytes); });
            results[request] = staged;
            if (staged != select_result_t::nothing_k) continue;
            [[maybe_unused]] st::status_t const named = deferred_keys.push_back(stored_key_t {stored_keys[request]});
            [[maybe_unused]] st::status_t const tracked = deferred_requests.push_back(std::size_t {request});
        }
        if (deferred_keys.empty()) return {};

        st::basic_vector<select_result_t> deferred_results;
        if (deferred_results.resize(deferred_keys.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A transaction could not track the keys of a batch"};

        std::span<stored_key_t const> const watched {deferred_keys};
        transaction_stamp_t const watch_from = watch_stamp_for_<watch_schedule_>();
        if (status_t const prepared = watch_now_or_prepare_<watch_schedule_>(watched); prepared.failed())
            return prepared;

        auto relay = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
            sink(deferred_requests[request], offset, bytes);
        };
        transaction_stamp_t const at = read_stamp_for_<watch_schedule_>();
        status_t const read = manager_.store_->select(channel, watched, std::span<select_result_t> {deferred_results},
                                                      relay, at);
        watch_later_<watch_schedule_>(watched, watch_from, watch_purpose_t::read_k);
        if (read.failed()) return read;
        for (std::size_t index = 0; index != deferred_results.size(); ++index)
            results[deferred_requests[index]] = deferred_results[index];
        return {};
    }

#pragma endregion Lookups

#pragma region Stamps

    /** The stamp this transaction was created or started again at, which scopes every watch. */
    [[nodiscard]] transaction_stamp_t start_stamp() const noexcept { return start_stamp_; }

    /** The stamp of the last successful stage. */
    [[nodiscard]] transaction_stamp_t last_stage_stamp() const noexcept { return last_stage_stamp_; }

    /** The stamp of the last successful commit. */
    [[nodiscard]] transaction_stamp_t last_commit_stamp() const noexcept { return last_commit_stamp_; }

    /** The stamp of the last commit attempt, whether it landed or not. */
    [[nodiscard]] transaction_stamp_t last_try_to_commit_stamp() const noexcept { return last_try_to_commit_stamp_; }

#pragma endregion Stamps

  private:

#pragma region Private Types

    /** Where one transaction is between its creation and its commit. */
    enum class state_t : std::uint8_t {

        /** Built but not yet started, and left there by nothing a caller can observe. */
        invalid_k,

        /** Running: it reads, it writes, and it holds no queue pair. */
        started_k,

        /** Staged: it holds the ledger's leader word until it commits or rolls back. */
        staged_k,
    };

#pragma endregion Private Types

#pragma region Private Methods

    /**
     *  @brief Calls @p visitor with the bytes this transaction wrote under @p stored_key.
     *  @param[in] stored_key The key to look for in this transaction's own write buffer.
     *  @param[in] visitor Called once with the staged bytes, which are empty for an empty value.
     *  @return What the write buffer holds, which is @c nothing_k when it holds nothing at all.
     */
    template <typename visitor_type_>
    [[nodiscard]] select_result_t visit_uncommitted_(stored_key_t stored_key, visitor_type_&& visitor) const noexcept {
        select_result_t answer = select_result_t::nothing_k;
        data_.changes.stored_values.probe_to_find(stored_key, [&](auto const& entry) noexcept {
            auto const& place = entry.value();
            if (place.kind == delta_kind_t::remove_k) {
                answer = select_result_t::erased_k;
                return;
            }
            answer = select_result_t::success_k;
            if (place.size == 0) {
                visitor(std::span<byte_t const> {});
                return;
            }
            visitor(std::span<byte_t const> {data_.changes.stored_values_buffer.data() + place.offset, place.size});
        });
        return answer;
    }

    /** The stamp a watch taken under @p watch_schedule_ is scoped from. */
    template <watch_schedule_t watch_schedule_>
    [[nodiscard]] transaction_stamp_t watch_stamp_for_() const noexcept {
        if constexpr (watch_schedule_ == watch_schedule_t::on_commit_k) return start_stamp_;
        else if constexpr (watch_schedule_ == watch_schedule_t::on_commit_since_operation_k)
            return manager_.current_stamp_();
        else return invalid_transaction_stamp_k;
    }

    /** The stamp a read under @p watch_schedule_ answers at: the start, but for a watch scoped from the read itself. */
    template <watch_schedule_t watch_schedule_>
    [[nodiscard]] transaction_stamp_t read_stamp_for_() const noexcept {
        if constexpr (watch_schedule_ == watch_schedule_t::on_commit_since_operation_k)
            return manager_.current_stamp_();
        else return start_stamp_;
    }

    /** Checks @p stored_keys now, or makes the room a later watch of them takes no allocation in. */
    template <watch_schedule_t watch_schedule_>
    status_t watch_now_or_prepare_(std::span<stored_key_t const> stored_keys) const noexcept {
        if constexpr (watch_schedule_ == watch_schedule_t::now_k) return manager_.watch_now_(start_stamp_, stored_keys);
        else if constexpr (watch_schedule_ == watch_schedule_t::on_commit_k ||
                           watch_schedule_ == watch_schedule_t::on_commit_since_operation_k)
            return data_.read_set.reserve_more(stored_keys.size());
        else return {};
    }

    /** Records a watch of every key of @p stored_keys from @p watch_stamp, in the room prepared for it. */
    template <watch_schedule_t watch_schedule_>
    void watch_later_(std::span<stored_key_t const> stored_keys, transaction_stamp_t watch_stamp,
                      watch_purpose_t purpose) const noexcept {
        if constexpr (watch_schedule_ == watch_schedule_t::on_commit_k ||
                      watch_schedule_ == watch_schedule_t::on_commit_since_operation_k)
            for (stored_key_t const& stored_key : stored_keys)
                data_.read_set.add(stored_key, watch_stamp, purpose, st::assume_reserved);
    }

    /** Refuses a write of @p stored_key right away when @p watch_schedule_ checks in place and a commit since the
     *  start wrote it; every other schedule leaves the check to the stage. */
    template <watch_schedule_t watch_schedule_>
    status_t check_write_now_(stored_key_t stored_key) const noexcept {
        if constexpr (watch_schedule_ == watch_schedule_t::now_k)
            return manager_.watch_now_(start_stamp_, std::span<stored_key_t const> {&stored_key, 1});
        else return {};
    }

    /** Clears what a landed commit carried and starts again at the stamp it published. */
    void restart_() noexcept {
        state_ = state_t::started_k;
        last_stage_stamp_ = last_try_to_commit_stamp_;
        last_commit_stamp_ = last_try_to_commit_stamp_;
        data_.clear();
        start_stamp_ = manager_.request_start_(pin_);
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The registry this transaction stages and commits through, borrowed and outliving it. */
    registry_t& manager_;

    /** The uncommitted changes and the watched keys, which a read of this transaction also walks. */
    transaction_state_t mutable data_;

    /** The start stamp, pinned so the history this transaction reads at stays kept. */
    stamp_pin<atomic_reference_, waiting_policy_type_> pin_;

    /** The stamp this transaction was created or started again at. */
    transaction_stamp_t start_stamp_ = invalid_transaction_stamp_k;

    /** The stamp of the last successful stage. */
    transaction_stamp_t last_stage_stamp_ = invalid_transaction_stamp_k;

    /** The stamp of the last successful commit. */
    transaction_stamp_t last_commit_stamp_ = invalid_transaction_stamp_k;

    /** The stamp of the last commit attempt, whether it landed or not. */
    transaction_stamp_t last_try_to_commit_stamp_ = invalid_transaction_stamp_k;

    /** Where this transaction is between its creation and its commit. */
    state_t state_ = state_t::invalid_k;

#pragma endregion Private Members
};

#pragma endregion Transaction

#pragma endregion Single Collection Database Transaction

#pragma region Single Collection Database

#pragma region Schema

/** The eight bytes that tell the schema blob of this layer from every other blob of a volume. */
inline constexpr std::uint64_t single_collection_database_magic_k = 0x5343444231303031ull;

/** The layout of the schema record, of which a mount reads exactly one version. */
inline constexpr std::uint32_t single_collection_database_format_version_k = 2;

/**
 *  @brief What a single-collection database writes into its schema blob, and a reopen checks against.
 *
 *  Fixed width and trivially copyable, so the record is the bytes: the volume already carries the
 *  database identity in its own head, and what is left is the shape a reopen must not contradict.
 *  The run records the blob carries after this one are what a reopen puts back into the tree.
 */
struct single_collection_database_schema_record_t {

    /** The magic every schema blob of this layer opens with. */
    std::uint64_t magic = single_collection_database_magic_k;

    /** The layout version this record follows. */
    std::uint32_t format_version = single_collection_database_format_version_k;

    /** The @c value_layout_t the collection was created under. */
    std::uint16_t value_layout = 0;

    /** Written as zero, so the record is the same bytes on every compiler that packs it. */
    std::uint16_t padding = 0;

    /** Bytes the largest value the collection admits takes. */
    std::uint64_t value_max_size = 0;

    /** Runs the blob names after this record, in the order their levels hold them. */
    std::uint64_t run_count = 0;

    /** Which of the two copies is newer, and the commits the runs this copy names hold whole. */
    catalog_stamps_t stamps;
};

static_assert(sizeof(single_collection_database_schema_record_t) == 48, "The schema record is its own bytes");

#pragma endregion Schema

#pragma region Database

/**
 *  @brief One collection, its transactions, its snapshots and its two blobs, over one volume.
 *  @tparam stored_key_ The key the collection is addressed by.
 *  @tparam volume_type_ The volume every run and every blob lives on.
 *  @tparam store_ The store the collection lives in.
 *
 *  Pinned rather than movable: the registry and every live transaction point back here, so a database
 *  is opened into a @c std::unique_ptr and stays where it was opened.
 */
template <typename stored_key_, typename volume_type_, template <typename> typename atomic_reference_,
          typename waiting_policy_type_, nvme_store store_>
class basic_single_collection_database {
  public:

#pragma region Types

    /** The key the runs order their key slots by. */
    using stored_key_t = stored_key_;

    /** The volume every run and every blob of this database lives on. */
    using volume_t = volume_type_;

    /** The collection every write of this database lands in. */
    using collection_t = store_;

    /** The key slot every run of this database holds. */
    using key_slot_t = typename collection_t::key_slot_t;

    /** The transport the volume moves bytes through. */
    using disk_t = std::remove_reference_t<decltype(std::declval<volume_t&>().disk())>;

    /** An ordered walk over that collection, memory runs merged in front of every run. */
    using scanner_t = collection_scanner<key_slot_t, volume_t, atomic_reference_, waiting_policy_type_,
                                         typename collection_t::fold_t>;

    /** One moment of this database, a pinned stamp no publication waits behind. */
    using snapshot_t = single_collection_database_snapshot<basic_single_collection_database>;

    /** One stamp a snapshot or a transaction holds against the ledger. */
    using pin_t = stamp_pin<atomic_reference_, waiting_policy_type_>;

    /** One optimistic transaction over this database. */
    using transaction_t = single_collection_database_transaction<stored_key_t, basic_single_collection_database,
                                                                 atomic_reference_, waiting_policy_type_>;

    /** The order every transaction of this database is placed in, and its conflicts. */
    using registry_t = single_collection_database_transaction_registry<stored_key_t, basic_single_collection_database,
                                                                       atomic_reference_, waiting_policy_type_>;

    /** The uncommitted changes of one transaction, as the registry hands them over. */
    using write_buffer_t = write_buffer<stored_key_t, atomic_reference_, waiting_policy_type_>;

    /** A database, which is pinned and therefore always owned through a pointer. */
    using database_unique_ptr_t = std::unique_ptr<basic_single_collection_database>;

    /** A transaction, which is owned by whoever created it and retires when it dies. */
    using transaction_unique_ptr_t = std::unique_ptr<transaction_t>;

    /** How the values of this database are stored. */
    static constexpr value_layout_t value_layout_k = key_slot_t::value_layout_k;

#pragma endregion Types

#pragma region Construction

    basic_single_collection_database(basic_single_collection_database const&) = delete;
    basic_single_collection_database& operator=(basic_single_collection_database const&) = delete;
    basic_single_collection_database(basic_single_collection_database&&) = delete;
    basic_single_collection_database& operator=(basic_single_collection_database&&) = delete;

    /** Writes whatever is still staged onto the volume and names every run, so the data outlives this. */
    ~basic_single_collection_database() noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().succeeded()) {
            [[maybe_unused]] status_t const flushed = collection_.flush(writing_channel_);
            // A destroyed database dropped its schema, and a close must not write one back over an empty volume.
            if (volume_.names(schema_blob_name_(0)) || volume_.names(schema_blob_name_(1))) {
                [[maybe_unused]] status_t const saved = save_schema_();
            }
        }
    }

    /**
     *  @brief Opens a database over @p volume, creating its schema blob when the volume holds none.
     *  @param[inout] volume The volume every run and blob lands on, borrowed and outliving the database.
     *  @param[inout] writing_channel The channel every write, flush and commit runs on, borrowed for the database's
     *      life.
     *  @param[inout] merging_channel The second channel a compaction writes its new runs on, which is not @p
     *      writing_channel.
     *  @param[in] config The collection's bounds and the transaction defaults.
     *  @param[in] resources The budget the conflict record, the roster and every transaction's writes are charged to.
     *  @return The database, or @c malformed_k for a volume whose schema names another value layout.
     */
    static expected<database_unique_ptr_t> open(
        volume_t& volume, io_channel_t& writing_channel, io_channel_t& merging_channel,
        single_collection_database_config_t const& config,
        resources_shared_ptr<atomic_reference_, waiting_policy_type_> resources) noexcept {
        if (!resources) return status_t {status_code_t::invalid_argument_k, "A database is charged to some budget"};
        std::unique_ptr<commit_ledger<atomic_reference_, waiting_policy_type_>> owned {
            new (std::nothrow) commit_ledger<atomic_reference_, waiting_policy_type_> {}};
        if (!owned) return status_t {status_code_t::out_of_memory_k, "The database did not fit"};
        commit_ledger<atomic_reference_, waiting_policy_type_>& ledger = *owned;
        return open_(volume, writing_channel, merging_channel, config, std::move(resources), ledger, std::move(owned));
    }

    /** Drops both blobs and every run, which destroys the data and leaves the volume empty of it. */
    status_t destroy() noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        status_t first;
        for (std::size_t copy = 0; copy != 2; ++copy)
            if (volume_.names(schema_blob_name_(copy)))
                if (status_t const dropped = drop_blob_(schema_blob_name_(copy)); dropped.failed() && first.succeeded())
                    first = dropped;
        if (status_t const dropped = drop_blob_(user_data_blob_name_()); dropped.failed() && first.succeeded())
            first = dropped;
        if (status_t const emptied = collection_.destroy(writing_channel_); emptied.failed() && first.succeeded())
            first = emptied;
        return first;
    }

#pragma endregion Construction

#pragma region Shape

    /** The bounds this database was opened with. */
    [[nodiscard]] single_collection_database_config_t const& config() const noexcept { return config_; }

    /** The collection every write lands in, which a scan walks and a benchmark measures. */
    [[nodiscard]] collection_t const& records() const noexcept { return collection_; }

    /** Bytes the largest value this database admits takes. */
    [[nodiscard]] stored_value_size_t value_max_size() const noexcept { return collection_.value_max_size(); }

    /** Whether a value of @p size bytes is one this database's layout can hold. */
    [[nodiscard]] bool admits(std::size_t size) const noexcept { return collection_.admits(size); }

    /** Records the memory runs hold plus key slots every level holds, one key counted once per run holding it. */
    [[nodiscard]] std::uint64_t population_count() const noexcept { return collection_.population_count(); }

    /** Bytes every run of every level takes on the volume, answered without excluding a writer. */
    [[nodiscard]] std::uint64_t size_bytes() const noexcept { return collection_.size_bytes(); }

    /** Records committed into memory runs and not yet flushed. */
    [[nodiscard]] std::size_t staged_count() const noexcept { return collection_.staged_count(); }

#pragma endregion Shape

#pragma region Lookups

    /**
     *  @brief Whether anything carries @p stored_key, and how many bytes its value takes.
     *  @param[in] stored_key The key to look for.
     *  @param[out] size Bytes the value takes, which is zero for anything but @c success_k.
     *  @param[in] stamp The stamp a reader is pinned at, which sees the newest version at or below it.
     */
    [[nodiscard]] select_result_t find(stored_key_t stored_key, stored_value_size_t& size,
                                       transaction_stamp_t stamp = head_stamp_k) const noexcept {
        return collection_.find(stored_key, size, stamp);
    }

    /**
     *  @brief Copies the value of @p stored_key into @p into, which needs only the value's own bytes.
     *  @param[inout] channel The channel the read is submitted and completed on, held by the calling thread.
     *  @param[in] stored_key The key to read.
     *  @param[out] into Where the value lands, which need not be aligned and need not be registered.
     *  @param[out] size Bytes written, which is zero for every failure.
     *  @param[in] stamp The stamp a reader is pinned at, which sees the newest version at or below it.
     */
    status_t select(io_channel_t& channel, stored_key_t stored_key, std::span<byte_t> into, stored_value_size_t& size,
                    transaction_stamp_t stamp = head_stamp_k) noexcept {
        return collection_.select(channel, stored_key, into, size, stamp);
    }

    /**
     *  @brief Reads the values of @p stored_keys, refilling the channel as completions retire.
     *  @param[inout] channel The channel the reads are submitted and completed on, held by the calling thread.
     *  @param[in] stored_keys The keys to read, in any order and with any number of misses among them.
     *  @param[out] results What every key answered, which separates a miss from an erasure.
     *  @param[in] sink Called as @c sink(request, offset, bytes) for every key that locates a value.
     *  @param[in] stamp The stamp a reader is pinned at, which sees the newest version at or below it.
     */
    template <typename sink_type_>
    status_t select(io_channel_t& channel, std::span<stored_key_t const> stored_keys,
                    std::span<select_result_t> results, sink_type_&& sink,
                    transaction_stamp_t stamp = head_stamp_k) const noexcept {
        return collection_.select(channel, stored_keys, results, std::forward<sink_type_>(sink), stamp);
    }

#pragma endregion Lookups

#pragma region Writes

    /** Writes @p stored_value under @p stored_key as a commit of its own, flushing when the memory runs are full. */
    status_t insert(stored_key_t stored_key, std::span<byte_t const> stored_value) noexcept {
        keyed_delta<stored_key_t> delta {stored_key, delta_view_t {delta_kind_t::write_k, stored_value}};
        return commit_alone_(collection_.seal(std::span<keyed_delta<stored_key_t>> {&delta, 1}));
    }

    /** Removes @p stored_key as a commit of its own. */
    status_t erase(stored_key_t stored_key) noexcept {
        keyed_delta<stored_key_t> delta {stored_key, delta_view_t {delta_kind_t::remove_k, {}}};
        return commit_alone_(collection_.seal(std::span<keyed_delta<stored_key_t>> {&delta, 1}));
    }

    /**
     *  @brief Writes one value per key as one commit of its own, flushing when they do not all fit.
     *  @param[in] stored_keys The keys to write, in any order and with repeats taking the last value.
     *  @param[in] stored_values The values back to back, in the order their keys name them.
     *  @param[in] sizes Bytes of every value, one per key, which must add up to @p stored_values.
     */
    status_t insert(std::span<stored_key_t const> stored_keys, std::span<byte_t const> stored_values,
                    std::span<stored_value_size_t const> sizes) noexcept {
        return commit_alone_(collection_.seal_inserts(stored_keys, stored_values, sizes));
    }

    /** Removes every key of @p stored_keys as one commit of its own. */
    status_t erase(std::span<stored_key_t const> stored_keys) noexcept {
        return commit_alone_(collection_.seal_erasures(stored_keys));
    }

#pragma endregion Writes

#pragma region Maintenance

    /** Writes every memory run as one run of level zero. */
    status_t flush() noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        if (status_t const flushed = collection_.flush(writing_channel_); flushed.failed()) return flushed;
        // The run the flush wrote is sealed by now, so the schema may name it.
        return save_schema_();
    }

    /** Whether any level is over the bytes its number admits, answered without excluding a writer. */
    [[nodiscard]] bool needs_compaction() const noexcept { return collection_.needs_compaction(); }

    /** Merges the shallowest level that is over its bounds, reading on one channel and writing on the other. */
    expected<compaction_outcome_t> compact() noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        lsm_tree_publication_hook_t published;
        published.notify = &record_schema_;
        published.context = this;
        return collection_.compact(writing_channel_, merging_channel_, published);
    }

    /** How many memory runs the collection holds, and how many commits they hold. */
    [[nodiscard]] memory_shape_t memory_shape() const noexcept { return collection_.memory_shape(); }

    /** How many times the schema blob has been saved, whose @c catalog_copy_of names the copy a mount reads. */
    [[nodiscard]] std::uint64_t catalog_generation() const noexcept { return generation_; }

    /** Drops every version above @p ceiling, which a mount runs once its ledger has resumed at @p ceiling. */
    status_t truncate_above(transaction_stamp_t ceiling) noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        lsm_tree_publication_hook_t published;
        published.notify = &record_schema_;
        published.context = this;
        return collection_.truncate_above(writing_channel_, merging_channel_, ceiling, published);
    }

    /** Pins the newest published stamp, at which every read through the snapshot answers, and flushes nothing. */
    expected<snapshot_t> take_snapshot() noexcept { return snapshot_t::of(*this); }

#pragma endregion Maintenance

#pragma region Transactions

    /** A transaction over this database, with the write-buffer bounds @p config names. */
    expected<transaction_unique_ptr_t> create_transaction(transaction_config_t const& config) noexcept {
        if (config.write_buffer_max_bytes > config_.collection.write_buffer_max_bytes ||
            config.write_buffer_max_elements > config_.collection.write_buffer_max_elements)
            return status_t {status_code_t::invalid_argument_k,
                             "A transaction may not outgrow the write buffer it commits into"};

        // The write buffer takes exactly one of the two widths, and which one is the layout's own answer.
        std::size_t const exact = is_fixed(value_layout_k) ? collection_.value_max_size() : 0;
        std::size_t const bounded = is_fixed(value_layout_k) ? 0 : collection_.value_max_size();
        return registry_.create_transaction(config, exact, bounded);
    }

    /** A transaction with the write-buffer bounds this database was opened with. */
    expected<transaction_unique_ptr_t> create_transaction() noexcept {
        return create_transaction(config_.default_transaction);
    }

    /** The order every commit of this database draws its stamp from. */
    [[nodiscard]] commit_ledger<atomic_reference_, waiting_policy_type_>& ledger() noexcept { return ledger_; }

    /** Whether a commit newer than @p from wrote @p stored_key, read from the published view. */
    [[nodiscard]] bool written_since(stored_key_t stored_key, transaction_stamp_t from) const noexcept {
        return collection_.written_since(stored_key, from);
    }

#pragma endregion Transactions

#pragma region User Data

    /** Bytes the owner's blob takes beside the data, which is zero when the volume holds none. */
    [[nodiscard]] std::uint64_t user_data_size_bytes() const noexcept {
        expected<volume_blob_t> const found = volume_.find(user_data_blob_name_());
        return found ? found->length : 0;
    }

    /** Replaces the owner's blob with @p bytes, which the volume keeps beside the runs. */
    status_t save_user_data(std::span<byte_t const> bytes) noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        if (bytes.empty()) return drop_blob_(user_data_blob_name_());
        return write_blob_(user_data_blob_name_(), blob_kind_t::user_data_k, bytes);
    }

    /** Copies the owner's blob into @p into, which must be at least as long as the blob. */
    status_t load_user_data(io_channel_t& channel, std::span<byte_t> into) const noexcept {
        auto [found, find_status] = volume_.find(user_data_blob_name_());
        if (find_status.failed()) return find_status;
        if (into.size() < found.length) return {status_code_t::invalid_argument_k, "A buffer shorter than the blob"};
        return read_blob_(channel, found, into.first(static_cast<std::size_t>(found.length)));
    }

    /** Forgets the owner's blob, which a volume holding none reports as @c not_found_k. */
    status_t clear_user_data() noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        return drop_blob_(user_data_blob_name_());
    }

#pragma endregion User Data

#pragma region Transaction Store Interface

    /** Seals every change of @p changes into @p sealed on the committing thread, flushing first when the memory runs
     *  are full, so the commit that follows cannot fail halfway. */
    status_t seal_write_buffer(write_buffer_t const& changes,
                               sealed_commit<stored_key_t, atomic_reference_>& sealed) noexcept {
        st::basic_vector<keyed_delta<stored_key_t>> deltas;
        if (deltas.reserve(changes.count()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to hand a commit over"};
        [[maybe_unused]] st::status_t const walked = changes.stored_values.for_each([&](auto const& entry) noexcept {
            auto const& place = entry.value();
            delta_view_t const delta {place.kind, changed_bytes_(changes, place)};
            [[maybe_unused]] st::status_t const placed = deltas.push_back(
                st::assume_reserved, keyed_delta<stored_key_t> {entry.key(), delta});
        });
        auto [taken, seal_status] = collection_.seal(std::span<keyed_delta<stored_key_t>> {deltas});
        if (seal_status.failed()) return seal_status;
        sealed = std::move(taken);
        return make_room_(sealed);
    }

    /** Lays out the view @p sealed is published in, on the thread holding the ledger's leader word. */
    status_t prepare_sealed(sealed_commit<stored_key_t, atomic_reference_>& sealed) noexcept {
        return collection_.prepare(sealed);
    }

    /** Publishes @p sealed under @p stamp, before the stamp itself is, on the thread holding the leader word. */
    void publish_sealed(sealed_commit<stored_key_t, atomic_reference_>& sealed, transaction_stamp_t stamp) noexcept {
        collection_.publish(sealed, stamp);
    }

    /** Forgets the view @c prepare_sealed laid out, for a commit the leader refuses. */
    void forget_sealed() noexcept { collection_.forget_prepared(); }

    /**
     *  @brief Merges the memory runs by generation, and flushes them and compacts the tree once they hold their bounds,
     *      which every commit runs once it lands.
     *
     *  Returns at once while another writer holds the write path, since that writer looks at the memory runs again
     *  after it lets go, so no commit waits behind another's maintenance.
     *  @note Called outside the ledger's epochs.
     */
    status_t maintain() noexcept {
        if (!collection_.maintenance_due()) return {};
        write_path_guard_t const held = write_path_guard_t::taken_if_free(*this);
        if (!held.holding()) return {};
        return maintain_due_();
    }

#pragma endregion Transaction Store Interface

  private:

#pragma region Private Types

    /**
     *  @brief One writer's hold on the collection, which every other writer parks behind.
     *
     *  A queue pair belongs to the thread that announced itself on it, so the holder adopts both channels before it
     *  writes through them; the claim is what makes that handoff a handoff and not a share.
     */
    class write_path_guard_t {
      public:

        /** Takes @p owner's write path, parked while another writer holds it, and adopts both channels. */
        explicit write_path_guard_t(basic_single_collection_database& owner) noexcept : owner_(&owner) {
            owner.write_path_claim_.take();
            owner.adopt_channels_();
        }

        /** Takes @p owner's write path if no writer holds it, or answers a guard holding nothing. */
        [[nodiscard]] static write_path_guard_t taken_if_free(basic_single_collection_database& owner) noexcept {
            write_path_guard_t guard;
            if (!owner.write_path_claim_.take_if_free()) return guard;
            owner.adopt_channels_();
            guard.owner_ = &owner;
            return guard;
        }

        write_path_guard_t(write_path_guard_t const&) = delete;
        write_path_guard_t& operator=(write_path_guard_t const&) = delete;
        write_path_guard_t(write_path_guard_t&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}
        ~write_path_guard_t() noexcept {
            if (owner_) owner_->release_write_path_();
        }

        /** Whether this guard holds the write path. */
        [[nodiscard]] bool holding() const noexcept { return owner_ != nullptr; }

        /** Whether this thread took the channels over, which every write through them needs. */
        status_t adopted() const noexcept { return owner_->write_path_adoption_; }

      private:

        write_path_guard_t() noexcept = default;

        /** The database whose collection this guard holds, borrowed for the guard's life, null once moved from. */
        basic_single_collection_database* owner_ = nullptr;
    };

#pragma endregion Private Types

#pragma region Private Construction

    /** Takes what @c open assembled, and points the registry at the database it will commit into. */
    basic_single_collection_database(
        volume_t& volume, io_channel_t& writing_channel, io_channel_t& merging_channel,
        single_collection_database_config_t const& config,
        resources_shared_ptr<atomic_reference_, waiting_policy_type_> resources, collection_t&& mounted,
        commit_ledger<atomic_reference_, waiting_policy_type_>& ledger,
        std::unique_ptr<commit_ledger<atomic_reference_, waiting_policy_type_>> owned) noexcept
        : volume_(volume), writing_channel_(writing_channel), merging_channel_(merging_channel), config_(config),
          owned_ledger_(std::move(owned)), ledger_(ledger), collection_(std::move(mounted)),
          registry_(*this, std::move(resources)) {}

    /** Mounts the collection and the database around it, drawing stamps from @p ledger. */
    static expected<database_unique_ptr_t> open_(
        volume_t& volume, io_channel_t& writing_channel, io_channel_t& merging_channel,
        single_collection_database_config_t const& config,
        resources_shared_ptr<atomic_reference_, waiting_policy_type_> resources,
        commit_ledger<atomic_reference_, waiting_policy_type_>& ledger,
        std::unique_ptr<commit_ledger<atomic_reference_, waiting_policy_type_>> owned) noexcept {
        if (&writing_channel == &merging_channel)
            return status_t {status_code_t::invalid_argument_k, "A compaction reads and writes on two channels"};
        auto [mounted, mount_collection_status] = collection_t::mount(volume, config.collection,
                                                                      resources->memory_constraint, ledger);
        if (mount_collection_status.failed()) return mount_collection_status;

        database_unique_ptr_t opened {new (std::nothrow) basic_single_collection_database {
            volume, writing_channel, merging_channel, config, std::move(resources), std::move(mounted), ledger,
            std::move(owned)}};
        if (!opened) return status_t {status_code_t::out_of_memory_k, "The database did not fit"};
        if (status_t const agreed = opened->agree_on_schema_(); agreed.failed()) return agreed;
        if (status_t const resumed = ledger.resume(opened->loaded_durable_); resumed.failed()) return resumed;
        if (status_t const truncated = opened->truncate_above(opened->loaded_durable_); truncated.failed())
            return truncated;
        return opened;
    }

#pragma endregion Private Construction

#pragma region Write Path

    /** Binds both channels to the thread that took the write path, which every write through them needs. */
    void adopt_channels_() noexcept {
        write_path_adoption_ = volume_.disk().bind_queue_pair(writing_channel_.queue_pair());
        if (write_path_adoption_.succeeded())
            write_path_adoption_ = volume_.disk().bind_queue_pair(merging_channel_.queue_pair());
    }

    /** Lands @p sealed as a commit of its own, making room first while the memory runs are past twice their bounds,
     *  and maintains the collection once it lands. */
    status_t commit_alone_(expected<sealed_commit<stored_key_t, atomic_reference_>> sealed) noexcept {
        if (!sealed) return sealed.status();
        if (!sealed->run) return {};
        if (status_t const room = make_room_(*sealed); room.failed()) return room;
        if (status_t const committed = collection_.commit(*sealed); committed.failed()) return committed;
        // The commit has landed, so a failure to maintain is met again by the next commit making room.
        [[maybe_unused]] status_t const maintained = maintain();
        return {};
    }

    /**
     *  @brief Flushes the memory runs while @p sealed would leave them past twice their bounds, holding no epoch and
     *      parking rather than spinning while another writer holds the write path.
     *  @return Success once the commit fits, or why a flush could not make room.
     */
    status_t make_room_(sealed_commit<stored_key_t, atomic_reference_> const& sealed) noexcept {
        if (!sealed.run) return {};
        while (collection_.memory_pressure(sealed) == memory_pressure_t::stalled_k) {
            write_path_guard_t const held = write_path_guard_t::taken_if_free(*this);
            if (!held.holding()) {
                // Whoever holds the write path may be flushing already, so the pressure is read again after.
                write_path_claim_.wait_until_released();
                continue;
            }
            if (held.adopted().failed()) return held.adopted();
            if (status_t const flushed = flush_and_compact_(); flushed.failed()) return flushed;
        }
        return {};
    }

    /** Flushes the memory runs, saves the schema naming the run, and compacts the tree for as long as the flush left
     *  a level over its bounds, holding the write path. */
    status_t flush_and_compact_() noexcept {
        if (status_t const flushed = collection_.flush(writing_channel_); flushed.failed()) return flushed;
        if (status_t const saved = save_schema_(); saved.failed()) return saved;
        lsm_tree_publication_hook_t published;
        published.notify = &record_schema_;
        published.context = this;
        while (collection_.needs_compaction()) {
            auto [outcome, compact_status] = collection_.compact(writing_channel_, merging_channel_, published);
            if (compact_status.failed()) return compact_status;
            if (outcome == compaction_outcome_t::nothing_to_compact_k) break;
        }
        return {};
    }

    /** Merges the memory runs by generation, and flushes and compacts once they hold their bounds, holding the write
     *  path with both channels adopted. */
    status_t maintain_due_() noexcept {
        if (write_path_adoption_.failed()) return write_path_adoption_;
        if (status_t const merged = collection_.merge_memory_runs(); merged.failed()) return merged;
        if (!collection_.flush_due()) return {};
        return flush_and_compact_();
    }

    /**
     *  @brief Hands the write path back, then looks once more at the memory runs and, if they are due and the write
     *      path is still free, takes it back to maintain them, so a commit that found the write path held left its
     *      maintenance to this writer.
     *
     *  A writer that finds the write path taken again stops, since the new holder looks once more after it lets go,
     *  and a failed adoption or maintenance is met again by the next commit.
     */
    void release_write_path_() noexcept {
        write_path_claim_.release();
        if (!collection_.maintenance_due() || !write_path_claim_.take_if_free()) return;
        adopt_channels_();
        [[maybe_unused]] status_t const maintained = maintain_due_();
        write_path_claim_.release();
    }

#pragma endregion Write Path

#pragma region Private Methods

    /** The bytes @p place names inside @p changes, empty for an empty value. */
    [[nodiscard]] static std::span<byte_t const> changed_bytes_(write_buffer_t const& changes,
                                                                auto const& place) noexcept {
        if (place.size == 0) return {};
        return std::span<byte_t const> {changes.stored_values_buffer.data() + place.offset, place.size};
    }

    /** The name copy @p copy of the schema blob answers to on the volume. */
    [[nodiscard]] static blob_name_t schema_blob_name_(std::size_t copy) noexcept {
        return copy == 0 ? blob_name_of("single-collection-database.schema.0")
                         : blob_name_of("single-collection-database.schema.1");
    }

    /** The name the owner's blob answers to on the volume. */
    [[nodiscard]] static blob_name_t user_data_blob_name_() noexcept {
        static constexpr char text[] = "single-collection-database.user";
        return blob_name_t {reinterpret_cast<byte_t const*>(text), sizeof(text) - 1};
    }

    /** The shape this database would write into its schema blob, less the runs that follow it there. */
    [[nodiscard]] single_collection_database_schema_record_t wanted_schema_() const noexcept {
        single_collection_database_schema_record_t wanted;
        wanted.value_layout = static_cast<std::uint16_t>(value_layout_k);
        wanted.value_max_size = collection_.value_max_size();
        return wanted;
    }

    /** Reads copy @p copy of the schema blob into @p image, leaving it empty when the volume holds no such copy. */
    status_t read_schema_copy_(std::size_t copy, st::basic_vector<byte_t>& image) noexcept {
        expected<volume_blob_t> const found = volume_.find(schema_blob_name_(copy));
        if (!found) return {};
        if (found->length < sizeof(single_collection_database_schema_record_t))
            return {status_code_t::malformed_k, "The volume's schema blob is shorter than one record"};
        if (image.resize(static_cast<std::size_t>(found->length)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to read the schema into"};
        return read_blob_(writing_channel_, *found, std::span<byte_t> {image});
    }

    /**
     *  @brief Reads the newer copy of the schema blob and takes its runs back, or writes the one this database would
     *      have written.
     *
     *  A copy is sealed only once durable, so whichever copies a crash left are whole, and the newer one wins.
     */
    status_t agree_on_schema_() noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        single_collection_database_schema_record_t const wanted = wanted_schema_();

        std::array<st::basic_vector<byte_t>, 2> images;
        std::array<single_collection_database_schema_record_t, 2> records {};
        for (std::size_t copy = 0; copy != 2; ++copy) {
            if (status_t const read = read_schema_copy_(copy, images[copy]); read.failed()) return read;
            if (!images[copy].empty()) std::memcpy(&records[copy], images[copy].data(), sizeof(records[copy]));
        }
        std::size_t const newer = records[1].stamps.generation > records[0].stamps.generation ? 1 : 0;
        if (images[newer].empty()) return write_schema_(std::span<lsm_tree_run_record_t const> {});
        st::basic_vector<byte_t> const& image = images[newer];
        single_collection_database_schema_record_t const& carried = records[newer];
        if (carried.magic != single_collection_database_magic_k)
            return {status_code_t::malformed_k, "The volume's schema blob carries another magic"};
        if (carried.format_version != single_collection_database_format_version_k)
            return {status_code_t::unsupported_k, "The volume's schema follows another format version"};
        if (carried.value_layout != wanted.value_layout || carried.value_max_size != wanted.value_max_size)
            return {status_code_t::invalid_argument_k, "The volume holds a collection of another value layout"};
        if (image.size() - sizeof(carried) != carried.run_count * sizeof(lsm_tree_run_record_t))
            return {status_code_t::malformed_k, "The volume's schema names another number of runs"};

        st::basic_vector<lsm_tree_run_record_t> runs;
        if (runs.resize(static_cast<std::size_t>(carried.run_count)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room for the runs the schema names"};
        if (!runs.empty())
            std::memcpy(runs.data(), image.data() + sizeof(carried), runs.size() * sizeof(lsm_tree_run_record_t));
        generation_ = carried.stamps.generation;
        loaded_durable_ = carried.stamps.durable;
        return collection_.restore(writing_channel_, std::span<lsm_tree_run_record_t const> {runs});
    }

    /**
     *  @brief Names every run the tree holds in the schema blob, which every publication of a run does.
     *
     *  The durable stamp is read before the runs are listed: every commit at or below it is in some run already, and
     *  a run a flush adds in between only holds commits above it, which a mount of this copy drops.
     */
    status_t save_schema_() noexcept {
        transaction_stamp_t const durable = ledger_.durable_stamp();
        st::basic_vector<lsm_tree_run_record_t> runs;
        if (status_t const named = record_lsm_tree_runs(collection_.tree(), runs); named.failed()) return named;
        return write_schema_(std::span<lsm_tree_run_record_t const> {runs}, durable);
    }

    /** Lays the schema record and @p runs out back to back and replaces the older copy of the schema blob. */
    status_t write_schema_(std::span<lsm_tree_run_record_t const> runs,
                           transaction_stamp_t durable = unwritten_stamp_k) noexcept {
        single_collection_database_schema_record_t record = wanted_schema_();
        record.run_count = runs.size();
        record.stamps.generation = generation_ + 1;
        record.stamps.durable = durable;
        st::basic_vector<byte_t> image;
        if (image.resize(sizeof(record) + runs.size() * sizeof(lsm_tree_run_record_t)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to lay the schema out"};
        std::memcpy(image.data(), &record, sizeof(record));
        if (!runs.empty())
            std::memcpy(image.data() + sizeof(record), runs.data(), runs.size() * sizeof(lsm_tree_run_record_t));
        if (status_t const written = write_blob_(schema_blob_name_(catalog_copy_of(record.stamps.generation)),
                                                 blob_kind_t::schema_k, std::span<byte_t const> {image});
            written.failed())
            return written;
        generation_ = record.stamps.generation;
        return {};
    }

    /** Writes the schema out for a compaction that has published, before it drops what it consumed. */
    static status_t record_schema_(void* context) noexcept {
        return static_cast<basic_single_collection_database*>(context)->save_schema_();
    }

    /** Forgets @p name, which a volume holding no such blob reports as @c not_found_k. */
    status_t drop_blob_(blob_name_t name) noexcept { return volume_.drop(writing_channel_, name); }

    /** Writes @p bytes as the blob @p name of @p kind on the writing channel, replacing any blob of that name. */
    status_t write_blob_(blob_name_t name, blob_kind_t kind, std::span<byte_t const> bytes) noexcept {
        if (volume_.names(name))
            if (status_t const dropped = volume_.drop(writing_channel_, name); dropped.failed()) return dropped;

        auto [reserved, create_status] = volume_.create(name, kind, bytes.size());
        if (create_status.failed()) return create_status;
        if (status_t const written = fill_blob_(reserved, bytes); written.failed()) {
            [[maybe_unused]] status_t const abandoned = volume_.drop(writing_channel_, name);
            return written;
        }
        if (status_t const sealed = volume_.seal(writing_channel_, reserved); sealed.failed()) {
            [[maybe_unused]] status_t const abandoned = volume_.drop(writing_channel_, name);
            return sealed;
        }
        return {};
    }

    /** Moves @p bytes into the range @p blob reserved, one block at a time and padded to a page. */
    status_t fill_blob_(volume_blob_t const& blob, std::span<byte_t const> bytes) noexcept {
        basic_write_pipeline<disk_t, waiting_policy_type_> writer {volume_.disk(), writing_channel_, volume_.medium()};
        std::uint64_t at = 0;
        while (at != bytes.size()) {
            auto [block, acquire_buffer_or_poll_status] = writer.acquire_buffer_or_poll();
            if (acquire_buffer_or_poll_status.failed()) return acquire_buffer_or_poll_status;
            std::uint64_t const room = round_down_to_multiple(block.size(), portable_alignment_k);
            if (room == 0) return {status_code_t::capacity_exhausted_k, "A channel's block is shorter than one page"};

            std::uint64_t const carried = room < bytes.size() - at ? room : bytes.size() - at;
            std::uint64_t const padded = round_up_to_multiple(carried, portable_alignment_k);
            std::span<byte_t> const room_bytes = block.bytes();
            std::memcpy(room_bytes.data(), bytes.data() + at, static_cast<std::size_t>(carried));
            std::memset(room_bytes.data() + carried, 0, static_cast<std::size_t>(padded - carried));
            if (status_t const queued = writer.enqueue_write(std::move(block), blob.slice(at, padded)); queued.failed())
                return queued;
            at += carried;
        }
        return writer.make_durable();
    }

    /** Copies the first @c into.size() bytes of @p blob into @p into, one block at a time. */
    status_t read_blob_(io_channel_t& channel, volume_blob_t const& blob, std::span<byte_t> into) const noexcept {
        std::uint64_t const chunk = round_down_to_multiple(channel.requests().buffer_bytes(), portable_alignment_k);
        if (chunk == 0) return {status_code_t::capacity_exhausted_k, "A channel's block is shorter than one page"};
        std::uint64_t const wanted = round_up_to_multiple(into.size(), portable_alignment_k);

        status_t first;
        auto sink = [&](completed_buffer_t const& completed) noexcept {
            if (status_t const whole = require_completed_in_full(completed); whole.failed()) {
                if (first.succeeded()) first = whole;
                return;
            }
            std::uint64_t const at = static_cast<std::uint64_t>(completed.tag) * chunk;
            if (at >= into.size()) return;
            std::uint64_t const room = into.size() - at;
            std::uint64_t const carried = completed.bytes.size() < room ? completed.bytes.size() : room;
            std::memcpy(into.data() + at, completed.bytes.data(), static_cast<std::size_t>(carried));
        };

        basic_read_pipeline<disk_t> reader {volume_.disk(), channel, volume_.medium()};
        std::uint64_t at = 0;
        request_tag_t tag = 0;
        while (at != wanted) {
            while (at != wanted && reader.can_enqueue()) {
                std::uint64_t const length = chunk < wanted - at ? chunk : wanted - at;
                if (status_t const offered = reader.enqueue_read(blob.slice(at, length), tag); offered.failed())
                    return offered;
                at += length;
                ++tag;
            }
            // The offers sit staged until the channel submits them, and a poll_completions waits only on what it did.
            if (status_t const flushed = reader.submit(); flushed.failed()) return flushed;
            if (status_t const polled = reader.poll_all_completions(sink); polled.failed()) return polled;
        }
        return first;
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The volume every run and every blob lives on, borrowed and outliving the database. */
    volume_t& volume_;

    /** The channel every write, flush and commit runs on, borrowed and outliving the database. */
    io_channel_t& writing_channel_;

    /** The second channel a compaction writes its new runs on, borrowed and outliving the database. */
    io_channel_t& merging_channel_;

    /** The bounds this database was opened with. */
    single_collection_database_config_t config_ {};

    /** The stamps of this database, on the heap, as the collection mounted before the database names them. */
    std::unique_ptr<commit_ledger<atomic_reference_, waiting_policy_type_>> owned_ledger_;

    /** The stamps every commit of this database draws, which @c owned_ledger_ holds. */
    commit_ledger<atomic_reference_, waiting_policy_type_>& ledger_;

    /** The collection every write lands in, which pins the ledger and so dies before it. */
    collection_t collection_;

    /** How many times the schema blob has been saved, which names the copy the next save replaces. */
    std::uint64_t generation_ = 0;

    /** The durable stamp of the schema copy the open read, above which a mount drops every version. */
    transaction_stamp_t loaded_durable_ = unwritten_stamp_k;

    /** The order every transaction is placed in, and the last commit to write each key. */
    registry_t registry_;

    /** Held by whichever writer flushes, compacts or saves a blob, a commit paying for its maintenance included. */
    parking_claim<atomic_reference_, waiting_policy_type_> write_path_claim_;

    /** Whether the writer holding the path took both channels over, written only under the claim. */
    status_t write_path_adoption_;

#pragma endregion Private Members
};

#pragma endregion Database

#pragma endregion Single Collection Database

} // namespace unum::ustore::blobs
