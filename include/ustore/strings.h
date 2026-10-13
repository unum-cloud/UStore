/**
 *  @file include/ustore/strings.h
 *  @author Ash Vardanian
 *  @date September 16, 2026
 *  @brief The binary interface for collections keyed by variable-length strings, mapped onto arbitrary bytes.
 *
 *  Strings live in collections created with @c ustore_modality_strings_k, which each engine keys by the strings
 *  themselves, so a prefix walk is an ordered walk of its own keyspace; the other calls refuse such a collection,
 *  and these calls refuse every other one. Strings and their values both arrive as @c ustore_strings_view_t, so
 *  this header mirrors @c blobs.h with strings in place of keys.
 *
 *  Strings order by their bytes, unsigned, and a prefix walk hands them over in that order. RAM, RocksDB and
 *  LevelDB key a strings collection by the strings themselves. An engine without such a keyspace maps a
 *  string onto a key of its leading @c sizeof(ustore_key_t) minus one bytes, zero-padded, followed by its
 *  length, and a longer string takes a marker length and lives in a sorted bucket under the key its leading
 *  run shares, so a prefix walk stays an ordered walk of keys.
 */

#ifndef UNUM_USTORE_STRINGS_H
#define UNUM_USTORE_STRINGS_H

#include "ustore/db.h"

#ifdef __cplusplus
extern "C" {
#endif

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
    ustore_collections_view_t collections;

    /** Strings to write, one per task. */
    ustore_strings_view_t strings;

    /** Values to write, one per string; a tape @c ustore_strings_read wrote is written back as it is. */
    ustore_strings_view_t values;

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
    ustore_collections_view_t collections;

    /** Strings to remove, one per task. */
    ustore_strings_view_t strings;

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
    ustore_collections_view_t collections;

    /** Strings to read, one per task. */
    ustore_strings_view_t strings;

    /** Optional output for a bitset of @c count bits marking present strings, addressed as Apache Arrow does, inside
     *  @c arena. */
    ustore_octet_t const** presences;

    /** Optional output for the length of every value, for reads that skip the bytes, inside @c arena. */
    ustore_length_t const** lengths;

    /** Optional output for the values as a tape inside @c arena, in the order of the strings, where a missing string
     *  reads as an empty value. */
    ustore_strings_view_t* values;

    /** Rewound by this call, then holding every output it writes; @c NULL is refused. */
    ustore_arena_t arena;

} ustore_strings_read_t;

/**
 *  @brief Reads a batch of values addressed by their strings.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_strings_read(ustore_strings_read_t* arguments);

/**
 *  @brief The arguments of @c ustore_strings_scan, which lists up to @c wanted strings under every prefix.
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
    ustore_collections_view_t collections;

    /** Prefixes every scan matches, where a zeroed view matches every string. */
    ustore_strings_view_t prefixes;

    /** Smallest string every scan may return, which paginates a walk of one prefix; a zeroed view starts first. */
    ustore_strings_view_t start_strings;

    /** Output for @c count plus one offsets into @c strings, the last one closing the final scan, inside @c arena. */
    ustore_length_t const** offsets;

    /** Output for the found strings as a tape inside @c arena, scan after scan. */
    ustore_strings_view_t* strings;

    /** Rewound by this call, then holding every output it writes; @c NULL is refused. */
    ustore_arena_t arena;

    /** Most strings every scan returns. */
    ustore_size_t wanted;

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

/**
 *  @brief An opaque handle to a walk over the strings of one prefix, which every pull resumes past the last string
 *      the pull before it handed out.
 *
 *  It keeps a copy of its prefix and of the string its next pull starts at, and holds its database open, and the
 *  transaction or snapshot it reads through, until @c ustore_strings_cursor_free, as @c ustore_blobs_cursor_t does. A
 *  cursor serves one pull at a time, so a second pull naming it finishes @c ustore_status_contended_k.
 */
typedef struct ustore_strings_cursor_handle_t* ustore_strings_cursor_t;

/** The arguments of @c ustore_strings_cursor_init. */
typedef struct ustore_strings_cursor_init_t {

    /** Output for the new cursor, which the caller frees with @c ustore_strings_cursor_free; @c NULL on failure. */
    ustore_strings_cursor_t* cursor;

    /** An open database. */
    ustore_database_t database;

    /** Output for the outcome, final on return. */
    ustore_outcome_t* outcome;

    /** What the walk sees, where @c NULL is the head, read pull by pull. */
    ustore_view_t view;

    /** Strings collection to walk. */
    ustore_collection_id_t collection;

    /** Prefix every string the walk returns starts with, copied, where a zeroed view matches every string. */
    ustore_bytes_view_t prefix;

    /** Smallest string the walk returns, copied, where a zeroed view starts at the first string of the prefix. */
    ustore_bytes_view_t start_string;

} ustore_strings_cursor_init_t;

/**
 *  @brief Opens a walk over the strings of one prefix, reading nothing until its first pull.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_strings_cursor_init(ustore_strings_cursor_init_t* arguments);

/** The arguments of @c ustore_strings_cursor_next, which pulls the next page of a walk, with the values of its strings
 *  when asked, in one call. */
typedef struct ustore_strings_cursor_next_t {

    /** The database the cursor was opened on. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** The cursor to advance, which a pull that fails or is cancelled leaves where it was. */
    ustore_strings_cursor_t cursor;

    /** Output for the strings of the page in the order of their text, as a tape inside @c arena. */
    ustore_strings_view_t* strings;

    /** Optional output for their values as a tape inside @c arena, where a string removed at the head between the
     *  listing and the read of one pull reads as an empty value; @c NULL skips them. */
    ustore_strings_view_t* values;

    /** Output for the strings written, zero once the walk is exhausted; zeroed on failure. */
    ustore_size_t* written;

    /** Rewound by this call, then holding every output it writes; @c NULL is refused. */
    ustore_arena_t arena;

    /** Most strings the page holds, where a page of fewer exhausts the walk. */
    ustore_size_t wanted;

    /** Optional callback reporting the one pull, which may cancel it before the cursor advances. */
    ustore_progress_callback_t progress;

    /** Opaque pointer passed to @c progress. */
    void* progress_state;

} ustore_strings_cursor_next_t;

/**
 *  @brief Pulls the next page of a walk, and the values of its strings when asked.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_strings_cursor_next(ustore_strings_cursor_next_t* arguments);

/**
 *  @brief Waits until every deferred call naming the cursor finishes, then frees it, giving back the holds it kept on
 *      its database and on the transaction or snapshot it reads through.
 *  @param[inout] cursor The cursor to be freed; @c NULL is safe.
 */
USTORE_EXPORT void ustore_strings_cursor_free(ustore_strings_cursor_t cursor);

#ifdef __cplusplus
}
#endif

#endif // UNUM_USTORE_STRINGS_H
