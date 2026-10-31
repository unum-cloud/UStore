/**
 *  @file src/engine.hpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief The verb tables every engine fills, which the router of <tt>src/lib.hpp</tt> routes each
 *      handle through, and the handles of databases, transactions, snapshots and executors.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`, `std::uint64_t`

#include <array>   // `std::array`
#include <new>     // `std::bad_alloc`
#include <thread>  // `std::thread::id`
#include <variant> // `std::variant`, `std::monostate`

#include <forkunion/capabilities.hpp>  // `fu::capabilities_t`
#include <smashtable/basic_vector.hpp> // `st::basic_vector`
#include <stringzilla/capabilities.h>  // `sz_capability_t`, `sz_cap_serial_k`

#include "ustore/blobs.h"   // `ustore_blobs_read_t`, `ustore_blobs_write_t` and the other binary calls
#include "ustore/db.h"      // `ustore_database_t`, `ustore_transaction_t`, `ustore_view_t` and the calls on them
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
using utilities::stringzilla_kernels_t;

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

    /** Which thread of @c executor runs the call: a pool thread's index, or the pool's size for a blocking call and a
     *  batch the driver runs itself, so no other call of that executor runs on the same index meanwhile. */
    std::size_t thread_index = 0;
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
    status_t (*remove)(ustore_strings_remove_t const&, strings_arg_t const& strings, view_t const&) noexcept;
    status_t (*read)(ustore_strings_read_t const&, strings_arg_t const& strings, view_t const&) noexcept;
    status_t (*scan)(ustore_strings_scan_t const&, strings_arg_t const& prefixes, strings_arg_t const& start_strings,
                     view_t const&) noexcept;
};

/** The verbs of strings collections on a device, whose strings a host index resolves. */
template <home_t home_>
    requires(home_ != home_t::host_k)
struct strings_verbs<home_> {
    status_t (*read)(ustore_strings_read_t const&, strings_arg_t const& strings, view_t const&,
                     home_executor<home_>&) noexcept;
    status_t (*scan)(ustore_strings_scan_t const&, strings_arg_t const& prefixes, strings_arg_t const& start_strings,
                     view_t const&, home_executor<home_>&) noexcept;
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

/** The database a call addresses: the one it names, or the one its snapshot holds open. */
template <typename arguments_type_>
ustore_database_t database_of(arguments_type_ const& arguments) noexcept {
    if constexpr (requires { arguments.database; }) return arguments.database;
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

/** Samples the keys of collections holding @p modality_ through @p values_verbs_, after @p resolve_ refuses every
 *  collection holding another modality: the documents of @c ustore_docs_sample. */
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

/** Mints a UUIDv7 for a new collection, refusing when the system gives no entropy. */
expected<ustore_collection_id_t> mint_collection_id() noexcept;

/** What an executor tells the NVMe databases it opens: how they move bytes, and on which threads. */
struct nvme_placement_t {

    /** How the databases move bytes, zeroed for the engine's defaults. */
    ustore_nvme_options_t nvme {};

    /** Threads the executor runs, each keeping an I/O channel of its own, or zero for the engine's default. */
    std::size_t threads = 0;

    /** The executor whose threads, its blocking caller's included, keep a channel and an epoch slot of their own in
     *  the databases it opens, or null for an executor of no NVMe placement. */
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

/** Deferred calls an executor holds at once when its constructor names no limit. */
constexpr std::size_t deferred_limit_default_k = 256;

/** The most deferred calls an executor holds, keeping every unfinished ticket within half the range of its counter. */
constexpr std::size_t deferred_limit_max_k = std::size_t {1} << 30;

/** Microseconds a parked pool sleeps between checks for the next batch. */
constexpr std::size_t parked_check_micros_k = 1000;

/** Every argument struct that names an executor, the inits, opens and views of the engines included, so a deferred
 *  submission keeps its call by value. */
using deferred_arguments_t =
    std::variant<ustore_database_describe_t, ustore_database_clear_t, ustore_database_compact_t, ustore_snapshot_init_t,
                 ustore_snapshot_save_t, ustore_collection_list_t, ustore_collection_create_t, ustore_collection_find_t,
                 ustore_collection_drop_t, ustore_transaction_init_t, ustore_transaction_stage_t,
                 ustore_transaction_commit_t, ustore_transaction_rollback_t, ustore_blobs_write_t,
                 ustore_blobs_remove_t, ustore_blobs_read_t, ustore_blobs_scan_t, ustore_blobs_sample_t,
                 ustore_blobs_measure_t, ustore_docs_write_t, ustore_docs_read_t, ustore_docs_gist_t,
                 ustore_docs_gather_t, ustore_docs_remove_t, ustore_docs_scan_t, ustore_docs_sample_t,
                 ustore_graph_read_edges_t, ustore_graph_write_edges_t, ustore_graph_remove_edges_t,
                 ustore_graph_write_vertices_t, ustore_graph_remove_vertices_t, ustore_graph_scan_vertices_t,
                 ustore_pages_import_t, ustore_pages_insert_t, ustore_pages_remove_t, ustore_pages_move_t,
                 ustore_pages_splice_t, ustore_pages_set_attributes_t, ustore_pages_remove_attributes_t,
                 ustore_pages_read_t, ustore_pages_scan_t, ustore_strings_write_t, ustore_strings_remove_t,
                 ustore_strings_read_t, ustore_strings_scan_t, ustore_blobs_cursor_next_t, ustore_strings_cursor_next_t,
                 ustore_docs_cursor_next_t, ustore_pages_cursor_next_t, ustore_graph_vertices_cursor_next_t,
                 ustore_database_init_ram_t, ustore_database_open_ram_t, ustore_database_open_rocksdb_t,
                 ustore_database_view_rocksdb_t, ustore_database_open_leveldb_t, ustore_database_open_nvme_t>;

/** What a deferred call leaves its driver, which publishes it once the call's table position is free again. */
struct deferred_result_t {

    /** The caller's outcome, which the driver finishes. */
    ustore_outcome_t* outcome = nullptr;

    /** What the engine answered. */
    status_t status;

    /** The deferred counts the call raised at submission, which the driver lowers once the outcome is final. */
    std::array<std::uint32_t*, 3> deferred_counts {};
};

/** One slot of the deferred table: a copy of the caller's argument struct and the routing that runs it. */
struct deferred_slot_t {

    /** A copy of the caller's argument struct, whose pointees stay borrowed until the call is final. */
    deferred_arguments_t arguments;

    /** Serves @c arguments in a runner's scratch and on its thread into a result for the driver to publish, as
     *  @c run_deferred does. */
    void (*run)(void* arguments, deferred_result_t& result, view_t const& runner) noexcept = nullptr;

    /** The call's place among every call on the executor, blocking ones included. */
    std::uint32_t ticket = 0;

    /** The slot's table position plus one once filled, stored with release ordering. */
    std::uint32_t ready = 0;
};

/** Whether the driver keeps serving the deferred table. */
enum class driver_state_t : std::uint32_t {

    /** Serving every deferred call submitted. */
    serving_k,

    /** Returning once the table is empty, as @c ustore_executor_free asks. */
    stopping_k,

    /** Never started, in a build without OS threads, where every call runs at submission. */
    absent_k,
};

/** What one handle may use: the masks this machine runs when nothing is pinned, the pinned class's masks when it runs
 *  them, as @c pick_cpu_ceiling picks them once at open. */
struct cpu_ceiling_t {
    sz_capability_t compute = sz_cap_serial_k;
    fu::capabilities_t sync = fu::capabilities_unknown_k;
};

/** Where an executor runs calls: one pool thread, or the calling thread of a blocking call. */
struct runner_t {

    /** The thread running a call here, empty while idle, through @c std::atomic_ref. */
    std::thread::id thread;

    /** The scratch every call run here works in, freed with the executor. */
    ustore_arena_handle_t scratch;
};

#pragma endregion Executors

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

    /** The StringZilla kernels calls on the database run directly, looked up under @c ceiling at open. */
    stringzilla_kernels_t kernels;

    /** Deferred calls naming the database that have not finished, through @c std::atomic_ref. */
    std::uint32_t deferred_calls = 0;

    /** The caller's hold plus one per transaction, snapshot or cursor keeping the database open, through
     *  @c std::atomic_ref. */
    std::uint32_t holds = 1;
};

/** What @c ustore_transaction_t points to: its engine and state, the database it keeps open, and the claim a call on
 *  it holds, so a second call on that transaction finds it contended. */
struct ustore_transaction_handle_t {
    engine_t const* engine = nullptr;
    void* state = nullptr;
    ustore_database_t database = nullptr;
    claim_t claim = claim_t::free_k;

    /** Deferred calls naming the transaction that have not finished, through @c std::atomic_ref. */
    std::uint32_t deferred_calls = 0;

    /** The caller's hold plus one per cursor reading through the transaction, through @c std::atomic_ref. */
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

    /** Deferred calls naming the snapshot that have not finished, through @c std::atomic_ref. */
    std::uint32_t deferred_calls = 0;

    /** The caller's hold plus one per cursor reading through the snapshot, through @c std::atomic_ref. */
    std::uint32_t holds = 1;
};

/** What @c ustore_executor_t points to: the threads that run calls, a pool owned or lent, the driver handing it
 *  batches, the deferred table, and what the executor computes with. */
struct ustore_executor_handle_t {

    /** The pool that runs deferred calls. */
    std::variant<executor_t, lent_pool_t> pool;

    /** One thread, waiting on @c doorbell while the table is empty. */
    executor_t driver;

    /** A ring of power-of-two slots, of which @c deferred_limit hold calls at once. */
    st::basic_vector<deferred_slot_t> deferred;

    /** One result per call of the batch the driver serves, read after the batch's table positions are free. */
    st::basic_vector<deferred_result_t> results;

    /** One runner per pool thread, plus one for a blocking call and a batch the driver runs itself. */
    st::basic_vector<runner_t> runners;

    /** Tickets taken in the low half and table positions taken in the high half. */
    std::uint64_t submitted = 0;

    /** The driver's loop, which @c ustore_executor_free joins. */
    fu_generation_t generation = 0;

    /** Deferred calls the table holds at once. */
    std::uint32_t deferred_limit = 0;

    /** Table positions the driver finished with, freeing the slots behind it. */
    std::uint32_t head = 0;

    /** Every ticket below it is final. */
    std::uint32_t finished = 0;

    /** Rung after every submission, waking the driver. */
    std::uint32_t doorbell = 0;

    /** What the pool and the driver do once the table empties. */
    ustore_mood_t mood = ustore_mood_parked_k;

    /** Whether the driver keeps serving. */
    driver_state_t state = driver_state_t::serving_k;

    /** The memory domain every host arena the executor makes lives on, the first one it serves. */
    fu_memory_domain_id_t memory_domain = 0;

    /** Nothing beyond the pool on RAM, the rings' options on NVMe, the device's stream on VRAM of either vendor; the
     *  variant is the kind tag, so no second word can disagree. */
    std::variant<std::monostate, ustore_nvme_options_t, cuda_executor_t, hip_executor_t> device;
};
