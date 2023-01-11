/**
 * @file paths.h
 * @author Ashot Vardanian
 * @date 23 Sep 2022
 * @addtogroup C
 *
 * @brief Binary Interface Standard for @b BLOB collections with @b variable-length keys.
 *
 * It is a bad practice to use strings as key, but if your application depends
 * on them, use "paths collections" to map such strings into Unique IDs and
 * saving them as values. Those Unique IDs can then be used to address richer
 * modalities, like Graphs and Docs.
 *
 * ## Separators
 *
 * Strings keys often represent hierarchical paths. The character used as
 * delimeter/separator can be passed together with the queries to add transparent
 * indexes, that on prefix scan - would narrow down the search space.
 *
 * ## Allowed Characters
 *
 * String keys can contain any characters, but if you plan to use `ukv_paths_match()`
 * for both RegEx and prefix matches it is recommended to avoid RegEx special characters
 * in names: ., +, *, ?, ^, $, (, ), [, ], {, }, |, \.
 * The other punctuation marks like: /, :, @, -, _, #, ~, comma.
 */

#pragma once

#include "db.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Maps string paths to binary values.
 * Generalization of @c ukv_write_t to variable-length key.
 * @see `ukv_paths_write()`, `ukv_write_t`, `ukv_write()`.
 */
typedef struct ukv_paths_write_t {

    /// @name Context
    /// @{

    /** @brief Already open database instance. */
    ukv_database_t db;
    /** @brief Pointer to exported error message. */
    ukv_error_t* error;
    /** @brief The transaction in which the operation will be watched. */
    ukv_transaction_t transaction;
    /** @brief Reusable memory handle. */
    ukv_arena_t* arena;
    /** @brief Read and Write options for Read-Modify-Write logic. @see `ukv_read_t`, `ukv_write_t`. */
    ukv_options_t options;

    /// @}
    /// @name Inputs
    /// @{

    ukv_size_t tasks_count;
    ukv_char_t path_separator;

    ukv_collection_t const* collections;
    ukv_size_t collections_stride;

    /// @name Variable Length Keys
    /// @{
    ukv_str_view_t const* paths;
    ukv_size_t paths_stride;

    ukv_length_t const* paths_offsets;
    ukv_size_t paths_offsets_stride;

    ukv_length_t const* paths_lengths;
    ukv_size_t paths_lengths_stride;
    // @}

    /// @name Variable Length Values
    /// @{
    ukv_octet_t const* values_presences;

    ukv_length_t const* values_offsets;
    ukv_size_t values_offsets_stride;

    ukv_length_t const* values_lengths;
    ukv_size_t values_lengths_stride;

    ukv_bytes_cptr_t const* values_bytes;
    ukv_size_t values_bytes_stride;
    // @}

    // @}

} ukv_paths_write_t;

/**
 * @brief Maps string paths to binary values.
 * Generalization of @c ukv_write_t to variable-length key.
 * @see `ukv_paths_write_t`, `ukv_write_t`, `ukv_write()`.
 */
void ukv_paths_write(ukv_paths_write_t*);

/**
 * @brief Retrieves binary values given string paths.
 * Generalization of @c ukv_read_t to variable-length key.
 * @see `ukv_paths_read()`, `ukv_read_t`, `ukv_read()`.
 */
typedef struct ukv_paths_read_t {

    /// @name Context
    /// @{

    /** @brief Already open database instance. */
    ukv_database_t db;
    /** @brief Pointer to exported error message. */
    ukv_error_t* error;
    /** @brief The transaction in which the operation will be watched. */
    ukv_transaction_t transaction;
    /** @brief Reusable memory handle. */
    ukv_arena_t* arena;
    /** @brief Read options. @see `ukv_read_t`. */
    ukv_options_t options;

    /// @}
    /// @name Inputs
    /// @{

    ukv_size_t tasks_count;
    ukv_char_t path_separator;

    ukv_collection_t const* collections;
    ukv_size_t collections_stride;

    /// @name Variable Length Keys
    /// @{
    ukv_str_view_t const* paths;
    ukv_size_t paths_stride;

    ukv_length_t const* paths_offsets;
    ukv_size_t paths_offsets_stride;

    ukv_length_t const* paths_lengths;
    ukv_size_t paths_lengths_stride;
    /// @}

    /// @}
    /// @name Outputs
    /// @{
    ukv_octet_t** presences;
    ukv_length_t** offsets;
    ukv_length_t** lengths;
    ukv_byte_t** values;
    /// @}

} ukv_paths_read_t;

/**
 * @brief Retrieves binary values given string paths.
 * Generalization of @c ukv_read_t to variable-length key.
 * @see `ukv_paths_read_t`, `ukv_read_t`, `ukv_read()`.
 */
void ukv_paths_read(ukv_paths_read_t*);

/**
 * @brief Vectorized "Prefix" and RegEx "Pattern Matching" for paths.
 * @see `ukv_paths_match()`.
 *
 * If a "pattern" contains RegEx special symbols, than it is
 * treated as a RegEx pattern: ., +, *, ?, ^, $, (, ), [, ], {, }, |, \.
 * Otherwise, it is treated as a prefix for search.
 */
typedef struct ukv_paths_match_t {

    /// @name Context
    /// @{

    /** @brief Already open database instance. */
    ukv_database_t db;
    /** @brief Pointer to exported error message. */
    ukv_error_t* error;
    /** @brief The transaction in which the operation will be watched. */
    ukv_transaction_t transaction;
    /** @brief Reusable memory handle. */
    ukv_arena_t* arena;
    /** @brief Read options. @see `ukv_read_t`. */
    ukv_options_t options;

    /// @}
    /// @name Inputs
    /// @{

    ukv_size_t tasks_count;
    ukv_char_t path_separator;

    ukv_collection_t const* collections;
    ukv_size_t collections_stride;

    ukv_length_t const* match_counts_limits;
    ukv_size_t match_counts_limits_stride;

    /// @name Variable Length Patterns to Match in Paths
    /// @{
    ukv_str_view_t const* patterns;
    ukv_size_t patterns_stride;

    ukv_length_t const* patterns_offsets;
    ukv_size_t patterns_offsets_stride;

    ukv_length_t const* patterns_lengths;
    ukv_size_t patterns_lengths_stride;
    /// @}

    /// @name Previous Matches Used for Pagination
    /// @{
    ukv_str_view_t const* previous;
    ukv_size_t previous_stride;

    ukv_length_t const* previous_offsets;
    ukv_size_t previous_offsets_stride;

    ukv_length_t const* previous_lengths;
    ukv_size_t previous_lengths_stride;
    /// @}

    /// @}
    /// @name Outputs
    /// @{
    ukv_length_t** match_counts;
    ukv_length_t** paths_offsets;
    ukv_char_t** paths_strings;
    /// @}

} ukv_paths_match_t;

/**
 * @brief Vectorized "Prefix" and RegEx "Pattern Matching" for paths.
 * @see `ukv_paths_match_t`.
 */
void ukv_paths_match(ukv_paths_match_t*);

#ifdef __cplusplus
} /* end extern "C" */
#endif