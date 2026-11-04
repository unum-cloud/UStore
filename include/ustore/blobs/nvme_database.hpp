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

#include <algorithm>   // `std::copy`, `std::sort`, `std::lower_bound`
#include <array>       // `std::array`
#include <limits>      // `std::numeric_limits`
#include <memory>      // `std::unique_ptr`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <type_traits> // `std::integral_constant`, `std::remove_reference_t`
#include <utility>     // `std::exchange`, `std::forward`, `std::index_sequence_for`, `std::move`
#include <variant>     // `std::variant`, `std::variant_alternative_t`, `std::visit`, `std::get_if`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/blobs/nvme_key_slot.hpp"       // `stored_value_size_t`, `collection_id_t`
#include "ustore/blobs/nvme_published_runs.hpp" // `pinned_snapshot`, `maintenance_claim`, `commit_ledger`
#include "ustore/blobs/nvme_store.hpp"          // `nvme_store_cursor`, `keyed_delta`
#include "ustore/blobs/nvme_transactions.hpp"   // `transaction_registry`, `transaction_core`, `nvme_store_concept`
#include "ustore/blobs/nvme_tree.hpp"           // `lsm_tree_run_record_t`, `database_config_t`
#include "ustore/utilities/nvme_manifest.hpp"   // `manifest_t`, `manifest_collection_t`, `manifest_file_t`
#include "ustore/utilities/nvme_channels.hpp"   // `io_channel_t`, `basic_io_channel_pool`
#include "ustore/utilities/nvme_mounts.hpp"     // `resources_shared_ptr`
#include "ustore/utilities/nvme_ring.hpp"       // `portable_alignment_k`
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
 *
 *  The core carries the writes, the watches, the stamps and the two-phase lifecycle; what is here
 *  resolves a collection's name to its ID and widths once per run of calls, and reads through the
 *  store at the right stamp.
 */
template <typename stored_key_type_, typename store_type_, template <typename> typename atomic_reference_,
          typename waiter_type_>
class database_transaction {
  public:

#pragma region Types

    /** The key every collection of the store is addressed by. */
    using stored_key_t = stored_key_type_;

    /** The store every committed write of this transaction lands in. */
    using store_t = store_type_;

    /** The registry that orders this transaction against every other of the same store. */
    using registry_t = transaction_registry<stored_key_t, store_t, atomic_reference_, waiter_type_>;

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

/**
 *  @brief Named collections of one database, the manifest that names them and the
 *      transactions they share.
 *  @tparam stored_key_type_ The key every collection of this database is addressed by.
 *  @tparam volume_type_ The volume every run of every collection lives on.
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
          typename waiter_type_, typename store_tags_type_, nvme_store_concept... store_types_>
class basic_database {
  public:

#pragma region Types

    /** The key every collection of this database is addressed by. */
    using stored_key_t = stored_key_type_;

    /** The volume every run of every collection lives on. */
    using volume_t = volume_type_;

    /** The transport those volumes move bytes through. */
    using disk_t = std::remove_reference_t<decltype(std::declval<volume_t&>().disk())>;

    /** The queue pairs every call that moves bytes borrows one of. */
    using channel_pool_t = basic_io_channel_pool<disk_t, atomic_reference_>;

    /** One borrowed queue pair, drained and given back when it dies. */
    using channel_guard_t = typename channel_pool_t::guard_t;

    /** A collection named by its ID or by its name, which is how every call addresses one. */
    using any_collection_id_t = std::variant<collection_id_t, collection_alphanumeric_id_t>;

    /** The transactions of this database, the order they commit in and the conflicts that stop them. */
    using registry_t = transaction_registry<stored_key_t, basic_database, atomic_reference_, waiter_type_>;

    /** One transaction over the collections of this database. */
    using transaction_t = database_transaction<stored_key_t, basic_database, atomic_reference_, waiter_type_>;

    /** A live transaction, owned by whoever asked for it and retired when it dies. */
    using transaction_unique_ptr_t = std::unique_ptr<transaction_t>;

    /** One pinned stamp, at which the whole database answers. */
    using snapshot_t = database_snapshot<basic_database>;

    /** One stamp a snapshot, a transaction or a scan holds against the ledger. */
    using pin_t = stamp_pin<atomic_reference_, waiter_type_>;

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
     */
    status_t mount(volume_t& volume, channel_pool_t& channels, database_config_t const& config,
                   compactor_ref_t compactor = {}) noexcept {
        if (volume_ != nullptr) return {status_code_t::already_exists_k, "This database has already mounted"};
        volume_ = &volume;
        channels_ = &channels;
        config_ = config;
        compactor_ = compactor;
        status_t const opened = open_manifest_();
        if (opened.failed()) {
            // A manifest this database could not open stays as it lies, never published again
            // without what it missed.
            collections_.clear();
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
            st::vector<entry_pointer_t> released = std::move(collections_);
            [[maybe_unused]] status_t const emptied = publish_collections_();
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
        return entry_at_in_(collection_entries_(), id) != nullptr;
    }

    /** The ID @p id resolves to, or @c collection_invalid_id_k when it names nothing. */
    [[nodiscard]] collection_id_t collection_id(any_collection_id_t const& id) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        collection_entry_t const* const found = entry_at_in_(collection_entries_(), id);
        return found ? found->id : collection_invalid_id_k;
    }

    /** The name @p collection_id answers to, which is empty when it names nothing. */
    [[nodiscard]] string_t collection_name(collection_id_t collection_id) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        collection_entry_t const* const found = entry_with_id_in_(collection_entries_(), collection_id);
        string_t name;
        if (found) { [[maybe_unused]] sz::status_t const copied = name.assign(found->name.view()); }
        return name;
    }

    /** The bounds @p collection_id was created with, which a transaction checks every write against. */
    status_t collection_config(collection_id_t collection_id, collection_config_t& config) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        collection_entry_t const* const found = entry_with_id_in_(collection_entries_(), collection_id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        config = found->config;
        return {};
    }

    /** The store tag @p collection_id was created with, which a copy of it is created with. */
    status_t collection_store_tag(collection_id_t collection_id, std::uint16_t& store_tag) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        collection_entry_t const* const found = entry_with_id_in_(collection_entries_(), collection_id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        store_tag = found->store_tag;
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
        collection_entry_t const* const found = entry_at_in_(collection_entries_(), id);
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
        if (entry_named_in_(collection_entries_(), name) != nullptr ||
            entry_with_id_in_(collection_entries_(), collection_id) != nullptr)
            return status_t {status_code_t::already_exists_k, "The database already names that collection"};

        entry_pointer_t entry {new (std::nothrow) collection_entry_t {}};
        if (!entry) return status_t {status_code_t::out_of_memory_k, "No room for another collection"};
        entry->id = collection_id;
        if (sz::failed(entry->name.assign(name)))
            return status_t {status_code_t::out_of_memory_k, "No room for a collection's name"};
        entry->store_tag = store_tag;
        entry->config = config;
        std::memcpy(entry->config.lsm_tree.collection.bytes, collection_id.uuid.bytes,
                    sizeof(collection_id.uuid.bytes));
        if (status_t const mounted = mount_collection_(*entry); mounted.failed()) return mounted;

        if (collections_.push_back(std::move(entry)) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room for another collection"};
        if (status_t const flushed = flush_into_publication_(claimed->channel(), flushed_with); flushed.failed()) {
            collections_.erase(collections_.size() - 1);
            return flushed;
        }
        if (status_t const saved = publish_manifest_(claimed->channel()); saved.failed()) {
            collections_.erase(collections_.size() - 1);
            return saved;
        }
        if (status_t const publication = publish_collections_(); publication.failed()) {
            collections_.erase(collections_.size() - 1);
            return publication;
        }
        return {};
    }

    /** Returns every run of @p id to its volume and forgets the name, which destroys the
     *  data; @p flushed_with names a collection whose memory runs the same manifest names, as
     *  the create does. */
    status_t drop_collection(any_collection_id_t const& id,
                             collection_id_t flushed_with = collection_invalid_id_k) noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        std::size_t const position = position_at_(id);
        if (position == collections_.size())
            return {status_code_t::not_found_k, "The database names no such collection"};
        if (status_t const flushed = flush_into_publication_(claimed->channel(), flushed_with); flushed.failed())
            return flushed;

        // The manifest stops naming the runs before a single one of them is handed back. The
        // publication takes the leader word, which keeps a staged or combining commit from
        // publishing into the collection meanwhile, and a request still pending finds it gone; it
        // returns once no reader is left in a view of the collection.
        entry_pointer_t doomed = std::move(collections_[position]);
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
        collection_entry_t const* const found = entry_at_in_(collection_entries_(), id);
        if (!found) return 0;
        return std::visit([](auto const& collection) noexcept { return collection.value_max_size(); }, found->held);
    }

#pragma endregion Shape

#pragma region Lookups

    /** What @p id carries for @p stored_key as a reader pinned at @p stamp sees it, and the bytes its value takes;
     *  a collection the database does not name carries nothing. Executor thread @p thread_index reads on its own
     *  epoch slot, as every lookup below does. */
    [[nodiscard]] lookup_result_t find(any_collection_id_t const& id, stored_key_t stored_key,
                                       stored_value_size_t& size, transaction_stamp_t stamp = head_stamp_k,
                                       std::size_t thread_index = outside_executor_k) noexcept {
        size = 0;
        reader_t const inside = ledger_.epochs().enter(thread_index);
        collection_entry_t const* const found = entry_at_in_(collection_entries_(), id);
        if (!found) return lookup_result_t::missing_k;
        return std::visit(
            [&]<typename store_type_>(store_type_ const& collection) noexcept {
                if constexpr (keyed_by_database<store_type_>())
                    return collection.find(stored_key, size, stamp, thread_index);
                else return lookup_result_t::missing_k;
            },
            found->held);
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
        lookup_result_t const found = find(id, stored_key, size, stamp, thread_index);
        if (found == lookup_result_t::written_k) return {};
        if (found != lookup_result_t::incomplete_k) return {status_code_t::not_found_k, "That collection misses it"};
        std::uint64_t measured = 0;
        lookup_result_t folded = lookup_result_t::missing_k;
        auto const count = [&](std::size_t, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
            measured = std::max<std::uint64_t>(measured, offset + bytes.size());
        };
        if (status_t const read = select(id, std::span<stored_key_t const> {&stored_key, 1},
                                         std::span<lookup_result_t> {&folded, 1}, count, stamp, thread_index);
            read.failed())
            return read;
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
        reader_t const inside = ledger_.epochs().enter(thread_index);
        collection_entry_t* const found = entry_at_in_(collection_entries_(), id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        auto [channel, acquire_status] = channels_->acquire(thread_index);
        if (acquire_status.failed()) return acquire_status;
        return std::visit(
            [&]<typename store_type_>(store_type_& collection) noexcept -> status_t {
                if constexpr (keyed_by_database<store_type_>())
                    return collection.select(channel.channel(), stored_key, into, size, stamp, thread_index);
                else return keyed_otherwise_();
            },
            found->held);
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
        reader_t const inside = ledger_.epochs().enter(thread_index);
        collection_entry_t const* const found = entry_at_in_(collection_entries_(), id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        auto [channel, acquire_status] = channels_->acquire(thread_index);
        if (acquire_status.failed()) return acquire_status;
        return std::visit(
            [&]<typename store_type_>(store_type_ const& collection) noexcept -> status_t {
                if constexpr (keyed_by_database<store_type_>())
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
     *      below @p stamp, removals included, answering whether the walk carries on.
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
        stamp_pin<atomic_reference_, waiter_type_> pin;
        if (stamp == head_stamp_k) stamp = ledger_.pin_reader(pin);
        std::array<scan_entry<stored_key_t>, scan_page_keys<stored_key_t>()> page;
        for (;;) {
            std::size_t count = 0;
            bool follows = false;
            {
                reader_t const inside = ledger_.epochs().enter(thread_index);
                collection_entry_t const* const found = entry_at_in_(collection_entries_(), id);
                if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
                status_t const read = std::visit(
                    [&]<typename store_type_>(store_type_ const& collection) noexcept -> status_t {
                        if constexpr (keyed_by_database<store_type_>()) {
                            using scanner_t =
                                nvme_store_cursor<typename store_type_::key_slot_t, volume_t, atomic_reference_,
                                                  waiter_type_, typename store_type_::fold_t,
                                                  typename store_type_::device_fold_t>;
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

    /**
     *  @brief Counts the runs of @p id a reader at @p stamp walks, for an unordered scan that walks each run on its
     *      own: every level's runs, then the memory runs oldest first.
     *  @note A transaction's own deltas are one more run, which @c database_transaction::for_each_uncommitted walks.
     */
    expected<counted_runs_t> runs_count(any_collection_id_t const& id, transaction_stamp_t stamp = head_stamp_k,
                                        std::size_t thread_index = outside_executor_k) noexcept {
        reader_t const inside = ledger_.epochs().enter(thread_index);
        collection_entry_t const* const found = entry_at_in_(collection_entries_(), id);
        if (!found) return status_t {status_code_t::not_found_k, "The database names no such collection"};
        return std::visit(
            [&]<typename store_type_>(store_type_ const& collection) noexcept -> expected<counted_runs_t> {
                if constexpr (keyed_by_database<store_type_>()) return collection.count_runs(stamp, thread_index);
                else return keyed_otherwise_();
            },
            found->held);
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
        reader_t const inside = ledger_.epochs().enter(thread_index);
        collection_entry_t const* const found = entry_at_in_(collection_entries_(), id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        return std::visit(
            [&]<typename store_type_>(store_type_ const& collection) noexcept -> status_t {
                if constexpr (keyed_by_database<store_type_>())
                    return collection.scan_run(counted, run_index, from, visitor, stamp, thread_index);
                else return keyed_otherwise_();
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
        std::size_t const position = position_at_(id);
        if (position == collections_.size())
            return {status_code_t::not_found_k, "The database names no such collection"};
        ingest.claim_ = std::move(claim);
        ingest.position_ = position;
        auto rung = [&]<std::size_t index_>(std::integral_constant<std::size_t, index_>) noexcept {
            any_collection_t& held = collections_[position]->held;
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
     *  @return The stamp, or the status that stopped it with every run dropped, either way with the claim given back.
     */
    expected<transaction_stamp_t> publish_ingest(ingest_t& ingest, ustore_durability_t durability) noexcept {
        // The channel goes back before the claim, as every claimed call gives them back
        expected<transaction_stamp_t> const landed = [&]() noexcept -> expected<transaction_stamp_t> {
            auto [guard, acquire_status] = channels_->acquire();
            if (acquire_status.failed()) return acquire_status;
            io_channel_t& channel = guard.channel();
            expected<transaction_stamp_t> const published = with_ingest_<expected<transaction_stamp_t>>(
                ingest,
                [&]<typename held_type_>(held_type_& held, typename held_type_::ingest_runs_t& ingested) noexcept {
                    return held.publish_ingest(channel, ingested);
                });
            if (!published || durability != ustore_durability_flushed_k) return published;
            if (status_t const durable = flush_holding_(channel); durable.failed()) return durable;
            return published;
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

    /** Merges one overfull level of every collection that has one, which needs two channels at once. */
    status_t compact() noexcept {
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
                [&](auto& held) noexcept {
                    return held.compact(reading->channel(), writing.channel(), compactor_, published);
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
            collection_entry_t const* const found = entry_with_id_in_(collection_entries_(), collection_id);
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
        collection_entry_t const* const found = entry_at_in_(collection_entries_(), id);
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
        collection_entry_t const* const found = entry_with_id_in_(collection_entries_(), collection_id);
        if (!found) return true;
        return std::visit(
            [&]<typename store_type_>(store_type_ const& collection) noexcept {
                if constexpr (keyed_by_database<store_type_>()) return collection.written_since(stored_key, from);
                else return true;
            },
            found->held);
    }

    /** Whether a commit newer than @p from wrote any key of @p collection_id from @p lower to @p upper inclusive,
     *  read from the published view; a collection gone since counts as written. */
    [[nodiscard]] bool range_written_since(collection_id_t collection_id, stored_key_t lower, stored_key_t upper,
                                           transaction_stamp_t from) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        collection_entry_t const* const found = entry_with_id_in_(collection_entries_(), collection_id);
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
    status_t seal_collection(collection_id_t collection_id,
                             transaction_deltas<stored_key_t, atomic_reference_, waiter_type_> const& written,
                             sealed_commit<stored_key_t, atomic_reference_>& sealed) noexcept {
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
        collection_entry_t* const found = entry_with_id_in_(collection_entries_(), collection_id);
        if (!found) return {status_code_t::write_conflict_k, "A collection the transaction wrote is gone"};
        return std::visit(
            [&]<typename store_type_>(store_type_& collection) noexcept -> status_t {
                if constexpr (keyed_by_database<store_type_>()) return collection.prepare(sealed);
                else return keyed_otherwise_();
            },
            found->held);
    }

    /** Publishes @p sealed under @p stamp in the view @c prepare_collection laid out, before the stamp itself is,
     *  on the thread holding the leader word, which a drop waits for. */
    void publish_collection(collection_id_t collection_id, sealed_commit<stored_key_t, atomic_reference_>& sealed,
                            transaction_stamp_t stamp) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        collection_entry_t* const found = entry_with_id_in_(collection_entries_(), collection_id);
        std::visit(
            [&]<typename store_type_>(store_type_& collection) noexcept {
                if constexpr (keyed_by_database<store_type_>()) collection.publish(sealed, stamp);
            },
            found->held);
    }

    /** Forgets the view @c prepare_collection laid out for a commit the leader refuses. */
    void forget_collection(collection_id_t collection_id) noexcept {
        reader_t const inside = ledger_.epochs().enter();
        if (collection_entry_t* const found = entry_with_id_in_(collection_entries_(), collection_id))
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
        collection_entry_t* const found = entry_at_in_(collection_entries_(), id);
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

    /** Flushes @p collection_id's memory runs under the claim, answering whether that gave
     *  memory-run records back, which a commit refused a record waits on before it resubmits. */
    [[nodiscard]] bool flushed_records(collection_id_t collection_id) noexcept {
        std::size_t const held = memory_shape(any_collection_id_t {collection_id}).runs;
        {
            claim_guard_t const claimed {*this};
            std::size_t const position = position_at_(any_collection_id_t {collection_id});
            if (position == collections_.size()) return false;
            if (std::visit([&](auto& collection) noexcept { return flush_and_compact_(collection); },
                           collections_[position]->held)
                    .failed())
                return false;
        }
        return memory_shape(any_collection_id_t {collection_id}).runs < held;
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
     *  @return What the save placed, or @c cancelled_k, or the status that stopped it, with the
     *      target as it was.
     *
     *  Runs the target holds already under the same ID and length are kept, so a second save of a
     *  database writes only the runs written since; runs the target's previous manifest named and
     *  this one does not are given back after it.
     */
    template <typename progress_type_>
    expected<archive_report_t> save(volume_config_t const& target, transaction_stamp_t stamp,
                                    progress_type_&& progress) noexcept {
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
        std::sort(
            held_by_run.begin(), held_by_run.end(),
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
            auto [extent, place_status] = place_run_(into, channel, writing.channel(), source, held_by_run, report,
                                                     placed);
            if (place_status.failed()) {
                failure = place_status;
                break;
            }
            manifest_file_t file = source.file;
            file.offset = extent.extent.offset;
            file.medium = extent.data_directory;
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
        if (failure.succeeded()) failure = into.publish_manifest(channel, std::span<char const> {json});
        if (failure.failed()) {
            for (medium_extent_t const& extent : placed) into.deallocate(extent);
            return failure;
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

    /** One collection entry, heap-pinned so a snapshot and a live transaction may name it. */
    using entry_pointer_t = std::unique_ptr<collection_entry_t>;

    /**
     *  @brief One published list of collections, naming every entry the database held when it was laid out.
     *
     *  A lookup that reads nothing but a name, an ID or a shape needs no claim: every entry is pinned on the heap, so
     *  a create and a drop publish only a new list naming them, and free the one it replaces once every reader that
     *  loaded it has left.
     */
    struct published_collections_t {

        /** The entries, in the order @c collections_ held them. */
        st::vector<collection_entry_t*> entries;

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

        /** Where the collection sits among the collections, which no create or drop moves while the
         *  claim is held. */
        std::size_t position_ = 0;

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
        for (entry_pointer_t const& entry : collections_) [[maybe_unused]]
            st::status_t const named = next->entries.push_back(st::assume_reserved, entry.get());
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

        /** Its deltas and the run they become. */
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
            collection_entry_t const* const found = entry_at_in_(collection_entries_(), id);
            if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
            head.id = found->id;
        }
        if (status_t const sealed = seal_into_(head.id, head.sealed, sealer); sealed.failed()) return sealed;
        if (head.sealed.empty()) return {};
        commit_request_t request;
        request.context = &head;
        request.prepare = &prepare_head_;
        request.publish = &publish_head_;
        for (;;) {
            status_t const committed = ledger_.submit(request).status();
            if (committed.succeeded()) break;
            // A commit refused a memory-run record resubmits only once a flush gave records back,
            // so it never spins.
            if (committed != status_code_t::capacity_exhausted_k || !flushed_records(head.id)) return committed;
        }
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
            collection_entry_t const* const found = entry_with_id_in_(collection_entries_(), collection_id);
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
                collection_entry_t const* const found = entry_with_id_in_(collection_entries_(), collection_id);
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
        while (collection.needs_compaction()) {
            auto [outcome, compact_status] = collection.compact(reading.channel(), writing.channel(), compactor_,
                                                                published);
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
        for (entry_pointer_t const& entry : collections_)
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

    /** Where @p id sits among the collections, or their count when it names nothing. */
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

    /** Answers @p action on the store @p ingest writes into and the runs it wrote so far. */
    template <typename result_type_, typename action_type_>
    result_type_ with_ingest_(ingest_t& ingest, action_type_&& action) noexcept {
        any_collection_t& held = collections_[ingest.position_]->held;
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
        std::size_t const position = position_at_(any_collection_id_t {collection_id});
        if (position == collections_.size())
            return {status_code_t::not_found_k, "The database names no such collection"};
        return std::visit([&](auto& held) noexcept { return held.flush(channel); }, collections_[position]->held);
    }

    /** Reserves room for one run pool per store, each made at its store's first collection
     *  and never moved. */
    status_t make_run_pools_() noexcept {
        run_pools_.clear();
        if (run_pools_.resize(sizeof...(store_types_)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room for the run pools"};
        return {};
    }

    /** Publishes @p written of @p bytes as the caller's own data, and gives the data it replaces
     *  back once the manifest naming the new one is durable; a refused publication keeps the old
     *  data and drops the new. */
    status_t replace_user_data_(io_channel_t& channel, uuid_t const& run, medium_extent_t const& written,
                                std::uint64_t bytes) noexcept {
        uuid_t const replaced_run = std::exchange(user_data_run_, run);
        medium_extent_t const replaced = std::exchange(user_data_, written);
        std::uint64_t const replaced_bytes = std::exchange(user_data_bytes_, bytes);
        if (status_t const published = publish_manifest_(channel); published.failed()) {
            user_data_run_ = replaced_run;
            user_data_ = replaced;
            user_data_bytes_ = replaced_bytes;
            if (bytes != 0) volume_->deallocate(written);
            return published;
        }
        if (replaced_bytes != 0) volume_->deallocate(replaced);
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
    expected<medium_extent_t> place_run_(volume_t& into, io_channel_t& reading, io_channel_t& writing,
                                         placed_file_t const& source,
                                         st::vector<manifest_file_t const*> const& held_by_run,
                                         archive_report_t& report, st::vector<medium_extent_t>& placed) noexcept {
        manifest_file_t const* const* const found = std::lower_bound(
            held_by_run.begin(), held_by_run.end(), source.file.run,
            [](manifest_file_t const* file, uuid_t const& run) noexcept { return file->run < run; });
        if (found != held_by_run.end() && (*found)->run == source.file.run && (*found)->bytes == source.file.bytes) {
            ++report.kept;
            return into.open(**found);
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
            ++report.linked;
            return extent;
        }
        auto [extent, allocate_status] = into.allocate(source.file.run, source.extent.extent.length);
        if (allocate_status.failed()) return allocate_status;
        [[maybe_unused]] st::status_t const tracked = placed.push_back(st::assume_reserved, medium_extent_t {extent});
        if (status_t const copied = copy_extent_(reading, writing, source.extent, extent); copied.failed())
            return copied;
        ++report.written;
        return extent;
    }

    /** Reads @p from on @p reading into @p into on @p writing, a block per request, each pipeline
     *  at its channel's full depth; two channels, since a pipeline polling a shared queue pair
     *  would reap the other's completions. */
    status_t copy_extent_(io_channel_t& reading, io_channel_t& writing, medium_extent_t const& from,
                          medium_extent_t const& into) noexcept {
        disk_t& disk = volume_->disk();
        basic_read_pipeline<disk_t> reader {disk, reading, from.medium};
        basic_write_pipeline<disk_t, waiter_type_> writer {disk, writing, into.medium};
        std::uint64_t const reading_bytes = reading.requests().buffer_bytes();
        std::uint64_t const writing_bytes = writing.requests().buffer_bytes();
        std::uint64_t const block_bytes = round_down_to_multiple<std::uint64_t>(
            reading_bytes < writing_bytes ? reading_bytes : writing_bytes, portable_alignment_k);
        if (block_bytes == 0) return {status_code_t::capacity_exhausted_k, "A channel's block holds no page"};
        std::uint64_t const blocks = divide_round_up(from.extent.length, block_bytes);

        status_t carried;
        auto sink = [&](utilities::completed_buffer_t const& completed) noexcept {
            if (carried.failed()) return;
            if (status_t const whole = utilities::require_completed_in_full(completed); whole.failed()) {
                carried = whole;
                return;
            }
            auto [block, acquire_status] = writer.acquire_buffer_or_poll();
            if (acquire_status.failed()) {
                carried = acquire_status;
                return;
            }
            std::memcpy(block.bytes().data(), completed.bytes.data(), completed.bytes.size());
            std::uint64_t const offset = std::uint64_t {completed.tag} * block_bytes;
            if (status_t const queued = writer.enqueue_write(std::move(block),
                                                             into.slice(offset, completed.bytes.size()));
                queued.failed())
                carried = queued;
            else if (status_t const submitted = writer.submit(); submitted.failed()) carried = submitted;
        };
        for (std::uint64_t next = 0; next != blocks && carried.succeeded();) {
            while (next != blocks && reader.can_enqueue()) {
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
        return writer.poll_all_completions();
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
        for (entry_pointer_t const& held : collections_) {
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
    status_t open_manifest_() noexcept {
        // Every executor thread with a channel of its own reads on an epoch slot of its own too.
        if (status_t const reserved = ledger_.epochs().reserve_threads(channels_->executor_channels());
            reserved.failed())
            return reserved;
        if (status_t const made = make_run_pools_(); made.failed()) return made;
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        std::uint64_t const durable_stamp = volume_->manifest().durable_stamp;
        if (status_t const adopted = adopt_manifest_(claimed->channel()); adopted.failed()) return adopted;

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
                    [&](auto& held) noexcept {
                        return held.truncate_above(claimed->channel(), writing.channel(), durable_stamp, published);
                    },
                    collections_[index]->held);
                truncated.failed())
                return truncated;
        return publish_collections_();
    }

    /** Opens every collection and the user data the manifest the volume read names, each collection
     *  over its runs in the order the manifest lists them. */
    status_t adopt_manifest_(io_channel_t& channel) noexcept {
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

        entry_pointer_t entry {new (std::nothrow) collection_entry_t {}};
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

    /** The queue pairs every call that moves bytes takes one of, null until the database is mounted. */
    channel_pool_t* channels_ = nullptr;

    /** The delta bounds a transaction takes when its caller names none. */
    database_config_t config_ {};

    /** Every collection the database names, pinned so a snapshot may point into one. */
    st::vector<entry_pointer_t> collections_;

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
