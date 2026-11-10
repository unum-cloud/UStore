/**
 *  @file include/ustore/utilities/types.hpp
 *  @author Ash Vardanian
 *  @date July 4, 2022
 *  @brief The vocabulary every template here speaks: statuses and the expected wrapper, UUIDs, keys
 *      and collection IDs with their orders, hashes and spellings, edges, value views, byte views
 *      and the serialization over them, owned text, integer rounding, and the waiter.
 *
 *  The statuses and @c expected are re-exported into @c unum::ustore here, and
 *  <tt>cpp/ustore/types.hpp</tt> re-exports the rest, so the C++ SDK and these templates name one
 *  set of types.
 */

#pragma once

#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `SIZE_MAX`, `std::int64_t`, `std::uint8_t`, `std::uint16_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::memcmp`, `std::strlen`

#include "ustore/types.h" // `USTORE_OS_WINDOWS_`

#if USTORE_OS_WINDOWS_
/*  Winsock 2 first keeps Winsock 1.1 out, unless a user's earlier `<windows.h>` brought it in. */
#if !defined(_WINSOCKAPI_)
#include <winsock2.h>
#endif
#include <windows.h> // NTSTATUS, which <bcrypt.h> needs
#include <bcrypt.h>  // `BCryptGenRandom`
#else
#include <sys/random.h> // `getentropy`
#endif

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
#include <type_traits> // `std::is_nothrow_move_constructible_v`, `std::is_trivially_copyable_v`, `std::remove_cvref_t`, `std::type_identity`
#include <utility> // `std::cmp_equal`, `std::move`

#include <forkunion/types.hpp>         // `ashvardanian::forkunion`, which `fu` names
#include <smashtable/basic_vector.hpp> // `st::vector`
#include <smashtable/shared.hpp>       // `st::hash`, `st::status_t`
#include <stringzilla/stringzilla.hpp> // `sz::string_t`, `sz::string_view_t`, `sz::failed`
#include <stringzilla/types.h>         // `sz_string_view_t`, `sz_strings_span_t`, `sz_status_t`

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

/** The base Linux adds to syscall numbers on MIPS and Alpha, so mirrored numbers add it too. */
#if defined(_MIPS_SIM) && _MIPS_SIM == _ABIO32
inline constexpr long linux_syscall_offset_k = 4000;
#elif defined(_MIPS_SIM) && _MIPS_SIM == _ABI64
inline constexpr long linux_syscall_offset_k = 5000;
#elif defined(_MIPS_SIM) && _MIPS_SIM == _ABIN32
inline constexpr long linux_syscall_offset_k = 6000;
#elif defined(__alpha__)
inline constexpr long linux_syscall_offset_k = 110;
#else
inline constexpr long linux_syscall_offset_k = 0;
#endif

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

#pragma region Statuses

/** What went wrong, with one enumerator per @c ustore_status_t that the C header documents.
 *  Statuses fall into five categories. Resources: @c out_of_memory_k, @c io_k and @c device_k.
 *  Input: @c invalid_argument_k and @c malformed_k. State: @c uninitialized_k, @c read_only_k,
 *  @c unsupported_k, @c not_found_k, @c already_exists_k, @c write_conflict_k, @c read_conflict_k,
 *  @c phantom_conflict_k, @c capacity_exhausted_k and @c contended_k. Interruptions: @c cancelled_k
 *  and @c deferred_k. Defects: @c internal_k. Any verb that allocates, binds a device or touches a
 *  file may also return a Resources status; any verb taking a progress callback may return
 *  @c cancelled_k. A verb's own block lists only what its contract adds. */
enum class status_code_t : int {
    success_k = ustore_status_success_k,
    deferred_k = ustore_status_deferred_k,
    out_of_memory_k = ustore_status_out_of_memory_k,
    read_only_k = ustore_status_read_only_k,
    io_k = ustore_status_io_k,
    device_k = ustore_status_device_k,
    malformed_k = ustore_status_malformed_k,
    internal_k = ustore_status_internal_k,
    uninitialized_k = ustore_status_uninitialized_k,
    contended_k = ustore_status_contended_k,
    cancelled_k = ustore_status_cancelled_k,
    unsupported_k = ustore_status_unsupported_k,
    invalid_argument_k = ustore_status_invalid_argument_k,
    not_found_k = ustore_status_not_found_k,
    already_exists_k = ustore_status_already_exists_k,
    write_conflict_k = ustore_status_write_conflict_k,
    read_conflict_k = ustore_status_read_conflict_k,
    phantom_conflict_k = ustore_status_phantom_conflict_k,
    capacity_exhausted_k = ustore_status_capacity_exhausted_k,
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

#pragma region SmashTable Status

/** The status reporting a SmashTable container's @p failure, which every
 *  fallible member answers. */
constexpr status_t to_status(st::status_t failure) noexcept {
    switch (failure) {
    case st::status_t::success_k: return {};
    case st::status_t::out_of_memory_heap_k: return {status_code_t::out_of_memory_k, "Out of memory"};
    case st::status_t::consistency_k: return {status_code_t::internal_k, "A container invariant did not hold"};
    case st::status_t::operation_not_permitted_k:
        return {status_code_t::invalid_argument_k, "The object does not admit the call"};
    case st::status_t::operation_would_block_k: return {status_code_t::internal_k, "A bounded retry gave up"};
    case st::status_t::invalid_argument_k: return {status_code_t::invalid_argument_k, "Invalid argument"};
    case st::status_t::capacity_exhausted_k: return {status_code_t::capacity_exhausted_k, "Capacity exhausted"};
    case st::status_t::input_output_k: return {status_code_t::io_k, "Input or output failed"};
    case st::status_t::key_already_exists_k: return {status_code_t::already_exists_k, "Key already exists"};
    case st::status_t::key_not_found_k: return {status_code_t::not_found_k, "Key not found"};
    case st::status_t::write_conflict_k: return {status_code_t::write_conflict_k, "Write conflict"};
    case st::status_t::read_conflict_k: return {status_code_t::read_conflict_k, "Read conflict"};
    case st::status_t::phantom_conflict_k: return {status_code_t::phantom_conflict_k, "Phantom conflict"};
    default: return {status_code_t::internal_k, "A container failed for an unknown reason"};
    }
}

#pragma endregion SmashTable Status

#pragma region Identifier Arithmetic

/** The UUID spelling @p integer big-endian in its last eight bytes, so UUIDs made from integers
 *  order as the integers, exactly as Python's <tt>uuid.UUID(int=integer)</tt> does. */
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

/** The key right after @p key in their order, where a scan resuming past @p key starts; the last
 *  key has none and wraps to the first. */
constexpr ustore_key_t key_after(ustore_key_t key) noexcept {
    for (unsigned index = 16; index != 0; --index)
        if (++key.uuid.bytes[index - 1] != 0) break;
    return key;
}

/** The key right before @p key in their order, where a descending scan resuming past @p key starts;
 *  the first key has none and wraps to the last. */
constexpr ustore_key_t key_before(ustore_key_t key) noexcept {
    for (unsigned index = 16; index != 0; --index)
        if (key.uuid.bytes[index - 1]-- != 0) break;
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

/** Characters the canonical @c 8-4-4-4-12 text of a UUID takes, dashes included. */
inline constexpr std::size_t uuid_text_length_k = 36;

/** Writes the canonical lowercase @c 8-4-4-4-12 text of @p uuid into @p text, without a terminator. */
constexpr void uuid_to_text(ustore_uuid_t const& uuid, std::span<char, uuid_text_length_k> text) noexcept {
    constexpr char digits_k[] = "0123456789abcdef";
    std::size_t position = 0;
    for (unsigned index = 0; index != 16; ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) text[position++] = '-';
        text[position++] = digits_k[uuid.bytes[index] >> 4];
        text[position++] = digits_k[uuid.bytes[index] & 0x0F];
    }
}

/** Reads @p text as canonical UUID text in either case, or as a non-negative decimal integer, which
 *  spells as @c uuid_from_integer does. */
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

#pragma region UUID

/** The bit pattern of octet 8, which says whose layout the remaining bytes follow. */
enum class uuid_variant_t : std::uint8_t {

    /** The @c 0xxx pattern of the Apollo Network Computing System. */
    network_computing_system_k,

    /** The @c 10xx pattern of RFC 4122, which every identifier this engine mints carries. */
    rfc_k,

    /** The @c 110x pattern of the Component Object Model. */
    microsoft_k,

    /** The @c 111x pattern, which no specification claims. */
    reserved_k,
};

/** A SHA-256 digest, all zeros where none was taken. */
struct sha256_t {
    std::uint8_t bytes[32] = {};

    [[nodiscard]] bool empty() const noexcept {
        for (std::uint8_t const byte : bytes)
            if (byte != 0) return false;
        return true;
    }

    friend bool operator==(sha256_t const& first, sha256_t const& second) noexcept {
        return std::memcmp(first.bytes, second.bytes, sizeof(first.bytes)) == 0;
    }
};

/**
 *  @brief A 128-bit identifier held as sixteen bytes, printed as the canonical 36 characters.
 *
 *  A zero identifier names nothing, which is what a default-constructed one holds and
 *  what @c is_valid reports; every minted identifier is version 4, so all but six of its
 *  bits are random.
 */
struct uuid128_t {

    /** Bytes the identifier occupies in memory and on disk. */
    static constexpr std::size_t size_bytes_k = 16;

    /** Characters the canonical @c 8-4-4-4-12 text form occupies, dashes included. */
    static constexpr std::size_t printed_length_k = 36;

    /** The sixteen bytes, in the order they are printed and stored. */
    std::uint8_t bytes[size_bytes_k] = {};

    uuid128_t() = default;

    /** Reads an identifier from the sixteen bytes at @p source. */
    explicit uuid128_t(std::span<std::uint8_t const, size_bytes_k> source) noexcept {
        std::memcpy(bytes, source.data(), size_bytes_k);
    }

    /** Whose layout the remaining bytes follow, read off octet 8. */
    constexpr uuid_variant_t variant() const noexcept {
        if ((bytes[8] & 0x80) == 0x00) return uuid_variant_t::network_computing_system_k;
        if ((bytes[8] & 0xC0) == 0x80) return uuid_variant_t::rfc_k;
        if ((bytes[8] & 0xE0) == 0xC0) return uuid_variant_t::microsoft_k;
        return uuid_variant_t::reserved_k;
    }

    /** Whether the identifier names anything, which a zero one does not. */
    constexpr bool is_valid() const noexcept {
        std::uint8_t any = 0;
        for (std::size_t index = 0; index != size_bytes_k; ++index) any |= bytes[index];
        return any != 0;
    }

    /** The first eight bytes read as a big-endian word, ordering identifiers as @c memcmp does. */
    constexpr std::uint64_t high_word() const noexcept { return word_(0); }

    /** The last eight bytes read as a big-endian word. */
    constexpr std::uint64_t low_word() const noexcept { return word_(8); }

    friend constexpr bool operator==(uuid128_t const&, uuid128_t const&) = default;

    friend constexpr std::strong_ordering operator<=>(uuid128_t const& first, uuid128_t const& second) noexcept {
        if (std::strong_ordering const high = first.high_word() <=> second.high_word(); high != 0) return high;
        return first.low_word() <=> second.low_word();
    }

    /** Writes the canonical @c 8-4-4-4-12 text form into @p destination, without a terminator. */
    constexpr void print(std::span<char, printed_length_k> destination) const noexcept {
        std::size_t written = 0;
        for (std::size_t index = 0; index != size_bytes_k; ++index) {
            if (index == 4 || index == 6 || index == 8 || index == 10) destination[written++] = '-';
            destination[written++] = digits_k[bytes[index] >> 4];
            destination[written++] = digits_k[bytes[index] & 0x0F];
        }
    }

    /** Whether @p text is 36 characters of the canonical form, so @c parse would read it whole. */
    static constexpr bool parses(std::span<char const, printed_length_k> text) noexcept {
        std::size_t read = 0;
        for (std::size_t index = 0; index != size_bytes_k; ++index) {
            if (index == 4 || index == 6 || index == 8 || index == 10)
                if (text[read++] != '-') return false;
            if (value_of_(text[read++]) == invalid_digit_k) return false;
            if (value_of_(text[read++]) == invalid_digit_k) return false;
        }
        return true;
    }

    /** Reads the canonical form out of @p text, which @c parses has already accepted. */
    static constexpr uuid128_t parse(std::span<char const, printed_length_k> text) noexcept {
        uuid128_t identifier;
        std::size_t read = 0;
        for (std::size_t index = 0; index != size_bytes_k; ++index) {
            if (index == 4 || index == 6 || index == 8 || index == 10) ++read;
            std::uint8_t const high = value_of_(text[read++]);
            std::uint8_t const low = value_of_(text[read++]);
            identifier.bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
        }
        return identifier;
    }

  private:

    /** The lowercase hexadecimal alphabet the text form prints. */
    static constexpr char digits_k[17] = "0123456789abcdef";

    /** What @c value_of_ answers for a character that is no hexadecimal digit. */
    static constexpr std::uint8_t invalid_digit_k = 0xFF;

    /** The value of one hexadecimal digit, in either case. */
    static constexpr std::uint8_t value_of_(char digit) noexcept {
        if (digit >= '0' && digit <= '9') return static_cast<std::uint8_t>(digit - '0');
        if (digit >= 'a' && digit <= 'f') return static_cast<std::uint8_t>(digit - 'a' + 10);
        if (digit >= 'A' && digit <= 'F') return static_cast<std::uint8_t>(digit - 'A' + 10);
        return invalid_digit_k;
    }

    /** The eight bytes at @p offset read as one big-endian word. */
    constexpr std::uint64_t word_(std::size_t offset) const noexcept {
        std::uint64_t word = 0;
        for (std::size_t index = 0; index != 8; ++index) word = (word << 8) | bytes[offset + index];
        return word;
    }
};

static_assert(sizeof(uuid128_t) == uuid128_t::size_bytes_k, "An identifier is its sixteen bytes and nothing else");

/** Fills @p destination with bytes from the operating system, answering whether it could. */
inline bool fill_with_entropy(std::span<std::uint8_t> destination) noexcept {
#if USTORE_OS_WINDOWS_
    return ::BCryptGenRandom(nullptr, destination.data(), static_cast<ULONG>(destination.size()),
                             BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
#else
    return ::getentropy(destination.data(), destination.size()) == 0;
#endif
}

/**
 *  @brief Mints a version 4 identifier from the entropy the operating system hands out.
 *
 *  Answers a zero identifier, which @c is_valid rejects, when the system refuses entropy; the
 *  caller learns of that through the identifier rather than through a status, since every source
 *  of a fresh identifier already validates it before recording it.
 */
inline uuid128_t uuid_generator() noexcept {
    uuid128_t identifier;
    if (!fill_with_entropy(identifier.bytes)) return {};
    identifier.bytes[6] = static_cast<std::uint8_t>((identifier.bytes[6] & 0x0F) | 0x40);
    identifier.bytes[8] = static_cast<std::uint8_t>((identifier.bytes[8] & 0x3F) | 0x80);
    return identifier;
}

/** The identifier folded onto one word, for a table keyed by a number rather than by the bytes. */
constexpr std::uint64_t uuid_to_u64(uuid128_t const& identifier) noexcept {
    std::uint64_t mixed = identifier.high_word() ^ (identifier.low_word() * 0x9E3779B97F4A7C15ull);
    mixed ^= mixed >> 30;
    mixed *= 0xBF58476D1CE4E5B9ull;
    mixed ^= mixed >> 27;
    mixed *= 0x94D049BB133111EBull;
    return mixed ^ (mixed >> 31);
}

#pragma endregion UUID

#pragma region Locations

/** A key inside a collection, laid out so a strided pointer can address either member. */
struct collection_key_t {

    /** Collection owning the key. */
    ustore_collection_id_t collection {};

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
inline constexpr ustore_length_t missing_length_k = (std::numeric_limits<ustore_length_t>::max)();

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

#pragma region Byte Views

/** Bytes a caller may write into. */
using bytes_span_t = std::span<std::byte>;

/** Bytes a caller may only read. */
using bytes_view_t = std::span<std::byte const>;

/** A value, or the bytes of a string, owned so it outlives the call that wrote it. */
using blob_t = st::vector<ustore_byte_t>;

/** The bytes of @p buffer, which a caller may only read. */
template <typename allocator_type_>
inline bytes_view_t as_bytes_view(st::vector<std::byte, allocator_type_> const& buffer) noexcept {
    return bytes_view_t {buffer};
}

#pragma endregion Byte Views

#pragma region Serialization

/** A value that measures, saves and loads its own bytes, where a successful load consumes its
 *  storage requirements. */
template <typename value_type_>
concept serializable_concept = requires(value_type_ const& constant, value_type_& value, bytes_span_t destination,
                                        bytes_view_t source) {
    { constant.storage_requirements() } noexcept -> std::same_as<std::size_t>;
    { constant.save(destination) } noexcept -> std::same_as<status_t>;
    { value.load(source) } noexcept -> std::same_as<status_t>;
};

/** A value stored as its object representation, having no serialization of its own. */
template <typename value_type_>
concept trivially_serializable_concept = std::is_trivially_copyable_v<value_type_> &&
                                         !serializable_concept<value_type_>;

/** The bytes of a trivially serializable value. */
template <trivially_serializable_concept value_type_>
std::size_t storage_requirements(value_type_ const&) noexcept {
    return sizeof(value_type_);
}

/** Copies the object representation of @p value into @p destination. */
template <trivially_serializable_concept value_type_>
status_t save(value_type_ const& value, bytes_span_t destination) noexcept {
    if (destination.size() < sizeof(value_type_))
        return status_t {status_code_t::invalid_argument_k, "Buffer is too small"};
    std::memcpy(destination.data(), &value, sizeof(value_type_));
    return {};
}

/** Copies the object representation of @p value out of @p source. */
template <trivially_serializable_concept value_type_>
status_t load(value_type_& value, bytes_view_t source) noexcept {
    if (source.size() < sizeof(value_type_)) return status_t {status_code_t::malformed_k, "Corrupted data"};
    std::memcpy(&value, source.data(), sizeof(value_type_));
    return {};
}

/** The bytes a serializable value reports. */
template <serializable_concept value_type_>
std::size_t storage_requirements(value_type_ const& value) noexcept {
    return value.storage_requirements();
}

/** Saves a serializable value through its own member. */
template <serializable_concept value_type_>
status_t save(value_type_ const& value, bytes_span_t destination) noexcept {
    return value.save(destination);
}

/** Loads a serializable value through its own member. */
template <serializable_concept value_type_>
status_t load(value_type_& value, bytes_view_t source) noexcept {
    return value.load(source);
}

/** The bytes of an array: its 64-bit count, then every element. */
template <typename element_type_, typename allocator_type_>
std::size_t storage_requirements(st::vector<element_type_, allocator_type_> const& elements) noexcept {
    std::size_t bytes = sizeof(std::uint64_t);
    for (element_type_ const& element : elements) bytes += storage_requirements(element);
    return bytes;
}

/** Saves the 64-bit count of @p elements, then every element back to back. */
template <typename element_type_, typename allocator_type_>
status_t save(st::vector<element_type_, allocator_type_> const& elements, bytes_span_t destination) noexcept {
    std::uint64_t const count = elements.size();
    if (status_t const status = save(count, destination); status.failed()) return status;
    std::size_t offset = sizeof(count);
    for (element_type_ const& element : elements) {
        if (status_t const status = save(element, destination.subspan(offset)); status.failed()) return status;
        offset += storage_requirements(element);
    }
    return {};
}

/** Loads an array saved as its 64-bit count, then every element back to back. */
template <typename element_type_, typename allocator_type_>
status_t load(st::vector<element_type_, allocator_type_>& elements, bytes_view_t source) noexcept {
    std::uint64_t count = 0;
    if (status_t const status = load(count, source); status.failed()) return status;
    std::size_t offset = sizeof(count);
    // Every element takes at least one byte, so a larger count is corrupt.
    if (count > source.size() - offset) return status_t {status_code_t::malformed_k, "Corrupted data"};
    if (st::status_t const status = elements.resize(count); status != st::status_t::success_k) return to_status(status);
    for (element_type_& element : elements) {
        if (status_t const status = load(element, source.subspan(offset)); status.failed()) return status;
        offset += storage_requirements(element);
    }
    return {};
}

/** The bytes of @p fields stored back to back. */
template <typename... field_types_>
std::size_t fields_storage_requirements(field_types_ const&... fields) noexcept {
    return (std::size_t {0} + ... + storage_requirements(fields));
}

/** Saves no fields. */
inline status_t save_fields(bytes_span_t) noexcept { return {}; }

/** Saves @p field, then @p fields, back to back into @p destination. */
template <typename field_type_, typename... field_types_>
status_t save_fields(bytes_span_t destination, field_type_ const& field, field_types_ const&... fields) noexcept {
    if (status_t const status = save(field, destination); status.failed()) return status;
    return save_fields(destination.subspan(storage_requirements(field)), fields...);
}

/** Loads no fields. */
inline status_t load_fields(bytes_view_t) noexcept { return {}; }

/** Loads @p field, then @p fields, stored back to back in @p source. */
template <typename field_type_, typename... field_types_>
status_t load_fields(bytes_view_t source, field_type_& field, field_types_&... fields) noexcept {
    if (status_t const status = load(field, source); status.failed()) return status;
    return load_fields(source.subspan(storage_requirements(field)), fields...);
}

#pragma endregion Serialization

#pragma region Text

/** Text the engine owns, grown through the fallible members that never throw. */
using string_t = sz::string_t;

/** A view of text the engine does not own. */
using string_view_t = sz::string_view_t;

/** The bytes of a string: its 16-bit length, then its characters. */
inline std::size_t storage_requirements(string_t const& text) noexcept { return sizeof(std::uint16_t) + text.size(); }

/** Saves the 16-bit length of @p text, then its characters. */
inline status_t save(string_t const& text, bytes_span_t destination) noexcept {
    if (text.size() > (std::numeric_limits<std::uint16_t>::max)())
        return status_t {status_code_t::capacity_exhausted_k, "Data is too big"};
    if (destination.size() < storage_requirements(text))
        return status_t {status_code_t::invalid_argument_k, "Buffer is too small"};
    std::uint16_t const length = static_cast<std::uint16_t>(text.size());
    std::memcpy(destination.data(), &length, sizeof(length));
    std::memcpy(destination.data() + sizeof(length), text.data(), length);
    return {};
}

/** Loads a string saved as its 16-bit length, then its characters. */
inline status_t load(string_t& text, bytes_view_t source) noexcept {
    std::uint16_t length = 0;
    if (status_t const status = load(length, source); status.failed()) return status;
    if (source.size() - sizeof(length) < length) return status_t {status_code_t::malformed_k, "Corrupted data"};
    char const* const characters = reinterpret_cast<char const*>(source.data() + sizeof(length));
    if (sz::failed(text.assign(string_view_t {characters, length})))
        return status_t {status_code_t::out_of_memory_k, "No room for a loaded string"};
    return {};
}

#pragma endregion Text

#pragma region Kits and StringZilla Bridges

/**
 *  @brief Picks the first of @p strings_kit_types_ this build compiled and @p capabilities_ runs,
 *      else @p floor_type_, in one closing @c >::type and no nested @c std::conditional_t pile.
 *
 *  Every kit states its own needs: @c capability_k, the capability it runs under,
 *  and @c compiled_k, whether this build carries its body. A kit whose target this
 *  build cannot carry is a stub of those two members alone, so naming it in a
 *  ladder never needs its body. @p capabilities_ is the capability set the kits
 *  state theirs in.
 */
template <auto capabilities_, typename floor_type_, typename... strings_kit_types_>
struct highest_strings_kit {
    using type = floor_type_;
};

template <auto capabilities_, typename floor_type_, typename head_type_, typename... tail_types_>
struct highest_strings_kit<capabilities_, floor_type_, head_type_, tail_types_...> {
    using type =
        std::conditional_t<head_type_::compiled_k && (capabilities_ & head_type_::capability_k) != 0, head_type_,
                           typename highest_strings_kit<capabilities_, floor_type_, tail_types_...>::type>;
};

/** Views @p text as StringZilla's pointer and length. */
inline sz_string_view_t stringzilla_view(std::string_view text) noexcept {
    sz_string_view_t view;
    view.start = text.data();
    view.length = text.size();
    return view;
}

/** The bytes of string @p index of @p tape, which a StringZilla kernel wrote. */
inline std::string_view tape_string(sz_strings_span_t const& tape, std::size_t index) noexcept {
    if (tape.layout == sz_strings_u64_tape_k) {
        sz_u64_t const* const offsets = tape.data.u64_tape.offsets;
        return {tape.data.u64_tape.data + offsets[index],
                static_cast<std::size_t>(offsets[index + 1] - offsets[index])};
    }
    sz_u32_t const* const offsets = tape.data.u32_tape.offsets;
    return {tape.data.u32_tape.data + offsets[index], static_cast<std::size_t>(offsets[index + 1] - offsets[index])};
}

/** The status of a failed StringZilla call: a resource or kernel failure as such, any other as
 *  @p refused, which the caller picks by whose bytes the call read, with @p message saying what
 *  it was doing. */
inline status_t stringzilla_refusal(sz_status_t status, status_code_t refused, char const* message) noexcept {
    switch (status) {
    case sz_bad_alloc_k: return {status_code_t::out_of_memory_k, message};
    case sz_missing_kernel_k: return {status_code_t::unsupported_k, message};
    default: return {refused, message};
    }
}

#pragma endregion Kits and StringZilla Bridges

#pragma region Arithmetic and Hashing

constexpr std::size_t bits_in_byte_k = 8;

/** The power of two just above the highest set bit of @p value. */
inline std::size_t next_power_of_two(std::size_t value) noexcept { return std::bit_floor(value) << 1; }

/** @p value divided by @p divisor and rounded up, without the overflow <tt>value + divisor - 1</tt>
 *  risks near the limit. */
template <std::unsigned_integral integer_type_>
constexpr integer_type_ divide_round_up(integer_type_ value, std::type_identity_t<integer_type_> divisor) noexcept {
    return static_cast<integer_type_>(value / divisor + (value % divisor != 0));
}

/** The greatest multiple of @p multiple at or below @p value. */
template <std::unsigned_integral integer_type_>
constexpr integer_type_ round_down_to_multiple(integer_type_ value,
                                               std::type_identity_t<integer_type_> multiple) noexcept {
    return static_cast<integer_type_>(value - value % multiple);
}

/** The least multiple of @p multiple at or above @p value, saturating at the greatest
 *  multiple @p integer_type_ holds. */
template <std::unsigned_integral integer_type_>
constexpr integer_type_ round_up_to_multiple(integer_type_ value,
                                             std::type_identity_t<integer_type_> multiple) noexcept {
    integer_type_ const below = round_down_to_multiple(value, multiple);
    if (below == value || below > (std::numeric_limits<integer_type_>::max)() - multiple) return below;
    return static_cast<integer_type_>(below + multiple);
}

/**
 *  Mixes the hash of @p value into @p seed.
 *  @see https://www.boost.org/doc/libs/1_37_0/doc/html/hash/reference.html#boost.hash_combine
 */
template <typename hashable_type_>
void hash_combine(std::size_t& seed, hashable_type_ const& value) noexcept {
    std::hash<hashable_type_> hasher;
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

#pragma region GPU Devices

/** The GPU vendors UStore drives, each through a runtime opened on first use, so one binary addresses every vendor's
 *  devices at once and loads where none is installed. */
enum class gpu_vendor_t : std::uint32_t {
    unknown_k = ustore_gpu_vendor_unknown_k,

    /** NVIDIA, through the CUDA driver. */
    nvidia_k = ustore_gpu_vendor_nvidia_k,

    /** AMD, through the HIP runtime. */
    amd_k = ustore_gpu_vendor_amd_k,
};

/** The GPU generations UStore tells apart, the device twin of its CPU classes: a class implies its vendor, and within
 *  a vendor a newer device takes the newest class it satisfies. */
enum class gpu_class_t : std::uint32_t {
    unknown_k = ustore_gpu_class_unknown_k,

    /** Ampere-class NVIDIA and every NVIDIA GPU without a class of its own - A100, A10 & RTX 30. */
    nvidia_ampere_k = ustore_gpu_class_nvidia_ampere_k,

    /** Ada-class NVIDIA - L4, L40S & RTX 40. */
    nvidia_ada_k = ustore_gpu_class_nvidia_ada_k,

    /** Hopper-class NVIDIA - H100 & H200: thread-block clusters and their distributed shared memory. */
    nvidia_hopper_k = ustore_gpu_class_nvidia_hopper_k,

    /** Blackwell-class NVIDIA - B200, B300 & GB300: cluster launch control. */
    nvidia_blackwell_k = ustore_gpu_class_nvidia_blackwell_k,

    /** Consumer Blackwell NVIDIA - RTX 50, RTX PRO 6000 & DGX Spark. */
    nvidia_blackwellrtx_k = ustore_gpu_class_nvidia_blackwellrtx_k,

    /** CDNA3 AMD - MI300X & MI325X: gfx942, 64-wide waves. */
    amd_cdna3_k = ustore_gpu_class_amd_cdna3_k,

    /** CDNA4 AMD - MI350X & MI355X: gfx950. */
    amd_cdna4_k = ustore_gpu_class_amd_cdna4_k,

    /** CDNA5 AMD - MI400: gfx1250, 32-wide waves. */
    amd_cdna5_k = ustore_gpu_class_amd_cdna5_k,
};

/** One GPU as its vendor's driver reports it: the vendor, that vendor's own ordinal, its raw architecture, and what a
 *  listing reports, so no caller asks a vendor SDK; the router maps the architecture to a class. */
struct gpu_device_t {
    gpu_vendor_t vendor = gpu_vendor_t::unknown_k;
    std::uint32_t vendor_ordinal = 0;

    /** The CUDA compute capability or the AMD @c gfx version, major then minor. */
    std::uint32_t architecture_major = 0;
    std::uint32_t architecture_minor = 0;

    /** Streaming multiprocessors or compute units. */
    std::uint32_t compute_units = 0;

    /** Device memory in bytes. */
    std::uint64_t memory_bytes = 0;

    /** The vendor's own name for the device, null-terminated and truncated to fit. */
    char name[64] = {};
};

/** Devices one vendor's listing holds at most, so the process-wide listing needs no heap. */
inline constexpr std::size_t gpu_devices_capacity_k = 64;

#pragma endregion GPU Devices

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
