/**
 *  @file src/ram/engine.cpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief Embedded in-memory engine on SmashTable, strictly serializable or snapshot-isolated as configured at open.
 *
 *  Every collection keeps a partitioned multi-version store of its own, of the kind its modality picks: key UUIDs
 *  mapped onto values for blobs, documents and pages, strings mapped onto 16-byte keys for strings, and the
 *  half-edges of a graph. No stored key carries its collection. The `_collections` system collection maps every
 *  collection's UUID onto its modality, its name and its store, and the `_names` system collection, a store of the
 *  strings kind, maps every name onto its collection's UUID. Every store is built into one commit order, so a
 *  transaction spanning any number of them publishes under one stamp and a snapshot reads all of them at it.
 *  Dropping a collection retires its store at the drop's stamp, and the store is freed once no reader holds an older
 *  one. A database with a directory persists as one binary image, rewritten whole on a flush and on close.
 */

#include <atomic>      // `std::memory_order`
#include <new>         // `std::nothrow`
#include <string_view> // `std::string_view`

#include "ustore/db.h"

#include "engine.hpp" // `engine_t`, `handle_cast`, `c_handle`, `guarded_by`, `docs_read_over_blobs`
#include "lib.hpp"    // `route_init`

#include "database.hpp" // `database_state`, `transaction_handle`, `snapshot_handle`, `resolve_modality`, `persist`
#include "blobs.hpp"    // the `serve` overloads of the blobs calls
#include "strings.hpp"  // the `serve` overloads of the strings calls
#include "graph.hpp"    // the `serve` overloads of the graph calls

namespace unum::ustore::ram {

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
    ram_atomic_ref<std::size_t>(state.open_snapshots).fetch_sub(1, std::memory_order_release);
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

/** The database verbs of this engine at @p isolation_. */
template <isolation_t isolation_>
database_verbs_t const database_verbs_k {
    .describe = &serve_database<isolation_, ustore_database_describe_t>,
    .clear = &serve_database<isolation_, ustore_database_clear_t>,
    .compact = &serve_database<isolation_, ustore_database_compact_t>,
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
    .transaction_free = &transaction_free<isolation_>,
    .snapshot_free = &snapshot_free<isolation_>,
    .database_free = &database_free<isolation_>,
};

/** The unchecked whole-value verbs at @p isolation_ of every collection keyed by UUIDs, which the layers over blobs
 *  reach. */
template <isolation_t isolation_>
constexpr blobs_verbs_t values_verbs_k {
    .write = &serve_database<isolation_, ustore_blobs_write_t, strings_arg_t>,
    .remove = &serve_database<isolation_, ustore_blobs_remove_t>,
    .read = &serve_database<isolation_, ustore_blobs_read_t>,
    .scan = &serve_database<isolation_, ustore_blobs_scan_t>,
    .sample = &serve_database<isolation_, ustore_blobs_sample_t>,
    .measure = &serve_database<isolation_, ustore_blobs_measure_t>,
};

/** The verbs of this engine's blobs collections at @p isolation_. */
template <isolation_t isolation_>
blobs_verbs_t const blobs_verbs_k {
    .write = &guarded_by<ustore_modality_blobs_k, &resolve_modality<isolation_>, values_verbs_k<isolation_>.write>,
    .remove = &guarded_by<ustore_modality_blobs_k, &resolve_modality<isolation_>, values_verbs_k<isolation_>.remove>,
    .read = &guarded_by<ustore_modality_blobs_k, &resolve_modality<isolation_>, values_verbs_k<isolation_>.read>,
    .scan = &guarded_by<ustore_modality_blobs_k, &resolve_modality<isolation_>, values_verbs_k<isolation_>.scan>,
    .sample = &guarded_by<ustore_modality_blobs_k, &resolve_modality<isolation_>, values_verbs_k<isolation_>.sample>,
    .measure = &guarded_by<ustore_modality_blobs_k, &resolve_modality<isolation_>, values_verbs_k<isolation_>.measure>,
};

/** The verbs of this engine's strings collections at @p isolation_. */
template <isolation_t isolation_>
strings_verbs_t const strings_verbs_k {
    .write = &serve_database<isolation_, ustore_strings_write_t, strings_arg_t>,
    .remove = &serve_database<isolation_, ustore_strings_remove_t, strings_arg_t>,
    .read = &serve_database<isolation_, ustore_strings_read_t, strings_arg_t>,
    .scan = &serve_database<isolation_, ustore_strings_scan_t, strings_arg_t, strings_arg_t>,
};

/** The verbs of this engine's documents collections at @p isolation_, over its values until it keeps them natively. */
template <isolation_t isolation_>
docs_verbs_t const docs_verbs_k {
    .write = &docs_write_over_blobs<values_verbs_k<isolation_>, &resolve_modality<isolation_>>,
    .read = &docs_read_over_blobs<values_verbs_k<isolation_>, &resolve_modality<isolation_>>,
    .gist = &docs_gist_over_blobs<values_verbs_k<isolation_>, &resolve_modality<isolation_>>,
    .gather = &docs_gather_over_blobs<values_verbs_k<isolation_>, &resolve_modality<isolation_>>,
    .remove = &remove_over_blobs<values_verbs_k<isolation_>, &resolve_modality<isolation_>, ustore_modality_docs_k>,
    .scan = &scan_over_blobs<values_verbs_k<isolation_>, &resolve_modality<isolation_>, ustore_modality_docs_k>,
    .sample = &sample_over_blobs<values_verbs_k<isolation_>, &resolve_modality<isolation_>, ustore_modality_docs_k>,
};

/** The verbs of this engine's graph collections at @p isolation_. */
template <isolation_t isolation_>
graph_verbs_t const graph_verbs_k {
    .read_edges = &serve_database<isolation_, ustore_graph_read_edges_t>,
    .write_edges = &serve_database<isolation_, ustore_graph_write_edges_t>,
    .remove_edges = &serve_database<isolation_, ustore_graph_remove_edges_t>,
    .write_vertices = &serve_database<isolation_, ustore_graph_write_vertices_t>,
    .remove_vertices = &serve_database<isolation_, ustore_graph_remove_vertices_t>,
    .scan_vertices =
        &scan_over_blobs<values_verbs_k<isolation_>, &resolve_modality<isolation_>, ustore_modality_graph_k>,
};

/** The verbs of this engine's pages collections at @p isolation_, over its values until it keeps them natively. */
template <isolation_t isolation_>
pages_verbs_t const pages_verbs_k {
    .import = &pages_import_over_blobs<values_verbs_k<isolation_>, &resolve_modality<isolation_>>,
    .insert = &pages_insert_over_blobs<values_verbs_k<isolation_>, &resolve_modality<isolation_>>,
    .remove = &pages_remove_over_blobs<values_verbs_k<isolation_>, &resolve_modality<isolation_>>,
    .move = &pages_move_over_blobs<values_verbs_k<isolation_>, &resolve_modality<isolation_>>,
    .splice = &pages_splice_over_blobs<values_verbs_k<isolation_>, &resolve_modality<isolation_>>,
    .set_attributes = &pages_set_attributes_over_blobs<values_verbs_k<isolation_>, &resolve_modality<isolation_>>,
    .remove_attributes = &pages_remove_attributes_over_blobs<values_verbs_k<isolation_>, &resolve_modality<isolation_>>,
    .read = &pages_read_over_blobs<values_verbs_k<isolation_>, &resolve_modality<isolation_>>,
    .scan = &scan_over_blobs<values_verbs_k<isolation_>, &resolve_modality<isolation_>, ustore_modality_pages_k>,
};

template <isolation_t isolation_>
engine_t const engine_k {
    .database = &database_verbs_k<isolation_>,
    .blobs = &blobs_verbs_k<isolation_>,
    .strings = &strings_verbs_k<isolation_>,
    .docs = &docs_verbs_k<isolation_>,
    .graph = &graph_verbs_k<isolation_>,
    .pages = &pages_verbs_k<isolation_>,
};

#pragma endregion Dispatch

} // namespace unum::ustore::ram

using namespace unum::ustore::ram;

#pragma region C ABI

void ustore_database_init_ram(ustore_database_init_ram_t* arguments) { route_init(arguments, &init); }

void ustore_database_open_ram(ustore_database_open_ram_t* arguments) { route_init(arguments, &open); }
