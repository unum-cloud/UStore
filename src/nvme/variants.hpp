/**
 *  @file src/nvme/variants.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Embedded on-disk engine over the LSM templates of `include/ustore/blobs/`, one NVMe collection per
 *      collection of the C ABI, served by one variant per CPU class.
 *
 *  Every call is a template on the CPU class, whose atomic reference and waiter instantiate the store, and
 *  @c nvme_variant names the table of one class and its open. Its members are defined below it, so the explicit
 *  instantiation in `src/nvme/variants_<class>.cpp` compiles every call of the class there, and the @c extern one in
 *  `src/nvme/engine.cpp` keeps them out of the unit that picks the class.
 *
 *  One mount opens the volumes and one database of 16-byte keys lives on them. Every collection of the C ABI is one
 *  collection of that database under the same UUID, and three more are the engine's own: the main collection under
 *  the nil UUID, and two system collections. The collections system collection, `catalog_collection_k`, maps every
 *  other collection's UUID onto its name, while the store schema records its modality, which a mount needs before it
 *  opens the collection. The names system collection maps every name onto the bare UUID of its collection. A strings
 *  collection, and the names, live in the strings store of `include/ustore/strings/nvme_store.hpp`, keyed by whole
 *  strings, each mapped onto a 16-byte key held in its cell, and a transaction's strings ride in the share of it every
 *  transaction here joins.
 *
 *  Every write of a caller goes through a transaction of the database, the caller's or one the call commits alone, so
 *  the registry records every commit a watched read may conflict with. The two system collections, which no caller's
 *  transaction reaches, change under a claim of their own, the catalog straight in the store and the names through a
 *  commit watching nothing, so a collection created or dropped never conflicts with the transactions running beside
 *  it. A call outside a transaction reads at the head, or
 *  at its snapshot's stamp. Reads resolve collections at the head, as every engine does, but through a snapshot, which
 *  resolves them as they stood at its stamp. A walk merges the runs of one collection with its memory runs and,
 *  inside a transaction, with the transaction's own writes.
 *
 *  Isolation, as delivered. Every read of a transaction answers at the stamp it started at, a walk included, so it
 *  sees one committed state whatever it watches. Watching keys, the default, a commit checks every key it read or
 *  wrote, and every range it walked, the keys it did not find included, against every commit since its start, all
 *  under the ledger's leader word, and lands under a stamp drawn after that check: committed transactions are strictly
 *  serializable in the order of their stamps. Watching nothing, a transaction still reads one snapshot, but its writes
 *  land unchecked and the last writer wins. `describe` reports snapshot isolation, as the C ABI does for every engine
 *  but the in-memory one.
 *
 *  A staged transaction holds the ledger's leader word, which every other commit waits for, so a stage only checks
 *  the transaction and lets the word go, and the commit checks it again. A snapshot is a stamp pinned in the store's
 *  ledger and holds no level set: every read through it answers the newest version at or below that stamp, which the
 *  memory runs and the merges keep for as long as the pin lives, so a snapshot kept across calls stalls no writer.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <algorithm>   // `std::max`, `std::min`, `std::sort`
#include <atomic>      // `std::memory_order_acquire`
#include <chrono>      // `std::chrono::steady_clock`
#include <limits>      // `std::numeric_limits`
#include <memory>      // `std::unique_ptr`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::is_same_v`, `std::remove_reference_t`
#include <utility>     // `std::in_range`, `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "ustore/blobs.h"
#include "ustore/db.h"
#include "ustore/ranges.hpp" // `place_batch_t`, `strided`
#include "ustore/strings.h"

#include "ustore/blobs/nvme_database.hpp"      // `basic_database`, `watch_schedule_t`, `lookup_result_t`
#include "ustore/blobs/nvme_tree.hpp"          // `user_collection_config_t`, `validate`
#include "ustore/docs/nvme_store.hpp"          // `docs::nvme_store`
#include "ustore/graph/adjacency_fold.hpp"     // `vram_nvidia_adjacency_fold_t`
#include "ustore/graph/nvme_store.hpp"         // `graph::nvme_store`
#include "ustore/pages/nvme_store.hpp"         // `pages::nvme_store`
#include "ustore/strings/nvme_store.hpp"       // `strings::nvme_store`, `nvme_transaction`, `strings_page_t`
#include "ustore/utilities/blocking_files.hpp" // `directory_exists`
#include "ustore/utilities/nvme_volume.hpp"    // `resources_config_t`, `open_resources`, `mounted_storage`
#include "ustore/utilities/ram_buffers.hpp"    // `arena_allocate`, `arena_allocator`, `growth_status`

#include "engine.hpp"             // `engine_t`, `handle_t`, `handle_cast`, `view_t`, `nvme_placement_of`
#include "lib.hpp"                // `cpu_class_t`, `atomic_ref_for_cpu_class`, `waiter_of`
#include "utilities/batches.hpp"  // `strings_arg_t`, `places_of`, `validate_write`
#include "utilities/outputs.hpp"  // `read_outputs_t`, `scan_outputs_t`, `collection_list_outputs_t`
#include "utilities/progress.hpp" // `progress_t`, `progress_cancelled_k`

namespace unum::ustore::nvme {
using namespace unum::ustore::blobs;
using namespace unum::ustore::strings;
using namespace unum::ustore::utilities;

#pragma region Store

/** The key every collection of this engine stores: the 16 bytes of the C ABI's key, ordered like `memcmp`. */
using stored_key_t = st::key128_t;

/** The mount the volumes of a store of @p cpu_class_ are opened over, which lends it its router and channels. */
template <cpu_class_t cpu_class_>
using storage_mount_of =
    typename mounted_storage<atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>>::files_mount_t;

/** What composes the half-edge changes of a graph's merge on a device: the CUDA tower in a build carrying CUDA, and
 *  nothing otherwise, which keeps such merges on the host. */
#if USTORE_WITH_CUDA
using graph_device_fold_t = graph::vram_nvidia_adjacency_fold_t;
#else
using graph_device_fold_t = blobs::no_device_fold_t;
#endif

/** The store of a blobs collection, and of the main one, whose values follow @p value_layout_. */
template <cpu_class_t cpu_class_, value_layout_t value_layout_>
using blobs_store_of =
    blobs::nvme_store<key_slot<stored_key_t, value_layout_>, typename storage_mount_of<cpu_class_>::volume_t,
                      atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>, last_write_fold_t,
                      blobs::no_device_fold_t>;

/** The store of a graph collection. */
template <cpu_class_t cpu_class_>
using graph_store_of =
    graph::nvme_store<stored_key_t, typename storage_mount_of<cpu_class_>::volume_t,
                      atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>, graph_device_fold_t>;

/** The store of a docs collection. */
template <cpu_class_t cpu_class_>
using docs_store_of = docs::nvme_store<stored_key_t, typename storage_mount_of<cpu_class_>::volume_t,
                                       atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>,
                                       blobs::no_device_fold_t>;

/** The store of a pages collection. */
template <cpu_class_t cpu_class_>
using pages_store_of = pages::nvme_store<stored_key_t, typename storage_mount_of<cpu_class_>::volume_t,
                                         atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>,
                                         blobs::no_device_fold_t>;

/** The store of a strings collection, keyed by whole strings. */
template <cpu_class_t cpu_class_>
using strings_store_of =
    strings::nvme_store<typename storage_mount_of<cpu_class_>::volume_t,
                        atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>>;

/** The database every call of @p cpu_class_ runs against, whose volumes live in files, owning its words through the
 *  class's atomic reference and waiting through its waiter, and whose collections live in the one store serving
 *  their modality and value layout, a pair no store serves being refused. */
template <cpu_class_t cpu_class_>
using database_of =
    basic_database<stored_key_t, typename storage_mount_of<cpu_class_>::volume_t,
                   atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>,
                   strings_store_of<cpu_class_>, graph_store_of<cpu_class_>, docs_store_of<cpu_class_>,
                   pages_store_of<cpu_class_>, blobs_store_of<cpu_class_, value_layout_t::inline_1_byte_k>,
                   blobs_store_of<cpu_class_, value_layout_t::inline_4_bytes_k>,
                   blobs_store_of<cpu_class_, value_layout_t::inline_8_bytes_k>,
                   blobs_store_of<cpu_class_, value_layout_t::inline_16_bytes_k>,
                   blobs_store_of<cpu_class_, value_layout_t::inline_32_bytes_k>,
                   blobs_store_of<cpu_class_, value_layout_t::inline_64_bytes_k>,
                   blobs_store_of<cpu_class_, value_layout_t::fixed_width_k>,
                   blobs_store_of<cpu_class_, value_layout_t::variable_width_k>>;

/** One transaction over the collections of that database. */
template <cpu_class_t cpu_class_>
using database_transaction_of = typename database_of<cpu_class_>::transaction_t;

/** The strings one transaction of that database writes and watches, joined to it as a share. */
template <cpu_class_t cpu_class_>
using strings_transaction_of = nvme_transaction<database_of<cpu_class_>, strings_store_of<cpu_class_>>;

/** The collections system collection, the catalog, mapping every collection's UUID onto its name, which no minted
 *  UUIDv7 spells, as its byte 6 is `0x7X`. */
inline constexpr ustore_collection_id_t catalog_collection_k {
    {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFD}}};

/** The names system collection, a strings collection mapping every collection's name onto its UUID, which no minted
 *  UUIDv7 spells either. */
inline constexpr ustore_collection_id_t names_collection_k {
    {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE}}};

/** The directory a mount opens its only volume in, under the database's own directory. */
inline constexpr char data_directory_name_k[] = "data";

/** Bytes the only volume of a database spans when its configuration names no capacity. */
inline constexpr std::size_t default_volume_bytes_k = 1024ull * 1024ull * 1024ull;

/** Channels the threads outside an executor share, beyond one per thread of the executor opening the database. */
inline constexpr std::size_t engine_shared_io_channels_k = 4;

/** Bytes the largest value of a collection takes. */
inline constexpr stored_value_size_t engine_value_max_size_k = 1024u * 1024u;

/** Bytes one collection's memory runs hold before a flush is due, and bytes one collection's value cache holds. */
inline constexpr std::size_t engine_collection_bytes_k = 16ull * 1024ull * 1024ull;

/** Deltas one collection's memory runs hold before a flush is due. */
inline constexpr std::size_t engine_memory_runs_deltas_k = 64ull * 1024ull;

/** Bytes one caller's transaction buffers, which it takes eagerly when it opens. */
inline constexpr std::size_t engine_transaction_bytes_k = 4ull * 1024ull * 1024ull;

/** Deltas one caller's transaction buffers at once. */
inline constexpr std::size_t engine_transaction_deltas_k = 16ull * 1024ull;

/** Times a call retries a commit of its own whose watched reads conflicted. */
inline constexpr std::size_t commit_attempts_k = 64;

/** Times a read measures and copies a value that keeps changing its size in between. */
inline constexpr std::size_t read_attempts_k = 8;

/** Keys a clear rewrites per commit, so its transaction stays bounded however large the collection. */
inline constexpr std::size_t clear_window_keys_k = 4096;

/** Merges a compaction pass after a write runs at most, so a pass another thread holds never spins a writer. */
inline constexpr std::size_t compaction_passes_k = 8;

/** The bytes an empty present value points at, as a missing value alone points at none. */
inline constexpr byte_t empty_value_bytes_k[1] {};

/** The stored key holding the 16 bytes of @p uuid. */
inline stored_key_t stored_of(ustore_uuid_t const& uuid) noexcept {
    return stored_key_t::from_bytes(std::as_bytes(std::span<std::uint8_t const, 16> {uuid.bytes}));
}

/** The key of the C ABI whose 16 bytes @p stored holds. */
inline ustore_key_t key_of(stored_key_t const& stored) noexcept {
    ustore_key_t key;
    stored.to_bytes(std::as_writable_bytes(std::span<std::uint8_t, 16> {key.uuid.bytes}));
    return key;
}

/** The shape of a strings collection: @p shape, every value of which is one 16-byte key. */
inline collection_config_t strings_shape_of(collection_config_t shape) noexcept {
    sorted_run_config_t& values = shape.lsm_tree.lsm_level_0.sorted_run;
    values.value_size = sizeof(ustore_uuid_t);
    values.value_max_size = 0;
    return shape;
}

/** Whether @p collection is one the engine keeps for itself, which no call of the C ABI may address. */
inline bool reserved(ustore_collection_id_t const& collection) noexcept {
    return collection == catalog_collection_k || collection == names_collection_k;
}

/** The shape every collection of a database takes unless its open names another. */
inline user_collection_config_t engine_collection_defaults() noexcept {
    user_collection_config_t config;
    config.cache_limit = engine_collection_bytes_k;
    config.memory_runs_max_deltas = engine_memory_runs_deltas_k;
    config.memory_runs_max_bytes = engine_collection_bytes_k;
    config.value_max_size = engine_value_max_size_k;
    return config;
}

#pragma endregion Store

#pragma region Handles

/** The calls a handle of @p cpu_class_ is served by, and the open that hands such handles out; both are defined below,
 *  so only the class's own unit compiles them. */
template <cpu_class_t cpu_class_>
struct nvme_variant {

    /** The database verbs of this class. */
    static database_verbs_t const database_verbs_k;

    /** The unchecked whole-value reads, removals and key scans of this class, through each collection's own store,
     *  which its docs, graph and pages verbs reach after their resolver. */
    static blobs_verbs_t const values_verbs_k;

    /** The verbs of this class's blobs collections. */
    static blobs_verbs_t const blobs_verbs_k;

    /** The verbs of this class's strings collections. */
    static strings_verbs_t const strings_verbs_k;

    /** The verbs of this class's documents collections. */
    static docs_verbs_t const docs_verbs_k;

    /** The verbs of this class's graph collections. */
    static graph_verbs_t const graph_verbs_k;

    /** The verbs of this class's pages collections. */
    static pages_verbs_t const pages_verbs_k;

    /** The table every handle the open of this class hands out is stamped with. */
    static engine_t const engine_k;

    /** Implements @c ustore_database_open_nvme over a store of this class. */
    static status_t open(ustore_database_open_nvme_t const& arguments, view_t const& view) noexcept;
};

/** Everything one open database holds, which is the database handle of the C ABI. */
template <cpu_class_t cpu_class_>
struct database_state : handle_t {

    /** The budgets and the mount the store borrows its router and channels from. */
    resources_shared_ptr<atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>> resources;

    /** The store itself, unmounted before the mount underneath it is torn down. */
    std::unique_ptr<database_of<cpu_class_>> store;

    /** The shape every collection this database creates takes. */
    collection_config_t collection_shape;

    /** The buffer every caller's transaction takes. */
    transaction_config_t transaction_shape;

    /** The executor that opened the database, whose threads read on a channel and an epoch slot of their own, or
     *  null; compared by address alone, as one made where a freed one was has threads as exclusive, and any index
     *  past the threads the database counted reads as an outside thread's would. */
    call_executor_t const* executor = nullptr;

    /** Held by whoever changes the catalog and the names, so a name is checked and taken in one step. */
    std::int32_t catalog_claim = 0;

    /** Snapshots not yet freed, through the class's atomic reference. */
    std::size_t open_snapshots = 0;

    /** Collections the catalog no longer lists whose runs an open snapshot may still read, dropped once the last
     *  snapshot is freed; guarded by @c catalog_claim. */
    st::basic_vector<ustore_collection_id_t> retired_collections;

    database_state() noexcept : handle_t {&nvme_variant<cpu_class_>::engine_k} {}
    database_state(database_state const&) = delete;
    database_state& operator=(database_state const&) = delete;

    /** Flushes and unmounts the store while its mount is still there, then lets both go. */
    ~database_state() noexcept {
        if (store) { [[maybe_unused]] status_t const unmounted = store->unmount(); }
        store.reset();
        resources.reset();
    }
};

/** A transaction handle of the C ABI. */
template <cpu_class_t cpu_class_>
struct transaction_handle : handle_t {

    /** The database the transaction runs against. */
    database_state<cpu_class_>& state;

    /** The strings the transaction writes and watches, joined to it, and outliving it. */
    strings_transaction_of<cpu_class_> strings;

    /** The transaction itself, retired when the handle dies. */
    typename database_of<cpu_class_>::transaction_unique_ptr_t held;

    /** Whether the caller staged it, which a rollback withdraws and a commit consumes, refusing writes meanwhile. */
    bool staged = false;

    transaction_handle(database_state<cpu_class_>& owner,
                       typename database_of<cpu_class_>::transaction_unique_ptr_t&& opened) noexcept
        : handle_t {&nvme_variant<cpu_class_>::engine_k}, state(owner), strings(*owner.store), held(std::move(opened)) {
        strings.join(*held);
    }
};

/** A snapshot handle of the C ABI, pinning the stamp it reads at in the store's ledger until freed. */
template <cpu_class_t cpu_class_>
struct snapshot_handle : handle_t {

    /** The database the snapshot reads. */
    database_state<cpu_class_>& state;

    /** The pin, which keeps every version a read at its stamp needs out of the reach of a flush and a merge. */
    typename database_of<cpu_class_>::pin_t pin;

    explicit snapshot_handle(database_state<cpu_class_>& owner) noexcept
        : handle_t {&nvme_variant<cpu_class_>::engine_k}, state(owner) {}
};

/** Holds the catalog of one database for the life of the guard, waiting while another change holds it. */
template <cpu_class_t cpu_class_>
class catalog_guard {
    std::int32_t& claim_;

  public:
    explicit catalog_guard(database_state<cpu_class_>& state) noexcept : claim_(state.catalog_claim) {
        atomic_ref_of<cpu_class_, std::int32_t> const word {claim_};
        std::int32_t expected_free = 0;
        while (!word.compare_exchange_weak(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
            waiter_of<cpu_class_> {}(&claim_, expected_free, std::size_t {0});
            expected_free = 0;
        }
    }
    catalog_guard(catalog_guard const&) = delete;
    catalog_guard& operator=(catalog_guard const&) = delete;
    ~catalog_guard() noexcept { atomic_ref_of<cpu_class_, std::int32_t> {claim_}.store(0, std::memory_order_release); }
};

/** The database a call addresses. */
template <cpu_class_t cpu_class_, typename arguments_type_>
database_state<cpu_class_>& state_of(arguments_type_ const& arguments) noexcept {
    return *handle_cast<database_state<cpu_class_>>(ustore::database_of(arguments));
}

/** Opens a transaction of @p state whose buffer @p shape bounds. */
template <cpu_class_t cpu_class_>
expected<typename database_of<cpu_class_>::transaction_unique_ptr_t> begin_transaction(
    database_state<cpu_class_>& state, transaction_config_t const& shape) noexcept {
    return state.store->create_transaction(shape);
}

#pragma endregion Handles

#pragma region Access

/** Where one call reads and writes: through a transaction, the caller's or its own, or at the head for reads. */
template <cpu_class_t cpu_class_>
struct call_access {

    /** The database the call runs against. */
    database_state<cpu_class_>& state;

    /** The transaction it reads and writes through, or null to read at the head. */
    database_transaction_of<cpu_class_>* transaction = nullptr;

    /** Whether the transaction watches what the call reads and writes. */
    watch_schedule_t schedule = watch_schedule_t::never_k;

    /** Where the call keeps the values it read until it exports them. */
    ustore_arena_t scratch = nullptr;

    /** The stamp a call outside a transaction reads at: a snapshot's, or the head's. */
    transaction_stamp_t stamp = head_stamp_k;

    /** The thread of the executor that opened the database running the call, which reads on its own channel and
     *  epoch slot, or @c outside_executor_k for any other thread. */
    std::size_t thread_index = outside_executor_k;

    /** The strings the transaction writes and watches, null with no transaction. */
    strings_transaction_of<cpu_class_>* strings = nullptr;
};

/** The schedule the watches of a call asking for @p watch take. */
constexpr watch_schedule_t schedule_of(ustore_watch_t watch) noexcept {
    return watch == ustore_watch_keys_k ? watch_schedule_t::on_commit_k : watch_schedule_t::never_k;
}

/** The thread index a call running on @p view's thread reads @p state with. */
template <cpu_class_t cpu_class_>
std::size_t thread_index_of(database_state<cpu_class_> const& state, view_t const& view) noexcept {
    return view.executor && view.executor == state.executor ? view.thread_index : outside_executor_k;
}

/** The access of a call reading through @p view: its transaction, its snapshot's stamp, or the head. */
template <cpu_class_t cpu_class_>
call_access<cpu_class_> access_of(database_state<cpu_class_>& state, view_t const& view) noexcept {
    std::size_t const thread_index = thread_index_of(state, view);
    if (view.snapshot)
        return {state,
                nullptr,
                watch_schedule_t::never_k,
                view.scratch,
                handle_cast<snapshot_handle<cpu_class_>>(view.snapshot)->pin.stamp(),
                thread_index};
    if (!view.transaction) return {state, nullptr, watch_schedule_t::never_k, view.scratch, head_stamp_k, thread_index};
    transaction_handle<cpu_class_>& handle = *handle_cast<transaction_handle<cpu_class_>>(view.transaction);
    return {state,        handle.held.get(), schedule_of(view.watch), view.scratch,
            head_stamp_k, thread_index,      &handle.strings};
}

/** Watches @p keys of @p collection through the transaction of @p access, when it watches at all. */
template <cpu_class_t cpu_class_>
status_t watch_keys(call_access<cpu_class_> const& access, collection_id_t collection,
                    std::span<stored_key_t const> keys) noexcept {
    if (!access.transaction || access.schedule == watch_schedule_t::never_k || keys.empty()) return {};
    return access.transaction->template watch_more<watch_schedule_t::on_commit_k>(collection, keys);
}

/** Watches every key of @p collection from @p lower to @p upper through the transaction of @p access, when it watches
 *  at all, so a commit landing a key the walk over them did not find conflicts too. */
template <cpu_class_t cpu_class_>
status_t watch_range(call_access<cpu_class_> const& access, collection_id_t collection, stored_key_t lower,
                     stored_key_t upper) noexcept {
    if (!access.transaction || access.schedule == watch_schedule_t::never_k) return {};
    return access.transaction->template watch_range<watch_schedule_t::on_commit_k>(collection, lower, upper);
}

/** Copies @p size bytes into the scratch of @p access through @p copy, answering the present value they hold. */
template <cpu_class_t cpu_class_, typename copy_type_>
expected<value_view_t> copied_value(call_access<cpu_class_> const& access, stored_value_size_t size,
                                    copy_type_&& copy) noexcept {
    if (size == 0) {
        if (status_t const copied = copy(std::span<byte_t> {}); copied.failed()) return copied;
        return value_view_t {empty_value_bytes_k, std::size_t {0}};
    }
    auto [bytes, allocate_status] = arena_allocate<byte_t>(access.scratch, size);
    if (allocate_status.failed()) return allocate_status;
    if (status_t const copied = copy(bytes); copied.failed()) return copied;
    return value_view_t {bytes};
}

/** The value a transaction's change @p own of a key in @p collection folds @p committed into, through the fold of the
 *  store holding it, copied into the scratch of @p access, or a missing value where the fold leaves none. */
template <cpu_class_t cpu_class_>
expected<value_view_t> fold_own_change(call_access<cpu_class_> const& access, collection_id_t collection,
                                       value_view_t committed, delta_view_t const& own) noexcept {
    // A present value of no bytes still views a byte, so a fold tells it from a missing one
    std::span<byte_t const> base;
    if (committed)
        base = committed.size() ? std::span<byte_t const> {committed.begin(), committed.size()}
                                : std::span<byte_t const> {empty_value_bytes_k, 0};
    expected<value_view_t> folded = value_view_t {};
    status_t const visited = access.state.store->visit_store(
        collection,
        [&]<typename store_type_>(store_type_&) noexcept -> status_t {
            arena_allocator<byte_t> arena {access.scratch};
            auto [applied, apply_status] = typename store_type_::fold_t {}.apply(
                base, std::span<delta_view_t const> {&own, 1}, arena);
            if (apply_status == status_code_t::not_found_k) return {};
            if (apply_status.failed()) return apply_status;
            folded = copied_value(access, static_cast<stored_value_size_t>(applied.size()),
                                  [&](std::span<byte_t> into) noexcept {
                                      if (!into.empty()) std::memcpy(into.data(), applied.data(), into.size());
                                      return status_t {};
                                  });
            return {};
        },
        access.thread_index);
    if (visited.failed()) return visited;
    return folded;
}

/**
 *  @brief The value of @p key in @p collection as @p access sees it, copied into its scratch, or a missing value.
 *
 *  A transaction's own write outranks the store, its own removal hides the store's value, and its own change folds
 *  over the store's value. A read through a watching transaction watches the key, its absence included, so a commit
 *  landing on it in between conflicts.
 */
template <cpu_class_t cpu_class_>
expected<value_view_t> read_value(call_access<cpu_class_> const& access, collection_id_t collection,
                                  stored_key_t key) noexcept {
    database_of<cpu_class_>& store = *access.state.store;
    database_transaction_of<cpu_class_>* const transaction = access.transaction;
    delta_view_t own;
    bool const changed = transaction && transaction->uncommitted_delta(collection, key, own) &&
                         own.kind == delta_kind_t::change_k;
    if (transaction) {
        lookup_result_t const own_status = transaction->lookup_uncommitted(collection, key);
        if (own_status == lookup_result_t::removed_k) return value_view_t {};
        if (own_status == lookup_result_t::written_k && !changed) {
            stored_value_size_t size = 0;
            if (status_t const measured = transaction->stored_value_size_uncommitted(collection, key, size);
                measured.failed())
                return measured;
            return copied_value(access, size, [&](std::span<byte_t> into) noexcept {
                stored_value_size_t copied = 0;
                return transaction->select_uncommitted(collection, key, into, copied);
            });
        }
    }

    // A value may change its size between the measurement and the copy, which the copy refuses rather than truncates
    for (std::size_t attempt = 0; attempt != read_attempts_k; ++attempt) {
        stored_value_size_t size = 0;
        std::size_t const thread_index = access.thread_index;
        status_t const measured = transaction
                                      ? transaction->stored_value_size_committed(collection, key, size, thread_index)
                                      : store.stored_value_size(collection, key, size, access.stamp, thread_index);
        bool const missing = measured == status_code_t::not_found_k;
        if (measured.failed() && !missing) return measured;
        if (!missing) {
            expected<value_view_t> read = copied_value(access, size, [&](std::span<byte_t> into) noexcept {
                stored_value_size_t copied = 0;
                if (!transaction) return store.select(collection, key, into, copied, access.stamp, thread_index);
                if (access.schedule == watch_schedule_t::never_k)
                    return transaction->template select_committed<watch_schedule_t::never_k>(collection, key, into,
                                                                                             copied, thread_index);
                return transaction->template select_committed<watch_schedule_t::on_commit_k>(collection, key, into,
                                                                                             copied, thread_index);
            });
            if (read && changed) return fold_own_change(access, collection, *read, own);
            if (read) return read;
            if (read.status() == status_code_t::invalid_argument_k) continue;
            if (read.status() != status_code_t::not_found_k) return read.status();
        }
        stored_key_t const absent[1] {key};
        if (status_t const watched = watch_keys(access, collection, absent); watched.failed()) return watched;
        if (changed) return fold_own_change(access, collection, value_view_t {}, own);
        return value_view_t {};
    }
    return status_t {status_code_t::contended_k, "A value kept changing its size while it was read"};
}

/** Writes @p value under @p key of @p collection, a missing value removing it: through the transaction of @p access,
 *  or straight into the store for the catalog and the names, which no transaction reaches. */
template <cpu_class_t cpu_class_>
status_t write_value(call_access<cpu_class_> const& access, collection_id_t collection, stored_key_t key,
                     value_view_t value) noexcept {
    stored_value_view_t const bytes {reinterpret_cast<byte_t const*>(value.data()), value.size()};
    if (!access.transaction) {
        database_of<cpu_class_>& store = *access.state.store;
        return value ? store.insert(collection, key, bytes) : store.remove(collection, key);
    }
    database_transaction_of<cpu_class_>& transaction = *access.transaction;
    bool const watched = access.schedule != watch_schedule_t::never_k;
    if (!value)
        return watched ? transaction.template remove<watch_schedule_t::on_commit_k>(collection, key)
                       : transaction.template remove<watch_schedule_t::never_k>(collection, key);
    return watched ? transaction.template insert<watch_schedule_t::on_commit_k>(collection, key, bytes)
                   : transaction.template insert<watch_schedule_t::never_k>(collection, key, bytes);
}

/** One write of a transaction to its own key, which a walk merges with the store. */
struct own_write_t {
    stored_key_t key;
    stored_value_size_t size = 0;
    delta_kind_t kind = delta_kind_t::write_k;
};

/**
 *  @brief Hands @p visitor every present key of @p collection from @p from up, as @p access sees it, in ascending
 *      order, as @c visitor(key, size) answering whether the walk carries on.
 *
 *  The store's walk holds the view it loaded, which a flush waits for, so @p visitor collects and calls nothing of the
 *  database.
 */
template <cpu_class_t cpu_class_, typename visitor_type_>
status_t walk(call_access<cpu_class_> const& access, collection_id_t collection, stored_key_t from,
              visitor_type_&& visitor) noexcept {
    database_of<cpu_class_>& store = *access.state.store;
    if (!access.transaction)
        return store.scan(
            collection, from,
            [&](stored_key_t key, lookup_result_t state, stored_value_size_t size) noexcept {
                return state != lookup_result_t::written_k || visitor(key, size);
            },
            access.stamp, access.thread_index);

    st::basic_vector<own_write_t, arena_allocator<own_write_t>> own {{access.scratch}};
    status_t listed;
    access.transaction->for_each_uncommitted(
        collection, [&](stored_key_t key, delta_kind_t kind, stored_value_size_t size) noexcept {
            if (key < from || listed.failed()) return;
            listed = growth_status(own.push_back(own_write_t {key, size, kind}));
        });
    if (listed.failed()) return listed;
    // A change folds over the committed value, which tells its size and whether any value is left
    for (own_write_t& write : own) {
        if (write.kind != delta_kind_t::change_k) continue;
        auto [folded, fold_status] = read_value(access, collection, write.key);
        if (fold_status.failed()) return fold_status;
        write.kind = folded ? delta_kind_t::write_k : delta_kind_t::remove_k;
        write.size = static_cast<stored_value_size_t>(folded.size());
    }
    std::sort(own.begin(), own.end(),
              [](own_write_t const& first, own_write_t const& second) noexcept { return first.key < second.key; });

    // The transaction's writes outrank the store's version of the same key, and its removals hide it
    std::size_t next = 0;
    bool carries_on = true;
    auto const emit_own = [&](own_write_t const& write) noexcept {
        if (write.kind != delta_kind_t::remove_k) carries_on = visitor(write.key, write.size);
        return carries_on;
    };
    // The store's side is what the transaction started at, which its reads of single keys answer too
    status_t const walked = store.scan(
        collection, from,
        [&](stored_key_t key, lookup_result_t state, stored_value_size_t size) noexcept {
            for (; next != own.size() && own[next].key < key; ++next)
                if (!emit_own(own[next])) return false;
            if (next != own.size() && own[next].key == key) return emit_own(own[next++]);
            if (state != lookup_result_t::written_k) return true;
            carries_on = visitor(key, size);
            return carries_on;
        },
        access.transaction->start_stamp(), access.thread_index);
    if (walked.failed()) return walked;
    for (; carries_on && next != own.size(); ++next) emit_own(own[next]);
    return {};
}

/** Collects into @p keys up to @p limit present keys of @p collection from @p from up, as @p access sees them. */
template <cpu_class_t cpu_class_, typename keys_type_>
status_t collect_keys(call_access<cpu_class_> const& access, collection_id_t collection, stored_key_t from,
                      std::size_t limit, keys_type_& keys) noexcept {
    status_t room;
    status_t const walked = walk(access, collection, from, [&](stored_key_t key, stored_value_size_t) noexcept {
        room = growth_status(keys.push_back(stored_key_t {key}));
        return room.succeeded() && keys.size() < limit;
    });
    return walked.failed() ? walked : room;
}

/** The key after @p key, or answers false for the largest key, which has none. */
inline bool advance_key(stored_key_t& key) noexcept {
    if (key == limit<stored_key_t>::max()) return false;
    key = stored_of(key_after(key_of(key)).uuid);
    return true;
}

#pragma endregion Access

#pragma region Maintenance

/** Merges every collection due for a compaction, a bounded number of passes, on the calling thread. */
template <cpu_class_t cpu_class_>
status_t compact_due(database_state<cpu_class_>& state) noexcept {
    for (std::size_t pass = 0; pass != compaction_passes_k && state.store->needs_compaction(); ++pass)
        if (status_t const compacted = state.store->compact(); compacted.failed()) return compacted;
    return {};
}

/** Flushes @p state when @p durability asks for it, then compacts what the write left due. */
template <cpu_class_t cpu_class_>
status_t settle(database_state<cpu_class_>& state, ustore_durability_t durability) noexcept {
    if (durability == ustore_durability_flushed_k)
        if (status_t const flushed = state.store->flush(); flushed.failed()) return flushed;
    return compact_due(state);
}

/**
 *  @brief Commits what @p writes writes through a transaction of its own, retrying conflicts of its watched reads.
 *  @param[in] count The writes @p writes makes at most, sizing the transaction's buffer.
 *  @param[in] bytes The bytes those writes take, a hint the buffer doubles past when @p writes outgrows it.
 *  @param[in] schedule Whether @p writes watches what it reads, which a read-modify-write needs.
 *
 *  @p writes is invoked as `writes(access)` on every attempt, and a failure it answers ends the call.
 */
template <cpu_class_t cpu_class_, typename writes_type_>
status_t commit_alone(database_state<cpu_class_>& state, ustore_arena_t scratch, std::size_t count, std::size_t bytes,
                      watch_schedule_t schedule, writes_type_&& writes) noexcept {
    transaction_config_t shape;
    shape.max_deltas = std::max<std::size_t>(count, 1);
    shape.max_bytes = std::max<std::size_t>(bytes, 1);
    for (std::size_t attempt = 0; attempt != commit_attempts_k; ++attempt) {
        // Declared first, so the transaction it joins dies before it does.
        strings_transaction_of<cpu_class_> strings {*state.store};
        auto [held, begin_status] = begin_transaction(state, shape);
        if (begin_status.failed()) return begin_status;
        strings.join(*held);
        call_access<cpu_class_> const access {state,        held.get(),         schedule, scratch,
                                              head_stamp_k, outside_executor_k, &strings};
        status_t const written = writes(access);
        if (written == status_code_t::capacity_exhausted_k &&
            shape.max_bytes < storage_limits_t::transaction_max_bytes_k) {
            shape.max_bytes *= 2;
            shape.max_deltas = std::min(shape.max_deltas * 2, storage_limits_t::transaction_max_deltas_k);
            continue;
        }
        if (written.failed()) return written;
        status_t const committed = strings.commit(*held);
        if (committed == status_code_t::write_conflict_k && schedule != watch_schedule_t::never_k) continue;
        return committed;
    }
    return {status_code_t::write_conflict_k, "Concurrent commits kept conflicting with the call"};
}

/** Writes @p writes into the transaction @p view carries, or commits them as a transaction of their own, flushed when
 *  @p durability asks, for every call that writes. */
template <cpu_class_t cpu_class_, typename writes_type_>
status_t serve_write(database_state<cpu_class_>& state, view_t const& view, ustore_durability_t durability,
                     std::size_t count, std::size_t bytes, watch_schedule_t schedule, writes_type_&& writes) noexcept {
    if (view.transaction) {
        transaction_handle<cpu_class_>& handle = *handle_cast<transaction_handle<cpu_class_>>(view.transaction);
        if (handle.staged)
            return {status_code_t::invalid_argument_k, "A staged transaction takes no writes until rolled back"};
        call_access<cpu_class_> access = access_of(state, view);
        if (schedule != watch_schedule_t::never_k) access.schedule = schedule;
        return writes(access);
    }
    if (status_t const committed = commit_alone(state, view.scratch, count, bytes, schedule, writes);
        committed.failed())
        return committed;
    return settle(state, durability);
}

#pragma endregion Maintenance

#pragma region Catalog

/** What the engine knows of a listed collection: its modality, which the store records, and its name, which the
 *  catalog records, empty for an anonymous one. */
struct catalog_entry_t {
    ustore_modality_t modality = ustore_modality_any_k;
    std::string_view name;
};

/** The entry of @p collection, whose catalog listing is @p held, refusing one the store no longer holds. */
template <cpu_class_t cpu_class_>
expected<catalog_entry_t> read_catalog_entry(database_state<cpu_class_>& state,
                                             ustore_collection_id_t const& collection, value_view_t held) noexcept {
    catalog_entry_t entry {ustore_modality_any_k, std::string_view {held}};
    if (status_t const known = state.store->collection_modality(collection, entry.modality); known.failed())
        return known;
    return entry;
}

/** What the catalog records of @p collection at @p stamp, refusing one it does not list, the reserved ones included. */
template <cpu_class_t cpu_class_>
expected<catalog_entry_t> look_up(database_state<cpu_class_>& state, ustore_arena_t scratch,
                                  ustore_collection_id_t const& collection, transaction_stamp_t stamp = head_stamp_k,
                                  std::size_t thread_index = outside_executor_k) noexcept {
    if (collection == ustore_collection_main_k) return catalog_entry_t {};
    call_access<cpu_class_> const listed {state, nullptr, watch_schedule_t::never_k, scratch, stamp, thread_index};
    auto [held, read_status] = read_value(listed, catalog_collection_k, stored_of(collection.uuid));
    if (read_status.failed()) return read_status;
    if (!held) return status_t {status_code_t::not_found_k, "No such collection"};
    return read_catalog_entry(state, collection, held);
}

/** Refuses a batch addressing a collection the catalog does not list, as @p view reads it, or one @c admit_modality
 *  refuses for @p modality, looking each run of tasks on one collection up once. */
template <cpu_class_t cpu_class_>
status_t resolve_runs(database_state<cpu_class_>& state, view_t const& view,
                      strided_iterator<ustore_collection_id_t const> collections, std::size_t count,
                      ustore_modality_t modality) noexcept {
    transaction_stamp_t const stamp = view.snapshot
                                          ? handle_cast<snapshot_handle<cpu_class_>>(view.snapshot)->pin.stamp()
                                          : head_stamp_k;
    for (std::size_t index = 0; index != count; ++index) {
        if (index != 0 && collections && collections[index] == collections[index - 1]) continue;
        ustore_collection_id_t const collection = collections ? collections[index] : ustore_collection_main_k;
        auto [entry, look_up_status] = look_up(state, view.scratch, collection, stamp, thread_index_of(state, view));
        if (look_up_status.failed()) return look_up_status;
        if (status_t const admitted = admit_modality(entry.modality, modality); admitted.failed()) return admitted;
        if (!collections) return {};
    }
    return {};
}

/** The resolver of this engine, which its docs and pages verbs run first. */
template <cpu_class_t cpu_class_>
status_t resolve_modality(ustore_database_t database, ustore_collections_view_t collections, ustore_size_t count,
                          ustore_modality_t modality, view_t const& view) noexcept {
    return resolve_runs(*handle_cast<database_state<cpu_class_>>(database), view, strided(collections), count,
                        modality);
}

/** The collection task @p index of @p collections names, the main one when they name none. */
inline ustore_collection_id_t task_collection(strided_iterator<ustore_collection_id_t const> collections,
                                              std::size_t index) noexcept {
    return collections ? collections[index] : ustore_collection_main_k;
}

#pragma endregion Catalog

#pragma region Strings

/** One string a batch writes or removes, which the last task naming it wins. */
struct string_task_t {
    ustore_collection_id_t collection;
    std::string_view text;

    /** Whether the task writes @c value or removes the string. */
    delta_kind_t kind = delta_kind_t::remove_k;

    /** The 16 bytes of the key the string maps onto, nil for a removal. */
    ustore_uuid_t value {};
};

/** Calls @p visitor with the strings store of @p collection inside the database's epochs, which keep it alive until
 *  @p visitor returns, answering what it answers, or refuses a collection another store keeps. */
template <cpu_class_t cpu_class_, typename visitor_type_>
status_t visit_strings(call_access<cpu_class_> const& access, collection_id_t collection,
                       visitor_type_&& visitor) noexcept {
    return access.state.store->visit_store(
        collection,
        [&]<typename store_type_>(store_type_& store) noexcept -> status_t {
            if constexpr (std::is_same_v<store_type_, strings_store_of<cpu_class_>>) return visitor(store);
            else return collection_holds_another_modality();
        },
        access.thread_index);
}

/** The stamp a read through @p access answers at: its transaction's start, its snapshot's, or the head's. */
template <cpu_class_t cpu_class_>
transaction_stamp_t read_stamp_of(call_access<cpu_class_> const& access) noexcept {
    return access.transaction ? access.transaction->start_stamp() : access.stamp;
}

/** The stamp a commit after which conflicts with what @p access reads and writes, or none when it watches nothing. */
template <cpu_class_t cpu_class_>
transaction_stamp_t watch_stamp_of(call_access<cpu_class_> const& access) noexcept {
    bool const watched = access.transaction && access.schedule != watch_schedule_t::never_k;
    return watched ? access.transaction->start_stamp() : invalid_transaction_stamp_k;
}

/** Writes or removes the string of every task of @p tasks through the strings of @p access's transaction, in batch
 *  order, so the last task naming a string wins, each watched on its own as @p access watches. */
template <cpu_class_t cpu_class_>
status_t stage_strings(call_access<cpu_class_> const& access, std::span<string_task_t const> tasks) noexcept {
    transaction_stamp_t const watch_stamp = watch_stamp_of(access);
    for (string_task_t const& task : tasks) {
        delta_view_t const delta = task.kind == delta_kind_t::write_k
                                       ? delta_view_t {delta_kind_t::write_k,
                                                       std::as_bytes(std::span {task.value.bytes})}
                                       : delta_view_t {delta_kind_t::remove_k, {}};
        if (status_t const written = access.strings->write(task.collection, task.text, delta, watch_stamp);
            written.failed())
            return written;
    }
    return {};
}

/**
 *  @brief The key @p text maps onto in @p collection as @p access sees it, and whether any write carries it.
 *
 *  A transaction's own write outranks the store, and its own removal hides the store's key. A read through a watching
 *  transaction watches the string, its absence included, so a commit landing on it in between conflicts.
 */
template <cpu_class_t cpu_class_>
status_t read_string(call_access<cpu_class_> const& access, collection_id_t collection, std::string_view text,
                     ustore_uuid_t& value, bool& present) noexcept {
    std::span<byte_t, sizeof(ustore_uuid_t)> const bytes = std::as_writable_bytes(std::span {value.bytes});
    delta_view_t own;
    if (access.strings && access.strings->uncommitted(collection, text, own)) {
        present = own.kind != delta_kind_t::remove_k;
        if (present) std::memcpy(bytes.data(), own.bytes.data(), bytes.size());
        return {};
    }
    transaction_stamp_t const stamp = read_stamp_of(access);
    if (transaction_stamp_t const watch_stamp = watch_stamp_of(access); watch_stamp != invalid_transaction_stamp_k)
        if (status_t const watched = access.strings->watch(collection, text, watch_stamp); watched.failed())
            return watched;
    return visit_strings(access, collection, [&](auto const& store) noexcept -> status_t {
        present = store.find(text, bytes, stamp, access.thread_index) == lookup_result_t::written_k;
        return {};
    });
}

/**
 *  @brief Hands @p visitor every string of @p collection starting with @p prefix and not below @p start, in order, as
 *      the store holds them at @p stamp, each with what its newest version carries, a removal included.
 *  @param[in] visitor Called as @c visitor(text, state, value) outside the epochs, answering whether the walk carries
 *      on, with the key the string maps onto; the text lives until it returns.
 *
 *  A page is read inside the epochs at a time, finding the collection again for each, so a commit waits for one page
 *  at most, and a head walk pins the stamp it starts at, so every page reads the same commits.
 */
template <cpu_class_t cpu_class_, typename visitor_type_>
status_t walk_strings(call_access<cpu_class_> const& access, collection_id_t collection, std::string_view prefix,
                      std::string_view start, transaction_stamp_t stamp, visitor_type_&& visitor) noexcept {
    typename database_of<cpu_class_>::pin_t pin;
    if (stamp == head_stamp_k) stamp = access.state.store->ledger().pin_reader(pin);
    strings_page_t page;
    st::basic_vector<char> from;
    std::string_view const first = std::max(start, prefix);
    if (from.resize(first.size()) != st::status_t::success_k)
        return {status_code_t::out_of_memory_k, "A walk could not hold where it starts"};
    if (!first.empty()) std::memcpy(from.data(), first.data(), first.size());
    for (;;) {
        std::string_view const resumed {from.begin(), from.end()};
        if (status_t const read = visit_strings(access, collection,
                                                [&](auto const& store) noexcept {
                                                    return store.read_page(prefix, resumed, stamp, access.thread_index,
                                                                           page);
                                                });
            read.failed())
            return read;
        for (std::size_t index = 0; index != page.size(); ++index)
            if (!visitor(page.text(index), page.listed[index].state, page.listed[index].value)) return {};
        if (page.size() != strings_page_k) return {};
        // The smallest string past the last one listed is that string and one zero byte more.
        std::string_view const last = page.text(page.size() - 1);
        if (from.resize(last.size() + 1) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A walk could not hold where it resumes"};
        if (!last.empty()) std::memcpy(from.data(), last.data(), last.size());
        from[last.size()] = '\0';
    }
}

/**
 *  @brief Appends to @p outputs up to @p limit strings of @p collection starting with @p prefix, from @p start on, as
 *      @p access sees them: a transaction's own writes merged in, its removals hiding the store's strings.
 *
 *  A watching transaction watches what the walk covered, a range up to the last string listed for a full page and to
 *  the end of the prefix otherwise, so a commit landing a string inside it conflicts, the strings it found none of
 *  included.
 */
template <cpu_class_t cpu_class_>
status_t scan_strings(call_access<cpu_class_> const& access, collection_id_t collection, std::string_view prefix,
                      std::string_view start, std::size_t limit, strings_scan_outputs_t& outputs) noexcept {
    if (limit == 0) return {};
    std::string_view const lower = std::max(start, prefix);

    // The transaction's own strings inside the window, in order, which outrank the store's versions of them
    struct own_string_t {
        std::string_view text;
        bool present = false;
    };
    st::basic_vector<own_string_t, arena_allocator<own_string_t>> own {{access.scratch}};
    status_t listed;
    if (access.strings)
        access.strings->for_each_uncommitted(collection, [&](std::string_view text, delta_view_t delta) noexcept {
            if (listed.failed() || text < lower || !text.starts_with(prefix)) return;
            listed = growth_status(own.push_back(own_string_t {text, delta.kind != delta_kind_t::remove_k}));
        });
    if (listed.failed()) return listed;
    std::sort(own.begin(), own.end(),
              [](own_string_t const& first, own_string_t const& second) noexcept { return first.text < second.text; });

    std::size_t found = 0;
    std::size_t next = 0;
    status_t pushed;
    auto const emit = [&](std::string_view text) noexcept {
        pushed = outputs.push(text);
        return pushed.succeeded() && ++found != limit;
    };
    auto const emit_own = [&](own_string_t const& written) noexcept { return !written.present || emit(written.text); };
    status_t const walked = walk_strings(
        access, collection, prefix, lower, read_stamp_of(access),
        [&](std::string_view text, lookup_result_t state, ustore_key_t const&) noexcept {
            for (; next != own.size() && own[next].text < text; ++next)
                if (!emit_own(own[next])) return false;
            if (next != own.size() && own[next].text == text) return emit_own(own[next++]);
            return state != lookup_result_t::written_k || emit(text);
        });
    if (walked.failed()) return walked;
    for (; found != limit && next != own.size() && pushed.succeeded(); ++next) emit_own(own[next]);
    if (pushed.failed()) return pushed;

    transaction_stamp_t const watch_stamp = watch_stamp_of(access);
    if (watch_stamp == invalid_transaction_stamp_k) return {};
    if (found == limit) return access.strings->watch_range(collection, lower, outputs[outputs.size() - 1], watch_stamp);
    return access.strings->watch_prefix(collection, prefix, lower, watch_stamp);
}

/** Commits @p tasks as a transaction of their own, which watches nothing, so the last writer wins. */
template <cpu_class_t cpu_class_>
status_t write_strings_alone(database_state<cpu_class_>& state, ustore_arena_t scratch,
                             std::span<string_task_t const> tasks) noexcept {
    std::size_t bytes = 0;
    for (string_task_t const& task : tasks) bytes += task.text.size() + sizeof(task.value);
    return commit_alone(state, scratch, tasks.size(), bytes, watch_schedule_t::never_k,
                        [&](call_access<cpu_class_> const& access) noexcept { return stage_strings(access, tasks); });
}

#pragma endregion Strings

#pragma region Names

/** The collection @p name maps onto as @p access reads the names, and whether any collection carries the name. */
template <cpu_class_t cpu_class_>
status_t look_up_name(call_access<cpu_class_> const& access, std::string_view name, ustore_collection_id_t& collection,
                      bool& present) noexcept {
    return read_string(access, names_collection_k, name, collection.uuid, present);
}

/** Commits @p name mapped onto @p collection, or its removal when @p collection is null, as a write of its own. */
template <cpu_class_t cpu_class_>
status_t write_name(database_state<cpu_class_>& state, ustore_arena_t scratch, std::string_view name,
                    ustore_collection_id_t const* collection) noexcept {
    string_task_t const task = collection
                                   ? string_task_t {names_collection_k, name, delta_kind_t::write_k, collection->uuid}
                                   : string_task_t {names_collection_k, name};
    return write_strings_alone(state, scratch, std::span<string_task_t const> {&task, 1});
}

#pragma endregion Names

#pragma region Clearing

/** Empties the strings collection @p collection for @c ustore_drop_pairs_k, removing every string in windows committed
 *  one after another, so the transaction of each stays bounded, and refuses @c ustore_drop_values_k, as a string always
 *  maps onto a key. */
template <cpu_class_t cpu_class_>
status_t empty_strings(database_state<cpu_class_>& state, ustore_arena_t scratch, collection_id_t collection,
                       ustore_drop_mode_t mode) noexcept {
    if (mode == ustore_drop_values_k)
        return {status_code_t::invalid_argument_k, "A string always maps onto a key, so its value never empties"};
    call_access<cpu_class_> const head {state, nullptr, watch_schedule_t::never_k, scratch};
    st::basic_vector<char> from;
    st::basic_vector<char> texts;
    st::basic_vector<std::size_t> ends;
    st::basic_vector<string_task_t> tasks;
    for (;;) {
        texts.clear();
        ends.clear();
        tasks.clear();
        status_t room;
        status_t const walked = walk_strings(
            head, collection, {}, std::string_view {from.begin(), from.end()}, head_stamp_k,
            [&](std::string_view text, lookup_result_t found, ustore_key_t const&) noexcept {
                if (found != lookup_result_t::written_k) return true;
                std::size_t const offset = texts.size();
                room = growth_status(texts.resize(offset + text.size()));
                if (room.succeeded()) room = growth_status(ends.push_back(offset + text.size()));
                if (room.failed()) return false;
                if (!text.empty()) std::memcpy(texts.data() + offset, text.data(), text.size());
                return ends.size() != clear_window_keys_k;
            });
        if (walked.failed()) return walked;
        if (room.failed()) return room;
        if (ends.empty()) return {};
        if (status_t const reserved = growth_status(tasks.reserve(ends.size())); reserved.failed()) return reserved;
        for (std::size_t index = 0, begin = 0; index != ends.size(); begin = ends[index++]) [[maybe_unused]]
            st::status_t const placed = tasks.push_back(
                string_task_t {collection, std::string_view {texts.data() + begin, ends[index] - begin}});
        if (status_t const written = write_strings_alone(state, scratch, std::span<string_task_t const> {tasks});
            written.failed())
            return written;
        if (ends.size() < clear_window_keys_k) return {};
        // The next window starts past the last string of this one, past the removals the walk would list again.
        std::string_view const last = tasks[tasks.size() - 1].text;
        if (from.resize(last.size() + 1) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A clear could not hold where it resumes"};
        if (!last.empty()) std::memcpy(from.data(), last.data(), last.size());
        from[last.size()] = '\0';
    }
}

/**
 *  @brief Empties @p collection holding @p modality: removing every key for @c ustore_drop_pairs_k, or emptying every
 *      value for @c ustore_drop_values_k, which a strings collection refuses and a graph, whose vertices hold no
 *      values but their edges, answers by keeping everything, as on the in-memory engine.
 *
 *  The keys are rewritten in windows committed one after another, so the transaction of each stays bounded.
 */
template <cpu_class_t cpu_class_>
status_t empty_collection(database_state<cpu_class_>& state, ustore_arena_t scratch, collection_id_t collection,
                          ustore_modality_t modality, ustore_drop_mode_t mode) noexcept {
    if (entry_layout_of(modality) == entry_layout_t::strings_to_uuids_k)
        return empty_strings(state, scratch, collection, mode);
    if (modality == ustore_modality_graph_k && mode == ustore_drop_values_k) return {};
    call_access<cpu_class_> const head {state, nullptr, watch_schedule_t::never_k, scratch};
    st::basic_vector<stored_key_t, arena_allocator<stored_key_t>> keys {{scratch}};
    stored_key_t from = limit<stored_key_t>::min();
    value_view_t const emptied = mode == ustore_drop_values_k ? value_view_t::make_empty() : value_view_t {};
    for (;;) {
        keys.clear();
        if (status_t const collected = collect_keys(head, collection, from, clear_window_keys_k, keys);
            collected.failed())
            return collected;
        if (keys.empty()) return {};
        status_t const committed = commit_alone(
            state, scratch, keys.size(), keys.size(), watch_schedule_t::never_k,
            [&](call_access<cpu_class_> const& access) noexcept -> status_t {
                for (std::size_t index = 0; index != keys.size(); ++index)
                    if (status_t const written = write_value(access, collection, keys[index], emptied);
                        written.failed())
                        return written;
                return {};
            });
        if (committed.failed()) return committed;
        if (keys.size() < clear_window_keys_k) return {};
        from = keys[keys.size() - 1];
        if (!advance_key(from)) return {};
    }
}

/** Every collection the catalog lists at the head, with its entry, the main one excluded. */
struct listed_collection_t {
    ustore_collection_id_t id;
    catalog_entry_t entry;
};

/** Collects every collection the catalog lists at @p stamp into @p listed. */
template <cpu_class_t cpu_class_, typename listed_type_>
status_t list_catalog(database_state<cpu_class_>& state, ustore_arena_t scratch, listed_type_& listed,
                      transaction_stamp_t stamp = head_stamp_k) noexcept {
    call_access<cpu_class_> const head {state, nullptr, watch_schedule_t::never_k, scratch, stamp};
    st::basic_vector<stored_key_t, arena_allocator<stored_key_t>> keys {{scratch}};
    if (status_t const collected = collect_keys(head, catalog_collection_k, limit<stored_key_t>::min(),
                                                std::numeric_limits<std::size_t>::max(), keys);
        collected.failed())
        return collected;
    for (std::size_t index = 0; index != keys.size(); ++index) {
        auto [held, read_status] = read_value(head, catalog_collection_k, keys[index]);
        if (read_status.failed()) return read_status;
        if (!held) continue;
        ustore_collection_id_t id;
        keys[index].to_bytes(std::as_writable_bytes(std::span<std::uint8_t, 16> {id.uuid.bytes}));
        auto [entry, entry_status] = read_catalog_entry(state, id, held);
        if (entry_status.failed()) return entry_status;
        if (status_t const pushed = growth_status(listed.push_back(listed_collection_t {id, entry})); pushed.failed())
            return pushed;
    }
    return {};
}

/**
 *  @brief Forgets @p collection in the catalog and the names, then returns its runs to the volume, or leaves them to
 *      the snapshots open, which still read them.
 *
 *  The catalog changes under its own claim and straight into the store, outside the registry, since no caller's
 *  transaction reads it, so a catalog change never shows as a commit a transaction's watches might collide with.
 *  The name goes first, so a crash in between leaves a listed collection without a name, never a dangling name, and a
 *  crash with the runs still kept leaves an unlisted collection, which the next open drops.
 */
template <cpu_class_t cpu_class_>
status_t drop_collection(database_state<cpu_class_>& state, ustore_arena_t scratch,
                         ustore_collection_id_t const& collection) noexcept {
    {
        catalog_guard<cpu_class_> const held {state};
        call_access<cpu_class_> const head {state, nullptr, watch_schedule_t::never_k, scratch};
        stored_key_t const listed = stored_of(collection.uuid);
        auto [listing, read_status] = read_value(head, catalog_collection_k, listed);
        if (read_status.failed()) return read_status;
        if (!listing) return {status_code_t::not_found_k, "No collection has such identifier"};
        if (std::string_view const name {listing}; !name.empty())
            if (status_t const unnamed = write_name(state, scratch, name, nullptr); unnamed.failed()) return unnamed;
        if (status_t const unlisted = write_value(head, catalog_collection_k, listed, {}); unlisted.failed())
            return unlisted;
        // A snapshot taken before the unlisting still reads the runs, which wait for the last snapshot to go
        if (atomic_ref_of<cpu_class_, std::size_t> {state.open_snapshots}.load(std::memory_order_acquire) != 0 &&
            state.retired_collections.push_back(ustore_collection_id_t {collection}) == st::status_t::success_k)
            return {};
    }
    return state.store->drop_collection(collection);
}

/** Drops every collection a drop left for the snapshots, which the last snapshot freed no longer reads. */
template <cpu_class_t cpu_class_>
void drop_retired(database_state<cpu_class_>& state) noexcept {
    catalog_guard<cpu_class_> const held {state};
    for (ustore_collection_id_t const& collection : state.retired_collections) [[maybe_unused]]
        status_t const dropped = state.store->drop_collection(collection);
    state.retired_collections.clear();
}

#pragma endregion Clearing

#pragma region Database Calls

/** Implements @c ustore_database_describe with the store's estimates, which count every version of a key that the
 *  runs still hold. */
template <cpu_class_t cpu_class_>
status_t database_describe(ustore_database_describe_t const& arguments, view_t const&) noexcept {
    if (!arguments.description) return {status_code_t::invalid_argument_k, "Need an output for the description"};
    database_state<cpu_class_>& state = state_of<cpu_class_>(arguments);
    *arguments.description = {
        .capabilities = ustore_capabilities_t(ustore_capabilities_transactions_k | ustore_capabilities_collections_k |
                                              ustore_capabilities_snapshots_k),
        .engine = ustore_engine_nvme_k,
        .isolation = ustore_isolation_snapshot_k,
        .keys = static_cast<ustore_size_t>(state.store->population_count()),
        .memory_bytes = static_cast<ustore_size_t>(state.resources->memory_constraint.used()),
        .disk_bytes = static_cast<ustore_size_t>(state.store->size_bytes()),
    };
    return {};
}

/** Implements @c ustore_database_clear: removing every named collection and emptying the main one, or emptying every
 *  collection's pairs or values. */
template <cpu_class_t cpu_class_>
status_t database_clear(ustore_database_clear_t const& arguments, view_t const& view) noexcept {
    ustore_drop_mode_t const mode = arguments.mode;
    if (mode != ustore_drop_collection_k && mode != ustore_drop_pairs_k && mode != ustore_drop_values_k)
        return {status_code_t::invalid_argument_k, "Unknown drop mode"};
    database_state<cpu_class_>& state = state_of<cpu_class_>(arguments);
    st::basic_vector<listed_collection_t, arena_allocator<listed_collection_t>> listed {{view.scratch}};
    if (status_t const collected = list_catalog(state, view.scratch, listed); collected.failed()) return collected;
    for (std::size_t index = 0; index != listed.size(); ++index) {
        // A string always maps onto a key, so emptying every value leaves the strings collections as they are
        if (mode == ustore_drop_values_k &&
            entry_layout_of(listed[index].entry.modality) == entry_layout_t::strings_to_uuids_k)
            continue;
        status_t const cleared = mode == ustore_drop_collection_k
                                     ? drop_collection(state, view.scratch, listed[index].id)
                                     : empty_collection(state, view.scratch, listed[index].id,
                                                        listed[index].entry.modality, mode);
        if (cleared.failed()) return cleared;
    }
    ustore_drop_mode_t const main_mode = mode == ustore_drop_collection_k ? ustore_drop_pairs_k : mode;
    if (status_t const cleared = empty_collection(state, view.scratch, ustore_collection_main_k, ustore_modality_any_k,
                                                  main_mode);
        cleared.failed())
        return cleared;
    if (status_t const flushed = state.store->flush(); flushed.failed()) return flushed;
    return compact_due(state);
}

/** Implements @c ustore_database_compact: flushing every collection, then merging every level over its bounds,
 *  reporting one unit per pass, as a pass merges every collection at once. */
template <cpu_class_t cpu_class_>
status_t database_compact(ustore_database_compact_t const& arguments, view_t const&) noexcept {
    database_state<cpu_class_>& state = state_of<cpu_class_>(arguments);
    if (status_t const flushed = state.store->flush(); flushed.failed()) return flushed;
    progress_t progress {arguments.progress, arguments.progress_state, compaction_passes_k};
    for (std::size_t pass = 0; pass != compaction_passes_k; ++pass) {
        if (state.store->needs_compaction())
            if (status_t const compacted = state.store->compact(); compacted.failed()) return compacted;
        if (!progress.advance(1)) return progress_cancelled_k;
    }
    return {};
}

#pragma endregion Database Calls

#pragma region Collection Calls

/** Implements @c ustore_collection_list as a walk of the catalog at the snapshot it names or at the head. */
template <cpu_class_t cpu_class_>
status_t collection_list(ustore_collection_list_t const& arguments, view_t const& view) noexcept {
    if (!arguments.written) return {status_code_t::invalid_argument_k, "Need an output for the collections written"};
    database_state<cpu_class_>& state = state_of<cpu_class_>(arguments);
    st::basic_vector<listed_collection_t, arena_allocator<listed_collection_t>> listed {{view.scratch}};
    transaction_stamp_t const stamp = view.snapshot
                                          ? handle_cast<snapshot_handle<cpu_class_>>(view.snapshot)->pin.stamp()
                                          : head_stamp_k;
    if (status_t const collected = list_catalog(state, view.scratch, listed, stamp); collected.failed())
        return collected;
    collection_list_outputs_t outputs {view.scratch};
    for (std::size_t index = 0; index != listed.size(); ++index) {
        catalog_entry_t const& entry = listed[index].entry;
        value_view_t const name = entry.name.empty() ? value_view_t {} : value_view_t {entry.name};
        if (status_t const pushed = outputs.push(listed[index].id, entry.modality, name); pushed.failed())
            return pushed;
    }
    return outputs.finish(arguments);
}

/** Implements @c ustore_collection_create: the collection's runs first, then its catalog entry and name in one
 *  commit, which drops the runs again when the name is taken. */
template <cpu_class_t cpu_class_>
status_t collection_create(ustore_collection_create_t const& arguments, view_t const& view) noexcept {
    if (!arguments.collection) return {status_code_t::invalid_argument_k, "Need an output for the collection"};
    if (arguments.modality > ustore_modality_pages_k) return {status_code_t::invalid_argument_k, "Unknown modality"};
    std::string_view const name = string_of(arguments.name);
    if (arguments.name.data && name.empty())
        return {status_code_t::invalid_argument_k, "A name is never empty, as the main collection has none"};
    database_state<cpu_class_>& state = state_of<cpu_class_>(arguments);
    auto [created, mint_status] = mint_collection_id();
    if (mint_status.failed()) return mint_status;

    // The engine's own name of a collection is its UUID's text, which never leaves the engine
    char text[uuid_t::printed_length_k] = {};
    uuid_t {std::span<std::uint8_t const, uuid_t::size_bytes_k> {created.uuid.bytes}}.print(text);
    collection_config_t const shape = entry_layout_of(arguments.modality) == entry_layout_t::strings_to_uuids_k
                                          ? strings_shape_of(state.collection_shape)
                                          : state.collection_shape;
    if (status_t const made = state.store->create_collection(created, string_view_t {text, sizeof(text)},
                                                             arguments.modality, shape);
        made.failed())
        return made;

    // The catalog changes under its own claim, outside the registry, and lists the collection before it names it
    status_t listed;
    {
        catalog_guard<cpu_class_> const held {state};
        call_access<cpu_class_> const head {state, nullptr, watch_schedule_t::never_k, view.scratch};
        bool taken = false;
        ustore_collection_id_t holder;
        if (!name.empty()) listed = look_up_name(head, name, holder, taken);
        if (listed.succeeded() && taken)
            listed = {status_code_t::already_exists_k, "A collection already carries the name"};
        if (listed.succeeded())
            listed = write_value(head, catalog_collection_k, stored_of(created.uuid), value_view_t {name});
        if (listed.succeeded() && !name.empty()) {
            listed = write_name(state, view.scratch, name, &created);
            if (listed.failed()) {
                [[maybe_unused]] status_t const unlisted = write_value(head, catalog_collection_k,
                                                                       stored_of(created.uuid), {});
            }
        }
    }
    if (listed.failed()) {
        [[maybe_unused]] status_t const dropped = state.store->drop_collection(created);
        return listed;
    }
    if (!name.empty())
        if (status_t const flushed = state.store->flush(); flushed.failed()) return flushed;
    *arguments.collection = created;
    return {};
}

/** Implements @c ustore_collection_drop: forgetting the collection and its runs, or emptying its pairs or values. */
template <cpu_class_t cpu_class_>
status_t collection_drop(ustore_collection_drop_t const& arguments, view_t const& view) noexcept {
    ustore_collection_id_t const collection = arguments.collection;
    ustore_drop_mode_t const mode = arguments.mode;
    if (mode != ustore_drop_collection_k && mode != ustore_drop_pairs_k && mode != ustore_drop_values_k)
        return {status_code_t::invalid_argument_k, "Unknown drop mode"};
    if (collection == ustore_collection_main_k && mode == ustore_drop_collection_k)
        return {status_code_t::invalid_argument_k, "The main collection can only be cleared"};
    if (reserved(collection)) return {status_code_t::not_found_k, "No collection has such identifier"};
    database_state<cpu_class_>& state = state_of<cpu_class_>(arguments);
    if (mode == ustore_drop_collection_k) {
        if (status_t const dropped = drop_collection(state, view.scratch, collection); dropped.failed()) return dropped;
        return state.store->flush();
    }
    auto [entry, look_up_status] = look_up(state, view.scratch, collection);
    if (look_up_status == status_code_t::not_found_k)
        return {status_code_t::not_found_k, "No collection has such identifier"};
    if (look_up_status.failed()) return look_up_status;
    if (status_t const emptied = empty_collection(state, view.scratch, collection, entry.modality, mode);
        emptied.failed())
        return emptied;
    return compact_due(state);
}

/** Implements @c ustore_collection_find, reading the names at the snapshot it names or at the head. */
template <cpu_class_t cpu_class_>
status_t collection_find(ustore_collection_find_t const& arguments, strings_arg_t const& names,
                         view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!names || !arguments.collections)
        return {status_code_t::invalid_argument_k, "Need the names and an output for the identifiers"};
    database_state<cpu_class_>& state = state_of<cpu_class_>(arguments);
    auto [outputs, outputs_status] = collection_find_outputs_t::make(arguments);
    if (outputs_status.failed()) return outputs_status;
    // Through a snapshot the names read as they stood at its stamp, and through anything else at the head
    call_access<cpu_class_> const head {state, nullptr, watch_schedule_t::never_k, view.scratch};
    call_access<cpu_class_> const named = view.snapshot ? access_of(state, view) : head;
    for (std::size_t index = 0; index != arguments.count; ++index) {
        bool present = false;
        ustore_collection_id_t found;
        if (status_t const looked = look_up_name(named, string_of(names[index]), found, present); looked.failed())
            return looked;
        // A drop unnames a collection before the store lets it go, so a name whose store is gone is a drop in flight
        ustore_modality_t modality = ustore_modality_any_k;
        if (present && state.store->collection_modality(found, modality).failed()) present = false;
        outputs.assign(index, present ? found : ustore_collection_main_k, modality, present);
    }
    outputs.finish(arguments);
    return {};
}

#pragma endregion Collection Calls

#pragma region Blob Calls

/** Implements @c ustore_blobs_write, committing a batch outside a transaction atomically. */
template <cpu_class_t cpu_class_>
status_t blobs_write(ustore_blobs_write_t const& arguments, strings_arg_t const& values, view_t const& view) noexcept {
    place_batch_t const places = places_of(arguments);
    if (status_t const status = validate_write(view.transaction, places, values, arguments.durability, view.watch);
        status.failed() || !arguments.count)
        return status;
    database_state<cpu_class_>& state = state_of<cpu_class_>(arguments);
    if (status_t const resolved = resolve_runs(state, view, places.collections_begin, places.size(),
                                               ustore_modality_blobs_k);
        resolved.failed())
        return resolved;
    std::size_t bytes = 0;
    for (std::size_t index = 0; index != places.size(); ++index) bytes += values[index].length;
    return serve_write(state, view, arguments.durability, places.size(), bytes, watch_schedule_t::never_k,
                       [&](call_access<cpu_class_> const& access) noexcept -> status_t {
                           for (std::size_t index = 0; index != places.size(); ++index) {
                               place_t const place = places[index];
                               status_t const written = write_value(access, place.collection, stored_of(place.key.uuid),
                                                                    value_view_t {values[index]});
                               if (written.failed()) return written;
                           }
                           return {};
                       });
}

/** Removes whole values through the store of each collection, committing a batch outside a transaction atomically,
 *  for a caller whose resolver checked the collections. */
template <cpu_class_t cpu_class_>
status_t remove_values(ustore_blobs_remove_t const& arguments, view_t const& view) noexcept {
    place_batch_t const places = places_of(arguments);
    if (status_t const status = validate_remove(view.transaction, places, arguments.durability, view.watch);
        status.failed() || !arguments.count)
        return status;
    database_state<cpu_class_>& state = state_of<cpu_class_>(arguments);
    return serve_write(state, view, arguments.durability, places.size(), 1, watch_schedule_t::never_k,
                       [&](call_access<cpu_class_> const& access) noexcept -> status_t {
                           for (std::size_t index = 0; index != places.size(); ++index) {
                               place_t const place = places[index];
                               status_t const removed = write_value(access, place.collection, stored_of(place.key.uuid),
                                                                    value_view_t {});
                               if (removed.failed()) return removed;
                           }
                           return {};
                       });
}

/** Implements @c ustore_blobs_remove over the values of blobs collections alone. */
template <cpu_class_t cpu_class_>
status_t blobs_remove(ustore_blobs_remove_t const& arguments, view_t const& view) noexcept {
    if (status_t const resolved = resolve_modality<cpu_class_>(arguments.database, arguments.collections,
                                                               arguments.count, ustore_modality_blobs_k, view);
        resolved.failed())
        return resolved;
    return remove_values<cpu_class_>(arguments, view);
}

/** Reads whole values through the store of each collection, folded as its modality folds them, copying every value
 *  into the scratch once and into the arena at the end, for a caller whose resolver checked the collections. */
template <cpu_class_t cpu_class_>
status_t read_values(ustore_blobs_read_t const& arguments, view_t const& view) noexcept {
    if (!arguments.count) return {};
    place_batch_t const places = places_of(arguments);
    if (status_t const status = validate_read(places); status.failed()) return status;
    database_state<cpu_class_>& state = state_of<cpu_class_>(arguments);
    auto [outputs, outputs_status] = read_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    call_access<cpu_class_> const access = access_of(state, view);
    for (std::size_t index = 0; index != places.size(); ++index) {
        place_t const place = places[index];
        auto [value, read_status] = read_value(access, place.collection, stored_of(place.key.uuid));
        if (read_status.failed()) return read_status;
        if (value) outputs.append(value);
        else outputs.append_missing();
    }
    return outputs.finish(arguments);
}

/** Implements @c ustore_blobs_read over the values of blobs collections alone. */
template <cpu_class_t cpu_class_>
status_t blobs_read(ustore_blobs_read_t const& arguments, view_t const& view) noexcept {
    if (status_t const resolved = resolve_modality<cpu_class_>(arguments.database, arguments.collections,
                                                               arguments.count, ustore_modality_blobs_k, view);
        resolved.failed())
        return resolved;
    return read_values<cpu_class_>(arguments, view);
}

/** Scans keys as ordered walks of the store of each collection, a graph's being its vertices, watching what a
 *  transaction hands out, for a caller whose resolver checked the collections. */
template <cpu_class_t cpu_class_>
status_t scan_values(ustore_blobs_scan_t const& arguments, view_t const& view) noexcept {
    if (status_t const status = validate_scan(arguments); status.failed() || !arguments.count) return status;
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    scan_batch_t const scans {collections, strided(arguments.start_keys), arguments.wanted, arguments.count};
    database_state<cpu_class_>& state = state_of<cpu_class_>(arguments);
    auto [outputs, outputs_status] = scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    progress_t progress {arguments.progress, arguments.progress_state, arguments.count};
    call_access<cpu_class_> const access = access_of(state, view);
    st::basic_vector<stored_key_t, arena_allocator<stored_key_t>> found {{view.scratch}};
    for (std::size_t task = 0; task != scans.size(); ++task) {
        scan_task_t const scan = scans[task];
        outputs.open_task(task);
        found.clear();
        if (scan.limit != 0)
            if (status_t const collected = collect_keys(access, scan.collection, stored_of(scan.start_key.uuid),
                                                        scan.limit, found);
                collected.failed())
                return collected;
        for (std::size_t index = 0; index != found.size(); ++index)
            if (status_t const pushed = outputs.push(key_of(found[index])); pushed.failed()) return pushed;
        if (status_t const watched = watch_keys(access, scan.collection, std::span<stored_key_t const> {found});
            watched.failed())
            return watched;
        // A full page covered the keys up to its last, and a short one every key up to the largest
        stored_key_t const upper = found.size() == scan.limit && !found.empty() ? found[found.size() - 1]
                                                                                : limit<stored_key_t>::max();
        if (scan.limit != 0)
            if (status_t const watched = watch_range(access, scan.collection, stored_of(scan.start_key.uuid), upper);
                watched.failed())
                return watched;
        if (!progress.advance(1)) return progress_cancelled_k;
    }
    return outputs.finish(arguments);
}

/** Implements @c ustore_blobs_scan over the keys of blobs collections alone. */
template <cpu_class_t cpu_class_>
status_t blobs_scan(ustore_blobs_scan_t const& arguments, view_t const& view) noexcept {
    if (status_t const resolved = resolve_modality<cpu_class_>(arguments.database, arguments.collections,
                                                               arguments.count, ustore_modality_blobs_k, view);
        resolved.failed())
        return resolved;
    return scan_values<cpu_class_>(arguments, view);
}

/** A splitmix64 generator, drawing the positions a reservoir keeps. */
struct sample_generator_t {
    using result_type = std::uint64_t;

    /** The state every draw advances. */
    std::uint64_t state;

    static constexpr result_type min() noexcept { return 0; }
    static constexpr result_type max() noexcept { return std::numeric_limits<result_type>::max(); }

    result_type operator()() noexcept {
        std::uint64_t mixed = (state += 0x9E3779B97F4A7C15ull);
        mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ull;
        mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBull;
        return mixed ^ (mixed >> 31);
    }
};

/** Implements @c ustore_blobs_sample as a reservoir filled from a walk of each collection. */
template <cpu_class_t cpu_class_>
status_t blobs_sample(ustore_blobs_sample_t const& arguments, view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!arguments.offsets || !arguments.keys)
        return {status_code_t::invalid_argument_k, "Need outputs for the offsets and the keys"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    sample_batch_t const samples {collections, arguments.wanted, arguments.count};
    database_state<cpu_class_>& state = state_of<cpu_class_>(arguments);
    if (status_t const resolved = resolve_runs(state, view, collections, arguments.count, ustore_modality_blobs_k);
        resolved.failed())
        return resolved;
    auto [outputs, outputs_status] = scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    auto const now = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    sample_generator_t generator {now ^ reinterpret_cast<std::uintptr_t>(&outputs)};
    call_access<cpu_class_> const access = access_of(state, view);
    st::basic_vector<stored_key_t, arena_allocator<stored_key_t>> kept {{view.scratch}};
    for (std::size_t task = 0; task != samples.size(); ++task) {
        sample_task_t const sample = samples[task];
        outputs.open_task(task);
        kept.clear();
        std::size_t seen = 0;
        status_t room;
        if (sample.limit != 0)
            if (status_t const walked = walk(access, sample.collection, limit<stored_key_t>::min(),
                                             [&](stored_key_t key, stored_value_size_t) noexcept {
                                                 std::size_t const slot = seen < sample.limit
                                                                              ? seen
                                                                              : st::draw_below(generator, seen + 1);
                                                 ++seen;
                                                 if (slot >= sample.limit) return true;
                                                 if (slot != kept.size()) {
                                                     kept[slot] = key;
                                                     return true;
                                                 }
                                                 room = growth_status(kept.push_back(stored_key_t {key}));
                                                 return room.succeeded();
                                             });
                walked.failed() || room.failed())
                return walked.failed() ? walked : room;
        for (std::size_t index = 0; index != kept.size(); ++index)
            if (status_t const pushed = outputs.push(key_of(kept[index])); pushed.failed()) return pushed;
        if (status_t const watched = watch_keys(access, sample.collection, std::span<stored_key_t const> {kept});
            watched.failed())
            return watched;
    }
    return outputs.finish(arguments);
}

/** Implements @c ustore_blobs_measure, exact for keys and value bytes at the view it reads. */
template <cpu_class_t cpu_class_>
status_t blobs_measure(ustore_blobs_measure_t const& arguments, view_t const& view) noexcept {
    if (!arguments.count) return {};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    strided_iterator<ustore_key_t const> const start_keys = strided(arguments.start_keys);
    strided_iterator<ustore_key_t const> const end_keys = strided(arguments.end_keys);
    database_state<cpu_class_>& state = state_of<cpu_class_>(arguments);
    if (status_t const resolved = resolve_runs(state, view, collections, arguments.count, ustore_modality_blobs_k);
        resolved.failed())
        return resolved;
    auto [outputs, outputs_status] = measure_outputs_t::make(arguments);
    if (outputs_status.failed()) return outputs_status;
    call_access<cpu_class_> const access = access_of(state, view);
    for (std::size_t task = 0; task != arguments.count; ++task) {
        stored_key_t const lower = stored_of((start_keys ? start_keys[task] : smallest_key_k).uuid);
        stored_key_t const last = stored_of((end_keys ? end_keys[task] : largest_key_k).uuid);
        std::size_t cardinality = 0;
        std::size_t value_bytes = 0;
        status_t const walked = walk(access, task_collection(collections, task), lower,
                                     [&](stored_key_t key, stored_value_size_t size) noexcept {
                                         if (last < key) return false;
                                         ++cardinality;
                                         value_bytes += size;
                                         return true;
                                     });
        if (walked.failed()) return walked;
        if (status_t const watched = watch_range(access, task_collection(collections, task), lower, last);
            watched.failed())
            return watched;
        outputs.min_cardinalities[task] = cardinality;
        outputs.max_cardinalities[task] = cardinality;
        outputs.min_value_bytes[task] = value_bytes;
        outputs.max_value_bytes[task] = value_bytes;
        outputs.min_space_usages[task] = value_bytes + cardinality * sizeof(stored_key_t);
        outputs.max_space_usages[task] = std::numeric_limits<ustore_size_t>::max();
    }
    outputs.finish(arguments);
    return {};
}

#pragma endregion Blob Calls

#pragma region Strings Calls

/** Lays the tasks of a strings write or removal out in the scratch of @p view, writing the keys of @p values, or
 *  removing the strings when @p values view none. */
inline expected<std::span<string_task_t>> string_tasks(view_t const& view,
                                                       strided_iterator<ustore_collection_id_t const> collections,
                                                       std::size_t count, strings_arg_t const& texts,
                                                       strided_iterator<ustore_key_t const> values) noexcept {
    auto [tasks, allocate_status] = arena_allocate<string_task_t>(view.scratch, count);
    if (allocate_status.failed()) return allocate_status;
    for (std::size_t index = 0; index != count; ++index)
        tasks[index] = values ? string_task_t {task_collection(collections, index), string_of(texts[index]),
                                               delta_kind_t::write_k, values[index].uuid}
                              : string_task_t {task_collection(collections, index), string_of(texts[index])};
    return tasks;
}

/** Writes or removes the strings of a batch, each one on its own, watched as the call's transaction watches and not
 *  at all outside one, where the last writer wins. */
template <cpu_class_t cpu_class_, typename arguments_type_>
status_t serve_strings_write(arguments_type_ const& arguments, strings_arg_t const& texts,
                             strided_iterator<ustore_key_t const> values, view_t const& view) noexcept {
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    database_state<cpu_class_>& state = state_of<cpu_class_>(arguments);
    if (status_t const resolved = resolve_runs(state, view, collections, arguments.count, ustore_modality_strings_k);
        resolved.failed())
        return resolved;
    auto [tasks, tasks_status] = string_tasks(view, collections, arguments.count, texts, values);
    if (tasks_status.failed()) return tasks_status;
    std::size_t bytes = 0;
    for (string_task_t const& task : tasks) bytes += task.text.size() + sizeof(task.value);
    return serve_write(state, view, arguments.durability, arguments.count, bytes, watch_schedule_t::never_k,
                       [&](call_access<cpu_class_> const& access) noexcept {
                           return stage_strings(access, std::span<string_task_t const> {tasks});
                       });
}

/** Implements @c ustore_strings_write, committing a batch outside a transaction atomically. */
template <cpu_class_t cpu_class_>
status_t strings_write(ustore_strings_write_t const& arguments, strings_arg_t const& texts,
                       view_t const& view) noexcept {
    if (!arguments.count) return {};
    strided_iterator<ustore_key_t const> const values = strided(arguments.values);
    if (!texts || !values) return {status_code_t::invalid_argument_k, "Need the strings and their keys"};
    return serve_strings_write<cpu_class_>(arguments, texts, values, view);
}

/** Implements @c ustore_strings_remove, committing a batch outside a transaction atomically. */
template <cpu_class_t cpu_class_>
status_t strings_remove(ustore_strings_remove_t const& arguments, strings_arg_t const& texts,
                        view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!texts) return {status_code_t::invalid_argument_k, "No strings were provided"};
    return serve_strings_write<cpu_class_>(arguments, texts, strided_iterator<ustore_key_t const> {}, view);
}

/** Implements @c ustore_strings_read, reading the key of every string on its own. */
template <cpu_class_t cpu_class_>
status_t strings_read(ustore_strings_read_t const& arguments, strings_arg_t const& texts, view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!texts) return {status_code_t::invalid_argument_k, "No strings were provided"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    database_state<cpu_class_>& state = state_of<cpu_class_>(arguments);
    if (status_t const resolved = resolve_runs(state, view, collections, arguments.count, ustore_modality_strings_k);
        resolved.failed())
        return resolved;
    auto [outputs, outputs_status] = strings_read_outputs_t::make(arguments);
    if (outputs_status.failed()) return outputs_status;
    call_access<cpu_class_> const access = access_of(state, view);
    for (std::size_t index = 0; index != arguments.count; ++index) {
        ustore_key_t value;
        bool present = false;
        if (status_t const read = read_string(access, task_collection(collections, index), string_of(texts[index]),
                                              value.uuid, present);
            read.failed())
            return read;
        if (present) outputs.push_present(value);
        else outputs.push_missing();
    }
    outputs.finish(arguments);
    return {};
}

/** Implements @c ustore_strings_scan as ordered walks of each collection's strings. */
template <cpu_class_t cpu_class_>
status_t strings_scan(ustore_strings_scan_t const& arguments, strings_arg_t const& prefixes,
                      strings_arg_t const& starts, view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!arguments.offsets || !arguments.strings)
        return {status_code_t::invalid_argument_k, "Need outputs for the offsets and the strings"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    database_state<cpu_class_>& state = state_of<cpu_class_>(arguments);
    if (status_t const resolved = resolve_runs(state, view, collections, arguments.count, ustore_modality_strings_k);
        resolved.failed())
        return resolved;
    auto [outputs, outputs_status] = strings_scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    progress_t progress {arguments.progress, arguments.progress_state, arguments.count};
    call_access<cpu_class_> const access = access_of(state, view);
    for (std::size_t task = 0; task != arguments.count; ++task) {
        std::string_view const prefix = prefixes ? string_of(prefixes[task]) : std::string_view {};
        std::string_view const start = starts ? string_of(starts[task]) : std::string_view {};
        outputs.open_task(task);
        if (status_t const scanned = scan_strings(access, task_collection(collections, task), prefix, start,
                                                  arguments.wanted, outputs);
            scanned.failed())
            return scanned;
        if (!progress.advance(1)) return progress_cancelled_k;
    }
    return outputs.finish(arguments);
}

#pragma endregion Strings Calls

#pragma region Transaction Calls

/** Implements @c ustore_transaction_init, opening a transaction or restarting the one it reuses. */
template <cpu_class_t cpu_class_>
status_t transaction_init(ustore_transaction_init_t const& arguments, view_t const& view) noexcept {
    if (status_t const status = validate_transaction_begin(arguments.transaction); status.failed()) return status;
    if (view.transaction) {
        transaction_handle<cpu_class_>& handle = *handle_cast<transaction_handle<cpu_class_>>(view.transaction);
        handle.held->rollback();
        handle.strings.clear();
        handle.staged = false;
        return {};
    }
    database_state<cpu_class_>& state = state_of<cpu_class_>(arguments);
    auto [opened, begin_status] = begin_transaction(state, state.transaction_shape);
    if (begin_status.failed()) return begin_status;
    auto* const handle = new (std::nothrow) transaction_handle<cpu_class_>(state, std::move(opened));
    if (!handle) return {status_code_t::out_of_memory_k, "Failed to allocate the transaction"};
    *arguments.transaction = c_handle<ustore_transaction_t>(handle);
    return {};
}

/** Implements @c ustore_transaction_stage: checking every watch and that every write fits, then handing the
 *  ledger's leader word back, so the commit checks them again. */
template <cpu_class_t cpu_class_>
status_t transaction_stage(ustore_transaction_stage_t const& arguments, view_t const& view) noexcept {
    if (!view.transaction) return {status_code_t::uninitialized_k, "Transaction is uninitialized"};
    transaction_handle<cpu_class_>& handle = *handle_cast<transaction_handle<cpu_class_>>(view.transaction);
    if (!handle.staged) {
        if (status_t const staged = handle.strings.stage(*handle.held); staged.failed()) return staged;
        handle.held->unstage();
        handle.staged = true;
    }
    if (arguments.read_stamp) *arguments.read_stamp = handle.held->start_stamp();
    return {};
}

/** Implements @c ustore_transaction_commit, staging first, flushing when asked and compacting what it left due. */
template <cpu_class_t cpu_class_>
status_t transaction_commit(ustore_transaction_commit_t const& arguments, view_t const& view) noexcept {
    if (status_t const status = validate_transaction_commit(view.transaction, arguments.durability); status.failed())
        return status;
    transaction_handle<cpu_class_>& handle = *handle_cast<transaction_handle<cpu_class_>>(view.transaction);
    if (status_t const committed = handle.strings.commit(*handle.held); committed.failed()) return committed;
    handle.staged = false;
    if (arguments.commit_stamp) *arguments.commit_stamp = handle.held->last_commit_stamp();
    return settle(handle.state, arguments.durability);
}

/** Implements @c ustore_transaction_rollback, withdrawing a stage and keeping the writes, which a transaction never
 *  staged has none of to withdraw. */
template <cpu_class_t cpu_class_>
status_t transaction_rollback(ustore_transaction_rollback_t const&, view_t const& view) noexcept {
    if (!view.transaction) return {status_code_t::uninitialized_k, "Transaction is uninitialized"};
    handle_cast<transaction_handle<cpu_class_>>(view.transaction)->staged = false;
    return {};
}

#pragma endregion Transaction Calls

#pragma region Lifetime

/** Fills @p resources with the directories @p arguments name, one data directory inside the database's own when they
 *  name none. */
inline status_t read_directories(ustore_database_open_nvme_t const& arguments, resources_config_t& resources) noexcept {
    if (sz::failed(resources.directory.assign(string_view_t {string_of(arguments.directory)})))
        return {status_code_t::out_of_memory_k, "Failed to copy the directory"};
    if (arguments.data_directories_count && !arguments.data_directories)
        return {status_code_t::invalid_argument_k, "Counted data directories are null"};
    for (std::size_t index = 0; index != arguments.data_directories_count; ++index) {
        ustore_data_directory_t const& given = arguments.data_directories[index];
        if (!given.path.length) return {status_code_t::invalid_argument_k, "A data directory needs a path"};
        data_directory_config_t directory;
        if (sz::failed(directory.path.assign(string_view_t {string_of(given.path)})))
            return {status_code_t::out_of_memory_k, "Failed to copy a data directory"};
        directory.max_size = given.max_bytes ? given.max_bytes : default_volume_bytes_k;
        if (st::failed(resources.data_directories.push_back(std::move(directory))))
            return {status_code_t::out_of_memory_k, "Failed to list the data directories"};
    }
    if (!resources.data_directories.empty()) return {};

    char path[path_max_bytes_k];
    if (status_t const joined = join_path(resources.directory.c_str(), data_directory_name_k, path); joined.failed())
        return joined;
    data_directory_config_t only;
    if (sz::failed(only.path.assign(string_view_t {path})))
        return {status_code_t::out_of_memory_k, "Failed to copy the data directory"};
    only.max_size = default_volume_bytes_k;
    if (st::failed(resources.data_directories.push_back(std::move(only))))
        return {status_code_t::out_of_memory_k, "Failed to list the data directory"};
    return {};
}

/** Replaces @p value with @p requested unless it is zero, answering whether @p requested fits @p value. */
template <typename value_type_>
bool take_unless_zero(value_type_& value, ustore_size_t requested) noexcept {
    if (requested == 0) return true;
    if (!std::in_range<value_type_>(requested)) return false;
    value = static_cast<value_type_>(requested);
    return true;
}

/** Reads the placement of the executor of @p arguments and their own sizes, keeping the engine's default for every
 *  zero among them. */
inline status_t read_options(ustore_database_open_nvme_t const& arguments, resources_config_t& resources,
                             database_config_t& transactions, user_collection_config_t& collections) noexcept {
    nvme_placement_t const placement = nvme_placement_of(arguments.executor);
    switch (placement.nvme.transport) {
    case ustore_transport_automatic_k: resources.io_transport = io_transport_t::automatic_k; break;
    case ustore_transport_blocking_k: resources.io_transport = io_transport_t::blocking_k; break;
    case ustore_transport_linux_uring_k: resources.io_transport = io_transport_t::linux_uring_k; break;
    case ustore_transport_linux_aio_k: resources.io_transport = io_transport_t::linux_aio_k; break;
    case ustore_transport_linux_spdk_k: resources.io_transport = io_transport_t::linux_spdk_k; break;
    default: return {status_code_t::invalid_argument_k, "Unknown transport"};
    }
    for (std::int32_t const device : placement.devices)
        if (st::failed(resources.gpu_devices.push_back(int {device})))
            return {status_code_t::out_of_memory_k, "Failed to list the GPU devices"};
    resources.gpu_memory_limit = placement.devices.empty() ? 0 : placement.scratch_limit;

    // Every thread of the executor, its blocking caller included, keeps a channel, and every other thread shares the
    // rest
    resources.threads = placement.threads + 1;
    resources.shared_io_channels = engine_shared_io_channels_k;
    ustore_nvme_options_t const& moving = placement.nvme;
    if (arguments.collection_value_size) collections.value_max_size = 0;
    bool const fits = take_unless_zero(resources.memory_limit, arguments.memory_limit) &&
                      take_unless_zero(resources.shared_io_channels, moving.shared_io_channels) &&
                      take_unless_zero(resources.channels.depth, moving.queue_depth) &&
                      take_unless_zero(resources.channels.request_buffer_bytes, moving.request_buffer_bytes) &&
                      take_unless_zero(resources.channels.request_buffers_count, moving.request_buffers_count) &&
                      take_unless_zero(resources.channels.stream_buffer_bytes, moving.stream_buffer_bytes) &&
                      take_unless_zero(resources.channels.stream_buffers_count, moving.stream_buffers_count) &&
                      take_unless_zero(transactions.default_transaction.max_bytes, arguments.transaction_max_bytes) &&
                      take_unless_zero(transactions.default_transaction.max_deltas, arguments.transaction_max_deltas) &&
                      take_unless_zero(collections.concurrency_limit, arguments.collection_concurrency_limit) &&
                      take_unless_zero(collections.cache_limit, arguments.collection_cache_limit) &&
                      take_unless_zero(collections.memory_runs_max_bytes, arguments.collection_memory_runs_max_bytes) &&
                      take_unless_zero(collections.memory_runs_max_deltas,
                                       arguments.collection_memory_runs_max_deltas) &&
                      take_unless_zero(collections.level_enlarge_factor, arguments.collection_level_enlarge_factor) &&
                      take_unless_zero(collections.value_size, arguments.collection_value_size) &&
                      take_unless_zero(collections.value_max_size, arguments.collection_value_max_size);
    if (!fits) return {status_code_t::invalid_argument_k, "An option of the engine exceeds its range"};
    auto ignore_issue = [](issue_kind_t, std::string_view) noexcept {};
    if (!validate(resources, transactions, ignore_issue) || !validate(resources, collections, ignore_issue))
        return {status_code_t::invalid_argument_k, "The configuration of the engine is invalid"};
    return {};
}

/** Creates the collection @p collection named @p name, holding @p modality, shaped as @p shape, unless the store holds
 *  it already. */
template <cpu_class_t cpu_class_>
status_t reserve_collection(database_state<cpu_class_>& state, collection_id_t const& collection, std::string_view name,
                            ustore_modality_t modality, collection_config_t const& shape) noexcept {
    if (state.store->has_collection(collection)) return {};
    return state.store->create_collection(collection, string_view_t {name}, modality, shape);
}

/** Drops every collection of the store the catalog does not list, which a crash between a create's two steps
 *  leaves behind. */
template <cpu_class_t cpu_class_>
status_t drop_unlisted(database_state<cpu_class_>& state, ustore_arena_t scratch) noexcept {
    st::basic_vector<collection_ids_t> const held = state.store->list_collections();
    status_t first;
    for (std::size_t index = 0; index != held.size() && first.succeeded(); ++index) {
        ustore_collection_id_t const id = held[index].id;
        if (id == ustore_collection_main_k || reserved(id)) continue;
        auto [entry, look_up_status] = look_up(state, scratch, id);
        if (look_up_status == status_code_t::not_found_k) first = state.store->drop_collection(id);
        else if (look_up_status.failed()) first = look_up_status;
    }
    return first;
}

/** Mounts the store of @p state over the storage its resources opened, which must be the one on files. */
template <cpu_class_t cpu_class_>
status_t mount_store(database_state<cpu_class_>& state, database_config_t const& transactions) noexcept {
    return state.resources->storage.visit([&](auto& mount) noexcept -> status_t {
        using mount_t = std::remove_reference_t<decltype(mount)>;
        if constexpr (std::is_same_v<mount_t, storage_mount_of<cpu_class_>>)
            return state.store->mount(mount.router(), mount.channels(), transactions);
        else return {status_code_t::unsupported_k, "The engine mounts its volumes on files alone"};
    });
}

/** Opens the store @p resources place, every collection of it shaped as @p shape, reserving the main collection, the
 *  catalog and the names on the first open. */
template <cpu_class_t cpu_class_>
expected<std::unique_ptr<database_state<cpu_class_>>> open_state(resources_config_t const& resources,
                                                                 database_config_t const& transactions,
                                                                 collection_config_t const& shape,
                                                                 ustore_arena_t scratch) noexcept {
    std::unique_ptr<database_state<cpu_class_>> state {new (std::nothrow) database_state<cpu_class_>};
    if (!state) return status_t {status_code_t::out_of_memory_k, "Failed to allocate the database"};
    state->collection_shape = shape;
    state->transaction_shape = transactions.default_transaction;
    auto [opened, open_status] =
        open_resources<atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>>(resources,
                                                                                                   uuid_t {});
    if (open_status.failed()) return open_status;
    state->resources = std::move(opened);
    state->store.reset(new (std::nothrow) database_of<cpu_class_> {state->resources});
    if (!state->store) return status_t {status_code_t::out_of_memory_k, "Failed to allocate the store"};
    if (status_t const mounted = mount_store(*state, transactions); mounted.failed()) return mounted;

    if (status_t const reserved = reserve_collection(*state, ustore_collection_main_k, "main", ustore_modality_any_k,
                                                     shape);
        reserved.failed())
        return reserved;
    // The catalog holds the names of collections, whatever one width the caller's values take
    collection_config_t records = shape;
    sorted_run_config_t& record_values = records.lsm_tree.lsm_level_0.sorted_run;
    if (record_values.value_size != 0)
        record_values.value_max_size = static_cast<stored_value_size_t>(
            std::min<std::size_t>(engine_value_max_size_k, records.memory_runs_max_bytes));
    record_values.value_size = 0;
    if (status_t const reserved = reserve_collection(*state, catalog_collection_k, "catalog", ustore_modality_blobs_k,
                                                     records);
        reserved.failed())
        return reserved;
    if (status_t const reserved = reserve_collection(*state, names_collection_k, "names", ustore_modality_strings_k,
                                                     strings_shape_of(shape));
        reserved.failed())
        return reserved;
    if (status_t const dropped = drop_unlisted(*state, scratch); dropped.failed()) return dropped;
    return state;
}

/** Implements @c ustore_database_open_nvme, reserving the main collection, the catalog and the names on the first
 *  open. */
template <cpu_class_t cpu_class_>
status_t nvme_variant<cpu_class_>::open(ustore_database_open_nvme_t const& arguments, view_t const& view) noexcept {
    if (!arguments.database) return {status_code_t::invalid_argument_k, "Need an output for the database"};
    *arguments.database = nullptr;
    if (!arguments.directory.length) return {status_code_t::invalid_argument_k, "A database needs a directory"};

    resources_config_t resources;
    database_config_t transactions;
    transactions.default_transaction = {engine_transaction_bytes_k, engine_transaction_deltas_k};
    user_collection_config_t collections = engine_collection_defaults();
    if (status_t const read = read_directories(arguments, resources); read.failed()) return read;
    auto [exists, exists_status] = directory_exists(resources.directory.c_str());
    if (exists_status.failed()) return exists_status;
    if (!exists) return {status_code_t::not_found_k, "The directory doesn't exist"};
    if (status_t const read = read_options(arguments, resources, transactions, collections); read.failed()) return read;
    auto [state, open_status] = open_state<cpu_class_>(resources, transactions, create_collection_config(collections),
                                                       view.scratch);
    if (open_status.failed()) return open_status;
    state->executor = nvme_placement_of(arguments.executor).executor;
    *arguments.database = c_handle<ustore_database_t>(state.release());
    return {};
}

#pragma endregion Lifetime

#pragma region Snapshot Calls

/** Implements @c ustore_snapshot_init, pinning the newest published stamp in the store's ledger. */
template <cpu_class_t cpu_class_>
status_t snapshot_init(ustore_snapshot_init_t const& arguments, view_t const&) noexcept {
    if (!arguments.snapshot) return {status_code_t::invalid_argument_k, "Need an output for the snapshot"};
    database_state<cpu_class_>& state = state_of<cpu_class_>(arguments);
    auto* const handle = new (std::nothrow) snapshot_handle<cpu_class_>(state);
    if (!handle) return {status_code_t::out_of_memory_k, "Failed to allocate the snapshot"};
    transaction_stamp_t const stamp = state.store->ledger().pin_reader(handle->pin);
    atomic_ref_of<cpu_class_, std::size_t> {state.open_snapshots}.fetch_add(1, std::memory_order_release);
    if (arguments.read_stamp) *arguments.read_stamp = static_cast<ustore_stamp_t>(stamp);
    *arguments.snapshot = c_handle<ustore_snapshot_t>(handle);
    return {};
}

/** Copies every string of @p collection that @p access reads into @p target with its key, @p window strings per
 *  commit. */
template <cpu_class_t cpu_class_>
status_t save_strings(call_access<cpu_class_> const& access, collection_id_t collection,
                      database_state<cpu_class_>& target, std::size_t window, progress_t& progress) noexcept {
    st::basic_vector<char> from;
    st::basic_vector<char> texts;
    st::basic_vector<std::size_t> text_ends;
    st::basic_vector<ustore_uuid_t> values;
    st::basic_vector<string_task_t> tasks;
    for (;;) {
        texts.clear();
        text_ends.clear();
        values.clear();
        tasks.clear();
        status_t room;
        status_t const walked = walk_strings(
            access, collection, {}, std::string_view {from.begin(), from.end()}, access.stamp,
            [&](std::string_view text, lookup_result_t found, ustore_key_t const& value) noexcept {
                if (found != lookup_result_t::written_k) return true;
                std::size_t const offset = texts.size();
                room = growth_status(texts.resize(offset + text.size()));
                if (room.succeeded()) room = growth_status(text_ends.push_back(offset + text.size()));
                if (room.succeeded()) room = growth_status(values.resize(values.size() + 1));
                if (room.failed()) return false;
                if (!text.empty()) std::memcpy(texts.data() + offset, text.data(), text.size());
                values[values.size() - 1] = value.uuid;
                return text_ends.size() != window;
            });
        if (walked.failed()) return walked;
        if (room.failed()) return room;
        if (text_ends.empty()) return {};
        if (status_t const reserved = growth_status(tasks.reserve(text_ends.size())); reserved.failed())
            return reserved;
        for (std::size_t index = 0, begin = 0; index != text_ends.size(); begin = text_ends[index++]) {
            std::string_view const text {texts.data() + begin, text_ends[index] - begin};
            [[maybe_unused]] st::status_t const placed = tasks.push_back(
                string_task_t {collection, text, delta_kind_t::write_k, values[index]});
            if (!progress.advance(text.size() + sizeof(ustore_uuid_t))) return progress_cancelled_k;
        }
        if (status_t const written = write_strings_alone(target, access.scratch,
                                                         std::span<string_task_t const> {tasks});
            written.failed())
            return written;
        if (text_ends.size() < window) return {};
        std::string_view const last = tasks[tasks.size() - 1].text;
        if (from.resize(last.size() + 1) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A save could not hold where it resumes"};
        if (!last.empty()) std::memcpy(from.data(), last.data(), last.size());
        from[last.size()] = '\0';
    }
}

/** Copies every pair of @p collection that @p access reads into @p target, under the same identifier and bounds. */
template <cpu_class_t cpu_class_>
status_t save_collection(call_access<cpu_class_> const& access, collection_ids_t const& collection,
                         database_state<cpu_class_>& target, progress_t& progress) noexcept {
    database_of<cpu_class_>& store = *access.state.store;
    ustore_modality_t modality = ustore_modality_any_k;
    if (status_t const held = store.collection_modality(collection.id, modality); held.failed()) return held;
    if (!target.store->has_collection(collection.id)) {
        collection_config_t config;
        if (status_t const shaped = store.collection_config(collection.id, config); shaped.failed()) return shaped;
        if (status_t const made = target.store->create_collection(collection.id, collection.name.view(), modality,
                                                                  config);
            made.failed())
            return made;
    }

    // One window of keys is one commit's memory run of the target, which it lands without a flush in between
    collection_config_t config;
    if (status_t const shaped = target.store->collection_config(collection.id, config); shaped.failed()) return shaped;
    std::size_t const window = config.memory_runs_max_deltas;
    if (entry_layout_of(modality) == entry_layout_t::strings_to_uuids_k)
        return save_strings(access, collection.id, target, window, progress);
    st::basic_vector<stored_key_t> keys;
    st::basic_vector<byte_t> value;
    stored_key_t from = limit<stored_key_t>::min();
    for (;;) {
        keys.clear();
        if (status_t const collected = collect_keys(access, collection.id, from, window, keys); collected.failed())
            return collected;
        for (stored_key_t const& key : keys) {
            stored_value_size_t size = 0;
            if (status_t const measured = store.stored_value_size(collection.id, key, size, access.stamp,
                                                                  access.thread_index);
                measured.failed())
                return measured;
            if (value.resize(size) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "Failed to hold a value the save copies"};
            stored_value_size_t copied = 0;
            if (status_t const read = store.select(collection.id, key, std::span<byte_t> {value}, copied, access.stamp,
                                                   access.thread_index);
                read.failed())
                return read;
            if (status_t const written = target.store->insert(collection.id, key,
                                                              stored_value_view_t {value.data(), copied});
                written.failed())
                return written;
            if (!progress.advance(sizeof(stored_key_t) + copied)) return progress_cancelled_k;
        }
        if (keys.size() < window) return {};
        from = keys[keys.size() - 1];
        if (!advance_key(from)) return {};
    }
}

/**
 *  @brief Implements @c ustore_snapshot_save as a database of this engine in the directory it names, holding every
 *      collection as the snapshot reads it.
 *
 *  A save its progress cancels, or one that fails, removes the volume it wrote and the directories it made, so a retry
 *  at the same path saves afresh.
 */
template <cpu_class_t cpu_class_>
status_t snapshot_save(ustore_snapshot_save_t const& arguments, view_t const& view) noexcept {
    if (!view.snapshot) return {status_code_t::invalid_argument_k, "A save reads through a snapshot"};
    database_state<cpu_class_>& source = state_of<cpu_class_>(arguments);
    call_access<cpu_class_> const access = access_of(source, view);

    ustore_database_open_nvme_t saving {};
    saving.directory = arguments.path;
    if (!saving.directory.length) return {status_code_t::invalid_argument_k, "A save needs a directory"};
    resources_config_t resources;
    database_config_t transactions;
    transactions.default_transaction = source.transaction_shape;
    user_collection_config_t collections = engine_collection_defaults();
    if (status_t const read = read_directories(saving, resources); read.failed()) return read;
    if (status_t const read = read_options(saving, resources, transactions, collections); read.failed()) return read;
    auto [existed, exists_status] = directory_exists(resources.directory.c_str());
    if (exists_status.failed()) return exists_status;

    st::basic_vector<collection_ids_t> const held = source.store->list_collections();
    ustore_size_t total = 0;
    if (arguments.progress)
        for (collection_ids_t const& collection : held) {
            ustore_modality_t modality = ustore_modality_any_k;
            if (status_t const known = source.store->collection_modality(collection.id, modality); known.failed())
                return known;
            status_t const measured = entry_layout_of(modality) == entry_layout_t::strings_to_uuids_k
                                          ? walk_strings(access, collection.id, {}, {}, access.stamp,
                                                         [&](std::string_view text, lookup_result_t found,
                                                             ustore_key_t const&) noexcept {
                                                             if (found == lookup_result_t::written_k)
                                                                 total += text.size() + sizeof(ustore_uuid_t);
                                                             return true;
                                                         })
                                          : walk(access, collection.id, limit<stored_key_t>::min(),
                                                 [&](stored_key_t, stored_value_size_t size) noexcept {
                                                     total += sizeof(stored_key_t) + size;
                                                     return true;
                                                 });
            if (measured.failed()) return measured;
        }
    progress_t progress {arguments.progress, arguments.progress_state, total};

    status_t saved;
    {
        auto [target, open_status] = open_state<cpu_class_>(resources, transactions, source.collection_shape,
                                                            view.scratch);
        saved = open_status;
        for (collection_ids_t const& collection : held)
            if (saved.succeeded()) saved = save_collection(access, collection, *target, progress);
        if (saved.succeeded()) saved = target->store->flush();
        if (saved.succeeded()) return {};
    }

    // The store is closed by now, so the volume file it kept, and the directories the open made, go with it
    for (data_directory_config_t const& directory : resources.data_directories) {
        char path[path_max_bytes_k];
        if (join_path(directory.path.c_str(), volume_file_name_k, path).succeeded()) [[maybe_unused]]
            status_t const removed = remove_file(path);
        [[maybe_unused]] status_t const emptied = remove_directory(directory.path.c_str());
    }
    if (!existed) [[maybe_unused]]
        status_t const emptied = remove_directory(resources.directory.c_str());
    return saved;
}

/** Frees @p snapshot, letting go of the stamp it pinned, and with the last one the runs of the collections dropped
 *  meanwhile. */
template <cpu_class_t cpu_class_>
void snapshot_free(ustore_snapshot_t snapshot) noexcept {
    auto* const handle = handle_cast<snapshot_handle<cpu_class_>>(snapshot);
    database_state<cpu_class_>& state = handle->state;
    delete handle;
    if (atomic_ref_of<cpu_class_, std::size_t> {state.open_snapshots}.fetch_sub(1, std::memory_order_acq_rel) == 1)
        drop_retired(state);
}

#pragma endregion Snapshot Calls

#pragma region Lifetime

/** Frees @p transaction, retiring it from the registry. */
template <cpu_class_t cpu_class_>
void transaction_free(ustore_transaction_t transaction) noexcept {
    delete handle_cast<transaction_handle<cpu_class_>>(transaction);
}

/** Flushes and unmounts @p database ahead of freeing it, which every transaction of it must precede. */
template <cpu_class_t cpu_class_>
void database_free(ustore_database_t database) noexcept {
    delete handle_cast<database_state<cpu_class_>>(database);
}

#pragma endregion Lifetime

} // namespace unum::ustore::nvme

// The graph, docs and pages calls build on the accesses, writes and catalog lookups above, and the table below names
// them
#include "docs_calls.hpp"  // `docs_write`
#include "graph_calls.hpp" // `graph_read_edges`, `graph_write_edges`, `graph_remove_vertices`
#include "pages_calls.hpp" // `pages_import`, `pages_insert`, `commit_page_edits`

namespace unum::ustore::nvme {

#pragma region Table

template <cpu_class_t cpu_class_>
database_verbs_t const nvme_variant<cpu_class_>::database_verbs_k {
    .describe = &database_describe<cpu_class_>,
    .clear = &database_clear<cpu_class_>,
    .compact = &database_compact<cpu_class_>,
    .snapshot_init = &snapshot_init<cpu_class_>,
    .snapshot_save = &snapshot_save<cpu_class_>,
    .collection_list = &collection_list<cpu_class_>,
    .collection_create = &collection_create<cpu_class_>,
    .collection_find = &collection_find<cpu_class_>,
    .collection_drop = &collection_drop<cpu_class_>,
    .transaction_init = &transaction_init<cpu_class_>,
    .transaction_stage = &transaction_stage<cpu_class_>,
    .transaction_commit = &transaction_commit<cpu_class_>,
    .transaction_rollback = &transaction_rollback<cpu_class_>,
    .transaction_free = &transaction_free<cpu_class_>,
    .snapshot_free = &snapshot_free<cpu_class_>,
    .database_free = &database_free<cpu_class_>,
};

template <cpu_class_t cpu_class_>
blobs_verbs_t const nvme_variant<cpu_class_>::values_verbs_k {
    .remove = &remove_values<cpu_class_>,
    .read = &read_values<cpu_class_>,
    .scan = &scan_values<cpu_class_>,
};

template <cpu_class_t cpu_class_>
blobs_verbs_t const nvme_variant<cpu_class_>::blobs_verbs_k {
    .write = &blobs_write<cpu_class_>,
    .remove = &blobs_remove<cpu_class_>,
    .read = &blobs_read<cpu_class_>,
    .scan = &blobs_scan<cpu_class_>,
    .sample = &blobs_sample<cpu_class_>,
    .measure = &blobs_measure<cpu_class_>,
};

template <cpu_class_t cpu_class_>
strings_verbs_t const nvme_variant<cpu_class_>::strings_verbs_k {
    .write = &strings_write<cpu_class_>,
    .remove = &strings_remove<cpu_class_>,
    .read = &strings_read<cpu_class_>,
    .scan = &strings_scan<cpu_class_>,
};

template <cpu_class_t cpu_class_>
docs_verbs_t const nvme_variant<cpu_class_>::docs_verbs_k {
    .write = &docs_write<cpu_class_>,
    .read = &docs_read_over_blobs<values_verbs_k, &resolve_modality<cpu_class_>>,
    .gist = &docs_gist_over_blobs<values_verbs_k, &resolve_modality<cpu_class_>>,
    .gather = &docs_gather_over_blobs<values_verbs_k, &resolve_modality<cpu_class_>>,
    .remove = &remove_over_blobs<values_verbs_k, &resolve_modality<cpu_class_>, ustore_modality_docs_k>,
    .scan = &scan_over_blobs<values_verbs_k, &resolve_modality<cpu_class_>, ustore_modality_docs_k>,
};

template <cpu_class_t cpu_class_>
graph_verbs_t const nvme_variant<cpu_class_>::graph_verbs_k {
    .read_edges = &graph_read_edges<cpu_class_>,
    .write_edges = &graph_write_edges<cpu_class_>,
    .remove_edges = &graph_remove_edges<cpu_class_>,
    .write_vertices = &graph_write_vertices<cpu_class_>,
    .remove_vertices = &graph_remove_vertices<cpu_class_>,
    .scan_vertices = &scan_over_blobs<values_verbs_k, &resolve_modality<cpu_class_>, ustore_modality_graph_k>,
};

template <cpu_class_t cpu_class_>
pages_verbs_t const nvme_variant<cpu_class_>::pages_verbs_k {
    .import = &pages_import<cpu_class_>,
    .insert = &pages_insert<cpu_class_>,
    .remove = &pages_remove<cpu_class_>,
    .move = &pages_move<cpu_class_>,
    .splice = &pages_splice<cpu_class_>,
    .set_attributes = &pages_set_attributes<cpu_class_>,
    .remove_attributes = &pages_remove_attributes<cpu_class_>,
    .read = &pages_read_over_blobs<values_verbs_k, &resolve_modality<cpu_class_>>,
    .scan = &scan_over_blobs<values_verbs_k, &resolve_modality<cpu_class_>, ustore_modality_pages_k>,
};

template <cpu_class_t cpu_class_>
engine_t const nvme_variant<cpu_class_>::engine_k {
    .database = &database_verbs_k,
    .blobs = &blobs_verbs_k,
    .strings = &strings_verbs_k,
    .docs = &docs_verbs_k,
    .graph = &graph_verbs_k,
    .pages = &pages_verbs_k,
};

#pragma endregion Table

} // namespace unum::ustore::nvme
