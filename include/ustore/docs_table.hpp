/**
 *  @file include/ustore/docs_table.hpp
 *  @author Ash Vardanian
 *  @date July 4, 2022
 *  @brief Columnar views over the results of @c ustore_docs_gather.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <algorithm>   // `std::copy_n`
#include <array>       // `std::array`
#include <concepts>    // `std::same_as`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <tuple>       // `std::tuple`
#include <type_traits> // `std::conditional_t`
#include <variant>     // `std::monostate`

#include "ustore/docs.h"
#include "ustore/ranges.hpp" // `strided_iterator`, `strided_range`, `bits_view_t`
#include "ustore/types.hpp"  // `value_view_t`, `ustore_doc_field`

namespace unum::ustore {

#pragma region Cells and Columns

/** Element types stored as offsets and lengths into a joined tape rather than as fixed-width scalars. */
template <typename element_>
concept variable_length_element_concept = std::same_as<element_, value_view_t> ||
                                          std::same_as<element_, std::string_view>;

/** One entry of a column, with the flags @c ustore_docs_gather reports for it. */
template <typename element_>
struct cell {

    /** Whether the value was found and converted. */
    bool valid {false};

    /** Whether the conversion changed the semantics of the value. */
    bool converted {false};

    /** Whether an inconvertible value sits under the field. */
    bool collides {false};

    /** The value, meaningful only when @c valid. */
    element_ value {};
};

/** Column of fixed-width scalars. */
template <typename element_>
class column_view_scalar {
    ustore_octet_t* validities_ {nullptr};
    ustore_octet_t* conversions_ {nullptr};
    ustore_octet_t* collisions_ {nullptr};
    element_* scalars_ {nullptr};
    ustore_size_t count_ {0};
    ustore_str_view_t name_ {nullptr};

  public:
    using cell_t = cell<element_>;
    using value_type = cell_t;

    column_view_scalar( //
        ustore_octet_t* validities, ustore_octet_t* conversions, ustore_octet_t* collisions, element_* scalars,
        ustore_size_t count, ustore_str_view_t name = nullptr) noexcept
        : validities_(validities), conversions_(conversions), collisions_(collisions), scalars_(scalars), count_(count),
          name_(name) {}

    ustore_str_view_t name() const noexcept { return name_; }
    std::size_t size() const noexcept { return count_; }

    cell_t operator[](std::size_t index) const noexcept {
        return {bits_view_t {validities_}[index], bits_view_t {conversions_}[index], bits_view_t {collisions_}[index],
                scalars_[index]};
    }
};

/** Column of strings, stored as offsets and lengths into a joined tape. */
template <typename element_>
class column_view_varlen {
    ustore_octet_t* validities_ {nullptr};
    ustore_octet_t* conversions_ {nullptr};
    ustore_octet_t* collisions_ {nullptr};
    ustore_byte_t* tape_ {nullptr};
    ustore_length_t* offsets_ {nullptr};
    ustore_length_t* lengths_ {nullptr};
    ustore_size_t count_ {0};
    ustore_str_view_t name_ {nullptr};

  public:
    using cell_t = cell<element_>;
    using value_type = cell_t;

    column_view_varlen( //
        ustore_octet_t* validities, ustore_octet_t* conversions, ustore_octet_t* collisions, ustore_byte_t* tape,
        ustore_length_t* offsets, ustore_length_t* lengths, ustore_size_t count,
        ustore_str_view_t name = nullptr) noexcept
        : validities_(validities), conversions_(conversions), collisions_(collisions), tape_(tape), offsets_(offsets),
          lengths_(lengths), count_(count), name_(name) {}

    ustore_str_view_t name() const noexcept { return name_; }
    std::size_t size() const noexcept { return count_; }

    cell_t operator[](std::size_t index) const noexcept {
        using character_t = typename element_::value_type;
        auto const begin = reinterpret_cast<character_t const*>(tape_ + offsets_[index]);
        return {bits_view_t {validities_}[index], bits_view_t {conversions_}[index], bits_view_t {collisions_}[index],
                element_ {begin, lengths_[index]}};
    }
};

/** Column view matching how @p element_ is stored. */
template <typename element_>
using column_view = std::conditional_t<variable_length_element_concept<element_>, //
                                       column_view_varlen<element_>, column_view_scalar<element_>>;

/** Column whose type is known only at runtime, viewed as a typed column through @c as. */
class column_view_t {
    ustore_octet_t* validities_ {nullptr};
    ustore_octet_t* conversions_ {nullptr};
    ustore_octet_t* collisions_ {nullptr};
    ustore_byte_t* scalars_ {nullptr};
    ustore_byte_t* tape_ {nullptr};
    ustore_length_t* offsets_ {nullptr};
    ustore_length_t* lengths_ {nullptr};
    ustore_size_t count_ {0};
    ustore_str_view_t name_ {nullptr};
    ustore_doc_field_type_t type_ {ustore_doc_field_json_k};

  public:
    column_view_t( //
        ustore_octet_t* validities, ustore_octet_t* conversions, ustore_octet_t* collisions, ustore_byte_t* scalars,
        ustore_byte_t* tape, ustore_length_t* offsets, ustore_length_t* lengths, ustore_size_t count,
        ustore_str_view_t name = nullptr, ustore_doc_field_type_t type = ustore_doc_field_json_k) noexcept
        : validities_(validities), conversions_(conversions), collisions_(collisions), scalars_(scalars), tape_(tape),
          offsets_(offsets), lengths_(lengths), count_(count), name_(name), type_(type) {}

    ustore_str_view_t name() const noexcept { return name_; }
    ustore_doc_field_type_t type() const noexcept { return type_; }
    std::size_t size() const noexcept { return count_; }

    template <typename element_>
    column_view<element_> as() const noexcept {
        if constexpr (variable_length_element_concept<element_>)
            return {validities_, conversions_, collisions_, tape_, offsets_, lengths_, count_, name_};
        else return {validities_, conversions_, collisions_, reinterpret_cast<element_*>(scalars_), count_, name_};
    }

    ustore_octet_t* validities() const noexcept { return validities_; }
    ustore_length_t* offsets() const noexcept { return offsets_; }
    ustore_length_t* lengths() const noexcept { return lengths_; }
    ustore_byte_t* contents() const noexcept { return scalars_ ? scalars_ : tape_; }
};

#pragma endregion Cells and Columns

#pragma region Headers

/** A column of a table header: the field to gather and the type to convert it into. */
struct field_type_t {

    /** Field inside every document. */
    ustore_str_view_t field {nullptr};

    /** Type the field converts into. */
    ustore_doc_field_type_t type {ustore_doc_field_json_k};
};

/** The first column of a table, naming the document of every row. */
struct table_index_view_t {
    strided_iterator<ustore_collection_id_t const> collections_begin;
    strided_iterator<ustore_key_t const> keys_begin;
    std::size_t count {0};

    strided_range<ustore_collection_id_t const> collections() const noexcept { return {collections_begin, count}; }
    strided_range<ustore_key_t const> keys() const noexcept { return {keys_begin, count}; }
};

/** The header row of a table, naming the field and type of every column. */
struct table_header_view_t {
    strided_iterator<ustore_str_view_t const> fields_begin;
    strided_iterator<ustore_doc_field_type_t const> types_begin;
    std::size_t count {0};

    strided_range<ustore_str_view_t const> fields() const noexcept { return {fields_begin, count}; }
    strided_range<ustore_doc_field_type_t const> types() const noexcept { return {types_begin, count}; }
};

/** Header of a table whose column types are known at compile time, built column by column through @c with. */
template <typename... column_types_>
struct table_header {

    /** Field and type of every column, in order. */
    std::array<field_type_t, sizeof...(column_types_)> columns;

    template <typename element_>
    table_header<column_types_..., element_> with(ustore_str_view_t name) && noexcept {
        table_header<column_types_..., element_> extended;
        std::copy_n(columns.begin(), columns.size(), extended.columns.begin());
        extended.columns.back() = field_type_t {name, ustore_doc_field<element_>()};
        return extended;
    }

    strided_range<ustore_str_view_t const> fields() const noexcept {
        return strided(columns).members(&field_type_t::field);
    }

    strided_range<ustore_doc_field_type_t const> types() const noexcept {
        return strided(columns).members(&field_type_t::type);
    }
};

/** Starts a header whose column types are known at compile time. */
inline table_header<> make_table_header() noexcept { return {}; }

/** Views @p columns, whose types are known only at runtime, as a table header. */
inline table_header_view_t make_table_header(std::span<field_type_t const> columns) noexcept {
    auto const range = strided(columns.data(), columns.data() + columns.size());
    return {range.members(&field_type_t::field).begin(), range.members(&field_type_t::type).begin(), columns.size()};
}

#pragma endregion Headers

#pragma region Tables

/**
 *  @brief Results of @c ustore_docs_gather, with documents as rows and fields as columns.
 *
 *  @tparam column_types_ Types of the columns, or @c std::monostate alone when they are known only at runtime.
 */
template <typename... column_types_>
class docs_table {
    using types_tuple_t = std::tuple<column_types_...>;
    static constexpr bool is_dynamic_k = std::is_same_v<std::tuple<std::monostate>, types_tuple_t>;

    template <std::size_t index_>
    using column_of = std::conditional_t<is_dynamic_k, //
                                         column_view_t, column_view<std::tuple_element_t<index_, types_tuple_t>>>;

    ustore_size_t docs_count_;
    ustore_size_t fields_count_;

    strided_iterator<ustore_collection_id_t const> collections_;
    strided_iterator<ustore_key_t const> keys_;
    strided_iterator<ustore_str_view_t const> fields_;
    strided_iterator<ustore_doc_field_type_t const> types_;

    ustore_octet_t** columns_validities_ {nullptr};
    ustore_octet_t** columns_conversions_ {nullptr};
    ustore_octet_t** columns_collisions_ {nullptr};
    ustore_byte_t** columns_scalars_ {nullptr};
    ustore_length_t** columns_offsets_ {nullptr};
    ustore_length_t** columns_lengths_ {nullptr};
    ustore_byte_t* tape_ {nullptr};

  public:
    docs_table( //
        ustore_size_t docs_count, ustore_size_t fields_count,
        strided_iterator<ustore_collection_id_t const> collections, strided_iterator<ustore_key_t const> keys,
        strided_iterator<ustore_str_view_t const> fields, strided_iterator<ustore_doc_field_type_t const> types,
        ustore_octet_t** columns_validities = nullptr, ustore_octet_t** columns_conversions = nullptr,
        ustore_octet_t** columns_collisions = nullptr, ustore_byte_t** columns_scalars = nullptr,
        ustore_length_t** columns_offsets = nullptr, ustore_length_t** columns_lengths = nullptr,
        ustore_byte_t* tape = nullptr) noexcept
        : docs_count_(docs_count), fields_count_(fields_count), collections_(collections), keys_(keys), fields_(fields),
          types_(types), columns_validities_(columns_validities), columns_conversions_(columns_conversions),
          columns_collisions_(columns_collisions), columns_scalars_(columns_scalars), columns_offsets_(columns_offsets),
          columns_lengths_(columns_lengths), tape_(tape) {}

    table_index_view_t index() const noexcept { return {collections_, keys_, docs_count_}; }
    table_header_view_t header() const noexcept { return {fields_, types_, fields_count_}; }

    template <typename element_ = std::monostate>
    auto column(std::size_t index) const noexcept {
        column_view_t const punned {
            columns_validities_[index],
            columns_conversions_[index],
            columns_collisions_[index],
            columns_scalars_[index],
            tape_,
            columns_offsets_[index],
            columns_lengths_[index],
            docs_count_,
            fields_[index],
            types_[index],
        };
        if constexpr (std::is_same_v<element_, std::monostate>) return punned;
        else return punned.template as<element_>();
    }

    template <std::size_t index_>
    column_of<index_> column() const noexcept {
        if constexpr (is_dynamic_k) return column(index_);
        else return this->template column<std::tuple_element_t<index_, types_tuple_t>>(index_);
    }

    std::size_t rows() const noexcept { return docs_count_; }
    std::size_t columns() const noexcept { return fields_count_; }

    ustore_octet_t*** member_validities() noexcept { return &columns_validities_; }
    ustore_octet_t*** member_conversions() noexcept { return &columns_conversions_; }
    ustore_octet_t*** member_collisions() noexcept { return &columns_collisions_; }
    ustore_byte_t*** member_scalars() noexcept { return &columns_scalars_; }
    ustore_length_t*** member_offsets() noexcept { return &columns_offsets_; }
    ustore_length_t*** member_lengths() noexcept { return &columns_lengths_; }
    ustore_byte_t** member_tape() noexcept { return &tape_; }
};

using docs_table_t = docs_table<std::monostate>;

#pragma endregion Tables

} // namespace unum::ustore
