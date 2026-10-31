/**
 *  @file src_oracles/rocksdb/engine.cpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief Embedded persistent key-value store on RocksDB, with two-phase transactions and snapshots.
 *
 *  Every collection is a column family of its own, so no key carries its collection. Every column family keeps
 *  RocksDB's bytewise comparator, which orders 16-byte keys and strings alike, so an open needs no modality:
 *
 *      Collection                      Column family name     Key bytes              Value bytes
 *      any UUIDs-to-bytes collection   its UUID text          uuid key[16]           bytes
 *      a strings collection            its UUID text          char string[]          uuid value[16]
 *      collections system collection   _collections           uuid collection[16]    u8 modality, char name[]
 *      names system collection         _names                 char name[]            uuid collection[16]
 *
 *  RocksDB requires the `default` column family, which exists and stores nothing.
 *
 *  A create adds the column family, then puts both system entries in one batch, and a drop deletes both entries in
 *  one batch, then the column family. A column family no collections entry records is what a crash between the two
 *  left, which a read-write open drops and a read-only open refuses. Tables stay block-based, since the plain-table
 *  format lacks non-prefix seeks that scans need.
 *
 *  Every write reaches RocksDB's log, which stays in memory until a flushed write or commit, a full buffer or a clean
 *  close writes it out, so a crash recovers a consistent prefix of the commits across every collection. Transactions
 *  buffer their writes and watched reads and lock nothing until they stage, when RocksDB's pessimistic transaction
 *  locks and validates every key against the snapshot its first read or write took, and prepares it into the log.
 */

#include "blobs_layers/docs.hpp"  // `docs_write_over_blobs`, `docs_read_over_blobs`, `docs_gist_over_blobs`
#include "blobs_layers/graph.hpp" // `graph_read_edges_over_blobs`, `graph_write_edges_over_blobs`
#include "blobs_layers/pages.hpp" // `pages_import_over_blobs`, `pages_read_over_blobs`
#include "engine.hpp"             // `engine_t`, `guarded`, `remove_over_blobs`, `scan_over_blobs`
#include "lib.hpp"                // `route_init`
#include "vram/uploads.hpp"       // `uploading_vram_tables`

#include "database.hpp" // `database_open`, `database_describe`, `collection_create`, `transaction_init`
#include "blobs.hpp"    // `blobs_write`, `blobs_remove`, `blobs_read`, `blobs_scan`, `blobs_sample`
#include "strings.hpp"  // `strings_write`, `strings_remove`, `strings_read`, `strings_scan`

namespace unum::ustore::oracles::rocksdb {

/** The database verbs of this engine. */
database_verbs_t const database_verbs_k {
    .describe = &guarded<&database_describe>,
    .clear = &guarded<&database_clear>,
    .compact = &guarded<&database_compact>,
    .snapshot_init = &guarded<&snapshot_init>,
    .snapshot_save = &guarded<&snapshot_save>,
    .collection_list = &guarded<&collection_list>,
    .collection_create = &guarded<&collection_create>,
    .collection_find = &guarded<&collection_find>,
    .collection_drop = &guarded<&collection_drop>,
    .transaction_init = &guarded<&transaction_init>,
    .transaction_stage = &guarded<&transaction_stage>,
    .transaction_commit = &guarded<&transaction_commit>,
    .transaction_rollback = &guarded<&transaction_rollback>,
    .transaction_free = &transaction_free,
    .snapshot_free = &snapshot_free,
    .database_free = &database_free,
};

/** The unchecked whole-value verbs of every collection keyed by UUIDs, which the layers over blobs reach. */
constexpr blobs_verbs<home_t::host_k> values_verbs_k {
    .write = &guarded<&blobs_write>,
    .remove = &guarded<&blobs_remove>,
    .read = &guarded<&blobs_read>,
    .scan = &guarded<&blobs_scan>,
    .sample = &guarded<&blobs_sample>,
    .measure = &guarded<&blobs_measure>,
};

/** The verbs of this engine's blobs collections. */
blobs_verbs<home_t::host_k> const blobs_verbs_k {
    .write = &guarded_by<ustore_modality_blobs_k, &resolve_modality, values_verbs_k.write>,
    .remove = &guarded_by<ustore_modality_blobs_k, &resolve_modality, values_verbs_k.remove>,
    .read = &guarded_by<ustore_modality_blobs_k, &resolve_modality, values_verbs_k.read>,
    .scan = &guarded_by<ustore_modality_blobs_k, &resolve_modality, values_verbs_k.scan>,
    .sample = &guarded_by<ustore_modality_blobs_k, &resolve_modality, values_verbs_k.sample>,
    .measure = &guarded_by<ustore_modality_blobs_k, &resolve_modality, values_verbs_k.measure>,
};

/** The verbs of this engine's strings collections. */
strings_verbs<home_t::host_k> const strings_verbs_k {
    .write = &guarded<&strings_write>,
    .remove = &guarded<&strings_remove>,
    .read = &guarded<&strings_read>,
    .scan = &guarded<&strings_scan>,
};

/** The verbs of this engine's documents collections. */
docs_verbs<home_t::host_k> const docs_verbs_k {
    .write = &docs_write_over_blobs<values_verbs_k, &resolve_modality>,
    .read = &docs_read_over_blobs<values_verbs_k, &resolve_modality>,
    .gist = &docs_gist_over_blobs<values_verbs_k, &resolve_modality>,
    .gather = &docs_gather_over_blobs<values_verbs_k, &resolve_modality>,
    .remove = &remove_over_blobs<values_verbs_k, &resolve_modality, ustore_modality_docs_k>,
    .scan = &scan_over_blobs<values_verbs_k, &resolve_modality, ustore_modality_docs_k>,
    .sample = &sample_over_blobs<values_verbs_k, &resolve_modality, ustore_modality_docs_k>,
};

/** The verbs of this engine's graph collections. */
graph_verbs<home_t::host_k> const graph_verbs_k {
    .read_edges = &graph_read_edges_over_blobs<values_verbs_k, &resolve_modality>,
    .write_edges = &graph_write_edges_over_blobs<values_verbs_k, &resolve_modality>,
    .remove_edges = &graph_remove_edges_over_blobs<values_verbs_k, &resolve_modality>,
    .write_vertices = &graph_write_vertices_over_blobs<values_verbs_k, &resolve_modality>,
    .remove_vertices = &graph_remove_vertices_over_blobs<values_verbs_k, &resolve_modality>,
    .scan_vertices = &scan_over_blobs<values_verbs_k, &resolve_modality, ustore_modality_graph_k>,
};

/** The verbs of this engine's pages collections. */
pages_verbs<home_t::host_k> const pages_verbs_k {
    .import = &pages_import_over_blobs<values_verbs_k, &resolve_modality>,
    .insert = &pages_insert_over_blobs<values_verbs_k, &resolve_modality>,
    .remove = &pages_remove_over_blobs<values_verbs_k, &resolve_modality>,
    .move = &pages_move_over_blobs<values_verbs_k, &resolve_modality>,
    .splice = &pages_splice_over_blobs<values_verbs_k, &resolve_modality>,
    .set_attributes = &pages_set_attributes_over_blobs<values_verbs_k, &resolve_modality>,
    .remove_attributes = &pages_remove_attributes_over_blobs<values_verbs_k, &resolve_modality>,
    .read = &pages_read_over_blobs<values_verbs_k, &resolve_modality>,
    .scan = &scan_over_blobs<values_verbs_k, &resolve_modality, ustore_modality_pages_k>,
};

engine_t const engine_k {
    .database = &database_verbs_k,
    .host = {.blobs = &blobs_verbs_k,
             .strings = &strings_verbs_k,
             .docs = &docs_verbs_k,
             .graph = &graph_verbs_k,
             .pages = &pages_verbs_k},
    .vram_nvidia = uploading_vram_tables<home_t::vram_nvidia_k>,
    .vram_amd = uploading_vram_tables<home_t::vram_amd_k>,
};

} // namespace unum::ustore::oracles::rocksdb

using namespace unum::ustore;
using namespace unum::ustore::oracles::rocksdb;

#if defined(__linux__)

/** Lets RocksDB's POSIX file system open io_uring rings, which it leaves off unless this weak symbol is defined. A
 *  thread whose ring the kernel or a seccomp profile refuses reads through `pread` instead. */
extern "C" bool RocksDbIOUringEnable() { return true; }
#endif

void ustore_database_open_rocksdb(ustore_database_open_rocksdb_t* arguments) {
    route_init<&guarded<&database_open, ustore_database_open_rocksdb_t, cpu_ceiling_t, stringzilla_kernels_t, view_t>>(
        arguments);
}

void ustore_database_view_rocksdb(ustore_database_view_rocksdb_t* arguments) {
    route_init<&guarded<&database_view, ustore_database_view_rocksdb_t, cpu_ceiling_t, stringzilla_kernels_t, view_t>>(
        arguments);
}
