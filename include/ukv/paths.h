/**
 * @file paths.h
 * @author Ashot Vardanian
 * @date 23 Sep 2022
 * @brief C bindings for paths ~ variable length string keys collections.
 *
 * It is a bad practice to use strings as key, but if your application depends
 * on them, use "paths collections" to map such strings into Unique IDs and
 * saving them as values. Those Unique IDs can then be used to address richer
 * modalities, like Graphs and Docs.
 *
 * @section Separators
 * Strings keys often represent hierarchical paths. The character used as
 * delimeter/separator can be passed together with the queries to add transparent
 * indexes, that on prefix scan - would narrow down the search space.
 *
 * @section Allowed Characters
 * String keys can contain any characters, but if you plan to use `ukv_paths_match`
 * for both RegEx and prefix matches it is recommended to avoid RegEx special characters
 * in names: ., +, *, ?, ^, $, (, ), [, ], {, }, |, \.
 * The other punctuation marks like: /, :, @, -, _, #, ~, comma.
 */

#pragma once

#include "db.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ukv_paths_write_t {

    ukv_database_t db;
    ukv_error_t* error;
    ukv_transaction_t transaction = NULL;
    ukv_arena_t* arena = NULL;
    ukv_options_t options = ukv_options_default_k;
    ukv_size_t tasks_count = 1;
    ukv_char_t path_separator = '\0';

    // Inputs:
    ukv_collection_t const* collections = NULL;
    ukv_size_t collections_stride = 0;

    // Inputs: Paths ~ Variable Length String Keys
    ukv_str_view_t const* paths = NULL;
    ukv_size_t paths_stride = 0;

    ukv_length_t const* paths_offsets = NULL;
    ukv_size_t paths_offsets_stride = 0;

    ukv_length_t const* paths_lengths = NULL;
    ukv_size_t paths_lengths_stride = 0;

    // Inputs: Paths ~ Variable Length String Keys
    ukv_octet_t const* values_presences = NULL;

    ukv_length_t const* values_offsets = NULL;
    ukv_size_t values_offsets_stride = 0;

    ukv_length_t const* values_lengths = NULL;
    ukv_size_t values_lengths_stride = 0;

    ukv_bytes_cptr_t const* values_bytes = NULL;
    ukv_size_t values_bytes_stride = 0;

} ukv_paths_wite_t;

/**
 * @brief Maps string paths to string values.
 * Exact generalization of ukv_write for variable-length keys.
 */
void ukv_paths_write(ukv_paths_wite_t*);

typedef struct ukv_paths_read_t {

    ukv_database_t db;
    ukv_error_t* error;
    ukv_transaction_t transaction = NULL;
    ukv_arena_t* arena = NULL;
    ukv_options_t options = ukv_options_default_k;
    ukv_size_t tasks_count = 1;
    ukv_char_t path_separator = '\0';

    // Inputs:
    ukv_collection_t const* collections = NULL;
    ukv_size_t collections_stride = 0;

    // Inputs: Paths ~ Variable Length String Keys
    ukv_str_view_t const* paths = NULL;
    ukv_size_t paths_stride = 0;

    ukv_length_t const* paths_offsets = NULL;
    ukv_size_t paths_offsets_stride = 0;

    ukv_length_t const* paths_lengths = NULL;
    ukv_size_t paths_lengths_stride = 0;

    // Outputs:
    ukv_octet_t** presences = NULL;
    ukv_length_t** offsets = NULL;
    ukv_length_t** lengths = NULL;
    ukv_byte_t** values = NULL;

} ukv_paths_read_t;
/**
 * @brief Retrieves binary values, given string paths.
 * Exact generalization of `ukv_read` for variable-length keys.
 */
void ukv_paths_read(ukv_paths_read_t*);

typedef struct ukv_paths_match_t {

    ukv_database_t db;
    ukv_error_t* error;
    ukv_transaction_t transaction = NULL;
    ukv_arena_t* arena = NULL;
    ukv_options_t options = ukv_options_default_k;
    ukv_size_t tasks_count = 1;
    ukv_char_t path_separator = '\0';

    // Inputs:
    ukv_collection_t const* collections = NULL;
    ukv_size_t collections_stride = 0;

    // Inputs: Patterns to match in Paths
    ukv_str_view_t const* patterns;
    ukv_size_t patterns_stride = 0;

    ukv_length_t const* patterns_offsets = NULL;
    ukv_size_t patterns_offsets_stride = 0;

    ukv_length_t const* patterns_lengths = NULL;
    ukv_size_t patterns_lengths_stride = 0;

    // Inputs: Match counts
    ukv_length_t const* match_counts_limits;
    ukv_size_t match_counts_limits_stride = 0;

    // Inputs: Previous results to use for pagination
    ukv_str_view_t const* previous = NULL;
    ukv_size_t previous_stride = 0;

    ukv_length_t const* previous_offsets = NULL;
    ukv_size_t previous_offsets_stride = 0;

    ukv_length_t const* previous_lengths = NULL;
    ukv_size_t previous_lengths_stride = 0;

    // Outputs:
    ukv_length_t** match_counts = NULL;
    ukv_length_t** paths_offsets = NULL;
    ukv_char_t** paths_strings = NULL;

} ukv_paths_match_t;

/**
 * @brief Implement "prefix" and "pattern" matching on paths stored in potentially
 * different collections. If a "pattern" contains RegEx special symbols, than it is
 * treated as a RegEx pattern: ., +, *, ?, ^, $, (, ), [, ], {, }, |, \.
 * Otherwise, it is treated as a prefix for search.
 */
void ukv_paths_match(ukv_paths_match_t*);

#ifdef __cplusplus
} /* end extern "C" */
#endif