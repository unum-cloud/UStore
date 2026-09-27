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
 *  database holds @c ustore_collection_main_k, which can be cleared but never dropped, next to the
 *  collections of binary values, documents or graphs it creates.
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

/** What went wrong, in one vocabulary shared by every engine and binding. */
typedef enum ustore_status_t {

    /** The call completed; @c message carries nothing. */
    ustore_success_k = 0,

    /** An allocation could not be satisfied. */
    ustore_error_out_of_memory_k = 1,

    /** An argument is null, contradicts another one, or carries options the call does not accept. */
    ustore_error_invalid_argument_k = 2,

    /** The call needs a database or transaction that was never opened. */
    ustore_error_uninitialized_k = 3,

    /** The engine or build does not implement what the call asks for. */
    ustore_error_unsupported_k = 4,

    /** A file or directory could not be opened, read, written, or closed. */
    ustore_error_io_k = 5,

    /** Stored or supplied data could not be decoded, like a corrupted file or an invalid document. */
    ustore_error_malformed_k = 6,

    /** A snapshot, collection, or document the call addresses does not exist. */
    ustore_error_not_found_k = 7,

    /** A snapshot, collection, or document the call creates already exists. */
    ustore_error_already_exists_k = 8,

    /** A concurrent writer changed a key this transaction wrote; retry the transaction. */
    ustore_error_write_conflict_k = 9,

    /** A concurrent writer changed a key this transaction read; retry the transaction. */
    ustore_error_read_conflict_k = 10,

    /** A concurrent writer inserted into a range this transaction scanned; retry the transaction. */
    ustore_error_phantom_conflict_k = 11,

    /** A hard design ceiling was reached, where a smaller request would fit. */
    ustore_error_capacity_exhausted_k = 12,

    /** A device could not be selected, or its memory could not be claimed. */
    ustore_error_device_k = 13,

    /** The engine failed in a way no other status names, which points at a defect. */
    ustore_error_internal_k = 14,

} ustore_status_t;

/** Outcome of one call: a status to branch on, and a static message that is never freed and is @c NULL exactly
 *  when the status is @c ustore_success_k. */
typedef struct ustore_error_t {

    /** What went wrong, or @c ustore_success_k. */
    ustore_status_t status;

    /** Static detail the status cannot carry. */
    char const* message;

} ustore_error_t;

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

    /** The engine supports collections beyond the main one. */
    ustore_supports_collections_k = 2,

    /** The engine supports snapshots. */
    ustore_supports_snapshots_k = 4,

} ustore_metadata_t;

/** The collection every database holds, which @c ustore_drop_keys_vals_k and
 *  @c ustore_drop_vals_k may only clear. */
extern ustore_collection_t const ustore_collection_main_k;

/** The collection reserved for the strings modality that maps names onto identifiers, which
 *  @c ustore_collection_create never hands out and @c ustore_collection_list never reports. An engine
 *  without collections of its own refuses it, as it refuses every collection but the main one. */
extern ustore_collection_t const ustore_collection_names_k;

/** The length reported for a missing value. */
extern ustore_length_t const ustore_length_missing_k;

/** The key that stands for no key. */
extern ustore_key_t const ustore_key_unknown_k;

#pragma endregion Handles and Constants

#pragma region Database

/** The arguments of every engine's init call. */
typedef struct ustore_database_init_t {

    /** Configuration of the engine as a JSON string. */
    ustore_str_view_t config;

    /** Output for the opened database, unless @c error is set. */
    ustore_database_t* db;

    /** Output for the error message. */
    ustore_error_t* error;

} ustore_database_init_t;

/**
 *  @brief Opens the in-memory engine, which persists as one image when the configuration names a directory.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_database_init_ram(ustore_database_init_t* arguments);

/** Opens the RocksDB engine, in a library built with `USTORE_WITH_ROCKSDB`. */
void ustore_database_init_rocksdb(ustore_database_init_t* arguments);

/** Opens the LevelDB engine, in a library built with `USTORE_WITH_LEVELDB`. */
void ustore_database_init_leveldb(ustore_database_init_t* arguments);

/** Opens the NVMe engine, in a library built with `USTORE_BUILD_NVME`. */
void ustore_database_init_nvme(ustore_database_init_t* arguments);

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
 *  Requests have no stable format and are expensive: "clear" removes all data and keeps the
 *  collections, "reset" removes both, "compact" flushes and compacts LSM trees, "info" describes the build,
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
 *  @brief The arguments of @c ustore_collection_list, which exports the identifiers of the collections.
 *
 *  The main collection is never listed. Listing is the only collection operation that runs on a snapshot.
 *  A name a caller gives a collection lives in the strings modality, never in the catalog of an engine.
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

    /** Output for the number of collections. */
    ustore_size_t* count;

    /** Output for the identifiers of the collections. */
    ustore_collection_t** ids;

} ustore_collection_list_t;

/**
 *  @brief Lists the collections of the database.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_collection_list(ustore_collection_list_t* arguments);

/** The arguments of @c ustore_collection_create, which always creates an empty collection and answers
 *  the identifier addressing it. */
typedef struct ustore_collection_create_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Optional configuration as a JSON string. */
    ustore_str_view_t config;

    /** Output for the identifier of the collection. */
    ustore_collection_t* id;

} ustore_collection_create_t;

/**
 *  @brief Creates an empty collection.
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

#pragma endregion Memory Reclamation

#ifdef __cplusplus
}
#endif
