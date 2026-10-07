/**
 *  @file include/ustore/types.hpp
 *  @author Ash Vardanian
 *  @date July 4, 2022
 *  @brief Locations, edges, value views and arenas shared by every C++ wrapper.
 */

#pragma once

#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `SIZE_MAX`, `std::int64_t`, `std::uint64_t`
#include <cstring> // `std::strlen`

#include <algorithm>   // `std::ranges::equal`
#include <bit>         // `std::bit_floor`
#include <compare>     // `std::strong_ordering`
#include <concepts>    // `std::same_as`, `std::integral`, `std::unsigned_integral`
#include <functional>  // `std::hash`
#include <limits>      // `std::numeric_limits`
#include <ranges>      // `std::ranges::contiguous_range`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::remove_cvref_t`, `std::is_same_v`, `std::type_identity_t`
#include <utility>     // `std::exchange`, `std::swap`

#include "ustore/status.hpp" // `expected`, `status_t`
#include "ustore/ustore.h"

namespace ashvardanian::forkunion {}
namespace ashvardanian::smashtable {}
namespace ashvardanian::stringzilla {}

#pragma region Identifiers

// The C structs live in the global namespace, so their comparisons live there too, where lookup finds them

/** Orders identifiers as their bytes, which is how every engine stores and walks them. */
constexpr std::strong_ordering operator<=>(ustore_uuid_t const& first, ustore_uuid_t const& second) noexcept {
    return std::lexicographical_compare_three_way(first.bytes, first.bytes + 16, second.bytes, second.bytes + 16);
}
constexpr bool operator==(ustore_uuid_t const& first, ustore_uuid_t const& second) noexcept {
    return (first <=> second) == 0;
}
constexpr std::strong_ordering operator<=>(ustore_key_t const& first, ustore_key_t const& second) noexcept {
    return first.uuid <=> second.uuid;
}
constexpr bool operator==(ustore_key_t const& first, ustore_key_t const& second) noexcept {
    return first.uuid == second.uuid;
}
constexpr std::strong_ordering operator<=>(ustore_collection_id_t const& first,
                                           ustore_collection_id_t const& second) noexcept {
    return first.uuid <=> second.uuid;
}
constexpr bool operator==(ustore_collection_id_t const& first, ustore_collection_id_t const& second) noexcept {
    return first.uuid == second.uuid;
}

template <>
struct std::hash<ustore_uuid_t> {
    std::size_t operator()(ustore_uuid_t const& uuid) const noexcept {
        std::uint64_t halves[2];
        std::memcpy(halves, uuid.bytes, sizeof(halves));
        return std::hash<std::uint64_t> {}(halves[0] ^ (halves[1] * 0x9E3779B97F4A7C15ull));
    }
};

template <>
struct std::hash<ustore_key_t> {
    std::size_t operator()(ustore_key_t const& key) const noexcept { return std::hash<ustore_uuid_t> {}(key.uuid); }
};

template <>
struct std::hash<ustore_collection_id_t> {
    std::size_t operator()(ustore_collection_id_t const& id) const noexcept {
        return std::hash<ustore_uuid_t> {}(id.uuid);
    }
};

#pragma endregion Identifiers

namespace unum::ustore {

namespace fu = ashvardanian::forkunion;
namespace st = ashvardanian::smashtable;
namespace sz = ashvardanian::stringzilla;

#pragma region Identifier Arithmetic

/** The UUID spelling @p integer big-endian in its last eight bytes, so UUIDs made from integers order as the
 *  integers, exactly as Python's `uuid.UUID(int=integer)` does. */
constexpr ustore_uuid_t uuid_from_integer(std::uint64_t integer) noexcept {
    ustore_uuid_t uuid {};
    for (unsigned index = 0; index != 8; ++index)
        uuid.bytes[15 - index] = static_cast<std::uint8_t>(integer >> (8 * index));
    return uuid;
}

/** The integer @p uuid spells, which only a UUID with its first eight bytes zero has. */
inline expected<std::uint64_t> uuid_to_integer(ustore_uuid_t const& uuid) noexcept {
    std::uint64_t integer = 0;
    for (unsigned index = 0; index != 8; ++index)
        if (uuid.bytes[index] != 0) return status_t {status_code_t::invalid_argument_k, "The UUID exceeds 64 bits"};
    for (unsigned index = 8; index != 16; ++index) integer = (integer << 8) | uuid.bytes[index];
    return integer;
}

/** The key spelling @p integer, as @c uuid_from_integer does. */
constexpr ustore_key_t key_from_integer(std::uint64_t integer) noexcept { return {uuid_from_integer(integer)}; }

/** The first key in their order, the nil UUID, where a walk of a whole collection starts. */
inline constexpr ustore_key_t smallest_key_k {};

/** The last key in their order, every bit set, which @c ustore_key_unknown_k spells too. */
inline constexpr ustore_key_t largest_key_k {
    {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}}};

/** The key right after @p key in their order, where a scan resuming past @p key starts; the last key has none and
 *  wraps to the first. */
constexpr ustore_key_t key_after(ustore_key_t key) noexcept {
    for (unsigned index = 16; index != 0; --index)
        if (++key.uuid.bytes[index - 1] != 0) break;
    return key;
}

#pragma endregion Identifier Arithmetic

#pragma region Identifier Text

/** Characters the canonical `8-4-4-4-12` text of a UUID takes, dashes included. */
inline constexpr std::size_t uuid_text_length_k = 36;

/** Writes the canonical lowercase `8-4-4-4-12` text of @p uuid into @p text, without a terminator. */
constexpr void uuid_to_text(ustore_uuid_t const& uuid, std::span<char, uuid_text_length_k> text) noexcept {
    constexpr char digits_k[] = "0123456789abcdef";
    std::size_t position = 0;
    for (unsigned index = 0; index != 16; ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) text[position++] = '-';
        text[position++] = digits_k[uuid.bytes[index] >> 4];
        text[position++] = digits_k[uuid.bytes[index] & 0x0F];
    }
}

/** Reads @p text as canonical UUID text in either case, or as a non-negative decimal integer, which spells as
 *  @c uuid_from_integer does. */
inline expected<ustore_uuid_t> uuid_from_text(std::string_view text) noexcept {
    status_t const malformed {status_code_t::invalid_argument_k, "Neither UUID text nor a non-negative integer"};
    ustore_uuid_t uuid {};
    if (text.size() == uuid_text_length_k) {
        std::size_t position = 0;
        for (unsigned index = 0; index != 16; ++index) {
            if (index == 4 || index == 6 || index == 8 || index == 10) {
                if (text[position] != '-') return malformed;
                ++position;
            }
            unsigned byte = 0;
            for (unsigned half = 0; half != 2; ++half, ++position) {
                char const digit = text[position];
                unsigned const value = digit >= '0' && digit <= '9'   ? unsigned(digit - '0')
                                       : digit >= 'a' && digit <= 'f' ? unsigned(digit - 'a' + 10)
                                       : digit >= 'A' && digit <= 'F' ? unsigned(digit - 'A' + 10)
                                                                      : 16u;
                if (value == 16) return malformed;
                byte = byte * 16 + value;
            }
            uuid.bytes[index] = static_cast<std::uint8_t>(byte);
        }
        return uuid;
    }
    if (text.empty() || text.size() > 20) return malformed;
    std::uint64_t integer = 0;
    for (char const digit : text) {
        if (digit < '0' || digit > '9') return malformed;
        std::uint64_t const scaled = integer * 10 + unsigned(digit - '0');
        if (scaled / 10 != integer) return malformed;
        integer = scaled;
    }
    return uuid_from_integer(integer);
}

#pragma endregion Identifier Text

using f64_t = double;
using f32_t = float;

using i64_t = std::int64_t;
using i32_t = std::int32_t;
using i16_t = std::int16_t;
using i8_t = std::int8_t;

using u64_t = std::uint64_t;
using u32_t = std::uint32_t;
using u16_t = std::uint16_t;
using u8_t = std::uint8_t;

/** A single byte of a stored value, kept distinct from characters and integers. */
using byte_t = std::byte;

#pragma region Access Modes

/** Whether a read inside a transaction registers its keys for conflict detection. */
enum class watch_t : bool { dont_watch_k, watch_k };

/** Whether a write returns only after its data reaches the disk. */
enum class flush_t : bool { dont_flush_k, flush_k };

#pragma endregion Access Modes

#pragma region Locations

/** A key inside a collection, laid out so a strided pointer can address either member. */
struct collection_key_t {

    /** Collection owning the key. */
    ustore_collection_id_t collection {ustore_collection_main_k};

    /** Key inside the collection. */
    ustore_key_t key {};

    collection_key_t() = default;
    explicit collection_key_t(ustore_key_t key) noexcept : key(key) {}
    collection_key_t(ustore_collection_id_t collection, ustore_key_t key) noexcept : collection(collection), key(key) {}

    /** The same key addressed inside @p other_collection. */
    collection_key_t in(ustore_collection_id_t other_collection) const noexcept { return {other_collection, key}; }

    friend bool operator==(collection_key_t const&, collection_key_t const&) = default;
    friend std::strong_ordering operator<=>(collection_key_t const&, collection_key_t const&) = default;
};

/** A key inside a collection, optionally narrowed down to one field of its document. */
struct collection_key_field_t {

    /** Collection owning the key. */
    ustore_collection_id_t collection {};

    /** Key inside the collection. */
    ustore_key_t key {ustore_key_unknown_k};

    /** Path of the document field, or @c nullptr for the whole value. */
    ustore_str_view_t field {nullptr};

    collection_key_field_t() = default;
    collection_key_field_t(ustore_collection_id_t collection, ustore_key_t key,
                           ustore_str_view_t field = nullptr) noexcept
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
concept key_location_concept = std::same_as<std::remove_cvref_t<location_>, ustore_key_t> ||
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
    ustore_key_t neighbor_id {};

    /** Identifier of the edge. */
    ustore_key_t edge_id {};

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

    ustore_arena_t memory_ = nullptr;

  public:
    arena_t() noexcept = default;
    arena_t(arena_t const&) = delete;
    arena_t& operator=(arena_t const&) = delete;
    ~arena_t() noexcept { ustore_arena_free(memory_); }

    arena_t(arena_t&& other) noexcept : memory_(std::exchange(other.memory_, nullptr)) {}

    arena_t& operator=(arena_t&& other) noexcept {
        std::swap(memory_, other.memory_);
        return *this;
    }

    ustore_arena_t* member_ptr() noexcept { return &memory_; }
    operator ustore_arena_t*() & noexcept { return member_ptr(); }
};

/** Either owns an arena or borrows one that a longer-lived container holds, while a copy owns a fresh arena. */
class any_arena_t {

    arena_t owned_;
    ustore_arena_t* borrowed_ = nullptr;

  public:
    any_arena_t() noexcept = default;
    explicit any_arena_t(ustore_arena_t* borrowed) noexcept : borrowed_(borrowed) {}

    any_arena_t(any_arena_t&&) noexcept = default;
    any_arena_t& operator=(any_arena_t&&) noexcept = default;

    any_arena_t(any_arena_t const&) noexcept {}
    any_arena_t& operator=(any_arena_t const&) noexcept {
        owned_ = arena_t {};
        borrowed_ = nullptr;
        return *this;
    }

    ustore_arena_t* member_ptr() noexcept { return borrowed_ ? borrowed_ : owned_.member_ptr(); }
    operator ustore_arena_t*() & noexcept { return member_ptr(); }
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
    return ustore_vertex_role_unknown_k;
}

constexpr std::size_t bits_in_byte_k = 8;

/** The power of two just above the highest set bit of @p value. */
inline std::size_t next_power_of_two(std::size_t value) noexcept { return std::bit_floor(value) << 1; }

/** @p value divided by @p divisor and rounded up, without the overflow `value + divisor - 1` risks near the limit. */
template <std::unsigned_integral integer_>
constexpr integer_ divide_round_up(integer_ value, std::type_identity_t<integer_> divisor) noexcept {
    return static_cast<integer_>(value / divisor + (value % divisor != 0));
}

/** The greatest multiple of @p multiple at or below @p value. */
template <std::unsigned_integral integer_>
constexpr integer_ round_down_to_multiple(integer_ value, std::type_identity_t<integer_> multiple) noexcept {
    return static_cast<integer_>(value - value % multiple);
}

/** The least multiple of @p multiple at or above @p value, saturating at the greatest multiple @p integer_ holds. */
template <std::unsigned_integral integer_>
constexpr integer_ round_up_to_multiple(integer_ value, std::type_identity_t<integer_> multiple) noexcept {
    integer_ const below = round_down_to_multiple(value, multiple);
    if (below == value || below > std::numeric_limits<integer_>::max() - multiple) return below;
    return static_cast<integer_>(below + multiple);
}

/** The document field type that stores @p element_. */
template <typename element_>
constexpr ustore_doc_field_type_t ustore_doc_field() noexcept {
    if constexpr (std::is_same_v<element_, bool>) return ustore_doc_field_bool_k;
    if constexpr (std::is_same_v<element_, f64_t>) return ustore_doc_field_f64_k;
    if constexpr (std::is_same_v<element_, f32_t>) return ustore_doc_field_f32_k;
    if constexpr (std::is_same_v<element_, i64_t>) return ustore_doc_field_i64_k;
    if constexpr (std::is_same_v<element_, i32_t>) return ustore_doc_field_i32_k;
    if constexpr (std::is_same_v<element_, i16_t>) return ustore_doc_field_i16_k;
    if constexpr (std::is_same_v<element_, i8_t>) return ustore_doc_field_i8_k;
    if constexpr (std::is_same_v<element_, u64_t>) return ustore_doc_field_u64_k;
    if constexpr (std::is_same_v<element_, u32_t>) return ustore_doc_field_u32_k;
    if constexpr (std::is_same_v<element_, u16_t>) return ustore_doc_field_u16_k;
    if constexpr (std::is_same_v<element_, u8_t>) return ustore_doc_field_u8_k;
    if constexpr (std::is_same_v<element_, value_view_t>) return ustore_doc_field_bin_k;
    if constexpr (std::is_same_v<element_, std::string_view> || std::is_same_v<element_, ustore_str_view_t>)
        return ustore_doc_field_str_k;
    return ustore_doc_field_json_k;
}

#pragma endregion Adapters

} // namespace unum::ustore

template <>
struct std::hash<unum::ustore::collection_key_t> : unum::ustore::collection_key_hash_t {};
