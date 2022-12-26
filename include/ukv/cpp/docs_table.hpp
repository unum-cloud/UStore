/**
 * @file docs_table.hpp
 * @author Ashot Vardanian
 * @date 4 Jul 2022
 * @addtogroup Cpp
 *
 * @brief C++ bindings for "ukv/docs.h".
 *
 * Most field-level operations are still accessible through normal @c member_refs_t.
 * This interface mostly helps with tabular and SoA <-> AoS operations involving:
 * - ::ukv_docs_gist
 * - ::ukv_docs_gather_t
 */

#pragma once
#include <limits.h>    // `CHAR_BIT`
#include <string_view> // `std::basic_string_view`
#include <vector>      // `std::vector`
#include <variant>     // `std::monostate`
#include <array>       // `std::array`

#include "ukv/docs.h"
#include "ukv/cpp/ranges.hpp"

namespace unum::ukv {

template <typename element_at>
constexpr bool is_variable_length() {
    return std::is_same_v<element_at, value_view_t> || std::is_same_v<element_at, std::string_view>;
}

template <typename element_at>
struct cell_gt;

template <typename element_at>
struct field_type_gt;

class column_view_t;

template <typename element_at>
class column_view_varlen_gt;

template <typename element_at>
class column_view_scalar_gt;

template <typename element_at>
using column_view_gt = std::conditional_t<is_variable_length<element_at>(),
                                          column_view_varlen_gt<element_at>,
                                          column_view_scalar_gt<element_at>>;

template <typename... column_types_at>
class docs_table_gt;

template <typename... column_types_at>
class table_header_gt;

using docs_table_t = docs_table_gt<std::monostate>;

using field_type_t = field_type_gt<std::monostate>;

using table_header_t = table_header_gt<std::monostate>;

/**
 * @brief The first column of the table, describing its contents.
 */
struct table_index_view_t {
    strided_iterator_gt<ukv_collection_t const> collections_begin;
    strided_iterator_gt<ukv_key_t const> keys_begin;
    std::size_t count {0};

    strided_range_gt<ukv_collection_t const> collections() const noexcept { return {collections_begin, count}; }
    strided_range_gt<ukv_key_t const> keys() const noexcept { return {keys_begin, count}; }
};

struct table_header_view_t {
    strided_iterator_gt<ukv_str_view_t const> fields_begin;
    strided_iterator_gt<ukv_doc_field_type_t const> types_begin;
    std::size_t count {0};

    strided_range_gt<ukv_str_view_t const> fields() const noexcept { return {fields_begin, count}; }
    strided_range_gt<ukv_doc_field_type_t const> types() const noexcept { return {types_begin, count}; }
};

template <typename element_at>
struct cell_gt {
    bool valid {false};
    bool converted {false};
    bool collides {false};
    element_at value;
};

template <>
struct cell_gt<value_view_t> {
    bool valid {false};
    bool converted {false};
    bool collides {false};
    value_view_t value;
};

template <typename element_at>
class column_view_scalar_gt {
    ukv_octet_t* validities_ {nullptr};
    ukv_octet_t* conversions_ {nullptr};
    ukv_octet_t* collisions_ {nullptr};
    element_at* scalars_ {nullptr};
    ukv_size_t count_ {0};
    ukv_str_view_t name_ {nullptr};

  public:
    using scalar_t = element_at;
    using cell_t = cell_gt<scalar_t>;
    using value_type = cell_t;

    column_view_scalar_gt( //
        ukv_octet_t* validities,
        ukv_octet_t* conversions,
        ukv_octet_t* collisions,
        scalar_t* scalars,
        ukv_size_t count,
        ukv_str_view_t name = nullptr) noexcept
        : validities_(validities), conversions_(conversions), collisions_(collisions), scalars_(scalars), count_(count),
          name_(name) {}

    column_view_scalar_gt(column_view_scalar_gt&&) = default;
    column_view_scalar_gt(column_view_scalar_gt const&) = default;
    column_view_scalar_gt& operator=(column_view_scalar_gt&&) = default;
    column_view_scalar_gt& operator=(column_view_scalar_gt const&) = default;

    ukv_str_view_t name() const noexcept { return name_; }
    std::size_t size() const noexcept { return count_; }
    cell_t operator[](std::size_t i) const noexcept {
        // Bitmaps are indexed from the last bit within every byte
        // https://arrow.apache.org/docs/format/Columnar.html#validity-bitmaps
        ukv_octet_t mask_bitmap = static_cast<ukv_octet_t>(1 << (i % CHAR_BIT));
        cell_t result;
        result.valid = validities_[i / CHAR_BIT] & mask_bitmap;
        result.converted = conversions_[i / CHAR_BIT] & mask_bitmap;
        result.collides = collisions_[i / CHAR_BIT] & mask_bitmap;
        result.value = scalars_[i];
        return result;
    }
};

template <typename element_at>
class column_view_varlen_gt {
    ukv_octet_t* validities_ {nullptr};
    ukv_octet_t* conversions_ {nullptr};
    ukv_octet_t* collisions_ {nullptr};
    ukv_byte_t* tape_ {nullptr};
    ukv_length_t* offsets_ {nullptr};
    ukv_length_t* lengths_ {nullptr};
    ukv_size_t count_ {0};
    ukv_str_view_t name_ {nullptr};

  public:
    using cell_t = cell_gt<element_at>;
    using value_type = cell_t;

    column_view_varlen_gt( //
        ukv_octet_t* validities,
        ukv_octet_t* conversions,
        ukv_octet_t* collisions,
        ukv_byte_t* tape,
        ukv_length_t* offsets,
        ukv_length_t* lengths,
        ukv_size_t count,
        ukv_str_view_t name = nullptr) noexcept
        : validities_(validities), conversions_(conversions), collisions_(collisions), tape_(tape), offsets_(offsets),
          lengths_(lengths), count_(count), name_(name) {}

    column_view_varlen_gt(column_view_varlen_gt&&) = default;
    column_view_varlen_gt(column_view_varlen_gt const&) = default;
    column_view_varlen_gt& operator=(column_view_varlen_gt&&) = default;
    column_view_varlen_gt& operator=(column_view_varlen_gt const&) = default;

    ukv_str_view_t name() const noexcept { return name_; }
    std::size_t size() const noexcept { return count_; }
    cell_t operator[](std::size_t i) const noexcept {
        // Bitmaps are indexed from the last bit within every byte
        // https://arrow.apache.org/docs/format/Columnar.html#validity-bitmaps
        using str_char_t = typename element_at::value_type;
        auto str_begin = reinterpret_cast<str_char_t const*>(tape_ + offsets_[i]);

        ukv_octet_t mask_bitmap = static_cast<ukv_octet_t>(1 << (i % CHAR_BIT));
        cell_t result;
        result.valid = validities_[i / CHAR_BIT] & mask_bitmap;
        result.converted = conversions_[i / CHAR_BIT] & mask_bitmap;
        result.collides = collisions_[i / CHAR_BIT] & mask_bitmap;
        result.value = element_at {str_begin, lengths_[i]};
        return result;
    }
};

class column_view_t {
    ukv_octet_t* validities_ {nullptr};
    ukv_octet_t* conversions_ {nullptr};
    ukv_octet_t* collisions_ {nullptr};
    ukv_byte_t* scalars_ {nullptr};
    ukv_byte_t* tape_ {nullptr};
    ukv_length_t* offsets_ {nullptr};
    ukv_length_t* lengths_ {nullptr};
    ukv_size_t count_ {0};
    ukv_str_view_t name_ {nullptr};
    ukv_doc_field_type_t type_ {ukv_doc_field_json_k};

  public:
    column_view_t( //
        ukv_octet_t* validities,
        ukv_octet_t* conversions,
        ukv_octet_t* collisions,
        ukv_byte_t* scalars,
        ukv_byte_t* tape,
        ukv_length_t* offsets,
        ukv_length_t* lengths,
        ukv_size_t count,
        ukv_str_view_t name = nullptr,
        ukv_doc_field_type_t type = ukv_doc_field_json_k) noexcept
        : validities_(validities), conversions_(conversions), collisions_(collisions), scalars_(scalars), tape_(tape),
          offsets_(offsets), lengths_(lengths), count_(count), name_(name), type_(type) {}

    column_view_t(column_view_t&&) = default;
    column_view_t(column_view_t const&) = default;
    column_view_t& operator=(column_view_t&&) = default;
    column_view_t& operator=(column_view_t const&) = default;

    ukv_str_view_t name() const noexcept { return name_; }
    ukv_doc_field_type_t type() const noexcept { return type_; }
    std::size_t size() const noexcept { return count_; }

    template <typename element_at>
    column_view_gt<element_at> as() const noexcept {
        if constexpr (is_variable_length<element_at>())
            return {validities_, conversions_, collisions_, tape_, offsets_, lengths_, count_, name_};
        else
            return {validities_, conversions_, collisions_, reinterpret_cast<element_at*>(scalars_), count_, name_};
    }

    ukv_octet_t* validities() const noexcept { return validities_; }
    ukv_length_t* offsets() const noexcept { return offsets_; }
    ukv_length_t* lengths() const noexcept { return lengths_; }
    ukv_byte_t* contents() const noexcept { return scalars_ ? scalars_ : tape_; }
};

/**
 * @brief
 *
 * @tparam column_types_at Optional type-annotation for columns.
 */
template <typename... column_types_at>
class docs_table_gt {

    using empty_tuple_t = std::tuple<std::monostate>;
    using types_tuple_t = std::tuple<column_types_at...>;
    using row_tuple_t = std::tuple<cell_gt<column_types_at>...>;
    static constexpr bool is_dynamic_k = std::is_same_v<empty_tuple_t, types_tuple_t>;

    template <std::size_t idx_ak>
    using column_at_gt =
        std::conditional_t<is_dynamic_k, column_view_t, column_view_gt<std::tuple_element_t<idx_ak, types_tuple_t>>>;

    ukv_size_t docs_count_;
    ukv_size_t fields_count_;

    strided_iterator_gt<ukv_collection_t const> collections_;
    strided_iterator_gt<ukv_key_t const> keys_;
    strided_iterator_gt<ukv_str_view_t const> fields_;
    strided_iterator_gt<ukv_doc_field_type_t const> types_;

    ukv_octet_t** columns_validities_ = nullptr;
    ukv_octet_t** columns_conversions_ = nullptr;
    ukv_octet_t** columns_collisions_ = nullptr;
    ukv_byte_t** columns_scalars_ = nullptr;
    ukv_length_t** columns_offsets_ = nullptr;
    ukv_length_t** columns_lengths_ = nullptr;
    ukv_byte_t* tape_ = nullptr;

  public:
    docs_table_gt( //
        ukv_size_t docs_count,
        ukv_size_t fields_count,
        strided_iterator_gt<ukv_collection_t const> collections,
        strided_iterator_gt<ukv_key_t const> keys,
        strided_iterator_gt<ukv_str_view_t const> fields,
        strided_iterator_gt<ukv_doc_field_type_t const> types,
        ukv_octet_t** columns_validities = nullptr,
        ukv_octet_t** columns_conversions = nullptr,
        ukv_octet_t** columns_collisions = nullptr,
        ukv_byte_t** columns_scalars = nullptr,
        ukv_length_t** columns_offsets = nullptr,
        ukv_length_t** columns_lengths = nullptr,
        ukv_byte_t* tape = nullptr) noexcept
        : docs_count_(docs_count), fields_count_(fields_count), collections_(collections), keys_(keys), fields_(fields),
          types_(types), columns_validities_(columns_validities), columns_conversions_(columns_conversions),
          columns_collisions_(columns_collisions), columns_scalars_(columns_scalars), columns_offsets_(columns_offsets),
          columns_lengths_(columns_lengths), tape_(tape) {}

    docs_table_gt(docs_table_gt&&) = default;
    docs_table_gt(docs_table_gt const&) = default;
    docs_table_gt& operator=(docs_table_gt&&) = default;
    docs_table_gt& operator=(docs_table_gt const&) = default;

    table_index_view_t index() const noexcept { return {collections_, keys_, docs_count_}; }
    table_header_view_t header() const noexcept { return {fields_, types_, docs_count_}; }

    template <typename element_at = std::monostate>
    auto column(std::size_t i) const noexcept {
        column_view_t punned {
            columns_validities_[i],
            columns_conversions_[i],
            columns_collisions_[i],
            columns_scalars_[i],
            tape_,
            columns_offsets_[i],
            columns_lengths_[i],
            docs_count_,
            fields_[i],
            types_[i],
        };
        if constexpr (std::is_same_v<element_at, std::monostate>)
            return punned;
        else
            return punned.template as<element_at>();
    }

    template <std::size_t idx_ak>
    column_at_gt<idx_ak> column() const noexcept {
        if constexpr (!is_dynamic_k) {
            using scalar_t = std::tuple_element_t<idx_ak, types_tuple_t>;
            return this->template column<scalar_t>(idx_ak);
        }
        else
            return column(idx_ak);
    }

    row_tuple_t row(std::size_t i) const noexcept {
        // TODO:
        return {};
    }

    std::size_t rows() const noexcept { return docs_count_; }
    std::size_t collections() const noexcept { return fields_count_; }

    ukv_octet_t*** member_validities() noexcept { return &columns_validities_; }
    ukv_octet_t*** member_conversions() noexcept { return &columns_conversions_; }
    ukv_octet_t*** member_collisions() noexcept { return &columns_collisions_; }
    ukv_byte_t*** member_scalars() noexcept { return &columns_scalars_; }
    ukv_length_t*** member_offsets() noexcept { return &columns_offsets_; }
    ukv_length_t*** member_lengths() noexcept { return &columns_lengths_; }
    ukv_byte_t** member_tape() noexcept { return &tape_; }
};

template <typename element_at>
struct field_type_gt {
    ukv_str_view_t field = nullptr;
    ukv_doc_field_type_t type = ukv_doc_field_json_k;
};

template <>
struct field_type_gt<std::monostate> {
    ukv_str_view_t field = nullptr;
    ukv_doc_field_type_t type = ukv_doc_field_json_k;
};

/**
 * @brief Combination of index column and header row,
 * defining the order of @b statically-typed contents in the table.
 */
template <typename... column_types_at>
struct table_header_gt {

    using types_tuple_t = std::tuple<column_types_at...>;
    static constexpr std::size_t collections_count_k = std::tuple_size_v<types_tuple_t>;
    using columns_t = std::array<field_type_t, collections_count_k>;

    columns_t columns;

    template <typename element_at>
    table_header_gt<column_types_at..., element_at> with(ukv_str_view_t name) && {
        using new_columns_t = std::array<field_type_t, collections_count_k + 1>;
        new_columns_t new_columns;
        std::copy_n(columns.begin(), collections_count_k, new_columns.begin());
        new_columns[collections_count_k] = field_type_t {name, ukv_doc_field<element_at>()};
        return {new_columns};
    }

    strided_range_gt<ukv_str_view_t const> fields() const noexcept {
        return strided_range(columns).members(&field_type_t::field);
    }

    strided_range_gt<ukv_doc_field_type_t const> types() const noexcept {
        return strided_range(columns).members(&field_type_t::type);
    }
};

inline table_header_gt<> table_header() {
    return {};
}

/**
 * @brief Combination of index column and header row,
 * defining the order of @b dynamically-typed contents in the table.
 */
template <>
struct table_header_gt<std::monostate> {

    std::vector<field_type_t> columns;

    void clear() noexcept { columns.clear(); }
    table_header_view_t view() const noexcept { return {fields().begin(), types().begin(), columns.size()}; }
    operator table_header_view_t() const noexcept { return view(); }

    template <typename element_at>
    table_header_gt& with(ukv_str_view_t name) & {
        return with(name, ukv_doc_field<element_at>());
    }

    table_header_gt& with(ukv_str_view_t name, ukv_doc_field_type_t type) & {
        columns.push_back({name, type});
        return *this;
    }

    strided_range_gt<ukv_str_view_t const> fields() const noexcept {
        return strided_range(columns).members(&field_type_t::field);
    }

    strided_range_gt<ukv_doc_field_type_t const> types() const noexcept {
        return strided_range(columns).members(&field_type_t::type);
    }
};

} // namespace unum::ukv
