/**
 *  @file src/nvme/single_collection_database_transaction.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief One optimistic transaction over one single-collection database, staged and then committed.
 *
 *  Writes go to a write buffer of the transaction's own and reach the collection only at the commit,
 *  so a rollback costs nothing and a failed stage leaves the changes exactly where a caller can still
 *  log them. Reads answer from that buffer first and from the database second, and every read of the
 *  database is watched, so a commit that landed between the read and this transaction's commit is what
 *  refuses it rather than what overwrites it.
 *
 *  Every watch is scoped from a stamp the registry publishes only once a commit's writes have landed,
 *  never from the counter a commit claims when it begins. A watch scoped from the claim would be
 *  excused by a commit still in flight, which is exactly the lost update this layer exists to refuse.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstring> // `std::memcpy`

#include <span>    // `std::span`
#include <utility> // `std::forward`, `std::move`

#include <smashtable/basic_hash_table.hpp> // `st::assume_reserved_t`
#include <smashtable/basic_vector.hpp>     // `st::basic_vector`

#include <ustore/status.hpp> // `status_t`, `status_code_t`
#include <ustore/types.hpp>  // `byte_t`

#include "nvme/collection_snapshot.hpp" // `select_result_t`
#include "nvme/io_lane.hpp"             // `io_lane_t`
#include "nvme/read_set.hpp"            // `transaction_stamp_t`, `watch_schedule_t`, `watch_purpose_t`
#include "nvme/single_collection_database_transaction_registry.hpp" // `single_collection_database_transaction_registry`
#include "nvme/stored_value.hpp"                                    // `stored_value_size_t`, `tombstone_offset_k`

namespace unum::ustore::nvme {

#pragma region Transaction

/**
 *  @brief An indivisible group of reads and writes over one single-collection database.
 *
 *  Reusable after a @c commit and after a @c rollback, both of which start it again from the moment
 *  they ran. Nothing throws: every modifying call answers a status, and a caller reading
 *  @c status_code_t::write_conflict_k rolls back and does the work again.
 */
template <typename stored_key_, typename store_type_>
class single_collection_database_transaction {
  public:

#pragma region Types

    /** The key the database is addressed by. */
    using stored_key_t = stored_key_;

    /** The database every commit of this transaction lands in. */
    using store_t = store_type_;

    /** The registry that orders this transaction against every other one of the database. */
    using registry_t = single_collection_database_transaction_registry<stored_key_, store_type_>;

    /** The uncommitted changes and the watched keys of this transaction. */
    using transaction_state_t = single_collection_database_transaction_state<stored_key_t>;

    /** The write buffer the uncommitted changes live in. */
    using write_buffer_t = typename transaction_state_t::write_buffer_t;

#pragma endregion Types

#pragma region Construction

    single_collection_database_transaction(single_collection_database_transaction const&) = delete;
    single_collection_database_transaction& operator=(single_collection_database_transaction const&) = delete;
    single_collection_database_transaction(single_collection_database_transaction&&) = delete;
    single_collection_database_transaction& operator=(single_collection_database_transaction&&) = delete;

    /**
     *  @brief Enrols a transaction of @p id, which starts running the moment it is built.
     *  @param[in] id The identifier the registry holds a roster slot under, unique among live transactions.
     *  @param[inout] manager The registry this transaction stages and commits through, outliving it.
     *  @param[in] changes The write buffer the uncommitted writes land in, moved in and owned from here on.
     *  @param[in] resources The budget the watched keys are charged to.
     */
    single_collection_database_transaction(transaction_id_t id, registry_t& manager, write_buffer_t&& changes,
                                           resources_shared_ptr_t resources) noexcept
        : manager_(manager), data_(std::move(changes), std::move(resources)), id_(id) {
        start_stamp_ = manager_.request_start_(id_);
        state_ = state_t::started_k;
    }

    /** Retires the roster slot, handing back the staging lease when this transaction still holds it. */
    ~single_collection_database_transaction() noexcept { manager_.notify_about_deletion_(id_); }

#pragma endregion Construction

#pragma region Two Phase Commit

    /**
     *  @brief Checks the watched keys and takes the write path, leaving the commit itself for later.
     *
     *  A failure keeps the changes exactly as they were, so a caller can log what it could not commit,
     *  and hands the staging lease back, so the transaction is running again rather than staged.
     */
    status_t stage() noexcept {
        if (state_ != state_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is not running"};
        transaction_stamp_t staged_stamp = invalid_transaction_stamp_k;
        if (status_t const staged = manager_.stage_(id_, data_, staged_stamp); staged.failed()) return staged;
        last_stage_stamp_ = staged_stamp;
        state_ = state_t::staged_k;
        return {};
    }

    /**
     *  @brief Stages whatever is not staged and lands every change, starting the transaction again.
     *
     *  The registry hands the staging lease back whatever it answers, so this transaction is running
     *  again either way; a failure keeps the changes for logging and a @c rollback drops them.
     */
    status_t commit() noexcept {
        if (state_ == state_t::invalid_k) return {status_code_t::invalid_argument_k, "That transaction is not running"};
        status_t const committed = manager_.commit_(id_, data_, last_try_to_commit_stamp_);
        state_ = state_t::started_k;
        if (committed.failed()) return committed;

        last_stage_stamp_ = last_try_to_commit_stamp_;
        last_commit_stamp_ = last_try_to_commit_stamp_;
        data_.clear();
        start_stamp_ = manager_.request_start_(id_);
        return {};
    }

    /** Drops every change and every watch, and starts the transaction again from this moment. */
    void rollback() noexcept {
        if (state_ == state_t::staged_k) manager_.discard_(id_);
        data_.clear();
        start_stamp_ = manager_.request_start_(id_);
        state_ = state_t::started_k;
    }

    /**
     *  @brief Starts the transaction again from this moment, keeping its changes.
     *
     *  Legal only for a transaction that has read nothing, since a read taken before the new start
     *  would be checked against a stamp it never ran at.
     */
    status_t renew() noexcept {
        if (state_ != state_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        if (data_.read_set.count(watch_purpose_t::read_k) != 0)
            return {status_code_t::invalid_argument_k, "That transaction has read something"};
        manager_.invalidate_(id_);
        start_stamp_ = manager_.request_start_(id_);
        return {};
    }

#pragma endregion Two Phase Commit

#pragma region Writes

    /** Whether @p count more values of @p bytes together still fit in this transaction's write buffer. */
    [[nodiscard]] bool has_available_space_for(std::size_t count, std::size_t bytes) const noexcept {
        return state_ == state_t::started_k && data_.changes.can_fit(count, bytes);
    }

    /** Writes @p stored_value under @p stored_key, which reaches the database only at the commit. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t insert(stored_key_t stored_key, std::span<byte_t const> stored_value) noexcept {
        if (state_ != state_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        if (!manager_.store_->admits(stored_value.size()))
            return {status_code_t::malformed_k, "This database admits no value of that size"};

        std::span<stored_key_t const> const only {&stored_key, 1};
        if (status_t const prepared = watch_now_or_prepare_<watch_schedule_>(only); prepared.failed()) return prepared;
        transaction_stamp_t const watch_from = watch_stamp_for_<watch_schedule_>();
        if (status_t const staged = data_.changes.add(stored_key, stored_value); staged.failed()) return staged;
        watch_later_<watch_schedule_>(only, watch_from, watch_purpose_t::write_k);
        return {};
    }

    /** Erases @p stored_key, which reaches the database as a tombstone only at the commit. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t erase(stored_key_t stored_key) noexcept {
        if (state_ != state_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};

        std::span<stored_key_t const> const only {&stored_key, 1};
        if (status_t const prepared = watch_now_or_prepare_<watch_schedule_>(only); prepared.failed()) return prepared;
        transaction_stamp_t const watch_from = watch_stamp_for_<watch_schedule_>();
        if (status_t const staged = data_.changes.mark_as_removed(stored_key); staged.failed()) return staged;
        watch_later_<watch_schedule_>(only, watch_from, watch_purpose_t::write_k);
        return {};
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
        select_result_t const carried = manager_.store_->find(stored_key, size);
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
     *  @param[inout] lane The lane a read of the database is submitted and reaped on, held by the caller.
     *  @param[in] stored_key The key to read.
     *  @param[out] into Where the value lands, which need not be aligned and need not be registered.
     *  @param[out] size Bytes written, which is zero for every failure.
     */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t select(io_lane_t& lane, stored_key_t stored_key, std::span<byte_t> into,
                    stored_value_size_t& size) const noexcept {
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
        status_t const read = manager_.store_->select(lane, stored_key, into, size);
        watch_later_<watch_schedule_>(only, watch_from, watch_purpose_t::read_k);
        return read;
    }

    /**
     *  @brief Reads the values of @p stored_keys, this transaction's own writes ahead of the database.
     *  @param[inout] lane The lane a read of the database is submitted and reaped on, held by the caller.
     *  @param[in] stored_keys The keys to read, in any order and with any number of misses among them.
     *  @param[out] results What every key answered, which separates a miss from an erasure.
     *  @param[in] sink Called as @c sink(request, offset, bytes) for every key that locates a value.
     *
     *  @note A value this transaction wrote arrives as one call carrying all of it, while one the
     *      database answers arrives as one call per block, exactly as a read of the database does.
     */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k, typename sink_type_>
    status_t select(io_lane_t& lane, std::span<stored_key_t const> stored_keys, std::span<select_result_t> results,
                    sink_type_&& sink) const noexcept {
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
        status_t const read = manager_.store_->select(lane, watched, std::span<select_result_t> {deferred_results},
                                                      relay);
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

        /** Running: it reads, it writes, and it holds no lease. */
        started_k,

        /** Staged: it holds the staging lease and the write path until it commits or rolls back. */
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
            if (place.offset == tombstone_offset_k) {
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
                data_.read_set.add(stored_key, watch_stamp, purpose, st::assume_reserved_t {});
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The registry this transaction stages and commits through, borrowed and outliving it. */
    registry_t& manager_;

    /** The uncommitted changes and the watched keys, which a read of this transaction also walks. */
    transaction_state_t mutable data_;

    /** The roster slot this transaction holds, released when it dies. */
    transaction_id_t id_ = invalid_transaction_id_k;

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

} // namespace unum::ustore::nvme
