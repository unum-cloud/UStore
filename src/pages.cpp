/**
 *  @file src/pages.cpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief Implements `c/ustore/pages.h`: the pages modality over the whole-value verbs of the one engine serving it,
 *      routing the page calls to their engine, and the spelling of page encodings.
 *
 *  Every page reads as one value in its collection, its current shape: the dense element tree of
 *  `ustore/pages/element_tree.hpp` under the page key, so rendering a page is one read and one decode.
 *
 *  An edit call reads the trees of the pages it touches and applies its tasks in order by the forgiving rules of
 *  `page_fold.hpp`. An engine keeping every page as one value, served by the `*_over_blobs` verbs, reads the pages
 *  watched in a transaction, writes every tree back in one write, then removes the pages the call removed. An engine
 *  keeping a page as its edits takes the chains of the `*_edits` calls, which read the pages unwatched, since the fold
 *  lets concurrent edits of one page land, and commits the edits of every page as one chain. An edit naming a missing
 *  page answers @c status_code_t::not_found_k, as a modification of a missing document does, while an edit of a missing
 *  element inside a page does nothing, as the fold does for racing edits. A Nil element removes the whole page.
 *
 *  Every call places its edits at one stamp, the wall clock in nanoseconds or one past the newest placement in the
 *  pages it touches, whichever is newer, which lands an insertion right after its anchor and prefixes the keys minted
 *  for new elements.
 *
 *  Every scratch structure lives in the scratch of the call, so the arena of the request holds only its outputs. XML
 *  and plain text import and render, while HTML, Markdown and the JSON tree answer @c status_code_t::unsupported_k.
 */

#include "ustore/pages.h" // `ustore_page_encoding_t`

#include "blobs_layers/pages.hpp" // `pages_import_over_blobs`, `pages_import_edits`, `pages_read_over_blobs`
#include "engine.hpp"             // `engine_t`, `pages_verbs_t`
#include "lib.hpp"                // `route`, `enumerator_spelling`, `name_in`, `value_in`

namespace unum::ustore {

#pragma region Names

/** The canonical spelling of every page encoding. */
constexpr enumerator_spelling<ustore_page_encoding_t> page_encoding_names_k[] {
    {ustore_page_encoding_html_k, "html"},         {ustore_page_encoding_xml_k, "xml"},
    {ustore_page_encoding_markdown_k, "markdown"}, {ustore_page_encoding_text_k, "text"},
    {ustore_page_encoding_json_k, "json"},
};

#pragma endregion Names

} // namespace unum::ustore

void ustore_pages_import(ustore_pages_import_t* arguments) {
    route<&engine_t::pages, &pages_verbs_t::import>(arguments);
}
void ustore_pages_insert(ustore_pages_insert_t* arguments) {
    route<&engine_t::pages, &pages_verbs_t::insert>(arguments);
}
void ustore_pages_remove(ustore_pages_remove_t* arguments) {
    route<&engine_t::pages, &pages_verbs_t::remove>(arguments);
}
void ustore_pages_move(ustore_pages_move_t* arguments) { route<&engine_t::pages, &pages_verbs_t::move>(arguments); }
void ustore_pages_splice(ustore_pages_splice_t* arguments) {
    route<&engine_t::pages, &pages_verbs_t::splice>(arguments);
}
void ustore_pages_set_attributes(ustore_pages_set_attributes_t* arguments) {
    route<&engine_t::pages, &pages_verbs_t::set_attributes>(arguments);
}
void ustore_pages_remove_attributes(ustore_pages_remove_attributes_t* arguments) {
    route<&engine_t::pages, &pages_verbs_t::remove_attributes>(arguments);
}
void ustore_pages_read(ustore_pages_read_t* arguments) { route<&engine_t::pages, &pages_verbs_t::read>(arguments); }
void ustore_pages_scan(ustore_pages_scan_t* arguments) { route<&engine_t::pages, &pages_verbs_t::scan>(arguments); }

char const* ustore_page_encoding_name(ustore_page_encoding_t encoding) {
    return name_in(page_encoding_names_k, encoding);
}

ustore_page_encoding_t ustore_page_encoding_from_name(ustore_bytes_view_t name) {
    return value_in(page_encoding_names_k, name);
}
