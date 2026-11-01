/**
 *  @file include/ustore/blobs/nvme_single_collection_database.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Single-collection databases, with their snapshots and their optimistic transactions over
 *      the registry and transaction core of @c nvme_transactions.hpp.
 *
 *  A single-collection database is the smallest thing that can be committed to: one collection,
 *  the registry that orders the commits landing in it, and two blobs on the volume - the schema a
 *  reopen checks itself against, and whatever bytes the owner keeps beside the data. Reads take
 *  the caller's channel, so any number of threads read at once; flushes, compactions and blob
 *  writes take the two channels the database was opened with, under its claim, which one thread
 *  holds at a time.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint16_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::memset`

#include <array>       // `std::array`
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
#include "ustore/utilities/nvme_catalog.hpp"    // `schema_copies`, `blob_kind_t`, `blob_name_t`, `blob_name_of`
#include "ustore/utilities/nvme_channels.hpp"   // `io_channel_t`, `basic_write_pipeline`, `basic_read_pipeline`
#include "ustore/utilities/nvme_volume.hpp"     // `volume_blob_t`
#include "ustore/utilities/types.hpp"           // `status_t`, `status_code_t`, `expected`

namespace unum::ustore::blobs {

using utilities::blob_name_of;
using utilities::schema_copies;
using utilities::schema_copy_of;
using utilities::schema_stamps_t;

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
 *  @tparam stored_key_type_ The key the collection is addressed by.
 *  @tparam volume_type_ The volume every run and every blob lives on.
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

    /** The volume every run and every blob of this database lives on. */
    using volume_t = volume_type_;

    /** The collection every write of this database lands in. */
    using collection_t = store_type_;

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

#pragma endregion Types

#pragma region Construction

    basic_single_collection_database(basic_single_collection_database const&) = delete;
    basic_single_collection_database& operator=(basic_single_collection_database const&) = delete;
    basic_single_collection_database(basic_single_collection_database&&) = delete;
    basic_single_collection_database& operator=(basic_single_collection_database&&) = delete;

    /** Writes whatever the memory runs still hold onto the volume and names every run, so the data outlives this. */
    ~basic_single_collection_database() noexcept {
        // A database that never mounted whole, or was destroyed, names no schema and must not write one back.
        if (schema_.generation() == 0) return;
        claim_guard_t const held {*this};
        if (adopt_channels_().failed()) return;
        [[maybe_unused]] status_t const flushed = collection_.flush(writing_channel_);
        [[maybe_unused]] status_t const saved = save_schema_();
    }

    /**
     *  @brief Opens a database over @p volume, creating its schema blob when the volume holds none.
     *  @param[in,out] volume The volume every run and blob lands on, borrowed and outliving the database.
     *  @param[in,out] writing_channel The channel every write, flush and commit runs on, borrowed for the database's
     *      life.
     *  @param[in,out] merging_channel The second channel a compaction writes its new runs on, which
     *      is not @p writing_channel.
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
        if (status_t const adopted = adopt_channels_(); adopted.failed()) return adopted;
        schema_.forget();
        named_blobs_t blobs {this, &writing_channel_};
        status_t first = schema_.drop(blobs);
        if (status_t const dropped = schema_.clear_user_data(blobs); dropped.failed() && first.succeeded())
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
        if (status_t const adopted = adopt_channels_(); adopted.failed()) return adopted;
        if (status_t const flushed = collection_.flush(writing_channel_); flushed.failed()) return flushed;
        // The run the flush wrote is sealed by now, so the schema may name it.
        return save_schema_();
    }

    /** Whether any level is over the bytes its number admits, answered without excluding a writer. */
    [[nodiscard]] bool needs_compaction() const noexcept { return collection_.needs_compaction(); }

    /** Merges the shallowest level that is over its bounds, reading on one channel and writing on the other. */
    expected<compaction_outcome_t> compact() noexcept {
        claim_guard_t const held {*this};
        if (status_t const adopted = adopt_channels_(); adopted.failed()) return adopted;
        lsm_tree_publication_hook_t published;
        published.notify = &record_schema_;
        published.context = this;
        return collection_.compact(writing_channel_, merging_channel_, compactor_, published);
    }

    /** How many memory runs the collection holds, and how many commits they hold. */
    [[nodiscard]] memory_shape_t memory_shape() const noexcept { return collection_.memory_shape(); }

    /** How many times the schema blob has been saved, whose @c schema_copy_of names the copy a mount reads. */
    [[nodiscard]] std::uint64_t schema_generation() const noexcept { return schema_.generation(); }

    /** Drops every version above @p ceiling, which a mount runs once its ledger has resumed at @p ceiling. */
    status_t truncate_above(transaction_stamp_t ceiling) noexcept {
        claim_guard_t const held {*this};
        if (status_t const adopted = adopt_channels_(); adopted.failed()) return adopted;
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

    /** Bytes the owner's blob takes beside the data, which is zero when the volume holds none. */
    [[nodiscard]] std::uint64_t user_data_size_bytes() noexcept {
        named_blobs_t blobs {this, &writing_channel_};
        return schema_.user_data_size_bytes(blobs);
    }

    /** Replaces the owner's blob with @p bytes, which the volume keeps beside the runs, dropping
     *  it for none. */
    status_t save_user_data(std::span<byte_t const> bytes) noexcept {
        claim_guard_t const held {*this};
        if (status_t const adopted = adopt_channels_(); adopted.failed()) return adopted;
        named_blobs_t blobs {this, &writing_channel_};
        return schema_.save_user_data(blobs, bytes);
    }

    /** Copies the owner's blob into @p into, which must be at least as long as the blob,
     *  on @p channel. */
    status_t load_user_data(io_channel_t& channel, std::span<byte_t> into) noexcept {
        named_blobs_t blobs {this, &channel};
        return schema_.load_user_data(blobs, into);
    }

    /** Forgets the owner's blob, which a volume holding none reports as @c not_found_k. */
    status_t clear_user_data() noexcept {
        claim_guard_t const held {*this};
        if (status_t const adopted = adopt_channels_(); adopted.failed()) return adopted;
        named_blobs_t blobs {this, &writing_channel_};
        return schema_.clear_user_data(blobs);
    }

#pragma endregion User Data

#pragma region Transaction Store Interface

    /** Whether a commit newer than @p from wrote @p stored_key, read from the published view; the
     *  one collection answers whichever ID a transaction names. */
    [[nodiscard]] bool written_since(collection_id_t, stored_key_t stored_key,
                                     transaction_stamp_t from) const noexcept {
        return collection_.written_since(stored_key, from);
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
        auto [taken, seal_status] = collection_.seal(std::span<keyed_delta<stored_key_t>> {deltas});
        if (seal_status.failed()) return seal_status;
        sealed = std::move(taken);
        return make_room_(sealed);
    }

    /** Lays out the view @p sealed is published in, on the thread holding the ledger's leader word. */
    status_t prepare_collection(collection_id_t, sealed_commit<stored_key_t, atomic_reference_>& sealed) noexcept {
        if (!sealed.run) return {};
        return collection_.prepare(sealed);
    }

    /** Publishes @p sealed under @p stamp, before the stamp itself is, on the thread holding the leader word. */
    void publish_collection(collection_id_t, sealed_commit<stored_key_t, atomic_reference_>& sealed,
                            transaction_stamp_t stamp) noexcept {
        if (sealed.run) collection_.publish(sealed, stamp);
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

    /** The named blobs of this database on its volume, written on the writing channel and read
     *  on @c reading. */
    struct named_blobs_t {

        /** The database whose volume holds the blobs. */
        basic_single_collection_database* store = nullptr;

        /** The channel a read is submitted and completed on, held by the calling thread. */
        io_channel_t* reading = nullptr;

        /** Bytes the blob @p name holds, or @c not_found_k for a name the volume holds not. */
        expected<std::uint64_t> length(blob_name_t name) noexcept {
            auto [found, find_status] = store->volume_.find(name);
            if (find_status.failed()) return find_status;
            return found.length;
        }

        /** Replaces the blob @p name with @p bytes of @p kind. */
        status_t write(blob_name_t name, blob_kind_t kind, bytes_view_t bytes) noexcept {
            return store->write_blob_(name, kind, bytes);
        }

        /** Reads the whole blob @p name into @p into. */
        status_t read(blob_name_t name, bytes_span_t into) noexcept {
            auto [found, find_status] = store->volume_.find(name);
            if (find_status.failed()) return find_status;
            return store->read_blob_(*reading, found, into);
        }

        /** Forgets the blob @p name, which a volume holding none reports as @c not_found_k. */
        status_t drop(blob_name_t name) noexcept { return store->drop_blob_(name); }
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
            opened->schema_.forget();
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
        if (status_t const adopted = adopt_channels_(); adopted.failed()) return adopted;
        if (status_t const agreed = agree_on_schema_(); agreed.failed()) return agreed;
        if (status_t const resumed = ledger_.resume(schema_.loaded_durable()); resumed.failed()) return resumed;
        lsm_tree_publication_hook_t published;
        published.notify = &record_schema_;
        published.context = this;
        if (status_t const truncated = collection_.truncate_above(writing_channel_, merging_channel_,
                                                                  schema_.loaded_durable(), published);
            truncated.failed())
            return truncated;
        if (schema_.generation() != 0) return {};
        return save_schema_();
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
        if (!sealed->run) return {};
        if (status_t const room = make_room_(*sealed); room.failed()) return room;
        if (status_t const committed = collection_.commit(*sealed); committed.failed()) return committed;
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
        if (!sealed.run) return {};
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
        if (status_t const merged = collection_.merge_memory_runs(); merged.failed()) return merged;
        if (!collection_.flush_due()) return {};
        return flush_and_compact_();
    }

    /**
     *  @brief Hands the claim back, then looks once more at the memory runs and, if they are due and the claim is
     *      still free, takes it back to maintain them, so a commit that found the claim held left its maintenance to
     *      this writer.
     *
     *  A writer that finds the claim taken again, or a caller parked on it, stops, since that
     *  holder looks once more after it lets go, and a failed adoption or maintenance is met again
     *  by the next commit.
     */
    void release_claim_() noexcept {
        claim_.release();
        if (!collection_.maintenance_due() || claim_.anyone_parked() || !claim_.take_if_free()) return;
        if (adopt_channels_().succeeded()) [[maybe_unused]]
            status_t const maintained = maintain_due_();
        claim_.release();
    }

#pragma endregion Claim

#pragma region Private Methods

    /** The shape this database would write into its schema blob, less the runs that follow it there. */
    [[nodiscard]] single_collection_database_schema_record_t wanted_schema_() const noexcept {
        single_collection_database_schema_record_t wanted;
        wanted.value_layout = static_cast<std::uint16_t>(value_layout_k);
        wanted.value_max_size = collection_.value_max_size();
        return wanted;
    }

    /**
     *  @brief Reads the newer copy of the schema blob and takes its runs back, leaving a volume that holds no copy as
     *      it is, holding the claim.
     */
    status_t agree_on_schema_() noexcept {
        std::array<st::vector<byte_t>, 2> images;
        std::array<single_collection_database_schema_record_t, 2> records {};
        named_blobs_t blobs {this, &writing_channel_};
        if (status_t const loaded = schema_.load(
                blobs,
                [&](std::size_t copy, st::vector<byte_t>& image) noexcept -> expected<schema_stamps_t> {
                    if (image.size() < sizeof(single_collection_database_schema_record_t))
                        return status_t {status_code_t::malformed_k,
                                         "The volume's schema blob is shorter than one record"};
                    std::memcpy(&records[copy], image.data(), sizeof(records[copy]));
                    images[copy] = std::move(image);
                    return records[copy].stamps;
                });
            loaded.failed())
            return loaded;
        if (schema_.generation() == 0) return {};

        std::size_t const newer = schema_copy_of(schema_.generation());
        single_collection_database_schema_record_t const wanted = wanted_schema_();
        st::vector<byte_t> const& image = images[newer];
        single_collection_database_schema_record_t const& carried = records[newer];
        if (carried.magic != single_collection_database_magic_k)
            return {status_code_t::malformed_k, "The volume's schema blob carries another magic"};
        if (carried.format_version != single_collection_database_format_version_k)
            return {status_code_t::unsupported_k, "The volume's schema follows another format version"};
        if (carried.value_layout != wanted.value_layout || carried.value_max_size != wanted.value_max_size)
            return {status_code_t::invalid_argument_k, "The volume holds a collection of another value layout"};
        if (image.size() - sizeof(carried) != carried.run_count * sizeof(lsm_tree_run_record_t))
            return {status_code_t::malformed_k, "The volume's schema names another number of runs"};

        st::vector<lsm_tree_run_record_t> runs;
        if (runs.resize(static_cast<std::size_t>(carried.run_count)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room for the runs the schema names"};
        if (!runs.empty())
            std::memcpy(runs.data(), image.data() + sizeof(carried), runs.size() * sizeof(lsm_tree_run_record_t));
        return collection_.restore(writing_channel_, std::span<lsm_tree_run_record_t const> {runs},
                                   std::span<lsm_tree_run_stamp_t const> {});
    }

    /** Names every run the tree holds in the schema blob, laid out back to back after the record,
     *  which every publication of a run does. */
    status_t save_schema_() noexcept {
        named_blobs_t blobs {this, &writing_channel_};
        return schema_.save(blobs, ledger_.durable_stamp(),
                            [&](schema_stamps_t const& stamps, st::vector<byte_t>& image) noexcept -> status_t {
                                st::vector<lsm_tree_run_record_t> runs;
                                if (status_t const named = record_lsm_tree_runs(collection_.tree(), runs);
                                    named.failed())
                                    return named;
                                single_collection_database_schema_record_t record = wanted_schema_();
                                record.run_count = runs.size();
                                record.stamps = stamps;
                                if (image.resize(sizeof(record) + runs.size() * sizeof(lsm_tree_run_record_t)) !=
                                    st::status_t::success_k)
                                    return {status_code_t::out_of_memory_k, "No room to lay the schema out"};
                                std::memcpy(image.data(), &record, sizeof(record));
                                if (!runs.empty())
                                    std::memcpy(image.data() + sizeof(record), runs.data(),
                                                runs.size() * sizeof(lsm_tree_run_record_t));
                                return {};
                            });
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

    /** The two copies of the schema blob and the owner's blob, and which copy is newer. */
    schema_copies<named_blobs_t> schema_ {blob_name_of("single-collection-database.schema.0"),
                                          blob_name_of("single-collection-database.schema.1"),
                                          blob_name_of("single-collection-database.user")};

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
