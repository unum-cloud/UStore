/**
 *  @file include/ustore/utilities/types.hpp
 *  @author Ash Vardanian
 *  @date July 4, 2022
 *  @brief The vocabulary every template here speaks: statuses and the expected wrapper, UUIDs, keys and collection
 *      IDs with their orders, hashes and spellings, edges, value views, integer rounding, and the waiter.
 *
 *  The statuses and @c expected are re-exported into `unum::ustore` here, and `cpp/ustore/types.hpp` re-exports the
 *  rest, so the C++ SDK and these templates name one set of types.
 */

#pragma once

#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `SIZE_MAX`, `std::int64_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::strlen`

#include <algorithm>   // `std::ranges::equal`, `std::lexicographical_compare_three_way`
#include <bit>         // `std::bit_floor`
#include <compare>     // `std::strong_ordering`
#include <concepts>    // `std::convertible_to`, `std::integral`, `std::same_as`, `std::unsigned_integral`
#include <functional>  // `std::hash`
#include <limits>      // `std::numeric_limits`
#include <new>         // placement `new`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <tuple>       // `std::tuple_size`, `std::tuple_element`
#include <type_traits> // `std::is_nothrow_move_constructible_v`, `std::remove_cvref_t`, `std::type_identity`
#include <utility>     // `std::cmp_equal`, `std::move`

#include <forkunion/capabilities.hpp> // `fu::preferred_yield_t`
#include <smashtable/shared.hpp>      // `st::hash`

#include "ustore/ustore.h"

namespace ashvardanian::forkunion {}
namespace ashvardanian::smashtable {}
namespace ashvardanian::stringzilla {}

#pragma region Identifier Orders

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

#pragma endregion Identifier Orders

namespace unum::ustore {

namespace fu = ashvardanian::forkunion;
namespace st = ashvardanian::smashtable;
namespace sz = ashvardanian::stringzilla;

namespace utilities {

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

/** How a thread stalls while it spins on a word: the newest pause, yield or monitored wait this build may run. */
using waiter_t = fu::preferred_yield_t;

#pragma region Statuses

/** What went wrong, with one enumerator per @c ustore_status_t that the C header documents. */
enum class status_code_t : int {
    success_k = ustore_status_success_k,
    deferred_k = ustore_status_deferred_k,
    out_of_memory_k = ustore_status_out_of_memory_k,
    invalid_argument_k = ustore_status_invalid_argument_k,
    uninitialized_k = ustore_status_uninitialized_k,
    unsupported_k = ustore_status_unsupported_k,
    io_k = ustore_status_io_k,
    malformed_k = ustore_status_malformed_k,
    not_found_k = ustore_status_not_found_k,
    already_exists_k = ustore_status_already_exists_k,
    write_conflict_k = ustore_status_write_conflict_k,
    read_conflict_k = ustore_status_read_conflict_k,
    phantom_conflict_k = ustore_status_phantom_conflict_k,
    capacity_exhausted_k = ustore_status_capacity_exhausted_k,
    device_k = ustore_status_device_k,
    internal_k = ustore_status_internal_k,
    contended_k = ustore_status_contended_k,
    cancelled_k = ustore_status_cancelled_k,
    read_only_k = ustore_status_read_only_k,
};

/** The C++ twin of @c ustore_outcome_t: a code and a static message, @c nullptr on success. */
struct [[nodiscard]] status_t {

    /** What went wrong, or @c status_code_t::success_k. */
    status_code_t code = status_code_t::success_k;

    /** Static detail the code cannot carry, never freed. */
    char const* message = nullptr;

    constexpr status_t() noexcept = default;
    constexpr status_t(status_code_t reason, char const* detail = nullptr) noexcept : code(reason), message(detail) {}
    constexpr status_t(ustore_outcome_t const& outcome) noexcept
        : code(static_cast<status_code_t>(outcome.status)), message(outcome.message) {}

    /** The same status as the C ABI spells it, for writing into an outcome. */
    constexpr operator ustore_outcome_t() const noexcept { return {static_cast<ustore_status_t>(code), message}; }

    constexpr bool succeeded() const noexcept { return code == status_code_t::success_k; }
    constexpr bool failed() const noexcept { return code != status_code_t::success_k; }

    friend constexpr bool operator==(status_t const& status, status_code_t code) noexcept {
        return status.code == code;
    }
};

static_assert(std::is_trivially_copyable_v<status_t>);

/**
 *  @brief A value, or the status that explains its absence.
 *
 *  The value lives in a @c union that is alive only on success, so @p value_type_ needs no default constructor and
 *  may be move-only. Decomposes as @c auto @c [value, status] where the value is nothrow default constructible.
 */
template <typename value_type_>
class [[nodiscard]] expected {
  public:
    using value_t = value_type_;
    using value_type = value_type_;

    static_assert(std::is_nothrow_move_constructible_v<value_t>, "expected moves its value without throwing");

  private:
    union {
        value_t value_;
    };
    status_t status_;

  public:

    /** A failure; a success that carries no value reads as @c status_code_t::internal_k. */
    constexpr expected(status_t status) noexcept
        : status_(status.succeeded() ? status_t {status_code_t::internal_k, "An expected value is missing"} : status) {}

    /** A failure without a message. */
    constexpr expected(status_code_t code) noexcept : expected(status_t {code}) {}

    expected(value_t&& value) noexcept { new (&value_) value_t(std::move(value)); }

    expected(value_t const& value) noexcept
        requires std::is_nothrow_copy_constructible_v<value_t>
    {
        new (&value_) value_t(value);
    }

    expected(expected&& other) noexcept : status_(other.status_) {
        if (status_.succeeded()) new (&value_) value_t(std::move(other.value_));
    }

    expected(expected const& other) noexcept
        requires std::is_nothrow_copy_constructible_v<value_t>
        : status_(other.status_) {
        if (status_.succeeded()) new (&value_) value_t(other.value_);
    }

    expected& operator=(expected&& other) noexcept {
        if (this == &other) return *this;
        if (status_.succeeded()) value_.~value_t();
        status_ = other.status_;
        if (status_.succeeded()) new (&value_) value_t(std::move(other.value_));
        return *this;
    }

    expected& operator=(expected const& other) noexcept
        requires std::is_nothrow_copy_constructible_v<value_t>
    {
        if (this == &other) return *this;
        if (status_.succeeded()) value_.~value_t();
        status_ = other.status_;
        if (status_.succeeded()) new (&value_) value_t(other.value_);
        return *this;
    }

    ~expected() noexcept {
        if (status_.succeeded()) value_.~value_t();
    }

    constexpr explicit operator bool() const noexcept { return status_.succeeded(); }
    constexpr bool has_value() const noexcept { return status_.succeeded(); }
    constexpr bool failed() const noexcept { return status_.failed(); }
    constexpr status_t status() const noexcept { return status_; }
    constexpr status_code_t code() const noexcept { return status_.code; }

    /** @warning Reading the value of a failed @c expected is undefined, so check it first. */
    constexpr value_t& operator*() & noexcept { return value_; }
    constexpr value_t const& operator*() const& noexcept { return value_; }
    constexpr value_t&& operator*() && noexcept { return std::move(value_); }
    constexpr value_t* operator->() noexcept { return &value_; }
    constexpr value_t const* operator->() const noexcept { return &value_; }

    /** The tuple protocol behind @c auto @c [value, status], default-constructing the value of a failure. */
    template <std::size_t index_>
        requires std::is_nothrow_default_constructible_v<value_t>
    constexpr auto get() && noexcept {
        if constexpr (index_ == 0) return status_.succeeded() ? value_t {std::move(value_)} : value_t {};
        else return status_;
    }

    template <std::size_t index_>
        requires(std::is_nothrow_default_constructible_v<value_t> && std::is_nothrow_copy_constructible_v<value_t>)
    constexpr auto get() const& noexcept {
        if constexpr (index_ == 0) return status_.succeeded() ? value_t {value_} : value_t {};
        else return status_;
    }
};

/** Whether @p result carries @p code, which is @c status_code_t::success_k exactly when it holds a value. */
template <typename value_type_>
constexpr bool operator==(expected<value_type_> const& result, status_code_t code) noexcept {
    return result.code() == code;
}

/** Whether @p result holds a value equal to @p value, so a failure equals nothing. */
template <typename value_type_, typename comparable_type_>
    requires(!std::same_as<std::remove_cvref_t<comparable_type_>, status_code_t>) &&
            requires(value_type_ const& held, comparable_type_ const& other) {
                { held == other } -> std::convertible_to<bool>;
            }
constexpr bool operator==(expected<value_type_> const& result, comparable_type_ const& value) noexcept {
    if (!result.has_value()) return false;
    if constexpr (std::integral<value_type_> && std::integral<comparable_type_> && !std::same_as<value_type_, bool> &&
                  !std::same_as<comparable_type_, bool>)
        return std::cmp_equal(*result, value);
    else return *result == value;
}

#pragma endregion Statuses

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

/** Orders keys and collection IDs by their bytes, as every engine does. */
struct uuid_order_t {
    template <typename uuid_type_>
    constexpr bool operator()(uuid_type_ const& first, uuid_type_ const& second) const noexcept {
        return first < second;
    }
};

/** Hashes a UUID as its two machine words, spreading the keys of a collection over every partition. */
struct uuid_hash_t {
    std::size_t operator()(ustore_uuid_t const& uuid) const noexcept {
        st::hash<std::uint64_t> const mixer;
        std::uint64_t halves[2];
        std::memcpy(halves, uuid.bytes, sizeof(halves));
        return mixer(halves[0] ^ (halves[1] * 0x9E3779B97F4A7C15ull));
    }
    std::size_t operator()(ustore_key_t const& key) const noexcept { return (*this)(key.uuid); }
    std::size_t operator()(ustore_collection_id_t const& id) const noexcept { return (*this)(id.uuid); }
};

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

#pragma endregion Locations

#pragma region Graph Elements

/** A graph edge, or a relationship in DBMS terms. */
struct edge_t {

    /** Vertex the edge leaves. */
    ustore_key_t source;

    /** Vertex the edge enters. */
    ustore_key_t target;

    /** Identifier of the edge itself. */
    ustore_key_t id {ustore_edge_id_default_k};

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

/** The role the other end of an edge plays. */
inline ustore_vertex_role_t invert(ustore_vertex_role_t role) noexcept {
    switch (role) {
    case ustore_vertex_role_source_k: return ustore_vertex_role_target_k;
    case ustore_vertex_role_target_k: return ustore_vertex_role_source_k;
    case ustore_vertex_role_any_k: return ustore_vertex_role_unknown_k;
    case ustore_vertex_role_unknown_k: return ustore_vertex_role_any_k;
    }
    return ustore_vertex_role_unknown_k;
}

#pragma endregion Graph Elements

#pragma region Variable Length Views

/** The length a missing value carries, which no stored value reaches, so a view tells it apart from an empty one. */
inline constexpr ustore_length_t missing_length_k = std::numeric_limits<ustore_length_t>::max();

/** A view of a stored value that tells a missing value apart from an empty one. */
class value_view_t {

    ustore_byte_t const* pointer_ = nullptr;
    ustore_length_t length_ = missing_length_k;

  public:
    using value_type = byte_t;

    value_view_t() = default;
    value_view_t(ustore_byte_t const* pointer, ustore_length_t length) noexcept : pointer_(pointer), length_(length) {}

    /** Views the bytes of @p bytes, a value that is always present. */
    value_view_t(ustore_bytes_view_t bytes) noexcept : pointer_(bytes.data), length_(bytes.length) {}

    value_view_t(byte_t const* begin, byte_t const* end) noexcept
        : pointer_(reinterpret_cast<ustore_byte_t const*>(begin)), length_(static_cast<ustore_length_t>(end - begin)) {}

    value_view_t(byte_t const* begin, std::size_t length) noexcept
        : pointer_(reinterpret_cast<ustore_byte_t const*>(begin)), length_(static_cast<ustore_length_t>(length)) {}

    /** Views @p bytes, a value that is always present, an empty one included. */
    value_view_t(std::span<byte_t const> bytes) noexcept
        : pointer_(reinterpret_cast<ustore_byte_t const*>(bytes.data())),
          length_(static_cast<ustore_length_t>(bytes.size())) {}

    value_view_t(char const* string) noexcept
        : pointer_(reinterpret_cast<ustore_byte_t const*>(string)),
          length_(static_cast<ustore_length_t>(std::strlen(string))) {}

    value_view_t(char const* string, std::size_t length) noexcept
        : pointer_(reinterpret_cast<ustore_byte_t const*>(string)), length_(static_cast<ustore_length_t>(length)) {}

    template <typename char_type_, typename traits_type_>
    value_view_t(std::basic_string_view<char_type_, traits_type_> view) noexcept
        : pointer_(reinterpret_cast<ustore_byte_t const*>(view.data())),
          length_(static_cast<ustore_length_t>(view.size() * sizeof(char_type_))) {}

    value_view_t(std::string_view view) noexcept
        : pointer_(reinterpret_cast<ustore_byte_t const*>(view.data())),
          length_(static_cast<ustore_length_t>(view.size())) {}

    /** An empty value that is present, unlike a default-constructed missing one. */
    static value_view_t make_empty() noexcept { return value_view_t {nullptr, nullptr}; }

    operator bool() const noexcept { return length_ != missing_length_k; }
    std::size_t size() const noexcept { return length_ == missing_length_k ? 0 : length_; }
    byte_t const* data() const noexcept {
        return length_ != missing_length_k ? reinterpret_cast<byte_t const*>(pointer_) : nullptr;
    }

    char const* c_str() const noexcept { return reinterpret_cast<char const*>(pointer_); }
    byte_t const* begin() const noexcept { return data(); }
    byte_t const* end() const noexcept { return data() + size(); }
    bool empty() const noexcept { return !size(); }
    operator std::string_view() const noexcept { return {c_str(), size()}; }

    /** The run of bytes the C ABI reads, where a missing value passes as an empty one. */
    ustore_bytes_view_t bytes() const noexcept { return {pointer_, size()}; }

    bool operator==(value_view_t other) const noexcept { return std::ranges::equal(*this, other); }
};

/** The bytes of @p text, which the C ABI reads without a terminator. */
inline ustore_bytes_view_t bytes_of(std::string_view text) noexcept {
    return {reinterpret_cast<ustore_byte_t const*>(text.data()), text.size()};
}

/** The text @p bytes spell. */
inline std::string_view string_of(ustore_bytes_view_t bytes) noexcept {
    return {reinterpret_cast<char const*>(bytes.data), static_cast<std::size_t>(bytes.length)};
}

/** A tape of 32-bit offsets, where string @c i spans @b [offsets[i],offsets[i+1]) of @p data. */
inline ustore_strings_view_t u32_tape(void const* data, std::uint32_t const* offsets) noexcept {
    ustore_strings_view_t tape {};
    tape.layout = ustore_strings_u32_tape_k;
    tape.data.u32_tape = {static_cast<ustore_byte_t const*>(data), offsets};
    return tape;
}

/** A tape of 64-bit offsets, where string @c i spans @b [offsets[i],offsets[i+1]) of @p data. */
inline ustore_strings_view_t u64_tape(void const* data, std::uint64_t const* offsets) noexcept {
    ustore_strings_view_t tape {};
    tape.layout = ustore_strings_u64_tape_k;
    tape.data.u64_tape = {static_cast<ustore_byte_t const*>(data), offsets};
    return tape;
}

#pragma endregion Variable Length Views

#pragma region Arithmetic and Hashing

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
        hash_combine(result, edge.source);
        hash_combine(result, edge.target);
        hash_combine(result, edge.id);
        return result;
    }
};

#pragma endregion Arithmetic and Hashing

} // namespace utilities

// The statuses every UStore call answers, in the namespace every SDK and engine opens
using utilities::expected;
using utilities::status_code_t;
using utilities::status_t;

} // namespace unum::ustore

template <typename value_type_>
struct std::tuple_size<unum::ustore::expected<value_type_>> {
    static constexpr std::size_t value = 2;
};

template <typename value_type_>
struct std::tuple_element<0, unum::ustore::expected<value_type_>> {
    using type = value_type_;
};

template <typename value_type_>
struct std::tuple_element<1, unum::ustore::expected<value_type_>> : std::type_identity<unum::ustore::status_t> {};

template <>
struct std::hash<unum::ustore::utilities::collection_key_t> : unum::ustore::utilities::collection_key_hash_t {};
