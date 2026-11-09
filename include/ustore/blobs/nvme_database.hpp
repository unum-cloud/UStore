/**
 *  @file include/ustore/blobs/nvme_database.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Databases of named collections, with their snapshots and their optimistic transactions
 *      over the registry, read sets and transaction core of @c nvme_transactions.hpp.
 *
 *  A database is a list of collections and nothing else: it resolves a name to a collection, hands
 *  that collection a channel, and persists the list in the manifest its volume commits, so a reopen
 *  finds the same names. Every byte of data belongs to the collection below it, and every conflict
 *  belongs to the registry beside it; what is left is placement, naming and the order the two are
 *  changed in. One claim guards every call that changes the collections, a tree or the user data,
 *  since flushes, compactions and manifest publications are serialized by it; commits land through
 *  the ledger's sequencer and take it only to pay for the maintenance they made due. A read takes
 *  no claim: a collection publishes immutable views a reader walks inside its ledger's epochs, the
 *  entries are pinned on the heap, and the list of collections is one more immutable view published
 *  the same way and freed by whoever replaces it, once its readers left.
 *
 *  A snapshot copies no run and moves no byte: it pins the stamp the ledger last published, which
 *  every merge keeps the versions of, and reads through whatever view each collection publishes.
 *  Every commit ordered at or before that stamp is inside the snapshot and every later one is not.
 *  A transaction's writes reach the collections only at the commit, which the registry lands whole
 *  or not at all.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint16_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::memset`

#include <algorithm>   // `std::copy`, `std::lower_bound`
#include <array>       // `std::array`
#include <limits>      // `std::numeric_limits`
#include <memory>      // `std::unique_ptr`
#include <new>         // `std::nothrow`
#include <optional>    // `std::optional`
#include <span>        // `std::span`, `std::as_bytes`
#include <type_traits> // `std::integral_constant`, `std::remove_reference_t`
#include <utility>     // `std::exchange`, `std::forward`, `std::index_sequence_for`, `std::move`
#include <variant>     // `std::variant`, `std::variant_alternative_t`, `std::visit`, `std::get_if`

#include <smashtable/basic_growing_pool.hpp> // `st::basic_growing_pool`
#include <smashtable/basic_hash_index.hpp>   // `st::basic_hash_index`, `st::key128_t`
#include <smashtable/basic_vector.hpp>       // `st::vector`

#include "ustore/blobs/nvme_key_slot.hpp"       // `stored_value_size_t`, `collection_id_t`
#include "ustore/blobs/nvme_published_runs.hpp" // `pinned_snapshot`, `maintenance_claim`, `commit_ledger`
#include "ustore/blobs/nvme_store.hpp"          // `nvme_store_cursor`, `keyed_delta`
#include "ustore/blobs/nvme_transactions.hpp"   // `transaction_registry`, `transaction_core`, `nvme_store_concept`
#include "ustore/blobs/nvme_tree.hpp"           // `lsm_tree_run_record_t`, `database_config_t`
#include "ustore/utilities/nvme_manifest.hpp"   // `manifest_t`, `manifest_collection_t`, `manifest_file_t`
#include "ustore/utilities/nvme_channels.hpp"   // `io_channel_t`, `basic_io_channel_pool`
#include "ustore/utilities/nvme_mounts.hpp"     // `resources_shared_ptr`
#include "ustore/utilities/nvme_ring.hpp"       // `portable_alignment_k`
#include "ustore/utilities/algorithm.hpp"       // `radix_sort`
#include "ustore/utilities/nvme_volume.hpp"     // `medium_extent_t`, `volume_config_t`
#include "ustore/utilities/ram_buffers.hpp"     // `constrained_allocator`
#include "ustore/utilities/types.hpp" // `status_t`, `expected`, `uuid_t`, `bytes_span_t`, `save_fields`, `load_fields`

namespace unum::ustore::blobs {

using utilities::basic_io_channel_pool;
using utilities::manifest_collection_t;
using utilities::manifest_file_t;
using utilities::manifest_no_collection_k;
using utilities::manifest_t;
using utilities::medium_extent_t;
using utilities::path_max_bytes_k;
using utilities::sha256_t;
using utilities::volume_config_t;

#pragma region Database Snapshot

/**
 *  @brief One moment of a database: a stamp pinned in its ledger, which every read through the
 *      snapshot answers at, naming any collection of the database by its ID or its name.
 *  @tparam store_type_ The database read, which must outlive the snapshot.
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
        taken.pinned_ = pinned_snapshot<store_t>::of(store);
        return taken;
    }

    /** The stamp every read through this snapshot answers at. */
    [[nodiscard]] transaction_stamp_t stamp() const noexcept { return pinned_.stamp(); }

    /** Whether @p id names a collection of the database now. */
    [[nodiscard]] bool contains(any_collection_id_t const& id) noexcept { return pinned_.store().has_collection(id); }

    /** What @p id held for @p stored_key at the snapshot's stamp, and how many bytes its value took. */
    [[nodiscard]] lookup_result_t find(any_collection_id_t const& id, stored_key_t stored_key,
                                       stored_value_size_t& size) noexcept {
        return pinned_.store().find(id, stored_key, size, pinned_.stamp());
    }

    /** Copies the value @p id held for @p stored_key at the snapshot's stamp into @p into, filing it in the value
     *  cache when it paid I/O for it. */
    status_t select(any_collection_id_t const& id, stored_key_t stored_key, std::span<byte_t> into,
                    stored_value_size_t& size) noexcept {
        return pinned_.store().select(id, stored_key, into, size, pinned_.stamp());
    }

  private:

    /** The database read and the stamp pinned in its ledger. */
    pinned_snapshot<store_t> pinned_;
};

#pragma endregion Database Snapshot

#pragma region Database Transaction

/**
 *  @brief A group of writes and reads over several collections that succeed or fail together.
 *  @tparam stored_key_type_ The key every collection of the store is addressed by.
 *  @tparam store_type_ The store the writes land in, which must satisfy @c transaction_store_concept.
 *  @tparam row_kit_type_ The SmashTable kit the writes' and watches' hash tables search with.
 *
 *  The core carries the writes, the watches, the stamps and the two-phase lifecycle; what is here
 *  resolves a collection's name to its ID and widths once per run of calls, and reads through the
 *  store at the right stamp.
 */
template <typename stored_key_type_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, typename row_kit_type_>
class database_transaction {
  public:

#pragma region Types

    /** The key every collection of the store is addressed by. */
    using stored_key_t = stored_key_type_;

    /** The store every committed write of this transaction lands in. */
    using store_t = store_type_;

    /** The registry that orders this transaction against every other of the same store. */
    using registry_t = transaction_registry<stored_key_t, store_t, atomic_reference_, waiter_type_, row_kit_type_>;

    /** The writes, the watches, the stamps and the lifecycle this transaction shares with
     *  every other kind. */
    using core_t = transaction_core<stored_key_t, registry_t, atomic_reference_, waiter_type_>;

    /** The uncommitted writes and the watched keys this transaction carries. */
    using state_t = typename core_t::state_t;

    /** The uncommitted deltas alone, filed under the collection they belong to. */
    using transaction_deltas_t = typename state_t::transaction_deltas_t;

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
     *  @param[in,out] manager The registry that orders and conflict-checks it, outliving it.
     *  @param[in] deltas The deltas it fills, already sized to its configuration.
     *  @param[in] resources The budget its watched keys are charged to.
     */
    database_transaction(registry_t& manager, transaction_deltas_t&& deltas,
                         resources_shared_ptr<atomic_reference_, waiter_type_> resources) noexcept
        : core_(manager, std::move(deltas), std::move(resources)) {}

#pragma endregion Construction

#pragma region Lifecycle

    /** Seals every write, takes the ledger's leader word and lays out every view the
     *  commit publishes, as @c transaction_core::stage does. */
    status_t stage() noexcept { return core_.stage(); }

    /** Lands every write of this transaction and starts it again,
     *  as @c transaction_core::commit does. */
    status_t commit() noexcept { return core_.commit(); }

    /** Drops every write and every watch, and starts the transaction over from this moment. */
    void rollback() noexcept { core_.rollback(); }

    /** Hands back the leader word a stage took, keeping every write and every watch. */
    void unstage() noexcept { core_.unstage(); }

    /** Starts over from this moment while keeping the writes, which only a transaction that
     *  read nothing may. */
    status_t renew() noexcept { return core_.renew(); }

    /** Joins @p share, which every stage and commit from now on lays out and lands with this transaction's own writes,
     *  and which must outlive the transaction. */
    void join(transaction_share_t& share) noexcept { core_.join(share); }

    /**
     *  @brief Watches @p stored_keys of @p id for conflicts, without reading or writing any of them.
     *  @param[in] id The collection the keys belong to, named by its ID or by its name.
     *  @param[in] stored_keys The keys to watch, which are dropped by a commit and by a rollback.
     */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t watch_more(any_collection_id_t const& id, std::span<stored_key_t const> stored_keys) noexcept {
        collection_id_t const collection_id = resolve_(id).id;
        if (collection_id == collection_invalid_id_k)
            return {status_code_t::not_found_k, "The store names no such collection"};
        return core_.template watch_more<watch_schedule_>(collection_id, stored_keys);
    }

    /**
     *  @brief Watches @p stored_keys of @p id for being removed or wholly written, letting
     *      change deltas through.
     *  @param[in] id The collection the keys belong to, named by its ID or by its name.
     *  @param[in] stored_keys The keys an edit folds onto, which must still stand when it commits.
     */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t watch_existence(any_collection_id_t const& id, std::span<stored_key_t const> stored_keys) noexcept {
        collection_id_t const collection_id = resolve_(id).id;
        if (collection_id == collection_invalid_id_k)
            return {status_code_t::not_found_k, "The store names no such collection"};
        return core_.template watch_existence<watch_schedule_>(collection_id, stored_keys);
    }

    /** Watches every key of @p id from @p lower to @p upper inclusive, the absent ones too, which a scan this
     *  transaction ran over them does so a commit inserting into the range conflicts. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t watch_range(any_collection_id_t const& id, stored_key_t lower, stored_key_t upper) noexcept {
        collection_id_t const collection_id = resolve_(id).id;
        if (collection_id == collection_invalid_id_k)
            return {status_code_t::not_found_k, "The store names no such collection"};
        return core_.template watch_range<watch_schedule_>(collection_id, lower, upper);
    }

#pragma endregion Lifecycle

#pragma region Catalog

    /** Whether the store names the collection @p id. */
    [[nodiscard]] bool has_collection(any_collection_id_t const& id) noexcept {
        if (!core_.started()) return false;
        return core_.store().has_collection(id);
    }

    /** The ID @p id resolves to, or @c collection_invalid_id_k when it names nothing. */
    [[nodiscard]] collection_id_t collection_id(any_collection_id_t const& id) noexcept {
        if (!core_.started()) return collection_invalid_id_k;
        return resolve_(id).id;
    }

    /** The name and the number of every collection of the store. */
    [[nodiscard]] st::vector<collection_ids_t> list_collections() noexcept {
        if (!core_.started()) return {};
        return core_.store().list_collections();
    }

    /** Bytes the largest value @p id admits takes, which is what a caller sizes a read buffer by. */
    [[nodiscard]] stored_value_size_t buffer_size_for_any_stored_value(any_collection_id_t const& id) noexcept {
        if (!core_.started()) return 0;
        return core_.store().buffer_size_for_any_stored_value(id);
    }

    /** Whether @p count more writes of @p bytes still fit in this transaction's own deltas. */
    [[nodiscard]] bool has_available_space_for(std::size_t count, std::size_t bytes) const noexcept {
        return core_.state().deltas.can_fit(count, bytes);
    }

#pragma endregion Catalog

#pragma region Writes

    /** Writes @p stored_value under @p stored_key of @p id into this transaction's own deltas, watching the key for
     *  another commit writing it as @p watch_schedule_ says. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t insert(any_collection_id_t const& id, stored_key_t stored_key, stored_value_view_t stored_value) noexcept {
        if (!core_.started()) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        resolved_collection_t const named = resolve_(id);
        if (named.id == collection_invalid_id_k)
            return {status_code_t::not_found_k, "The store names no such collection"};
        if (status_t const checked = core_.template check_write_now<watch_schedule_>(named.id, stored_key);
            checked.failed())
            return checked;
        return core_.state().deltas.write(named.id, named.widths, stored_key, stored_value,
                                          core_.template watch_stamp_for<watch_schedule_>());
    }

    /** Writes a removal of @p stored_key of @p id into this transaction's own deltas, watched as @c insert is. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t remove(any_collection_id_t const& id, stored_key_t stored_key) noexcept {
        if (!core_.started()) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        resolved_collection_t const named = resolve_(id);
        if (named.id == collection_invalid_id_k)
            return {status_code_t::not_found_k, "The store names no such collection"};
        if (status_t const checked = core_.template check_write_now<watch_schedule_>(named.id, stored_key);
            checked.failed())
            return checked;
        return core_.state().deltas.remove(named.id, named.widths, stored_key,
                                           core_.template watch_stamp_for<watch_schedule_>());
    }

    /** Removes every key of @p id from @p start up to @p end, exclusive, as one range this
     *  transaction's commit lands as a range tombstone, watching the range for another commit
     *  writing inside it as @p watch_schedule_ says. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t remove_range(any_collection_id_t const& id, stored_key_t start, stored_key_t end) noexcept {
        if (!core_.started()) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        resolved_collection_t const named = resolve_(id);
        if (named.id == collection_invalid_id_k)
            return {status_code_t::not_found_k, "The store names no such collection"};
        if (!(start < end)) return {status_code_t::invalid_argument_k, "A range ends at or before its start"};
        if (status_t const removed = core_.state().deltas.remove_range(named.id, named.widths, start, end);
            removed.failed())
            return removed;
        return core_.template watch_range<watch_schedule_>(named.id, start, stored_key_before(end));
    }

    /** The ranges this transaction's own deltas remove from @p collection_id, in removal order. */
    [[nodiscard]] std::span<range_tombstone<stored_key_t> const> uncommitted_ranges(
        collection_id_t collection_id) const noexcept {
        return core_.state().deltas.ranges(collection_id);
    }

    /**
     *  @brief Writes @p delta, a change delta of @p fold, under @p stored_key of @p id into this transaction's own
     *      deltas, folded after whatever they hold for the key.
     *
     *  A change reads nothing, so it watches nothing, and two transactions changing one key both commit; a caller
     *  whose change depends on what it read watches that key itself.
     */
    template <utilities::fold_concept fold_type_>
    status_t change(any_collection_id_t const& id, stored_key_t stored_key, stored_value_view_t delta,
                    fold_type_ const& fold) noexcept {
        if (!core_.started()) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        resolved_collection_t const named = resolve_(id);
        if (named.id == collection_invalid_id_k)
            return {status_code_t::not_found_k, "The store names no such collection"};
        return core_.state().deltas.change(named.id, named.widths, stored_key, delta, fold);
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
        status_t const own_status = stored_value_size_uncommitted(id, stored_key, size);
        if (own_status.succeeded() || !(own_status == status_code_t::not_found_k)) return own_status;
        return stored_value_size_committed(id, stored_key, size, thread_index);
    }

    /** Copies the value of @p stored_key into @p into, taking this transaction's own write first. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t select(any_collection_id_t const& id, stored_key_t stored_key, stored_value_span_t into,
                    stored_value_size_t& size, std::size_t thread_index = outside_executor_k) noexcept {
        status_t const own_status = select_uncommitted(id, stored_key, into, size);
        if (own_status.succeeded()) return own_status;
        return select_committed<watch_schedule_>(id, stored_key, into, size, thread_index);
    }

    /** What this transaction's own deltas did to @p stored_key of @p id: wrote it, removed it, or neither. */
    [[nodiscard]] lookup_result_t lookup_uncommitted(collection_id_t collection_id,
                                                     stored_key_t stored_key) const noexcept {
        return core_.state().deltas.lookup(collection_id, stored_key);
    }

    /** Fills @p held with what this transaction's own deltas did to @p stored_key of @p collection_id, a write, a
     *  removal or a change, answering whether they did anything. */
    [[nodiscard]] bool uncommitted_delta(collection_id_t collection_id, stored_key_t stored_key,
                                         delta_view_t& held) const noexcept {
        return core_.state().deltas.delta(collection_id, stored_key, held);
    }

    /** Hands @p callback every key this transaction wrote into @p collection_id, in no order, as
     *  @c callback(stored_key, kind, size), the kind telling a write from a removal and a change, which a reader
     *  folds over the committed value. */
    template <typename callback_type_>
    void for_each_uncommitted(collection_id_t collection_id, callback_type_&& callback) const noexcept {
        core_.state().deltas.for_each_delta(collection_id, [&](stored_key_t stored_key, delta_view_t delta) noexcept {
            callback(stored_key, delta.kind, static_cast<stored_value_size_t>(delta.bytes.size()));
        });
    }

    /** Whether this transaction's own deltas carry @p stored_key of @p id. */
    [[nodiscard]] bool contains_uncommitted(any_collection_id_t const& id, stored_key_t stored_key) noexcept {
        if (!core_.started()) return false;
        collection_id_t const collection_id = resolve_(id).id;
        if (collection_id == collection_invalid_id_k) return false;
        return core_.state().deltas.lookup(collection_id, stored_key) == lookup_result_t::written_k;
    }

    /** Bytes the value this transaction wrote for @p stored_key takes. */
    status_t stored_value_size_uncommitted(any_collection_id_t const& id, stored_key_t stored_key,
                                           stored_value_size_t& size) noexcept {
        if (!core_.started()) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        collection_id_t const collection_id = resolve_(id).id;
        if (collection_id == collection_invalid_id_k)
            return {status_code_t::not_found_k, "The store names no such collection"};
        return core_.state().deltas.stored_value_size(collection_id, stored_key, size);
    }

    /** Copies the value this transaction wrote for @p stored_key into @p into. */
    status_t select_uncommitted(any_collection_id_t const& id, stored_key_t stored_key, stored_value_span_t into,
                                stored_value_size_t& size) noexcept {
        if (!core_.started()) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        collection_id_t const collection_id = resolve_(id).id;
        if (collection_id == collection_invalid_id_k)
            return {status_code_t::not_found_k, "The store names no such collection"};
        return core_.state().deltas.select(collection_id, stored_key, into, size);
    }

    /** Whether the store carries @p stored_key of @p id, ignoring what this transaction wrote. */
    [[nodiscard]] bool contains_committed(any_collection_id_t const& id, stored_key_t stored_key,
                                          std::size_t thread_index = outside_executor_k) noexcept {
        if (!core_.started()) return false;
        return core_.store().contains(id, stored_key, core_.start_stamp(), thread_index);
    }

    /** Bytes the store held for @p stored_key of @p id when this transaction started, ignoring what it wrote. */
    status_t stored_value_size_committed(any_collection_id_t const& id, stored_key_t stored_key,
                                         stored_value_size_t& size,
                                         std::size_t thread_index = outside_executor_k) noexcept {
        if (!core_.started()) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        return core_.store().stored_value_size(id, stored_key, size, core_.start_stamp(), thread_index);
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
        if (!core_.started()) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        collection_id_t const collection_id = resolve_(id).id;
        if (collection_id == collection_invalid_id_k)
            return {status_code_t::not_found_k, "The store names no such collection"};

        transaction_stamp_t const at = core_.template read_stamp_for<watch_schedule_>();
        transaction_stamp_t const from = core_.template watch_stamp_for<watch_schedule_>();
        if (status_t const read = core_.store().select(collection_id, stored_key, into, size, at, thread_index);
            read.failed())
            return read;

        std::array<stored_key_t, 1> const alone {stored_key};
        std::span<stored_key_t const> const watched {alone};
        if (status_t const prepared = core_.template watch_now_or_prepare<watch_schedule_>(collection_id, watched);
            prepared.failed())
            return prepared;
        core_.template watch_later<watch_schedule_>(collection_id, watched, from, watch_purpose_t::read_k);
        return {};
    }

#pragma endregion Lookups

#pragma region Stamps

    /** The stamp this transaction was started or restarted at, which scopes every watch it takes. */
    [[nodiscard]] transaction_stamp_t start_stamp() const noexcept { return core_.start_stamp(); }

    /** The stamp this transaction last staged at, or @c invalid_transaction_stamp_k while it never has. */
    [[nodiscard]] transaction_stamp_t last_stage_stamp() const noexcept { return core_.last_stage_stamp(); }

    /** The stamp this transaction last committed at, or @c invalid_transaction_stamp_k while it never has. */
    [[nodiscard]] transaction_stamp_t last_commit_stamp() const noexcept { return core_.last_commit_stamp(); }

    /** The stamp this transaction last tried to commit at, whether or not that commit landed. */
    [[nodiscard]] transaction_stamp_t last_try_to_commit_stamp() const noexcept {
        return core_.last_try_to_commit_stamp();
    }

#pragma endregion Stamps

  private:

#pragma region Private Types

    /** The value widths one collection admits, which every write of it is checked against. */
    using value_widths_t = typename transaction_deltas_t::value_widths_t;

    /** What one lookup of a collection answered, which the next lookup of the same one reuses. */
    struct resolved_collection_t {

        /** The ID the collection answers to, invalid when the store names no such collection. */
        collection_id_t id = collection_invalid_id_k;

        /** The value widths that collection admits, which every write of it is checked against. */
        value_widths_t widths {};

        /** The version of the collections the answer was read at, which a create and a drop both move past. */
        std::uint64_t collections_version = 0;
    };

#pragma endregion Private Types

#pragma region Private Methods

    /**
     *  @brief What @p id resolves to, taken from the last answer while the collections have not moved.
     *  @param[in] id The collection to resolve, named by its ID or by its name.
     *  @return Its ID and its widths, the ID invalid when the store names no such collection.
     *
     *  @note One collection lookup answers both, and a run of calls over one collection takes one lookup
     *      rather than one per key; a create or a drop moves the version and the next call reads again.
     */
    [[nodiscard]] resolved_collection_t resolve_(any_collection_id_t const& id) noexcept {
        std::uint64_t const version = core_.store().collections_version();
        collection_id_t const* const identified = std::get_if<collection_id_t>(&id);
        if (identified != nullptr && resolved_.id != collection_invalid_id_k && *identified == resolved_.id &&
            version == resolved_.collections_version)
            return resolved_;

        collection_id_t collection_id = collection_invalid_id_k;
        collection_config_t shape;
        if (core_.store().resolve_collection(id, collection_id, shape).failed()) return resolved_collection_t {};
        resolved_.id = collection_id;
        resolved_.widths = value_widths_t {shape.lsm_tree.lsm_level_0.sorted_run.value_size,
                                           shape.lsm_tree.lsm_level_0.sorted_run.value_max_size};
        resolved_.collections_version = version;
        return resolved_;
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The writes, the watches, the stamps and the lifecycle, which the registry drives. */
    core_t core_;

    /** The collection the last lookup answered, which the next lookup of the same one reuses. */
    resolved_collection_t resolved_;

#pragma endregion Private Members
};

#pragma endregion Database Transaction

#pragma region Database

#pragma region Manifest Collections

/** Bytes the longest collection name takes. */
inline constexpr std::size_t collection_name_max_bytes_k = 64;

/** The bounds of the collection @p record describes, as the collection layer takes them. */
[[nodiscard]] inline collection_config_t config_of(manifest_collection_t const& record) noexcept {
    collection_config_t config;
    config.cache_limit = static_cast<std::size_t>(record.cache_limit);
    config.memory_runs_max_bytes = static_cast<std::size_t>(record.memory_runs_max_bytes);
    config.memory_runs_max_deltas = static_cast<std::size_t>(record.memory_runs_max_deltas);
    config.lsm_tree.level_enlarge_factor = static_cast<std::size_t>(record.level_enlarge_factor);
    config.lsm_tree.lsm_level_0.sorted_run.value_size = static_cast<stored_value_size_t>(record.value_size);
    config.lsm_tree.lsm_level_0.sorted_run.value_max_size = static_cast<stored_value_size_t>(record.value_max_size);
    std::memcpy(config.lsm_tree.collection.bytes, record.id.bytes, sizeof(record.id.bytes));
    return config;
}

#pragma endregion Manifest Collections

#pragma region Database

/** What a mount takes for a digester to check no run against the SHA-256 its manifest names. */
struct unverified_runs_t {};

/** What a published ingest answers: its stamp, or @c invalid_transaction_stamp_k when it never
 *  landed, beside the status of publishing it or, once landed, of the flush it asked for. */
struct ingest_outcome_t {
    transaction_stamp_t stamp = invalid_transaction_stamp_k;
    status_t status;
};

/**
 *  @brief Named collections of one database, the manifest that names them and the
 *      transactions they share.
 *  @tparam stored_key_type_ The key every collection of this database is addressed by.
 *  @tparam volume_type_ The volume every run of every collection lives on.
 *  @tparam row_kit_type_ The SmashTable kit the stores' memory runs, the transactions' hash tables
 *      and the collection indexes search with, which every store keyed by this database shares.
 *  @tparam store_tags_type_ Where among @p store_types_ the store a tag names sits, through a
 *      static @c index_of answering the count of @p store_types_ for a tag naming none.
 *  @tparam store_types_ The stores a collection may live in, of which a create and a mount take the
 *      one its tag names.
 *
 *  The volume and the channels are borrowed and must outlive the database, and so must the budget
 *  the collections charge their memory to. A database is pinned rather than movable, because its
 *  registry and every live transaction name it.
 */
template <typename stored_key_type_, typename volume_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, typename row_kit_type_, typename store_tags_type_, nvme_store_concept... store_types_>
class basic_database {
  public:

#pragma region Types

    /** The key every collection of this database is addressed by. */
    using stored_key_t = stored_key_type_;

    /** The volume every run of every collection lives on. */
    using volume_t = volume_type_;

    /** The transport those volumes move bytes through. */
    using disk_t = typename volume_t::disk_t;

    /** The queue pairs every call that moves bytes borrows one of. */
    using channel_pool_t = basic_io_channel_pool<disk_t, atomic_reference_, waiter_type_>;

    /** One borrowed queue pair, drained and given back when it dies. */
    using channel_guard_t = typename channel_pool_t::guard_t;

    /** A collection named by its ID or by its name, which is how every call addresses one. */
    using any_collection_id_t = std::variant<collection_id_t, collection_alphanumeric_id_t>;

    /** The transactions of this database, the order they commit in and the conflicts that stop them. */
    using registry_t =
        transaction_registry<stored_key_t, basic_database, atomic_reference_, waiter_type_, row_kit_type_>;

    /** One transaction over the collections of this database. */
    using transaction_t =
        database_transaction<stored_key_t, basic_database, atomic_reference_, waiter_type_, row_kit_type_>;

    /** A live transaction, owned by whoever asked for it and retired when it dies. */
    using transaction_unique_ptr_t = std::unique_ptr<transaction_t>;

    /** One pinned stamp, at which the whole database answers. */
    using snapshot_t = database_snapshot<basic_database>;

    /** One stamp a snapshot, a transaction, a scan or a change feed holds against the ledger. */
    using pin_t = stamp_pin<atomic_reference_, waiter_type_>;

    /** One change a feed of a collection hands out, which also marks where the feed stands. */
    using stored_change_t = stored_change<stored_key_t>;

    /** One collection, held by the store its tag names. */
    using any_collection_t = std::variant<store_types_...>;

    /** The runs of every collection held by one store, of which the database keeps one pool per
     *  store, made at that store's first collection. */
    using any_run_pool_t = std::variant<std::monostate, typename store_types_::run_pool_t...>;

    /** Runs one store's pool may grow to hold, the most a pool indexed by
     *  @c std::uint16_t admits. */
    static constexpr std::size_t run_pool_capacity_k = std::numeric_limits<std::uint16_t>::max() - 1;

    /** One collection of the database: what a collection is called, how it is shaped, and
     *  the collection itself. */
    struct collection_entry_t {

        /** The UUID the manifest, every commit and the C ABI address this collection by. */
        collection_id_t id = collection_invalid_id_k;

        /** The name a caller reaches this collection by. */
        string_t name;

        /** The tag of the store holding this collection, which its creator picked. */
        std::uint16_t store_tag = 0;

        /** The bounds this collection was mounted with. */
        collection_config_t config {};

        /** The collection itself, pinned here so a snapshot may name it. */
        any_collection_t held;

        /** The first stamp a reader sees this collection at: the one its create drew, or none
         *  for one a mount adopted, which every stamp sees. */
        transaction_stamp_t listed_stamp = unwritten_stamp_k;

        /** The first stamp a reader stops seeing it at: the stamp its unlisting drew, or none
         *  while it stays listed. */
        transaction_stamp_t unlisted_stamp = invalid_transaction_stamp_k;
    };

    /**
     *  @brief One collection read at one state for a whole call: the ledger's epochs entered once
     *      on the calling thread, the collection found once and the view of its store loaded once.
     *
     *  Every read through it walks that view with no pin, as the epochs keep the view and the runs
     *  it names alive while merges publish newer ones, and other threads of the call may read
     *  through it while it lives. Its thread holds the epochs until it dies, which every
     *  publication of a view waits out, so it lives through no write.
     */
    class collection_reading_t {
      public:
        collection_reading_t() noexcept = default;
        collection_reading_t(collection_reading_t const&) = delete;
        collection_reading_t& operator=(collection_reading_t const&) = delete;
        collection_reading_t(collection_reading_t&&) noexcept = default;
        collection_reading_t& operator=(collection_reading_t&&) noexcept = default;

        /** The collection read. */
        [[nodiscard]] collection_id_t id() const noexcept { return entry_->id; }

      private:
        friend class basic_database;

        /** The calling thread's announcement in the ledger's epochs, which keeps the entry and the
         *  view alive. */
        typename epoch_domain<atomic_reference_, waiter_type_>::reader_t inside_;

        /** The collection read. */
        collection_entry_t* entry_ = nullptr;

        /** The view of its store and the stamp every read answers at, at that store's index. */
        std::variant<typename store_types_::reading_t...> reading_;
    };

#pragma endregion Types

#pragma region Construction

    basic_database(basic_database const&) = delete;
    basic_database(basic_database&&) = delete;
    basic_database& operator=(basic_database const&) = delete;
    basic_database& operator=(basic_database&&) = delete;

    /** A database charging @p resources for every collection it opens, which @c mount then fills. */
    explicit basic_database(resources_shared_ptr<atomic_reference_, waiter_type_> resources) noexcept
        : resources_(std::move(resources)), transactions_(*this, resources_) {}

    ~basic_database() noexcept { [[maybe_unused]] status_t const closed = unmount(); }

    /**
     *  @brief Opens every collection the manifest @p volume read names, or starts
     *      an empty database.
     *  @param[in,out] volume The volume every run lands on, borrowed and outliving the database.
     *  @param[in,out] channels The queue pairs every call borrows one of, borrowed and outliving the database.
     *  @param[in] config The delta bounds a transaction takes when its caller names none.
     *  @param[in] compactor What every merge runs on: the calling thread, or a device compactor the caller owns, which
     *      outlives the database.
     *  @param[in] digester Steps a SHA-256 state, as @c save takes one, over every run the manifest
     *      names a SHA-256 for; @c unverified_runs_t checks none.
     *  @return Success, or @c malformed_k for a run whose bytes differ from the SHA-256 the
     *      manifest names, as a corrupted archive's do, found before any merge reads it.
     *
     *  Every manifest the database publishes afterwards names no SHA-256, so only the first open
     *  of a restored database hashes its runs; a save hashes them again as it copies them.
     */
    template <typename digester_type_ = unverified_runs_t>
    status_t mount(volume_t& volume, channel_pool_t& channels, database_config_t const& config,
                   compactor_ref_t compactor = {}, digester_type_ const& digester = {}) noexcept {
        if (volume_ != nullptr) return {status_code_t::already_exists_k, "This database has already mounted"};
        volume_ = &volume;
        channels_ = &channels;
        config_ = config;
        compactor_ = compactor;
        auto [entries, entries_status] = entries_t::make(1, entries_t::capacity_max_k,
                                                         catalog_allocator_t {resources_->memory_constraint});
        entries_ = std::move(entries);
        status_t const opened = entries_status != st::status_t::success_k
                                    ? status_t {status_code_t::out_of_memory_k, "No room for the collections"}
                                    : open_manifest_(digester);
        if (opened.failed()) {
            // A manifest this database could not open stays as it lies, never published again
            // without what it missed.
            collections_.clear();
            entries_ = entries_t {};
            run_pools_.clear();
            user_data_bytes_ = 0;
            volume_ = nullptr;
            channels_ = nullptr;
        }
        return opened;
    }

    /**
     *  @brief Flushes every collection, publishes the manifest and lets every collection go.
     *
     *  @note The volume and the channels are still needed here, so this runs before the mount
     *      underneath is torn down; the destructor calls it for a caller that did not.
     */
    status_t unmount() noexcept {
        if (volume_ == nullptr) return {};
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
                if (status_t const saved = publish_manifest_(claimed->channel()); saved.failed() && first.succeeded())
                    first = saved;
            }
            // The entries outlive the publication that stops naming them, which waits out every reader of the
            // collections it replaced.
            collections_.clear();
            [[maybe_unused]] status_t const emptied = publish_collections_();
            entries_ = entries_t {};
        }
        run_pools_.clear();
        user_data_bytes_ = 0;
        volume_ = nullptr;
        channels_ = nullptr;
        return first;
    }

#pragma endregion Construction

#pragma region Catalog

    /** Collections this database names right now. */
    [[nodiscard]] std::size_t size() noexcept {
        reader_t const inside = ledger_.epochs().enter();
        return collection_entries_().size();
    }

    /** Whether @p id names a collection of this database. */
    [[nodiscard]] bool has_collection(any_collection_id_t const& id) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        return entry_at_(id) != nullptr;
    }

    /** The ID @p id resolves to, or @c collection_invalid_id_k when it names nothing. */
    [[nodiscard]] collection_id_t collection_id(any_collection_id_t const& id) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        collection_entry_t const* const found = entry_at_(id);
        return found ? found->id : collection_invalid_id_k;
    }

    /** The name @p collection_id answers to, which is empty when it names nothing. */
    [[nodiscard]] string_t collection_name(collection_id_t collection_id) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        collection_entry_t const* const found = entry_with_id_(collection_id);
        string_t name;
        if (found) { [[maybe_unused]] sz::status_t const copied = name.assign(found->name.view()); }
        return name;
    }

    /** The bounds @p collection_id was created with, which a transaction checks every write against. */
    status_t collection_config(collection_id_t collection_id, collection_config_t& config) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        collection_entry_t const* const found = entry_with_id_(collection_id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        config = found->config;
        return {};
    }

    /** The store tag @p collection_id was created with, which a copy of it is created with. */
    status_t collection_store_tag(collection_id_t collection_id, std::uint16_t& store_tag) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        collection_entry_t const* const found = entry_with_id_(collection_id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        store_tag = found->store_tag;
        return {};
    }

    /** The store tag of @p collection_id as a reader pinned at @p stamp sees it, the head
     *  included, inside the epochs of @p thread_index, answering @c not_found_k for a collection
     *  not listed at that stamp. */
    status_t listed_store_tag(collection_id_t collection_id, transaction_stamp_t stamp, std::uint16_t& store_tag,
                              std::size_t thread_index = outside_executor_k) noexcept {
        reader_t const inside = ledger_.epochs().enter(thread_index);
        collection_entry_t const* const found = entry_with_id_(collection_id);
        if (!found) return {status_code_t::not_found_k, "The database lists no such collection at that stamp"};
        transaction_stamp_t const at = stamp == head_stamp_k ? ledger_.published() : stamp;
        transaction_stamp_t const listed = atomic_reference_<transaction_stamp_t const> {found->listed_stamp}.load(
            std::memory_order_acquire);
        transaction_stamp_t const unlisted = atomic_reference_<transaction_stamp_t const> {found->unlisted_stamp}.load(
            std::memory_order_acquire);
        if (at < listed || unlisted <= at)
            return {status_code_t::not_found_k, "The database lists no such collection at that stamp"};
        store_tag = found->store_tag;
        return {};
    }

    /** Unlists @p collection_id from the stamp a commit draws for it on, keeping it for the
     *  readers pinned below, answering @c not_found_k for a collection the database names not. */
    status_t unlist_collection(collection_id_t collection_id) noexcept {
        claim_guard_t const claim {*this};
        collection_entry_t* const found = entry_with_id_(collection_id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        stamp_next_commit_(found->unlisted_stamp);
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
        collection_entry_t const* const found = entry_at_(id);
        collection_id = found ? found->id : collection_invalid_id_k;
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        config = found->config;
        return {};
    }

    /** How many manifests this database has published, the one a mount read included. */
    [[nodiscard]] std::uint64_t manifest_generation() const noexcept { return volume_->generation(); }

    /** How many times the collections have been published, which a memo of a lookup is checked against. */
    [[nodiscard]] std::uint64_t collections_version() const noexcept {
        return atomic_reference_<std::uint64_t const> {collections_version_}.load(std::memory_order_acquire);
    }

    /** The name and the number of every collection, which is short of the full list only when memory runs out. */
    [[nodiscard]] st::vector<collection_ids_t> list_collections() noexcept {
        reader_t const inside = ledger_.epochs().enter();
        std::span<collection_entry_t* const> const entries = collection_entries_();
        st::vector<collection_ids_t> listed;
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
     *  @param[in] store_tag The tag of the store the collection lives in,
     *      which @c store_tags_type_ places.
     *  @param[in] config The memory-run bounds, the cache bound and the shape of the tree below.
     *  @param[in] flushed_with A collection whose memory runs the manifest naming the new one
     *      names too, as the caller wrote its own record of the collection there; an invalid
     *      ID flushes nothing.
     *
     *  @note @p store_tag names a store whose value layout fits @c config's value size, so a width of eight bytes is
     *      stored inside the key slots and a width of none is a collection of values that differ.
     */
    status_t create_collection(collection_id_t collection_id, string_view_t name, std::uint16_t store_tag,
                               collection_config_t const& config,
                               collection_id_t flushed_with = collection_invalid_id_k) noexcept {
        if (name.empty()) return status_t {status_code_t::invalid_argument_k, "A collection is named by a byte"};
        if (name.size() > collection_name_max_bytes_k)
            return status_t {status_code_t::invalid_argument_k, "A collection name is shorter than that"};
        if (collection_id == collection_invalid_id_k)
            return status_t {status_code_t::invalid_argument_k, "That ID names no collection"};
        if (store_tags_type_::index_of(store_tag) >= sizeof...(store_types_))
            return status_t {status_code_t::invalid_argument_k, "That tag names no store this database keeps"};
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        if (entry_named_(name) != nullptr || entry_with_id_(collection_id) != nullptr)
            return status_t {status_code_t::already_exists_k, "The database already names that collection"};

        collection_entry_t* entry = take_entry_();
        if (!entry) return status_t {status_code_t::out_of_memory_k, "No room for another collection"};
        entry->id = collection_id;
        entry->store_tag = store_tag;
        entry->config = config;
        std::memcpy(entry->config.lsm_tree.collection.bytes, collection_id.uuid.bytes,
                    sizeof(collection_id.uuid.bytes));
        status_t created;
        if (sz::failed(entry->name.assign(name)))
            created = status_t {status_code_t::out_of_memory_k, "No room for a collection's name"};
        else if (status_t const mounted = mount_collection_(*entry); mounted.failed()) created = mounted;
        else if (collections_.push_back(std::move(entry)) != st::status_t::success_k)
            created = status_t {status_code_t::out_of_memory_k, "No room for another collection"};
        else {
            // Hidden from every pinned stamp until the commit below draws its listing stamp.
            entry->listed_stamp = invalid_transaction_stamp_k;
            if (status_t const flushed = flush_into_publication_(claimed->channel(), flushed_with); flushed.failed())
                created = flushed;
            else if (status_t const saved = publish_manifest_(claimed->channel()); saved.failed()) created = saved;
            else created = publish_collections_();
            if (created.failed()) collections_.erase(collections_.size() - 1);
        }
        if (created.failed()) release_entry_(*entry);
        else stamp_next_commit_(entry->listed_stamp);
        return created;
    }

    /** Returns every run of @p id to its volume and forgets the name, which destroys the
     *  data; @p flushed_with names a collection whose memory runs the same manifest names, as
     *  the create does. */
    status_t drop_collection(any_collection_id_t const& id,
                             collection_id_t flushed_with = collection_invalid_id_k) noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        collection_entry_t* doomed = entry_at_(id);
        if (!doomed) return {status_code_t::not_found_k, "The database names no such collection"};
        if (status_t const flushed = flush_into_publication_(claimed->channel(), flushed_with); flushed.failed())
            return flushed;

        // The manifest stops naming the runs before a single one of them is handed back. The
        // publication takes the leader word, which keeps a staged or combining commit from
        // publishing into the collection meanwhile, and a request still pending finds it gone; it
        // returns once no reader is left in a view of the collection.
        std::size_t position = 0;
        while (collections_[position] != doomed) ++position;
        collections_.erase(position);
        status_t const publication = publish_collections_();
        if (publication.failed()) {
            [[maybe_unused]] st::status_t const returned = collections_.push_back(std::move(doomed));
            return publication;
        }
        if (status_t const saved = publish_manifest_(claimed->channel()); saved.failed()) {
            [[maybe_unused]] st::status_t const returned = collections_.push_back(std::move(doomed));
            [[maybe_unused]] status_t const restored = publish_collections_();
            return saved;
        }
        std::visit([](auto& held) noexcept { held.destroy(); }, doomed->held);
        release_entry_(*doomed);
        return {};
    }

#pragma endregion Catalog

#pragma region Shape

    /** Records the memory runs hold plus key slots every level of every collection holds. */
    [[nodiscard]] std::uint64_t population_count() noexcept {
        reader_t const inside = ledger_.epochs().enter();
        std::uint64_t counted = 0;
        for (collection_entry_t const* entry : collection_entries_())
            counted += std::visit([](auto const& collection) noexcept { return collection.population_count(); },
                                  entry->held);
        return counted;
    }

    /** Bytes every run of every collection takes on its volume. */
    [[nodiscard]] std::uint64_t size_bytes() noexcept {
        reader_t const inside = ledger_.epochs().enter();
        std::uint64_t counted = 0;
        for (collection_entry_t const* entry : collection_entries_())
            counted += std::visit([](auto const& collection) noexcept { return collection.size_bytes(); }, entry->held);
        return counted;
    }

    /** Bytes the largest value @p id admits takes, which is what a caller sizes a read buffer by. */
    [[nodiscard]] stored_value_size_t buffer_size_for_any_stored_value(any_collection_id_t const& id) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        collection_entry_t const* const found = entry_at_(id);
        if (!found) return 0;
        return std::visit([](auto const& collection) noexcept { return collection.value_max_size(); }, found->held);
    }

#pragma endregion Shape

#pragma region Lookups

    /**
     *  @brief Enters the ledger's epochs on executor thread @p thread_index, finds @p id and loads
     *      the view its store publishes for a read at @p stamp, which every read of one call then
     *      walks through the answer.
     *  @return @c not_found_k for a collection the database does not name.
     */
    expected<collection_reading_t> read_collection(any_collection_id_t const& id, transaction_stamp_t stamp,
                                                   std::size_t thread_index = outside_executor_k) noexcept {
        collection_reading_t read;
        read.inside_ = ledger_.epochs().enter(thread_index);
        read.entry_ = entry_at_(id);
        if (!read.entry_) return status_t {status_code_t::not_found_k, "The database names no such collection"};
        auto rung = [&]<std::size_t index_>(std::integral_constant<std::size_t, index_>) noexcept {
            if (read.entry_->held.index() == index_)
                read.reading_.template emplace<index_>(std::get<index_>(read.entry_->held).reading(stamp));
        };
        [&]<std::size_t... indices_>(std::index_sequence<indices_...>) noexcept {
            (rung(std::integral_constant<std::size_t, indices_> {}), ...);
        }(std::index_sequence_for<store_types_...> {});
        return read;
    }

    /** Calls @p visitor as <tt>visitor(store, reading)</tt> with the store @p read reads and the
     *  reading of it, answering what it answers. */
    template <typename visitor_type_>
    status_t visit_reading(collection_reading_t const& read, visitor_type_&& visitor) noexcept {
        status_t visited;
        auto rung = [&]<std::size_t index_>(std::integral_constant<std::size_t, index_>) noexcept {
            if (read.reading_.index() == index_)
                visited = visitor(std::get<index_>(read.entry_->held), std::get<index_>(read.reading_));
        };
        [&]<std::size_t... indices_>(std::index_sequence<indices_...>) noexcept {
            (rung(std::integral_constant<std::size_t, indices_> {}), ...);
        }(std::index_sequence_for<store_types_...> {});
        return visited;
    }

    /** What @p id carries for @p stored_key as a reader pinned at @p stamp sees it, and the bytes its value takes;
     *  a collection the database does not name carries nothing. Executor thread @p thread_index reads on its own
     *  epoch slot, as every lookup below does. */
    [[nodiscard]] lookup_result_t find(any_collection_id_t const& id, stored_key_t stored_key,
                                       stored_value_size_t& size, transaction_stamp_t stamp = head_stamp_k,
                                       std::size_t thread_index = outside_executor_k) noexcept {
        size = 0;
        auto [read, read_status] = read_collection(id, stamp, thread_index);
        if (read_status.failed()) return lookup_result_t::missing_k;
        return find(read, stored_key, size);
    }

    /** What the collection @p read reads carries for @p stored_key, and its value's bytes. */
    [[nodiscard]] lookup_result_t find(collection_reading_t const& read, stored_key_t stored_key,
                                       stored_value_size_t& size) noexcept {
        size = 0;
        lookup_result_t found = lookup_result_t::missing_k;
        [[maybe_unused]] status_t const visited = visit_reading(
            read,
            [&]<typename store_type_>(store_type_ const& collection,
                                      typename store_type_::reading_t const& reading) noexcept -> status_t {
                if constexpr (keyed_by_database<store_type_>()) found = collection.find(reading, stored_key, size);
                return {};
            });
        return found;
    }

    /** Whether @p id carries @p stored_key with a value rather than a remove delta, as a reader pinned at @p stamp
     *  sees it, a chain of changes always folding into one. */
    [[nodiscard]] bool contains(any_collection_id_t const& id, stored_key_t stored_key,
                                transaction_stamp_t stamp = head_stamp_k,
                                std::size_t thread_index = outside_executor_k) noexcept {
        stored_value_size_t size = 0;
        lookup_result_t const found = find(id, stored_key, size, stamp, thread_index);
        return found == lookup_result_t::written_k || found == lookup_result_t::incomplete_k;
    }

    /** Bytes the value of @p stored_key takes in @p id as a reader pinned at @p stamp sees it, or @c not_found_k for a
     *  miss and a removal; a chain of changes reaching values apart from their key slots is folded through a channel
     *  to be measured. */
    status_t stored_value_size(any_collection_id_t const& id, stored_key_t stored_key, stored_value_size_t& size,
                               transaction_stamp_t stamp = head_stamp_k,
                               std::size_t thread_index = outside_executor_k) noexcept {
        size = 0;
        auto [read, read_status] = read_collection(id, stamp, thread_index);
        if (read_status.failed()) return {status_code_t::not_found_k, "That collection misses it"};
        return stored_value_size(read, stored_key, size, thread_index);
    }

    /** Bytes the value of @p stored_key takes in the collection @p read reads, as @c
     *  stored_value_size above measures them, executor thread @p thread_index taking its own
     *  channel for a fold. */
    status_t stored_value_size(collection_reading_t const& read, stored_key_t stored_key, stored_value_size_t& size,
                               std::size_t thread_index = outside_executor_k) noexcept {
        lookup_result_t const found = find(read, stored_key, size);
        if (found == lookup_result_t::written_k) return {};
        if (found != lookup_result_t::incomplete_k) return {status_code_t::not_found_k, "That collection misses it"};
        std::uint64_t measured = 0;
        lookup_result_t folded = lookup_result_t::missing_k;
        auto const count = [&](std::size_t, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
            measured = std::max<std::uint64_t>(measured, offset + bytes.size());
        };
        utilities::local_arena_t scratch;
        if (status_t const selected = select(read, scratch.allocator().arena,
                                             std::span<stored_key_t const> {&stored_key, 1},
                                             std::span<lookup_result_t> {&folded, 1}, count, thread_index);
            selected.failed())
            return selected;
        if (folded != lookup_result_t::written_k) return {status_code_t::not_found_k, "That collection misses it"};
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
        auto [read, read_status] = read_collection(id, stamp, thread_index);
        if (read_status.failed()) return read_status;
        return select(read, stored_key, into, size, thread_index);
    }

    /** Copies the value the collection @p read reads holds for @p stored_key into @p into, as @c
     *  select above copies it, executor thread @p thread_index taking its own channel. */
    status_t select(collection_reading_t const& read, stored_key_t stored_key, std::span<byte_t> into,
                    stored_value_size_t& size, std::size_t thread_index = outside_executor_k) noexcept {
        size = 0;
        auto [channel, acquire_status] = channels_->acquire(thread_index);
        if (acquire_status.failed()) return acquire_status;
        return visit_reading(
            read,
            [&]<typename store_type_>(store_type_& collection,
                                      typename store_type_::reading_t const& reading) noexcept -> status_t {
                if constexpr (keyed_by_database<store_type_>())
                    return collection.select(channel.channel(), reading, stored_key, into, size);
                else return keyed_otherwise_();
            });
    }

    /**
     *  @brief Reads the values of @p stored_keys out of @p id, refilling the channel as completions retire.
     *  @param[in] id The collection to read from, named by its ID or by its name.
     *  @param[in] stored_keys The keys to read, in any order and with any number of misses among them.
     *  @param[out] results What every key answered, which separates a miss from a removal.
     *  @param[in] sink Called as @c sink(request, offset, bytes) for every key that locates a value.
     *  @param[in] stamp The stamp a reader is pinned at, which sees the newest version at or below it.
     *  @param[in] thread_index The executor thread reading, which takes its own channel and epoch slot.
     */
    template <typename sink_type_>
    status_t select(any_collection_id_t const& id, std::span<stored_key_t const> stored_keys,
                    std::span<lookup_result_t> results, sink_type_&& sink, transaction_stamp_t stamp = head_stamp_k,
                    std::size_t thread_index = outside_executor_k) noexcept {
        auto [read, read_status] = read_collection(id, stamp, thread_index);
        if (read_status.failed()) return read_status;
        utilities::local_arena_t scratch;
        return select(read, scratch.allocator().arena, stored_keys, results, sink, thread_index);
    }

    /** Reads the values of @p stored_keys out of the collection @p read reads, as the batched @c
     *  select above reads them, carving what it tracks from @p scratch, which the caller rewinds,
     *  executor thread @p thread_index taking its own channel. */
    template <typename sink_type_>
    status_t select(collection_reading_t const& read, ustore_arena_t scratch, std::span<stored_key_t const> stored_keys,
                    std::span<lookup_result_t> results, sink_type_&& sink,
                    std::size_t thread_index = outside_executor_k) noexcept {
        auto [channel, acquire_status] = channels_->acquire(thread_index);
        if (acquire_status.failed()) return acquire_status;
        return visit_reading(
            read,
            [&]<typename store_type_>(store_type_ const& collection,
                                      typename store_type_::reading_t const& reading) noexcept -> status_t {
                if constexpr (keyed_by_database<store_type_>())
                    return collection.select(channel.channel(), reading, scratch, stored_keys, results, sink);
                else return keyed_otherwise_();
            });
    }

    /**
     *  @brief Hands @p visitor every key of @p id from @p from up in ascending order, or down in
     *      descending order as @p direction says, memory runs included.
     *  @param[in] id The collection to walk, named by its ID or by its name.
     *  @param[in] scratch Where each page carves its run cursors, which the caller rewinds.
     *  @param[in] from The first key the walk reports, which need not be in the collection.
     *  @param[in] visitor Called as @c visitor(stored_key, state, size) for the newest version of every key at or
     *      below @p stamp, removals included, answering whether the walk carries on.
     *  @param[in] stamp The stamp a reader is pinned at, which sees the newest version at or below it.
     *  @param[in] thread_index The executor thread walking, which announces on its own epoch slot.
     *  @param[in] direction Whether the walk goes up or down, reading only the keys it hands out.
     *
     *  @note The walk reads one page of keys at a time inside the epochs, finding the collection again for each, and
     *      hands a page to @p visitor outside them, so a commit waits for one page at most; a collection dropped
     *      meanwhile ends the walk with @c not_found_k.
     */
    template <typename visitor_type_>
    status_t scan(any_collection_id_t const& id, ustore_arena_t scratch, stored_key_t from, visitor_type_&& visitor,
                  transaction_stamp_t stamp = head_stamp_k, std::size_t thread_index = outside_executor_k,
                  scan_direction_t direction = scan_direction_t::ascending_k) noexcept {
        // A head walk pins the stamp it starts at, so every page reads the same commits.
        stamp_pin<atomic_reference_, waiter_type_> pin;
        if (stamp == head_stamp_k) stamp = ledger_.pin_reader(pin);
        std::array<scan_entry<stored_key_t>, scan_page_keys<stored_key_t>()> page;
        for (;;) {
            std::size_t count = 0;
            bool follows = false;
            {
                reader_t const inside = ledger_.epochs().enter(thread_index);
                collection_entry_t const* const found = entry_at_(id);
                if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
                status_t const read = std::visit(
                    [&]<typename store_type_>(store_type_ const& collection) noexcept -> status_t {
                        if constexpr (keyed_by_database<store_type_>()) {
                            using scanner_t =
                                nvme_store_cursor<typename store_type_::key_slot_t, volume_t, atomic_reference_,
                                                  waiter_type_, typename store_type_::fold_t,
                                                  typename store_type_::device_fold_t, row_kit_type_>;
                            auto [scanner, over_status] = scanner_t::over(collection, scratch, from, stamp,
                                                                          thread_index, direction);
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
                if (entry.state == lookup_result_t::incomplete_k) {
                    status_t const measured = stored_value_size(id, entry.stored_key, entry.size, stamp, thread_index);
                    if (measured.failed() && !(measured == status_code_t::not_found_k)) return measured;
                    entry.state = measured.succeeded() ? lookup_result_t::written_k : lookup_result_t::removed_k;
                }
                if (!visitor(entry.stored_key, entry.state, entry.size)) return {};
            }
            if (!follows) return {};
        }
    }

    /** Hands @p visitor every key of the collection @p read reads from @p from on in @p direction,
     *  as @c scan above does, every page read through @p read inside the epochs it holds, so @p
     *  visitor runs inside them too and must not write; executor thread @p thread_index takes its
     *  own channel to fold a chain of changes. */
    template <typename visitor_type_>
    status_t scan(collection_reading_t const& read, ustore_arena_t scratch, stored_key_t from, visitor_type_&& visitor,
                  std::size_t thread_index = outside_executor_k,
                  scan_direction_t direction = scan_direction_t::ascending_k) noexcept {
        return visit_reading(
            read,
            [&]<typename store_type_>(store_type_ const& collection,
                                      typename store_type_::reading_t const& reading) noexcept -> status_t {
                if constexpr (keyed_by_database<store_type_>()) {
                    using scanner_t = nvme_store_cursor<typename store_type_::key_slot_t, volume_t, atomic_reference_,
                                                        waiter_type_, typename store_type_::fold_t,
                                                        typename store_type_::device_fold_t, row_kit_type_>;
                    auto [scanner, over_status] = scanner_t::over(collection, reading, scratch, from, direction);
                    if (over_status.failed()) return over_status;
                    while (!scanner.reached_end()) {
                        lookup_result_t state = scanner.state();
                        stored_value_size_t size = scanner.size();
                        if (state == lookup_result_t::incomplete_k) {
                            status_t const measured = stored_value_size(read, scanner.stored_key(), size, thread_index);
                            if (measured.failed() && !(measured == status_code_t::not_found_k)) return measured;
                            state = measured.succeeded() ? lookup_result_t::written_k : lookup_result_t::removed_k;
                        }
                        if (!visitor(scanner.stored_key(), state, size)) return {};
                        if (status_t const advanced = scanner.advance(); advanced.failed()) return advanced;
                    }
                    return {};
                }
                else return keyed_otherwise_();
            });
    }

    /**
     *  @brief Counts the runs of @p id a reader at @p stamp walks, for an unordered scan that walks each run on its
     *      own: every level's runs, then the memory runs oldest first.
     *  @note A transaction's own deltas are one more run, which @c database_transaction::for_each_uncommitted walks.
     */
    expected<counted_runs_t> runs_count(any_collection_id_t const& id, transaction_stamp_t stamp = head_stamp_k,
                                        std::size_t thread_index = outside_executor_k) noexcept {
        auto [read, read_status] = read_collection(id, stamp, thread_index);
        if (read_status.failed()) return read_status;
        return runs_count(read);
    }

    /** Counts the runs of the collection @p read reads, as @c runs_count above counts them, which
     *  no flush rearranges while @p read lives. */
    expected<counted_runs_t> runs_count(collection_reading_t const& read) noexcept {
        counted_runs_t counted;
        status_t const visited = visit_reading(
            read,
            [&]<typename store_type_>(store_type_ const& collection,
                                      typename store_type_::reading_t const& reading) noexcept -> status_t {
                if constexpr (keyed_by_database<store_type_>()) {
                    counted = collection.count_runs(reading);
                    return {};
                }
                else return keyed_otherwise_();
            });
        if (visited.failed()) return visited;
        return counted;
    }

    /**
     *  @brief Hands @p visitor the newest version at or below @p stamp of every key from @p from up
     *      in the run at @p run_index of @p counted, as @c runs_count counted the runs of @p id,
     *      consulting no other run, so a key another run removed or repeats is still handed out.
     *  @param[in] id The collection whose run is walked.
     *  @param[in] counted The runs as @c runs_count counted them, which the index names one of.
     *  @param[in] run_index Which of the counted runs to walk.
     *  @param[in] from The first key to hand out.
     *  @param[in] visitor Called as @c visitor(stored_key, state, size) inside the epochs, answering whether the walk
     *      carries on, so it must not block; a caller bounds one call's work by answering false and resuming past the
     *      last key.
     *  @param[in] stamp The stamp a reader is pinned at, which every run of one scan should share.
     *  @param[in] thread_index The executor thread reading, which announces on its own epoch slot.
     *  @retval contended_k A flush, a compaction or a merge rearranged the runs since they were counted, so the scan
     *      counts them again or walks in order.
     */
    template <typename visitor_type_>
    status_t scan_run(any_collection_id_t const& id, counted_runs_t const& counted, std::uint64_t run_index,
                      stored_key_t from, visitor_type_&& visitor, transaction_stamp_t stamp = head_stamp_k,
                      std::size_t thread_index = outside_executor_k) noexcept {
        auto [read, read_status] = read_collection(id, stamp, thread_index);
        if (read_status.failed()) return read_status;
        return scan_run(read, counted, run_index, from, visitor);
    }

    /** Hands @p visitor the keys of one run of the collection @p read reads, as @c scan_run above
     *  does. */
    template <typename visitor_type_>
    status_t scan_run(collection_reading_t const& read, counted_runs_t const& counted, std::uint64_t run_index,
                      stored_key_t from, visitor_type_&& visitor) noexcept {
        return visit_reading(
            read,
            [&]<typename store_type_>(store_type_ const& collection,
                                      typename store_type_::reading_t const& reading) noexcept -> status_t {
                if constexpr (keyed_by_database<store_type_>())
                    return collection.scan_run(reading, counted, run_index, from, visitor);
                else return keyed_otherwise_();
            });
    }

    /**
     *  @brief Pins @p pin at @p stamp for a feed of the changes committed to @p id after it, so no
     *      merge of that collection folds a version above it until the pin is released.
     *
     *  @par Statuses:
     *  - @c write_conflict_k A merge of @p id may have folded a version above @p stamp already.
     *  - @c unsupported_k The collection is keyed otherwise, by whole strings.
     */
    status_t pin_feed(any_collection_id_t const& id, pin_t& pin, transaction_stamp_t stamp) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        collection_entry_t* const found = entry_at_(id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        return std::visit(
            [&]<typename store_type_>(store_type_& collection) noexcept -> status_t {
                if constexpr (keyed_by_database<store_type_>()) return collection.pin_feed(pin, stamp);
                else return {status_code_t::unsupported_k, "A collection keyed by strings feeds no changes"};
            },
            found->held);
    }

    /**
     *  @brief Hands @p visitor up to @p wanted changes committed to @p id after @p after, oldest
     *      first and a commit's by key, as @c nvme_store::changes_after does, the version a merge
     *      kept for a pinned feed included.
     *  @param[in] visitor Called as <tt>visitor(change, value_bytes, read)</tt> inside the epochs, so it
     *      must not block.
     */
    template <typename visitor_type_>
    status_t changes_after(any_collection_id_t const& id, stored_change_t const& after, std::size_t wanted,
                           visitor_type_&& visitor, std::size_t thread_index = outside_executor_k) noexcept {
        reader_t const inside = ledger_.epochs().enter(thread_index);
        collection_entry_t const* const found = entry_at_(id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        auto [channel, acquire_status] = channels_->acquire(thread_index);
        if (acquire_status.failed()) return acquire_status;
        return std::visit(
            [&]<typename store_type_>(store_type_ const& collection) noexcept -> status_t {
                if constexpr (keyed_by_database<store_type_>())
                    return collection.changes_after(channel.channel(), after, wanted, visitor, thread_index);
                else return {status_code_t::unsupported_k, "A collection keyed by strings feeds no changes"};
            },
            found->held);
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
            return collection.seal_writes(stored_keys, stored_values, sizes);
        });
    }

    /** Removes every key of @p stored_keys in @p id, as one commit of its own. */
    status_t remove(any_collection_id_t const& id, std::span<stored_key_t const> stored_keys) noexcept {
        return commit_alone_(id,
                             [&](auto const& collection) noexcept { return collection.seal_removals(stored_keys); });
    }

#pragma endregion Writes

#pragma region Maintenance

    /** Writes the memory runs of every collection as one run of its level zero. */
    status_t flush() noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        return flush_holding_(claimed->channel());
    }

    /** An ingest under way into one collection: the claim it holds until destroyed, and the rows and runs so far. */
    class ingest_t;

    /**
     *  @brief Opens @p ingest into @p id, taking the claim it holds until it is destroyed, which keeps the collection
     *      from being dropped beneath its runs.
     *  @return @c not_found_k for no such collection, or success.
     */
    status_t begin_ingest(any_collection_id_t const& id, ingest_t& ingest) noexcept {
        claim_guard_t claim {*this};
        collection_entry_t* const found = entry_at_(id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        ingest.claim_ = std::move(claim);
        ingest.entry_ = found;
        auto rung = [&]<std::size_t index_>(std::integral_constant<std::size_t, index_>) noexcept {
            any_collection_t& held = found->held;
            if (held.index() == index_) ingest.runs_.template emplace<index_>(std::get<index_>(held).begin_ingest());
        };
        [&]<std::size_t... indices_>(std::index_sequence<indices_...>) noexcept {
            (rung(std::integral_constant<std::size_t, indices_> {}), ...);
        }(std::index_sequence_for<store_types_...> {});
        return {};
    }

    /**
     *  @brief Lays @p rows out behind the rows @p ingest laid out before, outside the memory runs, writing a run each
     *      time one fills, so appends of any size land in runs of a flush's size.
     *  @param[in] rows Rows ascending past every row before them, readable until the call returns.
     *  @return @c unsupported_k for a store writing no such rows, or the status that stopped it with @p ingest as it
     *      was.
     */
    template <typename row_type_>
    status_t write_ingest(ingest_t& ingest, std::span<row_type_ const> rows) noexcept {
        auto [guard, acquire_status] = channels_->acquire();
        if (acquire_status.failed()) return acquire_status;
        io_channel_t& channel = guard.channel();
        return with_ingest_<status_t>(
            ingest, [&]<typename held_type_>(held_type_& held, typename held_type_::ingest_runs_t& ingested) noexcept {
                if constexpr (requires { held.write_ingest(channel, rows, ingested); })
                    return held.write_ingest(channel, rows, ingested);
                else return status_t {status_code_t::unsupported_k, "This collection's store ingests no such rows"};
            });
    }

    /**
     *  @brief Publishes every run of @p ingest as one commit of its own above every level holding their keys, and
     *      answers its stamp.
     *  @param[in] durability Flushed for an ingest durable on return, flushing every collection and
     *      publishing the manifest, or buffered for one the next manifest names and a crash before
     *      it drops whole.
     *  @return The stamp beside the status of the flush @p durability asked for, or no stamp
     *      beside the status that stopped it with every run dropped, with the claim given back.
     */
    ingest_outcome_t publish_ingest(ingest_t& ingest, ustore_durability_t durability) noexcept {
        // The channel goes back before the claim, as every claimed call gives them back
        ingest_outcome_t const landed = [&]() noexcept -> ingest_outcome_t {
            auto [guard, acquire_status] = channels_->acquire();
            if (acquire_status.failed()) return ingest_outcome_t {invalid_transaction_stamp_k, acquire_status};
            io_channel_t& channel = guard.channel();
            expected<transaction_stamp_t> const published = with_ingest_<expected<transaction_stamp_t>>(
                ingest,
                [&]<typename held_type_>(held_type_& held, typename held_type_::ingest_runs_t& ingested) noexcept {
                    return held.publish_ingest(channel, ingested);
                });
            // The stamp is final once published, so a failed flush is retried as a flush
            if (!published) return ingest_outcome_t {invalid_transaction_stamp_k, published.status()};
            if (durability != ustore_durability_flushed_k) return ingest_outcome_t {*published, {}};
            return ingest_outcome_t {*published, flush_holding_(channel)};
        }();
        ingest.claim_ = claim_guard_t {};
        return landed;
    }

    /** Drops every run @p ingest wrote and never published, under the claim, which it takes again once given back. */
    void drop_ingest(ingest_t& ingest) noexcept {
        if (std::visit([](auto const& ingested) noexcept { return ingested.runs.empty(); }, ingest.runs_)) return;
        if (!ingest.claim_.holding()) ingest.claim_ = claim_guard_t {*this};
        [[maybe_unused]] status_t const dropped = with_ingest_<status_t>(
            ingest, [&]<typename held_type_>(held_type_& held, typename held_type_::ingest_runs_t& ingested) noexcept {
                held.drop_ingest(ingested);
                return status_t {};
            });
    }

    /** Whether any collection has a level over its bounds, which @c compact then merges. */
    [[nodiscard]] bool needs_compaction() noexcept {
        reader_t const inside = ledger_.epochs().enter();
        for (collection_entry_t const* entry : collection_entries_())
            if (std::visit([](auto const& collection) noexcept { return collection.needs_compaction(); }, entry->held))
                return true;
        return false;
    }

    /** Merges one overfull level of every collection that has one, which needs two channels at
     *  once, fanning each merge on the host out over @p fan_out. */
    status_t compact(compaction_fan_out_t const& fan_out = compaction_fan_out_t {}) noexcept {
        // The claim the channel comes with lends the database's one set of merge buffers.
        expected<claimed_channel_t> reading = claim_channel_();
        if (!reading) return reading.status();
        auto [writing, acquire_status] = channels_->acquire();
        if (acquire_status.failed()) return acquire_status;

        manifest_write_t recording {this, &reading->channel()};
        lsm_tree_publication_hook_t published;
        published.notify = &record_manifest_;
        published.context = &recording;

        status_t first;
        for (std::size_t index = 0; index != collections_.size(); ++index) {
            auto [merged, compact_status] = std::visit(
                [&]<typename store_type_>(store_type_& collection) noexcept -> expected<compaction_outcome_t> {
                    auto [buffers, lent_status] = lent_merge_buffers_<store_type_>(fan_out.slices());
                    if (lent_status.failed()) return lent_status;
                    return collection.compact(reading->channel(), writing.channel(), buffers, compactor_, published,
                                              fan_out);
                },
                collections_[index]->held);
            if (compact_status.failed() && first.succeeded()) first = compact_status;
        }
        return first;
    }

    /**
     *  @brief Once @p collection_id is due, merges its memory runs by tier, and flushes and compacts them once
     *      they hold their bounds, which every commit runs for the collections it wrote once it lands.
     *
     *  Returns at once while another caller holds the claim, since that holder looks at every collection again after
     *  it lets go, so no commit waits behind another's maintenance.
     *  @note Called outside the ledger's epochs.
     */
    status_t maintain(collection_id_t collection_id) noexcept {
        {
            reader_t const inside = ledger_.epochs().enter();
            collection_entry_t const* const found = entry_with_id_(collection_id);
            if (!found ||
                !std::visit([](auto const& collection) noexcept { return collection.maintenance_due(); }, found->held))
                return {};
        }
        claim_guard_t const held = claim_guard_t::taken_if_free(*this);
        if (!held.holding()) return {};
        collection_entry_t* const found = entry_with_id_(collection_id);
        if (!found) return {};
        return maintain_collection_(*found);
    }

    /** How many memory runs @p id's collection holds and how many commits they hold, or none for a missing one. */
    [[nodiscard]] memory_shape_t memory_shape(any_collection_id_t const& id) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        collection_entry_t const* const found = entry_at_(id);
        if (!found) return {};
        return std::visit([](auto const& collection) noexcept { return collection.memory_shape(); }, found->held);
    }

    /** Pins the newest published stamp, at which every read through the snapshot answers, and flushes nothing. */
    expected<snapshot_t> pin_stamp() noexcept { return snapshot_t::of(*this); }

#pragma endregion Maintenance

#pragma region Transactions

    /** A transaction over every collection, with the delta bounds of @p config. */
    expected<transaction_unique_ptr_t> create_transaction(transaction_config_t const& config) noexcept {
        return transactions_.template create_transaction<transaction_t>(config);
    }

    /** A transaction with the delta bounds this database was mounted with. */
    expected<transaction_unique_ptr_t> create_transaction() noexcept {
        return create_transaction(config_.default_transaction);
    }

    /** The order every commit of this database draws its stamp from, and the stamps its readers pin. */
    [[nodiscard]] commit_ledger<atomic_reference_, waiter_type_>& ledger() noexcept { return ledger_; }

#pragma endregion Transactions

#pragma region Write Path

    /** Whether a commit newer than @p from wrote @p stored_key of @p collection_id, read from the published view; a
     *  collection gone since counts as written. */
    [[nodiscard]] bool written_since(collection_id_t collection_id, stored_key_t stored_key,
                                     transaction_stamp_t from) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        collection_entry_t const* const found = entry_with_id_(collection_id);
        if (!found) return true;
        return std::visit(
            [&]<typename store_type_>(store_type_ const& collection) noexcept {
                if constexpr (keyed_by_database<store_type_>()) return collection.written_since(stored_key, from);
                else return true;
            },
            found->held);
    }

    /** Whether a commit newer than @p from removed @p stored_key of @p collection_id or wrote it
     *  whole, read from the published view; a collection gone since counts as replaced. */
    [[nodiscard]] bool replaced_since(collection_id_t collection_id, stored_key_t stored_key,
                                      transaction_stamp_t from) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        collection_entry_t const* const found = entry_with_id_(collection_id);
        if (!found) return true;
        return std::visit(
            [&]<typename store_type_>(store_type_ const& collection) noexcept {
                if constexpr (keyed_by_database<store_type_>()) return collection.replaced_since(stored_key, from);
                else return true;
            },
            found->held);
    }

    /** Whether a commit newer than @p from wrote any key of @p collection_id from @p lower to @p upper inclusive,
     *  read from the published view; a collection gone since counts as written. */
    [[nodiscard]] bool range_written_since(collection_id_t collection_id, stored_key_t lower, stored_key_t upper,
                                           transaction_stamp_t from) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        collection_entry_t const* const found = entry_with_id_(collection_id);
        if (!found) return true;
        return std::visit(
            [&]<typename store_type_>(store_type_ const& collection) noexcept {
                if constexpr (keyed_by_database<store_type_>())
                    return collection.range_written_since(lower, upper, from);
                else return true;
            },
            found->held);
    }

    /**
     *  @brief Seals what @p written holds for @p collection_id into @p sealed on the committing thread, making room
     *      first when the collection's memory runs are past their bounds.
     *  @return @c write_conflict_k for a collection gone since the transaction wrote it.
     */
    status_t seal_collection(
        collection_id_t collection_id,
        transaction_deltas<stored_key_t, atomic_reference_, waiter_type_, row_kit_type_> const& written,
        sealed_commit<stored_key_t, atomic_reference_, row_kit_type_>& sealed) noexcept {
        st::vector<keyed_delta<stored_key_t>, constrained_allocator<keyed_delta<stored_key_t>, atomic_reference_>>
            deltas {
                constrained_allocator<keyed_delta<stored_key_t>, atomic_reference_> {resources_->memory_constraint}};
        if (deltas.reserve(written.count(collection_id)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to hand a commit over"};
        written.for_each_delta(collection_id, [&](stored_key_t stored_key, delta_view_t delta) noexcept {
            [[maybe_unused]] st::status_t const placed = deltas.push_back(
                st::assume_reserved, keyed_delta<stored_key_t> {stored_key, delta});
        });
        return seal_into_(collection_id, sealed, [&](auto const& collection) noexcept {
            return collection.seal(std::span<keyed_delta<stored_key_t>> {deltas}, written.ranges(collection_id),
                                   written.lendable_bytes());
        });
    }

    /**
     *  @brief Lays out the view @p sealed's run is published in, out of the room @p claimed holds,
     *      on the thread holding the leader word.
     *  @return @c write_conflict_k for a collection gone since the transaction wrote it.
     */
    status_t prepare_collection(collection_id_t collection_id,
                                sealed_commit<stored_key_t, atomic_reference_, row_kit_type_>& sealed,
                                memory_run_claim_t&& claimed) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        collection_entry_t* const found = entry_with_id_(collection_id);
        if (!found) return {status_code_t::write_conflict_k, "A collection the transaction wrote is gone"};
        return std::visit(
            [&]<typename store_type_>(store_type_& collection) noexcept -> status_t {
                if constexpr (keyed_by_database<store_type_>()) {
                    collection.prepare(sealed, std::move(claimed));
                    return {};
                }
                else return keyed_otherwise_();
            },
            found->held);
    }

    /** Publishes @p sealed under @p stamp in the view @c prepare_collection laid out, before the stamp itself is,
     *  on the thread holding the leader word, which a drop waits for. */
    void publish_collection(collection_id_t collection_id,
                            sealed_commit<stored_key_t, atomic_reference_, row_kit_type_>& sealed,
                            transaction_stamp_t stamp) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        collection_entry_t* const found = entry_with_id_(collection_id);
        std::visit(
            [&]<typename store_type_>(store_type_& collection) noexcept {
                if constexpr (keyed_by_database<store_type_>()) collection.publish(sealed, stamp);
            },
            found->held);
    }

    /** Forgets the view @c prepare_collection laid out for a commit the leader refuses. */
    void forget_collection(collection_id_t collection_id) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        if (collection_entry_t* const found = entry_with_id_(collection_id))
            std::visit([](auto& collection) noexcept { collection.forget_prepared(); }, found->held);
    }

    /** Calls @p visitor with the store @p id's collection lives in, inside the ledger's epochs,
     *  which keep the store alive until it returns, and answers what it answers, or @c not_found_k
     *  for a collection the database names not; @p visitor takes every store this database holds
     *  and answers a status. */
    template <typename visitor_type_>
    status_t visit_store(any_collection_id_t const& id, visitor_type_&& visitor,
                         std::size_t thread_index = outside_executor_k) noexcept {
        reader_t const inside = ledger_.epochs().enter(thread_index);
        collection_entry_t* const found = entry_at_(id);
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

    /**
     *  @brief Claims a memory-run record of @p collection_id's store for a commit sealed for it,
     *      before the commit takes the leader word: a free one, else the flush of the collection's
     *      memory runs while nobody holds the claim, else one vacated while this waits.
     *  @return @c write_conflict_k for a collection gone since it was written, or why a flush or
     *      the view failed.
     *  @note Called outside the ledger's epochs, which every flush waits out.
     */
    status_t claim_memory_record(collection_id_t collection_id, memory_run_claim_t& claimed) noexcept {
        for (;;) {
            std::uint32_t const* vacated = nullptr;
            std::uint32_t observed = 0;
            {
                reader_t const inside = ledger_.epochs().enter();
                collection_entry_t* const found = entry_with_id_(collection_id);
                if (!found) return {status_code_t::write_conflict_k, "A collection the transaction wrote is gone"};
                status_t const taken = std::visit(
                    [&](auto& collection) noexcept -> status_t {
                        vacated = &collection.vacated();
                        observed = atomic_reference_<std::uint32_t const> {*vacated}.load(std::memory_order_acquire);
                        return collection.try_claim(claimed);
                    },
                    found->held);
                if (taken.succeeded()) {
                    claimed.owner = this;
                    claimed.give_back = &give_back_memory_record_;
                    claimed.collection = collection_id;
                    return {};
                }
                if (taken != status_code_t::capacity_exhausted_k) return taken;
            }
            // Only a vacate frees a record, so the claim is retried once the word moves rather
            // than under the lock the vacate takes. Entries never move, so the word stays
            // readable if a drop destroys the store.
            atomic_reference_<std::uint32_t const> const vacated_now {*vacated};
            while (vacated_now.load(std::memory_order_acquire) == observed) {
                bool flushable = false;
                {
                    reader_t const inside = ledger_.epochs().enter();
                    collection_entry_t* const found = entry_with_id_(collection_id);
                    if (!found) return {status_code_t::write_conflict_k, "A collection the transaction wrote is gone"};
                    flushable = std::visit(
                        [](auto& collection) noexcept { return collection.memory_shape().runs != 0; }, found->held);
                }
                // A flush frees every published run's record; with none published, every record
                // is out with a commit.
                if (flushable) {
                    claim_guard_t const held = claim_guard_t::taken_if_free(*this);
                    if (held.holding()) {
                        collection_entry_t* const claimed_entry = entry_with_id_(collection_id);
                        if (!claimed_entry)
                            return {status_code_t::write_conflict_k, "A collection the transaction wrote is gone"};
                        if (status_t const flushed = std::visit(
                                [&](auto& collection) noexcept { return flush_and_compact_(collection); },
                                claimed_entry->held);
                            flushed.failed())
                            return flushed;
                        break;
                    }
                }
                waiter_type_ {}(vacated, observed, std::size_t {0});
            }
        }
    }

#pragma endregion Write Path

#pragma region User Data

    /** Bytes the caller's own data takes, which is zero when it has saved none. */
    [[nodiscard]] std::uint64_t user_data_size_bytes() noexcept {
        claim_guard_t const claim {*this};
        return user_data_bytes_;
    }

    /** Replaces the caller's own data with @p bytes, which the engine never reads, as a manifest
     *  entry of no collection; none clears it. */
    status_t save_user_data(bytes_view_t bytes) noexcept {
        if (bytes.empty()) return clear_user_data();
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        uuid_t const run = uuid_generator();
        auto [written, allocate_status] = volume_->allocate(run, bytes.size());
        if (allocate_status.failed()) return allocate_status;
        if (status_t const filled = write_extent<disk_t, waiter_type_>(volume_->disk(), claimed->channel(), written,
                                                                       bytes);
            filled.failed()) {
            volume_->deallocate(written);
            return filled;
        }
        return replace_user_data_(claimed->channel(), run, written, bytes.size());
    }

    /** Reads the caller's own data into @p into, which must hold @c user_data_size_bytes of it. */
    status_t load_user_data(bytes_span_t into) noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        if (user_data_bytes_ == 0) return {status_code_t::not_found_k, "The database holds no user data"};
        if (into.size() < user_data_bytes_)
            return {status_code_t::invalid_argument_k, "A buffer shorter than the data"};
        return read_extent<disk_t>(volume_->disk(), claimed->channel(), user_data_,
                                   into.first(static_cast<std::size_t>(user_data_bytes_)));
    }

    /** Forgets the caller's own data and returns its bytes to the volume once the manifest
     *  stops naming it. */
    status_t clear_user_data() noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        if (user_data_bytes_ == 0) return {status_code_t::not_found_k, "The database holds no user data"};
        return replace_user_data_(claimed->channel(), uuid_t {}, medium_extent_t {}, 0);
    }

#pragma endregion User Data

#pragma region Archive

    /** What one save placed in its target, run by run. */
    struct archive_report_t {

        /** Runs the target already held at the same length, kept as they lie. */
        std::size_t kept = 0;

        /** Runs hard-linked or copied whole as files, from one filesystem to another. */
        std::size_t linked = 0;

        /** Runs read through a channel into fresh files or slots of the target. */
        std::size_t written = 0;
    };

    /**
     *  @brief Saves every collection as of @p stamp into the database @p target names, a manifest
     *      path or a raw address, writing the target's manifest last with @p stamp as its durable
     *      stamp, so a mount of it drops every version above.
     *  @param[in] target The target's data directories, its first holding the manifest, all
     *      of one kind.
     *  @param[in] stamp A stamp a pinned snapshot holds, whose versions every merge keeps.
     *  @param[in] progress Called as <tt>progress(placed_bytes, total_bytes)</tt> after every run;
     *      @c false cancels the save, which gives back every run it placed and publishes nothing.
     *  @param[in] digester Steps a SHA-256 state, as <tt>init(state)</tt>, <tt>update(state,
     *      bytes)</tt> and <tt>digest(state)</tt>, over every run's bytes.
     *  @return What the save placed, or @c cancelled_k, or the status that stopped it, with the
     *      target as it was.
     *
     *  Runs the target holds already under the same ID and length are kept, so a second save of a
     *  database writes only the runs written since; runs the target's previous manifest named and
     *  this one does not are given back after it. The target's manifest names the SHA-256 of
     *  every run, hashed as it is copied, or read back once it is linked or kept.
     */
    template <typename progress_type_, typename digester_type_>
    expected<archive_report_t> save(volume_config_t const& target, transaction_stamp_t stamp, progress_type_&& progress,
                                    digester_type_ const& digester) noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        io_channel_t& channel = claimed->channel();
        auto [writing, acquire_status] = channels_->acquire();
        if (acquire_status.failed()) return acquire_status;
        // Memory runs reach the runs first, so the files below hold every version at or
        // below the stamp.
        if (status_t const flushed = flush_holding_(channel); flushed.failed()) return flushed;
        st::vector<placed_file_t> sources;
        if (status_t const gathered = gather_files_(sources); gathered.failed()) return gathered;

        volume_config_t config = target;
        config.runs_capacity = volume_->runs_capacity();
        if (config.database.empty()) config.database = volume_->manifest().database.view();
        auto [into, mount_status] = volume_t::mount(
            volume_->disk(), channel, config,
            constrained_allocator<byte_t, atomic_reference_> {resources_->memory_constraint});
        if (mount_status.failed()) return mount_status;
        manifest_t const& held = into.manifest();
        st::vector<manifest_file_t const*> held_by_run;
        if (held_by_run.reserve(held.files.size()) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room to index the target's runs"};
        for (manifest_file_t const& file : held.files) [[maybe_unused]]
            st::status_t const indexed = held_by_run.push_back(st::assume_reserved, &file);
        utilities::radix_sort(
            std::span<manifest_file_t const*> {held_by_run}, uuid_t::size_bytes_k,
            [](manifest_file_t const* file, std::size_t digit) noexcept -> std::size_t {
                return file->run.bytes[digit];
            },
            [](manifest_file_t const* left, manifest_file_t const* right) noexcept { return left->run < right->run; });

        manifest_t manifest;
        if (status_t const laid = lay_out_collections_(held, manifest); laid.failed()) return laid;
        manifest.generation = into.generation() + 1;
        manifest.durable_stamp = stamp;
        manifest.placement = into.placement();
        st::vector<medium_extent_t> placed;
        if (manifest.files.reserve(sources.size()) != st::status_t::success_k ||
            placed.reserve(sources.size()) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room to lay the target's manifest out"};

        std::uint64_t total_bytes = 0, placed_bytes = 0;
        for (placed_file_t const& source : sources) total_bytes += source.extent.extent.length;
        archive_report_t report;
        status_t failure;
        for (placed_file_t const& source : sources) {
            sha256_t digest {};
            auto [extent, place_status] = place_run_(into, channel, writing.channel(), source, held_by_run, report,
                                                     placed, digester, digest);
            if (place_status.failed()) {
                failure = place_status;
                break;
            }
            manifest_file_t file = source.file;
            file.offset = extent.extent.offset;
            file.medium = extent.data_directory;
            file.sha256 = digest;
            [[maybe_unused]] st::status_t const named = manifest.files.push_back(st::assume_reserved, std::move(file));
            placed_bytes += source.extent.extent.length;
            if (!progress(placed_bytes, total_bytes)) {
                failure = {status_code_t::cancelled_k, "The progress callback cancelled the save"};
                break;
            }
        }
        st::vector<char> json;
        if (failure.succeeded())
            failure = manifest.print(constrained_allocator<byte_t, atomic_reference_> {resources_->memory_constraint},
                                     json);
        if (failure.failed()) {
            for (medium_extent_t const& extent : placed) into.deallocate(extent);
            return failure;
        }
        if (status_t const published = into.publish_manifest(channel, std::span<char const> {json});
            published.failed()) {
            // A publication failing past its rename leaves a manifest on disk naming these, so none
            // goes back yet.
            for (medium_extent_t const& extent : placed) into.deallocate_after_publication(extent);
            return published;
        }

        // The target gives back every run the previous manifest named and this one does not.
        for (manifest_file_t const& old : held.files) {
            bool kept = false;
            for (manifest_file_t const& file : manifest.files) kept = kept || file.run == old.run;
            if (kept) continue;
            if (auto [extent, open_status] = into.open(old); open_status.succeeded()) into.deallocate(extent);
        }
        return report;
    }

#pragma endregion Archive

  private:

#pragma region Private Types

    /** What every collection entry, the pools and the indexes over them are charged to. */
    using catalog_allocator_t = constrained_allocator<byte_t, atomic_reference_>;

    /** Where every collection entry lives, in records that never move, so a snapshot and a live
     *  transaction may point into one while a create grows the pool beside them. */
    using entries_t = st::basic_growing_pool<collection_entry_t, std::uint32_t, catalog_allocator_t>;

    /** The entries of the published collections by ID, their UUID bytes taken as the key. */
    using ids_index_t =
        st::basic_hash_index<st::key128_t, std::uint32_t, st::default_hash_t, catalog_allocator_t, row_kit_type_>;

    /** The entries of the published collections by a hash of their name, which a lookup confirms
     *  against the name. */
    using names_index_t =
        st::basic_hash_index<std::uint64_t, std::uint32_t, st::default_hash_t, catalog_allocator_t, row_kit_type_>;

    /**
     *  @brief One published list of collections, naming every entry the database held when it was laid out.
     *
     *  A lookup that reads nothing but a name, an ID or a shape needs no claim: every entry is
     *  pinned in @c entries_, so a create and a drop publish only a new list and new indexes naming
     *  them, and free the ones they replace once every reader that loaded them has left.
     */
    struct published_collections_t {

        /** The entries, in the order @c collections_ held them. */
        st::vector<collection_entry_t*> entries;

        /** The index into @c entries_ of every entry, by its ID. */
        ids_index_t by_id;

        /** The index into @c entries_ of every entry, by a hash of its name. */
        names_index_t by_name;

        /** Its room in the ledger's list of what the leader unpublished, once a publication replaces it. */
        unpublished_t unpublished;
    };

    /** One reader inside the ledger's epochs, announced until it dies. */
    using reader_t = typename epoch_domain<atomic_reference_, waiter_type_>::reader_t;

    /** The database's one claim, held for the life of the guard: it orders whatever rewrites the
     *  manifest, a tree or the user data, no read takes it, and a commit takes it once it landed or
     *  returns at once when another caller has it. */
    using claim_guard_t = typename maintenance_claim<atomic_reference_, waiter_type_>::template guard<basic_database>;

    friend claim_guard_t;

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

  public:
    class ingest_t {
      public:
        ingest_t() noexcept = default;
        ingest_t(ingest_t const&) = delete;
        ingest_t& operator=(ingest_t const&) = delete;

      private:
        friend class basic_database;

        /** The database's claim, held from @c begin_ingest until this is destroyed. */
        claim_guard_t claim_;

        /** The collection, which no drop releases while the claim is held. */
        collection_entry_t* entry_ = nullptr;

        /** The rows laid out and the runs written so far, of the store the collection is held in. */
        std::variant<typename store_types_::ingest_runs_t...> runs_;
    };

  private:
#pragma endregion Private Types

#pragma region Private Methods

    /** Whether @p store_type_ keeps its collections under this database's key, so the calls naming one reach it. */
    template <typename store_type_>
    static constexpr bool keyed_by_database() noexcept {
        return std::is_same_v<typename store_type_::stored_key_t, stored_key_t>;
    }

    /** What a call naming a key of this database answers for a collection keyed otherwise, which only the calls of
     *  its own store reach. */
    static status_t keyed_otherwise_() noexcept {
        return {status_code_t::invalid_argument_k, "That collection is keyed otherwise"};
    }

    /**
     *  @brief The entries of the collections published last, empty before the mount publishes them.
     *  @note A caller inside the ledger's epochs walks them until it leaves; one holding the claim reads them in step
     *      with @c collections_, since only a claim holder publishes.
     */
    [[nodiscard]] std::span<collection_entry_t* const> collection_entries_() const noexcept {
        published_collections_t const* const published = published_collections_.load();
        if (!published) return {};
        return published->entries;
    }

    /**
     *  @brief Lays out a list naming every entry of @c collections_, publishes it under the leader word, and frees
     *      the one it replaces as it lets the word go, once every reader that could hold it has left.
     *  @note Only a caller holding the claim and inside no epoch publishes, so an entry the new list no longer names
     *      may be destroyed as soon as this returns.
     */
    status_t publish_collections_() noexcept {
        std::unique_ptr<published_collections_t> next {new (std::nothrow) published_collections_t {}};
        if (!next || next->entries.reserve(collections_.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to publish the collections"};
        catalog_allocator_t const allocator {resources_->memory_constraint};
        auto [by_id, ids_status] = ids_index_t::make(collections_.size(), {}, allocator);
        auto [by_name, names_status] = names_index_t::make(collections_.size(), {}, allocator);
        if (ids_status != st::status_t::success_k || names_status != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to publish the collections"};
        for (collection_entry_t* entry : collections_) {
            std::uint32_t const index = entries_.index_of(*entry);
            [[maybe_unused]] st::status_t const listed = next->entries.push_back(st::assume_reserved, std::move(entry));
            [[maybe_unused]] st::status_t const by_ided = by_id.insert(id_key_(entry->id), index, st::assume_unique);
            [[maybe_unused]] st::status_t const by_named = by_name.insert(name_key_(entry->name.view()), index,
                                                                          st::assume_unique);
        }
        next->by_id = std::move(by_id);
        next->by_name = std::move(by_name);
        ledger_.lead();
        atomic_reference_<std::uint64_t> const version {collections_version_};
        version.store(version.load(std::memory_order_relaxed) + 1, std::memory_order_release);
        published_collections_.replace(*next);
        if (published_collections_t* const older = std::exchange(latest_collections_, std::move(next)).release())
            ledger_.unpublish(older->unpublished, older, &destroy_collections_);
        ledger_.stop_leading();
        return {};
    }

    static void destroy_collections_(void* collections) noexcept {
        delete static_cast<published_collections_t*>(collections);
    }

    [[nodiscard]] static st::key128_t id_key_(collection_id_t collection_id) noexcept {
        return st::key128_t::from_bytes(std::as_bytes(std::span<std::uint8_t const, 16> {collection_id.uuid.bytes}));
    }

    [[nodiscard]] static std::uint64_t name_key_(string_view_t name) noexcept {
        return st::hash<string_view_t> {}(name);
    }

    /**
     *  @brief The published entry @p collection_id names, or null when none is published.
     *  @note Called inside the ledger's epochs or holding the claim, as @c collection_entries_ is.
     */
    [[nodiscard]] collection_entry_t* entry_with_id_(collection_id_t collection_id) noexcept {
        published_collections_t const* const published = published_collections_.load();
        if (!published) return nullptr;
        std::uint32_t const index = published->by_id.find(id_key_(collection_id));
        return index == ids_index_t::absent_k ? nullptr : &entries_[index];
    }

    /** The published entry @p name answers to, or null when the published collections name none. */
    [[nodiscard]] collection_entry_t* entry_named_(string_view_t name) noexcept {
        published_collections_t const* const published = published_collections_.load();
        if (!published) return nullptr;
        std::uint32_t const index = published->by_name.find(
            name_key_(name), [&](std::uint32_t candidate) noexcept { return entries_[candidate].name.view() == name; });
        return index == names_index_t::absent_k ? nullptr : &entries_[index];
    }

    /** The published entry @p id names, or null when the published collections name none. */
    [[nodiscard]] collection_entry_t* entry_at_(any_collection_id_t const& id) noexcept {
        if (collection_id_t const* const identified = std::get_if<collection_id_t>(&id))
            return entry_with_id_(*identified);
        return entry_named_(std::get<collection_alphanumeric_id_t>(id).view());
    }

    /** A free record of @c entries_, or null when the pool may not grow. */
    [[nodiscard]] collection_entry_t* take_entry_() noexcept {
        std::uint32_t const index = entries_.take();
        if (index == entries_t::absent_k) return nullptr;
        collection_entry_t& entry = entries_[index];
        entry.listed_stamp = unwritten_stamp_k;
        entry.unlisted_stamp = invalid_transaction_stamp_k;
        return &entry;
    }

    /** Lands a commit of nothing, whose stamp the leader stores into @p word before it publishes
     *  the stamp, so a reader pinned at that stamp or later reads @p word set. */
    void stamp_next_commit_(transaction_stamp_t& word) noexcept {
        commit_request_t request;
        request.context = &word;
        request.prepare = [](void*) noexcept -> status_t { return {}; };
        request.publish = [](void* context, transaction_stamp_t stamp) noexcept {
            atomic_reference_<transaction_stamp_t> {*static_cast<transaction_stamp_t*>(context)}.store(
                stamp, std::memory_order_release);
        };
        // A commit preparing nothing is never refused.
        [[maybe_unused]] expected<transaction_stamp_t> const landed = ledger_.submit(request);
    }

    /** Destroys the collection @p entry holds and returns its record to @c entries_. */
    void release_entry_(collection_entry_t& entry) noexcept {
        entry.held.template emplace<0>();
        entries_.vacate(entries_.index_of(entry));
    }

    /** One head write's commit, as the sequencer hands it to its two steps. */
    struct head_commit_t {

        /** The database committed to. */
        basic_database* store = nullptr;

        /** The collection written, which the leader looks up again. */
        collection_id_t id = collection_invalid_id_k;

        /** Its deltas and the run they become. */
        sealed_commit<stored_key_t, atomic_reference_, row_kit_type_> sealed;

        /** The memory-run record the run lands in, claimed before the leader word. */
        memory_run_claim_t record;
    };

    static status_t prepare_head_(void* context) noexcept {
        head_commit_t& head = *static_cast<head_commit_t*>(context);
        return head.store->prepare_collection(head.id, head.sealed, std::move(head.record));
    }

    /** Gives @p claimed back to the store of its collection, or frees its view alone once a drop
     *  took the store. */
    static void give_back_memory_record_(memory_run_claim_t& claimed) noexcept {
        basic_database& owner = *static_cast<basic_database*>(claimed.owner);
        reader_t const inside = owner.ledger_.epochs().enter();
        if (collection_entry_t* const found = owner.entry_with_id_(claimed.collection))
            std::visit([&](auto& collection) noexcept { collection.give_back(claimed); }, found->held);
        else claimed.destroy_view(claimed.view);
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
            collection_entry_t const* const found = entry_at_(id);
            if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
            head.id = found->id;
        }
        if (status_t const sealed = seal_into_(head.id, head.sealed, sealer); sealed.failed()) return sealed;
        if (head.sealed.empty()) return {};
        // Claimed before the leader word, waiting on a flush when every record is out, so the
        // commit cannot be refused.
        if (status_t const claimed = claim_memory_record(head.id, head.record); claimed.failed()) return claimed;
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
    status_t seal_into_(collection_id_t collection_id,
                        sealed_commit<stored_key_t, atomic_reference_, row_kit_type_>& sealed,
                        sealer_type_&& sealer) noexcept {
        {
            reader_t const inside = ledger_.epochs().enter();
            collection_entry_t const* const found = entry_with_id_(collection_id);
            if (!found) return {status_code_t::write_conflict_k, "A collection the transaction wrote is gone"};
            status_t const status = std::visit(
                [&]<typename store_type_>(store_type_ const& collection) noexcept -> status_t {
                    if constexpr (keyed_by_database<store_type_>()) {
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
        if (sealed.empty()) return {};
        for (;;) {
            {
                reader_t const inside = ledger_.epochs().enter();
                collection_entry_t const* const found = entry_with_id_(collection_id);
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
            collection_entry_t* const claimed = entry_with_id_(collection_id);
            if (!claimed) return {status_code_t::write_conflict_k, "A collection the transaction wrote is gone"};
            if (status_t const flushed = std::visit(
                    [&](auto& collection) noexcept { return flush_and_compact_(collection); }, claimed->held);
                flushed.failed())
                return flushed;
        }
    }

    /** Flushes @p collection's memory runs, publishes the manifest naming the run, and compacts the
     *  tree for as long as the flush left a level over its bounds, holding the claim. */
    template <typename collection_type_>
    status_t flush_and_compact_(collection_type_& collection) noexcept {
        auto [reading, reading_status] = channels_->acquire();
        if (reading_status.failed()) return reading_status;
        if (status_t const flushed = collection.flush(reading.channel()); flushed.failed()) return flushed;
        if (status_t const saved = publish_manifest_(reading.channel()); saved.failed()) return saved;
        if (!collection.needs_compaction()) return {};
        auto [writing, writing_status] = channels_->acquire();
        if (writing_status.failed()) return writing_status;
        manifest_write_t recording {this, &reading.channel()};
        lsm_tree_publication_hook_t published;
        published.notify = &record_manifest_;
        published.context = &recording;
        auto [buffers, lent_status] = lent_merge_buffers_<collection_type_>(1);
        if (lent_status.failed()) return lent_status;
        while (collection.needs_compaction()) {
            auto [outcome, compact_status] = collection.compact(reading.channel(), writing.channel(), buffers,
                                                                compactor_, published);
            if (compact_status.failed()) return compact_status;
            if (outcome == compaction_outcome_t::nothing_to_compact_k) break;
        }
        return {};
    }

    /** Merges @p entry's memory runs by tier, and flushes and compacts them once they hold their bounds, holding
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
        for (collection_entry_t* entry : collections_)
            if (status_t const maintained = maintain_collection_(*entry); maintained.failed() && first.succeeded())
                first = maintained;
        return first;
    }

    /** Whether any collection the database names is due for a merge or a flush, read from
     *  the published views. */
    [[nodiscard]] bool maintenance_due_() noexcept {
        reader_t const inside = ledger_.epochs().enter();
        for (collection_entry_t const* entry : collection_entries_())
            if (std::visit([](auto const& collection) noexcept { return collection.maintenance_due(); }, entry->held))
                return true;
        return false;
    }

    /**
     *  @brief Hands the claim back, then looks again at every collection after each release and, if
     *      one is due and the claim is still free, takes it back to maintain them, so a commit that
     *      found the claim held left its maintenance to this caller.
     *
     *  A caller that finds the claim taken again, or a caller parked on it, stops, since
     *  that holder looks once more after it lets go, and a failed maintenance is met again
     *  by the next commit.
     */
    void release_claim_() noexcept {
        // Every release looks again, so a commit that found the claim held during a maintenance is
        // met after it.
        for (;;) {
            claim_.release();
            if (!maintenance_due_() || claim_.anyone_parked() || !claim_.take_if_free()) return;
            if (maintain_due_().failed()) break;
        }
        claim_.release();
    }

    /** Flushes every collection and publishes the manifest naming the runs, holding the claim;
     *  answers the first failure, after trying every collection. */
    status_t flush_holding_(io_channel_t& channel) noexcept {
        status_t first;
        for (std::size_t index = 0; index != collections_.size(); ++index)
            if (status_t const flushed = std::visit([&](auto& held) noexcept { return held.flush(channel); },
                                                    collections_[index]->held);
                flushed.failed() && first.succeeded())
                first = flushed;
        // Every run the flushes wrote is drained by now, and the publication flushes it
        // before naming it.
        if (status_t const saved = publish_manifest_(channel); saved.failed() && first.succeeded()) first = saved;
        return first;
    }

    /** Takes the claim and then a shared channel, which every call that moves bytes runs under. */
    expected<claimed_channel_t> claim_channel_() noexcept {
        claim_guard_t claim {*this};
        auto [guard, acquire_status] = channels_->acquire();
        if (acquire_status.failed()) return acquire_status;
        return claimed_channel_t {std::move(claim), std::move(guard)};
    }

    /** Answers @p action on the store @p ingest writes into and the runs it wrote so far. */
    template <typename result_type_, typename action_type_>
    result_type_ with_ingest_(ingest_t& ingest, action_type_&& action) noexcept {
        any_collection_t& held = ingest.entry_->held;
        result_type_ result = status_t {status_code_t::internal_k, "The ingest lost its store"};
        auto rung = [&]<std::size_t index_>(std::integral_constant<std::size_t, index_>) noexcept {
            if (held.index() == index_) result = action(std::get<index_>(held), std::get<index_>(ingest.runs_));
        };
        [&]<std::size_t... indices_>(std::index_sequence<indices_...>) noexcept {
            (rung(std::integral_constant<std::size_t, indices_> {}), ...);
        }(std::index_sequence_for<store_types_...> {});
        return result;
    }

    /** Mounts @p entry's collection on its volume in the one of @c store_types_ its tag names,
     *  which the caller checked. */
    status_t mount_collection_(collection_entry_t& entry) noexcept {
        status_t mounted;
        std::size_t const picked = store_tags_type_::index_of(entry.store_tag);
        auto rung = [&]<std::size_t index_>(std::integral_constant<std::size_t, index_>) noexcept {
            if (picked != index_) return;
            using store_t = std::variant_alternative_t<index_, any_collection_t>;
            any_run_pool_t& pool = run_pools_[index_];
            if (std::holds_alternative<std::monostate>(pool)) {
                std::size_t const capacity = volume_->runs_capacity() < run_pool_capacity_k ? volume_->runs_capacity()
                                                                                            : run_pool_capacity_k;
                // The records grow in chunks that never move, from one record up to the capacity.
                auto [made, make_status] = store_t::run_pool_t::make(1, capacity, resources_->memory_constraint);
                if (make_status.failed()) {
                    mounted = make_status;
                    return;
                }
                pool.template emplace<index_ + 1>(std::move(made));
            }
            auto [store, mount_status] = store_t::mount(*volume_, entry.config, resources_->memory_constraint, ledger_,
                                                        std::get<index_ + 1>(pool));
            mounted = mount_status;
            if (mount_status.succeeded()) entry.held.template emplace<index_>(std::move(store));
        };
        [&]<std::size_t... indices_>(std::index_sequence<indices_...>) noexcept {
            (rung(std::integral_constant<std::size_t, indices_> {}), ...);
        }(std::index_sequence_for<store_types_...> {});
        return mounted;
    }

    /** Writes the memory runs of @p collection_id out as runs without publishing a manifest, so
     *  the publication that follows names them together with the change that wrote them; an
     *  invalid ID flushes nothing. */
    status_t flush_into_publication_(io_channel_t& channel, collection_id_t collection_id) noexcept {
        if (collection_id == collection_invalid_id_k) return {};
        collection_entry_t* const found = entry_with_id_(collection_id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        return std::visit([&](auto& held) noexcept { return held.flush(channel); }, found->held);
    }

    /** Where @p store_type_ first sits among @c store_types_, one before its buffers' place in
     *  @c merge_buffers_. */
    template <typename store_type_>
    static constexpr std::size_t store_index_() noexcept {
        constexpr bool matches[] {std::is_same_v<store_type_, store_types_>...};
        std::size_t index = 0;
        while (!matches[index]) ++index;
        return index;
    }

    /** The merge buffers lent to a merge of a collection of @p store_type_ over @p slices slices,
     *  under the claim; a merge of another store's collection frees what the last one grew, as
     *  their buffers differ in type. */
    template <typename store_type_>
    expected<std::span<typename store_type_::merge_buffers_t>> lent_merge_buffers_(std::size_t slices) noexcept {
        constexpr std::size_t held_index = store_index_<store_type_>() + 1;
        if (merge_buffers_.index() != held_index) merge_buffers_.template emplace<held_index>();
        st::vector<typename store_type_::merge_buffers_t>& buffers = std::get<held_index>(merge_buffers_);
        if (buffers.size() < slices && buffers.resize(slices) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room for a merge's buffers"};
        return std::span<typename store_type_::merge_buffers_t> {buffers}.first(slices);
    }

    /** Reserves room for one run pool per store, each made at its store's first collection
     *  and never moved. */
    status_t make_run_pools_() noexcept {
        run_pools_.clear();
        if (run_pools_.resize(sizeof...(store_types_)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room for the run pools"};
        return {};
    }

    /** Publishes @p written of @p bytes as the caller's own data; whichever extent the durable
     *  manifest stops naming, the replaced one or a refused new one, goes back after the next
     *  publication that succeeds. */
    status_t replace_user_data_(io_channel_t& channel, uuid_t const& run, medium_extent_t const& written,
                                std::uint64_t bytes) noexcept {
        uuid_t const replaced_run = std::exchange(user_data_run_, run);
        medium_extent_t const replaced = std::exchange(user_data_, written);
        std::uint64_t const replaced_bytes = std::exchange(user_data_bytes_, bytes);
        if (status_t const published = publish_manifest_(channel); published.failed()) {
            user_data_run_ = replaced_run;
            user_data_ = replaced;
            user_data_bytes_ = replaced_bytes;
            if (bytes != 0) volume_->deallocate_after_publication(written);
            return published;
        }
        if (replaced_bytes != 0) volume_->deallocate_after_publication(replaced);
        return {};
    }

    /** Prints the manifest naming every collection, its runs and the user data, and publishes it
     *  through the volume, which flushes every run written since the last one before it commits. */
    status_t publish_manifest_(io_channel_t& channel) noexcept {
        st::vector<char> json;
        if (status_t const printed = print_manifest_(ledger_.durable_stamp(), json); printed.failed()) return printed;
        return volume_->publish_manifest(channel, std::span<char const> {json});
    }

    /** One file a manifest names and where it lies on this database's volume. */
    struct placed_file_t {
        manifest_file_t file;
        medium_extent_t extent;
    };

    /**
     *  @brief Places the run @p source names in @p into: keeps one the target holds at the same
     *      length, links its file from one filesystem to another, or reads it through @p channel
     *      into a fresh file or slot.
     *  @param[in,out] placed Every extent this save placed, which a cancelled or failed
     *      save gives back.
     */
    template <typename digester_type_>
    expected<medium_extent_t> place_run_(volume_t& into, io_channel_t& reading, io_channel_t& writing,
                                         placed_file_t const& source,
                                         st::vector<manifest_file_t const*> const& held_by_run,
                                         archive_report_t& report, st::vector<medium_extent_t>& placed,
                                         digester_type_ const& digester, sha256_t& digest) noexcept {
        manifest_file_t const* const* const found = std::lower_bound(
            held_by_run.begin(), held_by_run.end(), source.file.run,
            [](manifest_file_t const* file, uuid_t const& run) noexcept { return file->run < run; });
        if (found != held_by_run.end() && (*found)->run == source.file.run && (*found)->bytes == source.file.bytes) {
            ++report.kept;
            auto [extent, open_status] = into.open(**found);
            if (open_status.failed()) return open_status;
            digest = (*found)->sha256;
            if (!digest.empty()) return extent;
            if (status_t const hashed = move_extent_(reading, nullptr, extent, nullptr, digester, digest);
                hashed.failed())
                return hashed;
            return extent;
        }
        char from[path_max_bytes_k], to[path_max_bytes_k];
        if (volume_->file_path(source.extent.data_directory, source.file.run, from).succeeded() &&
            into.file_path(0, source.file.run, to).succeeded()) {
            auto [landing, landing_status] = utilities::link_or_copy_file(from, to);
            if (landing_status.failed()) return landing_status;
            manifest_file_t linked = source.file;
            linked.medium = 0;
            auto [extent, open_status] = into.open(linked);
            if (open_status.failed()) return open_status;
            [[maybe_unused]] st::status_t const tracked = placed.push_back(st::assume_reserved,
                                                                           medium_extent_t {extent});
            // A copy across filesystems is a copy all the same, so the landed bytes are hashed.
            if (status_t const hashed = move_extent_(reading, nullptr, extent, nullptr, digester, digest);
                hashed.failed())
                return hashed;
            ++report.linked;
            return extent;
        }
        auto [extent, allocate_status] = into.allocate(source.file.run, source.extent.extent.length);
        if (allocate_status.failed()) return allocate_status;
        [[maybe_unused]] st::status_t const tracked = placed.push_back(st::assume_reserved, medium_extent_t {extent});
        if (status_t const copied = move_extent_(reading, &writing, source.extent, &extent, digester, digest);
            copied.failed())
            return copied;
        ++report.written;
        return extent;
    }

    /** Hashes the run @p file names, opened at @p extent, where the manifest names its SHA-256 and
     *  @p digester checks runs, refusing one whose bytes differ. */
    template <typename digester_type_>
    status_t verify_run_(io_channel_t& channel, manifest_file_t const& file, medium_extent_t const& extent,
                         digester_type_ const& digester) noexcept {
        if constexpr (std::is_same_v<digester_type_, unverified_runs_t>) return {};
        else {
            if (file.sha256.empty()) return {};
            sha256_t digest {};
            if (status_t const hashed = move_extent_(channel, nullptr, extent, nullptr, digester, digest);
                hashed.failed())
                return hashed;
            if (!(digest == file.sha256))
                return {status_code_t::malformed_k, "A run's bytes differ from the SHA-256 its manifest names"};
            return {};
        }
    }

    /** Blocks a copy reads ahead of the oldest one not yet hashed, whose bytes wait in order. */
    static constexpr std::size_t hashed_window_k = 32;

    /**
     *  @brief Reads @p from on @p reading, a block per request at the channel's full depth, into
     *      @p into on @p writing where both are given, hashing the bytes in order into @p digest.
     *
     *  Two channels, since a pipeline polling a shared queue pair would reap the other's
     *  completions. Blocks complete in any order, so one ahead of the oldest unhashed block waits
     *  in a window of @c hashed_window_k blocks, which bounds how far the reads run ahead.
     */
    template <typename digester_type_>
    status_t move_extent_(io_channel_t& reading, io_channel_t* writing, medium_extent_t const& from,
                          medium_extent_t const* into, digester_type_ const& digester, sha256_t& digest) noexcept {
        disk_t& disk = volume_->disk();
        auto [reader_channel, reader_channel_status] = reading.acquire();
        if (reader_channel_status.failed()) return reader_channel_status;
        basic_read_pipeline<disk_t> reader {disk, std::move(reader_channel), from.medium};
        std::optional<basic_write_pipeline<disk_t, waiter_type_>> writer;
        std::uint64_t block_bytes = reading.requests().buffer_bytes();
        if (writing) {
            auto [writer_channel, writer_channel_status] = writing->acquire();
            if (writer_channel_status.failed()) return writer_channel_status;
            writer.emplace(disk, std::move(writer_channel), into->medium);
            if (writing->requests().buffer_bytes() < block_bytes) block_bytes = writing->requests().buffer_bytes();
        }
        block_bytes = round_down_to_multiple<std::uint64_t>(block_bytes, portable_alignment_k);
        if (block_bytes == 0) return {status_code_t::capacity_exhausted_k, "A channel's block holds no page"};
        std::uint64_t const blocks = divide_round_up(from.extent.length, block_bytes);

        st::vector<byte_t, constrained_allocator<byte_t, atomic_reference_>> waiting {
            constrained_allocator<byte_t, atomic_reference_> {resources_->memory_constraint}};
        st::vector<std::uint64_t, constrained_allocator<std::uint64_t, atomic_reference_>> waiting_bytes {
            constrained_allocator<std::uint64_t, atomic_reference_> {resources_->memory_constraint}};
        if (waiting.resize(hashed_window_k * block_bytes) != st::status_t::success_k ||
            waiting_bytes.resize(hashed_window_k, 0) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room for the blocks a hash waits on"};
        sz_sha256_state_t state;
        digester.init(state);
        std::uint64_t hashed = 0;

        status_t carried;
        auto sink = [&](utilities::completed_buffer_t const& completed) noexcept {
            if (carried.failed()) return;
            if (status_t const whole = utilities::require_completed_in_full(completed); whole.failed()) {
                carried = whole;
                return;
            }
            std::uint64_t const block = completed.tag;
            if (block == hashed) {
                digester.update(state, completed.bytes);
                ++hashed;
                for (std::size_t slot = hashed % hashed_window_k; waiting_bytes[slot] != 0;
                     slot = hashed % hashed_window_k) {
                    digester.update(state,
                                    std::span<byte_t const> {waiting.data() + slot * block_bytes, waiting_bytes[slot]});
                    waiting_bytes[slot] = 0;
                    ++hashed;
                }
            }
            else {
                std::size_t const slot = block % hashed_window_k;
                std::memcpy(waiting.data() + slot * block_bytes, completed.bytes.data(), completed.bytes.size());
                waiting_bytes[slot] = completed.bytes.size();
            }
            if (!writer) return;
            auto [buffer, acquire_status] = writer->acquire_buffer_or_poll();
            if (acquire_status.failed()) {
                carried = acquire_status;
                return;
            }
            std::memcpy(buffer.bytes().data(), completed.bytes.data(), completed.bytes.size());
            if (status_t const queued = writer->enqueue_write(std::move(buffer),
                                                              into->slice(block * block_bytes, completed.bytes.size()));
                queued.failed())
                carried = queued;
            else if (status_t const submitted = writer->submit(); submitted.failed()) carried = submitted;
        };
        for (std::uint64_t next = 0; next != blocks && carried.succeeded();) {
            while (next != blocks && next < hashed + hashed_window_k && reader.can_enqueue()) {
                std::uint64_t const offset = next * block_bytes;
                std::uint64_t const chunk = from.extent.length - offset < block_bytes ? from.extent.length - offset
                                                                                      : block_bytes;
                if (status_t const queued = reader.enqueue_read(from.slice(offset, chunk),
                                                                static_cast<request_tag_t>(next));
                    queued.failed())
                    return queued;
                ++next;
            }
            if (status_t const submitted = reader.submit(); submitted.failed()) return submitted;
            if (status_t const polled = reader.poll_one_completion(sink); polled.failed()) return polled;
        }
        if (status_t const polled = reader.poll_all_completions(sink); polled.failed()) return polled;
        if (carried.failed()) return carried;
        if (hashed != blocks) return {status_code_t::internal_k, "A copy hashed fewer blocks than it read"};
        digest = digester.digest(state);
        return writer ? writer->poll_all_completions() : status_t {};
    }

    /** Lays the manifest out as JSON into @p json, promising every commit at or
     *  below @p durable_stamp. */
    status_t print_manifest_(std::uint64_t durable_stamp, st::vector<char>& json) noexcept {
        manifest_t manifest;
        if (status_t const laid = lay_out_collections_(volume_->manifest(), manifest); laid.failed()) return laid;
        manifest.generation = volume_->generation() + 1;
        manifest.durable_stamp = durable_stamp;
        manifest.placement = volume_->placement();
        st::vector<placed_file_t> placed;
        if (status_t const gathered = gather_files_(placed); gathered.failed()) return gathered;
        if (manifest.files.reserve(placed.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to lay the manifest out"};
        for (placed_file_t const& entry : placed) [[maybe_unused]]
            st::status_t const named = manifest.files.push_back(st::assume_reserved, manifest_file_t {entry.file});
        return manifest.print(constrained_allocator<byte_t, atomic_reference_> {resources_->memory_constraint}, json);
    }

    /** Names @p manifest after @p identity and lists every collection of this database in it. */
    status_t lay_out_collections_(manifest_t const& identity, manifest_t& manifest) noexcept {
        if (sz::failed(manifest.database.assign(identity.database.view())))
            return {status_code_t::out_of_memory_k, "No room to lay the manifest out"};
        manifest.id = identity.id;
        if (manifest.collections.reserve(collections_.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to lay the manifest out"};
        for (collection_entry_t const* held : collections_) {
            collection_entry_t const& entry = *held;
            manifest_collection_t record;
            std::memcpy(record.id.bytes, entry.id.uuid.bytes, sizeof(record.id.bytes));
            if (sz::failed(record.name.assign(entry.name.view())))
                return {status_code_t::out_of_memory_k, "No room to lay the manifest out"};
            record.store = entry.store_tag;
            record.value_size = entry.config.lsm_tree.lsm_level_0.sorted_run.value_size;
            record.value_max_size = entry.config.lsm_tree.lsm_level_0.sorted_run.value_max_size;
            record.level_enlarge_factor = entry.config.lsm_tree.level_enlarge_factor;
            record.cache_limit = entry.config.cache_limit;
            record.memory_runs_max_bytes = entry.config.memory_runs_max_bytes;
            record.memory_runs_max_deltas = entry.config.memory_runs_max_deltas;
            [[maybe_unused]] st::status_t const placed = manifest.collections.push_back(st::assume_reserved,
                                                                                        std::move(record));
        }
        return {};
    }

    /** Lists every run of every collection, level by level, and the caller's own data, as the
     *  manifest names them. */
    status_t gather_files_(st::vector<placed_file_t>& into) noexcept {
        st::vector<lsm_tree_run_record_t> runs;
        for (std::size_t index = 0; index != collections_.size(); ++index) {
            runs.clear();
            if (status_t const listed = std::visit(
                    [&](auto const& held) noexcept { return record_lsm_tree_runs(held.tree(), runs); },
                    collections_[index]->held);
                listed.failed())
                return listed;
            for (lsm_tree_run_record_t const& run : runs) {
                placed_file_t entry {file_of_(run.extent, run.content_id, run.extent.extent.length,
                                              static_cast<std::uint32_t>(index), run.level_number, run.stamp),
                                     run.extent};
                if (into.push_back(std::move(entry)) != st::status_t::success_k)
                    return {status_code_t::out_of_memory_k, "No room to lay the manifest out"};
            }
        }
        if (user_data_bytes_ == 0) return {};
        placed_file_t entry {file_of_(user_data_, user_data_run_, user_data_bytes_, manifest_no_collection_k, 0, 0),
                             user_data_};
        if (into.push_back(std::move(entry)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to lay the manifest out"};
        return {};
    }

    /** The manifest entry of @p extent, run @p run of @p bytes, owned by
     *  collection @p collection. */
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

    /** Opens every collection the manifest names, drops the versions a crash tore and publishes the
     *  collections, which a mount does once, leaving nothing published when it fails. */
    template <typename digester_type_>
    status_t open_manifest_(digester_type_ const& digester) noexcept {
        // Every executor thread with a channel of its own reads on an epoch slot of its own too.
        if (status_t const reserved = ledger_.epochs().reserve_threads(channels_->executor_channels());
            reserved.failed())
            return reserved;
        if (status_t const made = make_run_pools_(); made.failed()) return made;
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        std::uint64_t const durable_stamp = volume_->manifest().durable_stamp;
        if (status_t const adopted = adopt_manifest_(claimed->channel(), digester); adopted.failed()) return adopted;

        // The stamps continue above the durable stamp, and every version above it is a commit a
        // crash tore, which is dropped before anything draws a stamp; resuming first is what lets
        // the truncation's manifest promise it.
        if (status_t const resumed = ledger_.resume(durable_stamp); resumed.failed()) return resumed;
        auto [writing, acquire_status] = channels_->acquire();
        if (acquire_status.failed()) return acquire_status;
        manifest_write_t recording {this, &claimed->channel()};
        lsm_tree_publication_hook_t published;
        published.notify = &record_manifest_;
        published.context = &recording;
        for (std::size_t index = 0; index != collections_.size(); ++index)
            if (status_t const truncated = std::visit(
                    [&]<typename store_type_>(store_type_& collection) noexcept -> status_t {
                        auto [buffers, lent_status] = lent_merge_buffers_<store_type_>(1);
                        if (lent_status.failed()) return lent_status;
                        return collection.truncate_above(claimed->channel(), writing.channel(), buffers, durable_stamp,
                                                         published);
                    },
                    collections_[index]->held);
                truncated.failed())
                return truncated;
        return publish_collections_();
    }

    /** Opens every collection and the user data the manifest the volume read names, each collection
     *  over its runs in the order the manifest lists them. */
    template <typename digester_type_>
    status_t adopt_manifest_(io_channel_t& channel, digester_type_ const& digester) noexcept {
        manifest_t const& loaded = volume_->manifest();
        std::size_t const collections = loaded.collections.size();
        // A counting sort by collection keeps every collection's runs in the manifest's order.
        st::vector<std::size_t> starts;
        st::vector<lsm_tree_run_record_t> runs;
        if (starts.resize(collections + 1, 0) != st::status_t::success_k ||
            runs.resize(loaded.files.size()) != st::status_t::success_k ||
            collections_.reserve(collections) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room for what the manifest names"};
        std::size_t run_count = 0;
        for (manifest_file_t const& file : loaded.files) {
            if (file.collection == manifest_no_collection_k) {
                if (user_data_bytes_ != 0)
                    return {status_code_t::malformed_k, "The manifest names the user data twice"};
                auto [opened, open_status] = volume_->open(file);
                if (open_status.failed()) return open_status;
                if (status_t const verified = verify_run_(channel, file, opened, digester); verified.failed())
                    return verified;
                user_data_run_ = file.run;
                user_data_ = opened;
                user_data_bytes_ = file.bytes;
                continue;
            }
            ++starts[file.collection + 1];
            ++run_count;
        }
        for (std::size_t index = 0; index != collections; ++index) starts[index + 1] += starts[index];
        for (manifest_file_t const& file : loaded.files) {
            if (file.collection == manifest_no_collection_k) continue;
            auto [opened, open_status] = volume_->open(file);
            if (open_status.failed()) return open_status;
            if (status_t const verified = verify_run_(channel, file, opened, digester); verified.failed())
                return verified;
            lsm_tree_run_record_t& record = runs[starts[file.collection]++];
            record.level_number = file.level;
            record.content_id = file.run;
            record.extent = opened;
            record.stamp = file.stamp;
        }

        std::size_t begin = 0;
        for (std::size_t index = 0; index != collections; ++index) {
            std::size_t const end = starts[index];
            if (status_t const opened = adopt_(
                    channel, loaded.collections[index],
                    std::span<lsm_tree_run_record_t const> {runs.data() + begin, end - begin});
                opened.failed())
                return opened;
            begin = end;
        }
        if (begin != run_count) return {status_code_t::internal_k, "The manifest's runs were not all adopted"};
        return {};
    }

    /** Opens the collection @p record describes over the runs @p runs names, and takes it
     *  into the database. */
    status_t adopt_(io_channel_t& channel, manifest_collection_t const& record,
                    std::span<lsm_tree_run_record_t const> runs) noexcept {
        if (store_tags_type_::index_of(record.store) >= sizeof...(store_types_))
            return {status_code_t::malformed_k, "The manifest names a store this database does not keep"};
        if (record.name.size() == 0 || record.name.size() > collection_name_max_bytes_k)
            return {status_code_t::malformed_k, "The manifest names a collection by no name or too long a one"};

        // A refusal below leaves the record taken; the failed mount gives up every record at once.
        collection_entry_t* entry = take_entry_();
        if (!entry) return {status_code_t::out_of_memory_k, "No room for another collection"};
        std::memcpy(entry->id.uuid.bytes, record.id.bytes, sizeof(record.id.bytes));
        if (sz::failed(entry->name.assign(record.name.view())))
            return {status_code_t::out_of_memory_k, "No room for a collection's name"};
        entry->store_tag = record.store;
        entry->config = config_of(record);
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
    struct manifest_write_t {

        /** The database whose manifest the hook publishes. */
        basic_database* store = nullptr;

        /** The channel the manifest is published on, held by the compacting thread. */
        io_channel_t* channel = nullptr;
    };

    /** Publishes the manifest for a compaction that has published, before it drops
     *  what it consumed. */
    static status_t record_manifest_(void* context) noexcept {
        manifest_write_t const& writing = *static_cast<manifest_write_t const*>(context);
        return writing.store->publish_manifest_(*writing.channel);
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The budget every collection and every transaction of this database charges. */
    resources_shared_ptr<atomic_reference_, waiter_type_> resources_;

    /** The stamps every commit of this database draws, a transaction's or a head write's, and the stamps pinned. */
    commit_ledger<atomic_reference_, waiter_type_> ledger_;

    /** What every merge of this database runs on, picked at mount: the calling thread, or a device compactor the
     *  opener owns. */
    compactor_ref_t compactor_;

    /** The transactions of this database, which names this store for as long as it lives. */
    registry_t transactions_;

    /** The volume every run lands on and every manifest is committed through, null until the
     *  database is mounted. */
    volume_t* volume_ = nullptr;

    /** The ID a directory volume names the file of the caller's own data by. */
    uuid_t user_data_run_ {};

    /** Where the caller's own data lies, meaningful while @c user_data_bytes_ is not zero. */
    medium_extent_t user_data_ {};

    /** Bytes of the caller's own data, zero when it has saved none. */
    std::uint64_t user_data_bytes_ = 0;

    /** One pool of runs per store, made at its store's first collection and
     *  outliving every collection. */
    st::vector<any_run_pool_t> run_pools_;

    /** The one set of merge buffers, of the store whose collection merged last, which the claim
     *  lends to one merge at a time instead of every collection keeping the largest it grew. */
    std::variant<std::monostate, st::vector<typename store_types_::merge_buffers_t>...> merge_buffers_;

    /** The queue pairs every call that moves bytes takes one of, null until the database is mounted. */
    channel_pool_t* channels_ = nullptr;

    /** The delta bounds a transaction takes when its caller names none. */
    database_config_t config_ {};

    /** Every collection the database names, in the order the manifest lists them. */
    st::vector<collection_entry_t*> collections_;

    /** The records every entry of @c collections_ lives in, made at the mount and given up at
     *  the unmount. */
    entries_t entries_;

    /** The collections readers load, which @c latest_collections_ owns. */
    published_view<published_collections_t, atomic_reference_> published_collections_;

    /** The collections published last, owned until the publication replacing them frees them. */
    std::unique_ptr<published_collections_t> latest_collections_;

    /** How many times the collections have been published, which is what lets a caller memoize a lookup. */
    std::uint64_t collections_version_ = 0;

    /** Held by whichever call changes the manifest, a tree or the user data, a commit paying for
     *  its maintenance included. */
    maintenance_claim<atomic_reference_, waiter_type_> claim_;

#pragma endregion Private Members
};

#pragma endregion Database

#pragma endregion Database

} // namespace unum::ustore::blobs
