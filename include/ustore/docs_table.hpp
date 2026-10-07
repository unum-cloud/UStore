/**
 *  @file include/ustore/docs_table.hpp
 *  @author Ash Vardanian
 *  @date July 4, 2022
 *  @brief Columnar views over the results of @c ustore_docs_gather.
 */

#pragma once

#include <cstddef> // `std::byte`, `std::max_align_t`, `std::ptrdiff_t`, `std::size_t`

#include <algorithm>   // `std::copy_n`
#include <array>       // `std::array`
#include <concepts>    // `std::same_as`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <tuple>       // `std::tuple`
#include <type_traits> // `std::conditional_t`
#include <utility>     // `std::exchange`, `std::move`
#include <variant>     // `std::monostate`

#include "ustore/docs.h"
#include "ustore/ranges.hpp" // `strided_iterator`, `strided_range`, `bits_view_t`, `owning_view`
#include "ustore/status.hpp" // `status_t`, `expected`
#include "ustore/types.hpp"  // `value_view_t`, `ustore_doc_field`, `round_up_to_multiple`

namespace unum::ustore {

#pragma region Cells and Columns

/** Whether a gathered column of @p type holds strings, which offsets and lengths address, rather than scalars. */
constexpr bool is_variable_length(ustore_doc_field_type_t type) noexcept {
    return type == ustore_doc_field_str_k || type == ustore_doc_field_bin_k;
}

/** Bytes one cell of a gathered scalar column of @p type takes, and zero for a type no scalar column holds. */
constexpr std::size_t scalar_bytes(ustore_doc_field_type_t type) noexcept {
    switch (type) {
    case ustore_doc_field_bool_k: return sizeof(bool);
    case ustore_doc_field_uuid_k: return sizeof(ustore_uuid_t);
    case ustore_doc_field_f64_k: return sizeof(f64_t);
    case ustore_doc_field_f32_k: return sizeof(f32_t);
    case ustore_doc_field_f16_k: return sizeof(u16_t);
    case ustore_doc_field_i64_k: return sizeof(i64_t);
    case ustore_doc_field_i32_k: return sizeof(i32_t);
    case ustore_doc_field_i16_k: return sizeof(i16_t);
    case ustore_doc_field_i8_k: return sizeof(i8_t);
    case ustore_doc_field_u64_k: return sizeof(u64_t);
    case ustore_doc_field_u32_k: return sizeof(u32_t);
    case ustore_doc_field_u16_k: return sizeof(u16_t);
    case ustore_doc_field_u8_k: return sizeof(u8_t);
    default: return 0;
    }
}

/** Bytes a gathered column of @p type takes for @p rows documents, its offsets and lengths for strings, aligned for
 *  any scalar. */
constexpr std::size_t column_bytes(ustore_doc_field_type_t type, std::size_t rows) noexcept {
    std::size_t const bytes = is_variable_length(type) ? sizeof(ustore_length_t) * (2 * rows + 1)
                                                       : scalar_bytes(type) * rows;
    return round_up_to_multiple<std::size_t>(bytes, alignof(std::max_align_t));
}

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
 *  The columns live in a buffer that @c make allocates and its result owns, while the joined strings stay in the
 *  arena of the gather.
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

    /** Six rows of @c fields_count_ column addresses one after another: the validities, conversions, collisions,
     *  scalars, offsets and lengths. */
    ustore_octet_t** addresses_ {nullptr};
    ustore_byte_t* tape_ {nullptr};

  public:
    docs_table( //
        ustore_size_t docs_count, ustore_size_t fields_count,
        strided_iterator<ustore_collection_id_t const> collections, strided_iterator<ustore_key_t const> keys,
        strided_iterator<ustore_str_view_t const> fields, strided_iterator<ustore_doc_field_type_t const> types,
        ustore_octet_t** addresses) noexcept
        : docs_count_(docs_count), fields_count_(fields_count), collections_(collections), keys_(keys), fields_(fields),
          types_(types), addresses_(addresses) {}

    table_index_view_t index() const noexcept { return {collections_, keys_, docs_count_}; }
    table_header_view_t header() const noexcept { return {fields_, types_, fields_count_}; }

    template <typename element_ = std::monostate>
    auto column(std::size_t index) const noexcept {
        ustore_octet_t** const validities = columns_validities();
        ustore_octet_t** const conversions = columns_conversions();
        ustore_octet_t** const collisions = columns_collisions();
        ustore_byte_t** const scalars = columns_scalars();
        ustore_length_t** const offsets = columns_offsets();
        ustore_length_t** const lengths = columns_lengths();
        column_view_t const punned {
            validities[index], conversions[index], collisions[index], scalars[index], tape_,
            offsets[index],    lengths[index],     docs_count_,       fields_[index], types_[index],
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

    ustore_octet_t** columns_validities() const noexcept { return addresses_; }
    ustore_octet_t** columns_conversions() const noexcept { return addresses_ + fields_count_; }
    ustore_octet_t** columns_collisions() const noexcept { return addresses_ + 2 * fields_count_; }
    ustore_byte_t** columns_scalars() const noexcept { return addresses_ + 3 * fields_count_; }
    ustore_length_t** columns_offsets() const noexcept {
        return reinterpret_cast<ustore_length_t**>(addresses_ + 4 * fields_count_);
    }
    ustore_length_t** columns_lengths() const noexcept { return columns_offsets() + fields_count_; }
    ustore_byte_t** member_tape() noexcept { return &tape_; }

    /** A table of @p docs_count rows and @p fields_count columns, whose cells live in one zeroed buffer: six rows of
     *  column addresses, then three bitmaps per column, then the columns themselves. The addresses a column's type
     *  leaves unused stay null. */
    static expected<owning_view<docs_table>> make( //
        ustore_size_t docs_count, ustore_size_t fields_count,
        strided_iterator<ustore_collection_id_t const> collections, strided_iterator<ustore_key_t const> keys,
        strided_iterator<ustore_str_view_t const> fields,
        strided_iterator<ustore_doc_field_type_t const> types) noexcept {

        std::size_t const bitmap_bytes = divide_round_up<std::size_t>(docs_count, bits_in_byte_k);
        std::size_t const addresses_bytes = 6 * sizeof(void*) * fields_count;
        std::size_t const bitmaps_bytes = round_up_to_multiple<std::size_t>(3 * bitmap_bytes * fields_count,
                                                                            alignof(std::max_align_t));
        std::size_t columns_bytes = 0;
        for (std::size_t field = 0; field != fields_count; ++field)
            columns_bytes += column_bytes(types[static_cast<std::ptrdiff_t>(field)], docs_count);
        st::basic_vector<std::byte> buffer;
        if (st::failed(buffer.resize(addresses_bytes + bitmaps_bytes + columns_bytes)))
            return status_t {status_code_t::out_of_memory_k, "Failed to allocate the columns of a table"};

        std::byte* const addresses = buffer.data();
        docs_table const table {
            docs_count, fields_count, collections, keys, fields, types, reinterpret_cast<ustore_octet_t**>(addresses)};
        ustore_octet_t* bitmap = reinterpret_cast<ustore_octet_t*>(addresses + addresses_bytes);
        std::byte* column = addresses + addresses_bytes + bitmaps_bytes;
        ustore_octet_t** const validities = table.columns_validities();
        ustore_octet_t** const conversions = table.columns_conversions();
        ustore_octet_t** const collisions = table.columns_collisions();
        ustore_byte_t** const scalars = table.columns_scalars();
        ustore_length_t** const offsets_columns = table.columns_offsets();
        ustore_length_t** const lengths_columns = table.columns_lengths();
        for (std::size_t field = 0; field != fields_count; ++field) {
            validities[field] = std::exchange(bitmap, bitmap + bitmap_bytes);
            conversions[field] = std::exchange(bitmap, bitmap + bitmap_bytes);
            collisions[field] = std::exchange(bitmap, bitmap + bitmap_bytes);
            ustore_doc_field_type_t const type = types[static_cast<std::ptrdiff_t>(field)];
            if (is_variable_length(type)) {
                ustore_length_t* const offsets = reinterpret_cast<ustore_length_t*>(column);
                offsets_columns[field] = offsets;
                lengths_columns[field] = offsets + docs_count + 1;
            }
            else scalars[field] = reinterpret_cast<ustore_byte_t*>(column);
            column += column_bytes(type, docs_count);
        }
        return owning_view<docs_table> {table, std::move(buffer)};
    }
};

using docs_table_t = docs_table<std::monostate>;

#pragma endregion Tables

} // namespace unum::ustore
