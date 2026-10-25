/**
 *  @file src/pages.cpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief Implements `c/ustore/pages.h`: routes every page call to the pages verbs of the engine serving its database,
 *      and spells the page encodings.
 */

#include "ustore/pages.h" // `ustore_page_encoding_t`

#include "engine.hpp" // `engine_t`, `pages_verbs_t`
#include "lib.hpp"    // `route`, `enumerator_spelling`, `name_in`, `value_in`

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
