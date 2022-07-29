/**
 * @file utility.hpp
 * @author Ashot Vardanian
 * @date 4 Jul 2022
 *
 * @brief Smart Pointers, Monads and Range-like abstractions for C++ bindings.
 */

#pragma once
#include <functional> // `std::hash`

#include "ukv/ukv.h"

namespace unum::ukv {

using key_t = ukv_key_t;
using val_len_t = ukv_val_len_t;
using tape_ptr_t = ukv_val_ptr_t;
using size_t = ukv_size_t;

enum class byte_t : uint8_t {};

/**
 * @brief An OOP-friendly location representation for objects in the DB.
 * Should be used with `stride` set to `sizeof(col_key_t)`.
 */
struct col_key_t {

    ukv_collection_t collection = ukv_default_collection_k;
    ukv_key_t key = 0;

    col_key_t() = default;
    col_key_t(col_key_t const&) = default;
    col_key_t& operator=(col_key_t const&) = default;

    inline col_key_t(ukv_collection_t c, ukv_key_t k) noexcept : collection(c), key(k) {}
    inline col_key_t(ukv_key_t k) noexcept : key(k) {}
    inline col_key_t in(ukv_collection_t col) noexcept { return {col, key}; }

    inline bool operator==(col_key_t const& other) const noexcept {
        return (collection == other.collection) & (key == other.key);
    }
    inline bool operator!=(col_key_t const& other) const noexcept {
        return (collection != other.collection) | (key != other.key);
    }
    inline bool operator<(col_key_t const& other) const noexcept { return key < other.key; }
    inline bool operator>(col_key_t const& other) const noexcept { return key > other.key; }
    inline bool operator<=(col_key_t const& other) const noexcept { return key <= other.key; }
    inline bool operator>=(col_key_t const& other) const noexcept { return key >= other.key; }
};

struct col_key_field_t {
    ukv_collection_t collection = 0;
    ukv_key_t key = ukv_key_unknown_k;
    ukv_str_view_t field = nullptr;

    col_key_field_t() = default;

    col_key_field_t(ukv_key_t key) noexcept : collection(ukv_default_collection_k), key(key), field(nullptr) {}

    col_key_field_t(ukv_collection_t col, ukv_key_t key, ukv_str_view_t field = nullptr) noexcept
        : collection(col), key(key), field(field) {}

    col_key_field_t(ukv_key_t key, ukv_str_view_t field) noexcept
        : collection(ukv_default_collection_k), key(key), field(field) {}
};

template <typename... args_at>
inline col_key_field_t ckf(args_at&&... args) {
    return {std::forward<args_at>(args)...};
}

/**
 * @brief Graph Edge, or in DBMS terms - a relationship.
 */
struct edge_t {
    ukv_key_t source_id;
    ukv_key_t target_id;
    ukv_key_t id = ukv_default_edge_id_k;

    inline bool operator==(edge_t const& other) const noexcept {
        return (source_id == other.source_id) & (target_id == other.target_id) & (id == other.id);
    }
    inline bool operator!=(edge_t const& other) const noexcept {
        return (source_id != other.source_id) | (target_id != other.target_id) | (id != other.id);
    }
};

/**
 * @brief An asymmetric slice of a bond/relation.
 * Every vertex stores a list of such @c `neighborship_t`s
 * in a sorted order.
 */
struct neighborship_t {
    ukv_key_t neighbor_id = 0;
    ukv_key_t edge_id = 0;

    friend inline bool operator<(neighborship_t a, neighborship_t b) noexcept {
        return (a.neighbor_id < b.neighbor_id) | ((a.neighbor_id == b.neighbor_id) & (a.edge_id < b.edge_id));
    }
    friend inline bool operator==(neighborship_t a, neighborship_t b) noexcept {
        return (a.neighbor_id == b.neighbor_id) & (a.edge_id == b.edge_id);
    }
    friend inline bool operator!=(neighborship_t a, neighborship_t b) noexcept {
        return (a.neighbor_id != b.neighbor_id) | (a.edge_id != b.edge_id);
    }

    friend inline bool operator<(ukv_key_t a_vertex_id, neighborship_t b) noexcept {
        return a_vertex_id < b.neighbor_id;
    }
    friend inline bool operator<(neighborship_t a, ukv_key_t b_vertex_id) noexcept {
        return a.neighbor_id < b_vertex_id;
    }
    friend inline bool operator==(ukv_key_t a_vertex_id, neighborship_t b) noexcept {
        return a_vertex_id == b.neighbor_id;
    }
    friend inline bool operator==(neighborship_t a, ukv_key_t b_vertex_id) noexcept {
        return a.neighbor_id == b_vertex_id;
    }
};

#pragma region - Variable Length Views

/**
 * @brief Similar to `std::optional<std::string_view>`.
 * It's NULL state and "empty string" states are not identical.
 * The NULL state generally reflects missing values.
 * Unlike `indexed_range_gt<byte_t const*>`, this classes layout allows
 * easily passing it to the internals of UKV implementations
 * without additional bit-twiddling.
 */
class value_view_t {

    ukv_val_ptr_t ptr_ = nullptr;
    ukv_val_len_t length_ = 0;

  public:
    inline value_view_t() = default;
    inline value_view_t(ukv_val_ptr_t ptr, ukv_val_len_t length) noexcept {
        ptr_ = ptr;
        length_ = length;
    }

    inline value_view_t(byte_t const* begin, byte_t const* end) noexcept
        : ptr_(ukv_val_ptr_t(begin)), length_(static_cast<ukv_val_len_t>(end - begin)) {}

    inline value_view_t(char const* c_str) noexcept
        : ptr_(ukv_val_ptr_t(c_str)), length_(static_cast<ukv_val_len_t>(std::strlen(c_str))) {}

    inline byte_t const* begin() const noexcept { return reinterpret_cast<byte_t const*>(ptr_); }
    inline byte_t const* end() const noexcept { return begin() + size(); }
    inline char const* c_str() const noexcept { return reinterpret_cast<char const*>(ptr_); }
    inline std::size_t size() const noexcept { return length_ == ukv_val_len_missing_k ? 0 : length_; }
    inline bool empty() const noexcept { return !size(); }
    inline operator bool() const noexcept { return length_ != ukv_val_len_missing_k; }

    ukv_val_ptr_t const* member_ptr() const noexcept { return &ptr_; }
    ukv_val_len_t const* member_length() const noexcept { return &length_; }

    bool operator==(value_view_t other) const noexcept {
        return size() == other.size() && std::equal(begin(), end(), other.begin());
    }
    bool operator!=(value_view_t other) const noexcept {
        return size() != other.size() || !std::equal(begin(), end(), other.begin());
    }
};

#pragma region - Memory Management

/**
 * @brief A view of a tape received from the DB.
 * Allocates no memory, but is responsible for the cleanup.
 */
class arena_t {

    ukv_t db_ = nullptr;
    ukv_arena_t memory_ = nullptr;

  public:
    arena_t(ukv_t db) noexcept : db_(db) {}
    arena_t(arena_t const&) = delete;
    arena_t& operator=(arena_t const&) = delete;

    ~arena_t() {
        ukv_arena_free(db_, memory_);
        memory_ = nullptr;
    }

    inline arena_t(arena_t&& other) noexcept : db_(other.db_), memory_(std::exchange(other.memory_, nullptr)) {}

    inline arena_t& operator=(arena_t&& other) noexcept {
        std::swap(db_, other.db_);
        std::swap(memory_, other.memory_);
        return *this;
    }

    inline ukv_arena_t* member_ptr() noexcept { return &memory_; }
    inline ukv_t db() const noexcept { return db_; }
};

class any_arena_t {

    arena_t owned_;
    arena_t* accessible_ = nullptr;

  public:
    any_arena_t(ukv_t db) noexcept : owned_(db), accessible_(nullptr) {}
    any_arena_t(arena_t& accessible) noexcept : owned_(nullptr), accessible_(&accessible) {}

    any_arena_t(any_arena_t&&) = default;
    any_arena_t& operator=(any_arena_t&&) = default;

    any_arena_t(any_arena_t const&) = delete;
    any_arena_t& operator=(any_arena_t const&) = delete;

    arena_t& arena() noexcept { return accessible_ ? *accessible_ : owned_; }
    ukv_arena_t* member_ptr() noexcept { return arena().member_ptr(); }
    arena_t release_owned() noexcept { return std::exchange(owned_, arena_t {owned_.db()}); }
};

#pragma region - Adapters

/**
 * @brief Trivial hash-mixing scheme from Boost.
 * @see https://www.boost.org/doc/libs/1_37_0/doc/html/hash/reference.html#boost.hash_combine
 */
template <typename hashable_at>
inline void hash_combine(std::size_t& seed, hashable_at const& v) {
    std::hash<hashable_at> hasher;
    seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

struct sub_key_hash_t {
    inline std::size_t operator()(col_key_t const& sub) const noexcept {
        std::size_t result = SIZE_MAX;
        hash_combine(result, sub.key);
        hash_combine(result, sub.collection);
        return result;
    }
};

struct edge_hash_t {
    inline std::size_t operator()(edge_t const& edge) const noexcept {
        std::size_t result = SIZE_MAX;
        hash_combine(result, edge.source_id);
        hash_combine(result, edge.target_id);
        hash_combine(result, edge.id);
        return result;
    }
};

inline ukv_vertex_role_t invert(ukv_vertex_role_t role) {
    switch (role) {
    case ukv_vertex_source_k: return ukv_vertex_target_k;
    case ukv_vertex_target_k: return ukv_vertex_source_k;
    case ukv_vertex_role_any_k: return ukv_vertex_role_unknown_k;
    case ukv_vertex_role_unknown_k: return ukv_vertex_role_any_k;
    }
    __builtin_unreachable();
}

} // namespace unum::ukv
