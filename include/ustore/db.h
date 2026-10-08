/**
 *  @file include/ustore/db.h
 *  @author Ash Vardanian
 *  @date June 12, 2022
 *  @brief The binary interface every engine implements: databases, snapshots, transactions, views and
 *      collections.
 */

#pragma once

#include "ustore/types.h"

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

/** An opaque handle to a point-in-time state of the whole database, which reads address through
 *  @c ustore_snapshot_view. Short-lived: it pins the versions its reads need and holds its database open until
 *  @c ustore_snapshot_free. */
typedef struct ustore_snapshot_handle_t* ustore_snapshot_t;

/** An opaque handle to an optimistic transaction that groups operations across collections and
 *  modalities. It serves one call at a time, so a call on a transaction that serves an unfinished one finishes
 *  with @c ustore_status_contended_k, and holds its database open until @c ustore_transaction_free. */
typedef struct ustore_transaction_handle_t* ustore_transaction_t;

/**
 *  @brief An opaque handle to what a read sees: the head of the database, a transaction or a snapshot.
 *
 *  @c NULL is the head. Others come from @c ustore_transaction_view and @c ustore_snapshot_view, which tag the
 *  parent's pointer and allocate nothing, so a view needs no free and dies with its parent. A view names its
 *  parent, not a moment: after a commit, a transaction's view reads the transaction's next generation. Writes take a
 *  transaction instead, so nothing can write through a snapshot.
 */
typedef struct ustore_view_handle_t* ustore_view_t;

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

/** The smallest bitset, holding eight bits. */
typedef uint8_t ustore_octet_t;

/** A position in the order commits apply in, which grows with every commit: where a commit landed, or the newest
 *  commit a transaction or a snapshot reads. */
typedef uint64_t ustore_stamp_t;

/** A string the caller owns and the database never frees. */
typedef char const* ustore_str_view_t;

/** A mutable string the caller owns and the database never frees. */
typedef char* ustore_str_span_t;

/** A pointer to mutable bytes. */
typedef uint8_t* ustore_bytes_ptr_t;

/** A pointer to read-only bytes. */
typedef uint8_t const* ustore_bytes_cptr_t;

/** How durable the writes a call publishes are once the call finishes. */
typedef enum ustore_durability_t {

    /** The writes are visible, and reach the disk as the engine's buffers fill. */
    ustore_durability_buffered_k = 0,

    /** The writes are on disk, at a cost that varies by engine and drive. */
    ustore_durability_flushed_k = 1,

} ustore_durability_t;

/** Whether a transaction records the keys a call reads or writes, to refuse its commit when another writer
 *  changes them first. */
typedef enum ustore_watch_t {

    /** Records the keys, so a concurrent change to any of them fails the commit with a conflict. */
    ustore_watch_keys_k = 0,

    /** Records nothing, so concurrent changes to these keys never fail the commit. */
    ustore_watch_none_k = 1,

} ustore_watch_t;

/** In what order a scan hands its keys over. */
typedef enum ustore_scan_mode_t {

    /** Keys come in order, each once, and only while present. */
    ustore_scan_ordered_k = 0,

    /** Keys may come out of order, deleted or repeated, to speed up the read after the scan; has no effect yet. */
    ustore_scan_unordered_k = 1,

} ustore_scan_mode_t;

/** What dropping a collection removes. */
typedef enum ustore_drop_mode_t {

    /** The collection together with its name and every pair it holds. */
    ustore_drop_collection_k = 0,

    /** Every key and value, keeping the collection. */
    ustore_drop_pairs_k = 1,

    /** Every value, keeping the keys. */
    ustore_drop_values_k = 2,

} ustore_drop_mode_t;

/** Capabilities an engine reports. */
typedef enum ustore_capabilities_t {

    /** The engine supports transactions. */
    ustore_capabilities_transactions_k = 1,

    /** The engine supports collections beyond the main one. */
    ustore_capabilities_collections_k = 2,

    /** The engine supports snapshots. */
    ustore_capabilities_snapshots_k = 4,

} ustore_capabilities_t;

/** The collection every database holds, named by the nil UUID, which @c ustore_drop_pairs_k and
 *  @c ustore_drop_values_k may only clear. */
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

    /** Output for the opened database, left @c NULL on failure. */
    ustore_database_t* database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** Directory the database persists in as one image, or null to keep it in memory alone. */
    ustore_str_view_t directory;

    /** How transactions see each other. */
    ustore_isolation_t isolation;

} ustore_database_init_ram_t;

/**
 *  @brief Opens the in-memory engine, which persists as one image when it names a directory.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_database_init_ram(ustore_database_init_ram_t* arguments);

/** The arguments of @c ustore_database_init_rocksdb. */
typedef struct ustore_database_init_rocksdb_t {

    /** Output for the opened database, left @c NULL on failure. */
    ustore_database_t* database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** Directory of the database, which must exist. */
    ustore_str_view_t directory;

    /** Directories the table files fill in order, or null to keep them in @c directory. */
    ustore_data_directory_t const* data_directories;

    /** Entries of @c data_directories. */
    ustore_size_t data_directories_count;

} ustore_database_init_rocksdb_t;

/** Opens the RocksDB engine, or reports @c ustore_status_unsupported_k in a library built without it. */
USTORE_EXPORT void ustore_database_init_rocksdb(ustore_database_init_rocksdb_t* arguments);

/** The arguments of @c ustore_database_init_leveldb. */
typedef struct ustore_database_init_leveldb_t {

    /** Output for the opened database, left @c NULL on failure. */
    ustore_database_t* database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** Directory of the database, which must exist. */
    ustore_str_view_t directory;

} ustore_database_init_leveldb_t;

/** Opens the LevelDB engine, or reports @c ustore_status_unsupported_k in a library built without it. */
USTORE_EXPORT void ustore_database_init_leveldb(ustore_database_init_leveldb_t* arguments);

/** The arguments of @c ustore_database_init_nvme, where every zero size or count keeps the engine's default. */
typedef struct ustore_database_init_nvme_t {

    /** Output for the opened database, left @c NULL on failure. */
    ustore_database_t* database;

    /** Executor that runs the call, then the background flushes and compactions, made by
     *  @c ustore_executor_for_nvme and borrowed until the database closes, where @c NULL spawns one with that
     *  call's defaults. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

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

    /** CUDA devices compaction may run on, or null for none. */
    int const* gpu_devices;

    /** Entries of @c gpu_devices. */
    ustore_size_t gpu_devices_count;

    /** Bytes of device memory compaction may take on each of @c gpu_devices. */
    ustore_size_t gpu_memory_limit;

    /** I/O channels the threads share beyond their own. */
    ustore_size_t shared_io_channels;

    /** Submissions a channel holds at once. */
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

/** Opens the NVMe engine, or reports @c ustore_status_unsupported_k in a library built without it. */
USTORE_EXPORT void ustore_database_init_nvme(ustore_database_init_nvme_t* arguments);

/** What an open database reports about itself in one call. */
typedef struct ustore_database_description_t {

    /** The capabilities of the engine. */
    ustore_capabilities_t capabilities;

} ustore_database_description_t;

/** The arguments of @c ustore_database_describe. */
typedef struct ustore_database_describe_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** Output for the description, filled whole. */
    ustore_database_description_t* description;

} ustore_database_describe_t;

/**
 *  @brief Reports what the engine of an open database supports.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_database_describe(ustore_database_describe_t* arguments);

/**
 *  @brief The arguments of @c ustore_database_control, a free-form tunnel to the engine.
 *
 *  Requests have no stable format and are expensive: "clear" removes all data and keeps the
 *  collections, "reset" removes both, "compact" flushes and compacts LSM trees, "info" describes the build,
 *  and "usage" estimates the sizes of collections and the use of memory and disk.
 */
typedef struct ustore_database_control_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** The request as a null-terminated string. */
    ustore_str_view_t request;

    /** Output for the response as a null-terminated string, inside @c arena. */
    ustore_str_view_t* response;

    /** Memory for @c response: reused when it points to an arena, created when it points to @c NULL. */
    ustore_arena_t* arena;

    /** Optional callback reporting the bytes a long request, such as a compaction, has processed. */
    ustore_progress_callback_t progress;

    /** Opaque pointer passed to @c progress. */
    void* progress_state;

} ustore_database_control_t;

/**
 *  @brief Sends a free-form request to the engine.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_database_control(ustore_database_control_t* arguments);

#pragma endregion Database

#pragma region Snapshots and Views

/** The arguments of @c ustore_snapshot_init. */
typedef struct ustore_snapshot_init_t {

    /** Output for the new snapshot, which the caller frees with @c ustore_snapshot_free. */
    ustore_snapshot_t* snapshot;

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** Optional output for the stamp the snapshot reads at. */
    ustore_stamp_t* read_stamp;

} ustore_snapshot_init_t;

/**
 *  @brief Pins a point-in-time state of the database until @c ustore_snapshot_free releases it.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_snapshot_init(ustore_snapshot_init_t* arguments);

/** The arguments of @c ustore_snapshot_export. */
typedef struct ustore_snapshot_export_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** The snapshot to export. */
    ustore_snapshot_t snapshot;

    /** Directory that receives the exported database. */
    ustore_str_view_t path;

    /** Optional callback reporting the bytes exported. */
    ustore_progress_callback_t progress;

    /** Opaque pointer passed to @c progress. */
    void* progress_state;

} ustore_snapshot_export_t;

/**
 *  @brief Exports a snapshot as a new database.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_snapshot_export(ustore_snapshot_export_t* arguments);

/**
 *  @brief Names a snapshot as the view a read sees, without allocating.
 *  @param[in] snapshot A snapshot, which the view dies with.
 *  @return The view, or @c NULL, the head, for a @c NULL snapshot.
 */
USTORE_EXPORT ustore_view_t ustore_snapshot_view(ustore_snapshot_t snapshot);

#pragma endregion Snapshots and Views

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
 *  The main collection is never listed, and the order of the others is the engine's. A listing describes the
 *  database, never a transaction, so it reads the head or a snapshot.
 */
typedef struct ustore_collection_list_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** Optional snapshot whose collections are listed, where @c NULL lists the head. */
    ustore_snapshot_t snapshot;

    /** Output for the number of collections. */
    ustore_size_t* count;

    /** Optional output for the identifiers of the collections, inside @c arena. */
    ustore_collection_id_t** collections;

    /** Optional output for what every collection holds, inside @c arena. */
    ustore_modality_t** modalities;

    /** Optional output for @c count plus one offsets into @c names, inside @c arena, where a collection without a
     *  name has an empty one. */
    ustore_length_t** names_offsets;

    /** Optional output for the tape of names, in the order of the identifiers, inside @c arena. */
    ustore_char_t** names;

    /** Memory for @c collections, @c modalities, @c names_offsets and @c names: reused when it points to an arena,
     *  created when it points to @c NULL. */
    ustore_arena_t* arena;

} ustore_collection_list_t;

/**
 *  @brief Lists the collections of the database.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_collection_list(ustore_collection_list_t* arguments);

/**
 *  @brief The arguments of @c ustore_collection_create, which always creates an empty collection and answers the
 *      identifier addressing it.
 *
 *  Runs at the head of the database. A name is recorded with the collection in one step and survives a crash beside
 *  it, and a name another collection carries, even one a concurrent creator took first, fails the call with
 *  @c ustore_status_already_exists_k and creates nothing. Each engine keeps the names apart from every collection,
 *  so no identifier reaches them, and a name resolves in two phases: once through @c ustore_collection_find, and
 *  from then on by identifier, as no other call carries one.
 */
typedef struct ustore_collection_create_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

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
USTORE_EXPORT void ustore_collection_create(ustore_collection_create_t* arguments);

/**
 *  @brief The arguments of @c ustore_collection_find, which reads the collections a batch of names address.
 *
 *  Collections are created and dropped only at the head, so their names are read at the head or at a snapshot and
 *  never inside a transaction, and resolving a name never joins the conflicts of the work that follows it. A name no
 *  collection carries answers @c ustore_collection_main_k, @c ustore_modality_any_k and an unset presence bit. Every
 *  output is a span of the caller's, sized by @c count.
 */
typedef struct ustore_collection_find_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** Optional snapshot whose names are read, where @c NULL reads the head. */
    ustore_snapshot_t snapshot;

    /** Number of tasks, one per name. */
    ustore_size_t count;

    /** Names to resolve, @c NULL only when there are no tasks. */
    ustore_str_view_t const* names;

    /** Bytes between consecutive name pointers, where zero repeats the first one. */
    ustore_size_t names_stride;

    /** Optional lengths of the names, inferred from null terminators when absent. */
    ustore_length_t const* names_lengths;

    /** Bytes between consecutive name lengths, where zero repeats the first one. */
    ustore_size_t names_lengths_stride;

    /** Optional output for a bitset of at least @c count bits marking the names a collection carries,
     *  addressed as Apache Arrow does. */
    ustore_octet_t* presences;

    /** Output for the identifiers of the collections the names address. */
    ustore_collection_id_t* collections;

    /** Bytes between consecutive output identifiers, never zero. */
    ustore_size_t collections_stride;

    /** Optional output for what the collections hold. */
    ustore_modality_t* modalities;

    /** Bytes between consecutive output modalities, never zero. */
    ustore_size_t modalities_stride;

} ustore_collection_find_t;

/**
 *  @brief Reads the collections a batch of names address.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_collection_find(ustore_collection_find_t* arguments);

/** The arguments of @c ustore_collection_drop, where the main collection can only be cleared, and dropping a
 *  collection whole forgets its name before the collection goes. */
typedef struct ustore_collection_drop_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** Identifier of an existing collection. */
    ustore_collection_id_t collection;

    /** Whether values, pairs or the whole collection go. */
    ustore_drop_mode_t mode;

} ustore_collection_drop_t;

/**
 *  @brief Removes or clears a collection.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_collection_drop(ustore_collection_drop_t* arguments);

#pragma endregion Collections

#pragma region Transactions

/** The arguments of @c ustore_transaction_init. */
typedef struct ustore_transaction_init_t {

    /** The transaction to begin or to reset for reuse. */
    ustore_transaction_t* transaction;

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

} ustore_transaction_init_t;

/**
 *  @brief Begins a transaction, or resets an existing one.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_transaction_init(ustore_transaction_init_t* arguments);

/**
 *  @brief Names a transaction as the view a read sees, without allocating.
 *  @param[in] transaction A transaction, which the view dies with.
 *  @param[in] watch Whether reads through the view record their keys for conflicts.
 *  @return The view, or @c NULL, the head, for a @c NULL transaction.
 */
USTORE_EXPORT ustore_view_t ustore_transaction_view(ustore_transaction_t transaction, ustore_watch_t watch);

/** The arguments of @c ustore_transaction_stage, which keeps the contents for logging or retries
 *  whatever the outcome. */
typedef struct ustore_transaction_stage_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** An initialized transaction. */
    ustore_transaction_t transaction;

    /** Optional output for the stamp the transaction reads at, which its commit validates against. */
    ustore_stamp_t* read_stamp;

} ustore_transaction_stage_t;

/**
 *  @brief Stages a transaction as the first phase of a two-phase commit.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_transaction_stage(ustore_transaction_stage_t* arguments);

/** The arguments of @c ustore_transaction_commit, which keeps the contents for logging or retries
 *  whatever the outcome. */
typedef struct ustore_transaction_commit_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** An initialized transaction, staged first when the caller did not. */
    ustore_transaction_t transaction;

    /** Optional output for the commit's position in commit order, visible to every snapshot at or above it. */
    ustore_stamp_t* commit_stamp;

    /** How durable the writes are once the commit finishes. */
    ustore_durability_t durability;

} ustore_transaction_commit_t;

/**
 *  @brief Commits a transaction.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_transaction_commit(ustore_transaction_commit_t* arguments);

/** The arguments of @c ustore_transaction_rollback. */
typedef struct ustore_transaction_rollback_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** A transaction, staged or not. */
    ustore_transaction_t transaction;

} ustore_transaction_rollback_t;

/**
 *  @brief Withdraws a stage, releasing whatever it claimed, while keeping the reads and writes, so the transaction
 *      can stage again or be reset through @c ustore_transaction_init.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_transaction_rollback(ustore_transaction_rollback_t* arguments);

#pragma endregion Transactions

#pragma region Memory Reclamation

/**
 *  @brief Waits until every deferred call naming the transaction finishes, then resets it and frees its memory with
 *      every view of it.
 *  @param[inout] transaction The transaction to be freed; @c NULL is safe.
 */
USTORE_EXPORT void ustore_transaction_free(ustore_transaction_t transaction);

/**
 *  @brief Waits until every deferred call naming the snapshot finishes, then releases it, unpinning the versions
 *      only it kept, and frees its memory with every view of it.
 *  @param[inout] snapshot The snapshot to be freed; @c NULL is safe.
 */
USTORE_EXPORT void ustore_snapshot_free(ustore_snapshot_t snapshot);

/**
 *  @brief Waits until every deferred call naming the database finishes, then closes it and frees its memory once
 *      no transaction or snapshot of it holds it open, while the data persists on disk.
 *  @param[inout] database The database to be closed; @c NULL is safe.
 */
USTORE_EXPORT void ustore_database_free(ustore_database_t database);

#pragma endregion Memory Reclamation

#ifdef __cplusplus
}
#endif
