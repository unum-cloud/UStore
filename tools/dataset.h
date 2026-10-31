/**
 *  @file tools/dataset.h
 *  @author Ash Vardanian
 *  @date April 16, 2023
 *  @brief Imports and exports documents and graph edges as NDJSON or CSV files, and whole collections of every modality
 *      as Apache Arrow IPC streams and files.
 */

#pragma once

#include "ustore/db.h"

#ifdef __cplusplus
extern "C" {
#endif

/** How the bytes of a dataset are spelled. */
typedef enum ustore_dataset_format_t {

    /** Named by the extension of the path, where the standard input of @c - is sniffed on import and streamed on
     *  export. */
    ustore_dataset_format_automatic_k = 0,

    /** One JSON document per line, @c .ndjson. */
    ustore_dataset_format_ndjson_k = 1,

    /** Comma-separated records under a header, @c .csv. */
    ustore_dataset_format_csv_k = 2,

    /** An Apache Arrow IPC stream, @c .arrows. */
    ustore_dataset_format_arrow_stream_k = 3,

    /** An Apache Arrow IPC file, @c .arrow or @c .feather, read sequentially so a pipe carries it too. */
    ustore_dataset_format_arrow_file_k = 4,

} ustore_dataset_format_t;

/** The format @p path spells through its extension, or @p asked when it names one, staying automatic for a path that
 *  spells none. */
ustore_dataset_format_t ustore_dataset_format_of(ustore_bytes_view_t path, ustore_dataset_format_t asked);

/** How an Arrow import lands its rows. */
typedef enum ustore_dataset_ingestion_t {

    /** Through the writes of the modality, a batch at a time. */
    ustore_dataset_ingestion_batched_k = 0,

    /** As one commit of strictly ascending keys, through an ingest of @c ustore_ingest_sorted_k. */
    ustore_dataset_ingestion_sorted_k = 1,

    /** As one commit of keys in any order, the last repeat winning, through an @c ustore_ingest_unsorted_k ingest. */
    ustore_dataset_ingestion_unsorted_k = 2,

} ustore_dataset_ingestion_t;

/** Imports an Apache Arrow stream or file of any modality into a named collection, created when missing. */
typedef struct ustore_dataset_import_t {

    /** Already open database instance. */
    ustore_database_t database;

    /** Receives the outcome of the import. */
    ustore_outcome_t* outcome;

    /** How durable the import is once final. */
    ustore_durability_t durability;

    /** Name of the collection to write, created when missing, where empty takes the schema's @c ustore.collection. */
    ustore_bytes_view_t collection_name;

    /** What the collection holds, where zero takes the schema's @c ustore.modality, else what its columns spell. A
     *  collection that already exists must hold what is named here. */
    ustore_modality_t modality;

    /** The file to import, mapped whole, or @c - for standard input, read in the byte counts each message names. */
    ustore_bytes_view_t path;

    /** How the input is spelled, where @c ustore_dataset_format_automatic_k reads the extension. */
    ustore_dataset_format_t format;

    /** How the rows land, where a bulk ingest that fails drops the collection it created. */
    ustore_dataset_ingestion_t ingestion;

    /** Optional output for the rows written. */
    ustore_size_t* written;
} ustore_dataset_import_t;

void ustore_dataset_import(ustore_dataset_import_t*);

/** Exports a collection of any modality as an Apache Arrow stream or file, its buffers gathered without copies. */
typedef struct ustore_dataset_export_t {

    /** Already open database instance. */
    ustore_database_t database;

    /** Receives the outcome of the export. */
    ustore_outcome_t* outcome;

    /** What the export sees, where @c NULL takes a snapshot of the head. */
    ustore_view_t view;

    /** Collection to export. */
    ustore_collection_id_t collection;

    /** Name written as the schema's @c ustore.collection, so an import recreates the collection under it, or empty for
     *  none. */
    ustore_bytes_view_t collection_name;

    /** The file to create, or @c - for standard output. */
    ustore_bytes_view_t path;

    /** How the output is spelled, where @c ustore_dataset_format_automatic_k reads the extension and @c - streams. */
    ustore_dataset_format_t format;

    /** Where every body and buffer starts at its absolute position: 64, the default for zero, or 4096 for readers
     *  through @c O_DIRECT and cuFile. */
    ustore_size_t alignment;

    /** Most rows per batch, where zero takes the export's default. */
    ustore_size_t batch_rows;

    /** Optional output for the rows written. */
    ustore_size_t* written;
} ustore_dataset_export_t;

void ustore_dataset_export(ustore_dataset_export_t*);

/** Imports every row of one NDJSON or CSV file as a document, keyed by its @c id_field. */
typedef struct ustore_docs_import_t {

    /** Already open database instance. */
    ustore_database_t database;

    /** Receives the outcome of the import. */
    ustore_outcome_t* outcome;

    /** How durable the document writes are once the import is final. */
    ustore_durability_t durability;

    /** Collection receiving the documents. */
    ustore_collection_id_t collection;

    /** The file to import, whose @c .ndjson or @c .csv extension selects the format. */
    ustore_bytes_view_t path;

    /** Bytes of documents buffered before one batched write. */
    ustore_size_t max_batch_size;

    /** Number of @c fields to keep, or zero to keep whole rows. */
    ustore_size_t fields_count;

    /** Field names or JSON pointers to keep, @c fields_count of them. */
    ustore_strings_view_t fields;

    /** Field holding each document's key, which @c fields must include. */
    ustore_bytes_view_t id_field;
} ustore_docs_import_t;

void ustore_docs_import(ustore_docs_import_t*);

/** Exports every document of a collection as @c {"_id":key,"doc":document} NDJSON lines or @c _id,doc CSV rows. */
typedef struct ustore_docs_export_t {

    /** Already open database instance. */
    ustore_database_t database;

    /** Receives the outcome of the export. */
    ustore_outcome_t* outcome;

    /** Memory every read of documents rewinds and reuses, or @c NULL for a private one. */
    ustore_arena_t arena;

    /** What the scans and reads see, where @c NULL is the head. */
    ustore_view_t view;

    /** Collection to export. */
    ustore_collection_id_t collection;

    /** The file to create, whose @c .ndjson or @c .csv extension selects the format. */
    ustore_bytes_view_t path;

    /** Bytes of output buffered before one write to the file. */
    ustore_size_t max_batch_size;

    /** Number of @c fields to keep, or zero to keep whole documents. */
    ustore_size_t fields_count;

    /** Field names or JSON pointers to keep, @c fields_count of them. */
    ustore_strings_view_t fields;
} ustore_docs_export_t;

void ustore_docs_export(ustore_docs_export_t*);

/** Imports every row of one NDJSON or CSV file as a graph edge. */
typedef struct ustore_graph_import_t {

    /** Already open database instance. */
    ustore_database_t database;

    /** Receives the outcome of the import. */
    ustore_outcome_t* outcome;

    /** How durable the edge upserts are once the import is final. */
    ustore_durability_t durability;

    /** Collection receiving the edges. */
    ustore_collection_id_t collection;

    /** The file to import, whose @c .ndjson or @c .csv extension selects the format. */
    ustore_bytes_view_t path;

    /** Bytes of edges buffered before one batched upsert. */
    ustore_size_t max_batch_size;

    /** Integer field or JSON pointer holding the source vertex. */
    ustore_bytes_view_t source_id_field;

    /** Integer field or JSON pointer holding the target vertex. */
    ustore_bytes_view_t target_id_field;

    /** Integer field or JSON pointer holding the edge, or empty for the default edge. */
    ustore_bytes_view_t edge_id_field;
} ustore_graph_import_t;

void ustore_graph_import(ustore_graph_import_t*);

/** Exports every edge of a graph collection as NDJSON lines or CSV rows. */
typedef struct ustore_graph_export_t {

    /** Already open database instance. */
    ustore_database_t database;

    /** Receives the outcome of the export. */
    ustore_outcome_t* outcome;

    /** Memory every lookup of edges rewinds and reuses, or @c NULL for a private one. */
    ustore_arena_t arena;

    /** What the scans and edge lookups see, where @c NULL is the head. */
    ustore_view_t view;

    /** Collection to export. */
    ustore_collection_id_t collection;

    /** The file to create, whose @c .ndjson or @c .csv extension selects the format. */
    ustore_bytes_view_t path;

    /** Bytes of output buffered before one write to the file. */
    ustore_size_t max_batch_size;

    /** Name written for the source vertex column. */
    ustore_bytes_view_t source_id_field;

    /** Name written for the target vertex column. */
    ustore_bytes_view_t target_id_field;

    /** Name written for the edge column, or empty to omit edges. */
    ustore_bytes_view_t edge_id_field;
} ustore_graph_export_t;

void ustore_graph_export(ustore_graph_export_t*);

#ifdef __cplusplus
} /* end extern "C" */
#endif
