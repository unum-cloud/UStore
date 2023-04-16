/**
 * @file paths.h
 * @author Ashot Vardanian
 * @date 23 Sep 2022
 * @addtogroup C
 *
 * @brief Binary Interface Standard for @b Vector collections.
 */

#pragma once

#include "db.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {

    ustore_vector_metric_cos_k = 0,
    ustore_vector_metric_dot_k = 1,
    ustore_vector_metric_l2_k = 2,

} ustore_vector_metric_t;

typedef enum {

    ustore_vector_scalar_f32_k = 0,
    ustore_vector_scalar_f16_k = 1,
    ustore_vector_scalar_i8_k = 2,
    ustore_vector_scalar_f64_k = 3,

} ustore_vector_scalar_t;

/**
 * @brief Maps keys to High-Dimensional Vectors.
 * Generalization of @c ustore_write_t to numerical vectors.
 * @see `ustore_vectors_write()`, `ustore_write_t`, `ustore_write()`.
 *
 * Assuming all the vectors within the operation will have the
 * same dimensionality and their scalar components would form
 * continuous chunks, we need less arguments for this call,
 * than some binary methods.
 */
typedef struct ustore_vectors_write_t {

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
    ustore_length_t dimensions;
    ustore_vector_scalar_t scalar_type;

    ustore_collection_t const* collections;
    ustore_size_t collections_stride;

    ustore_key_t const* keys;
    ustore_size_t keys_stride;

    ustore_bytes_cptr_t const* vectors_starts;
    ustore_size_t vectors_starts_stride;
    ustore_size_t vectors_stride;

    ustore_length_t const* offsets;
    ustore_size_t offsets_stride;

    // @}

} ustore_vectors_write_t;

/**
 * @brief Maps keys to High-Dimensional Vectors.
 * Generalization of @c ustore_write_t to numerical vectors.
 * @see `ustore_vectors_write_t`, `ustore_write_t`, `ustore_write()`.
 */
void ustore_vectors_write(ustore_vectors_write_t*);

/**
 * @brief Retrieves binary representations of vectors.
 * Generalization of @c ustore_read_t to numerical vectors.
 * Packs everything into a @b row-major dense matrix.
 * @see `ustore_vectors_read()`, `ustore_read_t`, `ustore_read()`.
 */
typedef struct ustore_vectors_read_t {

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
    ustore_length_t dimensions;
    ustore_vector_scalar_t scalar_type;

    ustore_collection_t const* collections;
    ustore_size_t collections_stride;

    ustore_key_t const* keys;
    ustore_size_t keys_stride;

    /// @}

    /// @}
    /// @name Outputs
    /// @{
    ustore_octet_t** presences;
    ustore_length_t** offsets;
    ustore_byte_t** vectors;
    /// @}

} ustore_vectors_read_t;

/**
 * @brief Retrieves binary values given string paths.
 * Generalization of @c ustore_read_t to numerical vectors.
 * @see `ustore_vectors_read_t`, `ustore_read_t`, `ustore_read()`.
 */
void ustore_vectors_read(ustore_vectors_read_t*);

/**
 * @brief Performs K-Approximate Nearest Neighbors Search.
 * @see `ustore_vectors_search()`.
 */
typedef struct ustore_vectors_search_t {

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
    ustore_length_t dimensions;
    ustore_vector_scalar_t scalar_type;
    ustore_vector_metric_t metric;
    ustore_float_t metric_threshold;

    ustore_collection_t const* collections;
    ustore_size_t collections_stride;

    ustore_length_t const* match_counts_limits;
    ustore_size_t match_counts_limits_stride;

    ustore_bytes_cptr_t const* queries_starts;
    ustore_size_t queries_starts_stride;
    ustore_size_t queries_stride;

    ustore_length_t const* queries_offsets;
    ustore_size_t queries_offsets_stride;

    /// @}
    /// @name Outputs
    /// @{
    ustore_length_t** match_counts;
    ustore_length_t** match_offsets;
    ustore_key_t** match_keys;
    ustore_float_t** match_metrics;
    /// @}

} ustore_vectors_search_t;

/**
 * @brief Performs K-Approximate Nearest Neighbors Search.
 * @see `ustore_vectors_search_t`.
 */
void ustore_vectors_search(ustore_vectors_search_t*);

#ifdef __cplusplus
} /* end extern "C" */
#endif