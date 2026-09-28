/**
 *  @file include/ustore/docs.h
 *  @author Ash Vardanian
 *  @date June 27, 2022
 *  @brief The binary interface for collections of JSON documents.
 *
 *  Documents extend the binary values of @c blobs.h with hierarchical contents stored as JSON text,
 *  so reading a document back through the binary interface may not return the exact bytes written.
 *
 *  A field addresses a value inside a document: a top-level key such as "_id", or an RFC 6901 JSON
 *  Pointer starting with a slash, such as "/user/followers_count" or "/posts/0/text". Reads, writes
 *  and gists take one field per task, while a gather reads every field of every document into
 *  columns, converting each value into the requested type on a best-effort basis.
 */

#pragma once

#include <ustore/db.h>

#ifdef __cplusplus
extern "C" {
#endif

#pragma region Types

/**
 *  @brief Type of a document, or of a value extracted from one.
 *
 *  @c ustore_doc_field_json_k is JSON text, @c ustore_doc_field_str_k is a value printed as text, and
 *  @c ustore_doc_field_bin_k is the same text without a null terminator; the rest are fixed-width scalars.
 */
typedef enum ustore_doc_field_type_t {
    ustore_doc_field_json_k = 0,

    ustore_doc_field_null_k = 10,
    ustore_doc_field_bool_k = 11,
    ustore_doc_field_uuid_k = 12,

    ustore_doc_field_f64_k = 42,
    ustore_doc_field_f32_k = 41,
    ustore_doc_field_f16_k = 40,

    ustore_doc_field_i64_k = 23,
    ustore_doc_field_i32_k = 22,
    ustore_doc_field_i16_k = 21,
    ustore_doc_field_i8_k = 20,

    ustore_doc_field_u64_k = 33,
    ustore_doc_field_u32_k = 32,
    ustore_doc_field_u16_k = 31,
    ustore_doc_field_u8_k = 30,

    ustore_doc_field_bin_k = 50,
    ustore_doc_field_str_k = 51,
} ustore_doc_field_type_t;

/** Modification @c ustore_docs_write applies: an upsert, update or insert, an RFC 6902 patch, or an
 *  RFC 7386 merge. */
typedef enum ustore_doc_modification_t {
    ustore_doc_modify_upsert_k = 0,
    ustore_doc_modify_update_k = 1,
    ustore_doc_modify_insert_k = 2,
    ustore_doc_modify_patch_k = 3,
    ustore_doc_modify_merge_k = 4,
} ustore_doc_modification_t;

#pragma endregion Types

#pragma region Primary Functions

/**
 *  @brief The arguments of @c ustore_docs_write, which writes whole documents or fields inside them.
 *
 *  Without @c keys, the key of every document is read from its @c id_field, which must hold a non-negative
 *  integer, spelled as the key of that integer, or a UUID in canonical text.
 */
typedef struct ustore_docs_write_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Transaction the write belongs to, or @c NULL. */
    ustore_transaction_t transaction;

    /** Reusable memory for intermediate results. */
    ustore_arena_t* arena;

    /** Write options, or read and write options for modifications that read the document first. */
    ustore_options_t options;

    /** Number of documents or fields to write. */
    ustore_size_t tasks_count;

    /** Format of the written values. */
    ustore_doc_field_type_t type;

    /** Modification applied to every document. */
    ustore_doc_modification_t modification;

    /** Collections of the documents, where @c NULL stands for the main collection. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Keys of the documents, or @c NULL to read them from @c id_field. */
    ustore_key_t const* keys;

    /** Bytes between consecutive keys, where zero repeats the first one. */
    ustore_size_t keys_stride;

    /** Fields to modify inside the documents, or @c NULL for whole documents. */
    ustore_str_view_t const* fields;

    /** Bytes between consecutive fields, where zero repeats the first one. */
    ustore_size_t fields_stride;

    /** Bitmap of present values, or @c NULL when every value is present. */
    ustore_octet_t const* presences;

    /** Offsets of the values inside their buffers, or @c NULL. */
    ustore_length_t const* offsets;

    /** Bytes between consecutive offsets, where zero repeats the first one. */
    ustore_size_t offsets_stride;

    /** Lengths of the values, or @c NULL to derive them from @c offsets or null terminators. */
    ustore_length_t const* lengths;

    /** Bytes between consecutive lengths, where zero repeats the first one. */
    ustore_size_t lengths_stride;

    /** Buffers holding the values. */
    ustore_bytes_cptr_t const* values;

    /** Bytes between consecutive buffer pointers, where zero repeats the first one. */
    ustore_size_t values_stride;

    /** Field holding the key of every document when @c keys is @c NULL, such as "_id". */
    ustore_str_view_t id_field;

} ustore_docs_write_t;

/**
 *  @brief Writes whole documents or fields inside them.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_docs_write(ustore_docs_write_t* arguments);

/**
 *  @brief The arguments of @c ustore_docs_read, which reads whole documents or fields inside them.
 *
 *  Whole documents read as JSON pass straight through to the engine; everything else is parsed and
 *  exported, printing scalars as text for @c ustore_doc_field_str_k and nested values as JSON text.
 */
typedef struct ustore_docs_read_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Transaction that watches the read documents, or @c NULL. */
    ustore_transaction_t transaction;

    /** Optional snapshot whose view is read, never set beside a transaction. */
    ustore_snapshot_t snapshot;

    /** Reusable memory for the results. */
    ustore_arena_t* arena;

    /** Read options. */
    ustore_options_t options;

    /** Format to export the values in, either JSON or a string. */
    ustore_doc_field_type_t type;

    /** Number of documents or fields to read. */
    ustore_size_t tasks_count;

    /** Collections of the documents, where @c NULL stands for the main collection. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Keys of the documents. */
    ustore_key_t const* keys;

    /** Bytes between consecutive keys, where zero repeats the first one. */
    ustore_size_t keys_stride;

    /** Fields to read inside the documents, or @c NULL for whole documents. */
    ustore_str_view_t const* fields;

    /** Bytes between consecutive fields, where zero repeats the first one. */
    ustore_size_t fields_stride;

    /** Optional output for a bitmap of found values. */
    ustore_octet_t** presences;

    /** Optional output for the offsets of the values inside @c values. */
    ustore_length_t** offsets;

    /** Optional output for the lengths of the values. */
    ustore_length_t** lengths;

    /** Optional output for the joined contents of the values. */
    ustore_bytes_ptr_t* values;

} ustore_docs_read_t;

/**
 *  @brief Reads whole documents or fields inside them.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_docs_read(ustore_docs_read_t* arguments);

/** The arguments of @c ustore_docs_gist, which lists the unique paths to the leaves of the requested
 *  documents. */
typedef struct ustore_docs_gist_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Transaction that watches the read documents, or @c NULL. */
    ustore_transaction_t transaction;

    /** Optional snapshot whose view is read, never set beside a transaction. */
    ustore_snapshot_t snapshot;

    /** Reusable memory for the results. */
    ustore_arena_t* arena;

    /** Read options. */
    ustore_options_t options;

    /** Number of documents to inspect. */
    ustore_size_t docs_count;

    /** Collections of the documents, where @c NULL stands for the main collection. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Keys of the documents. */
    ustore_key_t const* keys;

    /** Bytes between consecutive keys, where zero repeats the first one. */
    ustore_size_t keys_stride;

    /** Output for the number of unique paths. */
    ustore_size_t* fields_count;

    /** Output for the offsets of the paths inside @c fields. */
    ustore_length_t** offsets;

    /** Output for the joined null-terminated JSON Pointers. */
    ustore_char_t** fields;

} ustore_docs_gist_t;

/**
 *  @brief Lists the unique paths to the leaves of the requested documents.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_docs_gist(ustore_docs_gist_t* arguments);

/**
 *  @brief The arguments of @c ustore_docs_gather, which reads @c fields_count fields of
 *      @c docs_count documents into columns.
 *
 *  Every output points to an array of @c fields_count columns, each holding @c docs_count entries.
 *  Validity, conversion and collision columns are bitmaps, least significant bit first. A value is
 *  valid when it was found and converted, converted when the conversion changed its semantics, such
 *  as a string parsed into an integer, and colliding when a value of an inconvertible type sits under
 *  the field. Strings requested as @c ustore_doc_field_str_k end with a null terminator, and binary
 *  strings do not; their offsets and lengths are laid out column by column, their contents row by row.
 */
typedef struct ustore_docs_gather_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Transaction that watches the read documents, or @c NULL. */
    ustore_transaction_t transaction;

    /** Optional snapshot whose view is read, never set beside a transaction. */
    ustore_snapshot_t snapshot;

    /** Reusable memory for the results. */
    ustore_arena_t* arena;

    /** Read options. */
    ustore_options_t options;

    /** Number of documents, the rows of the output. */
    ustore_size_t docs_count;

    /** Number of fields, the columns of the output. */
    ustore_size_t fields_count;

    /** Collections of the documents, where @c NULL stands for the main collection. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Keys of the documents. */
    ustore_key_t const* keys;

    /** Bytes between consecutive keys, where zero repeats the first one. */
    ustore_size_t keys_stride;

    /** Fields to gather from every document. */
    ustore_str_view_t const* fields;

    /** Bytes between consecutive fields, where zero repeats the first one. */
    ustore_size_t fields_stride;

    /** Types to convert every field into. */
    ustore_doc_field_type_t const* types;

    /** Bytes between consecutive types, where zero repeats the first one. */
    ustore_size_t types_stride;

    /** Optional output for the validity bitmaps. */
    ustore_octet_t*** columns_validities;

    /** Optional output for the conversion bitmaps. */
    ustore_octet_t*** columns_conversions;

    /** Optional output for the collision bitmaps. */
    ustore_octet_t*** columns_collisions;

    /** Output for the scalar columns, with @c NULL for string columns. */
    ustore_byte_t*** columns_scalars;

    /** Output for the offsets of strings inside @c joined_strings, with @c NULL for scalar columns. */
    ustore_length_t*** columns_offsets;

    /** Output for the lengths of strings, with @c NULL for scalar columns. */
    ustore_length_t*** columns_lengths;

    /** Output for the contents of all strings, joined row by row. */
    ustore_byte_t** joined_strings;

} ustore_docs_gather_t;

/**
 *  @brief Reads, type-checks and converts @c fields_count fields of @c docs_count documents into columns.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_docs_gather(ustore_docs_gather_t* arguments);

#pragma endregion Primary Functions

#ifdef __cplusplus
}
#endif
