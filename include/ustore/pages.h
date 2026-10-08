/**
 *  @file include/ustore/pages.h
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
 *  Reads render a page or a subtree as their view sees it, in the encoding the caller asks for, and a
 *  history lists the edits between two commit stamps. Cursors, selections and presence are never
 *  stored; they are the application's ephemeral state.
 */

#pragma once

#include "ustore/db.h"

#ifdef __cplusplus
extern "C" {
#endif

#pragma region Types

/** Encoding a page or a fragment is imported from or rendered into. */
typedef enum ustore_page_encoding_t {

    /** HTML, parsed into elements and rendered back as markup. */
    ustore_page_html_k = 0,

    /** XML, with every element and attribute kept as written. */
    ustore_page_xml_k = 1,

    /** Markdown, parsed into headings, paragraphs, lists, tables and code, and rendered back. */
    ustore_page_markdown_k = 2,

    /** The text of every run in document order, with structure dropped. */
    ustore_page_text_k = 3,

    /** The element tree as JSON: key, tag, attributes, text and children, for programmatic access. */
    ustore_page_json_k = 4,

} ustore_page_encoding_t;

/** The kind of an edit @c ustore_pages_history lists, named after the verb that applies it. */
typedef enum ustore_page_modification_t {

    /** Replaces a whole page with parsed markup, minting every element, as @c ustore_pages_import does. */
    ustore_page_modify_import_k = 0,

    /** Parses a fragment into elements under a parent after an anchor, as @c ustore_pages_insert does. */
    ustore_page_modify_insert_k = 1,

    /** Removes an element with its subtree, as @c ustore_pages_remove does. */
    ustore_page_modify_remove_k = 2,

    /** Moves an element under a parent after an anchor, keeping its key, as @c ustore_pages_move does. */
    ustore_page_modify_move_k = 3,

    /** Replaces a run of an element's text with new bytes, as @c ustore_pages_splice does. */
    ustore_page_modify_splice_k = 4,

    /** Sets an attribute of an element, as @c ustore_pages_set_attributes does. */
    ustore_page_modify_set_attributes_k = 5,

    /** Removes an attribute of an element, as @c ustore_pages_remove_attributes does. */
    ustore_page_modify_remove_attributes_k = 6,

} ustore_page_modification_t;

#pragma endregion Types

#pragma region Edits

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

    /** Collections of the pages, where @c NULL stands for the main collection. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Keys of the pages. */
    ustore_key_t const* keys;

    /** Bytes between consecutive keys, where zero repeats the first one. */
    ustore_size_t keys_stride;

    /** Optional starts of the markup within the chunks of @c values, where @c NULL starts every page at its chunk. */
    ustore_length_t const* starts;

    /** Bytes between consecutive starts, where zero repeats the first one. */
    ustore_size_t starts_stride;

    /** Optional lengths of the markup; without them every page ends where the next one starts, reading one start
     *  past the last page, or at a null terminator when @c starts is absent too. */
    ustore_length_t const* lengths;

    /** Bytes between consecutive lengths, where zero repeats the first one. */
    ustore_size_t lengths_stride;

    /** Pointers to the chunks holding the markup, @c NULL only when there are no tasks. */
    ustore_bytes_cptr_t const* values;

    /** Bytes between consecutive chunk pointers, where zero repeats the first one. */
    ustore_size_t values_stride;

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

    /** Collections of the pages, where @c NULL stands for the main collection. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Keys of the pages. */
    ustore_key_t const* keys;

    /** Bytes between consecutive keys, where zero repeats the first one. */
    ustore_size_t keys_stride;

    /** Keys to give the inserted elements, where Nil or @c NULL mints one. */
    ustore_key_t const* elements;

    /** Bytes between consecutive elements, where zero repeats the first one. */
    ustore_size_t elements_stride;

    /** Parents the elements land under, where Nil stands for the root. */
    ustore_key_t const* parents;

    /** Bytes between consecutive parents, where zero repeats the first one. */
    ustore_size_t parents_stride;

    /** Siblings after which the elements land, where Nil stands for the front. */
    ustore_key_t const* anchors;

    /** Bytes between consecutive anchors, where zero repeats the first one. */
    ustore_size_t anchors_stride;

    /** Optional starts of the fragments within the chunks of @c values, where @c NULL starts every fragment at its
     *  chunk. */
    ustore_length_t const* starts;

    /** Bytes between consecutive starts, where zero repeats the first one. */
    ustore_size_t starts_stride;

    /** Optional lengths of the fragments; without them every fragment ends where the next one starts, reading one
     *  start past the last fragment, or at a null terminator when @c starts is absent too. */
    ustore_length_t const* lengths;

    /** Bytes between consecutive lengths, where zero repeats the first one. */
    ustore_size_t lengths_stride;

    /** Pointers to the chunks holding the fragments, @c NULL only when there are no tasks. */
    ustore_bytes_cptr_t const* values;

    /** Bytes between consecutive chunk pointers, where zero repeats the first one. */
    ustore_size_t values_stride;

    /** Optional output for the key every inserted element ended up with, minted or given. */
    ustore_key_t* effective_keys;

    /** Bytes between consecutive output keys, never zero. */
    ustore_size_t effective_keys_stride;

    /** Encoding of the fragments. */
    ustore_page_encoding_t encoding;

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

    /** Collections of the pages, where @c NULL stands for the main collection. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Keys of the pages. */
    ustore_key_t const* keys;

    /** Bytes between consecutive keys, where zero repeats the first one. */
    ustore_size_t keys_stride;

    /** Elements to remove with their subtrees. */
    ustore_key_t const* elements;

    /** Bytes between consecutive elements, where zero repeats the first one. */
    ustore_size_t elements_stride;

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

    /** Collections of the pages, where @c NULL stands for the main collection. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Keys of the pages. */
    ustore_key_t const* keys;

    /** Bytes between consecutive keys, where zero repeats the first one. */
    ustore_size_t keys_stride;

    /** Elements to move with their subtrees. */
    ustore_key_t const* elements;

    /** Bytes between consecutive elements, where zero repeats the first one. */
    ustore_size_t elements_stride;

    /** Parents the elements land under, where Nil stands for the root. */
    ustore_key_t const* parents;

    /** Bytes between consecutive parents, where zero repeats the first one. */
    ustore_size_t parents_stride;

    /** Siblings after which the elements land, where Nil stands for the front. */
    ustore_key_t const* anchors;

    /** Bytes between consecutive anchors, where zero repeats the first one. */
    ustore_size_t anchors_stride;

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

    /** Collections of the pages, where @c NULL stands for the main collection. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Keys of the pages. */
    ustore_key_t const* keys;

    /** Bytes between consecutive keys, where zero repeats the first one. */
    ustore_size_t keys_stride;

    /** Elements whose text is spliced. */
    ustore_key_t const* elements;

    /** Bytes between consecutive elements, where zero repeats the first one. */
    ustore_size_t elements_stride;

    /** Bytes into the text of every element where its splice starts. */
    ustore_length_t const* text_starts;

    /** Bytes between consecutive text starts, where zero repeats the first one. */
    ustore_size_t text_starts_stride;

    /** Bytes of text every splice replaces, where zero only inserts. */
    ustore_length_t const* text_lengths;

    /** Bytes between consecutive text lengths, where zero repeats the first one. */
    ustore_size_t text_lengths_stride;

    /** Optional starts of the new bytes within the chunks of @c values, where @c NULL starts them at their chunk. */
    ustore_length_t const* starts;

    /** Bytes between consecutive starts, where zero repeats the first one. */
    ustore_size_t starts_stride;

    /** Optional lengths of the new bytes, where zero only removes; without them every run ends where the next one
     *  starts, reading one start past the last run, or at a null terminator when @c starts is absent too. */
    ustore_length_t const* lengths;

    /** Bytes between consecutive lengths, where zero repeats the first one. */
    ustore_size_t lengths_stride;

    /** Pointers to the chunks holding the new bytes, @c NULL only when there are no tasks. */
    ustore_bytes_cptr_t const* values;

    /** Bytes between consecutive chunk pointers, where zero repeats the first one. */
    ustore_size_t values_stride;

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

    /** Collections of the pages, where @c NULL stands for the main collection. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Keys of the pages. */
    ustore_key_t const* keys;

    /** Bytes between consecutive keys, where zero repeats the first one. */
    ustore_size_t keys_stride;

    /** Elements whose attributes are set. */
    ustore_key_t const* elements;

    /** Bytes between consecutive elements, where zero repeats the first one. */
    ustore_size_t elements_stride;

    /** Names of the attributes to set. */
    ustore_str_view_t const* fields;

    /** Bytes between consecutive names, where zero repeats the first one. */
    ustore_size_t fields_stride;

    /** Optional starts of the attribute values within the chunks of @c values, where @c NULL starts every value at
     *  its chunk. */
    ustore_length_t const* starts;

    /** Bytes between consecutive starts, where zero repeats the first one. */
    ustore_size_t starts_stride;

    /** Optional lengths of the attribute values; without them every value ends where the next one starts, reading
     *  one start past the last value, or at a null terminator when @c starts is absent too. */
    ustore_length_t const* lengths;

    /** Bytes between consecutive lengths, where zero repeats the first one. */
    ustore_size_t lengths_stride;

    /** Pointers to the chunks holding the attribute values, @c NULL only when there are no tasks. */
    ustore_bytes_cptr_t const* values;

    /** Bytes between consecutive chunk pointers, where zero repeats the first one. */
    ustore_size_t values_stride;

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

    /** Collections of the pages, where @c NULL stands for the main collection. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Keys of the pages. */
    ustore_key_t const* keys;

    /** Bytes between consecutive keys, where zero repeats the first one. */
    ustore_size_t keys_stride;

    /** Elements whose attributes are removed. */
    ustore_key_t const* elements;

    /** Bytes between consecutive elements, where zero repeats the first one. */
    ustore_size_t elements_stride;

    /** Names of the attributes to remove. */
    ustore_str_view_t const* fields;

    /** Bytes between consecutive names, where zero repeats the first one. */
    ustore_size_t fields_stride;

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

#pragma endregion Edits

#pragma region Reads

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

    /** Collections of the pages, where @c NULL stands for the main collection. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Keys of the pages. */
    ustore_key_t const* keys;

    /** Bytes between consecutive keys, where zero repeats the first one. */
    ustore_size_t keys_stride;

    /** Elements to render with their subtrees, or @c NULL for whole pages. */
    ustore_key_t const* elements;

    /** Bytes between consecutive elements, where zero repeats the first one. */
    ustore_size_t elements_stride;

    /** Optional output for a bitset of at least @c count bits marking found pages or elements. */
    ustore_octet_t* presences;

    /** Optional output for @c count plus one offsets into @c values, the last one closing the final rendering. */
    ustore_length_t* offsets;

    /** Optional output for the tape of renderings, inside @c arena. */
    ustore_byte_t** values;

    /** Memory for @c values: reused when it points to an arena, created when it points to @c NULL. */
    ustore_arena_t* arena;

    /** Encoding to render in. */
    ustore_page_encoding_t encoding;

} ustore_pages_read_t;

/**
 *  @brief Renders pages or subtrees in one encoding.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_pages_read(ustore_pages_read_t* arguments);

/**
 *  @brief The arguments of @c ustore_pages_history, which lists the edits of pages between two commits.
 *
 *  Edits come oldest first, one row per edit across the outputs, with the bytes of an import, an
 *  insertion, a splice or an attribute in @c values. Only the call finds the number of edits, so every
 *  output but @c edits_offsets lands in @c arena. Replaying the rows of one page, each through the verb
 *  its modification names, onto its rendering at @c since reproduces its rendering at @c until.
 */
typedef struct ustore_pages_history_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** What bounds the history, where @c NULL is the head. */
    ustore_view_t view;

    /** Number of pages. */
    ustore_size_t count;

    /** Collections of the pages, where @c NULL stands for the main collection. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Keys of the pages. */
    ustore_key_t const* keys;

    /** Bytes between consecutive keys, where zero repeats the first one. */
    ustore_size_t keys_stride;

    /** First commit stamp included, where zero starts at the page's creation. */
    ustore_stamp_t const* since;

    /** Bytes between consecutive stamps, where zero repeats the first one. */
    ustore_size_t since_stride;

    /** Last commit stamp included, where zero ends at the moment read. */
    ustore_stamp_t const* until;

    /** Bytes between consecutive stamps, where zero repeats the first one. */
    ustore_size_t until_stride;

    /** Output for @c count plus one offsets into the rows of edits, the last one closing the final page. */
    ustore_length_t* edits_offsets;

    /** Optional output for the commit stamp of every edit. */
    ustore_stamp_t** stamps;

    /** Optional output for the modification of every edit. */
    ustore_page_modification_t** modifications;

    /** Optional output for the element of every edit. */
    ustore_key_t** elements;

    /** Optional output for the parent of every insertion or move. */
    ustore_key_t** parents;

    /** Optional output for the anchor of every insertion or move. */
    ustore_key_t** anchors;

    /** Optional output for the attribute name of every attribute edit, joined and null-terminated. */
    ustore_char_t** fields;

    /** Optional output for the byte of the text where every splice starts. */
    ustore_length_t** text_starts;

    /** Optional output for the bytes of text every splice replaced. */
    ustore_length_t** text_lengths;

    /** Optional output for the start of every edit's bytes inside @c values. */
    ustore_length_t** starts;

    /** Optional output for the length of every edit's bytes. */
    ustore_length_t** lengths;

    /** Optional output for the joined bytes of every edit. */
    ustore_byte_t** values;

    /** Memory for every output but @c edits_offsets: reused when it points to an arena, created when it points to
     *  @c NULL. */
    ustore_arena_t* arena;

    /** Encoding of the imported and inserted fragments in @c values. */
    ustore_page_encoding_t encoding;

} ustore_pages_history_t;

/**
 *  @brief Lists the edits of pages between two commits, oldest first.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_pages_history(ustore_pages_history_t* arguments);

#pragma endregion Reads

#ifdef __cplusplus
}
#endif
