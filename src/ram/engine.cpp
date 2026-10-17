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

#include <algorithm>   // `std::max`
#include <array>       // `std::array`
#include <atomic>      // `std::memory_order`
#include <chrono>      // `std::chrono::steady_clock`
#include <limits>      // `std::numeric_limits`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::is_same_v`, `std::remove_cvref_t`
#include <utility>     // `std::exchange`, `std::forward`, `std::move`
#include <variant>     // `std::in_place_type`, `std::variant`, `std::visit`

#include <forkunion/atomics.hpp> // `fu::preferred_atomic_ref`, `fu::standard_atomic_ref`
#include <smashtable/basic_avl_tree.hpp>
#include <smashtable/basic_vector.hpp>
#include <stringzilla/stringzilla.hpp>

#include "ustore/blobs.h"
#include "ustore/db.h"
#include "ustore/graph.h"
#include "ustore/ranges.hpp" // `place_batch_t`, `edges_view_t`, `strided`
#include "ustore/strings.h"

#include "ustore/blobs/ram_store.hpp"          // `collection_store`, `collections_transaction`, `translate`
#include "ustore/graph/half_edges.hpp"         // `half_edge_t`, `half_edge_direction_t`
#include "ustore/graph/ram_graph.hpp"          // `export_edges`, `stage_edges`, `stage_vertex_removal`
#include "ustore/strings/string_keys.hpp"      // `string_key_t`, `blob_t`
#include "ustore/utilities/blocking_files.hpp" // `directory_exists`
#include "ustore/utilities/ram_buffers.hpp"    // `arena_allocate`, `arena_copy`, `arena_allocator`

#include "engine.hpp"             // `engine_t`, `handle_t`, `handle_cast`, `view_t`
#include "utilities/batches.hpp"  // `scan_batch_t`, `strings_arg_t`, `edges_batch_t`, `places_of`, `edges_of`
#include "utilities/outputs.hpp"  // `read_outputs_t`, `scan_outputs_t`, `collection_list_outputs_t`
#include "utilities/progress.hpp" // `progress_t`, `progress_cancelled_k`

namespace unum::ustore::ram {
using namespace unum::ustore::blobs;
using namespace unum::ustore::graph;
using namespace unum::ustore::strings;
using namespace unum::ustore::utilities;

#pragma region Files

/** Whether @p path names an existing directory. */
inline bool names_directory(char const* path) noexcept {
    expected<bool> const exists = directory_exists(path);
    return exists && *exists;
}

#pragma endregion Files

#pragma region Handles

/** The calls this engine serves at @p isolation_, which stamp every handle a database at it hands out, so the router
 *  refuses a handle of the other isolation. */
template <isolation_t isolation_>
extern engine_t const engine_k;

/** Everything a database at @p isolation_ holds, which is the database handle of the C ABI. */
template <isolation_t isolation_>
struct database_state : handle_t {

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

    database_state() noexcept : handle_t {&engine_k<isolation_>} {}
    database_state(database_state const&) = delete;
    database_state& operator=(database_state const&) = delete;
    ~database_state() noexcept;
};

/** A transaction handle of the C ABI. */
template <isolation_t isolation_>
struct transaction_handle : handle_t {

    /** The database the transaction runs against. */
    database_state<isolation_>& state;

    /** The transaction over every store it reaches. */
    collections_transaction<isolation_> transaction;

    transaction_handle(database_state<isolation_>& owner, collections_transaction<isolation_>&& opened) noexcept
        : handle_t {&engine_k<isolation_>}, state(owner), transaction(std::move(opened)) {}
};

/** A snapshot handle of the C ABI, whose claim pins the stamp every read through it answers at until it is freed. */
template <isolation_t isolation_>
struct snapshot_handle : handle_t {

    /** The database the snapshot reads. */
    database_state<isolation_>& state;

    /** The claim on the newest stamp published when the snapshot opened. */
    claim_t claim;

    explicit snapshot_handle(database_state<isolation_>& owner) noexcept
        : handle_t {&engine_k<isolation_>}, state(owner) {}
};

#pragma endregion Handles

#pragma region Retirement

/** Retires @p store at @p commit_stamp, which the catalog stopped listing it at, to be freed once no reader holds an
 *  older stamp. */
template <isolation_t isolation_>
void retire(database_state<isolation_>& state, collection_store<isolation_>* store,
            generation_t commit_stamp) noexcept {
    store->retirement_stamp = commit_stamp;
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

    store_status_t prepare(std::size_t count) const noexcept { return transaction.reserve(count); }

    template <typename key_type_>
    store_status_t watch(key_type_ const& key) const noexcept {
        return watches() ? transaction.watch(key) : store_status_t::success_k;
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
    generation_t read_stamp;

    /** A reader of @p store at the stamp. */
    template <typename store_type_>
    st::expected<reader_view<typename store_type_::reader_t>> view(store_type_& store) const noexcept {
        return reader_view<typename store_type_::reader_t> {store.reader_at(read_stamp)};
    }

    /** A reader of the catalog at the stamp, which resolves the collections a call names. */
    reader_view<typename collections_by_id_store<isolation_>::reader_t> catalog() const noexcept {
        return {state.collections_by_id.reader_at(read_stamp)};
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

/** Whether reads of a transaction at @p isolation_ watch what they hand out when a call asks for @p watch, where a
 *  strictly serializable one records every read already. */
template <isolation_t isolation_>
constexpr ustore_watch_t watches_reads(ustore_watch_t watch) noexcept {
    return isolation_ == isolation_t::snapshot_k && watch == ustore_watch_keys_k ? ustore_watch_keys_k
                                                                                 : ustore_watch_none_k;
}

/** Hands @p action the source @p view names: a snapshot, a transaction, or the newest stamp, claimed for the call. */
template <isolation_t isolation_, typename action_type_>
auto with_source(database_state<isolation_>& state, view_t const& view, action_type_&& action) noexcept {
    if (view.snapshot) {
        reader_source<isolation_> source {state,
                                          handle_cast<snapshot_handle<isolation_>>(view.snapshot)->claim.snapshot()};
        return action(source);
    }
    if (!view.transaction) {
        claim_t claim;
        reader_source<isolation_> source {state, state.order.take_snapshot(claim)};
        return action(source);
    }
    auto& transaction = handle_cast<transaction_handle<isolation_>>(view.transaction)->transaction;
    transaction_source<isolation_> source {state, transaction, watches_reads<isolation_>(view.watch)};
    return action(source);
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
                            std::size_t count, status_t& outcome, action_type_&& action) noexcept {
    for (std::size_t first = 0; first != count;) {
        ustore_collection_id_t const collection = collections ? collections[first] : ustore_collection_main_k;
        std::size_t end = first + 1;
        while (end != count && (!collections || collections[end] == collection)) ++end;
        resolved_collection<source_type_::isolation_k> resolved;
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

#pragma endregion Resolution

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

/** Watches the entries of the keys @p outputs hold from @p first on, which only a watching view records, once the
 *  walk that found them let go of its partitions. */
template <typename entry_type_, typename view_type_>
store_status_t watch_keys(view_type_ const& view, scan_outputs_t& outputs, std::size_t first) noexcept {
    if (!view.watches()) return store_status_t::success_k;
    for (std::size_t index = first; index != outputs.size(); ++index) {
        store_status_t const watched = view.watch(entry_for<entry_type_>(outputs[index]));
        if (st::failed(watched)) return watched;
    }
    return store_status_t::success_k;
}

/** Appends up to the limit of @p scan keys of a collection of @p entry_type_, from its start key on, halting with
 *  @p outcome when the scratch refuses. */
template <typename entry_type_, typename view_type_>
store_status_t scan_window(view_type_ const& view, scan_task_t scan, scan_outputs_t& outputs,
                           status_t& outcome) noexcept {
    std::size_t const first = outputs.size();
    if (scan.limit == 0) return store_status_t::success_k;
    store_status_t const walked = view.range_from(
        entry_for<entry_type_>(scan.start_key), [&](entry_type_ const& entry) noexcept -> walk_control_t {
            if (!is_listed(entry)) return walk_control_t::resume_k;
            outcome = outputs.push(listed_key(entry));
            if (outcome.failed()) return walk_control_t::halt_k;
            return outputs.size() - first == scan.limit ? walk_control_t::halt_k : walk_control_t::resume_k;
        });
    if (st::failed(walked) || outcome.failed()) return walked;
    return watch_keys<entry_type_>(view, outputs, first);
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

/** Appends a uniform reservoir of up to the limit of @p sample keys of a collection of @p entry_type_, halting with
 *  @p outcome when the scratch refuses. */
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

/** Reads the value of every string of tasks [ @p first, @p end ) of @p texts through @p view into @p outputs. */
template <typename view_type_>
store_status_t read_strings(view_type_ const& view, strings_arg_t const& texts, std::size_t first, std::size_t end,
                            read_outputs_t& outputs) noexcept {
    store_status_t const reserved = view.prepare(end - first);
    if (st::failed(reserved)) return reserved;
    auto const found = [&](string_pair_t const& pair) noexcept { outputs.append(view_of(pair.mapped)); };
    auto const missing = [&]() noexcept { outputs.append_missing(); };
    for (std::size_t index = first; index != end; ++index) {
        auto [key, key_status] = string_key_from(string_of(texts[index]));
        if (st::failed(key_status)) return key_status;
        store_status_t const looked_up = view.find(key, found, missing);
        if (st::failed(looked_up)) return looked_up;
    }
    return store_status_t::success_k;
}

/** Appends up to @p limit strings starting with @p prefix, from @p start on, to @p outputs. */
template <typename view_type_>
store_status_t scan_strings(view_type_ const& view, std::string_view prefix, std::string_view start, std::size_t limit,
                            strings_scan_outputs_t& outputs, status_t& outcome) noexcept {
    if (limit == 0) return store_status_t::success_k;
    auto [first, first_status] = string_key_from(start > prefix ? start : prefix);
    if (st::failed(first_status)) return first_status;
    std::size_t const found_before = outputs.size();
    store_status_t const walked = view.range_from(first, [&](string_pair_t const& pair) noexcept -> walk_control_t {
        std::string_view const text = pair.key.view();
        if (!text.starts_with(prefix)) return walk_control_t::halt_k;
        outcome = outputs.push(text);
        if (outcome.failed()) return walk_control_t::halt_k;
        return outputs.size() - found_before == limit ? walk_control_t::halt_k : walk_control_t::resume_k;
    });
    if (st::failed(walked) || !view.watches()) return walked;

    // A walk holds its partitions, so the strings it handed out are watched once it let go of them
    for (std::size_t index = found_before; index != outputs.size() && outcome.succeeded(); ++index) {
        auto [key, key_status] = string_key_from(outputs[index]);
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
store_status_t stage_erase(transaction_type_& transaction, key_type_ const& key, ustore_watch_t watch) noexcept {
    store_status_t const erased = transaction.erase(key);
    if (erased != store_status_t::key_not_found_k) return erased;
    if (watch == ustore_watch_none_k) return store_status_t::success_k;
    return transaction.watch(key);
}

/** Stages the values of tasks [ @p first, @p end ) of a batch into @p transaction. */
template <typename transaction_type_>
store_status_t stage_writes(transaction_type_& transaction, place_batch_t const& places, strings_arg_t const& values,
                            std::size_t first, std::size_t end) noexcept {
    for (std::size_t index = first; index != end; ++index) {
        store_status_t const staged = stage_upsert(transaction, places[index].key, values[index]);
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

/** Stages the values of tasks [ @p first, @p end ) of a batch of strings into @p transaction. */
template <typename transaction_type_>
store_status_t stage_strings(transaction_type_& transaction, strings_arg_t const& texts, strings_arg_t const& values,
                             std::size_t first, std::size_t end) noexcept {
    for (std::size_t index = first; index != end; ++index) {
        auto [key, key_status] = string_key_from(string_of(texts[index]));
        if (st::failed(key_status)) return key_status;
        store_status_t const staged = stage_upsert(transaction, std::move(key), values[index]);
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

/**
 *  @brief Commits what @p stage_changes stages through a transaction of its own, retrying conflicts up to
 *      @c commit_attempts_k times.
 *  @param[out] commit_stamp Optional output for the stamp the commit published under.
 *
 *  @p stage_changes takes a @c transaction_source and may refuse the call through its outcome, answering a failure
 *  that is not a conflict, so no attempt follows.
 */
template <isolation_t isolation_, typename stage_changes_type_>
store_status_t commit_group(database_state<isolation_>& state, stage_changes_type_&& stage_changes,
                            generation_t* commit_stamp = nullptr) noexcept {
    auto opened = collections_transaction<isolation_>::make(state.order);
    if (!opened) return opened.status();
    transaction_source<isolation_> source {state, *opened, ustore_watch_none_k};
    store_status_t const committed = st::commit_with_retries(
        *opened, commit_attempts_k,
        [&](collections_transaction<isolation_>&) noexcept -> store_status_t { return stage_changes(source); });
    if (commit_stamp && st::succeeded(committed)) *commit_stamp = opened->commit_stamp();
    return committed;
}

#pragma endregion Writes

#pragma region Catalog

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
    generation_t commit_stamp = 0;
    store_status_t const committed = commit_group(
        state,
        [&](transaction_source<isolation_>& source) noexcept -> store_status_t {
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
                                    status_t& outcome) noexcept {
    strided_iterator<ustore_collection_id_t const> const one {&collection, sizeof(collection)};
    return for_each_run(source, one, 1, outcome, [&](auto& entries, std::size_t, std::size_t) noexcept {
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
    return path.assign(sz::string_view_t {directory}) == sz::status_t::success_k &&
           path.append(name.data(), name.size()) == sz::status_t::success_k;
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

/** Bytes of the key and the value of an entry, which the progress of a save counts, a half-edge being all key. */
inline std::size_t saved_bytes(value_pair_t const& pair) noexcept { return sizeof(ustore_key_t) + pair.mapped.size(); }
inline std::size_t saved_bytes(string_pair_t const& pair) noexcept { return pair.key.text.size() + pair.mapped.size(); }
inline std::size_t saved_bytes(half_edge_t const&) noexcept { return sizeof(half_edge_t); }

/** Counts into @p count the entries @p store holds at @p read_stamp, and adds the bytes of their keys and values to
 *  @p bytes. */
template <typename store_type_>
store_status_t tally_entries(store_type_ const& store, generation_t read_stamp, std::uint64_t& count,
                             std::size_t& bytes) noexcept {
    using entry_t = entry_of<store_type_>;
    return store.reader_at(read_stamp).range_from(first_entry<entry_t>(), [&](entry_t const& entry) noexcept {
        ++count;
        bytes += saved_bytes(entry);
    });
}

/** Writes how many entries @p store holds at @p read_stamp, then every one of them, adding the bytes of their keys and
 *  values to @p bytes, and clearing @p written when any of it misses @p file. */
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

/** Writes the image of every collection of @p state at @p read_stamp into @p directory, reporting the bytes of the keys
 *  and values written to @p progress after each collection, whose cancellation removes the partial image. */
template <isolation_t isolation_>
status_t write_image(database_state<isolation_>& state, generation_t read_stamp, char const* directory,
                     ustore_progress_callback_t progress = nullptr, void* progress_state = nullptr) noexcept {
    sz::string_t partial_path;
    sz::string_t final_path;
    if (!join_path(partial_path, directory, partial_image_name_k) || !join_path(final_path, directory, image_name_k))
        return {status_code_t::out_of_memory_k, "Failed to build the image path"};
    listed_collections<isolation_> listed;
    reader_source<isolation_> source {state, read_stamp};
    auto catalog = source.view(state.collections_by_id);
    if (store_status_t const collected = collect_catalog<isolation_>(*catalog, listed); st::failed(collected))
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

    bool written = std::fwrite(image_signature_k, sizeof(image_signature_k), 1, stream.get()) == 1;
    for (std::size_t index = 0; index != listed.size() && written; ++index) {
        listed_collection<isolation_> const& collection = listed[index];
        auto const modality = static_cast<std::uint8_t>(collection.modality);
        written = std::fwrite(&collection.id, sizeof(ustore_collection_id_t), 1, stream.get()) == 1 &&
                  std::fwrite(&modality, sizeof(modality), 1, stream.get()) == 1 &&
                  write_blob(stream.get(), collection.name);
        std::size_t bytes = 0;
        store_status_t const walked = std::visit(
            [&](auto const& entries) noexcept {
                return write_entries(stream.get(), entries, read_stamp, bytes, written);
            },
            collection.store->entries);
        if (st::failed(walked)) return translate(walked);
        if (written && !reporter.advance(bytes)) {
            [[maybe_unused]] bool const closed = stream.close();
            [[maybe_unused]] int const removed = std::remove(partial_path.c_str());
            return progress_cancelled_k;
        }
    }

    bool const synced = written && std::fflush(stream.get()) == 0 && sync_file(stream.get());
    bool const closed = stream.close();
    if (!synced || !closed) return {status_code_t::io_k, "Failed to write the image"};
    if (!replace_file(partial_path.c_str(), final_path.c_str()))
        return {status_code_t::io_k, "Failed to replace the image"};
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
status_t read_image(database_state<isolation_>& state, collection_store<isolation_>* main) noexcept {
    sz::string_t path;
    if (!join_path(path, state.directory.c_str(), image_name_k))
        return {status_code_t::out_of_memory_k, "Failed to build the image path"};
    stream_t stream {std::fopen(path.c_str(), "rb")};
    if (!stream.get()) return {};
    char signature[sizeof(image_signature_k)];
    if (std::fread(signature, sizeof(signature), 1, stream.get()) != 1 ||
        std::memcmp(signature, image_signature_k, sizeof(signature)) != 0)
        return {status_code_t::malformed_k, "The image has an unknown format"};
    image_step_t step = image_step_t::loaded_k;
    while (step == image_step_t::loaded_k) step = load_collection(stream.get(), state, main);
    return image_step_statuses_k[static_cast<std::size_t>(step)];
}

/** Rewrites the image of @p state from its newest published commit, when it has a directory. */
template <isolation_t isolation_>
status_t persist(database_state<isolation_>& state) noexcept {
    if (state.directory.empty()) return {};
    claim_t claim;
    return write_image(state, state.order.take_snapshot(claim), state.directory.c_str());
}

#pragma endregion Image

#pragma region Lifetime

/** Lists the main collection in the catalog of @p state, then remembers the directory and loads its image. */
template <isolation_t isolation_>
status_t open_database(database_state<isolation_>& state, std::string_view directory) noexcept {
    collection_store<isolation_>* const main = allocate_store<isolation_>(state.order, ustore_modality_any_k);
    if (!main) return {status_code_t::out_of_memory_k, "Failed to allocate the main collection"};
    catalog_pair<isolation_> listed {ustore_collection_main_k,
                                     collection_entry<isolation_> {ustore_modality_any_k, {}, main}};
    if (st::failed(state.collections_by_id.upsert(std::move(listed)))) {
        delete main;
        return {status_code_t::out_of_memory_k, "Failed to list the main collection"};
    }
    if (directory.empty()) return {};
    if (state.directory.assign(sz::string_view_t {directory}) != sz::status_t::success_k)
        return {status_code_t::out_of_memory_k, "Failed to copy the directory"};
    if (!names_directory(state.directory.c_str())) return {status_code_t::not_found_k, "The directory doesn't exist"};
    return read_image(state, main);
}

/** Gives every collection a fresh empty store, keeping the catalog and the names. */
template <isolation_t isolation_>
status_t clear_pairs(database_state<isolation_>& state) noexcept {
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
    return translate(cleared);
}

/** Removes every collection but the main one, which it empties, and every name, which no open snapshot may still be
 *  reading. */
template <isolation_t isolation_>
status_t reset_database(database_state<isolation_>& state) noexcept {
    if (fu::preferred_atomic_ref<std::size_t>(state.open_snapshots).load(std::memory_order_acquire) != 0)
        return {status_code_t::invalid_argument_k, "A reset waits until every snapshot is freed"};
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
    return translate(reset);
}

/** Empties every value of every collection, keeping the keys, which a graph holds none of. */
template <isolation_t isolation_>
status_t clear_values(database_state<isolation_>& state) noexcept {
    status_t outcome;
    store_status_t const cleared = commit_group(state, [&](transaction_source<isolation_>& source) noexcept {
        auto catalog = source.view(state.collections_by_id);
        if (!catalog) return catalog.status();
        listed_collections<isolation_> listed;
        store_status_t const collected = collect_catalog<isolation_>(*catalog, listed);
        for (std::size_t index = 0; index != listed.size() && st::succeeded(collected); ++index) {
            store_status_t const emptied = stage_values_emptied(source, listed[index].id, outcome);
            if (st::failed(emptied)) return emptied;
        }
        return collected;
    });
    return reported(cleared, outcome);
}

/** Bytes the image of @p state takes on disk, zero for a volatile database or one not yet persisted. */
template <isolation_t isolation_>
std::size_t image_bytes(database_state<isolation_> const& state) noexcept {
    sz::string_t path;
    if (state.directory.empty() || !join_path(path, state.directory.c_str(), image_name_k)) return 0;
    expected<seekable_file_t> const image = seekable_file_t::open(path.c_str());
    if (!image) return 0;
    expected<std::size_t> const length = image->length();
    return length ? *length : 0;
}

#pragma endregion Lifetime

#pragma region Argument Views

/** The tasks of a scan. */
inline scan_batch_t scans_of(ustore_blobs_scan_t const& arguments) noexcept {
    return {strided(arguments.collections), strided(arguments.start_keys), arguments.wanted, arguments.count};
}

/** The tasks of a sample. */
inline sample_batch_t samples_of(ustore_blobs_sample_t const& arguments) noexcept {
    return {strided(arguments.collections), arguments.wanted, arguments.count};
}

#pragma endregion Argument Views

#pragma region Database Calls

/** Implements @c ustore_database_describe, counting the live keys of every collection at the newest stamp. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_database_describe_t const& arguments, view_t const&) noexcept {
    if (!arguments.description) return {status_code_t::invalid_argument_k, "Need an output for the description"};
    claim_t claim;
    reader_source<isolation_> source {state, state.order.take_snapshot(claim)};
    listed_collections<isolation_> listed;
    auto catalog = source.view(state.collections_by_id);
    if (store_status_t const collected = collect_catalog<isolation_>(*catalog, listed); st::failed(collected))
        return translate(collected);
    std::size_t keys = 0;
    for (std::size_t index = 0; index != listed.size(); ++index)
        std::visit([&](auto const& entries) noexcept { keys += entries.size(); }, listed[index].store->entries);
    *arguments.description = {
        .capabilities = ustore_capabilities_t(ustore_capabilities_transactions_k | ustore_capabilities_collections_k |
                                              ustore_capabilities_snapshots_k),
        .engine = ustore_engine_ram_k,
        .isolation = isolation_ == isolation_t::snapshot_k ? ustore_isolation_snapshot_k
                                                           : ustore_isolation_strict_serializable_k,
        .keys = keys,
        .disk_bytes = image_bytes(state),
    };
    return {};
}

/** Implements @c ustore_database_clear, persisting a change of the names as a drop of one collection does. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_database_clear_t const& arguments, view_t const&) noexcept {
    switch (arguments.mode) {
    case ustore_drop_pairs_k: return clear_pairs(state);
    case ustore_drop_values_k: return clear_values(state);
    case ustore_drop_collection_k:
        if (status_t const status = reset_database(state); status.failed()) return status;
        return persist(state);
    default: return {status_code_t::invalid_argument_k, "Unknown drop mode"};
    }
}

/** Implements @c ustore_database_compact, reclaiming unreachable versions of every store, freeing unreachable retired
 *  stores and rewriting the image, reporting one unit per collection, as the versions it frees carry no byte count. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_database_compact_t const& arguments, view_t const&) noexcept {
    claim_t claim;
    reader_source<isolation_> source {state, state.order.take_snapshot(claim)};
    listed_collections<isolation_> listed;
    auto catalog = source.view(state.collections_by_id);
    if (store_status_t const collected = collect_catalog<isolation_>(*catalog, listed); st::failed(collected))
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
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_snapshot_init_t const& arguments, view_t const&) noexcept {
    if (!arguments.snapshot) return {status_code_t::invalid_argument_k, "Need an output for the snapshot"};
    auto* const handle = new (std::nothrow) snapshot_handle<isolation_>(state);
    if (!handle) return {status_code_t::out_of_memory_k, "Failed to allocate the snapshot"};
    state.order.take_snapshot(handle->claim);
    fu::preferred_atomic_ref<std::size_t>(state.open_snapshots).fetch_add(1, std::memory_order_release);
    if (arguments.read_stamp) *arguments.read_stamp = handle->claim.snapshot();
    *arguments.snapshot = c_handle<ustore_snapshot_t>(handle);
    return {};
}

/** Implements @c ustore_snapshot_save as an image in the directory it names. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_snapshot_save_t const& arguments,
               view_t const& view) noexcept {
    sz::string_t path;
    if (path.assign(sz::string_view_t {string_of(arguments.path)}) != sz::status_t::success_k)
        return {status_code_t::out_of_memory_k, "Failed to copy the save path"};
    if (path.empty() || !names_directory(path.c_str()))
        return {status_code_t::not_found_k, "The save path isn't a directory"};
    return write_image(state, handle_cast<snapshot_handle<isolation_>>(view.snapshot)->claim.snapshot(), path.c_str(),
                       arguments.progress, arguments.progress_state);
}

#pragma endregion Snapshot Calls

#pragma region Blob Calls

/** Implements @c ustore_blobs_read, copying every value into the caller's arena once, where a graph's vertices read
 *  as empty values. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_blobs_read_t const& arguments, view_t const& view) noexcept {
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
                using entry_t = entry_of<decltype(entries)>;
                if constexpr (std::is_same_v<entry_t, string_pair_t>)
                    return refused(outcome, "The collection holds strings");
                else {
                    auto store = source.view(entries);
                    if (!store) return store.status();
                    return read_keys<entry_t>(*store, places, first, end, outputs);
                }
            });
        if (st::succeeded(runs) && outcome.succeeded()) outcome = outputs.finish(arguments);
        return runs;
    });
    return reported(read, outcome);
}

/** Hands @p stage every run of @p count tasks on a collection of values through @p source, with the part of the
 *  transaction over its store, refusing strings and graphs, which change through calls of their own. */
template <typename source_type_, typename stage_type_>
store_status_t stage_value_runs(source_type_& source, strided_iterator<ustore_collection_id_t const> collections,
                                std::size_t count, status_t& outcome, stage_type_&& stage) noexcept {
    return for_each_run(source, collections, count, outcome,
                        [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
                            using entry_t = entry_of<decltype(entries)>;
                            if constexpr (std::is_same_v<entry_t, string_pair_t>)
                                return refused(outcome, "The collection holds strings");
                            else if constexpr (std::is_same_v<entry_t, half_edge_t>)
                                return refused(outcome, "A graph collection changes through the graph calls");
                            else {
                                auto part = source.part(entries);
                                if (!part) return part.status();
                                return stage(**part, first, end);
                            }
                        });
}

/** Writes @p content under @p key at the newest stamp, straight into the store of the collection @p collections
 *  names first. */
template <isolation_t isolation_>
store_status_t write_one(database_state<isolation_>& state, strided_iterator<ustore_collection_id_t const> collections,
                         ustore_key_t const& key, value_view_t content, status_t& outcome) noexcept {
    claim_t claim;
    reader_source<isolation_> source {state, state.order.take_snapshot(claim)};
    return for_each_run(source, collections, 1, outcome,
                        [&](auto& entries, std::size_t, std::size_t) noexcept -> store_status_t {
                            using entry_t = entry_of<decltype(entries)>;
                            if constexpr (std::is_same_v<entry_t, string_pair_t>)
                                return refused(outcome, "The collection holds strings");
                            else if constexpr (std::is_same_v<entry_t, half_edge_t>)
                                return refused(outcome, "A graph collection changes through the graph calls");
                            else return stage_upsert(entries, key, content);
                        });
}

/** Stages @p stage_runs into the transaction @p view carries, or commits it as a transaction of its own and persists
 *  it when @p durability asks, for every call that writes. @p stage_runs is invoked as `stage_runs(source, outcome)`
 *  and may refuse the call through the outcome. */
template <isolation_t isolation_, typename stage_runs_type_>
status_t serve_write(database_state<isolation_>& state, ustore_durability_t durability, view_t const& view,
                     stage_runs_type_&& stage_runs) noexcept {
    if (view.transaction && durability == ustore_durability_flushed_k)
        return {status_code_t::invalid_argument_k, "A transaction flushes at commit, not at a write"};
    status_t outcome;
    if (view.transaction) {
        auto& transaction = handle_cast<transaction_handle<isolation_>>(view.transaction)->transaction;
        transaction_source<isolation_> source {state, transaction, watches_reads<isolation_>(view.watch)};
        return reported(stage_runs(source, outcome), outcome);
    }
    store_status_t const committed = commit_group(
        state, [&](transaction_source<isolation_>& source) noexcept { return stage_runs(source, outcome); });
    if (status_t const status = reported(committed, outcome); status.failed()) return status;
    if (durability == ustore_durability_flushed_k) return persist(state);
    return {};
}

/** Implements @c ustore_blobs_write, committing a batch outside a transaction atomically. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_blobs_write_t const& arguments, strings_arg_t const& values,
               view_t const& view) noexcept {
    place_batch_t const places = places_of(arguments);
    if (status_t const status = validate_write(view.transaction, places, values, arguments.durability, view.watch);
        status.failed() || !arguments.count)
        return status;
    if (view.transaction || places.size() != 1)
        return serve_write(state, arguments.durability, view, [&](auto& source, status_t& outcome) noexcept {
            return stage_value_runs(source, places.collections_begin, places.size(), outcome,
                                    [&](auto& part, std::size_t first, std::size_t end) noexcept {
                                        return stage_writes(part, places, values, first, end);
                                    });
        });
    status_t outcome;
    store_status_t const written = write_one(state, places.collections_begin, places[0].key, values[0], outcome);
    if (status_t const status = reported(written, outcome); status.failed()) return status;
    if (arguments.durability == ustore_durability_flushed_k) return persist(state);
    return {};
}

/** Implements @c ustore_blobs_remove, committing a batch outside a transaction atomically, where a key already gone
 *  stays gone. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_blobs_remove_t const& arguments, view_t const& view) noexcept {
    place_batch_t const places = places_of(arguments);
    if (status_t const status = validate_remove(view.transaction, places, arguments.durability, view.watch);
        status.failed() || !arguments.count)
        return status;
    return serve_write(state, arguments.durability, view, [&](auto& source, status_t& outcome) noexcept {
        return stage_value_runs(source, places.collections_begin, places.size(), outcome,
                                [&](auto& part, std::size_t first, std::size_t end) noexcept {
                                    return stage_removals(part, places, first, end, source.watch_mode);
                                });
    });
}

/** Implements @c ustore_blobs_scan as ordered walks of each collection's keys, a graph's being its vertices. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_blobs_scan_t const& arguments, view_t const& view) noexcept {
    scan_batch_t const scans = scans_of(arguments);
    if (status_t const status = validate_scan(arguments); status.failed() || !arguments.count) return status;
    auto [outputs, outputs_status] = scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    progress_t progress {arguments.progress, arguments.progress_state, arguments.count};

    status_t outcome;
    store_status_t const walked = with_source(state, view, [&](auto& source) noexcept {
        return for_each_run(source, scans.collections, scans.size(), outcome,
                            [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
                                using entry_t = entry_of<decltype(entries)>;
                                if constexpr (std::is_same_v<entry_t, string_pair_t>)
                                    return refused(outcome, "The collection holds strings");
                                else {
                                    auto store = source.view(entries);
                                    if (!store) return store.status();
                                    for (std::size_t task = first; task != end && outcome.succeeded(); ++task) {
                                        outputs.open_task(task);
                                        store_status_t const scanned = scan_window<entry_t>(*store, scans[task],
                                                                                            outputs, outcome);
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

/** Implements @c ustore_blobs_sample as a reservoir filled from a walk of each collection's keys. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_blobs_sample_t const& arguments, view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!arguments.offsets || !arguments.keys)
        return {status_code_t::invalid_argument_k, "Need outputs for the offsets and the keys"};
    sample_batch_t const samples = samples_of(arguments);
    auto [outputs, outputs_status] = scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    auto const now = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    sample_generator_t generator {now ^ reinterpret_cast<std::uintptr_t>(&outputs)};

    status_t outcome;
    store_status_t const walked = with_source(state, view, [&](auto& source) noexcept {
        return for_each_run(source, samples.collections, samples.size(), outcome,
                            [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
                                using entry_t = entry_of<decltype(entries)>;
                                if constexpr (std::is_same_v<entry_t, string_pair_t>)
                                    return refused(outcome, "The collection holds strings");
                                else {
                                    auto store = source.view(entries);
                                    if (!store) return store.status();
                                    for (std::size_t task = first; task != end; ++task) {
                                        outputs.open_task(task);
                                        store_status_t const sampled = sample_window<entry_t>(
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

/** Records the exact measure of @p task into @p outputs. */
inline void assign_measure(measure_outputs_t& outputs, std::size_t task, window_measure_t measured) noexcept {
    outputs.min_cardinalities[task] = measured.cardinality;
    outputs.max_cardinalities[task] = measured.cardinality;
    outputs.min_value_bytes[task] = measured.value_bytes;
    outputs.max_value_bytes[task] = measured.value_bytes;
    outputs.min_space_usages[task] = measured.space_bytes;
    outputs.max_space_usages[task] = std::numeric_limits<ustore_size_t>::max();
}

/** Implements @c ustore_blobs_measure, exact for entries and value bytes at the view it reads. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_blobs_measure_t const& arguments,
               view_t const& view) noexcept {
    if (!arguments.count) return {};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    strided_iterator<ustore_key_t const> const start_keys = strided(arguments.start_keys);
    strided_iterator<ustore_key_t const> const end_keys = strided(arguments.end_keys);
    auto [outputs, outputs_status] = measure_outputs_t::make(arguments);
    if (outputs_status.failed()) return outputs_status;

    status_t outcome;
    store_status_t const walked = with_source(state, view, [&](auto& source) noexcept {
        return for_each_run(source, collections, arguments.count, outcome,
                            [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
                                using entry_t = entry_of<decltype(entries)>;
                                if constexpr (std::is_same_v<entry_t, string_pair_t>)
                                    return refused(outcome, "The collection holds strings");
                                else {
                                    auto store = source.view(entries);
                                    if (!store) return store.status();
                                    for (std::size_t task = first; task != end; ++task) {
                                        window_measure_t measured;
                                        ustore_key_t const lower = start_keys ? start_keys[task] : smallest_key_k;
                                        ustore_key_t const last = end_keys ? end_keys[task] : largest_key_k;
                                        store_status_t const measuring = measure_window<entry_t>(*store, lower, last,
                                                                                                 measured);
                                        if (st::failed(measuring)) return measuring;
                                        assign_measure(outputs, task, measured);
                                    }
                                    return store_status_t::success_k;
                                }
                            });
    });
    if (status_t const status = reported(walked, outcome); status.failed()) return status;
    outputs.finish(arguments);
    return {};
}

#pragma endregion Blob Calls

#pragma region Collection Calls

/** Implements @c ustore_collection_list as a walk of the catalog at the head or at a snapshot, whose entries carry
 *  the modalities and the names. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_collection_list_t const& arguments,
               view_t const& view) noexcept {
    if (!arguments.written) return {status_code_t::invalid_argument_k, "Need an output for the collections written"};
    collection_list_outputs_t outputs {view.scratch};
    status_t outcome;
    store_status_t const walked = with_source(state, view, [&](auto& source) noexcept {
        return source.catalog().range_from(
            ustore_collection_id_t {}, [&](catalog_pair<isolation_> const& pair) noexcept -> walk_control_t {
                if (pair.key == ustore_collection_main_k) return walk_control_t::resume_k;
                outcome = outputs.push(pair.key, pair.mapped.modality, view_of(pair.mapped.name));
                return outcome.failed() ? walk_control_t::halt_k : walk_control_t::resume_k;
            });
    });
    if (status_t const status = reported(walked, outcome); status.failed()) return status;
    return outputs.finish(arguments);
}

/** Implements @c ustore_collection_create as one commit of the entry, its fresh store and the name, a named
 *  collection persisted at once as a flushed write would be. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_collection_create_t const& arguments, view_t const&) noexcept {
    if (!arguments.collection) return {status_code_t::invalid_argument_k, "Need an output for the collection"};
    if (arguments.modality > ustore_modality_pages_k) return {status_code_t::invalid_argument_k, "Unknown modality"};
    std::string_view const name = string_of(arguments.name);
    if (arguments.name.data && name.empty())
        return {status_code_t::invalid_argument_k, "A name is never empty, as the main collection has none"};
    auto [created, status] = mint_collection_id();
    if (status.failed()) return status;

    // Two creations of one name both write it, so the later commit conflicts and its retry finds the name taken
    store_status_t const committed = commit_catalog_change(
        state, [&](transaction_source<isolation_>& source, catalog_change<isolation_>& change) noexcept {
            return stage_collection(source, change, created, arguments.modality, name);
        });
    if (committed == store_status_t::key_already_exists_k)
        return {status_code_t::already_exists_k, "A collection already carries the name"};
    if (st::failed(committed)) return translate(committed);
    *arguments.collection = created;
    if (!name.empty()) return persist(state);
    return {};
}

/** Implements @c ustore_collection_drop: forgetting the collection and retiring its store, giving it a fresh empty
 *  store, or emptying its values. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_collection_drop_t const& arguments, view_t const&) noexcept {
    ustore_collection_id_t const collection = arguments.collection;
    ustore_drop_mode_t const mode = arguments.mode;
    if (collection == ustore_collection_main_k && mode == ustore_drop_collection_k)
        return {status_code_t::invalid_argument_k, "The main collection can only be cleared"};
    status_t outcome;
    store_status_t dropped = store_status_t::success_k;
    switch (mode) {
    case ustore_drop_collection_k:
        dropped = commit_catalog_change(
            state, [&](transaction_source<isolation_>& source, catalog_change<isolation_>& change) noexcept {
                return stage_forget(source, change, collection);
            });
        break;
    case ustore_drop_pairs_k:
        dropped = commit_catalog_change(
            state, [&](transaction_source<isolation_>& source, catalog_change<isolation_>& change) noexcept {
                return stage_emptied(source, change, collection);
            });
        break;
    case ustore_drop_values_k:
        dropped = commit_group(state, [&](transaction_source<isolation_>& source) noexcept {
            return stage_values_emptied(source, collection, outcome);
        });
        break;
    default: return {status_code_t::invalid_argument_k, "Unknown drop mode"};
    }
    if (dropped == store_status_t::key_not_found_k && outcome.succeeded())
        return {status_code_t::not_found_k, "No collection has such identifier"};
    if (status_t const status = reported(dropped, outcome); status.failed()) return status;
    if (mode == ustore_drop_collection_k) return persist(state);
    return {};
}

/** Implements @c ustore_collection_find, reading the names and the catalog at the newest stamp or at a snapshot. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_collection_find_t const& arguments, strings_arg_t const& names,
               view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!names || !arguments.collections)
        return {status_code_t::invalid_argument_k, "Need the names and an output for the identifiers"};
    auto [outputs, outputs_status] = collection_find_outputs_t::make(arguments);
    if (outputs_status.failed()) return outputs_status;

    status_t const resolved = translate(with_source(state, view, [&](auto& source) noexcept -> store_status_t {
        auto named = source.view(state.collection_ids_by_name);
        if (!named) return named.status();
        auto const catalog = source.catalog();
        for (std::size_t index = 0; index != arguments.count; ++index) {
            auto [key, key_status] = string_key_from(string_of(names[index]));
            if (st::failed(key_status)) return key_status;
            ustore_collection_id_t id = ustore_collection_main_k;
            bool present = false;
            store_status_t const looked_up = named->find(
                key, [&](name_pair_t const& pair) noexcept { id = pair.mapped; }, st::no_op_t {});
            if (st::failed(looked_up)) return looked_up;
            ustore_modality_t modality = ustore_modality_any_k;
            if (id != ustore_collection_main_k) {
                store_status_t const listed = catalog.find(
                    id,
                    [&](catalog_pair<isolation_> const& pair) noexcept {
                        present = true;
                        modality = pair.mapped.modality;
                    },
                    st::no_op_t {});
                if (st::failed(listed)) return listed;
            }
            outputs.assign(index, present ? id : ustore_collection_main_k, modality, present);
        }
        return store_status_t::success_k;
    }));
    if (resolved.failed()) return resolved;
    outputs.finish(arguments);
    return {};
}

#pragma endregion Collection Calls

#pragma region Strings Calls

/** Hands @p stage every run of @p count tasks on a strings collection through @p source, with the part of the
 *  transaction over its store, refusing a collection that holds no strings. */
template <typename source_type_, typename stage_type_>
store_status_t stage_string_runs(source_type_& source, strided_iterator<ustore_collection_id_t const> collections,
                                 std::size_t count, status_t& outcome, stage_type_&& stage) noexcept {
    return for_each_run(source, collections, count, outcome,
                        [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
                            using entry_t = entry_of<decltype(entries)>;
                            if constexpr (!std::is_same_v<entry_t, string_pair_t>)
                                return refused(outcome, "The collection holds no strings");
                            else {
                                auto part = source.part(entries);
                                if (!part) return part.status();
                                return stage(**part, first, end);
                            }
                        });
}

/** Implements @c ustore_strings_write, committing a batch outside a transaction atomically. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_strings_write_t const& arguments, strings_arg_t const& texts,
               strings_arg_t const& values, view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!texts || !values) return {status_code_t::invalid_argument_k, "Need the strings and their values"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    return serve_write(state, arguments.durability, view, [&](auto& source, status_t& outcome) noexcept {
        return stage_string_runs(source, collections, arguments.count, outcome,
                                 [&](auto& part, std::size_t first, std::size_t end) noexcept {
                                     return stage_strings(part, texts, values, first, end);
                                 });
    });
}

/** Implements @c ustore_strings_remove, committing a batch outside a transaction atomically, where a string already
 *  gone stays gone. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_strings_remove_t const& arguments, strings_arg_t const& texts,
               view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!texts) return {status_code_t::invalid_argument_k, "No strings were provided"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    return serve_write(state, arguments.durability, view, [&](auto& source, status_t& outcome) noexcept {
        return stage_string_runs(source, collections, arguments.count, outcome,
                                 [&](auto& part, std::size_t first, std::size_t end) noexcept {
                                     return stage_string_removals(part, texts, first, end, source.watch_mode);
                                 });
    });
}

/** Implements @c ustore_strings_read, copying every value into the caller's arena once. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_strings_read_t const& arguments, strings_arg_t const& texts,
               view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!texts) return {status_code_t::invalid_argument_k, "No strings were provided"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    status_t outcome;
    auto [outputs, outputs_status] = read_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;

    // The source's claim keeps every version found alive until the outputs copy its value
    store_status_t const read = with_source(state, view, [&](auto& source) noexcept {
        store_status_t const runs = for_each_run(
            source, collections, arguments.count, outcome,
            [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
                using entry_t = entry_of<decltype(entries)>;
                if constexpr (!std::is_same_v<entry_t, string_pair_t>)
                    return refused(outcome, "The collection holds no strings");
                else {
                    auto store = source.view(entries);
                    if (!store) return store.status();
                    return read_strings(*store, texts, first, end, outputs);
                }
            });
        if (st::succeeded(runs) && outcome.succeeded()) outcome = outputs.finish(arguments);
        return runs;
    });
    return reported(read, outcome);
}

/** Implements @c ustore_strings_scan as ordered walks of each collection's strings. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_strings_scan_t const& arguments, strings_arg_t const& prefixes,
               strings_arg_t const& starts, view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!arguments.offsets || !arguments.strings)
        return {status_code_t::invalid_argument_k, "Need outputs for the offsets and the strings"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    auto [outputs, outputs_status] = strings_scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    progress_t progress {arguments.progress, arguments.progress_state, arguments.count};

    status_t outcome;
    store_status_t const walked = with_source(state, view, [&](auto& source) noexcept {
        return for_each_run(
            source, collections, arguments.count, outcome,
            [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
                using entry_t = entry_of<decltype(entries)>;
                if constexpr (!std::is_same_v<entry_t, string_pair_t>)
                    return refused(outcome, "The collection holds no strings");
                else {
                    auto store = source.view(entries);
                    if (!store) return store.status();
                    for (std::size_t task = first; task != end && outcome.succeeded(); ++task) {
                        std::string_view const prefix = prefixes ? string_of(prefixes[task]) : std::string_view {};
                        std::string_view const start = starts ? string_of(starts[task]) : std::string_view {};
                        outputs.open_task(task);
                        store_status_t const scanned = scan_strings(*store, prefix, start, arguments.wanted, outputs,
                                                                    outcome);
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

#pragma endregion Strings Calls

#pragma region Graph Calls

/** Implements @c ustore_graph_read_edges as walks of each vertex's half-edges in the roles it asks for. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_graph_read_edges_t const& arguments,
               view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!arguments.vertices.data || !arguments.degrees_per_vertex)
        return {status_code_t::invalid_argument_k, "Need the vertices and an output for their degrees"};
    if (arguments.edges_per_vertex && !arguments.edges_offsets)
        return {status_code_t::invalid_argument_k, "Need an output for the offsets of the edges"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    edges_batch_t const tasks {collections, strided(arguments.vertices), strided(arguments.roles), arguments.count};
    auto [degrees, degrees_status] = arena_allocate<ustore_vertex_degree_t>(arguments.arena, arguments.count);
    if (degrees_status.failed()) return degrees_status;
    st::basic_vector<ustore_key_t, arena_allocator<ustore_key_t>> edges {{view.scratch}};
    st::basic_vector<half_edge_t, arena_allocator<half_edge_t>> walked {{view.scratch}};

    status_t outcome;
    store_status_t const exported = with_source(state, view, [&](auto& source) noexcept {
        return for_each_run(source, collections, arguments.count, outcome,
                            [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
                                using entry_t = entry_of<decltype(entries)>;
                                if constexpr (!std::is_same_v<entry_t, half_edge_t>)
                                    return refused(outcome, "The collection holds no graph");
                                else {
                                    auto store = source.view(entries);
                                    if (!store) return store.status();
                                    return export_edges(*store, tasks, first, end, degrees,
                                                        arguments.edges_per_vertex ? &edges : nullptr, walked, outcome);
                                }
                            });
    });
    if (status_t const status = reported(exported, outcome); status.failed()) return status;
    auto [copied_edges, edges_status] = arena_copy<ustore_key_t>(arguments.arena, edges);
    if (edges_status.failed()) return edges_status;

    // A missing vertex spans no edges
    auto [offsets, offsets_status] = arena_allocate<ustore_length_t>(arguments.arena,
                                                                     arguments.edges_offsets ? arguments.count + 1 : 0);
    if (offsets_status.failed()) return offsets_status;
    if (!offsets.empty()) offsets[0] = 0;
    for (std::size_t index = 0; index + 1 < offsets.size(); ++index)
        offsets[index + 1] = offsets[index] + (degrees[index] == ustore_vertex_degree_missing_k ? 0 : degrees[index]);

    *arguments.degrees_per_vertex = degrees.data();
    if (arguments.edges_offsets) *arguments.edges_offsets = offsets.data();
    if (arguments.edges_per_vertex) *arguments.edges_per_vertex = copied_edges.data();
    return {};
}

/** Hands @p stage every run of @p count tasks on a graph collection through @p source, with the part of the
 *  transaction over its half-edges, refusing a collection that holds no graph. */
template <typename source_type_, typename stage_type_>
store_status_t stage_graph_runs(source_type_& source, strided_iterator<ustore_collection_id_t const> collections,
                                std::size_t count, status_t& outcome, stage_type_&& stage) noexcept {
    return for_each_run(source, collections, count, outcome,
                        [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
                            using entry_t = entry_of<decltype(entries)>;
                            if constexpr (!std::is_same_v<entry_t, half_edge_t>)
                                return refused(outcome, "The collection holds no graph");
                            else {
                                auto part = source.part(entries);
                                if (!part) return part.status();
                                return stage(**part, first, end);
                            }
                        });
}

/** Implements @c ustore_graph_write_edges, filing every edge under both of its ends. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_graph_write_edges_t const& arguments,
               view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!arguments.sources.data || !arguments.targets.data)
        return {status_code_t::invalid_argument_k, "Need the sources and the targets of the edges"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    edges_view_t const edges = edges_of(arguments);
    return serve_write(state, arguments.durability, view, [&](auto& source, status_t& outcome) noexcept {
        return stage_graph_runs(source, collections, arguments.count, outcome,
                                [&](auto& part, std::size_t first, std::size_t end) noexcept {
                                    return stage_edges(part, edges, first, end);
                                });
    });
}

/** Implements @c ustore_graph_remove_edges, erasing every edge under both of its ends and keeping the vertices. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_graph_remove_edges_t const& arguments,
               view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!arguments.sources.data || !arguments.targets.data)
        return {status_code_t::invalid_argument_k, "Need the sources and the targets of the edges"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    edges_view_t const edges = edges_of(arguments);
    return serve_write(state, arguments.durability, view, [&](auto& source, status_t& outcome) noexcept {
        return stage_graph_runs(source, collections, arguments.count, outcome,
                                [&](auto& part, std::size_t first, std::size_t end) noexcept {
                                    return stage_edges_removal(part, edges, first, end);
                                });
    });
}

/** Implements @c ustore_graph_write_vertices, adding every vertex that is not one already. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_graph_write_vertices_t const& arguments,
               view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!arguments.vertices.data) return {status_code_t::invalid_argument_k, "No vertices were provided"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    strided_iterator<ustore_key_t const> const vertices = strided(arguments.vertices);
    return serve_write(state, arguments.durability, view, [&](auto& source, status_t& outcome) noexcept {
        return stage_graph_runs(source, collections, arguments.count, outcome,
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
status_t serve(database_state<isolation_>& state, ustore_graph_remove_vertices_t const& arguments,
               view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!arguments.vertices.data) return {status_code_t::invalid_argument_k, "No vertices were provided"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    strided_iterator<ustore_key_t const> const vertices = strided(arguments.vertices);
    strided_iterator<ustore_vertex_role_t const> const roles = strided(arguments.roles);
    st::basic_vector<half_edge_t, arena_allocator<half_edge_t>> walked {{view.scratch}};
    return serve_write(state, arguments.durability, view, [&](auto& source, status_t& outcome) noexcept {
        return stage_graph_runs(
            source, collections, arguments.count, outcome,
            [&](auto& part, std::size_t first, std::size_t end) noexcept {
                for (std::size_t index = first; index != end; ++index) {
                    ustore_vertex_role_t const role = roles ? roles[index] : ustore_vertex_role_any_k;
                    store_status_t const removed = stage_vertex_removal(part, vertices[index], role, walked);
                    if (st::failed(removed)) return removed;
                }
                return store_status_t::success_k;
            });
    });
}

#pragma endregion Graph Calls

#pragma region Transaction Calls

/** Implements @c ustore_transaction_init, opening a transaction or resetting the one it reuses onto a fresh
 *  snapshot. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_transaction_init_t const& arguments,
               view_t const& view) noexcept {
    if (status_t const status = validate_transaction_begin(arguments.transaction); status.failed()) return status;
    if (view.transaction) {
        auto* const handle = handle_cast<transaction_handle<isolation_>>(view.transaction);
        store_status_t const reset = handle->transaction.reset();
        free_unreachable(handle->state);
        return translate(reset);
    }
    auto opened = collections_transaction<isolation_>::make(state.order);
    if (!opened) return translate(opened.status());
    auto* const handle = new (std::nothrow) transaction_handle<isolation_>(state, std::move(*opened));
    if (!handle) return {status_code_t::out_of_memory_k, "Failed to allocate the transaction"};
    *arguments.transaction = c_handle<ustore_transaction_t>(handle);
    return {};
}

/** Stages @p transaction unless it already is. */
template <isolation_t isolation_>
store_status_t stage_once(collections_transaction<isolation_>& transaction) noexcept {
    if (transaction.staging() == staging_t::staged_k) return store_status_t::success_k;
    return transaction.stage();
}

/** Implements @c ustore_transaction_stage. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>&, ustore_transaction_stage_t const& arguments, view_t const& view) noexcept {
    if (!view.transaction) return {status_code_t::uninitialized_k, "Transaction is uninitialized"};
    auto& transaction = handle_cast<transaction_handle<isolation_>>(view.transaction)->transaction;
    if (store_status_t const staged = stage_once<isolation_>(transaction); st::failed(staged)) return translate(staged);
    if (arguments.read_stamp) *arguments.read_stamp = transaction.snapshot();
    return {};
}

/** Implements @c ustore_transaction_commit, staging first when the caller did not. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>& state, ustore_transaction_commit_t const& arguments,
               view_t const& view) noexcept {
    if (status_t const status = validate_transaction_commit(view.transaction, arguments.durability); status.failed())
        return status;
    auto& transaction = handle_cast<transaction_handle<isolation_>>(view.transaction)->transaction;
    if (store_status_t const staged = stage_once<isolation_>(transaction); st::failed(staged)) return translate(staged);
    if (store_status_t const committed = transaction.commit(); st::failed(committed)) return translate(committed);
    if (arguments.commit_stamp) *arguments.commit_stamp = transaction.commit_stamp();
    free_unreachable(state);
    if (arguments.durability == ustore_durability_flushed_k) return persist(state);
    return {};
}

/** Implements @c ustore_transaction_rollback, pulling the staged writes back into the transaction, which may then
 *  stage again, while one never staged has nothing to withdraw. */
template <isolation_t isolation_>
status_t serve(database_state<isolation_>&, ustore_transaction_rollback_t const&, view_t const& view) noexcept {
    if (!view.transaction) return {status_code_t::uninitialized_k, "Transaction is uninitialized"};
    auto& transaction = handle_cast<transaction_handle<isolation_>>(view.transaction)->transaction;
    if (transaction.staging() != staging_t::staged_k) return {};
    return translate(transaction.rollback());
}

#pragma endregion Transaction Calls

#pragma region Dispatch

/** Serves a call on the database @p arguments address, whose handle the table of @p isolation_ stamped, with the
 *  ragged inputs the router gathered for it. */
template <isolation_t isolation_, typename arguments_type_, typename... strings_types_>
status_t serve_database(arguments_type_ const& arguments, strings_types_ const&... strings,
                        view_t const& view) noexcept {
    return serve(*handle_cast<database_state<isolation_>>(database_of(arguments)), arguments, strings..., view);
}

/** Allocates a database at @p isolation_ and loads the image of @p directory. */
template <isolation_t isolation_>
expected<ustore_database_t> make_database(std::string_view directory) noexcept {
    auto* const database = new (std::nothrow) database_state<isolation_>();
    if (!database) return status_t {status_code_t::out_of_memory_k, "Failed to allocate the database"};
    if (status_t const status = open_database(*database, directory); status.failed()) {
        delete database;
        return status;
    }
    return c_handle<ustore_database_t>(database);
}

/** Opens the database @p arguments describe into their output, which stays null unless the open succeeds, persisting
 *  in @p directory unless it is empty. */
template <typename arguments_type_>
status_t open_in(arguments_type_ const& arguments, std::string_view directory) noexcept {
    if (!arguments.database) return {status_code_t::invalid_argument_k, "Need an output for the database"};
    *arguments.database = nullptr;
    if (arguments.isolation != ustore_isolation_strict_serializable_k &&
        arguments.isolation != ustore_isolation_snapshot_k)
        return {status_code_t::invalid_argument_k, "Unknown isolation"};
    auto [database, status] = arguments.isolation == ustore_isolation_snapshot_k
                                  ? make_database<isolation_t::snapshot_k>(directory)
                                  : make_database<isolation_t::strict_serializable_k>(directory);
    *arguments.database = database;
    return status;
}

status_t init(ustore_database_init_ram_t const& arguments, view_t const&) noexcept { return open_in(arguments, {}); }

status_t open(ustore_database_open_ram_t const& arguments, view_t const&) noexcept {
    if (!arguments.directory.length) return {status_code_t::invalid_argument_k, "A database needs a directory"};
    return open_in(arguments, string_of(arguments.directory));
}

/** Frees @p transaction, releasing the stamp it claimed, and the stores only it still reached. */
template <isolation_t isolation_>
void transaction_free(ustore_transaction_t transaction) noexcept {
    auto* const handle = handle_cast<transaction_handle<isolation_>>(transaction);
    database_state<isolation_>& state = handle->state;
    delete handle;
    free_unreachable(state);
}

/** Gives back the count @p snapshot holds and frees it, releasing the stamp it claimed, and the stores only it still
 *  reached. */
template <isolation_t isolation_>
void snapshot_free(ustore_snapshot_t snapshot) noexcept {
    auto* const handle = handle_cast<snapshot_handle<isolation_>>(snapshot);
    database_state<isolation_>& state = handle->state;
    fu::preferred_atomic_ref<std::size_t>(state.open_snapshots).fetch_sub(1, std::memory_order_release);
    delete handle;
    free_unreachable(state);
}

/** Persists @p database ahead of freeing it, which every snapshot and transaction of it must precede. */
template <isolation_t isolation_>
void database_free(ustore_database_t database) noexcept {
    auto* const state = handle_cast<database_state<isolation_>>(database);
    [[maybe_unused]] status_t const persisted = persist(*state);
    delete state;
}

template <isolation_t isolation_>
engine_t const engine_k {
    .database_describe = &serve_database<isolation_, ustore_database_describe_t>,
    .database_clear = &serve_database<isolation_, ustore_database_clear_t>,
    .database_compact = &serve_database<isolation_, ustore_database_compact_t>,
    .snapshot_init = &serve_database<isolation_, ustore_snapshot_init_t>,
    .snapshot_save = &serve_database<isolation_, ustore_snapshot_save_t>,
    .collection_list = &serve_database<isolation_, ustore_collection_list_t>,
    .collection_create = &serve_database<isolation_, ustore_collection_create_t>,
    .collection_find = &serve_database<isolation_, ustore_collection_find_t, strings_arg_t>,
    .collection_drop = &serve_database<isolation_, ustore_collection_drop_t>,
    .transaction_init = &serve_database<isolation_, ustore_transaction_init_t>,
    .transaction_stage = &serve_database<isolation_, ustore_transaction_stage_t>,
    .transaction_commit = &serve_database<isolation_, ustore_transaction_commit_t>,
    .transaction_rollback = &serve_database<isolation_, ustore_transaction_rollback_t>,
    .blobs_write = &serve_database<isolation_, ustore_blobs_write_t, strings_arg_t>,
    .blobs_remove = &serve_database<isolation_, ustore_blobs_remove_t>,
    .blobs_read = &serve_database<isolation_, ustore_blobs_read_t>,
    .blobs_scan = &serve_database<isolation_, ustore_blobs_scan_t>,
    .blobs_sample = &serve_database<isolation_, ustore_blobs_sample_t>,
    .blobs_measure = &serve_database<isolation_, ustore_blobs_measure_t>,
    .docs_write = &docs_write_in_json_values,
    .docs_read = &docs_read_in_json_values,
    .docs_gist = &docs_gist_in_json_values,
    .docs_gather = &docs_gather_in_json_values,
    .graph_read_edges = &serve_database<isolation_, ustore_graph_read_edges_t>,
    .graph_write_edges = &serve_database<isolation_, ustore_graph_write_edges_t>,
    .graph_remove_edges = &serve_database<isolation_, ustore_graph_remove_edges_t>,
    .graph_write_vertices = &serve_database<isolation_, ustore_graph_write_vertices_t>,
    .graph_remove_vertices = &serve_database<isolation_, ustore_graph_remove_vertices_t>,
    .strings_write = &serve_database<isolation_, ustore_strings_write_t, strings_arg_t, strings_arg_t>,
    .strings_remove = &serve_database<isolation_, ustore_strings_remove_t, strings_arg_t>,
    .strings_read = &serve_database<isolation_, ustore_strings_read_t, strings_arg_t>,
    .strings_scan = &serve_database<isolation_, ustore_strings_scan_t, strings_arg_t, strings_arg_t>,
    .transaction_free = &transaction_free<isolation_>,
    .snapshot_free = &snapshot_free<isolation_>,
    .database_free = &database_free<isolation_>,
};

#pragma endregion Dispatch

} // namespace unum::ustore::ram

using namespace unum::ustore::ram;

#pragma region C ABI

void ustore_database_init_ram(ustore_database_init_ram_t* arguments) { route_init(arguments, &init); }

void ustore_database_open_ram(ustore_database_open_ram_t* arguments) { route_init(arguments, &open); }
