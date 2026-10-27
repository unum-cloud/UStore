/**
 *  @file c/ustore/blobs.h
 *  @author Ash Vardanian
 *  @date June 12, 2022
 *  @brief The binary interface for collections of binary values: writes, reads, scans, samples and
 *      measurements.
 *
 *  Every call takes a batch, so a single operation is a batch of one, while no sequence of single
 *  operations reaches the throughput of one batch. Keys arrive as a strided run and values as a
 *  @c ustore_strings_view_t, so a tape returned by @c ustore_blobs_read is written back without a copy, and a
 *  runtime holding its values in objects of its own passes them through an accessor.
 */

#ifndef UNUM_USTORE_BLOBS_H
#define UNUM_USTORE_BLOBS_H

#include "ustore/db.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 *  @brief The arguments of @c ustore_blobs_write, which upserts values in a batch.
 *
 *  Inserts, updates and compare-and-swap are left to transactions, which compose them atomically out
 *  of reads and writes, so the interface needs no verb for each of them. An empty value is still a
 *  value; @c ustore_blobs_remove removes keys.
 */
typedef struct ustore_blobs_write_t {

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

    /** Number of tasks, one per key. */
    ustore_size_t count;

    /** Collections owning the keys. */
    ustore_collections_view_t collections;

    /** Keys to write. */
    ustore_keys_view_t keys;

    /** Values to write, one per key; a tape @c ustore_blobs_read wrote is written back as it is. */
    ustore_strings_view_t values;

    /** How durable the writes are once the call finishes at the head; a transaction's commit decides its own. */
    ustore_durability_t durability;

    /** Whether the @c transaction records the written keys for conflicts. */
    ustore_watch_t watch;

} ustore_blobs_write_t;

/**
 *  @brief Writes a batch of values.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_contended_k The @c transaction serves another call, the executor's deferred
 *    table is full, or a callback blocks on the executor running it.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL.
 *  - @c ustore_status_read_only_k The database was opened read-only.
 *  - @c ustore_status_invalid_argument_k The @c collections, @c keys or @c values are missing, the
 *    values come in an unknown layout, the @c transaction belongs to another engine or is staged,
 *    the @c durability or @c watch is unknown, a flushed write names a @c transaction,
 *    @c ustore_watch_none_k comes without one, or a collection holds another modality.
 *  - @c ustore_status_unsupported_k The executor computes on a device.
 *  - @c ustore_status_not_found_k A collection does not exist or was dropped meanwhile.
 *  - @c ustore_status_write_conflict_k Concurrent commits kept conflicting, or a staged RocksDB
 *    transaction holds a key.
 *  - @c ustore_status_malformed_k A key or value falls outside what its NVMe collection admits.
 *  - @c ustore_status_capacity_exhausted_k The values outgrow the NVMe transaction's buffer.
 */
USTORE_EXPORT void ustore_blobs_write(ustore_blobs_write_t* arguments);

/** The arguments of @c ustore_blobs_remove, which removes keys with their values in a batch. */
typedef struct ustore_blobs_remove_t {

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

    /** Number of tasks, one per key. */
    ustore_size_t count;

    /** Collections owning the keys. */
    ustore_collections_view_t collections;

    /** Keys to remove. */
    ustore_keys_view_t keys;

    /** How durable the removals are once the call finishes at the head; a transaction's commit decides its own. */
    ustore_durability_t durability;

    /** Whether the @c transaction records the removed keys for conflicts. */
    ustore_watch_t watch;

} ustore_blobs_remove_t;

/**
 *  @brief Removes a batch of keys with their values.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_contended_k The @c transaction serves another call, the executor's deferred
 *    table is full, or a callback blocks on the executor running it.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL.
 *  - @c ustore_status_read_only_k The database was opened read-only.
 *  - @c ustore_status_invalid_argument_k The @c collections or @c keys are missing, the
 *    @c transaction belongs to another engine or is staged, the @c durability or @c watch is
 *    unknown, a flushed remove names a @c transaction, @c ustore_watch_none_k comes without one, or
 *    a collection holds another modality.
 *  - @c ustore_status_unsupported_k The executor computes on a device.
 *  - @c ustore_status_not_found_k A collection does not exist or was dropped meanwhile.
 *  - @c ustore_status_write_conflict_k Concurrent commits kept conflicting, or a staged RocksDB
 *    transaction holds a key.
 *  - @c ustore_status_malformed_k A key falls outside what its NVMe collection admits.
 *  - @c ustore_status_capacity_exhausted_k The removes outgrow the NVMe transaction's buffer.
 */
USTORE_EXPORT void ustore_blobs_remove(ustore_blobs_remove_t* arguments);

/**
 *  @brief The arguments of @c ustore_blobs_read, which reads values, their lengths or their presence in a batch.
 *
 *  The outputs form an Apache Arrow binary array inside @c arena: presence bits and the tape of values, in the order
 *  of the keys. A missing key reads as an unset presence bit and an empty value.
 */
typedef struct ustore_blobs_read_t {

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

    /** Number of tasks, one per key. */
    ustore_size_t count;

    /** Collections owning the keys. */
    ustore_collections_view_t collections;

    /** Keys to read. */
    ustore_keys_view_t keys;

    /** Optional output for a bitset of @c count bits marking present keys, inside @c arena. */
    ustore_octet_t const** presences;

    /** Optional output for the length of every value, for reads that skip the bytes, inside @c arena. */
    ustore_length_t const** lengths;

    /** Optional output for the values as a tape inside @c arena, where a missing key reads as an empty value. */
    ustore_strings_view_t* values;

    /** Rewound by this call, then holding every output it writes; @c NULL is refused. */
    ustore_arena_t arena;

} ustore_blobs_read_t;

/**
 *  @brief Reads a batch of values.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c arena, @c collections or @c keys are missing, the
 *    @c view was made by neither view constructor or belongs to another engine, a watching read goes
 *    through a staged transaction, or a collection holds another modality.
 *  - @c ustore_status_device_k The arena lives where another executor computes.
 *  - @c ustore_status_contended_k The arena or the transaction the @c view reads through serves
 *    another call, the executor's deferred table is full, a callback blocks on the executor running
 *    it, or an NVMe value keeps changing size across reads.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL, or an NVMe transfer was cancelled.
 *  - @c ustore_status_unsupported_k The executor computes on a device, where no engine serves the
 *    call yet.
 *  - @c ustore_status_not_found_k A collection does not exist or was dropped meanwhile.
 *  - @c ustore_status_capacity_exhausted_k Every shared NVMe channel is taken.
 */
USTORE_EXPORT void ustore_blobs_read(ustore_blobs_read_t* arguments);

/**
 *  @brief The arguments of @c ustore_blobs_scan, which lists up to @c wanted keys from a start key in
 *      every collection.
 *
 *  Values are not exported, so a read or a modality follows the scan, or a @c ustore_blobs_cursor_t walks one
 *  collection with its values. Consistent iterators over concurrent state are costly, so scans paginate instead.
 */
typedef struct ustore_blobs_scan_t {

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

    /** Smallest key every scan may return, where @c NULL data starts at the first key. */
    ustore_keys_view_t start_keys;

    /** Optional patterns the value of every listed key matches, shared by every scan; zeroed lists every key. */
    ustore_patterns_t patterns;

    /** Optional verdict on the listed keys, asked once per page as its @c cost says; host executors only. */
    ustore_predicate_t predicate;

    /** Output for @c count plus one offsets into @c keys, the last one closing the final scan, inside @c arena. */
    ustore_length_t const** offsets;

    /** Output for the found keys, scan after scan, inside @c arena. */
    ustore_key_t const** keys;

    /** Optional output for one BM25 score per listed key when @c patterns carry weights, inside @c arena; @c NULL
     *  skips them. */
    float const** scores;

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

} ustore_blobs_scan_t;

/**
 *  @brief Scans a batch of key ranges.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c arena, @c collections or an output is missing, the
 *    @c mode, a pattern kind, a case sensitivity or a predicate cost is unknown, the @c patterns do
 *    not compile, the @c view was made by neither view constructor or belongs to another engine, a
 *    watching scan goes through a staged transaction, or a collection holds another modality.
 *  - @c ustore_status_device_k The arena lives where another executor computes, or a device
 *    executor is given a predicate.
 *  - @c ustore_status_contended_k The arena or the transaction the @c view reads through serves
 *    another call, the executor's deferred table is full, a callback blocks on the executor running
 *    it, or an NVMe value keeps changing size across reads.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL, or an NVMe transfer was cancelled.
 *  - @c ustore_status_unsupported_k The executor computes on a device, where no engine serves the
 *    call yet.
 *  - @c ustore_status_not_found_k A collection does not exist or was dropped meanwhile.
 *  - @c ustore_status_capacity_exhausted_k Every shared NVMe channel is taken.
 */
USTORE_EXPORT void ustore_blobs_scan(ustore_blobs_scan_t* arguments);

/**
 *  @brief An opaque handle to a walk over the keys of one collection, which every pull resumes past the last key the
 *      pull before it handed out.
 *
 *  It keeps only the key its next pull starts at, so a key written behind the walk is never handed out and one
 *  written ahead of it is, while @c ustore_blobs_scan stays the batched listing of many ranges. It holds its database
 *  open, and the transaction or snapshot it reads through, until @c ustore_blobs_cursor_free. A cursor serves one
 *  pull at a time, so a second pull naming it finishes @c ustore_status_contended_k.
 */
typedef struct ustore_blobs_cursor_handle_t* ustore_blobs_cursor_t;

/** The arguments of @c ustore_blobs_cursor_init. */
typedef struct ustore_blobs_cursor_init_t {

    /** Output for the new cursor, which the caller frees with @c ustore_blobs_cursor_free; @c NULL on failure. */
    ustore_blobs_cursor_t* cursor;

    /** An open database. */
    ustore_database_t database;

    /** Executor of the kind every pull runs on, which picks the side the walk computes on, where @c NULL walks on host
     *  executors; a pull on another kind of executor reports @c ustore_status_device_k. */
    ustore_executor_t executor;

    /** Output for the outcome, final on return. */
    ustore_outcome_t* outcome;

    /** What the walk sees, where @c NULL is the head, read pull by pull. */
    ustore_view_t view;

    /** Collection to walk, which holds keys rather than strings. */
    ustore_collection_id_t collection;

    /** Smallest key the walk returns. */
    ustore_key_t start_key;

    /** Optional patterns every value the walk hands out matches, compiled here once; zeroed walks every key. */
    ustore_patterns_t patterns;

    /** Optional verdict on the keys of every page, asked as its @c cost says; host executors only. */
    ustore_predicate_t predicate;

} ustore_blobs_cursor_init_t;

/**
 *  @brief Opens a walk over the keys of one collection, reading nothing until its first pull.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c cursor output is @c NULL, the @c view was made by
 *    neither view constructor or reads another database, a pattern kind, a case sensitivity or a
 *    predicate cost is unknown, or the @c patterns do not compile.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL.
 *  - @c ustore_status_unsupported_k A device @c executor is given @c patterns.
 *  - @c ustore_status_device_k A device @c executor is given a predicate.
 */
USTORE_EXPORT void ustore_blobs_cursor_init(ustore_blobs_cursor_init_t* arguments);

/** The arguments of @c ustore_blobs_cursor_next, which pulls the next page of a walk, with the values of its keys when
 *  asked, in one call. */
typedef struct ustore_blobs_cursor_next_t {

    /** The database the cursor was opened on. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** The cursor to advance, which a pull that fails or is cancelled leaves where it was. */
    ustore_blobs_cursor_t cursor;

    /** Output for the keys of the page in their order, inside @c arena. */
    ustore_key_t const** keys;

    /** Optional output for their values as a tape inside @c arena, so a walk reads without a second call, where a key
     *  removed at the head between the listing and the read of one pull reads as an empty value; @c NULL skips them. */
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

} ustore_blobs_cursor_next_t;

/**
 *  @brief Pulls the next page of a walk, and the values of its keys when asked.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c cursor, the @c arena or the @c written output is
 *    @c NULL, the cursor walks another database, a watching pull goes through a staged
 *    transaction, or the collection holds another modality.
 *  - @c ustore_status_device_k The arena lives where another executor computes, or a cursor opened
 *    for a device is pulled on host threads.
 *  - @c ustore_status_contended_k The arena, the cursor or the transaction it reads through serves
 *    another call, the executor's deferred table is full, a callback blocks on the executor running
 *    it, or an NVMe value keeps changing size across reads.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL, or an NVMe transfer was cancelled.
 *  - @c ustore_status_unsupported_k The executor computes on a device, where no engine walks yet.
 *  - @c ustore_status_not_found_k The collection does not exist or was dropped meanwhile.
 *  - @c ustore_status_capacity_exhausted_k Every shared NVMe channel is taken.
 */
USTORE_EXPORT void ustore_blobs_cursor_next(ustore_blobs_cursor_next_t* arguments);

/**
 *  @brief Waits until every deferred call naming the cursor finishes, then frees it, giving back the holds it kept on
 *      its database and on the transaction or snapshot it reads through.
 *  @param[in,out] cursor The cursor to be freed; @c NULL is safe.
 */
USTORE_EXPORT void ustore_blobs_cursor_free(ustore_blobs_cursor_t cursor);

/** The arguments of @c ustore_blobs_sample, which draws up to @c wanted keys uniformly at random from every
 *  collection. */
typedef struct ustore_blobs_sample_t {

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

} ustore_blobs_sample_t;

/**
 *  @brief Samples keys from a batch of collections.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c arena, @c collections or an output is missing, the
 *    @c view was made by neither view constructor or belongs to another engine, a watching sample
 *    goes through a staged transaction, or a collection holds another modality.
 *  - @c ustore_status_device_k The arena lives where another executor computes.
 *  - @c ustore_status_contended_k The arena or the transaction the @c view reads through serves
 *    another call, the executor's deferred table is full, a callback blocks on the executor running
 *    it, or an NVMe value keeps changing size across reads.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL, or an NVMe transfer was cancelled.
 *  - @c ustore_status_unsupported_k The executor computes on a device, where no engine serves the
 *    call yet.
 *  - @c ustore_status_not_found_k A collection does not exist or was dropped meanwhile.
 *  - @c ustore_status_capacity_exhausted_k Every shared NVMe channel is taken.
 */
USTORE_EXPORT void ustore_blobs_sample(ustore_blobs_sample_t* arguments);

/** The arguments of @c ustore_blobs_measure, which estimates the entries and the space inside key ranges, every
 *  output holding one bound per range. */
typedef struct ustore_blobs_measure_t {

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

    /** First key of every range, where @c NULL data starts at the first key. */
    ustore_keys_view_t start_keys;

    /** Last key of every range, where @c NULL data ends at the last key. */
    ustore_keys_view_t end_keys;

    /** Optional output for the lower bound on the entries in every range, inside @c arena. */
    ustore_size_t const** min_cardinalities;

    /** Optional output for the upper bound on the entries in every range, inside @c arena. */
    ustore_size_t const** max_cardinalities;

    /** Optional output for the lower bound on the bytes of values in every range, inside @c arena. */
    ustore_size_t const** min_value_bytes;

    /** Optional output for the upper bound on the bytes of values in every range, inside @c arena. */
    ustore_size_t const** max_value_bytes;

    /** Optional output for the lower bound on the disk space every range takes, inside @c arena. */
    ustore_size_t const** min_space_usages;

    /** Optional output for the upper bound on the disk space every range takes, inside @c arena. */
    ustore_size_t const** max_space_usages;

    /** Rewound by this call, then holding every output it writes; @c NULL is refused. */
    ustore_arena_t arena;

} ustore_blobs_measure_t;

/**
 *  @brief Estimates the entries and the space inside a batch of key ranges.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c arena or the @c collections are missing, the
 *    @c view was made by neither view constructor or belongs to another engine, a watching measure
 *    goes through a staged transaction, or a collection holds another modality.
 *  - @c ustore_status_device_k The arena lives where another executor computes.
 *  - @c ustore_status_contended_k The arena or the transaction the @c view reads through serves
 *    another call, the executor's deferred table is full, or a callback blocks on the executor
 *    running it.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL, or an NVMe transfer was cancelled.
 *  - @c ustore_status_unsupported_k The executor computes on a device, where no engine serves the
 *    call yet.
 *  - @c ustore_status_not_found_k A collection does not exist or was dropped meanwhile.
 *  - @c ustore_status_capacity_exhausted_k Every shared NVMe channel is taken.
 */
USTORE_EXPORT void ustore_blobs_measure(ustore_blobs_measure_t* arguments);

#ifdef __cplusplus
}
#endif

#endif // UNUM_USTORE_BLOBS_H
