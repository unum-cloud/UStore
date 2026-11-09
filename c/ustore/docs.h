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
 *  Every type but JSON text, strings and binary text is a fixed-width scalar.
 */
typedef enum ustore_docs_field_type_t {

    /** JSON text. */
    ustore_docs_field_type_json_k = 0,

    /** The JSON null. */
    ustore_docs_field_type_null_k = 10,

    /** A boolean. */
    ustore_docs_field_type_bool_k = 11,

    /** A 128-bit UUID. */
    ustore_docs_field_type_uuid_k = 12,

    /** A 64-bit float. */
    ustore_docs_field_type_f64_k = 42,

    /** A 32-bit float. */
    ustore_docs_field_type_f32_k = 41,

    /** A 16-bit float. */
    ustore_docs_field_type_f16_k = 40,

    /** A signed 64-bit integer. */
    ustore_docs_field_type_i64_k = 23,

    /** A signed 32-bit integer. */
    ustore_docs_field_type_i32_k = 22,

    /** A signed 16-bit integer. */
    ustore_docs_field_type_i16_k = 21,

    /** A signed 8-bit integer. */
    ustore_docs_field_type_i8_k = 20,

    /** An unsigned 64-bit integer. */
    ustore_docs_field_type_u64_k = 33,

    /** An unsigned 32-bit integer. */
    ustore_docs_field_type_u32_k = 32,

    /** An unsigned 16-bit integer. */
    ustore_docs_field_type_u16_k = 31,

    /** An unsigned 8-bit integer. */
    ustore_docs_field_type_u8_k = 30,

    /** The text of a value without a null terminator. */
    ustore_docs_field_type_binary_k = 50,

    /** A value printed as text. */
    ustore_docs_field_type_string_k = 51,
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

/** Reads a field type back from its canonical spelling, or fails with
 *  @c ustore_status_invalid_argument_k. */
USTORE_EXPORT ustore_status_t ustore_docs_field_type_from_name(ustore_bytes_view_t name,
                                                               ustore_docs_field_type_t* field_type);

/** The canonical spelling of a modification, as @c ustore_docs_modify_merge_k spells "merge", or "unrecognized". */
USTORE_EXPORT char const* ustore_docs_modification_name(ustore_docs_modification_t modification);

/** Reads a modification back from its canonical spelling, or fails with
 *  @c ustore_status_invalid_argument_k. */
USTORE_EXPORT ustore_status_t ustore_docs_modification_from_name(ustore_bytes_view_t name,
                                                                 ustore_docs_modification_t* modification);

/**
 *  @brief The arguments of @c ustore_docs_write, which writes whole documents or fields inside them.
 *
 *  Without @c keys, the key of every document is read from its @c id_field, which must hold a
 *  non-negative integer, spelled as the key of that integer, or a UUID in canonical text.
 *  Whole documents are removed with @c ustore_docs_remove, and fields inside them with a patch
 *  or a merge.
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

    /** Collections of the documents. */
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
 *
 *  @par Statuses:
 *  - @c ustore_status_contended_k The @c transaction serves another call, or the executor serves
 *    another call.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL.
 *  - @c ustore_status_read_only_k The database was opened read-only.
 *
 *  @c ustore_status_invalid_argument_k means the @c collections or @c values are missing, so are
 *  both the @c keys and the @c id_field, the fields or values come in an unknown layout, the
 *  @c transaction belongs to another engine or is staged, the @c durability, @c watch or
 *  @c modification is unknown, a flushed write names a @c transaction, @c ustore_watch_none_k comes
 *  without one, a collection holds another modality, a patch or a modifier does not apply to its
 *  field, or a field or value is past 4 GiB.
 *
 *  - @c ustore_status_unsupported_k The executor computes on a device, or the @c type is not
 *    supported.
 *  - @c ustore_status_not_found_k A collection does not exist or was dropped meanwhile, a field
 *    path does not resolve, or the document a field modification, an update or a patch needs is
 *    missing.
 *  - @c ustore_status_already_exists_k An inserted document already exists.
 *  - @c ustore_status_malformed_k A document or a value does not parse, is empty or holds NaN or
 *    Infinity, an id field is neither an unsigned integer nor a UUID, or a document outgrows what
 *    its NVMe collection admits.
 *  - @c ustore_status_write_conflict_k Concurrent commits kept conflicting, or a staged RocksDB
 *    transaction holds a key.
 *  - @c ustore_status_capacity_exhausted_k The documents outgrow the NVMe transaction's buffer.
 */
USTORE_EXPORT void ustore_docs_write(ustore_docs_write_t* arguments);

/**
 *  @brief The arguments of @c ustore_docs_read, which reads whole documents or fields inside them.
 *
 *  Whole documents read as JSON pass straight through to the engine; everything else is parsed
 *  and exported, printing scalars as text for @c ustore_docs_field_type_string_k and nested
 *  values as JSON text.
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

    /** Collections of the documents. */
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
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c arena, @c collections or @c keys are missing, the
 *    fields come in an unknown layout, the @c view is none of the engine's, a watching read goes
 *    through a staged transaction, or a collection holds another modality.
 *  - @c ustore_status_device_k The arena lives where another executor computes, or the device
 *    refused a copy or a launch.
 *  - @c ustore_status_contended_k The arena or the transaction the @c view reads through serves
 *    another call, the executor serves another call, or an NVMe value keeps changing size across
 *    reads.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL or an NVMe transfer was cancelled.
 *  - @c ustore_status_unsupported_k Fields are read as neither JSON nor strings.
 *  - @c ustore_status_not_found_k A collection does not exist or was dropped meanwhile.
 *  - @c ustore_status_malformed_k A document does not parse.
 *  - @c ustore_status_capacity_exhausted_k Every shared NVMe channel is taken.
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

    /** Collections of the documents. */
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
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c arena, @c collections or @c keys are missing,
 *    the @c view was made by neither view constructor or belongs to another engine, a watching
 *    read goes through a staged transaction, or a collection holds another modality.
 *  - @c ustore_status_device_k The arena lives where another executor computes.
 *  - @c ustore_status_contended_k The arena or the transaction the @c view reads through serves
 *    another call, the executor serves another call, or an NVMe value keeps changing size across
 *    reads.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL or an NVMe transfer was cancelled.
 *  - @c ustore_status_unsupported_k The executor computes on a device.
 *  - @c ustore_status_not_found_k A collection does not exist or was dropped meanwhile.
 *  - @c ustore_status_malformed_k A document does not parse.
 *  - @c ustore_status_capacity_exhausted_k A path is 512 bytes or longer, or every shared NVMe
 *    channel is taken.
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
 *  @brief The arguments of @c ustore_docs_gather, which reads @c fields_count fields of @c count
 *      documents into columns.
 *
 *  Every field lands in one @c ustore_docs_column_view_t of @c count rows
 *  inside @c arena, converted into its entry of @c outputs_types. Strings
 *  requested as @c ustore_docs_field_type_string_k end with a null terminator,
 *  which their offsets span, and binary strings do not.
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

    /** Collections of the documents. */
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
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c arena, @c collections, @c keys or @c columns are
 *    missing, the @c fields or their types are missing or unknown, the @c view is foreign, a
 *    watching read goes through a staged transaction, or a collection holds another modality.
 *  - @c ustore_status_device_k The arena lives where another executor computes, or the device
 *    refused a copy or a launch.
 *  - @c ustore_status_contended_k The arena or the transaction the @c view reads through serves
 *    another call, the executor serves another call, or an NVMe value keeps changing size across
 *    reads.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL or an NVMe transfer was cancelled.
 *  - @c ustore_status_not_found_k A collection does not exist or was dropped meanwhile.
 *  - @c ustore_status_malformed_k A document does not parse.
 *  - @c ustore_status_capacity_exhausted_k Every shared NVMe channel is taken.
 */
USTORE_EXPORT void ustore_docs_gather(ustore_docs_gather_t* arguments);

/** The arguments of @c ustore_docs_remove, which removes whole documents in a batch. */
typedef struct ustore_docs_remove_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** Transaction the removals join, where @c NULL removes at the head. */
    ustore_transaction_t transaction;

    /** Number of tasks, one per document. */
    ustore_size_t count;

    /** Collections of the documents. */
    ustore_collections_view_t collections;

    /** Keys of the documents to remove, or inclusive starts of ranges when @c end_keys is given. */
    ustore_keys_view_t keys;

    /** Exclusive ends of the ranges to remove, or a zeroed view to remove @c keys alone. */
    ustore_keys_view_t end_keys;

    /** How durable the removals are once the call finishes at the head; a transaction's commit decides its own. */
    ustore_durability_t durability;

    /** Whether the @c transaction records the removed keys for conflicts. */
    ustore_watch_t watch;

} ustore_docs_remove_t;

/**
 *  @brief Removes a batch of whole documents.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_contended_k The @c transaction serves another call, or the executor serves
 *    another call.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL.
 *  - @c ustore_status_read_only_k The database was opened read-only.
 *
 *  @c ustore_status_invalid_argument_k means the @c collections or @c keys are missing, the
 *  @c transaction belongs to another engine or is staged, the @c durability or @c watch is
 *  unknown, a flushed remove names a @c transaction, @c ustore_watch_none_k comes without one, or
 *  a collection holds another modality.
 *
 *  - @c ustore_status_unsupported_k The executor computes on a device.
 *  - @c ustore_status_not_found_k A collection does not exist or was dropped meanwhile.
 *  - @c ustore_status_write_conflict_k Concurrent commits kept conflicting, or a staged RocksDB
 *    transaction holds a key.
 *  - @c ustore_status_malformed_k A key falls outside what its NVMe collection admits.
 *  - @c ustore_status_capacity_exhausted_k The removes outgrow the NVMe transaction's buffer.
 */
USTORE_EXPORT void ustore_docs_remove(ustore_docs_remove_t* arguments);

/**
 *  @brief The arguments of @c ustore_docs_scan, which lists up to @c wanted document keys from a start key in every
 *      collection.
 *
 *  Documents are not exported, so a read or a gather follows the scan. Consistent iterators over concurrent state
 *  are costly, so scans paginate instead.
 */
typedef struct ustore_docs_scan_t {

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

    /** Number of scans. */
    ustore_size_t count;

    /** Collections to scan. */
    ustore_collections_view_t collections;

    /** Inclusive start of every scan, @c NULL data starting at the first key in @c direction. */
    ustore_keys_view_t start_keys;

    /** Key every scan stops before, exclusive, or a zeroed view for no bound. */
    ustore_keys_view_t end_keys;

    /** Ascending, the default, or descending from the start key. */
    ustore_scan_direction_t direction;

    /** Optional JSONPath, or a bare root filter such as @c ?@.age>30, that every listed document matches, shared by
     *  every scan; a zeroed view lists every key. */
    ustore_bytes_view_t filter;

    /** Optional verdict on the listed keys, asked on the host once per page as its @c cost says. */
    ustore_predicate_t predicate;

    /** Output for @c count plus one offsets into @c keys, the last one closing the final scan, inside @c arena. */
    ustore_length_t const** offsets;

    /** Output for the found keys, scan after scan, inside @c arena. */
    ustore_key_t const** keys;

    /** Rewound by this call, then holding every output it writes; @c NULL is refused. */
    ustore_arena_t arena;

    /** Most keys every scan returns. */
    ustore_size_t wanted;

    /** In what order the keys come. */
    ustore_scan_mode_t mode;

    /** Optional callback reporting the scans finished. */
    ustore_progress_callback_t progress;

    /** Opaque pointer passed to @c progress. */
    void* progress_state;

} ustore_docs_scan_t;

/**
 *  @brief Scans a batch of document key ranges.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  @c ustore_status_invalid_argument_k means the @c arena, @c collections or an output is missing,
 *  the @c mode, @c direction or a predicate cost is unknown, the @c filter does not compile, the
 *  @c view was made by neither view constructor or belongs to another engine, a watching scan goes
 *  through a staged transaction, or a collection holds another modality.
 *
 *  - @c ustore_status_device_k The arena lives where another executor computes.
 *  - @c ustore_status_contended_k The arena or the transaction the @c view reads through serves
 *    another call, the executor serves another call, or an NVMe value keeps changing size across
 *    reads.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL or an NVMe transfer was cancelled.
 *  - @c ustore_status_unsupported_k The executor's device loaded no bundle of the library's kernels
 *    and a @c filter is given.
 *  - @c ustore_status_not_found_k A collection does not exist or was dropped meanwhile.
 *  - @c ustore_status_malformed_k A document the @c filter reads does not parse.
 *  - @c ustore_status_capacity_exhausted_k Every shared NVMe channel is taken.
 */
USTORE_EXPORT void ustore_docs_scan(ustore_docs_scan_t* arguments);

/**
 *  @brief An opaque handle to a walk over the document keys of one collection, which every pull resumes past the
 *      last key the pull before it handed out, as @c ustore_blobs_cursor_t walks.
 *
 *  It holds its database open, and the transaction or snapshot it reads through, until @c ustore_docs_cursor_free, and
 *  serves one pull at a time, so a second pull naming it finishes @c ustore_status_contended_k.
 */
typedef struct ustore_docs_cursor_handle_t* ustore_docs_cursor_t;

/** The arguments of @c ustore_docs_cursor_init. */
typedef struct ustore_docs_cursor_init_t {

    /** Output for the new cursor, which the caller frees with @c ustore_docs_cursor_free; @c NULL on failure. */
    ustore_docs_cursor_t* cursor;

    /** An open database. */
    ustore_database_t database;

    /** Executor of the kind every pull runs on, which picks the side the walk computes on, where @c NULL walks on host
     *  executors; a pull on another kind of executor reports @c ustore_status_device_k. */
    ustore_executor_t executor;

    /** Output for the outcome, final on return. */
    ustore_outcome_t* outcome;

    /** What the walk sees, where @c NULL is the head, read pull by pull. */
    ustore_view_t view;

    /** Docs collection to walk. */
    ustore_collection_id_t collection;

    /** Key the walk starts from, inclusive. */
    ustore_key_t start_key;

    /** Key the walk stops before, exclusive, or @c NULL for no bound. */
    ustore_key_t const* end_key;

    /** Ascending, the default, or descending from the start key. */
    ustore_scan_direction_t direction;

    /** Optional JSONPath or bare root filter every document the walk hands out matches, copied and compiled here
     *  once; a zeroed view walks every key. */
    ustore_bytes_view_t filter;

    /** Optional verdict on the keys of every page, asked as its @c cost says; host executors only. */
    ustore_predicate_t predicate;

} ustore_docs_cursor_init_t;

/**
 *  @brief Opens a walk over the document keys of one collection, compiling its filter once and reading nothing
 *      until its first pull.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c cursor output is @c NULL, the @c view was made
 *    by neither view constructor or reads another database, a predicate cost is unknown, or
 *    the @c filter does not compile.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL.
 */
USTORE_EXPORT void ustore_docs_cursor_init(ustore_docs_cursor_init_t* arguments);

/** The arguments of @c ustore_docs_cursor_next, which pulls the next page of a walk, with the documents when asked,
 *  in one call. */
typedef struct ustore_docs_cursor_next_t {

    /** The database the cursor was opened on. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** The cursor to advance, which a pull that fails or is cancelled leaves where it was. */
    ustore_docs_cursor_t cursor;

    /** Output for the keys of the page in their order, inside @c arena. */
    ustore_key_t const** keys;

    /** Optional output for the whole documents as a tape inside @c arena, where a key removed at the head between the
     *  listing and the read of one pull reads as an empty document; @c NULL skips them. */
    ustore_strings_view_t* values;

    /** Output for the keys written, zero once the walk is exhausted; zeroed on failure. */
    ustore_size_t* written;

    /** Rewound by this call, then holding every output it writes; @c NULL is refused. */
    ustore_arena_t arena;

    /** Most keys the page holds, where a page of fewer exhausts the walk. */
    ustore_size_t wanted;

    /** Optional callback reporting the one pull, which may cancel it before the cursor advances. */
    ustore_progress_callback_t progress;

    /** Opaque pointer passed to @c progress. */
    void* progress_state;

} ustore_docs_cursor_next_t;

/**
 *  @brief Pulls the next page of a walk, and the documents of its keys when asked.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c cursor, the @c arena or the @c written output
 *    is @c NULL, the cursor walks another database, a watching pull goes through a staged
 *    transaction, or the collection holds another modality.
 *  - @c ustore_status_device_k The arena lives where another executor computes, a cursor opened
 *    for a device is pulled on host threads, or a host cursor is pulled on a device.
 *  - @c ustore_status_contended_k The arena, the cursor or the transaction it reads through serves
 *    another call, the executor serves another call, or an NVMe value keeps changing size across
 *    reads.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL or an NVMe transfer was cancelled.
 *  - @c ustore_status_unsupported_k The pulling executor's device loaded no bundle of the library's
 *    kernels and the walk has a @c filter.
 *  - @c ustore_status_not_found_k The collection does not exist or was dropped meanwhile.
 *  - @c ustore_status_malformed_k A document the filter reads does not parse.
 *  - @c ustore_status_capacity_exhausted_k Every shared NVMe channel is taken.
 */
USTORE_EXPORT void ustore_docs_cursor_next(ustore_docs_cursor_next_t* arguments);

/**
 *  @brief Waits until every deferred call naming the cursor finishes, then frees it, giving back the holds it kept on
 *      its database and on the transaction or snapshot it reads through.
 *  @param[in,out] cursor The cursor to be freed; @c NULL is safe.
 */
USTORE_EXPORT void ustore_docs_cursor_free(ustore_docs_cursor_t cursor);

/** The arguments of @c ustore_docs_sample, which draws up to @c wanted document keys uniformly at random from every
 *  collection, exporting no documents, so a read or a gather follows the sample. */
typedef struct ustore_docs_sample_t {

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

    /** Number of samples. */
    ustore_size_t count;

    /** Collections to sample. */
    ustore_collections_view_t collections;

    /** Output for @c count plus one offsets into @c keys, the last one closing the final sample, inside @c arena. */
    ustore_length_t const** offsets;

    /** Output for the sampled keys, sample after sample, inside @c arena. */
    ustore_key_t const** keys;

    /** Rewound by this call, then holding every output it writes; @c NULL is refused. */
    ustore_arena_t arena;

    /** Most keys every sample draws. */
    ustore_size_t wanted;

} ustore_docs_sample_t;

/**
 *  @brief Samples document keys from a batch of collections.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c arena, @c collections or an output is missing,
 *    the @c view was made by neither view constructor or belongs to another engine, a watching
 *    sample goes through a staged transaction, or a collection holds another modality.
 *  - @c ustore_status_device_k The arena lives where another executor computes.
 *  - @c ustore_status_contended_k The arena or the transaction the @c view reads through serves
 *    another call, the executor serves another call, or an NVMe value keeps changing size across
 *    reads.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL or an NVMe transfer was cancelled.
 *  - @c ustore_status_not_found_k A collection does not exist or was dropped meanwhile.
 *  - @c ustore_status_capacity_exhausted_k Every shared NVMe channel is taken.
 */
USTORE_EXPORT void ustore_docs_sample(ustore_docs_sample_t* arguments);

/** The arguments of @c ustore_docs_measure, which estimates the documents and the space inside key
 *  ranges, every output holding one bound per range. */
typedef struct ustore_docs_measure_t {

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

    /** Number of measured ranges. */
    ustore_size_t count;

    /** Collections to measure. */
    ustore_collections_view_t collections;

    /** Inclusive start of every range, @c NULL data starting at the first key in @c direction. */
    ustore_keys_view_t start_keys;

    /** Key every range stops before, exclusive, or a zeroed view for no bound. */
    ustore_keys_view_t end_keys;

    /** Ascending, the default, or descending from the start key. */
    ustore_scan_direction_t direction;

    /** Optional output for the lower bound on the documents in every range, inside @c arena. */
    ustore_size_t const** min_cardinalities;

    /** Optional output for the upper bound on the documents in every range, inside @c arena. */
    ustore_size_t const** max_cardinalities;

    /** Optional output for the lower bound on the value bytes in every range, inside @c arena. */
    ustore_size_t const** min_value_bytes;

    /** Optional output for the upper bound on the value bytes in every range, inside @c arena. */
    ustore_size_t const** max_value_bytes;

    /** Optional output for the lower bound on the disk space every range takes, inside @c arena. */
    ustore_size_t const** min_space_usages;

    /** Optional output for the upper bound on the disk space every range takes, inside @c arena. */
    ustore_size_t const** max_space_usages;

    /** Rewound by this call, then holding every output it writes; @c NULL is refused. */
    ustore_arena_t arena;

} ustore_docs_measure_t;

/**
 *  @brief Estimates the documents and the space inside a batch of key ranges.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_device_k The arena lives where another executor computes.
 *  - @c ustore_status_contended_k The arena or the transaction the @c view reads through serves
 *    another call, or the executor serves another call.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL or an NVMe transfer was cancelled.
 *  - @c ustore_status_unsupported_k The executor computes on a device.
 *  - @c ustore_status_not_found_k A collection does not exist or was dropped meanwhile.
 *  - @c ustore_status_capacity_exhausted_k Every shared NVMe channel is taken.
 *
 *  @c ustore_status_invalid_argument_k means the @c arena or the @c collections are missing,
 *  the @c direction is unknown, the @c view was made by neither view constructor or belongs
 *  to another engine, a watching measure goes through a staged transaction, or a collection
 *  holds another modality.
 */
USTORE_EXPORT void ustore_docs_measure(ustore_docs_measure_t* arguments);

#ifdef __cplusplus
}
#endif

#endif // UNUM_USTORE_DOCS_H
