/**
 *  @file src/nvme/database_transaction.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief One transaction over the collections of a database: its writes, its watches and its stamps.
 *
 *  Everything a transaction owns is its own thread's: the write buffer it fills, the keys it watches
 *  and the four stamps it reports. What it shares lives in the registry beside it, so nothing here
 *  takes a lock and nothing here reads another transaction's state.
 *
 *  A watch is scoped from a stamp, never from a clock. @c watch_schedule_t::on_commit_k scopes from
 *  the transaction's own start, and @c on_commit_since_operation_k from the watermark that stands
 *  when the operation runs, which is the last commit whose writes have landed rather than the last
 *  stamp claimed.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`

#include <array>   // `std::array`
#include <span>    // `std::span`
#include <utility> // `std::move`
#include <variant> // `std::get_if`

#include <ustore/status.hpp> // `status_t`, `status_code_t`, `expected`
#include <ustore/types.hpp>  // `byte_t`

#include "nvme/database_transaction_registry.hpp" // `database_transaction_registry`
#include "nvme/database_transaction_state.hpp"    // `database_transaction_state`
#include "nvme/read_set.hpp"                      // `transaction_stamp_t`, `watch_schedule_t`, `watch_purpose_t`
#include "nvme/stored_value.hpp"                  // `stored_value_size_t`, `stored_value_view_t`
#include "nvme/types.hpp"                         // `collection_id_t`, `transaction_id_t`

namespace unum::ustore::nvme {

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
template <typename stored_key_, typename store_type_>
class database_transaction {
  public:

#pragma region Types

    /** The key every collection of the store is addressed by. */
    using stored_key_t = stored_key_;

    /** The store every committed write of this transaction lands in. */
    using store_t = store_type_;

    /** The registry that orders this transaction against every other of the same store. */
    using registry_t = database_transaction_registry<stored_key_t, store_t>;

    /** The uncommitted writes and the watched keys this transaction carries. */
    using state_t = database_transaction_state<stored_key_t>;

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
     *  @brief A transaction of @p id, started at the stamp @p manager hands it.
     *  @param[in] id The identifier the registry enrolled, which this transaction holds until it dies.
     *  @param[inout] manager The registry that orders and conflict-checks it, outliving it.
     *  @param[in] changes The write buffer it fills, already sized to its configuration.
     *  @param[in] resources The budget its watched keys are charged to.
     */
    database_transaction(transaction_id_t id, registry_t& manager, write_buffer_t&& changes,
                         resources_shared_ptr_t resources) noexcept
        : manager_(manager), data_(std::move(changes), std::move(resources)), id_(id),
          start_stamp_(manager.request_start_(id)), phase_(phase_t::started_k) {}

    ~database_transaction() noexcept { manager_.notify_about_deletion_(id_); }

#pragma endregion Construction

#pragma region Lifecycle

    /**
     *  @brief Checks every watched key and takes the store's write path, without applying anything.
     *
     *  A failure keeps the changes, so a caller can still log what it could not stage, and a success
     *  holds the write path until @c commit or @c rollback hands it back.
     */
    status_t stage() noexcept {
        if (phase_ != phase_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        transaction_stamp_t staged_stamp = invalid_transaction_stamp_k;
        status_t const staged = manager_.stage_(id_, data_, staged_stamp);
        if (staged.succeeded()) {
            last_stage_stamp_ = staged_stamp;
            phase_ = phase_t::staged_k;
        }
        return staged;
    }

    /**
     *  @brief Stages what is not staged yet and applies every write of this transaction.
     *
     *  @note The write path is handed back whichever way this ends, so the transaction is started
     *      again on return; only a success clears its changes and its watches.
     */
    status_t commit() noexcept {
        status_t const committed = manager_.commit_(id_, data_, last_try_to_commit_stamp_);
        phase_ = phase_t::started_k;
        if (committed.failed()) return committed;

        last_stage_stamp_ = last_try_to_commit_stamp_;
        last_commit_stamp_ = last_try_to_commit_stamp_;
        data_.clear();
        start_stamp_ = manager_.request_start_(id_);
        return {};
    }

    /** Drops every write and every watch, and starts the transaction over from this moment. */
    void rollback() noexcept {
        if (phase_ == phase_t::staged_k) manager_.discard_(id_);
        phase_ = phase_t::started_k;
        data_.clear();
        start_stamp_ = manager_.request_start_(id_);
    }

    /**
     *  @brief Starts over from this moment while keeping the writes, which only a write-only transaction may.
     *  @return @c invalid_argument_k when the transaction has read anything, whose watches cannot move.
     */
    status_t renew() noexcept {
        if (phase_ != phase_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        if (data_.read_set.count(watch_purpose_t::read_k) != 0)
            return {status_code_t::invalid_argument_k, "That transaction has read something"};
        manager_.invalidate_(id_);
        start_stamp_ = manager_.request_start_(id_);
        return {};
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

#pragma endregion Lifecycle

#pragma region Catalog

    /** Whether the store names the collection @p id. */
    [[nodiscard]] bool has_collection(any_collection_id_t const& id) const noexcept {
        if (!idle_()) return false;
        return manager_.has_collection_(id);
    }

    /** The ID @p id resolves to, or @c collection_invalid_id_k when it names nothing. */
    [[nodiscard]] collection_id_t collection_id(any_collection_id_t const& id) const noexcept {
        if (!idle_()) return collection_invalid_id_k;
        return resolve_(id).id;
    }

    /** The name and the number of every collection of the store. */
    [[nodiscard]] st::basic_vector<collection_ids_t> list_collections() const noexcept {
        if (!idle_()) return {};
        return manager_.list_collections_();
    }

    /** Bytes the largest value @p id admits takes, which is what a caller sizes a read buffer by. */
    [[nodiscard]] stored_value_size_t buffer_size_for_any_stored_value(any_collection_id_t const& id) const noexcept {
        if (!idle_()) return 0;
        return manager_.buffer_size_for_any_stored_value_(id);
    }

    /** Whether @p count more writes of @p bytes still fit in this transaction's own buffer. */
    [[nodiscard]] bool has_available_space_for(std::size_t count, std::size_t bytes) const noexcept {
        return data_.changes.can_fit(count, bytes);
    }

#pragma endregion Catalog

#pragma region Writes

    /** Writes @p stored_value under @p stored_key of @p id into this transaction's own buffer. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t insert(any_collection_id_t const& id, stored_key_t stored_key, stored_value_view_t stored_value) noexcept {
        if (phase_ != phase_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        resolved_collection_t const named = resolve_(id);
        collection_id_t const collection_id = named.id;
        if (collection_id == collection_invalid_id_k)
            return {status_code_t::not_found_k, "The store names no such collection"};

        std::array<stored_key_t, 1> const alone {stored_key};
        std::span<stored_key_t const> const watched {alone};
        if (status_t const prepared = watch_now_or_prepare_for_later_<watch_schedule_>(collection_id, watched);
            prepared.failed())
            return prepared;

        transaction_stamp_t const from = watch_stamp_for_<watch_schedule_>();
        status_t const written = data_.changes.add(collection_id, group_config_of_(named.config), stored_key,
                                                   stored_value);
        if (written.succeeded()) watch_later_<watch_schedule_>(collection_id, watched, from, watch_purpose_t::write_k);
        return written;
    }

    /** Writes a removal of @p stored_key of @p id into this transaction's own buffer. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t remove(any_collection_id_t const& id, stored_key_t stored_key) noexcept {
        if (phase_ != phase_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        resolved_collection_t const named = resolve_(id);
        collection_id_t const collection_id = named.id;
        if (collection_id == collection_invalid_id_k)
            return {status_code_t::not_found_k, "The store names no such collection"};

        std::array<stored_key_t, 1> const alone {stored_key};
        std::span<stored_key_t const> const watched {alone};
        if (status_t const prepared = watch_now_or_prepare_for_later_<watch_schedule_>(collection_id, watched);
            prepared.failed())
            return prepared;

        transaction_stamp_t const from = watch_stamp_for_<watch_schedule_>();
        status_t const written = data_.changes.mark_as_removed(collection_id, group_config_of_(named.config),
                                                               stored_key);
        if (written.succeeded()) watch_later_<watch_schedule_>(collection_id, watched, from, watch_purpose_t::write_k);
        return written;
    }

#pragma endregion Writes

#pragma region Lookups

    /** Whether this transaction or the store carries @p stored_key of @p id. */
    [[nodiscard]] bool contains(any_collection_id_t const& id, stored_key_t stored_key) const noexcept {
        return contains_uncommitted(id, stored_key) || contains_committed(id, stored_key);
    }

    /** Bytes the value of @p stored_key takes, as this transaction would see it. */
    status_t stored_value_size(any_collection_id_t const& id, stored_key_t stored_key,
                               stored_value_size_t& size) const noexcept {
        status_t const staged = stored_value_size_uncommitted(id, stored_key, size);
        if (staged.succeeded() || !(staged == status_code_t::not_found_k)) return staged;
        return stored_value_size_committed(id, stored_key, size);
    }

    /** Copies the value of @p stored_key into @p into, taking this transaction's own write first. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t select(any_collection_id_t const& id, stored_key_t stored_key, stored_value_span_t into,
                    stored_value_size_t& size) const noexcept {
        status_t const staged = select_uncommitted(id, stored_key, into, size);
        if (staged.succeeded()) return staged;
        return select_committed<watch_schedule_>(id, stored_key, into, size);
    }

    /** Whether this transaction's own buffer carries @p stored_key of @p id. */
    [[nodiscard]] bool contains_uncommitted(any_collection_id_t const& id, stored_key_t stored_key) const noexcept {
        if (!idle_()) return false;
        collection_id_t const collection_id = resolve_(id).id;
        if (collection_id == collection_invalid_id_k) return false;
        return data_.changes.status(collection_id, stored_key) == write_buffer_status_t::found_k;
    }

    /** Bytes the value this transaction wrote for @p stored_key takes. */
    status_t stored_value_size_uncommitted(any_collection_id_t const& id, stored_key_t stored_key,
                                           stored_value_size_t& size) const noexcept {
        if (!idle_()) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        collection_id_t const collection_id = resolve_(id).id;
        if (collection_id == collection_invalid_id_k)
            return {status_code_t::not_found_k, "The store names no such collection"};
        return data_.changes.stored_value_size(collection_id, stored_key, size);
    }

    /** Copies the value this transaction wrote for @p stored_key into @p into. */
    status_t select_uncommitted(any_collection_id_t const& id, stored_key_t stored_key, stored_value_span_t into,
                                stored_value_size_t& size) const noexcept {
        if (!idle_()) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        collection_id_t const collection_id = resolve_(id).id;
        if (collection_id == collection_invalid_id_k)
            return {status_code_t::not_found_k, "The store names no such collection"};
        return data_.changes.select(collection_id, stored_key, into, size);
    }

    /** Whether the store carries @p stored_key of @p id, ignoring what this transaction wrote. */
    [[nodiscard]] bool contains_committed(any_collection_id_t const& id, stored_key_t stored_key) const noexcept {
        if (!idle_()) return false;
        return manager_.contains_(id, stored_key);
    }

    /** Bytes the store holds for @p stored_key of @p id, ignoring what this transaction wrote. */
    status_t stored_value_size_committed(any_collection_id_t const& id, stored_key_t stored_key,
                                         stored_value_size_t& size) const noexcept {
        if (!idle_()) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        return manager_.stored_value_size_(id, stored_key, size);
    }

    /**
     *  @brief Reads @p stored_key of @p id out of the store and watches it for conflicts.
     *  @param[in] id The collection to read from, named by its ID or by its name.
     *  @param[in] stored_key The key to read.
     *  @param[out] into Where the value lands, which need not be aligned and need not be registered.
     *  @param[out] size Bytes written, which is zero for every failure.
     *
     *  @note The read runs before the watch is taken, so a key the store refuses is never watched.
     */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t select_committed(any_collection_id_t const& id, stored_key_t stored_key, stored_value_span_t into,
                              stored_value_size_t& size) const noexcept {
        if (phase_ != phase_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        collection_id_t const collection_id = resolve_(id).id;
        if (collection_id == collection_invalid_id_k)
            return {status_code_t::not_found_k, "The store names no such collection"};

        transaction_stamp_t const from = watch_stamp_for_<watch_schedule_>();
        if (status_t const read = manager_.select_(collection_id, stored_key, into, size); read.failed()) return read;

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

    /** Whether this transaction is filling its buffer or holding the store's write path. */
    enum class phase_t : std::uint8_t {

        /** The transaction takes reads and writes, and holds nothing of the store. */
        started_k,

        /** The transaction has checked its watches and holds the store's write path. */
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

    /** Whether this transaction is filling its buffer rather than holding the store's write path. */
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
    [[nodiscard]] resolved_collection_t resolve_(any_collection_id_t const& id) const noexcept {
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

    /** Checks @p stored_keys right away, or makes the room a later watch of them needs. */
    template <watch_schedule_t watch_schedule_>
    status_t watch_now_or_prepare_for_later_(collection_id_t collection_id,
                                             std::span<stored_key_t const> stored_keys) const noexcept {
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
                      transaction_stamp_t from, watch_purpose_t purpose) const noexcept {
        if constexpr (watch_schedule_ == watch_schedule_t::on_commit_k ||
                      watch_schedule_ == watch_schedule_t::on_commit_since_operation_k)
            for (stored_key_t const& stored_key : stored_keys)
                data_.read_set.add(collection_id, stored_key, from, purpose, st::assume_reserved_t {});
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The registry that orders this transaction and checks its watches, outliving it. */
    registry_t& manager_;

    /** The writes and the watches this transaction carries, which a read of it also fills. */
    state_t mutable data_;

    /** The collection the last lookup answered, which the next lookup of the same one reuses. */
    resolved_collection_t mutable resolved_;

    /** The identifier the registry enrolled this transaction under. */
    transaction_id_t id_ = invalid_transaction_id_k;

    /** The stamp this transaction was started or restarted at. */
    transaction_stamp_t start_stamp_ = invalid_transaction_stamp_k;

    /** The stamp this transaction last staged at. */
    transaction_stamp_t last_stage_stamp_ = invalid_transaction_stamp_k;

    /** The stamp this transaction last committed at. */
    transaction_stamp_t last_commit_stamp_ = invalid_transaction_stamp_k;

    /** The stamp this transaction last tried to commit at. */
    transaction_stamp_t last_try_to_commit_stamp_ = invalid_transaction_stamp_k;

    /** Whether this transaction is filling its buffer or holding the store's write path. */
    phase_t phase_ = phase_t::started_k;

#pragma endregion Private Members
};

#pragma endregion Transaction

} // namespace unum::ustore::nvme
