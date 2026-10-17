/**
 *  @file src/pages.cpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief Implements `c/ustore/pages.h`: routing the page calls to their engine, and the spelling of page encodings
 *      and modifications.
 */

#include "ustore/pages.h" // `ustore_pages_import_t`, `ustore_page_encoding_t`

#include "lib.hpp" // `route`, `enumerator_spelling`, `name_in`, `value_in`

namespace unum::ustore {

/** The canonical spelling of every page encoding. */
constexpr enumerator_spelling<ustore_page_encoding_t> page_encoding_names_k[] {
    {ustore_page_encoding_html_k, "html"},         {ustore_page_encoding_xml_k, "xml"},
    {ustore_page_encoding_markdown_k, "markdown"}, {ustore_page_encoding_text_k, "text"},
    {ustore_page_encoding_json_k, "json"},
};

/** The canonical spelling of every page modification. */
constexpr enumerator_spelling<ustore_page_modification_t> page_modification_names_k[] {
    {ustore_page_modify_import_k, "import"},
    {ustore_page_modify_insert_k, "insert"},
    {ustore_page_modify_remove_k, "remove"},
    {ustore_page_modify_move_k, "move"},
    {ustore_page_modify_splice_k, "splice"},
    {ustore_page_modify_set_attributes_k, "set-attributes"},
    {ustore_page_modify_remove_attributes_k, "remove-attributes"},
};

} // namespace unum::ustore

void ustore_pages_import(ustore_pages_import_t* arguments) { route<&engine_t::pages_import>(arguments); }
void ustore_pages_insert(ustore_pages_insert_t* arguments) { route<&engine_t::pages_insert>(arguments); }
void ustore_pages_remove(ustore_pages_remove_t* arguments) { route<&engine_t::pages_remove>(arguments); }
void ustore_pages_move(ustore_pages_move_t* arguments) { route<&engine_t::pages_move>(arguments); }
void ustore_pages_splice(ustore_pages_splice_t* arguments) { route<&engine_t::pages_splice>(arguments); }
void ustore_pages_set_attributes(ustore_pages_set_attributes_t* arguments) {
    route<&engine_t::pages_set_attributes>(arguments);
}
void ustore_pages_remove_attributes(ustore_pages_remove_attributes_t* arguments) {
    route<&engine_t::pages_remove_attributes>(arguments);
}
void ustore_pages_read(ustore_pages_read_t* arguments) { route<&engine_t::pages_read>(arguments); }
void ustore_pages_history(ustore_pages_history_t* arguments) { route<&engine_t::pages_history>(arguments); }

char const* ustore_page_encoding_name(ustore_page_encoding_t encoding) {
    return name_in(page_encoding_names_k, encoding);
}

ustore_page_encoding_t ustore_page_encoding_from_name(ustore_bytes_view_t name) {
    return value_in(page_encoding_names_k, name);
}

char const* ustore_page_modification_name(ustore_page_modification_t modification) {
    return name_in(page_modification_names_k, modification);
}

ustore_page_modification_t ustore_page_modification_from_name(ustore_bytes_view_t name) {
    return value_in(page_modification_names_k, name);
}
