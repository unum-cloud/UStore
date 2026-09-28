/**
 *  @file src/nvme/database_transaction_registry.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Registers, stages and commits the transactions of a database, and detects their key conflicts.
 *
 *  Nothing here takes a lock. A start reads the stamp counter, a commit claims one with a @c fetch_add
 *  and publishes what it wrote with one @c fetch_max per key, and a roster slot is claimed by writing
 *  its identifier with one compare-exchange. The single exclusion is the staging queue pair, which a staged
 *  transaction holds from @c stage until @c commit because those are two separate calls from its owner.
 */
#pragma once

#include <cassert> // `assert`
#include <cstddef> // `std::size_t`

#include <concepts> // `std::same_as`
#include <memory>   // `std::unique_ptr`
#include <new>      // `std::nothrow`
#include <span>     // `std::span`
#include <utility>  // `std::move`

#include "nvme/database_transaction_state.hpp" // `database_transaction_state`, `grouped_write_buffer`
#include "nvme/read_set.hpp"                   // `commit_record_t`, `transaction_roster_t`

namespace unum::ustore::nvme {

template <typename stored_key_, typename store_type_>
class database_transaction;

#pragma region Store Interface

/**
 *  @brief What a registry needs from the store beneath it to stage and to commit.
 *
 *  Only the commit path is named here; the read accessors a transaction reaches the store through
 *  forward from members that instantiate when a caller uses them, so a store serving reads of one
 *  shape alone still satisfies this.
 */
template <typename store_type_, typename stored_key_type_>
concept transaction_store_concept = requires(store_type_& store, store_type_ const& constant, collection_id_t group_id,
                                             stored_key_type_ stored_key, stored_value_view_t stored_value,
                                             std::size_t count) {
    { store.acquire_write_path() } -> std::same_as<void>;
    { store.release_write_path() } -> std::same_as<void>;
    { constant.contains_collection(group_id) } -> std::same_as<bool>;
    { constant.collection_fits(group_id, count, count) } -> std::same_as<bool>;
    { store.flush_collection(group_id) } -> std::same_as<status_t>;
    { store.insert_in_collection(group_id, stored_key, stored_value) } -> std::same_as<status_t>;
    { store.remove_from_collection(group_id, stored_key) } -> std::same_as<status_t>;
};

#pragma endregion Store Interface

/**
 *  @brief The transactions of one database, the order they commit in and the conflicts that stop them.
 *  @tparam stored_key_ The key every collection of the store is addressed by.
 *  @tparam store_type_ The store the commits land in, which must satisfy @c transaction_store_concept.
 */
template <typename stored_key_, typename store_type_>
class database_transaction_registry {
  public:
#pragma region Types

    using stored_key_t = stored_key_;
    using store_t = store_type_;
    using transaction_t = database_transaction<stored_key_, store_type_>;
    using transaction_state_t = database_transaction_state<stored_key_t>;
    using write_buffer_t = typename transaction_state_t::write_buffer_t;
    using group_info_t = typename write_buffer_t::group_info_t;
    using transaction_unique_ptr_t = std::unique_ptr<transaction_t>;
    using return_transaction_t = expected<transaction_unique_ptr_t>;

    /** The commit-record slots a registry keeps when its owner names none. */
    static constexpr std::size_t default_conflict_slots_k = 1ull << 16;

    /** The transactions a registry admits at once when its owner names no count. */
    static constexpr std::size_t default_transaction_slots_k = 256;

#pragma endregion Types

#pragma region Construction

    inline database_transaction_registry(store_t& store, resources_shared_ptr_t resources,
                                         std::size_t conflict_slots = default_conflict_slots_k,
                                         std::size_t transaction_slots = default_transaction_slots_k) noexcept
        : store_(&store), resources_(std::move(resources)), record_(conflict_slots, resources_->memory_constraint),
          roster_(transaction_slots, resources_->memory_constraint) {}

    inline database_transaction_registry(database_transaction_registry&&) = delete;
    inline database_transaction_registry(database_transaction_registry const&) = delete;
    inline database_transaction_registry& operator=(database_transaction_registry const&) = delete;
    inline database_transaction_registry& operator=(database_transaction_registry&&) = delete;
    inline ~database_transaction_registry() noexcept { assert(roster_.live() == 0 && "Transactions outlive registry"); }

#pragma endregion Construction

#pragma region Interface Methods

    /** Whether @p id names a transaction this registry holds. */
    inline bool has_transaction(transaction_id_t id) const noexcept { return roster_.contains(id); }

    /** The bytes the conflict record and the roster together occupy, which no commit ever changes. */
    inline std::size_t shared_state_bytes() const noexcept { return record_.size_bytes() + roster_.size_bytes(); }

    inline return_transaction_t create_transaction(transaction_id_t id, transaction_config_t const& config) noexcept;

#pragma endregion Interface Methods

  private:
#pragma region Transactions

    friend store_t;
    friend transaction_t;

    inline status_t stage_(transaction_id_t id, transaction_state_t& data,
                           transaction_stamp_t& try_to_stage_stamp) noexcept;
    inline status_t commit_(transaction_id_t id, transaction_state_t& data,
                            transaction_stamp_t& try_to_commit_stamp) noexcept;
    inline void discard_(transaction_id_t id) noexcept;

    inline transaction_stamp_t request_start_(transaction_id_t id) noexcept;
    inline void invalidate_(transaction_id_t id) noexcept { roster_.stop(id); }
    inline void notify_about_deletion_(transaction_id_t id) noexcept;

    /** The lowest stamp any later commit can claim, which scopes a watch taken now. */
    inline transaction_stamp_t current_stamp_() const noexcept { return record_.watch_stamp(); }

    inline status_t watch_now_(transaction_stamp_t start_stamp, collection_id_t group_id,
                               std::span<stored_key_t const> stored_keys) const noexcept;

#pragma endregion Transactions

#pragma region Helper Methods

    inline bool conflicting_(transaction_state_t const& data) const noexcept;
    inline void record_commit_(write_buffer_t const& changes, transaction_stamp_t commit_stamp) noexcept;
    inline status_t prepare_collections_(write_buffer_t const& changes) noexcept;
    inline status_t apply_(write_buffer_t const& changes) noexcept;

#pragma endregion Helper Methods

#pragma region Store Accessor Methods

    inline void lock_shared_() const noexcept { store_->lock_shared(); }
    inline void unlock_shared_() const noexcept { store_->unlock_shared(); }

    inline auto begin_(auto const& id) const noexcept { return store_->begin(id); }
    inline auto end_(auto const& id) const noexcept { return store_->end(id); }
    inline auto find_(auto const& id, stored_key_t stored_key) const noexcept { return store_->find(id, stored_key); }
    inline auto find_or_next_(auto const& id, stored_key_t stored_key) const noexcept {
        return store_->find_or_next(id, stored_key);
    }

    inline bool has_collection_(auto const& id) const noexcept { return store_->has_collection(id); }
    inline collection_id_t collection_id_(auto const& id) const noexcept { return store_->collection_id(id); }
    inline auto list_collections_() const noexcept { return store_->list_collections(); }
    inline status_t collection_config_(collection_id_t group_id, collection_config_t& config) const noexcept {
        return store_->collection_config(group_id, config);
    }

    inline bool contains_(auto const& id, stored_key_t stored_key) const noexcept {
        return store_->contains(id, stored_key);
    }
    inline status_t stored_value_size_(auto const& id, stored_key_t stored_key,
                                       stored_value_size_t& size) const noexcept {
        return store_->stored_value_size(id, stored_key, size);
    }
    inline stored_value_size_t buffer_size_for_any_stored_value_(auto const& id) const noexcept {
        return store_->buffer_size_for_any_stored_value(id);
    }
    template <typename allocator_>
    inline auto create_stored_value_locations_(auto const& id, std::size_t count) const noexcept {
        return store_->template create_stored_value_locations<allocator_>(id, count);
    }

    inline status_t select_(auto const& id, stored_key_t stored_key, stored_value_span_t stored_value,
                            stored_value_size_t& size) const noexcept {
        return store_->select(id, stored_key, stored_value, size);
    }
    inline status_t find_(auto const& id, std::span<stored_key_t const> stored_keys, auto stored_value_locations,
                          missing_locations_t missing = missing_locations_t::invalidate_k) const noexcept {
        return store_->find(id, stored_keys, stored_value_locations, missing);
    }
    inline status_t select_(auto const& id, auto stored_value_locations, bytes_span_t buffer,
                            notifier_t notifier) const noexcept {
        return store_->select(id, stored_value_locations, buffer, notifier);
    }

#pragma endregion Store Accessor Methods

#pragma region Private Members

    /** The store every commit of this registry lands in, owned by whoever named it. */
    store_t* store_;

    /** The budget the record, the roster and every transaction's writes are charged to. */
    resources_shared_ptr_t resources_;

    /** The order every transaction is placed in, and the last commit to write each key. */
    commit_record_t record_;

    /** The live transactions and the staging queue pair. */
    transaction_roster_t roster_;

#pragma endregion Private Members
};

#pragma region Interface Methods

template <typename stored_key_, typename store_type_>
inline typename database_transaction_registry<stored_key_, store_type_>::return_transaction_t
database_transaction_registry<stored_key_, store_type_>::create_transaction(
    transaction_id_t id, transaction_config_t const& config) noexcept {

    status_t const enrolled = roster_.enroll(id);
    if (enrolled.failed()) return enrolled;

    write_buffer_t changes(config.write_buffer_max_bytes, config.write_buffer_max_elements, resources_);
    transaction_unique_ptr_t transaction {new (std::nothrow) transaction_t {id, *this, std::move(changes), resources_}};
    if (!transaction) {
        roster_.retire(id);
        return status_t {status_code_t::out_of_memory_k, "No room for another transaction"};
    }
    return transaction;
}

#pragma endregion Interface Methods

#pragma region Transactions

template <typename stored_key_, typename store_type_>
inline status_t database_transaction_registry<stored_key_, store_type_>::stage_(
    transaction_id_t id, transaction_state_t& data, transaction_stamp_t& try_to_stage_stamp) noexcept {

    roster_.claim_staging(id);
    store_->acquire_write_path();
    try_to_stage_stamp = record_.watch_stamp();

    // A transaction that neither wrote nor watched anything stages by having nothing to check.
    if (data.empty()) return status_t {};

    if (conflicting_(data)) {
        discard_(id);
        return status_t {status_code_t::write_conflict_k, "Keys conflict"};
    }

    // Without writes, the absence of watched key collisions is the whole of the stage.
    if (data.changes.empty()) return status_t {};

    status_t const prepared = prepare_collections_(data.changes);
    if (prepared.failed()) discard_(id);
    return prepared;
}

template <typename stored_key_, typename store_type_>
inline status_t database_transaction_registry<stored_key_, store_type_>::commit_(
    transaction_id_t id, transaction_state_t& data, transaction_stamp_t& try_to_commit_stamp) noexcept {

    if (!roster_.staged_by(id)) {
        // Staging inside the commit makes both stamps equal, so a staged commit reports one instant.
        transaction_stamp_t staged_stamp = invalid_transaction_stamp_k;
        status_t const staged = stage_(id, data, staged_stamp);
        if (staged.failed()) {
            // The changes stay exactly as they were, so a caller can still log what it could not commit.
            try_to_commit_stamp = record_.watch_stamp();
            return staged;
        }
    }

    transaction_stamp_t const commit_stamp = record_.issue_stamp();
    try_to_commit_stamp = commit_stamp;

    status_t status = status_t {};
    if (!data.changes.empty()) {
        // The warning is published before the writes it warns about, so a reader never sees the
        // second without the first, and a refused write leaves a conflict that never happened.
        record_commit_(data.changes, commit_stamp);
        status = apply_(data.changes);
    }

    // Only now may a transaction start after this commit, because only now has all of it landed.
    record_.publish_stamp(commit_stamp);
    discard_(id);
    if (status.succeeded()) invalidate_(id);
    return status;
}

template <typename stored_key_, typename store_type_>
inline void database_transaction_registry<stored_key_, store_type_>::discard_(
    [[maybe_unused]] transaction_id_t id) noexcept {

    assert(roster_.staged_by(id) && "The transaction is not in committing state");
    store_->release_write_path();
    roster_.release_staging();
}

template <typename stored_key_, typename store_type_>
inline transaction_stamp_t database_transaction_registry<stored_key_, store_type_>::request_start_(
    transaction_id_t id) noexcept {

    // A start consumes no stamp: it only has to order before every commit that follows it.
    transaction_stamp_t const start_stamp = record_.watch_stamp();
    roster_.start(id, start_stamp);
    return start_stamp;
}

template <typename stored_key_, typename store_type_>
inline void database_transaction_registry<stored_key_, store_type_>::notify_about_deletion_(
    transaction_id_t id) noexcept {

    if (roster_.staged_by(id)) discard_(id);
    roster_.stop(id);
    roster_.retire(id);
}

template <typename stored_key_, typename store_type_>
inline status_t database_transaction_registry<stored_key_, store_type_>::watch_now_(
    transaction_stamp_t start_stamp, collection_id_t group_id,
    std::span<stored_key_t const> stored_keys) const noexcept {

    for (stored_key_t const& stored_key : stored_keys)
        if (record_.conflicts(watched_key_fingerprint(group_id, stored_key), start_stamp))
            return status_t {status_code_t::write_conflict_k, "Keys conflict"};
    return status_t {};
}

#pragma endregion Transactions

#pragma region Helper Methods

template <typename stored_key_, typename store_type_>
inline bool database_transaction_registry<stored_key_, store_type_>::conflicting_(
    transaction_state_t const& data) const noexcept {

    bool conflict = false;
    [[maybe_unused]] st::status_t const walked = data.read_set.groups.for_each(
        [&](auto const& group) noexcept -> st::walk_control_t {
            collection_id_t const group_id = group.key();
            [[maybe_unused]] st::status_t const watched = group.value().stored_keys.for_each(
                [&](auto const& watch) noexcept -> st::walk_control_t {
                    conflict = record_.conflicts(watched_key_fingerprint(group_id, watch.key()),
                                                 watch.value().watch_stamp);
                    return conflict ? st::walk_control_t::halt_k : st::walk_control_t::resume_k;
                });
            return conflict ? st::walk_control_t::halt_k : st::walk_control_t::resume_k;
        });
    return conflict;
}

template <typename stored_key_, typename store_type_>
inline void database_transaction_registry<stored_key_, store_type_>::record_commit_(
    write_buffer_t const& changes, transaction_stamp_t commit_stamp) noexcept {

    [[maybe_unused]] st::status_t const walked = changes.groups_by_id.for_each([&](auto const& group) noexcept {
        collection_id_t const group_id = group.key();
        [[maybe_unused]] st::status_t const written = group.value().stored_values.for_each(
            [&](auto const& entry) noexcept {
                record_.record(watched_key_fingerprint(group_id, entry.key()), commit_stamp);
            });
    });
}

template <typename stored_key_, typename store_type_>
inline status_t database_transaction_registry<stored_key_, store_type_>::prepare_collections_(
    write_buffer_t const& changes) noexcept {

    status_t status = status_t {};
    changes.for_each_group([&](group_info_t const& group) noexcept {
        if (status.failed()) return;
        if (!store_->contains_collection(group.id)) {
            status = status_t {status_code_t::write_conflict_k, "A collection the transaction wrote is gone"};
            return;
        }
        if (!store_->collection_fits(group.id, group.elements_count, group.elements_size_bytes))
            status = store_->flush_collection(group.id);
    });
    return status;
}

template <typename stored_key_, typename store_type_>
inline status_t database_transaction_registry<stored_key_, store_type_>::apply_(
    write_buffer_t const& changes) noexcept {

    status_t status = status_t {};
    [[maybe_unused]] st::status_t const walked = changes.groups_by_id.for_each(
        [&](auto const& group) noexcept -> st::walk_control_t {
            collection_id_t const group_id = group.key();
            [[maybe_unused]] st::status_t const applied = group.value().stored_values.for_each(
                [&](auto const& entry) noexcept -> st::walk_control_t {
                    auto const& place = entry.value();
                    if (place.offset == tombstone_offset_k)
                        status = store_->remove_from_collection(group_id, entry.key());
                    else if (place.offset == empty_stored_value_offset_k)
                        status = store_->insert_in_collection(group_id, entry.key(), stored_value_view_t {});
                    else
                        status = store_->insert_in_collection(
                            group_id, entry.key(),
                            as_bytes_view(changes.insertions_buffer).subspan(place.offset, place.size));
                    return status.failed() ? st::walk_control_t::halt_k : st::walk_control_t::resume_k;
                });
            return status.failed() ? st::walk_control_t::halt_k : st::walk_control_t::resume_k;
        });
    return status;
}

#pragma endregion Helper Methods

} // namespace unum::ustore::nvme
