/**
 * @file docs.h
 * @author Ashot Vardanian
 * @date 27 Jun 2022
 * @addtogroup C
 *
 * @brief Binary Interface Standard for JSON-like @b Documents collections.
 *
 * It extends the basic "ukv.h" towards values storing hierarchical documents.
 * Examples: JSONs, MsgPacks, BSONs and a number of other similar formats.
 * Yet no guarantees are provided regarding the internal representation of the
 * values, so if if you want to access same values through binary interface,
 * you may not get the exact same bytes as you have provided in.
 *
 * ## Understanding Fields
 *
 * A field is an intra-document @b potentially-nested key, like: "_id" or "user".
 * To define a nested path, build an RFC 6901 JSON-Pointer, starting with a slash:
 * - "/user/followers_count"
 * - "/posts/0/text"
 *
 * ## Number of Keys vs Number of Fields
 *
 * One of the biggest questions to API is preferring "Zips" vs the "Cartesian Product"
 * of "Key" and "Field" combinations. When writing we may want to discard a certain
 * subset of fields in every document, but we may also be interested in having a
 * more targetted approach.
 *
 * ## Type Checking and Casting
 *
 * Ideally, the data shouldn't be parsed more than once, to avoid performance loss.
 * So the primary interfaces of Docs Store are type-agnostic. Vectorized "gather"
 * operations perform the best effort to convert into the requested format, but
 * it's not always possible.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ukv/db.h"

/*********************************************************/
/*****************   Structures & Consts  ****************/
/*********************************************************/

/**
 * @brief Type IDs needed to describing (sub-) document contents.
 * Most types mimic what's present in Apache Arrow. Others, describe
 * hierarchical documents, like JSON, BSON and MessagePack.
 *
 * For Business Intelligence and Analytics mostly the `::ukv_doc_field_i64_k`
 * and `::ukv_doc_field_f64_k` are used.
 */
typedef enum ukv_doc_field_type_t {

    ukv_doc_field_json_k = 0,
    ukv_doc_field_bson_k = 1,
    ukv_doc_field_msgpack_k = 2,
    ukv_doc_field_default_k = ukv_doc_field_json_k,

    ukv_doc_field_null_k = 10,
    ukv_doc_field_bool_k = 11,
    ukv_doc_field_uuid_k = 12,

    ukv_doc_field_i8_k = 20,
    ukv_doc_field_i16_k = 21,
    ukv_doc_field_i32_k = 22,
    ukv_doc_field_i64_k = 23,

    ukv_doc_field_u8_k = 30,
    ukv_doc_field_u16_k = 31,
    ukv_doc_field_u32_k = 32,
    ukv_doc_field_u64_k = 33,

    ukv_doc_field_f16_k = 40,
    ukv_doc_field_f32_k = 41,
    ukv_doc_field_f64_k = 42,

    ukv_doc_field_bin_k = 50,
    ukv_doc_field_str_k = 51,

} ukv_doc_field_type_t;

/**
 * @brief Kind of document modification to be applied on `ukv_docs_write()`.
 */
typedef enum ukv_doc_modification_t {
    ukv_doc_modify_upsert_k = 0,
    ukv_doc_modify_update_k = 1,
    ukv_doc_modify_insert_k = 2,
    ukv_doc_modify_patch_k = 3,
    ukv_doc_modify_merge_k = 4,
} ukv_doc_modification_t;

/*********************************************************/
/*****************	 Primary Functions	  ****************/
/*********************************************************/

/**
 * @brief Main "setter" interface for (sub-)document-level data.
 * Generalization of @c ukv_write_t to structured values.
 * @see `ukv_docs_write()`, `ukv_write_t`, `ukv_write()`.
 *
 * ## Inferring Document IDs
 *
 * In other interfaces it's necessary to explicitly provide the @c ukv_key_t keys.
 * With documents, you can skip the `keys` and pass just `fields`, which will be
 * used to dynamically extract the keys. To make it compatible with MongoDB and
 * ElasticSearch you can pass @b "_id" into `fields`.
 */

typedef struct ukv_docs_write_t {

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
    /** @brief Write or Read+Write options for Read-Modify-Write operations. @see `ukv_write_t`. */
    ukv_options_t options;

    /// @}
    /// @name Inputs
    /// @{

    ukv_size_t tasks_count;
    ukv_doc_field_type_t type;
    ukv_doc_modification_t modification;

    ukv_collection_t const* collections;
    ukv_size_t collections_stride;

    ukv_key_t const* keys;
    ukv_size_t keys_stride;

    ukv_str_view_t const* fields;
    ukv_size_t fields_stride;

    ukv_octet_t const* presences;

    ukv_length_t const* offsets;
    ukv_size_t offsets_stride;

    ukv_length_t const* lengths;
    ukv_size_t lengths_stride;

    ukv_bytes_cptr_t const* values;
    ukv_size_t values_stride;
    /// @}

} ukv_docs_write_t;

/**
 * @brief Main "setter" interface for (sub-)document-level data.
 * Generalization of @c ukv_write_t to structured values.
 * @see `ukv_docs_write_t`, `ukv_write_t`, `ukv_write()`.
 */
void ukv_docs_write(ukv_docs_write_t*);

/**
 * @brief Main "getter" interface for (sub-)document-level data.
 * Generalization of @c ukv_read_t to structured values.
 * @see `ukv_docs_read()`, `ukv_read_t`, `ukv_read()`.
 */
typedef struct ukv_docs_read_t {

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

    ukv_doc_field_type_t type;
    ukv_size_t tasks_count;

    ukv_collection_t const* collections;
    ukv_size_t collections_stride;

    ukv_key_t const* keys;
    ukv_size_t keys_stride;

    ukv_str_view_t const* fields;
    ukv_size_t fields_stride;

    /// @}
    /// @name Outputs
    /// @{

    ukv_octet_t** presences;
    ukv_length_t** offsets;
    ukv_length_t** lengths;
    ukv_bytes_ptr_t* values;

    /// @}

} ukv_docs_read_t;

/**
 * @brief Main "getter" interface for (sub-)document-level data.
 * Generalization of @c ukv_read_t to structured values.
 * @see `ukv_docs_read_t`, `ukv_read_t`, `ukv_read()`.
 */
void ukv_docs_read(ukv_docs_read_t*);

/**
 * @brief Lists fields & paths present in wanted documents or entire collections.
 * @see `ukv_docs_gist()`.
 */
typedef struct ukv_docs_gist_t {

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

    ukv_size_t docs_count;

    ukv_collection_t const* collections;
    ukv_size_t collections_stride;

    ukv_key_t const* keys;
    ukv_size_t keys_stride;

    /// @}
    /// @name Outputs
    /// @{

    ukv_size_t* fields_count;
    ukv_length_t** offsets;
    ukv_char_t** fields;

    /// @}

} ukv_docs_gist_t;

/**
 * @brief Lists fields & paths present in wanted documents or entire collections.
 * @see `ukv_docs_gist_t`.
 */
void ukv_docs_gist(ukv_docs_gist_t*);

/**
 * @brief Gathers `N*M` values matching `M` fields from `N` docs in @b columnar form.
 * @see `ukv_docs_gather()`.
 *
 * ## Validity Columns
 *
 * Just like Apache Arrow, we export bitsets indicating the validity of matches.
 * Unlike Apache Arrow, we return @b three such bitsets for every array of content:
 *
 * - `columns_validities`: same as in Arrow.
 * - `columns_conversions`: with ones where "string to int" or similar conversions took place.
 * - `columns_collisions`: with ones where non-convertible entry was found under given path.
 *
 * More explicitly, "conversions" mean, that the export/cast changes the semantics.
 * We identify following type groups: booleans, integers, floats, strings.
 * Any downcasting conversion between them will be done with best-effort,
 * but may not be lossless. Meaning that:
 *
 * - `bool` to `int` or `float` isn't a downcast.
 * - `int`, `bool`, `str` to `bool` is a downcast.
 *
 * ## Columns Layout
 *
 * All of `columns_validities`, `columns_conversions`, `columns_collisions`,
 * `columns_scalars`, `columns_offsets` and `columns_lengths` are triple pointers.
 * Meaning that they are pointer to where an array of arrays will exported:
 *
 * - Number of columns will be `== fields_count`.
 * - Number of entries in each column will be `>= docs_count`.
 *
 * ## Strings Layout
 *
 * Texts requested with `::ukv_doc_field_str_k` will be appended with a null-termination
 * character. Binary strings requested with `::ukv_doc_field_bin_k` - will not.
 * Offsets and lengths will be organized in a @b column-major layout with `docs_count`
 * entries in every column, but the contents of the joined string will be organized
 * in a @b row-major order. It will make the data easier to pass into bulk text-search
 * systems or Language Models training pipelines.
 */

typedef struct ukv_docs_gather_t {

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

    ukv_size_t docs_count;
    ukv_size_t fields_count;

    ukv_collection_t const* collections;
    ukv_size_t collections_stride;

    ukv_key_t const* keys;
    ukv_size_t keys_stride;

    ukv_str_view_t const* fields;
    ukv_size_t fields_stride;

    ukv_doc_field_type_t const* types;
    ukv_size_t types_stride;

    /// @}
    /// @name Outputs
    /// @{

    ukv_octet_t*** columns_validities;
    ukv_octet_t*** columns_conversions;
    ukv_octet_t*** columns_collisions;

    ukv_byte_t*** columns_scalars;

    ukv_length_t*** columns_offsets;
    ukv_length_t*** columns_lengths;
    ukv_byte_t** joined_strings;

    /// @}

} ukv_docs_gather_t;

/**
 * @brief Vectorized "gather" interface, that collects, type-checks and
 * casts `N*M` values matching `M` fields from `N` docs into a @b columnar form.
 * @see `ukv_docs_gather_t`.
 */
void ukv_docs_gather(ukv_docs_gather_t*);

#ifdef __cplusplus
} /* end extern "C" */
#endif
