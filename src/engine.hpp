/**
 *  @file src/engine.hpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief The verb tables every engine fills, which the router of <tt>src/lib.hpp</tt> routes each
 *      handle through, the handles of databases, transactions, snapshots and executors, and
 *      the CPU classes.
 *
 *  A CPU class pairs a strings kit, an atomic reference and a waiter, which an engine instantiates
 *  its templates with per class; an open picks the newest class the running CPU admits, and
 *  class-free words go through the floor class's reference and the picked class's verbs.
 */

#pragma once

#include <cstddef> // `std::size_t`, `std::byte`, `std::max_align_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`, `std::uint64_t`

#include <array>       // `std::array`
#include <memory>      // `std::unique_ptr`
#include <new>         // `std::bad_alloc`, `std::nothrow`
#include <thread>      // `std::thread::id`
#include <type_traits> // `std::is_same_v`, `std::conditional_t`
#include <utility>     // `std::forward`
#include <variant>     // `std::variant`, `std::monostate`

#include <forkunion/atomics.hpp>       // `fu::standard_atomic_ref` & kin - the classes' atomic references
#include <forkunion/capabilities.hpp>  // `fu::capabilities_t`, `fu::x86_pause_t` & waiter kin, `fu::cpu_capabilities`
#include <smashtable/basic_vector.hpp> // `st::vector`
#include <smashtable/row_search.hpp>   // `st::row_kit_t`, `st::serial_row_kit_t` & kin, `st::native_row_kit_t`
#include <stringzilla/capabilities.h>  // `sz_capability_t`, `sz_cap_serial_k`

#include "ustore/blobs.h"   // `ustore_blobs_read_t`, `ustore_blobs_write_t` and the other binary calls
#include "ustore/db.h"      // `ustore_database_t`, `ustore_transaction_t` and the calls on them
#include "ustore/docs.h"    // `ustore_docs_write_t`, `ustore_docs_read_t` and the other document calls
#include "ustore/graph.h"   // `ustore_graph_read_edges_t`, `ustore_graph_write_edges_t` and the other graph calls
#include "ustore/pages.h"   // `ustore_pages_import_t`, `ustore_pages_read_t` and the other page calls
#include "ustore/strings.h" // `ustore_strings_write_t`, `ustore_strings_remove_t` and the other strings calls

#include "ustore/status.hpp" // `expected`, `status_t`

#include "ustore/utilities/ram_buffers.hpp" // `host_blocks_t`, `cuda_blocks_t`, `hip_blocks_t`
#include "ustore/utilities/ram_device.hpp"  // `claim_t`
#include "ustore/utilities/vram_device.hpp" // `cuda_executor_t`, `hip_executor_t`

#include "utilities/batches.hpp" // `strings_arg_t`

namespace unum::ustore {

using utilities::cuda_blocks_t;
using utilities::cuda_executor_t;
using utilities::executor_t;
using utilities::hip_blocks_t;
using utilities::hip_executor_t;
using utilities::host_blocks_t;
using utilities::lent_pool_t;
using utilities::strings_arg_t;

/** What a call reads through, decoded once by the router from its view, or from the transaction a write carries, and
 *  the scratch and the executor thread it works on. */
struct view_t {

    /** Transaction the call reads or writes through, or null. */
    ustore_transaction_t transaction = nullptr;

    /** Snapshot the call reads, or null. */
    ustore_snapshot_t snapshot = nullptr;

    /** Whether the transaction records the keys the call touches. */
    ustore_watch_t watch = ustore_watch_keys_k;

    /** Where the call keeps whatever it needs only while it runs, so its arena holds only its outputs: the scratch of
     *  the executor thread running it, or one made for this call alone, rewound only before the call starts. */
    ustore_arena_t scratch = nullptr;

    /** The executor whose thread runs the call, or null for a call run alone. */
    ustore_executor_t executor = nullptr;

    /** Which thread of @c executor serves the work: a pool thread's index for a slice of a call,
     *  or the pool's size for the call's body, which runs on its caller or on pool thread 0. */
    std::size_t thread_index = 0;

    /** The stamp every verb of the call reads at outside a transaction and a snapshot, which the
     *  router pinned for a call of several verbs, or zero for each verb to pin its own. */
    ustore_stamp_t stamp = 0;

    /** What the engine took for every verb of a call of several verbs to read its one collection
     *  through instead of a stamp, which only that engine reads, or null. */
    void const* reading = nullptr;
};

/** Bytes an engine's pin of one call's stamp may take in @c call_pin_t, checked by each engine. */
inline constexpr std::size_t call_pin_bytes_k = 128;

/** The pin an engine holds while every verb of one call reads at one stamp, kept on the router's
 *  stack for the call and given back when it ends. */
struct call_pin_t {

    /** The stamp the verbs read at, zero while nothing is pinned. */
    ustore_stamp_t stamp = 0;

    /** The reading of the call's one collection the verbs read through instead, or null. */
    void const* reading = nullptr;

    /** Gives the pin back, set by the engine that took it. */
    void (*release)(call_pin_t&) noexcept = nullptr;

    /** Where the engine keeps its pin. */
    alignas(std::max_align_t) std::byte room[call_pin_bytes_k];

    call_pin_t() noexcept = default;
    call_pin_t(call_pin_t const&) = delete;
    call_pin_t& operator=(call_pin_t const&) = delete;
    ~call_pin_t() noexcept {
        if (release) release(*this);
    }
};

/** Where a verb runs, which is the kind of executor it takes; the storage engine is fixed at open. */
enum class home_t : std::uint8_t {

    /** Host threads, as every RAM, NVMe and host-only executor runs. */
    host_k,

    /** One NVIDIA device, fed by the host thread of a VRAM executor. */
    vram_nvidia_k,

    /** One AMD device, fed by the host thread of a VRAM executor. */
    vram_amd_k,
};

/** The executor the verbs of one device home take. */
template <home_t>
struct executor_for_home;

template <>
struct executor_for_home<home_t::vram_nvidia_k> {
    using type = cuda_executor_t;
};

template <>
struct executor_for_home<home_t::vram_amd_k> {
    using type = hip_executor_t;
};

/** The executor the verbs of device home @p home_ take as their last argument. */
template <home_t home_>
using home_executor = typename executor_for_home<home_>::type;

/** The home whose verbs take @p executors_types_ as their last argument, host threads for none. */
template <typename... executors_types_>
inline constexpr home_t home_of_executors = home_t::host_k;

template <>
inline constexpr home_t home_of_executors<cuda_executor_t> = home_t::vram_nvidia_k;

template <>
inline constexpr home_t home_of_executors<hip_executor_t> = home_t::vram_amd_k;

/** The database, snapshot, collection and transaction verbs every engine serves, on host threads alone. */
struct database_verbs_t {
    status_t (*describe)(ustore_database_describe_t const&, view_t const&) noexcept;
    status_t (*clear)(ustore_database_clear_t const&, view_t const&) noexcept;
    status_t (*compact)(ustore_database_compact_t const&, view_t const&) noexcept;
    status_t (*flush)(ustore_database_flush_t const&, view_t const&) noexcept;
    status_t (*snapshot_init)(ustore_snapshot_init_t const&, view_t const&) noexcept;
    status_t (*snapshot_save)(ustore_snapshot_save_t const&, view_t const&) noexcept;
    status_t (*collection_list)(ustore_collection_list_t const&, view_t const&) noexcept;
    status_t (*collection_create)(ustore_collection_create_t const&, view_t const&) noexcept;
    status_t (*collection_find)(ustore_collection_find_t const&, strings_arg_t const& names, view_t const&) noexcept;
    status_t (*collection_drop)(ustore_collection_drop_t const&, view_t const&) noexcept;
    status_t (*ingest_init)(ustore_ingest_init_t const&, view_t const&) noexcept;
    status_t (*ingest_append)(ustore_ingest_append_t const&, strings_arg_t const& values, view_t const&) noexcept;
    status_t (*ingest_finish)(ustore_ingest_finish_t const&, view_t const&) noexcept;
    void (*ingest_free)(ustore_ingest_t) noexcept;
    status_t (*transaction_init)(ustore_transaction_init_t const&, view_t const&) noexcept;
    status_t (*transaction_stage)(ustore_transaction_stage_t const&, view_t const&) noexcept;
    status_t (*transaction_commit)(ustore_transaction_commit_t const&, view_t const&) noexcept;
    status_t (*transaction_rollback)(ustore_transaction_rollback_t const&, view_t const&) noexcept;
    void (*transaction_free)(ustore_transaction_t) noexcept;
    void (*snapshot_free)(ustore_snapshot_t) noexcept;
    void (*database_free)(ustore_database_t) noexcept;
    status_t (*changes_cursor_init)(ustore_changes_cursor_init_t const&, ustore_changes_cursor_t,
                                    view_t const&) noexcept;
    status_t (*changes_cursor_next)(ustore_changes_cursor_next_t const&, view_t const&) noexcept;
    void (*changes_cursor_free)(ustore_changes_cursor_t) noexcept;

    /** Pins the newest stamp into @p pin for one call's verbs, or takes a reading of the one
     *  @p collection they read where it is not null; null without stamps. */
    void (*pin_call)(ustore_database_t, ustore_collection_id_t const* collection, view_t const& view,
                     call_pin_t& pin) noexcept;
};

/** The verbs of blobs collections served at one home. */
template <home_t>
struct blobs_verbs;

/** The verbs of blobs collections on host threads. */
template <>
struct blobs_verbs<home_t::host_k> {
    status_t (*write)(ustore_blobs_write_t const&, strings_arg_t const& values, view_t const&) noexcept;
    status_t (*remove)(ustore_blobs_remove_t const&, view_t const&) noexcept;
    status_t (*read)(ustore_blobs_read_t const&, view_t const&) noexcept;
    status_t (*scan)(ustore_blobs_scan_t const&, view_t const&) noexcept;
    status_t (*sample)(ustore_blobs_sample_t const&, view_t const&) noexcept;
    status_t (*measure)(ustore_blobs_measure_t const&, view_t const&) noexcept;
};

/** The verbs of blobs collections on a device, which has no slot for a change. */
template <home_t home_>
    requires(home_ != home_t::host_k)
struct blobs_verbs<home_> {
    status_t (*read)(ustore_blobs_read_t const&, view_t const&, home_executor<home_>&) noexcept;
    status_t (*scan)(ustore_blobs_scan_t const&, view_t const&, home_executor<home_>&) noexcept;
    status_t (*sample)(ustore_blobs_sample_t const&, view_t const&, home_executor<home_>&) noexcept;
};

/** The verbs of strings collections served at one home. */
template <home_t>
struct strings_verbs;

/** The verbs of strings collections on host threads. */
template <>
struct strings_verbs<home_t::host_k> {
    status_t (*write)(ustore_strings_write_t const&, strings_arg_t const& strings, view_t const&) noexcept;
    status_t (*remove)(ustore_strings_remove_t const&, strings_arg_t const& strings, strings_arg_t const& end_strings,
                       view_t const&) noexcept;
    status_t (*read)(ustore_strings_read_t const&, strings_arg_t const& strings, view_t const&) noexcept;
    status_t (*scan)(ustore_strings_scan_t const&, strings_arg_t const& prefixes, strings_arg_t const& start_strings,
                     strings_arg_t const& end_strings, view_t const&) noexcept;
    status_t (*sample)(ustore_strings_sample_t const&, view_t const&) noexcept;
    status_t (*measure)(ustore_strings_measure_t const&, strings_arg_t const& start_strings,
                        strings_arg_t const& end_strings, view_t const&) noexcept;
};

/** The verbs of strings collections on a device, whose strings a host index resolves. */
template <home_t home_>
    requires(home_ != home_t::host_k)
struct strings_verbs<home_> {
    status_t (*read)(ustore_strings_read_t const&, strings_arg_t const& strings, view_t const&,
                     home_executor<home_>&) noexcept;
    status_t (*scan)(ustore_strings_scan_t const&, strings_arg_t const& prefixes, strings_arg_t const& start_strings,
                     strings_arg_t const& end_strings, view_t const&, home_executor<home_>&) noexcept;
};

/** The verbs of documents collections served at one home. */
template <home_t>
struct docs_verbs;

/** The verbs of documents collections on host threads. */
template <>
struct docs_verbs<home_t::host_k> {
    status_t (*write)(ustore_docs_write_t const&, strings_arg_t const& fields, strings_arg_t const& values,
                      view_t const&) noexcept;
    status_t (*read)(ustore_docs_read_t const&, strings_arg_t const& fields, view_t const&) noexcept;
    status_t (*gist)(ustore_docs_gist_t const&, view_t const&) noexcept;
    status_t (*gather)(ustore_docs_gather_t const&, strings_arg_t const& fields, view_t const&) noexcept;
    status_t (*remove)(ustore_docs_remove_t const&, view_t const&) noexcept;
    status_t (*scan)(ustore_docs_scan_t const&, view_t const&) noexcept;
    status_t (*sample)(ustore_docs_sample_t const&, view_t const&) noexcept;
    status_t (*measure)(ustore_docs_measure_t const&, view_t const&) noexcept;
};

/** The verbs of documents collections on a device, whose field paths the host compiles. */
template <home_t home_>
    requires(home_ != home_t::host_k)
struct docs_verbs<home_> {
    status_t (*read)(ustore_docs_read_t const&, strings_arg_t const& fields, view_t const&,
                     home_executor<home_>&) noexcept;
    status_t (*gather)(ustore_docs_gather_t const&, strings_arg_t const& fields, view_t const&,
                       home_executor<home_>&) noexcept;
    status_t (*scan)(ustore_docs_scan_t const&, view_t const&, home_executor<home_>&) noexcept;
    status_t (*sample)(ustore_docs_sample_t const&, view_t const&, home_executor<home_>&) noexcept;
};

/** The verbs of graph collections served at one home. */
template <home_t>
struct graph_verbs;

/** The verbs of graph collections on host threads. */
template <>
struct graph_verbs<home_t::host_k> {
    status_t (*read_edges)(ustore_graph_read_edges_t const&, view_t const&) noexcept;
    status_t (*write_edges)(ustore_graph_write_edges_t const&, view_t const&) noexcept;
    status_t (*remove_edges)(ustore_graph_remove_edges_t const&, view_t const&) noexcept;
    status_t (*write_vertices)(ustore_graph_write_vertices_t const&, view_t const&) noexcept;
    status_t (*remove_vertices)(ustore_graph_remove_vertices_t const&, view_t const&) noexcept;
    status_t (*scan_vertices)(ustore_graph_scan_vertices_t const&, view_t const&) noexcept;
    status_t (*sample_vertices)(ustore_graph_sample_vertices_t const&, view_t const&) noexcept;
    status_t (*measure_vertices)(ustore_graph_measure_vertices_t const&, view_t const&) noexcept;
};

/** The verbs of graph collections on a device. */
template <home_t home_>
    requires(home_ != home_t::host_k)
struct graph_verbs<home_> {
    status_t (*read_edges)(ustore_graph_read_edges_t const&, view_t const&, home_executor<home_>&) noexcept;
    status_t (*scan_vertices)(ustore_graph_scan_vertices_t const&, view_t const&, home_executor<home_>&) noexcept;
};

/** The verbs of pages collections served at one home. */
template <home_t>
struct pages_verbs;

/** The verbs of pages collections on host threads. */
template <>
struct pages_verbs<home_t::host_k> {
    status_t (*import)(ustore_pages_import_t const&, strings_arg_t const& values, view_t const&) noexcept;
    status_t (*insert)(ustore_pages_insert_t const&, strings_arg_t const& values, view_t const&) noexcept;
    status_t (*remove)(ustore_pages_remove_t const&, view_t const&) noexcept;
    status_t (*move)(ustore_pages_move_t const&, view_t const&) noexcept;
    status_t (*splice)(ustore_pages_splice_t const&, strings_arg_t const& values, view_t const&) noexcept;
    status_t (*set_attributes)(ustore_pages_set_attributes_t const&, strings_arg_t const& fields,
                               strings_arg_t const& values, view_t const&) noexcept;
    status_t (*remove_attributes)(ustore_pages_remove_attributes_t const&, strings_arg_t const& fields,
                                  view_t const&) noexcept;
    status_t (*read)(ustore_pages_read_t const&, view_t const&) noexcept;
    status_t (*scan)(ustore_pages_scan_t const&, view_t const&) noexcept;
    status_t (*sample)(ustore_pages_sample_t const&, view_t const&) noexcept;
    status_t (*measure)(ustore_pages_measure_t const&, view_t const&) noexcept;
};

/** The verbs of pages collections on a device. */
template <home_t home_>
    requires(home_ != home_t::host_k)
struct pages_verbs<home_> {
    status_t (*read)(ustore_pages_read_t const&, view_t const&, home_executor<home_>&) noexcept;
    status_t (*scan)(ustore_pages_scan_t const&, view_t const&, home_executor<home_>&) noexcept;
};

/** The verbs of every modality an engine serves at one home; a null host table or verb answers
 *  @c ustore_status_unsupported_k, while every device table is filled. */
template <home_t home_>
struct home_tables {
    blobs_verbs<home_> const* blobs = nullptr;
    strings_verbs<home_> const* strings = nullptr;
    docs_verbs<home_> const* docs = nullptr;
    graph_verbs<home_> const* graph = nullptr;
    pages_verbs<home_> const* pages = nullptr;
};

/** Answers a device verb whose stage has not landed yet, which fills every entry of the device tables. */
template <typename... parameters_types_>
status_t unsupported_on_vram(parameters_types_...) noexcept {
    return {status_code_t::unsupported_k, "No engine serves this call on devices yet"};
}

template <home_t home_>
inline constexpr blobs_verbs<home_> unsupported_vram_blobs {
    .read = &unsupported_on_vram,
    .scan = &unsupported_on_vram,
    .sample = &unsupported_on_vram,
};

template <home_t home_>
inline constexpr strings_verbs<home_> unsupported_vram_strings {
    .read = &unsupported_on_vram,
    .scan = &unsupported_on_vram,
};

template <home_t home_>
inline constexpr docs_verbs<home_> unsupported_vram_docs {
    .read = &unsupported_on_vram,
    .gather = &unsupported_on_vram,
    .scan = &unsupported_on_vram,
    .sample = &unsupported_on_vram,
};

template <home_t home_>
inline constexpr graph_verbs<home_> unsupported_vram_graph {
    .read_edges = &unsupported_on_vram,
    .scan_vertices = &unsupported_on_vram,
};

template <home_t home_>
inline constexpr pages_verbs<home_> unsupported_vram_pages {
    .read = &unsupported_on_vram,
    .scan = &unsupported_on_vram,
};

/** The device tables of an engine with no device tower yet at @p home_, every entry answering
 *  @c unsupported_on_vram. */
template <home_t home_>
inline constexpr home_tables<home_> unsupported_vram_tables {
    .blobs = &unsupported_vram_blobs<home_>,
    .strings = &unsupported_vram_strings<home_>,
    .docs = &unsupported_vram_docs<home_>,
    .graph = &unsupported_vram_graph<home_>,
    .pages = &unsupported_vram_pages<home_>,
};

/**
 *  @brief One engine: its database verbs, and one table of modality verbs per home.
 *
 *  Every verb is @c noexcept and answers a status, which the router publishes into the caller's outcome: the in-house
 *  engines never throw, and an oracle wraps each call of its library in @c guarded. Every ragged input arrives
 *  readable in any layout, in the order its argument structure names them, since the router gathers what an accessor
 *  writes.
 */
struct engine_t {

    /** The control plane: opens, compactions, transactions and snapshots, on host threads alone. */
    database_verbs_t const* database = nullptr;

    /** The modality verbs on host threads. */
    home_tables<home_t::host_k> host;

    /** The modality verbs on one NVIDIA device. */
    home_tables<home_t::vram_nvidia_k> vram_nvidia = unsupported_vram_tables<home_t::vram_nvidia_k>;

    /** The modality verbs on one AMD device, which no engine serves yet. */
    home_tables<home_t::vram_amd_k> vram_amd = unsupported_vram_tables<home_t::vram_amd_k>;

    /** The tables of the verbs served at @p home_. */
    template <home_t home_>
    home_tables<home_> const& tables() const noexcept {
        if constexpr (home_ == home_t::host_k) return host;
        else if constexpr (home_ == home_t::vram_nvidia_k) return vram_nvidia;
        else return vram_amd;
    }
};

/** Whether a database takes changes, or the router refuses every call that would change it. */
enum class access_t : std::uint8_t {

    /** Every call serves, as every open but a view leaves it. */
    read_write_k,

    /** Writes, removes, transactions, collection creates and drops, clears and compactions answer
     *  @c ustore_status_read_only_k, as a view opens it. */
    read_only_k,
};

/** The engine's own state behind @p handle, a database, transaction or snapshot handle, which only the entries of
 *  the engine that made it cast back to their type, or null for a null @p handle. */
template <typename state_type_, typename handle_type_>
state_type_* state_of(handle_type_* handle) noexcept {
    return handle ? static_cast<state_type_*>(handle->state) : nullptr;
}

/** The engine behind @p handle, as the engine stamped it. */
template <typename handle_type_>
engine_t const& engine_of(handle_type_* handle) noexcept {
    return *handle->engine;
}

/** The transaction state of @p state_type_ behind @p transaction, or @c uninitialized_k for a
 *  call naming none. */
template <typename state_type_>
expected<state_type_*> transaction_state_of(ustore_transaction_t transaction) noexcept {
    if (!transaction) return status_t {status_code_t::uninitialized_k, "Transaction is uninitialized"};
    return state_of<state_type_>(transaction);
}

/** A handle of @p handle_type_ stamped with its engine and the state of @p state_type_ it is
 *  about to own, kept apart until @c release publishes them, so an open or a pin failing
 *  between frees both. */
template <typename handle_type_, typename state_type_>
struct made_handle {

    /** The handle, its engine stamped and its state still null. */
    std::unique_ptr<handle_type_> handle;

    /** The engine's own state, which @c release hands to the handle. */
    std::unique_ptr<state_type_> state;

    /** Hands the state to the handle and the handle to the caller, which owns it from here on. */
    [[nodiscard]] handle_type_* release() noexcept {
        handle->state = state.release();
        return handle.release();
    }
};

/** A handle of @p handle_type_ stamped with @p engine over a state of @p state_type_ made from
 *  @p arguments, or @c out_of_memory_k. */
template <typename handle_type_, typename state_type_, typename... arguments_types_>
expected<made_handle<handle_type_, state_type_>> make_handle(engine_t const& engine,
                                                             arguments_types_&&... arguments) noexcept {
    made_handle<handle_type_, state_type_> made;
    made.handle.reset(new (std::nothrow) handle_type_ {});
    if (made.handle) made.state.reset(new (std::nothrow) state_type_(std::forward<arguments_types_>(arguments)...));
    if (!made.state) return status_t {status_code_t::out_of_memory_k, "Failed to allocate the handle and its state"};
    made.handle->engine = &engine;
    return made;
}

/** The database a call addresses: the one it names, or the one its snapshot holds open. */
template <typename arguments_type_>
ustore_database_t database_of(arguments_type_ const& arguments) noexcept {
    if constexpr (requires { arguments.database; }) return arguments.database;
    else if constexpr (requires { arguments.cursor->cursor.database; })
        return arguments.cursor ? arguments.cursor->cursor.database : nullptr;
    else return arguments.snapshot ? arguments.snapshot->database : nullptr;
}

/** The status every verb answers for a collection holding another modality than the verb serves. */
inline status_t collection_holds_another_modality() noexcept {
    return {status_code_t::invalid_argument_k, "The collection holds another modality"};
}

/** Admits a collection holding @p held to the verbs of @p served only when both name one modality. */
inline status_t admit_modality(ustore_modality_t held, ustore_modality_t served) noexcept {
    if (held == served) return {};
    return collection_holds_another_modality();
}

/**
 *  @brief Serves @p serve_, an unchecked verb of one engine, after @p resolve_, that engine's resolver, refuses every
 *      collection of the call holding no @p modality_.
 *
 *  Every engine's resolver is called as <tt>resolve_(database, collections, count, modality, view)</tt>, refusing a
 *  collection it does not list, as @p view sees it, and one @c admit_modality refuses.
 */
template <ustore_modality_t modality_, auto resolve_, auto serve_, typename arguments_type_>
status_t guarded_by(arguments_type_ const& arguments, view_t const& view) noexcept {
    status_t const resolved = resolve_(arguments.database, arguments.collections, arguments.count, modality_, view);
    return resolved.failed() ? resolved : serve_(arguments, view);
}

/** Serves @p serve_, taking one ragged input, after @p resolve_ refuses every collection holding no @p modality_. */
template <ustore_modality_t modality_, auto resolve_, auto serve_, typename arguments_type_>
status_t guarded_by(arguments_type_ const& arguments, strings_arg_t const& values, view_t const& view) noexcept {
    status_t const resolved = resolve_(arguments.database, arguments.collections, arguments.count, modality_, view);
    return resolved.failed() ? resolved : serve_(arguments, values, view);
}

/** Removes the whole values a removal of collections holding @p modality_ names through @p values_verbs_, after
 *  @p resolve_ refuses every collection holding another modality: the documents of @c ustore_docs_remove. */
template <blobs_verbs<home_t::host_k> const& values_verbs_, auto resolve_, ustore_modality_t modality_,
          typename arguments_type_>
status_t remove_over_blobs(arguments_type_ const& arguments, view_t const& view) noexcept {
    status_t const resolved = resolve_(arguments.database, arguments.collections, arguments.count, modality_, view);
    if (resolved.failed()) return resolved;
    ustore_blobs_remove_t const remove {
        .database = arguments.database,
        .executor = arguments.executor,
        .outcome = arguments.outcome,
        .completion = arguments.completion,
        .transaction = arguments.transaction,
        .count = arguments.count,
        .collections = arguments.collections,
        .keys = arguments.keys,
        .end_keys = arguments.end_keys,
        .durability = arguments.durability,
        .watch = arguments.watch,
    };
    return values_verbs_.remove(remove, view);
}

/** Scans the keys of collections holding @p modality_ through @p values_verbs_, after @p resolve_ refuses every
 *  collection holding another modality: the documents, vertices and pages of the scans of those modalities. */
template <blobs_verbs<home_t::host_k> const& values_verbs_, auto resolve_, ustore_modality_t modality_,
          typename arguments_type_>
status_t scan_over_blobs(arguments_type_ const& arguments, view_t const& view) noexcept {
    status_t const resolved = resolve_(arguments.database, arguments.collections, arguments.count, modality_, view);
    if (resolved.failed()) return resolved;
    ustore_blobs_scan_t const scan {
        .database = arguments.database,
        .executor = arguments.executor,
        .outcome = arguments.outcome,
        .completion = arguments.completion,
        .view = arguments.view,
        .count = arguments.count,
        .collections = arguments.collections,
        .start_keys = arguments.start_keys,
        .end_keys = arguments.end_keys,
        .direction = arguments.direction,
        .offsets = arguments.offsets,
        .keys = arguments.keys,
        .arena = arguments.arena,
        .wanted = arguments.wanted,
        .mode = arguments.mode,
        .progress = arguments.progress,
        .progress_state = arguments.progress_state,
    };
    return values_verbs_.scan(scan, view);
}

/** Samples the keys of collections holding @p modality_ through @p values_verbs_, after @p resolve_
 *  refuses every collection holding another modality: the documents, vertices and pages of the
 *  samples of those modalities. */
template <blobs_verbs<home_t::host_k> const& values_verbs_, auto resolve_, ustore_modality_t modality_,
          typename arguments_type_>
status_t sample_over_blobs(arguments_type_ const& arguments, view_t const& view) noexcept {
    status_t const resolved = resolve_(arguments.database, arguments.collections, arguments.count, modality_, view);
    if (resolved.failed()) return resolved;
    ustore_blobs_sample_t const sample {
        .database = arguments.database,
        .executor = arguments.executor,
        .outcome = arguments.outcome,
        .completion = arguments.completion,
        .view = arguments.view,
        .count = arguments.count,
        .collections = arguments.collections,
        .offsets = arguments.offsets,
        .keys = arguments.keys,
        .arena = arguments.arena,
        .wanted = arguments.wanted,
    };
    return values_verbs_.sample(sample, view);
}

/** Measures the key ranges of collections holding @p modality_ through @p values_verbs_, after
 *  @p resolve_ refuses every collection holding another modality: the documents, vertices and pages
 *  of the measures of those modalities. */
template <blobs_verbs<home_t::host_k> const& values_verbs_, auto resolve_, ustore_modality_t modality_,
          typename arguments_type_>
status_t measure_over_blobs(arguments_type_ const& arguments, view_t const& view) noexcept {
    status_t const resolved = resolve_(arguments.database, arguments.collections, arguments.count, modality_, view);
    if (resolved.failed()) return resolved;
    ustore_blobs_measure_t const measure {
        .database = arguments.database,
        .executor = arguments.executor,
        .outcome = arguments.outcome,
        .completion = arguments.completion,
        .view = arguments.view,
        .count = arguments.count,
        .collections = arguments.collections,
        .start_keys = arguments.start_keys,
        .end_keys = arguments.end_keys,
        .direction = arguments.direction,
        .min_cardinalities = arguments.min_cardinalities,
        .max_cardinalities = arguments.max_cardinalities,
        .min_value_bytes = arguments.min_value_bytes,
        .max_value_bytes = arguments.max_value_bytes,
        .min_space_usages = arguments.min_space_usages,
        .max_space_usages = arguments.max_space_usages,
        .arena = arguments.arena,
    };
    return values_verbs_.measure(measure, view);
}

/** Mints a UUIDv7 for a new collection, refusing when the system gives no entropy. */
expected<ustore_collection_id_t> mint_collection_id() noexcept;

/** What an executor tells the NVMe databases it opens: how they move bytes, and on which threads. */
struct nvme_placement_t {

    /** How the databases move bytes, zeroed for the engine's defaults. */
    ustore_nvme_options_t nvme {};

    /** Threads the executor runs, each keeping an I/O channel of its own, or zero for the engine's default. */
    std::size_t threads = 0;

    /** The executor whose pool threads and holding call keep a channel and an epoch slot of their
     *  own in the databases it opens, or null for an executor of no NVMe placement. */
    ustore_executor_handle_t const* executor = nullptr;
};

/** The placement @p executor carries when @c ustore_executor_for_nvme made it, and the defaults for any other and for
 *  none. */
nvme_placement_t nvme_placement_of(ustore_executor_t executor) noexcept;

/** Serves @p serve_ of an oracle, whose library may throw, turning whatever it throws into a status. */
template <auto serve_, typename... arguments_types_>
status_t guarded(arguments_types_ const&... arguments) noexcept {
    try {
        return serve_(arguments...);
    }
    catch (std::bad_alloc const&) {
        return {status_code_t::out_of_memory_k, "Out of memory"};
    }
    catch (...) {
        return {status_code_t::internal_k, "Unexpected engine failure"};
    }
}

/** What the entries of a collection map, which picks how an engine serializes them. */
enum class entry_layout_t : std::uint8_t {

    /** A 16-byte UUID onto bytes: blobs, documents, pages, graph vertices and the collections system collection. */
    uuids_to_bytes_k,

    /** A string onto a 16-byte UUID: the strings modality, and the names system collection. */
    strings_to_uuids_k,
};

/** The layout of the entries of a collection holding @p modality. */
constexpr entry_layout_t entry_layout_of(ustore_modality_t modality) noexcept {
    return modality == ustore_modality_strings_k ? entry_layout_t::strings_to_uuids_k
                                                 : entry_layout_t::uuids_to_bytes_k;
}

#pragma region Executors

/** Every argument struct that names an executor, the inits, opens and views of the engines
 *  included, so a delegated call keeps its arguments by value. */
using deferred_arguments_t = std::variant<
    ustore_database_describe_t, ustore_database_clear_t, ustore_database_compact_t, ustore_database_flush_t,
    ustore_snapshot_init_t, ustore_snapshot_save_t, ustore_collection_list_t, ustore_collection_create_t,
    ustore_collection_find_t, ustore_collection_drop_t, ustore_transaction_init_t, ustore_transaction_stage_t,
    ustore_transaction_commit_t, ustore_transaction_rollback_t, ustore_blobs_write_t, ustore_blobs_remove_t,
    ustore_blobs_read_t, ustore_blobs_scan_t, ustore_blobs_sample_t, ustore_blobs_measure_t, ustore_strings_sample_t,
    ustore_strings_measure_t, ustore_docs_measure_t, ustore_graph_sample_vertices_t, ustore_graph_measure_vertices_t,
    ustore_pages_sample_t, ustore_pages_measure_t, ustore_docs_write_t, ustore_docs_read_t, ustore_docs_gist_t,
    ustore_docs_gather_t, ustore_docs_remove_t, ustore_docs_scan_t, ustore_docs_sample_t, ustore_graph_read_edges_t,
    ustore_graph_write_edges_t, ustore_graph_remove_edges_t, ustore_graph_write_vertices_t,
    ustore_graph_remove_vertices_t, ustore_graph_scan_vertices_t, ustore_pages_import_t, ustore_pages_insert_t,
    ustore_pages_remove_t, ustore_pages_move_t, ustore_pages_splice_t, ustore_pages_set_attributes_t,
    ustore_pages_remove_attributes_t, ustore_pages_read_t, ustore_pages_scan_t, ustore_strings_write_t,
    ustore_strings_remove_t, ustore_strings_read_t, ustore_strings_scan_t, ustore_blobs_cursor_next_t,
    ustore_strings_cursor_next_t, ustore_docs_cursor_next_t, ustore_pages_cursor_next_t,
    ustore_graph_vertices_cursor_next_t, ustore_changes_cursor_init_t, ustore_changes_cursor_next_t,
    ustore_database_init_ram_t, ustore_database_open_ram_t, ustore_database_open_rocksdb_t,
    ustore_database_view_rocksdb_t, ustore_database_open_leveldb_t, ustore_database_open_nvme_t>;

/** The one call that holds a UStore executor, from its claim to its end. */
struct holding_call_t {

    /** Free, or held by the call from its claim to its end. */
    claim_t claim = claim_t::free_k;

    /** A delegated call's copy, served on pool thread 0, as the caller's struct may die first. */
    deferred_arguments_t delegated_arguments;

    /** The caller's outcome, which @c end_call finishes last. */
    ustore_outcome_t* outcome = nullptr;

    /** What the engine answered, which @c end_call publishes. */
    status_t recorded;

    /** The counts of the database, the transaction, snapshot or cursor, and the arena the call
     *  names, raised at its claim and lowered at its end, each null when absent. */
    std::array<std::uint32_t*, 3> named_calls {};

    /** The arena, cursor and transaction claims the call took, released first at its end. */
    std::array<claim_t*, 3> claims {};

    /** The thread serving the call, its caller or pool thread 0 for a delegated one, empty while
     *  the executor is free, through the floor class's reference. */
    std::thread::id body_thread;
};

/** The right to dispatch on @p pool, owned or lent. */
inline utilities::pool_view_t view_of(
    std::variant<utilities::executor_t, utilities::lent_pool_t> const& pool) noexcept {
    return std::visit([](auto const& alternative) noexcept { return alternative.view(); }, pool);
}

/** What one handle may use: the masks this machine runs when nothing is pinned, the pinned class's masks when it runs
 *  them, as @c pick_cpu_ceiling picks them once at open. */
struct cpu_ceiling_t {
    sz_capability_t strings = sz_cap_serial_k;
    fu::capabilities_t sync = fu::capabilities_unknown_k;
};

#pragma endregion Executors

#pragma region CPU Classes

/** The CPU families UStore ships variants for, named as USearch names them. Each class bundles the
 *  wait instruction and the atomics its silicon generation guarantees, and the StringZilla kernels
 *  its calls dispatch to; only a class whose strings kit, waiter or atomic reference differs from
 *  its parent's carries a variant. */
enum class cpu_class_t : std::uint32_t {

    /** The zero value, which names no class. */
    unknown_k = 0,

    /** The portable floor: standard atomics, spinning on the hint every core of its architecture
     *  carries, @c PAUSE or @c YIELD, and yielding to the scheduler elsewhere. */
    serial_k,

    /** Haswell-class x86: the @c serial_k waits and atomics, and AVX2 string kernels. */
    intel_haswell_k,

    /** Sierra Forest-class x86 - the E-core Xeons, and Diamond Rapids after them: @c TPAUSE waits
     *  and @c CMPCCXADD conditional adds. */
    intel_sierra_k,

    /** Skylake-class x86 servers: AVX-512 string kernels over the @c intel_haswell_k
     *  waits and atomics. */
    intel_skylake_k,

    /** Ice Lake-class x86 servers: the @c intel_skylake_k kernels widened by VBMI2 and SHA-NI. */
    intel_icelake_k,

    /** Graviton2-class Arm - Neoverse N1: the @c serial_k waits and atomics, and
     *  NEON string kernels. */
    aws_g2_k,

    /** Armv8.3+ Arm, named for Apple M2: single-instruction LSE read-modify-writes under
     *  RCpc acquiring loads. */
    apple_m2_k,

    /** Graviton3-class Arm and newer: the @c apple_m2_k atomics with @c WFE waits. */
    aws_g5_k,

    /** RVA23-class RISC-V, named for Alibaba's C930: @c WRS waits, Zacas compare-and-swaps and
     *  RVV string kernels. */
    alibaba_c930_k,

    /** The one class of a monoversioned build: the waiter, atomic reference and
     *  string kernels @c USTORE_MARCH implies or a preset vouches for, as the
     *  libraries compiled them. */
    monoversioned_k,
};

/** The class a refused rung degrades to - the next one down its own lineage, which is the class a
 *  variant must be distinct from to earn instantiation. */
constexpr cpu_class_t parent_class_of(cpu_class_t cpu_class) noexcept {
    switch (cpu_class) {
    case cpu_class_t::intel_icelake_k: return cpu_class_t::intel_skylake_k;
    case cpu_class_t::intel_skylake_k: return cpu_class_t::intel_haswell_k;
    case cpu_class_t::intel_sierra_k: return cpu_class_t::intel_haswell_k;
    case cpu_class_t::apple_m2_k: return cpu_class_t::aws_g2_k;
    case cpu_class_t::aws_g5_k: return cpu_class_t::apple_m2_k;
    default: return cpu_class_t::serial_k;
    }
}

/** The wait instruction one CPU class pairs: the serial floor spins @c PAUSE on x86 and @c YIELD on
 *  Arm, the WAITPKG Xeons arm @c TPAUSE, the Graviton5 class sleeps in @c WFE on uncapped waits,
 *  everything else yields - as does any class whose @c FORKUNION_TARGET_<BIT> is off in the probe
 *  list. The timed @c WFET only a monoversioned build reaches, where a preset vouches for it. */
template <cpu_class_t cpu_class_>
struct waiter_for_cpu_class {
    using type = fu::standard_yield_t;
};
#if FORKUNION_TARGET_X86_PAUSE
template <>
struct waiter_for_cpu_class<cpu_class_t::serial_k> {
    using type = fu::x86_pause_t;
};
template <>
struct waiter_for_cpu_class<cpu_class_t::intel_haswell_k> {
    using type = fu::x86_pause_t;
};
template <>
struct waiter_for_cpu_class<cpu_class_t::intel_skylake_k> {
    using type = fu::x86_pause_t;
};
template <>
struct waiter_for_cpu_class<cpu_class_t::intel_icelake_k> {
    using type = fu::x86_pause_t;
};
#endif
#if FORKUNION_TARGET_X86_TPAUSE
template <>
struct waiter_for_cpu_class<cpu_class_t::intel_sierra_k> {
    using type = fu::x86_tpause_t;
};
#endif
#if FORKUNION_TARGET_ARM64_YIELD
template <>
struct waiter_for_cpu_class<cpu_class_t::serial_k> {
    using type = fu::arm64_yield_t;
};
template <>
struct waiter_for_cpu_class<cpu_class_t::aws_g2_k> {
    using type = fu::arm64_yield_t;
};
template <>
struct waiter_for_cpu_class<cpu_class_t::apple_m2_k> {
    using type = fu::arm64_yield_t;
};
#endif
#if FORKUNION_TARGET_ARM64_WFE
template <>
struct waiter_for_cpu_class<cpu_class_t::aws_g5_k> {
    using type = fu::arm64_wfe_t;
};
#endif
#if FORKUNION_TARGET_RISCV64_WRS
template <>
struct waiter_for_cpu_class<cpu_class_t::alibaba_c930_k> {
    using type = fu::riscv64_wrs_t;
};
#endif
#if USTORE_TARGET_MONOVERSIONED
template <>
struct waiter_for_cpu_class<cpu_class_t::monoversioned_k> {
    using type = fu::preferred_yield_t;
};
#endif

/** The waiter a CPU class pairs, spelled short for the variants. */
template <cpu_class_t cpu_class_>
using waiter_of = typename waiter_for_cpu_class<cpu_class_>::type;

/** The atomic reference one CPU class pairs: single-instruction LSE read-modify-writes under RCpc
 *  acquiring loads on the Armv8.3+ classes and conditional adds on the CMPCCXADD Xeons - the
 *  floors, the older classes, and any class whose @c FORKUNION_TARGET_<BIT> is off in the probe
 *  list take the standard one. */
template <cpu_class_t cpu_class_>
struct atomic_ref_for_cpu_class {
    template <typename value_type_>
    using type = fu::standard_atomic_ref<value_type_>;
};
#if FORKUNION_TARGET_X86_CMPCCXADD
template <>
struct atomic_ref_for_cpu_class<cpu_class_t::intel_sierra_k> {
    template <typename value_type_>
    using type = fu::x86_cmpccxadd_atomic_ref<value_type_>;
};
#endif
#if FORKUNION_TARGET_ARM64_RCPC
template <>
struct atomic_ref_for_cpu_class<cpu_class_t::apple_m2_k> {
    template <typename value_type_>
    using type = fu::arm64_rcpc_atomic_ref<value_type_>;
};
template <>
struct atomic_ref_for_cpu_class<cpu_class_t::aws_g5_k> {
    template <typename value_type_>
    using type = fu::arm64_rcpc_atomic_ref<value_type_>;
};
#elif FORKUNION_TARGET_ARM64_LSE
template <>
struct atomic_ref_for_cpu_class<cpu_class_t::apple_m2_k> {
    template <typename value_type_>
    using type = fu::arm64_lse_atomic_ref<value_type_>;
};
template <>
struct atomic_ref_for_cpu_class<cpu_class_t::aws_g5_k> {
    template <typename value_type_>
    using type = fu::arm64_lse_atomic_ref<value_type_>;
};
#endif
#if FORKUNION_TARGET_RISCV64_ZACAS
template <>
struct atomic_ref_for_cpu_class<cpu_class_t::alibaba_c930_k> {
    template <typename value_type_>
    using type = fu::riscv64_zacas_atomic_ref<value_type_>;
};
#endif
#if USTORE_TARGET_MONOVERSIONED
template <>
struct atomic_ref_for_cpu_class<cpu_class_t::monoversioned_k> {
    template <typename value_type_>
    using type = fu::preferred_atomic_ref<value_type_>;
};
#endif

/** The reference a CPU class owns one word of @p value_type_ through, spelled short for the
 *  engines' own words. */
template <cpu_class_t cpu_class_, typename value_type_>
using atomic_ref_of = typename atomic_ref_for_cpu_class<cpu_class_>::template type<value_type_>;

/** The spin hints every core of an architecture carries - PAUSE, YIELD, Zihintpause: the runtime
 *  confirms nothing about them. */
inline constexpr fu::capabilities_t baseline_hints_k = fu::capability_x86_pause_k | fu::capability_arm64_yield_k |
                                                       fu::capability_riscv64_pause_k;

/** Lowers one CPU class onto ForkUnion's mask - the bits the runtime must confirm before the
 *  class's waiter and atomic reference are trusted, read off the two types themselves. */
template <cpu_class_t cpu_class_>
constexpr fu::capabilities_t forkunion_capabilities_of() noexcept {
    constexpr fu::capabilities_t waits_k = waiter_of<cpu_class_>::capability_k;
    constexpr fu::capabilities_t atomics_k = atomic_ref_of<cpu_class_, std::uint32_t>::capabilities_k;
    return static_cast<fu::capabilities_t>((waits_k | atomics_k) & ~baseline_hints_k);
}

/** Whether the build compiled the variants of @p cpu_class, as CMake decided and told every unit
 *  through @c USTORE_TARGET_<CLASS>; a ladder never picks a class without one. */
constexpr bool cpu_class_compiled(cpu_class_t cpu_class) noexcept {
    switch (cpu_class) {
#if USTORE_TARGET_SERIAL
    case cpu_class_t::serial_k: return true;
#endif
#if USTORE_TARGET_INTEL_HASWELL
    case cpu_class_t::intel_haswell_k: return true;
#endif
#if USTORE_TARGET_INTEL_SKYLAKE
    case cpu_class_t::intel_skylake_k: return true;
#endif
#if USTORE_TARGET_INTEL_ICELAKE
    case cpu_class_t::intel_icelake_k: return true;
#endif
#if USTORE_TARGET_INTEL_SIERRA
    case cpu_class_t::intel_sierra_k: return true;
#endif
#if USTORE_TARGET_AWS_G2
    case cpu_class_t::aws_g2_k: return true;
#endif
#if USTORE_TARGET_APPLE_M2
    case cpu_class_t::apple_m2_k: return true;
#endif
#if USTORE_TARGET_AWS_G5
    case cpu_class_t::aws_g5_k: return true;
#endif
#if USTORE_TARGET_ALIBABA_C930
    case cpu_class_t::alibaba_c930_k: return true;
#endif
#if USTORE_TARGET_MONOVERSIONED
    case cpu_class_t::monoversioned_k: return true;
#endif
    default: return false;
    }
}

/** ForkUnion's mask for a class named at run time; Haswell, Skylake and Ice Lake wait
 *  and synchronize alike. */
constexpr fu::capabilities_t forkunion_capabilities_of(cpu_class_t cpu_class) noexcept {
    switch (cpu_class) {
    case cpu_class_t::intel_haswell_k:
    case cpu_class_t::intel_skylake_k:
    case cpu_class_t::intel_icelake_k: return forkunion_capabilities_of<cpu_class_t::intel_haswell_k>();
    case cpu_class_t::intel_sierra_k: return forkunion_capabilities_of<cpu_class_t::intel_sierra_k>();
    case cpu_class_t::aws_g2_k: return forkunion_capabilities_of<cpu_class_t::aws_g2_k>();
    case cpu_class_t::apple_m2_k: return forkunion_capabilities_of<cpu_class_t::apple_m2_k>();
    case cpu_class_t::aws_g5_k: return forkunion_capabilities_of<cpu_class_t::aws_g5_k>();
    case cpu_class_t::alibaba_c930_k: return forkunion_capabilities_of<cpu_class_t::alibaba_c930_k>();
    case cpu_class_t::monoversioned_k: return forkunion_capabilities_of<cpu_class_t::monoversioned_k>();
    default: return forkunion_capabilities_of<cpu_class_t::serial_k>();
    }
}

/** The StringZilla capabilities one CPU class dispatches its string kernels on, as UCall's
 *  classes name them. */
constexpr sz_capability_t stringzilla_capabilities_of(cpu_class_t cpu_class) noexcept {
    switch (cpu_class) {
    case cpu_class_t::intel_haswell_k: return sz_cap_serial_k | sz_cap_haswell_k | sz_cap_westmere_k;
    case cpu_class_t::intel_skylake_k:
        return stringzilla_capabilities_of(cpu_class_t::intel_haswell_k) | sz_cap_skylake_k;
    case cpu_class_t::intel_icelake_k:
        return stringzilla_capabilities_of(cpu_class_t::intel_skylake_k) | sz_cap_icelake_k | sz_cap_goldmont_k;
    case cpu_class_t::intel_sierra_k:
        return stringzilla_capabilities_of(cpu_class_t::intel_haswell_k) | sz_cap_goldmont_k;
    case cpu_class_t::aws_g2_k:
    case cpu_class_t::apple_m2_k:
    case cpu_class_t::aws_g5_k: return sz_cap_serial_k | sz_cap_neon_k | sz_cap_neonaes_k | sz_cap_neonsha_k;
    case cpu_class_t::alibaba_c930_k: return sz_cap_serial_k | sz_cap_rvv_k;
    case cpu_class_t::monoversioned_k: return sz_capabilities_compiled_cpu_mask();
    default: return sz_cap_serial_k;
    }
}

/** The StringZilla kernels the stores and calls of @p cpu_class_ run their string work through. */
template <cpu_class_t cpu_class_>
using strings_kit_of = typename utilities::strings_ladder<stringzilla_capabilities_of(cpu_class_)>::strings_kit_t;

/** The SmashTable row kit one CPU class searches its trees and hash tables with; the Arm classes
 *  stay on NEON, as none of them requires SVE. */
constexpr st::row_kit_t row_kit_kind_of(cpu_class_t cpu_class) noexcept {
    switch (cpu_class) {
    case cpu_class_t::intel_haswell_k:
    case cpu_class_t::intel_sierra_k: return st::row_kit_t::haswell_k;
    case cpu_class_t::intel_skylake_k: return st::row_kit_t::skylake_k;
    case cpu_class_t::intel_icelake_k: return st::row_kit_t::icelake_k;
    case cpu_class_t::aws_g2_k:
    case cpu_class_t::apple_m2_k:
    case cpu_class_t::aws_g5_k: return st::row_kit_t::neon_k;
    case cpu_class_t::alibaba_c930_k: return st::row_kit_t::rvv_k;
    case cpu_class_t::monoversioned_k: return st::native_row_kit_t::kit_k;
    default: return st::row_kit_t::serial_k;
    }
}

/** The first of @p row_kit_types_ that @p kit_ names and this build compiled,
 *  else @p floor_type_. */
template <st::row_kit_t kit_, typename floor_type_, typename... row_kit_types_>
struct compiled_row_kit {
    using type = floor_type_;
};

template <st::row_kit_t kit_, typename floor_type_, typename head_type_, typename... tail_types_>
struct compiled_row_kit<kit_, floor_type_, head_type_, tail_types_...> {
    using type = std::conditional_t<head_type_::compiled_k && head_type_::kit_k == kit_, head_type_,
                                    typename compiled_row_kit<kit_, floor_type_, tail_types_...>::type>;
};

/** The SmashTable kit the trees and hash tables of @p cpu_class_ search with, the serial one where
 *  this build lacks the class's own. */
template <cpu_class_t cpu_class_>
using row_kit_of = typename compiled_row_kit<row_kit_kind_of(cpu_class_), st::serial_row_kit_t, st::icelake_row_kit_t,
                                             st::skylake_row_kit_t, st::haswell_row_kit_t, st::neon_row_kit_t,
                                             st::rvv_row_kit_t, st::native_row_kit_t>::type;

/** Whether one compiled class is distinct from the class it degrades to: another strings kit,
 *  waiter or atomic reference, the one fact justifying its variant. */
template <cpu_class_t cpu_class_>
consteval bool rung_advances() noexcept {
    constexpr cpu_class_t parent_k = parent_class_of(cpu_class_);
    return cpu_class_compiled(cpu_class_) &&
           (!std::is_same_v<strings_kit_of<cpu_class_>, strings_kit_of<parent_k>> ||
            !std::is_same_v<waiter_of<cpu_class_>, waiter_of<parent_k>> ||
            !std::is_same_v<atomic_ref_of<cpu_class_, std::uint32_t>, atomic_ref_of<parent_k, std::uint32_t>>);
}

/** The C enum's class as the engines name it; @c ustore_cpu_class_unknown_k derives nothing, so it
 *  names the serial floor. */
constexpr cpu_class_t cpu_class_of(ustore_cpu_class_t cpu_class) noexcept {
    if (cpu_class == ustore_cpu_class_unknown_k || cpu_class > ustore_cpu_class_monoversioned_k)
        return cpu_class_t::serial_k;
    return static_cast<cpu_class_t>(cpu_class);
}

static_assert(static_cast<std::uint32_t>(cpu_class_t::monoversioned_k) == ustore_cpu_class_monoversioned_k);

/** Whether @p rung fits under @p ceiling: its waiter, atomics and string kernels need nothing
 *  the ceiling lacks. */
constexpr bool cpu_rung_fits(cpu_class_t rung, cpu_ceiling_t ceiling) noexcept {
    return (stringzilla_capabilities_of(rung) & ~ceiling.strings) == 0 &&
           (forkunion_capabilities_of(rung) & ~ceiling.sync) == 0;
}

/** The mask a pool of an executor under @p ceiling is allowed: every placement bit, and the
 *  instructions the ceiling keeps. */
constexpr fu_capabilities_t forkunion_allowed_of(cpu_ceiling_t ceiling) noexcept {
    return static_cast<fu_capabilities_t>((fu_capabilities_all_k & ~fu_capability_any_instruction_k) | ceiling.sync |
                                          baseline_hints_k);
}

/** CPU classes newest first, the order a ladder tries them in. */
template <cpu_class_t... classes_>
struct cpu_ladder {};

/** The classes carrying variants, each lineage newest first and the serial floor last, or the
 *  one class of a monoversioned build; a rung the build left out is skipped by the walk, never
 *  by the list. */
#if USTORE_TARGET_MONOVERSIONED
using cpu_ladder_t = cpu_ladder<cpu_class_t::monoversioned_k>;
inline constexpr cpu_class_t cpu_ladder_floor_k = cpu_class_t::monoversioned_k;
#else
using cpu_ladder_t = cpu_ladder<cpu_class_t::intel_icelake_k, cpu_class_t::intel_skylake_k, cpu_class_t::intel_sierra_k,
                                cpu_class_t::intel_haswell_k, cpu_class_t::aws_g5_k, cpu_class_t::apple_m2_k,
                                cpu_class_t::aws_g2_k, cpu_class_t::alibaba_c930_k, cpu_class_t::serial_k>;
inline constexpr cpu_class_t cpu_ladder_floor_k = cpu_class_t::serial_k;
#endif

/** What a handle may use: the masks this machine runs when nothing is pinned, the pinned class's
 *  masks when it runs them, refusing an unknown class, one this machine does not run, or a ceiling
 *  the build compiled no class under, as a monoversioned build on an older CPU or under a pin below
 *  its one class. The one caller of StringZilla's and ForkUnion's detection. */
inline expected<cpu_ceiling_t> pick_cpu_ceiling(ustore_cpu_class_t pinned) noexcept {
    cpu_ceiling_t ceiling;
    if (sz_capabilities_detected_cpu(&ceiling.strings) != sz_success_k) ceiling.strings = sz_cap_serial_k;
    ceiling.sync = fu::cpu_capabilities();
    if (pinned > ustore_cpu_class_monoversioned_k)
        return status_t {status_code_t::invalid_argument_k, "Unknown CPU class"};
    if (pinned != ustore_cpu_class_unknown_k) {
        cpu_class_t const pinned_class = cpu_class_of(pinned);
        cpu_ceiling_t const wanted {stringzilla_capabilities_of(pinned_class), forkunion_capabilities_of(pinned_class)};
        if ((ceiling.strings & wanted.strings) != wanted.strings || (ceiling.sync & wanted.sync) != wanted.sync)
            return status_t {status_code_t::unsupported_k, "The CPU does not run the pinned class"};
        ceiling = wanted;
    }
    bool const compiled = []<cpu_class_t... classes_>(cpu_ladder<classes_...>, cpu_ceiling_t under) noexcept {
        return ((cpu_class_compiled(classes_) && cpu_rung_fits(classes_, under)) || ...);
    }(cpu_ladder_t {}, ceiling);
    if (!compiled) return status_t {status_code_t::unsupported_k, "The build compiled no class the CPU runs"};
    return ceiling;
}

/** Opens through @p open on the newest rung of @p ladder the build compiled and @p ceiling fits,
 *  the serial floor last, calling it as <tt>open.template operator()<class>()</tt>; a class picking its
 *  parent's waiter and atomics is skipped without instantiating its variant. */
template <cpu_class_t... classes_, typename open_type_>
status_t open_fitting_rung(cpu_ladder<classes_...>, cpu_ceiling_t ceiling, open_type_&& open) noexcept {
    status_t opened;
    bool picked = false;
    auto rung = [&]<cpu_class_t class_>() noexcept {
        constexpr bool floor_k = class_ == cpu_class_t::serial_k || class_ == cpu_class_t::monoversioned_k;
        if constexpr ((floor_k && cpu_class_compiled(class_)) || rung_advances<class_>()) {
            if (!picked && cpu_rung_fits(class_, ceiling)) picked = true, opened = open.template operator()<class_>();
        }
        else static_assert(!cpu_class_compiled(class_), "A compiled class no longer advances past the one below");
    };
    (rung.template operator()<classes_>(), ...);
    return opened;
}

/** The verbs over the words a call's waiters and claims touch, lowered onto one CPU class's
 *  reference and waiter; the release store is the same instruction on every class, the wait and the
 *  exchange are what differ. */
struct class_verbs_t {
    void (*publish)(ustore_outcome_t*, ustore_status_t, std::memory_order) noexcept;
    ustore_status_t (*load)(ustore_outcome_t const*) noexcept;
    ustore_status_t (*wait)(ustore_outcome_t const*) noexcept;
    bool (*take)(claim_t*) noexcept;
    void (*release)(claim_t*) noexcept;
    void (*wait_while_claimed)(claim_t const*) noexcept;

    /** Waits until the word stops reading the value seen, watching its address, so
     *  no store notifies. */
    void (*await_change)(std::uint32_t const*, std::uint32_t) noexcept;
};

template <cpu_class_t cpu_class_>
class_verbs_t class_verbs_of() noexcept {
    using status_ref_t = atomic_ref_of<cpu_class_, ustore_status_t>;
    using status_reader_t = atomic_ref_of<cpu_class_, ustore_status_t const>;
    using claim_ref_t = atomic_ref_of<cpu_class_, claim_t>;
    using claim_reader_t = atomic_ref_of<cpu_class_, claim_t const>;
    using word_reader_t = atomic_ref_of<cpu_class_, std::uint32_t const>;
    using waiter_t = waiter_of<cpu_class_>;
    return {
        [](ustore_outcome_t* outcome, ustore_status_t status, std::memory_order order) noexcept {
            status_ref_t(outcome->status).store(status, order);
        },
        [](ustore_outcome_t const* outcome) noexcept {
            return status_reader_t(outcome->status).load(std::memory_order_acquire);
        },
        [](ustore_outcome_t const* outcome) noexcept {
            status_reader_t const status {outcome->status};
            ustore_status_t seen = status.load(std::memory_order_acquire);
            for (; seen == ustore_status_deferred_k; seen = status.load(std::memory_order_acquire))
                waiter_t {}(&outcome->status, seen, 0u, fu::wait_capped_t {});
            return seen;
        },
        [](claim_t* word) noexcept {
            return claim_ref_t(*word).exchange(claim_t::held_k, std::memory_order_seq_cst) == claim_t::free_k;
        },
        [](claim_t* word) noexcept { claim_ref_t(*word).store(claim_t::free_k, std::memory_order_release); },
        [](claim_t const* word) noexcept {
            claim_reader_t const claim {*word};
            while (claim.load(std::memory_order_acquire) == claim_t::held_k)
                waiter_t {}(word, claim_t::held_k, 0u, fu::wait_uncapped_k);
        },
        [](std::uint32_t const* word, std::uint32_t seen) noexcept {
            word_reader_t const watched {*word};
            while (watched.load(std::memory_order_acquire) == seen) waiter_t {}(word, seen, 0u, fu::wait_uncapped_k);
        },
    };
}

/** The row of the newest compiled class this machine admits, picked once by the ladder walk. */
inline class_verbs_t const& host_verbs() noexcept {
    static class_verbs_t const picked = []<cpu_class_t... classes_>(cpu_ladder<classes_...>) noexcept {
        expected<cpu_ceiling_t> const ceiling = pick_cpu_ceiling(ustore_cpu_class_unknown_k);
        class_verbs_t row = class_verbs_of<cpu_ladder_floor_k>();
        bool found = false;
        auto rung = [&]<cpu_class_t class_>() noexcept {
            if (!found && ceiling && cpu_class_compiled(class_) && cpu_rung_fits(class_, *ceiling))
                row = class_verbs_of<class_>(), found = true;
        };
        (rung.template operator()<classes_>(), ...);
        return row;
    }(cpu_ladder_t {});
    return picked;
}

/** The reference class-free host words share, the floor's, which every machine this build
 *  runs on admits. */
template <typename value_type_>
using floor_atomic_ref = atomic_ref_of<cpu_ladder_floor_k, value_type_>;

/** Counts one more open reader, a snapshot or a transaction, on @p open_readers, through the
 *  reference of @p cpu_class_. */
template <cpu_class_t cpu_class_>
void count_reader_opened(std::size_t& open_readers) noexcept {
    atomic_ref_of<cpu_class_, std::size_t> {open_readers}.fetch_add(1, std::memory_order_release);
}

/** Counts one reader closed on @p open_readers, answering whether it was the last one open,
 *  after which whatever the open readers kept alive may go. */
template <cpu_class_t cpu_class_>
[[nodiscard]] bool count_reader_closed(std::size_t& open_readers) noexcept {
    return atomic_ref_of<cpu_class_, std::size_t> {open_readers}.fetch_sub(1, std::memory_order_acq_rel) == 1;
}

#pragma endregion CPU Classes

} // namespace unum::ustore

using namespace unum::ustore;

/** What @c ustore_database_t points to: the engine picked once at open, the state it serves, which only that engine's
 *  entries cast back, and the words the router synchronizes calls on. */
struct ustore_database_handle_t {
    engine_t const* engine = nullptr;
    void* state = nullptr;

    /** Whether calls may change the database, as an open or a view set it. */
    access_t access = access_t::read_write_k;

    /** The kernels and waits every call on the database may use, picked once at open. */
    cpu_ceiling_t ceiling;

    /** Executor calls naming the database that have not ended, through the floor reference. */
    std::uint32_t named_calls = 0;

    /** The caller's hold plus one per transaction, snapshot or cursor keeping the database open,
     *  through the floor class's reference. */
    std::uint32_t holds = 1;
};

/** What @c ustore_transaction_t points to: its engine and state, the database it keeps open, and the claim a call on
 *  it holds, so a second call on that transaction finds it contended. */
struct ustore_transaction_handle_t {
    engine_t const* engine = nullptr;
    void* state = nullptr;
    ustore_database_t database = nullptr;
    claim_t claim = claim_t::free_k;

    /** Executor calls naming the transaction that have not ended, through the floor reference. */
    std::uint32_t named_calls = 0;

    /** The caller's hold plus one per cursor reading through the transaction, through the
     *  floor class's reference. */
    std::uint32_t holds = 1;
};

/** What @c ustore_ingest_t points to: its engine and state, null once finished, the database it keeps open, and the
 *  scratch every call on it runs in, as the @c view_t the router hands its engine names it. */
struct ustore_ingest_handle_t {
    engine_t const* engine = nullptr;
    void* state = nullptr;
    ustore_database_t database = nullptr;
    ustore_arena_handle_t scratch {};
};

/** What @c ustore_snapshot_t points to: its engine and state, and the database it keeps open. */
struct ustore_snapshot_handle_t {
    engine_t const* engine = nullptr;
    void* state = nullptr;
    ustore_database_t database = nullptr;

    /** Executor calls naming the snapshot that have not ended, through the floor reference. */
    std::uint32_t named_calls = 0;

    /** The caller's hold plus one per cursor reading through the snapshot, through the
     *  floor class's reference. */
    std::uint32_t holds = 1;
};

/** What @c ustore_executor_t points to: the pool its calls fork on, the scratch of every thread,
 *  the one call holding it, and what the executor computes with. */
struct ustore_executor_handle_t {

    /** The pool every call forks on, owned or lent; an exclusive one also runs delegated calls. */
    std::variant<executor_t, lent_pool_t> pool;

    /** One scratch per pool thread and one for the call at index @c threads, each kept local. */
    st::vector<ustore_arena_handle_t> scratches;

    /** The one call that holds the executor, from its claim to its end. */
    holding_call_t holding_call;

    /** The memory domain of pool thread 0, holding the call's scratch and every arena made here. */
    fu_memory_domain_id_t memory_domain = 0;

    /** Nothing beyond the pool on RAM, the rings' options on NVMe, the device's stream on VRAM of either vendor; the
     *  variant is the kind tag, so no second word can disagree. */
    std::variant<std::monostate, ustore_nvme_options_t, cuda_executor_t, hip_executor_t> device;
};
