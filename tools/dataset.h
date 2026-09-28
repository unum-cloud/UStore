/**
 *  @file tools/dataset.h
 *  @author Ash Vardanian
 *  @date April 16, 2023
 *  @brief Imports and exports documents and graph edges as NDJSON or CSV files.
 */

#pragma once

#include <ustore/db.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Imports every row of one NDJSON or CSV file as a document, keyed by its @c id_field. */
typedef struct ustore_docs_import_t {

    /** Already open database instance. */
    ustore_database_t db;

    /** Receives the error message of a failed import. */
    ustore_error_t* error;

    /** Reusable memory handle, or null for a private one. */
    ustore_arena_t* arena;

    /** Options forwarded to the document writes. */
    ustore_options_t options;

    /** Collection receiving the documents. */
    ustore_collection_id_t collection;

    /** The file to import, whose @c .ndjson or @c .csv extension selects the format. */
    ustore_str_view_t path;

    /** Bytes of documents buffered before one batched write. */
    ustore_size_t max_batch_size;

    /** Number of @c fields to keep, or zero to keep whole rows. */
    ustore_size_t fields_count;

    /** Field names or JSON pointers to keep. */
    ustore_str_view_t const* fields;

    /** Step between @c fields. */
    ustore_size_t fields_stride;

    /** Field holding each document's key, which @c fields must include. */
    ustore_str_view_t id_field;
} ustore_docs_import_t;

void ustore_docs_import(ustore_docs_import_t*);

/** Exports every document of a collection as @c {"_id":key,"doc":document} NDJSON lines or @c _id,doc CSV rows. */
typedef struct ustore_docs_export_t {

    /** Already open database instance. */
    ustore_database_t db;

    /** Receives the error message of a failed export. */
    ustore_error_t* error;

    /** Reusable memory handle, or null for a private one. */
    ustore_arena_t* arena;

    /** Options forwarded to the scans and reads. */
    ustore_options_t options;

    /** Collection to export. */
    ustore_collection_id_t collection;

    /** The file to create, whose @c .ndjson or @c .csv extension selects the format. */
    ustore_str_view_t path;

    /** Bytes of output buffered before one write to the file. */
    ustore_size_t max_batch_size;

    /** Number of @c fields to keep, or zero to keep whole documents. */
    ustore_size_t fields_count;

    /** Field names or JSON pointers to keep. */
    ustore_str_view_t const* fields;

    /** Step between @c fields. */
    ustore_size_t fields_stride;
} ustore_docs_export_t;

void ustore_docs_export(ustore_docs_export_t*);

/** Imports every row of one NDJSON or CSV file as a graph edge. */
typedef struct ustore_graph_import_t {

    /** Already open database instance. */
    ustore_database_t db;

    /** Receives the error message of a failed import. */
    ustore_error_t* error;

    /** Reusable memory handle, or null for a private one. */
    ustore_arena_t* arena;

    /** Options forwarded to the edge upserts. */
    ustore_options_t options;

    /** Collection receiving the edges. */
    ustore_collection_id_t collection;

    /** The file to import, whose @c .ndjson or @c .csv extension selects the format. */
    ustore_str_view_t path;

    /** Bytes of edges buffered before one batched upsert. */
    ustore_size_t max_batch_size;

    /** Integer field or JSON pointer holding the source vertex. */
    ustore_str_view_t source_id_field;

    /** Integer field or JSON pointer holding the target vertex. */
    ustore_str_view_t target_id_field;

    /** Integer field or JSON pointer holding the edge, or null for the default edge. */
    ustore_str_view_t edge_id_field;
} ustore_graph_import_t;

void ustore_graph_import(ustore_graph_import_t*);

/** Exports every edge of a graph collection as NDJSON lines or CSV rows. */
typedef struct ustore_graph_export_t {

    /** Already open database instance. */
    ustore_database_t db;

    /** Receives the error message of a failed export. */
    ustore_error_t* error;

    /** Reusable memory handle, or null for a private one. */
    ustore_arena_t* arena;

    /** Options forwarded to the scans and edge lookups. */
    ustore_options_t options;

    /** Collection to export. */
    ustore_collection_id_t collection;

    /** The file to create, whose @c .ndjson or @c .csv extension selects the format. */
    ustore_str_view_t path;

    /** Bytes of output buffered before one write to the file. */
    ustore_size_t max_batch_size;

    /** Name written for the source vertex column. */
    ustore_str_view_t source_id_field;

    /** Name written for the target vertex column. */
    ustore_str_view_t target_id_field;

    /** Name written for the edge column, or null to omit edges. */
    ustore_str_view_t edge_id_field;
} ustore_graph_export_t;

void ustore_graph_export(ustore_graph_export_t*);

#ifdef __cplusplus
} /* end extern "C" */
#endif
