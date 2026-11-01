/**
 *  @file src/nvme/variants.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Embedded on-disk engine over the LSM templates of <tt>include/ustore/blobs/</tt>, one NVMe collection per
 *      collection of the C ABI, served by one variant per CPU class.
 *
 *  Every call is a template on the CPU class, whose atomic reference and waiter instantiate the
 *  store, and @c nvme_variant names the table of one class and its open. Its members are defined
 *  below it, so the explicit instantiation in <tt>src/nvme/variants_&lt;class&gt;.cpp</tt> compiles every call
 *  of the class there, and the @c extern one in <tt>src/nvme/engine.cpp</tt> keeps them out of the unit
 *  that picks the class.
 *
 *  One mount opens the volumes and one database of 16-byte keys lives on them. Every
 *  collection of the C ABI is one collection of that database under the same UUID, and
 *  two more are the engine's own system collections. The collections system
 *  collection, @c collections_collection_k, maps every other collection's UUID onto
 *  its name, while the store schema records its modality, which a mount needs before
 *  it opens the collection. The names system collection, @c names_collection_k, maps
 *  every name onto the bare UUID of its collection. A strings collection, and the
 *  names, live in the strings store of <tt>include/ustore/strings/nvme_store.hpp</tt>, keyed
 *  by whole strings, each mapped onto a 16-byte key held in its cell, and a
 *  transaction's strings ride in the share of it every transaction here joins.
 *
 *  Every write of a caller goes through a transaction of the database, the caller's or one the call commits alone, so
 *  the registry records every commit a watched read may conflict with. The two system collections, which no caller's
 *  transaction reaches, change under a claim of their own, the collections straight in the store and the names through
 *  a commit watching nothing, so a collection created or dropped never conflicts with the transactions running beside
 *  it. A call outside a transaction reads at the head, or at its snapshot's stamp. Reads resolve collections at the
 *  head, as every engine does, but through a snapshot, which resolves them as they stood at its stamp. A walk merges
 *  the runs of one collection with its memory runs and, inside a transaction, with the transaction's own writes.
 *
 *  Isolation, as delivered. Every read of a transaction answers at the stamp it started at, a walk included, so it
 *  sees one committed state whatever it watches. Watching keys, the default, a commit checks every key it read or
 *  wrote, and every range it walked, the keys it did not find included, against every commit since its start, all
 *  under the ledger's leader word, and lands under a stamp drawn after that check: committed transactions are strictly
 *  serializable in the order of their stamps. Watching nothing, a transaction still reads one snapshot, but its writes
 *  land unchecked and the last writer wins. @c describe reports snapshot isolation, as the C ABI does for every engine
 *  but the in-memory one.
 *
 *  A staged transaction holds the ledger's leader word, which every other commit waits for, so a stage only checks
 *  the transaction and lets the word go, and the commit checks it again. A snapshot is a stamp pinned in the store's
 *  ledger and holds no level set: every read through it answers the newest version at or below that stamp, which the
 *  memory runs and the merges keep for as long as the pin lives, so a snapshot kept across calls stalls no writer.
 */

#pragma once

#include "docs_layer.hpp"   // `docs_read_over_blobs`, `docs_gist_over_blobs`, `docs_gather_over_blobs`
#include "pages_layer.hpp"  // `pages_read_over_blobs`
#include "vram/uploads.hpp" // `uploading_vram_tables`

#include "database.hpp" // `nvme_variant`, `database_describe`, `resolve_modality`, `transaction_free`
#include "blobs.hpp"    // `blobs_write`, `remove_values`, `read_values`, `scan_values`, `sample_values`
#include "strings.hpp"  // `strings_write`, `strings_remove`, `strings_read`, `strings_scan`
#include "docs.hpp"     // `docs_write`
#include "graph.hpp"    // `graph_read_edges`, `graph_write_edges`, `graph_remove_vertices`
#include "ingest.hpp"   // `ingest_init`, `ingest_append`, `ingest_finish`, `ingest_free`
#include "pages.hpp"    // `pages_import`, `pages_insert`

namespace unum::ustore::nvme {

#pragma region Table

template <cpu_class_t cpu_class_>
database_verbs_t const nvme_variant<cpu_class_>::database_verbs_k {
    .describe = &database_describe<cpu_class_>,
    .clear = &database_clear<cpu_class_>,
    .compact = &database_compact<cpu_class_>,
    .snapshot_init = &snapshot_init<cpu_class_>,
    .snapshot_save = &snapshot_save<cpu_class_>,
    .collection_list = &collection_list<cpu_class_>,
    .collection_create = &collection_create<cpu_class_>,
    .collection_find = &collection_find<cpu_class_>,
    .collection_drop = &collection_drop<cpu_class_>,
    .ingest_init = &ingest_init<cpu_class_>,
    .ingest_append = &ingest_append<cpu_class_>,
    .ingest_finish = &ingest_finish<cpu_class_>,
    .ingest_free = &ingest_free<cpu_class_>,
    .transaction_init = &transaction_init<cpu_class_>,
    .transaction_stage = &transaction_stage<cpu_class_>,
    .transaction_commit = &transaction_commit<cpu_class_>,
    .transaction_rollback = &transaction_rollback<cpu_class_>,
    .transaction_free = &transaction_free<cpu_class_>,
    .snapshot_free = &snapshot_free<cpu_class_>,
    .database_free = &database_free<cpu_class_>,
};

template <cpu_class_t cpu_class_>
blobs_verbs<home_t::host_k> const nvme_variant<cpu_class_>::values_verbs_k {
    .remove = &remove_values<cpu_class_>,
    .read = &read_values<cpu_class_>,
    .scan = &scan_values<cpu_class_>,
    .sample = &sample_values<cpu_class_>,
};

template <cpu_class_t cpu_class_>
blobs_verbs<home_t::host_k> const nvme_variant<cpu_class_>::blobs_verbs_k {
    .write = &blobs_write<cpu_class_>,
    .remove = &blobs_remove<cpu_class_>,
    .read = &blobs_read<cpu_class_>,
    .scan = &blobs_scan<cpu_class_>,
    .sample = &blobs_sample<cpu_class_>,
    .measure = &blobs_measure<cpu_class_>,
};

template <cpu_class_t cpu_class_>
strings_verbs<home_t::host_k> const nvme_variant<cpu_class_>::strings_verbs_k {
    .write = &strings_write<cpu_class_>,
    .remove = &strings_remove<cpu_class_>,
    .read = &strings_read<cpu_class_>,
    .scan = &strings_scan<cpu_class_>,
};

template <cpu_class_t cpu_class_>
docs_verbs<home_t::host_k> const nvme_variant<cpu_class_>::docs_verbs_k {
    .write = &docs_write<cpu_class_>,
    .read = &docs_read_over_blobs<strings_kit_t, values_verbs_k, &resolve_modality<cpu_class_>>,
    .gist = &docs_gist_over_blobs<values_verbs_k, &resolve_modality<cpu_class_>>,
    .gather = &docs_gather_over_blobs<strings_kit_t, values_verbs_k, &resolve_modality<cpu_class_>>,
    .remove = &remove_over_blobs<values_verbs_k, &resolve_modality<cpu_class_>, ustore_modality_docs_k>,
    .scan = &scan_over_blobs<values_verbs_k, &resolve_modality<cpu_class_>, ustore_modality_docs_k>,
    .sample = &sample_over_blobs<values_verbs_k, &resolve_modality<cpu_class_>, ustore_modality_docs_k>,
};

template <cpu_class_t cpu_class_>
graph_verbs<home_t::host_k> const nvme_variant<cpu_class_>::graph_verbs_k {
    .read_edges = &graph_read_edges<cpu_class_>,
    .write_edges = &graph_write_edges<cpu_class_>,
    .remove_edges = &graph_remove_edges<cpu_class_>,
    .write_vertices = &graph_write_vertices<cpu_class_>,
    .remove_vertices = &graph_remove_vertices<cpu_class_>,
    .scan_vertices = &scan_over_blobs<values_verbs_k, &resolve_modality<cpu_class_>, ustore_modality_graph_k>,
};

template <cpu_class_t cpu_class_>
pages_verbs<home_t::host_k> const nvme_variant<cpu_class_>::pages_verbs_k {
    .import = &pages_import<cpu_class_>,
    .insert = &pages_insert<cpu_class_>,
    .remove = &pages_remove<cpu_class_>,
    .move = &pages_move<cpu_class_>,
    .splice = &pages_splice<cpu_class_>,
    .set_attributes = &pages_set_attributes<cpu_class_>,
    .remove_attributes = &pages_remove_attributes<cpu_class_>,
    .read = &pages_read_over_blobs<strings_kit_t, values_verbs_k, &resolve_modality<cpu_class_>>,
    .scan = &scan_over_blobs<values_verbs_k, &resolve_modality<cpu_class_>, ustore_modality_pages_k>,
};

template <cpu_class_t cpu_class_>
engine_t const nvme_variant<cpu_class_>::engine_k {
    .database = &database_verbs_k,
    .host = {.blobs = &blobs_verbs_k,
             .strings = &strings_verbs_k,
             .docs = &docs_verbs_k,
             .graph = &graph_verbs_k,
             .pages = &pages_verbs_k},
    .vram_nvidia = uploading_vram_tables<home_t::vram_nvidia_k>,
    .vram_amd = uploading_vram_tables<home_t::vram_amd_k>,
};

#pragma endregion Table

} // namespace unum::ustore::nvme
