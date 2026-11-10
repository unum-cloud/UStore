/**
 *  @file c/ustore/db.h
 *  @author Ash Vardanian
 *  @date June 12, 2022
 *  @brief The binary interface every engine implements: databases, snapshots, transactions, views and
 *      collections.
 */

#ifndef UNUM_USTORE_DB_H
#define UNUM_USTORE_DB_H

#include "ustore/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 *  @brief An opaque handle to an open database, safe to share across threads from open until free.
 *
 *  An embedded database owns its directory, so no two processes may open the same one at once. A database holds no
 *  collection until one is created, and every call addresses the collections it created by identifier.
 */
typedef struct ustore_database_handle_t* ustore_database_t;

/** An opaque handle to a point-in-time state of the whole database, which reads address through
 *  @c ustore_snapshot_view. Short-lived: it pins the versions its reads need and holds its database
 *  open until @c ustore_snapshot_free. */
typedef struct ustore_snapshot_handle_t* ustore_snapshot_t;

/** An opaque handle to an optimistic transaction that groups operations across collections and
 *  modalities. It serves one call at a time, so a call on a transaction that serves an unfinished one finishes
 *  with @c ustore_status_contended_k, and holds its database open until @c ustore_transaction_free. */
typedef struct ustore_transaction_handle_t* ustore_transaction_t;

/**
 *  @brief An opaque handle to what a read sees: the head of the database, a transaction or a snapshot.
 *
 *  @c NULL is the head. Others come from @c ustore_transaction_view and @c ustore_snapshot_view, which tag the
 *  parent's pointer and allocate nothing, so a view needs no free and dies with its parent, which a cursor reading
 *  through it holds until the cursor is freed. A view names its parent, not a moment: after a commit, a transaction's
 *  view reads the transaction's next generation. Writes take a transaction instead, so nothing can write through a
 *  snapshot.
 */
typedef struct ustore_view_handle_t* ustore_view_t;

/**
 *  @brief Sixteen bytes naming one thing everywhere, ordered as their bytes and compared with @c memcmp.
 *
 *  An integer spells as its 128-bit big-endian bytes, as Python's <tt>uuid.UUID(int=...)</tt> does,
 *  so identifiers made from integers order as the integers.
 */
typedef struct ustore_uuid_t {
    uint8_t bytes[16];
} ustore_uuid_t;

/** The identifier of a collection: a UUIDv7 its create mints once and never reuses, so it stays the same across
 *  restarts and never names two collections. The nil UUID names none. */
typedef struct ustore_collection_id_t {
    ustore_uuid_t uuid;
} ustore_collection_id_t;

/** The identifier of a value within one collection. */
typedef struct ustore_key_t {
    ustore_uuid_t uuid;
} ustore_key_t;

/** The elementary piece of a binary value. */
typedef uint8_t ustore_byte_t;

/** The length of a value in bytes, or an offset into a run of keys, wide enough for a run past 4 GB. */
typedef uint64_t ustore_length_t;

/** The smallest bitset, holding eight bits. */
typedef uint8_t ustore_octet_t;

/** A position in the order commits apply in, which grows with every commit: where a commit landed, or the newest
 *  commit a transaction or a snapshot reads. */
typedef uint64_t ustore_stamp_t;

/** A strided run of keys, one per task, where a zero stride repeats the first. */
typedef struct ustore_keys_view_t {

    /** First key of the run. */
    ustore_key_t const* data;

    /** Bytes between consecutive keys, where zero repeats the first one. */
    ustore_size_t stride;

} ustore_keys_view_t;

/** A strided run of collections, one per task, whose @c NULL data a call refuses with
 *  @c ustore_status_invalid_argument_k, as no collection was named. */
typedef struct ustore_collections_view_t {

    /** First collection of the run. */
    ustore_collection_id_t const* data;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t stride;

} ustore_collections_view_t;

/** A strided run of lengths or positions, one per task. */
typedef struct ustore_lengths_view_t {

    /** First length of the run. */
    ustore_length_t const* data;

    /** Bytes between consecutive lengths, where zero repeats the first one. */
    ustore_size_t stride;

} ustore_lengths_view_t;

/** Whether a predicate keeps a listed key or passes over it, numbered as USearch numbers its admissions. */
typedef enum ustore_admission_t {

    /** Pass over the key. */
    ustore_reject_k = 0,

    /** Keep the key among the listed ones. */
    ustore_admit_k = 1,

} ustore_admission_t;

/** Admits or rejects @p count listed keys at once, never zero, writing one admission per key into @p admissions, a
 *  buffer the library owns for the call. Runs on the thread running the call, never concurrently with itself within
 *  one call. */
typedef void (*ustore_predicate_callback_t)(ustore_key_t const* keys, ustore_admission_t* admissions,
                                            ustore_size_t count, void* state);

/** How costly one predicate call is, which decides where in a scan it is asked. */
typedef enum ustore_predicate_cost_t {

    /** Asked once per page of listed keys, before patterns or filters read any value, so it narrows those reads. */
    ustore_predicate_cheap_k = 0,

    /** Asked once per page about the keys patterns and filters kept, for handlers that pay per call, such as a lookup
     *  in an external database. */
    ustore_predicate_expensive_k = 1,

} ustore_predicate_cost_t;

/** The canonical spelling of a predicate cost, as @c ustore_predicate_expensive_k spells "expensive", or
 *  "unrecognized". */
USTORE_EXPORT char const* ustore_predicate_cost_name(ustore_predicate_cost_t cost);

/** Reads a predicate cost back from its canonical spelling, or fails with
 *  @c ustore_status_invalid_argument_k. */
USTORE_EXPORT ustore_status_t ustore_predicate_cost_from_name(ustore_bytes_view_t name, ustore_predicate_cost_t* cost);

/** The caller's verdict on the keys a scan lists, always asked on the host, as a scan on a device executor lists its
 *  pages there too. */
typedef struct ustore_predicate_t {

    /** The optional handler, where @c NULL admits every key. */
    ustore_predicate_callback_t callback;

    /** Opaque pointer passed to every @c callback call. */
    void* state;

    /** Where in a scan @c callback is asked. */
    ustore_predicate_cost_t cost;

} ustore_predicate_t;

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

    /** Keys may come out of order, deleted or repeated, as each partition or run is walked on its own with no merge
     *  across them, so a page of fewer than all keys cannot resume where it ended. */
    ustore_scan_unordered_k = 1,

} ustore_scan_mode_t;

/** Which way a scan, a cursor or a measure walks a range from its start key. */
typedef enum ustore_scan_direction_t {

    /** Up from the start key, inclusive, to the end key, exclusive, smallest key first. */
    ustore_scan_ascending_k = 0,

    /** Down from the start key, inclusive, to the end key, exclusive, largest key first. */
    ustore_scan_descending_k = 1,

} ustore_scan_direction_t;

/** What dropping a collection removes. */
typedef enum ustore_drop_mode_t {

    /** The collection together with its name and every pair it holds. */
    ustore_drop_collection_k = 0,

    /** Every key and value, keeping the collection. */
    ustore_drop_pairs_k = 1,

    /** Every value, keeping the keys, which a strings collection refuses, as its values are keys.
     *  @c ustore_database_clear with this mode leaves strings collections untouched. */
    ustore_drop_values_k = 2,

} ustore_drop_mode_t;

/** Capabilities an engine reports. */
typedef enum ustore_capabilities_t {

    /** The engine supports transactions. */
    ustore_capabilities_transactions_k = 1,

    /** The engine supports snapshots. */
    ustore_capabilities_snapshots_k = 4,

} ustore_capabilities_t;

/** The key that stands for no key, every bit of it set. */
static ustore_key_t const ustore_key_unknown_k = {
    {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}}};

/** How the transactions of the in-memory engine see each other's writes. */
typedef enum ustore_isolation_t {

    /** Every transaction appears to run alone at one moment between its start and its commit. */
    ustore_isolation_strict_serializable_k = 0,

    /** Every transaction reads the state its start saw and conflicts only over what it writes. */
    ustore_isolation_snapshot_k = 1,

} ustore_isolation_t;

/** The engine serving a database, as @c ustore_database_describe reports it. */
typedef enum ustore_engine_t {

    /** The in-memory engine of @c ustore_database_init_ram and @c ustore_database_open_ram. */
    ustore_engine_ram_k = 0,

    /** The RocksDB engine of @c ustore_database_open_rocksdb and @c ustore_database_view_rocksdb. */
    ustore_engine_rocksdb_k = 1,

    /** The LevelDB engine of @c ustore_database_open_leveldb. */
    ustore_engine_leveldb_k = 2,

    /** The NVMe engine of @c ustore_database_open_nvme. */
    ustore_engine_nvme_k = 3,

} ustore_engine_t;

#pragma region Database

/** A directory the data files may occupy, with the bytes they may take there. */
typedef struct ustore_data_directory_t {

    /** The directory, which must exist; on NVMe also a raw device from a base, as in
     *  @c /dev/nvme0n1@64GiB. */
    ustore_bytes_view_t path;

    /** Bytes the files may take there, zero being unlimited on RocksDB and the whole
     *  medium on NVMe. */
    ustore_size_t max_bytes;

} ustore_data_directory_t;

/** The arguments of @c ustore_database_init_ram. */
typedef struct ustore_database_init_ram_t {

    /** Output for the new database, left @c NULL on failure. */
    ustore_database_t* database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** How transactions see each other. */
    ustore_isolation_t isolation;

    /** The CPU class the database confines its waits, atomics and string kernels to, where zero takes what the CPU
     *  offers. */
    ustore_cpu_class_t cpu_class;

} ustore_database_init_ram_t;

/**
 *  @brief Starts an empty database of the in-memory engine, which persists nowhere.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_device_k The executor computes on a device.
 *  - @c ustore_status_contended_k The executor serves another call.
 *  - @c ustore_status_invalid_argument_k The @c database output is @c NULL, or the isolation or the
 *    CPU class is unknown.
 *  - @c ustore_status_unsupported_k The CPU does not run the @c cpu_class.
 */
USTORE_EXPORT void ustore_database_init_ram(ustore_database_init_ram_t* arguments);

/** The arguments of @c ustore_database_open_ram. */
typedef struct ustore_database_open_ram_t {

    /** Output for the opened database, left @c NULL on failure. */
    ustore_database_t* database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** Directory the database persists in as one image, which must exist. */
    ustore_bytes_view_t directory;

    /** How transactions see each other. */
    ustore_isolation_t isolation;

    /** The CPU class the database confines its waits, atomics and string kernels to, where zero takes what the CPU
     *  offers. */
    ustore_cpu_class_t cpu_class;

} ustore_database_open_ram_t;

/**
 *  @brief Serves a directory in place through the in-memory engine, loading the image it holds, if any, and rewriting
 *      it whole on a flushed commit and on close.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_device_k The executor computes on a device.
 *  - @c ustore_status_contended_k The executor serves another call.
 *  - @c ustore_status_invalid_argument_k The @c directory is empty, the @c database output
 *    is @c NULL, or the isolation or the CPU class is unknown.
 *  - @c ustore_status_unsupported_k The CPU does not run the @c cpu_class.
 *  - @c ustore_status_not_found_k The directory does not exist.
 *  - @c ustore_status_malformed_k The image has an unknown format, is truncated or names an unknown
 *    modality.
 */
USTORE_EXPORT void ustore_database_open_ram(ustore_database_open_ram_t* arguments);

/** The arguments of @c ustore_database_open_rocksdb. */
typedef struct ustore_database_open_rocksdb_t {

    /** Output for the opened database, left @c NULL on failure. */
    ustore_database_t* database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** Directory of the database, which must exist. */
    ustore_bytes_view_t directory;

    /** Directories the table files fill in order, or null to keep them in @c directory. */
    ustore_data_directory_t const* data_directories;

    /** Entries of @c data_directories. */
    ustore_size_t data_directories_count;

} ustore_database_open_rocksdb_t;

/**
 *  @brief Serves a directory in place through the RocksDB engine, creating the database if absent.
 *
 *  @par Statuses:
 *  - @c ustore_status_unsupported_k The library was built without the engine.
 *  - @c ustore_status_device_k The executor computes on a device.
 *  - @c ustore_status_contended_k The executor serves another call.
 *  - @c ustore_status_invalid_argument_k The @c database output is @c NULL, the @c directory is
 *    empty, or the counted @c data_directories are @c NULL or one has an empty path.
 *  - @c ustore_status_not_found_k The directory does not exist.
 *  - @c ustore_status_malformed_k A column family or a collections entry is of another layout or
 *    corrupt.
 */
USTORE_EXPORT void ustore_database_open_rocksdb(ustore_database_open_rocksdb_t* arguments);

/** The arguments of @c ustore_database_view_rocksdb. */
typedef struct ustore_database_view_rocksdb_t {

    /** Output for the opened database, left @c NULL on failure. */
    ustore_database_t* database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** Directory of an existing database. */
    ustore_bytes_view_t directory;

    /** Directories the table files fill, as the database was opened with, or null to find them in @c directory. */
    ustore_data_directory_t const* data_directories;

    /** Entries of @c data_directories. */
    ustore_size_t data_directories_count;

} ustore_database_view_rocksdb_t;

/**
 *  @brief Serves a directory in place through the RocksDB engine, read-only.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  The database reads the state its directory held at open, and refuses every call that would
 *  change it with @c ustore_status_read_only_k: writes, removes, transactions, collection creates
 *  and drops, clears and compactions. Snapshots and their saves still serve.
 *
 *  @par Statuses:
 *  - @c ustore_status_unsupported_k The library was built without the engine.
 *  - @c ustore_status_device_k The executor computes on a device.
 *  - @c ustore_status_contended_k The executor serves another call.
 *  - @c ustore_status_invalid_argument_k The @c database output is @c NULL, the @c directory is
 *    empty, or the counted @c data_directories are @c NULL or one has an empty path.
 *  - @c ustore_status_not_found_k The directory does not exist.
 *  - @c ustore_status_malformed_k A column family or a collections entry is of another layout or
 *    corrupt, the system collections are missing, or a crashed create or drop left a family behind.
 */
USTORE_EXPORT void ustore_database_view_rocksdb(ustore_database_view_rocksdb_t* arguments);

/** The arguments of @c ustore_database_open_leveldb. */
typedef struct ustore_database_open_leveldb_t {

    /** Output for the opened database, left @c NULL on failure. */
    ustore_database_t* database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** Directory of the database, which must exist. */
    ustore_bytes_view_t directory;

} ustore_database_open_leveldb_t;

/**
 *  @brief Serves a directory in place through the LevelDB engine, creating the database if absent.
 *
 *  @par Statuses:
 *  - @c ustore_status_unsupported_k The library was built without the engine.
 *  - @c ustore_status_device_k The executor computes on a device.
 *  - @c ustore_status_contended_k The executor serves another call.
 *  - @c ustore_status_invalid_argument_k The @c database output is @c NULL, or the @c directory is
 *    empty.
 *  - @c ustore_status_not_found_k The directory does not exist.
 */
USTORE_EXPORT void ustore_database_open_leveldb(ustore_database_open_leveldb_t* arguments);

/** The arguments of @c ustore_database_open_nvme, where every zero size or count keeps the engine's default. */
typedef struct ustore_database_open_nvme_t {

    /** Output for the opened database, left @c NULL on failure. */
    ustore_database_t* database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. The database moves bytes as the
     *  placement of one made by @c ustore_executor_for_nvme names, read once here, and with that call's defaults for
     *  any other executor and for none. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** The manifest, <tt>\<database\>.ustore.json</tt>, whose directory must exist and holds the database's
     *  first data directory, or the raw address of its first slot, as in @c /dev/nvme0n1@64GiB. */
    ustore_bytes_view_t directory;

    /** Further data directories every run may land on, the one with the most free bytes taking
     *  it, or null. */
    ustore_data_directory_t const* data_directories;

    /** Entries of @c data_directories. */
    ustore_size_t data_directories_count;

    /** Executor of @c ustore_executor_for_vram whose device, scratch budget and GPUDirect Storage mode every merge of
     *  the database takes, read once here, so it may be freed afterwards; @c NULL merges on host threads. */
    ustore_executor_t compactions;

    /** Bytes of host memory the engine may pin and cache. */
    ustore_size_t memory_limit;

    /** Bytes a transaction may buffer before it must commit. */
    ustore_size_t transaction_max_bytes;

    /** Deltas a transaction may buffer before it must commit. */
    ustore_size_t transaction_max_deltas;

    /** Operations that may run on one collection at once. */
    ustore_size_t collection_concurrency_limit;

    /** Bytes of values each collection may cache. */
    ustore_size_t collection_cache_limit;

    /** Bytes of memory runs a collection holds before the commit that crosses them flushes level 0. */
    ustore_size_t collection_memory_runs_max_bytes;

    /** Deltas of memory runs a collection holds before the commit that crosses them flushes level 0. */
    ustore_size_t collection_memory_runs_max_deltas;

    /** How many times larger each level of a collection is than the one above it. */
    ustore_size_t collection_level_enlarge_factor;

    /** Bytes of every value of a collection whose values share one size, or zero for varying sizes. */
    ustore_size_t collection_value_size;

    /** Bytes of the largest value of a collection whose values vary in size. */
    ustore_size_t collection_value_max_size;

    /** The CPU class the database confines its waits, atomics and string kernels to, where zero takes what the CPU
     *  offers. */
    ustore_cpu_class_t cpu_class;

} ustore_database_open_nvme_t;

/**
 *  @brief Serves the database a manifest names through the NVMe engine, creating it when the
 *      manifest is absent.
 *
 *  @par Statuses:
 *  @c ustore_status_unsupported_k means the library was built without the engine, SPDK is asked
 *  for, the manifest or a slot has another format version, the @c compactions executor is given
 *  to a library built without a CUDA compiler or drives an AMD device, the CPU does not run the
 *  @c cpu_class, or the io_uring transport was named on a kernel without one.
 *
 *  - @c ustore_status_device_k The executor computes on a device, or the @c compactions executor is
 *    no VRAM one.
 *  - @c ustore_status_contended_k The executor serves another call.
 *  - @c ustore_status_invalid_argument_k The @c database output is @c NULL, the @c directory is
 *    empty or names no @c .ustore.json file nor raw address, the counted @c data_directories are
 *    @c NULL, one has an empty path or a path past the platform's limit, the transport or the CPU
 *    class is unknown, or a size or count is out of range.
 *  - @c ustore_status_out_of_memory_k The @c memory_limit refuses the arena or the channels.
 *  - @c ustore_status_not_found_k The manifest's directory does not exist.
 *  - @c ustore_status_already_exists_k The first slot of a raw medium belongs to something else.
 *  - @c ustore_status_capacity_exhausted_k The drives need more queue pairs than a disk holds,
 *    the @c memory_limit leaves no block per channel, or every shared channel is taken.
 *  - @c ustore_status_malformed_k The manifest is corrupt or names a SHA-256 a run's bytes miss,
 *    or raw data directories are listed in another order than the database was created with.
 */
USTORE_EXPORT void ustore_database_open_nvme(ustore_database_open_nvme_t* arguments);

/** What an open database reports about itself in one call, where every size is the engine's estimate, and zero where
 *  the engine keeps none. */
typedef struct ustore_database_description_t {

    /** The capabilities of the engine. */
    ustore_capabilities_t capabilities;

    /** The engine serving the database. */
    ustore_engine_t engine;

    /** How transactions see each other: what the RAM engine was opened with, and @c ustore_isolation_snapshot_k
     *  elsewhere. */
    ustore_isolation_t isolation;

    /** Keys across every collection. */
    ustore_size_t keys;

    /** Bytes of memory the engine holds for the data. */
    ustore_size_t memory_bytes;

    /** Bytes the files of the database take on disk. */
    ustore_size_t disk_bytes;

    /** Bytes of the NVMe engine's io_uring arena the locked-memory cap refused to pin, so each
     *  request pins its own. */
    ustore_size_t pin_refused_bytes;

    /** Requests the NVMe engine sent unpinned while its arena was pinned, as each crossed a
     *  gibibyte of it. */
    ustore_size_t crossing_requests;

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

    /** Output for the description, filled whole; zeroed on failure. */
    ustore_database_description_t* description;

} ustore_database_describe_t;

/**
 *  @brief Reports the engine of an open database, what it supports and how much it holds.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_contended_k The executor serves another call.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL.
 *  - @c ustore_status_unsupported_k The executor computes on a device.
 *  - @c ustore_status_invalid_argument_k The @c description output is @c NULL, on the RAM and NVMe
 *    engines.
 */
USTORE_EXPORT void ustore_database_describe(ustore_database_describe_t* arguments);

/** The arguments of @c ustore_database_clear, which empties every collection at once at the head. */
typedef struct ustore_database_clear_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** What goes: every pair, keeping the collections and their names; every collection with its name; or every
     *  value, keeping the keys where the engine can. */
    ustore_drop_mode_t mode;

} ustore_database_clear_t;

/**
 *  @brief Empties every collection at once, as @c ustore_collection_drop empties one.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  The RAM and RocksDB engines refuse to drop the collections while a snapshot is open, since the snapshot belongs
 *  to its holder.
 *
 *  @par Statuses:
 *  - @c ustore_status_contended_k The executor serves another call.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL.
 *  - @c ustore_status_read_only_k The database was opened read-only.
 *  - @c ustore_status_unsupported_k The executor computes on a device.
 *  - @c ustore_status_invalid_argument_k The @c mode is unknown, or a snapshot
 *    is open while @c ustore_drop_collection_k drops the collections on the RAM
 *    and RocksDB engines.
 *  - @c ustore_status_not_found_k A listed collection was dropped meanwhile, on the NVMe engine.
 *  - @c ustore_status_write_conflict_k Concurrent commits kept conflicting, on the RAM and NVMe
 *    engines.
 */
USTORE_EXPORT void ustore_database_clear(ustore_database_clear_t* arguments);

/** The arguments of @c ustore_database_compact. */
typedef struct ustore_database_compact_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** Optional callback reporting the compaction's advance. */
    ustore_progress_callback_t progress;

    /** Opaque pointer passed to @c progress. */
    void* progress_state;

} ustore_database_compact_t;

/**
 *  @brief Reclaims the space overwritten and removed data still takes: RocksDB and LevelDB flush and compact their
 *      trees, and the RAM engine frees the versions no reader needs and rewrites its image.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  The work runs one collection at a time, and its progress counts the bytes of the collections compacted where the
 *  engine measures them, as RocksDB does, and one unit per collection elsewhere. A cancelled compaction stops between
 *  collections, keeping what it compacted.
 *
 *  @par Statuses:
 *  - @c ustore_status_contended_k The executor serves another call.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL.
 *  - @c ustore_status_read_only_k The database was opened read-only.
 *  - @c ustore_status_unsupported_k The executor computes on a device.
 */
USTORE_EXPORT void ustore_database_compact(ustore_database_compact_t* arguments);

/** The arguments of @c ustore_database_flush. */
typedef struct ustore_database_flush_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

} ustore_database_flush_t;

/**
 *  @brief Makes every commit that returned before this call durable, as if each had asked for
 *      @c ustore_durability_flushed_k.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  LevelDB syncs only its current log, so a commit in a log it already rotated stays unsynced
 *  until its compaction lands.
 *
 *  @par Statuses:
 *  - @c ustore_status_contended_k The executor serves another call.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL.
 *  - @c ustore_status_read_only_k The database was opened read-only.
 *  - @c ustore_status_unsupported_k The executor computes on a device.
 */
USTORE_EXPORT void ustore_database_flush(ustore_database_flush_t* arguments);

/**
 *  @brief Waits until every deferred call naming the database finishes, then closes it and frees
 *      its memory once no transaction, snapshot or cursor of it holds it open, while the data
 *      persists on disk.
 *  @param[in,out] database The database to be closed; @c NULL is safe.
 */
USTORE_EXPORT void ustore_database_free(ustore_database_t database);

#pragma endregion Database

#pragma region Snapshots

/** The arguments of @c ustore_snapshot_init. */
typedef struct ustore_snapshot_init_t {

    /** Output for the new snapshot, which the caller frees with @c ustore_snapshot_free; @c NULL on failure. */
    ustore_snapshot_t* snapshot;

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** Optional output for the stamp the snapshot reads at; zeroed on failure. */
    ustore_stamp_t* read_stamp;

} ustore_snapshot_init_t;

/**
 *  @brief Pins a point-in-time state of the database until @c ustore_snapshot_free releases it.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_contended_k The executor serves another call.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL.
 *  - @c ustore_status_unsupported_k The executor computes on a device.
 *  - @c ustore_status_invalid_argument_k The @c snapshot output is @c NULL.
 */
USTORE_EXPORT void ustore_snapshot_init(ustore_snapshot_init_t* arguments);

/** The arguments of @c ustore_snapshot_save. */
typedef struct ustore_snapshot_save_t {

    /** The snapshot to save, which holds its database open. */
    ustore_snapshot_t snapshot;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** Directory that receives the saved database, or on NVMe its manifest,
     *  <tt>\<database\>.ustore.json</tt>, or the raw address of its first slot. */
    ustore_bytes_view_t path;

    /** Optional callback reporting the bytes saved. */
    ustore_progress_callback_t progress;

    /** Opaque pointer passed to @c progress. */
    void* progress_state;

} ustore_snapshot_save_t;

/**
 *  @brief Writes a snapshot out as a new database of the same engine, which that engine's open then serves.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  The copy holds exactly the collections and names the snapshot records, whatever changed since.
 *  RocksDB and LevelDB create @c path, while the RAM engine writes its image into the directory
 *  @c path names. Its progress counts the bytes of the keys and values saved, and a save its
 *  progress callback cancels removes what it wrote, as a failed one does. The NVMe engine names the
 *  SHA-256 of every run in the saved manifest, which the first open of the copy checks before it
 *  reads and then drops from every manifest it publishes.
 *
 *  @par Statuses:
 *  - @c ustore_status_contended_k The executor serves another call.
 *  - @c ustore_status_uninitialized_k The @c snapshot is @c NULL.
 *  - @c ustore_status_unsupported_k The executor computes on a device.
 *  - @c ustore_status_invalid_argument_k The @c path is empty on the NVMe engine, or RocksDB finds a
 *    database there already.
 *  - @c ustore_status_not_found_k The @c path is no directory on the RAM engine, or a collection was
 *    dropped meanwhile on the NVMe engine.
 */
USTORE_EXPORT void ustore_snapshot_save(ustore_snapshot_save_t* arguments);

/**
 *  @brief Names a snapshot as the view a read sees, without allocating.
 *  @param[in] snapshot A snapshot, which the view dies with.
 *  @return The view, or @c NULL, the head, for a @c NULL snapshot.
 */
USTORE_EXPORT ustore_view_t ustore_snapshot_view(ustore_snapshot_t snapshot);

/**
 *  @brief Waits until every deferred call naming the snapshot finishes, then releases it,
 *      unpinning the versions only it kept, and frees its memory with every view of it once no
 *      cursor reads through it.
 *  @param[in,out] snapshot The snapshot to be freed; @c NULL is safe.
 */
USTORE_EXPORT void ustore_snapshot_free(ustore_snapshot_t snapshot);

#pragma endregion Snapshots

/** What a collection holds, recorded beside its name so a view of another kind is refused. Zero names no modality. */
typedef enum ustore_modality_t {

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

/** The canonical spelling of a durability, as @c ustore_durability_flushed_k spells "flushed", or "unrecognized". */
USTORE_EXPORT char const* ustore_durability_name(ustore_durability_t durability);

/** Reads a durability back from its canonical spelling, or fails with
 *  @c ustore_status_invalid_argument_k. */
USTORE_EXPORT ustore_status_t ustore_durability_from_name(ustore_bytes_view_t name, ustore_durability_t* durability);

/** The canonical spelling of a watch, as @c ustore_watch_none_k spells "none", or "unrecognized". */
USTORE_EXPORT char const* ustore_watch_name(ustore_watch_t watch);

/** Reads a watch back from its canonical spelling, or fails with
 *  @c ustore_status_invalid_argument_k. */
USTORE_EXPORT ustore_status_t ustore_watch_from_name(ustore_bytes_view_t name, ustore_watch_t* watch);

/** The canonical spelling of a scan mode, as @c ustore_scan_unordered_k spells "unordered", or "unrecognized". */
USTORE_EXPORT char const* ustore_scan_mode_name(ustore_scan_mode_t scan_mode);

/** Reads a scan mode back from its canonical spelling, or fails with
 *  @c ustore_status_invalid_argument_k. */
USTORE_EXPORT ustore_status_t ustore_scan_mode_from_name(ustore_bytes_view_t name, ustore_scan_mode_t* scan_mode);

/** The canonical spelling of a scan direction, as @c ustore_scan_descending_k spells
 *  "descending", or "unrecognized". */
USTORE_EXPORT char const* ustore_scan_direction_name(ustore_scan_direction_t scan_direction);

/** Reads a scan direction back from its canonical spelling, or fails with
 *  @c ustore_status_invalid_argument_k. */
USTORE_EXPORT ustore_status_t ustore_scan_direction_from_name(ustore_bytes_view_t name,
                                                              ustore_scan_direction_t* scan_direction);

/** The canonical spelling of a drop mode, as @c ustore_drop_pairs_k spells "pairs", or "unrecognized". */
USTORE_EXPORT char const* ustore_drop_mode_name(ustore_drop_mode_t drop_mode);

/** Reads a drop mode back from its canonical spelling, or fails with
 *  @c ustore_status_invalid_argument_k. */
USTORE_EXPORT ustore_status_t ustore_drop_mode_from_name(ustore_bytes_view_t name, ustore_drop_mode_t* drop_mode);

/** The canonical spelling of an isolation, as @c ustore_isolation_strict_serializable_k spells "strict-serializable",
 *  or "unrecognized". */
USTORE_EXPORT char const* ustore_isolation_name(ustore_isolation_t isolation);

/** Reads an isolation back from its canonical spelling, or fails with
 *  @c ustore_status_invalid_argument_k. */
USTORE_EXPORT ustore_status_t ustore_isolation_from_name(ustore_bytes_view_t name, ustore_isolation_t* isolation);

/** The canonical spelling of an engine, as @c ustore_engine_rocksdb_k spells "rocksdb", or "unrecognized". */
USTORE_EXPORT char const* ustore_engine_name(ustore_engine_t engine);

/** Reads an engine back from its canonical spelling, or fails with
 *  @c ustore_status_invalid_argument_k. */
USTORE_EXPORT ustore_status_t ustore_engine_from_name(ustore_bytes_view_t name, ustore_engine_t* engine);

/** The canonical spelling of a modality, as @c ustore_modality_graph_k spells "graph", or "unrecognized". */
USTORE_EXPORT char const* ustore_modality_name(ustore_modality_t modality);

/** Reads a modality back from its canonical spelling, or fails with
 *  @c ustore_status_invalid_argument_k. */
USTORE_EXPORT ustore_status_t ustore_modality_from_name(ustore_bytes_view_t name, ustore_modality_t* modality);

#pragma region Collections

/**
 *  @brief The arguments of @c ustore_collection_list, which exports every collection with its name and modality.
 *
 *  The system collections, @c _names and @c _collections, are never listed, and the order of the others is the
 *  engine's. A listing describes the database, never a transaction, so it reads the head or a snapshot.
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

    /** Optional output for the identifiers of the collections, inside @c arena. */
    ustore_collection_id_t const** collections;

    /** Optional output for what every collection holds, inside @c arena. */
    ustore_modality_t const** modalities;

    /** Optional output for the names as a tape inside @c arena, in the order of the identifiers, where a collection
     *  without a name has an empty one. */
    ustore_strings_view_t* names;

    /** Output for the collections written; zeroed on failure. */
    ustore_size_t* written;

    /** Rewound by this call, then holding every output it writes; @c NULL is refused. */
    ustore_arena_t arena;

} ustore_collection_list_t;

/**
 *  @brief Lists the collections of the database.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c arena or the @c written output is @c NULL, or
 *    the @c snapshot belongs to another engine.
 *  - @c ustore_status_device_k The arena lives where another executor computes.
 *  - @c ustore_status_contended_k The arena serves another call, or the executor serves another
 *    call.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL.
 *  - @c ustore_status_unsupported_k The executor computes on a device.
 *  - @c ustore_status_not_found_k A listed collection was dropped meanwhile, on the NVMe engine.
 */
USTORE_EXPORT void ustore_collection_list(ustore_collection_list_t* arguments);

/**
 *  @brief The arguments of @c ustore_collection_create, which always creates an empty collection and answers the
 *      identifier addressing it.
 *
 *  Runs at the head of the database. A name is recorded with the collection in one step and
 *  survives a crash beside it, and a name another collection carries, even one a concurrent creator
 *  took first, fails the call and creates nothing. Each engine keeps the names in the system
 *  collection @c _names and every collection's modality and name in @c _collections, which no
 *  identifier reaches, and a name resolves in two phases: once through @c ustore_collection_find,
 *  and from then on by identifier, as no other call carries one. A name starting with @c _ is
 *  reserved for system collections and refused.
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

    /** Optional name no other collection carries and not starting with @c _, where a zeroed view creates a collection
     *  without one. */
    ustore_bytes_view_t name;

    /** What the collection holds, which fixes how it is keyed and so which calls reach it. */
    ustore_modality_t modality;

    /** Output for the identifier of the collection, zeroed on failure. */
    ustore_collection_id_t* collection;

} ustore_collection_create_t;

/**
 *  @brief Creates an empty collection, named or not.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_contended_k The executor serves another call.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL.
 *  - @c ustore_status_read_only_k The database was opened read-only.
 *  - @c ustore_status_invalid_argument_k The @c modality is unknown, the @c name starts with @c _ or
 *    is empty with non-null data, or the @c collection output is @c NULL.
 *  - @c ustore_status_unsupported_k The executor computes on a device, or a documents, graph or
 *    pages collection is asked of an NVMe database whose values share one size.
 *  - @c ustore_status_already_exists_k Another collection carries the @c name.
 *  - @c ustore_status_write_conflict_k Concurrent commits kept conflicting, on the RAM and NVMe
 *    engines.
 *  - @c ustore_status_capacity_exhausted_k The name outgrows the NVMe schema's record, or every
 *    shared channel is taken.
 */
USTORE_EXPORT void ustore_collection_create(ustore_collection_create_t* arguments);

/**
 *  @brief The arguments of @c ustore_collection_find, which reads the collections a batch of names address.
 *
 *  Collections are created and dropped only at the head, so their names are read at the head or at a snapshot and
 *  never inside a transaction, and resolving a name never joins the conflicts of the work that follows it. A name no
 *  collection carries, the names of the system collections included, answers the nil UUID, a zero modality and an
 *  unset presence bit. Every output holds one entry per name inside @c arena.
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

    /** Names to resolve, one per task. */
    ustore_strings_view_t names;

    /** Optional output for a bitset of @c count bits marking the names a collection carries, addressed as Apache Arrow
     *  does, inside @c arena. */
    ustore_octet_t const** presences;

    /** Output for the identifiers of the collections the names address, inside @c arena. */
    ustore_collection_id_t const** collections;

    /** Optional output for what the collections hold, inside @c arena. */
    ustore_modality_t const** modalities;

    /** Rewound by this call, then holding every output it writes; @c NULL is refused. */
    ustore_arena_t arena;

} ustore_collection_find_t;

/**
 *  @brief Reads the collections a batch of names address.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c arena or the @c collections output is @c NULL,
 *    the @c names come in an unknown layout, or the @c snapshot belongs to another engine.
 *  - @c ustore_status_device_k The arena lives where another executor computes.
 *  - @c ustore_status_contended_k The arena serves another call, or the executor serves another
 *    call.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL.
 *  - @c ustore_status_unsupported_k The executor computes on a device.
 */
USTORE_EXPORT void ustore_collection_find(ustore_collection_find_t* arguments);

/** The arguments of @c ustore_collection_drop, where dropping a collection whole forgets its name before the
 *  collection goes. */
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
 *
 *  @par Statuses:
 *  - @c ustore_status_contended_k The executor serves another call.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL.
 *  - @c ustore_status_read_only_k The database was opened read-only.
 *  - @c ustore_status_unsupported_k The executor computes on a device.
 *  - @c ustore_status_invalid_argument_k The @c mode is unknown, @c ustore_drop_values_k empties a
 *    strings collection, or a snapshot is open while @c ustore_drop_collection_k drops the
 *    collection on the RAM and RocksDB engines.
 *  - @c ustore_status_not_found_k No collection carries the identifier, a system one included.
 *  - @c ustore_status_write_conflict_k Concurrent commits kept conflicting, on the RAM and NVMe
 *    engines.
 *  - @c ustore_status_capacity_exhausted_k Every shared channel is taken on the NVMe engine.
 */
USTORE_EXPORT void ustore_collection_drop(ustore_collection_drop_t* arguments);

#pragma endregion Collections

#pragma region Transactions

/** The arguments of @c ustore_transaction_init. */
typedef struct ustore_transaction_init_t {

    /** In and out: a @c NULL handle here begins a new transaction, which the caller frees with
     *  @c ustore_transaction_free, written on success and left @c NULL on failure; a transaction of @c database is
     *  reset in place onto a fresh snapshot, dropping its reads and writes, and keeps its handle whatever the outcome,
     *  so after a failed reset it can only be freed or initialized again. */
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
 *
 *  @par Statuses:
 *  - @c ustore_status_contended_k The transaction reset serves another call, or the executor serves
 *    another call.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL.
 *  - @c ustore_status_read_only_k The database was opened read-only.
 *  - @c ustore_status_unsupported_k The executor computes on a device, or the engine has no
 *    transactions, as LevelDB has none.
 *  - @c ustore_status_invalid_argument_k The @c transaction output is @c NULL, or the transaction
 *    reset belongs to another database.
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

    /** Optional output for the stamp the transaction reads at, which its commit validates against; zeroed on
     *  failure. */
    ustore_stamp_t* read_stamp;

} ustore_transaction_stage_t;

/**
 *  @brief Stages a transaction as the first phase of a two-phase commit.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_contended_k The transaction serves another call, or the executor serves
 *    another call.
 *  - @c ustore_status_uninitialized_k The @c database or the @c transaction is @c NULL.
 *  - @c ustore_status_read_only_k The database was opened read-only.
 *  - @c ustore_status_unsupported_k The executor computes on a device, or the engine has no
 *    transactions, as LevelDB has none.
 *  - @c ustore_status_invalid_argument_k A failed RocksDB stage left the transaction needing a reset.
 *  - @c ustore_status_write_conflict_k Another commit changed a key this transaction wrote, or, on
 *    the NVMe engine, any key, range or prefix it watched or a collection it touched.
 *  - @c ustore_status_read_conflict_k Another commit changed a key this transaction read, on the RAM
 *    and RocksDB engines.
 *  - @c ustore_status_phantom_conflict_k Another commit changed a window this transaction walked, on
 *    the strictly serializable RAM engine.
 *  - @c ustore_status_not_found_k A collection the transaction touched was dropped, on RocksDB.
 *  - @c ustore_status_malformed_k A value outgrows what its NVMe collection admits.
 *  - @c ustore_status_capacity_exhausted_k The strings outgrow one NVMe run file.
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

    /** Optional output for the commit's position in commit order, visible to every snapshot at or above it; zeroed
     *  unless the commit landed, so a failed flush still reports it. */
    ustore_stamp_t* commit_stamp;

    /** How durable the writes are once the commit finishes. */
    ustore_durability_t durability;

} ustore_transaction_commit_t;

/**
 *  @brief Commits a transaction.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_contended_k The transaction serves another call, or the executor serves
 *    another call.
 *  - @c ustore_status_uninitialized_k The @c database or the @c transaction is @c NULL.
 *  - @c ustore_status_read_only_k The database was opened read-only.
 *  - @c ustore_status_unsupported_k The executor computes on a device, or the engine has no
 *    transactions, as LevelDB has none.
 *  - @c ustore_status_invalid_argument_k The @c durability is unknown, or a failed RocksDB stage left
 *    the transaction needing a reset.
 *  - @c ustore_status_write_conflict_k Another commit changed a key this transaction wrote, or, on
 *    the NVMe engine, any key, range or prefix it watched or a collection it touched.
 *  - @c ustore_status_read_conflict_k Another commit changed a key this transaction read, on the RAM
 *    and RocksDB engines.
 *  - @c ustore_status_phantom_conflict_k Another commit changed a window this transaction walked, on
 *    the strictly serializable RAM engine.
 *  - @c ustore_status_not_found_k A collection the transaction touched was dropped, on RocksDB.
 *  - @c ustore_status_malformed_k A value outgrows what its NVMe collection admits.
 *  - @c ustore_status_capacity_exhausted_k The strings outgrow one NVMe run file.
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
 *
 *  @par Statuses:
 *  - @c ustore_status_contended_k The transaction serves another call, or the executor serves
 *    another call.
 *  - @c ustore_status_uninitialized_k The @c database or the @c transaction is @c NULL.
 *  - @c ustore_status_read_only_k The database was opened read-only.
 *  - @c ustore_status_unsupported_k The executor computes on a device, or the engine has no
 *    transactions, as LevelDB has none.
 *  - @c ustore_status_invalid_argument_k A failed RocksDB stage left the transaction needing a reset.
 */
USTORE_EXPORT void ustore_transaction_rollback(ustore_transaction_rollback_t* arguments);

/**
 *  @brief Waits until every deferred call naming the transaction finishes, then resets it and frees its memory with
 *      every view of it once no cursor reads through it.
 *  @param[in,out] transaction The transaction to be freed; @c NULL is safe.
 */
USTORE_EXPORT void ustore_transaction_free(ustore_transaction_t transaction);

#pragma endregion Transactions

#pragma region Changes

/** What one committed change did to its key. */
typedef enum ustore_change_kind_t {

    /** Wrote the whole value. */
    ustore_change_write_k = 0,

    /** Removed the key. */
    ustore_change_removal_k = 1,

    /** Changed part of the value, as a document patch or a page edit does. */
    ustore_change_change_k = 2,

} ustore_change_kind_t;

/** A change kind's canonical name, "removal" for @c ustore_change_removal_k, or "unrecognized". */
USTORE_EXPORT char const* ustore_change_kind_name(ustore_change_kind_t change_kind);

/** Reads a change kind back from its canonical spelling, or fails with
 *  @c ustore_status_invalid_argument_k. */
USTORE_EXPORT ustore_status_t ustore_change_kind_from_name(ustore_bytes_view_t name, ustore_change_kind_t* change_kind);

/**
 *  @brief An opaque feed of the changes committed to one collection after a stamp, oldest first.
 *
 *  It pins the stamp it reads from the way a snapshot does, so no compaction drops a version above
 *  it, and every pull resumes past the last change the pull before it handed out. Nothing reads
 *  ahead of the caller: a pull costs what it hands out. It serves one pull at a time and holds its
 *  database open until @c ustore_changes_cursor_free.
 */
typedef struct ustore_changes_cursor_handle_t* ustore_changes_cursor_t;

/** The arguments of @c ustore_changes_cursor_init. */
typedef struct ustore_changes_cursor_init_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** The collection whose changes the cursor yields. */
    ustore_collection_id_t collection;

    /** Changes committed after this stamp are yielded, oldest first. */
    ustore_stamp_t since_stamp;

    /** Output for the new cursor, which pins @c since_stamp until @c ustore_changes_cursor_free,
     *  or @c NULL on failure. */
    ustore_changes_cursor_t* cursor;

} ustore_changes_cursor_init_t;

/**
 *  @brief Opens a feed of one collection's changes committed after a stamp, reading nothing yet.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c cursor output is @c NULL.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL.
 *  - @c ustore_status_contended_k The executor serves another call.
 *  - @c ustore_status_not_found_k The collection does not exist or was dropped meanwhile.
 *  - @c ustore_status_unsupported_k The executor computes on a device, or the engine keeps no
 *    stamps, as LevelDB does.
 *
 *  @c ustore_status_write_conflict_k means the versions after @c since_stamp are already gone, as
 *  no pin kept them, which a starved snapshot reader is refused with too. The RAM engine also
 *  refuses a stamp below the head unless an open feed already pins one at or below it, while NVMe
 *  and RocksDB serve any stamp their history still holds.
 */
USTORE_EXPORT void ustore_changes_cursor_init(ustore_changes_cursor_init_t* arguments);

/** The arguments of @c ustore_changes_cursor_next, which pulls the next changes of a feed, with
 *  their values when asked, in one call. */
typedef struct ustore_changes_cursor_next_t {

    /** The feed to advance, which a pull that fails leaves where it was. */
    ustore_changes_cursor_t cursor;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** Most changes the pull yields, where fewer reach the newest commit. */
    ustore_size_t wanted;

    /** Output for the changes yielded, zero at the newest commit or on failure. */
    ustore_size_t* count;

    /** Output for the keys of the changes, oldest change first, inside @c arena. */
    ustore_key_t const** keys;

    /** Output for the stamp every change was committed at, inside @c arena. */
    ustore_stamp_t const** stamps;

    /** Output for what every change did, inside @c arena. */
    ustore_change_kind_t const** kinds;

    /** Optional output for the bytes every change carries as a tape inside @c arena, empty for a
     *  removal; @c NULL skips them. */
    ustore_strings_view_t* values;

    /** Rewound by this call, then holding every output it writes; @c NULL is refused. */
    ustore_arena_t arena;

} ustore_changes_cursor_next_t;

/**
 *  @brief Pulls the next changes of a feed, oldest first, and their values when asked.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  A pull yields every change committed after the last one it handed out, up to @c wanted; a key
 *  changed twice comes twice, once per commit, and the changes of one commit come by key. A range
 *  removal comes as the removal of every key it covered.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c cursor, the @c arena or an output is @c NULL.
 *  - @c ustore_status_device_k The arena lives where another executor computes.
 *  - @c ustore_status_contended_k The arena or the cursor serves another call, or the executor
 *    serves another call.
 *  - @c ustore_status_not_found_k The collection was dropped meanwhile.
 *  - @c ustore_status_unsupported_k The executor computes on a device.
 *  - @c ustore_status_capacity_exhausted_k Every shared NVMe channel is taken.
 */
USTORE_EXPORT void ustore_changes_cursor_next(ustore_changes_cursor_next_t* arguments);

/**
 *  @brief Waits until every deferred call naming the feed finishes, then frees it, releasing the
 *      stamp it pinned and the hold it kept on its database.
 *  @param[in,out] cursor The feed to be freed; @c NULL is safe.
 */
USTORE_EXPORT void ustore_changes_cursor_free(ustore_changes_cursor_t cursor);

#pragma endregion Changes

#pragma region Ingests

/** In what order an ingest's rows arrive, which decides whether its appends land as they come or wait to be sorted. */
typedef enum ustore_ingest_order_t {

    /** Keys strictly ascend across every append, checked as each one is written into runs of its own. */
    ustore_ingest_sorted_k = 0,

    /** Keys arrive in any order and may repeat, the last row of a key winning; every append is held until finished. */
    ustore_ingest_unsorted_k = 1,

} ustore_ingest_order_t;

/** The canonical spelling of an order, as @c ustore_ingest_unsorted_k spells "unsorted", or "unrecognized". */
USTORE_EXPORT char const* ustore_ingest_order_name(ustore_ingest_order_t order);

/** Reads an order back from its canonical spelling, or fails with
 *  @c ustore_status_invalid_argument_k. */
USTORE_EXPORT ustore_status_t ustore_ingest_order_from_name(ustore_bytes_view_t name, ustore_ingest_order_t* order);

/**
 *  @brief An ingest under way into one collection, which lands every row appended to it as one commit of its own.
 *
 *  It joins no transaction and watches nothing: every row lands at one stamp, so a reader sees all of the ingest or
 *  none of it, and a transaction that began before it and wrote a key it lands fails at commit. Its calls run on the
 *  calling thread, one at a time. On NVMe it holds the database's maintenance claim from its init to its finish or
 *  free, so flushes, compactions, creates and drops wait for it meanwhile.
 */
typedef struct ustore_ingest_handle_t* ustore_ingest_t;

/** The arguments of @c ustore_ingest_init, which opens an ingest into one collection. */
typedef struct ustore_ingest_init_t {

    /** Output for the ingest, freed with @c ustore_ingest_free; @c NULL on failure. */
    ustore_ingest_t* ingest;

    /** An open database, which stays open until the ingest is freed. */
    ustore_database_t database;

    /** Output for the outcome, final on return. */
    ustore_outcome_t* outcome;

    /** Collection the rows land in. */
    ustore_collection_id_t collection;

    /** In what order the rows arrive. */
    ustore_ingest_order_t order;

} ustore_ingest_init_t;

/**
 *  @brief Opens an ingest into one collection.
 *
 *  @par Statuses:
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL.
 *  - @c ustore_status_invalid_argument_k The @c ingest output is @c NULL, or the @c order is unknown.
 *  - @c ustore_status_read_only_k The database was opened read-only.
 *  - @c ustore_status_unsupported_k The engine ingests nothing, or not into that collection's modality.
 *  - @c ustore_status_not_found_k The collection does not exist.
 */
USTORE_EXPORT void ustore_ingest_init(ustore_ingest_init_t* arguments);

/**
 *  @brief The arguments of @c ustore_ingest_append, which hands an ingest one batch of rows.
 *
 *  The rows are copied or written before the call returns, so every view may die after it. Blobs, documents and pages
 *  take @c keys and @c values, strings take their strings as @c values and the keys they map onto as @c keys, and edges
 *  take @c sources, @c targets and optionally @c edges.
 */
typedef struct ustore_ingest_append_t {

    /** The ingest, which a failed append leaves as it was. */
    ustore_ingest_t ingest;

    /** Output for the outcome, final on return. */
    ustore_outcome_t* outcome;

    /** Number of rows. */
    ustore_size_t count;

    /** Keys of blobs, documents and pages, or the keys strings map onto. */
    ustore_keys_view_t keys;

    /** Values of blobs, JSON documents and XML pages, or the strings of a strings collection. */
    ustore_strings_view_t values;

    /** Sources of edges. */
    ustore_keys_view_t sources;

    /** Targets of edges. */
    ustore_keys_view_t targets;

    /** Identifiers of edges, where @c NULL data takes @c ustore_edge_id_default_k for each. */
    ustore_keys_view_t edges;

} ustore_ingest_append_t;

/**
 *  @brief Hands an ingest one batch of rows.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c ingest is @c NULL or finished, a view the modality
 *    needs is missing, the @c values come in an unknown layout, or a key does not ascend past every
 *    key before it under @c ustore_ingest_sorted_k.
 *  - @c ustore_status_malformed_k A value is past what the collection admits, a document no JSON, or a page no XML.
 *  - @c ustore_status_capacity_exhausted_k The volume is full.
 *  - @c ustore_status_contended_k Another append or the finish runs on the same @c ingest.
 */
USTORE_EXPORT void ustore_ingest_append(ustore_ingest_append_t* arguments);

/** The arguments of @c ustore_ingest_finish, which lands every row appended as one commit and ends the ingest. */
typedef struct ustore_ingest_finish_t {

    /** The ingest, finished on return whatever the outcome, which only @c ustore_ingest_free still takes. */
    ustore_ingest_t ingest;

    /** Output for the outcome, final on return. */
    ustore_outcome_t* outcome;

    /** Buffered: visible on return, durable at the next flush, dropped whole by a crash before it. */
    ustore_durability_t durability;

    /** Optional output for the one stamp every row landed at; zeroed unless the rows landed, so a
     *  failed flush still reports it. */
    ustore_stamp_t* commit_stamp;

} ustore_ingest_finish_t;

/**
 *  @brief Lands every row appended to an ingest as one commit.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c ingest is @c NULL or finished, or the @c durability is unknown.
 *  - @c ustore_status_not_found_k The collection was dropped meanwhile.
 *  - @c ustore_status_contended_k An append runs on the same @c ingest, or memory runs kept
 *    refilling while the ingest waited them out.
 *  - @c ustore_status_capacity_exhausted_k The volume is full.
 */
USTORE_EXPORT void ustore_ingest_finish(ustore_ingest_finish_t* arguments);

/** Frees an ingest, dropping every row of an unfinished one; @c NULL is safe. */
USTORE_EXPORT void ustore_ingest_free(ustore_ingest_t ingest);

#pragma endregion Ingests

#ifdef __cplusplus
}
#endif

#endif // UNUM_USTORE_DB_H
