/**
 *  @file src_oracles/leveldb/engine.cpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief Embedded persistent key-value store on LevelDB, with collections as key ranges and without transactions.
 *
 *  LevelDB keeps one keyspace in bytewise order, so every stored key leads with the UUID of its collection and the
 *  pairs of each collection lie in one contiguous run. The collection's modality shapes the rest of the key and the
 *  value, and two system collections live under reserved UUIDs: `_collections`, recording the modality and the name
 *  of every collection once, and `_names`, mapping every name onto its collection as a strings collection would:
 *
 *  @code{.c}
 *  struct pairs {              // blobs, docs, graph and pages, valued by bytes
 *      uint8_t collection[16]; // offset 0
 *      uint8_t key[16];        // offset 16, 32 bytes in all
 *  };
 *  struct strings {            // strings collections, valued by the 16-byte key the string maps onto
 *      uint8_t collection[16]; // offset 0
 *      char string[];          // offset 16
 *  };
 *  struct collections {        // every collection, valued by a `struct collections_entry`
 *      uint8_t collection[16]; // offset 0, all bytes 0xFF
 *      uint8_t listed[16];     // offset 16
 *  };
 *  struct collections_entry {
 *      uint8_t modality;       // offset 0
 *      char name[];            // offset 1, empty for an anonymous collection
 *  };
 *  struct names {              // every named collection, valued by its 16-byte UUID
 *      uint8_t collection[16]; // offset 0, all bytes 0xFF but the last, 0xFE
 *      char name[];            // offset 16
 *  };
 *  @endcode
 *
 *  A block stores the bytes a key shares with its predecessor once, so the leading UUID costs little on disk.
 */

#include "blobs_layers/docs.hpp"  // `docs_write_over_blobs`, `docs_read_over_blobs`, `docs_gist_over_blobs`
#include "blobs_layers/graph.hpp" // `graph_read_edges_over_blobs`, `graph_write_edges_over_blobs`
#include "blobs_layers/pages.hpp" // `pages_import_over_blobs`, `pages_read_over_blobs`
#include "engine.hpp"             // `engine_t`, `guarded`, `route_init`, `remove_over_blobs`, `scan_over_blobs`

#include "database.hpp" // `database_open`, `database_describe`, `collection_create`, `snapshot_init`
#include "blobs.hpp"    // `blobs_write`, `blobs_remove`, `blobs_read`, `blobs_scan`, `blobs_sample`
#include "strings.hpp"  // `strings_write`, `strings_remove`, `strings_read`, `strings_scan`

namespace unum::ustore::oracles::leveldb {

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
    .snapshot_free = &snapshot_free,
    .database_free = &database_free,
};

/** The unchecked whole-value verbs of every collection keyed by UUIDs, which the layers over blobs reach. */
constexpr blobs_verbs_t values_verbs_k {
    .write = &guarded<&blobs_write>,
    .remove = &guarded<&blobs_remove>,
    .read = &guarded<&blobs_read>,
    .scan = &guarded<&blobs_scan>,
    .sample = &guarded<&blobs_sample>,
    .measure = &guarded<&blobs_measure>,
};

/** The verbs of this engine's blobs collections. */
blobs_verbs_t const blobs_verbs_k {
    .write = &guarded_by<ustore_modality_blobs_k, &resolve_modality, values_verbs_k.write>,
    .remove = &guarded_by<ustore_modality_blobs_k, &resolve_modality, values_verbs_k.remove>,
    .read = &guarded_by<ustore_modality_blobs_k, &resolve_modality, values_verbs_k.read>,
    .scan = &guarded_by<ustore_modality_blobs_k, &resolve_modality, values_verbs_k.scan>,
    .sample = &guarded_by<ustore_modality_blobs_k, &resolve_modality, values_verbs_k.sample>,
    .measure = &guarded_by<ustore_modality_blobs_k, &resolve_modality, values_verbs_k.measure>,
};

/** The verbs of this engine's strings collections. */
strings_verbs_t const strings_verbs_k {
    .write = &guarded<&strings_write>,
    .remove = &guarded<&strings_remove>,
    .read = &guarded<&strings_read>,
    .scan = &guarded<&strings_scan>,
};

/** The verbs of this engine's documents collections. */
docs_verbs_t const docs_verbs_k {
    .write = &docs_write_over_blobs<values_verbs_k, &resolve_modality>,
    .read = &docs_read_over_blobs<values_verbs_k, &resolve_modality>,
    .gist = &docs_gist_over_blobs<values_verbs_k, &resolve_modality>,
    .gather = &docs_gather_over_blobs<values_verbs_k, &resolve_modality>,
    .remove = &remove_over_blobs<values_verbs_k, &resolve_modality, ustore_modality_docs_k>,
    .scan = &scan_over_blobs<values_verbs_k, &resolve_modality, ustore_modality_docs_k>,
    .sample = &sample_over_blobs<values_verbs_k, &resolve_modality, ustore_modality_docs_k>,
};

/** The verbs of this engine's graph collections. */
graph_verbs_t const graph_verbs_k {
    .read_edges = &graph_read_edges_over_blobs<values_verbs_k, &resolve_modality>,
    .write_edges = &graph_write_edges_over_blobs<values_verbs_k, &resolve_modality>,
    .remove_edges = &graph_remove_edges_over_blobs<values_verbs_k, &resolve_modality>,
    .write_vertices = &graph_write_vertices_over_blobs<values_verbs_k, &resolve_modality>,
    .remove_vertices = &graph_remove_vertices_over_blobs<values_verbs_k, &resolve_modality>,
    .scan_vertices = &scan_over_blobs<values_verbs_k, &resolve_modality, ustore_modality_graph_k>,
};

/** The verbs of this engine's pages collections. */
pages_verbs_t const pages_verbs_k {
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
    .blobs = &blobs_verbs_k,
    .strings = &strings_verbs_k,
    .docs = &docs_verbs_k,
    .graph = &graph_verbs_k,
    .pages = &pages_verbs_k,
};

} // namespace unum::ustore::oracles::leveldb

using namespace unum::ustore;
using namespace unum::ustore::oracles::leveldb;

void ustore_database_open_leveldb(ustore_database_open_leveldb_t* arguments) {
    route_init(arguments, &guarded<&database_open, ustore_database_open_leveldb_t, view_t>);
}
