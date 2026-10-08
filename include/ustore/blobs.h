/**
 *  @file include/ustore/blobs.h
 *  @author Ash Vardanian
 *  @date June 12, 2022
 *  @brief The binary interface for collections of binary values: writes, reads, scans, samples and
 *      measurements.
 *
 *  Every call takes a batch, so a single operation is a batch of one, while no sequence of single
 *  operations reaches the throughput of one batch. Values address their bytes through a pointer, a
 *  start and a length, each strided. Starts let a tape returned by @c ustore_read be updated in place
 *  and written back, and let runtimes like CPython skip the header in front of every object's bytes.
 *  Lengths may be left out for null-terminated strings, or strided by zero when every value is as long.
 */

#pragma once

#include "ustore/db.h"

#ifdef __cplusplus
extern "C" {
#endif

#pragma region Writes

/**
 *  @brief The arguments of @c ustore_write, which upserts values in a batch.
 *
 *  Inserts, updates and compare-and-swap are left to transactions, which compose them atomically out
 *  of reads and writes, so the interface needs no verb for each of them. An empty value is still a
 *  value; @c ustore_remove removes keys.
 */
typedef struct ustore_write_t {

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

    /** Collections owning the keys, where @c NULL stands for the main collection. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Keys to write, @c NULL only when there are no tasks. */
    ustore_key_t const* keys;

    /** Bytes between consecutive keys, where zero repeats the first one. */
    ustore_size_t keys_stride;

    /** Optional starts of the values within the chunks of @c values, which may skip the header of a runtime
     *  object, where @c NULL starts every value at its chunk. */
    ustore_length_t const* starts;

    /** Bytes between consecutive starts, where zero repeats the first one. */
    ustore_size_t starts_stride;

    /** Optional lengths of the values; without them every value ends where the next one starts, reading one start
     *  past the last value, or at a null terminator when @c starts is absent too. */
    ustore_length_t const* lengths;

    /** Bytes between consecutive lengths, where zero repeats the first one. */
    ustore_size_t lengths_stride;

    /** Pointers to the chunks holding the values, @c NULL only when there are no tasks. */
    ustore_bytes_cptr_t const* values;

    /** Bytes between consecutive chunk pointers, where zero repeats the first one. */
    ustore_size_t values_stride;

    /** How durable the writes are once the call finishes at the head; a transaction's commit decides its own. */
    ustore_durability_t durability;

    /** Whether the @c transaction records the written keys for conflicts. */
    ustore_watch_t watch;

} ustore_write_t;

/**
 *  @brief Writes a batch of values.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_write(ustore_write_t* arguments);

/** The arguments of @c ustore_remove, which removes keys with their values in a batch. */
typedef struct ustore_remove_t {

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

    /** Collections owning the keys, where @c NULL stands for the main collection. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Keys to remove, @c NULL only when there are no tasks. */
    ustore_key_t const* keys;

    /** Bytes between consecutive keys, where zero repeats the first one. */
    ustore_size_t keys_stride;

    /** How durable the removals are once the call finishes at the head; a transaction's commit decides its own. */
    ustore_durability_t durability;

    /** Whether the @c transaction records the removed keys for conflicts. */
    ustore_watch_t watch;

} ustore_remove_t;

/**
 *  @brief Removes a batch of keys with their values.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_remove(ustore_remove_t* arguments);

#pragma endregion Writes

#pragma region Reads

/**
 *  @brief The arguments of @c ustore_read, which reads values, their lengths or their presence in a batch.
 *
 *  The outputs form an Apache Arrow large binary array: presence bits and offsets in the caller's spans, sized by
 *  @c count, and the tape of values in @c arena, in the order of the keys. A missing key reads as an unset presence
 *  bit and an empty value.
 */
typedef struct ustore_read_t {

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

    /** Collections owning the keys, where @c NULL stands for the main collection. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Keys to read. */
    ustore_key_t const* keys;

    /** Bytes between consecutive keys, where zero repeats the first one. */
    ustore_size_t keys_stride;

    /** Optional output for a bitset of at least @c count bits marking present keys. */
    ustore_octet_t* presences;

    /** Optional output for @c count plus one offsets into @c values, the last one closing the final value. */
    ustore_length_t* offsets;

    /** Optional output for the tape of values, inside @c arena. */
    ustore_byte_t** values;

    /** Memory for @c values: reused when it points to an arena, created when it points to @c NULL. */
    ustore_arena_t* arena;

} ustore_read_t;

/**
 *  @brief Reads a batch of values.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_read(ustore_read_t* arguments);

#pragma endregion Reads

#pragma region Scans

/**
 *  @brief The arguments of @c ustore_scan, which lists up to a limit of keys from a start key in
 *      every collection.
 *
 *  Values are not exported, so a read or a modality follows the scan. Consistent iterators over
 *  concurrent state are costly, so scans paginate instead. The limits bound every output, so the
 *  outputs are the caller's spans and the scan needs no arena.
 */
typedef struct ustore_scan_t {

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

    /** Collections to scan, where @c NULL stands for the main collection. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Smallest key every scan may return, never @c NULL. */
    ustore_key_t const* start_keys;

    /** Bytes between consecutive start keys, where zero repeats the first one. */
    ustore_size_t start_keys_stride;

    /** Most keys every scan returns, never @c NULL. */
    ustore_length_t const* count_limits;

    /** Bytes between consecutive limits, where zero repeats the first one. */
    ustore_size_t count_limits_stride;

    /** Output for @c count plus one offsets into @c keys, the last one closing the final scan. */
    ustore_length_t* offsets;

    /** Output for the found keys, scan after scan, with room for the sum of @c count_limits. */
    ustore_key_t* keys;

    /** In what order the keys come. */
    ustore_scan_mode_t mode;

    /** Optional callback reporting the scans finished. */
    ustore_progress_callback_t progress;

    /** Opaque pointer passed to @c progress. */
    void* progress_state;

} ustore_scan_t;

/**
 *  @brief Scans a batch of key ranges.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_scan(ustore_scan_t* arguments);

#pragma endregion Scans

#pragma region Samples and Measurements

/** The arguments of @c ustore_sample, which draws keys uniformly at random from every collection into the
 *  caller's spans, which the limits bound. */
typedef struct ustore_sample_t {

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

    /** Collections to sample, where @c NULL stands for the main collection. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Number of keys every sample draws. */
    ustore_length_t const* count_limits;

    /** Bytes between consecutive limits, where zero repeats the first one. */
    ustore_size_t count_limits_stride;

    /** Output for @c count plus one offsets into @c keys, the last one closing the final sample. */
    ustore_length_t* offsets;

    /** Output for the sampled keys, sample after sample, with room for the sum of @c count_limits. */
    ustore_key_t* keys;

} ustore_sample_t;

/**
 *  @brief Samples keys from a batch of collections.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_sample(ustore_sample_t* arguments);

/** The arguments of @c ustore_measure, which estimates the entries and the space inside key ranges into the
 *  caller's spans of @c count entries. */
typedef struct ustore_measure_t {

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

    /** Collections to measure, where @c NULL stands for the main collection. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** First key of every range. */
    ustore_key_t const* start_keys;

    /** Bytes between consecutive start keys, where zero repeats the first one. */
    ustore_size_t start_keys_stride;

    /** Last key of every range. */
    ustore_key_t const* end_keys;

    /** Bytes between consecutive end keys, where zero repeats the first one. */
    ustore_size_t end_keys_stride;

    /** Output for the lower bound on the entries in every range. */
    ustore_size_t* min_cardinalities;

    /** Bytes between consecutive lower bounds on the entries, never zero. */
    ustore_size_t min_cardinalities_stride;

    /** Output for the upper bound on the entries in every range. */
    ustore_size_t* max_cardinalities;

    /** Bytes between consecutive upper bounds on the entries, never zero. */
    ustore_size_t max_cardinalities_stride;

    /** Output for the lower bound on the bytes of values in every range. */
    ustore_size_t* min_value_bytes;

    /** Bytes between consecutive lower bounds on the bytes of values, never zero. */
    ustore_size_t min_value_bytes_stride;

    /** Output for the upper bound on the bytes of values in every range. */
    ustore_size_t* max_value_bytes;

    /** Bytes between consecutive upper bounds on the bytes of values, never zero. */
    ustore_size_t max_value_bytes_stride;

    /** Output for the lower bound on the disk space every range takes. */
    ustore_size_t* min_space_usages;

    /** Bytes between consecutive lower bounds on the disk space, never zero. */
    ustore_size_t min_space_usages_stride;

    /** Output for the upper bound on the disk space every range takes. */
    ustore_size_t* max_space_usages;

    /** Bytes between consecutive upper bounds on the disk space, never zero. */
    ustore_size_t max_space_usages_stride;

} ustore_measure_t;

/**
 *  @brief Estimates the entries and the space inside a batch of key ranges.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_measure(ustore_measure_t* arguments);

#pragma endregion Samples and Measurements

#ifdef __cplusplus
}
#endif
