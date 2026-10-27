/**
 *  @file c/ustore/pages.h
 *  @author Ash Vardanian
 *  @date September 22, 2026
 *  @brief The binary interface for pages: ordered documents such as HTML, XML, Markdown, spreadsheets
 *      and slide decks, edited concurrently by many authors.
 *
 *  A JSON document of @c docs.h is a map addressed by field, replaced or patched as a whole value. A
 *  page is an ordered tree of elements addressed by identity, where every element carries a key, a
 *  tag, attributes and text, and where the order of siblings is the content. A paragraph is an
 *  element whose text is a run of characters, a spreadsheet row is an element whose children are its
 *  cells, and a slide is an element whose children carry geometry in their attributes.
 *
 *  Edits are positional and small, one verb each: insert elements after a sibling, remove or move a
 *  subtree, splice bytes inside a text run, or set or remove an attribute. An element inserted without
 *  a key is minted a time-ordered one, and concurrent insertions after the same sibling order newest
 *  first, so authors editing one position converge on one order without coordination, as replicated
 *  growable arrays do. Element keys are stable across every edit, so a formula, a link or a comment
 *  refers to an element and survives the rows moving around it.
 *
 *  Every edit names its page by key and its element, parent or anchor by element key, where the page
 *  key doubles as the key of the root element. Tasks of one call apply in order inside one commit, so
 *  a later task may anchor on an element an earlier one minted.
 *
 *  A page stores only its current shape, never the edits that produced it, and reads render a page or
 *  a subtree as their view sees it, in the encoding the caller asks for. Cursors, selections and
 *  presence are never stored; they are the application's ephemeral state.
 */

#ifndef UNUM_USTORE_PAGES_H
#define UNUM_USTORE_PAGES_H

#include "ustore/db.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Encoding a page or a fragment is imported from or rendered into. */
typedef enum ustore_page_encoding_t {

    /** HTML, parsed into elements and rendered back as markup. */
    ustore_page_encoding_html_k = 0,

    /** XML, with every element and attribute kept as written. */
    ustore_page_encoding_xml_k = 1,

    /** Markdown, parsed into headings, paragraphs, lists, tables and code, and rendered back. */
    ustore_page_encoding_markdown_k = 2,

    /** The text of every run in document order, with structure dropped. */
    ustore_page_encoding_text_k = 3,

    /** The element tree as JSON: key, tag, attributes, text and children, for programmatic access. */
    ustore_page_encoding_json_k = 4,

} ustore_page_encoding_t;

/** The canonical spelling of an encoding, as @c ustore_page_encoding_markdown_k spells "markdown", or
 *  "unrecognized". */
USTORE_EXPORT char const* ustore_page_encoding_name(ustore_page_encoding_t encoding);

/** Reads an encoding back from its canonical spelling; @c ustore_page_encoding_html_k when unrecognized. */
USTORE_EXPORT ustore_page_encoding_t ustore_page_encoding_from_name(ustore_bytes_view_t name);

/** The arguments of @c ustore_pages_import, which replaces whole pages with parsed markup. */
typedef struct ustore_pages_import_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** Transaction the edits join, where @c NULL writes to the head. */
    ustore_transaction_t transaction;

    /** Number of pages. */
    ustore_size_t count;

    /** Collections of the pages. */
    ustore_collections_view_t collections;

    /** Keys of the pages. */
    ustore_keys_view_t keys;

    /** Markup of the pages, one per task. */
    ustore_strings_view_t values;

    /** Encoding of the markup. */
    ustore_page_encoding_t encoding;

    /** How durable the edits are once the call finishes at the head; a transaction's commit decides its own. */
    ustore_durability_t durability;

    /** Whether the @c transaction records the pages it reads and writes for conflicts. */
    ustore_watch_t watch;

    /** Optional callback reporting the bytes of @c values parsed. */
    ustore_progress_callback_t progress;

    /** Opaque pointer passed to @c progress. */
    void* progress_state;

} ustore_pages_import_t;

/**
 *  @brief Imports whole pages, replacing what they held and minting every element.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_pages_import(ustore_pages_import_t* arguments);

/** The arguments of @c ustore_pages_insert, which parses fragments into elements under parents after anchors. */
typedef struct ustore_pages_insert_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** Transaction the edits join, where @c NULL writes to the head. */
    ustore_transaction_t transaction;

    /** Number of insertions. */
    ustore_size_t count;

    /** Collections of the pages. */
    ustore_collections_view_t collections;

    /** Keys of the pages. */
    ustore_keys_view_t keys;

    /** Keys to give the inserted elements, where Nil or @c NULL data mints one. */
    ustore_keys_view_t elements;

    /** Parents the elements land under, where Nil stands for the root. */
    ustore_keys_view_t parents;

    /** Siblings after which the elements land, where Nil stands for the front. */
    ustore_keys_view_t anchors;

    /** Fragments to parse into elements, one per task. */
    ustore_strings_view_t values;

    /** Encoding of the fragments. */
    ustore_page_encoding_t encoding;

    /** Optional output for the key every inserted element ended up with, minted or given, inside @c arena. */
    ustore_key_t const** effective_keys;

    /** Rewound by this call, then holding every output it writes; @c NULL is refused. */
    ustore_arena_t arena;

    /** How durable the edits are once the call finishes at the head; a transaction's commit decides its own. */
    ustore_durability_t durability;

    /** Whether the @c transaction records the pages it reads and writes for conflicts. */
    ustore_watch_t watch;

} ustore_pages_insert_t;

/**
 *  @brief Inserts parsed fragments as elements under parents after anchors, or at the front on a Nil anchor.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_pages_insert(ustore_pages_insert_t* arguments);

/** The arguments of @c ustore_pages_remove, which removes elements with their subtrees. */
typedef struct ustore_pages_remove_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** Transaction the edits join, where @c NULL writes to the head. */
    ustore_transaction_t transaction;

    /** Number of removals. */
    ustore_size_t count;

    /** Collections of the pages. */
    ustore_collections_view_t collections;

    /** Keys of the pages. */
    ustore_keys_view_t keys;

    /** Elements to remove with their subtrees. */
    ustore_keys_view_t elements;

    /** How durable the edits are once the call finishes at the head; a transaction's commit decides its own. */
    ustore_durability_t durability;

    /** Whether the @c transaction records the pages it reads and writes for conflicts. */
    ustore_watch_t watch;

} ustore_pages_remove_t;

/**
 *  @brief Removes elements with their subtrees.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_pages_remove(ustore_pages_remove_t* arguments);

/** The arguments of @c ustore_pages_move, which moves elements under parents after anchors. */
typedef struct ustore_pages_move_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** Transaction the edits join, where @c NULL writes to the head. */
    ustore_transaction_t transaction;

    /** Number of moves. */
    ustore_size_t count;

    /** Collections of the pages. */
    ustore_collections_view_t collections;

    /** Keys of the pages. */
    ustore_keys_view_t keys;

    /** Elements to move with their subtrees. */
    ustore_keys_view_t elements;

    /** Parents the elements land under, where Nil stands for the root. */
    ustore_keys_view_t parents;

    /** Siblings after which the elements land, where Nil stands for the front. */
    ustore_keys_view_t anchors;

    /** How durable the edits are once the call finishes at the head; a transaction's commit decides its own. */
    ustore_durability_t durability;

    /** Whether the @c transaction records the pages it reads and writes for conflicts. */
    ustore_watch_t watch;

} ustore_pages_move_t;

/**
 *  @brief Moves elements under parents after anchors, keeping their keys.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_pages_move(ustore_pages_move_t* arguments);

/** The arguments of @c ustore_pages_splice, which replaces runs of text inside elements with new bytes. */
typedef struct ustore_pages_splice_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** Transaction the edits join, where @c NULL writes to the head. */
    ustore_transaction_t transaction;

    /** Number of splices. */
    ustore_size_t count;

    /** Collections of the pages. */
    ustore_collections_view_t collections;

    /** Keys of the pages. */
    ustore_keys_view_t keys;

    /** Elements whose text is spliced. */
    ustore_keys_view_t elements;

    /** Bytes into the text of every element where its splice starts. */
    ustore_lengths_view_t text_starts;

    /** Bytes of text every splice replaces, where zero only inserts. */
    ustore_lengths_view_t text_lengths;

    /** New bytes of every splice, where an empty run only removes. */
    ustore_strings_view_t values;

    /** How durable the edits are once the call finishes at the head; a transaction's commit decides its own. */
    ustore_durability_t durability;

    /** Whether the @c transaction records the pages it reads and writes for conflicts. */
    ustore_watch_t watch;

} ustore_pages_splice_t;

/**
 *  @brief Replaces runs of text inside elements with new bytes.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_pages_splice(ustore_pages_splice_t* arguments);

/** The arguments of @c ustore_pages_set_attributes, which sets one attribute of an element per task. */
typedef struct ustore_pages_set_attributes_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** Transaction the edits join, where @c NULL writes to the head. */
    ustore_transaction_t transaction;

    /** Number of attributes to set. */
    ustore_size_t count;

    /** Collections of the pages. */
    ustore_collections_view_t collections;

    /** Keys of the pages. */
    ustore_keys_view_t keys;

    /** Elements whose attributes are set. */
    ustore_keys_view_t elements;

    /** Names of the attributes to set, one per task. */
    ustore_strings_view_t fields;

    /** Values of the attributes, one per task. */
    ustore_strings_view_t values;

    /** How durable the edits are once the call finishes at the head; a transaction's commit decides its own. */
    ustore_durability_t durability;

    /** Whether the @c transaction records the pages it reads and writes for conflicts. */
    ustore_watch_t watch;

} ustore_pages_set_attributes_t;

/**
 *  @brief Sets attributes of elements, adding the ones they lack.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_pages_set_attributes(ustore_pages_set_attributes_t* arguments);

/** The arguments of @c ustore_pages_remove_attributes, which removes one attribute of an element per task. */
typedef struct ustore_pages_remove_attributes_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** Transaction the edits join, where @c NULL writes to the head. */
    ustore_transaction_t transaction;

    /** Number of attributes to remove. */
    ustore_size_t count;

    /** Collections of the pages. */
    ustore_collections_view_t collections;

    /** Keys of the pages. */
    ustore_keys_view_t keys;

    /** Elements whose attributes are removed. */
    ustore_keys_view_t elements;

    /** Names of the attributes to remove, one per task. */
    ustore_strings_view_t fields;

    /** How durable the edits are once the call finishes at the head; a transaction's commit decides its own. */
    ustore_durability_t durability;

    /** Whether the @c transaction records the pages it reads and writes for conflicts. */
    ustore_watch_t watch;

} ustore_pages_remove_attributes_t;

/**
 *  @brief Removes attributes of elements.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_pages_remove_attributes(ustore_pages_remove_attributes_t* arguments);

/**
 *  @brief The arguments of @c ustore_pages_read, which renders pages or subtrees in one encoding.
 *
 *  A task names a page and optionally an element, where Nil renders the whole page and any other
 *  key renders that element with its subtree. The rendering is the page as its view sees it, with
 *  every committed edit folded in.
 */
typedef struct ustore_pages_read_t {

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

    /** Number of pages or subtrees to render. */
    ustore_size_t count;

    /** Collections of the pages. */
    ustore_collections_view_t collections;

    /** Keys of the pages. */
    ustore_keys_view_t keys;

    /** Elements to render with their subtrees, where @c NULL data renders whole pages. */
    ustore_keys_view_t elements;

    /** Optional output for a bitset of @c count bits marking found pages or elements, inside @c arena. */
    ustore_octet_t const** presences;

    /** Optional output for the length of every rendering, for reads that skip the bytes, inside @c arena. */
    ustore_length_t const** lengths;

    /** Optional output for the renderings as a tape inside @c arena. */
    ustore_strings_view_t* values;

    /** Rewound by this call, then holding every output it writes; @c NULL is refused. */
    ustore_arena_t arena;

    /** Encoding the renderings are converted into. */
    ustore_page_encoding_t outputs_encoding;

} ustore_pages_read_t;

/**
 *  @brief Renders pages or subtrees in one encoding.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_pages_read(ustore_pages_read_t* arguments);

/**
 *  @brief The arguments of @c ustore_pages_scan, which lists up to @c wanted page keys from a start key in every
 *      collection.
 *
 *  Pages are not rendered, so @c ustore_pages_read follows the scan. Consistent iterators over concurrent state are
 *  costly, so scans paginate instead.
 */
typedef struct ustore_pages_scan_t {

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

    /** Collections of the pages. */
    ustore_collections_view_t collections;

    /** Smallest page key every scan may return, where @c NULL data starts at the first page. */
    ustore_keys_view_t start_keys;

    /** Optional path in StringZilla's XPath subset that every listed page holds a node of, shared by every scan; a
     *  zeroed view lists every page. */
    ustore_bytes_view_t filter;

    /** Optional verdict on the listed keys, asked once per page as its @c cost says; host executors only. */
    ustore_predicate_t predicate;

    /** Output for @c count plus one offsets into @c keys, the last one closing the final scan, inside @c arena. */
    ustore_length_t const** offsets;

    /** Output for the found page keys, scan after scan, inside @c arena. */
    ustore_key_t const** keys;

    /** Rewound by this call, then holding every output it writes; @c NULL is refused. */
    ustore_arena_t arena;

    /** Most page keys every scan returns. */
    ustore_size_t wanted;

    /** In what order the page keys come. */
    ustore_scan_mode_t mode;

    /** Optional callback reporting the scans finished. */
    ustore_progress_callback_t progress;

    /** Opaque pointer passed to @c progress. */
    void* progress_state;

} ustore_pages_scan_t;

/**
 *  @brief Scans a batch of page key ranges.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_pages_scan(ustore_pages_scan_t* arguments);

/**
 *  @brief An opaque handle to a walk over the page keys of one collection, which every pull resumes past the last key
 *      the pull before it handed out, as @c ustore_blobs_cursor_t walks.
 *
 *  It holds its database open, and the transaction or snapshot it reads through, until @c ustore_pages_cursor_free, and
 *  serves one pull at a time, so a second pull naming it finishes @c ustore_status_contended_k.
 */
typedef struct ustore_pages_cursor_handle_t* ustore_pages_cursor_t;

/** The arguments of @c ustore_pages_cursor_init. */
typedef struct ustore_pages_cursor_init_t {

    /** Output for the new cursor, which the caller frees with @c ustore_pages_cursor_free; @c NULL on failure. */
    ustore_pages_cursor_t* cursor;

    /** An open database. */
    ustore_database_t database;

    /** Executor of the kind every pull runs on, which picks the side the walk computes on, where @c NULL walks on host
     *  executors; a pull on another kind of executor reports @c ustore_status_device_k. */
    ustore_executor_t executor;

    /** Output for the outcome, final on return. */
    ustore_outcome_t* outcome;

    /** What the walk sees, where @c NULL is the head, read pull by pull. */
    ustore_view_t view;

    /** Pages collection to walk. */
    ustore_collection_id_t collection;

    /** Smallest key the walk returns. */
    ustore_key_t start_key;

    /** Optional path in StringZilla's XPath subset every page the walk hands out holds a node of, copied and compiled
     *  here once; a zeroed view walks every page. */
    ustore_bytes_view_t filter;

    /** Optional verdict on the keys of every page, asked as its @c cost says; host executors only. */
    ustore_predicate_t predicate;

} ustore_pages_cursor_init_t;

/**
 *  @brief Opens a walk over the page keys of one collection, compiling its filter once and reading nothing until its
 *      first pull.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_pages_cursor_init(ustore_pages_cursor_init_t* arguments);

/** The arguments of @c ustore_pages_cursor_next, which pulls the next page of a walk, with the pages when asked, in one
 *  call. */
typedef struct ustore_pages_cursor_next_t {

    /** The database the cursor was opened on. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** The cursor to advance, which a pull that fails or is cancelled leaves where it was. */
    ustore_pages_cursor_t cursor;

    /** Output for the keys of the page in their order, inside @c arena. */
    ustore_key_t const** keys;

    /** Optional output for the whole pages rendered as a tape inside @c arena, where a key removed at the head between
     *  the listing and the read of one pull renders empty; @c NULL skips them. */
    ustore_strings_view_t* values;

    /** Output for the keys written, zero once the walk is exhausted; zeroed on failure. */
    ustore_size_t* written;

    /** Rewound by this call, then holding every output it writes; @c NULL is refused. */
    ustore_arena_t arena;

    /** Most keys the page holds, where a page of fewer exhausts the walk. */
    ustore_size_t wanted;

    /** Encoding the renderings of @c values are converted into. */
    ustore_page_encoding_t outputs_encoding;

    /** Optional callback reporting the one pull, which may cancel it before the cursor advances. */
    ustore_progress_callback_t progress;

    /** Opaque pointer passed to @c progress. */
    void* progress_state;

} ustore_pages_cursor_next_t;

/**
 *  @brief Pulls the next page of a walk, and the pages of its keys when asked.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_pages_cursor_next(ustore_pages_cursor_next_t* arguments);

/**
 *  @brief Waits until every deferred call naming the cursor finishes, then frees it, giving back the holds it kept on
 *      its database and on the transaction or snapshot it reads through.
 *  @param[inout] cursor The cursor to be freed; @c NULL is safe.
 */
USTORE_EXPORT void ustore_pages_cursor_free(ustore_pages_cursor_t cursor);

#ifdef __cplusplus
}
#endif

#endif // UNUM_USTORE_PAGES_H
