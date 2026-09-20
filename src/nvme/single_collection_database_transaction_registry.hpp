/**
 *  @file src/nvme/single_collection_database_transaction_registry.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Registers single-collection database transactions, detects key conflicts and orders their commits.
 *
 *  The same machinery its multi-collection twin uses, over one collection instead of many: no locks,
 *  a stamp counter for the order, one @c fetch_max per committed key for the record, and the staging
 *  lease as the layer's single exclusion.
 */
#pragma once

#include <cassert> // `assert`
#include <cstddef> // `std::size_t`

#include <concepts> // `std::same_as`
#include <memory>   // `std::unique_ptr`
#include <new>      // `std::nothrow`
#include <span>     // `std::span`
#include <utility>  // `std::move`

#include "nvme/read_set.hpp"                                     // `commit_record_t`, `transaction_roster_t`
#include "nvme/single_collection_database_transaction_state.hpp" // `single_collection_database_transaction_state`

namespace unum::ustore::nvme {

template <typename stored_key_, value_layout_t value_layout_>
class single_collection_database;

template <typename stored_key_, typename store_type_>
class single_collection_database_transaction;

#pragma region Store Interface

/**
 *  @brief What a registry needs from the single collection beneath it to stage and to commit.
 *
 *  A whole write buffer travels across this boundary rather than one key at a time, because the sole
 *  collection admits the batch in one call and has nothing to group it by.
 */
template <typename store_type_, typename write_buffer_type_>
concept single_collection_transaction_store_concept = requires(store_type_& store, store_type_ const& constant,
                                                               write_buffer_type_ const& changes) {
    { store.acquire_write_path() } -> std::same_as<void>;
    { store.release_write_path() } -> std::same_as<void>;
    { constant.collection_fits(changes) } -> std::same_as<bool>;
    { store.flush_collection() } -> std::same_as<status_t>;
    { store.insert_write_buffer(changes) } -> std::same_as<status_t>;
};

#pragma endregion Store Interface

/**
 *  @brief The transactions of one single-collection database, their commit order and their conflicts.
 *  @tparam stored_key_ The key the collection is addressed by.
 *  @tparam store_type_ The collection the commits land in, satisfying @c single_collection_transaction_store_concept.
 */
template <typename stored_key_, typename store_type_>
class single_collection_database_transaction_registry {
  public:
#pragma region Types

    using stored_key_t = stored_key_;
    using store_t = store_type_;
    using transaction_t = single_collection_database_transaction<stored_key_, store_type_>;
    using transaction_state_t = single_collection_database_transaction_state<stored_key_t>;
    using write_buffer_t = typename transaction_state_t::write_buffer_t;
    using transaction_unique_ptr_t = std::unique_ptr<transaction_t>;
    using return_transaction_t = expected<transaction_unique_ptr_t>;

    /** The commit-record slots a registry keeps when its owner names none. */
    static constexpr std::size_t default_conflict_slots_k = 1ull << 16;

    /** The transactions a registry admits at once when its owner names no count. */
    static constexpr std::size_t default_transaction_slots_k = 256;

#pragma endregion Types

#pragma region Construction

    inline single_collection_database_transaction_registry(
        store_t& store, resources_shared_ptr_t resources, std::size_t conflict_slots = default_conflict_slots_k,
        std::size_t transaction_slots = default_transaction_slots_k) noexcept
        : store_(&store), resources_(std::move(resources)), record_(conflict_slots, resources_->memory_constraint),
          roster_(transaction_slots, resources_->memory_constraint) {}

    inline single_collection_database_transaction_registry(single_collection_database_transaction_registry&&) = delete;
    inline single_collection_database_transaction_registry(single_collection_database_transaction_registry const&) =
        delete;
    inline single_collection_database_transaction_registry& operator=(
        single_collection_database_transaction_registry const&) = delete;
    inline single_collection_database_transaction_registry& operator=(
        single_collection_database_transaction_registry&&) = delete;
    inline ~single_collection_database_transaction_registry() noexcept {
        assert(roster_.live() == 0 && "Transactions outlive registry");
    }

#pragma endregion Construction

#pragma region Interface Methods

    /** Whether @p id names a transaction this registry holds. */
    inline bool has_transaction(transaction_id_t id) const noexcept { return roster_.contains(id); }

    /** The bytes the conflict record and the roster together occupy, which no commit ever changes. */
    inline std::size_t shared_state_bytes() const noexcept { return record_.size_bytes() + roster_.size_bytes(); }

    inline return_transaction_t create_transaction(transaction_id_t id, transaction_config_t const& config,
                                                   std::size_t value_size, std::size_t value_max_size) noexcept;

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

    inline status_t watch_now_(transaction_stamp_t start_stamp,
                               std::span<stored_key_t const> stored_keys) const noexcept;

#pragma endregion Transactions

#pragma region Helper Methods

    inline bool conflicting_(transaction_state_t const& data) const noexcept;
    inline void record_commit_(write_buffer_t const& changes, transaction_stamp_t commit_stamp) noexcept;

#pragma endregion Helper Methods

#pragma region Store Accessor Methods

    inline void lock_shared_() const { store_->lock_shared(); }
    inline void unlock_shared_() const { store_->unlock_shared(); }

    inline auto begin_() const { return store_->begin(); }
    inline auto end_() const { return store_->end(); }
    inline auto find_(stored_key_t stored_key) const { return store_->find(stored_key); }
    inline auto find_or_next_(stored_key_t stored_key) const { return store_->find_or_next(stored_key); }

    inline bool contains_(stored_key_t stored_key) const noexcept { return store_->contains(stored_key); }
    inline status_t stored_value_size_(stored_key_t stored_key, stored_value_size_t& size) const noexcept {
        return store_->stored_value_size(stored_key, size);
    }
    inline stored_value_size_t buffer_size_for_any_stored_value_() const noexcept {
        return store_->buffer_size_for_any_stored_value();
    }

    inline status_t select_(stored_key_t stored_key, stored_value_span_t stored_value,
                            stored_value_size_t& size) const noexcept {
        return store_->select(stored_key, stored_value, size);
    }
    inline status_t find_(std::span<stored_key_t const> stored_keys, auto stored_value_locations,
                          missing_locations_t missing = missing_locations_t::invalidate_k) const noexcept {
        return store_->find(stored_keys, stored_value_locations, missing);
    }
    inline status_t select_(auto stored_value_locations, bytes_span_t buffer, notifier_t notifier) const noexcept {
        return store_->select(stored_value_locations, buffer, notifier);
    }

#pragma endregion Store Accessor Methods

#pragma region Private Members

    /** The collection every commit of this registry lands in, owned by whoever named it. */
    store_t* store_;

    /** The budget the record, the roster and every transaction's writes are charged to. */
    resources_shared_ptr_t resources_;

    /** The order every transaction is placed in, and the last commit to write each key. */
    commit_record_t record_;

    /** The live transactions and the staging lease. */
    transaction_roster_t roster_;

#pragma endregion Private Members
};

#pragma region Interface Methods

template <typename stored_key_, typename store_type_>
inline typename single_collection_database_transaction_registry<stored_key_, store_type_>::return_transaction_t
single_collection_database_transaction_registry<stored_key_, store_type_>::create_transaction(
    transaction_id_t id, transaction_config_t const& config, std::size_t value_size,
    std::size_t value_max_size) noexcept {

    status_t const enrolled = roster_.enroll(id);
    if (enrolled.failed()) return enrolled;

    write_buffer_t changes(config.write_buffer_max_bytes, config.write_buffer_max_elements, 1, value_size,
                           value_max_size, resources_);
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
inline status_t single_collection_database_transaction_registry<stored_key_, store_type_>::stage_(
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
    if (store_->collection_fits(data.changes)) return status_t {};

    status_t const flushed = store_->flush_collection();
    if (flushed.failed()) discard_(id);
    return flushed;
}

template <typename stored_key_, typename store_type_>
inline status_t single_collection_database_transaction_registry<stored_key_, store_type_>::commit_(
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
        status = store_->insert_write_buffer(data.changes);
    }

    // Only now may a transaction start after this commit, because only now has all of it landed.
    record_.publish_stamp(commit_stamp);
    discard_(id);
    if (status.succeeded()) invalidate_(id);
    return status;
}

template <typename stored_key_, typename store_type_>
inline void single_collection_database_transaction_registry<stored_key_, store_type_>::discard_(
    [[maybe_unused]] transaction_id_t id) noexcept {

    assert(roster_.staged_by(id) && "The transaction is not in committing state");
    store_->release_write_path();
    roster_.release_staging();
}

template <typename stored_key_, typename store_type_>
inline transaction_stamp_t single_collection_database_transaction_registry<stored_key_, store_type_>::request_start_(
    transaction_id_t id) noexcept {

    // A start consumes no stamp: it only has to order before every commit that follows it.
    transaction_stamp_t const start_stamp = record_.watch_stamp();
    roster_.start(id, start_stamp);
    return start_stamp;
}

template <typename stored_key_, typename store_type_>
inline void single_collection_database_transaction_registry<stored_key_, store_type_>::notify_about_deletion_(
    transaction_id_t id) noexcept {

    if (roster_.staged_by(id)) discard_(id);
    roster_.stop(id);
    roster_.retire(id);
}

template <typename stored_key_, typename store_type_>
inline status_t single_collection_database_transaction_registry<stored_key_, store_type_>::watch_now_(
    transaction_stamp_t start_stamp, std::span<stored_key_t const> stored_keys) const noexcept {

    for (stored_key_t const& stored_key : stored_keys)
        if (record_.conflicts(watched_key_fingerprint(sole_collection_id_k, stored_key), start_stamp))
            return status_t {status_code_t::write_conflict_k, "Keys conflict"};
    return status_t {};
}

#pragma endregion Transactions

#pragma region Helper Methods

template <typename stored_key_, typename store_type_>
inline bool single_collection_database_transaction_registry<stored_key_, store_type_>::conflicting_(
    transaction_state_t const& data) const noexcept {

    bool conflict = false;
    [[maybe_unused]] st::status_t const walked = data.read_set.stored_keys.for_each(
        [&](auto const& watch) noexcept -> st::walk_control_t {
            conflict = record_.conflicts(watched_key_fingerprint(sole_collection_id_k, watch.key()),
                                         watch.value().watch_stamp);
            return conflict ? st::walk_control_t::halt_k : st::walk_control_t::resume_k;
        });
    return conflict;
}

template <typename stored_key_, typename store_type_>
inline void single_collection_database_transaction_registry<stored_key_, store_type_>::record_commit_(
    write_buffer_t const& changes, transaction_stamp_t commit_stamp) noexcept {

    [[maybe_unused]] st::status_t const written = changes.stored_values.for_each([&](auto const& entry) noexcept {
        record_.record(watched_key_fingerprint(sole_collection_id_k, entry.key()), commit_stamp);
    });
}

#pragma endregion Helper Methods

} // namespace unum::ustore::nvme
