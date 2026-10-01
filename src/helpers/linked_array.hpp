/**
 *  @file src/helpers/linked_array.hpp
 *  @author Ash Vardanian
 *  @date January 11, 2023
 *  @brief Arrays and tapes that grow inside an arena and report allocation failures through the C ABI.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <memory>      // `std::uninitialized_default_construct`, `std::destroy`
#include <span>        // `std::span`
#include <type_traits> // `std::is_nothrow_constructible_v`
#include <utility>     // `std::exchange`, `std::move`, `std::swap`

#include "ustore/ranges.hpp" // `strided_range`, `trivial_insert`, `trivial_erase`
#include "ustore/types.hpp"  // `value_view_t`, `next_power_of_two`

#include "helpers/linked_memory.hpp" // `linked_memory_lock_t`

namespace unum::ustore {

/** A growable array in arena memory whose elements start uninitialized and move as bytes. */
template <typename element_>
class uninitialized_array {
    element_* data_ = nullptr;
    ustore_length_t length_ = 0;
    ustore_length_t capacity_ = 0;
    linked_memory_lock_t* arena_ = nullptr;

  public:
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

    uninitialized_array(linked_memory_lock_t& arena) noexcept : arena_(&arena) {}

    uninitialized_array(std::size_t size, linked_memory_lock_t& arena, ustore_error_t* error) noexcept
        : arena_(&arena) {
        if (!size) return;
        std::span<element_> const tape = arena_->alloc<element_>(size, error);
        if (error->status) return;
        data_ = tape.data();
        capacity_ = length_ = static_cast<ustore_length_t>(size);
    }

    /** Forgets the elements without freeing them, since their memory belongs to the arena. */
    void reset() noexcept {
        data_ = nullptr;
        length_ = 0;
        capacity_ = 0;
    }

    void resize(std::size_t size, ustore_error_t* error) noexcept {
        reserve(size, error);
        if (error->status) return;
        length_ = static_cast<ustore_length_t>(size);
    }

    void reserve(std::size_t capacity, ustore_error_t* error) noexcept {
        if (capacity <= capacity_) return;
        std::size_t const grown_capacity = next_power_of_two(capacity);
        std::span<element_> const tape = data_ ? arena_->grow<element_>({data_, capacity_}, grown_capacity - capacity_,
                                                                        error)
                                               : arena_->alloc<element_>(grown_capacity, error);
        if (error->status) return;
        data_ = tape.data();
        capacity_ = static_cast<ustore_length_t>(grown_capacity);
    }

    void push_back(element_ value, ustore_error_t* error) noexcept {
        reserve(length_ + 1u, error);
        if (error->status) return;
        data_[length_] = std::move(value);
        ++length_;
    }

    void insert(std::size_t offset, element_ const* inserted_begin, element_ const* inserted_end,
                ustore_error_t* error) noexcept {
        if (offset > length_) {
            *error = {ustore_error_invalid_argument_k, "Insertion offset is out of bounds"};
            return;
        }
        reserve(length_ + static_cast<std::size_t>(inserted_end - inserted_begin), error);
        if (error->status) return;
        length_ = static_cast<ustore_length_t>(trivial_insert(data_, length_, offset, inserted_begin, inserted_end));
    }

    void erase(std::size_t offset, std::size_t count, ustore_error_t* error) noexcept {
        if (offset + count > length_) {
            *error = {ustore_error_invalid_argument_k, "Erased range is out of bounds"};
            return;
        }
        length_ = static_cast<ustore_length_t>(trivial_erase(data_, length_, offset, count));
    }

    element_ const* data() const noexcept { return data_; }
    element_* data() noexcept { return data_; }
    element_* begin() const noexcept { return data_; }
    element_* end() const noexcept { return data_ + length_; }
    element_& operator[](std::size_t index) noexcept { return data_[index]; }
    std::size_t size() const noexcept { return length_; }
    explicit operator bool() const noexcept { return length_; }
    void clear() noexcept { length_ = 0; }
};

/** Default-constructs the elements of an array for the lifetime of this guard, and destroys them after. */
template <typename element_>
class initialized_range {
    uninitialized_array<element_> const& owner_;

  public:
    explicit initialized_range(uninitialized_array<element_> const& owner) noexcept : owner_(owner) {
        static_assert(std::is_nothrow_constructible_v<element_>);
        std::uninitialized_default_construct(owner_.begin(), owner_.end());
    }

    initialized_range(initialized_range const&) = delete;
    initialized_range& operator=(initialized_range const&) = delete;

    ~initialized_range() noexcept { std::destroy(owner_.begin(), owner_.end()); }
};

/** An append-only tape of variable-length values that prepares arguments for the C ABI inside an arena. */
class growing_tape_t {
    uninitialized_array<ustore_octet_t> presences_;
    uninitialized_array<ustore_length_t> offsets_;
    uninitialized_array<ustore_length_t> lengths_;
    uninitialized_array<byte_t> contents_;

  public:
    growing_tape_t(linked_memory_lock_t& arena) noexcept
        : presences_(arena), offsets_(arena), lengths_(arena), contents_(arena) {}

    /** Appends a copy of @p value and returns the view of that copy. */
    value_view_t push_back(value_view_t value, ustore_error_t* error) noexcept {
        auto const offset = static_cast<ustore_length_t>(contents_.size());
        auto const length = static_cast<ustore_length_t>(value.size());
        std::size_t const old_count = lengths_.size();

        lengths_.push_back(value ? length : ustore_length_missing_k, error);
        if (error->status) return {};

        presences_.resize(divide_round_up<std::size_t>(old_count + 1, bits_in_byte_k), error);
        if (error->status) return {};
        presences()[old_count] = bool(value);

        // One offset more than values, so the length of the last value follows from the offsets alone
        offsets_.resize(lengths_.size() + 1, error);
        if (error->status) return {};
        offsets_[old_count] = offset;
        offsets_[old_count + 1] = offset + length;

        contents_.insert(contents_.size(), value.begin(), value.end(), error);
        if (error->status) return {};

        return value_view_t {contents_.data() + contents_.size() - value.size(), value.size()};
    }

    /** Appends @p terminator to the last value without counting it in the value's length. */
    void add_terminator(byte_t terminator, ustore_error_t* error) noexcept {
        contents_.push_back(terminator, error);
        if (error->status) return;
        offsets_[lengths_.size()] += 1;
    }

    void reserve(std::size_t capacity, ustore_error_t* error) noexcept {
        presences_.reserve(divide_round_up<std::size_t>(capacity, bits_in_byte_k), error);
        if (error->status) return;
        offsets_.reserve(capacity + 1, error);
        if (error->status) return;
        lengths_.reserve(capacity, error);
    }

    void clear() noexcept {
        presences_.clear();
        offsets_.clear();
        lengths_.clear();
        contents_.clear();
    }

    bits_span_t presences() noexcept { return bits_span_t(presences_.begin()); }
    strided_range<ustore_length_t> offsets() noexcept { return strided(offsets_.begin(), offsets_.end()); }
    strided_range<ustore_length_t> lengths() noexcept { return strided(lengths_.begin(), lengths_.end()); }
    strided_range<byte_t> contents() noexcept { return strided(contents_.begin(), contents_.end()); }

    operator joined_blobs_t() noexcept {
        return {lengths_.size(), offsets_.data(), ustore_bytes_ptr_t(contents_.data())};
    }

    operator embedded_blobs_t() noexcept {
        return {lengths_.size(), offsets_.data(), lengths_.data(), ustore_bytes_ptr_t(contents_.data())};
    }
};

} // namespace unum::ustore
