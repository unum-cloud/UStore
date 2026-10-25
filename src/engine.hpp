/**
 *  @file src/engine.hpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief The table of calls every engine fills, which the router of `src/lib.hpp` routes each handle through.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::int32_t`, `std::uint8_t`, `std::uint32_t`

#include <new>  // `std::bad_alloc`
#include <span> // `std::span`

#include "ustore/blobs.h"   // `ustore_blobs_read_t`, `ustore_blobs_write_t` and the other binary calls
#include "ustore/db.h"      // `ustore_database_t`, `ustore_transaction_t`, `ustore_view_t` and the calls on them
#include "ustore/docs.h"    // `ustore_docs_write_t`, `ustore_docs_read_t` and the other document calls
#include "ustore/graph.h"   // `ustore_graph_read_edges_t`, `ustore_graph_write_edges_t` and the other graph calls
#include "ustore/pages.h"   // `ustore_pages_import_t`, `ustore_pages_read_t` and the other page calls
#include "ustore/strings.h" // `ustore_strings_write_t`, `ustore_strings_remove_t` and the other strings calls

#include "ustore/status.hpp" // `expected`, `status_t`

#include "ustore/utilities/ram_device.hpp" // `claim_t`, `residency_t`

#include "utilities/batches.hpp" // `strings_arg_t`

namespace unum::ustore {

using utilities::strings_arg_t;

struct call_executor_t;

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

    /** The executor whose thread runs the call, compared by address alone, or null for a call run alone. */
    call_executor_t const* executor = nullptr;

    /** Which thread of @c executor runs the call: a pool thread's index, or the pool's size for a blocking call and a
     *  batch the driver runs itself, so no other call of that executor runs on the same index meanwhile. */
    std::size_t thread_index = 0;
};

/**
 *  @brief One engine's implementation of every call of the C ABI but its own init.
 *
 *  Every call is `noexcept` and answers a status, which the router publishes into the caller's outcome: the in-house
 *  engines never throw, and an oracle wraps each call of its library in @c guarded. A null entry answers
 *  @c ustore_status_unsupported_k. Every ragged input arrives readable in any layout, in the order its argument
 *  structure names them, since the router gathers what an accessor writes.
 */
struct engine_t {
    status_t (*database_describe)(ustore_database_describe_t const&, view_t const&) noexcept;
    status_t (*database_clear)(ustore_database_clear_t const&, view_t const&) noexcept;
    status_t (*database_compact)(ustore_database_compact_t const&, view_t const&) noexcept;
    status_t (*snapshot_init)(ustore_snapshot_init_t const&, view_t const&) noexcept;
    status_t (*snapshot_save)(ustore_snapshot_save_t const&, view_t const&) noexcept;
    status_t (*collection_list)(ustore_collection_list_t const&, view_t const&) noexcept;
    status_t (*collection_create)(ustore_collection_create_t const&, view_t const&) noexcept;
    status_t (*collection_find)(ustore_collection_find_t const&, strings_arg_t const& names, view_t const&) noexcept;
    status_t (*collection_drop)(ustore_collection_drop_t const&, view_t const&) noexcept;
    status_t (*transaction_init)(ustore_transaction_init_t const&, view_t const&) noexcept;
    status_t (*transaction_stage)(ustore_transaction_stage_t const&, view_t const&) noexcept;
    status_t (*transaction_commit)(ustore_transaction_commit_t const&, view_t const&) noexcept;
    status_t (*transaction_rollback)(ustore_transaction_rollback_t const&, view_t const&) noexcept;
    status_t (*blobs_write)(ustore_blobs_write_t const&, strings_arg_t const& values, view_t const&) noexcept;
    status_t (*blobs_remove)(ustore_blobs_remove_t const&, view_t const&) noexcept;
    status_t (*blobs_read)(ustore_blobs_read_t const&, view_t const&) noexcept;
    status_t (*blobs_scan)(ustore_blobs_scan_t const&, view_t const&) noexcept;
    status_t (*blobs_sample)(ustore_blobs_sample_t const&, view_t const&) noexcept;
    status_t (*blobs_measure)(ustore_blobs_measure_t const&, view_t const&) noexcept;
    status_t (*docs_write)(ustore_docs_write_t const&, strings_arg_t const& fields, strings_arg_t const& values,
                           view_t const&) noexcept;
    status_t (*docs_read)(ustore_docs_read_t const&, strings_arg_t const& fields, view_t const&) noexcept;
    status_t (*docs_gist)(ustore_docs_gist_t const&, view_t const&) noexcept;
    status_t (*docs_gather)(ustore_docs_gather_t const&, strings_arg_t const& fields, view_t const&) noexcept;
    status_t (*graph_read_edges)(ustore_graph_read_edges_t const&, view_t const&) noexcept;
    status_t (*graph_write_edges)(ustore_graph_write_edges_t const&, view_t const&) noexcept;
    status_t (*graph_remove_edges)(ustore_graph_remove_edges_t const&, view_t const&) noexcept;
    status_t (*graph_write_vertices)(ustore_graph_write_vertices_t const&, view_t const&) noexcept;
    status_t (*graph_remove_vertices)(ustore_graph_remove_vertices_t const&, view_t const&) noexcept;
    status_t (*pages_import)(ustore_pages_import_t const&, strings_arg_t const& values, view_t const&) noexcept;
    status_t (*pages_insert)(ustore_pages_insert_t const&, strings_arg_t const& values, view_t const&) noexcept;
    status_t (*pages_remove)(ustore_pages_remove_t const&, view_t const&) noexcept;
    status_t (*pages_move)(ustore_pages_move_t const&, view_t const&) noexcept;
    status_t (*pages_splice)(ustore_pages_splice_t const&, strings_arg_t const& values, view_t const&) noexcept;
    status_t (*pages_set_attributes)(ustore_pages_set_attributes_t const&, strings_arg_t const& fields,
                                     strings_arg_t const& values, view_t const&) noexcept;
    status_t (*pages_remove_attributes)(ustore_pages_remove_attributes_t const&, strings_arg_t const& fields,
                                        view_t const&) noexcept;
    status_t (*pages_read)(ustore_pages_read_t const&, view_t const&) noexcept;
    status_t (*strings_write)(ustore_strings_write_t const&, strings_arg_t const& strings, view_t const&) noexcept;
    status_t (*strings_remove)(ustore_strings_remove_t const&, strings_arg_t const& strings, view_t const&) noexcept;
    status_t (*strings_read)(ustore_strings_read_t const&, strings_arg_t const& strings, view_t const&) noexcept;
    status_t (*strings_scan)(ustore_strings_scan_t const&, strings_arg_t const& prefixes,
                             strings_arg_t const& start_strings, view_t const&) noexcept;
    void (*transaction_free)(ustore_transaction_t) noexcept;
    void (*snapshot_free)(ustore_snapshot_t) noexcept;
    void (*database_free)(ustore_database_t) noexcept;
};

/** Whether a database takes changes, or the router refuses every call that would change it. */
enum class access_t : std::uint8_t {

    /** Every call serves, as every open but a view leaves it. */
    read_write_k,

    /** Writes, removes, transactions, collection creates and drops, clears and compactions answer
     *  @c ustore_status_read_only_k, as a view opens it. */
    read_only_k,
};

/** Leads every database, transaction and snapshot handle an engine hands out, naming the engine that serves it and
 *  the claim a call on a transaction holds, so a second call on that transaction finds it contended. */
struct handle_t {
    engine_t const* engine = nullptr;
    claim_t claim = claim_t::free_k;

    /** Deferred calls naming this handle that have not finished, through `std::atomic_ref`. */
    std::uint32_t deferred_calls = 0;

    /** The caller's hold plus one per transaction, snapshot or cursor keeping the handle open, through
     *  `std::atomic_ref`. */
    std::uint32_t holds = 1;

    /** On a transaction or a snapshot, the database its hold keeps open. */
    ustore_database_t database = nullptr;

    /** Where the handle's payload lives, which the executor of every call naming it must compute at. */
    residency_t residency = residency_t::host_k;

    /** On a database, whether calls may change it. */
    access_t access = access_t::read_write_k;
};

/** The engine's own handle behind @p handle, which the engine handed out as the @c handle_t it derives from. */
template <typename handle_type_>
handle_type_* handle_cast(void* handle) noexcept {
    return static_cast<handle_type_*>(static_cast<handle_t*>(handle));
}

/** The engine behind @p handle, as the engine stamped it. */
inline engine_t const& engine_of(void* handle) noexcept { return *handle_cast<handle_t>(handle)->engine; }

/** The database a call addresses: the one it names, or the one its snapshot holds open. */
template <typename arguments_type_>
ustore_database_t database_of(arguments_type_ const& arguments) noexcept {
    if constexpr (requires { arguments.database; }) return arguments.database;
    else return arguments.snapshot ? handle_cast<handle_t>(arguments.snapshot)->database : nullptr;
}

/**
 *  @brief Routes the init @p arguments of an engine as `src/lib.hpp` routes every call: on the calling thread,
 *      blocking on their executor, or submitted to it, served by the engine's @p open, since no handle names the
 *      engine yet.
 *
 *  Defined for the inits, opens and views of the in-memory, RocksDB, LevelDB and NVMe engines, each of which hands in
 *  its own @p open, so a program still links only the engines it opens.
 */
template <typename arguments_type_>
void route_init(arguments_type_* arguments, status_t (*open)(arguments_type_ const&, view_t const&) noexcept) noexcept;

/** The document calls of an engine keeping every document as one value of JSON text, which `src/docs.cpp` serves
 *  through the engine's own reads and writes. */
status_t docs_write_in_json_values(ustore_docs_write_t const&, strings_arg_t const& fields, strings_arg_t const& values,
                                   view_t const&) noexcept;
status_t docs_read_in_json_values(ustore_docs_read_t const&, strings_arg_t const& fields, view_t const&) noexcept;
status_t docs_gist_in_json_values(ustore_docs_gist_t const&, view_t const&) noexcept;
status_t docs_gather_in_json_values(ustore_docs_gather_t const&, strings_arg_t const& fields, view_t const&) noexcept;

/** The graph calls of an engine keeping every vertex as one value of its degrees and sorted neighborships, which
 *  `src/graph.cpp` serves through the engine's own reads and writes. */
status_t graph_read_edges_in_vertex_values(ustore_graph_read_edges_t const&, view_t const&) noexcept;
status_t graph_write_edges_in_vertex_values(ustore_graph_write_edges_t const&, view_t const&) noexcept;
status_t graph_remove_edges_in_vertex_values(ustore_graph_remove_edges_t const&, view_t const&) noexcept;
status_t graph_write_vertices_in_vertex_values(ustore_graph_write_vertices_t const&, view_t const&) noexcept;
status_t graph_remove_vertices_in_vertex_values(ustore_graph_remove_vertices_t const&, view_t const&) noexcept;

/** The page calls of an engine keeping every page as one value, its dense element tree, which `src/pages.cpp` serves
 *  through the engine's own reads, writes and removes. */
status_t pages_import_in_page_values(ustore_pages_import_t const&, strings_arg_t const& values, view_t const&) noexcept;
status_t pages_insert_in_page_values(ustore_pages_insert_t const&, strings_arg_t const& values, view_t const&) noexcept;
status_t pages_remove_in_page_values(ustore_pages_remove_t const&, view_t const&) noexcept;
status_t pages_move_in_page_values(ustore_pages_move_t const&, view_t const&) noexcept;
status_t pages_splice_in_page_values(ustore_pages_splice_t const&, strings_arg_t const& values, view_t const&) noexcept;
status_t pages_set_attributes_in_page_values(ustore_pages_set_attributes_t const&, strings_arg_t const& fields,
                                             strings_arg_t const& values, view_t const&) noexcept;
status_t pages_remove_attributes_in_page_values(ustore_pages_remove_attributes_t const&, strings_arg_t const& fields,
                                                view_t const&) noexcept;
status_t pages_read_in_page_values(ustore_pages_read_t const&, view_t const&) noexcept;

/** The edits one page call made to one page, in task order, as a chain of `ustore/pages/page_edits.hpp`. */
struct page_edits_t {
    ustore_collection_id_t collection;
    ustore_key_t key;
    std::span<utilities::byte_t const> chain;
};

/** Commits the edit chains of one page call at once, as an engine keeping a page as its edits does. */
using page_edits_commit_t = status_t (*)(ustore_database_t, std::span<page_edits_t const>, ustore_durability_t,
                                         view_t const&) noexcept;

/** The page edit calls of an engine keeping a page as its edits, which `src/pages.cpp` serves through the engine's
 *  own reads, handing the edits of every call to @c commit; reads stay `pages_read_in_page_values`, as the engine's
 *  reads fold the edits into the tree. */
status_t pages_import_in_page_edits(ustore_pages_import_t const&, strings_arg_t const& values, view_t const&,
                                    page_edits_commit_t commit) noexcept;
status_t pages_insert_in_page_edits(ustore_pages_insert_t const&, strings_arg_t const& values, view_t const&,
                                    page_edits_commit_t commit) noexcept;
status_t pages_remove_in_page_edits(ustore_pages_remove_t const&, view_t const&, page_edits_commit_t commit) noexcept;
status_t pages_move_in_page_edits(ustore_pages_move_t const&, view_t const&, page_edits_commit_t commit) noexcept;
status_t pages_splice_in_page_edits(ustore_pages_splice_t const&, strings_arg_t const& values, view_t const&,
                                    page_edits_commit_t commit) noexcept;
status_t pages_set_attributes_in_page_edits(ustore_pages_set_attributes_t const&, strings_arg_t const& fields,
                                            strings_arg_t const& values, view_t const&,
                                            page_edits_commit_t commit) noexcept;
status_t pages_remove_attributes_in_page_edits(ustore_pages_remove_attributes_t const&, strings_arg_t const& fields,
                                               view_t const&, page_edits_commit_t commit) noexcept;

/** Mints a UUIDv7 for a new collection, refusing when the system gives no entropy. */
expected<ustore_collection_id_t> mint_collection_id() noexcept;

/** What an executor tells the NVMe databases it opens: how they move bytes and where they compact. */
struct nvme_placement_t {

    /** How the databases move bytes, zeroed for the engine's defaults. */
    ustore_nvme_options_t nvme {};

    /** Threads the executor runs, each keeping an I/O channel of its own, or zero for the engine's default. */
    std::size_t threads = 0;

    /** The executor whose threads, its blocking caller's included, keep a channel and an epoch slot of their own in
     *  the databases it opens, or null for an executor of no NVMe placement. */
    call_executor_t const* executor = nullptr;

    /** CUDA ordinals compactions run on, viewing the VRAM executor the NVMe executor borrows, or empty. */
    std::span<std::int32_t const> devices {};

    /** Bytes of device memory compactions may take on each of @c devices. */
    std::size_t scratch_limit = 0;
};

/** The placement @p executor carries when @c ustore_executor_for_nvme made it, and the defaults for any other and for
 *  none. */
nvme_placement_t nvme_placement_of(ustore_executor_t executor) noexcept;

/** The C ABI's opaque spelling of @p handle, which @c handle_cast turns back into the engine's own handle. */
template <typename c_handle_type_>
c_handle_type_ c_handle(handle_t* handle) noexcept {
    return reinterpret_cast<c_handle_type_>(handle);
}

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

    /** A 16-byte UUID onto bytes: blobs, documents, pages, graph vertices and the main collection. */
    uuids_to_bytes_k,

    /** A string onto a 16-byte UUID: the strings modality, and the names system collection. */
    strings_to_uuids_k,
};

/** The layout of the entries of a collection holding @p modality. */
constexpr entry_layout_t entry_layout_of(ustore_modality_t modality) noexcept {
    return modality == ustore_modality_strings_k ? entry_layout_t::strings_to_uuids_k
                                                 : entry_layout_t::uuids_to_bytes_k;
}

} // namespace unum::ustore
