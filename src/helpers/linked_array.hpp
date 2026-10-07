/**
 *  @file src/helpers/linked_array.hpp
 *  @author Ash Vardanian
 *  @date January 11, 2023
 *  @brief Arrays and tapes that grow inside an arena and answer allocation failures as statuses.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <utility> // `std::exchange`, `std::move`, `std::swap`

#include "ustore/ranges.hpp" // `strided_range`, `trivial_insert`
#include "ustore/status.hpp" // `expected`, `status_t`
#include "ustore/types.hpp"  // `value_view_t`, `next_power_of_two`

#include "helpers/linked_memory.hpp" // `arena_allocate`, `arena_grow`

namespace unum::ustore {

/** A growable array in arena memory whose elements start uninitialized and move as bytes. */
template <typename element_type_>
class uninitialized_array {
    element_type_* data_ = nullptr;
    ustore_length_t length_ = 0;
    ustore_length_t capacity_ = 0;
    ustore_arena_t* arena_ = nullptr;

  public:
    uninitialized_array(ustore_arena_t* arena) noexcept : arena_(arena) {}

    uninitialized_array(uninitialized_array const&) = delete;
    uninitialized_array& operator=(uninitialized_array const&) = delete;

    uninitialized_array(uninitialized_array&& other) noexcept
        : data_(std::exchange(other.data_, nullptr)), length_(std::exchange(other.length_, 0)),
          capacity_(std::exchange(other.capacity_, 0)), arena_(std::exchange(other.arena_, nullptr)) {}

    uninitialized_array& operator=(uninitialized_array&& other) noexcept {
        std::swap(data_, other.data_);
        std::swap(length_, other.length_);
        std::swap(capacity_, other.capacity_);
        std::swap(arena_, other.arena_);
        return *this;
    }

    status_t reserve(std::size_t capacity) noexcept {
        if (capacity <= capacity_) return {};
        std::size_t const grown_capacity = next_power_of_two(capacity);
        auto [tape, status] = data_ ? arena_grow<element_type_>(arena_, {data_, capacity_}, grown_capacity - capacity_)
                                    : arena_allocate<element_type_>(arena_, grown_capacity);
        if (status.failed()) return status;
        data_ = tape.data();
        capacity_ = static_cast<ustore_length_t>(grown_capacity);
        return {};
    }

    status_t push_back(element_type_ value) noexcept {
        if (status_t const status = reserve(length_ + 1u); status.failed()) return status;
        data_[length_] = std::move(value);
        ++length_;
        return {};
    }

    status_t insert(std::size_t offset, element_type_ const* inserted_begin,
                    element_type_ const* inserted_end) noexcept {
        if (offset > length_) return {status_code_t::invalid_argument_k, "Insertion offset is out of bounds"};
        std::size_t const inserted_count = static_cast<std::size_t>(inserted_end - inserted_begin);
        if (status_t const status = reserve(length_ + inserted_count); status.failed()) return status;
        length_ = static_cast<ustore_length_t>(trivial_insert(data_, length_, offset, inserted_begin, inserted_end));
        return {};
    }

    element_type_ const* data() const noexcept { return data_; }
    element_type_* data() noexcept { return data_; }
    element_type_* begin() const noexcept { return data_; }
    element_type_* end() const noexcept { return data_ + length_; }
    element_type_& operator[](std::size_t index) noexcept { return data_[index]; }
    std::size_t size() const noexcept { return length_; }
    explicit operator bool() const noexcept { return length_; }
};

/** An append-only tape of values in an arena, with one offset per value and one more closing the last. */
class growing_tape_t {
    uninitialized_array<ustore_length_t> offsets_;
    uninitialized_array<byte_t> contents_;

  public:
    growing_tape_t(ustore_arena_t* arena) noexcept : offsets_(arena), contents_(arena) {}

    /** Appends a copy of @p value, where a missing value takes no bytes, and returns the view of that copy. */
    expected<value_view_t> push_back(value_view_t value) noexcept {
        if (!offsets_.size())
            if (status_t const status = offsets_.push_back(0); status.failed()) return status;
        if (status_t const status = contents_.insert(contents_.size(), value.begin(), value.end()); status.failed())
            return status;
        if (status_t const status = offsets_.push_back(static_cast<ustore_length_t>(contents_.size())); status.failed())
            return status;
        return value_view_t {contents_.data() + contents_.size() - value.size(), value.size()};
    }

    /** Appends @p terminator to the last value, which its closing offset then covers. */
    status_t add_terminator(byte_t terminator) noexcept {
        if (status_t const status = contents_.push_back(terminator); status.failed()) return status;
        offsets_[offsets_.size() - 1] += 1;
        return {};
    }

    status_t reserve(std::size_t capacity) noexcept { return offsets_.reserve(capacity + 1); }

    strided_range<ustore_length_t> offsets() noexcept { return strided(offsets_.begin(), offsets_.end()); }
    strided_range<byte_t> contents() noexcept { return strided(contents_.begin(), contents_.end()); }
};

} // namespace unum::ustore
