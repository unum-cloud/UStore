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
typedef struct ustore_database_handle_t* ustore_database_t;

/** An opaque handle to a point-in-time view of the whole database, which a read names instead of a
 *  transaction. Short-lived: it pins the versions its reads need until @c ustore_snapshot_free, and is
 *  freed before its database closes. */
typedef struct ustore_snapshot_handle_t* ustore_snapshot_t;

/** An opaque handle to an optimistic transaction that groups operations across collections and
 *  modalities. Not thread-safe, and freed with @c ustore_transaction_free before its database closes. */
typedef struct ustore_transaction_handle_t* ustore_transaction_t;

/**
 *  @brief Sixteen bytes naming one thing everywhere, ordered as their bytes and compared with @c memcmp.
 *
 *  An integer spells as its 128-bit big-endian bytes, as Python's `uuid.UUID(int=...)` does, so identifiers made from
 *  integers order as the integers.
 */
typedef struct ustore_uuid_t {
    uint8_t bytes[16];
} ustore_uuid_t;

/** The identifier of a collection: a UUIDv7 the catalog mints once and never reuses, so it stays the same across
 *  restarts and never names two collections. */
typedef struct ustore_collection_id_t {
    ustore_uuid_t uuid;
} ustore_collection_id_t;

/** The identifier of a value within one collection. */
typedef struct ustore_key_t {
    ustore_uuid_t uuid;
} ustore_key_t;

/** The elementary piece of a binary value. */
typedef uint8_t ustore_byte_t;

/** A single-precision floating-point number. */
typedef float ustore_float_t;

/** The elementary piece of a string, such as a collection name. */
typedef char ustore_char_t;

/** The length of a value in bytes, or an offset into a tape of values, wide enough for a tape past 4 GB. */
typedef uint64_t ustore_length_t;

/** A pointer-sized unsigned integer. */
typedef uint64_t ustore_size_t;

/** The smallest bitset, holding eight bits. */
typedef uint8_t ustore_octet_t;

/** The position of a commit in the order commits apply in, which grows with every commit. */
typedef uint64_t ustore_stamp_t;

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

/** An opaque handle to temporary memory for results, which the database allocates and grows and the
 *  caller frees with @c ustore_arena_free. */
typedef struct ustore_arena_handle_t* ustore_arena_t;

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

} ustore_capabilities_t;

/** The collection every database holds, named by the nil UUID, which @c ustore_drop_keys_vals_k and
 *  @c ustore_drop_vals_k may only clear. */
extern ustore_collection_id_t const ustore_collection_main_k;

/** The length reported for a missing value. */
extern ustore_length_t const ustore_length_missing_k;

/** The key that stands for no key, every bit of it set. */
extern ustore_key_t const ustore_key_unknown_k;

#pragma endregion Handles and Constants

#pragma region Database

/** How the transactions of the in-memory engine see each other's writes. */
typedef enum ustore_isolation_t {

    /** Every transaction appears to run alone at one moment between its start and its commit. */
    ustore_isolation_strict_serializable_k = 0,

    /** Every transaction reads the state its start saw and conflicts only over what it writes. */
    ustore_isolation_snapshot_k = 1,

} ustore_isolation_t;

/** How the NVMe engine moves bytes between its files and memory. */
typedef enum ustore_transport_t {

    /** io_uring where the kernel offers it, Linux AIO where it offers only that, and blocking calls elsewhere. */
    ustore_transport_automatic_k = 0,

    /** Positioned reads and writes that block the polling thread, which every platform has. */
    ustore_transport_blocking_k = 1,

    /** io_uring on unbuffered files, refused where the kernel creates no ring. */
    ustore_transport_linux_uring_k = 2,

    /** Linux AIO on unbuffered files. */
    ustore_transport_linux_aio_k = 3,

    /** SPDK's user-space driver on NVMe devices the process owns. */
    ustore_transport_linux_spdk_k = 4,

} ustore_transport_t;

/** A directory the data files may occupy, with the bytes they may take there. */
typedef struct ustore_data_directory_t {

    /** The directory, which must exist. */
    ustore_str_view_t path;

    /** Bytes the files may take there: zero is unlimited on RocksDB and the default volume size on NVMe. */
    ustore_size_t max_bytes;

} ustore_data_directory_t;

/** The arguments of @c ustore_database_init_ram. */
typedef struct ustore_database_init_ram_t {

    /** Output for the opened database, unless @c error is set. */
    ustore_database_t* db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Directory the database persists in as one image, or null to keep it in memory alone. */
    ustore_str_view_t directory;

    /** How transactions see each other. */
    ustore_isolation_t isolation;

} ustore_database_init_ram_t;

/**
 *  @brief Opens the in-memory engine, which persists as one image when it names a directory.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_database_init_ram(ustore_database_init_ram_t* arguments);

/** The arguments of @c ustore_database_init_rocksdb. */
typedef struct ustore_database_init_rocksdb_t {

    /** Output for the opened database, unless @c error is set. */
    ustore_database_t* db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Directory of the database, which must exist. */
    ustore_str_view_t directory;

    /** Directories the table files fill in order, or null to keep them in @c directory. */
    ustore_data_directory_t const* data_directories;

    /** Entries of @c data_directories. */
    ustore_size_t data_directories_count;

} ustore_database_init_rocksdb_t;

/** Opens the RocksDB engine, or reports @c ustore_error_unsupported_k in a library built without it. */
void ustore_database_init_rocksdb(ustore_database_init_rocksdb_t* arguments);

/** The arguments of @c ustore_database_init_leveldb. */
typedef struct ustore_database_init_leveldb_t {

    /** Output for the opened database, unless @c error is set. */
    ustore_database_t* db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Directory of the database, which must exist. */
    ustore_str_view_t directory;

} ustore_database_init_leveldb_t;

/** Opens the LevelDB engine, or reports @c ustore_error_unsupported_k in a library built without it. */
void ustore_database_init_leveldb(ustore_database_init_leveldb_t* arguments);

/** The arguments of @c ustore_database_init_nvme, where every zero size or count keeps the engine's default. */
typedef struct ustore_database_init_nvme_t {

    /** Output for the opened database, unless @c error is set. */
    ustore_database_t* db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Directory of the database's own files, which must exist. */
    ustore_str_view_t directory;

    /** Directories the volumes fill, or null for one inside @c directory. */
    ustore_data_directory_t const* data_directories;

    /** Entries of @c data_directories. */
    ustore_size_t data_directories_count;

    /** How bytes move between the files and memory. */
    ustore_transport_t transport;

    /** Bytes of host memory the engine may pin and cache. */
    ustore_size_t memory_limit;

    /** Background threads, derived from the cores by default. */
    ustore_size_t threads;

    /** CUDA devices compaction may run on, or null for none. */
    int const* gpu_devices;

    /** Entries of @c gpu_devices. */
    ustore_size_t gpu_devices_count;

    /** Bytes of device memory compaction may take on each of @c gpu_devices. */
    ustore_size_t gpu_memory_limit;

    /** I/O channels the threads share beyond their own. */
    ustore_size_t shared_io_channels;

    /** Submissions a channel keeps in flight. */
    ustore_size_t queue_depth;

    /** Bytes of each request buffer of a channel. */
    ustore_size_t request_buffer_bytes;

    /** Request buffers of each channel. */
    ustore_size_t request_buffers_count;

    /** Bytes of each streaming buffer of a channel. */
    ustore_size_t stream_buffer_bytes;

    /** Streaming buffers of each channel. */
    ustore_size_t stream_buffers_count;

    /** Bytes a transaction may buffer before it must commit. */
    ustore_size_t transaction_max_bytes;

    /** Writes a transaction may buffer before it must commit. */
    ustore_size_t transaction_max_elements;

    /** Operations that may run on one collection at once. */
    ustore_size_t collection_concurrency_limit;

    /** Bytes of values each collection may cache. */
    ustore_size_t collection_cache_limit;

    /** Bytes each collection buffers before it flushes a sorted run. */
    ustore_size_t collection_write_buffer_max_bytes;

    /** Writes each collection buffers before it flushes a sorted run. */
    ustore_size_t collection_write_buffer_max_elements;

    /** Bytes the first level of each collection holds before it compacts into the next. */
    ustore_size_t collection_first_level_max_bytes;

    /** How many times larger each level of a collection is than the one above it. */
    ustore_size_t collection_level_enlarge_factor;

    /** Bytes of every value of a collection whose values share one size, or zero for varying sizes. */
    ustore_size_t collection_value_size;

    /** Bytes of the largest value of a collection whose values vary in size. */
    ustore_size_t collection_value_max_size;

} ustore_database_init_nvme_t;

/** Opens the NVMe engine, or reports @c ustore_error_unsupported_k in a library built without it. */
void ustore_database_init_nvme(ustore_database_init_nvme_t* arguments);

/** The arguments of @c ustore_database_capabilities. */
typedef struct ustore_database_capabilities_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Output for the capabilities of the engine. */
    ustore_capabilities_t* capabilities;

} ustore_database_capabilities_t;

/**
 *  @brief Reports the capabilities of the engine.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_database_capabilities(ustore_database_capabilities_t* arguments);

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

/** The arguments of @c ustore_snapshot_init. */
typedef struct ustore_snapshot_init_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Output for the new snapshot, which the caller frees with @c ustore_snapshot_free. */
    ustore_snapshot_t* snapshot;

} ustore_snapshot_init_t;

/**
 *  @brief Pins a point-in-time view of the database until @c ustore_snapshot_free releases it.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_snapshot_init(ustore_snapshot_init_t* arguments);

/** The arguments of @c ustore_snapshot_export. */
typedef struct ustore_snapshot_export_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** The snapshot to export. */
    ustore_snapshot_t snapshot;

    /** Directory that receives the exported database. */
    ustore_str_view_t path;

} ustore_snapshot_export_t;

/**
 *  @brief Exports a snapshot as a new database.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_snapshot_export(ustore_snapshot_export_t* arguments);

#pragma endregion Snapshots

#pragma region Collections

/** What a collection holds, recorded beside its name so a view of another kind is refused. */
typedef enum ustore_modality_t {

    /** Anything, which the main collection and every collection created without a modality hold. */
    ustore_modality_any_k = 0,

    /** Binary values of @c blobs.h. */
    ustore_modality_blobs_k = 1,

    /** JSON documents of @c docs.h. */
    ustore_modality_docs_k = 2,

    /** Vertices and their edges of @c graph.h. */
    ustore_modality_graph_k = 3,

    /** Values keyed by strings of @c strings.h. */
    ustore_modality_strings_k = 4,

    /** Ordered documents of @c pages.h. */
    ustore_modality_pages_k = 5,

} ustore_modality_t;

/**
 *  @brief The arguments of @c ustore_collection_list, which exports every collection with its name and modality.
 *
 *  The main collection is never listed, and the order of the others is the engine's. Listing is the only collection
 *  operation beside @c ustore_collection_find that runs on a snapshot.
 */
typedef struct ustore_collection_list_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Transaction whose view is listed. */
    ustore_transaction_t transaction;

    /** Optional snapshot whose view is listed, never set beside a transaction. */
    ustore_snapshot_t snapshot;

    /** Reusable memory for the results. */
    ustore_arena_t* arena;

    /** Accepts @c ustore_option_dont_discard_memory_k. */
    ustore_options_t options;

    /** Output for the number of collections. */
    ustore_size_t* count;

    /** Optional output for the identifiers of the collections. */
    ustore_collection_id_t** collections;

    /** Optional output for what every collection holds. */
    ustore_modality_t** modalities;

    /** Optional output for @c count plus one offsets into @c names, where a collection without a name has an empty
     *  one. */
    ustore_length_t** names_offsets;

    /** Optional output for the tape of names, in the order of the identifiers. */
    ustore_char_t** names;

} ustore_collection_list_t;

/**
 *  @brief Lists the collections of the database.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_collection_list(ustore_collection_list_t* arguments);

/**
 *  @brief The arguments of @c ustore_collection_create, which always creates an empty collection and answers the
 *      identifier addressing it.
 *
 *  Runs at the head of the database. A name is recorded with the collection in one step and survives a crash beside
 *  it, and a name another collection carries, even one a concurrent creator took first, fails the call with
 *  @c ustore_error_already_exists_k and creates nothing. Each engine keeps the names apart from every collection,
 *  so no identifier reaches them, and a name resolves in two phases: once through @c ustore_collection_find, and
 *  from then on by identifier, as no other call carries one.
 */
typedef struct ustore_collection_create_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Optional name no other collection carries, where @c NULL creates a collection without one. */
    ustore_str_view_t name;

    /** Length of the name, inferred from a null terminator when zero. */
    ustore_length_t name_length;

    /** What the collection holds, which fixes how it is keyed and so which calls reach it. */
    ustore_modality_t modality;

    /** Output for the identifier of the collection. */
    ustore_collection_id_t* collection;

} ustore_collection_create_t;

/**
 *  @brief Creates an empty collection, named or not.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_collection_create(ustore_collection_create_t* arguments);

/**
 *  @brief The arguments of @c ustore_collection_find, which reads the collections a batch of names address.
 *
 *  Names are read at the head of the database or at a snapshot, never inside a transaction, so resolving a name
 *  never joins the conflicts of the work that follows it. A name no collection carries answers
 *  @c ustore_collection_main_k, @c ustore_modality_any_k and an unset presence bit.
 */
typedef struct ustore_collection_find_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Optional snapshot whose names are read, where @c NULL reads the head. */
    ustore_snapshot_t snapshot;

    /** Reusable memory for the results. */
    ustore_arena_t* arena;

    /** Accepts @c ustore_option_dont_discard_memory_k. */
    ustore_options_t options;

    /** Number of tasks, one per name. */
    ustore_size_t tasks_count;

    /** Names to resolve, @c NULL only when there are no tasks. */
    ustore_str_view_t const* names;

    /** Bytes between consecutive name pointers, where zero repeats the first one. */
    ustore_size_t names_stride;

    /** Optional lengths of the names, inferred from null terminators when absent. */
    ustore_length_t const* names_lengths;

    /** Bytes between consecutive name lengths, where zero repeats the first one. */
    ustore_size_t names_lengths_stride;

    /** Optional output for a bitset of at least @c tasks_count bits marking the names a collection carries,
     *  addressed as Apache Arrow does. */
    ustore_octet_t** presences;

    /** Output for the identifiers of the collections the names address. */
    ustore_collection_id_t** collections;

    /** Optional output for what the collections hold. */
    ustore_modality_t** modalities;

} ustore_collection_find_t;

/**
 *  @brief Reads the collections a batch of names address.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_collection_find(ustore_collection_find_t* arguments);

/** The arguments of @c ustore_collection_drop, where the main collection can only be cleared, and dropping a
 *  collection whole forgets its name before the collection goes. */
typedef struct ustore_collection_drop_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Identifier of an existing collection. */
    ustore_collection_id_t collection;

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

    /** Optional output for the stamp the stage took. */
    ustore_stamp_t* stamp;

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

    /** Optional output for the stamp the commit published under. */
    ustore_stamp_t* stamp;

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
 *  @brief Releases a snapshot, unpinning the versions only it kept, and frees its memory.
 *  @param[inout] snapshot The snapshot to be freed; @c NULL is safe.
 */
void ustore_snapshot_free(ustore_snapshot_t snapshot);

/**
 *  @brief Closes the database and frees its memory, while the data persists on disk.
 *  @param[inout] database The database to be closed; @c NULL is safe.
 */
void ustore_database_free(ustore_database_t database);

#pragma endregion Memory Reclamation

#ifdef __cplusplus
}
#endif
