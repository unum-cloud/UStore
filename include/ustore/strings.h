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
 *  @brief The arguments of @c ustore_strings_write, which upserts or removes the values of strings.
 *
 *  A task whose value is absent removes its string, as @c ustore_write removes a key. Strings sharing a
 *  bucket are rewritten together, so one batch touching many of them costs one read and one write.
 */
typedef struct ustore_strings_write_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Transaction that watches the written buckets. */
    ustore_transaction_t transaction;

    /** Reusable memory for intermediate results. */
    ustore_arena_t* arena;

    /** Read and write options, as @c ustore_read_t and @c ustore_write_t accept them. */
    ustore_options_t options;

    /** Number of tasks, one per string. */
    ustore_size_t tasks_count;

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

    /** Optional bitset of at least @c tasks_count bits, where a set bit marks a value that is not
     *  null, addressed as Apache Arrow does. */
    ustore_octet_t const* values_presences;

    /** Offsets of the values within the chunks of @c values, which may skip the header of a runtime object. */
    ustore_length_t const* values_offsets;

    /** Bytes between consecutive value offsets, where zero repeats the first one. */
    ustore_size_t values_offsets_stride;

    /** Optional lengths of the values, inferred from consecutive offsets or null terminators when absent. */
    ustore_length_t const* values_lengths;

    /** Bytes between consecutive value lengths, where zero repeats the first one. */
    ustore_size_t values_lengths_stride;

    /** Pointers to the chunks holding the values, where @c NULL removes every string. */
    ustore_bytes_cptr_t const* values;

    /** Bytes between consecutive chunk pointers, where zero repeats the first one. */
    ustore_size_t values_stride;

} ustore_strings_write_t;

/**
 *  @brief Writes a batch of strings and their values.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_strings_write(ustore_strings_write_t* arguments);

#pragma endregion Writes

#pragma region Reads

/** The arguments of @c ustore_strings_read, which reads the values of strings, their lengths or their presence. */
typedef struct ustore_strings_read_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Transaction that watches the read buckets. */
    ustore_transaction_t transaction;

    /** Optional snapshot whose view is read, never set beside a transaction. */
    ustore_snapshot_t snapshot;

    /** Reusable memory for the results. */
    ustore_arena_t* arena;

    /** Read options, as @c ustore_read_t accepts them. */
    ustore_options_t options;

    /** Number of tasks, one per string. */
    ustore_size_t tasks_count;

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

    /** Optional output for a bitset of at least @c tasks_count bits marking present strings, addressed
     *  as Apache Arrow does. */
    ustore_octet_t** presences;

    /** Optional output for @c tasks_count plus one offsets into @c values, the last one closing the
     *  final value as Apache Arrow expects. */
    ustore_length_t** offsets;

    /** Optional output for the lengths of the values, with @c ustore_length_missing_k for missing strings. */
    ustore_length_t** lengths;

    /** Optional output for the tape of values, which follow each other without gaps. */
    ustore_byte_t** values;

} ustore_strings_read_t;

/**
 *  @brief Reads a batch of values addressed by their strings.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_strings_read(ustore_strings_read_t* arguments);

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
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Transaction that watches the scanned buckets. */
    ustore_transaction_t transaction;

    /** Optional snapshot whose view is scanned, never set beside a transaction. */
    ustore_snapshot_t snapshot;

    /** Reusable memory for the results. */
    ustore_arena_t* arena;

    /** Read and scan options, as @c ustore_read_t and @c ustore_scan_t accept them. */
    ustore_options_t options;

    /** Number of scans. */
    ustore_size_t tasks_count;

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

    /** Optional output for @c tasks_count plus one offsets into the found strings, the last one closing
     *  the final scan. */
    ustore_length_t** offsets;

    /** Optional output for the number of strings every scan found, never above its limit. */
    ustore_length_t** counts;

    /** Optional output for one offset into @c strings per found string, plus one closing the last one. */
    ustore_length_t** strings_offsets;

    /** Output for the tape of found strings, which follow each other without gaps. */
    ustore_char_t** strings;

} ustore_strings_scan_t;

/**
 *  @brief Scans a batch of prefixes.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_strings_scan(ustore_strings_scan_t* arguments);

#pragma endregion Scans

#ifdef __cplusplus
}
#endif
