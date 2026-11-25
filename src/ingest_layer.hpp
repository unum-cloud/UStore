/**
 *  @file src/ingest_layer.hpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief The values an ingest lands for its keyed rows, as an engine keeping every document and page as one value
 *      files them: blobs as given, documents once they parse as JSON, and pages as the trees their
 *      XML parses into, parsed over the threads of the call's executor.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`

#include <span> // `std::span`

#include "ustore/db.h" // `ustore_modality_t`

#include "ustore/docs/json_values.hpp"      // `json_parse`
#include "ustore/pages/element_tree.hpp"    // `placement_t`
#include "ustore/utilities/ram_buffers.hpp" // `arena_allocate`

#include "pages_layer.hpp"       // `import_page_tree`, `element_key_of`, `page_allocator_t`
#include "utilities/batches.hpp" // `keyed_rows_t`
#include "utilities/slices.hpp"  // `for_task_slices`

namespace unum::ustore {

/** What an ingest lands as the value of a keyed row. */
enum class ingest_values_t : std::uint8_t {

    /** The row's bytes as given, for blobs. */
    bytes_k,

    /** The row's bytes once they parse as JSON, for documents. */
    documents_k,

    /** The dense element tree the row's XML parses into, for pages. */
    pages_k,
};

/** The values a collection of @p modality lands, its bytes for every modality but documents and pages. */
inline ingest_values_t ingest_values_of(ustore_modality_t modality) noexcept {
    if (modality == ustore_modality_docs_k) return ingest_values_t::documents_k;
    if (modality == ustore_modality_pages_k) return ingest_values_t::pages_k;
    return ingest_values_t::bytes_k;
}

/**
 *  @brief The value of every row of @p rows as the engine files it, carved in the scratch of
 *      @p view and parsed over the slices of its executor, readable until the call ends.
 *
 *  Blobs land as given, documents once they parse as JSON, and pages as the trees their XML parses
 *  into, unescaped through @p strings_kit_type_, the page of row @c r placed at @p placement with
 *  its sequence advanced by @c r.
 */
template <a_strings_kit strings_kit_type_>
expected<std::span<std::span<byte_t const>>> parse_ingest_values(ingest_values_t values, util::keyed_rows_t const& rows,
                                                                 pages::placement_t placement,
                                                                 view_t const& view) noexcept {
    auto [parsed, parsed_status] = util::arena_allocate<std::span<byte_t const>>(view.scratch, rows.size());
    if (parsed_status.failed()) return parsed_status;
    for (std::size_t row = 0; row != rows.size(); ++row)
        parsed[row] = bytes_of(rows.values[row].data, static_cast<std::size_t>(rows.values[row].length));
    if (values == ingest_values_t::bytes_k) return parsed;
    status_t const status = util::for_task_slices(
        view, rows.size(),
        [&](std::size_t first, std::size_t length, std::size_t, view_t const& slice) noexcept -> status_t {
            for (std::size_t row = first; row != first + length; ++row) {
                if (values == ingest_values_t::documents_k) {
                    auto [document, document_status] = docs::json_parse(value_view_t {parsed[row]}, slice.scratch);
                    if (document_status.failed()) return document_status;
                    continue;
                }
                pages::placement_t const placed {placement.stamp, static_cast<std::uint32_t>(placement.sequence + row)};
                auto [tree, tree_status] = import_page_tree<strings_kit_type_>(
                    pages::page_encoding_t::xml_k, chars_of(rows.values[row]), element_key_of(rows.keys[row]), placed,
                    page_allocator_t {slice.scratch});
                if (tree_status.failed()) return tree_status;
                parsed[row] = tree;
            }
            return {};
        });
    if (status.failed()) return status;
    return parsed;
}

} // namespace unum::ustore
