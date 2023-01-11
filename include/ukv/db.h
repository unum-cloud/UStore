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
 * @see `ukv_database_free()`.
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
 * Every database always has at least one collection - the `::ukv_collection_main_k`.
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
typedef void* ukv_database_t;

/**
 * @brief Opaque Transaction handle.
 * @see `ukv_transaction_free()`.
 * @see https://unum.cloud/ukv/c#transactions
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
 * - Lifetime: Must be freed before the @c ukv_database_t is closed.
 * - Concurrency Control: Optimistic.
 */
typedef void* ukv_transaction_t;

/**
 * @brief Some unique integer identifier of a collection.
 * A @c ukv_database_t database can have many of those,
 * but never with repeating names or identifiers.
 * Those identifiers are not guaranteed to remain the same
 * between DBMS restarts.
 */
typedef uint64_t ukv_collection_t;

/**
 * @brief The unique identifier of any value within a single collection.
 */
typedef int64_t ukv_key_t;

/**
 * @brief The elementary binary piece of any value.
 */
typedef uint8_t ukv_byte_t;

/**
 * @brief Single-precisions floating-point number.
 */
typedef float ukv_float_t;

/**
 * @brief The elementary piece of any string, like collection name.
 */
typedef char ukv_char_t;

/**
 * @brief The length of any value in the DB.
 */
typedef uint32_t ukv_length_t;

/**
 * @brief Pointer-sized integer type.
 */
typedef uint64_t ukv_size_t;

/**
 * @brief The smallest possible "bitset" type, storing eight zeros or ones.
 */
typedef uint8_t ukv_octet_t;

/**
 * @brief Monotonically increasing unique identifier that reflects the order of applied transactions
 */
typedef uint64_t ukv_sequence_number_t;

/**
 * @brief Owning error message string.
 * If not null, must be deallocated via `ukv_error_free()`.
 */
typedef char const* ukv_error_t;

/**
 * @brief Non-owning string reference.
 * Always provided by user and we don't participate
 * in its lifetime management in any way.
 */
typedef char const* ukv_str_view_t;
typedef char* ukv_str_span_t;

/**
 * @brief Temporary memory handle, used mostly for read requests.
 * It's allocated, resized and deallocated only by UKV itself.
 * Once done, must be deallocated with `ukv_arena_free()`.
 * @see `ukv_arena_free()`.
 */
typedef void* ukv_arena_t;

typedef uint8_t* ukv_bytes_ptr_t;
typedef uint8_t const* ukv_bytes_cptr_t;

typedef void* ukv_callback_payload_t;
typedef void (*ukv_callback_t)(ukv_callback_payload_t);

typedef enum {

    ukv_options_default_k = 0,
    /**
     * @brief Forces absolute consistency on the write operations
     * flushing all the data to disk after each write. It's usage
     * may cause severe performance degradation in some implementations.
     * Yet the users must be warned, that modern IO drivers still often
     * can't guarantee that everything will reach the disk.
     */
    ukv_option_write_flush_k = 1 << 1,
    /**
     * @brief When reading from a transaction, we track the requested keys.
     * If the requested key was updated since the read, the transaction
     * will fail on commit or prior to that. This option disables collision
     * detection on separate parts of transactional reads and writes.
     */
    ukv_option_transaction_dont_watch_k = 1 << 2,
    /**
     * @brief On every API call, the arena is cleared for reuse.
     * If the arguments of the function are results of another UKV call,
     * you can use this flag to avoid discarding the memory.
     */
    ukv_option_dont_discard_memory_k = 1 << 4,
    /**
     * @brief Will output data into shared memory, not the one privately
     * to do further transformations without any copies.
     * Is relevant for standalone distributions used with drivers supporting
     * Apache Arrow buffers or standardized Tensor representations.
     */
    ukv_option_read_shared_memory_k = 1 << 5,
    /**
     * @brief When set, the underlying engine may avoid strict keys ordering
     * and may include irrelevant (deleted & duplicate) keys in order to maximize
     * throughput. The purpose is not accelerating the `ukv_scan()`, but the
     * following `ukv_read()`. Generally used for Machine Learning applications.
     */
    ukv_option_scan_bulk_k = 0, // TODO

} ukv_options_t;

/**
 * @brief The "mode" of collection removal.
 */
typedef enum {
    /** @brief Remove the handle and all of the contents. */
    ukv_drop_keys_vals_handle_k = 0,
    /** @brief Remove keys and values, but keep the collection. */
    ukv_drop_keys_vals_k = 1,
    /** @brief Clear the values, but keep the keys. */
    ukv_drop_vals_k = 2,
} ukv_drop_mode_t;

/**
 * @brief The handle to the default nameless collection.
 * It exists from start, doesn't have to be created and can't be fully dropped.
 * Only `::ukv_drop_keys_vals_k` and `::ukv_drop_vals_k` apply to it.
 */
extern ukv_collection_t const ukv_collection_main_k;
extern ukv_length_t const ukv_length_missing_k;
extern ukv_key_t const ukv_key_unknown_k;

extern bool const ukv_supports_transactions_k;
extern bool const ukv_supports_named_collections_k;
extern bool const ukv_supports_snapshots_k;

/*********************************************************/
/*****************	 Primary Functions	  ****************/
/*********************************************************/

/**
 * @brief Opens the underlying Key-Value Store.
 * @see `ukv_database_init()`.
 *
 * Depending on the selected distribution can be any of:
 *
 * - embedded persistent transactional KVS
 * - embedded in-memory transactional KVS
 * - remote persistent transactional KVS
 * - remote in-memory transactional KVS
 */
typedef struct ukv_database_init_t {
    /**
     * @brief Configuration parameter for the DBMS.
     *
     * For embedded distributions should be the root directory,
     * under which the DBMS and it's files will be organized.
     *
     * Recommendations:
     * - UMem: empty or `/var/lib/ukv/umem/` for eventually persistent.
     * - RocksDB: `/var/lib/ukv/rocksdb/` optionally storing `config_rocksdb.ini`.
     * - LevelDB: `/var/lib/ukv/leveldb/` optionally storing `config_leveldb.json`.
     * - Flight API Client: `grpc://0.0.0.0:38709`.
     */
    ukv_str_view_t config;
    /** @brief A pointer to the opened KVS, unless `error` is filled. */
    ukv_database_t* db;
    /** @brief Pointer to exported error message. */
    ukv_error_t* error;
} ukv_database_init_t;

/**
 * @brief Opens the underlying Key-Value Store.
 * @see `ukv_database_init()`.
 */
void ukv_database_init(ukv_database_init_t*);

/*********************************************************/
/***************** Collection Management  ****************/
/*********************************************************/

/**
 * @brief Lists all named collections in the DB.
 * @see `ukv_collection_list()`.
 *
 * Retrieves a list of collection IDs & names in a NULL-delimited form.
 * The default nameless collection won't be described in any form, as its always
 * present. This is the only collection-management operation that can be performed
 * on a DB state snapshot, and not just on the HEAD state.
 */
typedef struct ukv_collection_list_t {

    /// @name Context
    /// @{

    /** @brief Already open database instance. */
    ukv_database_t db;
    /**
     * @brief Pointer to exported error message.
     * If not NULL, must be deallocated with `ukv_error_free()`.
     */
    ukv_error_t* error;
    /**
     * @brief The snapshot in which the retrieval will be conducted.
     * @see `ukv_transaction_init()`, `ukv_transaction_commit()`, `ukv_transaction_free()`.
     */
    ukv_transaction_t transaction;
    /**
     * @brief Reusable memory handle.
     * @see `ukv_arena_free()`.
     */
    ukv_arena_t* arena;
    /**
     * @brief Listing options.
     *
     * Possible values:
     * - `::ukv_option_dont_discard_memory_k`: Won't reset the `arena` before the operation begins.
     */
    ukv_options_t options;

    /// @}
    /// @name Contents
    /// @{

    /** @brief Number of present collections. */
    ukv_size_t* count;
    /** @brief Handles of all the collections in same order as `names`. */
    ukv_collection_t** ids;
    /** @brief Offsets of separate strings in the `names` tape. */
    ukv_length_t** offsets;
    /** @brief NULL-terminated collection names tape in same order as `ids`. */
    ukv_char_t** names;
    /// @}

} ukv_collection_list_t;

/**
 * @brief Lists all named collections in the DB.
 * @see `ukv_collection_list_t`.
 */
void ukv_collection_list(ukv_collection_list_t*);

/**
 * @brief Creates a new uniquely named collection in the DB.
 * @see `ukv_collection_create()`.
 *
 * This function may never be called, as the default nameless collection
 * always exists and can be addressed via `::ukv_collection_main_k`.
 * You can "re-create" an empty collection with a new config.
 */
typedef struct ukv_collection_create_t {
    /** @brief Already open database instance. */
    ukv_database_t db;
    /** @brief Pointer to exported error message. */
    ukv_error_t* error;
    /** @brief Unique name for the new collection. */
    ukv_str_view_t name;
    /** @brief Optional configuration JSON string. */
    ukv_str_view_t config;
    /** @brief Output for the collection handle. */
    ukv_collection_t* id;
} ukv_collection_create_t;

/**
 * @brief Creates a new uniquely named collection in the DB.
 * @see `ukv_collection_create_t`.
 */
void ukv_collection_create(ukv_collection_create_t*);

/**
 * @brief Removes or clears an existing collection.
 * @see `ukv_collection_drop()`.
 *
 * Removes a collection or its contents depending on `mode`.
 * The default nameless collection can't be removed, only cleared.
 */
typedef struct ukv_collection_drop_t {
    /** @brief Already open database instance. */
    ukv_database_t db;
    /** @brief Pointer to exported error message. */
    ukv_error_t* error;
    /** @brief Existing collection handle. */
    ukv_collection_t id;
    /** @brief Controls if values, pairs or the whole collection must be dropped. */
    ukv_drop_mode_t mode;
} ukv_collection_drop_t;

/**
 * @brief Removes or clears an existing collection.
 * @see `ukv_collection_drop_t`.
 */
void ukv_collection_drop(ukv_collection_drop_t*);

/**
 * @brief Free-form communication tunnel with the underlying engine.
 * @see `ukv_database_control()`.
 *
 * Performs free-form queries on the DB, that may not necessarily
 * have a stable API and a fixed format output. Generally, those requests
 * are very expensive and shouldn't be executed in most applications.
 * This is the "kitchen-sink" of UKV interface, similar to `fcntl` & `ioctl`.
 *
 * ## Possible Commands
 * - "clear":   Removes all the data from DB, while keeping collection names.
 * - "reset":   Removes all the data from DB, including collection names.
 * - "compact": Flushes and compacts all the data in LSM-tree implementations.
 * - "info":    Metadata about the current software version, used for debugging.
 * - "usage":   Metadata about approximate collection sizes, RAM and disk usage.
 */
typedef struct ukv_database_control_t {
    /** @brief Already open database instance. */
    ukv_database_t db;
    /** @brief Reusable memory handle. */
    ukv_arena_t* arena;
    /** @brief Pointer to exported error message. */
    ukv_error_t* error;
    /** @brief The input command as a NULL-terminated string. */
    ukv_str_view_t request;
    /** @brief The output response as a NULL-terminated string. */
    ukv_str_view_t* response;
} ukv_database_control_t;

/**
 * @brief Free-form communication tunnel with the underlying engine.
 * @see `ukv_database_control()`.
 */
void ukv_database_control(ukv_database_control_t*);

/*********************************************************/
/*****************		Transactions	  ****************/
/*********************************************************/

/**
 * @brief Begins a new ACID transaction or resets an existing one.
 * @see `ukv_transaction_init()`.
 */
typedef struct ukv_transaction_init_t {

    /** @brief Already open database instance. */
    ukv_database_t db;
    /** @brief Pointer to exported error message. */
    ukv_error_t* error;

    /**
     * @brief Transaction options.
     *
     * Possible values:
     * - `::ukv_option_dont_discard_memory_k`: Won't reset the `arena` before the operation begins.
     */
    ukv_options_t options;

    /** @brief In-out transaction handle. */
    ukv_transaction_t* transaction;
} ukv_transaction_init_t;

/**
 * @brief Begins a new ACID transaction or resets an existing one.
 * @see `ukv_transaction_init_t`.
 */
void ukv_transaction_init(ukv_transaction_init_t*);

/**
 * @brief Stages an ACID transaction for Two Phase Commits.
 * @see `ukv_transaction_stage()`.
 *
 * Regardless of result, the content is preserved to allow further
 * logging, serialization or retries. The underlying memory can be
 * cleaned and reused by consecutive `ukv_transaction_init()` call.
 */
typedef struct ukv_transaction_stage_t {

    /** @brief Already open database instance. */
    ukv_database_t db;
    /** @brief Pointer to exported error message. */
    ukv_error_t* error;
    /** @brief Initialized transaction handle. */
    ukv_transaction_t transaction;
    /** @brief Staging options. */
    ukv_options_t options;
    /** @brief Optional output for the transaction stage sequence number. */
    ukv_sequence_number_t* sequence_number;

} ukv_transaction_stage_t;

/**
 * @brief Stages an ACID transaction for Two Phase Commits.
 * @see `ukv_transaction_stage_t`.
 */
void ukv_transaction_stage(ukv_transaction_stage_t*);

/**
 * @brief Commits an ACID transaction.
 * @see `ukv_transaction_commit()`.
 *
 * Regardless of result, the content is preserved to allow further
 * logging, serialization or retries. The underlying memory can be
 * cleaned and reused by consecutive `ukv_transaction_init()` call.
 */
typedef struct ukv_transaction_commit_t {

    /** @brief Already open database instance. */
    ukv_database_t db;
    /** @brief Pointer to exported error message. */
    ukv_error_t* error;
    /** @brief Initialized transaction handle. */
    ukv_transaction_t transaction;
    /** @brief Staging options. */
    ukv_options_t options;
    /** @brief Optional output for the transaction commit sequence number. */
    ukv_sequence_number_t* sequence_number;

} ukv_transaction_commit_t;

/**
 * @brief Commits an ACID transaction.
 * @see `ukv_transaction_commit_t`.
 */
void ukv_transaction_commit(ukv_transaction_commit_t*);

/*********************************************************/
/*****************	 Memory Reclamation   ****************/
/*********************************************************/

/**
 * @brief Deallocates reusable memory arenas.
 * Passing NULLs is safe.
 */
void ukv_arena_free(ukv_arena_t);

/**
 * @brief Resets the transaction and deallocates the underlying memory.
 * Passing NULLs is safe.
 */
void ukv_transaction_free(ukv_transaction_t);

/**
 * @brief Closes the DB and deallocates used memory.
 * The database would still persist on disk.
 * Passing NULLs is safe.
 */
void ukv_database_free(ukv_database_t);

/**
 * @brief Deallocates error messages.
 * Passing NULLs is safe.
 */
void ukv_error_free(ukv_error_t);

#ifdef __cplusplus
} /* end extern "C" */
#endif
