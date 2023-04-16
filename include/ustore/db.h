/**
 * @file db.h
 * @author Ashot Vardanian
 * @date 12 Jun 2022
 * @addtogroup C
 *
 * @brief Binary Interface Standard for Transactional @b Key-Value Stores.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*********************************************************/
/*****************   Structures & Consts  ****************/
/*********************************************************/

/**
 * @brief Opaque multi-modal Database handle.
 * @see `ustore_database_free()`.
 *
 * Properties:
 * - Thread safety: Safe to use across threads after open and before free.
 * - Lifetime: Must live longer than all the transactions.
 *
 * ## Concurrency
 *
 * In embedded setup this handle manages the lifetime of the database.
 * In that case user must guarantee, that concurrent processes won't be
 * opening the same database (generally same directory).
 *
 * In standalone "client-server" setup, manages the lifetime of the "client".
 * Many concurrent clients can be connecting to the same server from the same
 * process.
 *
 * ## Collections
 *
 * Every database always has at least one collection - the `::ustore_collection_main_k`.
 * That one has no name and can't be deleted. Others are referenced by names.
 * The same database can have many collections, of different modalities:
 * - Binary Large Objects or BLOBs.
 * - Hierarchical documents, like JSONs, BSONs, MessagePacks.
 * - Discrete labeled and potentially directed Graphs.
 * - Paths or collections of string keys.
 *
 * ## Choosing the Engine
 *
 * Dynamic dispatch of engines isn't yet supported.
 *
 * ## CAP Theorem
 *
 * Distributed engines are not yet supported.
 */
typedef void* ustore_database_t;

/**
 * @brief
 *
 */
typedef uint64_t ustore_snapshot_t;

/**
 * @brief Opaque Transaction handle.
 * @see `ustore_transaction_free()`.
 * @see https://unum.cloud/ustore/c#transactions
 *
 * Allows ACID-ly grouping operations across different collections and even modalities.
 * This means, that the same transaction might be:
 * - inserting a blob of media data into a collection of images.
 * - updating users metadata in a documents collection to reference new avatar.
 * - introducing links between the user and other in a graph collection...
 * and all of the operations here either succeed or fail together. DBMS will
 * do the synchronization heavy-lifting, so you don't have to.
 *
 * Properties:
 * - Thread safety: None.
 * - Lifetime: Must be freed before the @c ustore_database_t is closed.
 * - Concurrency Control: Optimistic.
 */
typedef void* ustore_transaction_t;

/**
 * @brief Some unique integer identifier of a collection.
 * A @c ustore_database_t database can have many of those,
 * but never with repeating names or identifiers.
 * Those identifiers are not guaranteed to remain the same
 * between DBMS restarts.
 */
typedef uint64_t ustore_collection_t;

/**
 * @brief The unique identifier of any value within a single collection.
 */
typedef int64_t ustore_key_t;

/**
 * @brief The elementary binary piece of any value.
 */
typedef uint8_t ustore_byte_t;

/**
 * @brief Single-precisions floating-point number.
 */
typedef float ustore_float_t;

/**
 * @brief The elementary piece of any string, like collection name.
 */
typedef char ustore_char_t;

/**
 * @brief The length of any value in the DB.
 */
typedef uint32_t ustore_length_t;

/**
 * @brief Pointer-sized integer type.
 */
typedef uint64_t ustore_size_t;

/**
 * @brief The smallest possible "bitset" type, storing eight zeros or ones.
 */
typedef uint8_t ustore_octet_t;

/**
 * @brief Monotonically increasing unique identifier that reflects the order of applied transactions
 */
typedef uint64_t ustore_sequence_number_t;

/**
 * @brief Owning error message string.
 * If not null, must be deallocated via `ustore_error_free()`.
 */
typedef char const* ustore_error_t;

/**
 * @brief Non-owning string reference.
 * Always provided by user and we don't participate
 * in its lifetime management in any way.
 */
typedef char const* ustore_str_view_t;
typedef char* ustore_str_span_t;

/**
 * @brief Temporary memory handle, used mostly for read requests.
 * It's allocated, resized and deallocated only by UStore itself.
 * Once done, must be deallocated with `ustore_arena_free()`.
 * @see `ustore_arena_free()`.
 */
typedef void* ustore_arena_t;

typedef uint8_t* ustore_bytes_ptr_t;
typedef uint8_t const* ustore_bytes_cptr_t;

typedef void* ustore_callback_payload_t;
typedef void (*ustore_callback_t)(ustore_callback_payload_t);

typedef enum {

    ustore_options_default_k = 0,
    /**
     * @brief Forces absolute consistency on the write operations
     * flushing all the data to disk after each write. It's usage
     * may cause severe performance degradation in some implementations.
     * Yet the users must be warned, that modern IO drivers still often
     * can't guarantee that everything will reach the disk.
     */
    ustore_option_write_flush_k = 1 << 1,
    /**
     * @brief When reading from a transaction, we track the requested keys.
     * If the requested key was updated since the read, the transaction
     * will fail on commit or prior to that. This option disables collision
     * detection on separate parts of transactional reads and writes.
     */
    ustore_option_transaction_dont_watch_k = 1 << 2,
    /**
     * @brief On every API call, the arena is cleared for reuse.
     * If the arguments of the function are results of another UStore call,
     * you can use this flag to avoid discarding the memory.
     */
    ustore_option_dont_discard_memory_k = 1 << 4,
    /**
     * @brief Will output data into shared memory, not the one privately
     * to do further transformations without any copies.
     * Is relevant for standalone distributions used with drivers supporting
     * Apache Arrow buffers or standardized Tensor representations.
     */
    ustore_option_read_shared_memory_k = 1 << 5,
    /**
     * @brief When set, the underlying engine may avoid strict keys ordering
     * and may include irrelevant (deleted & duplicate) keys in order to maximize
     * throughput. The purpose is not accelerating the `ustore_scan()`, but the
     * following `ustore_read()`. Generally used for Machine Learning applications.
     */
    ustore_option_scan_bulk_k = 0, // TODO

} ustore_options_t;

/**
 * @brief The "mode" of collection removal.
 */
typedef enum {
    /** @brief Remove the handle and all of the contents. */
    ustore_drop_keys_vals_handle_k = 0,
    /** @brief Remove keys and values, but keep the collection. */
    ustore_drop_keys_vals_k = 1,
    /** @brief Clear the values, but keep the keys. */
    ustore_drop_vals_k = 2,
} ustore_drop_mode_t;

/**
 * @brief The handle to the default nameless collection.
 * It exists from start, doesn't have to be created and can't be fully dropped.
 * Only `::ustore_drop_keys_vals_k` and `::ustore_drop_vals_k` apply to it.
 */
extern ustore_collection_t const ustore_collection_main_k;
extern ustore_length_t const ustore_length_missing_k;
extern ustore_key_t const ustore_key_unknown_k;

extern bool const ustore_supports_transactions_k;
extern bool const ustore_supports_named_collections_k;
extern bool const ustore_supports_snapshots_k;

/*********************************************************/
/*****************	 Primary Functions	  ****************/
/*********************************************************/

/**
 * @brief Opens the underlying Key-Value Store.
 * @see `ustore_database_init()`.
 *
 * Depending on the selected distribution can be any of:
 *
 * - embedded persistent transactional KVS
 * - embedded in-memory transactional KVS
 * - remote persistent transactional KVS
 * - remote in-memory transactional KVS
 */
typedef struct ustore_database_init_t {
    /**
     * @brief Configuration parameter for the DBMS.
     * @see `db_config.json` file.
     *
     * For embedded distributions should be a json string containing DB options.
     *
     * Special:
     * - Flight API Client: `grpc://0.0.0.0:38709`.
     */
    ustore_str_view_t config;
    /** @brief A pointer to the opened KVS, unless `error` is filled. */
    ustore_database_t* db;
    /** @brief Pointer to exported error message. */
    ustore_error_t* error;
} ustore_database_init_t;

/**
 * @brief Opens the underlying Key-Value Store.
 * @see `ustore_database_init()`.
 */
void ustore_database_init(ustore_database_init_t*);

/*********************************************************/
/***************** Snapshot Management  ****************/
/*********************************************************/

typedef struct ustore_snapshot_list_t { /// @name Context
    /// @{

    /** @brief Already open database instance. */
    ustore_database_t db;
    /**
     * @brief Pointer to exported error message.
     * If not NULL, must be deallocated with `ustore_error_free()`.
     */
    ustore_error_t* error;
    /**
     * @brief Reusable memory handle.
     * @see `ustore_arena_free()`.
     */
    ustore_arena_t* arena;
    /**
     * @brief Listing options.
     *
     * Possible values:
     * - `::ustore_option_dont_discard_memory_k`: Won't reset the `arena` before the operation begins.
     */
    ustore_options_t options;

    /// @}
    /// @name Contents
    /// @{

    /** @brief Number of present snapshots. */
    ustore_size_t* count;
    /** @brief All snapshots id. */
    ustore_snapshot_t** ids;
    /// @}
} ustore_snapshot_list_t;

/**
 * @brief Lists all snapshots in the DB.
 * @see `ustore_snapshot_list_t`.
 */
void ustore_snapshot_list(ustore_snapshot_list_t*);

typedef struct ustore_snapshot_create_t {
    /** @brief Already open database instance. */
    ustore_database_t db;
    /** @brief Pointer to exported error message. */
    ustore_error_t* error;
    /** @brief Output for the snapshot id. */
    ustore_snapshot_t* id;
} ustore_snapshot_create_t;

void ustore_snapshot_create(ustore_snapshot_create_t*);

typedef struct ustore_snapshot_drop_t {
    /** @brief Already open database instance. */
    ustore_database_t db;
    /** @brief Pointer to exported error message. */
    ustore_error_t* error;
    /** @brief Existing snapshot id. */
    ustore_snapshot_t id;
} ustore_snapshot_drop_t;

void ustore_snapshot_drop(ustore_snapshot_drop_t*);

/*********************************************************/
/***************** Collection Management  ****************/
/*********************************************************/

/**
 * @brief Lists all named collections in the DB.
 * @see `ustore_collection_list()`.
 *
 * Retrieves a list of collection IDs & names in a NULL-delimited form.
 * The default nameless collection won't be described in any form, as its always
 * present. This is the only collection-management operation that can be performed
 * on a DB state snapshot, and not just on the HEAD state.
 */
typedef struct ustore_collection_list_t {

    /// @name Context
    /// @{

    /** @brief Already open database instance. */
    ustore_database_t db;
    /**
     * @brief Pointer to exported error message.
     * If not NULL, must be deallocated with `ustore_error_free()`.
     */
    ustore_error_t* error;
    /**
     * @brief The snapshot in which the retrieval will be conducted.
     * @see `ustore_transaction_init()`, `ustore_transaction_commit()`, `ustore_transaction_free()`.
     */
    ustore_transaction_t transaction;
    /**
     * @brief A snapshot captures a point-in-time view of the DB at the time it's created.
     * @see `ustore_snapshot_list()`, `ustore_snapshot_create()`, `ustore_snapshot_drop()`.
     */
    ustore_snapshot_t snapshot;
    /**
     * @brief Reusable memory handle.
     * @see `ustore_arena_free()`.
     */
    ustore_arena_t* arena;
    /**
     * @brief Listing options.
     *
     * Possible values:
     * - `::ustore_option_dont_discard_memory_k`: Won't reset the `arena` before the operation begins.
     */
    ustore_options_t options;

    /// @}
    /// @name Contents
    /// @{

    /** @brief Number of present collections. */
    ustore_size_t* count;
    /** @brief Handles of all the collections in same order as `names`. */
    ustore_collection_t** ids;
    /** @brief Offsets of separate strings in the `names` tape. */
    ustore_length_t** offsets;
    /** @brief NULL-terminated collection names tape in same order as `ids`. */
    ustore_char_t** names;
    /// @}

} ustore_collection_list_t;

/**
 * @brief Lists all named collections in the DB.
 * @see `ustore_collection_list_t`.
 */
void ustore_collection_list(ustore_collection_list_t*);

/**
 * @brief Creates a new uniquely named collection in the DB.
 * @see `ustore_collection_create()`.
 *
 * This function may never be called, as the default nameless collection
 * always exists and can be addressed via `::ustore_collection_main_k`.
 * You can "re-create" an empty collection with a new config.
 */
typedef struct ustore_collection_create_t {
    /** @brief Already open database instance. */
    ustore_database_t db;
    /** @brief Pointer to exported error message. */
    ustore_error_t* error;
    /** @brief Unique name for the new collection. */
    ustore_str_view_t name;
    /** @brief Optional configuration JSON string. */
    ustore_str_view_t config;
    /** @brief Output for the collection handle. */
    ustore_collection_t* id;
} ustore_collection_create_t;

/**
 * @brief Creates a new uniquely named collection in the DB.
 * @see `ustore_collection_create_t`.
 */
void ustore_collection_create(ustore_collection_create_t*);

/**
 * @brief Removes or clears an existing collection.
 * @see `ustore_collection_drop()`.
 *
 * Removes a collection or its contents depending on `mode`.
 * The default nameless collection can't be removed, only cleared.
 */
typedef struct ustore_collection_drop_t {
    /** @brief Already open database instance. */
    ustore_database_t db;
    /** @brief Pointer to exported error message. */
    ustore_error_t* error;
    /** @brief Existing collection handle. */
    ustore_collection_t id;
    /** @brief Controls if values, pairs or the whole collection must be dropped. */
    ustore_drop_mode_t mode;
} ustore_collection_drop_t;

/**
 * @brief Removes or clears an existing collection.
 * @see `ustore_collection_drop_t`.
 */
void ustore_collection_drop(ustore_collection_drop_t*);

/**
 * @brief Free-form communication tunnel with the underlying engine.
 * @see `ustore_database_control()`.
 *
 * Performs free-form queries on the DB, that may not necessarily
 * have a stable API and a fixed format output. Generally, those requests
 * are very expensive and shouldn't be executed in most applications.
 * This is the "kitchen-sink" of UStore interface, similar to `fcntl` & `ioctl`.
 *
 * ## Possible Commands
 * - "clear":   Removes all the data from DB, while keeping collection names.
 * - "reset":   Removes all the data from DB, including collection names.
 * - "compact": Flushes and compacts all the data in LSM-tree implementations.
 * - "info":    Metadata about the current software version, used for debugging.
 * - "usage":   Metadata about approximate collection sizes, RAM and disk usage.
 */
typedef struct ustore_database_control_t {
    /** @brief Already open database instance. */
    ustore_database_t db;
    /** @brief Reusable memory handle. */
    ustore_arena_t* arena;
    /** @brief Pointer to exported error message. */
    ustore_error_t* error;
    /** @brief The input command as a NULL-terminated string. */
    ustore_str_view_t request;
    /** @brief The output response as a NULL-terminated string. */
    ustore_str_view_t* response;
} ustore_database_control_t;

/**
 * @brief Free-form communication tunnel with the underlying engine.
 * @see `ustore_database_control()`.
 */
void ustore_database_control(ustore_database_control_t*);

/*********************************************************/
/*****************		Transactions	  ****************/
/*********************************************************/

/**
 * @brief Begins a new ACID transaction or resets an existing one.
 * @see `ustore_transaction_init()`.
 */
typedef struct ustore_transaction_init_t {

    /** @brief Already open database instance. */
    ustore_database_t db;
    /** @brief Pointer to exported error message. */
    ustore_error_t* error;

    /**
     * @brief Transaction options.
     *
     * Possible values:
     * - `::ustore_option_dont_discard_memory_k`: Won't reset the `arena` before the operation begins.
     */
    ustore_options_t options;

    /** @brief In-out transaction handle. */
    ustore_transaction_t* transaction;
} ustore_transaction_init_t;

/**
 * @brief Begins a new ACID transaction or resets an existing one.
 * @see `ustore_transaction_init_t`.
 */
void ustore_transaction_init(ustore_transaction_init_t*);

/**
 * @brief Stages an ACID transaction for Two Phase Commits.
 * @see `ustore_transaction_stage()`.
 *
 * Regardless of result, the content is preserved to allow further
 * logging, serialization or retries. The underlying memory can be
 * cleaned and reused by consecutive `ustore_transaction_init()` call.
 */
typedef struct ustore_transaction_stage_t {

    /** @brief Already open database instance. */
    ustore_database_t db;
    /** @brief Pointer to exported error message. */
    ustore_error_t* error;
    /** @brief Initialized transaction handle. */
    ustore_transaction_t transaction;
    /** @brief Staging options. */
    ustore_options_t options;
    /** @brief Optional output for the transaction stage sequence number. */
    ustore_sequence_number_t* sequence_number;

} ustore_transaction_stage_t;

/**
 * @brief Stages an ACID transaction for Two Phase Commits.
 * @see `ustore_transaction_stage_t`.
 */
void ustore_transaction_stage(ustore_transaction_stage_t*);

/**
 * @brief Commits an ACID transaction.
 * @see `ustore_transaction_commit()`.
 *
 * Regardless of result, the content is preserved to allow further
 * logging, serialization or retries. The underlying memory can be
 * cleaned and reused by consecutive `ustore_transaction_init()` call.
 */
typedef struct ustore_transaction_commit_t {

    /** @brief Already open database instance. */
    ustore_database_t db;
    /** @brief Pointer to exported error message. */
    ustore_error_t* error;
    /** @brief Initialized transaction handle. */
    ustore_transaction_t transaction;
    /** @brief Staging options. */
    ustore_options_t options;
    /** @brief Optional output for the transaction commit sequence number. */
    ustore_sequence_number_t* sequence_number;

} ustore_transaction_commit_t;

/**
 * @brief Commits an ACID transaction.
 * @see `ustore_transaction_commit_t`.
 */
void ustore_transaction_commit(ustore_transaction_commit_t*);

/*********************************************************/
/*****************	 Memory Reclamation   ****************/
/*********************************************************/

/**
 * @brief Deallocates reusable memory arenas.
 * Passing NULLs is safe.
 */
void ustore_arena_free(ustore_arena_t);

/**
 * @brief Resets the transaction and deallocates the underlying memory.
 * Passing NULLs is safe.
 */
void ustore_transaction_free(ustore_transaction_t);

/**
 * @brief Closes the DB and deallocates used memory.
 * The database would still persist on disk.
 * Passing NULLs is safe.
 */
void ustore_database_free(ustore_database_t);

/**
 * @brief Deallocates error messages.
 * Passing NULLs is safe.
 */
void ustore_error_free(ustore_error_t);

#ifdef __cplusplus
} /* end extern "C" */
#endif
