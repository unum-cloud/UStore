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
 *  Edits are positional and small: insert elements after a sibling, remove or move a subtree, splice
 *  bytes inside a text run, or set an attribute. An element inserted without a key is minted a
 *  time-ordered one, and concurrent insertions after the same sibling order newest first, so authors
 *  editing one position converge on one order without coordination, as replicated growable arrays do.
 *  Element keys are stable across every edit, so a formula, a link or a comment refers to an element
 *  and survives the rows moving around it.
 *
 *  Reads render a page or a subtree at the transaction's or the snapshot's moment in the encoding the
 *  caller asks for, and a history lists the edits between two commit stamps. Cursors, selections and
 *  presence are never stored; they are the application's ephemeral state.
 */

#pragma once

#include <ustore/db.h>

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

/** Modification @c ustore_pages_write applies to every task. */
typedef enum ustore_page_modification_t {

    /** Replaces the whole page with the parsed @c values, minting every element. */
    ustore_page_modify_replace_k = 0,

    /** Parses @c values into elements under @c parents after @c anchors, or at the front on a Nil anchor. */
    ustore_page_modify_insert_k = 1,

    /** Removes @c elements with their subtrees. */
    ustore_page_modify_remove_k = 2,

    /** Moves @c elements under @c parents after @c anchors, keeping their keys. */
    ustore_page_modify_move_k = 3,

    /** Replaces @c lengths bytes of the text of @c elements from @c offsets with @c values. */
    ustore_page_modify_splice_k = 4,

    /** Sets the attribute @c fields of @c elements to @c values, or removes it on an absent value. */
    ustore_page_modify_attribute_k = 5,

} ustore_page_modification_t;

#pragma endregion Types

#pragma region Edits

/**
 *  @brief The arguments of @c ustore_pages_write, which imports pages or edits elements inside them.
 *
 *  Every task names its page by key and its element, parent or anchor by element key, where the page
 *  key doubles as the key of the root element. Tasks of one call apply in order inside one commit, so
 *  a later task may anchor on an element an earlier one minted.
 */
typedef struct ustore_pages_write_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Transaction the edits belong to, or @c NULL. */
    ustore_transaction_t transaction;

    /** Reusable memory for intermediate results. */
    ustore_arena_t* arena;

    /** Write options, or read and write options for modifications that read the page first. */
    ustore_options_t options;

    /** Number of edits. */
    ustore_size_t tasks_count;

    /** Encoding of the imported or inserted @c values. */
    ustore_page_encoding_t encoding;

    /** Modification applied to every task. */
    ustore_page_modification_t modification;

    /** Collections of the pages, where @c NULL stands for the main collection. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Keys of the pages. */
    ustore_key_t const* keys;

    /** Bytes between consecutive keys, where zero repeats the first one. */
    ustore_size_t keys_stride;

    /** Elements edited, or the keys to give inserted elements, where Nil or @c NULL mints one. */
    ustore_key_t const* elements;

    /** Bytes between consecutive elements, where zero repeats the first one. */
    ustore_size_t elements_stride;

    /** Parents of inserted or moved elements, where Nil stands for the root. */
    ustore_key_t const* parents;

    /** Bytes between consecutive parents, where zero repeats the first one. */
    ustore_size_t parents_stride;

    /** Siblings after which inserted or moved elements land, where Nil stands for the front. */
    ustore_key_t const* anchors;

    /** Bytes between consecutive anchors, where zero repeats the first one. */
    ustore_size_t anchors_stride;

    /** Names of the attributes set, for @c ustore_page_modify_attribute_k. */
    ustore_str_view_t const* fields;

    /** Bytes between consecutive names, where zero repeats the first one. */
    ustore_size_t fields_stride;

    /** Bitmap of present values, or @c NULL when every value is present. */
    ustore_octet_t const* presences;

    /** Offsets of the values inside their buffers, or the text offsets a splice starts at. */
    ustore_length_t const* offsets;

    /** Bytes between consecutive offsets, where zero repeats the first one. */
    ustore_size_t offsets_stride;

    /** Lengths of the values, or the text lengths a splice replaces. */
    ustore_length_t const* lengths;

    /** Bytes between consecutive lengths, where zero repeats the first one. */
    ustore_size_t lengths_stride;

    /** Buffers holding the imported markup, the spliced bytes or the attribute values. */
    ustore_bytes_cptr_t const* values;

    /** Bytes between consecutive buffer pointers, where zero repeats the first one. */
    ustore_size_t values_stride;

    /** Optional output for the key every task's element ended up with, minted or given. */
    ustore_key_t** effective_keys;

} ustore_pages_write_t;

/**
 *  @brief Imports pages or edits elements inside them.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_pages_write(ustore_pages_write_t* arguments);

#pragma endregion Edits

#pragma region Reads

/**
 *  @brief The arguments of @c ustore_pages_read, which renders pages or subtrees in one encoding.
 *
 *  A task names a page and optionally an element, where Nil renders the whole page and any other
 *  key renders that element with its subtree. The rendering is the page as of the transaction's or
 *  the snapshot's moment, with every committed edit folded in.
 */
typedef struct ustore_pages_read_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Transaction that watches the read pages, or @c NULL. */
    ustore_transaction_t transaction;

    /** Optional snapshot whose view is rendered, never set beside a transaction. */
    ustore_snapshot_t snapshot;

    /** Reusable memory for the results. */
    ustore_arena_t* arena;

    /** Read options. */
    ustore_options_t options;

    /** Encoding to render in. */
    ustore_page_encoding_t encoding;

    /** Number of pages or subtrees to render. */
    ustore_size_t tasks_count;

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

    /** Optional output for a bitmap of found pages or elements. */
    ustore_octet_t** presences;

    /** Optional output for the offsets of the renderings inside @c values. */
    ustore_length_t** offsets;

    /** Optional output for the lengths of the renderings. */
    ustore_length_t** lengths;

    /** Optional output for the joined renderings. */
    ustore_bytes_ptr_t* values;

} ustore_pages_read_t;

/**
 *  @brief Renders pages or subtrees in one encoding.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_pages_read(ustore_pages_read_t* arguments);

/**
 *  @brief The arguments of @c ustore_pages_history, which lists the edits of pages between two commits.
 *
 *  Edits come oldest first, one row per edit across the outputs, with the bytes of a splice, an
 *  insertion or an attribute in @c values. Replaying the rows of one page through @c ustore_pages_write
 *  onto its rendering at @c since reproduces its rendering at @c until.
 */
typedef struct ustore_pages_history_t {

    /** An open database. */
    ustore_database_t db;

    /** Output for the error message. */
    ustore_error_t* error;

    /** Transaction that watches the read pages, or @c NULL. */
    ustore_transaction_t transaction;

    /** Optional snapshot whose view bounds the history, never set beside a transaction. */
    ustore_snapshot_t snapshot;

    /** Reusable memory for the results. */
    ustore_arena_t* arena;

    /** Read options. */
    ustore_options_t options;

    /** Encoding of the inserted fragments in @c values. */
    ustore_page_encoding_t encoding;

    /** Number of pages. */
    ustore_size_t tasks_count;

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

    /** Output for the number of edits of every page. */
    ustore_length_t** counts;

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
    ustore_str_view_t** fields;

    /** Optional output for the offsets of every edit's bytes inside @c values, or the text offset of a splice. */
    ustore_length_t** offsets;

    /** Optional output for the lengths of every edit's bytes, or the text length a splice replaced. */
    ustore_length_t** lengths;

    /** Optional output for the joined bytes of every edit. */
    ustore_bytes_ptr_t* values;

} ustore_pages_history_t;

/**
 *  @brief Lists the edits of pages between two commits, oldest first.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
void ustore_pages_history(ustore_pages_history_t* arguments);

#pragma endregion Reads

#ifdef __cplusplus
}
#endif
