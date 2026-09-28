/**
 *  @file include/ustore/blobs.h
 *  @author Ash Vardanian
 *  @date June 12, 2022
 *  @brief The binary interface for collections of binary values: writes, reads, scans, samples and
 *      measurements.
 *
 *  Every call takes a batch, so a single operation is a batch of one, while no sequence of single
 *  operations reaches the throughput of one batch. Values address their bytes through a pointer, an
 *  offset and a length, each strided. Offsets let a tape returned by @c ustore_read be updated in place
 *  and written back, and let runtimes like CPython skip the header in front of every object's bytes.
 *  Lengths may be left out for null-terminated strings, or strided by zero when every value is as long.
 */

#pragma once

#include <ustore/db.h>

#ifdef __cplusplus
extern "C" {
#endif

#pragma region Writes

/**
 *  @brief The arguments of @c ustore_write, which upserts, clears or removes values in a batch.
 *
 *  Inserts, updates and compare-and-swap are left to transactions, which compose them atomically out
 *  of reads and writes, so the interface needs no verb for each of them.
 */
typedef struct ustore_write_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Transaction that watches the written keys. */
    ustore_transaction_t transaction;

    /** Reusable memory for intermediate results. */
    ustore_arena_t* arena;

    /** Accepts @c ustore_option_write_flush_k without a transaction, @c ustore_option_transaction_dont_watch_k
     *  with one, and @c ustore_option_dont_discard_memory_k. */
    ustore_options_t options;

    /** Number of tasks, one per key. */
    ustore_size_t tasks_count;

    /** Collections owning the keys, where @c NULL stands for the main collection. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Keys to write, @c NULL only when there are no tasks. */
    ustore_key_t const* keys;

    /** Bytes between consecutive keys, where zero repeats the first one. */
    ustore_size_t keys_stride;

    /** Optional bitset of at least @c tasks_count bits, where a set bit marks a value that is not
     *  null, addressed as Apache Arrow does. */
    ustore_octet_t const* presences;

    /** Offsets of the values within the chunks of @c values, which may skip the header of a runtime object. */
    ustore_length_t const* offsets;

    /** Bytes between consecutive offsets, where zero repeats the first one. */
    ustore_size_t offsets_stride;

    /** Optional lengths of the values, inferred from consecutive offsets or null terminators when absent. */
    ustore_length_t const* lengths;

    /** Bytes between consecutive lengths, where zero repeats the first one. */
    ustore_size_t lengths_stride;

    /** Pointers to the chunks holding the values, where @c NULL removes every key. */
    ustore_bytes_cptr_t const* values;

    /** Bytes between consecutive chunk pointers, where zero repeats the first one. */
    ustore_size_t values_stride;

} ustore_write_t;

/**
 *  @brief Writes a batch of values.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_write(ustore_write_t* arguments);

#pragma endregion Writes

#pragma region Reads

/** The arguments of @c ustore_read, which reads values, their lengths or their presence in a batch. */
typedef struct ustore_read_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Transaction that watches the read keys. */
    ustore_transaction_t transaction;

    /** Optional snapshot whose view is read, never set beside a transaction. */
    ustore_snapshot_t snapshot;

    /** Reusable memory for the results. */
    ustore_arena_t* arena;

    /** Accepts @c ustore_option_transaction_dont_watch_k with a transaction, @c ustore_option_read_shared_memory_k,
     *  @c ustore_option_scan_bulk_k and @c ustore_option_dont_discard_memory_k. */
    ustore_options_t options;

    /** Number of tasks, one per key. */
    ustore_size_t tasks_count;

    /** Collections owning the keys, where @c NULL stands for the main collection. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Keys to read. */
    ustore_key_t const* keys;

    /** Bytes between consecutive keys, where zero repeats the first one. */
    ustore_size_t keys_stride;

    /** Optional output for a bitset of at least @c tasks_count bits marking present keys, addressed
     *  as Apache Arrow does. */
    ustore_octet_t** presences;

    /** Optional output for @c tasks_count plus one offsets into @c values, the last one closing the
     *  final value as Apache Arrow expects. */
    ustore_length_t** offsets;

    /** Optional output for the lengths of the values, with @c ustore_length_missing_k for missing keys. */
    ustore_length_t** lengths;

    /** Optional output for the tape of values. With offsets and lengths the values may sit in any
     *  order; with only one of them they follow each other without gaps, and with only offsets an
     *  empty value reads the same as a missing one unless the presences are exported too. */
    ustore_byte_t** values;

} ustore_read_t;

/**
 *  @brief Reads a batch of values.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_read(ustore_read_t* arguments);

#pragma endregion Reads

#pragma region Scans

/**
 *  @brief The arguments of @c ustore_scan, which lists up to a limit of keys from a start key in
 *      every collection.
 *
 *  Values are not exported, so a read or a modality follows the scan. Consistent iterators over
 *  concurrent state are costly, so scans paginate instead.
 */
typedef struct ustore_scan_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Transaction that watches the scanned keys. */
    ustore_transaction_t transaction;

    /** Optional snapshot whose view is scanned, never set beside a transaction. */
    ustore_snapshot_t snapshot;

    /** Reusable memory for the results. */
    ustore_arena_t* arena;

    /** Accepts @c ustore_option_scan_bulk_k, @c ustore_option_transaction_dont_watch_k,
     *  @c ustore_option_read_shared_memory_k and @c ustore_option_dont_discard_memory_k. */
    ustore_options_t options;

    /** Number of scans. */
    ustore_size_t tasks_count;

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

    /** Optional output for @c tasks_count plus one offsets into @c keys, the last one closing the final scan. */
    ustore_length_t** offsets;

    /** Optional output for the number of keys every scan found, never above its limit. */
    ustore_length_t** counts;

    /** Output for the tape of found keys. */
    ustore_key_t** keys;

} ustore_scan_t;

/**
 *  @brief Scans a batch of key ranges.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_scan(ustore_scan_t* arguments);

#pragma endregion Scans

#pragma region Samples and Measurements

/** The arguments of @c ustore_sample, which draws keys uniformly at random from every collection. */
typedef struct ustore_sample_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Transaction that watches the sampled keys. */
    ustore_transaction_t transaction;

    /** Optional snapshot whose view is sampled, never set beside a transaction. */
    ustore_snapshot_t snapshot;

    /** Reusable memory for the results. */
    ustore_arena_t* arena;

    /** Accepts @c ustore_option_scan_bulk_k, @c ustore_option_transaction_dont_watch_k,
     *  @c ustore_option_read_shared_memory_k and @c ustore_option_dont_discard_memory_k. */
    ustore_options_t options;

    /** Number of samples. */
    ustore_size_t tasks_count;

    /** Collections to sample, where @c NULL stands for the main collection. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Number of keys every sample draws. */
    ustore_length_t const* count_limits;

    /** Bytes between consecutive limits, where zero repeats the first one. */
    ustore_size_t count_limits_stride;

    /** Optional output for @c tasks_count plus one offsets into @c keys, the last one closing the final sample. */
    ustore_length_t** offsets;

    /** Optional output for the number of keys every sample drew, never above its limit. */
    ustore_length_t** counts;

    /** Output for the tape of sampled keys. */
    ustore_key_t** keys;

} ustore_sample_t;

/**
 *  @brief Samples keys from a batch of collections.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_sample(ustore_sample_t* arguments);

/** The arguments of @c ustore_measure, which estimates the entries and the space inside key ranges. */
typedef struct ustore_measure_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Transaction whose view is measured. */
    ustore_transaction_t transaction;

    /** Optional snapshot whose view is measured, never set beside a transaction. */
    ustore_snapshot_t snapshot;

    /** Reusable memory for the results. */
    ustore_arena_t* arena;

    /** Accepts @c ustore_option_scan_bulk_k, @c ustore_option_transaction_dont_watch_k,
     *  @c ustore_option_read_shared_memory_k and @c ustore_option_dont_discard_memory_k. */
    ustore_options_t options;

    /** Number of measured ranges. */
    ustore_size_t tasks_count;

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
    ustore_size_t** min_cardinalities;

    /** Output for the upper bound on the entries in every range. */
    ustore_size_t** max_cardinalities;

    /** Output for the lower bound on the bytes of values in every range. */
    ustore_size_t** min_value_bytes;

    /** Output for the upper bound on the bytes of values in every range. */
    ustore_size_t** max_value_bytes;

    /** Output for the lower bound on the disk space every range takes. */
    ustore_size_t** min_space_usages;

    /** Output for the upper bound on the disk space every range takes. */
    ustore_size_t** max_space_usages;

} ustore_measure_t;

/**
 *  @brief Estimates the entries and the space inside a batch of key ranges.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_measure(ustore_measure_t* arguments);

#pragma endregion Samples and Measurements

#ifdef __cplusplus
}
#endif
