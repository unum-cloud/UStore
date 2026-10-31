/**
 *  @file include/ustore/blobs/nvme_single_collection_database.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Single-collection databases, with their snapshots, their optimistic transactions, and the registries that
 *      order those transactions.
 *
 *  A single-collection database is the smallest thing that can be committed to: one collection, the
 *  registry that orders the commits landing in it, and two blobs on the volume - the schema a reopen
 *  checks itself against, and whatever bytes the owner keeps beside the data.
 *  Reads take the caller's channel, so any number of threads read at once; flushes, compactions and blob writes
 *  take the two channels the database was opened with, under its claim, which one thread holds at a time.
 */

#pragma once

#include <cassert> // `assert`
#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint16_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::memset`

#include <array>       // `std::array`
#include <concepts>    // `std::same_as`
#include <memory>      // `std::unique_ptr`
#include <new>         // `std::nothrow`, placement `new`
#include <span>        // `std::span`
#include <type_traits> // `std::remove_reference_t`
#include <utility>     // `std::declval`, `std::exchange`, `std::forward`, `std::move`

#include <smashtable/basic_hash_table.hpp> // `st::assume_reserved`
#include <smashtable/basic_vector.hpp>     // `st::basic_vector`
#include <smashtable/shared.hpp>           // `st::walk_control_t`

#include "ustore/blobs/nvme_database.hpp"           // `read_set`, `maintenance_claim`, `watch_schedule_t`
#include "ustore/blobs/nvme_key_slot.hpp"           // `key_slot`, `value_layout_t`, `collection_id_t`
#include "ustore/blobs/nvme_store.hpp"              // `nvme_store_cursor`, `commit_ledger`, `sealed_commit`
#include "ustore/blobs/nvme_transaction_deltas.hpp" // `transaction_deltas`
#include "ustore/blobs/nvme_tree.hpp"               // `lsm_tree_run_record_t`, `lsm_tree_publication_hook_t`
#include "ustore/utilities/nvme_channels.hpp"       // `io_channel_t`
#include "ustore/utilities/types.hpp"               // `status_t`, `status_code_t`, `expected`

namespace unum::ustore::blobs {

#pragma region Single Collection Database Transaction State

/** The ID a single-collection transaction files its deltas under, the nil UUID, as the database has one collection. */
inline constexpr collection_id_t single_collection_id_k {};

template <typename stored_key_, template <typename> typename atomic_reference_, typename waiter_type_>
struct single_collection_database_transaction_state {
    using stored_key_t = stored_key_;
    using transaction_deltas_t = transaction_deltas<stored_key_t, atomic_reference_, waiter_type_>;
    using read_set_t = read_set<stored_key_t, atomic_reference_, waiter_type_>;

    inline single_collection_database_transaction_state(
        transaction_deltas_t&& written, resources_shared_ptr<atomic_reference_, waiter_type_> resources) noexcept
        : deltas(std::move(written)), read_set(resources) {}

    inline bool empty() const noexcept { return deltas.empty() && read_set.empty(); }

    inline void clear() {
        deltas.clear();
        read_set.clear();
        sealed = sealed_commit<stored_key_t, atomic_reference_> {};
    }

    /** The deltas the transaction wrote, filed under the one collection's ID. */
    transaction_deltas_t deltas;
    read_set_t read_set;

    /** The writes sealed by a stage or a commit, emptied once it lands or is refused. */
    sealed_commit<stored_key_t, atomic_reference_> sealed;
};

#pragma endregion Single Collection Database Transaction State

#pragma region Single Collection Database Transaction Registry

template <typename stored_key_, value_layout_t value_layout_>
class single_collection_database;

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiter_type_>
class single_collection_database_transaction;

#pragma region Store Interface

/**
 *  @brief What a registry needs from the single collection beneath it to stage and to commit.
 *
 *  A transaction's whole deltas travel across this boundary rather than one key at a time, because the sole
 *  collection seals the batch in one call and has nothing to group it by. The committer seals on its own thread;
 *  the thread holding the ledger's leader word prepares, publishes, or forgets what it prepared; and the committer
 *  maintains the collection once its commit lands.
 */
template <typename store_type_, typename stored_key_type_, template <typename> typename atomic_reference_,
          typename waiter_type_>
concept single_collection_transaction_store_concept = requires(
    store_type_& store, store_type_ const& constant,
    transaction_deltas<stored_key_type_, atomic_reference_, waiter_type_> const& deltas,
    sealed_commit<stored_key_type_, atomic_reference_>& sealed, stored_key_type_ stored_key,
    transaction_stamp_t stamp) {
    { store.ledger() } -> std::same_as<commit_ledger<atomic_reference_, waiter_type_>&>;
    { constant.written_since(stored_key, stamp) } -> std::same_as<bool>;
    { store.seal_deltas(deltas, sealed) } -> std::same_as<status_t>;
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
          typename waiter_type_>
class single_collection_database_transaction_registry {
  public:
#pragma region Types

    using stored_key_t = stored_key_;
    using store_t = store_type_;
    using transaction_t =
        single_collection_database_transaction<stored_key_, store_type_, atomic_reference_, waiter_type_>;
    using transaction_state_t =
        single_collection_database_transaction_state<stored_key_t, atomic_reference_, waiter_type_>;
    using transaction_deltas_t = typename transaction_state_t::transaction_deltas_t;
    using transaction_unique_ptr_t = std::unique_ptr<transaction_t>;
    using return_transaction_t = expected<transaction_unique_ptr_t>;

#pragma endregion Types

#pragma region Construction

    /** A registry committing into @p store, which outlives it. */
    inline single_collection_database_transaction_registry(
        store_t& store, resources_shared_ptr<atomic_reference_, waiter_type_> resources) noexcept
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

    /** A transaction whose deltas @p config bounds. */
    inline return_transaction_t create_transaction(transaction_config_t const& config) noexcept;

#pragma endregion Interface Methods

  private:
#pragma region Transactions

    friend store_t;
    friend transaction_t;

    /** One transaction's commit, as the sequencer hands it to its two steps. */
    struct commit_t {

        /** The registry committing. */
        single_collection_database_transaction_registry* owner = nullptr;

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
    inline void publish_(transaction_state_t& data, transaction_stamp_t stamp) noexcept;
    static inline status_t prepare_commit_(void* context) noexcept;
    static inline void publish_commit_(void* context, transaction_stamp_t stamp) noexcept;

    /** Pins @p pin at the newest published stamp, which a transaction starts at and reads from. */
    inline transaction_stamp_t request_start_(stamp_pin<atomic_reference_, waiter_type_>& pin) noexcept {
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
    resources_shared_ptr<atomic_reference_, waiter_type_> resources_;

#pragma endregion Private Members
};

#pragma region Interface Methods

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiter_type_>
inline typename single_collection_database_transaction_registry<stored_key_, store_type_, atomic_reference_,
                                                                waiter_type_>::return_transaction_t
single_collection_database_transaction_registry<stored_key_, store_type_, atomic_reference_, waiter_type_>::
    create_transaction(transaction_config_t const& config) noexcept {

    transaction_deltas_t deltas(config.max_bytes, config.max_deltas, resources_);
    transaction_unique_ptr_t transaction {new (std::nothrow) transaction_t {*this, std::move(deltas), resources_}};
    if (!transaction) return status_t {status_code_t::out_of_memory_k, "No room for another transaction"};
    return transaction;
}

#pragma endregion Interface Methods

#pragma region Transactions

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiter_type_>
inline status_t
single_collection_database_transaction_registry<stored_key_, store_type_, atomic_reference_, waiter_type_>::stage_(
    transaction_state_t& data, transaction_stamp_t start_stamp, transaction_stamp_t& stage_stamp) noexcept {

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
          typename waiter_type_>
inline transaction_stamp_t single_collection_database_transaction_registry<
    stored_key_, store_type_, atomic_reference_, waiter_type_>::commit_staged_(transaction_state_t& data) noexcept {

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
          typename waiter_type_>
inline expected<transaction_stamp_t>
single_collection_database_transaction_registry<stored_key_, store_type_, atomic_reference_, waiter_type_>::commit_(
    transaction_state_t& data, transaction_stamp_t start_stamp) noexcept {

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
          typename waiter_type_>
inline void
single_collection_database_transaction_registry<stored_key_, store_type_, atomic_reference_, waiter_type_>::discard_(
    transaction_state_t& data) noexcept {
    if (data.sealed.run) store_->forget_sealed();
    store_->ledger().stop_leading();
    data.sealed = sealed_commit<stored_key_t, atomic_reference_> {};
}

/** Seals the transaction's writes on the committing thread, making room first when the memory runs are full. */
template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiter_type_>
inline status_t
single_collection_database_transaction_registry<stored_key_, store_type_, atomic_reference_, waiter_type_>::seal_(
    transaction_state_t& data) noexcept {
    data.sealed = sealed_commit<stored_key_t, atomic_reference_> {};
    if (data.deltas.empty()) return status_t {};
    return store_->seal_deltas(data.deltas, data.sealed);
}

/** Checks the watches against the views published so far and lays out the collection's view, on the thread holding
 *  the leader word; a refusal forgets the view. */
template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiter_type_>
inline status_t
single_collection_database_transaction_registry<stored_key_, store_type_, atomic_reference_, waiter_type_>::prepare_(
    transaction_state_t& data, transaction_stamp_t start_stamp) noexcept {
    // Every watch is scoped from the start or later, so nothing published since the start leaves nothing to check.
    if (current_stamp_() != start_stamp && conflicting_(data))
        return status_t {status_code_t::write_conflict_k, "Keys conflict"};
    if (!data.sealed.run) return status_t {};
    return store_->prepare_sealed(data.sealed);
}

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiter_type_>
inline void
single_collection_database_transaction_registry<stored_key_, store_type_, atomic_reference_, waiter_type_>::publish_(
    transaction_state_t& data, transaction_stamp_t stamp) noexcept {
    if (data.sealed.run) store_->publish_sealed(data.sealed, stamp);
}

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiter_type_>
inline status_t single_collection_database_transaction_registry<stored_key_, store_type_, atomic_reference_,
                                                                waiter_type_>::prepare_commit_(void* context) noexcept {
    commit_t const& committing = *static_cast<commit_t const*>(context);
    return committing.owner->prepare_(*committing.data, committing.start_stamp);
}

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiter_type_>
inline void single_collection_database_transaction_registry<
    stored_key_, store_type_, atomic_reference_, waiter_type_>::publish_commit_(void* context,
                                                                                transaction_stamp_t stamp) noexcept {
    commit_t const& committing = *static_cast<commit_t const*>(context);
    committing.owner->publish_(*committing.data, stamp);
}

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiter_type_>
inline status_t
single_collection_database_transaction_registry<stored_key_, store_type_, atomic_reference_, waiter_type_>::watch_now_(
    transaction_stamp_t start_stamp, std::span<stored_key_t const> stored_keys) const noexcept {

    bool written = false;
    for (std::size_t index = 0; index != stored_keys.size() && !written; ++index)
        written = store_->written_since(stored_keys[index], start_stamp);
    if (written) return status_t {status_code_t::write_conflict_k, "Keys conflict"};
    return status_t {};
}

#pragma endregion Transactions

#pragma region Helper Methods

template <typename stored_key_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiter_type_>
inline bool single_collection_database_transaction_registry<stored_key_, store_type_, atomic_reference_,
                                                            waiter_type_>::conflicting_(transaction_state_t const& data)
    const noexcept {

    // First-committer-wins over the watched writes, then every key read or tracked.
    bool conflict = false;
    data.deltas.for_each_write([&](collection_id_t, stored_key_t const& stored_key, auto const& place) noexcept {
        if (!conflict && place.watch_stamp != invalid_transaction_stamp_k)
            conflict = store_->written_since(stored_key, place.watch_stamp);
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
     *  @param[out] size Bytes the value takes, which is zero for anything but @c written_k.
     */
    [[nodiscard]] lookup_result_t find(stored_key_t stored_key, stored_value_size_t& size) const noexcept {
        return store_->find(stored_key, size, stamp_);
    }

    /**
     *  @brief Copies the value of @p stored_key into @p into, which needs only the value's own bytes.
     *  @param[in,out] channel The channel the read is submitted and completed on, held by the calling thread.
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
     *  @param[in,out] channel The channel the reads are submitted and completed on, held by the calling thread.
     *  @param[in] stored_keys The keys to read, in any order and with any number of misses among them.
     *  @param[out] results What every key answered, which separates a miss from a removal.
     *  @param[in] sink Called as @c sink(request, offset, bytes) for every key that locates a value.
     */
    template <typename sink_type_>
    status_t select(io_channel_t& channel, std::span<stored_key_t const> stored_keys,
                    std::span<lookup_result_t> results, sink_type_&& sink) const noexcept {
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
          typename waiter_type_>
class single_collection_database_transaction {
  public:

#pragma region Types

    /** The key the database is addressed by. */
    using stored_key_t = stored_key_;

    /** The database every commit of this transaction lands in. */
    using store_t = store_type_;

    /** The registry that orders this transaction against every other one of the database. */
    using registry_t =
        single_collection_database_transaction_registry<stored_key_, store_type_, atomic_reference_, waiter_type_>;

    /** The uncommitted changes and the watched keys of this transaction. */
    using transaction_state_t =
        single_collection_database_transaction_state<stored_key_t, atomic_reference_, waiter_type_>;

    /** The deltas the uncommitted writes live in. */
    using transaction_deltas_t = typename transaction_state_t::transaction_deltas_t;

#pragma endregion Types

#pragma region Construction

    single_collection_database_transaction(single_collection_database_transaction const&) = delete;
    single_collection_database_transaction& operator=(single_collection_database_transaction const&) = delete;
    single_collection_database_transaction(single_collection_database_transaction&&) = delete;
    single_collection_database_transaction& operator=(single_collection_database_transaction&&) = delete;

    /**
     *  @brief A transaction started at the newest stamp @p manager has published, which it pins until it dies.
     *  @param[in,out] manager The registry this transaction stages and commits through, outliving it.
     *  @param[in] deltas The deltas the uncommitted writes land in, moved in and owned from here on.
     *  @param[in] resources The budget the watched keys are charged to.
     */
    single_collection_database_transaction(registry_t& manager, transaction_deltas_t&& deltas,
                                           resources_shared_ptr<atomic_reference_, waiter_type_> resources) noexcept
        : manager_(manager), data_(std::move(deltas), std::move(resources)) {
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
        data_.deltas.rescope_watches(start_stamp_);
        return {};
    }

#pragma endregion Two Phase Commit

#pragma region Writes

    /** Whether @p count more values of @p bytes together still fit in this transaction's deltas. */
    [[nodiscard]] bool has_available_space_for(std::size_t count, std::size_t bytes) const noexcept {
        return state_ == state_t::started_k && data_.deltas.can_fit(count, bytes);
    }

    /** Writes @p stored_value under @p stored_key, which reaches the database only at the commit, watching the key
     *  for another commit writing it as @p watch_schedule_ says. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t insert(stored_key_t stored_key, std::span<byte_t const> stored_value) noexcept {
        if (state_ != state_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        if (!manager_.store_->admits(stored_value.size()))
            return {status_code_t::malformed_k, "This database admits no value of that size"};
        if (status_t const checked = check_write_now_<watch_schedule_>(stored_key); checked.failed()) return checked;
        return data_.deltas.write(single_collection_id_k, manager_.store_->value_widths(), stored_key, stored_value,
                                  watch_stamp_for_<watch_schedule_>());
    }

    /** Removes @p stored_key, which reaches the database as a remove delta only at the commit, watched as @c insert
     *  is. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t remove(stored_key_t stored_key) noexcept {
        if (state_ != state_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        if (status_t const checked = check_write_now_<watch_schedule_>(stored_key); checked.failed()) return checked;
        return data_.deltas.remove(single_collection_id_k, manager_.store_->value_widths(), stored_key,
                                   watch_stamp_for_<watch_schedule_>());
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
     *  @param[out] size Bytes the value takes, which is zero for anything but @c written_k.
     */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    [[nodiscard]] lookup_result_t find(stored_key_t stored_key, stored_value_size_t& size) const noexcept {
        size = 0;
        if (state_ != state_t::started_k) return lookup_result_t::missing_k;
        lookup_result_t const own = find_uncommitted(stored_key, size);
        if (own != lookup_result_t::missing_k) return own;

        std::span<stored_key_t const> const only {&stored_key, 1};
        transaction_stamp_t const watch_from = watch_stamp_for_<watch_schedule_>();
        if (watch_now_or_prepare_<watch_schedule_>(only).failed()) return lookup_result_t::missing_k;
        lookup_result_t const carried = manager_.store_->find(stored_key, size, read_stamp_for_<watch_schedule_>());
        watch_later_<watch_schedule_>(only, watch_from, watch_purpose_t::read_k);
        return carried;
    }

    /** What this transaction's own deltas hold under @p stored_key, without touching the database. */
    [[nodiscard]] lookup_result_t find_uncommitted(stored_key_t stored_key, stored_value_size_t& size) const noexcept {
        size = 0;
        stored_value_size_t held = 0;
        lookup_result_t const own = visit_uncommitted_(stored_key, [&](std::span<byte_t const> bytes) noexcept {
            held = static_cast<stored_value_size_t>(bytes.size());
        });
        size = held;
        return own;
    }

    /**
     *  @brief Copies the value of @p stored_key into @p into, from this transaction or from the database.
     *  @param[in,out] channel The channel a read of the database is submitted and completed on, held by the caller.
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
        lookup_result_t const own = visit_uncommitted_(stored_key, [&](std::span<byte_t const> bytes) noexcept {
            if (into.size() < bytes.size()) {
                refused = true;
                return;
            }
            if (!bytes.empty()) std::memcpy(into.data(), bytes.data(), bytes.size());
            size = static_cast<stored_value_size_t>(bytes.size());
        });
        if (refused) return {status_code_t::invalid_argument_k, "A buffer shorter than the value"};
        if (own == lookup_result_t::removed_k) return {status_code_t::not_found_k, "This transaction removed that key"};
        if (own == lookup_result_t::written_k) return {};

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
     *  @param[in,out] channel The channel a read of the database is submitted and completed on, held by the caller.
     *  @param[in] stored_keys The keys to read, in any order and with any number of misses among them.
     *  @param[out] results What every key answered, which separates a miss from a removal.
     *  @param[in] sink Called as @c sink(request, offset, bytes) for every key that locates a value.
     *
     *  @note A value this transaction wrote arrives as one call carrying all of it, while one the
     *      database answers arrives as one call per block, exactly as a read of the database does.
     */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k, typename sink_type_>
    status_t select(io_channel_t& channel, std::span<stored_key_t const> stored_keys,
                    std::span<lookup_result_t> results, sink_type_&& sink) const noexcept {
        if (state_ != state_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        if (results.size() != stored_keys.size())
            return {status_code_t::invalid_argument_k, "A batched select answers one result per key"};

        st::basic_vector<stored_key_t> deferred_keys;
        st::basic_vector<std::size_t> deferred_requests;
        if (deferred_keys.reserve(stored_keys.size()) != st::status_t::success_k ||
            deferred_requests.reserve(stored_keys.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A transaction could not track the keys of a batch"};

        for (std::size_t request = 0; request != stored_keys.size(); ++request) {
            lookup_result_t const own = visit_uncommitted_(
                stored_keys[request],
                [&](std::span<byte_t const> bytes) noexcept { sink(request, std::uint64_t {0}, bytes); });
            results[request] = own;
            if (own != lookup_result_t::missing_k) continue;
            [[maybe_unused]] st::status_t const named = deferred_keys.push_back(stored_key_t {stored_keys[request]});
            [[maybe_unused]] st::status_t const tracked = deferred_requests.push_back(std::size_t {request});
        }
        if (deferred_keys.empty()) return {};

        st::basic_vector<lookup_result_t> deferred_results;
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
        status_t const read = manager_.store_->select(channel, watched, std::span<lookup_result_t> {deferred_results},
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
     *  @param[in] stored_key The key to look for in this transaction's own deltas.
     *  @param[in] visitor Called once with the written bytes, which are empty for an empty value.
     *  @return What the deltas hold, which is @c missing_k when they hold nothing at all.
     */
    template <typename visitor_type_>
    [[nodiscard]] lookup_result_t visit_uncommitted_(stored_key_t stored_key, visitor_type_&& visitor) const noexcept {
        delta_view_t held;
        if (!data_.deltas.delta(single_collection_id_k, stored_key, held)) return lookup_result_t::missing_k;
        if (held.kind == delta_kind_t::remove_k) return lookup_result_t::removed_k;
        visitor(held.bytes);
        return lookup_result_t::written_k;
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
    stamp_pin<atomic_reference_, waiter_type_> pin_;

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
    schema_stamps_t stamps;
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
          typename waiter_type_, nvme_store_concept store_>
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
    using scanner_t = nvme_store_cursor<key_slot_t, volume_t, atomic_reference_, waiter_type_,
                                        typename collection_t::fold_t, typename collection_t::device_fold_t>;

    /** One moment of this database, a pinned stamp no publication waits behind. */
    using snapshot_t = single_collection_database_snapshot<basic_single_collection_database>;

    /** One stamp a snapshot or a transaction holds against the ledger. */
    using pin_t = stamp_pin<atomic_reference_, waiter_type_>;

    /** One optimistic transaction over this database. */
    using transaction_t = single_collection_database_transaction<stored_key_t, basic_single_collection_database,
                                                                 atomic_reference_, waiter_type_>;

    /** The order every transaction of this database is placed in, and its conflicts. */
    using registry_t = single_collection_database_transaction_registry<stored_key_t, basic_single_collection_database,
                                                                       atomic_reference_, waiter_type_>;

    /** The uncommitted deltas of one transaction, as the registry hands them over. */
    using transaction_deltas_t = transaction_deltas<stored_key_t, atomic_reference_, waiter_type_>;

    /** The value widths the collection admits, which every write of a transaction is checked against. */
    using value_widths_t = typename transaction_deltas_t::value_widths_t;

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

    /** Writes whatever the memory runs still hold onto the volume and names every run, so the data outlives this. */
    ~basic_single_collection_database() noexcept {
        // A database that never mounted whole, or was destroyed, names no schema and must not write one back.
        if (generation_ == 0) return;
        claim_guard_t const held {*this};
        if (held.adopted().failed()) return;
        [[maybe_unused]] status_t const flushed = collection_.flush(writing_channel_);
        [[maybe_unused]] status_t const saved = save_schema_();
    }

    /**
     *  @brief Opens a database over @p volume, creating its schema blob when the volume holds none.
     *  @param[in,out] volume The volume every run and blob lands on, borrowed and outliving the database.
     *  @param[in,out] writing_channel The channel every write, flush and commit runs on, borrowed for the database's
     *      life.
     *  @param[in,out] merging_channel The second channel a compaction writes its new runs on, which is not @p
     *      writing_channel.
     *  @param[in] config The collection's bounds and the transaction defaults.
     *  @param[in] resources The budget the conflict record, the roster and every transaction's writes are charged to.
     *  @param[in] compactor What every merge runs on: the calling thread, or a device compactor the caller owns, which
     *      outlives the database.
     *  @return The database, or @c malformed_k for a volume whose schema names another value layout.
     */
    static expected<database_unique_ptr_t> open(volume_t& volume, io_channel_t& writing_channel,
                                                io_channel_t& merging_channel,
                                                single_collection_database_config_t const& config,
                                                resources_shared_ptr<atomic_reference_, waiter_type_> resources,
                                                compactor_ref_t compactor = {}) noexcept {
        if (!resources) return status_t {status_code_t::invalid_argument_k, "A database is charged to some budget"};
        std::unique_ptr<commit_ledger<atomic_reference_, waiter_type_>> owned {
            new (std::nothrow) commit_ledger<atomic_reference_, waiter_type_> {}};
        if (!owned) return status_t {status_code_t::out_of_memory_k, "The database did not fit"};
        commit_ledger<atomic_reference_, waiter_type_>& ledger = *owned;
        return open_(volume, writing_channel, merging_channel, config, std::move(resources), compactor, ledger,
                     std::move(owned));
    }

    /** Drops both blobs and every run, which destroys the data and leaves the volume empty of it. */
    status_t destroy() noexcept {
        claim_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        generation_ = 0;
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

    /** Deltas committed into memory runs and not yet flushed. */
    [[nodiscard]] std::size_t memory_deltas_count() const noexcept { return collection_.memory_deltas_count(); }

#pragma endregion Shape

#pragma region Lookups

    /**
     *  @brief Whether anything carries @p stored_key, and how many bytes its value takes.
     *  @param[in] stored_key The key to look for.
     *  @param[out] size Bytes the value takes, which is zero for anything but @c written_k.
     *  @param[in] stamp The stamp a reader is pinned at, which sees the newest version at or below it.
     */
    [[nodiscard]] lookup_result_t find(stored_key_t stored_key, stored_value_size_t& size,
                                       transaction_stamp_t stamp = head_stamp_k) const noexcept {
        return collection_.find(stored_key, size, stamp);
    }

    /**
     *  @brief Copies the value of @p stored_key into @p into, which needs only the value's own bytes.
     *  @param[in,out] channel The channel the read is submitted and completed on, held by the calling thread.
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
     *  @param[in,out] channel The channel the reads are submitted and completed on, held by the calling thread.
     *  @param[in] stored_keys The keys to read, in any order and with any number of misses among them.
     *  @param[out] results What every key answered, which separates a miss from a removal.
     *  @param[in] sink Called as @c sink(request, offset, bytes) for every key that locates a value.
     *  @param[in] stamp The stamp a reader is pinned at, which sees the newest version at or below it.
     */
    template <typename sink_type_>
    status_t select(io_channel_t& channel, std::span<stored_key_t const> stored_keys,
                    std::span<lookup_result_t> results, sink_type_&& sink,
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
    status_t remove(stored_key_t stored_key) noexcept {
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
        return commit_alone_(collection_.seal_writes(stored_keys, stored_values, sizes));
    }

    /** Removes every key of @p stored_keys as one commit of its own. */
    status_t remove(std::span<stored_key_t const> stored_keys) noexcept {
        return commit_alone_(collection_.seal_removals(stored_keys));
    }

#pragma endregion Writes

#pragma region Maintenance

    /** Writes every memory run as one run of level zero. */
    status_t flush() noexcept {
        claim_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        if (status_t const flushed = collection_.flush(writing_channel_); flushed.failed()) return flushed;
        // The run the flush wrote is sealed by now, so the schema may name it.
        return save_schema_();
    }

    /** Whether any level is over the bytes its number admits, answered without excluding a writer. */
    [[nodiscard]] bool needs_compaction() const noexcept { return collection_.needs_compaction(); }

    /** Merges the shallowest level that is over its bounds, reading on one channel and writing on the other. */
    expected<compaction_outcome_t> compact() noexcept {
        claim_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        lsm_tree_publication_hook_t published;
        published.notify = &record_schema_;
        published.context = this;
        return collection_.compact(writing_channel_, merging_channel_, compactor_, published);
    }

    /** How many memory runs the collection holds, and how many commits they hold. */
    [[nodiscard]] memory_shape_t memory_shape() const noexcept { return collection_.memory_shape(); }

    /** How many times the schema blob has been saved, whose @c schema_copy_of names the copy a mount reads. */
    [[nodiscard]] std::uint64_t schema_generation() const noexcept { return generation_; }

    /** Drops every version above @p ceiling, which a mount runs once its ledger has resumed at @p ceiling. */
    status_t truncate_above(transaction_stamp_t ceiling) noexcept {
        claim_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        lsm_tree_publication_hook_t published;
        published.notify = &record_schema_;
        published.context = this;
        return collection_.truncate_above(writing_channel_, merging_channel_, ceiling, published);
    }

    /** Pins the newest published stamp, at which every read through the snapshot answers, and flushes nothing. */
    expected<snapshot_t> pin_stamp() noexcept { return snapshot_t::of(*this); }

#pragma endregion Maintenance

#pragma region Transactions

    /** A transaction over this database, with the delta limits @p config names. */
    expected<transaction_unique_ptr_t> create_transaction(transaction_config_t const& config) noexcept {
        if (config.max_bytes > config_.collection.memory_runs_max_bytes ||
            config.max_deltas > config_.collection.memory_runs_max_deltas)
            return status_t {status_code_t::invalid_argument_k,
                             "A transaction may not outgrow the memory runs it commits into"};
        return registry_.create_transaction(config);
    }

    /** The widths a transaction checks every write against, exactly one of them set, as the layout fixes or bounds
     *  every value. */
    [[nodiscard]] value_widths_t value_widths() const noexcept {
        if (is_fixed(value_layout_k)) return {collection_.value_max_size(), 0};
        return {0, collection_.value_max_size()};
    }

    /** A transaction with the delta bounds this database was opened with. */
    expected<transaction_unique_ptr_t> create_transaction() noexcept {
        return create_transaction(config_.default_transaction);
    }

    /** The order every commit of this database draws its stamp from. */
    [[nodiscard]] commit_ledger<atomic_reference_, waiter_type_>& ledger() noexcept { return ledger_; }

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
        claim_guard_t const held {*this};
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
        claim_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        return drop_blob_(user_data_blob_name_());
    }

#pragma endregion User Data

#pragma region Transaction Store Interface

    /** Seals every delta of @p written into @p sealed on the committing thread, flushing first when the memory runs
     *  are full, so the commit that follows cannot fail halfway. */
    status_t seal_deltas(transaction_deltas_t const& written,
                         sealed_commit<stored_key_t, atomic_reference_>& sealed) noexcept {
        st::basic_vector<keyed_delta<stored_key_t>> deltas;
        if (deltas.reserve(written.count(single_collection_id_k)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to hand a commit over"};
        written.for_each_delta(single_collection_id_k, [&](stored_key_t stored_key, delta_view_t delta) noexcept {
            [[maybe_unused]] st::status_t const placed = deltas.push_back(
                st::assume_reserved, keyed_delta<stored_key_t> {stored_key, delta});
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
     *  @brief Merges the memory runs by tier, and flushes them and compacts the tree once they hold their bounds,
     *      which every commit runs once it lands.
     *
     *  Returns at once while another writer holds the claim, since that writer looks at the memory runs again
     *  after it lets go, so no commit waits behind another's maintenance.
     *  @note Called outside the ledger's epochs.
     */
    status_t maintain() noexcept {
        if (!collection_.maintenance_due()) return {};
        claim_guard_t const held = claim_guard_t::taken_if_free(*this);
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
    class claim_guard_t {
      public:

        /** Takes @p owner's claim, parked while another writer holds it, and adopts both channels. */
        explicit claim_guard_t(basic_single_collection_database& owner) noexcept : owner_(&owner) {
            owner.claim_.take();
            owner.adopt_channels_();
        }

        /** Takes @p owner's claim if no writer holds it, or answers a guard holding nothing. */
        [[nodiscard]] static claim_guard_t taken_if_free(basic_single_collection_database& owner) noexcept {
            claim_guard_t guard;
            if (!owner.claim_.take_if_free()) return guard;
            owner.adopt_channels_();
            guard.owner_ = &owner;
            return guard;
        }

        claim_guard_t(claim_guard_t const&) = delete;
        claim_guard_t& operator=(claim_guard_t const&) = delete;
        claim_guard_t(claim_guard_t&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}
        ~claim_guard_t() noexcept {
            if (owner_) owner_->release_claim_();
        }

        /** Whether this guard holds the claim. */
        [[nodiscard]] bool holding() const noexcept { return owner_ != nullptr; }

        /** Whether this thread took the channels over, which every write through them needs. */
        status_t adopted() const noexcept { return owner_->channel_adoption_; }

      private:

        claim_guard_t() noexcept = default;

        /** The database whose collection this guard holds, borrowed for the guard's life, null once moved from. */
        basic_single_collection_database* owner_ = nullptr;
    };

#pragma endregion Private Types

#pragma region Private Construction

    /** Takes what @c open assembled, and points the registry at the database it will commit into. */
    basic_single_collection_database(volume_t& volume, io_channel_t& writing_channel, io_channel_t& merging_channel,
                                     single_collection_database_config_t const& config,
                                     resources_shared_ptr<atomic_reference_, waiter_type_> resources,
                                     compactor_ref_t compactor, collection_t&& mounted,
                                     commit_ledger<atomic_reference_, waiter_type_>& ledger,
                                     std::unique_ptr<commit_ledger<atomic_reference_, waiter_type_>> owned) noexcept
        : volume_(volume), writing_channel_(writing_channel), merging_channel_(merging_channel), config_(config),
          owned_ledger_(std::move(owned)), ledger_(ledger), collection_(std::move(mounted)), compactor_(compactor),
          registry_(*this, std::move(resources)) {}

    /** Mounts the collection and the database around it, drawing stamps from @p ledger. */
    static expected<database_unique_ptr_t> open_(
        volume_t& volume, io_channel_t& writing_channel, io_channel_t& merging_channel,
        single_collection_database_config_t const& config,
        resources_shared_ptr<atomic_reference_, waiter_type_> resources, compactor_ref_t compactor,
        commit_ledger<atomic_reference_, waiter_type_>& ledger,
        std::unique_ptr<commit_ledger<atomic_reference_, waiter_type_>> owned) noexcept {
        if (&writing_channel == &merging_channel)
            return status_t {status_code_t::invalid_argument_k, "A compaction reads and writes on two channels"};
        auto [mounted, mount_collection_status] = collection_t::mount(volume, config.collection,
                                                                      resources->memory_constraint, ledger);
        if (mount_collection_status.failed()) return mount_collection_status;

        database_unique_ptr_t opened {new (std::nothrow) basic_single_collection_database {
            volume, writing_channel, merging_channel, config, std::move(resources), compactor, std::move(mounted),
            ledger, std::move(owned)}};
        if (!opened) return status_t {status_code_t::out_of_memory_k, "The database did not fit"};
        if (status_t const restored = opened->mount_(); restored.failed()) {
            // The schema the mount read stays as it lies, never saved again without the runs it failed to take back.
            opened->generation_ = 0;
            return restored;
        }
        return opened;
    }

    /**
     *  @brief Takes back the runs the newer schema copy names, resumes the ledger above its durable stamp, drops every
     *      version above that, and only then writes the schema a fresh volume lacks.
     *
     *  The ledger resumes before the truncation, which is what lets the truncation's schema promise its stamp.
     */
    status_t mount_() noexcept {
        claim_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        if (status_t const agreed = agree_on_schema_(); agreed.failed()) return agreed;
        if (status_t const resumed = ledger_.resume(loaded_durable_); resumed.failed()) return resumed;
        lsm_tree_publication_hook_t published;
        published.notify = &record_schema_;
        published.context = this;
        if (status_t const truncated = collection_.truncate_above(writing_channel_, merging_channel_, loaded_durable_,
                                                                  published);
            truncated.failed())
            return truncated;
        if (generation_ != 0) return {};
        return save_schema_();
    }

#pragma endregion Private Construction

#pragma region Claim

    /** Binds both channels to the thread that took the claim, which every write through them needs. */
    void adopt_channels_() noexcept {
        channel_adoption_ = volume_.disk().bind_queue_pair(writing_channel_.queue_pair());
        if (channel_adoption_.succeeded())
            channel_adoption_ = volume_.disk().bind_queue_pair(merging_channel_.queue_pair());
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
     *      parking rather than spinning while another writer holds the claim.
     *  @return Success once the commit fits, or why a flush could not make room.
     */
    status_t make_room_(sealed_commit<stored_key_t, atomic_reference_> const& sealed) noexcept {
        if (!sealed.run) return {};
        while (collection_.memory_pressure(sealed) == memory_pressure_t::stalled_k) {
            claim_guard_t const held = claim_guard_t::taken_if_free(*this);
            if (!held.holding()) {
                // Whoever holds the claim may be flushing already, so the pressure is read again after.
                claim_.wait_until_released();
                continue;
            }
            if (held.adopted().failed()) return held.adopted();
            if (status_t const flushed = flush_and_compact_(); flushed.failed()) return flushed;
        }
        return {};
    }

    /** Flushes the memory runs, saves the schema naming the run, and compacts the tree for as long as the flush left
     *  a level over its bounds, holding the claim. */
    status_t flush_and_compact_() noexcept {
        if (status_t const flushed = collection_.flush(writing_channel_); flushed.failed()) return flushed;
        if (status_t const saved = save_schema_(); saved.failed()) return saved;
        lsm_tree_publication_hook_t published;
        published.notify = &record_schema_;
        published.context = this;
        while (collection_.needs_compaction()) {
            auto [outcome, compact_status] = collection_.compact(writing_channel_, merging_channel_, compactor_,
                                                                 published);
            if (compact_status.failed()) return compact_status;
            if (outcome == compaction_outcome_t::nothing_to_compact_k) break;
        }
        return {};
    }

    /** Merges the memory runs by tier, and flushes and compacts once they hold their bounds, holding the claim with
     *  both channels adopted. */
    status_t maintain_due_() noexcept {
        if (channel_adoption_.failed()) return channel_adoption_;
        if (status_t const merged = collection_.merge_memory_runs(); merged.failed()) return merged;
        if (!collection_.flush_due()) return {};
        return flush_and_compact_();
    }

    /**
     *  @brief Hands the claim back, then looks once more at the memory runs and, if they are due and the claim is
     *      still free, takes it back to maintain them, so a commit that found the claim held left its maintenance to
     *      this writer.
     *
     *  A writer that finds the claim taken again stops, since the new holder looks once more after it lets go,
     *  and a failed adoption or maintenance is met again by the next commit.
     */
    void release_claim_() noexcept {
        claim_.release();
        if (!collection_.maintenance_due() || !claim_.take_if_free()) return;
        adopt_channels_();
        [[maybe_unused]] status_t const maintained = maintain_due_();
        claim_.release();
    }

#pragma endregion Claim

#pragma region Private Methods

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
     *  @brief Reads the newer copy of the schema blob and takes its runs back, leaving a volume that holds no copy as
     *      it is, holding the claim.
     *
     *  A copy is sealed only once durable, so whichever copies a crash left are whole, and the newer one wins.
     */
    status_t agree_on_schema_() noexcept {
        single_collection_database_schema_record_t const wanted = wanted_schema_();

        std::array<st::basic_vector<byte_t>, 2> images;
        std::array<single_collection_database_schema_record_t, 2> records {};
        for (std::size_t copy = 0; copy != 2; ++copy) {
            if (status_t const read = read_schema_copy_(copy, images[copy]); read.failed()) return read;
            if (!images[copy].empty()) std::memcpy(&records[copy], images[copy].data(), sizeof(records[copy]));
        }
        std::size_t const newer = records[1].stamps.generation > records[0].stamps.generation ? 1 : 0;
        if (images[newer].empty()) return {};
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
        return collection_.restore(writing_channel_, std::span<lsm_tree_run_record_t const> {runs},
                                   std::span<lsm_tree_run_stamp_t const> {});
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
    status_t write_schema_(std::span<lsm_tree_run_record_t const> runs, transaction_stamp_t durable) noexcept {
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
        if (status_t const written = write_blob_(schema_blob_name_(schema_copy_of(record.stamps.generation)),
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
        basic_write_pipeline<disk_t, waiter_type_> writer {volume_.disk(), writing_channel_, volume_.medium()};
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
            // The offers sit queued until the channel submits them, and a poll_completions waits only on what it did.
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
    std::unique_ptr<commit_ledger<atomic_reference_, waiter_type_>> owned_ledger_;

    /** The stamps every commit of this database draws, which @c owned_ledger_ holds. */
    commit_ledger<atomic_reference_, waiter_type_>& ledger_;

    /** The collection every write lands in, which pins the ledger and so dies before it. */
    collection_t collection_;

    /** How many times the schema blob has been saved, which names the copy the next save replaces. */
    std::uint64_t generation_ = 0;

    /** The durable stamp of the schema copy the open read, above which a mount drops every version. */
    transaction_stamp_t loaded_durable_ = unwritten_stamp_k;

    /** What every merge runs on, picked at open: the calling thread, or a device compactor the opener owns. */
    compactor_ref_t compactor_;

    /** The order every transaction is placed in, and the last commit to write each key. */
    registry_t registry_;

    /** Held by whichever writer flushes, compacts or saves a blob, a commit paying for its maintenance included. */
    maintenance_claim<atomic_reference_, waiter_type_> claim_;

    /** Whether the writer holding the claim took both channels over, written only under it. */
    status_t channel_adoption_;

#pragma endregion Private Members
};

#pragma endregion Database

#pragma endregion Single Collection Database

} // namespace unum::ustore::blobs
