/**
 *  @file c/ustore/strings.h
 *  @author Ash Vardanian
 *  @date September 16, 2026
 *  @brief The binary interface for collections mapping variable-length strings onto keys of other collections.
 *
 *  Strings live in collections created with @c ustore_modality_strings_k, which map every string onto exactly one
 *  @c ustore_key_t: names, paths and terms resolved into the keys every other modality uses. Each engine keys such a
 *  collection by the strings themselves, so a prefix walk is an ordered walk of its own keyspace; the other calls
 *  refuse such a collection, and these calls refuse every other one. Strings arrive as @c ustore_strings_view_t and
 *  their values as @c ustore_keys_view_t, so this header mirrors @c blobs.h with strings in place of keys and keys in
 *  place of values.
 *
 *  Strings order by their bytes, unsigned, and a prefix walk hands them over in that order. Every value is one key,
 *  so a strings collection holds no empty values, and @c ustore_drop_values_k refuses it.
 */

#ifndef UNUM_USTORE_STRINGS_H
#define UNUM_USTORE_STRINGS_H

#include "ustore/db.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The arguments of @c ustore_strings_write, which upserts the keys strings map onto; @c ustore_strings_remove
 *  removes strings. */
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

    /** The key every string maps onto, one per string. */
    ustore_keys_view_t values;

    /** How durable the writes are once the call finishes at the head; a transaction's commit decides its own. */
    ustore_durability_t durability;

    /** Whether the @c transaction records the strings it reads and writes for conflicts. */
    ustore_watch_t watch;

} ustore_strings_write_t;

/**
 *  @brief Writes a batch of strings and the keys they map onto.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_contended_k The @c transaction serves another call, the executor's deferred
 *    table is full, or a callback blocks on the executor running it.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL.
 *  - @c ustore_status_read_only_k The database was opened read-only.
 *  - @c ustore_status_invalid_argument_k The @c collections, @c strings or @c values are missing,
 *    the strings come in an unknown layout, the @c transaction belongs to another engine or is
 *    staged, a flushed write names a @c transaction on the RAM engine, or a collection holds another
 *    modality.
 *  - @c ustore_status_unsupported_k The executor computes on a device.
 *  - @c ustore_status_not_found_k A collection does not exist or was dropped meanwhile.
 *  - @c ustore_status_write_conflict_k Concurrent commits kept conflicting, or a staged RocksDB
 *    transaction holds a string.
 *  - @c ustore_status_capacity_exhausted_k The strings outgrow one NVMe run file.
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

    /** Whether the @c transaction records the strings it reads and writes for conflicts. */
    ustore_watch_t watch;

} ustore_strings_remove_t;

/**
 *  @brief Removes a batch of strings with their values.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_contended_k The @c transaction serves another call, the executor's deferred
 *    table is full, or a callback blocks on the executor running it.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL.
 *  - @c ustore_status_read_only_k The database was opened read-only.
 *  - @c ustore_status_invalid_argument_k The @c collections or @c strings are missing, the strings
 *    come in an unknown layout, the @c transaction belongs to another engine or is staged, a
 *    flushed remove names a @c transaction on the RAM engine, or a collection holds another
 *    modality.
 *  - @c ustore_status_unsupported_k The executor computes on a device.
 *  - @c ustore_status_not_found_k A collection does not exist or was dropped meanwhile.
 *  - @c ustore_status_write_conflict_k Concurrent commits kept conflicting, or a staged RocksDB
 *    transaction holds a string.
 *  - @c ustore_status_capacity_exhausted_k The strings outgrow one NVMe run file.
 */
USTORE_EXPORT void ustore_strings_remove(ustore_strings_remove_t* arguments);

/** The arguments of @c ustore_strings_read, which reads the keys strings map onto, or their presence. */
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

    /** Optional output for the key of every string inside @c arena, in the order of the strings, where a missing
     *  string reads as the nil UUID. */
    ustore_key_t const** values;

    /** Rewound by this call, then holding every output it writes; @c NULL is refused. */
    ustore_arena_t arena;

} ustore_strings_read_t;

/**
 *  @brief Reads the keys a batch of strings map onto.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c arena, @c collections or @c strings are missing,
 *    the strings come in an unknown layout, the @c view was made by neither view constructor or
 *    belongs to another engine, a watching read goes through a staged transaction, or a collection
 *    holds another modality.
 *  - @c ustore_status_device_k The arena lives where another executor computes.
 *  - @c ustore_status_contended_k The arena or the transaction the @c view reads through serves
 *    another call, the executor's deferred table is full, or a callback blocks on the executor
 *    running it.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL.
 *  - @c ustore_status_unsupported_k The executor computes on a device, where no engine serves the
 *    call yet.
 *  - @c ustore_status_not_found_k A collection does not exist or was dropped meanwhile.
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

    /** Optional patterns every listed string matches, shared by every scan of the call; zeroed lists every string. */
    ustore_patterns_t patterns;

    /** Optional verdict on the keys the listed strings map onto, asked once per page as its @c cost says; host
     *  executors only. */
    ustore_predicate_t predicate;

    /** Output for @c count plus one offsets into @c strings, the last one closing the final scan, inside @c arena. */
    ustore_length_t const** offsets;

    /** Output for the found strings as a tape inside @c arena, scan after scan. */
    ustore_strings_view_t* strings;

    /** Optional output for one BM25 score per listed string when @c patterns carry weights, inside @c arena; @c NULL
     *  skips them. */
    float const** scores;

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
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c arena, @c collections or an output is missing, the
 *    prefixes or start strings come in an unknown layout, a pattern kind, a case sensitivity or a
 *    predicate cost is unknown, the @c mode is unknown under @c patterns or a predicate, the
 *    @c patterns do not compile, the @c view was made by neither view constructor or belongs to
 *    another engine, a watching scan goes through a staged transaction, or a collection holds
 *    another modality.
 *  - @c ustore_status_device_k The arena lives where another executor computes.
 *  - @c ustore_status_contended_k The arena or the transaction the @c view reads through serves
 *    another call, the executor's deferred table is full, or a callback blocks on the executor
 *    running it.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL.
 *  - @c ustore_status_unsupported_k The executor computes on a device, where no engine serves the
 *    call yet, or on one matching no values, as AMD devices do, given @c patterns.
 *  - @c ustore_status_not_found_k A collection does not exist or was dropped meanwhile.
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

    /** Executor of the kind every pull runs on, which picks the side the walk computes on, where @c NULL walks on host
     *  executors; a pull on another kind of executor reports @c ustore_status_device_k. */
    ustore_executor_t executor;

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

    /** Optional patterns every string the walk hands out matches, compiled here once; zeroed walks every string. */
    ustore_patterns_t patterns;

    /** Optional verdict on the keys of the strings of every page, asked as its @c cost says; host executors only. */
    ustore_predicate_t predicate;

} ustore_strings_cursor_init_t;

/**
 *  @brief Opens a walk over the strings of one prefix, reading nothing until its first pull.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c cursor output is @c NULL, the @c view was made by
 *    neither view constructor or reads another database, a pattern kind, a case sensitivity or a
 *    predicate cost is unknown, or the @c patterns do not compile.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL.
 *  - @c ustore_status_unsupported_k A device @c executor is given @c patterns.
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

    /** Optional output for the keys of the page's strings inside @c arena, where a string removed at the head
     *  between the listing and the read of one pull reads as the nil UUID; @c NULL skips them. */
    ustore_key_t const** values;

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
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c cursor, the @c arena or an output is @c NULL, the
 *    cursor walks another database, a watching pull goes through a staged transaction, or the
 *    collection holds another modality.
 *  - @c ustore_status_device_k The arena lives where another executor computes, or a cursor opened
 *    for a device is pulled on host threads.
 *  - @c ustore_status_contended_k The arena, the cursor or the transaction it reads through serves
 *    another call, the executor's deferred table is full, or a callback blocks on the executor
 *    running it.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL.
 *  - @c ustore_status_unsupported_k The executor computes on a device, where no engine walks yet.
 *  - @c ustore_status_not_found_k The collection does not exist or was dropped meanwhile.
 */
USTORE_EXPORT void ustore_strings_cursor_next(ustore_strings_cursor_next_t* arguments);

/**
 *  @brief Waits until every deferred call naming the cursor finishes, then frees it, giving back the holds it kept on
 *      its database and on the transaction or snapshot it reads through.
 *  @param[in,out] cursor The cursor to be freed; @c NULL is safe.
 */
USTORE_EXPORT void ustore_strings_cursor_free(ustore_strings_cursor_t cursor);

#ifdef __cplusplus
}
#endif

#endif // UNUM_USTORE_STRINGS_H
