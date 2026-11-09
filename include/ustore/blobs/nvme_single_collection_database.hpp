/**
 *  @file include/ustore/blobs/nvme_single_collection_database.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Single-collection databases, with their snapshots and their optimistic transactions over
 *      the registry and transaction core of @c nvme_transactions.hpp.
 *
 *  A single-collection database is the smallest thing that can be committed to: one collection,
 *  the registry that orders the commits landing in it, and a manifest on the volume naming the
 *  collection a reopen checks itself against, its runs, and whatever bytes the owner keeps beside
 *  the data. Reads take the caller's channel, so any number of threads read at once; flushes,
 *  compactions and user-data writes take the two channels the database was opened with, under its
 *  claim, which one thread holds at a time.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint16_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::memset`

#include <limits>      // `std::numeric_limits`
#include <memory>      // `std::unique_ptr`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <type_traits> // `std::remove_reference_t`
#include <utility>     // `std::declval`, `std::forward`, `std::move`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/blobs/nvme_key_slot.hpp"       // `value_layout_t`, `collection_id_t`, `is_fixed`
#include "ustore/blobs/nvme_published_runs.hpp" // `pinned_snapshot`, `maintenance_claim`, `commit_ledger`
#include "ustore/blobs/nvme_store.hpp"          // `nvme_store_cursor`, `sealed_commit`, `keyed_delta`
#include "ustore/blobs/nvme_transactions.hpp"   // `transaction_registry`, `transaction_core`, `nvme_store_concept`
#include "ustore/blobs/nvme_tree.hpp"           // `lsm_tree_run_record_t`, `lsm_tree_publication_hook_t`
#include "ustore/utilities/nvme_manifest.hpp"   // `manifest_t`, `manifest_collection_t`, `manifest_file_t`
#include "ustore/utilities/nvme_mounts.hpp"     // `resources_shared_ptr`
#include "ustore/utilities/nvme_channels.hpp"   // `io_channel_t`, `basic_write_pipeline`, `basic_read_pipeline`
#include "ustore/utilities/nvme_volume.hpp"     // `medium_extent_t`, `write_extent`, `read_extent`
#include "ustore/utilities/types.hpp"           // `status_t`, `status_code_t`, `expected`

namespace unum::ustore::blobs {

using utilities::manifest_collection_t;
using utilities::manifest_file_t;
using utilities::manifest_no_collection_k;
using utilities::manifest_t;
using utilities::medium_extent_t;

/** The ID a single-collection transaction files its deltas under, the nil UUID, as the database has one collection. */
inline constexpr collection_id_t single_collection_id_k {};

#pragma region Single Collection Database Snapshot

/**
 *  @brief One moment of a single-collection database: a stamp pinned in its ledger, which every read through the
 *      snapshot answers at.
 *  @tparam store_type_ The database read, which must outlive the snapshot.
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
        taken.pinned_ = pinned_snapshot<store_t>::of(store);
        return taken;
    }

    /** The stamp every read through this snapshot answers at, which every commit inside it is at or below. */
    [[nodiscard]] transaction_stamp_t stamp() const noexcept { return pinned_.stamp(); }

    /**
     *  @brief Whether the database carried @p stored_key at the snapshot's stamp, and how many bytes its value took.
     *  @param[in] stored_key The key to look for.
     *  @param[out] size Bytes the value takes, which is zero for anything but @c written_k.
     */
    [[nodiscard]] lookup_result_t find(stored_key_t stored_key, stored_value_size_t& size) const noexcept {
        return pinned_.store().find(stored_key, size, pinned_.stamp());
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
        return pinned_.store().select(channel, stored_key, into, size, pinned_.stamp());
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
        return pinned_.store().select(channel, stored_keys, results, std::forward<sink_type_>(sink), pinned_.stamp());
    }

  private:

    /** The database read and the stamp pinned in its ledger. */
    pinned_snapshot<store_t> pinned_;
};

#pragma endregion Single Collection Database Snapshot

#pragma region Single Collection Database Transaction

/**
 *  @brief An indivisible group of reads and writes over one single-collection database.
 *
 *  The core carries the writes, the watches, the stamps and the two-phase lifecycle; what is here
 *  reads through the one collection on the caller's channel, its own writes ahead of the database.
 *  Nothing throws: every modifying call answers a status, and a
 *  caller reading @c status_code_t::write_conflict_k rolls back and
 *  does the work again.
 */
template <typename stored_key_type_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiter_type_>
class single_collection_database_transaction {
  public:

#pragma region Types

    /** The key the database is addressed by. */
    using stored_key_t = stored_key_type_;

    /** The database every commit of this transaction lands in. */
    using store_t = store_type_;

    /** The registry that orders this transaction against every other one of the database. */
    using registry_t = transaction_registry<stored_key_t, store_t, atomic_reference_, waiter_type_>;

    /** The writes, the watches, the stamps and the lifecycle this transaction shares with
     *  every other kind. */
    using core_t = transaction_core<stored_key_t, registry_t, atomic_reference_, waiter_type_>;

    /** The uncommitted changes and the watched keys of this transaction. */
    using transaction_state_t = typename core_t::state_t;

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
        : core_(manager, std::move(deltas), std::move(resources)) {}

#pragma endregion Construction

#pragma region Two Phase Commit

    /** Seals the changes, takes the ledger's leader word and lays out the view the
     *  commit publishes, as @c transaction_core::stage does. */
    status_t stage() noexcept { return core_.stage(); }

    /** Lands every change and starts the transaction again, as @c transaction_core::commit does. */
    status_t commit() noexcept { return core_.commit(); }

    /** Drops every change and every watch, and starts the transaction again from this moment. */
    void rollback() noexcept { core_.rollback(); }

    /** Hands back the leader word a stage took, keeping every change and every watch. */
    void unstage() noexcept { core_.unstage(); }

    /** Starts the transaction again from this moment, keeping its changes, which only one that
     *  read nothing may. */
    status_t renew() noexcept { return core_.renew(); }

#pragma endregion Two Phase Commit

#pragma region Writes

    /** Whether @p count more values of @p bytes together still fit in this transaction's deltas. */
    [[nodiscard]] bool has_available_space_for(std::size_t count, std::size_t bytes) const noexcept {
        return core_.started() && core_.state().deltas.can_fit(count, bytes);
    }

    /** Writes @p stored_value under @p stored_key, which reaches the database only at the commit, watching the key
     *  for another commit writing it as @p watch_schedule_ says. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t insert(stored_key_t stored_key, std::span<byte_t const> stored_value) noexcept {
        if (!core_.started()) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        if (!core_.store().admits(stored_value.size()))
            return {status_code_t::malformed_k, "This database admits no value of that size"};
        if (status_t const checked = core_.template check_write_now<watch_schedule_>(single_collection_id_k,
                                                                                     stored_key);
            checked.failed())
            return checked;
        return core_.state().deltas.write(single_collection_id_k, core_.store().value_widths(), stored_key,
                                          stored_value, core_.template watch_stamp_for<watch_schedule_>());
    }

    /** Removes @p stored_key, which reaches the database as a remove delta only at the commit, watched as @c insert
     *  is. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t remove(stored_key_t stored_key) noexcept {
        if (!core_.started()) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        if (status_t const checked = core_.template check_write_now<watch_schedule_>(single_collection_id_k,
                                                                                     stored_key);
            checked.failed())
            return checked;
        return core_.state().deltas.remove(single_collection_id_k, core_.store().value_widths(), stored_key,
                                           core_.template watch_stamp_for<watch_schedule_>());
    }

    /** Watches @p stored_keys for conflicts until the next commit or rollback, without reading them. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t watch_more(std::span<stored_key_t const> stored_keys) noexcept {
        return core_.template watch_more<watch_schedule_>(single_collection_id_k, stored_keys);
    }

#pragma endregion Writes

#pragma region Lookups

    /**
     *  @brief Whether anything carries @p stored_key, watching the database when it is what answered.
     *  @param[in] stored_key The key to look for.
     *  @param[out] size Bytes the value takes, which is zero for anything but @c written_k.
     */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    [[nodiscard]] lookup_result_t find(stored_key_t stored_key, stored_value_size_t& size) noexcept {
        size = 0;
        if (!core_.started()) return lookup_result_t::missing_k;
        lookup_result_t const own_lookup = find_uncommitted(stored_key, size);
        if (own_lookup != lookup_result_t::missing_k) return own_lookup;

        std::span<stored_key_t const> const only {&stored_key, 1};
        if (core_.template watch_now_or_prepare<watch_schedule_>(single_collection_id_k, only).failed())
            return lookup_result_t::missing_k;
        transaction_stamp_t const at = core_.template read_stamp_for<watch_schedule_>();
        transaction_stamp_t const watch_from = core_.template watch_stamp_for<watch_schedule_>();
        lookup_result_t const carried = core_.store().find(stored_key, size, at);
        core_.template watch_later<watch_schedule_>(single_collection_id_k, only, watch_from, watch_purpose_t::read_k);
        return carried;
    }

    /** What this transaction's own deltas hold under @p stored_key, without touching the database. */
    [[nodiscard]] lookup_result_t find_uncommitted(stored_key_t stored_key, stored_value_size_t& size) const noexcept {
        size = 0;
        stored_value_size_t held = 0;
        lookup_result_t const own_lookup = visit_uncommitted_(stored_key, [&](std::span<byte_t const> bytes) noexcept {
            held = static_cast<stored_value_size_t>(bytes.size());
        });
        size = held;
        return own_lookup;
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
        if (!core_.started()) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        size = 0;
        bool refused = false;
        lookup_result_t const own_lookup = visit_uncommitted_(stored_key, [&](std::span<byte_t const> bytes) noexcept {
            if (into.size() < bytes.size()) {
                refused = true;
                return;
            }
            if (!bytes.empty()) std::memcpy(into.data(), bytes.data(), bytes.size());
            size = static_cast<stored_value_size_t>(bytes.size());
        });
        if (refused) return {status_code_t::invalid_argument_k, "A buffer shorter than the value"};
        if (own_lookup == lookup_result_t::removed_k)
            return {status_code_t::not_found_k, "This transaction removed that key"};
        if (own_lookup == lookup_result_t::written_k) return {};

        // The watch is scoped from no later than the read, so a commit racing this read is
        // refused by it.
        std::span<stored_key_t const> const only {&stored_key, 1};
        if (status_t const prepared = core_.template watch_now_or_prepare<watch_schedule_>(single_collection_id_k,
                                                                                           only);
            prepared.failed())
            return prepared;
        transaction_stamp_t const at = core_.template read_stamp_for<watch_schedule_>();
        transaction_stamp_t const watch_from = core_.template watch_stamp_for<watch_schedule_>();
        status_t const read = core_.store().select(channel, stored_key, into, size, at);
        core_.template watch_later<watch_schedule_>(single_collection_id_k, only, watch_from, watch_purpose_t::read_k);
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
                    std::span<lookup_result_t> results, sink_type_&& sink) noexcept {
        if (!core_.started()) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        if (results.size() != stored_keys.size())
            return {status_code_t::invalid_argument_k, "A batched select answers one result per key"};

        st::vector<stored_key_t> deferred_keys;
        st::vector<std::size_t> deferred_requests;
        if (deferred_keys.reserve(stored_keys.size()) != st::status_t::success_k ||
            deferred_requests.reserve(stored_keys.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A transaction could not track the keys of a batch"};

        for (std::size_t request = 0; request != stored_keys.size(); ++request) {
            lookup_result_t const own_lookup = visit_uncommitted_(
                stored_keys[request],
                [&](std::span<byte_t const> bytes) noexcept { sink(request, std::uint64_t {0}, bytes); });
            results[request] = own_lookup;
            if (own_lookup != lookup_result_t::missing_k) continue;
            [[maybe_unused]] st::status_t const named = deferred_keys.push_back(stored_key_t {stored_keys[request]});
            [[maybe_unused]] st::status_t const tracked = deferred_requests.push_back(std::size_t {request});
        }
        if (deferred_keys.empty()) return {};

        st::vector<lookup_result_t> deferred_results;
        if (deferred_results.resize(deferred_keys.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A transaction could not track the keys of a batch"};

        std::span<stored_key_t const> const watched {deferred_keys};
        if (status_t const prepared = core_.template watch_now_or_prepare<watch_schedule_>(single_collection_id_k,
                                                                                           watched);
            prepared.failed())
            return prepared;

        auto relay = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
            sink(deferred_requests[request], offset, bytes);
        };
        transaction_stamp_t const at = core_.template read_stamp_for<watch_schedule_>();
        transaction_stamp_t const watch_from = core_.template watch_stamp_for<watch_schedule_>();
        status_t const read = core_.store().select(channel, watched, std::span<lookup_result_t> {deferred_results},
                                                   relay, at);
        core_.template watch_later<watch_schedule_>(single_collection_id_k, watched, watch_from,
                                                    watch_purpose_t::read_k);
        if (read.failed()) return read;
        for (std::size_t index = 0; index != deferred_results.size(); ++index)
            results[deferred_requests[index]] = deferred_results[index];
        return {};
    }

#pragma endregion Lookups

#pragma region Stamps

    /** The stamp this transaction was created or started again at, which scopes every watch. */
    [[nodiscard]] transaction_stamp_t start_stamp() const noexcept { return core_.start_stamp(); }

    /** The stamp of the last successful stage. */
    [[nodiscard]] transaction_stamp_t last_stage_stamp() const noexcept { return core_.last_stage_stamp(); }

    /** The stamp of the last successful commit. */
    [[nodiscard]] transaction_stamp_t last_commit_stamp() const noexcept { return core_.last_commit_stamp(); }

    /** The stamp of the last commit attempt, whether it landed or not. */
    [[nodiscard]] transaction_stamp_t last_try_to_commit_stamp() const noexcept {
        return core_.last_try_to_commit_stamp();
    }

#pragma endregion Stamps

  private:

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
        if (!core_.state().deltas.delta(single_collection_id_k, stored_key, held)) return lookup_result_t::missing_k;
        if (held.kind == delta_kind_t::remove_k) return lookup_result_t::removed_k;
        visitor(held.bytes);
        return lookup_result_t::written_k;
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The writes, the watches, the stamps and the lifecycle, which the registry drives. */
    core_t core_;

#pragma endregion Private Members
};

#pragma endregion Single Collection Database Transaction

#pragma region Single Collection Database

#pragma region Manifest Collection

/** The name the one collection of a single-collection database carries in its manifest. */
inline constexpr char single_collection_name_k[] = "records";

#pragma endregion Manifest Collection

#pragma region Database

/**
 *  @brief One collection, its transactions, its snapshots and its user data, over one volume.
 *  @tparam stored_key_type_ The key the collection is addressed by.
 *  @tparam volume_type_ The volume every run and the user data live on.
 *  @tparam store_type_ The store the collection lives in.
 *
 *  Pinned rather than movable: the registry and every live transaction point back here, so a database
 *  is opened into a @c std::unique_ptr and stays where it was opened.
 */
template <typename stored_key_type_, typename volume_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, nvme_store_concept store_type_>
class basic_single_collection_database {
  public:

#pragma region Types

    /** The key the runs order their key slots by. */
    using stored_key_t = stored_key_type_;

    /** The volume every run and the user data of this database live on. */
    using volume_t = volume_type_;

    /** The collection every write of this database lands in. */
    using collection_t = store_type_;

    /** The key slot every run of this database holds. */
    using key_slot_t = typename collection_t::key_slot_t;

    /** The transport the volume moves bytes through. */
    using disk_t = typename volume_t::disk_t;

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
    using registry_t =
        transaction_registry<stored_key_t, basic_single_collection_database, atomic_reference_, waiter_type_>;

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

    /** The runs of the collection, held here as the collection's tree points into them. */
    using run_pool_t = typename collection_t::run_pool_t;

    /** Runs the collection's pool may grow to hold, the most a @c std::uint16_t index admits. */
    static constexpr std::size_t run_pool_capacity_k = std::numeric_limits<std::uint16_t>::max() - 1;

#pragma endregion Types

#pragma region Construction

    basic_single_collection_database(basic_single_collection_database const&) = delete;
    basic_single_collection_database& operator=(basic_single_collection_database const&) = delete;
    basic_single_collection_database(basic_single_collection_database&&) = delete;
    basic_single_collection_database& operator=(basic_single_collection_database&&) = delete;

    /** Writes whatever the memory runs still hold onto the volume and names every run, so the data outlives this. */
    ~basic_single_collection_database() noexcept {
        // A database that never mounted whole, or was destroyed, must not publish over its gaps.
        if (published_generation_ == 0) return;
        claim_guard_t const held {*this};
        if (adopt_channels_().failed()) return;
        [[maybe_unused]] status_t const flushed = collection_.flush(writing_channel_);
        [[maybe_unused]] status_t const published = publish_manifest_();
    }

    /**
     *  @brief Opens a database over @p volume, publishing a manifest when the volume read none.
     *  @param[in,out] volume The volume every run and the user data land on, outliving it.
     *  @param[in,out] writing_channel The channel every write, flush and commit runs on, borrowed
     *      for the database's life.
     *  @param[in,out] merging_channel The second channel a compaction writes its new runs on, which
     *      is not @p writing_channel.
     *  @param[in] config The collection's bounds and the transaction defaults.
     *  @param[in] resources The budget the conflict record, the roster and every transaction's
     *      writes are charged to.
     *  @param[in] compactor What every merge runs on: the calling thread, or a device compactor
     *      the caller owns, which outlives the database.
     *  @return The database, @c invalid_argument_k for a volume whose manifest names another value
     *      layout, or @c malformed_k for one naming more than one collection.
     */
    static expected<database_unique_ptr_t> open(volume_t& volume, io_channel_t& writing_channel,
                                                io_channel_t& merging_channel,
                                                single_collection_database_config_t const& config,
                                                resources_shared_ptr<atomic_reference_, waiter_type_> resources,
                                                compactor_ref_t compactor = {}) noexcept {
        if (!resources) return status_t {status_code_t::invalid_argument_k, "A database is charged to some budget"};
        if (&writing_channel == &merging_channel)
            return status_t {status_code_t::invalid_argument_k, "A compaction reads and writes on two channels"};
        database_unique_ptr_t opened {new (std::nothrow) basic_single_collection_database {
            volume, writing_channel, merging_channel, config, resources, compactor}};
        if (!opened) return status_t {status_code_t::out_of_memory_k, "The database did not fit"};
        // The collection's tree points into the pool and its views into the ledger, both members.
        std::size_t const capacity = volume.runs_capacity() < run_pool_capacity_k ? volume.runs_capacity()
                                                                                  : run_pool_capacity_k;
        auto [pool, make_pool_status] = run_pool_t::make(1, capacity, resources->memory_constraint);
        if (make_pool_status.failed()) return make_pool_status;
        opened->runs_ = std::move(pool);
        auto [mounted, mount_collection_status] = collection_t::mount(
            volume, config.collection, resources->memory_constraint, opened->ledger_, opened->runs_);
        if (mount_collection_status.failed()) return mount_collection_status;
        opened->collection_ = std::move(mounted);
        // A manifest the mount could not take back whole stays as it lies, never published over.
        if (status_t const restored = opened->mount_(); restored.failed()) return restored;
        return opened;
    }

    /** Publishes a manifest naming nothing, then gives back every run and the user data, which
     *  destroys the data and leaves the volume empty of it. */
    status_t destroy() noexcept {
        claim_guard_t const held {*this};
        if (status_t const adopted = adopt_channels_(); adopted.failed()) return adopted;
        published_generation_ = 0;
        if (status_t const published = publish_empty_manifest_(); published.failed()) return published;
        collection_.destroy();
        if (user_data_bytes_ != 0) volume_.deallocate(user_data_);
        user_data_bytes_ = 0;
        return {};
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
        if (status_t const adopted = adopt_channels_(); adopted.failed()) return adopted;
        if (status_t const flushed = collection_.flush(writing_channel_); flushed.failed()) return flushed;
        // The run the flush wrote is drained by now, and the publication flushes it before naming.
        return publish_manifest_();
    }

    /** Whether any level is over the bytes its number admits, answered without excluding a writer. */
    [[nodiscard]] bool needs_compaction() const noexcept { return collection_.needs_compaction(); }

    /** Merges the shallowest level that is over its bounds, reading on one channel and writing on the other. */
    expected<compaction_outcome_t> compact() noexcept {
        claim_guard_t const held {*this};
        if (status_t const adopted = adopt_channels_(); adopted.failed()) return adopted;
        lsm_tree_publication_hook_t published;
        published.notify = &record_manifest_;
        published.context = this;
        return collection_.compact(writing_channel_, merging_channel_, compactor_, published);
    }

    /** How many memory runs the collection holds, and how many commits they hold. */
    [[nodiscard]] memory_shape_t memory_shape() const noexcept { return collection_.memory_shape(); }

    /** How many manifests the volume has published, the one a mount read included. */
    [[nodiscard]] std::uint64_t manifest_generation() const noexcept { return volume_.generation(); }

    /** Drops every version above @p ceiling, which a mount runs once its ledger has resumed at @p ceiling. */
    status_t truncate_above(transaction_stamp_t ceiling) noexcept {
        claim_guard_t const held {*this};
        if (status_t const adopted = adopt_channels_(); adopted.failed()) return adopted;
        lsm_tree_publication_hook_t published;
        published.notify = &record_manifest_;
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
        return registry_.template create_transaction<transaction_t>(config);
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

#pragma endregion Transactions

#pragma region User Data

    /** Bytes the owner's data takes beside the runs, which is zero when it has saved none. */
    [[nodiscard]] std::uint64_t user_data_size_bytes() noexcept { return user_data_bytes_; }

    /** Replaces the owner's data with @p bytes, named beside the runs, clearing it for none. */
    status_t save_user_data(std::span<byte_t const> bytes) noexcept {
        if (bytes.empty()) return clear_user_data();
        claim_guard_t const held {*this};
        if (status_t const adopted = adopt_channels_(); adopted.failed()) return adopted;
        uuid_t const run = uuid_generator();
        auto [written, allocate_status] = volume_.allocate(run, bytes.size());
        if (allocate_status.failed()) return allocate_status;
        if (status_t const filled = write_extent<disk_t, waiter_type_>(volume_.disk(), writing_channel_, written,
                                                                       bytes);
            filled.failed()) {
            volume_.deallocate(written);
            return filled;
        }
        return replace_user_data_(run, written, bytes.size());
    }

    /** Copies the owner's data into @p into, at least as long as it, on @p channel. */
    status_t load_user_data(io_channel_t& channel, std::span<byte_t> into) noexcept {
        if (user_data_bytes_ == 0) return {status_code_t::not_found_k, "The database holds no user data"};
        if (into.size() < user_data_bytes_)
            return {status_code_t::invalid_argument_k, "A buffer shorter than the data"};
        return read_extent<disk_t>(volume_.disk(), channel, user_data_,
                                   into.first(static_cast<std::size_t>(user_data_bytes_)));
    }

    /** Forgets the owner's data, which a database holding none reports as @c not_found_k. */
    status_t clear_user_data() noexcept {
        claim_guard_t const held {*this};
        if (status_t const adopted = adopt_channels_(); adopted.failed()) return adopted;
        if (user_data_bytes_ == 0) return {status_code_t::not_found_k, "The database holds no user data"};
        return replace_user_data_(uuid_t {}, medium_extent_t {}, 0);
    }

#pragma endregion User Data

#pragma region Transaction Store Interface

    /** Whether a commit newer than @p from wrote @p stored_key, read from the published view; the
     *  one collection answers whichever ID a transaction names. */
    [[nodiscard]] bool written_since(collection_id_t, stored_key_t stored_key,
                                     transaction_stamp_t from) const noexcept {
        return collection_.written_since(stored_key, from);
    }

    /** Whether a commit newer than @p from removed @p stored_key or wrote it whole. */
    [[nodiscard]] bool replaced_since(collection_id_t, stored_key_t stored_key,
                                      transaction_stamp_t from) const noexcept {
        return collection_.replaced_since(stored_key, from);
    }

    /** Whether a commit newer than @p from wrote any key from @p lower to @p upper inclusive. */
    [[nodiscard]] bool range_written_since(collection_id_t, stored_key_t lower, stored_key_t upper,
                                           transaction_stamp_t from) const noexcept {
        return collection_.range_written_since(lower, upper, from);
    }

    /** Seals every delta of @p written into @p sealed on the committing thread, flushing first when the memory runs
     *  are full, so the commit that follows cannot fail halfway. */
    status_t seal_collection(collection_id_t collection_id, transaction_deltas_t const& written,
                             sealed_commit<stored_key_t, atomic_reference_>& sealed) noexcept {
        st::vector<keyed_delta<stored_key_t>> deltas;
        if (deltas.reserve(written.count(collection_id)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to hand a commit over"};
        written.for_each_delta(collection_id, [&](stored_key_t stored_key, delta_view_t delta) noexcept {
            [[maybe_unused]] st::status_t const placed = deltas.push_back(
                st::assume_reserved, keyed_delta<stored_key_t> {stored_key, delta});
        });
        auto [taken, seal_status] = collection_.seal(std::span<keyed_delta<stored_key_t>> {deltas}, {},
                                                     written.lendable_bytes());
        if (seal_status.failed()) return seal_status;
        sealed = std::move(taken);
        return make_room_(sealed);
    }

    /**
     *  @brief Claims a memory-run record for a commit sealed for the collection, before the commit
     *      takes the leader word: a free one, else the flush of the memory runs while nobody holds
     *      the claim, else one vacated while this waits.
     *  @note Called outside the ledger's epochs, which every flush waits out.
     */
    status_t claim_memory_record(collection_id_t, memory_run_claim_t& claimed) noexcept {
        atomic_reference_<std::uint32_t const> const vacated {collection_.vacated()};
        for (;;) {
            std::uint32_t const observed = vacated.load(std::memory_order_acquire);
            status_t const taken = collection_.try_claim(claimed);
            if (taken != status_code_t::capacity_exhausted_k) return taken;
            // A flush frees every published run's record; with none published, every record is out
            // with a commit.
            if (collection_.memory_shape().runs != 0) {
                claim_guard_t const held = claim_guard_t::taken_if_free(*this);
                if (held.holding()) {
                    if (status_t const adopted = adopt_channels_(); adopted.failed()) return adopted;
                    if (status_t const flushed = flush_and_compact_(); flushed.failed()) return flushed;
                    continue;
                }
            }
            if (vacated.load(std::memory_order_acquire) == observed)
                waiter_type_ {}(&collection_.vacated(), observed, std::size_t {0});
        }
    }

    /** Lays out the view @p sealed is published in, out of the room @p claimed holds, on the
     *  thread holding the ledger's leader word. */
    status_t prepare_collection(collection_id_t, sealed_commit<stored_key_t, atomic_reference_>& sealed,
                                memory_run_claim_t&& claimed) noexcept {
        collection_.prepare(sealed, std::move(claimed));
        return {};
    }

    /** Publishes @p sealed under @p stamp, before the stamp itself is, on the thread holding the leader word. */
    void publish_collection(collection_id_t, sealed_commit<stored_key_t, atomic_reference_>& sealed,
                            transaction_stamp_t stamp) noexcept {
        if (!sealed.empty()) collection_.publish(sealed, stamp);
    }

    /** Forgets the view @c prepare_collection laid out, for a commit the leader refuses. */
    void forget_collection(collection_id_t) noexcept { collection_.forget_prepared(); }

    /**
     *  @brief Merges the memory runs by tier, and flushes them and compacts the tree once they hold their bounds,
     *      which every commit runs once it lands.
     *
     *  Returns at once while another writer holds the claim, since that writer looks at the memory runs again
     *  after it lets go, so no commit waits behind another's maintenance.
     *  @note Called outside the ledger's epochs.
     */
    status_t maintain(collection_id_t) noexcept {
        if (!collection_.maintenance_due()) return {};
        claim_guard_t const held = claim_guard_t::taken_if_free(*this);
        if (!held.holding()) return {};
        if (status_t const adopted = adopt_channels_(); adopted.failed()) return adopted;
        return maintain_due_();
    }

#pragma endregion Transaction Store Interface

  private:

#pragma region Private Types

    /** One writer's hold on the collection, which every other writer parks behind; the holder
     *  adopts both channels after taking it, since a queue pair belongs to the thread that
     *  announced itself on it. */
    using claim_guard_t =
        typename maintenance_claim<atomic_reference_, waiter_type_>::template guard<basic_single_collection_database>;

    friend claim_guard_t;

#pragma endregion Private Types

#pragma region Private Construction

    /** Points the registry at this database; @c open fills the pool and the collection after. */
    basic_single_collection_database(volume_t& volume, io_channel_t& writing_channel, io_channel_t& merging_channel,
                                     single_collection_database_config_t const& config,
                                     resources_shared_ptr<atomic_reference_, waiter_type_> resources,
                                     compactor_ref_t compactor) noexcept
        : volume_(volume), writing_channel_(writing_channel), merging_channel_(merging_channel), config_(config),
          compactor_(compactor), resources_(resources), registry_(*this, std::move(resources)) {}

    /**
     *  @brief Takes back the runs the manifest names, resumes the ledger above its durable stamp,
     *      drops every version above that, and only then publishes what a fresh volume lacks.
     *
     *  The ledger resumes before the truncation, which lets the truncation's manifest promise it.
     */
    status_t mount_() noexcept {
        claim_guard_t const held {*this};
        if (status_t const adopted = adopt_channels_(); adopted.failed()) return adopted;
        std::uint64_t const durable_stamp = volume_.manifest().durable_stamp;
        bool const fresh = volume_.manifest().collections.empty();
        if (status_t const agreed = agree_on_manifest_(); agreed.failed()) return agreed;
        if (status_t const resumed = ledger_.resume(durable_stamp); resumed.failed()) return resumed;
        published_generation_ = volume_.generation() + 1;
        lsm_tree_publication_hook_t published;
        published.notify = &record_manifest_;
        published.context = this;
        if (status_t const truncated = collection_.truncate_above(writing_channel_, merging_channel_, durable_stamp,
                                                                  published);
            truncated.failed()) {
            published_generation_ = 0;
            return truncated;
        }
        if (!fresh) return {};
        return publish_manifest_();
    }

#pragma endregion Private Construction

#pragma region Claim

    /** Binds both channels to the thread that took the claim, which every write through them needs. */
    status_t adopt_channels_() noexcept {
        channel_adoption_ = volume_.disk().bind_queue_pair(writing_channel_.queue_pair());
        if (channel_adoption_.succeeded())
            channel_adoption_ = volume_.disk().bind_queue_pair(merging_channel_.queue_pair());
        return channel_adoption_;
    }

    /** Lands @p sealed as a commit of its own, making room first while the memory runs are past twice their bounds,
     *  and maintains the collection once it lands. */
    status_t commit_alone_(expected<sealed_commit<stored_key_t, atomic_reference_>> sealed) noexcept {
        if (!sealed) return sealed.status();
        if (sealed->empty()) return {};
        if (status_t const room = make_room_(*sealed); room.failed()) return room;
        // Claimed before the leader word, waiting on a flush when every record is out, so the
        // commit cannot be refused.
        memory_run_claim_t claimed;
        if (status_t const taken = claim_memory_record(single_collection_id_k, claimed); taken.failed()) return taken;
        if (status_t const committed = collection_.commit(*sealed, std::move(claimed)); committed.failed())
            return committed;
        // The commit has landed, so a failure to maintain is met again by the next commit making room.
        [[maybe_unused]] status_t const maintained = maintain(single_collection_id_k);
        return {};
    }

    /**
     *  @brief Flushes the memory runs while @p sealed would leave them past twice their bounds, holding no epoch and
     *      parking rather than spinning while another writer holds the claim.
     *  @return Success once the commit fits, or why a flush could not make room.
     */
    status_t make_room_(sealed_commit<stored_key_t, atomic_reference_> const& sealed) noexcept {
        if (sealed.empty()) return {};
        while (collection_.memory_pressure(sealed) == memory_pressure_t::stalled_k) {
            claim_guard_t const held = claim_guard_t::taken_if_free(*this);
            if (!held.holding()) {
                // Whoever holds the claim may be flushing already, so the pressure is read again after.
                claim_.wait_until_released();
                continue;
            }
            if (status_t const adopted = adopt_channels_(); adopted.failed()) return adopted;
            if (status_t const flushed = flush_and_compact_(); flushed.failed()) return flushed;
        }
        return {};
    }

    /** Flushes the memory runs, publishes the manifest naming the run, and compacts the tree while
     *  the flush left a level over its bounds, holding the claim. */
    status_t flush_and_compact_() noexcept {
        if (status_t const flushed = collection_.flush(writing_channel_); flushed.failed()) return flushed;
        if (status_t const published = publish_manifest_(); published.failed()) return published;
        lsm_tree_publication_hook_t published;
        published.notify = &record_manifest_;
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
        if (status_t const merged = collection_.merge_memory_runs(); merged.failed()) return merged;
        if (!collection_.flush_due()) return {};
        return flush_and_compact_();
    }

    /**
     *  @brief Hands the claim back, then looks again at the memory runs after each release and, if
     *      they are due and the claim is still free, takes it back to maintain them, so a commit
     *      that found the claim held left its maintenance to this writer.
     *
     *  A writer that finds the claim taken again, or a caller parked on it, stops, since that
     *  holder looks once more after it lets go, and a failed adoption or maintenance is met again
     *  by the next commit.
     */
    void release_claim_() noexcept {
        // Every release looks again, so a commit that found the claim held is met after it.
        for (;;) {
            claim_.release();
            if (!collection_.maintenance_due() || claim_.anyone_parked() || !claim_.take_if_free()) return;
            if (adopt_channels_().failed() || maintain_due_().failed()) break;
        }
        claim_.release();
    }

#pragma endregion Claim

#pragma region Private Methods

    /** Takes back the runs and the user data the volume's manifest names, refusing one naming
     *  another collection shape, holding the claim. */
    status_t agree_on_manifest_() noexcept {
        manifest_t const& loaded = volume_.manifest();
        if (loaded.collections.empty()) return {};
        if (loaded.collections.size() != 1)
            return {status_code_t::malformed_k, "The volume's manifest names more than one collection"};
        manifest_collection_t const& carried = loaded.collections[0];
        if (carried.store != static_cast<std::uint16_t>(value_layout_k) ||
            carried.value_max_size != collection_.value_max_size())
            return {status_code_t::invalid_argument_k, "The volume holds a collection of another value layout"};

        st::vector<lsm_tree_run_record_t> runs;
        if (runs.reserve(loaded.files.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room for the runs the manifest names"};
        for (manifest_file_t const& file : loaded.files) {
            auto [opened, open_status] = volume_.open(file);
            if (open_status.failed()) return open_status;
            if (file.collection == manifest_no_collection_k) {
                user_data_run_ = file.run;
                user_data_ = opened;
                user_data_bytes_ = file.bytes;
                continue;
            }
            lsm_tree_run_record_t record;
            record.level_number = file.level;
            record.content_id = file.run;
            record.extent = opened;
            record.stamp = file.stamp;
            [[maybe_unused]] st::status_t const placed = runs.push_back(st::assume_reserved, std::move(record));
        }
        return collection_.restore(writing_channel_, std::span<lsm_tree_run_record_t const> {runs});
    }

    /** Publishes @p written of @p bytes as the owner's data; whichever extent the durable
     *  manifest stops naming, the replaced one or a refused new one, goes back after the next
     *  publication that succeeds. */
    status_t replace_user_data_(uuid_t const& run, medium_extent_t const& written, std::uint64_t bytes) noexcept {
        uuid_t const replaced_run = std::exchange(user_data_run_, run);
        medium_extent_t const replaced = std::exchange(user_data_, written);
        std::uint64_t const replaced_bytes = std::exchange(user_data_bytes_, bytes);
        if (status_t const published = publish_manifest_(); published.failed()) {
            user_data_run_ = replaced_run;
            user_data_ = replaced;
            user_data_bytes_ = replaced_bytes;
            if (bytes != 0) volume_.deallocate_after_publication(written);
            return published;
        }
        if (replaced_bytes != 0) volume_.deallocate_after_publication(replaced);
        return {};
    }

    /** Publishes the manifest naming the collection, every run its tree holds and the owner's
     *  data, which every publication of a run does. */
    status_t publish_manifest_() noexcept {
        manifest_t manifest;
        if (status_t const laid = lay_out_manifest_(manifest); laid.failed()) return laid;
        manifest_collection_t record;
        std::memcpy(record.id.bytes, single_collection_id_k.uuid.bytes, sizeof(record.id.bytes));
        if (sz::failed(record.name.assign(string_view_t {single_collection_name_k})))
            return {status_code_t::out_of_memory_k, "No room to lay the manifest out"};
        record.store = static_cast<std::uint16_t>(value_layout_k);
        record.value_size = is_fixed(value_layout_k) ? collection_.value_max_size() : 0;
        record.value_max_size = collection_.value_max_size();
        record.level_enlarge_factor = config_.collection.lsm_tree.level_enlarge_factor;
        record.cache_limit = config_.collection.cache_limit;
        record.memory_runs_max_bytes = config_.collection.memory_runs_max_bytes;
        record.memory_runs_max_deltas = config_.collection.memory_runs_max_deltas;
        if (manifest.collections.push_back(std::move(record)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to lay the manifest out"};

        st::vector<lsm_tree_run_record_t> runs;
        if (status_t const named = record_lsm_tree_runs(collection_.tree(), runs); named.failed()) return named;
        if (manifest.files.reserve(runs.size() + 1) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to lay the manifest out"};
        for (lsm_tree_run_record_t const& run : runs) [[maybe_unused]]
            st::status_t const placed = manifest.files.push_back(
                st::assume_reserved,
                file_of_(run.extent, run.content_id, run.extent.extent.length, 0, run.level_number, run.stamp));
        if (user_data_bytes_ != 0) [[maybe_unused]]
            st::status_t const placed = manifest.files.push_back(
                st::assume_reserved,
                file_of_(user_data_, user_data_run_, user_data_bytes_, manifest_no_collection_k, 0, 0));
        return print_and_publish_(manifest);
    }

    /** Publishes a manifest naming no collection and no file, which a destroy does first. */
    status_t publish_empty_manifest_() noexcept {
        manifest_t manifest;
        if (status_t const laid = lay_out_manifest_(manifest); laid.failed()) return laid;
        return print_and_publish_(manifest);
    }

    /** Fills the identity of @p manifest from the volume and its durable stamp from the ledger. */
    status_t lay_out_manifest_(manifest_t& manifest) noexcept {
        manifest_t const& loaded = volume_.manifest();
        if (sz::failed(manifest.database.assign(loaded.database.view())))
            return {status_code_t::out_of_memory_k, "No room to lay the manifest out"};
        manifest.id = loaded.id;
        manifest.generation = volume_.generation() + 1;
        manifest.durable_stamp = ledger_.durable_stamp();
        manifest.placement = volume_.placement();
        return {};
    }

    /** Prints @p manifest and publishes it through the volume, flushing every run written since. */
    status_t print_and_publish_(manifest_t const& manifest) noexcept {
        st::vector<char> json;
        if (status_t const printed = manifest.print(
                constrained_allocator<byte_t, atomic_reference_> {resources_->memory_constraint}, json);
            printed.failed())
            return printed;
        return volume_.publish_manifest(writing_channel_, std::span<char const> {json});
    }

    /** The manifest entry of @p extent, run @p run of @p bytes, of collection @p collection. */
    [[nodiscard]] static manifest_file_t file_of_(medium_extent_t const& extent, uuid_t const& run, std::uint64_t bytes,
                                                  std::uint32_t collection, std::uint32_t level,
                                                  std::uint64_t stamp) noexcept {
        manifest_file_t file;
        file.run = run;
        file.offset = extent.extent.offset;
        file.bytes = bytes;
        file.stamp = stamp;
        file.collection = collection;
        file.level = level;
        file.medium = extent.data_directory;
        return file;
    }

    /** Publishes the manifest for a published compaction, before it drops what it consumed. */
    static status_t record_manifest_(void* context) noexcept {
        return static_cast<basic_single_collection_database*>(context)->publish_manifest_();
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

    /** The stamps every commit of this database draws. */
    commit_ledger<atomic_reference_, waiter_type_> ledger_;

    /** The runs of the collection, which the collection's tree points into and so outlives it. */
    run_pool_t runs_;

    /** The collection every write lands in, which pins the ledger and so dies before it. */
    collection_t collection_;

    /** The ID a directory volume names the file of the owner's data by. */
    uuid_t user_data_run_ {};

    /** Where the owner's data lies, meaningful while @c user_data_bytes_ is not zero. */
    medium_extent_t user_data_ {};

    /** Bytes of the owner's data, zero when it has saved none. */
    std::uint64_t user_data_bytes_ = 0;

    /** The generation the mount expects to publish next, zero for a database that never mounted
     *  whole or was destroyed, which must not publish over what it missed. */
    std::uint64_t published_generation_ = 0;

    /** What every merge runs on, picked at open: the calling thread, or a device compactor the opener owns. */
    compactor_ref_t compactor_;

    /** The budget the manifests this database prints are charged to. */
    resources_shared_ptr<atomic_reference_, waiter_type_> resources_;

    /** The order every transaction is placed in, and the last commit to write each key. */
    registry_t registry_;

    /** Held by whichever writer flushes, compacts or saves the user data, a commit paying for its
     *  maintenance included. */
    maintenance_claim<atomic_reference_, waiter_type_> claim_;

    /** Whether the writer holding the claim took both channels over, written only under it. */
    status_t channel_adoption_;

#pragma endregion Private Members
};

#pragma endregion Database

#pragma endregion Single Collection Database

} // namespace unum::ustore::blobs
