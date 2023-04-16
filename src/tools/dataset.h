
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <ustore/db.h>

typedef struct ustore_docs_import_t {

    ustore_database_t db;
    ustore_error_t* error;
    ustore_arena_t* arena; // optional
    ustore_options_t options; // ustore_options_default_k

    ustore_collection_t collection; // ustore_collection_main_k
    ustore_str_view_t paths_pattern; // ".*\\.(csv|ndjson|parquet)"
    ustore_size_t max_batch_size; // 1024ul * 1024ul * 1024ul
    ustore_callback_t callback; // optional
    ustore_callback_payload_t callback_payload; // optional

    ustore_size_t fields_count; // optional
    ustore_str_view_t const* fields; // optional
    ustore_size_t fields_stride; // optional

    ustore_str_view_t id_field; // "_id"
    ustore_collection_t paths_collection; // ustore_collection_main_k

} ustore_docs_import_t;

void ustore_docs_import(ustore_docs_import_t*);

typedef struct ustore_docs_export_t {

    ustore_database_t db;
    ustore_error_t* error;
    ustore_arena_t* arena; // optional
    ustore_options_t options; // ustore_options_default_k

    ustore_collection_t collection; // ustore_collection_main_k
    ustore_str_view_t paths_extension; // ".parquet"
    ustore_size_t max_batch_size; // 1024ul * 1024ul * 1024ul
    ustore_callback_t callback; // optional
    ustore_callback_payload_t callback_payload; // optional

    ustore_size_t fields_count; // optional
    ustore_str_view_t const* fields; // optional
    ustore_size_t fields_stride; // optional

} ustore_docs_export_t;

void ustore_docs_export(ustore_docs_export_t*);

#ifdef __cplusplus
} /* end extern "C" */
#endif
