/**
 *  @file src/nvme/engine.cpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Embedded on-disk engine over the LSM templates of `include/ustore/blobs/`, one NVMe collection per
 *      collection of the C ABI.
 *
 *  One mount opens the volumes and one database of 16-byte keys lives on them. Every collection of the C ABI is one
 *  collection of that database under the same UUID, and three more are the engine's own: the main collection under
 *  the nil UUID, the catalog, and the names. The catalog maps every other collection's UUID onto its modality byte
 *  and its name, and the names file every name as a string mapped onto its `name_record_t`. A strings collection, and
 *  the names, file their strings in the buckets of `include/ustore/strings/string_keys.hpp`.
 *
 *  Every write of a caller goes through a transaction of the database, the caller's or one the call commits alone, so
 *  the registry records every commit a watched read may conflict with. The catalog and the names, which no caller's
 *  transaction reaches, change straight in the store under a claim of their own, so a collection created or dropped
 *  never shows as a commit to the transactions running beside it. A call outside a transaction reads at the head, or
 *  at its snapshot's stamp. Reads resolve collections at the head, as every engine does, but through a snapshot, which
 *  resolves them as they stood at its stamp. A walk merges the runs of one collection with its write buffer and,
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
 *  write buffers and the merges keep for as long as the pin lives, so a snapshot kept across calls stalls no writer.
 */

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

#include <forkunion/atomics.hpp>       // `fu::preferred_atomic_ref`
#include <forkunion/capabilities.hpp>  // `fu::preferred_yield_t`
#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "ustore/blobs.h"
#include "ustore/db.h"
#include "ustore/ranges.hpp" // `place_batch_t`, `strided`
#include "ustore/strings.h"

#include "ustore/blobs/nvme_database.hpp"      // `database`, `watch_schedule_t`, `write_buffer_status_t`
#include "ustore/blobs/nvme_tree.hpp"          // `user_collection_config_t`, `validate`
#include "ustore/strings/string_keys.hpp"      // `bucket_key_of`, `bucket_view_t`, `write_bucket`
#include "ustore/utilities/blocking_files.hpp" // `directory_exists`
#include "ustore/utilities/nvme_volume.hpp"    // `resources_config_t`, `open_resources`, `mounted_storage_t`
#include "ustore/utilities/ram_buffers.hpp"    // `arena_allocate`, `arena_allocator`, `growth_status`

#include "engine.hpp"             // `engine_t`, `handle_t`, `handle_cast`, `view_t`, `nvme_placement_of`
#include "graph_store.hpp"        // `serve_graph`
#include "utilities/batches.hpp"  // `strings_arg_t`, `places_of`, `validate_write`
#include "utilities/outputs.hpp"  // `read_outputs_t`, `scan_outputs_t`, `collection_list_outputs_t`
#include "utilities/progress.hpp" // `progress_t`, `progress_cancelled_k`

namespace unum::ustore::nvme {
using namespace unum::ustore::blobs;
using namespace unum::ustore::strings;
using namespace unum::ustore::utilities;

#pragma region Store

/** The key every collection of this engine stores: the 16 bytes of the C ABI's key, ordered like `memcmp`. */
using stored_key_t = uuid_stored_key_t;

/** The store every call runs against, whose volumes live in files. */
using store_t = database<stored_key_t>;

/** One transaction over the collections of the store. */
using store_transaction_t = typename store_t::transaction_t;

/** A transaction owned by whoever asked for it, retired when it dies. */
using store_transaction_pointer_t = typename store_t::transaction_unique_ptr_t;

/** The mount the store's volumes are opened over, which lends the store its router and channels. */
using storage_mount_t = mounted_storage_t::files_mount_t;

/** The collection the catalog lives in, which no minted UUIDv7 spells, as its byte 6 is `0x7X`. */
inline constexpr ustore_collection_id_t catalog_collection_k {
    {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFD}}};

/** The collection the names of collections live in, which no minted UUIDv7 spells either. */
inline constexpr ustore_collection_id_t names_collection_k {
    {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE}}};

/** The directory a mount opens its only volume in, under the database's own directory. */
inline constexpr char data_directory_name_k[] = "data";

/** Bytes the only volume of a database spans when its configuration names no capacity. */
inline constexpr std::size_t default_volume_bytes_k = 1024ull * 1024ull * 1024ull;

/** Channels the threads outside an executor share, beyond one per thread of the executor opening the database. */
inline constexpr std::size_t engine_shared_io_channels_k = 4;

/** Bytes the largest value of a collection takes, which a bucket of strings must also fit in. */
inline constexpr stored_value_size_t engine_value_max_size_k = 1024u * 1024u;

/** Bytes one collection's write buffer and one collection's value cache each hold. */
inline constexpr std::size_t engine_collection_buffer_bytes_k = 16ull * 1024ull * 1024ull;

/** Writes one collection's write buffer holds at once. */
inline constexpr std::size_t engine_collection_buffer_elements_k = 64ull * 1024ull;

/** Bytes level zero of a collection admits before a compaction is due. */
inline constexpr std::size_t engine_first_level_bytes_k = 16ull * 1024ull * 1024ull;

/** Bytes one caller's transaction buffers, which it takes eagerly when it opens. */
inline constexpr std::size_t engine_transaction_bytes_k = 4ull * 1024ull * 1024ull;

/** Writes one caller's transaction buffers at once. */
inline constexpr std::size_t engine_transaction_elements_k = 16ull * 1024ull;

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
inline stored_key_t stored_of(ustore_uuid_t const& uuid) noexcept { return stored_key_t::from_bytes(uuid.bytes); }

/** The key of the C ABI whose 16 bytes @p stored holds. */
inline ustore_key_t key_of(stored_key_t const& stored) noexcept {
    ustore_key_t key;
    stored.to_bytes(key.uuid.bytes);
    return key;
}

/** Whether @p collection is one the engine keeps for itself, which no call of the C ABI may address. */
inline bool reserved(ustore_collection_id_t const& collection) noexcept {
    return collection == catalog_collection_k || collection == names_collection_k;
}

/** The shape every collection of a database takes unless its open names another. */
inline user_collection_config_t engine_collection_defaults() noexcept {
    user_collection_config_t config;
    config.cache_limit = engine_collection_buffer_bytes_k;
    config.write_buffer_max_elements = engine_collection_buffer_elements_k;
    config.write_buffer_max_bytes = engine_collection_buffer_bytes_k;
    config.first_level_max_bytes = engine_first_level_bytes_k;
    config.value_max_size = engine_value_max_size_k;
    return config;
}

#pragma endregion Store

#pragma region Handles

/** The calls this engine serves, which stamp every handle it hands out. */
extern engine_t const engine_k;

/** Everything one open database holds, which is the database handle of the C ABI. */
struct database_state_t : handle_t {

    /** The budgets and the mount the store borrows its router and channels from. */
    resources_shared_ptr_t resources;

    /** The store itself, unmounted before the mount underneath it is torn down. */
    std::unique_ptr<store_t> store;

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

    /** Snapshots not yet freed, through `fu::preferred_atomic_ref`. */
    std::size_t open_snapshots = 0;

    /** Collections the catalog no longer lists whose runs an open snapshot may still read, dropped once the last
     *  snapshot is freed; guarded by @c catalog_claim. */
    st::basic_vector<ustore_collection_id_t> retired_collections;

    database_state_t() noexcept : handle_t {&engine_k} {}
    database_state_t(database_state_t const&) = delete;
    database_state_t& operator=(database_state_t const&) = delete;

    /** Flushes and unmounts the store while its mount is still there, then lets both go. */
    ~database_state_t() noexcept {
        if (store) { [[maybe_unused]] status_t const unmounted = store->unmount(); }
        store.reset();
        resources.reset();
    }
};

/** A transaction handle of the C ABI. */
struct transaction_handle_t : handle_t {

    /** The database the transaction runs against. */
    database_state_t& state;

    /** The transaction itself, retired when the handle dies. */
    store_transaction_pointer_t held;

    /** Whether the caller staged it, which a rollback withdraws and a commit consumes, refusing writes meanwhile. */
    bool staged = false;

    transaction_handle_t(database_state_t& owner, store_transaction_pointer_t&& opened) noexcept
        : handle_t {&engine_k}, state(owner), held(std::move(opened)) {}
};

/** A snapshot handle of the C ABI, pinning the stamp it reads at in the store's ledger until freed. */
struct snapshot_handle_t : handle_t {

    /** The database the snapshot reads. */
    database_state_t& state;

    /** The pin, which keeps every version a read at its stamp needs out of the reach of a flush and a merge. */
    stamp_pin_t pin;

    explicit snapshot_handle_t(database_state_t& owner) noexcept : handle_t {&engine_k}, state(owner) {}
};

/** Holds the catalog of one database for the life of the guard, waiting while another change holds it. */
class catalog_guard_t {
    std::int32_t& claim_;

  public:
    explicit catalog_guard_t(database_state_t& state) noexcept : claim_(state.catalog_claim) {
        fu::preferred_atomic_ref<std::int32_t> const word {claim_};
        std::int32_t expected_free = 0;
        while (!word.compare_exchange_weak(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
            fu::preferred_yield_t {}(claim_, expected_free, std::size_t {0});
            expected_free = 0;
        }
    }
    catalog_guard_t(catalog_guard_t const&) = delete;
    catalog_guard_t& operator=(catalog_guard_t const&) = delete;
    ~catalog_guard_t() noexcept { fu::preferred_atomic_ref<std::int32_t> {claim_}.store(0, std::memory_order_release); }
};

/** The database a call addresses. */
template <typename arguments_type_>
database_state_t& state_of(arguments_type_ const& arguments) noexcept {
    return *handle_cast<database_state_t>(database_of(arguments));
}

/** Opens a transaction of @p state whose buffer @p shape bounds. */
inline expected<store_transaction_pointer_t> begin_transaction(database_state_t& state,
                                                               transaction_config_t const& shape) noexcept {
    return state.store->create_transaction(shape);
}

#pragma endregion Handles

#pragma region Access

/** Where one call reads and writes: through a transaction, the caller's or its own, or at the head for reads. */
struct access_t {

    /** The database the call runs against. */
    database_state_t& state;

    /** The transaction it reads and writes through, or null to read at the head. */
    store_transaction_t* transaction = nullptr;

    /** Whether the transaction watches what the call reads and writes. */
    watch_schedule_t schedule = watch_schedule_t::never_k;

    /** Where the call keeps the values it read until it exports them. */
    ustore_arena_t scratch = nullptr;

    /** The stamp a call outside a transaction reads at: a snapshot's, or the head's. */
    transaction_stamp_t stamp = head_stamp_k;

    /** The thread of the executor that opened the database running the call, which reads on its own channel and
     *  epoch slot, or @c outside_executor_k for any other thread. */
    std::size_t thread_index = outside_executor_k;
};

/** The schedule the watches of a call asking for @p watch take. */
constexpr watch_schedule_t schedule_of(ustore_watch_t watch) noexcept {
    return watch == ustore_watch_keys_k ? watch_schedule_t::on_commit_k : watch_schedule_t::never_k;
}

/** The thread index a call running on @p view's thread reads @p state with. */
inline std::size_t thread_index_of(database_state_t const& state, view_t const& view) noexcept {
    return view.executor && view.executor == state.executor ? view.thread_index : outside_executor_k;
}

/** The access of a call reading through @p view: its transaction, its snapshot's stamp, or the head. */
inline access_t access_of(database_state_t& state, view_t const& view) noexcept {
    std::size_t const thread_index = thread_index_of(state, view);
    if (view.snapshot)
        return {state,
                nullptr,
                watch_schedule_t::never_k,
                view.scratch,
                handle_cast<snapshot_handle_t>(view.snapshot)->pin.stamp(),
                thread_index};
    if (!view.transaction) return {state, nullptr, watch_schedule_t::never_k, view.scratch, head_stamp_k, thread_index};
    store_transaction_t* const transaction = handle_cast<transaction_handle_t>(view.transaction)->held.get();
    return {state, transaction, schedule_of(view.watch), view.scratch, head_stamp_k, thread_index};
}

/** Watches @p keys of @p collection through the transaction of @p access, when it watches at all. */
inline status_t watch_keys(access_t const& access, collection_id_t collection,
                           std::span<stored_key_t const> keys) noexcept {
    if (!access.transaction || access.schedule == watch_schedule_t::never_k || keys.empty()) return {};
    return access.transaction->watch_more<watch_schedule_t::on_commit_k>(collection, keys);
}

/** Watches every key of @p collection from @p lower to @p upper through the transaction of @p access, when it watches
 *  at all, so a commit landing a key the walk over them did not find conflicts too. */
inline status_t watch_range(access_t const& access, collection_id_t collection, stored_key_t lower,
                            stored_key_t upper) noexcept {
    if (!access.transaction || access.schedule == watch_schedule_t::never_k) return {};
    return access.transaction->watch_range<watch_schedule_t::on_commit_k>(collection, lower, upper);
}

/** Copies @p size bytes into the scratch of @p access through @p copy, answering the present value they hold. */
template <typename copy_type_>
expected<value_view_t> copied_value(access_t const& access, stored_value_size_t size, copy_type_&& copy) noexcept {
    if (size == 0) {
        if (status_t const copied = copy(std::span<byte_t> {}); copied.failed()) return copied;
        return value_view_t {empty_value_bytes_k, std::size_t {0}};
    }
    auto [bytes, allocate_status] = arena_allocate<byte_t>(access.scratch, size);
    if (allocate_status.failed()) return allocate_status;
    if (status_t const copied = copy(bytes); copied.failed()) return copied;
    return value_view_t {bytes};
}

/**
 *  @brief The value of @p key in @p collection as @p access sees it, copied into its scratch, or a missing value.
 *
 *  A transaction's own write outranks the store, and its own removal hides the store's value. A read through a
 *  watching transaction watches the key, its absence included, so a commit landing on it in between conflicts.
 */
inline expected<value_view_t> read_value(access_t const& access, collection_id_t collection,
                                         stored_key_t key) noexcept {
    store_t& store = *access.state.store;
    store_transaction_t* const transaction = access.transaction;
    if (transaction) {
        write_buffer_status_t const own = transaction->uncommitted(collection, key);
        if (own == write_buffer_status_t::removed_k) return value_view_t {};
        if (own == write_buffer_status_t::found_k) {
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
                    return transaction->select_committed<watch_schedule_t::never_k>(collection, key, into, copied,
                                                                                    thread_index);
                return transaction->select_committed<watch_schedule_t::on_commit_k>(collection, key, into, copied,
                                                                                    thread_index);
            });
            if (read) return read;
            if (read.status() == status_code_t::invalid_argument_k) continue;
            if (read.status() != status_code_t::not_found_k) return read.status();
        }
        stored_key_t const absent[1] {key};
        if (status_t const watched = watch_keys(access, collection, absent); watched.failed()) return watched;
        return value_view_t {};
    }
    return status_t {status_code_t::contended_k, "A value kept changing its size while it was read"};
}

/** Writes @p value under @p key of @p collection, a missing value removing it: through the transaction of @p access,
 *  or straight into the store for the catalog and the names, which no transaction reaches. */
inline status_t write_value(access_t const& access, collection_id_t collection, stored_key_t key,
                            value_view_t value) noexcept {
    stored_value_view_t const bytes {reinterpret_cast<byte_t const*>(value.data()), value.size()};
    if (!access.transaction) {
        store_t& store = *access.state.store;
        return value ? store.insert(collection, key, bytes) : store.remove(collection, key);
    }
    store_transaction_t& transaction = *access.transaction;
    bool const watched = access.schedule != watch_schedule_t::never_k;
    if (!value)
        return watched ? transaction.remove<watch_schedule_t::on_commit_k>(collection, key)
                       : transaction.remove<watch_schedule_t::never_k>(collection, key);
    return watched ? transaction.insert<watch_schedule_t::on_commit_k>(collection, key, bytes)
                   : transaction.insert<watch_schedule_t::never_k>(collection, key, bytes);
}

/** One write of a transaction to its own key, which a walk merges with the store. */
struct own_write_t {
    stored_key_t key;
    stored_value_size_t size = 0;
    write_buffer_status_t status = write_buffer_status_t::found_k;
};

/**
 *  @brief Hands @p visitor every present key of @p collection from @p from up, as @p access sees it, in ascending
 *      order, as @c visitor(key, size) answering whether the walk carries on.
 *
 *  The store's walk holds the view it loaded, which a flush waits for, so @p visitor collects and calls nothing of the
 *  database.
 */
template <typename visitor_type_>
status_t walk(access_t const& access, collection_id_t collection, stored_key_t from, visitor_type_&& visitor) noexcept {
    store_t& store = *access.state.store;
    if (!access.transaction)
        return store.scan(
            collection, from,
            [&](stored_key_t key, select_result_t state, stored_value_size_t size) noexcept {
                return state != select_result_t::success_k || visitor(key, size);
            },
            access.stamp, access.thread_index);

    st::basic_vector<own_write_t, arena_allocator<own_write_t>> own {{access.scratch}};
    status_t listed;
    access.transaction->for_each_uncommitted(
        collection, [&](stored_key_t key, write_buffer_status_t status, stored_value_size_t size) noexcept {
            if (key < from || listed.failed()) return;
            listed = growth_status(own.push_back(own_write_t {key, size, status}));
        });
    if (listed.failed()) return listed;
    std::sort(own.begin(), own.end(),
              [](own_write_t const& first, own_write_t const& second) noexcept { return first.key < second.key; });

    // The transaction's writes outrank the store's version of the same key, and its removals hide it
    std::size_t next = 0;
    bool carries_on = true;
    auto const emit_own = [&](own_write_t const& write) noexcept {
        if (write.status == write_buffer_status_t::found_k) carries_on = visitor(write.key, write.size);
        return carries_on;
    };
    // The store's side is what the transaction started at, which its reads of single keys answer too
    status_t const walked = store.scan(
        collection, from,
        [&](stored_key_t key, select_result_t state, stored_value_size_t size) noexcept {
            for (; next != own.size() && own[next].key < key; ++next)
                if (!emit_own(own[next])) return false;
            if (next != own.size() && own[next].key == key) return emit_own(own[next++]);
            if (state != select_result_t::success_k) return true;
            carries_on = visitor(key, size);
            return carries_on;
        },
        access.transaction->start_stamp(), access.thread_index);
    if (walked.failed()) return walked;
    for (; carries_on && next != own.size(); ++next) emit_own(own[next]);
    return {};
}

/** Collects into @p keys up to @p limit present keys of @p collection from @p from up, as @p access sees them. */
template <typename keys_type_>
status_t collect_keys(access_t const& access, collection_id_t collection, stored_key_t from, std::size_t limit,
                      keys_type_& keys) noexcept {
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
inline status_t compact_due(database_state_t& state) noexcept {
    for (std::size_t pass = 0; pass != compaction_passes_k && state.store->needs_compaction(); ++pass)
        if (status_t const compacted = state.store->compact(); compacted.failed()) return compacted;
    return {};
}

/** Flushes @p state when @p durability asks for it, then compacts what the write left due. */
inline status_t settle(database_state_t& state, ustore_durability_t durability) noexcept {
    if (durability == ustore_durability_flushed_k)
        if (status_t const flushed = state.store->flush(); flushed.failed()) return flushed;
    return compact_due(state);
}

/**
 *  @brief Commits what @p stage stages through a transaction of its own, retrying conflicts of its watched reads.
 *  @param[in] count The writes @p stage makes at most, sizing the transaction's buffer.
 *  @param[in] bytes The bytes those writes take, a hint the buffer doubles past when @p stage outgrows it.
 *  @param[in] schedule Whether @p stage watches what it reads, which a read-modify-write needs.
 *
 *  @p stage is invoked as `stage(access)` on every attempt, and a failure it answers ends the call.
 */
template <typename stage_type_>
status_t commit_alone(database_state_t& state, ustore_arena_t scratch, std::size_t count, std::size_t bytes,
                      watch_schedule_t schedule, stage_type_&& stage) noexcept {
    transaction_config_t shape;
    shape.write_buffer_max_elements = std::max<std::size_t>(count, 1);
    shape.write_buffer_max_bytes = std::max<std::size_t>(bytes, 1);
    for (std::size_t attempt = 0; attempt != commit_attempts_k; ++attempt) {
        auto [held, begin_status] = begin_transaction(state, shape);
        if (begin_status.failed()) return begin_status;
        access_t const access {state, held.get(), schedule, scratch};
        status_t const staged = stage(access);
        if (staged == status_code_t::capacity_exhausted_k &&
            shape.write_buffer_max_bytes < storage_limits_t::transaction_write_buffer_max_capacity_k) {
            shape.write_buffer_max_bytes *= 2;
            shape.write_buffer_max_elements = std::min(shape.write_buffer_max_elements * 2,
                                                       storage_limits_t::transaction_write_buffer_elements_max_count_k);
            continue;
        }
        if (staged.failed()) return staged;
        status_t const committed = held->commit();
        if (committed == status_code_t::write_conflict_k && schedule != watch_schedule_t::never_k) continue;
        return committed;
    }
    return {status_code_t::write_conflict_k, "Concurrent commits kept conflicting with the call"};
}

/** Stages @p stage into the transaction @p view carries, or commits it as a transaction of its own, flushed when
 *  @p durability asks, for every call that writes. */
template <typename stage_type_>
status_t serve_write(database_state_t& state, view_t const& view, ustore_durability_t durability, std::size_t count,
                     std::size_t bytes, watch_schedule_t schedule, stage_type_&& stage) noexcept {
    if (view.transaction) {
        transaction_handle_t& handle = *handle_cast<transaction_handle_t>(view.transaction);
        if (handle.staged)
            return {status_code_t::invalid_argument_k, "A staged transaction takes no writes until rolled back"};
        access_t access = access_of(state, view);
        if (schedule != watch_schedule_t::never_k) access.schedule = schedule;
        return stage(access);
    }
    if (status_t const committed = commit_alone(state, view.scratch, count, bytes, schedule, stage); committed.failed())
        return committed;
    return settle(state, durability);
}

#pragma endregion Maintenance

#pragma region Catalog

/** What the catalog records of a collection: its modality, then its name, empty for an anonymous one. */
struct catalog_entry_t {
    ustore_modality_t modality = ustore_modality_any_k;
    std::string_view name;
};

/** Reads the catalog entry @p held, whose name views the same bytes, refusing bytes too short to be one. */
inline expected<catalog_entry_t> read_catalog_entry(value_view_t held) noexcept {
    if (held.size() == 0) return status_t {status_code_t::malformed_k, "The catalog entry of a collection is empty"};
    return catalog_entry_t {static_cast<ustore_modality_t>(*held.begin()), {held.c_str() + 1, held.size() - 1}};
}

/** What the catalog records of @p collection at @p stamp, refusing one it does not list, the reserved ones included. */
inline expected<catalog_entry_t> look_up(database_state_t& state, ustore_arena_t scratch,
                                         ustore_collection_id_t const& collection,
                                         transaction_stamp_t stamp = head_stamp_k,
                                         std::size_t thread_index = outside_executor_k) noexcept {
    if (collection == ustore_collection_main_k) return catalog_entry_t {};
    access_t const listed {state, nullptr, watch_schedule_t::never_k, scratch, stamp, thread_index};
    auto [held, read_status] = read_value(listed, catalog_collection_k, stored_of(collection.uuid));
    if (read_status.failed()) return read_status;
    if (!held) return status_t {status_code_t::not_found_k, "No such collection"};
    return read_catalog_entry(held);
}

/** Refuses a batch addressing a collection the catalog does not list, as @p view reads it, or one keyed otherwise
 *  than @p keying, looking each run of tasks on one collection up once. */
inline status_t resolve_runs(database_state_t& state, view_t const& view,
                             strided_iterator<ustore_collection_id_t const> collections, std::size_t count,
                             keying_t keying) noexcept {
    transaction_stamp_t const stamp = view.snapshot ? handle_cast<snapshot_handle_t>(view.snapshot)->pin.stamp()
                                                    : head_stamp_k;
    for (std::size_t index = 0; index != count; ++index) {
        if (index != 0 && collections && collections[index] == collections[index - 1]) continue;
        ustore_collection_id_t const collection = collections ? collections[index] : ustore_collection_main_k;
        auto [entry, look_up_status] = look_up(state, view.scratch, collection, stamp, thread_index_of(state, view));
        if (look_up_status.failed()) return look_up_status;
        if (keying_of(entry.modality) != keying)
            return {status_code_t::invalid_argument_k,
                    keying == keying_t::strings_k ? "The collection holds no strings" : "The collection holds strings"};
        if (index == 0 && !collections) return {};
    }
    return {};
}

/** The collection task @p index of @p collections names, the main one when they name none. */
inline ustore_collection_id_t task_collection(strided_iterator<ustore_collection_id_t const> collections,
                                              std::size_t index) noexcept {
    return collections ? collections[index] : ustore_collection_main_k;
}

#pragma endregion Catalog

#pragma region Strings

/** One string a batch writes or removes, ordered so the tasks of one bucket sit together and the last one wins. */
struct string_task_t {
    ustore_collection_id_t collection;
    ustore_key_t bucket;
    std::string_view text;

    /** The value written, or a missing one for a removal. */
    value_view_t value;

    /** Where the task sat in the batch. */
    std::size_t index = 0;
};

/** Whether @p first goes before @p second: by collection, bucket, string and batch position. */
inline bool precedes(string_task_t const& first, string_task_t const& second) noexcept {
    if (first.collection != second.collection) return first.collection < second.collection;
    if (first.bucket != second.bucket) return first.bucket < second.bucket;
    if (first.text != second.text) return sz::string_view_t {first.text} < sz::string_view_t {second.text};
    return first.index < second.index;
}

/**
 *  @brief Rewrites every bucket @p tasks touch through @p access, merging each with the tasks on its strings.
 *
 *  A bucket is read through the access, so a watching transaction conflicts with any commit rewriting it in
 *  between, and a bucket left without entries is removed.
 */
inline status_t stage_strings(access_t const& access, std::span<string_task_t> tasks) noexcept {
    std::sort(tasks.begin(), tasks.end(), precedes);
    st::basic_vector<bucket_entry_t, arena_allocator<bucket_entry_t>> merged {{access.scratch}};
    for (std::size_t begin = 0; begin != tasks.size();) {
        std::size_t end = begin + 1;
        while (end != tasks.size() && tasks[end].collection == tasks[begin].collection &&
               tasks[end].bucket == tasks[begin].bucket)
            ++end;
        collection_id_t const collection = tasks[begin].collection;
        stored_key_t const bucket = stored_of(tasks[begin].bucket.uuid);
        auto [stored, read_status] = read_value(access, collection, bucket);
        if (read_status.failed()) return read_status;
        bucket_view_t const existing {stored};

        // Two sorted runs meet, where the last task naming a string replaces or removes its entry, in room reserved
        merged.clear();
        if (status_t const room = growth_status(merged.reserve(existing.size() + (end - begin))); room.failed())
            return room;
        auto const keep = [&](bucket_entry_t const& entry) noexcept {
            [[maybe_unused]] st::status_t const kept = merged.push_back(st::assume_reserved, bucket_entry_t {entry});
        };
        std::size_t kept = 0;
        for (std::size_t task = begin; task != end;) {
            std::size_t last = task;
            while (last + 1 != end && tasks[last + 1].text == tasks[task].text) ++last;
            std::string_view const text = tasks[task].text;
            for (; kept != existing.size() && sz::string_view_t {existing.text_of(kept)} < sz::string_view_t {text};
                 ++kept)
                keep(existing[kept]);
            if (kept != existing.size() && existing.text_of(kept) == text) ++kept;
            if (tasks[last].value) keep(bucket_entry_t {text, tasks[last].value});
            task = last + 1;
        }
        for (; kept != existing.size(); ++kept) keep(existing[kept]);

        value_view_t rewritten;
        if (!merged.empty()) {
            std::span<bucket_entry_t const> const entries {merged};
            auto [bytes, allocate_status] = arena_allocate<byte_t>(access.scratch, bucket_bytes(entries));
            if (allocate_status.failed()) return allocate_status;
            write_bucket(reinterpret_cast<ustore_byte_t*>(bytes.data()), entries);
            rewritten = value_view_t {bytes};
        }
        if (status_t const written = write_value(access, collection, bucket, rewritten); written.failed())
            return written;
        begin = end;
    }
    return {};
}

/** The value of @p text in @p collection as @p access sees it, a missing one when no bucket holds the string. */
inline expected<value_view_t> read_string(access_t const& access, collection_id_t collection,
                                          std::string_view text) noexcept {
    auto [stored, read_status] = read_value(access, collection, stored_of(bucket_key_of(text).uuid));
    if (read_status.failed()) return read_status;
    bucket_view_t const bucket {stored};
    std::size_t const position = bucket.find(text);
    return position == bucket.size() ? value_view_t {} : bucket.value_of(position);
}

/** Appends to @p outputs up to @p limit strings of @p collection starting with @p prefix, from @p start on. */
inline status_t scan_strings(access_t const& access, collection_id_t collection, std::string_view prefix,
                             std::string_view start, std::size_t limit, strings_scan_outputs_t& outputs) noexcept {
    if (limit == 0) return {};
    bucket_window_t const window = bucket_window_of(prefix);
    stored_key_t const last = stored_of(window.last.uuid);
    std::string_view const lower = sz::string_view_t {start} > sz::string_view_t {prefix} ? start : prefix;
    stored_key_t next = stored_of(bucket_key_of(lower).uuid);
    std::size_t found = 0;
    st::basic_vector<stored_key_t, arena_allocator<stored_key_t>> buckets {{access.scratch}};
    while (found != limit && !(last < next)) {
        // Every bucket of the window holds a string past the lower bound but the first, so the next walk wants fewer
        buckets.clear();
        std::size_t const wanted = limit - found + 1;
        if (status_t const collected = collect_keys(access, collection, next, wanted, buckets); collected.failed())
            return collected;
        std::size_t inside = 0;
        while (inside != buckets.size() && !(last < buckets[inside])) ++inside;
        for (std::size_t index = 0; index != inside && found != limit; ++index) {
            auto [stored, read_status] = read_value(access, collection, buckets[index]);
            if (read_status.failed()) return read_status;
            bucket_view_t const bucket {stored};
            std::size_t entry = bucket.lower_bound(lower);
            for (; entry != bucket.size() && found != limit; ++entry) {
                std::string_view const text = bucket.text_of(entry);
                if (!text.starts_with(prefix)) continue;
                if (status_t const pushed = outputs.push(text); pushed.failed()) return pushed;
                ++found;
            }
        }
        if (inside != buckets.size() || buckets.size() < wanted || inside == 0) break;
        next = buckets[inside - 1];
        if (!advance_key(next)) break;
    }
    return {};
}

#pragma endregion Strings

#pragma region Names

/** The record @p name maps onto at the head, and whether any collection carries the name. */
inline expected<name_record_t> look_up_name(access_t const& access, std::string_view name, bool& present) noexcept {
    present = false;
    auto [record, read_status] = read_string(access, names_collection_k, name);
    if (read_status.failed()) return read_status;
    if (!record) return name_record_t {};
    present = true;
    return read_name_record(record);
}

/** Stages @p name mapped onto @p record, or its removal when @p record is null. */
inline status_t stage_name(access_t const& access, std::string_view name, name_record_t const* record) noexcept {
    string_task_t task {names_collection_k, bucket_key_of(name), name,
                        record ? value_view_t {record->view()} : value_view_t {}, 0};
    return stage_strings(access, std::span<string_task_t> {&task, 1});
}

#pragma endregion Names

#pragma region Clearing

/**
 *  @brief Empties @p collection keyed as @p keying: removing every key for @c ustore_drop_pairs_k, or emptying every
 *      value for @c ustore_drop_values_k, a strings collection keeping its strings.
 *
 *  The keys are rewritten in windows committed one after another, so the transaction of each stays bounded.
 */
inline status_t empty_collection(database_state_t& state, ustore_arena_t scratch, collection_id_t collection,
                                 keying_t keying, ustore_drop_mode_t mode) noexcept {
    access_t const head {state, nullptr, watch_schedule_t::never_k, scratch};
    st::basic_vector<stored_key_t, arena_allocator<stored_key_t>> keys {{scratch}};
    stored_key_t from = limit<stored_key_t>::min();
    for (;;) {
        keys.clear();
        if (status_t const collected = collect_keys(head, collection, from, clear_window_keys_k, keys);
            collected.failed())
            return collected;
        if (keys.empty()) return {};
        status_t const committed = commit_alone(
            state, scratch, keys.size(), keys.size() * sizeof(bucket_word_t) * 4, watch_schedule_t::never_k,
            [&](access_t const& access) noexcept -> status_t {
                for (std::size_t index = 0; index != keys.size(); ++index) {
                    value_view_t emptied;
                    if (mode == ustore_drop_values_k && keying == keying_t::uuids_k)
                        emptied = value_view_t::make_empty();
                    if (mode == ustore_drop_values_k && keying == keying_t::strings_k) {
                        auto [stored, read_status] = read_value(access, collection, keys[index]);
                        if (read_status.failed()) return read_status;
                        bucket_view_t const bucket {stored};
                        st::basic_vector<bucket_entry_t, arena_allocator<bucket_entry_t>> entries {{scratch}};
                        if (status_t const room = growth_status(entries.reserve(bucket.size())); room.failed())
                            return room;
                        for (std::size_t entry = 0; entry != bucket.size(); ++entry) {
                            bucket_entry_t emptied_entry {bucket.text_of(entry), value_view_t::make_empty()};
                            [[maybe_unused]] st::status_t const kept = entries.push_back(st::assume_reserved,
                                                                                         std::move(emptied_entry));
                        }
                        std::span<bucket_entry_t const> const listed {entries};
                        auto [bytes, allocate_status] = arena_allocate<byte_t>(scratch, bucket_bytes(listed));
                        if (allocate_status.failed()) return allocate_status;
                        write_bucket(reinterpret_cast<ustore_byte_t*>(bytes.data()), listed);
                        emptied = value_view_t {bytes};
                    }
                    if (status_t const written = write_value(access, collection, keys[index], emptied);
                        written.failed())
                        return written;
                }
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
template <typename listed_type_>
status_t list_catalog(database_state_t& state, ustore_arena_t scratch, listed_type_& listed,
                      transaction_stamp_t stamp = head_stamp_k) noexcept {
    access_t const head {state, nullptr, watch_schedule_t::never_k, scratch, stamp};
    st::basic_vector<stored_key_t, arena_allocator<stored_key_t>> keys {{scratch}};
    if (status_t const collected = collect_keys(head, catalog_collection_k, limit<stored_key_t>::min(),
                                                std::numeric_limits<std::size_t>::max(), keys);
        collected.failed())
        return collected;
    for (std::size_t index = 0; index != keys.size(); ++index) {
        auto [held, read_status] = read_value(head, catalog_collection_k, keys[index]);
        if (read_status.failed()) return read_status;
        if (!held) continue;
        auto [entry, entry_status] = read_catalog_entry(held);
        if (entry_status.failed()) return entry_status;
        ustore_collection_id_t id;
        keys[index].to_bytes(id.uuid.bytes);
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
inline status_t drop_collection(database_state_t& state, ustore_arena_t scratch,
                                ustore_collection_id_t const& collection) noexcept {
    {
        catalog_guard_t const held {state};
        access_t const head {state, nullptr, watch_schedule_t::never_k, scratch};
        stored_key_t const listed = stored_of(collection.uuid);
        auto [listing, read_status] = read_value(head, catalog_collection_k, listed);
        if (read_status.failed()) return read_status;
        if (!listing) return {status_code_t::not_found_k, "No collection has such identifier"};
        auto [entry, entry_status] = read_catalog_entry(listing);
        if (entry_status.failed()) return entry_status;
        if (!entry.name.empty())
            if (status_t const unnamed = stage_name(head, entry.name, nullptr); unnamed.failed()) return unnamed;
        if (status_t const unlisted = write_value(head, catalog_collection_k, listed, {}); unlisted.failed())
            return unlisted;
        // A snapshot taken before the unlisting still reads the runs, which wait for the last snapshot to go
        if (fu::preferred_atomic_ref<std::size_t> {state.open_snapshots}.load(std::memory_order_acquire) != 0 &&
            state.retired_collections.push_back(ustore_collection_id_t {collection}) == st::status_t::success_k)
            return {};
    }
    return state.store->drop_collection(collection);
}

/** Drops every collection a drop left for the snapshots, which the last snapshot freed no longer reads. */
inline void drop_retired(database_state_t& state) noexcept {
    catalog_guard_t const held {state};
    for (ustore_collection_id_t const& collection : state.retired_collections) [[maybe_unused]]
        status_t const dropped = state.store->drop_collection(collection);
    state.retired_collections.clear();
}

#pragma endregion Clearing

#pragma region Database Calls

/** Implements @c ustore_database_describe with the store's estimates, which count every version of a key that the
 *  runs still hold. */
status_t database_describe(ustore_database_describe_t const& arguments, view_t const&) noexcept {
    if (!arguments.description) return {status_code_t::invalid_argument_k, "Need an output for the description"};
    database_state_t& state = state_of(arguments);
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
status_t database_clear(ustore_database_clear_t const& arguments, view_t const& view) noexcept {
    ustore_drop_mode_t const mode = arguments.mode;
    if (mode != ustore_drop_collection_k && mode != ustore_drop_pairs_k && mode != ustore_drop_values_k)
        return {status_code_t::invalid_argument_k, "Unknown drop mode"};
    database_state_t& state = state_of(arguments);
    st::basic_vector<listed_collection_t, arena_allocator<listed_collection_t>> listed {{view.scratch}};
    if (status_t const collected = list_catalog(state, view.scratch, listed); collected.failed()) return collected;
    for (std::size_t index = 0; index != listed.size(); ++index) {
        status_t const cleared = mode == ustore_drop_collection_k
                                     ? drop_collection(state, view.scratch, listed[index].id)
                                     : empty_collection(state, view.scratch, listed[index].id,
                                                        keying_of(listed[index].entry.modality), mode);
        if (cleared.failed()) return cleared;
    }
    ustore_drop_mode_t const main_mode = mode == ustore_drop_collection_k ? ustore_drop_pairs_k : mode;
    if (status_t const cleared = empty_collection(state, view.scratch, ustore_collection_main_k, keying_t::uuids_k,
                                                  main_mode);
        cleared.failed())
        return cleared;
    if (status_t const flushed = state.store->flush(); flushed.failed()) return flushed;
    return compact_due(state);
}

/** Implements @c ustore_database_compact: flushing every collection, then merging every level over its bounds,
 *  reporting one unit per pass, as a pass merges every collection at once. */
status_t database_compact(ustore_database_compact_t const& arguments, view_t const&) noexcept {
    database_state_t& state = state_of(arguments);
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
status_t collection_list(ustore_collection_list_t const& arguments, view_t const& view) noexcept {
    if (!arguments.written) return {status_code_t::invalid_argument_k, "Need an output for the collections written"};
    database_state_t& state = state_of(arguments);
    st::basic_vector<listed_collection_t, arena_allocator<listed_collection_t>> listed {{view.scratch}};
    transaction_stamp_t const stamp = view.snapshot ? handle_cast<snapshot_handle_t>(view.snapshot)->pin.stamp()
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
status_t collection_create(ustore_collection_create_t const& arguments, view_t const& view) noexcept {
    if (!arguments.collection) return {status_code_t::invalid_argument_k, "Need an output for the collection"};
    if (arguments.modality > ustore_modality_pages_k) return {status_code_t::invalid_argument_k, "Unknown modality"};
    std::string_view const name = string_of(arguments.name);
    if (arguments.name.data && name.empty())
        return {status_code_t::invalid_argument_k, "A name is never empty, as the main collection has none"};
    database_state_t& state = state_of(arguments);
    auto [created, mint_status] = mint_collection_id();
    if (mint_status.failed()) return mint_status;

    // The engine's own name of a collection is its UUID's text, which never leaves the engine
    char text[uuid_t::printed_length_k] = {};
    uuid_t {std::span<std::uint8_t const, uuid_t::size_bytes_k> {created.uuid.bytes}}.print(text);
    if (status_t const made = state.store->create_collection(created, string_view_t {text, sizeof(text)},
                                                             state.collection_shape);
        made.failed())
        return made;

    // The catalog changes under its own claim, outside the registry, and lists the collection before it names it
    status_t listed;
    {
        catalog_guard_t const held {state};
        access_t const head {state, nullptr, watch_schedule_t::never_k, view.scratch};
        bool taken = false;
        if (!name.empty()) listed = look_up_name(head, name, taken).status();
        if (listed.succeeded() && taken)
            listed = {status_code_t::already_exists_k, "A collection already carries the name"};
        auto [entry, allocate_status] = arena_allocate<byte_t>(view.scratch, 1 + name.size());
        if (listed.succeeded()) listed = allocate_status;
        if (listed.succeeded()) {
            entry[0] = static_cast<byte_t>(arguments.modality);
            if (!name.empty()) std::memcpy(entry.data() + 1, name.data(), name.size());
            listed = write_value(head, catalog_collection_k, stored_of(created.uuid), value_view_t {entry});
        }
        name_record_t const record {created, arguments.modality};
        if (listed.succeeded() && !name.empty()) {
            listed = stage_name(head, name, &record);
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
status_t collection_drop(ustore_collection_drop_t const& arguments, view_t const& view) noexcept {
    ustore_collection_id_t const collection = arguments.collection;
    ustore_drop_mode_t const mode = arguments.mode;
    if (mode != ustore_drop_collection_k && mode != ustore_drop_pairs_k && mode != ustore_drop_values_k)
        return {status_code_t::invalid_argument_k, "Unknown drop mode"};
    if (collection == ustore_collection_main_k && mode == ustore_drop_collection_k)
        return {status_code_t::invalid_argument_k, "The main collection can only be cleared"};
    if (reserved(collection)) return {status_code_t::not_found_k, "No collection has such identifier"};
    database_state_t& state = state_of(arguments);
    if (mode == ustore_drop_collection_k) {
        if (status_t const dropped = drop_collection(state, view.scratch, collection); dropped.failed()) return dropped;
        return state.store->flush();
    }
    auto [entry, look_up_status] = look_up(state, view.scratch, collection);
    if (look_up_status == status_code_t::not_found_k)
        return {status_code_t::not_found_k, "No collection has such identifier"};
    if (look_up_status.failed()) return look_up_status;
    if (status_t const emptied = empty_collection(state, view.scratch, collection, keying_of(entry.modality), mode);
        emptied.failed())
        return emptied;
    return compact_due(state);
}

/** Implements @c ustore_collection_find, reading the names at the snapshot it names or at the head. */
status_t collection_find(ustore_collection_find_t const& arguments, strings_arg_t const& names,
                         view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!names || !arguments.collections)
        return {status_code_t::invalid_argument_k, "Need the names and an output for the identifiers"};
    database_state_t& state = state_of(arguments);
    auto [outputs, outputs_status] = collection_find_outputs_t::make(arguments);
    if (outputs_status.failed()) return outputs_status;
    // Through a snapshot the names read as they stood at its stamp, and through anything else at the head
    access_t const named = view.snapshot ? access_of(state, view)
                                         : access_t {state, nullptr, watch_schedule_t::never_k, view.scratch};
    for (std::size_t index = 0; index != arguments.count; ++index) {
        bool present = false;
        auto [record, record_status] = look_up_name(named, string_of(names[index]), present);
        if (record_status.failed()) return record_status;
        outputs.assign(index, present ? record.id() : ustore_collection_main_k, record.modality(), present);
    }
    outputs.finish(arguments);
    return {};
}

#pragma endregion Collection Calls

#pragma region Blob Calls

/** Implements @c ustore_blobs_write, committing a batch outside a transaction atomically. */
status_t blobs_write(ustore_blobs_write_t const& arguments, strings_arg_t const& values, view_t const& view) noexcept {
    place_batch_t const places = places_of(arguments);
    if (status_t const status = validate_write(view.transaction, places, values, arguments.durability, view.watch);
        status.failed() || !arguments.count)
        return status;
    database_state_t& state = state_of(arguments);
    if (status_t const resolved = resolve_runs(state, view, places.collections_begin, places.size(), keying_t::uuids_k);
        resolved.failed())
        return resolved;
    std::size_t bytes = 0;
    for (std::size_t index = 0; index != places.size(); ++index) bytes += values[index].length;
    return serve_write(state, view, arguments.durability, places.size(), bytes, watch_schedule_t::never_k,
                       [&](access_t const& access) noexcept -> status_t {
                           for (std::size_t index = 0; index != places.size(); ++index) {
                               place_t const place = places[index];
                               status_t const written = write_value(access, place.collection, stored_of(place.key.uuid),
                                                                    value_view_t {values[index]});
                               if (written.failed()) return written;
                           }
                           return {};
                       });
}

/** Implements @c ustore_blobs_remove, committing a batch outside a transaction atomically. */
status_t blobs_remove(ustore_blobs_remove_t const& arguments, view_t const& view) noexcept {
    place_batch_t const places = places_of(arguments);
    if (status_t const status = validate_remove(view.transaction, places, arguments.durability, view.watch);
        status.failed() || !arguments.count)
        return status;
    database_state_t& state = state_of(arguments);
    if (status_t const resolved = resolve_runs(state, view, places.collections_begin, places.size(), keying_t::uuids_k);
        resolved.failed())
        return resolved;
    return serve_write(state, view, arguments.durability, places.size(), 1, watch_schedule_t::never_k,
                       [&](access_t const& access) noexcept -> status_t {
                           for (std::size_t index = 0; index != places.size(); ++index) {
                               place_t const place = places[index];
                               status_t const removed = write_value(access, place.collection, stored_of(place.key.uuid),
                                                                    value_view_t {});
                               if (removed.failed()) return removed;
                           }
                           return {};
                       });
}

/** Implements @c ustore_blobs_read, copying every value into the scratch once and into the arena at the end. */
status_t blobs_read(ustore_blobs_read_t const& arguments, view_t const& view) noexcept {
    if (!arguments.count) return {};
    place_batch_t const places = places_of(arguments);
    if (status_t const status = validate_read(places); status.failed()) return status;
    database_state_t& state = state_of(arguments);
    if (status_t const resolved = resolve_runs(state, view, places.collections_begin, places.size(), keying_t::uuids_k);
        resolved.failed())
        return resolved;
    auto [outputs, outputs_status] = read_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    access_t const access = access_of(state, view);
    for (std::size_t index = 0; index != places.size(); ++index) {
        place_t const place = places[index];
        auto [value, read_status] = read_value(access, place.collection, stored_of(place.key.uuid));
        if (read_status.failed()) return read_status;
        if (value) outputs.append(value);
        else outputs.append_missing();
    }
    return outputs.finish(arguments);
}

/** Implements @c ustore_blobs_scan as ordered walks of each collection, watching what a transaction hands out. */
status_t blobs_scan(ustore_blobs_scan_t const& arguments, view_t const& view) noexcept {
    if (status_t const status = validate_scan(arguments); status.failed() || !arguments.count) return status;
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    scan_batch_t const scans {collections, strided(arguments.start_keys), arguments.wanted, arguments.count};
    database_state_t& state = state_of(arguments);
    if (status_t const resolved = resolve_runs(state, view, collections, arguments.count, keying_t::uuids_k);
        resolved.failed())
        return resolved;
    auto [outputs, outputs_status] = scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    progress_t progress {arguments.progress, arguments.progress_state, arguments.count};
    access_t const access = access_of(state, view);
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
status_t blobs_sample(ustore_blobs_sample_t const& arguments, view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!arguments.offsets || !arguments.keys)
        return {status_code_t::invalid_argument_k, "Need outputs for the offsets and the keys"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    sample_batch_t const samples {collections, arguments.wanted, arguments.count};
    database_state_t& state = state_of(arguments);
    if (status_t const resolved = resolve_runs(state, view, collections, arguments.count, keying_t::uuids_k);
        resolved.failed())
        return resolved;
    auto [outputs, outputs_status] = scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    auto const now = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    sample_generator_t generator {now ^ reinterpret_cast<std::uintptr_t>(&outputs)};
    access_t const access = access_of(state, view);
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
status_t blobs_measure(ustore_blobs_measure_t const& arguments, view_t const& view) noexcept {
    if (!arguments.count) return {};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    strided_iterator<ustore_key_t const> const start_keys = strided(arguments.start_keys);
    strided_iterator<ustore_key_t const> const end_keys = strided(arguments.end_keys);
    database_state_t& state = state_of(arguments);
    if (status_t const resolved = resolve_runs(state, view, collections, arguments.count, keying_t::uuids_k);
        resolved.failed())
        return resolved;
    auto [outputs, outputs_status] = measure_outputs_t::make(arguments);
    if (outputs_status.failed()) return outputs_status;
    access_t const access = access_of(state, view);
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

/** Lays the tasks of a strings write or removal out in the scratch of @p view, a null @p values removing them. */
inline expected<std::span<string_task_t>> string_tasks(view_t const& view,
                                                       strided_iterator<ustore_collection_id_t const> collections,
                                                       std::size_t count, strings_arg_t const& texts,
                                                       strings_arg_t const* values) noexcept {
    auto [tasks, allocate_status] = arena_allocate<string_task_t>(view.scratch, count);
    if (allocate_status.failed()) return allocate_status;
    for (std::size_t index = 0; index != count; ++index) {
        std::string_view const text = string_of(texts[index]);
        tasks[index] = string_task_t {task_collection(collections, index), bucket_key_of(text), text,
                                      values ? value_view_t {(*values)[index]} : value_view_t {}, index};
    }
    return tasks;
}

/** Writes or removes the strings of a batch: a read-modify-write of every bucket it touches, which watches the
 *  buckets whatever the call asks, so a concurrent rewrite of one conflicts. */
template <typename arguments_type_>
status_t serve_strings_write(arguments_type_ const& arguments, strings_arg_t const& texts, strings_arg_t const* values,
                             view_t const& view) noexcept {
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    database_state_t& state = state_of(arguments);
    if (status_t const resolved = resolve_runs(state, view, collections, arguments.count, keying_t::strings_k);
        resolved.failed())
        return resolved;
    auto [tasks, tasks_status] = string_tasks(view, collections, arguments.count, texts, values);
    if (tasks_status.failed()) return tasks_status;
    std::size_t bytes = 0;
    for (string_task_t const& task : tasks) bytes += task.text.size() + task.value.size() + 4 * sizeof(bucket_word_t);
    return serve_write(state, view, arguments.durability, arguments.count, 2 * bytes, watch_schedule_t::on_commit_k,
                       [&](access_t const& access) noexcept { return stage_strings(access, tasks); });
}

/** Implements @c ustore_strings_write, committing a batch outside a transaction atomically. */
status_t strings_write(ustore_strings_write_t const& arguments, strings_arg_t const& texts, strings_arg_t const& values,
                       view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!texts || !values) return {status_code_t::invalid_argument_k, "Need the strings and their values"};
    return serve_strings_write(arguments, texts, &values, view);
}

/** Implements @c ustore_strings_remove, committing a batch outside a transaction atomically. */
status_t strings_remove(ustore_strings_remove_t const& arguments, strings_arg_t const& texts,
                        view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!texts) return {status_code_t::invalid_argument_k, "No strings were provided"};
    return serve_strings_write(arguments, texts, nullptr, view);
}

/** Implements @c ustore_strings_read, reading the bucket of every string. */
status_t strings_read(ustore_strings_read_t const& arguments, strings_arg_t const& texts, view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!texts) return {status_code_t::invalid_argument_k, "No strings were provided"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    database_state_t& state = state_of(arguments);
    if (status_t const resolved = resolve_runs(state, view, collections, arguments.count, keying_t::strings_k);
        resolved.failed())
        return resolved;
    auto [outputs, outputs_status] = read_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    access_t const access = access_of(state, view);
    for (std::size_t index = 0; index != arguments.count; ++index) {
        auto [value, read_status] = read_string(access, task_collection(collections, index), string_of(texts[index]));
        if (read_status.failed()) return read_status;
        if (value) outputs.append(value);
        else outputs.append_missing();
    }
    return outputs.finish(arguments);
}

/** Implements @c ustore_strings_scan as ordered walks of each collection's buckets. */
status_t strings_scan(ustore_strings_scan_t const& arguments, strings_arg_t const& prefixes,
                      strings_arg_t const& starts, view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!arguments.offsets || !arguments.strings)
        return {status_code_t::invalid_argument_k, "Need outputs for the offsets and the strings"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    database_state_t& state = state_of(arguments);
    if (status_t const resolved = resolve_runs(state, view, collections, arguments.count, keying_t::strings_k);
        resolved.failed())
        return resolved;
    auto [outputs, outputs_status] = strings_scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    progress_t progress {arguments.progress, arguments.progress_state, arguments.count};
    access_t const access = access_of(state, view);
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
status_t transaction_init(ustore_transaction_init_t const& arguments, view_t const& view) noexcept {
    if (status_t const status = validate_transaction_begin(arguments.transaction); status.failed()) return status;
    if (view.transaction) {
        transaction_handle_t& handle = *handle_cast<transaction_handle_t>(view.transaction);
        handle.held->rollback();
        handle.staged = false;
        return {};
    }
    database_state_t& state = state_of(arguments);
    auto [opened, begin_status] = begin_transaction(state, state.transaction_shape);
    if (begin_status.failed()) return begin_status;
    auto* const handle = new (std::nothrow) transaction_handle_t(state, std::move(opened));
    if (!handle) return {status_code_t::out_of_memory_k, "Failed to allocate the transaction"};
    *arguments.transaction = c_handle<ustore_transaction_t>(handle);
    return {};
}

/** Implements @c ustore_transaction_stage: checking every watch and that every write fits, then handing the
 *  ledger's leader word back, so the commit checks them again. */
status_t transaction_stage(ustore_transaction_stage_t const& arguments, view_t const& view) noexcept {
    if (!view.transaction) return {status_code_t::uninitialized_k, "Transaction is uninitialized"};
    transaction_handle_t& handle = *handle_cast<transaction_handle_t>(view.transaction);
    if (!handle.staged) {
        if (status_t const staged = handle.held->stage(); staged.failed()) return staged;
        handle.held->unstage();
        handle.staged = true;
    }
    if (arguments.read_stamp) *arguments.read_stamp = handle.held->start_stamp();
    return {};
}

/** Implements @c ustore_transaction_commit, staging first, flushing when asked and compacting what it left due. */
status_t transaction_commit(ustore_transaction_commit_t const& arguments, view_t const& view) noexcept {
    if (status_t const status = validate_transaction_commit(view.transaction, arguments.durability); status.failed())
        return status;
    transaction_handle_t& handle = *handle_cast<transaction_handle_t>(view.transaction);
    if (status_t const committed = handle.held->commit(); committed.failed()) return committed;
    handle.staged = false;
    if (arguments.commit_stamp) *arguments.commit_stamp = handle.held->last_commit_stamp();
    return settle(handle.state, arguments.durability);
}

/** Implements @c ustore_transaction_rollback, withdrawing a stage and keeping the writes, which a transaction never
 *  staged has none of to withdraw. */
status_t transaction_rollback(ustore_transaction_rollback_t const&, view_t const& view) noexcept {
    if (!view.transaction) return {status_code_t::uninitialized_k, "Transaction is uninitialized"};
    handle_cast<transaction_handle_t>(view.transaction)->staged = false;
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
    bool const fits =
        take_unless_zero(resources.memory_limit, arguments.memory_limit) &&
        take_unless_zero(resources.shared_io_channels, moving.shared_io_channels) &&
        take_unless_zero(resources.channels.depth, moving.queue_depth) &&
        take_unless_zero(resources.channels.request_buffer_bytes, moving.request_buffer_bytes) &&
        take_unless_zero(resources.channels.request_buffers_count, moving.request_buffers_count) &&
        take_unless_zero(resources.channels.stream_buffer_bytes, moving.stream_buffer_bytes) &&
        take_unless_zero(resources.channels.stream_buffers_count, moving.stream_buffers_count) &&
        take_unless_zero(transactions.default_transaction.write_buffer_max_bytes, arguments.transaction_max_bytes) &&
        take_unless_zero(transactions.default_transaction.write_buffer_max_elements,
                         arguments.transaction_max_elements) &&
        take_unless_zero(collections.concurrency_limit, arguments.collection_concurrency_limit) &&
        take_unless_zero(collections.cache_limit, arguments.collection_cache_limit) &&
        take_unless_zero(collections.write_buffer_max_bytes, arguments.collection_write_buffer_max_bytes) &&
        take_unless_zero(collections.write_buffer_max_elements, arguments.collection_write_buffer_max_elements) &&
        take_unless_zero(collections.first_level_max_bytes, arguments.collection_first_level_max_bytes) &&
        take_unless_zero(collections.level_enlarge_factor, arguments.collection_level_enlarge_factor) &&
        take_unless_zero(collections.value_size, arguments.collection_value_size) &&
        take_unless_zero(collections.value_max_size, arguments.collection_value_max_size);
    if (!fits) return {status_code_t::invalid_argument_k, "An option of the engine exceeds its range"};
    auto ignore_issue = [](issue_kind_t, std::string_view) noexcept {};
    if (!validate(resources, transactions, ignore_issue) || !validate(resources, collections, ignore_issue))
        return {status_code_t::invalid_argument_k, "The configuration of the engine is invalid"};
    return {};
}

/** Creates the collection @p collection named @p name unless the store holds it already. */
inline status_t reserve_collection(database_state_t& state, collection_id_t const& collection,
                                   std::string_view name) noexcept {
    if (state.store->has_collection(collection)) return {};
    return state.store->create_collection(collection, string_view_t {name}, state.collection_shape);
}

/** Drops every collection of the store the catalog does not list, which a crash between a create's two steps
 *  leaves behind. */
inline status_t drop_unlisted(database_state_t& state, ustore_arena_t scratch) noexcept {
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
inline status_t mount_store(database_state_t& state, database_config_t const& transactions) noexcept {
    return state.resources->storage.visit([&](auto& mount) noexcept -> status_t {
        using mount_t = std::remove_reference_t<decltype(mount)>;
        if constexpr (std::is_same_v<mount_t, storage_mount_t>)
            return state.store->mount(mount.router(), mount.channels(), transactions);
        else return {status_code_t::unsupported_k, "The engine mounts its volumes on files alone"};
    });
}

/** Opens the store @p resources place, every collection of it shaped as @p shape, reserving the main collection, the
 *  catalog and the names on the first open. */
inline expected<std::unique_ptr<database_state_t>> open_state(resources_config_t const& resources,
                                                              database_config_t const& transactions,
                                                              collection_config_t const& shape,
                                                              ustore_arena_t scratch) noexcept {
    std::unique_ptr<database_state_t> state {new (std::nothrow) database_state_t};
    if (!state) return status_t {status_code_t::out_of_memory_k, "Failed to allocate the database"};
    state->collection_shape = shape;
    state->transaction_shape = transactions.default_transaction;
    auto [opened, open_status] = open_resources(resources, uuid_t {});
    if (open_status.failed()) return open_status;
    state->resources = std::move(opened);
    state->store.reset(new (std::nothrow) store_t {state->resources});
    if (!state->store) return status_t {status_code_t::out_of_memory_k, "Failed to allocate the store"};
    if (status_t const mounted = mount_store(*state, transactions); mounted.failed()) return mounted;

    if (status_t const reserved = reserve_collection(*state, ustore_collection_main_k, "main"); reserved.failed())
        return reserved;
    if (status_t const reserved = reserve_collection(*state, catalog_collection_k, "catalog"); reserved.failed())
        return reserved;
    if (status_t const reserved = reserve_collection(*state, names_collection_k, "names"); reserved.failed())
        return reserved;
    if (status_t const dropped = drop_unlisted(*state, scratch); dropped.failed()) return dropped;
    return state;
}

/** Implements @c ustore_database_open_nvme, reserving the main collection, the catalog and the names on the first
 *  open. */
status_t open(ustore_database_open_nvme_t const& arguments, view_t const& view) noexcept {
    if (!arguments.database) return {status_code_t::invalid_argument_k, "Need an output for the database"};
    *arguments.database = nullptr;
    if (!arguments.directory.length) return {status_code_t::invalid_argument_k, "A database needs a directory"};

    resources_config_t resources;
    database_config_t transactions;
    transactions.default_transaction = {engine_transaction_bytes_k, engine_transaction_elements_k};
    user_collection_config_t collections = engine_collection_defaults();
    if (status_t const read = read_directories(arguments, resources); read.failed()) return read;
    auto [exists, exists_status] = directory_exists(resources.directory.c_str());
    if (exists_status.failed()) return exists_status;
    if (!exists) return {status_code_t::not_found_k, "The directory doesn't exist"};
    if (status_t const read = read_options(arguments, resources, transactions, collections); read.failed()) return read;
    auto [state, open_status] = open_state(resources, transactions, create_collection_config(collections),
                                           view.scratch);
    if (open_status.failed()) return open_status;
    state->executor = nvme_placement_of(arguments.executor).executor;
    *arguments.database = c_handle<ustore_database_t>(state.release());
    return {};
}

#pragma endregion Lifetime

#pragma region Snapshot Calls

/** Implements @c ustore_snapshot_init, pinning the newest published stamp in the store's ledger. */
status_t snapshot_init(ustore_snapshot_init_t const& arguments, view_t const&) noexcept {
    if (!arguments.snapshot) return {status_code_t::invalid_argument_k, "Need an output for the snapshot"};
    database_state_t& state = state_of(arguments);
    auto* const handle = new (std::nothrow) snapshot_handle_t(state);
    if (!handle) return {status_code_t::out_of_memory_k, "Failed to allocate the snapshot"};
    transaction_stamp_t const stamp = state.store->ledger().pin_reader(handle->pin);
    fu::preferred_atomic_ref<std::size_t> {state.open_snapshots}.fetch_add(1, std::memory_order_release);
    if (arguments.read_stamp) *arguments.read_stamp = static_cast<ustore_stamp_t>(stamp);
    *arguments.snapshot = c_handle<ustore_snapshot_t>(handle);
    return {};
}

/** Copies every pair of @p collection that @p access reads into @p target, under the same identifier and bounds. */
inline status_t save_collection(access_t const& access, collection_ids_t const& collection, database_state_t& target,
                                progress_t& progress) noexcept {
    store_t& store = *access.state.store;
    if (!target.store->has_collection(collection.id)) {
        collection_config_t config;
        if (status_t const shaped = store.collection_config(collection.id, config); shaped.failed()) return shaped;
        if (status_t const made = target.store->create_collection(collection.id, collection.name.view(), config);
            made.failed())
            return made;
    }

    // One window of keys is one write buffer of the target, which it lands without a flush in between
    collection_config_t config;
    if (status_t const shaped = target.store->collection_config(collection.id, config); shaped.failed()) return shaped;
    std::size_t const window = config.write_buffer_max_elements;
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
status_t snapshot_save(ustore_snapshot_save_t const& arguments, view_t const& view) noexcept {
    if (!view.snapshot) return {status_code_t::invalid_argument_k, "A save reads through a snapshot"};
    database_state_t& source = state_of(arguments);
    access_t const access = access_of(source, view);

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
        for (collection_ids_t const& collection : held)
            if (status_t const measured = walk(access, collection.id, limit<stored_key_t>::min(),
                                               [&](stored_key_t, stored_value_size_t size) noexcept {
                                                   total += sizeof(stored_key_t) + size;
                                                   return true;
                                               });
                measured.failed())
                return measured;
    progress_t progress {arguments.progress, arguments.progress_state, total};

    status_t saved;
    {
        auto [target, open_status] = open_state(resources, transactions, source.collection_shape, view.scratch);
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
void snapshot_free(ustore_snapshot_t snapshot) noexcept {
    auto* const handle = handle_cast<snapshot_handle_t>(snapshot);
    database_state_t& state = handle->state;
    delete handle;
    if (fu::preferred_atomic_ref<std::size_t> {state.open_snapshots}.fetch_sub(1, std::memory_order_acq_rel) == 1)
        drop_retired(state);
}

#pragma endregion Snapshot Calls

#pragma region Lifetime

/** Frees @p transaction, retiring it from the registry. */
void transaction_free(ustore_transaction_t transaction) noexcept {
    delete handle_cast<transaction_handle_t>(transaction);
}

/** Flushes and unmounts @p database ahead of freeing it, which every transaction of it must precede. */
void database_free(ustore_database_t database) noexcept { delete handle_cast<database_state_t>(database); }

#pragma endregion Lifetime

#pragma region Graph Store

// The graph store's serves stay out of the table below, and build here over the store's own access
template status_t serve_graph(graph::graph_store_t::access_t, io_channel_t&, ustore_graph_write_vertices_t const&,
                              ustore_arena_t) noexcept;
template status_t serve_graph(graph::graph_store_t::access_t, io_channel_t&, ustore_graph_remove_vertices_t const&,
                              ustore_arena_t) noexcept;
template status_t serve_graph(graph::graph_store_t::access_t, io_channel_t&, ustore_graph_write_edges_t const&,
                              ustore_arena_t) noexcept;
template status_t serve_graph(graph::graph_store_t::access_t, io_channel_t&, ustore_graph_remove_edges_t const&,
                              ustore_arena_t) noexcept;
template status_t serve_graph(graph::graph_store_t::access_t, io_channel_t&, ustore_graph_read_edges_t const&,
                              ustore_arena_t) noexcept;

#pragma endregion Graph Store

engine_t const engine_k {
    .database_describe = &database_describe,
    .database_clear = &database_clear,
    .database_compact = &database_compact,
    .snapshot_init = &snapshot_init,
    .snapshot_save = &snapshot_save,
    .collection_list = &collection_list,
    .collection_create = &collection_create,
    .collection_find = &collection_find,
    .collection_drop = &collection_drop,
    .transaction_init = &transaction_init,
    .transaction_stage = &transaction_stage,
    .transaction_commit = &transaction_commit,
    .transaction_rollback = &transaction_rollback,
    .blobs_write = &blobs_write,
    .blobs_remove = &blobs_remove,
    .blobs_read = &blobs_read,
    .blobs_scan = &blobs_scan,
    .blobs_sample = &blobs_sample,
    .blobs_measure = &blobs_measure,
    .docs_write = &docs_write_in_json_values,
    .docs_read = &docs_read_in_json_values,
    .docs_gist = &docs_gist_in_json_values,
    .docs_gather = &docs_gather_in_json_values,
    .graph_read_edges = &graph_read_edges_in_vertex_values,
    .graph_write_edges = &graph_write_edges_in_vertex_values,
    .graph_remove_edges = &graph_remove_edges_in_vertex_values,
    .graph_write_vertices = &graph_write_vertices_in_vertex_values,
    .graph_remove_vertices = &graph_remove_vertices_in_vertex_values,
    .strings_write = &strings_write,
    .strings_remove = &strings_remove,
    .strings_read = &strings_read,
    .strings_scan = &strings_scan,
    .transaction_free = &transaction_free,
    .snapshot_free = &snapshot_free,
    .database_free = &database_free,
};

} // namespace unum::ustore::nvme

using namespace unum::ustore::nvme;

#pragma region C ABI

void ustore_database_open_nvme(ustore_database_open_nvme_t* arguments) { route_init(arguments, &open); }

#pragma endregion C ABI
