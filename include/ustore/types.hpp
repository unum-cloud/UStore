/**
 *  @file include/ustore/types.hpp
 *  @author Ash Vardanian
 *  @date July 4, 2022
 *  @brief Locations, edges, value views and arenas shared by every C++ wrapper.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `SIZE_MAX`, `std::uint8_t`
#include <cstring> // `std::strlen`

#include <algorithm>   // `std::ranges::equal`
#include <bit>         // `std::bit_floor`
#include <compare>     // `std::strong_ordering`
#include <concepts>    // `std::same_as`, `std::integral`
#include <functional>  // `std::hash`
#include <ranges>      // `std::ranges::contiguous_range`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::remove_cvref_t`, `std::is_same_v`
#include <utility>     // `std::exchange`, `std::swap`

#include <ustore/ustore.h>

namespace ashvardanian::forkunion {}
namespace ashvardanian::numkong {}
namespace ashvardanian::smashtable {}
namespace ashvardanian::stringzilla {}

namespace fu = ashvardanian::forkunion;
namespace nk = ashvardanian::numkong;
namespace st = ashvardanian::smashtable;
namespace sz = ashvardanian::stringzilla;

namespace unum::ustore {

/** A single byte of a stored value, kept distinct from characters and integers. */
enum class byte_t : std::uint8_t {};

#pragma region Access Modes

/** Whether a read inside a transaction registers its keys for conflict detection. */
enum class watch_t : bool { dont_watch_k, watch_k };

/** Whether a write returns only after its data reaches the disk. */
enum class flush_t : bool { dont_flush_k, flush_k };

/** Maps a watch mode onto the options of the C ABI. */
constexpr ustore_options_t to_options(watch_t watch) noexcept {
    return watch == watch_t::dont_watch_k ? ustore_option_transaction_dont_watch_k : ustore_options_default_k;
}

/** Maps a flush mode onto the options of the C ABI. */
constexpr ustore_options_t to_options(flush_t flush) noexcept {
    return flush == flush_t::flush_k ? ustore_option_write_flush_k : ustore_options_default_k;
}

#pragma endregion Access Modes

#pragma region Locations

/** A key inside a collection, laid out so a strided pointer can address either member. */
struct collection_key_t {

    /** Collection owning the key. */
    ustore_collection_t collection {ustore_collection_main_k};

    /** Key inside the collection. */
    ustore_key_t key {0};

    collection_key_t() = default;
    explicit collection_key_t(ustore_key_t key) noexcept : key(key) {}
    collection_key_t(ustore_collection_t collection, ustore_key_t key) noexcept : collection(collection), key(key) {}

    /** The same key addressed inside @p other_collection. */
    collection_key_t in(ustore_collection_t other_collection) const noexcept { return {other_collection, key}; }

    friend bool operator==(collection_key_t const&, collection_key_t const&) = default;
    friend std::strong_ordering operator<=>(collection_key_t const&, collection_key_t const&) = default;
};

/** A key inside a collection, optionally narrowed down to one field of its document. */
struct collection_key_field_t {

    /** Collection owning the key. */
    ustore_collection_t collection {0};

    /** Key inside the collection. */
    ustore_key_t key {ustore_key_unknown_k};

    /** Path of the document field, or @c nullptr for the whole value. */
    ustore_str_view_t field {nullptr};

    collection_key_field_t() = default;
    collection_key_field_t(ustore_collection_t collection, ustore_key_t key, ustore_str_view_t field = nullptr) noexcept
        : collection(collection), key(key), field(field) {}
    collection_key_field_t(ustore_key_t key) noexcept
        : collection(ustore_collection_main_k), key(key), field(nullptr) {}
    explicit collection_key_field_t(ustore_key_t key, ustore_str_view_t field) noexcept
        : collection(ustore_collection_main_k), key(key), field(field) {}
};

/** A location that names the collection it lives in. */
template <typename location_>
concept has_collection_concept = requires(location_ const& location) { location.collection; };

/** A location that narrows its value down to one document field. */
template <typename location_>
concept has_field_concept = requires(location_ const& location) { location.field; };

/** A single key, bare or with its collection, that widens into a @c collection_key_field_t. */
template <typename location_>
concept key_location_concept = std::same_as<std::remove_cvref_t<location_>, int> ||
                               std::same_as<std::remove_cvref_t<location_>, ustore_key_t> ||
                               std::same_as<std::remove_cvref_t<location_>, collection_key_t>;

#pragma endregion Locations

#pragma region Graph Elements

/** A graph edge, or a relationship in DBMS terms. */
struct edge_t {

    /** Vertex the edge leaves. */
    ustore_key_t source_id;

    /** Vertex the edge enters. */
    ustore_key_t target_id;

    /** Identifier of the edge itself. */
    ustore_key_t id {ustore_default_edge_id_k};

    friend bool operator==(edge_t const&, edge_t const&) = default;
};

/** One side of an edge as a vertex stores it, kept sorted inside the vertex's adjacency list. */
struct neighborship_t {

    /** Vertex on the other end of the edge. */
    ustore_key_t neighbor_id {0};

    /** Identifier of the edge. */
    ustore_key_t edge_id {0};

    friend bool operator==(neighborship_t const&, neighborship_t const&) = default;
    friend std::strong_ordering operator<=>(neighborship_t const&, neighborship_t const&) = default;
};

#pragma endregion Graph Elements

#pragma region Variable Length Views

/** A view of a stored value that tells a missing value apart from an empty one.
 *  Its layout of a pointer and a length passes into the C ABI without conversion. */
class value_view_t {

    ustore_bytes_cptr_t pointer_ = nullptr;
    ustore_length_t length_ = ustore_length_missing_k;

  public:
    using value_type = byte_t;

    value_view_t() = default;
    value_view_t(ustore_bytes_cptr_t pointer, ustore_length_t length) noexcept : pointer_(pointer), length_(length) {}

    value_view_t(byte_t const* begin, byte_t const* end) noexcept
        : pointer_(ustore_bytes_cptr_t(begin)), length_(static_cast<ustore_length_t>(end - begin)) {}

    value_view_t(byte_t const* begin, std::size_t length) noexcept
        : pointer_(ustore_bytes_cptr_t(begin)), length_(static_cast<ustore_length_t>(length)) {}

    value_view_t(char const* string) noexcept
        : pointer_(ustore_bytes_cptr_t(string)), length_(static_cast<ustore_length_t>(std::strlen(string))) {}

    value_view_t(char const* string, std::size_t length) noexcept
        : pointer_(ustore_bytes_cptr_t(string)), length_(static_cast<ustore_length_t>(length)) {}

    template <typename char_, typename traits_>
    value_view_t(std::basic_string_view<char_, traits_> view) noexcept
        : pointer_(ustore_bytes_cptr_t(view.data())),
          length_(static_cast<ustore_length_t>(view.size() * sizeof(char_))) {}

    value_view_t(std::string_view view) noexcept
        : pointer_(ustore_bytes_cptr_t(view.data())), length_(static_cast<ustore_length_t>(view.size())) {}

    /** An empty value that is present, unlike a default-constructed missing one. */
    static value_view_t make_empty() noexcept { return value_view_t {nullptr, nullptr}; }

    operator bool() const noexcept { return length_ != ustore_length_missing_k; }
    std::size_t size() const noexcept { return length_ == ustore_length_missing_k ? 0 : length_; }
    byte_t const* data() const noexcept {
        return length_ != ustore_length_missing_k ? reinterpret_cast<byte_t const*>(pointer_) : nullptr;
    }

    char const* c_str() const noexcept { return reinterpret_cast<char const*>(pointer_); }
    byte_t const* begin() const noexcept { return data(); }
    byte_t const* end() const noexcept { return data() + size(); }
    bool empty() const noexcept { return !size(); }
    operator std::string_view() const noexcept { return {c_str(), size()}; }

    ustore_bytes_cptr_t const* member_ptr() const noexcept { return &pointer_; }
    ustore_length_t const* member_length() const noexcept { return &length_; }

    bool operator==(value_view_t other) const noexcept { return std::ranges::equal(*this, other); }
};

/** A mutable view of a value inside a tape, addressed by its offset and length. */
class value_ref_t {

    ustore_byte_t* pointer_ = nullptr;
    ustore_length_t* offset_ = nullptr;
    ustore_length_t* length_ = nullptr;

  public:
    using value_type = byte_t;

    value_ref_t() = default;
    value_ref_t(ustore_byte_t* pointer, ustore_length_t& offset, ustore_length_t& length) noexcept
        : pointer_(pointer), offset_(&offset), length_(&length) {}

    byte_t const* begin() const noexcept { return reinterpret_cast<byte_t const*>(pointer_); }
    byte_t const* end() const noexcept { return begin() + size(); }
    char const* c_str() const noexcept { return reinterpret_cast<char const*>(pointer_); }
    std::size_t size() const noexcept { return *length_ == ustore_length_missing_k ? 0 : *length_; }
    bool empty() const noexcept { return !size(); }
    operator bool() const noexcept { return *length_ != ustore_length_missing_k; }

    ustore_byte_t* const* member_ptr() const noexcept { return &pointer_; }
    ustore_length_t const* member_offset() const noexcept { return offset_; }
    ustore_length_t const* member_length() const noexcept { return length_; }

    bool operator==(value_ref_t other) const noexcept { return std::ranges::equal(*this, other); }

    void swap(value_ref_t& other) noexcept {
        std::swap(*pointer_, *other.pointer_);
        std::swap(*offset_, *other.offset_);
        std::swap(*length_, *other.length_);
    }

    friend void swap(value_ref_t& first, value_ref_t& second) noexcept { first.swap(second); }
};

/** Views the bytes of a contiguous container. */
template <std::ranges::contiguous_range container_>
    requires std::ranges::sized_range<container_>
value_view_t value_view(container_&& container) noexcept {
    using element_t = std::ranges::range_value_t<container_>;
    return {reinterpret_cast<byte_t const*>(std::ranges::data(container)),
            std::ranges::size(container) * sizeof(element_t)};
}

#pragma endregion Variable Length Views

#pragma region Memory Management

/** Owns an arena the C ABI fills with results, freeing it on destruction. */
class arena_t {

    ustore_database_t database_ = nullptr;
    ustore_arena_t memory_ = nullptr;

  public:
    arena_t(ustore_database_t database) noexcept : database_(database) {}
    arena_t(arena_t const&) = delete;
    arena_t& operator=(arena_t const&) = delete;

    ~arena_t() {
        ustore_arena_free(memory_);
        memory_ = nullptr;
    }

    arena_t(arena_t&& other) noexcept : database_(other.database_), memory_(std::exchange(other.memory_, nullptr)) {}

    arena_t& operator=(arena_t&& other) noexcept {
        std::swap(database_, other.database_);
        std::swap(memory_, other.memory_);
        return *this;
    }

    ustore_arena_t* member_ptr() noexcept { return &memory_; }
    operator ustore_arena_t*() & noexcept { return member_ptr(); }
    ustore_database_t db() const noexcept { return database_; }
};

/** Either owns an arena or borrows one that a longer-lived container holds. */
class any_arena_t {

    arena_t owned_;
    ustore_arena_t* accessible_ = nullptr;

  public:
    any_arena_t(ustore_database_t database) noexcept : owned_(database), accessible_(nullptr) {}
    any_arena_t(arena_t& accessible) noexcept : owned_(nullptr), accessible_(accessible.member_ptr()) {}
    any_arena_t(ustore_database_t database, ustore_arena_t* accessible) noexcept
        : owned_(accessible ? nullptr : database), accessible_(accessible) {}

    any_arena_t(any_arena_t&&) = default;
    any_arena_t& operator=(any_arena_t&&) = default;

    any_arena_t(any_arena_t const& other) noexcept
        : owned_(other.is_remote() ? nullptr : other.owned_.db()), accessible_(other.accessible_) {}

    any_arena_t& operator=(any_arena_t const& other) noexcept {
        owned_ = other.is_remote() ? arena_t(nullptr) : other.owned_.db();
        accessible_ = other.accessible_;
        return *this;
    }

    /** Whether the arena is borrowed rather than owned. */
    bool is_remote() const noexcept { return accessible_; }
    ustore_arena_t* member_ptr() noexcept { return accessible_ ? accessible_ : owned_.member_ptr(); }
    operator ustore_arena_t*() & noexcept { return member_ptr(); }
    arena_t release_owned() noexcept { return std::exchange(owned_, arena_t {owned_.db()}); }
};

#pragma endregion Memory Management

#pragma region Adapters

/**
 *  Mixes the hash of @p value into @p seed.
 *  @see https://www.boost.org/doc/libs/1_37_0/doc/html/hash/reference.html#boost.hash_combine
 */
template <typename hashable_>
void hash_combine(std::size_t& seed, hashable_ const& value) noexcept {
    std::hash<hashable_> hasher;
    seed ^= hasher(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

/** Hashes a key together with its collection. */
struct collection_key_hash_t {
    std::size_t operator()(collection_key_t const& location) const noexcept {
        std::size_t result = SIZE_MAX;
        hash_combine(result, location.key);
        hash_combine(result, location.collection);
        return result;
    }
};

/** Hashes an edge by both its ends and its identifier. */
struct edge_hash_t {
    std::size_t operator()(edge_t const& edge) const noexcept {
        std::size_t result = SIZE_MAX;
        hash_combine(result, edge.source_id);
        hash_combine(result, edge.target_id);
        hash_combine(result, edge.id);
        return result;
    }
};

/** The role the other end of an edge plays. */
inline ustore_vertex_role_t invert(ustore_vertex_role_t role) noexcept {
    switch (role) {
    case ustore_vertex_source_k: return ustore_vertex_target_k;
    case ustore_vertex_target_k: return ustore_vertex_source_k;
    case ustore_vertex_role_any_k: return ustore_vertex_role_unknown_k;
    case ustore_vertex_role_unknown_k: return ustore_vertex_role_any_k;
    }
    __builtin_unreachable();
}

constexpr std::size_t bits_in_byte_k = 8;

/** The power of two just above the highest set bit of @p value. */
inline std::size_t next_power_of_two(std::size_t value) noexcept { return std::bit_floor(value) << 1; }

template <std::integral integer_ = std::size_t>
integer_ divide_round_up(integer_ value, integer_ divisor) noexcept {
    return (value + (divisor - 1)) / divisor;
}

template <std::integral integer_ = std::size_t>
integer_ next_multiple(integer_ value, integer_ divisor) noexcept {
    return divide_round_up(value, divisor) * divisor;
}

/** The document field type that stores @p element_. */
template <typename element_>
constexpr ustore_doc_field_type_t ustore_doc_field() noexcept {
    if constexpr (std::is_same_v<element_, bool>) return ustore_doc_field_bool_k;
    if constexpr (std::is_same_v<element_, std::int8_t>) return ustore_doc_field_i8_k;
    if constexpr (std::is_same_v<element_, std::int16_t>) return ustore_doc_field_i16_k;
    if constexpr (std::is_same_v<element_, std::int32_t>) return ustore_doc_field_i32_k;
    if constexpr (std::is_same_v<element_, std::int64_t>) return ustore_doc_field_i64_k;
    if constexpr (std::is_same_v<element_, std::uint8_t>) return ustore_doc_field_u8_k;
    if constexpr (std::is_same_v<element_, std::uint16_t>) return ustore_doc_field_u16_k;
    if constexpr (std::is_same_v<element_, std::uint32_t>) return ustore_doc_field_u32_k;
    if constexpr (std::is_same_v<element_, std::uint64_t>) return ustore_doc_field_u64_k;
    if constexpr (std::is_same_v<element_, float>) return ustore_doc_field_f32_k;
    if constexpr (std::is_same_v<element_, double>) return ustore_doc_field_f64_k;
    if constexpr (std::is_same_v<element_, value_view_t>) return ustore_doc_field_bin_k;
    if constexpr (std::is_same_v<element_, std::string_view> || std::is_same_v<element_, ustore_str_view_t>)
        return ustore_doc_field_str_k;
    return ustore_doc_field_json_k;
}

#pragma endregion Adapters

} // namespace unum::ustore

template <>
struct std::hash<unum::ustore::collection_key_t> : unum::ustore::collection_key_hash_t {};
