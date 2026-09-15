/**
 *  @file include/ustore/db.h
 *  @author Ash Vardanian
 *  @date June 12, 2022
 *  @brief The binary interface every engine implements: databases, snapshots, collections,
 *      transactions, arenas and errors.
 */

#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#pragma region Handles and Constants

/**
 *  @brief An opaque handle to an open database, safe to share across threads from open until free.
 *
 *  An embedded database owns its directory, so no two processes may open the same one at once. Every
 *  database holds the nameless @c ustore_collection_main_k, which can be cleared but never dropped,
 *  next to named collections of binary values, documents or graphs.
 */
typedef void* ustore_database_t;

/** An identifier of a point-in-time view of the whole database. */
typedef uint64_t ustore_snapshot_t;

/** An opaque handle to an optimistic transaction that groups operations across collections and
 *  modalities. Not thread-safe, and freed with @c ustore_transaction_free before its database closes. */
typedef void* ustore_transaction_t;

/** An identifier of a collection, unique within its database but not stable across restarts. */
typedef uint64_t ustore_collection_t;

/** The identifier of a value within one collection. */
typedef int64_t ustore_key_t;

/** The elementary piece of a binary value. */
typedef uint8_t ustore_byte_t;

/** A single-precision floating-point number. */
typedef float ustore_float_t;

/** The elementary piece of a string, such as a collection name. */
typedef char ustore_char_t;

/** The length of a value in bytes. */
typedef uint32_t ustore_length_t;

/** A pointer-sized unsigned integer. */
typedef uint64_t ustore_size_t;

/** The smallest bitset, holding eight bits. */
typedef uint8_t ustore_octet_t;

/** A monotonically increasing identifier of the order in which transactions applied. */
typedef uint64_t ustore_sequence_number_t;

/** An owning error message, which the caller frees with @c ustore_error_free unless it is @c NULL. */
typedef char const* ustore_error_t;

/** A string the caller owns and the database never frees. */
typedef char const* ustore_str_view_t;

/** A mutable string the caller owns and the database never frees. */
typedef char* ustore_str_span_t;

/** Temporary memory for results, which the database allocates and grows and the caller frees with
 *  @c ustore_arena_free. */
typedef void* ustore_arena_t;

/** A pointer to mutable bytes. */
typedef uint8_t* ustore_bytes_ptr_t;

/** A pointer to read-only bytes. */
typedef uint8_t const* ustore_bytes_cptr_t;

/** The argument a callback receives. */
typedef void* ustore_callback_payload_t;

/** A callback over its payload. */
typedef void (*ustore_callback_t)(ustore_callback_payload_t);

/** Flags that adjust reads, writes, scans and transactions. */
typedef enum {

    /** No flags. */
    ustore_options_default_k = 0,

    /** Flushes writes to disk before returning, at a cost that varies by engine and drive. */
    ustore_option_write_flush_k = 1 << 1,

    /** Stops a transaction from watching the keys it reads or writes for conflicts. */
    ustore_option_transaction_dont_watch_k = 1 << 2,

    /** Keeps the results of earlier calls in the arena, so they may be passed back in as arguments. */
    ustore_option_dont_discard_memory_k = 1 << 4,

    /** Places results in shared memory that another process can map without copies. */
    ustore_option_read_shared_memory_k = 1 << 5,

    /** Lets a scan return keys out of order, deleted or repeated, to speed up the read after it;
     *  has no effect yet. */
    ustore_option_scan_bulk_k = 0,

} ustore_options_t;

/** What dropping a collection removes. */
typedef enum {

    /** The collection handle together with all of its contents. */
    ustore_drop_keys_vals_handle_k = 0,

    /** Every key and value, keeping the collection. */
    ustore_drop_keys_vals_k = 1,

    /** Every value, keeping the keys. */
    ustore_drop_vals_k = 2,

} ustore_drop_mode_t;

/** Capabilities an engine reports. */
typedef enum {

    /** The engine supports transactions. */
    ustore_supports_transactions_k = 1,

    /** The engine supports named collections. */
    ustore_supports_named_collections_k = 2,

    /** The engine supports snapshots. */
    ustore_supports_snapshots_k = 4,

} ustore_metadata_t;

/** The nameless collection every database holds, which @c ustore_drop_keys_vals_k and
 *  @c ustore_drop_vals_k may only clear. */
extern ustore_collection_t const ustore_collection_main_k;

/** The length reported for a missing value. */
extern ustore_length_t const ustore_length_missing_k;

/** The key that stands for no key. */
extern ustore_key_t const ustore_key_unknown_k;

#pragma endregion Handles and Constants

#pragma region Database

/** The arguments of @c ustore_database_init. */
typedef struct ustore_database_init_t {

    /** Configuration of the engine as a JSON string. */
    ustore_str_view_t config;

    /** Output for the opened database, unless @c error is set. */
    ustore_database_t* db;

    /** Output for the error message. */
    ustore_error_t* error;

} ustore_database_init_t;

/**
 *  @brief Opens an embedded database, persistent or in memory.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_database_init(ustore_database_init_t* arguments);

/** The arguments of @c ustore_get_metadata. */
typedef struct ustore_get_metadata_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Output for the capabilities of the engine. */
    ustore_metadata_t* metadata;

} ustore_get_metadata_t;

/**
 *  @brief Reports the capabilities of the engine.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_get_metadata(ustore_get_metadata_t* arguments);

/**
 *  @brief The arguments of @c ustore_database_control, a free-form tunnel to the engine.
 *
 *  Requests have no stable format and are expensive: "clear" removes all data and keeps collection
 *  names, "reset" removes both, "compact" flushes and compacts LSM trees, "info" describes the build,
 *  and "usage" estimates the sizes of collections and the use of memory and disk.
 */
typedef struct ustore_database_control_t {

    /** An open database. */
    ustore_database_t db;

    /** Reusable memory for the response. */
    ustore_arena_t* arena;

    /** Output for the error message. */
    ustore_error_t* error;

    /** The request as a null-terminated string. */
    ustore_str_view_t request;

    /** Output for the response as a null-terminated string. */
    ustore_str_view_t* response;

} ustore_database_control_t;

/**
 *  @brief Sends a free-form request to the engine.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_database_control(ustore_database_control_t* arguments);

#pragma endregion Database

#pragma region Snapshots

/** The arguments of @c ustore_snapshot_list. */
typedef struct ustore_snapshot_list_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Reusable memory for the results. */
    ustore_arena_t* arena;

    /** Accepts @c ustore_option_dont_discard_memory_k. */
    ustore_options_t options;

    /** Output for the number of snapshots. */
    ustore_size_t* count;

    /** Output for the identifiers of the snapshots. */
    ustore_snapshot_t** ids;

} ustore_snapshot_list_t;

/**
 *  @brief Lists every snapshot of the database.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_snapshot_list(ustore_snapshot_list_t* arguments);

/** The arguments of @c ustore_snapshot_create. */
typedef struct ustore_snapshot_create_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Output for the identifier of the new snapshot. */
    ustore_snapshot_t* id;

} ustore_snapshot_create_t;

/**
 *  @brief Captures a point-in-time view of the database.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_snapshot_create(ustore_snapshot_create_t* arguments);

/** The arguments of @c ustore_snapshot_export. */
typedef struct ustore_snapshot_export_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Identifier of the snapshot. */
    ustore_snapshot_t id;

    /** Directory that receives the exported database. */
    ustore_str_view_t path;

} ustore_snapshot_export_t;

/**
 *  @brief Exports a snapshot as a new database.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_snapshot_export(ustore_snapshot_export_t* arguments);

/** The arguments of @c ustore_snapshot_drop. */
typedef struct ustore_snapshot_drop_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Identifier of an existing snapshot. */
    ustore_snapshot_t id;

} ustore_snapshot_drop_t;

/**
 *  @brief Releases a snapshot.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_snapshot_drop(ustore_snapshot_drop_t* arguments);

#pragma endregion Snapshots

#pragma region Collections

/**
 *  @brief The arguments of @c ustore_collection_list, which exports identifiers and a tape of
 *      null-terminated names.
 *
 *  The main collection is never listed. Listing is the only collection operation that runs on a snapshot.
 */
typedef struct ustore_collection_list_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Transaction whose view is listed. */
    ustore_transaction_t transaction;

    /** Snapshot whose view is listed. */
    ustore_snapshot_t snapshot;

    /** Reusable memory for the results. */
    ustore_arena_t* arena;

    /** Accepts @c ustore_option_dont_discard_memory_k. */
    ustore_options_t options;

    /** Output for the number of named collections. */
    ustore_size_t* count;

    /** Output for the identifiers, in the order of @c names. */
    ustore_collection_t** ids;

    /** Output for the offsets of every name inside @c names. */
    ustore_length_t** offsets;

    /** Output for the tape of null-terminated names, in the order of @c ids. */
    ustore_char_t** names;

} ustore_collection_list_t;

/**
 *  @brief Lists the named collections of the database.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_collection_list(ustore_collection_list_t* arguments);

/** The arguments of @c ustore_collection_create, which also recreates an empty collection with a
 *  new configuration. */
typedef struct ustore_collection_create_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Unique name of the new collection. */
    ustore_str_view_t name;

    /** Optional configuration as a JSON string. */
    ustore_str_view_t config;

    /** Output for the identifier of the collection. */
    ustore_collection_t* id;

} ustore_collection_create_t;

/**
 *  @brief Creates a uniquely named collection.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_collection_create(ustore_collection_create_t* arguments);

/** The arguments of @c ustore_collection_drop, where the main collection can only be cleared. */
typedef struct ustore_collection_drop_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Identifier of an existing collection. */
    ustore_collection_t id;

    /** Whether values, pairs or the whole collection go. */
    ustore_drop_mode_t mode;

} ustore_collection_drop_t;

/**
 *  @brief Removes or clears a collection.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_collection_drop(ustore_collection_drop_t* arguments);

#pragma endregion Collections

#pragma region Transactions

/** The arguments of @c ustore_transaction_init. */
typedef struct ustore_transaction_init_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Accepts @c ustore_option_dont_discard_memory_k. */
    ustore_options_t options;

    /** The transaction to begin or to reset for reuse. */
    ustore_transaction_t* transaction;

} ustore_transaction_init_t;

/**
 *  @brief Begins a transaction, or resets an existing one.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_transaction_init(ustore_transaction_init_t* arguments);

/** The arguments of @c ustore_transaction_stage, which keeps the contents for logging or retries
 *  whatever the outcome. */
typedef struct ustore_transaction_stage_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** An initialized transaction. */
    ustore_transaction_t transaction;

    /** Options of the stage. */
    ustore_options_t options;

    /** Optional output for the sequence number of the stage. */
    ustore_sequence_number_t* sequence_number;

} ustore_transaction_stage_t;

/**
 *  @brief Stages a transaction as the first phase of a two-phase commit.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_transaction_stage(ustore_transaction_stage_t* arguments);

/** The arguments of @c ustore_transaction_commit, which keeps the contents for logging or retries
 *  whatever the outcome. */
typedef struct ustore_transaction_commit_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** An initialized transaction. */
    ustore_transaction_t transaction;

    /** Accepts @c ustore_option_write_flush_k. */
    ustore_options_t options;

    /** Optional output for the sequence number of the commit. */
    ustore_sequence_number_t* sequence_number;

} ustore_transaction_commit_t;

/**
 *  @brief Commits a transaction.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_transaction_commit(ustore_transaction_commit_t* arguments);

#pragma endregion Transactions

#pragma region Memory Reclamation

/**
 *  @brief Frees a reusable arena and everything exported into it.
 *  @param[inout] arena The arena to be freed; @c NULL is safe.
 */
void ustore_arena_free(ustore_arena_t arena);

/**
 *  @brief Resets a transaction and frees its memory.
 *  @param[inout] transaction The transaction to be freed; @c NULL is safe.
 */
void ustore_transaction_free(ustore_transaction_t transaction);

/**
 *  @brief Closes the database and frees its memory, while the data persists on disk.
 *  @param[inout] database The database to be closed; @c NULL is safe.
 */
void ustore_database_free(ustore_database_t database);

/**
 *  @brief Frees an error message.
 *  @param[inout] error The message to be freed; @c NULL is safe.
 */
void ustore_error_free(ustore_error_t error);

#pragma endregion Memory Reclamation

#ifdef __cplusplus
}
#endif
