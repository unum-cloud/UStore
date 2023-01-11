/**
 * @file blobs.h
 * @author Ashot Vardanian
 * @date 12 Jun 2022
 * @addtogroup C
 *
 * @brief Binary Interface Standard for trivial @b BLOB collections.
 *
 * ## Usage Recommendations
 *
 * ### Pack operations into batches wherever possible
 *
 * Using the batch APIs to issue a single read/write request is trivial, but achieving
 * batch-level performance with singular operations is impossible. Especially, with
 * a client-server setup. Regardless of IO layer, a lot of synchronization and locks
 * must be issued to provide consistency.
 *
 * ## Why use offsets?
 *
 * In the underlying layer, using offsets to adds no additional overhead,
 * but what is the point of using them, if we can immediately pass adjusted
 * pointers? It serves two primary purposes:
 *
 * - Supporting input tapes (values_stride = 0, offsets_stride != 0).
 * - List-oriented wrappers (values_stride != 0, offsets_stride = 0).
 *
 * In the first case, we may have received a tape from `ukv_read()`, which we
 * update in-place and write back, without changing the size of the original
 * entries.
 *
 * In the second case, we may be working with higher-level runtimes, like
 * CPython, where objects metadata (like its length) is stored in front of
 * the allocated region. In such cases, we may still need additional memory
 * to store the lengths of the objects, unless those are NULL-terminated
 * strings (lengths = NULL) or if all have the same length (length_stride = 0).
 *
 * Further reading on the implementation of strings and arrays of strings in
 * different languages:
 *
 * - Python/CPython:
 *      https://docs.python.org/3/c-api/bytes.html
 * - JavaScript/V8:
 *      https://github.com/v8/v8/blob/main/include/v8-data.h
 *      https://github.com/v8/v8/blob/main/include/v8-array-buffer.h
 * - GoLang:
 *      https://boakye.yiadom.org/go/strings/
 *      https://github.com/golang/go/blob/master/src/runtime/string.go (`stringStruct`)
 *      https://github.com/golang/go/blob/master/src/runtime/slice.go (`slice`)
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ukv/db.h"

/**
 * @brief Main "setter" or "scatter" interface.
 * @see `ukv_write()`.
 * @see https://unum.cloud/ukv/c#writes
 *
 * ## Functionality
 *
 * This is one of the two primary methods, together with `ukv_read()`,
 * so its functionality is wide. It knots together various kinds of updates:
 *
 * - Single writes and Batches.
 * - On Head state or a Transactional.
 * - Transparent or Watching through Transactions.
 * - Upserting, Clearing or Removing values.
 *
 * Check docs below to see how different variants can be invoked.
 *
 * ## Upserts, Updates & Inserts
 *
 * Higher-level interfaces may choose to implement any of those verbs:
 *
 * 1. Insert: add if missing.
 * 2. Update: overwrite if present.
 * 3. Upsert: write.
 * 4. Remove: overwrite with `NULL` if present.
 *
 * Instead of adding all three to the binary C interface, we focus on better ACID transactions,
 * which can be used to implement any advanced multi-step operations (often including conditionals),
 * like Compare-And-Swap, without losing atomicity.
 */
typedef struct ukv_write_t {

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
     * @brief The transaction in which the operation will be watched.
     * @see `ukv_transaction_init()`, `ukv_transaction_commit()`, `ukv_transaction_free()`.
     */
    ukv_transaction_t transaction;
    /**
     * @brief Reusable memory handle.
     * @see `ukv_arena_free()`.
     */
    ukv_arena_t* arena;
    /**
     * @brief Write options.
     *
     * Possible values:
     * - `::ukv_option_write_flush_k`: Forces to persist non-transactional writes on disk before returning.
     * - `::ukv_option_transaction_dont_watch_k`: Disables collision-detection for transactional writes.
     * - `::ukv_option_dont_discard_memory_k`: Won't reset the `arena` before the operation begins.
     */
    ukv_options_t options;

    /// @}
    /// @name Locations
    /// @{

    /**
     * @brief Number of separate operations packed in this read.
     * Always equal to the number of provided `keys`.
     */
    ukv_size_t tasks_count;
    /**
     * @brief Sequence of collections owning the `keys`.
     *
     * If `NULL` is passed, the default collection is assumed.
     * If multiple collections are passed, the step between them is defined by `collections_stride`.
     * Use `ukv_collection_create()` or `ukv_collection_list()` to obtain collection IDs for string names.
     * Is @b optional.
     */
    ukv_collection_t const* collections;
    /**
     * @brief Step between `collections`.
     *
     * Contains the number of bytes separating entries in the `collections` array.
     * Zero stride would reuse the same address for all tasks.
     * You can use it to retrieve different keys from the same collection in one call.
     * Is @b optional.
     */
    ukv_size_t collections_stride;
    /**
     * @brief Sequence of keys to update.
     *
     * Contains the pointer to the first of `tasks_count` keys to be collected from `collections`.
     * If multiple keys are passed, the step between them is defined by `keys_stride`.
     */
    ukv_key_t const* keys;
    /**
     * @brief Step between `keys`.
     *
     * The number of bytes separating entries in the `keys` array.
     * Zero stride would reuse the same address for all tasks.
     * You can use it to retrieve the same key from different collections in one call.
     * Is @b optional.
     */
    ukv_size_t keys_stride;

    /// @}
    /// @name Contents
    /// @{

    /**
     * @brief Bitmask of "presence" indicators with at least `tasks_count` bits.
     *
     * Each set bit means that the respective content chunk isn't `NULL`.
     * Is addressed the same way as in Apache Arrow.
     * Is @b optional.
     */
    ukv_octet_t const* presences;
    /**
     * @brief The pointer to the offset (in bytes) of the first content within
     * the first chunk of `values`.
     *
     * If tasks are executing, the step between offsets is defined by `offsets_stride`.
     * Not only it allows addressing different parts of a concatenated tape,
     * but also allows to skip some structure header, like the @c PyObject_HEAD
     * in every CPython runtime object.
     */
    ukv_length_t const* offsets;
    /**
     * @brief Step between `offsets`.
     *
     * The number of bytes separating entries in the `offsets` array.
     * Zero stride would reuse the same address for all tasks.
     * Is @b optional.
     */
    ukv_size_t offsets_stride;
    /**
     * @brief The pointer to the offset (in bytes) of the first content within
     * the first chunk of `values`. Zero-length entries are allowed.
     *
     * Is @b optional, as lengths can be inferred from consecutive offsets.
     * If neither `offsets` nor `lengths` are passed, we assume values
     * to be `NULL`-terminated and infer the length from `values` themselves.
     */
    ukv_length_t const* lengths;
    /**
     * @brief Step between `keys`.
     *
     * The number of bytes separating entries in the `keys` array.
     * Zero stride would reuse the same address for all tasks.
     * Is @b optional.
     */
    ukv_size_t lengths_stride;
    /**
     * @brief An array of pointers to data chunks.
     * If `NULL`, will simply delete all the `keys` from respective `collections`.
     *
     * Generally, we expect each chunk to be identical to the content we want to write,
     * but we can specify subspans by using `offsets` and `lengths`.
     */
    ukv_bytes_cptr_t const* values;
    /**
     * @brief The number of bytes separating entries in the `values` array.
     * Zero stride would reuse the same address for all tasks.
     * Is @b optional.
     */
    ukv_size_t values_stride;

    /// @}

} ukv_write_t;

/**
 * @brief Main "setter" or "scatter" interface.
 * @see `ukv_write_t`.
 */
void ukv_write(ukv_write_t*);

/**
 * @brief Main "getter" or "gather" interface.
 * @see `ukv_read()`.
 * @see https://unum.cloud/ukv/c#reads
 *
 * ## Functionality
 *
 * This is one of the two primary methods, together with `ukv_write()`,
 * so its functionality is wide. It knots together various kinds of reads:
 *
 * - Single reads and Batches.
 * - On Head state or a Snapshot.
 * - Transparent or Watching through Transactions.
 * - Reading the entire values or just checking existence or lengths.
 *
 * Check docs below to see how different variants can be invoked.
 */
typedef struct ukv_read_t {

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
     * @brief The transaction in which the operation will be watched.
     * @see `ukv_transaction_init()`, `ukv_transaction_commit()`, `ukv_transaction_free()`.
     */
    ukv_transaction_t transaction;
    ukv_transaction_t snapshot;
    /**
     * @brief Reusable memory handle.
     * @see `ukv_arena_free()`.
     */
    ukv_arena_t* arena;
    /**
     * @brief Read options.
     *
     * Possible values:
     * - `::ukv_option_transaction_dont_watch_k`: Disables collision-detection for transactional reads.
     * - `::ukv_option_read_shared_memory_k`: Exports to shared memory to accelerate inter-process communication.
     * - `::ukv_option_scan_bulk_k`: Suggests that the list of keys was received from a bulk scan.
     * - `::ukv_option_dont_discard_memory_k`: Won't reset the `arena` before the operation begins.
     */
    ukv_options_t options;

    /// @}
    /// @name Inputs
    /// @{

    /**
     * @brief Number of separate operations packed in this read.
     * Always equal to the number of provided `keys`.
     */
    ukv_size_t tasks_count;
    /**
     * @brief Sequence of collections owning the `keys`.
     *
     * If `NULL` is passed, the default collection is assumed.
     * If multiple collections are passed, the step between them is defined by `collections_stride`.
     * Use `ukv_collection_create()` or `ukv_collection_list()` to obtain collection IDs for string names.
     * Is @b optional.
     */
    ukv_collection_t const* collections;
    /**
     * @brief Step between `collections`.
     *
     * Contains the number of bytes separating entries in the `collections` array.
     * Zero stride would reuse the same address for all tasks.
     * You can use it to retrieve different keys from the same collection in one call.
     * Is @b optional.
     */
    ukv_size_t collections_stride;
    /**
     * @brief Sequence of `keys` to retrieve.
     *
     * Contains the pointer to the first of `tasks_count` keys to be collected from `collections`.
     * If multiple keys are passed, the step between them is defined by `keys_stride`.
     */
    ukv_key_t const* keys;
    /**
     * @brief Step between `keys`.
     *
     * The number of bytes separating entries in the `keys` array.
     * Zero stride would reuse the same address for all tasks.
     * You can use it to retrieve the same key from different collections in one call.
     * Is @b optional.
     */
    ukv_size_t keys_stride;

    /// @}
    /// @name Outputs
    /// @{

    /**
     * @brief Output presence (non-NULL) indicators.
     *
     * Will contain a bitset with at least `tasks_count` bits.
     * Each set bit means that such key is present in DB.
     * Is addressed the same way as in Apache Arrow.
     * Is @b optional.
     */
    ukv_octet_t** presences;
    /**
     * @brief Output content offsets within `values`.
     *
     * Will contain a pointer to an array of `tasks_count` integer offsets.
     * Each marks a response offset in bytes starting from `values`.
     * To be fully compatible with Apache Arrow we append one more offset at
     * the end to allow inferring the length of the last entry without using `lengths`.
     * Is @b optional, as you may only want to get `lengths` or check `presences`.
     */
    ukv_length_t** offsets;
    /**
     * @brief Output content lengths within `values`.
     *
     * Will contain a pointer to an array of `tasks_count` integer offsets.
     * Each defines a response length in bytes within `values`.
     * Is @b optional.
     */
    ukv_length_t** lengths;
    /**
     * @brief Output content tape.
     *
     * Will contain the base pointer for the `tasks_count` strings.
     * Instead of allocating every "string" separately, we join them into
     * a single "tape" structure, which later be exported into (often disjoint)
     * runtime- or library-specific implementations.
     * You should use `presences`, `offsets` and `lengths` to split the contents.
     * - If `offsets` and `lengths` are exported, entries aren't guaranteed to be in any order.
     * - If only `offsets` are exported, entries are concatenated in-order without any gaps.
     *   The resulting `offsets` are guaranteed to have `tasks_count + 1` entries for Apache Arrow.
     *   The last offset will be used to determine the length of the last entry.
     *   Zero-length and Missing entries will be indistinguishable unless you also check `presences`.
     * - If only `lengths` are exported, entries are concatenated in-order without any gaps.
     *   Missing entries will have a length equal to `::ukv_length_missing_k`.
     *
     * Is @b optional, as you may only want to get `lengths` or check `presences`.
     */
    ukv_byte_t** values;
    /// @}
} ukv_read_t;

/**
 * @brief Main "getter" interface.
 * @see `ukv_read_t`.
 */
void ukv_read(ukv_read_t*);

/**
 * @brief Main "scanning", "range selection", "iteration", "enumeration" interface.
 * @see `ukv_scan()`.
 *
 * Retrieves the following (upto) `count_limits[i]` keys starting
 * from `start_key[i]` or the smallest following key in each collection.
 * Values are not exported, for that - follow up with a `ukv_read()` or
 * a higher-level interface for Graphs, Docs or other modalities.
 *
 * ## Scans vs Iterators
 *
 * Implementing consistent iterators over concurrent state is exceptionally
 * expensive, thus we plan to implement those via "Pagination".
 *
 * ## Bulk Scans
 *
 */
typedef struct ukv_scan_t {

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
     * @brief The transaction in which the operation will be watched.
     * @see `ukv_transaction_init()`, `ukv_transaction_commit()`, `ukv_transaction_free()`.
     */
    ukv_transaction_t transaction;
    /**
     * @brief Reusable memory handle.
     * @see `ukv_arena_free()`.
     */
    ukv_arena_t* arena;
    /**
     * @brief Scan options.
     *
     * Possible values:
     * - `::ukv_option_scan_bulk_k`: Allows out-of-order retrieval for higher throughput.
     * - `::ukv_option_transaction_dont_watch_k`: Disables collision-detection for transactional reads.
     * - `::ukv_option_read_shared_memory_k`: Exports to shared memory to accelerate inter-process communication.
     * - `::ukv_option_dont_discard_memory_k`: Won't reset the `arena` before the operation begins.
     */
    ukv_options_t options;

    /// @}
    /// @name Inputs
    /// @{

    /**
     * @brief Number of separate operations packed in this read.
     * Always equal to the number of provided `start_keys`.
     */
    ukv_size_t tasks_count;
    /**
     * @brief Sequence of collections owning the `start_keys`.
     *
     * If `NULL` is passed, the default collection is assumed.
     * If multiple collections are passed, the step between them is defined by `collections_stride`.
     * Use `ukv_collection_create()` or `ukv_collection_list()` to obtain collection IDs for string names.
     * Is @b optional.
     */
    ukv_collection_t const* collections;
    /**
     * @brief Step between `collections`.
     *
     * Contains the number of bytes separating entries in the `collections` array.
     * Zero stride would reuse the same address for all tasks.
     * You can use it to retrieve different keys from the same collection in one call.
     * Is @b optional.
     */
    ukv_size_t collections_stride;
    /**
     * @brief Starting points for each scan or "range select".
     *
     * Contains the pointer to the first of `tasks_count` starting points.
     * If multiple scan tasks are passed, the step between them is defined by `start_keys_stride`.
     */
    ukv_key_t const* start_keys;
    /**
     * @brief Step between `start_keys`.
     *
     * Contains the number of bytes separating entries in the `start_keys` array.
     * Zero stride would reuse the same address for all tasks.
     * You can use it to retrieve the same "ray" of keys from different `collections`.
     * Is @b optional.
     */
    ukv_size_t start_keys_stride;
    /**
     * @brief Number of consecutive entries to read in each request.
     *
     * Contains the pointer to the first of `tasks_count` starting points.
     * If multiple scan tasks are passed, the step between them is defined by `count_limits_stride`.
     */
    ukv_length_t const* count_limits;
    /**
     * @brief Step between `count_limits`.
     *
     * Contains the number of bytes separating entries in the `count_limits` array.
     * Zero stride would reuse the same address for all tasks.
     * You can use it to retrieve identical number of entries from different start points.
     * Is @b optional.
     */
    ukv_size_t count_limits_stride;

    /// @}
    /// @name Outputs
    /// @{

    /**
     * @brief Output number of exported entries before for each scan.
     *
     * Will contain a pointer to an array of `tasks_count` integer offsets.
     * Each marks a response offset in number of @c ukv_key_t keys starting from `*keys`.
     * To be fully compatible with Apache Arrow we append one more offset at
     * the end to allow inferring the length of the last entry without using `lengths`.
     * Is @b optional, as you may only want to get `lengths` or check `presences`.
     */
    ukv_length_t** offsets;
    /**
     * @brief Output number of found entries for each scan.
     *
     * Will contain a pointer to an array of `tasks_count` integer offsets.
     * Each defines a response length in bytes within `keys`.
     * For any `i` under `tasks_count`, this holds: `count_limits[i] >= counts[i]`.
     * Is @b optional.
     */
    ukv_length_t** counts;
    /**
     * @brief Output keys tape.
     *
     * Will contain the base pointer for the `tasks_count` variable length arrays.
     * Instead of allocating every array of key ranges separately, we join them into
     * a single "tape" structure, which later be exported into (often disjoint)
     * runtime- or library-specific implementations.
     */
    ukv_key_t** keys;
    /// @}

} ukv_scan_t;

/**
 * @brief Main "scanning", "range selection", "iteration", "enumeration" interface.
 * @see `ukv_scan_t`.
 */
void ukv_scan(ukv_scan_t*);

/**
 * @brief Uniformly randomly samples keys from provided collections.
 * @see `ukv_sample()`.
 */
typedef struct ukv_sample_t {

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
     * @brief The transaction in which the operation will be watched.
     * @see `ukv_transaction_init()`, `ukv_transaction_commit()`, `ukv_transaction_free()`.
     */
    ukv_transaction_t transaction;
    /**
     * @brief Reusable memory handle.
     * @see `ukv_arena_free()`.
     */
    ukv_arena_t* arena;
    /**
     * @brief Sampling options.
     *
     * Possible values:
     * - `::ukv_option_scan_bulk_k`: Allows out-of-order retrieval for higher throughput.
     * - `::ukv_option_transaction_dont_watch_k`: Disables collision-detection for transactional reads.
     * - `::ukv_option_read_shared_memory_k`: Exports to shared memory to accelerate inter-process communication.
     * - `::ukv_option_dont_discard_memory_k`: Won't reset the `arena` before the operation begins.
     */
    ukv_options_t options;

    /// @}
    /// @name Inputs
    /// @{

    /**
     * @brief Number of separate operations packed in this read.
     * Always equal to the number of provided `start_keys`.
     */
    ukv_size_t tasks_count;
    /**
     * @brief Sequence of collections owning the `start_keys`.
     *
     * If `NULL` is passed, the default collection is assumed.
     * If multiple collections are passed, the step between them is defined by `collections_stride`.
     * Use `ukv_collection_create()` or `ukv_collection_list()` to obtain collection IDs for string names.
     * Is @b optional.
     */
    ukv_collection_t const* collections;
    /**
     * @brief Step between `collections`.
     *
     * Contains the number of bytes separating entries in the `collections` array.
     * Zero stride would reuse the same address for all tasks.
     * You can use it to retrieve different keys from the same collection in one call.
     * Is @b optional.
     */
    ukv_size_t collections_stride;
    /**
     * @brief Number of samples to be gathered for each request.
     *
     * Contains the pointer to the first of `tasks_count` starting points.
     * If multiple samples are requested, the step between them is defined by `count_limits_stride`.
     */
    ukv_length_t const* count_limits;
    /**
     * @brief Step between `count_limits`.
     *
     * Contains the number of bytes separating entries in the `count_limits` array.
     * Zero stride would reuse the same address for all tasks.
     * You can use it to retrieve identical number of entries from different collections.
     * Is @b optional.
     */
    ukv_size_t count_limits_stride;

    /// @}
    /// @name Outputs
    /// @{

    /**
     * @brief Output number of exported entries before for each scan.
     *
     * Will contain a pointer to an array of `tasks_count` integer offsets.
     * Each marks a response offset in number of @c ukv_key_t keys starting from `*keys`.
     * To be fully compatible with Apache Arrow we append one more offset at
     * the end to allow inferring the length of the last entry without using `lengths`.
     * Is @b optional, as you may only want to get `lengths` or check `presences`.
     */
    ukv_length_t** offsets;
    /**
     * @brief Output number of found entries for each scan.
     *
     * Will contain a pointer to an array of `tasks_count` integer offsets.
     * Each defines a response length in bytes within `keys`.
     * For any `i` under `tasks_count`, this holds: `count_limits[i] >= counts[i]`.
     * Is @b optional.
     */
    ukv_length_t** counts;
    /**
     * @brief Output keys tape.
     *
     * Will contain the base pointer for the `tasks_count` variable length arrays.
     * Instead of allocating every array of key ranges separately, we join them into
     * a single "tape" structure, which later be exported into (often disjoint)
     * runtime- or library-specific implementations.
     */
    ukv_key_t** keys;
    /// @}

} ukv_sample_t;

/**
 * @brief Uniformly randomly samples keys from provided collections.
 * @see `ukv_sample_t`.
 */
void ukv_sample(ukv_sample_t*);

/**
 * @brief Estimates the number of entries and memory usage for a range of keys.
 * @see `ukv_measure()`.
 */
typedef struct ukv_measure_t {

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
     * @brief The transaction in which the operation will be watched.
     * @see `ukv_transaction_init()`, `ukv_transaction_commit()`, `ukv_transaction_free()`.
     */
    ukv_transaction_t transaction;
    /**
     * @brief Reusable memory handle.
     * @see `ukv_arena_free()`.
     */
    ukv_arena_t* arena;
    /**
     * @brief Scan options.
     *
     * Possible values:
     * - `::ukv_option_scan_bulk_k`: Allows out-of-order retrieval for higher throughput.
     * - `::ukv_option_transaction_dont_watch_k`: Disables collision-detection for transactional reads.
     * - `::ukv_option_read_shared_memory_k`: Exports to shared memory to accelerate inter-process communication.
     * - `::ukv_option_dont_discard_memory_k`: Won't reset the `arena` before the operation begins.
     */
    ukv_options_t options;

    /// @}
    /// @name Inputs
    /// @{

    /**
     * @brief Number of separate operations packed in this read.
     * Always equal to the number of provided `start_keys`.
     */
    ukv_size_t tasks_count;
    /**
     * @brief Sequence of collections owning the `start_keys`.
     *
     * If `NULL` is passed, the default collection is assumed.
     * If multiple collections are passed, the step between them is defined by `collections_stride`.
     * Use `ukv_collection_create()` or `ukv_collection_list()` to obtain collection IDs for string names.
     * Is @b optional.
     */
    ukv_collection_t const* collections;
    /**
     * @brief Step between `collections`.
     *
     * Contains the number of bytes separating entries in the `collections` array.
     * Zero stride would reuse the same address for all tasks.
     * You can use it to retrieve different keys from the same collection in one call.
     * Is @b optional.
     */
    ukv_size_t collections_stride;
    /**
     * @brief Starting points for each estimate.
     *
     * Contains the pointer to the first of `tasks_count` starting points.
     * If multiple scan tasks are passed, the step between them is defined by `start_keys_stride`.
     */
    ukv_key_t const* start_keys;
    /**
     * @brief Step between `start_keys`.
     *
     * Contains the number of bytes separating entries in the `start_keys` array.
     * Zero stride would reuse the same address for all tasks.
     * You can use it to retrieve the same "ray" of keys from different `collections`.
     * Is @b optional.
     */
    ukv_size_t start_keys_stride;
    /**
     * @brief Ending points for each estimate.
     *
     * Contains the pointer to the first of `tasks_count` ending points.
     * If multiple scan tasks are passed, the step between them is defined by `end_keys_stride`.
     */
    ukv_key_t const* end_keys;
    /**
     * @brief Step between `end_keys`.
     *
     * Contains the number of bytes separating entries in the `end_keys` array.
     * Zero stride would reuse the same address for all tasks.
     * You can use it to retrieve the same "ray" of keys from different `collections`.
     * Is @b optional.
     */
    ukv_size_t end_keys_stride;

    /// @}
    /// @name Outputs
    /// @{

    ukv_size_t** min_cardinalities;
    ukv_size_t** max_cardinalities;
    ukv_size_t** min_value_bytes;
    ukv_size_t** max_value_bytes;
    ukv_size_t** min_space_usages;
    ukv_size_t** max_space_usages;

    /// @}

} ukv_measure_t;

/**
 * @brief Estimates the number of entries and memory usage for a range of keys.
 * @see `ukv_measure()`.
 */
void ukv_measure(ukv_measure_t*);

#ifdef __cplusplus
} /* end extern "C" */
#endif
