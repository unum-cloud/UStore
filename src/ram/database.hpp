/**
 *  @file src/ram/database.hpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief The state, handles, views, collections, image, transactions and snapshots of the in-memory engine, which its
 *      calls of every modality build on.
 */

#pragma once

#include <cerrno>  // `errno`, `ENOENT`
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint64_t`
#include <cstdio>  // `std::FILE`, `std::fopen`, `std::fread`, `std::fwrite`, `std::rename`
#include <cstring> // `std::memcmp`

#include <algorithm>   // `std::partition_point`
#include <array>       // `std::array`
#include <new>         // `std::nothrow`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::is_same_v`, `std::remove_cvref_t`
#include <utility>     // `std::exchange`, `std::move`
#include <variant>     // `std::in_place_type`, `std::variant`, `std::visit`

#include "ustore/types.h" // `USTORE_OS_WINDOWS_`

#if USTORE_OS_WINDOWS_
#include <io.h>      // `_commit`, `_fileno`
#include <windows.h> // `MoveFileExA`
#else
#include <unistd.h> // `fsync`
#endif

#include <smashtable/basic_avl_tree.hpp>
#include <smashtable/basic_vector.hpp>
#include <stringzilla/stringzilla.hpp>

#include "ustore/blobs.h"
#include "ustore/db.h"
#include "ustore/ranges.hpp" // `place_batch_t`, `strided`
#include "ustore/strings.h"

#include "ustore/blobs/ram_store.hpp"          // `ordered_store`, `values_by_key_store`, `order_t`, `translate`
#include "ustore/docs/ram_store.hpp"           // `docs::document_pair_t`, `documents_by_key_store`
#include "ustore/graph/half_edges.hpp"         // `half_edge_t`, `half_edge_direction_t`
#include "ustore/graph/ram_store.hpp"          // `half_edges_store`
#include "ustore/pages/ram_store.hpp"          // `pages::page_pair_t`, `pages_by_key_store`
#include "ustore/strings/ram_store.hpp"        // `string_pair_t`, `keys_by_string_store`, `string_key_from`
#include "ustore/strings/string_keys.hpp"      // `string_key_t`, `blob_t`
#include "ustore/utilities/blocking_files.hpp" // `directory_exists`

#include "engine.hpp"             // `engine_t`, `ustore_*_handle_t`, `state_of`, `view_t`, `strings_kit_of`
#include "lib.hpp"                // `cpu_class_t`, `atomic_ref_for_cpu_class`, `atomic_ref_of`, `waiter_of`
#include "utilities/batches.hpp"  // `scan_batch_t`, `strings_arg_t`, `places_of`, `scans_of`, `measures_of`
#include "utilities/outputs.hpp"  // `read_outputs_t`, `strings_read_outputs_t`, `scan_outputs_t`, `measure_outputs_t`
#include "utilities/progress.hpp" // `progress_t`, `progress_cancelled_k`

namespace unum::ustore::ram {
using namespace unum::ustore::blobs;
using namespace unum::ustore::graph;
using namespace unum::ustore::strings;
using namespace unum::ustore::utilities;

#pragma region Stores

template <isolation_t isolation_, template <typename> typename atomic_reference_, typename waiter_type_,
          strings_kit_concept strings_kit_type_>
struct collection_store;

/** What the collections system collection records of a collection: its name, and the store its
 *  entries live in, whose kind says what it holds. */
template <isolation_t isolation_, template <typename> typename atomic_reference_, typename waiter_type_,
          strings_kit_concept strings_kit_type_>
struct collection_entry {

    /** The name of the collection, empty for one created without. */
    blob_t name;

    /** The store holding the collection's entries. */
    collection_store<isolation_, atomic_reference_, waiter_type_, strings_kit_type_>* store {nullptr};
};

/** A collection's UUID mapped onto what the collections system collection records of it. */
template <isolation_t isolation_, template <typename> typename atomic_reference_, typename waiter_type_,
          strings_kit_concept strings_kit_type_>
using collections_pair = st::mapping<ustore_collection_id_t,
                                     collection_entry<isolation_, atomic_reference_, waiter_type_, strings_kit_type_>>;

/** The collections system collection: every collection's UUID mapped onto its entry. */
template <isolation_t isolation_, template <typename> typename atomic_reference_, typename waiter_type_,
          strings_kit_concept strings_kit_type_>
using collections_by_id_store =
    ordered_store<collections_pair<isolation_, atomic_reference_, waiter_type_, strings_kit_type_>, uuid_order_t,
                  uuid_hash_t, isolation_, atomic_reference_, waiter_type_>;

/** The store of one collection, of the kind its creator picked, which a drop retires
 *  rather than frees. */
template <isolation_t isolation_, template <typename> typename atomic_reference_, typename waiter_type_,
          strings_kit_concept strings_kit_type_>
struct collection_store {

    /** The collection's entries. */
    std::variant<values_by_key_store<isolation_, atomic_reference_, waiter_type_>,
                 strings::keys_by_string_store<isolation_, atomic_reference_, waiter_type_, strings_kit_type_>,
                 graph::half_edges_store<isolation_, atomic_reference_, waiter_type_>,
                 docs::documents_by_key_store<isolation_, atomic_reference_, waiter_type_>,
                 pages::pages_by_key_store<isolation_, atomic_reference_, waiter_type_>>
        entries;

    /** The store retired before this one, while this one waits to be freed. */
    collection_store* next_retired {nullptr};

    /** The stamp the change that took this store out of the collections system
     *  collection published under. */
    generation_t retirement_stamp {0};

    template <typename kind_type_>
    collection_store(std::in_place_type_t<kind_type_> kind, order_t& order) noexcept : entries(kind, order) {}

    /** Allocates the store of a collection keeping its entries in a @p kind_type_ store,
     *  or answers null. */
    template <typename kind_type_>
    static collection_store* allocate(order_t& order) noexcept {
        return new (std::nothrow) collection_store(std::in_place_type<kind_type_>, order);
    }
};

/** A transaction over both system collections and every collection store it reaches, published
 *  under one stamp. */
template <isolation_t isolation_, template <typename> typename atomic_reference_, typename waiter_type_,
          strings_kit_concept strings_kit_type_>
using collections_transaction = st::dynamic_transaction_group<
    collections_by_id_store<isolation_, atomic_reference_, waiter_type_, strings_kit_type_>,
    values_by_key_store<isolation_, atomic_reference_, waiter_type_>,
    strings::keys_by_string_store<isolation_, atomic_reference_, waiter_type_, strings_kit_type_>,
    graph::half_edges_store<isolation_, atomic_reference_, waiter_type_>,
    docs::documents_by_key_store<isolation_, atomic_reference_, waiter_type_>,
    pages::pages_by_key_store<isolation_, atomic_reference_, waiter_type_>>;

/** The store of one collection at @p isolation_, whose partition locks own their words through the
 *  atomic reference of @p cpu_class_ and wait through its waiter. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
using collection_store_of = collection_store<isolation_, atomic_ref_for_cpu_class<cpu_class_>::template type,
                                             waiter_of<cpu_class_>, strings_kit_of<cpu_class_>>;

/** What the collections system collection of @p cpu_class_ at @p isolation_ records of a collection. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
using collection_entry_of = collection_entry<isolation_, atomic_ref_for_cpu_class<cpu_class_>::template type,
                                             waiter_of<cpu_class_>, strings_kit_of<cpu_class_>>;

/** A collection's UUID mapped onto its entry, on @p cpu_class_ at @p isolation_. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
using collections_pair_of = collections_pair<isolation_, atomic_ref_for_cpu_class<cpu_class_>::template type,
                                             waiter_of<cpu_class_>, strings_kit_of<cpu_class_>>;

/** The collections system collection on @p cpu_class_ at @p isolation_. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
using collections_by_id_store_of =
    collections_by_id_store<isolation_, atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>,
                            strings_kit_of<cpu_class_>>;

/** The names system collection on @p cpu_class_ at @p isolation_. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
using keys_by_string_store_of =
    strings::keys_by_string_store<isolation_, atomic_ref_for_cpu_class<cpu_class_>::template type,
                                  waiter_of<cpu_class_>, strings_kit_of<cpu_class_>>;

/** A transaction over every store of a database on @p cpu_class_ at @p isolation_. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
using collections_transaction_of =
    collections_transaction<isolation_, atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>,
                            strings_kit_of<cpu_class_>>;

/** Allocates the store a collection holding @p modality keeps its entries in, or answers null. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
collection_store_of<cpu_class_, isolation_>* allocate_store(order_t& order, ustore_modality_t modality) noexcept {
    using store_t = collection_store_of<cpu_class_, isolation_>;
    switch (modality) {
    case ustore_modality_strings_k:
        return store_t::template allocate<keys_by_string_store_of<cpu_class_, isolation_>>(order);
    case ustore_modality_graph_k:
        return store_t::template allocate<graph::half_edges_store<
            isolation_, atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>>>(order);
    case ustore_modality_docs_k:
        return store_t::template allocate<docs::documents_by_key_store<
            isolation_, atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>>>(order);
    case ustore_modality_pages_k:
        return store_t::template allocate<pages::pages_by_key_store<
            isolation_, atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>>>(order);
    default:
        return store_t::template allocate<values_by_key_store<
            isolation_, atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>>>(order);
    }
}

/** The modality of a collection whose entries live in @p store, which picked the kind of that store. */
template <typename collection_store_type_>
ustore_modality_t modality_of(collection_store_type_ const& store) noexcept {
    return std::visit(
        []<typename kind_type_>(kind_type_ const&) noexcept {
            using entry_t = typename kind_type_::value_t;
            if constexpr (std::is_same_v<entry_t, string_pair_t>) return ustore_modality_strings_k;
            else if constexpr (std::is_same_v<entry_t, half_edge_t>) return ustore_modality_graph_k;
            else if constexpr (std::is_same_v<entry_t, docs::document_pair_t>) return ustore_modality_docs_k;
            else if constexpr (std::is_same_v<entry_t, pages::page_pair_t>) return ustore_modality_pages_k;
            else return ustore_modality_blobs_k;
        },
        store.entries);
}

#pragma endregion Stores

#pragma region Files

/** Whether @p path names an existing directory. */
inline bool names_directory(char const* path) noexcept {
    expected<bool> const exists = directory_exists(path);
    return exists && *exists;
}

#pragma endregion Files

#pragma region Handles

/** The RAM engine of one CPU class and isolation level, whose stores own their words through the
 *  class's atomic reference and wait through its waiter. Each modality's table is defined beside
 *  the calls it names, and the database and engine tables in <tt>src/ram/variants.hpp</tt>, so the
 *  explicit instantiation in <tt>src/ram/variants_&lt;class&gt;.cpp</tt> compiles every call there. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
struct ram_variant {

    /** The StringZilla kernels this class runs its string work through. */
    using strings_kit_t = strings_kit_of<cpu_class_>;

    static_assert(collections_by_id_store_of<cpu_class_, isolation_>::isolation_k == isolation_,
                  "the partitions must keep the isolation of their stores");

    /** The database verbs of this class and isolation. */
    static database_verbs_t const database_verbs_k;

    /** The verbs of this class's blobs collections. */
    static blobs_verbs<home_t::host_k> const blobs_verbs_k;

    /** The verbs of this class's strings collections. */
    static strings_verbs<home_t::host_k> const strings_verbs_k;

    /** The verbs of this class's documents collections. */
    static docs_verbs<home_t::host_k> const docs_verbs_k;

    /** The verbs of this class's graph collections. */
    static graph_verbs<home_t::host_k> const graph_verbs_k;

    /** The verbs of this class's pages collections. */
    static pages_verbs<home_t::host_k> const pages_verbs_k;

    /** The table every handle of a database of this class and isolation is stamped with, so the
     *  router refuses a handle of another. */
    static engine_t const engine_k;

    /** Opens a database of this class and isolation into @p database, persisting in @p directory unless it is empty. */
    static status_t open(ustore_database_t* database, std::string_view directory) noexcept;
};

template <cpu_class_t cpu_class_, isolation_t isolation_>
struct changes_feed_state;

/** Everything a database at @p isolation_ holds, behind its database handle of the C ABI. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
struct database_state {

    /** The order every store commits in. */
    order_t order;

    /** The collections system collection: every collection's UUID mapped onto its entry. */
    collections_by_id_store_of<cpu_class_, isolation_> collections_by_id {order};

    /** The names system collection: every collection's name mapped onto its UUID. */
    keys_by_string_store_of<cpu_class_, isolation_> collection_ids_by_name {order};

    /** Stores the collections system collection no longer lists, linked through
     *  @c collection_store::next_retired and freed once no reader holds a stamp older than the one
     *  they were retired at. */
    collection_store_of<cpu_class_, isolation_>* retired {nullptr};

    /** Snapshots handed out and not yet freed, which a reset refuses to run beside. */
    std::size_t open_snapshots {0};

    using feeds_mutex_t = engine_mutex<atomic_ref_for_cpu_class<cpu_class_>::template type, waiter_of<cpu_class_>>;

    /** Guards @c open_feeds. */
    feeds_mutex_t feeds_mutex;

    /** Feeds handed out and not yet freed, whose claims a feed opened at an older stamp shares. */
    st::vector<changes_feed_state<cpu_class_, isolation_>*> open_feeds;

    /** Directory the image persists in, empty for a volatile database. */
    sz::string_t directory;

    database_state() noexcept = default;
    database_state(database_state const&) = delete;
    database_state& operator=(database_state const&) = delete;
    ~database_state() noexcept;
};

/** What a transaction holds, behind its transaction handle of the C ABI. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
struct transaction_state {

    /** The database the transaction runs against. */
    database_state<cpu_class_, isolation_>& state;

    /** The transaction over every store it reaches. */
    collections_transaction_of<cpu_class_, isolation_> transaction;

    transaction_state(database_state<cpu_class_, isolation_>& owner,
                      collections_transaction_of<cpu_class_, isolation_>&& opened) noexcept
        : state(owner), transaction(std::move(opened)) {}
};

/** What a snapshot holds, behind its snapshot handle of the C ABI, whose claim pins the stamp every
 *  read through it answers at until it is freed. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
struct snapshot_state {

    /** The database the snapshot reads. */
    database_state<cpu_class_, isolation_>& state;

    /** The claim on the newest stamp published when the snapshot opened. */
    claim_t claim;

    explicit snapshot_state(database_state<cpu_class_, isolation_>& owner) noexcept : state(owner) {}
};

#pragma endregion Handles

#pragma region Retirement

/** Retires @p store at @p commit_stamp, which the collections system collection stopped listing it
 *  at, to be freed once no reader holds an older stamp. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
void retire(database_state<cpu_class_, isolation_>& state, collection_store_of<cpu_class_, isolation_>* store,
            generation_t commit_stamp) noexcept {
    store->retirement_stamp = commit_stamp;
    atomic_ref_of<cpu_class_, collection_store_of<cpu_class_, isolation_>*> head(state.retired);
    store->next_retired = head.load(st::memory_order_relaxed_k);
    while (!head.compare_exchange_weak(store->next_retired, store, st::memory_order_release_k,
                                       st::memory_order_relaxed_k)) {}
}

/** Frees every retired store no reader can reach any more, and keeps the others retired. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
void free_unreachable(database_state<cpu_class_, isolation_>& state) noexcept {
    using store_t = collection_store_of<cpu_class_, isolation_>;
    atomic_ref_of<cpu_class_, store_t*> head(state.retired);
    if (!head.load(st::memory_order_relaxed_k)) return;
    store_t* taken = head.exchange(nullptr, st::memory_order_acquire_k);
    // Read after taking the list, so every store taken was retired at or below the stamps it is compared with
    generation_t const oldest_read = state.order.low_water_mark();
    store_t* kept = nullptr;
    store_t* kept_last = nullptr;
    while (taken) {
        store_t* const next = taken->next_retired;
        if (taken->retirement_stamp <= oldest_read) delete taken;
        else {
            taken->next_retired = kept;
            if (!kept) kept_last = taken;
            kept = taken;
        }
        taken = next;
    }
    if (!kept) return;
    kept_last->next_retired = head.load(st::memory_order_relaxed_k);
    while (!head.compare_exchange_weak(kept_last->next_retired, kept, st::memory_order_release_k,
                                       st::memory_order_relaxed_k)) {}
}

template <cpu_class_t cpu_class_, isolation_t isolation_>
database_state<cpu_class_, isolation_>::~database_state() noexcept {
    [[maybe_unused]] store_status_t const walked = collections_by_id.for_each(
        [](collections_pair_of<cpu_class_, isolation_> const& pair) noexcept { delete pair.mapped.store; });
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

    template <typename from_type_, typename until_type_, typename callback_type_>
    store_status_t range(from_type_ const& from, until_type_ const& until, callback_type_&& callback,
                         st::descending_t descending) const noexcept {
        return reader.range(from, until, callback, descending);
    }

    store_status_t prepare(std::size_t) const noexcept { return store_status_t::success_k; }

    template <typename key_type_>
    store_status_t watch(key_type_ const&) const noexcept {
        return store_status_t::success_k;
    }
};

/** Reads through the part a transaction joined a store with, watching what it hands out when a
 *  snapshot-isolated call asks, as a strictly serializable one records every read already. */
template <typename transaction_type_>
struct transaction_view {

    /** The part of the transaction over the store. */
    transaction_type_& transaction;

    /** Whether reads watch what they hand out. */
    ustore_watch_t watch_mode;

    /** Whether @c find and @c watch record what they hand out. */
    bool watches() const noexcept { return watch_mode == ustore_watch_keys_k; }

    template <typename key_type_, typename found_type_, typename missing_type_>
    store_status_t find(key_type_ const& key, found_type_&& found, missing_type_&& missing) const noexcept {
        return watches() ? transaction.find_and_watch(key, found, missing) : transaction.find(key, found, missing);
    }

    template <typename key_type_, typename callback_type_>
    store_status_t range(key_type_ const& lower, key_type_ const& upper, callback_type_&& callback) const noexcept {
        return transaction.range(lower, upper, callback);
    }

    template <typename key_type_, typename callback_type_>
    store_status_t range_from(key_type_ const& lower, callback_type_&& callback) const noexcept {
        return transaction.range_from(lower, callback);
    }

    template <typename from_type_, typename until_type_, typename callback_type_>
    store_status_t range(from_type_ const& from, until_type_ const& until, callback_type_&& callback,
                         st::descending_t descending) const noexcept {
        return transaction.range(from, until, callback, descending);
    }

    /** Hands @p callback every entry partition by partition, merging nothing, as an unordered scan walks. */
    template <typename callback_type_>
    store_status_t for_each(callback_type_&& callback) const noexcept
        requires requires { transaction.for_each(callback); }
    {
        return transaction.for_each(callback);
    }

    store_status_t prepare(std::size_t count) const noexcept { return transaction.reserve(count); }

    template <typename key_type_>
    store_status_t watch(key_type_ const& key) const noexcept {
        return watches() ? transaction.watch(key) : store_status_t::success_k;
    }
};

/** The stores a call outside any transaction reads, each at the one stamp a claim of the call or of
 *  a snapshot pins. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
struct reader_source {

    static constexpr cpu_class_t cpu_class_k = cpu_class_;
    static constexpr isolation_t isolation_k = isolation_;

    /** The database being read. */
    database_state<cpu_class_, isolation_>& state;

    /** The stamp every store is read at. */
    generation_t read_stamp;

    /** A reader of @p store at the stamp. */
    template <typename store_type_>
    st::expected<reader_view<typename store_type_::reader_t>> view(store_type_& store) const noexcept {
        return reader_view<typename store_type_::reader_t> {store.reader_at(read_stamp)};
    }

    /** A reader of the collections system collection at the stamp, which resolves the collections a call names. */
    reader_view<typename collections_by_id_store_of<cpu_class_, isolation_>::reader_t> collections() const noexcept {
        return {state.collections_by_id.reader_at(read_stamp)};
    }
};

/** The stores a transaction reaches, each through the part it joins the transaction with. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
struct transaction_source {

    static constexpr cpu_class_t cpu_class_k = cpu_class_;
    static constexpr isolation_t isolation_k = isolation_;

    /** The database the transaction runs against. */
    database_state<cpu_class_, isolation_>& state;

    /** The transaction every store joins. */
    collections_transaction_of<cpu_class_, isolation_>& transaction;

    /** Whether reads watch what they hand out, and erases watch the absence of keys already gone. */
    ustore_watch_t watch_mode;

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
        return transaction_view<typename store_type_::transaction_t> {**joined, watch_mode};
    }

    /**
     *  @brief A reader of the collections system collection at the newest stamp, which resolves the collections a
     *      call names.
     *
     *  Collections resolve at the head, as in every engine, since the C ABI creates them only there and a transaction
     *  must reach one created after it began. The transaction's own claim is older than the stamp read, so it keeps
     *  every store the read may hand out alive.
     */
    reader_view<typename collections_by_id_store_of<cpu_class_, isolation_>::reader_t> collections() const noexcept {
        return {state.collections_by_id.reader_at(state.order.published_stamp())};
    }
};

/** Whether reads of a transaction at @p isolation_ watch what they hand out when a call asks for
 *  @p watch, where a strictly serializable one records every read already. */
template <isolation_t isolation_>
constexpr ustore_watch_t watches_reads(ustore_watch_t watch) noexcept {
    return isolation_ == isolation_t::snapshot_k && watch == ustore_watch_keys_k ? ustore_watch_keys_k
                                                                                 : ustore_watch_none_k;
}

/** @p source as is, which records no reads. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
reader_source<cpu_class_, isolation_> reading_as(reader_source<cpu_class_, isolation_> const& source,
                                                 ustore_watch_t) noexcept {
    return source;
}

/** @p source watching what it hands out as @p watch asks. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
transaction_source<cpu_class_, isolation_> reading_as(transaction_source<cpu_class_, isolation_> const& source,
                                                      ustore_watch_t watch) noexcept {
    return {source.state, source.transaction, watches_reads<isolation_>(watch)};
}

/** Hands @p action the source @p view names: a snapshot, a transaction, or the newest stamp, claimed for the call. */
template <cpu_class_t cpu_class_, isolation_t isolation_, typename action_type_>
auto with_source(database_state<cpu_class_, isolation_>& state, view_t const& view, action_type_&& action) noexcept {
    if (view.snapshot) {
        reader_source<cpu_class_, isolation_> source {
            state, state_of<snapshot_state<cpu_class_, isolation_>>(view.snapshot)->claim.snapshot()};
        return action(source);
    }
    if (!view.transaction) {
        claim_t claim;
        reader_source<cpu_class_, isolation_> source {state, state.order.take_snapshot(claim)};
        return action(source);
    }
    auto& transaction = state_of<transaction_state<cpu_class_, isolation_>>(view.transaction)->transaction;
    transaction_source<cpu_class_, isolation_> source {state, transaction, watches_reads<isolation_>(view.watch)};
    return action(source);
}

#pragma endregion Views

#pragma region Resolution

/** A collection as a call resolved it: the store its entries live in, whose kind says what it holds. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
struct resolved_collection {
    collection_store_of<cpu_class_, isolation_>* store {nullptr};
};

/** Reads what the collections system collection records of @p collection for @p source, answering
 *  @c key_not_found_k for a collection it does not list. */
template <typename source_type_>
store_status_t resolve(source_type_& source, ustore_collection_id_t const& collection,
                       resolved_collection<source_type_::cpu_class_k, source_type_::isolation_k>& resolved) noexcept {
    auto const collections = source.collections();
    store_status_t listing = store_status_t::key_not_found_k;
    store_status_t const looked_up = collections.find(
        collection,
        [&](collections_pair_of<source_type_::cpu_class_k, source_type_::isolation_k> const& pair) noexcept {
            listing = store_status_t::success_k;
            resolved = {pair.mapped.store};
        },
        st::no_op_t {});
    return st::failed(looked_up) ? looked_up : listing;
}

/**
 *  @brief Hands @p action every run of tasks naming one collection, with the store of that collection.
 *
 *  Resolves each run once through @p source, refusing a collection the collections
 *  system collection does not list, and stops at the first failure @p action answers or
 *  reports. @p action is invoked as <tt>action(entries, first, end)</tt> with the concrete
 *  store and the tasks [ @p first, @p end ) it serves.
 */
template <typename source_type_, typename action_type_>
store_status_t for_each_run(source_type_& source, strided_iterator<ustore_collection_id_t const> collections,
                            std::size_t count, status_t& outcome, action_type_&& action) noexcept {
    for (std::size_t first = 0; first != count;) {
        ustore_collection_id_t const collection = collections[first];
        std::size_t end = first + 1;
        while (end != count && collections[end] == collection) ++end;
        resolved_collection<source_type_::cpu_class_k, source_type_::isolation_k> resolved;
        store_status_t const listed = resolve(source, collection, resolved);
        if (listed == store_status_t::key_not_found_k) {
            outcome = {status_code_t::not_found_k, "No such collection"};
            return listed;
        }
        if (st::failed(listed)) return listed;
        store_status_t const served = std::visit(
            [&](auto& entries) noexcept -> store_status_t { return action(entries, first, end); },
            resolved.store->entries);
        if (st::failed(served)) return served;
        if (outcome.failed()) return store_status_t::invalid_argument_k;
        first = end;
    }
    return store_status_t::success_k;
}

/** The entries a store of @p store_type_ holds. */
template <typename store_type_>
using entry_of = typename std::remove_cvref_t<store_type_>::value_t;

/** Refuses into @p outcome a collection whose store holds entries of another modality than the call serves. */
inline store_status_t refused_modality(status_t& outcome) noexcept {
    outcome = collection_holds_another_modality();
    return store_status_t::invalid_argument_k;
}

/** The resolver of this engine at @p isolation_, which every checked verb and every layer over
 *  blobs runs first, reading the collections system collection as @p view sees it. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t resolve_modality(ustore_database_t database, ustore_collections_view_t collections, ustore_size_t count,
                          ustore_modality_t modality, view_t const& view) noexcept {
    strided_iterator<ustore_collection_id_t const> const tasks = strided(collections);
    status_t outcome;
    database_state<cpu_class_, isolation_>& state = *state_of<database_state<cpu_class_, isolation_>>(database);
    store_status_t const resolved = with_source(state, view, [&](auto& source) noexcept -> store_status_t {
        for (std::size_t index = 0; index != count; ++index) {
            if (index != 0 && tasks[index] == tasks[index - 1]) continue;
            resolved_collection<cpu_class_, isolation_> collection;
            store_status_t const listed = resolve(source, tasks[index], collection);
            if (listed == store_status_t::key_not_found_k) {
                outcome = {status_code_t::not_found_k, "No such collection"};
                return listed;
            }
            if (st::failed(listed)) return listed;
            outcome = admit_modality(modality_of(*collection.store), modality);
            if (outcome.failed()) break;
        }
        return store_status_t::success_k;
    });
    return reported(resolved, outcome);
}

#pragma endregion Resolution

#pragma region Reads

/** Where the entry @p key names sits among entries of @p entry_type_: the pair itself, or the vertex's own entry. */
template <typename entry_type_>
constexpr auto entry_for(ustore_key_t const& key) noexcept {
    if constexpr (std::is_same_v<entry_type_, half_edge_t>) return half_edge_t::vertex_entry(key);
    else return key;
}

/** Whether a walk of a collection's keys hands @p entry out: every pair, but only the vertex entries of a graph. */
template <typename mapped_type_>
constexpr bool is_listed(st::mapping<ustore_key_t, mapped_type_> const&) noexcept {
    return true;
}
constexpr bool is_listed(half_edge_t const& entry) noexcept {
    return entry.direction == half_edge_direction_t::vertex_k;
}

/** The key a listed entry names. */
template <typename mapped_type_>
constexpr ustore_key_t const& listed_key(st::mapping<ustore_key_t, mapped_type_> const& pair) noexcept {
    return pair.key;
}
constexpr ustore_key_t const& listed_key(half_edge_t const& entry) noexcept { return entry.vertex; }

/** The bytes a blob, a document or a page reads as. */
template <typename mapped_type_>
value_view_t value_of(st::mapping<ustore_key_t, mapped_type_> const& pair) noexcept {
    return view_of(blob_of(pair.mapped));
}

/** Reads tasks [ @p first, @p end ) of @p places through @p view of a collection of @p entry_type_ into @p outputs. */
template <typename entry_type_, typename view_type_>
store_status_t read_keys(view_type_ const& view, place_batch_t const& places, std::size_t first, std::size_t end,
                         read_outputs_t& outputs) noexcept {
    store_status_t const reserved = view.prepare(end - first);
    if (st::failed(reserved)) return reserved;
    auto const found = [&](entry_type_ const& entry) noexcept { outputs.append(value_of(entry)); };
    auto const missing = [&]() noexcept { outputs.append_missing(); };
    for (std::size_t index = first; index != end; ++index) {
        store_status_t const looked_up = view.find(entry_for<entry_type_>(places[index].key), found, missing);
        if (st::failed(looked_up)) return looked_up;
    }
    return store_status_t::success_k;
}

/** Watches the entries of the keys @p outputs hold from @p first on, which only a watching view
 *  records, once the walk that found them let go of its partitions. */
template <typename entry_type_, typename view_type_>
store_status_t watch_keys(view_type_ const& view, scan_outputs_t& outputs, std::size_t first) noexcept {
    if (!view.watches()) return store_status_t::success_k;
    for (std::size_t index = first; index != outputs.size(); ++index) {
        store_status_t const watched = view.watch(entry_for<entry_type_>(outputs[index]));
        if (st::failed(watched)) return watched;
    }
    return store_status_t::success_k;
}

/** Appends up to the limit of @p scan keys of a collection of @p entry_type_, smallest first,
 *  halting with @p outcome when the scratch refuses. */
template <typename entry_type_, typename view_type_>
store_status_t scan_ascending(view_type_ const& view, scan_task_t const& scan, scan_outputs_t& outputs,
                              status_t& outcome) noexcept {
    std::size_t const first = outputs.size();
    return view.range_from(
        entry_for<entry_type_>(scan.keys.lower), [&](entry_type_ const& entry) noexcept -> walk_control_t {
            if (!is_listed(entry)) return walk_control_t::resume_k;
            if (scan.keys.upper < listed_key(entry)) return walk_control_t::halt_k;
            outcome = outputs.push(listed_key(entry));
            if (outcome.failed()) return walk_control_t::halt_k;
            return outputs.size() - first == scan.limit ? walk_control_t::halt_k : walk_control_t::resume_k;
        });
}

/** Appends up to the limit of @p scan keys of a collection of @p entry_type_, largest first,
 *  halting with @p outcome when the scratch refuses. */
template <typename entry_type_, typename view_type_>
store_status_t scan_descending(view_type_ const& view, scan_task_t const& scan, scan_outputs_t& outputs,
                               status_t& outcome) noexcept {
    std::size_t const first = outputs.size();
    return view.range(
        entry_for<entry_type_>(scan.keys.upper), st::unbounded,
        [&](entry_type_ const& entry) noexcept -> walk_control_t {
            if (!is_listed(entry)) return walk_control_t::resume_k;
            if (listed_key(entry) < scan.keys.lower) return walk_control_t::halt_k;
            outcome = outputs.push(listed_key(entry));
            if (outcome.failed()) return walk_control_t::halt_k;
            return outputs.size() - first == scan.limit ? walk_control_t::halt_k : walk_control_t::resume_k;
        },
        st::descending);
}

/** Appends up to the limit of @p scan keys of a collection of @p entry_type_ in any order,
 *  partition by partition, halting with @p outcome when the scratch refuses. A view offering no
 *  such walk merges the partitions, which is one order an unordered scan may take. */
template <typename entry_type_, typename view_type_>
store_status_t scan_unordered(view_type_ const& view, scan_task_t const& scan, scan_outputs_t& outputs,
                              status_t& outcome) noexcept {
    std::size_t const first = outputs.size();
    // Partitions hold no order, so a key past the upper bound ends nothing
    auto const take = [&](entry_type_ const& entry) noexcept -> walk_control_t {
        if (!is_listed(entry) || !scan.keys.contains(listed_key(entry))) return walk_control_t::resume_k;
        outcome = outputs.push(listed_key(entry));
        if (outcome.failed()) return walk_control_t::halt_k;
        return outputs.size() - first == scan.limit ? walk_control_t::halt_k : walk_control_t::resume_k;
    };
    if constexpr (requires { view.for_each(take); }) return view.for_each(take);
    else return scan_ascending<entry_type_>(view, scan, outputs, outcome);
}

/** Appends up to the limit of @p scan keys of a collection of @p entry_type_ in its range
 *  and direction, halting with @p outcome when the scratch refuses, and watches them as
 *  the view asks. */
template <typename entry_type_, typename view_type_>
store_status_t scan_window(view_type_ const& view, scan_task_t const& scan, ustore_scan_mode_t mode,
                           scan_outputs_t& outputs, status_t& outcome) noexcept {
    std::size_t const first = outputs.size();
    if (scan.limit == 0) return store_status_t::success_k;
    store_status_t walked = store_status_t::success_k;
    if (mode == ustore_scan_unordered_k) walked = scan_unordered<entry_type_>(view, scan, outputs, outcome);
    else if (scan.direction == ustore_scan_descending_k)
        walked = scan_descending<entry_type_>(view, scan, outputs, outcome);
    else walked = scan_ascending<entry_type_>(view, scan, outputs, outcome);
    if (st::failed(walked) || outcome.failed()) return walked;
    return watch_keys<entry_type_>(view, outputs, first);
}

/** Appends a uniform reservoir of up to the limit of @p sample keys of a collection of
 *  @p entry_type_, halting with @p outcome when the scratch refuses. */
template <typename entry_type_, typename view_type_>
store_status_t sample_window(view_type_ const& view, sample_task_t sample, sample_generator_t& generator,
                             scan_outputs_t& outputs, status_t& outcome) noexcept {
    std::size_t const first = outputs.size();
    if (sample.limit == 0) return store_status_t::success_k;
    std::size_t seen = 0;
    store_status_t const walked = view.range_from(
        entry_for<entry_type_>(smallest_key_k), [&](entry_type_ const& entry) noexcept -> walk_control_t {
            if (!is_listed(entry)) return walk_control_t::resume_k;
            std::size_t const slot = seen < sample.limit ? seen : st::draw_below(generator, seen + 1);
            ++seen;
            if (slot >= sample.limit) return walk_control_t::resume_k;
            if (slot != outputs.size() - first) {
                outputs[first + slot] = listed_key(entry);
                return walk_control_t::resume_k;
            }
            outcome = outputs.push(listed_key(entry));
            return outcome.failed() ? walk_control_t::halt_k : walk_control_t::resume_k;
        });
    if (st::failed(walked) || outcome.failed()) return walked;
    return watch_keys<entry_type_>(view, outputs, first);
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

/** The bytes of value an entry holds, none for a half-edge, which is all key. */
template <typename mapped_type_>
std::size_t value_bytes_of(st::mapping<ustore_key_t, mapped_type_> const& pair) noexcept {
    return value_of(pair).size();
}
constexpr std::size_t value_bytes_of(half_edge_t const&) noexcept { return 0; }

/** Measures @p keys of an @p entry_type_ collection, a vertex's space counting its half-edges. */
template <typename entry_type_, typename view_type_>
store_status_t measure_window(view_type_ const& view, key_range_t const& keys, window_measure_t& measured) noexcept {
    return view.range_from(entry_for<entry_type_>(keys.lower),
                           [&](entry_type_ const& entry) noexcept -> walk_control_t {
                               if (keys.upper < listed_key(entry)) return walk_control_t::halt_k;
                               std::size_t const value_bytes = value_bytes_of(entry);
                               measured.cardinality += is_listed(entry) ? 1 : 0;
                               measured.value_bytes += value_bytes;
                               measured.space_bytes += sizeof(entry_type_) + value_bytes;
                               return walk_control_t::resume_k;
                           });
}

/** Reads the key every string of tasks [ @p first, @p end ) of @p texts maps onto through @p view into @p outputs. */
template <typename view_type_>
store_status_t read_strings(view_type_ const& view, strings_arg_t const& texts, std::size_t first, std::size_t end,
                            strings_read_outputs_t& outputs) noexcept {
    store_status_t const reserved = view.prepare(end - first);
    if (st::failed(reserved)) return reserved;
    auto const found = [&](string_pair_t const& pair) noexcept { outputs.push_present(pair.mapped); };
    auto const missing = [&]() noexcept { outputs.push_missing(); };
    for (std::size_t index = first; index != end; ++index) {
        auto [key, key_status] = string_key_from(string_of(texts[index]));
        if (st::failed(key_status)) return key_status;
        store_status_t const looked_up = view.find(key, found, missing);
        if (st::failed(looked_up)) return looked_up;
    }
    return store_status_t::success_k;
}

/** The strings one task of a strings call names: those starting with @c prefix, from @c start,
 *  inclusive, towards @c end, exclusive, in @c direction, where an empty start begins at the first
 *  string in the direction and an empty end bounds nothing. */
struct strings_window_t {
    std::string_view prefix;
    std::string_view start;
    std::string_view end;
    ustore_scan_direction_t direction = ustore_scan_ascending_k;
};

/** Hands @p visit every string of @p window through @p view, smallest first, as <tt>visit(text)</tt>
 *  answering whether the walk goes on. */
template <typename view_type_, typename visit_type_>
store_status_t walk_strings(view_type_ const& view, strings_window_t const& window, visit_type_&& visit) noexcept {
    bool const descending = window.direction == ustore_scan_descending_k;
    std::string_view const lower = descending ? window.end : window.start;
    std::string_view const upper = descending ? window.start : window.end;
    auto [first, first_status] = string_key_from(lower > window.prefix ? lower : window.prefix);
    if (st::failed(first_status)) return first_status;
    return view.range_from(first, [&](string_pair_t const& pair) noexcept -> walk_control_t {
        std::string_view const text = pair.key.view();
        if (!text.starts_with(window.prefix)) return walk_control_t::halt_k;
        if (!upper.empty() && (descending ? upper < text : !(text < upper))) return walk_control_t::halt_k;
        if (descending && !lower.empty() && text == lower) return walk_control_t::resume_k;
        return visit(text);
    });
}

/** Watches the strings @p outputs hold from @p first on, which only a watching view records, once
 *  the walk that found them let go of its partitions. */
template <typename view_type_>
store_status_t watch_strings(view_type_ const& view, strings_scan_outputs_t const& outputs,
                             std::size_t first) noexcept {
    if (!view.watches()) return store_status_t::success_k;
    for (std::size_t index = first; index != outputs.size(); ++index) {
        auto [key, key_status] = string_key_from(outputs[index]);
        store_status_t const watched = st::failed(key_status) ? key_status : view.watch(key);
        if (st::failed(watched)) return watched;
    }
    return store_status_t::success_k;
}

/** The smallest string after every string starting with @p prefix, written into @p scratch, or
 *  empty when every string longer than the prefix of only the largest bytes, the empty one
 *  included, may start with it. */
inline expected<std::string_view> after_prefix(std::string_view prefix, ustore_arena_t scratch) noexcept {
    while (!prefix.empty() && static_cast<unsigned char>(prefix.back()) == 0xFF) prefix.remove_suffix(1);
    if (prefix.empty()) return std::string_view {};
    auto [bytes, bytes_status] = arena_allocate<char>(scratch, prefix.size());
    if (bytes_status.failed()) return bytes_status;
    std::memcpy(bytes.data(), prefix.data(), prefix.size());
    bytes[prefix.size() - 1] = static_cast<char>(static_cast<unsigned char>(prefix.back()) + 1);
    return std::string_view {bytes.begin(), bytes.end()};
}

/** Hands @p visit every string of @p window through @p view, largest first, from @p upper down, the
 *  key of a string or @c st::unbounded to start at the largest, as <tt>visit(text)</tt> answering whether
 *  the walk goes on. */
template <typename view_type_, typename upper_type_, typename visit_type_>
store_status_t walk_strings_down(view_type_ const& view, strings_window_t const& window, upper_type_ const& upper,
                                 visit_type_&& visit) noexcept {
    return view.range(
        upper, st::unbounded,
        [&](string_pair_t const& pair) noexcept -> walk_control_t {
            std::string_view const text = pair.key.view();
            if (!text.starts_with(window.prefix))
                return text < window.prefix ? walk_control_t::halt_k : walk_control_t::resume_k;
            if (!window.end.empty() && !(window.end < text)) return walk_control_t::halt_k;
            return visit(text);
        },
        st::descending);
}

/** Appends up to @p limit strings of @p window to @p outputs in its direction, finding where a
 *  descending walk starts in @p scratch, and watches them as the view asks. */
template <typename view_type_>
store_status_t scan_strings(view_type_ const& view, strings_window_t const& window, std::size_t limit,
                            ustore_arena_t scratch, strings_scan_outputs_t& outputs, status_t& outcome) noexcept {
    if (limit == 0) return store_status_t::success_k;
    std::size_t const found_before = outputs.size();
    auto const take = [&](std::string_view text) noexcept -> walk_control_t {
        outcome = outputs.push(text);
        if (outcome.failed()) return walk_control_t::halt_k;
        return outputs.size() - found_before == limit ? walk_control_t::halt_k : walk_control_t::resume_k;
    };
    store_status_t walked = store_status_t::success_k;
    if (window.direction != ustore_scan_descending_k) walked = walk_strings(view, window, take);
    else {
        auto [after, after_status] = after_prefix(window.prefix, scratch);
        if (after_status.failed()) {
            outcome = after_status;
            return store_status_t::success_k;
        }
        std::string_view upper = window.start;
        if (upper.empty() || (!after.empty() && after < upper)) upper = after;
        if (upper.empty()) walked = walk_strings_down(view, window, st::unbounded, take);
        else {
            auto [first, first_status] = string_key_from(upper);
            walked = st::failed(first_status) ? first_status : walk_strings_down(view, window, first, take);
        }
    }
    if (st::failed(walked) || outcome.failed()) return walked;
    return watch_strings(view, outputs, found_before);
}

/** Appends a uniform reservoir of up to @p limit strings of a collection to @p outputs, drawing it
 *  in @p scratch, and watches them as the view asks. */
template <typename view_type_>
store_status_t sample_strings(view_type_ const& view, std::size_t limit, sample_generator_t& generator,
                              ustore_arena_t scratch, strings_scan_outputs_t& outputs, status_t& outcome) noexcept {
    if (limit == 0) return store_status_t::success_k;
    st::vector<std::string_view, arena_allocator<std::string_view>> kept {{scratch}};
    std::size_t seen = 0;
    store_status_t const walked = walk_strings(
        view, strings_window_t {}, [&](std::string_view text) noexcept -> walk_control_t {
            std::size_t const slot = seen < limit ? seen : st::draw_below(generator, seen + 1);
            ++seen;
            if (slot >= limit) return walk_control_t::resume_k;
            if (slot != kept.size()) {
                kept[slot] = text;
                return walk_control_t::resume_k;
            }
            outcome = growth_status(kept.emplace_back(text));
            return outcome.failed() ? walk_control_t::halt_k : walk_control_t::resume_k;
        });
    if (st::failed(walked) || outcome.failed()) return walked;
    std::size_t const first = outputs.size();
    for (std::size_t index = 0; index != kept.size() && outcome.succeeded(); ++index)
        outcome = outputs.push(kept[index]);
    if (outcome.failed()) return store_status_t::success_k;
    return watch_strings(view, outputs, first);
}

/** Measures the strings of @p window, each holding the key it maps onto as its value. */
template <typename view_type_>
store_status_t measure_strings(view_type_ const& view, strings_window_t const& window,
                               window_measure_t& measured) noexcept {
    return walk_strings(view, window, [&](std::string_view text) noexcept -> walk_control_t {
        ++measured.cardinality;
        measured.value_bytes += sizeof(ustore_key_t);
        measured.space_bytes += sizeof(string_pair_t) + text.size();
        return walk_control_t::resume_k;
    });
}

#pragma endregion Reads

#pragma region Writes

/** Stages @p content under @p key into @p transaction, which may be a store publishing it at once,
 *  as the blob, the document or the page an entry of @p entry_type_ holds. */
template <typename entry_type_, typename transaction_type_>
store_status_t stage_upsert(transaction_type_& transaction, ustore_key_t const& key, value_view_t content) noexcept {
    auto [copied, copy_blob_status] = copy_blob(content);
    if (st::failed(copy_blob_status)) return copy_blob_status;
    return transaction.upsert(entry_type_ {key, typename entry_type_::mapped_type {std::move(copied)}});
}

/** Stages erasing @p key, watching its absence when @p watch asks and the key is already gone. */
template <typename transaction_type_, typename key_type_>
store_status_t stage_erase(transaction_type_& transaction, key_type_ const& key, ustore_watch_t watch) noexcept {
    store_status_t const erased = transaction.erase(key);
    if (erased != store_status_t::key_not_found_k) return erased;
    if (watch == ustore_watch_none_k) return store_status_t::success_k;
    return transaction.watch(key);
}

/** Stages the values of tasks [ @p first, @p end ) of a batch into @p transaction, as entries of @p entry_type_. */
template <typename entry_type_, typename transaction_type_>
store_status_t stage_writes(transaction_type_& transaction, place_batch_t const& places, strings_arg_t const& values,
                            std::size_t first, std::size_t end) noexcept {
    for (std::size_t index = first; index != end; ++index) {
        store_status_t const staged = stage_upsert<entry_type_>(transaction, places[index].key, values[index]);
        if (st::failed(staged)) return staged;
    }
    return store_status_t::success_k;
}

/** Stages erasing the keys of tasks [ @p first, @p end ) of a batch from @p transaction. */
template <typename transaction_type_>
store_status_t stage_removals(transaction_type_& transaction, place_batch_t const& places, std::size_t first,
                              std::size_t end, ustore_watch_t watch) noexcept {
    for (std::size_t index = first; index != end; ++index) {
        store_status_t const erased = stage_erase(transaction, places[index].key, watch);
        if (st::failed(erased)) return erased;
    }
    return store_status_t::success_k;
}

/** Stages erasing every key in [ key, end key ) of tasks [ @p first, @p end ) of a batch from
 *  @p transaction, where the tombstones guard the keys found as their single removals do. */
template <typename transaction_type_>
store_status_t stage_range_removals(transaction_type_& transaction, place_batch_t const& places,
                                    strided_iterator<ustore_key_t const> end_keys, std::size_t first,
                                    std::size_t end) noexcept {
    for (std::size_t index = first; index != end; ++index) {
        store_status_t const erased = transaction.erase_range(places[index].key, end_keys[index], st::no_op_t {});
        if (st::failed(erased)) return erased;
    }
    return store_status_t::success_k;
}

/** Stages the keys tasks [ @p first, @p end ) of a batch of strings map onto into @p transaction. */
template <typename transaction_type_>
store_status_t stage_strings(transaction_type_& transaction, strings_arg_t const& texts,
                             strided_iterator<ustore_key_t const> values, std::size_t first, std::size_t end) noexcept {
    for (std::size_t index = first; index != end; ++index) {
        auto [key, key_status] = string_key_from(string_of(texts[index]));
        if (st::failed(key_status)) return key_status;
        store_status_t const staged = transaction.upsert(string_pair_t {std::move(key), values[index]});
        if (st::failed(staged)) return staged;
    }
    return store_status_t::success_k;
}

/** Stages erasing the strings of tasks [ @p first, @p end ) of a batch from @p transaction. */
template <typename transaction_type_>
store_status_t stage_string_removals(transaction_type_& transaction, strings_arg_t const& texts, std::size_t first,
                                     std::size_t end, ustore_watch_t watch) noexcept {
    for (std::size_t index = first; index != end; ++index) {
        auto [key, key_status] = string_key_from(string_of(texts[index]));
        if (st::failed(key_status)) return key_status;
        store_status_t const erased = stage_erase(transaction, key, watch);
        if (st::failed(erased)) return erased;
    }
    return store_status_t::success_k;
}

/** Stages erasing every string in [ string, end string ) of tasks [ @p first, @p end ) of a batch
 *  from @p transaction, where an empty end string erases up to the last string. */
template <typename transaction_type_>
store_status_t stage_string_range_removals(transaction_type_& transaction, strings_arg_t const& texts,
                                           strings_arg_t const& end_texts, std::size_t first,
                                           std::size_t end) noexcept {
    for (std::size_t index = first; index != end; ++index) {
        auto [lower, lower_status] = string_key_from(string_of(texts[index]));
        if (st::failed(lower_status)) return lower_status;
        std::string_view const end_text = string_of(end_texts[index]);
        if (end_text.empty()) {
            store_status_t const erased = transaction.erase_from(lower, st::no_op_t {});
            if (st::failed(erased)) return erased;
            continue;
        }
        auto [upper, upper_status] = string_key_from(end_text);
        if (st::failed(upper_status)) return upper_status;
        store_status_t const erased = transaction.erase_range(lower, upper, st::no_op_t {});
        if (st::failed(erased)) return erased;
    }
    return store_status_t::success_k;
}

/**
 *  @brief Commits what @p stage_changes stages through a transaction of its own, retrying conflicts
 *      up to @c commit_attempts_k times.
 *  @param[out] commit_stamp Optional output for the stamp the commit published under.
 *
 *  @p stage_changes takes a @c transaction_source and may refuse the call through its outcome, answering a failure
 *  that is not a conflict, so no attempt follows.
 */
template <cpu_class_t cpu_class_, isolation_t isolation_, typename stage_changes_type_>
store_status_t commit_group(database_state<cpu_class_, isolation_>& state, stage_changes_type_&& stage_changes,
                            generation_t* commit_stamp = nullptr) noexcept {
    auto opened = collections_transaction_of<cpu_class_, isolation_>::make(state.order);
    if (!opened) return opened.status();
    transaction_source<cpu_class_, isolation_> source {state, *opened, ustore_watch_none_k};
    store_status_t const committed = st::commit_with_retries(
        *opened, commit_attempts_k,
        [&](collections_transaction_of<cpu_class_, isolation_>&) noexcept -> store_status_t {
            return stage_changes(source);
        });
    if (commit_stamp && st::succeeded(committed)) *commit_stamp = opened->commit_stamp();
    return committed;
}

/** Stages @p stage_runs into the transaction @p view carries, or commits it as a transaction of its
 *  own and persists it when @p durability asks, for every call that writes. @p stage_runs is
 *  invoked as <tt>stage_runs(source, outcome)</tt> and may refuse the call through the outcome. */
template <cpu_class_t cpu_class_, isolation_t isolation_, typename stage_runs_type_>
status_t serve_write(database_state<cpu_class_, isolation_>& state, ustore_durability_t durability, view_t const& view,
                     stage_runs_type_&& stage_runs) noexcept {
    if (view.transaction && durability == ustore_durability_flushed_k)
        return {status_code_t::invalid_argument_k, "A transaction flushes at commit, not at a write"};
    status_t outcome;
    if (view.transaction) {
        auto& transaction = state_of<transaction_state<cpu_class_, isolation_>>(view.transaction)->transaction;
        transaction_source<cpu_class_, isolation_> source {state, transaction, watches_reads<isolation_>(view.watch)};
        return reported(stage_runs(source, outcome), outcome);
    }
    store_status_t const committed = commit_group(
        state,
        [&](transaction_source<cpu_class_, isolation_>& source) noexcept { return stage_runs(source, outcome); });
    if (status_t const status = reported(committed, outcome); status.failed()) return status;
    if (durability == ustore_durability_flushed_k) return persist(state);
    return {};
}

/** Hands @p stage every run of @p count tasks on a collection whose store holds @p entry_type_
 *  through @p source, with the part of the transaction over that store, refusing a collection of
 *  any other modality. */
template <typename entry_type_, typename source_type_, typename stage_type_>
store_status_t stage_runs_of(source_type_& source, strided_iterator<ustore_collection_id_t const> collections,
                             std::size_t count, status_t& outcome, stage_type_&& stage) noexcept {
    return for_each_run(source, collections, count, outcome,
                        [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
                            if constexpr (!std::is_same_v<entry_of<decltype(entries)>, entry_type_>)
                                return refused_modality(outcome);
                            else {
                                auto part = source.part(entries);
                                if (!part) return part.status();
                                return stage(**part, first, end);
                            }
                        });
}

/** Removes the keys @p arguments name, or every key of their ranges when they give end keys, from
 *  collections whose stores hold @p entry_type_, committing a batch outside a transaction
 *  atomically, where a key already gone stays gone. */
template <typename entry_type_, cpu_class_t cpu_class_, isolation_t isolation_, typename arguments_type_>
status_t remove_keys(database_state<cpu_class_, isolation_>& state, arguments_type_ const& arguments,
                     view_t const& view) noexcept {
    place_batch_t const places = places_of(arguments);
    if (status_t const status = validate_remove(view.transaction, places, arguments.durability, view.watch);
        status.failed() || !arguments.count)
        return status;
    strided_iterator<ustore_key_t const> const end_keys = strided(arguments.end_keys);
    return serve_write(state, arguments.durability, view, [&](auto& source, status_t& outcome) noexcept {
        return stage_runs_of<entry_type_>(source, places.collections_begin, places.size(), outcome,
                                          [&](auto& part, std::size_t first, std::size_t end) noexcept {
                                              if (end_keys)
                                                  return stage_range_removals(part, places, end_keys, first, end);
                                              return stage_removals(part, places, first, end, source.watch_mode);
                                          });
    });
}

/**
 *  @brief Serves a read-modify-write of documents or pages through @c serve_write, reading and writing in one
 *      transaction: the caller's, or one of the call's own, retried on conflicts.
 *
 *  @p modify is invoked as <tt>modify(source, failure)</tt> on every attempt and answers the status of the
 *  call, recording into @c failure the first failure of the stores, which a commit retries when it
 *  is a conflict.
 */
template <cpu_class_t cpu_class_, isolation_t isolation_, typename modify_type_>
status_t serve_read_modify_write(database_state<cpu_class_, isolation_>& state, ustore_durability_t durability,
                                 view_t const& view, modify_type_&& modify) noexcept {
    return serve_write(
        state, durability, view,
        [&](transaction_source<cpu_class_, isolation_>& source, status_t& outcome) noexcept -> store_status_t {
            store_status_t failure = store_status_t::success_k;
            outcome = modify(source, failure);
            if (st::failed(failure)) {
                outcome = {};
                return failure;
            }
            return outcome.failed() ? store_status_t::invalid_argument_k : store_status_t::success_k;
        });
}

#pragma endregion Writes

#pragma region Collections

/** One collection as the collections system collection lists it at some stamp. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
struct listed_collection {
    ustore_collection_id_t id;
    collection_store_of<cpu_class_, isolation_>* store;
    blob_t name;
};

/** Every collection the collections system collection lists at some stamp. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
using listed_collections = st::vector<listed_collection<cpu_class_, isolation_>>;

/** Collects every collection the collections system collection lists through @p view. */
template <cpu_class_t cpu_class_, isolation_t isolation_, typename view_type_>
store_status_t collect_collections(view_type_ const& view,
                                   listed_collections<cpu_class_, isolation_>& listed) noexcept {
    store_status_t appended = store_status_t::success_k;
    store_status_t const walked = view.range_from(
        ustore_collection_id_t {},
        [&](collections_pair_of<cpu_class_, isolation_> const& pair) noexcept -> walk_control_t {
            auto name = pair.mapped.name.copy();
            appended = name ? listed.push_back(listed_collection<cpu_class_, isolation_> {pair.key, pair.mapped.store,
                                                                                          std::move(*name)})
                            : name.status();
            return st::failed(appended) ? walk_control_t::halt_k : walk_control_t::resume_k;
        });
    return st::failed(walked) ? walked : appended;
}

/** The stores a change of the collections system collection installs and those it takes out, so its
 *  commit retires the latter and its failure frees the former. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
struct collections_change {

    /** Stores the change installs, which nothing else reaches until it commits. */
    st::vector<collection_store_of<cpu_class_, isolation_>*> installed;

    /** Stores the change takes out of the collections system collection, retired at its stamp. */
    st::vector<collection_store_of<cpu_class_, isolation_>*> taken_out;

    /** Frees what a refused attempt installed and forgets what it took out, ahead of the next attempt. */
    void discard() noexcept {
        for (std::size_t index = 0; index != installed.size(); ++index) delete installed[index];
        installed.clear();
        taken_out.clear();
    }

    /** Allocates a store for a collection holding @p modality, answering null after an allocation failed. */
    collection_store_of<cpu_class_, isolation_>* install(order_t& order, ustore_modality_t modality) noexcept {
        collection_store_of<cpu_class_, isolation_>* fresh = allocate_store<cpu_class_, isolation_>(order, modality);
        if (!fresh) return nullptr;
        if (st::succeeded(installed.push_back(std::exchange(fresh, nullptr)))) return installed[installed.size() - 1];
        return nullptr;
    }

    /** Records that @p store leaves the collections system collection. */
    store_status_t take_out(collection_store_of<cpu_class_, isolation_>* store) noexcept {
        return taken_out.push_back(std::move(store));
    }
};

/** Commits @p stage_change as one change of the collections system collection, retiring what it
 *  took out at its stamp or freeing what it installed, invoking @p stage_change as
 *  <tt>stage_change(source, change)</tt> on every attempt. */
template <cpu_class_t cpu_class_, isolation_t isolation_, typename stage_change_type_>
store_status_t commit_collections_change(database_state<cpu_class_, isolation_>& state,
                                         stage_change_type_&& stage_change) noexcept {
    collections_change<cpu_class_, isolation_> change;
    generation_t commit_stamp = 0;
    store_status_t const committed = commit_group(
        state,
        [&](transaction_source<cpu_class_, isolation_>& source) noexcept -> store_status_t {
            change.discard();
            return stage_change(source, change);
        },
        &commit_stamp);
    if (st::failed(committed)) {
        change.discard();
        return committed;
    }
    for (std::size_t index = 0; index != change.taken_out.size(); ++index)
        retire(state, change.taken_out[index], commit_stamp);
    free_unreachable(state);
    return store_status_t::success_k;
}

/** Stages collection @p id holding @p modality under @p name in a store @p change installs,
 *  refusing a name another collection carries. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
store_status_t stage_collection(transaction_source<cpu_class_, isolation_>& source,
                                collections_change<cpu_class_, isolation_>& change, ustore_collection_id_t id,
                                ustore_modality_t modality, std::string_view name) noexcept {
    if (!name.empty()) {
        auto names = source.part(source.state.collection_ids_by_name);
        if (!names) return names.status();
        auto [key, key_status] = string_key_from(name);
        if (st::failed(key_status)) return key_status;
        bool taken = false;
        store_status_t const looked_up = (*names)->find(
            key, [&](string_pair_t const&) noexcept { taken = true; }, st::no_op_t {});
        if (st::failed(looked_up)) return looked_up;
        if (taken) return store_status_t::key_already_exists_k;
        store_status_t const named = (*names)->upsert(string_pair_t {std::move(key), ustore_key_t {id.uuid}});
        if (st::failed(named)) return named;
    }
    collection_store_of<cpu_class_, isolation_>* const store = change.install(source.state.order, modality);
    if (!store) return store_status_t::out_of_memory_heap_k;
    auto entries = source.part(source.state.collections_by_id);
    if (!entries) return entries.status();
    auto [copied, copy_blob_status] = copy_blob(value_view_t {name});
    if (st::failed(copy_blob_status)) return copy_blob_status;
    return (*entries)->upsert(collections_pair_of<cpu_class_, isolation_> {
        id, collection_entry_of<cpu_class_, isolation_> {std::move(copied), store}});
}

/** Stages taking @p collection out of both system collections, retiring its store. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
store_status_t stage_forget(transaction_source<cpu_class_, isolation_>& source,
                            collections_change<cpu_class_, isolation_>& change,
                            ustore_collection_id_t collection) noexcept {
    auto entries = source.part(source.state.collections_by_id);
    if (!entries) return entries.status();
    string_key_t name;
    collection_store_of<cpu_class_, isolation_>* store = nullptr;
    store_status_t listing = store_status_t::key_not_found_k;
    store_status_t const looked_up = (*entries)->find(
        collection,
        [&](collections_pair_of<cpu_class_, isolation_> const& pair) noexcept {
            store = pair.mapped.store;
            auto copied = pair.mapped.name.copy();
            listing = copied ? store_status_t::success_k : copied.status();
            if (copied) name.text = std::move(*copied);
        },
        st::no_op_t {});
    if (st::failed(looked_up)) return looked_up;
    if (st::failed(listing)) return listing;
    store_status_t const unlisted = (*entries)->erase(collection);
    if (st::failed(unlisted)) return unlisted;
    store_status_t const retiring = change.take_out(store);
    if (st::failed(retiring) || name.text.size() == 0) return retiring;
    auto names = source.part(source.state.collection_ids_by_name);
    if (!names) return names.status();
    return (*names)->erase(name);
}

/** Stages giving @p collection a fresh empty store, retiring the one it held. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
store_status_t stage_emptied(transaction_source<cpu_class_, isolation_>& source,
                             collections_change<cpu_class_, isolation_>& change,
                             ustore_collection_id_t collection) noexcept {
    auto entries = source.part(source.state.collections_by_id);
    if (!entries) return entries.status();
    collection_entry_of<cpu_class_, isolation_> emptied;
    collection_store_of<cpu_class_, isolation_>* held = nullptr;
    store_status_t listing = store_status_t::key_not_found_k;
    store_status_t const looked_up = (*entries)->find(
        collection,
        [&](collections_pair_of<cpu_class_, isolation_> const& pair) noexcept {
            held = pair.mapped.store;
            auto copied = pair.mapped.name.copy();
            listing = copied ? store_status_t::success_k : copied.status();
            if (copied) emptied.name = std::move(*copied);
        },
        st::no_op_t {});
    if (st::failed(looked_up)) return looked_up;
    if (st::failed(listing)) return listing;
    emptied.store = change.install(source.state.order, modality_of(*held));
    if (!emptied.store) return store_status_t::out_of_memory_heap_k;
    store_status_t const retiring = change.take_out(held);
    if (st::failed(retiring)) return retiring;
    return (*entries)->upsert(collections_pair_of<cpu_class_, isolation_> {collection, std::move(emptied)});
}

/** Stages emptying every blob, document or page of @p entry_type_ that @p transaction reaches, keeping every key. */
template <typename entry_type_, typename transaction_type_>
store_status_t stage_emptied_values(transaction_type_& transaction) noexcept {
    st::vector<ustore_key_t> keys;
    store_status_t copied = store_status_t::success_k;
    store_status_t const walked = transaction.range_from(
        smallest_key_k, [&](entry_type_ const& pair) noexcept -> walk_control_t {
            copied = keys.emplace_back(pair.key);
            return st::failed(copied) ? walk_control_t::halt_k : walk_control_t::resume_k;
        });
    if (st::failed(walked)) return walked;
    if (st::failed(copied)) return copied;
    for (std::size_t index = 0; index != keys.size(); ++index) {
        store_status_t const emptied = transaction.upsert(entry_type_ {keys[index]});
        if (st::failed(emptied)) return emptied;
    }
    return store_status_t::success_k;
}

/** Stages emptying every value of @p collection, keeping its keys, which a graph holds none of, and
 *  refusing a strings collection, whose values are the keys its strings map onto. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
store_status_t stage_values_emptied(transaction_source<cpu_class_, isolation_>& source,
                                    ustore_collection_id_t collection, status_t& outcome) noexcept {
    strided_iterator<ustore_collection_id_t const> const collections {&collection, sizeof(collection)};
    return for_each_run(
        source, collections, 1, outcome, [&](auto& entries, std::size_t, std::size_t) noexcept -> store_status_t {
            using entry_t = entry_of<decltype(entries)>;
            if constexpr (std::is_same_v<entry_t, half_edge_t>) return store_status_t::success_k;
            else if constexpr (std::is_same_v<entry_t, string_pair_t>)
                return refused(outcome, "A strings collection's values are keys, which can't be emptied");
            else {
                auto part = source.part(entries);
                if (!part) return part.status();
                return stage_emptied_values<entry_t>(**part);
            }
        });
}

#pragma endregion Collections

#pragma region Image

/**
 *  @brief Leads every image, so a file of another format is refused rather than misread.
 *
 *  The image then lists every collection until the end of the file:
 *
 *  @code{.c}
 *  struct collection {
 *      uint8_t id[16];       // offset 0
 *      uint8_t modality;     // offset 16, which picks the shape of every entry below
 *      uint64_t name_length; // offset 17, zero for a collection without a name
 *      char name[];          // offset 25
 *      uint64_t count;       // entries that follow
 *      union {
 *          struct { uint8_t key[16]; uint64_t length; uint8_t value[]; } value;                // blobs, docs, pages
 *          struct { uint64_t length; char text[]; uint8_t value[16]; } string;                 // strings
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

#if USTORE_OS_WINDOWS_

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
    return path.assign(sz::string_view_t {directory}) == sz::status_t::success_k &&
           path.append(name.data(), name.size()) == sz::status_t::success_k;
}

/** Writes @p blob as its length and its bytes, answering whether all of it reached @p file. */
inline bool write_blob(std::FILE* file, blob_t const& blob) noexcept {
    auto const length = static_cast<std::uint64_t>(blob.size());
    return std::fwrite(&length, sizeof(length), 1, file) == 1 &&
           (length == 0 || std::fwrite(blob.data(), 1, length, file) == length);
}

/** Writes one entry of a collection of blobs, documents or pages, answering whether all of it reached @p file. */
template <typename mapped_type_>
bool write_entry(std::FILE* file, st::mapping<ustore_key_t, mapped_type_> const& pair) noexcept {
    return std::fwrite(&pair.key, sizeof(ustore_key_t), 1, file) == 1 && write_blob(file, blob_of(pair.mapped));
}

/** Writes one entry of a collection of strings, answering whether all of it reached @p file. */
inline bool write_entry(std::FILE* file, string_pair_t const& pair) noexcept {
    return write_blob(file, pair.key.text) && std::fwrite(&pair.mapped, sizeof(ustore_key_t), 1, file) == 1;
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

/** Bytes of the key and the value of an entry, which the progress of a save counts, a half-edge being all key. */
template <typename mapped_type_>
std::size_t saved_bytes(st::mapping<ustore_key_t, mapped_type_> const& pair) noexcept {
    return sizeof(ustore_key_t) + blob_of(pair.mapped).size();
}
inline std::size_t saved_bytes(string_pair_t const& pair) noexcept {
    return pair.key.text.size() + sizeof(ustore_key_t);
}
inline std::size_t saved_bytes(half_edge_t const&) noexcept { return sizeof(half_edge_t); }

/** Counts into @p count the entries @p store holds at @p read_stamp, and adds the bytes of their
 *  keys and values to @p bytes. */
template <typename store_type_>
store_status_t tally_entries(store_type_ const& store, generation_t read_stamp, std::uint64_t& count,
                             std::size_t& bytes) noexcept {
    using entry_t = entry_of<store_type_>;
    return store.reader_at(read_stamp).range_from(first_entry<entry_t>(), [&](entry_t const& entry) noexcept {
        ++count;
        bytes += saved_bytes(entry);
    });
}

/** Writes how many entries @p store holds at @p read_stamp, then every one of them, adding the
 *  bytes of their keys and values to @p bytes, and clearing @p written when any of it misses
 *  @p file. */
template <typename store_type_>
store_status_t write_entries(std::FILE* file, store_type_ const& store, generation_t read_stamp, std::size_t& bytes,
                             bool& written) noexcept {
    using entry_t = entry_of<store_type_>;
    std::uint64_t count = 0;
    store_status_t const counted = tally_entries(store, read_stamp, count, bytes);
    if (st::failed(counted)) return counted;
    written = written && std::fwrite(&count, sizeof(count), 1, file) == 1;
    return store.reader_at(read_stamp)
        .range_from(first_entry<entry_t>(), [&](entry_t const& entry) noexcept -> walk_control_t {
            written = written && write_entry(file, entry);
            return written ? walk_control_t::resume_k : walk_control_t::halt_k;
        });
}

/** Writes the image of every collection of @p state at @p read_stamp into @p directory, reporting
 *  the bytes of the keys and values written to @p progress after each collection, removing the
 *  partial image when the save fails or is cancelled. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t write_image(database_state<cpu_class_, isolation_>& state, generation_t read_stamp, char const* directory,
                     ustore_progress_callback_t progress = nullptr, void* progress_state = nullptr) noexcept {
    sz::string_t partial_path;
    sz::string_t final_path;
    if (!join_path(partial_path, directory, partial_image_name_k) || !join_path(final_path, directory, image_name_k))
        return {status_code_t::out_of_memory_k, "Failed to build the image path"};
    listed_collections<cpu_class_, isolation_> listed;
    reader_source<cpu_class_, isolation_> source {state, read_stamp};
    auto collections = source.view(state.collections_by_id);
    if (store_status_t const collected = collect_collections<cpu_class_, isolation_>(*collections, listed);
        st::failed(collected))
        return translate(collected);

    // The total costs one more walk of every collection, which only a callback needs
    std::size_t total = 0;
    for (std::size_t index = 0; progress && index != listed.size(); ++index) {
        std::uint64_t count = 0;
        store_status_t const tallied = std::visit(
            [&](auto const& entries) noexcept { return tally_entries(entries, read_stamp, count, total); },
            listed[index].store->entries);
        if (st::failed(tallied)) return translate(tallied);
    }
    progress_t reporter {progress, progress_state, total};
    stream_t stream {std::fopen(partial_path.c_str(), "wb")};
    if (!stream.get()) return {status_code_t::io_k, "Failed to open the image for writing"};
    auto abandon = [&](status_t status) noexcept {
        if (stream.get()) { [[maybe_unused]] bool const closed = stream.close(); }
        [[maybe_unused]] int const removed = std::remove(partial_path.c_str());
        return status;
    };

    bool written = std::fwrite(image_signature_k, sizeof(image_signature_k), 1, stream.get()) == 1;
    for (std::size_t index = 0; index != listed.size() && written; ++index) {
        listed_collection<cpu_class_, isolation_> const& collection = listed[index];
        auto const modality = static_cast<std::uint8_t>(modality_of(*collection.store));
        written = std::fwrite(&collection.id, sizeof(ustore_collection_id_t), 1, stream.get()) == 1 &&
                  std::fwrite(&modality, sizeof(modality), 1, stream.get()) == 1 &&
                  write_blob(stream.get(), collection.name);
        std::size_t bytes = 0;
        store_status_t const walked = std::visit(
            [&](auto const& entries) noexcept {
                return write_entries(stream.get(), entries, read_stamp, bytes, written);
            },
            collection.store->entries);
        if (st::failed(walked)) return abandon(translate(walked));
        if (written && !reporter.advance(bytes)) return abandon(progress_cancelled_k);
    }

    bool const synced = written && std::fflush(stream.get()) == 0 && sync_file(stream.get());
    bool const closed = stream.close();
    if (!synced || !closed) return abandon({status_code_t::io_k, "Failed to write the image"});
    if (!replace_file(partial_path.c_str(), final_path.c_str()))
        return abandon({status_code_t::io_k, "Failed to replace the image"});
    return {};
}

/** What loading one part of an image came to. */
enum class image_step_t : std::uint8_t { loaded_k, finished_k, truncated_k, malformed_k, out_of_memory_k, refused_k };

/** The status of every image step, success for the two that are not failures. */
inline constexpr std::array<status_t, 6> image_step_statuses_k {{
    {},
    {},
    {status_code_t::malformed_k, "The image is truncated"},
    {status_code_t::malformed_k, "The image names an unknown modality"},
    {status_code_t::out_of_memory_k, "Out of memory while loading the image"},
    {status_code_t::internal_k, "The store refused an entry of the image"},
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
    else if constexpr (std::is_same_v<entry_t, string_pair_t>) {
        string_key_t text;
        ustore_key_t value;
        if (image_step_t const loaded = load_blob(file, text.text); loaded != image_step_t::loaded_k) return loaded;
        if (std::fread(&value, sizeof(ustore_key_t), 1, file) != 1) return image_step_t::truncated_k;
        return st::failed(store.upsert(string_pair_t {std::move(text), value})) ? image_step_t::refused_k
                                                                                : image_step_t::loaded_k;
    }
    else {
        ustore_key_t key;
        blob_t value;
        if (std::fread(&key, sizeof(ustore_key_t), 1, file) != 1) return image_step_t::truncated_k;
        if (image_step_t const loaded = load_blob(file, value); loaded != image_step_t::loaded_k) return loaded;
        using mapped_t = typename entry_t::mapped_type;
        return st::failed(store.upsert(entry_t {key, mapped_t {std::move(value)}})) ? image_step_t::refused_k
                                                                                    : image_step_t::loaded_k;
    }
}

/** Loads the next collection of @p file into @p state. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
image_step_t load_collection(std::FILE* file, database_state<cpu_class_, isolation_>& state) noexcept {
    ustore_collection_id_t id;
    if (std::fread(&id, sizeof(id), 1, file) != 1) return image_step_t::finished_k;
    std::uint8_t modality = 0;
    blob_t name;
    std::uint64_t count = 0;
    if (std::fread(&modality, sizeof(modality), 1, file) != 1) return image_step_t::truncated_k;
    if (modality < ustore_modality_blobs_k || modality > ustore_modality_pages_k) return image_step_t::malformed_k;
    if (image_step_t const loaded = load_blob(file, name); loaded != image_step_t::loaded_k) return loaded;
    if (std::fread(&count, sizeof(count), 1, file) != 1) return image_step_t::truncated_k;

    if (name.size() != 0) {
        auto [key, key_status] = string_key_from(view_of(name));
        if (st::failed(key_status)) return image_step_t::out_of_memory_k;
        if (st::failed(state.collection_ids_by_name.upsert(string_pair_t {std::move(key), ustore_key_t {id.uuid}})))
            return image_step_t::refused_k;
    }
    collection_store_of<cpu_class_, isolation_>* const store = allocate_store<cpu_class_, isolation_>(
        state.order, static_cast<ustore_modality_t>(modality));
    if (!store) return image_step_t::out_of_memory_k;
    collections_pair_of<cpu_class_, isolation_> pair {
        id, collection_entry_of<cpu_class_, isolation_> {std::move(name), store}};
    if (st::failed(state.collections_by_id.upsert(std::move(pair)))) {
        delete store;
        return image_step_t::refused_k;
    }
    for (std::uint64_t index = 0; index != count; ++index) {
        image_step_t const loaded = std::visit([&](auto& entries) noexcept { return load_entry(file, entries); },
                                               store->entries);
        if (loaded != image_step_t::loaded_k) return loaded;
    }
    return image_step_t::loaded_k;
}

/** Loads the image of the directory of @p state into its stores, leaving them empty when there is none. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t read_image(database_state<cpu_class_, isolation_>& state) noexcept {
    sz::string_t path;
    if (!join_path(path, state.directory.c_str(), image_name_k))
        return {status_code_t::out_of_memory_k, "Failed to build the image path"};
    stream_t stream {std::fopen(path.c_str(), "rb")};
    if (!stream.get())
        return errno == ENOENT ? status_t {} : status_t {status_code_t::io_k, "Failed to open the image"};
    char signature[sizeof(image_signature_k)];
    if (std::fread(signature, sizeof(signature), 1, stream.get()) != 1 ||
        std::memcmp(signature, image_signature_k, sizeof(signature)) != 0)
        return {status_code_t::malformed_k, "The image has an unknown format"};
    image_step_t step = image_step_t::loaded_k;
    while (step == image_step_t::loaded_k) step = load_collection(stream.get(), state);
    return image_step_statuses_k[static_cast<std::size_t>(step)];
}

/** Rewrites the image of @p state from its newest published commit, when it has a directory. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t persist(database_state<cpu_class_, isolation_>& state) noexcept {
    if (state.directory.empty()) return {};
    claim_t claim;
    return write_image(state, state.order.take_snapshot(claim), state.directory.c_str());
}

#pragma endregion Image

#pragma region Lifetime

/** Remembers the directory of @p state and loads its image. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t open_database(database_state<cpu_class_, isolation_>& state, std::string_view directory) noexcept {
    if (directory.empty()) return {};
    if (state.directory.assign(sz::string_view_t {directory}) != sz::status_t::success_k)
        return {status_code_t::out_of_memory_k, "Failed to copy the directory"};
    if (!names_directory(state.directory.c_str())) return {status_code_t::not_found_k, "The directory doesn't exist"};
    return read_image(state);
}

/** Gives every collection a fresh empty store, keeping both system collections. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t clear_pairs(database_state<cpu_class_, isolation_>& state) noexcept {
    store_status_t const cleared = commit_collections_change(
        state, [&](transaction_source<cpu_class_, isolation_>& source,
                   collections_change<cpu_class_, isolation_>& change) noexcept {
            auto collections = source.view(state.collections_by_id);
            if (!collections) return collections.status();
            listed_collections<cpu_class_, isolation_> listed;
            store_status_t const collected = collect_collections<cpu_class_, isolation_>(*collections, listed);
            for (std::size_t index = 0; index != listed.size() && st::succeeded(collected); ++index) {
                store_status_t const emptied = stage_emptied(source, change, listed[index].id);
                if (st::failed(emptied)) return emptied;
            }
            return collected;
        });
    return translate(cleared);
}

/** Removes every collection and every name, which no open snapshot may still be reading. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t reset_database(database_state<cpu_class_, isolation_>& state) noexcept {
    if (atomic_ref_of<cpu_class_, std::size_t>(state.open_snapshots).load(std::memory_order_acquire) != 0)
        return {status_code_t::invalid_argument_k, "A reset waits until every snapshot is freed"};
    store_status_t const reset = commit_collections_change(
        state, [&](transaction_source<cpu_class_, isolation_>& source,
                   collections_change<cpu_class_, isolation_>& change) noexcept {
            auto collections = source.view(state.collections_by_id);
            if (!collections) return collections.status();
            listed_collections<cpu_class_, isolation_> listed;
            store_status_t const collected = collect_collections<cpu_class_, isolation_>(*collections, listed);
            if (st::failed(collected)) return collected;
            for (std::size_t index = 0; index != listed.size(); ++index) {
                store_status_t const forgotten = stage_forget(source, change, listed[index].id);
                if (st::failed(forgotten)) return forgotten;
            }
            return store_status_t::success_k;
        });
    return translate(reset);
}

/** Empties every value of every collection, keeping the keys, which a graph holds none of, and
 *  leaving every strings collection as it is, whose values are keys. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t clear_values(database_state<cpu_class_, isolation_>& state) noexcept {
    status_t outcome;
    store_status_t const cleared = commit_group(
        state, [&](transaction_source<cpu_class_, isolation_>& source) noexcept {
            auto collections = source.view(state.collections_by_id);
            if (!collections) return collections.status();
            listed_collections<cpu_class_, isolation_> listed;
            store_status_t const collected = collect_collections<cpu_class_, isolation_>(*collections, listed);
            for (std::size_t index = 0; index != listed.size() && st::succeeded(collected); ++index) {
                if (modality_of(*listed[index].store) == ustore_modality_strings_k) continue;
                store_status_t const emptied = stage_values_emptied(source, listed[index].id, outcome);
                if (st::failed(emptied)) return emptied;
            }
            return collected;
        });
    return reported(cleared, outcome);
}

/** Bytes the image of @p state takes on disk, zero for a volatile database or one not yet persisted. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
std::size_t image_bytes(database_state<cpu_class_, isolation_> const& state) noexcept {
    sz::string_t path;
    if (state.directory.empty() || !join_path(path, state.directory.c_str(), image_name_k)) return 0;
    expected<seekable_file_t> const image = seekable_file_t::open(path.c_str());
    if (!image) return 0;
    expected<std::size_t> const length = image->length();
    return length ? *length : 0;
}

#pragma endregion Lifetime

#pragma region Calls of Every Modality

/** Implements the read of whole values from collections whose stores hold @p entry_type_, refusing
 *  any other, copying every value into the caller's arena once. */
template <typename entry_type_, cpu_class_t cpu_class_, isolation_t isolation_, typename arguments_type_>
status_t read_values(database_state<cpu_class_, isolation_>& state, arguments_type_ const& arguments,
                     view_t const& view) noexcept {
    if (!arguments.count) return {};
    place_batch_t const places = places_of(arguments);
    if (status_t const status = validate_read(places); status.failed()) return status;
    status_t outcome;
    auto [outputs, outputs_status] = read_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;

    // The source's claim keeps every version found alive until the outputs copy its value
    store_status_t const read = with_source(state, view, [&](auto& source) noexcept {
        store_status_t const runs = for_each_run(
            source, places.collections_begin, places.size(), outcome,
            [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
                if constexpr (!std::is_same_v<entry_of<decltype(entries)>, entry_type_>)
                    return refused_modality(outcome);
                else {
                    auto store = source.view(entries);
                    if (!store) return store.status();
                    return read_keys<entry_type_>(*store, places, first, end, outputs);
                }
            });
        if (st::succeeded(runs) && outcome.succeeded()) outcome = outputs.finish(arguments);
        return runs;
    });
    return reported(read, outcome);
}

/**
 *  @brief Reads the documents or pages at @p places of collections whose stores hold @p entry_type_, refusing any
 *      other, through @p source watching as @p view asks, appending their bytes to its scratch.
 *  @param[out] presences The presence bits of the values read, in the scratch.
 *  @param[out] contents The values read, missing ones empty, in the scratch.
 *  @param[out] failure The failure of the stores, which a commit retries when it is a conflict.
 */
template <typename entry_type_, typename source_type_>
status_t read_into_scratch(source_type_& source, place_batch_t const& places, view_t const& view,
                           ustore_octet_t const*& presences, ustore_strings_view_t& contents,
                           store_status_t& failure) noexcept {
    ustore_blobs_read_t read {};
    read.count = places.count;
    read.presences = &presences;
    read.values = &contents;
    read.arena = view.scratch;
    auto [outputs, outputs_status] = read_outputs_t::make(read, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    source_type_ reading = reading_as(source, view.watch);
    status_t outcome;
    store_status_t const runs = for_each_run(
        reading, places.collections_begin, places.size(), outcome,
        [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
            if constexpr (!std::is_same_v<entry_of<decltype(entries)>, entry_type_>) return refused_modality(outcome);
            else {
                auto store = reading.view(entries);
                if (!store) return store.status();
                return read_keys<entry_type_>(*store, places, first, end, outputs);
            }
        });
    if (outcome.failed()) return outcome;
    if (st::failed(runs)) {
        failure = runs;
        return translate(runs);
    }
    return outputs.finish(read);
}

/** Implements the scan of collections whose stores hold @p entry_type_, refusing any other, as
 *  ordered walks of each collection's keys, a graph's being its vertices. */
template <typename entry_type_, cpu_class_t cpu_class_, isolation_t isolation_, typename arguments_type_>
status_t scan_keys(database_state<cpu_class_, isolation_>& state, arguments_type_ const& arguments,
                   view_t const& view) noexcept {
    scan_batch_t const scans = scans_of(arguments);
    if (status_t const status = validate_scan(arguments); status.failed() || !arguments.count) return status;
    auto [outputs, outputs_status] = scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    progress_t progress {arguments.progress, arguments.progress_state, arguments.count};

    status_t outcome;
    store_status_t const walked = with_source(state, view, [&](auto& source) noexcept {
        return for_each_run(source, scans.collections, scans.size(), outcome,
                            [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
                                if constexpr (!std::is_same_v<entry_of<decltype(entries)>, entry_type_>)
                                    return refused_modality(outcome);
                                else {
                                    auto store = source.view(entries);
                                    if (!store) return store.status();
                                    for (std::size_t task = first; task != end && outcome.succeeded(); ++task) {
                                        outputs.open_task(task);
                                        store_status_t const scanned = scan_window<entry_type_>(
                                            *store, scans[task], arguments.mode, outputs, outcome);
                                        if (st::failed(scanned) || outcome.failed()) return scanned;
                                        if (!progress.advance(1)) outcome = progress_cancelled_k;
                                    }
                                    return store_status_t::success_k;
                                }
                            });
    });
    if (status_t const status = reported(walked, outcome); status.failed()) return status;
    return outputs.finish(arguments);
}

/** Implements the sample of collections whose stores hold @p entry_type_, refusing any other, as a
 *  reservoir filled from a walk of each collection's keys. */
template <typename entry_type_, cpu_class_t cpu_class_, isolation_t isolation_, typename arguments_type_>
status_t sample_keys(database_state<cpu_class_, isolation_>& state, arguments_type_ const& arguments,
                     view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!arguments.offsets || !arguments.keys)
        return {status_code_t::invalid_argument_k, "Need outputs for the offsets and the keys"};
    sample_batch_t const samples = samples_of(arguments);
    auto [outputs, outputs_status] = scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    sample_generator_t generator = sample_generator_t::seeded(&outputs);

    status_t outcome;
    store_status_t const walked = with_source(state, view, [&](auto& source) noexcept {
        return for_each_run(source, samples.collections, samples.size(), outcome,
                            [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
                                if constexpr (!std::is_same_v<entry_of<decltype(entries)>, entry_type_>)
                                    return refused_modality(outcome);
                                else {
                                    auto store = source.view(entries);
                                    if (!store) return store.status();
                                    for (std::size_t task = first; task != end; ++task) {
                                        outputs.open_task(task);
                                        store_status_t const sampled = sample_window<entry_type_>(
                                            *store, samples[task], generator, outputs, outcome);
                                        if (st::failed(sampled) || outcome.failed()) return sampled;
                                    }
                                    return store_status_t::success_k;
                                }
                            });
    });
    if (status_t const status = reported(walked, outcome); status.failed()) return status;
    return outputs.finish(arguments);
}

/** Implements the measure of collections whose stores hold @p entry_type_, refusing any other,
 *  exact for entries and value bytes at the view it reads, a graph's entries being its vertices. */
template <typename entry_type_, cpu_class_t cpu_class_, isolation_t isolation_, typename arguments_type_>
status_t measure_keys(database_state<cpu_class_, isolation_>& state, arguments_type_ const& arguments,
                      view_t const& view) noexcept {
    if (status_t const status = validate_direction(arguments.direction); status.failed() || !arguments.count)
        return status;
    scan_batch_t const measures = measures_of(arguments);
    auto [outputs, outputs_status] = measure_outputs_t::make(arguments);
    if (outputs_status.failed()) return outputs_status;

    status_t outcome;
    store_status_t const walked = with_source(state, view, [&](auto& source) noexcept {
        return for_each_run(source, measures.collections, measures.size(), outcome,
                            [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
                                if constexpr (!std::is_same_v<entry_of<decltype(entries)>, entry_type_>)
                                    return refused_modality(outcome);
                                else {
                                    auto store = source.view(entries);
                                    if (!store) return store.status();
                                    for (std::size_t task = first; task != end; ++task) {
                                        scan_task_t const range = measures[task];
                                        window_measure_t measured;
                                        if (range.limit != 0)
                                            if (store_status_t const measuring = measure_window<entry_type_>(
                                                    *store, range.keys, measured);
                                                st::failed(measuring))
                                                return measuring;
                                        outputs.assign(task, measured.cardinality, measured.value_bytes,
                                                       measured.space_bytes);
                                    }
                                    return store_status_t::success_k;
                                }
                            });
    });
    if (status_t const status = reported(walked, outcome); status.failed()) return status;
    outputs.finish(arguments);
    return {};
}

#pragma endregion Calls of Every Modality

#pragma region Database Calls

/** Implements @c ustore_database_describe, counting the live keys of every collection at the newest stamp. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_database_describe_t const& arguments,
               view_t const&) noexcept {
    claim_t claim;
    reader_source<cpu_class_, isolation_> source {state, state.order.take_snapshot(claim)};
    listed_collections<cpu_class_, isolation_> listed;
    auto collections = source.view(state.collections_by_id);
    if (store_status_t const collected = collect_collections<cpu_class_, isolation_>(*collections, listed);
        st::failed(collected))
        return translate(collected);
    std::size_t keys = 0;
    for (std::size_t index = 0; index != listed.size(); ++index)
        std::visit([&](auto const& entries) noexcept { keys += entries.size(); }, listed[index].store->entries);
    return assign_describe(
        arguments,
        {
            .capabilities = ustore_capabilities_t(ustore_capabilities_transactions_k | ustore_capabilities_snapshots_k),
            .engine = ustore_engine_ram_k,
            .isolation = isolation_ == isolation_t::snapshot_k ? ustore_isolation_snapshot_k
                                                               : ustore_isolation_strict_serializable_k,
            .keys = keys,
            .disk_bytes = image_bytes(state),
        });
}

/** Implements @c ustore_database_clear, persisting a change of the names as a drop of one collection does. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_database_clear_t const& arguments,
               view_t const&) noexcept {
    if (status_t const status = validate_drop_mode(arguments.mode); status.failed()) return status;
    if (arguments.mode == ustore_drop_pairs_k) return clear_pairs(state);
    if (arguments.mode == ustore_drop_values_k) return clear_values(state);
    if (status_t const status = reset_database(state); status.failed()) return status;
    return persist(state);
}

/** Implements @c ustore_database_compact, reclaiming unreachable versions of every store, freeing
 *  unreachable retired stores and rewriting the image, reporting one unit per collection, as the
 *  versions it frees carry no byte count. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_database_compact_t const& arguments,
               view_t const&) noexcept {
    claim_t claim;
    reader_source<cpu_class_, isolation_> source {state, state.order.take_snapshot(claim)};
    listed_collections<cpu_class_, isolation_> listed;
    auto collections = source.view(state.collections_by_id);
    if (store_status_t const collected = collect_collections<cpu_class_, isolation_>(*collections, listed);
        st::failed(collected))
        return translate(collected);
    progress_t progress {arguments.progress, arguments.progress_state, listed.size()};
    for (std::size_t index = 0; index != listed.size(); ++index) {
        store_status_t const vacuumed = std::visit([](auto& entries) noexcept { return entries.vacuum().status(); },
                                                   listed[index].store->entries);
        if (st::failed(vacuumed)) return translate(vacuumed);
        if (!progress.advance(1)) return progress_cancelled_k;
    }
    if (store_status_t const vacuumed = state.collections_by_id.vacuum().status(); st::failed(vacuumed))
        return translate(vacuumed);
    if (store_status_t const vacuumed = state.collection_ids_by_name.vacuum().status(); st::failed(vacuumed))
        return translate(vacuumed);
    claim.retire();
    free_unreachable(state);
    return persist(state);
}

#pragma endregion Database Calls

#pragma region Snapshot Calls

/** Implements @c ustore_snapshot_init with a claim on the newest published stamp. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_snapshot_init_t const& arguments,
               view_t const&) noexcept {
    if (!arguments.snapshot) return {status_code_t::invalid_argument_k, "Need an output for the snapshot"};
    auto [made, status] = make_handle<ustore_snapshot_handle_t, snapshot_state<cpu_class_, isolation_>>(
        ram_variant<cpu_class_, isolation_>::engine_k, state);
    if (status.failed()) return status;
    state.order.take_snapshot(made.state->claim);
    count_snapshot_opened<cpu_class_>(state.open_snapshots);
    if (arguments.read_stamp) *arguments.read_stamp = made.state->claim.snapshot();
    *arguments.snapshot = made.release();
    return {};
}

/** Implements @c ustore_snapshot_save as an image in the directory it names. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_snapshot_save_t const& arguments,
               view_t const& view) noexcept {
    sz::string_t path;
    if (path.assign(sz::string_view_t {string_of(arguments.path)}) != sz::status_t::success_k)
        return {status_code_t::out_of_memory_k, "Failed to copy the save path"};
    if (path.empty() || !names_directory(path.c_str()))
        return {status_code_t::not_found_k, "The save path isn't a directory"};
    return write_image(state, state_of<snapshot_state<cpu_class_, isolation_>>(view.snapshot)->claim.snapshot(),
                       path.c_str(), arguments.progress, arguments.progress_state);
}

#pragma endregion Snapshot Calls

#pragma region Collection Calls

/** Implements @c ustore_collection_list as a walk of the collections system collection at the head
 *  or at a snapshot, whose entries carry the modalities and the names. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_collection_list_t const& arguments,
               view_t const& view) noexcept {
    if (!arguments.written) return {status_code_t::invalid_argument_k, "Need an output for the collections written"};
    collection_list_outputs_t outputs {view.scratch};
    status_t outcome;
    store_status_t const walked = with_source(state, view, [&](auto& source) noexcept {
        return source.collections().range_from(
            ustore_collection_id_t {},
            [&](collections_pair_of<cpu_class_, isolation_> const& pair) noexcept -> walk_control_t {
                outcome = outputs.push(pair.key, modality_of(*pair.mapped.store), view_of(pair.mapped.name));
                return outcome.failed() ? walk_control_t::halt_k : walk_control_t::resume_k;
            });
    });
    if (status_t const status = reported(walked, outcome); status.failed()) return status;
    return outputs.finish(arguments);
}

/** Implements @c ustore_collection_create as one commit of the entry, its fresh store and the name,
 *  a named collection persisted at once as a flushed write would be. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_collection_create_t const& arguments,
               view_t const&) noexcept {
    auto [name, name_status] = validate_collection_create(arguments);
    if (name_status.failed()) return name_status;
    auto [created, status] = mint_collection_id();
    if (status.failed()) return status;

    // Two creations of one name both write it, so the later commit conflicts and its retry finds the name taken
    store_status_t const committed = commit_collections_change(
        state, [&](transaction_source<cpu_class_, isolation_>& source,
                   collections_change<cpu_class_, isolation_>& change) noexcept {
            return stage_collection(source, change, created, arguments.modality, name);
        });
    if (committed == store_status_t::key_already_exists_k)
        return {status_code_t::already_exists_k, "A collection already carries the name"};
    if (st::failed(committed)) return translate(committed);
    *arguments.collection = created;
    if (!name.empty()) return persist(state);
    return {};
}

/** Implements @c ustore_collection_drop: forgetting the collection and retiring its store, giving
 *  it a fresh empty store, or emptying its values. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_collection_drop_t const& arguments,
               view_t const&) noexcept {
    ustore_collection_id_t const collection = arguments.collection;
    ustore_drop_mode_t const mode = arguments.mode;
    if (status_t const status = validate_drop_mode(mode); status.failed()) return status;
    if (mode == ustore_drop_collection_k &&
        atomic_ref_of<cpu_class_, std::size_t>(state.open_snapshots).load(std::memory_order_acquire) != 0)
        return {status_code_t::invalid_argument_k, "A drop waits until every snapshot is freed"};
    status_t outcome;
    store_status_t dropped = store_status_t::success_k;
    if (mode == ustore_drop_collection_k)
        dropped = commit_collections_change(state, [&](transaction_source<cpu_class_, isolation_>& source,
                                                       collections_change<cpu_class_, isolation_>& change) noexcept {
            return stage_forget(source, change, collection);
        });
    else if (mode == ustore_drop_pairs_k)
        dropped = commit_collections_change(state, [&](transaction_source<cpu_class_, isolation_>& source,
                                                       collections_change<cpu_class_, isolation_>& change) noexcept {
            return stage_emptied(source, change, collection);
        });
    else
        dropped = commit_group(state, [&](transaction_source<cpu_class_, isolation_>& source) noexcept {
            return stage_values_emptied(source, collection, outcome);
        });
    if (dropped == store_status_t::key_not_found_k && outcome.succeeded())
        return {status_code_t::not_found_k, "No collection has such identifier"};
    if (status_t const status = reported(dropped, outcome); status.failed()) return status;
    if (mode == ustore_drop_collection_k) return persist(state);
    return {};
}

/** Implements @c ustore_collection_find, reading both system collections at the newest stamp or at a snapshot. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_collection_find_t const& arguments,
               strings_arg_t const& names, view_t const& view) noexcept {
    if (!arguments.count) return {};
    auto [outputs, outputs_status] = validate_collection_find(arguments, names);
    if (outputs_status.failed()) return outputs_status;

    status_t const resolved = translate(with_source(state, view, [&](auto& source) noexcept -> store_status_t {
        auto named = source.view(state.collection_ids_by_name);
        if (!named) return named.status();
        auto const collections = source.collections();
        for (std::size_t index = 0; index != arguments.count; ++index) {
            auto [key, key_status] = string_key_from(string_of(names[index]));
            if (st::failed(key_status)) return key_status;
            ustore_collection_id_t id {};
            bool present = false;
            store_status_t const looked_up = named->find(
                key, [&](string_pair_t const& pair) noexcept { id = ustore_collection_id_t {pair.mapped.uuid}; },
                st::no_op_t {});
            if (st::failed(looked_up)) return looked_up;
            ustore_modality_t modality {};
            if (id != ustore_collection_id_t {}) {
                store_status_t const listed = collections.find(
                    id,
                    [&](collections_pair_of<cpu_class_, isolation_> const& pair) noexcept {
                        present = true;
                        modality = modality_of(*pair.mapped.store);
                    },
                    st::no_op_t {});
                if (st::failed(listed)) return listed;
            }
            outputs.assign(index, present ? id : ustore_collection_id_t {}, modality, present);
        }
        return store_status_t::success_k;
    }));
    if (resolved.failed()) return resolved;
    outputs.finish(arguments);
    return {};
}

#pragma endregion Collection Calls

#pragma region Transaction Calls

/** Implements @c ustore_transaction_init, opening a transaction or resetting the one it reuses onto
 *  a fresh snapshot. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_transaction_init_t const& arguments,
               view_t const& view) noexcept {
    if (status_t const status = validate_transaction_begin(arguments.transaction); status.failed()) return status;
    if (view.transaction) {
        auto* const transaction = state_of<transaction_state<cpu_class_, isolation_>>(view.transaction);
        store_status_t const reset = transaction->transaction.reset();
        free_unreachable(transaction->state);
        return translate(reset);
    }
    auto opened = collections_transaction_of<cpu_class_, isolation_>::make(state.order);
    if (!opened) return translate(opened.status());
    auto [made, status] = make_handle<ustore_transaction_handle_t, transaction_state<cpu_class_, isolation_>>(
        ram_variant<cpu_class_, isolation_>::engine_k, state, std::move(*opened));
    if (status.failed()) return status;
    *arguments.transaction = made.release();
    return {};
}

/** Stages @p transaction unless it already is. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
store_status_t stage_once(collections_transaction_of<cpu_class_, isolation_>& transaction) noexcept {
    if (transaction.staging() == staging_t::staged_k) return store_status_t::success_k;
    return transaction.stage();
}

/** Implements @c ustore_transaction_stage. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>&, ustore_transaction_stage_t const& arguments,
               view_t const& view) noexcept {
    auto [state, status] = transaction_state_of<transaction_state<cpu_class_, isolation_>>(view.transaction);
    if (status.failed()) return status;
    auto& transaction = state->transaction;
    if (store_status_t const staged = stage_once<cpu_class_, isolation_>(transaction); st::failed(staged))
        return translate(staged);
    if (arguments.read_stamp) *arguments.read_stamp = transaction.snapshot();
    return {};
}

/** Implements @c ustore_transaction_commit, staging first when the caller did not. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_transaction_commit_t const& arguments,
               view_t const& view) noexcept {
    if (status_t const status = validate_transaction_commit(view.transaction, arguments.durability); status.failed())
        return status;
    auto& transaction = state_of<transaction_state<cpu_class_, isolation_>>(view.transaction)->transaction;
    if (store_status_t const staged = stage_once<cpu_class_, isolation_>(transaction); st::failed(staged))
        return translate(staged);
    if (store_status_t const committed = transaction.commit(); st::failed(committed)) return translate(committed);
    if (arguments.commit_stamp) *arguments.commit_stamp = transaction.commit_stamp();
    free_unreachable(state);
    if (arguments.durability == ustore_durability_flushed_k) return persist(state);
    return {};
}

/** Implements @c ustore_transaction_rollback, pulling the staged writes back into the transaction,
 *  which may then stage again, while one never staged has nothing to withdraw. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>&, ustore_transaction_rollback_t const&,
               view_t const& view) noexcept {
    auto [state, status] = transaction_state_of<transaction_state<cpu_class_, isolation_>>(view.transaction);
    if (status.failed()) return status;
    auto& transaction = state->transaction;
    if (transaction.staging() != staging_t::staged_k) return {};
    return translate(transaction.rollback());
}

#pragma endregion Transaction Calls

#pragma region Change Feed Calls

/** What a feed holds, behind its handle of the C ABI: the store of the collection it reads, a claim
 *  at or below the stamp it reads after, which keeps every version committed above, and the change
 *  it handed out last. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
struct changes_feed_state {

    /** The database the feed reads. */
    database_state<cpu_class_, isolation_>& state;

    /** The store of the collection, which the claim keeps alive while it is retired. */
    collection_store_of<cpu_class_, isolation_>* store {nullptr};

    /** The claim at or below @c since_stamp. */
    claim_t claim;

    /** The stamp the feed reads after. */
    generation_t since_stamp {0};

    /** The stamp of the change handed out last, or @c since_stamp before the first pull. */
    generation_t last_stamp {0};

    /** The key of the change handed out last, or the largest key before the first pull. */
    ustore_key_t last_key {largest_key_k};

    explicit changes_feed_state(database_state<cpu_class_, isolation_>& owner) noexcept : state(owner) {}
};

/** The store @p collection lives in at the newest stamp of @p state. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
expected<collection_store_of<cpu_class_, isolation_>*> store_at_head(
    database_state<cpu_class_, isolation_>& state, ustore_collection_id_t const& collection) noexcept {
    claim_t claim;
    reader_source<cpu_class_, isolation_> source {state, state.order.take_snapshot(claim)};
    resolved_collection<cpu_class_, isolation_> resolved;
    store_status_t const listed = resolve(source, collection, resolved);
    if (listed == store_status_t::key_not_found_k) return status_t {status_code_t::not_found_k, "No such collection"};
    if (st::failed(listed)) return translate(listed);
    return resolved.store;
}

/** Points the claim of @p feed at or below @p since_stamp: shared with the open feed of @p state
 *  claiming the newest stamp no later, or the newest stamp itself when that is @p since_stamp,
 *  answering whether it could. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
store_status_t claim_since(database_state<cpu_class_, isolation_>& state,
                           changes_feed_state<cpu_class_, isolation_>& feed, generation_t since_stamp) noexcept {
    // No claim is safe to take at an older stamp, so a feed shares the claim of one opened no later
    changes_feed_state<cpu_class_, isolation_>* shared = nullptr;
    for (std::size_t index = 0; index != state.open_feeds.size(); ++index) {
        changes_feed_state<cpu_class_, isolation_>* const open = state.open_feeds[index];
        if (open->claim.snapshot() > since_stamp) continue;
        if (!shared || shared->claim.snapshot() < open->claim.snapshot()) shared = open;
    }
    if (shared) state.order.share_snapshot(shared->claim, feed.claim);
    else if (state.order.take_snapshot(feed.claim) > since_stamp) return store_status_t::key_not_found_k;
    return state.open_feeds.push_back(&feed);
}

/** Implements @c ustore_changes_cursor_init, claiming a stamp at or below @c since_stamp, so every
 *  version committed above stays, and refusing a stamp no claim keeps the history above of, and a
 *  strings collection, whose keys are strings. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t changes_cursor_init(ustore_changes_cursor_init_t const& arguments, ustore_changes_cursor_t cursor,
                             view_t const&) noexcept {
    database_state<cpu_class_, isolation_>& state = *state_of<database_state<cpu_class_, isolation_>>(
        arguments.database);
    auto [store, store_status] = store_at_head(state, arguments.collection);
    if (store_status.failed()) return store_status;
    if (modality_of(*store) == ustore_modality_strings_k)
        return {status_code_t::unsupported_k,
                "A strings collection keys its changes by strings, which feeds can't hand"};
    auto* const feed = new (std::nothrow) changes_feed_state<cpu_class_, isolation_> {state};
    if (!feed) return {status_code_t::out_of_memory_k, "Failed to allocate the change feed"};
    generation_t const since_stamp = static_cast<generation_t>(arguments.since_stamp);
    store_status_t claimed = store_status_t::success_k;
    {
        st::unique_lock<typename database_state<cpu_class_, isolation_>::feeds_mutex_t> const lock {state.feeds_mutex};
        claimed = claim_since(state, *feed, since_stamp);
    }
    if (claimed == store_status_t::key_not_found_k) {
        delete feed;
        return {status_code_t::write_conflict_k, "The changes after the stamp are already gone"};
    }
    if (st::failed(claimed)) {
        delete feed;
        return translate(claimed);
    }
    feed->store = store;
    feed->since_stamp = since_stamp;
    feed->last_stamp = since_stamp;
    cursor->state = feed;
    return {};
}

/** One change a pull hands out, as the walk of the versions found it. */
struct found_change_t {
    generation_t stamp {0};
    ustore_key_t key {};
    ustore_change_kind_t kind {ustore_change_write_k};
    value_view_t value;
};

/** How strongly a vertex change speaks for its commit, as one commit changes entries often. */
constexpr unsigned change_rank(ustore_change_kind_t kind) noexcept {
    return kind == ustore_change_removal_k ? 2 : kind == ustore_change_write_k ? 1 : 0;
}

/** The change a version of a half-edge makes: a write or a removal of its vertex for the vertex's
 *  own entry, and a change of the vertex for any other. */
inline found_change_t change_of(half_edge_t const& entry, half_edge_t const&, generation_t stamp,
                                st::presence_t presence) noexcept {
    if (entry.direction != half_edge_direction_t::vertex_k) return {stamp, entry.vertex, ustore_change_change_k, {}};
    bool const present = presence == st::presence_t::present_k;
    return {stamp, entry.vertex, present ? ustore_change_write_k : ustore_change_removal_k, {}};
}

/** The change a version of a blob, a document or a page makes: writing its bytes, or a removal. */
template <typename mapped_type_>
found_change_t change_of(ustore_key_t const& key, st::mapping<ustore_key_t, mapped_type_> const& pair,
                         generation_t stamp, st::presence_t presence) noexcept {
    if (presence != st::presence_t::present_k) return {stamp, key, ustore_change_removal_k, {}};
    return {stamp, key, ustore_change_write_k, value_of(pair)};
}

/** Keeps @p change in @p found, ordered by stamp and then by key, holding the @p wanted smallest,
 *  where a second change of one key in one commit merges into the first. */
inline status_t keep_smallest(st::vector<found_change_t, arena_allocator<found_change_t>>& found,
                              found_change_t const& change, std::size_t wanted) noexcept {
    found_change_t* const first = found.data();
    found_change_t* const end = first + found.size();
    found_change_t* const position = std::partition_point(first, end, [&](found_change_t const& kept) noexcept {
        return kept.stamp != change.stamp ? kept.stamp < change.stamp : kept.key < change.key;
    });
    if (position != end && position->stamp == change.stamp && position->key == change.key) {
        if (change_rank(change.kind) > change_rank(position->kind)) position->kind = change.kind;
        return {};
    }
    std::size_t const index = static_cast<std::size_t>(position - first);
    if (index == wanted) return {};
    if (status_t const inserted = growth_status(found.insert(index, found_change_t {change})); inserted.failed())
        return inserted;
    if (found.size() > wanted) found.pop_back();
    return {};
}

/** Collects into @p found the @p wanted smallest changes of @p entries committed after @p feed's
 *  last change and at or below @p read_stamp, telling a document or a page written over an earlier
 *  one as changed. */
template <cpu_class_t cpu_class_, isolation_t isolation_, typename store_type_>
status_t find_changes(changes_feed_state<cpu_class_, isolation_> const& feed, store_type_ const& entries,
                      generation_t read_stamp, std::size_t wanted,
                      st::vector<found_change_t, arena_allocator<found_change_t>>& found) noexcept {
    using entry_t = entry_of<store_type_>;
    using identifier_t = typename store_type_::identifier_t;
    // A pull resumes inside the commit of the last change it handed out
    generation_t const after = feed.last_stamp > feed.since_stamp ? feed.last_stamp - 1 : feed.since_stamp;
    status_t kept;
    store_status_t const walked = entries.reader_at(read_stamp)
                                      .for_each_version_after(
                                          after,
                                          [&](identifier_t const& key, generation_t stamp, st::presence_t presence,
                                              entry_t const& value) noexcept -> walk_control_t {
                                              found_change_t const change = change_of(key, value, stamp, presence);
                                              if (change.stamp < feed.last_stamp ||
                                                  (change.stamp == feed.last_stamp && !(feed.last_key < change.key)))
                                                  return walk_control_t::resume_k;
                                              kept = keep_smallest(found, change, wanted);
                                              return kept.failed() ? walk_control_t::halt_k : walk_control_t::resume_k;
                                          });
    if (st::failed(walked)) return translate(walked);
    if (kept.failed()) return kept;
    if constexpr (std::is_same_v<entry_t, docs::document_pair_t> || std::is_same_v<entry_t, pages::page_pair_t>)
        for (std::size_t index = 0; index != found.size(); ++index) {
            if (found[index].kind != ustore_change_write_k) continue;
            bool existed = false;
            store_status_t const looked_up =
                entries.reader_at(found[index].stamp - 1)
                    .find(found[index].key, [&](entry_t const&) noexcept { existed = true; }, st::no_op_t {});
            if (st::failed(looked_up)) return translate(looked_up);
            if (existed) found[index].kind = ustore_change_change_k;
        }
    return {};
}

/** Implements @c ustore_changes_cursor_next, walking the versions committed after the last change
 *  handed out and handing out the smallest by stamp and key. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t changes_cursor_next(ustore_changes_cursor_next_t const& arguments, view_t const& view) noexcept {
    auto* const feed = state_of<changes_feed_state<cpu_class_, isolation_>>(arguments.cursor);
    auto [store, store_status] = store_at_head(feed->state, arguments.cursor->cursor.collection);
    if (store_status.failed()) return store_status;
    if (store != feed->store) return {status_code_t::not_found_k, "The collection was dropped meanwhile"};
    claim_t claim;
    generation_t const read_stamp = feed->state.order.take_snapshot(claim);
    std::size_t const wanted = static_cast<std::size_t>(arguments.wanted);
    st::vector<found_change_t, arena_allocator<found_change_t>> found {{view.scratch}};
    status_t const collected = std::visit(
        [&](auto const& entries) noexcept -> status_t {
            if constexpr (std::is_same_v<entry_of<decltype(entries)>, string_pair_t>) return {};
            else return find_changes(*feed, entries, read_stamp, wanted, found);
        },
        feed->store->entries);
    if (collected.failed()) return collected;

    std::size_t const count = found.size();
    auto [keys, keys_status] = arena_allocate<ustore_key_t>(arguments.arena, count);
    if (keys_status.failed()) return keys_status;
    auto [stamps, stamps_status] = arena_allocate<ustore_stamp_t>(arguments.arena, count);
    if (stamps_status.failed()) return stamps_status;
    auto [kinds, kinds_status] = arena_allocate<ustore_change_kind_t>(arguments.arena, count);
    if (kinds_status.failed()) return kinds_status;
    std::size_t value_bytes = 0;
    for (std::size_t index = 0; index != count; ++index) {
        keys[index] = found[index].key;
        stamps[index] = static_cast<ustore_stamp_t>(found[index].stamp);
        kinds[index] = found[index].kind;
        value_bytes += found[index].value.size();
    }
    if (arguments.values) {
        auto const values = [&](std::size_t index) noexcept { return found[index].value.bytes(); };
        if (status_t const status = export_strings(arguments.arena, count, value_bytes, values, *arguments.values);
            status.failed())
            return status;
    }
    if (count) {
        feed->last_stamp = found[count - 1].stamp;
        feed->last_key = found[count - 1].key;
    }
    *arguments.count = count;
    *arguments.keys = keys.data();
    *arguments.stamps = stamps.data();
    *arguments.kinds = kinds.data();
    return {};
}

/** Frees the feed behind @p cursor, releasing its claim, and the stores only it still reached. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
void changes_cursor_free(ustore_changes_cursor_t cursor) noexcept {
    auto* const feed = state_of<changes_feed_state<cpu_class_, isolation_>>(cursor);
    database_state<cpu_class_, isolation_>& state = feed->state;
    {
        st::unique_lock<typename database_state<cpu_class_, isolation_>::feeds_mutex_t> const lock {state.feeds_mutex};
        std::size_t position = 0;
        while (state.open_feeds[position] != feed) ++position;
        state.open_feeds.erase(position);
    }
    delete feed;
    free_unreachable(state);
}

#pragma endregion Change Feed Calls

#pragma region Table

/** Serves a call on the database @p arguments address, whose handle the table of @p cpu_class_ at
 *  @p isolation_ stamped, with the ragged inputs the router gathered for it. */
template <cpu_class_t cpu_class_, isolation_t isolation_, typename arguments_type_, typename... strings_types_>
status_t serve_database(arguments_type_ const& arguments, strings_types_ const&... strings,
                        view_t const& view) noexcept {
    return serve(*state_of<database_state<cpu_class_, isolation_>>(database_of(arguments)), arguments, strings...,
                 view);
}

/** Frees the transaction behind @p handle, releasing the stamp it claimed, and the stores only it still reached. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
void transaction_free(ustore_transaction_t handle) noexcept {
    auto* const transaction = state_of<transaction_state<cpu_class_, isolation_>>(handle);
    database_state<cpu_class_, isolation_>& state = transaction->state;
    delete transaction;
    free_unreachable(state);
}

/** Gives back the count the snapshot behind @p handle holds and frees it, releasing the stamp it
 *  claimed, and the stores only it still reached. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
void snapshot_free(ustore_snapshot_t handle) noexcept {
    auto* const snapshot = state_of<snapshot_state<cpu_class_, isolation_>>(handle);
    database_state<cpu_class_, isolation_>& state = snapshot->state;
    [[maybe_unused]] bool const last = count_snapshot_closed<cpu_class_>(state.open_snapshots);
    delete snapshot;
    free_unreachable(state);
}

/** Persists @p database ahead of freeing it, which every snapshot and transaction of it must precede. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
void database_free(ustore_database_t database) noexcept {
    auto* const state = state_of<database_state<cpu_class_, isolation_>>(database);
    [[maybe_unused]] status_t const persisted = persist(*state);
    delete state;
}

template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t ram_variant<cpu_class_, isolation_>::open(ustore_database_t* database, std::string_view directory) noexcept {
    auto [made, status] = make_handle<ustore_database_handle_t, database_state<cpu_class_, isolation_>>(
        ram_variant<cpu_class_, isolation_>::engine_k);
    if (status.failed()) return status;
    if (status_t const opened = open_database(*made.state, directory); opened.failed()) return opened;
    *database = made.release();
    return {};
}

#pragma endregion Table

} // namespace unum::ustore::ram
