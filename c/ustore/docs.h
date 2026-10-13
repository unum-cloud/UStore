/**
 *  @file c/ustore/docs.h
 *  @author Ash Vardanian
 *  @date June 27, 2022
 *  @brief The binary interface for collections of JSON documents.
 *
 *  Documents extend the binary values of @c blobs.h with hierarchical contents stored as JSON text,
 *  so reading a document back through the binary interface may not return the exact bytes written.
 *
 *  A field addresses a value inside a document: a top-level key such as "_id", or an RFC 6901 JSON
 *  Pointer starting with a slash, such as "/user/followers_count" or "/posts/0/text", while an empty
 *  field addresses the whole document, as the empty JSON Pointer does. Reads and writes take one field
 *  per task, while a gather reads every field of every document into columns, converting each value
 *  into the requested type on a best-effort basis.
 */

#ifndef UNUM_USTORE_DOCS_H
#define UNUM_USTORE_DOCS_H

#include "ustore/db.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 *  @brief Type of a document, or of a value extracted from one.
 *
 *  @c ustore_docs_field_type_json_k is JSON text, @c ustore_docs_field_type_str_k is a value printed as text, and
 *  @c ustore_docs_field_type_bin_k is the same text without a null terminator; the rest are fixed-width scalars.
 */
typedef enum ustore_docs_field_type_t {
    ustore_docs_field_type_json_k = 0,

    ustore_docs_field_type_null_k = 10,
    ustore_docs_field_type_bool_k = 11,
    ustore_docs_field_type_uuid_k = 12,

    ustore_docs_field_type_f64_k = 42,
    ustore_docs_field_type_f32_k = 41,
    ustore_docs_field_type_f16_k = 40,

    ustore_docs_field_type_i64_k = 23,
    ustore_docs_field_type_i32_k = 22,
    ustore_docs_field_type_i16_k = 21,
    ustore_docs_field_type_i8_k = 20,

    ustore_docs_field_type_u64_k = 33,
    ustore_docs_field_type_u32_k = 32,
    ustore_docs_field_type_u16_k = 31,
    ustore_docs_field_type_u8_k = 30,

    ustore_docs_field_type_bin_k = 50,
    ustore_docs_field_type_str_k = 51,
} ustore_docs_field_type_t;

/** Modification @c ustore_docs_write applies: an upsert, update or insert, an RFC 6902 patch, or an
 *  RFC 7386 merge. */
typedef enum ustore_docs_modification_t {
    ustore_docs_modify_upsert_k = 0,
    ustore_docs_modify_update_k = 1,
    ustore_docs_modify_insert_k = 2,
    ustore_docs_modify_patch_k = 3,
    ustore_docs_modify_merge_k = 4,
} ustore_docs_modification_t;

/** The canonical spelling of a field type, as @c ustore_docs_field_type_f64_k spells "f64", or "unrecognized". */
USTORE_EXPORT char const* ustore_docs_field_type_name(ustore_docs_field_type_t field_type);

/** Reads a field type back from its canonical spelling; @c ustore_docs_field_type_json_k when unrecognized. */
USTORE_EXPORT ustore_docs_field_type_t ustore_docs_field_type_from_name(ustore_bytes_view_t name);

/** The canonical spelling of a modification, as @c ustore_docs_modify_merge_k spells "merge", or "unrecognized". */
USTORE_EXPORT char const* ustore_docs_modification_name(ustore_docs_modification_t modification);

/** Reads a modification back from its canonical spelling; @c ustore_docs_modify_upsert_k when unrecognized. */
USTORE_EXPORT ustore_docs_modification_t ustore_docs_modification_from_name(ustore_bytes_view_t name);

/**
 *  @brief The arguments of @c ustore_docs_write, which writes whole documents or fields inside them.
 *
 *  Without @c keys, the key of every document is read from its @c id_field, which must hold a non-negative
 *  integer, spelled as the key of that integer, or a UUID in canonical text. Whole documents are removed with
 *  @c ustore_blobs_remove, and fields inside them with a patch or a merge.
 */
typedef struct ustore_docs_write_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** Transaction the writes join, where @c NULL writes to the head. */
    ustore_transaction_t transaction;

    /** Number of documents or fields to write. */
    ustore_size_t count;

    /** Collections of the documents, where @c NULL data stands for the main collection. */
    ustore_collections_view_t collections;

    /** Keys of the documents, where @c NULL data reads every key from @c id_field. */
    ustore_keys_view_t keys;

    /** Field holding the key of every document when @c keys has no data, such as "_id". */
    ustore_bytes_view_t id_field;

    /** Fields to modify inside the documents, one per task, where a zeroed view modifies whole documents. */
    ustore_strings_view_t fields;

    /** Values to write, one per task. */
    ustore_strings_view_t values;

    /** Format of the written values. */
    ustore_docs_field_type_t type;

    /** Modification applied to every document. */
    ustore_docs_modification_t modification;

    /** How durable the writes are once the call finishes at the head; a transaction's commit decides its own. */
    ustore_durability_t durability;

    /** Whether the @c transaction records the documents it reads and writes for conflicts. */
    ustore_watch_t watch;

} ustore_docs_write_t;

/**
 *  @brief Writes whole documents or fields inside them.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_docs_write(ustore_docs_write_t* arguments);

/**
 *  @brief The arguments of @c ustore_docs_read, which reads whole documents or fields inside them.
 *
 *  Whole documents read as JSON pass straight through to the engine; everything else is parsed and
 *  exported, printing scalars as text for @c ustore_docs_field_type_str_k and nested values as JSON text.
 */
typedef struct ustore_docs_read_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** What the call sees, where @c NULL is the head. */
    ustore_view_t view;

    /** Number of documents or fields to read. */
    ustore_size_t count;

    /** Collections of the documents, where @c NULL data stands for the main collection. */
    ustore_collections_view_t collections;

    /** Keys of the documents. */
    ustore_keys_view_t keys;

    /** Fields to read inside the documents, one per task, where a zeroed view reads whole documents. */
    ustore_strings_view_t fields;

    /** Optional output for a bitset of @c count bits marking found values, inside @c arena. */
    ustore_octet_t const** presences;

    /** Optional output for the length of every value, for reads that skip the bytes, inside @c arena. */
    ustore_length_t const** lengths;

    /** Optional output for the values as a tape inside @c arena, where a missing value reads as an empty one. */
    ustore_strings_view_t* values;

    /** Rewound by this call, then holding every output it writes; @c NULL is refused. */
    ustore_arena_t arena;

    /** Format the values are converted into on export, either JSON or a string. */
    ustore_docs_field_type_t outputs_type;

} ustore_docs_read_t;

/**
 *  @brief Reads whole documents or fields inside them.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_docs_read(ustore_docs_read_t* arguments);

/** The arguments of @c ustore_docs_gist, which lists the unique paths to the leaves of the requested
 *  documents. */
typedef struct ustore_docs_gist_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** What the call sees, where @c NULL is the head. */
    ustore_view_t view;

    /** Number of documents to inspect. */
    ustore_size_t count;

    /** Collections of the documents, where @c NULL data stands for the main collection. */
    ustore_collections_view_t collections;

    /** Keys of the documents. */
    ustore_keys_view_t keys;

    /** Output for the @c written unique JSON Pointers as a tape inside @c arena. */
    ustore_strings_view_t* fields;

    /** Output for the unique paths written; zeroed on failure. */
    ustore_size_t* written;

    /** Rewound by this call, then holding every output it writes; @c NULL is refused. */
    ustore_arena_t arena;

} ustore_docs_gist_t;

/**
 *  @brief Lists the unique paths to the leaves of the requested documents.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_docs_gist(ustore_docs_gist_t* arguments);

/**
 *  @brief One column of @c ustore_docs_gather: @c count rows of one field, as an Apache Arrow array of its type.
 *
 *  Validity, conversion and collision columns are bitmaps, least significant bit first. A value is valid when it was
 *  found and converted, converted when the conversion changed its semantics, such as a string parsed into an integer,
 *  and colliding when a value of an inconvertible type sits under the field.
 */
typedef struct ustore_docs_column_view_t {

    /** Bits marking the rows whose value was found and converted. */
    ustore_octet_t const* validities;

    /** Bits marking the rows whose conversion changed the value's semantics. */
    ustore_octet_t const* conversions;

    /** Bits marking the rows where a value of an inconvertible type sits under the field. */
    ustore_octet_t const* collisions;

    /** The scalars, each as wide as the column's type and aligned to it, or @c NULL for a string column. */
    ustore_byte_t const* scalars;

    /** The @c count strings of a string column as a tape, or a zeroed view for a scalar column. */
    ustore_strings_view_t strings;

} ustore_docs_column_view_t;

/**
 *  @brief The arguments of @c ustore_docs_gather, which reads @c fields_count fields of
 *      @c count documents into columns.
 *
 *  Every field lands in one @c ustore_docs_column_view_t of @c count rows inside @c arena, converted into its entry
 *  of @c outputs_types. Strings requested as @c ustore_docs_field_type_str_k end with a null terminator, which their
 *  offsets span, and binary strings do not.
 */
typedef struct ustore_docs_gather_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** What the call sees, where @c NULL is the head. */
    ustore_view_t view;

    /** Number of documents, the rows of the output. */
    ustore_size_t count;

    /** Collections of the documents, where @c NULL data stands for the main collection. */
    ustore_collections_view_t collections;

    /** Keys of the documents. */
    ustore_keys_view_t keys;

    /** Number of fields, the columns of the output. */
    ustore_size_t fields_count;

    /** Fields to gather from every document, @c fields_count of them. */
    ustore_strings_view_t fields;

    /** Output for @c fields_count columns of @c count rows, one per field, inside @c arena. */
    ustore_docs_column_view_t const** columns;

    /** Rewound by this call, then holding every output it writes; @c NULL is refused. */
    ustore_arena_t arena;

    /** Types the columns are converted into, @c fields_count of them, one per field. */
    ustore_docs_field_type_t const* outputs_types;

} ustore_docs_gather_t;

/**
 *  @brief Reads, type-checks and converts @c fields_count fields of @c count documents into columns.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_docs_gather(ustore_docs_gather_t* arguments);

#ifdef __cplusplus
}
#endif

#endif // UNUM_USTORE_DOCS_H
