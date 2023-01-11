/**
 * @file helpers/linked_array.hpp
 * @author Ashot Vardanian
 *
 * @brief Replacing `std::vector` with non-throwing alternatives.
 */
#pragma once
#include "linked_memory.hpp" // `linked_memory_lock_t`

namespace unum::ukv {

/**
 * @brief An `std::vector`-like class, with open layout,
 * friendly to our C API. Internal elements aren't initialized
 * and must be trivially copy-constructible.
 */
template <typename element_at>
class uninitialized_array_gt {
    using element_t = element_at;
    using elementc_t = element_t const;
    using ptrc_t = elementc_t*;
    using ptr_t = element_t*;

  private:
    ptr_t ptr_ = nullptr;
    ukv_length_t length_ = 0;
    ukv_length_t cap_ = 0;
    linked_memory_lock_t* arena_ptr_ = nullptr;

  public:
    uninitialized_array_gt(uninitialized_array_gt const&) = delete;
    uninitialized_array_gt& operator=(uninitialized_array_gt const&) = delete;

    uninitialized_array_gt(uninitialized_array_gt&& v) noexcept
        : ptr_(std::exchange(v.ptr_, nullptr)), length_(std::exchange(v.length_, 0)), cap_(std::exchange(v.cap_, 0)),
          arena_ptr_(std::exchange(v.arena_ptr_, nullptr)) {}

    uninitialized_array_gt& operator=(uninitialized_array_gt&& v) noexcept {
        std::swap(v.ptr_, ptr_);
        std::swap(v.length_, length_);
        std::swap(v.cap_, cap_);
        std::swap(v.arena_ptr_, arena_ptr_);
        return *this;
    }
    uninitialized_array_gt(linked_memory_lock_t& arena) : arena_ptr_(&arena) {}
    uninitialized_array_gt(std::size_t size, linked_memory_lock_t& arena, ukv_error_t* c_error) : arena_ptr_(&arena) {
        if (!size)
            return;
        auto tape = arena_ptr_->alloc<element_t>(size, c_error);
        ptr_ = tape.begin();
        cap_ = length_ = static_cast<ukv_length_t>(size);
    }

    uninitialized_array_gt(value_view_t view) : uninitialized_array_gt(view.size()) {
        std::memcpy(ptr_, view.begin(), view.size());
    }
    ~uninitialized_array_gt() { reset(); }

    void reset() {
        ptr_ = nullptr;
        length_ = 0;
        cap_ = 0;
    }

    void resize(std::size_t size, ukv_error_t* c_error) {
        if (size == length_)
            return;
        if (size <= cap_) {
            length_ = static_cast<ukv_length_t>(size);
            return;
        }

        auto new_cap = next_power_of_two(size);
        auto tape = ptr_ ? arena_ptr_->grow<element_t>({ptr_, cap_}, new_cap - cap_, c_error)
                         : arena_ptr_->alloc<element_t>(new_cap, c_error);
        return_if_error_m(c_error);

        ptr_ = tape.begin();
        cap_ = static_cast<ukv_length_t>(new_cap);
        length_ = static_cast<ukv_length_t>(size);
    }

    void reserve(std::size_t new_cap, ukv_error_t* c_error) {
        if (new_cap <= cap_)
            return;
        new_cap = next_power_of_two(new_cap);
        auto tape = ptr_ ? arena_ptr_->grow<element_t>({ptr_, cap_}, new_cap - cap_, c_error)
                         : arena_ptr_->alloc<element_t>(new_cap, c_error);
        return_if_error_m(c_error);

        ptr_ = tape.begin();
        cap_ = static_cast<ukv_length_t>(new_cap);
    }

    void push_back(element_t val, ukv_error_t* c_error) {
        auto new_size = length_ + 1;
        reserve(new_size, c_error);
        return_if_error_m(c_error);

        ptr_[length_] = std::move(val);
        length_ = new_size;
    }

    void insert(std::size_t offset, ptrc_t inserted_begin, ptrc_t inserted_end, ukv_error_t* c_error) {
        return_error_if_m(size() >= offset, c_error, out_of_range_k, "Can't insert");

        auto inserted_len = static_cast<ukv_length_t>(inserted_end - inserted_begin);
        auto following_len = static_cast<ukv_length_t>(length_ - offset);
        auto new_size = length_ + inserted_len;

        if (new_size > cap_) {
            auto tape = arena_ptr_->grow<element_t>({ptr_, cap_}, new_size - cap_, c_error);
            return_if_error_m(c_error);
            ptr_ = tape.begin();
            cap_ = length_ = new_size;
        }
        else
            length_ = new_size;

        std::memmove(ptr_ + offset + inserted_len, ptr_ + offset, following_len * sizeof(element_t));
        std::memcpy(ptr_ + offset, inserted_begin, inserted_len * sizeof(element_t));
    }

    void erase(std::size_t offset, std::size_t length, ukv_error_t* c_error) {
        return_error_if_m(size() >= offset + length, c_error, out_of_range_k, "Can't erase");

        auto following_len = length_ - (offset + length);
        std::memmove(ptr_ + offset, ptr_ + offset + length, following_len * sizeof(element_t));
        length_ -= length;
    }

    inline ptrc_t data() const noexcept { return ptr_; }
    inline ptr_t data() noexcept { return ptr_; }
    inline ptr_t begin() const noexcept { return reinterpret_cast<ptr_t>(ptr_); }
    inline ptr_t end() const noexcept { return begin() + length_; }
    inline element_t& operator[](std::size_t i) noexcept { return ptr_[i]; }
    inline std::size_t size() const noexcept { return length_; }
    inline explicit operator bool() const noexcept { return length_; }
    inline void clear() noexcept { length_ = 0; }

    inline ptr_t* member_ptr() noexcept { return &ptr_; }
    inline ukv_length_t* member_length() noexcept { return &length_; }
    inline ukv_length_t* member_cap() noexcept { return &cap_; }
};

template <typename element_at>
class initialized_range_gt {
    using element_t = element_at;
    using vector_t = uninitialized_array_gt<element_t>;
    vector_t const& owner_;

  public:
    static_assert(std::is_nothrow_constructible<element_t>());

    initialized_range_gt(vector_t const& owner) noexcept : owner_(owner) {
        std::uninitialized_default_construct(owner_.begin(), owner_.end());
    }

    ~initialized_range_gt() noexcept { std::destroy(owner_.begin(), owner_.end()); }
};

/**
 * @brief Append-only data-structure for variable length blobs.
 * Owns the underlying arena and is external to the underlying DB.
 * Is suited for data preparation before passing to the C API.
 */
class growing_tape_t {
    uninitialized_array_gt<ukv_octet_t> presences_;
    uninitialized_array_gt<ukv_length_t> offsets_;
    uninitialized_array_gt<ukv_length_t> lengths_;
    uninitialized_array_gt<byte_t> contents_;

  public:
    growing_tape_t(linked_memory_lock_t& arena)
        : presences_(arena), offsets_(arena), lengths_(arena), contents_(arena) {}

    /**
     * @return Memory region occupied by the new copy.
     */
    value_view_t push_back(value_view_t value, ukv_error_t* c_error) {
        auto offset = static_cast<ukv_length_t>(contents_.size());
        auto length = static_cast<ukv_length_t>(value.size());
        auto old_count = lengths_.size();

        lengths_.push_back(value ? length : ukv_length_missing_k, c_error);

        presences_.resize(divide_round_up(old_count + 1, bits_in_byte_k), c_error);
        if (*c_error)
            return value_view_t {};
        presences()[old_count] = bool(value);

        // We need to store one more offset for Apache Arrow.
        offsets_.resize(lengths_.size() + 1, c_error);
        if (*c_error)
            return value_view_t {};
        offsets_[old_count] = offset;
        offsets_[old_count + 1] = offset + length;

        contents_.insert(contents_.size(), value.begin(), value.end(), c_error);
        if (*c_error)
            return value_view_t {};

        return value_view_t {contents_.data() + contents_.size() - value.size(), value.size()};
    }

    void add_terminator(byte_t terminator, ukv_error_t* c_error) {
        contents_.push_back(terminator, c_error);
        return_if_error_m(c_error);
        offsets_[lengths_.size()] += 1;
    }

    void reserve(size_t new_cap, ukv_error_t* c_error) {
        presences_.reserve(divide_round_up(new_cap, bits_in_byte_k), c_error);
        offsets_.reserve(new_cap + 1, c_error);
        lengths_.reserve(new_cap, c_error);
    }

    void clear() {
        presences_.clear();
        offsets_.clear();
        lengths_.clear();
        contents_.clear();
    }

    bits_span_t presences() noexcept { return bits_span_t(presences_.begin()); }
    strided_range_gt<ukv_length_t> offsets() noexcept {
        return strided_range<ukv_length_t>(offsets_.begin(), offsets_.end());
    }
    strided_range_gt<ukv_length_t> lengths() noexcept {
        return strided_range<ukv_length_t>(lengths_.begin(), lengths_.end());
    }
    strided_range_gt<byte_t> contents() noexcept { return strided_range<byte_t>(contents_.begin(), contents_.end()); }

    operator joined_blobs_t() noexcept { return {lengths_.size(), offsets_.data(), ukv_bytes_ptr_t(contents_.data())}; }

    operator embedded_blobs_t() noexcept {
        return {lengths_.size(), offsets_.data(), lengths_.data(), ukv_bytes_ptr_t(contents_.data())};
    }
};

} // namespace unum::ukv
