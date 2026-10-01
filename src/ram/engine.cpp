/**
 *  @file src/ram/engine.cpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief Embedded in-memory engine on SmashTable, strictly serializable or snapshot-isolated as configured at open.
 *
 *  Every collection keeps a partitioned multi-version store of its own, of the kind its modality picks: key UUIDs
 *  mapped onto values for blobs, documents and anything else, strings mapped onto values for strings, and the
 *  half-edges of a graph. No stored key carries its collection. A catalog store maps every collection's UUID onto its
 *  modality, its name and its store, and a names store maps every name onto its collection's UUID. Every store is
 *  built into one commit order, so a transaction spanning any number of them publishes under one stamp and a snapshot
 *  reads all of them at it. Dropping a collection retires its store at the drop's stamp, and the store is freed once
 *  no reader holds an older one. A database with a directory persists as one binary image, rewritten whole on a flush
 *  and on close.
 */

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint64_t`
#include <cstdio>  // `std::FILE`, `std::fopen`, `std::fread`, `std::fwrite`, `std::rename`
#include <cstring> // `std::memcmp`, `std::memcpy`

#if defined(_WIN32)
#include <io.h>      // `_commit`, `_fileno`
#include <windows.h> // `MoveFileExA`
#else
#include <unistd.h> // `fsync`
#endif

#include <algorithm>   // `std::max`, `std::min`
#include <array>       // `std::array`
#include <chrono>      // `std::chrono::steady_clock`
#include <limits>      // `std::numeric_limits`
#include <memory>      // `std::unique_ptr`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::is_same_v`, `std::remove_cvref_t`
#include <utility>     // `std::exchange`, `std::forward`, `std::move`
#include <variant>     // `std::in_place_type`, `std::variant`, `std::visit`

#include <fmt/format.h> // `fmt::format_to_n`

#include <forkunion/atomics.hpp> // `fu::standard_atomic_ref`
#include <smashtable/basic_avl_tree.hpp>
#include <smashtable/basic_vector.hpp>
#include <smashtable/partitioned_store.hpp>
#include <smashtable/snapshot_store.hpp>
#include <stringzilla/stringzilla.hpp>

#include "ustore/blobs.h"
#include "ustore/db.h"
#include "ustore/graph.h"
#include "ustore/ranges_args.hpp" // `places_arg_t`, `contents_arg_t`, `scans_arg_t`, `strings_arg_t`, `find_edges_t`
#include "ustore/strings.h"

#include "engine.hpp"                 // `engine_t`, `handle_t`, `handle_cast`
#include "helpers/blocking_files.hpp" // `directory_exists`
#include "helpers/linked_array.hpp"   // `growing_tape_t`, `uninitialized_array`
#include "helpers/linked_memory.hpp"  // `linked_memory_lock_t`
#include "ram/key.hpp"                // `string_key_t`, `half_edge_t`

namespace unum::ustore::ram {

#pragma region Stores

/** The status every SmashTable call answers with. */
using store_status_t = st::status_t;

/** The isolation level a store instantiation promises. */
using isolation_t = st::isolation_t;

/** Whether a walk carries on after an element. */
using walk_control_t = st::walk_control_t;

/** Whether a transaction's writes sit in the store, invisible. */
using staging_t = st::staging_t;

/** A stamp of the commit order: what a snapshot reads at, and what a commit publishes under. */
using generation_t = st::generation_t;

/** A value filed under its key. */
using value_pair_t = st::mapping<ustore_key_t, blob_t>;

/** A value filed under its string. */
using string_pair_t = st::mapping<string_key_t, blob_t>;

/** The ID of a named collection filed under its name. */
using name_pair_t = st::mapping<string_key_t, ustore_collection_id_t>;

/** A multi-version store of @p entry_type_ ordered by @p order_type_ at @p isolation_, in partitions @p hash_type_
 *  picks, built into the order of its database. */
template <typename entry_type_, typename order_type_, typename hash_type_, isolation_t isolation_>
using ordered_store = st::partitioned_store<
    st::snapshot_store<st::basic_avl_tree<entry_type_, order_type_, st::default_allocator<entry_type_>>, isolation_>,
    hash_type_>;

/** A collection of blobs, documents, pages or anything: every key mapped onto the bytes of its value. */
template <isolation_t isolation_>
using values_by_key_store = ordered_store<value_pair_t, uuid_order_t, uuid_hash_t, isolation_>;

/** A collection of strings: every string mapped onto the bytes of its value. */
template <isolation_t isolation_>
using values_by_string_store = ordered_store<string_pair_t, string_key_order_t, string_key_hash_t, isolation_>;

/** A graph collection: every vertex once and every edge twice, under each of its vertices, with no values. */
template <isolation_t isolation_>
using half_edges_store = ordered_store<half_edge_t, half_edge_order_t, half_edge_hash_t, isolation_>;

/** The order every store of a database commits in, so one stamp spans a commit reaching any number of them. */
using order_t = values_by_key_store<isolation_t::snapshot_k>::order_t;

/** A claim on the stamp a call, a snapshot or a transaction reads at, which keeps every store it may reach alive. */
using claim_t = order_t::snapshot_claim_t;

template <isolation_t isolation_>
struct collection_store;

/** What the catalog records of a collection: what it holds, its name, and the store its entries live in. */
template <isolation_t isolation_>
struct collection_entry {

    /** What the collection holds, which picked the kind of its store. */
    ustore_modality_t modality {ustore_modality_any_k};

    /** The name of the collection, empty for one created without. */
    blob_t name;

    /** The store holding the collection's entries. */
    collection_store<isolation_>* store {nullptr};
};

/** A collection's UUID mapped onto what the catalog records of it. */
template <isolation_t isolation_>
using catalog_pair = st::mapping<ustore_collection_id_t, collection_entry<isolation_>>;

/** Every collection, the main one included under the nil UUID: its UUID mapped onto its entry. */
template <isolation_t isolation_>
using collections_by_id_store = ordered_store<catalog_pair<isolation_>, uuid_order_t, uuid_hash_t, isolation_>;

/** Every named collection: its name mapped onto its UUID. */
template <isolation_t isolation_>
using collection_ids_by_name_store = ordered_store<name_pair_t, string_key_order_t, string_key_hash_t, isolation_>;

/** The store of one collection, of the kind its modality picks, which a drop retires rather than frees. */
template <isolation_t isolation_>
struct collection_store {

    /** The collection's entries. */
    std::variant<values_by_key_store<isolation_>, values_by_string_store<isolation_>, half_edges_store<isolation_>>
        entries;

    /** The store retired before this one, while this one waits to be freed. */
    collection_store* next_retired {nullptr};

    /** The stamp the change that took this store out of the catalog published under. */
    generation_t retirement_stamp {0};

    template <typename kind_type_>
    collection_store(std::in_place_type_t<kind_type_> kind, order_t& order) noexcept : entries(kind, order) {}
};

/** A transaction over the catalog, the names and every collection store it reaches, published under one stamp. */
template <isolation_t isolation_>
using collections_transaction =
    st::dynamic_transaction_group<collections_by_id_store<isolation_>, collection_ids_by_name_store<isolation_>,
                                  values_by_key_store<isolation_>, values_by_string_store<isolation_>,
                                  half_edges_store<isolation_>>;

static_assert(values_by_key_store<isolation_t::strict_serializable_k>::isolation_k ==
                  isolation_t::strict_serializable_k,
              "the partitions must keep the strict serializability of their stores");

/** How many times an engine-internal commit is attempted before its conflicts reach the caller. */
inline constexpr std::size_t commit_attempts_k = 16;

/** The error of the C ABI a caller reads for @p status. */
constexpr ustore_error_t translate(store_status_t status) noexcept {
    switch (status) {
    case store_status_t::success_k: return {};
    case store_status_t::unknown_k: return {ustore_error_internal_k, "SmashTable failed for an unknown reason"};
    case store_status_t::consistency_k: return {ustore_error_internal_k, "SmashTable found a staged version missing"};
    case store_status_t::write_conflict_k:
        return {ustore_error_write_conflict_k, "Write conflict: a written key was committed over"};
    case store_status_t::read_conflict_k:
        return {ustore_error_read_conflict_k, "Read conflict: a read key was committed over"};
    case store_status_t::phantom_conflict_k:
        return {ustore_error_phantom_conflict_k, "Phantom conflict: a walked range gained or lost a key"};
    case store_status_t::out_of_memory_heap_k: return {ustore_error_out_of_memory_k, "Out of memory"};
    case store_status_t::invalid_argument_k: return {ustore_error_invalid_argument_k, "Invalid argument"};
    case store_status_t::operation_not_permitted_k:
        return {ustore_error_invalid_argument_k, "Operation not permitted in the transaction's state"};
    case store_status_t::operation_would_block_k:
        return {ustore_error_write_conflict_k, "Gave up after every retry met a conflict"};
    case store_status_t::capacity_exhausted_k: return {ustore_error_capacity_exhausted_k, "Capacity exhausted"};
    case store_status_t::input_output_k: return {ustore_error_io_k, "Input or output failed"};
    case store_status_t::key_already_exists_k: return {ustore_error_already_exists_k, "Key already exists"};
    case store_status_t::key_not_found_k: return {ustore_error_not_found_k, "Key not found"};
    }
    return {ustore_error_internal_k, "SmashTable answered an unrecognized status"};
}

/** Reports a failed @p status through @p error, answering whether it failed. */
inline bool reported_failure(store_status_t status, ustore_error_t* error) noexcept {
    if (st::succeeded(status)) return false;
    *error = translate(status);
    return true;
}

/** Reports @p status through @p error, unless a refusal of the call itself already did. */
inline void report(store_status_t status, ustore_error_t* error) noexcept {
    if (!error->status) reported_failure(status, error);
}

/** Refuses a call with @p message, answering the status that stops the walk it is in. */
inline store_status_t refused(ustore_error_t* error, char const* message) noexcept {
    *error = {ustore_error_invalid_argument_k, message};
    return store_status_t::invalid_argument_k;
}

/** Whether @p path names an existing directory. */
inline bool names_directory(char const* path) noexcept {
    expected<bool> const exists = directory_exists(path);
    return exists && *exists;
}

/** Copies @p value into a blob the store owns. */
inline st::expected<blob_t> copy_blob(value_view_t value) noexcept {
    blob_t blob;
    if (st::failed(blob.resize(value.size()))) return store_status_t::out_of_memory_heap_k;
    if (value.size() != 0) std::memcpy(blob.data(), value.data(), value.size());
    return blob;
}

/** Views the bytes of a stored blob. */
inline value_view_t view_of(blob_t const& blob) noexcept {
    return value_view_t {blob.data(), static_cast<ustore_length_t>(blob.size())};
}

/** The key @p text is stored under, owning a copy of its bytes. */
inline st::expected<string_key_t> string_key_from(std::string_view text) noexcept {
    auto [copied, copy_blob_status] = copy_blob(value_view_t {text});
    if (st::failed(copy_blob_status)) return copy_blob_status;
    return string_key_t {std::move(copied)};
}

/** Allocates the store a collection holding @p modality keeps its entries in, or answers null. */
template <isolation_t isolation_>
collection_store<isolation_>* allocate_store(order_t& order, ustore_modality_t modality) noexcept {
    using store_t = collection_store<isolation_>;
    if (modality == ustore_modality_strings_k)
        return new (std::nothrow) store_t(std::in_place_type<values_by_string_store<isolation_>>, order);
    if (modality == ustore_modality_graph_k)
        return new (std::nothrow) store_t(std::in_place_type<half_edges_store<isolation_>>, order);
    return new (std::nothrow) store_t(std::in_place_type<values_by_key_store<isolation_>>, order);
}

#pragma endregion Stores

#pragma region Handles

/** Everything a database at @p isolation_ holds. */
template <isolation_t isolation_>
struct database_state {

    /** The order every store commits in. */
    order_t order;

    /** Every collection, the main one included under the nil UUID: its UUID mapped onto its entry. */
    collections_by_id_store<isolation_> collections_by_id {order};

    /** Every named collection: its name mapped onto its UUID. */
    collection_ids_by_name_store<isolation_> collection_ids_by_name {order};

    /** Stores the catalog no longer lists, linked through @c collection_store::next_retired and freed once no reader
     *  holds a stamp older than the one they were retired at. */
    collection_store<isolation_>* retired {nullptr};

    /** Snapshots handed out and not yet freed, which a reset refuses to run beside. */
    std::size_t open_snapshots {0};

    /** Directory the image persists in, empty for a volatile database. */
    sz::string_t directory;

    database_state() noexcept = default;
    database_state(database_state const&) = delete;
    database_state& operator=(database_state const&) = delete;
    ~database_state() noexcept;
};

/** The calls this engine serves, which stamp every handle it hands out. */
extern engine_t const engine_k;

/** A database handle of the C ABI, holding one isolation chosen at open. */
struct database_t : handle_t {

    /** The database at the isolation its configuration named. */
    std::variant<database_state<isolation_t::strict_serializable_k>, database_state<isolation_t::snapshot_k>> state;

    template <isolation_t isolation_>
    explicit database_t(std::in_place_type_t<database_state<isolation_>> alternative) noexcept : state(alternative) {}
};

/** Leads every transaction and snapshot handle, so a call tells a handle of another isolation apart before using it. */
struct isolated_handle_t : handle_t {

    /** The isolation of the database that opened the handle. */
    isolation_t isolation;
};

/** A transaction handle of the C ABI. */
template <isolation_t isolation_>
struct transaction_handle : isolated_handle_t {

    /** The database the transaction runs against. */
    database_state<isolation_>& state;

    /** The transaction over every store it reaches. */
    collections_transaction<isolation_> transaction;

    /** Whether the writes of the transaction are staged. */
    staging_t staging {staging_t::pending_k};

    transaction_handle(database_state<isolation_>& owner, collections_transaction<isolation_>&& opened) noexcept
        : isolated_handle_t {{&engine_k}, isolation_}, state(owner), transaction(std::move(opened)) {}
};

/** The transaction behind @p transaction, or null after reporting one of another isolation. */
template <isolation_t isolation_>
transaction_handle<isolation_>* transaction_of(ustore_transaction_t transaction, ustore_error_t* error) noexcept {
    auto* const header = handle_cast<isolated_handle_t>(transaction);
    if (header->isolation == isolation_) return static_cast<transaction_handle<isolation_>*>(header);
    *error = {ustore_error_invalid_argument_k, "The transaction belongs to a database of another isolation"};
    return nullptr;
}

/** A snapshot handle of the C ABI, whose claim pins the stamp every read through it answers at until it is freed. */
template <isolation_t isolation_>
struct snapshot_handle : isolated_handle_t {

    /** The database the snapshot reads. */
    database_state<isolation_>& state;

    /** The claim on the newest stamp published when the snapshot opened. */
    claim_t claim;

    explicit snapshot_handle(database_state<isolation_>& owner) noexcept
        : isolated_handle_t {{&engine_k}, isolation_}, state(owner) {}
};

/** The snapshot behind @p snapshot, or null after reporting one of another isolation. */
template <isolation_t isolation_>
snapshot_handle<isolation_>* snapshot_of(ustore_snapshot_t snapshot, ustore_error_t* error) noexcept {
    auto* const header = handle_cast<isolated_handle_t>(snapshot);
    if (header->isolation == isolation_) return static_cast<snapshot_handle<isolation_>*>(header);
    *error = {ustore_error_invalid_argument_k, "The snapshot belongs to a database of another isolation"};
    return nullptr;
}

#pragma endregion Handles

#pragma region Retirement

/** Retires @p store at @p stamp, which the catalog stopped listing it at, to be freed once no reader holds an older
 *  stamp. */
template <isolation_t isolation_>
void retire(database_state<isolation_>& state, collection_store<isolation_>* store, generation_t stamp) noexcept {
    store->retirement_stamp = stamp;
    fu::standard_atomic_ref<collection_store<isolation_>*> head(state.retired);
    store->next_retired = head.load(st::memory_order_relaxed_k);
    while (!head.compare_exchange_weak(store->next_retired, store, st::memory_order_release_k,
                                       st::memory_order_relaxed_k)) {}
}

/** Frees every retired store no reader can reach any more, and keeps the others retired. */
template <isolation_t isolation_>
void free_unreachable(database_state<isolation_>& state) noexcept {
    using store_t = collection_store<isolation_>;
    fu::standard_atomic_ref<store_t*> head(state.retired);
    if (!head.load(st::memory_order_relaxed_k)) return;
    store_t* pending = head.exchange(nullptr, st::memory_order_acquire_k);
    // Read after taking the list, so every store taken was retired at or below the stamps it is compared with
    generation_t const oldest_read = state.order.low_water_mark();
    store_t* kept = nullptr;
    store_t* kept_last = nullptr;
    while (pending) {
        store_t* const next = pending->next_retired;
        if (pending->retirement_stamp <= oldest_read) delete pending;
        else {
            pending->next_retired = kept;
            if (!kept) kept_last = pending;
            kept = pending;
        }
        pending = next;
    }
    if (!kept) return;
    kept_last->next_retired = head.load(st::memory_order_relaxed_k);
    while (!head.compare_exchange_weak(kept_last->next_retired, kept, st::memory_order_release_k,
                                       st::memory_order_relaxed_k)) {}
}

template <isolation_t isolation_>
database_state<isolation_>::~database_state() noexcept {
    [[maybe_unused]] store_status_t const walked = collections_by_id.for_each(
        [](catalog_pair<isolation_> const& pair) noexcept { delete pair.mapped.store; });
    while (retired) delete std::exchange(retired, retired->next_retired);
}

#pragma endregion Retirement

#pragma region Views

/** Reads a store at the stamp a claim held elsewhere pins, recording nothing. */
template <typename reader_type_>
struct reader_view {

    /** The reader of the store. */
    reader_type_ reader;

    /** Whether @c find and @c watch record what they hand out. */
    bool watches() const noexcept { return false; }

    template <typename key_type_, typename found_type_, typename missing_type_>
    store_status_t find(key_type_ const& key, found_type_&& found, missing_type_&& missing) const noexcept {
        return reader.find(key, found, missing);
    }

    template <typename key_type_, typename callback_type_>
    store_status_t range(key_type_ const& lower, key_type_ const& upper, callback_type_&& callback) const noexcept {
        return reader.range(lower, upper, callback);
    }

    template <typename key_type_, typename callback_type_>
    store_status_t range_from(key_type_ const& lower, callback_type_&& callback) const noexcept {
        return reader.range_from(lower, callback);
    }

    store_status_t prepare(std::size_t) const noexcept { return store_status_t::success_k; }

    template <typename key_type_>
    store_status_t watch(key_type_ const&) const noexcept {
        return store_status_t::success_k;
    }
};

/** Reads through the part a transaction joined a store with, watching what it hands out when a snapshot-isolated
 *  call asks, as a strictly serializable one records every read already. */
template <typename transaction_type_>
struct transaction_view {

    /** The part of the transaction over the store. */
    transaction_type_& transaction;

    /** Whether reads watch what they hand out. */
    bool watching;

    /** Whether @c find and @c watch record what they hand out. */
    bool watches() const noexcept { return watching; }

    template <typename key_type_, typename found_type_, typename missing_type_>
    store_status_t find(key_type_ const& key, found_type_&& found, missing_type_&& missing) const noexcept {
        return watching ? transaction.find_and_watch(key, found, missing) : transaction.find(key, found, missing);
    }

    template <typename key_type_, typename callback_type_>
    store_status_t range(key_type_ const& lower, key_type_ const& upper, callback_type_&& callback) const noexcept {
        return transaction.range(lower, upper, callback);
    }

    template <typename key_type_, typename callback_type_>
    store_status_t range_from(key_type_ const& lower, callback_type_&& callback) const noexcept {
        return transaction.range_from(lower, callback);
    }

    store_status_t prepare(std::size_t count) const noexcept { return transaction.reserve(count); }

    template <typename key_type_>
    store_status_t watch(key_type_ const& key) const noexcept {
        return watching ? transaction.watch(key) : store_status_t::success_k;
    }
};

/** The stores a call outside any transaction reads, each at the one stamp a claim of the call or of a snapshot
 *  pins. */
template <isolation_t isolation_>
struct reader_source {

    static constexpr isolation_t isolation_k = isolation_;

    /** The database being read. */
    database_state<isolation_>& state;

    /** The stamp every store is read at. */
    generation_t stamp;

    /** A reader of @p store at the stamp. */
    template <typename store_type_>
    st::expected<reader_view<typename store_type_::reader_t>> view(store_type_& store) const noexcept {
        return reader_view<typename store_type_::reader_t> {store.reader_at(stamp)};
    }

    /** A reader of the catalog at the stamp, which resolves the collections a call names. */
    reader_view<typename collections_by_id_store<isolation_>::reader_t> catalog() const noexcept {
        return {state.collections_by_id.reader_at(stamp)};
    }
};

/** The stores a transaction reaches, each through the part it joins the transaction with. */
template <isolation_t isolation_>
struct transaction_source {

    static constexpr isolation_t isolation_k = isolation_;

    /** The database the transaction runs against. */
    database_state<isolation_>& state;

    /** The transaction every store joins. */
    collections_transaction<isolation_>& transaction;

    /** Whether reads watch what they hand out. */
    bool watching;

    /** The part of the transaction over @p store, joining it at the transaction's snapshot unless it already has. */
    template <typename store_type_>
    st::expected<typename store_type_::transaction_t*> part(store_type_& store) noexcept {
        if (auto* const joined = transaction.participant(store)) return joined;
        return transaction.join(store);
    }

    /** A view of @p store through its part of the transaction. */
    template <typename store_type_>
    st::expected<transaction_view<typename store_type_::transaction_t>> view(store_type_& store) noexcept {
        auto joined = part(store);
        if (!joined) return joined.status();
        return transaction_view<typename store_type_::transaction_t> {**joined, watching};
    }

    /**
     *  @brief A reader of the catalog at the newest stamp, which resolves the collections a call names.
     *
     *  Collections resolve at the head, as in every engine, since the C ABI creates them only there and a transaction
     *  must reach one created after it began. The transaction's own claim is older than the stamp read, so it keeps
     *  every store the read may hand out alive.
     */
    reader_view<typename collections_by_id_store<isolation_>::reader_t> catalog() const noexcept {
        return {state.collections_by_id.reader_at(state.order.published_stamp())};
    }
};

/** Whether reads of a transaction at @p isolation_ called with @p options watch what they hand out. */
template <isolation_t isolation_>
constexpr bool watches_reads(ustore_options_t options) noexcept {
    return isolation_ == isolation_t::snapshot_k && !(options & ustore_option_transaction_dont_watch_k);
}

/** The view a read, scan, sample, measure or listing names. */
struct view_request_t {

    /** The transaction to read through, or null. */
    ustore_transaction_t transaction;

    /** The snapshot to read at, or null, which the C ABI never lets a call name beside a transaction. */
    ustore_snapshot_t snapshot;

    /** The options of the call, which decide whether a snapshot-isolated transaction watches. */
    ustore_options_t options;
};

/** Hands @p action the source @p request names: a snapshot, a transaction, or the newest stamp, claimed for the
 *  call. */
template <isolation_t isolation_, typename action_type_>
void with_source(database_state<isolation_>& state, view_request_t const& request, ustore_error_t* error,
                 action_type_&& action) noexcept {
    if (request.snapshot) {
        snapshot_handle<isolation_>* const handle = snapshot_of<isolation_>(request.snapshot, error);
        if (!handle) return;
        reader_source<isolation_> source {state, handle->claim.snapshot()};
        return action(source);
    }
    if (!request.transaction) {
        claim_t claim;
        reader_source<isolation_> source {state, state.order.take_snapshot(claim)};
        return action(source);
    }
    transaction_handle<isolation_>* const handle = transaction_of<isolation_>(request.transaction, error);
    if (!handle) return;
    transaction_source<isolation_> source {state, handle->transaction, watches_reads<isolation_>(request.options)};
    action(source);
}

#pragma endregion Views

#pragma region Resolution

/** A collection as a call resolved it: what it holds and the store its entries live in. */
template <isolation_t isolation_>
struct resolved_collection {
    ustore_modality_t modality {ustore_modality_any_k};
    collection_store<isolation_>* store {nullptr};
};

/** Reads what the catalog records of @p collection for @p source, answering @c key_not_found_k for a collection the
 *  catalog does not list. */
template <typename source_type_>
store_status_t resolve(source_type_& source, ustore_collection_id_t const& collection,
                       resolved_collection<source_type_::isolation_k>& resolved) noexcept {
    auto const catalog = source.catalog();
    store_status_t listing = store_status_t::key_not_found_k;
    store_status_t const looked_up = catalog.find(
        collection,
        [&](catalog_pair<source_type_::isolation_k> const& pair) noexcept {
            listing = store_status_t::success_k;
            resolved = {pair.mapped.modality, pair.mapped.store};
        },
        st::no_op_t {});
    return st::failed(looked_up) ? looked_up : listing;
}

/**
 *  @brief Hands @p action every run of tasks naming one collection, with the store of that collection.
 *
 *  Resolves each run once through @p source, refusing a collection the catalog does not list, and stops at the first
 *  failure @p action answers or reports. @p action is invoked as `action(entries, first, end)` with the concrete store
 *  and the tasks [ @p first, @p end ) it serves.
 */
template <typename source_type_, typename action_type_>
store_status_t for_each_run(source_type_& source, strided_iterator<ustore_collection_id_t const> collections,
                            std::size_t count, ustore_error_t* error, action_type_&& action) noexcept {
    for (std::size_t first = 0; first != count;) {
        ustore_collection_id_t const collection = collections ? collections[first] : ustore_collection_main_k;
        std::size_t end = first + 1;
        while (end != count && (!collections || collections[end] == collection)) ++end;
        resolved_collection<source_type_::isolation_k> resolved;
        store_status_t const listed = resolve(source, collection, resolved);
        if (listed == store_status_t::key_not_found_k) {
            *error = {ustore_error_not_found_k, "No such collection"};
            return listed;
        }
        if (st::failed(listed)) return listed;
        store_status_t const served = std::visit(
            [&](auto& entries) noexcept -> store_status_t { return action(entries, first, end); },
            resolved.store->entries);
        if (st::failed(served)) return served;
        if (error->status) return store_status_t::invalid_argument_k;
        first = end;
    }
    return store_status_t::success_k;
}

/** The entries a store of @p store_type_ holds. */
template <typename store_type_>
using entry_of = typename std::remove_cvref_t<store_type_>::value_t;

#pragma endregion Resolution

#pragma region Output Tapes

/** The columns of a batched read, every value copied into the caller's arena exactly once. */
class value_tape {
    linked_memory_lock_t& arena_;
    ustore_error_t* error_;
    range_or_dummy<bits_span_t> presences_;
    range_or_dummy<std::span<ustore_length_t>> offsets_;
    range_or_dummy<std::span<ustore_length_t>> lengths_;
    std::span<byte_t> contents_ {};
    ustore_byte_t** values_;
    std::size_t count_;
    std::size_t index_ {0};
    std::size_t used_ {0};

    /** Makes room for @p length more bytes, extending the tape in place while it ends its arena block. */
    bool reserve_(std::size_t length) noexcept {
        if (!values_ || used_ + length <= contents_.size()) return true;
        std::size_t const predicted = std::max(contents_.size() * 2, length * (count_ - index_));
        std::size_t const wanted = std::max(predicted, used_ + length);
        if (contents_.size() == 0) contents_ = arena_.alloc<byte_t>(wanted, error_);
        else contents_ = arena_.grow(contents_, wanted - contents_.size(), error_);
        return !error_->status;
    }

  public:
    template <typename arguments_type_>
    value_tape(linked_memory_lock_t& arena, arguments_type_& arguments) noexcept
        : arena_(arena), error_(arguments.error),
          presences_(arena.alloc_or_dummy(arguments.tasks_count, arguments.error, arguments.presences)),
          offsets_(arena.alloc_or_dummy(arguments.tasks_count + 1, arguments.error, arguments.offsets)),
          lengths_(arena.alloc_or_dummy(arguments.tasks_count, arguments.error, arguments.lengths)),
          values_(arguments.values), count_(arguments.tasks_count) {}

    /** Appends a found value. */
    void append(value_view_t value) noexcept {
        std::size_t const length = value.size();
        if (!reserve_(length)) return;
        if (values_ && length != 0) std::memcpy(contents_.data() + used_, value.data(), length);
        presences_[index_] = true;
        offsets_[index_] = static_cast<ustore_length_t>(used_);
        lengths_[index_] = static_cast<ustore_length_t>(length);
        used_ += length;
        ++index_;
    }

    /** Appends a missing value. */
    void append_missing() noexcept {
        presences_[index_] = false;
        offsets_[index_] = static_cast<ustore_length_t>(used_);
        lengths_[index_] = ustore_length_missing_k;
        ++index_;
    }

    /** Closes the offsets and exports the tape of values. */
    void finish() noexcept {
        offsets_[count_] = static_cast<ustore_length_t>(used_);
        if (values_) *values_ = reinterpret_cast<ustore_byte_t*>(contents_.data());
    }
};

/** The columns of batched scans and samples: offsets and counts per task over one tape of keys. */
class key_tape {
    ustore_error_t* error_;
    range_or_dummy<std::span<ustore_length_t>> offsets_;
    range_or_dummy<std::span<ustore_length_t>> counts_;
    uninitialized_array<ustore_key_t> keys_;
    std::size_t tasks_;

  public:
    key_tape(linked_memory_lock_t& arena, std::size_t tasks, ustore_length_t** offsets, ustore_length_t** counts,
             ustore_error_t* error) noexcept
        : error_(error), offsets_(arena.alloc_or_dummy(tasks + 1, error, offsets)),
          counts_(arena.alloc_or_dummy(tasks, error, counts)), keys_(arena), tasks_(tasks) {}

    /** Sizes the tape for @p keys keys, so a walk within it never moves the tape. */
    void reserve(std::size_t keys) noexcept { keys_.reserve(keys, error_); }

    /** How many keys the tape holds. */
    std::size_t size() const noexcept { return keys_.size(); }

    /** The key at @p index. */
    ustore_key_t& operator[](std::size_t index) noexcept { return keys_[index]; }

    /** Appends @p key, halting the walk when the arena refuses. */
    walk_control_t push(ustore_key_t key) noexcept {
        keys_.push_back(key, error_);
        return error_->status ? walk_control_t::halt_k : walk_control_t::resume_k;
    }

    /** Records where task @p task starts and how many keys it holds, from @p first on. */
    void close_task(std::size_t task, std::size_t first) noexcept {
        offsets_[task] = static_cast<ustore_length_t>(first);
        counts_[task] = static_cast<ustore_length_t>(keys_.size() - first);
    }

    /** Closes the offsets and exports the tape of keys. */
    void finish(ustore_key_t** keys) noexcept {
        offsets_[tasks_] = static_cast<ustore_length_t>(keys_.size());
        *keys = keys_.begin();
    }
};

#pragma endregion Output Tapes

#pragma region Reads

/** Where the entry @p key names sits among entries of @p entry_type_: the pair itself, or the vertex's own entry. */
template <typename entry_type_>
constexpr auto entry_for(ustore_key_t const& key) noexcept {
    if constexpr (std::is_same_v<entry_type_, half_edge_t>) return half_edge_t::vertex_entry(key);
    else return key;
}

/** Whether a walk of a collection's keys hands @p entry out: every pair, but only the vertex entries of a graph. */
constexpr bool is_listed(value_pair_t const&) noexcept { return true; }
constexpr bool is_listed(half_edge_t const& entry) noexcept {
    return entry.direction == half_edge_direction_t::vertex_k;
}

/** The key a listed entry names. */
constexpr ustore_key_t const& listed_key(value_pair_t const& pair) noexcept { return pair.key; }
constexpr ustore_key_t const& listed_key(half_edge_t const& entry) noexcept { return entry.vertex; }

/** The value an entry reads as: its bytes, or none for a vertex, which keeps no value. */
inline value_view_t value_of(value_pair_t const& pair) noexcept { return view_of(pair.mapped); }
inline value_view_t value_of(half_edge_t const&) noexcept { return {}; }

/** Reads tasks [ @p first, @p end ) of @p places through @p view of a collection of @p entry_type_ into @p tape. */
template <typename entry_type_, typename view_type_>
store_status_t read_keys(view_type_ const& view, places_arg_t const& places, std::size_t first, std::size_t end,
                         value_tape& tape, ustore_error_t* error) noexcept {
    store_status_t const reserved = view.prepare(end - first);
    if (st::failed(reserved)) return reserved;
    auto const found = [&](entry_type_ const& entry) noexcept { tape.append(value_of(entry)); };
    auto const missing = [&]() noexcept { tape.append_missing(); };
    for (std::size_t index = first; index != end && !error->status; ++index) {
        store_status_t const looked_up = view.find(entry_for<entry_type_>(places[index].key), found, missing);
        if (st::failed(looked_up)) return looked_up;
    }
    return store_status_t::success_k;
}

/** Watches the entries of the keys the tape holds from @p first on, which only a watching view records, once the
 *  walk that found them let go of its partitions. */
template <typename entry_type_, typename view_type_>
store_status_t watch_keys(view_type_ const& view, key_tape& tape, std::size_t first) noexcept {
    if (!view.watches()) return store_status_t::success_k;
    for (std::size_t index = first; index != tape.size(); ++index) {
        store_status_t const watched = view.watch(entry_for<entry_type_>(tape[index]));
        if (st::failed(watched)) return watched;
    }
    return store_status_t::success_k;
}

/** Appends up to the limit of @p scan keys of a collection of @p entry_type_, from its start key on. */
template <typename entry_type_, typename view_type_>
store_status_t scan_window(view_type_ const& view, scan_t scan, key_tape& tape) noexcept {
    std::size_t const first = tape.size();
    if (scan.limit == 0) return store_status_t::success_k;
    store_status_t const walked = view.range_from(
        entry_for<entry_type_>(scan.min_key), [&](entry_type_ const& entry) noexcept -> walk_control_t {
            if (!is_listed(entry)) return walk_control_t::resume_k;
            if (tape.push(listed_key(entry)) == walk_control_t::halt_k) return walk_control_t::halt_k;
            return tape.size() - first == scan.limit ? walk_control_t::halt_k : walk_control_t::resume_k;
        });
    if (st::failed(walked)) return walked;
    return watch_keys<entry_type_>(view, tape, first);
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

/** Appends a uniform reservoir of up to the limit of @p sample keys of a collection of @p entry_type_. */
template <typename entry_type_, typename view_type_>
store_status_t sample_window(view_type_ const& view, sample_arg_t sample, sample_generator_t& generator,
                             key_tape& tape) noexcept {
    std::size_t const first = tape.size();
    std::size_t seen = 0;
    store_status_t const walked = view.range_from(
        entry_for<entry_type_>(smallest_key_k), [&](entry_type_ const& entry) noexcept -> walk_control_t {
            if (!is_listed(entry)) return walk_control_t::resume_k;
            std::size_t const slot = seen < sample.limit ? seen : st::draw_below(generator, seen + 1);
            ++seen;
            if (slot >= sample.limit) return walk_control_t::resume_k;
            if (slot == tape.size() - first) return tape.push(listed_key(entry));
            tape[first + slot] = listed_key(entry);
            return walk_control_t::resume_k;
        });
    if (st::failed(walked)) return walked;
    return watch_keys<entry_type_>(view, tape, first);
}

/** The entries, value bytes and space one measured window holds. */
struct window_measure_t {

    /** Entries in the window. */
    std::size_t cardinality {0};

    /** Bytes of every value in the window. */
    std::size_t value_bytes {0};

    /** Bytes the entries take in the store, values included. */
    std::size_t space_bytes {0};
};

/** Measures the keys of a collection of @p entry_type_ in [ @p lower, @p last ]. */
template <typename entry_type_, typename view_type_>
store_status_t measure_window(view_type_ const& view, ustore_key_t const& lower, ustore_key_t const& last,
                              window_measure_t& measured) noexcept {
    return view.range_from(entry_for<entry_type_>(lower), [&](entry_type_ const& entry) noexcept -> walk_control_t {
        if (!is_listed(entry)) return walk_control_t::resume_k;
        if (last < listed_key(entry)) return walk_control_t::halt_k;
        std::size_t const value_bytes = value_of(entry).size();
        ++measured.cardinality;
        measured.value_bytes += value_bytes;
        measured.space_bytes += sizeof(entry_type_) + value_bytes;
        return walk_control_t::resume_k;
    });
}

/** Reads the value of every string of tasks [ @p first, @p end ) of @p texts through @p view into @p tape. */
template <typename view_type_>
store_status_t read_strings(view_type_ const& view, strings_arg_t const& texts, std::size_t first, std::size_t end,
                            value_tape& tape, ustore_error_t* error) noexcept {
    store_status_t const reserved = view.prepare(end - first);
    if (st::failed(reserved)) return reserved;
    auto const found = [&](string_pair_t const& pair) noexcept { tape.append(view_of(pair.mapped)); };
    auto const missing = [&]() noexcept { tape.append_missing(); };
    for (std::size_t index = first; index != end && !error->status; ++index) {
        auto [key, key_status] = string_key_from(texts[index]);
        if (st::failed(key_status)) return key_status;
        store_status_t const looked_up = view.find(key, found, missing);
        if (st::failed(looked_up)) return looked_up;
    }
    return store_status_t::success_k;
}

/** Appends up to @p limit strings starting with @p prefix, from @p start on, to @p found. */
template <typename view_type_>
store_status_t scan_strings(view_type_ const& view, std::string_view prefix, std::string_view start, std::size_t limit,
                            growing_tape_t& found, ustore_error_t* error) noexcept {
    if (limit == 0) return store_status_t::success_k;
    auto [first, first_status] = string_key_from(start > prefix ? start : prefix);
    if (st::failed(first_status)) return first_status;
    std::size_t const found_before = found.lengths().size();
    store_status_t const walked = view.range_from(first, [&](string_pair_t const& pair) noexcept -> walk_control_t {
        std::string_view const text = pair.key.view();
        if (!text.starts_with(prefix)) return walk_control_t::halt_k;
        found.push_back(value_view_t {text}, error);
        if (error->status) return walk_control_t::halt_k;
        return found.lengths().size() - found_before == limit ? walk_control_t::halt_k : walk_control_t::resume_k;
    });
    if (st::failed(walked) || !view.watches()) return walked;

    // A walk holds its partitions, so the strings it handed out are watched once it let go of them
    auto const offsets = found.offsets();
    auto const lengths = found.lengths();
    char const* const tape = reinterpret_cast<char const*>(found.contents().begin().get());
    for (std::size_t index = found_before; index != lengths.size() && !error->status; ++index) {
        std::string_view const text {tape + offsets[index], lengths[index]};
        auto [key, key_status] = string_key_from(text);
        store_status_t const watched = st::failed(key_status) ? key_status : view.watch(key);
        if (st::failed(watched)) return watched;
    }
    return store_status_t::success_k;
}

#pragma endregion Reads

#pragma region Writes

/** Stages @p content under @p key into @p transaction, which may be a store publishing it at once. */
template <typename transaction_type_, typename key_type_>
store_status_t stage_upsert(transaction_type_& transaction, key_type_&& key, value_view_t content) noexcept {
    auto [copied, copy_blob_status] = copy_blob(content);
    if (st::failed(copy_blob_status)) return copy_blob_status;
    using staged_pair_t = st::mapping<std::remove_cvref_t<key_type_>, blob_t>;
    return transaction.upsert(staged_pair_t {std::forward<key_type_>(key), std::move(copied)});
}

/** Stages erasing @p key, watching its absence when @p watch asks and the key is already gone. */
template <typename transaction_type_, typename key_type_>
store_status_t stage_erase(transaction_type_& transaction, key_type_ const& key, watch_t watch) noexcept {
    store_status_t const erased = transaction.erase(key);
    if (erased != store_status_t::key_not_found_k) return erased;
    if (watch == watch_t::dont_watch_k) return store_status_t::success_k;
    return transaction.watch(key);
}

/** Stages tasks [ @p first, @p end ) of a batch into @p transaction, where a missing value erases its key. */
template <typename transaction_type_>
store_status_t stage_writes(transaction_type_& transaction, places_arg_t const& places, contents_arg_t const& contents,
                            std::size_t first, std::size_t end, watch_t watch) noexcept {
    for (std::size_t index = first; index != end; ++index) {
        ustore_key_t const key = places[index].key;
        value_view_t const content = contents[index];
        store_status_t const staged = content ? stage_upsert(transaction, key, content)
                                              : stage_erase(transaction, key, watch);
        if (st::failed(staged)) return staged;
    }
    return store_status_t::success_k;
}

/** Stages tasks [ @p first, @p end ) of a batch of strings into @p transaction, where a missing value erases its
 *  string. */
template <typename transaction_type_>
store_status_t stage_strings(transaction_type_& transaction, strings_arg_t const& texts, contents_arg_t const& contents,
                             std::size_t first, std::size_t end, watch_t watch) noexcept {
    for (std::size_t index = first; index != end; ++index) {
        auto [key, key_status] = string_key_from(texts[index]);
        if (st::failed(key_status)) return key_status;
        value_view_t const content = contents[index];
        store_status_t const staged = content ? stage_upsert(transaction, std::move(key), content)
                                              : stage_erase(transaction, key, watch);
        if (st::failed(staged)) return staged;
    }
    return store_status_t::success_k;
}

/** Publishes one write into @p store under a stamp of its own, with no transaction to open. */
template <typename store_type_, typename key_type_>
store_status_t write_directly(store_type_& store, key_type_&& key, value_view_t content) noexcept {
    if (content) return stage_upsert(store, std::forward<key_type_>(key), content);
    store_status_t const erased = store.erase(key);
    return erased == store_status_t::key_not_found_k ? store_status_t::success_k : erased;
}

/**
 *  @brief Commits what @p stage_changes stages through a transaction of its own, retrying conflicts up to
 *      @c commit_attempts_k times.
 *  @param[out] stamp Optional output for the stamp the commit published under.
 *
 *  @p stage_changes takes a @c transaction_source and may refuse the call through its error, answering a failure that
 *  is not a conflict, so no attempt follows.
 */
template <isolation_t isolation_, typename stage_changes_type_>
store_status_t commit_group(database_state<isolation_>& state, stage_changes_type_&& stage_changes,
                            generation_t* stamp = nullptr) noexcept {
    auto opened = collections_transaction<isolation_>::make(state.order);
    if (!opened) return opened.status();
    transaction_source<isolation_> source {state, *opened, false};
    store_status_t const committed = st::commit_with_retries(
        *opened, commit_attempts_k,
        [&](collections_transaction<isolation_>&) noexcept -> store_status_t { return stage_changes(source); });
    if (stamp && st::succeeded(committed)) *stamp = opened->commit_stamp();
    return committed;
}

#pragma endregion Writes

#pragma region Catalog

/** The collections a listing exports, with what each holds and the name it carries. */
struct catalog_listing_t {

    /** The UUID of every collection. */
    uninitialized_array<ustore_collection_id_t> ids;

    /** What every collection holds. */
    uninitialized_array<ustore_modality_t> modalities;

    /** The name of every collection, empty for one without. */
    growing_tape_t names;
};

/** Appends the catalog entry @p pair to @p listing, skipping the main collection and halting the walk when the arena
 *  refuses. */
template <isolation_t isolation_>
walk_control_t list_entry(catalog_listing_t& listing, catalog_pair<isolation_> const& pair,
                          ustore_error_t* error) noexcept {
    if (pair.key == ustore_collection_main_k) return walk_control_t::resume_k;
    listing.ids.push_back(pair.key, error);
    listing.modalities.push_back(pair.mapped.modality, error);
    listing.names.push_back(view_of(pair.mapped.name), error);
    return error->status ? walk_control_t::halt_k : walk_control_t::resume_k;
}

/** One collection as the catalog lists it at some stamp. */
template <isolation_t isolation_>
struct listed_collection {
    ustore_collection_id_t id;
    ustore_modality_t modality;
    collection_store<isolation_>* store;
    blob_t name;
};

/** Every collection the catalog lists at some stamp. */
template <isolation_t isolation_>
using listed_collections = st::basic_vector<listed_collection<isolation_>>;

/** Collects every collection the catalog lists through @p view, the main one included. */
template <isolation_t isolation_, typename view_type_>
store_status_t collect_catalog(view_type_ const& view, listed_collections<isolation_>& listed) noexcept {
    store_status_t appended = store_status_t::success_k;
    store_status_t const walked = view.range_from(
        ustore_collection_id_t {}, [&](catalog_pair<isolation_> const& pair) noexcept -> walk_control_t {
            auto name = pair.mapped.name.copy();
            appended = name ? listed.push_back(listed_collection<isolation_> {pair.key, pair.mapped.modality,
                                                                              pair.mapped.store, std::move(*name)})
                            : name.status();
            return st::failed(appended) ? walk_control_t::halt_k : walk_control_t::resume_k;
        });
    return st::failed(walked) ? walked : appended;
}

/** The stores a catalog change installs and those it takes out, so its commit retires the latter and its failure
 *  frees the former. */
template <isolation_t isolation_>
struct catalog_change {

    /** Stores the change installs, which nothing else reaches until it commits. */
    st::basic_vector<collection_store<isolation_>*> installed;

    /** Stores the change takes out of the catalog, retired at its stamp. */
    st::basic_vector<collection_store<isolation_>*> taken_out;

    /** Frees what a refused attempt installed and forgets what it took out, ahead of the next attempt. */
    void discard() noexcept {
        for (std::size_t index = 0; index != installed.size(); ++index) delete installed[index];
        installed.clear();
        taken_out.clear();
    }

    /** Allocates a store for a collection holding @p modality, answering null after an allocation failed. */
    collection_store<isolation_>* install(order_t& order, ustore_modality_t modality) noexcept {
        collection_store<isolation_>* fresh = allocate_store<isolation_>(order, modality);
        if (!fresh) return nullptr;
        if (st::succeeded(installed.push_back(std::exchange(fresh, nullptr)))) return installed[installed.size() - 1];
        return nullptr;
    }

    /** Records that @p store leaves the catalog. */
    store_status_t take_out(collection_store<isolation_>* store) noexcept {
        return taken_out.push_back(std::move(store));
    }
};

/** Commits @p stage_change as one catalog change, retiring what it took out at its stamp or freeing what it
 *  installed. @p stage_change is invoked as `stage_change(source, change)` on every attempt. */
template <isolation_t isolation_, typename stage_change_type_>
store_status_t commit_catalog_change(database_state<isolation_>& state, stage_change_type_&& stage_change) noexcept {
    catalog_change<isolation_> change;
    generation_t stamp = 0;
    store_status_t const committed = commit_group(
        state,
        [&](transaction_source<isolation_>& source) noexcept -> store_status_t {
            change.discard();
            return stage_change(source, change);
        },
        &stamp);
    if (st::failed(committed)) {
        change.discard();
        return committed;
    }
    for (std::size_t index = 0; index != change.taken_out.size(); ++index)
        retire(state, change.taken_out[index], stamp);
    free_unreachable(state);
    return store_status_t::success_k;
}

/** Stages collection @p id holding @p modality under @p name in a store @p change installs, refusing a name another
 *  collection carries. */
template <isolation_t isolation_>
store_status_t stage_collection(transaction_source<isolation_>& source, catalog_change<isolation_>& change,
                                ustore_collection_id_t id, ustore_modality_t modality, std::string_view name) noexcept {
    if (!name.empty()) {
        auto names = source.part(source.state.collection_ids_by_name);
        if (!names) return names.status();
        auto [key, key_status] = string_key_from(name);
        if (st::failed(key_status)) return key_status;
        bool taken = false;
        store_status_t const looked_up = (*names)->find(
            key, [&](name_pair_t const&) noexcept { taken = true; }, st::no_op_t {});
        if (st::failed(looked_up)) return looked_up;
        if (taken) return store_status_t::key_already_exists_k;
        store_status_t const named = (*names)->upsert(name_pair_t {std::move(key), id});
        if (st::failed(named)) return named;
    }
    collection_store<isolation_>* const store = change.install(source.state.order, modality);
    if (!store) return store_status_t::out_of_memory_heap_k;
    auto catalog = source.part(source.state.collections_by_id);
    if (!catalog) return catalog.status();
    auto [copied, copy_blob_status] = copy_blob(value_view_t {name});
    if (st::failed(copy_blob_status)) return copy_blob_status;
    return (*catalog)->upsert(
        catalog_pair<isolation_> {id, collection_entry<isolation_> {modality, std::move(copied), store}});
}

/** Stages taking @p collection out of the catalog, and its name out of the names, retiring its store. */
template <isolation_t isolation_>
store_status_t stage_forget(transaction_source<isolation_>& source, catalog_change<isolation_>& change,
                            ustore_collection_id_t collection) noexcept {
    auto catalog = source.part(source.state.collections_by_id);
    if (!catalog) return catalog.status();
    string_key_t name;
    collection_store<isolation_>* store = nullptr;
    store_status_t listing = store_status_t::key_not_found_k;
    store_status_t const looked_up = (*catalog)->find(
        collection,
        [&](catalog_pair<isolation_> const& pair) noexcept {
            store = pair.mapped.store;
            auto copied = pair.mapped.name.copy();
            listing = copied ? store_status_t::success_k : copied.status();
            if (copied) name.text = std::move(*copied);
        },
        st::no_op_t {});
    if (st::failed(looked_up)) return looked_up;
    if (st::failed(listing)) return listing;
    store_status_t const unlisted = (*catalog)->erase(collection);
    if (st::failed(unlisted)) return unlisted;
    store_status_t const retiring = change.take_out(store);
    if (st::failed(retiring) || name.text.size() == 0) return retiring;
    auto names = source.part(source.state.collection_ids_by_name);
    if (!names) return names.status();
    return (*names)->erase(name);
}

/** Stages giving @p collection a fresh empty store, retiring the one it held. */
template <isolation_t isolation_>
store_status_t stage_emptied(transaction_source<isolation_>& source, catalog_change<isolation_>& change,
                             ustore_collection_id_t collection) noexcept {
    auto catalog = source.part(source.state.collections_by_id);
    if (!catalog) return catalog.status();
    collection_entry<isolation_> emptied;
    collection_store<isolation_>* held = nullptr;
    store_status_t listing = store_status_t::key_not_found_k;
    store_status_t const looked_up = (*catalog)->find(
        collection,
        [&](catalog_pair<isolation_> const& pair) noexcept {
            held = pair.mapped.store;
            emptied.modality = pair.mapped.modality;
            auto copied = pair.mapped.name.copy();
            listing = copied ? store_status_t::success_k : copied.status();
            if (copied) emptied.name = std::move(*copied);
        },
        st::no_op_t {});
    if (st::failed(looked_up)) return looked_up;
    if (st::failed(listing)) return listing;
    emptied.store = change.install(source.state.order, emptied.modality);
    if (!emptied.store) return store_status_t::out_of_memory_heap_k;
    store_status_t const retiring = change.take_out(held);
    if (st::failed(retiring)) return retiring;
    return (*catalog)->upsert(catalog_pair<isolation_> {collection, std::move(emptied)});
}

/** Stages emptying every value @p transaction reaches, keeping every key, from @p first on. */
template <typename transaction_type_, typename key_type_>
store_status_t stage_emptied_values(transaction_type_& transaction, key_type_ const& first) noexcept {
    st::basic_vector<key_type_> keys;
    store_status_t copied = store_status_t::success_k;
    store_status_t const walked = transaction.range_from(first, [&](auto const& pair) noexcept -> walk_control_t {
        if constexpr (std::is_same_v<key_type_, string_key_t>) {
            auto key = pair.key.copy();
            copied = key ? keys.push_back(std::move(*key)) : key.status();
        }
        else copied = keys.push_back(key_type_ {pair.key});
        return st::failed(copied) ? walk_control_t::halt_k : walk_control_t::resume_k;
    });
    if (st::failed(walked)) return walked;
    if (st::failed(copied)) return copied;
    for (std::size_t index = 0; index != keys.size(); ++index) {
        store_status_t const emptied = stage_upsert(transaction, std::move(keys[index]), value_view_t {});
        if (st::failed(emptied)) return emptied;
    }
    return store_status_t::success_k;
}

/** Stages emptying every value of @p collection, keeping its keys, which a graph holds none of. */
template <isolation_t isolation_>
store_status_t stage_values_emptied(transaction_source<isolation_>& source, ustore_collection_id_t collection,
                                    ustore_error_t* error) noexcept {
    strided_iterator<ustore_collection_id_t const> const one {&collection, sizeof(collection)};
    return for_each_run(source, one, 1, error, [&](auto& entries, std::size_t, std::size_t) noexcept {
        using entry_t = entry_of<decltype(entries)>;
        if constexpr (std::is_same_v<entry_t, half_edge_t>) return store_status_t::success_k;
        else {
            auto part = source.part(entries);
            if (!part) return part.status();
            if constexpr (std::is_same_v<entry_t, string_pair_t>) return stage_emptied_values(**part, string_key_t {});
            else return stage_emptied_values(**part, smallest_key_k);
        }
    });
}

#pragma endregion Catalog

#pragma region Graph

/** Walks the entries of @p vertex in @p direction through @p view, handing @p visit every one. */
template <typename view_type_, typename visit_type_>
store_status_t walk_direction(view_type_ const& view, ustore_key_t const& vertex, half_edge_direction_t direction,
                              visit_type_&& visit) noexcept {
    auto const next = static_cast<half_edge_direction_t>(static_cast<std::uint8_t>(direction) + 1);
    return view.range(half_edge_t::first_of(vertex, direction), half_edge_t::first_of(vertex, next), visit);
}

/** Exports the degree of tasks [ @p first, @p end ) through @p view of a graph into @p degrees, with their edges as
 *  source, target and edge triples when @p edges is given, watching what it walked when the view watches. */
template <typename view_type_, typename degrees_type_>
store_status_t export_edges(view_type_ const& view, find_edges_t const& tasks, std::size_t first, std::size_t end,
                            degrees_type_& degrees, uninitialized_array<ustore_key_t>* edges,
                            uninitialized_array<half_edge_t>& walked, ustore_error_t* error) noexcept {
    std::size_t const walked_before = walked.size();
    for (std::size_t task = first; task != end && !error->status; ++task) {
        find_edge_t const asked = tasks[task];
        bool present = false;
        store_status_t const looked_up = view.find(
            half_edge_t::vertex_entry(asked.vertex_id), [&](half_edge_t const&) noexcept { present = true; },
            st::no_op_t {});
        if (st::failed(looked_up)) return looked_up;
        if (!present) {
            degrees[task] = ustore_vertex_degree_missing_k;
            continue;
        }
        ustore_vertex_degree_t degree = 0;
        auto const visit = [&](half_edge_t const& entry) noexcept -> walk_control_t {
            ++degree;
            if (view.watches()) walked.push_back(entry, error);
            if (edges) {
                bool const outgoing = entry.direction == half_edge_direction_t::outgoing_k;
                edges->push_back(outgoing ? entry.vertex : entry.neighbor, error);
                edges->push_back(outgoing ? entry.neighbor : entry.vertex, error);
                edges->push_back(entry.edge, error);
            }
            return error->status ? walk_control_t::halt_k : walk_control_t::resume_k;
        };
        if (asked.role & ustore_vertex_source_k) {
            store_status_t const outgoing = walk_direction(view, asked.vertex_id, half_edge_direction_t::outgoing_k,
                                                           visit);
            if (st::failed(outgoing)) return outgoing;
        }
        if (asked.role & ustore_vertex_target_k) {
            store_status_t const incoming = walk_direction(view, asked.vertex_id, half_edge_direction_t::incoming_k,
                                                           visit);
            if (st::failed(incoming)) return incoming;
        }
        degrees[task] = degree;
    }
    for (std::size_t index = walked_before; index != walked.size() && !error->status; ++index) {
        store_status_t const watched = view.watch(walked[index]);
        if (st::failed(watched)) return watched;
    }
    return store_status_t::success_k;
}

/** The three key columns of a batch of edges, whose edge IDs default to @c ustore_default_edge_id_k. */
struct edge_columns_t {
    strided_iterator<ustore_key_t const> edges;
    strided_iterator<ustore_key_t const> sources;
    strided_iterator<ustore_key_t const> targets;

    /** The ID of edge @p index. */
    ustore_key_t edge(std::size_t index) const noexcept { return edges ? edges[index] : ustore_default_edge_id_k; }
};

/** Stages @p vertex into @p part of a graph unless it already is one, which never writes over a present vertex, so
 *  edges added to one vertex by two transactions don't conflict over it. */
template <typename transaction_type_>
store_status_t stage_vertex(transaction_type_& part, ustore_key_t const& vertex) noexcept {
    return part.insert_if_missing(half_edge_t::vertex_entry(vertex));
}

/** Stages edges [ @p first, @p end ) into @p part of a graph, under both of their ends, and each end as a vertex. */
template <typename transaction_type_>
store_status_t stage_edges(transaction_type_& part, edge_columns_t const& columns, std::size_t first,
                           std::size_t end) noexcept {
    for (std::size_t index = first; index != end; ++index) {
        ustore_key_t const source = columns.sources[index];
        ustore_key_t const target = columns.targets[index];
        ustore_key_t const edge = columns.edge(index);
        store_status_t status = stage_vertex(part, source);
        if (st::succeeded(status)) status = stage_vertex(part, target);
        if (st::succeeded(status))
            status = part.upsert(half_edge_t {source, half_edge_direction_t::outgoing_k, target, edge});
        if (st::succeeded(status))
            status = part.upsert(half_edge_t {target, half_edge_direction_t::incoming_k, source, edge});
        if (st::failed(status)) return status;
    }
    return store_status_t::success_k;
}

/** Stages erasing @p entry from @p part, which may be gone already. */
template <typename transaction_type_>
store_status_t stage_absent(transaction_type_& part, half_edge_t const& entry) noexcept {
    store_status_t const erased = part.erase(entry);
    return erased == store_status_t::key_not_found_k ? store_status_t::success_k : erased;
}

/** Stages erasing edges [ @p first, @p end ) from @p part of a graph, under both of their ends, keeping the
 *  vertices. */
template <typename transaction_type_>
store_status_t stage_edges_removal(transaction_type_& part, edge_columns_t const& columns, std::size_t first,
                                   std::size_t end) noexcept {
    for (std::size_t index = first; index != end; ++index) {
        ustore_key_t const source = columns.sources[index];
        ustore_key_t const target = columns.targets[index];
        ustore_key_t const edge = columns.edge(index);
        store_status_t status = stage_absent(part,
                                             half_edge_t {source, half_edge_direction_t::outgoing_k, target, edge});
        if (st::succeeded(status))
            status = stage_absent(part, half_edge_t {target, half_edge_direction_t::incoming_k, source, edge});
        if (st::failed(status)) return status;
    }
    return store_status_t::success_k;
}

/** Stages removing @p vertex from @p part of a graph: the edges its neighbors file of it in @p role, then every entry
 *  of its own. */
template <typename transaction_type_>
store_status_t stage_vertex_removal(transaction_type_& part, ustore_key_t const& vertex, ustore_vertex_role_t role,
                                    uninitialized_array<half_edge_t>& walked, ustore_error_t* error) noexcept {
    std::size_t const first = walked.size();
    auto const collect = [&](half_edge_t const& entry) noexcept -> walk_control_t {
        walked.push_back(entry, error);
        return error->status ? walk_control_t::halt_k : walk_control_t::resume_k;
    };
    if (role & ustore_vertex_source_k) {
        store_status_t const outgoing = walk_direction(part, vertex, half_edge_direction_t::outgoing_k, collect);
        if (st::failed(outgoing) || error->status) return outgoing;
    }
    if (role & ustore_vertex_target_k) {
        store_status_t const incoming = walk_direction(part, vertex, half_edge_direction_t::incoming_k, collect);
        if (st::failed(incoming) || error->status) return incoming;
    }
    for (std::size_t index = first; index != walked.size(); ++index) {
        half_edge_t const seen = walked[index];
        half_edge_direction_t const mirrored = seen.direction == half_edge_direction_t::outgoing_k
                                                   ? half_edge_direction_t::incoming_k
                                                   : half_edge_direction_t::outgoing_k;
        store_status_t const erased = stage_absent(part, half_edge_t {seen.neighbor, mirrored, vertex, seen.edge});
        if (st::failed(erased)) return erased;
    }
    return part.erase_range(half_edge_t::vertex_entry(vertex),
                            half_edge_t::first_of(vertex, half_edge_direction_t::past_k), st::no_op_t {});
}

#pragma endregion Graph

#pragma region Image

/**
 *  @brief Leads every image, so a file of another format is refused rather than misread.
 *
 *  The image then lists every collection until the end of the file, the main one under the nil UUID:
 *
 *  @code{.c}
 *  struct collection {
 *      uint8_t id[16];       // offset 0
 *      uint8_t modality;     // offset 16, which picks the shape of every entry below
 *      uint64_t name_length; // offset 17, zero for a collection without a name
 *      char name[];          // offset 25
 *      uint64_t count;       // entries that follow
 *      union {
 *          struct { uint8_t key[16]; uint64_t length; uint8_t value[]; } value;                // blobs, docs, any
 *          struct { uint64_t length; char text[]; uint64_t value_length; uint8_t value[]; } string; // strings
 *          struct half_edge edge;                                                            // graph, 49 bytes
 *      } entries[];
 *  };
 *  @endcode
 */
inline constexpr char image_signature_k[8] = {'U', 'S', 'T', 'S', 'M', 'T', 'B', '1'};

/** Name of the image inside the database directory. */
inline constexpr std::string_view image_name_k = "/ram.image";

/** Name of the image while it is being written, renamed over the image once whole. */
inline constexpr std::string_view partial_image_name_k = "/ram.image.partial";

#if defined(_WIN32)

/** Flushes the file behind @p file to the drive. */
inline bool sync_file(std::FILE* file) noexcept { return ::_commit(::_fileno(file)) == 0; }

/** Replaces @p target with @p source in one step, overwriting an existing target. */
inline bool replace_file(char const* source, char const* target) noexcept {
    return ::MoveFileExA(source, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
#else

/** Flushes the file behind @p file to the drive. */
inline bool sync_file(std::FILE* file) noexcept { return ::fsync(::fileno(file)) == 0; }

/** Replaces @p target with @p source in one step, overwriting an existing target. */
inline bool replace_file(char const* source, char const* target) noexcept { return std::rename(source, target) == 0; }
#endif

/** Closes a C stream when it leaves scope, unless it was closed explicitly. */
class stream_t {
    std::FILE* handle_ = nullptr;

  public:
    explicit stream_t(std::FILE* handle) noexcept : handle_(handle) {}
    stream_t(stream_t const&) = delete;
    stream_t& operator=(stream_t const&) = delete;

    ~stream_t() noexcept {
        if (handle_) { [[maybe_unused]] int const closed = std::fclose(handle_); }
    }

    /** The stream, or null once closed. */
    std::FILE* get() const noexcept { return handle_; }

    /** Closes the stream now, answering whether everything buffered reached the file. */
    bool close() noexcept { return std::fclose(std::exchange(handle_, nullptr)) == 0; }
};

/** Joins @p directory and @p name into @p path, answering whether it fit in memory. */
inline bool join_path(sz::string_t& path, char const* directory, std::string_view name) noexcept {
    return path.try_assign(sz::string_view_t {directory}) && path.try_append(name.data(), name.size());
}

/** Writes @p blob as its length and its bytes, answering whether all of it reached @p file. */
inline bool write_blob(std::FILE* file, blob_t const& blob) noexcept {
    auto const length = static_cast<std::uint64_t>(blob.size());
    return std::fwrite(&length, sizeof(length), 1, file) == 1 &&
           (length == 0 || std::fwrite(blob.data(), 1, length, file) == length);
}

/** Writes one entry of a collection of values, answering whether all of it reached @p file. */
inline bool write_entry(std::FILE* file, value_pair_t const& pair) noexcept {
    return std::fwrite(&pair.key, sizeof(ustore_key_t), 1, file) == 1 && write_blob(file, pair.mapped);
}

/** Writes one entry of a collection of strings, answering whether all of it reached @p file. */
inline bool write_entry(std::FILE* file, string_pair_t const& pair) noexcept {
    return write_blob(file, pair.key.text) && write_blob(file, pair.mapped);
}

/** Writes one entry of a graph, answering whether all of it reached @p file. */
inline bool write_entry(std::FILE* file, half_edge_t const& entry) noexcept {
    return std::fwrite(&entry, sizeof(half_edge_t), 1, file) == 1;
}

/** The first entry a store of @p entry_type_ can hold, where a walk of the whole store starts. */
template <typename entry_type_>
auto first_entry() noexcept {
    if constexpr (std::is_same_v<entry_type_, string_pair_t>) return string_key_t {};
    else return entry_for<entry_type_>(smallest_key_k);
}

/** Writes how many entries @p store holds at @p stamp, then every one of them, clearing @p written when any of it
 *  misses @p file. */
template <typename store_type_>
store_status_t write_entries(std::FILE* file, store_type_ const& store, generation_t stamp, bool& written) noexcept {
    using entry_t = entry_of<store_type_>;
    auto const reader = store.reader_at(stamp);
    std::uint64_t count = 0;
    store_status_t const counted = reader.range_from(first_entry<entry_t>(), [&](entry_t const&) noexcept { ++count; });
    if (st::failed(counted)) return counted;
    written = written && std::fwrite(&count, sizeof(count), 1, file) == 1;
    return reader.range_from(first_entry<entry_t>(), [&](entry_t const& entry) noexcept -> walk_control_t {
        written = written && write_entry(file, entry);
        return written ? walk_control_t::resume_k : walk_control_t::halt_k;
    });
}

/** Writes the image of every collection of @p state at @p stamp into @p directory. */
template <isolation_t isolation_>
void write_image(database_state<isolation_>& state, generation_t stamp, char const* directory,
                 ustore_error_t* error) noexcept {
    sz::string_t partial_path;
    sz::string_t final_path;
    if (!join_path(partial_path, directory, partial_image_name_k) || !join_path(final_path, directory, image_name_k)) {
        *error = {ustore_error_out_of_memory_k, "Failed to build the image path"};
        return;
    }
    listed_collections<isolation_> listed;
    reader_source<isolation_> source {state, stamp};
    auto catalog = source.view(state.collections_by_id);
    if (reported_failure(collect_catalog<isolation_>(*catalog, listed), error)) return;
    stream_t stream {std::fopen(partial_path.c_str(), "wb")};
    if (!stream.get()) {
        *error = {ustore_error_io_k, "Failed to open the image for writing"};
        return;
    }

    bool written = std::fwrite(image_signature_k, sizeof(image_signature_k), 1, stream.get()) == 1;
    for (std::size_t index = 0; index != listed.size() && written; ++index) {
        listed_collection<isolation_> const& collection = listed[index];
        auto const modality = static_cast<std::uint8_t>(collection.modality);
        written = std::fwrite(&collection.id, sizeof(ustore_collection_id_t), 1, stream.get()) == 1 &&
                  std::fwrite(&modality, sizeof(modality), 1, stream.get()) == 1 &&
                  write_blob(stream.get(), collection.name);
        store_status_t const walked = std::visit(
            [&](auto const& entries) noexcept { return write_entries(stream.get(), entries, stamp, written); },
            collection.store->entries);
        if (reported_failure(walked, error)) return;
    }

    bool const synced = written && std::fflush(stream.get()) == 0 && sync_file(stream.get());
    bool const closed = stream.close();
    if (!synced || !closed) {
        *error = {ustore_error_io_k, "Failed to write the image"};
        return;
    }
    if (!replace_file(partial_path.c_str(), final_path.c_str()))
        *error = {ustore_error_io_k, "Failed to replace the image"};
}

/** What loading one part of an image came to. */
enum class image_step_t : std::uint8_t { loaded_k, finished_k, truncated_k, malformed_k, out_of_memory_k, refused_k };

/** The error of every image step, success for the two that are not failures. */
inline constexpr std::array<ustore_error_t, 6> image_step_errors_k {{
    {},
    {},
    {ustore_error_malformed_k, "The image is truncated"},
    {ustore_error_malformed_k, "The image names an unknown modality"},
    {ustore_error_out_of_memory_k, "Out of memory while loading the image"},
    {ustore_error_internal_k, "The store refused an entry of the image"},
}};

/** Reads a length and that many bytes of @p file into @p blob. */
inline image_step_t load_blob(std::FILE* file, blob_t& blob) noexcept {
    std::uint64_t length = 0;
    if (std::fread(&length, sizeof(length), 1, file) != 1) return image_step_t::truncated_k;
    if (st::failed(blob.resize(length))) return image_step_t::out_of_memory_k;
    if (length != 0 && std::fread(blob.data(), 1, length, file) != length) return image_step_t::truncated_k;
    return image_step_t::loaded_k;
}

/** Loads the next entry of @p file into @p store. */
template <typename store_type_>
image_step_t load_entry(std::FILE* file, store_type_& store) noexcept {
    using entry_t = entry_of<store_type_>;
    if constexpr (std::is_same_v<entry_t, half_edge_t>) {
        half_edge_t entry;
        if (std::fread(&entry, sizeof(half_edge_t), 1, file) != 1) return image_step_t::truncated_k;
        return st::failed(store.upsert(std::move(entry))) ? image_step_t::refused_k : image_step_t::loaded_k;
    }
    else {
        blob_t value;
        if constexpr (std::is_same_v<entry_t, string_pair_t>) {
            string_key_t key;
            if (image_step_t const loaded = load_blob(file, key.text); loaded != image_step_t::loaded_k) return loaded;
            if (image_step_t const loaded = load_blob(file, value); loaded != image_step_t::loaded_k) return loaded;
            return st::failed(store.upsert(string_pair_t {std::move(key), std::move(value)})) ? image_step_t::refused_k
                                                                                              : image_step_t::loaded_k;
        }
        else {
            ustore_key_t key;
            if (std::fread(&key, sizeof(ustore_key_t), 1, file) != 1) return image_step_t::truncated_k;
            if (image_step_t const loaded = load_blob(file, value); loaded != image_step_t::loaded_k) return loaded;
            return st::failed(store.upsert(value_pair_t {key, std::move(value)})) ? image_step_t::refused_k
                                                                                  : image_step_t::loaded_k;
        }
    }
}

/** Loads the next collection of @p file into @p state, filling @p main for the nil UUID. */
template <isolation_t isolation_>
image_step_t load_collection(std::FILE* file, database_state<isolation_>& state,
                             collection_store<isolation_>* main) noexcept {
    ustore_collection_id_t id;
    if (std::fread(&id, sizeof(id), 1, file) != 1) return image_step_t::finished_k;
    std::uint8_t modality = 0;
    blob_t name;
    std::uint64_t count = 0;
    if (std::fread(&modality, sizeof(modality), 1, file) != 1) return image_step_t::truncated_k;
    if (modality > ustore_modality_pages_k) return image_step_t::malformed_k;
    if (image_step_t const loaded = load_blob(file, name); loaded != image_step_t::loaded_k) return loaded;
    if (std::fread(&count, sizeof(count), 1, file) != 1) return image_step_t::truncated_k;

    collection_store<isolation_>* store = main;
    if (id != ustore_collection_main_k) {
        if (name.size() != 0) {
            auto [key, key_status] = string_key_from(view_of(name));
            if (st::failed(key_status)) return image_step_t::out_of_memory_k;
            if (st::failed(state.collection_ids_by_name.upsert(name_pair_t {std::move(key), id})))
                return image_step_t::refused_k;
        }
        store = allocate_store<isolation_>(state.order, static_cast<ustore_modality_t>(modality));
        if (!store) return image_step_t::out_of_memory_k;
        catalog_pair<isolation_> pair {
            id, collection_entry<isolation_> {static_cast<ustore_modality_t>(modality), std::move(name), store}};
        if (st::failed(state.collections_by_id.upsert(std::move(pair)))) {
            delete store;
            return image_step_t::refused_k;
        }
    }
    for (std::uint64_t index = 0; index != count; ++index) {
        image_step_t const loaded = std::visit([&](auto& entries) noexcept { return load_entry(file, entries); },
                                               store->entries);
        if (loaded != image_step_t::loaded_k) return loaded;
    }
    return image_step_t::loaded_k;
}

/** Loads the image of the directory of @p state into its stores, the main collection's entries into @p main, leaving
 *  them empty when there is none. */
template <isolation_t isolation_>
void read_image(database_state<isolation_>& state, collection_store<isolation_>* main, ustore_error_t* error) noexcept {
    sz::string_t path;
    if (!join_path(path, state.directory.c_str(), image_name_k)) {
        *error = {ustore_error_out_of_memory_k, "Failed to build the image path"};
        return;
    }
    stream_t stream {std::fopen(path.c_str(), "rb")};
    if (!stream.get()) return;
    char signature[sizeof(image_signature_k)];
    if (std::fread(signature, sizeof(signature), 1, stream.get()) != 1 ||
        std::memcmp(signature, image_signature_k, sizeof(signature)) != 0) {
        *error = {ustore_error_malformed_k, "The image has an unknown format"};
        return;
    }
    image_step_t step = image_step_t::loaded_k;
    while (step == image_step_t::loaded_k) step = load_collection(stream.get(), state, main);
    *error = image_step_errors_k[static_cast<std::size_t>(step)];
}

/** Rewrites the image of @p state from its newest published commit, when it has a directory. */
template <isolation_t isolation_>
void persist(database_state<isolation_>& state, ustore_error_t* error) noexcept {
    if (state.directory.empty()) return;
    claim_t claim;
    write_image(state, state.order.take_snapshot(claim), state.directory.c_str(), error);
}

#pragma endregion Image

#pragma region Lifetime

/** Lists the main collection in the catalog of @p state, then remembers the directory and loads its image. */
template <isolation_t isolation_>
void open_database(database_state<isolation_>& state, std::string_view directory, ustore_error_t* error) noexcept {
    collection_store<isolation_>* const main = allocate_store<isolation_>(state.order, ustore_modality_any_k);
    if (!main) {
        *error = {ustore_error_out_of_memory_k, "Failed to allocate the main collection"};
        return;
    }
    catalog_pair<isolation_> listed {ustore_collection_main_k,
                                     collection_entry<isolation_> {ustore_modality_any_k, {}, main}};
    if (st::failed(state.collections_by_id.upsert(std::move(listed)))) {
        delete main;
        *error = {ustore_error_out_of_memory_k, "Failed to list the main collection"};
        return;
    }
    if (directory.empty()) return;
    if (!state.directory.try_assign(sz::string_view_t {directory})) {
        *error = {ustore_error_out_of_memory_k, "Failed to copy the directory"};
        return;
    }
    read_image(state, main, error);
}

/** Persists @p state ahead of freeing it, which every snapshot and transaction of it must precede. */
template <isolation_t isolation_>
void close_database(database_state<isolation_>& state) noexcept {
    ustore_error_t error {};
    persist(state, &error);
}

/** Serves "clear", giving every collection a fresh empty store and keeping the catalog and the names. */
template <isolation_t isolation_>
void clear_pairs(database_state<isolation_>& state, ustore_database_control_t& arguments) noexcept {
    store_status_t const cleared = commit_catalog_change(
        state, [&](transaction_source<isolation_>& source, catalog_change<isolation_>& change) noexcept {
            auto catalog = source.view(state.collections_by_id);
            if (!catalog) return catalog.status();
            listed_collections<isolation_> listed;
            store_status_t const collected = collect_catalog<isolation_>(*catalog, listed);
            for (std::size_t index = 0; index != listed.size() && st::succeeded(collected); ++index) {
                store_status_t const emptied = stage_emptied(source, change, listed[index].id);
                if (st::failed(emptied)) return emptied;
            }
            return collected;
        });
    reported_failure(cleared, arguments.error);
}

/** Serves "reset", removing every collection but the main one, which it empties, and every name, which no open
 *  snapshot may still be reading. */
template <isolation_t isolation_>
void reset_database(database_state<isolation_>& state, ustore_database_control_t& arguments) noexcept {
    if (fu::standard_atomic_ref<std::size_t>(state.open_snapshots).load(st::memory_order_acquire_k) != 0) {
        *arguments.error = {ustore_error_invalid_argument_k, "A reset waits until every snapshot is freed"};
        return;
    }
    store_status_t const reset = commit_catalog_change(
        state, [&](transaction_source<isolation_>& source, catalog_change<isolation_>& change) noexcept {
            auto catalog = source.view(state.collections_by_id);
            if (!catalog) return catalog.status();
            listed_collections<isolation_> listed;
            store_status_t const collected = collect_catalog<isolation_>(*catalog, listed);
            if (st::failed(collected)) return collected;
            for (std::size_t index = 0; index != listed.size(); ++index) {
                ustore_collection_id_t const collection = listed[index].id;
                store_status_t const changed = collection == ustore_collection_main_k
                                                   ? stage_emptied(source, change, collection)
                                                   : stage_forget(source, change, collection);
                if (st::failed(changed)) return changed;
            }
            return store_status_t::success_k;
        });
    reported_failure(reset, arguments.error);
}

/** Serves "compact", reclaiming unreachable versions of every store, freeing unreachable retired stores and
 *  rewriting the image. */
template <isolation_t isolation_>
void compact_database(database_state<isolation_>& state, ustore_database_control_t& arguments) noexcept {
    claim_t claim;
    reader_source<isolation_> source {state, state.order.take_snapshot(claim)};
    listed_collections<isolation_> listed;
    auto catalog = source.view(state.collections_by_id);
    if (reported_failure(collect_catalog<isolation_>(*catalog, listed), arguments.error)) return;
    for (std::size_t index = 0; index != listed.size(); ++index) {
        store_status_t const vacuumed = std::visit([](auto& entries) noexcept { return entries.vacuum().status(); },
                                                   listed[index].store->entries);
        if (reported_failure(vacuumed, arguments.error)) return;
    }
    if (reported_failure(state.collections_by_id.vacuum().status(), arguments.error)) return;
    if (reported_failure(state.collection_ids_by_name.vacuum().status(), arguments.error)) return;
    claim.retire();
    free_unreachable(state);
    persist(state, arguments.error);
}

/** Serves "info", naming the engine and its isolation. */
template <isolation_t isolation_>
void describe_database(database_state<isolation_>&, ustore_database_control_t& arguments) noexcept {
    *arguments.response = isolation_ == isolation_t::strict_serializable_k
                              ? R"({"engine":"ram","isolation":"strict_serializable"})"
                              : R"({"engine":"ram","isolation":"snapshot"})";
}

/** Serves "usage", counting keys and versions of every store into the arena. */
template <isolation_t isolation_>
void report_usage(database_state<isolation_>& state, ustore_database_control_t& arguments) noexcept {
    linked_memory_lock_t arena = linked_memory(arguments.arena, ustore_options_default_k, arguments.error);
    if (arguments.error->status) return;
    claim_t claim;
    reader_source<isolation_> source {state, state.order.take_snapshot(claim)};
    listed_collections<isolation_> listed;
    auto catalog = source.view(state.collections_by_id);
    if (reported_failure(collect_catalog<isolation_>(*catalog, listed), arguments.error)) return;
    std::size_t keys = state.collections_by_id.size() + state.collection_ids_by_name.size();
    std::size_t versions = state.collections_by_id.versions_count() + state.collection_ids_by_name.versions_count();
    for (std::size_t index = 0; index != listed.size(); ++index)
        std::visit(
            [&](auto const& entries) noexcept {
                keys += entries.size();
                versions += entries.versions_count();
            },
            listed[index].store->entries);

    constexpr std::size_t response_capacity_k = 96;
    char* const response = arena.alloc<char>(response_capacity_k, arguments.error).data();
    if (arguments.error->status) return;
    auto const written = fmt::format_to_n(response, response_capacity_k - 1, R"({{"keys":{},"versions":{}}})", keys,
                                          versions);
    *written.out = '\0';
    *arguments.response = response;
}

/** One free-form request of @c ustore_database_control. */
template <isolation_t isolation_>
struct control_request {

    /** The name a caller sends. */
    std::string_view name;

    /** Serves the request. */
    void (*serve)(database_state<isolation_>&, ustore_database_control_t&) noexcept;
};

/** Every request @c ustore_database_control serves. */
template <isolation_t isolation_>
inline constexpr std::array<control_request<isolation_>, 5> control_requests_k {{
    {"clear", &clear_pairs<isolation_>},
    {"reset", &reset_database<isolation_>},
    {"compact", &compact_database<isolation_>},
    {"info", &describe_database<isolation_>},
    {"usage", &report_usage<isolation_>},
}};

#pragma endregion Lifetime

#pragma region Argument Views

/** The locations of a read. */
inline places_arg_t places_of(ustore_read_t const& arguments) noexcept {
    return {{arguments.collections, arguments.collections_stride},
            {arguments.keys, arguments.keys_stride},
            {},
            arguments.tasks_count};
}

/** The locations of a write. */
inline places_arg_t places_of(ustore_write_t const& arguments) noexcept {
    return {{arguments.collections, arguments.collections_stride},
            {arguments.keys, arguments.keys_stride},
            {},
            arguments.tasks_count};
}

/** The values of a write. */
inline contents_arg_t contents_of(ustore_write_t const& arguments) noexcept {
    return {bits_view_t {arguments.presences},
            {arguments.offsets, arguments.offsets_stride},
            {arguments.lengths, arguments.lengths_stride},
            {arguments.values, arguments.values_stride},
            arguments.tasks_count};
}

/** The tasks of a scan. */
inline scans_arg_t scans_of(ustore_scan_t const& arguments) noexcept {
    return {{arguments.collections, arguments.collections_stride},
            {arguments.start_keys, arguments.start_keys_stride},
            {arguments.count_limits, arguments.count_limits_stride},
            arguments.tasks_count};
}

/** The tasks of a sample. */
inline sample_args_t samples_of(ustore_sample_t const& arguments) noexcept {
    return {{arguments.collections, arguments.collections_stride},
            {arguments.count_limits, arguments.count_limits_stride},
            arguments.tasks_count};
}

/** The keys @p scans may return together, capped so a huge limit reserves nothing it may never fill. */
inline std::size_t expected_keys(scans_arg_t const& scans) noexcept {
    constexpr std::size_t reservation_cap_k = std::size_t {1} << 16;
    std::size_t expected = 0;
    for (std::size_t task = 0; task != scans.size() && expected < reservation_cap_k; ++task)
        expected += scans[task].limit;
    return std::min(expected, reservation_cap_k);
}

/** The view a call names, from the fields every view-taking argument struct shares. */
template <typename arguments_type_>
view_request_t view_of(arguments_type_ const& arguments) noexcept {
    return {arguments.transaction, arguments.snapshot, arguments.options};
}

/** The watch an erase of a transaction at @p isolation_ called with @p options takes on a key already gone, which a
 *  strictly serializable one records already. */
template <isolation_t isolation_>
constexpr watch_t erase_watch(ustore_options_t options) noexcept {
    return watches_reads<isolation_>(options) ? watch_t::watch_k : watch_t::dont_watch_k;
}

#pragma endregion Argument Views

#pragma region Database Calls

/** Implements @c ustore_database_control through the table of known requests. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_database_control_t& arguments) noexcept {
    if (!arguments.request || !arguments.response) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need a request and an output for the response"};
        return;
    }
    *arguments.response = nullptr;
    std::string_view const request {arguments.request};
    for (control_request<isolation_> const& known : control_requests_k<isolation_>)
        if (known.name == request) return known.serve(state, arguments);
    *arguments.error = {ustore_error_invalid_argument_k,
                        "Unknown request: expected clear, reset, compact, info or usage"};
}

#pragma endregion Database Calls

#pragma region Snapshot Calls

/** Implements @c ustore_snapshot_init with a claim on the newest published stamp. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_snapshot_init_t& arguments) noexcept {
    if (!arguments.snapshot) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need an output for the snapshot"};
        return;
    }
    auto* const handle = new (std::nothrow) snapshot_handle<isolation_>(state);
    if (!handle) {
        *arguments.error = {ustore_error_out_of_memory_k, "Failed to allocate the snapshot"};
        return;
    }
    state.order.take_snapshot(handle->claim);
    fu::standard_atomic_ref<std::size_t>(state.open_snapshots).fetch_add(1, st::memory_order_release_k);
    *arguments.snapshot = c_handle<ustore_snapshot_t>(handle);
}

/** Implements @c ustore_snapshot_export as an image in the export directory. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_snapshot_export_t& arguments) noexcept {
    if (!arguments.snapshot) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need a snapshot to export"};
        return;
    }
    if (!arguments.path || !names_directory(arguments.path)) {
        *arguments.error = {ustore_error_not_found_k, "The export path isn't a directory"};
        return;
    }
    snapshot_handle<isolation_>* const handle = snapshot_of<isolation_>(arguments.snapshot, arguments.error);
    if (!handle) return;
    write_image(state, handle->claim.snapshot(), arguments.path, arguments.error);
}

#pragma endregion Snapshot Calls

#pragma region Blob Calls

/** Implements @c ustore_read, copying every value into the caller's arena once, where a graph's vertices read as
 *  empty values. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_read_t& arguments) noexcept {
    if (!arguments.tasks_count) return;
    places_arg_t const places = places_of(arguments);
    validate_read(arguments.transaction, places, arguments.options, arguments.error);
    if (arguments.error->status) return;
    strided_iterator<ustore_collection_id_t const> const collections {arguments.collections,
                                                                      arguments.collections_stride};
    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;
    value_tape tape {arena, arguments};
    if (arguments.error->status) return;

    with_source(state, view_of(arguments), arguments.error, [&](auto& source) noexcept {
        store_status_t const read = for_each_run(
            source, collections, places.size(), arguments.error,
            [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
                using entry_t = entry_of<decltype(entries)>;
                if constexpr (std::is_same_v<entry_t, string_pair_t>)
                    return refused(arguments.error, "The collection holds strings");
                else {
                    auto view = source.view(entries);
                    if (!view) return view.status();
                    return read_keys<entry_t>(*view, places, first, end, tape, arguments.error);
                }
            });
        report(read, arguments.error);
    });
    if (arguments.error->status) return;
    tape.finish();
}

/** Stages every run of @p places into the collection it names through @p source, refusing a graph, which changes
 *  through the graph calls. */
template <typename source_type_>
store_status_t stage_write_runs(source_type_& source, strided_iterator<ustore_collection_id_t const> collections,
                                places_arg_t const& places, contents_arg_t const& contents, watch_t watch,
                                ustore_error_t* error) noexcept {
    return for_each_run(source, collections, places.size(), error,
                        [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
                            using entry_t = entry_of<decltype(entries)>;
                            if constexpr (std::is_same_v<entry_t, string_pair_t>)
                                return refused(error, "The collection holds strings");
                            else if constexpr (std::is_same_v<entry_t, half_edge_t>)
                                return refused(error, "A graph collection changes through the graph calls");
                            else {
                                auto part = source.part(entries);
                                if (!part) return part.status();
                                return stage_writes(**part, places, contents, first, end, watch);
                            }
                        });
}

/** Writes the one task of @p arguments at the newest stamp, straight into its collection's store. */
template <isolation_t isolation_>
store_status_t write_one(database_state<isolation_>& state, ustore_write_t& arguments, places_arg_t const& places,
                         contents_arg_t const& contents) noexcept {
    claim_t claim;
    reader_source<isolation_> source {state, state.order.take_snapshot(claim)};
    strided_iterator<ustore_collection_id_t const> const collections {arguments.collections,
                                                                      arguments.collections_stride};
    return for_each_run(source, collections, 1, arguments.error,
                        [&](auto& entries, std::size_t, std::size_t) noexcept -> store_status_t {
                            using entry_t = entry_of<decltype(entries)>;
                            if constexpr (std::is_same_v<entry_t, string_pair_t>)
                                return refused(arguments.error, "The collection holds strings");
                            else if constexpr (std::is_same_v<entry_t, half_edge_t>)
                                return refused(arguments.error, "A graph collection changes through the graph calls");
                            else return write_directly(entries, places[0].key, contents[0]);
                        });
}

/** Implements @c ustore_write, committing a batch outside a transaction atomically. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_write_t& arguments) noexcept {
    places_arg_t const places = places_of(arguments);
    contents_arg_t const contents = contents_of(arguments);
    validate_write(arguments.transaction, places, contents, arguments.options, arguments.error);
    if (arguments.error->status || !arguments.tasks_count) return;
    strided_iterator<ustore_collection_id_t const> const collections {arguments.collections,
                                                                      arguments.collections_stride};
    if (arguments.transaction) {
        transaction_handle<isolation_>* const handle = transaction_of<isolation_>(arguments.transaction,
                                                                                  arguments.error);
        if (!handle) return;
        transaction_source<isolation_> source {state, handle->transaction,
                                               watches_reads<isolation_>(arguments.options)};
        report(stage_write_runs(source, collections, places, contents, erase_watch<isolation_>(arguments.options),
                                arguments.error),
               arguments.error);
        return;
    }

    store_status_t const written = places.size() == 1
                                       ? write_one(state, arguments, places, contents)
                                       : commit_group(state, [&](transaction_source<isolation_>& source) noexcept {
                                             return stage_write_runs(source, collections, places, contents,
                                                                     watch_t::dont_watch_k, arguments.error);
                                         });
    report(written, arguments.error);
    if (!arguments.error->status && (arguments.options & ustore_option_write_flush_k)) persist(state, arguments.error);
}

/** Implements @c ustore_scan as ordered walks of each collection's keys, a graph's being its vertices. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_scan_t& arguments) noexcept {
    scans_arg_t const scans = scans_of(arguments);
    validate_scan(arguments.transaction, scans, arguments.options, arguments.error);
    if (arguments.error->status || !arguments.tasks_count) return;
    strided_iterator<ustore_collection_id_t const> const collections {arguments.collections,
                                                                      arguments.collections_stride};
    if (!arguments.keys) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need an output for the keys"};
        return;
    }
    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;
    key_tape tape {arena, scans.size(), arguments.offsets, arguments.counts, arguments.error};
    tape.reserve(expected_keys(scans));
    if (arguments.error->status) return;

    with_source(state, view_of(arguments), arguments.error, [&](auto& source) noexcept {
        store_status_t const walked = for_each_run(
            source, collections, scans.size(), arguments.error,
            [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
                using entry_t = entry_of<decltype(entries)>;
                if constexpr (std::is_same_v<entry_t, string_pair_t>)
                    return refused(arguments.error, "The collection holds strings");
                else {
                    auto view = source.view(entries);
                    if (!view) return view.status();
                    for (std::size_t task = first; task != end; ++task) {
                        std::size_t const before = tape.size();
                        store_status_t const scanned = scan_window<entry_t>(*view, scans[task], tape);
                        if (st::failed(scanned) || arguments.error->status) return scanned;
                        tape.close_task(task, before);
                    }
                    return store_status_t::success_k;
                }
            });
        report(walked, arguments.error);
    });
    if (arguments.error->status) return;
    tape.finish(arguments.keys);
}

/** Implements @c ustore_sample as a reservoir filled from a walk of each collection's keys. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_sample_t& arguments) noexcept {
    if (!arguments.tasks_count) return;
    if (!arguments.count_limits || !arguments.keys) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need count limits and an output for the keys"};
        return;
    }
    sample_args_t const samples = samples_of(arguments);
    strided_iterator<ustore_collection_id_t const> const collections {arguments.collections,
                                                                      arguments.collections_stride};
    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;
    key_tape tape {arena, samples.size(), arguments.offsets, arguments.counts, arguments.error};
    if (arguments.error->status) return;
    auto const now = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    sample_generator_t generator {now ^ reinterpret_cast<std::uintptr_t>(&tape)};

    with_source(state, view_of(arguments), arguments.error, [&](auto& source) noexcept {
        store_status_t const walked = for_each_run(
            source, collections, samples.size(), arguments.error,
            [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
                using entry_t = entry_of<decltype(entries)>;
                if constexpr (std::is_same_v<entry_t, string_pair_t>)
                    return refused(arguments.error, "The collection holds strings");
                else {
                    auto view = source.view(entries);
                    if (!view) return view.status();
                    for (std::size_t task = first; task != end; ++task) {
                        std::size_t const before = tape.size();
                        store_status_t const sampled = sample_window<entry_t>(*view, samples[task], generator, tape);
                        if (st::failed(sampled) || arguments.error->status) return sampled;
                        tape.close_task(task, before);
                    }
                    return store_status_t::success_k;
                }
            });
        report(walked, arguments.error);
    });
    if (arguments.error->status) return;
    tape.finish(arguments.keys);
}

/** The six columns a measurement exports. */
struct measure_columns_t {

    /** Lower bounds on the entries. */
    range_or_dummy<std::span<ustore_size_t>> min_cardinalities;

    /** Upper bounds on the entries. */
    range_or_dummy<std::span<ustore_size_t>> max_cardinalities;

    /** Lower bounds on the value bytes. */
    range_or_dummy<std::span<ustore_size_t>> min_value_bytes;

    /** Upper bounds on the value bytes. */
    range_or_dummy<std::span<ustore_size_t>> max_value_bytes;

    /** Lower bounds on the space. */
    range_or_dummy<std::span<ustore_size_t>> min_space_usages;

    /** Upper bounds on the space. */
    range_or_dummy<std::span<ustore_size_t>> max_space_usages;

    /** Records the exact measure of @p task. */
    void assign(std::size_t task, window_measure_t measured) noexcept {
        min_cardinalities[task] = measured.cardinality;
        max_cardinalities[task] = measured.cardinality;
        min_value_bytes[task] = measured.value_bytes;
        max_value_bytes[task] = measured.value_bytes;
        min_space_usages[task] = measured.space_bytes;
        max_space_usages[task] = std::numeric_limits<ustore_size_t>::max();
    }
};

/** Implements @c ustore_measure, exact for entries and value bytes at the view it reads. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_measure_t& arguments) noexcept {
    if (!arguments.tasks_count) return;
    strided_iterator<ustore_collection_id_t const> const collections {arguments.collections,
                                                                      arguments.collections_stride};
    strided_iterator<ustore_key_t const> const start_keys {arguments.start_keys, arguments.start_keys_stride};
    strided_iterator<ustore_key_t const> const end_keys {arguments.end_keys, arguments.end_keys_stride};
    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;
    std::size_t const count = arguments.tasks_count;
    measure_columns_t columns {
        arena.alloc_or_dummy(count, arguments.error, arguments.min_cardinalities),
        arena.alloc_or_dummy(count, arguments.error, arguments.max_cardinalities),
        arena.alloc_or_dummy(count, arguments.error, arguments.min_value_bytes),
        arena.alloc_or_dummy(count, arguments.error, arguments.max_value_bytes),
        arena.alloc_or_dummy(count, arguments.error, arguments.min_space_usages),
        arena.alloc_or_dummy(count, arguments.error, arguments.max_space_usages),
    };
    if (arguments.error->status) return;

    with_source(state, view_of(arguments), arguments.error, [&](auto& source) noexcept {
        store_status_t const walked = for_each_run(
            source, collections, count, arguments.error,
            [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
                using entry_t = entry_of<decltype(entries)>;
                if constexpr (std::is_same_v<entry_t, string_pair_t>)
                    return refused(arguments.error, "The collection holds strings");
                else {
                    auto view = source.view(entries);
                    if (!view) return view.status();
                    for (std::size_t task = first; task != end; ++task) {
                        window_measure_t measured;
                        ustore_key_t const lower = start_keys ? start_keys[task] : smallest_key_k;
                        ustore_key_t const last = end_keys ? end_keys[task] : largest_key_k;
                        store_status_t const measuring = measure_window<entry_t>(*view, lower, last, measured);
                        if (st::failed(measuring)) return measuring;
                        columns.assign(task, measured);
                    }
                    return store_status_t::success_k;
                }
            });
        report(walked, arguments.error);
    });
}

#pragma endregion Blob Calls

#pragma region Collection Calls

/** Implements @c ustore_collection_list as a walk of the catalog, whose entries carry the modalities and the names. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_collection_list_t& arguments) noexcept {
    if (!arguments.count) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need an output for the count"};
        return;
    }
    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;
    catalog_listing_t listing {{arena}, {arena}, {arena}};
    with_source(state, view_of(arguments), arguments.error, [&](auto& source) noexcept {
        report(source.catalog().range_from(ustore_collection_id_t {},
                                           [&](catalog_pair<isolation_> const& pair) noexcept {
                                               return list_entry<isolation_>(listing, pair, arguments.error);
                                           }),
               arguments.error);
    });
    if (arguments.error->status) return;

    *arguments.count = static_cast<ustore_size_t>(listing.ids.size());
    if (arguments.collections) *arguments.collections = listing.ids.begin();
    if (arguments.modalities) *arguments.modalities = listing.modalities.begin();
    if (arguments.names_offsets) *arguments.names_offsets = listing.names.offsets().begin().get();
    if (arguments.names) *arguments.names = reinterpret_cast<ustore_char_t*>(listing.names.contents().begin().get());
}

/** Implements @c ustore_collection_create as one commit of the entry, its fresh store and the name, a named
 *  collection persisted at once as a flushed write would be. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_collection_create_t& arguments) noexcept {
    if (!arguments.collection) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need an output for the collection"};
        return;
    }
    if (arguments.modality > ustore_modality_pages_k) {
        *arguments.error = {ustore_error_invalid_argument_k, "Unknown modality"};
        return;
    }
    std::string_view const name = string_of(arguments.name, arguments.name_length);
    if (arguments.name && name.empty()) {
        *arguments.error = {ustore_error_invalid_argument_k, "A name is never empty, as the main collection has none"};
        return;
    }
    ustore_collection_id_t created {};
    if (!mint_collection_id(created)) {
        *arguments.error = {ustore_error_internal_k, "The system gave no entropy for a collection identifier"};
        return;
    }

    // Two creations of one name both write it, so the later commit conflicts and its retry finds the name taken
    store_status_t const committed = commit_catalog_change(
        state, [&](transaction_source<isolation_>& source, catalog_change<isolation_>& change) noexcept {
            return stage_collection(source, change, created, arguments.modality, name);
        });
    if (committed == store_status_t::key_already_exists_k) {
        *arguments.error = {ustore_error_already_exists_k, "A collection already carries the name"};
        return;
    }
    if (reported_failure(committed, arguments.error)) return;
    *arguments.collection = created;
    if (!name.empty()) persist(state, arguments.error);
}

/** Implements @c ustore_collection_drop: forgetting the collection and retiring its store, giving it a fresh empty
 *  store, or emptying its values. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_collection_drop_t& arguments) noexcept {
    ustore_collection_id_t const collection = arguments.collection;
    ustore_drop_mode_t const mode = arguments.mode;
    if (collection == ustore_collection_main_k && mode == ustore_drop_keys_vals_handle_k) {
        *arguments.error = {ustore_error_invalid_argument_k, "The main collection can only be cleared"};
        return;
    }
    store_status_t dropped = store_status_t::success_k;
    switch (mode) {
    case ustore_drop_keys_vals_handle_k:
        dropped = commit_catalog_change(
            state, [&](transaction_source<isolation_>& source, catalog_change<isolation_>& change) noexcept {
                return stage_forget(source, change, collection);
            });
        break;
    case ustore_drop_keys_vals_k:
        dropped = commit_catalog_change(
            state, [&](transaction_source<isolation_>& source, catalog_change<isolation_>& change) noexcept {
                return stage_emptied(source, change, collection);
            });
        break;
    case ustore_drop_vals_k:
        dropped = commit_group(state, [&](transaction_source<isolation_>& source) noexcept {
            return stage_values_emptied(source, collection, arguments.error);
        });
        break;
    default: *arguments.error = {ustore_error_invalid_argument_k, "Unknown drop mode"}; return;
    }
    if (dropped == store_status_t::key_not_found_k && !arguments.error->status) {
        *arguments.error = {ustore_error_not_found_k, "No collection has such identifier"};
        return;
    }
    report(dropped, arguments.error);
    if (!arguments.error->status && mode == ustore_drop_keys_vals_handle_k) persist(state, arguments.error);
}

/** Implements @c ustore_collection_find, reading the names and the catalog at the newest stamp or at a snapshot. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_collection_find_t& arguments) noexcept {
    if (!arguments.tasks_count) return;
    if (!arguments.names || !arguments.collections) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need the names and an output for the identifiers"};
        return;
    }
    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;
    std::size_t const count = arguments.tasks_count;
    strings_arg_t const names {{arguments.names, arguments.names_stride},
                               {arguments.names_lengths, arguments.names_lengths_stride}};
    std::span<ustore_collection_id_t> const ids = arena.alloc<ustore_collection_id_t>(count, arguments.error);
    auto modalities = arena.alloc_or_dummy(count, arguments.error, arguments.modalities);
    auto presences = arena.alloc_or_dummy(count, arguments.error, arguments.presences);
    if (arguments.error->status) return;

    view_request_t const request {nullptr, arguments.snapshot, arguments.options};
    with_source(state, request, arguments.error, [&](auto& source) noexcept {
        auto named = source.view(state.collection_ids_by_name);
        if (!named) return report(named.status(), arguments.error);
        auto catalog = source.view(state.collections_by_id);
        if (!catalog) return report(catalog.status(), arguments.error);
        for (std::size_t index = 0; index != count; ++index) {
            auto [key, key_status] = string_key_from(names[index]);
            if (reported_failure(key_status, arguments.error)) return;
            ustore_collection_id_t id = ustore_collection_main_k;
            bool present = false;
            store_status_t const looked_up = named->find(
                key, [&](name_pair_t const& pair) noexcept { id = pair.mapped; }, st::no_op_t {});
            if (reported_failure(looked_up, arguments.error)) return;
            ustore_modality_t modality = ustore_modality_any_k;
            if (id != ustore_collection_main_k) {
                store_status_t const listed = catalog->find(
                    id,
                    [&](catalog_pair<isolation_> const& pair) noexcept {
                        present = true;
                        modality = pair.mapped.modality;
                    },
                    st::no_op_t {});
                if (reported_failure(listed, arguments.error)) return;
            }
            ids[index] = present ? id : ustore_collection_main_k;
            modalities[index] = modality;
            presences[index] = present;
        }
    });
    if (!arguments.error->status) *arguments.collections = ids.data();
}

#pragma endregion Collection Calls

#pragma region Strings Calls

/** Stages every run of strings into the collection it names through @p source, refusing one that holds no strings. */
template <typename source_type_>
store_status_t stage_string_runs(source_type_& source, strided_iterator<ustore_collection_id_t const> collections,
                                 strings_arg_t const& texts, contents_arg_t const& contents, std::size_t count,
                                 watch_t watch, ustore_error_t* error) noexcept {
    return for_each_run(source, collections, count, error,
                        [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
                            using entry_t = entry_of<decltype(entries)>;
                            if constexpr (!std::is_same_v<entry_t, string_pair_t>)
                                return refused(error, "The collection holds no strings");
                            else {
                                auto part = source.part(entries);
                                if (!part) return part.status();
                                return stage_strings(**part, texts, contents, first, end, watch);
                            }
                        });
}

/** Implements @c ustore_strings_write, committing a batch outside a transaction atomically. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_strings_write_t& arguments) noexcept {
    if (!arguments.tasks_count) return;
    if (!arguments.strings) {
        *arguments.error = {ustore_error_invalid_argument_k, "No strings were provided"};
        return;
    }
    if (arguments.transaction && (arguments.options & ustore_option_write_flush_k)) {
        *arguments.error = {ustore_error_invalid_argument_k, "A transaction flushes at commit, not at a write"};
        return;
    }
    std::size_t const count = arguments.tasks_count;
    strided_iterator<ustore_collection_id_t const> const collections {arguments.collections,
                                                                      arguments.collections_stride};
    strings_arg_t const texts {{arguments.strings, arguments.strings_stride},
                               {arguments.strings_lengths, arguments.strings_lengths_stride}};
    contents_arg_t const contents {bits_view_t {arguments.values_presences},
                                   {arguments.values_offsets, arguments.values_offsets_stride},
                                   {arguments.values_lengths, arguments.values_lengths_stride},
                                   {arguments.values, arguments.values_stride},
                                   count};

    if (arguments.transaction) {
        transaction_handle<isolation_>* const handle = transaction_of<isolation_>(arguments.transaction,
                                                                                  arguments.error);
        if (!handle) return;
        transaction_source<isolation_> source {state, handle->transaction,
                                               watches_reads<isolation_>(arguments.options)};
        report(stage_string_runs(source, collections, texts, contents, count,
                                 erase_watch<isolation_>(arguments.options), arguments.error),
               arguments.error);
        return;
    }
    store_status_t const written = commit_group(state, [&](transaction_source<isolation_>& source) noexcept {
        return stage_string_runs(source, collections, texts, contents, count, watch_t::dont_watch_k, arguments.error);
    });
    report(written, arguments.error);
    if (!arguments.error->status && (arguments.options & ustore_option_write_flush_k)) persist(state, arguments.error);
}

/** Implements @c ustore_strings_read, copying every value into the caller's arena once. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_strings_read_t& arguments) noexcept {
    if (!arguments.tasks_count) return;
    if (!arguments.strings) {
        *arguments.error = {ustore_error_invalid_argument_k, "No strings were provided"};
        return;
    }
    strided_iterator<ustore_collection_id_t const> const collections {arguments.collections,
                                                                      arguments.collections_stride};
    strings_arg_t const texts {{arguments.strings, arguments.strings_stride},
                               {arguments.strings_lengths, arguments.strings_lengths_stride}};
    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;
    value_tape tape {arena, arguments};
    if (arguments.error->status) return;

    with_source(state, view_of(arguments), arguments.error, [&](auto& source) noexcept {
        store_status_t const read = for_each_run(
            source, collections, arguments.tasks_count, arguments.error,
            [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
                using entry_t = entry_of<decltype(entries)>;
                if constexpr (!std::is_same_v<entry_t, string_pair_t>)
                    return refused(arguments.error, "The collection holds no strings");
                else {
                    auto view = source.view(entries);
                    if (!view) return view.status();
                    return read_strings(*view, texts, first, end, tape, arguments.error);
                }
            });
        report(read, arguments.error);
    });
    if (arguments.error->status) return;
    tape.finish();
}

/** Implements @c ustore_strings_scan as ordered walks of each collection's strings. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_strings_scan_t& arguments) noexcept {
    if (!arguments.tasks_count) return;
    if (!arguments.count_limits || !arguments.strings) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need the limits and an output for the strings"};
        return;
    }
    std::size_t const count = arguments.tasks_count;
    strided_iterator<ustore_collection_id_t const> const collections {arguments.collections,
                                                                      arguments.collections_stride};
    strings_arg_t const prefixes {{arguments.prefixes, arguments.prefixes_stride},
                                  {arguments.prefixes_lengths, arguments.prefixes_lengths_stride}};
    strings_arg_t const starts {{arguments.start_strings, arguments.start_strings_stride},
                                {arguments.start_strings_lengths, arguments.start_strings_lengths_stride}};
    strided_iterator<ustore_length_t const> const limits {arguments.count_limits, arguments.count_limits_stride};
    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;
    auto offsets = arena.alloc_or_dummy(count + 1, arguments.error, arguments.offsets);
    auto counts = arena.alloc_or_dummy(count, arguments.error, arguments.counts);
    if (arguments.error->status) return;

    growing_tape_t found {arena};
    with_source(state, view_of(arguments), arguments.error, [&](auto& source) noexcept {
        store_status_t const walked = for_each_run(
            source, collections, count, arguments.error,
            [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
                using entry_t = entry_of<decltype(entries)>;
                if constexpr (!std::is_same_v<entry_t, string_pair_t>)
                    return refused(arguments.error, "The collection holds no strings");
                else {
                    auto view = source.view(entries);
                    if (!view) return view.status();
                    for (std::size_t task = first; task != end; ++task) {
                        std::string_view const prefix = arguments.prefixes ? prefixes[task] : std::string_view {};
                        std::string_view const start = arguments.start_strings ? starts[task] : std::string_view {};
                        std::size_t const found_before = found.lengths().size();
                        offsets[task] = static_cast<ustore_length_t>(found_before);
                        store_status_t const scanned = scan_strings(*view, prefix, start, limits[task], found,
                                                                    arguments.error);
                        if (st::failed(scanned) || arguments.error->status) return scanned;
                        counts[task] = static_cast<ustore_length_t>(found.lengths().size() - found_before);
                    }
                    return store_status_t::success_k;
                }
            });
        report(walked, arguments.error);
    });
    if (arguments.error->status) return;
    offsets[count] = static_cast<ustore_length_t>(found.lengths().size());
    if (arguments.strings_offsets) *arguments.strings_offsets = found.offsets().begin().get();
    *arguments.strings = reinterpret_cast<ustore_char_t*>(found.contents().begin().get());
}

#pragma endregion Strings Calls

#pragma region Graph Calls

/** Implements @c ustore_graph_find_edges as walks of each vertex's half-edges in the roles it asks for. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_graph_find_edges_t& arguments) noexcept {
    if (!arguments.tasks_count) return;
    if (!arguments.vertices) {
        *arguments.error = {ustore_error_invalid_argument_k, "No vertices were provided"};
        return;
    }
    std::size_t const count = arguments.tasks_count;
    strided_iterator<ustore_collection_id_t const> const collections {arguments.collections,
                                                                      arguments.collections_stride};
    find_edges_t const tasks {collections,
                              {arguments.vertices, arguments.vertices_stride},
                              {arguments.roles, arguments.roles_stride},
                              count};
    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;
    auto degrees = arena.alloc_or_dummy(count, arguments.error, arguments.degrees_per_vertex);
    if (arguments.error->status) return;
    uninitialized_array<ustore_key_t> edges {arena};
    uninitialized_array<half_edge_t> walked {arena};

    with_source(state, view_of(arguments), arguments.error, [&](auto& source) noexcept {
        store_status_t const exported = for_each_run(
            source, collections, count, arguments.error,
            [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
                using entry_t = entry_of<decltype(entries)>;
                if constexpr (!std::is_same_v<entry_t, half_edge_t>)
                    return refused(arguments.error, "The collection holds no graph");
                else {
                    auto view = source.view(entries);
                    if (!view) return view.status();
                    return export_edges(*view, tasks, first, end, degrees,
                                        arguments.edges_per_vertex ? &edges : nullptr, walked, arguments.error);
                }
            });
        report(exported, arguments.error);
    });
    if (!arguments.error->status && arguments.edges_per_vertex) *arguments.edges_per_vertex = edges.begin();
}

/** Stages @p stage_runs into the transaction @p transaction names, or commits it as a transaction of its own, then
 *  flushes when @p options ask, for every graph call that writes. */
template <isolation_t isolation_, typename stage_runs_type_>
void serve_graph_write(database_state<isolation_>& state, ustore_transaction_t transaction, ustore_options_t options,
                       ustore_error_t* error, stage_runs_type_&& stage_runs) noexcept {
    if (transaction && (options & ustore_option_write_flush_k)) {
        *error = {ustore_error_invalid_argument_k, "A transaction flushes at commit, not at a write"};
        return;
    }
    if (transaction) {
        transaction_handle<isolation_>* const handle = transaction_of<isolation_>(transaction, error);
        if (!handle) return;
        transaction_source<isolation_> source {state, handle->transaction, watches_reads<isolation_>(options)};
        return report(stage_runs(source), error);
    }
    report(commit_group(state, [&](transaction_source<isolation_>& source) noexcept { return stage_runs(source); }),
           error);
    if (!error->status && (options & ustore_option_write_flush_k)) persist(state, error);
}

/** Hands @p stage every run of @p count tasks on a graph collection through @p source, with the part of the
 *  transaction over its half-edges, refusing a collection that holds no graph. */
template <typename source_type_, typename stage_type_>
store_status_t stage_graph_runs(source_type_& source, strided_iterator<ustore_collection_id_t const> collections,
                                std::size_t count, ustore_error_t* error, stage_type_&& stage) noexcept {
    return for_each_run(source, collections, count, error,
                        [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
                            using entry_t = entry_of<decltype(entries)>;
                            if constexpr (!std::is_same_v<entry_t, half_edge_t>)
                                return refused(error, "The collection holds no graph");
                            else {
                                auto part = source.part(entries);
                                if (!part) return part.status();
                                return stage(**part, first, end);
                            }
                        });
}

/** Implements @c ustore_graph_upsert_edges, filing every edge under both of its ends. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_graph_upsert_edges_t& arguments) noexcept {
    if (!arguments.tasks_count) return;
    if (!arguments.sources_ids || !arguments.targets_ids) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need the sources and the targets of the edges"};
        return;
    }
    strided_iterator<ustore_collection_id_t const> const collections {arguments.collections,
                                                                      arguments.collections_stride};
    edge_columns_t const columns {{arguments.edges_ids, arguments.edges_stride},
                                  {arguments.sources_ids, arguments.sources_stride},
                                  {arguments.targets_ids, arguments.targets_stride}};
    serve_graph_write(state, arguments.transaction, arguments.options, arguments.error, [&](auto& source) noexcept {
        return stage_graph_runs(source, collections, arguments.tasks_count, arguments.error,
                                [&](auto& part, std::size_t first, std::size_t end) noexcept {
                                    return stage_edges(part, columns, first, end);
                                });
    });
}

/** Implements @c ustore_graph_remove_edges, erasing every edge under both of its ends and keeping the vertices. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_graph_remove_edges_t& arguments) noexcept {
    if (!arguments.tasks_count) return;
    if (!arguments.sources_ids || !arguments.targets_ids) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need the sources and the targets of the edges"};
        return;
    }
    strided_iterator<ustore_collection_id_t const> const collections {arguments.collections,
                                                                      arguments.collections_stride};
    edge_columns_t const columns {{arguments.edges_ids, arguments.edges_stride},
                                  {arguments.sources_ids, arguments.sources_stride},
                                  {arguments.targets_ids, arguments.targets_stride}};
    serve_graph_write(state, arguments.transaction, arguments.options, arguments.error, [&](auto& source) noexcept {
        return stage_graph_runs(source, collections, arguments.tasks_count, arguments.error,
                                [&](auto& part, std::size_t first, std::size_t end) noexcept {
                                    return stage_edges_removal(part, columns, first, end);
                                });
    });
}

/** Implements @c ustore_graph_upsert_vertices, adding every vertex that is not one already. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_graph_upsert_vertices_t& arguments) noexcept {
    if (!arguments.tasks_count) return;
    if (!arguments.vertices) {
        *arguments.error = {ustore_error_invalid_argument_k, "No vertices were provided"};
        return;
    }
    strided_iterator<ustore_collection_id_t const> const collections {arguments.collections,
                                                                      arguments.collections_stride};
    strided_iterator<ustore_key_t const> const vertices {arguments.vertices, arguments.vertices_stride};
    serve_graph_write(state, arguments.transaction, arguments.options, arguments.error, [&](auto& source) noexcept {
        return stage_graph_runs(source, collections, arguments.tasks_count, arguments.error,
                                [&](auto& part, std::size_t first, std::size_t end) noexcept {
                                    for (std::size_t index = first; index != end; ++index) {
                                        store_status_t const staged = stage_vertex(part, vertices[index]);
                                        if (st::failed(staged)) return staged;
                                    }
                                    return store_status_t::success_k;
                                });
    });
}

/** Implements @c ustore_graph_remove_vertices, erasing the edges every neighbor files of each vertex in its role,
 *  then every entry of the vertex. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_graph_remove_vertices_t& arguments) noexcept {
    if (!arguments.tasks_count) return;
    if (!arguments.vertices) {
        *arguments.error = {ustore_error_invalid_argument_k, "No vertices were provided"};
        return;
    }
    strided_iterator<ustore_collection_id_t const> const collections {arguments.collections,
                                                                      arguments.collections_stride};
    strided_iterator<ustore_key_t const> const vertices {arguments.vertices, arguments.vertices_stride};
    strided_iterator<ustore_vertex_role_t const> const roles {arguments.roles, arguments.roles_stride};
    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;
    uninitialized_array<half_edge_t> walked {arena};
    serve_graph_write(state, arguments.transaction, arguments.options, arguments.error, [&](auto& source) noexcept {
        return stage_graph_runs(source, collections, arguments.tasks_count, arguments.error,
                                [&](auto& part, std::size_t first, std::size_t end) noexcept {
                                    for (std::size_t index = first; index != end; ++index) {
                                        ustore_vertex_role_t const role = roles ? roles[index]
                                                                                : ustore_vertex_role_any_k;
                                        store_status_t const removed = stage_vertex_removal(part, vertices[index], role,
                                                                                            walked, arguments.error);
                                        if (st::failed(removed) || arguments.error->status) return removed;
                                    }
                                    return store_status_t::success_k;
                                });
    });
}

#pragma endregion Graph Calls

#pragma region Transaction Calls

/** Opens a transaction handle for @p arguments. */
template <isolation_t isolation_>
void open_transaction(database_state<isolation_>& state, ustore_transaction_init_t& arguments) noexcept {
    auto opened = collections_transaction<isolation_>::make(state.order);
    if (reported_failure(opened.status(), arguments.error)) return;
    auto* const handle = new (std::nothrow) transaction_handle<isolation_>(state, std::move(*opened));
    if (!handle) {
        *arguments.error = {ustore_error_out_of_memory_k, "Failed to allocate the transaction"};
        return;
    }
    *arguments.transaction = c_handle<ustore_transaction_t>(handle);
}

/** Resets @p transaction onto a fresh snapshot for reuse. */
template <isolation_t isolation_>
void reset_transaction(ustore_transaction_t transaction, ustore_error_t* error) noexcept {
    transaction_handle<isolation_>* const handle = transaction_of<isolation_>(transaction, error);
    if (!handle) return;
    handle->staging = staging_t::pending_k;
    reported_failure(handle->transaction.reset(), error);
    free_unreachable(handle->state);
}

/** Implements @c ustore_transaction_init. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_transaction_init_t& arguments) noexcept {
    validate_transaction_begin(arguments.transaction, arguments.options, arguments.error);
    if (arguments.error->status) return;
    if (*arguments.transaction) return reset_transaction<isolation_>(*arguments.transaction, arguments.error);
    open_transaction(state, arguments);
}

/** Stages @p handle unless it already is, answering whether its writes sit staged. */
template <isolation_t isolation_>
bool staged(transaction_handle<isolation_>& handle, ustore_error_t* error) noexcept {
    if (handle.staging == staging_t::staged_k) return true;
    if (reported_failure(handle.transaction.stage(), error)) return false;
    handle.staging = staging_t::staged_k;
    return true;
}

/** Implements @c ustore_transaction_stage. */
template <isolation_t isolation_>
void serve(database_state<isolation_>&, ustore_transaction_stage_t& arguments) noexcept {
    if (!arguments.transaction) {
        *arguments.error = {ustore_error_uninitialized_k, "Transaction is uninitialized"};
        return;
    }
    transaction_handle<isolation_>* const handle = transaction_of<isolation_>(arguments.transaction, arguments.error);
    if (!handle || !staged(*handle, arguments.error)) return;
    if (arguments.stamp) *arguments.stamp = handle->transaction.generation();
}

/** Implements @c ustore_transaction_commit, staging first when the caller did not. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_transaction_commit_t& arguments) noexcept {
    validate_transaction_commit(arguments.transaction, arguments.options, arguments.error);
    if (arguments.error->status) return;
    transaction_handle<isolation_>* const handle = transaction_of<isolation_>(arguments.transaction, arguments.error);
    if (!handle || !staged(*handle, arguments.error)) return;
    if (reported_failure(handle->transaction.commit(), arguments.error)) return;
    handle->staging = staging_t::pending_k;
    if (arguments.stamp) *arguments.stamp = handle->transaction.commit_stamp();
    free_unreachable(state);
    if (arguments.options & ustore_option_write_flush_k) persist(state, arguments.error);
}

#pragma endregion Transaction Calls

#pragma region Dispatch

/** Visits the isolation of the database @p arguments names once, and serves the call at it. */
template <typename arguments_type_>
void serve_database(arguments_type_& arguments) noexcept {
    database_t& database = *handle_cast<database_t>(arguments.db);
    std::visit([&](auto& state) noexcept { serve(state, arguments); }, database.state);
}

/** Allocates a database at @p isolation_ and loads the image of @p directory, or answers null. */
template <isolation_t isolation_>
database_t* make_database(std::string_view directory, ustore_error_t* error) noexcept {
    std::unique_ptr<database_t> database {new (std::nothrow)
                                              database_t(std::in_place_type<database_state<isolation_>>)};
    if (!database) {
        *error = {ustore_error_out_of_memory_k, "Failed to allocate the database"};
        return nullptr;
    }
    database->engine = &engine_k;
    open_database(*std::get_if<database_state<isolation_>>(&database->state), directory, error);
    return error->status ? nullptr : database.release();
}

void database_capabilities(ustore_database_capabilities_t& arguments) noexcept {
    if (!arguments.capabilities) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need an output for the capabilities"};
        return;
    }
    *arguments.capabilities = ustore_capabilities_t(ustore_supports_transactions_k | ustore_supports_collections_k |
                                                    ustore_supports_snapshots_k);
}

/** Frees @p handle, releasing the stamp its transaction claimed, and the stores only it still reached. */
template <isolation_t isolation_>
void close_transaction(transaction_handle<isolation_>* handle) noexcept {
    database_state<isolation_>& state = handle->state;
    delete handle;
    free_unreachable(state);
}

void transaction_free(ustore_transaction_t transaction) noexcept {
    auto* const header = handle_cast<isolated_handle_t>(transaction);
    if (header->isolation == isolation_t::snapshot_k)
        return close_transaction(static_cast<transaction_handle<isolation_t::snapshot_k>*>(header));
    close_transaction(static_cast<transaction_handle<isolation_t::strict_serializable_k>*>(header));
}

/** Gives back the count @p handle holds and frees it, releasing the stamp it claimed, and the stores only it still
 *  reached. */
template <isolation_t isolation_>
void close_snapshot(snapshot_handle<isolation_>* handle) noexcept {
    database_state<isolation_>& state = handle->state;
    fu::standard_atomic_ref<std::size_t>(state.open_snapshots).fetch_sub(1, st::memory_order_release_k);
    delete handle;
    free_unreachable(state);
}

void snapshot_free(ustore_snapshot_t snapshot) noexcept {
    auto* const header = handle_cast<isolated_handle_t>(snapshot);
    if (header->isolation == isolation_t::snapshot_k)
        return close_snapshot(static_cast<snapshot_handle<isolation_t::snapshot_k>*>(header));
    close_snapshot(static_cast<snapshot_handle<isolation_t::strict_serializable_k>*>(header));
}

void database_free(ustore_database_t database) noexcept {
    std::unique_ptr<database_t> freed {handle_cast<database_t>(database)};
    std::visit([](auto& state) noexcept { close_database(state); }, freed->state);
}

engine_t const engine_k {
    .database_capabilities = &database_capabilities,
    .database_control = &serve_database<ustore_database_control_t>,
    .snapshot_init = &serve_database<ustore_snapshot_init_t>,
    .snapshot_export = &serve_database<ustore_snapshot_export_t>,
    .read = &serve_database<ustore_read_t>,
    .write = &serve_database<ustore_write_t>,
    .scan = &serve_database<ustore_scan_t>,
    .sample = &serve_database<ustore_sample_t>,
    .measure = &serve_database<ustore_measure_t>,
    .collection_create = &serve_database<ustore_collection_create_t>,
    .collection_drop = &serve_database<ustore_collection_drop_t>,
    .collection_list = &serve_database<ustore_collection_list_t>,
    .collection_find = &serve_database<ustore_collection_find_t>,
    .strings_write = &serve_database<ustore_strings_write_t>,
    .strings_read = &serve_database<ustore_strings_read_t>,
    .strings_scan = &serve_database<ustore_strings_scan_t>,
    .graph_find_edges = &serve_database<ustore_graph_find_edges_t>,
    .graph_upsert_edges = &serve_database<ustore_graph_upsert_edges_t>,
    .graph_remove_edges = &serve_database<ustore_graph_remove_edges_t>,
    .graph_upsert_vertices = &serve_database<ustore_graph_upsert_vertices_t>,
    .graph_remove_vertices = &serve_database<ustore_graph_remove_vertices_t>,
    .transaction_init = &serve_database<ustore_transaction_init_t>,
    .transaction_stage = &serve_database<ustore_transaction_stage_t>,
    .transaction_commit = &serve_database<ustore_transaction_commit_t>,
    .transaction_free = &transaction_free,
    .snapshot_free = &snapshot_free,
    .database_free = &database_free,
};

#pragma endregion Dispatch

} // namespace unum::ustore::ram

using namespace unum::ustore;
using namespace unum::ustore::ram;

#pragma region C ABI

void ustore_database_init_ram(ustore_database_init_ram_t* arguments) {
    if (!arguments->db) {
        *arguments->error = {ustore_error_invalid_argument_k, "Need an output for the database"};
        return;
    }
    std::string_view const directory = arguments->directory ? arguments->directory : "";
    if (!directory.empty() && !names_directory(arguments->directory)) {
        *arguments->error = {ustore_error_not_found_k, "The directory doesn't exist"};
        return;
    }
    database_t* database = nullptr;
    switch (arguments->isolation) {
    case ustore_isolation_strict_serializable_k:
        database = make_database<isolation_t::strict_serializable_k>(directory, arguments->error);
        break;
    case ustore_isolation_snapshot_k:
        database = make_database<isolation_t::snapshot_k>(directory, arguments->error);
        break;
    default: *arguments->error = {ustore_error_invalid_argument_k, "Unknown isolation"}; return;
    }
    *arguments->db = c_handle<ustore_database_t>(database);
}
