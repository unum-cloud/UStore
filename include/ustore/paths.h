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
 * String keys can contain any characters, but if you plan to use `ustore_paths_match()`
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
 * Generalization of @c ustore_write_t to variable-length key.
 * @see `ustore_paths_write()`, `ustore_write_t`, `ustore_write()`.
 */
typedef struct ustore_paths_write_t {

    /// @name Context
    /// @{

    /** @brief Already open database instance. */
    ustore_database_t db;
    /** @brief Pointer to exported error message. */
    ustore_error_t* error;
    /** @brief The transaction in which the operation will be watched. */
    ustore_transaction_t transaction;
    /** @brief Reusable memory handle. */
    ustore_arena_t* arena;
    /** @brief Read and Write options for Read-Modify-Write logic. @see `ustore_read_t`, `ustore_write_t`. */
    ustore_options_t options;

    /// @}
    /// @name Inputs
    /// @{

    ustore_size_t tasks_count;
    ustore_char_t path_separator;

    ustore_collection_t const* collections;
    ustore_size_t collections_stride;

    /// @name Variable Length Keys
    /// @{
    ustore_str_view_t const* paths;
    ustore_size_t paths_stride;

    ustore_length_t const* paths_offsets;
    ustore_size_t paths_offsets_stride;

    ustore_length_t const* paths_lengths;
    ustore_size_t paths_lengths_stride;
    // @}

    /// @name Variable Length Values
    /// @{
    ustore_octet_t const* values_presences;

    ustore_length_t const* values_offsets;
    ustore_size_t values_offsets_stride;

    ustore_length_t const* values_lengths;
    ustore_size_t values_lengths_stride;

    ustore_bytes_cptr_t const* values_bytes;
    ustore_size_t values_bytes_stride;
    // @}

    // @}

} ustore_paths_write_t;

/**
 * @brief Maps string paths to binary values.
 * Generalization of @c ustore_write_t to variable-length key.
 * @see `ustore_paths_write_t`, `ustore_write_t`, `ustore_write()`.
 */
void ustore_paths_write(ustore_paths_write_t*);

/**
 * @brief Retrieves binary values given string paths.
 * Generalization of @c ustore_read_t to variable-length key.
 * @see `ustore_paths_read()`, `ustore_read_t`, `ustore_read()`.
 */
typedef struct ustore_paths_read_t {

    /// @name Context
    /// @{

    /** @brief Already open database instance. */
    ustore_database_t db;
    /** @brief Pointer to exported error message. */
    ustore_error_t* error;
    /** @brief The transaction in which the operation will be watched. */
    ustore_transaction_t transaction;
    /** @brief The snapshot captures a view of the database at the time it's created. */
    ustore_snapshot_t snapshot;
    /** @brief Reusable memory handle. */
    ustore_arena_t* arena;
    /** @brief Read options. @see `ustore_read_t`. */
    ustore_options_t options;

    /// @}
    /// @name Inputs
    /// @{

    ustore_size_t tasks_count;
    ustore_char_t path_separator;

    ustore_collection_t const* collections;
    ustore_size_t collections_stride;

    /// @name Variable Length Keys
    /// @{
    ustore_str_view_t const* paths;
    ustore_size_t paths_stride;

    ustore_length_t const* paths_offsets;
    ustore_size_t paths_offsets_stride;

    ustore_length_t const* paths_lengths;
    ustore_size_t paths_lengths_stride;
    /// @}

    /// @}
    /// @name Outputs
    /// @{
    ustore_octet_t** presences;
    ustore_length_t** offsets;
    ustore_length_t** lengths;
    ustore_byte_t** values;
    /// @}

} ustore_paths_read_t;

/**
 * @brief Retrieves binary values given string paths.
 * Generalization of @c ustore_read_t to variable-length key.
 * @see `ustore_paths_read_t`, `ustore_read_t`, `ustore_read()`.
 */
void ustore_paths_read(ustore_paths_read_t*);

/**
 * @brief Vectorized "Prefix" and RegEx "Pattern Matching" for paths.
 * @see `ustore_paths_match()`.
 *
 * If a "pattern" contains RegEx special symbols, than it is
 * treated as a RegEx pattern: ., +, *, ?, ^, $, (, ), [, ], {, }, |, \.
 * Otherwise, it is treated as a prefix for search.
 */
typedef struct ustore_paths_match_t {

    /// @name Context
    /// @{

    /** @brief Already open database instance. */
    ustore_database_t db;
    /** @brief Pointer to exported error message. */
    ustore_error_t* error;
    /** @brief The transaction in which the operation will be watched. */
    ustore_transaction_t transaction;
    /** @brief Reusable memory handle. */
    ustore_arena_t* arena;
    /** @brief Read options. @see `ustore_read_t`. */
    ustore_options_t options;

    /// @}
    /// @name Inputs
    /// @{

    ustore_size_t tasks_count;
    ustore_char_t path_separator;

    ustore_collection_t const* collections;
    ustore_size_t collections_stride;

    ustore_length_t const* match_counts_limits;
    ustore_size_t match_counts_limits_stride;

    /// @name Variable Length Patterns to Match in Paths
    /// @{
    ustore_str_view_t const* patterns;
    ustore_size_t patterns_stride;

    ustore_length_t const* patterns_offsets;
    ustore_size_t patterns_offsets_stride;

    ustore_length_t const* patterns_lengths;
    ustore_size_t patterns_lengths_stride;
    /// @}

    /// @name Previous Matches Used for Pagination
    /// @{
    ustore_str_view_t const* previous;
    ustore_size_t previous_stride;

    ustore_length_t const* previous_offsets;
    ustore_size_t previous_offsets_stride;

    ustore_length_t const* previous_lengths;
    ustore_size_t previous_lengths_stride;
    /// @}

    /// @}
    /// @name Outputs
    /// @{
    ustore_length_t** match_counts;
    ustore_length_t** paths_offsets;
    ustore_char_t** paths_strings;
    /// @}

} ustore_paths_match_t;

/**
 * @brief Vectorized "Prefix" and RegEx "Pattern Matching" for paths.
 * @see `ustore_paths_match_t`.
 */
void ustore_paths_match(ustore_paths_match_t*);

#ifdef __cplusplus
} /* end extern "C" */
#endif