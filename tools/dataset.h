
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <ukv/db.h>

typedef struct ukv_graph_import_t {

    ukv_database_t db;
    ukv_error_t* error;
    ukv_arena_t* arena = NULL;
    ukv_options_t options = ukv_options_default_k;

    ukv_collection_t collection = ukv_collection_main_k;
    ukv_str_view_t paths_pattern = ".*\\.(csv|ndjson|parquet)";
    ukv_size_t max_batch_size = 1024ul * 1024ul * 1024ul;
    ukv_callback_t callback = NULL;
    ukv_callback_payload_t callback_payload = NULL;

    ukv_str_view_t source_id_field = "source";
    ukv_str_view_t target_id_field = "target";
    ukv_str_view_t edge_id_field = "edge";

} ukv_graph_import_t;

void ukv_graph_import(ukv_graph_import_t*);

typedef struct ukv_graph_export_t {

    ukv_database_t db;
    ukv_error_t* error;
    ukv_arena_t* arena = NULL;
    ukv_options_t options = ukv_options_default_k;

    ukv_collection_t collection = ukv_collection_main_k;
    ukv_str_view_t paths_extension = ".parquet";
    ukv_size_t max_batch_size = 1024ul * 1024ul * 1024ul;
    ukv_callback_t callback = NULL;
    ukv_callback_payload_t callback_payload = NULL;

    ukv_str_view_t source_id_field = "source";
    ukv_str_view_t target_id_field = "target";
    ukv_str_view_t edge_id_field = "edge";

} ukv_graph_export_t;

void ukv_graph_export(ukv_graph_export_t*);

typedef struct ukv_docs_import_t {

    ukv_database_t db;
    ukv_error_t* error;
    ukv_arena_t* arena = NULL;
    ukv_options_t options = ukv_options_default_k;

    ukv_collection_t collection = ukv_collection_main_k;
    ukv_str_view_t paths_pattern = ".*\\.(csv|ndjson|parquet)";
    ukv_size_t max_batch_size = 1024ul * 1024ul * 1024ul;
    ukv_callback_t callback = NULL;
    ukv_callback_payload_t callback_payload = NULL;

    ukv_size_t fields_count = 0;
    ukv_str_view_t const* fields = NULL;
    ukv_size_t fields_stride = 0;

    ukv_str_view_t id_field = "_id";
    ukv_collection_t paths_collection = ukv_collection_main_k;

} ukv_docs_import_t;

void ukv_docs_import(ukv_docs_import_t*);

typedef struct ukv_docs_export_t {

    ukv_database_t db;
    ukv_error_t* error;
    ukv_arena_t* arena = NULL;
    ukv_options_t options = ukv_options_default_k;

    ukv_collection_t collection = ukv_collection_main_k;
    ukv_str_view_t paths_extension = ".parquet";
    ukv_size_t max_batch_size = 1024ul * 1024ul * 1024ul;
    ukv_callback_t callback = NULL;
    ukv_callback_payload_t callback_payload = NULL;

    ukv_size_t fields_count = 0;
    ukv_str_view_t const* fields = NULL;
    ukv_size_t fields_stride = 0;

} ukv_docs_export_t;

void ukv_docs_export(ukv_docs_export_t*);

#ifdef __cplusplus
} /* end extern "C" */
#endif

