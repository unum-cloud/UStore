
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <ukv/db.h>

typedef struct ukv_docs_import_t {

    ukv_database_t db;
    ukv_error_t* error;
    ukv_arena_t* arena; // optional
    ukv_options_t options; // ukv_options_default_k

    ukv_collection_t collection; // ukv_collection_main_k
    ukv_str_view_t paths_pattern; // ".*\\.(csv|ndjson|parquet)"
    ukv_size_t max_batch_size; // 1024ul * 1024ul * 1024ul
    ukv_callback_t callback; // optional
    ukv_callback_payload_t callback_payload; // optional

    ukv_size_t fields_count; // optional
    ukv_str_view_t const* fields; // optional
    ukv_size_t fields_stride; // optional

    ukv_str_view_t id_field; // "_id"
    ukv_collection_t paths_collection; // ukv_collection_main_k

} ukv_docs_import_t;

void ukv_docs_import(ukv_docs_import_t*);

typedef struct ukv_docs_export_t {

    ukv_database_t db;
    ukv_error_t* error;
    ukv_arena_t* arena; // optional
    ukv_options_t options; // ukv_options_default_k

    ukv_collection_t collection; // ukv_collection_main_k
    ukv_str_view_t paths_extension; // ".parquet"
    ukv_size_t max_batch_size; // 1024ul * 1024ul * 1024ul
    ukv_callback_t callback; // optional
    ukv_callback_payload_t callback_payload; // optional

    ukv_size_t fields_count; // optional
    ukv_str_view_t const* fields; // optional
    ukv_size_t fields_stride; // optional

} ukv_docs_export_t;

void ukv_docs_export(ukv_docs_export_t*);

#ifdef __cplusplus
} /* end extern "C" */
#endif
