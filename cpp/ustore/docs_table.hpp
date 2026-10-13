/**
 *  @file cpp/ustore/docs_table.hpp
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
#include "ustore/ranges.hpp" // `strided_iterator`, `strided_span`, `bits_view_t`
#include "ustore/types.hpp"  // `value_view_t`, `docs_field_type`

namespace unum::ustore {

#pragma region Cells and Columns

/** Whether a gathered column of @p type holds strings, which offsets address, rather than scalars. */
constexpr bool is_variable_length(ustore_docs_field_type_t type) noexcept {
    return type == ustore_docs_field_type_str_k || type == ustore_docs_field_type_bin_k;
}

/** Element types stored as offsets into a joined tape rather than as fixed-width scalars. */
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

/** Column of fixed-width scalars, viewing the arena of the gather. */
template <typename element_>
class column_view_scalar {
    ustore_docs_column_view_t arrays_ {};
    ustore_size_t count_ {0};
    std::string_view name_ {};

  public:
    using cell_t = cell<element_>;
    using value_type = cell_t;

    column_view_scalar(ustore_docs_column_view_t const& arrays, ustore_size_t count,
                       std::string_view name = {}) noexcept
        : arrays_(arrays), count_(count), name_(name) {}

    std::string_view name() const noexcept { return name_; }
    std::size_t size() const noexcept { return count_; }

    cell_t operator[](std::size_t index) const noexcept {
        element_ const* const scalars = reinterpret_cast<element_ const*>(arrays_.scalars);
        return {bits_view_t {arrays_.validities}[index], bits_view_t {arrays_.conversions}[index],
                bits_view_t {arrays_.collisions}[index], scalars[index]};
    }
};

/** Column of strings, a tape of either offset width inside the arena of the gather. */
template <typename element_>
class column_view_varlen {
    ustore_docs_column_view_t arrays_ {};
    ustore_size_t count_ {0};
    std::string_view name_ {};
    ustore_docs_field_type_t type_ {ustore_docs_field_type_str_k};

  public:
    using cell_t = cell<element_>;
    using value_type = cell_t;

    column_view_varlen(ustore_docs_column_view_t const& arrays, ustore_size_t count, std::string_view name,
                       ustore_docs_field_type_t type) noexcept
        : arrays_(arrays), count_(count), name_(name), type_(type) {}

    std::string_view name() const noexcept { return name_; }
    std::size_t size() const noexcept { return count_; }

    cell_t operator[](std::size_t index) const noexcept {
        using character_t = typename element_::value_type;
        ustore_bytes_view_t const bytes = tape_string(arrays_.strings, index);
        std::size_t length = bytes.length;
        // A text column spans the null terminator of every value, which the cell leaves out
        if (type_ == ustore_docs_field_type_str_k && length) --length;
        auto const begin = reinterpret_cast<character_t const*>(bytes.data);
        return {bits_view_t {arrays_.validities}[index], bits_view_t {arrays_.conversions}[index],
                bits_view_t {arrays_.collisions}[index], element_ {begin, length}};
    }
};

/** Column view matching how @p element_ is stored. */
template <typename element_>
using column_view = std::conditional_t<variable_length_element_concept<element_>, //
                                       column_view_varlen<element_>, column_view_scalar<element_>>;

/** Column whose type is known only at runtime, viewed as a typed column through @c as. */
class column_view_t {
    ustore_docs_column_view_t arrays_ {};
    ustore_size_t count_ {0};
    std::string_view name_ {};
    ustore_docs_field_type_t type_ {ustore_docs_field_type_json_k};

  public:
    column_view_t(ustore_docs_column_view_t const& arrays, ustore_size_t count, std::string_view name = {},
                  ustore_docs_field_type_t type = ustore_docs_field_type_json_k) noexcept
        : arrays_(arrays), count_(count), name_(name), type_(type) {}

    std::string_view name() const noexcept { return name_; }
    ustore_docs_field_type_t type() const noexcept { return type_; }
    std::size_t size() const noexcept { return count_; }

    template <typename element_>
    column_view<element_> as() const noexcept {
        if constexpr (variable_length_element_concept<element_>) return {arrays_, count_, name_, type_};
        else return {arrays_, count_, name_};
    }

    ustore_octet_t const* validities() const noexcept { return arrays_.validities; }

    /** The scalars of a scalar column, or null for a string column. */
    ustore_byte_t const* scalars() const noexcept { return arrays_.scalars; }

    /** The strings of a string column as a tape, or a zeroed view for a scalar column. */
    ustore_strings_view_t const& strings() const noexcept { return arrays_.strings; }
};

#pragma endregion Cells and Columns

#pragma region Headers

/** A column of a table header: the field to gather and the type to convert it into. */
struct field_type_t {

    /** Field inside every document. */
    std::string_view field {};

    /** Type the field converts into. */
    ustore_docs_field_type_t type {ustore_docs_field_type_json_k};
};

/** The first column of a table, naming the document of every row. */
struct table_index_view_t {
    strided_iterator<ustore_collection_id_t const> collections_begin;
    strided_iterator<ustore_key_t const> keys_begin;
    std::size_t count {0};

    strided_span<ustore_collection_id_t const> collections() const noexcept { return {collections_begin, count}; }
    strided_span<ustore_key_t const> keys() const noexcept { return {keys_begin, count}; }
};

/** The header row of a table, naming the field and type of every column. */
struct table_header_view_t {
    strided_iterator<std::string_view const> fields_begin;
    strided_iterator<ustore_docs_field_type_t const> types_begin;
    std::size_t count {0};

    strided_span<std::string_view const> fields() const noexcept { return {fields_begin, count}; }
    strided_span<ustore_docs_field_type_t const> types() const noexcept { return {types_begin, count}; }
};

/** Header of a table whose column types are known at compile time, built column by column through @c with. */
template <typename... column_types_>
struct table_header {

    /** Field and type of every column, in order. */
    std::array<field_type_t, sizeof...(column_types_)> columns;

    template <typename element_>
    table_header<column_types_..., element_> with(std::string_view name) && noexcept {
        table_header<column_types_..., element_> extended;
        std::copy_n(columns.begin(), columns.size(), extended.columns.begin());
        extended.columns.back() = field_type_t {name, docs_field_type<element_>()};
        return extended;
    }

    strided_span<std::string_view const> fields() const noexcept {
        return strided(columns).members(&field_type_t::field);
    }

    strided_span<ustore_docs_field_type_t const> types() const noexcept {
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
 *  A view that copies cheaply: its columns live in the arena of the gather, while its index and header view the keys
 *  and fields the gather was asked for.
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

    ustore_size_t docs_count_ {0};
    ustore_size_t fields_count_ {0};

    strided_iterator<ustore_collection_id_t const> collections_;
    strided_iterator<ustore_key_t const> keys_;
    strided_iterator<std::string_view const> fields_;
    strided_iterator<ustore_docs_field_type_t const> types_;

    /** One column per field, inside the arena of the gather. */
    ustore_docs_column_view_t const* columns_ {nullptr};

  public:
    docs_table() noexcept = default;
    docs_table( //
        ustore_size_t docs_count, ustore_size_t fields_count,
        strided_iterator<ustore_collection_id_t const> collections, strided_iterator<ustore_key_t const> keys,
        strided_iterator<std::string_view const> fields, strided_iterator<ustore_docs_field_type_t const> types,
        ustore_docs_column_view_t const* columns) noexcept
        : docs_count_(docs_count), fields_count_(fields_count), collections_(collections), keys_(keys), fields_(fields),
          types_(types), columns_(columns) {}

    table_index_view_t index() const noexcept { return {collections_, keys_, docs_count_}; }
    table_header_view_t header() const noexcept { return {fields_, types_, fields_count_}; }

    template <typename element_ = std::monostate>
    auto column(std::size_t index) const noexcept {
        auto const signed_index = static_cast<std::ptrdiff_t>(index);
        column_view_t const punned {columns_[index], docs_count_, fields_[signed_index], types_[signed_index]};
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
};

using docs_table_t = docs_table<std::monostate>;

#pragma endregion Tables

} // namespace unum::ustore
