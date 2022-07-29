/**
 * @file helpers.htpp
 * @author Ashot Vardanian
 *
 * @brief Helper functions for the C++ backend implementations.
 */
#pragma once
#include <memory>    // `std::allocator`
#include <cstring>   // `std::memcpy`
#include <stdexcept> // `std::runtime_error`
#include <algorithm> // `std::sort`
#include <limits.h>  // `CHAR_BIT`

#include "ukv/ukv.hpp"

namespace unum::ukv {

using allocator_t = std::allocator<byte_t>;
using buffer_t = std::vector<byte_t, allocator_t>;
using generation_t = std::int64_t;

inline std::size_t next_power_of_two(std::size_t x) {
    return 1ull << (sizeof(std::size_t) * CHAR_BIT - __builtin_clzll(x));
}

/**
 * @brief An `std::vector`-like class, with open layout,
 * friendly to our C API. Can't grow, but can shrink.
 * Just like humans after puberty :)
 */
class value_t {
    ukv_val_ptr_t ptr_ = nullptr;
    ukv_val_len_t length_ = 0;
    ukv_val_len_t cap_ = 0;

  public:
    value_t() noexcept = default;
    value_t(value_t const&) = delete;
    value_t& operator=(value_t const&) = delete;

    value_t(value_t&& v) noexcept
        : ptr_(std::exchange(v.ptr_, nullptr)), length_(std::exchange(v.length_, 0)), cap_(std::exchange(v.cap_, 0)) {}

    value_t& operator=(value_t&& v) noexcept {
        std::swap(v.ptr_, ptr_);
        std::swap(v.length_, length_);
        std::swap(v.cap_, cap_);
        return *this;
    }

    value_t(std::size_t size) {
        if (!size)
            return;
        auto new_ptr = allocator_t {}.allocate(size);
        if (!new_ptr)
            throw std::bad_alloc();
        ptr_ = reinterpret_cast<ukv_val_ptr_t>(new_ptr);
        cap_ = length_ = size;
    }

    value_t(value_view_t view) : value_t(view.size()) { std::memcpy(ptr_, view.begin(), view.size()); }
    ~value_t() { reset(); }

    void reset() {
        if (ptr_)
            allocator_t {}.deallocate(reinterpret_cast<byte_t*>(ptr_), cap_);
        ptr_ = nullptr;
        length_ = 0;
        cap_ = 0;
    }

    void resize(std::size_t size) {
        if (size > cap_)
            throw std::invalid_argument("Only shrinking is currently supported");
        length_ = size;
    }

    void push_back(byte_t byte) {
        auto new_size = length_ + 1;
        if (new_size > cap_) {
            auto new_cap = next_power_of_two(new_size);
            auto new_ptr = allocator_t {}.allocate(new_cap);
            if (!new_ptr)
                throw std::bad_alloc();
            std::memcpy(new_ptr, ptr_, length_);
            if (ptr_)
                allocator_t {}.deallocate(reinterpret_cast<byte_t*>(ptr_), cap_);

            ptr_ = reinterpret_cast<ukv_val_ptr_t>(new_ptr);
            cap_ = new_cap;
        }

        reinterpret_cast<byte_t*>(ptr_)[length_] = byte;
        length_ = new_size;
    }

    void insert(std::size_t offset, byte_t const* inserted_begin, byte_t const* inserted_end) {
        if (offset > size())
            throw std::out_of_range("Can't insert");

        auto inserted_len = static_cast<ukv_val_len_t>(inserted_end - inserted_begin);
        auto following_len = static_cast<ukv_val_len_t>(length_ - offset);
        auto new_size = length_ + inserted_len;
        if (new_size > cap_) {
            auto new_ptr = allocator_t {}.allocate(new_size);
            if (!new_ptr)
                throw std::bad_alloc();
            std::memcpy(new_ptr, ptr_, offset);
            std::memcpy(new_ptr + offset, inserted_begin, inserted_len);
            std::memcpy(new_ptr + offset + inserted_len, ptr_ + offset, following_len);
            if (ptr_)
                allocator_t {}.deallocate(reinterpret_cast<byte_t*>(ptr_), cap_);

            ptr_ = reinterpret_cast<ukv_val_ptr_t>(new_ptr);
            cap_ = length_ = new_size;
        }
        else {
            std::memmove(ptr_ + offset + inserted_len, ptr_ + offset, following_len);
            std::memcpy(ptr_ + offset, inserted_begin, inserted_len);
            length_ = new_size;
        }
    }

    void erase(std::size_t offset, std::size_t length) {
        if (offset + length > size())
            throw std::out_of_range("Can't erase");

        std::memmove(ptr_ + offset, ptr_ + offset + length, length_ - (offset + length));
        length_ -= length;
    }

    inline byte_t* begin() const noexcept { return reinterpret_cast<byte_t*>(ptr_); }
    inline byte_t* end() const noexcept { return begin() + length_; }
    inline std::size_t size() const noexcept { return length_; }
    inline operator bool() const noexcept { return length_; }
    inline operator value_view_t() const noexcept { return {ptr_, length_}; }
    inline void clear() noexcept { length_ = 0; }

    inline ukv_val_ptr_t* member_ptr() noexcept { return &ptr_; }
    inline ukv_val_len_t* member_length() noexcept { return &length_; }
    inline ukv_val_len_t* member_cap() noexcept { return &cap_; }
};

/**
 * @brief Append-only data-structure for variable length blobs.
 * Owns the underlying arena and is external to the underlying DB.
 * Is suited for data preparation before passing to the C API.
 */
class growing_tape_t {
    std::vector<ukv_val_len_t> offsets_;
    std::vector<ukv_val_len_t> lengths_;
    std::vector<byte_t> contents_;

  public:
    void push_back(value_view_t value) {
        offsets_.push_back(static_cast<ukv_val_len_t>(contents_.size()));
        lengths_.push_back(static_cast<ukv_val_len_t>(value.size()));
        contents_.insert(contents_.end(), value.begin(), value.end());
    }

    void clear() {
        offsets_.clear();
        lengths_.clear();
        contents_.clear();
    }

    strided_range_gt<ukv_val_len_t> offsets() noexcept { return offsets_; }
    strided_range_gt<ukv_val_len_t> lengths() noexcept { return lengths_; }
    strided_range_gt<byte_t> contents() noexcept { return contents_; }

    operator taped_values_view_t() noexcept {
        return {lengths_.data(), ukv_val_ptr_t(contents_.data()), contents_.size()};
    }
};

struct stl_arena_t {
    std::vector<byte_t> output_tape;
    std::vector<byte_t> unpacked_tape;
    growing_tape_t growing_tape;
    /**
     * In complex multi-step operations we need arrays
     * of `col_key_t` to sort/navigate them more easily.
     */
    std::vector<col_key_t> updated_keys;
    /**
     * In complex multi-step operations we need disjoint arrays
     * variable-length buffers to avoid expensive `memmove`s in
     * big batch operations.
     */
    std::vector<value_t> updated_vals;
};

inline stl_arena_t* cast_arena(ukv_arena_t* c_arena, ukv_error_t* c_error) noexcept {
    try {
        if (!*c_arena)
            *c_arena = new stl_arena_t;
        return *reinterpret_cast<stl_arena_t**>(c_arena);
    }
    catch (...) {
        *c_error = "Failed to allocate memory!";
        return nullptr;
    }
}

template <typename element_at>
inline element_at* prepare_memory(std::vector<element_at>& elems, std::size_t n, ukv_error_t* c_error) noexcept {
    try {
        elems.resize(n);
        return elems.data();
    }
    catch (...) {
        *c_error = "Failed to allocate memory!";
        return nullptr;
    }
}

/**
 * @brief Solves the problem of modulo arithmetic and `generation_t` overflow.
 * Still works correctly, when `max` has overflown, but `min` hasn't yet,
 * so `min` can be bigger than `max`.
 */
inline bool entry_was_overwritten(generation_t entry_generation,
                                  generation_t transaction_generation,
                                  generation_t youngest_generation) noexcept {

    return transaction_generation <= youngest_generation
               ? ((entry_generation >= transaction_generation) & (entry_generation <= youngest_generation))
               : ((entry_generation >= transaction_generation) | (entry_generation <= youngest_generation));
}

struct read_task_t {
    ukv_collection_t col;
    ukv_key_t const& key;

    inline col_key_t location() const noexcept { return col_key_t {col, key}; }
};

/**
 * @brief Arguments of `ukv_read` aggregated into a Structure-of-Arrays.
 * Is used to validate various combinations of arguments, strides, NULLs, etc.
 */
struct read_tasks_soa_t {
    strided_iterator_gt<ukv_collection_t const> cols;
    strided_iterator_gt<ukv_key_t const> keys;
    ukv_size_t count = 0;

    inline std::size_t size() const noexcept { return count; }

    inline read_task_t operator[](ukv_size_t i) const noexcept {
        ukv_collection_t col = cols && cols[i] ? cols[i] : ukv_default_collection_k;
        ukv_key_t const& key = keys[i];
        return {col, key};
    }
};

struct scan_task_t {
    ukv_collection_t col;
    ukv_key_t const& min_key;
    ukv_size_t length;

    inline col_key_t location() const noexcept { return col_key_t {col, min_key}; }
};

/**
 * @brief Arguments of `ukv_scan` aggregated into a Structure-of-Arrays.
 * Is used to validate various combinations of arguments, strides, NULLs, etc.
 */
struct scan_tasks_soa_t {
    strided_iterator_gt<ukv_collection_t const> cols;
    strided_iterator_gt<ukv_key_t const> min_keys;
    strided_iterator_gt<ukv_size_t const> lengths;
    ukv_size_t count = 0;

    inline std::size_t size() const noexcept { return count; }

    inline scan_task_t operator[](ukv_size_t i) const noexcept {
        ukv_collection_t col = cols && cols[i] ? cols[i] : ukv_default_collection_k;
        ukv_key_t const& key = min_keys[i];
        ukv_size_t len = lengths[i];
        return {col, key, len};
    }
};

struct write_task_t {
    ukv_collection_t col;
    ukv_key_t const& key;
    byte_t const* begin;
    ukv_val_len_t offset;
    ukv_val_len_t length;

    inline col_key_t location() const noexcept { return col_key_t {col, key}; }
    inline bool is_deleted() const noexcept { return begin == nullptr; }
    value_view_t view() const noexcept { return {begin + offset, begin + offset + length}; }
    buffer_t buffer() const { return {begin + offset, begin + offset + length}; }
};

/**
 * @brief Arguments of `ukv_write` aggregated into a Structure-of-Arrays.
 * Is used to validate various combinations of arguments, strides, NULLs, etc.
 */
struct write_tasks_soa_t {
    strided_iterator_gt<ukv_collection_t const> cols;
    strided_iterator_gt<ukv_key_t const> keys;
    strided_iterator_gt<ukv_val_ptr_t const> vals;
    strided_iterator_gt<ukv_val_len_t const> offs;
    strided_iterator_gt<ukv_val_len_t const> lens;
    ukv_size_t count = 0;

    inline std::size_t size() const noexcept { return count; }

    inline write_task_t operator[](ukv_size_t i) const noexcept {
        ukv_collection_t col = cols && cols[i] ? cols[i] : ukv_default_collection_k;
        ukv_key_t const& key = keys[i];
        byte_t const* begin;
        ukv_val_len_t off;
        ukv_val_len_t len;
        if (vals) {
            begin = reinterpret_cast<byte_t const*>(vals[i]);
            off = offs ? offs[i] : 0u;
            len = lens ? lens[i] : std::strlen(reinterpret_cast<char const*>(begin + off));
        }
        else {
            begin = nullptr;
            off = 0u;
            len = 0u;
        }
        return {col, key, begin, off, len};
    }
};

class file_handle_t {
    std::FILE* handle_ = nullptr;

  public:
    status_t open(char const* path, char const* mode) {
        if (handle_)
            return "Close previous file before opening the new one!";
        handle_ = std::fopen(path, mode);
        if (!handle_)
            return "Failed to open a file";
        return {};
    }

    status_t close() {
        if (!handle_)
            return {};
        if (std::fclose(handle_) == EOF)
            return "Couldn't close the file after write.";
        else
            handle_ = nullptr;
        return {};
    }

    ~file_handle_t() {
        if (handle_)
            std::fclose(handle_);
    }

    operator std::FILE*() const noexcept { return handle_; }
};

template <typename range_at, typename comparable_at>
inline range_at equal_subrange(range_at range, comparable_at&& comparable) {
    auto p = std::equal_range(range.begin(), range.end(), comparable);
    return range_at {p.first, p.second};
}

template <typename element_at>
void sort_and_deduplicate(std::vector<element_at>& elems) {
    std::sort(elems.begin(), elems.end());
    elems.erase(std::unique(elems.begin(), elems.end()), elems.end());
}

template <typename element_at>
std::size_t offset_in_sorted(std::vector<element_at> const& elems, element_at const& wanted) {
    return std::lower_bound(elems.begin(), elems.end(), wanted) - elems.begin();
}

template <typename element_at>
void inplace_inclusive_prefix_sum(element_at* begin, element_at* const end) {
    element_at sum = 0;
    for (; begin != end; ++begin)
        sum += std::exchange(*begin, *begin + sum);
}

} // namespace unum::ukv
