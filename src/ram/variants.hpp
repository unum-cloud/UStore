/**
 *  @file src/ram/variants.hpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief The tables of the in-memory engine, one variant per CPU class and isolation level, over the calls of every
 *      modality and the ingest.
 *
 *  Every call is a template on the CPU class and the isolation level, and @c ram_variant names the tables of one pair
 *  and its open. The tables are defined here, below every call they name, so the explicit instantiation in
 *  <tt>src/ram/variants_&lt;class&gt;.cpp</tt> compiles every call of the class there, and the @c extern one in
 *  <tt>src/ram/engine.cpp</tt> keeps them out of the unit that picks the class.
 */

#pragma once

#include "vram/uploads.hpp" // `uploading_vram_tables`

#include "blobs.hpp"   // `ram_variant::blobs_verbs_k`
#include "strings.hpp" // `ram_variant::strings_verbs_k`
#include "docs.hpp"    // `ram_variant::docs_verbs_k`
#include "graph.hpp"   // `ram_variant::graph_verbs_k`
#include "pages.hpp"   // `ram_variant::pages_verbs_k`
#include "ingest.hpp"  // `ingest_init`, `ingest_append`, `ingest_finish`, `ingest_free`

namespace unum::ustore::ram {

#pragma region Table

template <cpu_class_t cpu_class_, isolation_t isolation_>
database_verbs_t const ram_variant<cpu_class_, isolation_>::database_verbs_k {
    .describe = &serve_database<cpu_class_, isolation_, ustore_database_describe_t>,
    .clear = &serve_database<cpu_class_, isolation_, ustore_database_clear_t>,
    .compact = &serve_database<cpu_class_, isolation_, ustore_database_compact_t>,
    .flush = &serve_database<cpu_class_, isolation_, ustore_database_flush_t>,
    .snapshot_init = &serve_database<cpu_class_, isolation_, ustore_snapshot_init_t>,
    .snapshot_save = &serve_database<cpu_class_, isolation_, ustore_snapshot_save_t>,
    .collection_list = &serve_database<cpu_class_, isolation_, ustore_collection_list_t>,
    .collection_create = &serve_database<cpu_class_, isolation_, ustore_collection_create_t>,
    .collection_find = &serve_database<cpu_class_, isolation_, ustore_collection_find_t, strings_arg_t>,
    .collection_drop = &serve_database<cpu_class_, isolation_, ustore_collection_drop_t>,
    .ingest_init = &ingest_init<cpu_class_, isolation_>,
    .ingest_append = &ingest_append<cpu_class_, isolation_>,
    .ingest_finish = &ingest_finish<cpu_class_, isolation_>,
    .ingest_free = &ingest_free<cpu_class_, isolation_>,
    .transaction_init = &serve_database<cpu_class_, isolation_, ustore_transaction_init_t>,
    .transaction_stage = &serve_database<cpu_class_, isolation_, ustore_transaction_stage_t>,
    .transaction_commit = &serve_database<cpu_class_, isolation_, ustore_transaction_commit_t>,
    .transaction_rollback = &serve_database<cpu_class_, isolation_, ustore_transaction_rollback_t>,
    .transaction_free = &transaction_free<cpu_class_, isolation_>,
    .snapshot_free = &snapshot_free<cpu_class_, isolation_>,
    .database_free = &database_free<cpu_class_, isolation_>,
    .changes_cursor_init = &changes_cursor_init<cpu_class_, isolation_>,
    .changes_cursor_next = &changes_cursor_next<cpu_class_, isolation_>,
    .changes_cursor_free = &changes_cursor_free<cpu_class_, isolation_>,
    .pin_call = &pin_call<cpu_class_, isolation_>,
};

template <cpu_class_t cpu_class_, isolation_t isolation_>
engine_t const ram_variant<cpu_class_, isolation_>::engine_k {
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

} // namespace unum::ustore::ram
