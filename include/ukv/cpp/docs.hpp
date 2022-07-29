/**
 * @file docs.hpp
 * @author Ashot Vardanian
 * @date 4 Jul 2022
 * @brief C++ bindings for @see "ukv/docs.h".
 *
 * Most field-level operations are still accessible through normal @c `member_refs_t`.
 * This interface mostly helps with tabular and SoA <-> AoS operations involving:
 * > ukv_docs_gist(...)
 * > ukv_docs_gather_scalars(...)
 * > ukv_docs_gather_strings(...)
 */

#pragma once
#include <vector>

#include "ukv/docs.h"
#include "ukv/ukv.hpp"

namespace unum::ukv {

/**
 * @brief Iterates through a predetermined number of NULL-delimited
 * strings joined one after another in continuous memory.
 * Can be used for `ukv_docs_gist` or `ukv_collection_list`.
 */
class strings_tape_iterator_t {
    ukv_size_t remaining_count_ = 0;
    ukv_str_view_t current_ = nullptr;

  public:
    using iterator_category = std::forward_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = std::string_view;
    using pointer = ukv_str_view_t*;
    using reference = std::string_view;

    strings_tape_iterator_t(ukv_size_t remaining = 0, ukv_str_view_t current = nullptr)
        : remaining_count_(remaining), current_(current) {}

    strings_tape_iterator_t(strings_tape_iterator_t&&) = default;
    strings_tape_iterator_t& operator=(strings_tape_iterator_t&&) = default;

    strings_tape_iterator_t(strings_tape_iterator_t const&) = default;
    strings_tape_iterator_t& operator=(strings_tape_iterator_t const&) = default;

    strings_tape_iterator_t& operator++() noexcept {
        current_ += std::strlen(current_) + 1;
        --remaining_count_;
        return *this;
    }

    strings_tape_iterator_t operator++(int) noexcept {
        return {remaining_count_ - 1, current_ + std::strlen(current_) + 1};
    }

    bool is_end() const noexcept { return !remaining_count_; }
};

template <typename scalar_at>
class field_cell_gt {
    bool valid = false;
    bool converted = false;
    bool collides = false;
    scalar_at* scalars_ = nullptr;
};

template <typename scalar_at>
class field_column_view_gt {
    std::uint8_t* validities_ = nullptr;
    std::uint8_t* conversions_ = nullptr;
    std::uint8_t* collisions_ = nullptr;
    scalar_at* scalars_ = nullptr;
};

class doc_row_view_t {

    strided_iterator_gt<ukv_str_view_t> fields;
    strided_iterator_gt<ukv_type_t> types;

    std::uint8_t* all_validities_ = nullptr;
    std::uint8_t* all_conversions_ = nullptr;
    std::uint8_t* all_collisions_ = nullptr;
    std::uint8_t* all_scalars_ = nullptr;

    col_key_t row_id_;
    std::uint8_t row_mask_ = 0;
    std::size_t row_idx_ = 0;
};

inline std::size_t bytes_in_bitset_column(std::size_t docs_count) noexcept {
    return ((docs_count + 7) / 8) * 8;
}

inline std::size_t bytes_in_scalar_column(std::size_t docs_count, ukv_type_t type) noexcept {
    switch (type) {
    case ukv_type_bool_k: return docs_count;
    case ukv_type_i64_k: return docs_count * 8;
    case ukv_type_f64_k: return docs_count * 8;
    case ukv_type_uuid_k: return docs_count * 16;
    default: return 0;
    }
}

class docs_table_view_t {

    std::size_t docs_count_;
    std::size_t fields_count_;

    strided_iterator_gt<ukv_str_view_t> fields_;
    strided_iterator_gt<ukv_type_t> types_;

    std::uint8_t* validities_ = nullptr;
    std::uint8_t* conversions_ = nullptr;
    std::uint8_t* collisions_ = nullptr;
    std::uint8_t* scalars_ = nullptr;

  public:
    docs_table_view_t(std::size_t docs_count,
                      std::size_t fields_count,
                      strided_iterator_gt<ukv_str_view_t const> fields,
                      strided_iterator_gt<ukv_type_t const> types,
                      ukv_val_ptr_t columns_validities,
                      ukv_val_ptr_t columns_conversions,
                      ukv_val_ptr_t columns_collisions,
                      ukv_val_ptr_t columns_scalars);

    template <typename scalar_at>
    field_column_view_gt<scalar_at> column(std::size_t column_idx) const noexcept {
        std::size_t jump_bitset = bytes_in_bitset_column(docs_count_);
        std::uint8_t* validities = validities_ + jump_bitset * column_idx;
        std::uint8_t* conversions = conversions_ + jump_bitset * column_idx;
        std::uint8_t* collisions = collisions_ + jump_bitset * column_idx;

        std::uint8_t* scalars = scalars_;
        if (types_.repeats())
            scalars += bytes_in_scalar_column(docs_count_, types_[0]) * column_idx;
        else
            for (std::size_t i = 0; i != column_idx; ++i)
                scalars += bytes_in_scalar_column(docs_count_, types_[i]);

        return {validities, conversions, collisions, reinterpret_cast<scalar_at*>(scalars)};
    }

    std::size_t rows() const noexcept { return docs_count_; }
    std::size_t cols() const noexcept { return fields_count_; }
};

class docs_table_t {

    struct column_info_t {
        ukv_str_view_t field;
        ukv_type_t type;
    };
    std::vector<std::uint8_t> buffer_;
    std::vector<column_info_t> columns_;
    std::size_t const docs_count_ = 0;

  public:
    docs_table_t(std::size_t docs_count) noexcept(false) : docs_count_(docs_count) {}
    docs_table_t& add_column(ukv_str_view_t name, ukv_type_t type) noexcept(false) {
        auto bitset_size = bytes_in_bitset_column(docs_count_);
        auto scalars_size = bytes_in_scalar_column(docs_count_, type);
        buffer_.resize(buffer_.size() + bitset_size * 3 + scalars_size);
        columns_.push_back({name, type});
        return *this;
    }

    void clear() noexcept {
        columns_.clear();
        buffer_.clear();
    }

    ukv_val_ptr_t data() const noexcept { return ukv_val_ptr_t(buffer_.data()); }
    ukv_val_ptr_t validities() const noexcept { return data() + bytes_in_bitset_column(docs_count_) * 0; }
    ukv_val_ptr_t conversions() const noexcept { return data() + bytes_in_bitset_column(docs_count_) * 1; }
    ukv_val_ptr_t collisions() const noexcept { return data() + bytes_in_bitset_column(docs_count_) * 2; }
    ukv_val_ptr_t scalars() const noexcept { return data() + bytes_in_bitset_column(docs_count_) * 3; }

    docs_table_view_t view() const noexcept {
        auto cols = strided_range_gt<column_info_t const>(columns_);
        return {
            docs_count_,
            columns_.size(),
            cols.members(&column_info_t::field).begin(),
            cols.members(&column_info_t::type).begin(),
            validities(),
            conversions(),
            collisions(),
            scalars(),
        };
    }
};

} // namespace unum::ukv
