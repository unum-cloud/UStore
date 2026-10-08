/**
 *  @file include/ustore/strings.h
 *  @author Ash Vardanian
 *  @date September 16, 2026
 *  @brief The binary interface for collections keyed by variable-length strings, mapped onto arbitrary bytes.
 *
 *  Strings live in collections created with @c ustore_modality_strings_k, which each engine keys by the strings
 *  themselves, so a prefix walk is an ordered walk of its own keyspace; the other calls refuse such a collection,
 *  and these calls refuse every other one.
 *
 *  Strings order by their bytes, unsigned, and a prefix walk hands them over in that order. RAM, RocksDB and
 *  LevelDB key a strings collection by the strings themselves. An engine without such a keyspace maps a
 *  string onto a key of its leading @c sizeof(ustore_key_t) minus one bytes, zero-padded, followed by its
 *  length, and a longer string takes a marker length and lives in a sorted bucket under the key its leading
 *  run shares, so a prefix walk stays an ordered walk of keys.
 */

#pragma once

#include "ustore/db.h"

#ifdef __cplusplus
extern "C" {
#endif

#pragma region Writes

/**
 *  @brief The arguments of @c ustore_strings_write, which upserts the values of strings.
 *
 *  Strings sharing a bucket are rewritten together, so one batch touching many of them costs one read and one
 *  write; @c ustore_strings_remove removes strings.
 */
typedef struct ustore_strings_write_t {

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

    /** Number of tasks, one per string. */
    ustore_size_t count;

    /** Strings collections owning the strings. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Strings to write, @c NULL only when there are no tasks. */
    ustore_str_view_t const* strings;

    /** Bytes between consecutive string pointers, where zero repeats the first one. */
    ustore_size_t strings_stride;

    /** Optional lengths of the strings, inferred from null terminators when absent. */
    ustore_length_t const* strings_lengths;

    /** Bytes between consecutive string lengths, where zero repeats the first one. */
    ustore_size_t strings_lengths_stride;

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

    /** Whether the @c transaction records the buckets it reads and writes for conflicts. */
    ustore_watch_t watch;

} ustore_strings_write_t;

/**
 *  @brief Writes a batch of strings and their values.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_strings_write(ustore_strings_write_t* arguments);

/** The arguments of @c ustore_strings_remove, which removes strings with their values in a batch. */
typedef struct ustore_strings_remove_t {

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

    /** Number of tasks, one per string. */
    ustore_size_t count;

    /** Strings collections owning the strings. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Strings to remove, @c NULL only when there are no tasks. */
    ustore_str_view_t const* strings;

    /** Bytes between consecutive string pointers, where zero repeats the first one. */
    ustore_size_t strings_stride;

    /** Optional lengths of the strings, inferred from null terminators when absent. */
    ustore_length_t const* strings_lengths;

    /** Bytes between consecutive string lengths, where zero repeats the first one. */
    ustore_size_t strings_lengths_stride;

    /** How durable the removals are once the call finishes at the head; a transaction's commit decides its own. */
    ustore_durability_t durability;

    /** Whether the @c transaction records the buckets it reads and writes for conflicts. */
    ustore_watch_t watch;

} ustore_strings_remove_t;

/**
 *  @brief Removes a batch of strings with their values.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_strings_remove(ustore_strings_remove_t* arguments);

#pragma endregion Writes

#pragma region Reads

/** The arguments of @c ustore_strings_read, which reads the values of strings, their lengths or their presence. */
typedef struct ustore_strings_read_t {

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

    /** Number of tasks, one per string. */
    ustore_size_t count;

    /** Strings collections owning the strings. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Strings to read, @c NULL only when there are no tasks. */
    ustore_str_view_t const* strings;

    /** Bytes between consecutive string pointers, where zero repeats the first one. */
    ustore_size_t strings_stride;

    /** Optional lengths of the strings, inferred from null terminators when absent. */
    ustore_length_t const* strings_lengths;

    /** Bytes between consecutive string lengths, where zero repeats the first one. */
    ustore_size_t strings_lengths_stride;

    /** Optional output for a bitset of at least @c count bits marking present strings, addressed
     *  as Apache Arrow does. */
    ustore_octet_t* presences;

    /** Optional output for @c count plus one offsets into @c values, the last one closing the
     *  final value as Apache Arrow expects. */
    ustore_length_t* offsets;

    /** Optional output for the tape of values in the order of the strings, inside @c arena. */
    ustore_byte_t** values;

    /** Memory for @c values: reused when it points to an arena, created when it points to @c NULL. */
    ustore_arena_t* arena;

} ustore_strings_read_t;

/**
 *  @brief Reads a batch of values addressed by their strings.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_strings_read(ustore_strings_read_t* arguments);

#pragma endregion Reads

#pragma region Scans

/**
 *  @brief The arguments of @c ustore_strings_scan, which lists up to a limit of strings under every prefix.
 *
 *  Strings come in the order of their text. Values are not exported, so a read follows the scan, and a
 *  scan resumes from @c start_strings rather than holding an iterator over concurrent state.
 */
typedef struct ustore_strings_scan_t {

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

    /** Strings collections to scan. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Prefixes every scan matches, where an empty prefix walks the whole collection. */
    ustore_str_view_t const* prefixes;

    /** Bytes between consecutive prefix pointers, where zero repeats the first one. */
    ustore_size_t prefixes_stride;

    /** Optional lengths of the prefixes, inferred from null terminators when absent. */
    ustore_length_t const* prefixes_lengths;

    /** Bytes between consecutive prefix lengths, where zero repeats the first one. */
    ustore_size_t prefixes_lengths_stride;

    /** Optional smallest string every scan may return, which paginates a walk of one prefix. */
    ustore_str_view_t const* start_strings;

    /** Bytes between consecutive start strings, where zero repeats the first one. */
    ustore_size_t start_strings_stride;

    /** Optional lengths of the start strings, inferred from null terminators when absent. */
    ustore_length_t const* start_strings_lengths;

    /** Bytes between consecutive start string lengths, where zero repeats the first one. */
    ustore_size_t start_strings_lengths_stride;

    /** Most strings every scan returns, never @c NULL. */
    ustore_length_t const* count_limits;

    /** Bytes between consecutive limits, where zero repeats the first one. */
    ustore_size_t count_limits_stride;

    /** Output for @c count plus one offsets into the found strings, the last one closing the final scan. */
    ustore_length_t* offsets;

    /** Output for one offset into @c strings per found string, plus one closing the last one, with room for the
     *  sum of @c count_limits plus one. */
    ustore_length_t* strings_offsets;

    /** Output for the tape of found strings, which follow each other without gaps, inside @c arena. */
    ustore_char_t** strings;

    /** Memory for @c strings: reused when it points to an arena, created when it points to @c NULL. */
    ustore_arena_t* arena;

    /** In what order the strings come. */
    ustore_scan_mode_t mode;

    /** Optional callback reporting the scans finished. */
    ustore_progress_callback_t progress;

    /** Opaque pointer passed to @c progress. */
    void* progress_state;

} ustore_strings_scan_t;

/**
 *  @brief Scans a batch of prefixes.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_strings_scan(ustore_strings_scan_t* arguments);

#pragma endregion Scans

#ifdef __cplusplus
}
#endif
