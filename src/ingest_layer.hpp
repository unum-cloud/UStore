/**
 *  @file src/ingest_layer.hpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief The values an ingest lands for its keyed rows, as an engine keeping every document and page as one value
 *      files them: blobs as given, documents once they parse as JSON, and pages as the trees their XML parses into.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`

#include <span> // `std::span`

#include "ustore/db.h" // `ustore_modality_t`

#include "ustore/docs/json_values.hpp"      // `json_parse`
#include "ustore/pages/element_tree.hpp"    // `placement_t`
#include "ustore/utilities/ram_buffers.hpp" // `arena_rewind`, `arena_copy`, `arena_free_blocks`

#include "pages_layer.hpp"       // `import_page_tree`, `element_key_of`, `clock_stamp`, `page_allocator_t`
#include "utilities/batches.hpp" // `keyed_rows_t`

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

/** The bytes an ingest files for the value of every keyed row, parsing documents and pages, and keeping every page
 *  tree until @c rewind, unescaping pages through @p strings_kit_type_. */
template <strings_kit_concept strings_kit_type_>
class ingest_value_parser {
  public:
    explicit ingest_value_parser(ingest_values_t values) noexcept : values_(values), placement_ {clock_stamp(), 0} {}
    ingest_value_parser(ingest_value_parser const&) = delete;
    ingest_value_parser& operator=(ingest_value_parser const&) = delete;
    ~ingest_value_parser() noexcept {
        arena_free_blocks(parsing_);
        arena_free_blocks(trees_);
    }

    /** Forgets every page tree laid out so far, whose rows the engine has copied or written. */
    void rewind() noexcept { arena_rewind(&trees_); }

    /** The value of row @p row of @p rows as the engine files it, readable until @c rewind. */
    expected<std::span<byte_t const>> operator()(keyed_rows_t const& rows, std::size_t row) noexcept {
        ustore_bytes_view_t const given = rows.values[row];
        std::span<byte_t const> const bytes {reinterpret_cast<byte_t const*>(given.data),
                                             static_cast<std::size_t>(given.length)};
        if (values_ == ingest_values_t::bytes_k) return bytes;
        arena_rewind(&parsing_);
        if (values_ == ingest_values_t::documents_k) {
            status_t const parsed = docs::json_parse(value_view_t {bytes}, &parsing_).status();
            if (parsed.failed()) return parsed;
            return bytes;
        }
        auto [tree, tree_status] = import_page_tree<strings_kit_type_>(pages::page_encoding_t::xml_k, string_of(given),
                                                                       element_key_of(rows.keys[row]), placement_,
                                                                       page_allocator_t {&parsing_});
        if (tree_status.failed()) return tree_status;
        ++placement_.sequence;
        auto [kept, kept_status] = arena_copy<byte_t>(&trees_, tree);
        if (kept_status.failed()) return kept_status;
        return std::span<byte_t const> {kept};
    }

  private:
    ingest_values_t values_ = ingest_values_t::bytes_k;

    /** Where every page of the ingest is placed, its sequence counting the pages parsed so far. */
    pages::placement_t placement_;

    /** Where every value parses, rewound before each. */
    ustore_arena_handle_t parsing_ {};

    /** Where every page tree waits until @c rewind. */
    ustore_arena_handle_t trees_ {};
};

} // namespace unum::ustore
