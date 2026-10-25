/**
 *  @file include/ustore/docs/json_values.hpp
 *  @author Ash Vardanian
 *  @date December 26, 2022
 *  @brief JSON documents parsed with yyjson into an arena, the conversions between their values and the fixed-width
 *      scalars, strings and UUIDs a column of a gather holds, and the keys documents name in an id field.
 *
 *  Every document parses into the arena it is given, which owns all of its memory, so nothing is freed one allocation
 *  at a time. A conversion reports how faithfully a value fits its column: exactly, converted, colliding with the
 *  column's range, or missing.
 */

#pragma once

#include <cmath>   // `std::abs`, `std::isfinite`, `std::ldexp`
#include <cstddef> // `std::size_t`, `std::max_align_t`
#include <cstdint> // `std::uint8_t`, `std::uint16_t`, `std::uint32_t`
#include <cstring> // `std::memcpy`, `std::memset`

#include <bit>          // `std::bit_cast`
#include <charconv>     // `std::from_chars`
#include <limits>       // `std::numeric_limits`
#include <span>         // `std::span`
#include <string_view>  // `std::string_view`
#include <system_error> // `std::errc`
#include <type_traits>  // `std::is_same_v`
#include <utility>      // `std::in_range`

#include <fmt/format.h> // `fmt::format_to_n`
#include <yyjson.h>     // `yyjson_read_opts`, `yyjson_mut_doc`

#include "ustore/utilities/ram_buffers.hpp" // `arena_allocate`, `arena_grow`
#include "ustore/utilities/types.hpp"       // `value_view_t`, `expected`, `status_t`

namespace unum::ustore::docs {

using utilities::arena_allocate;
using utilities::arena_grow;
using utilities::byte_t;
using utilities::expected;
using utilities::status_code_t;
using utilities::status_t;
using utilities::uuid_text_length_k;
using utilities::value_view_t;

#pragma region Number Formatting

/** Longest text a printed number takes, including the null terminator. */
constexpr std::size_t printed_number_length_limit_k = 32;

/** Bytes of a UUID. */
constexpr std::size_t uuid_bytes_k = 16;

using printed_number_buffer_t = char[printed_number_length_limit_k];

constexpr std::string_view true_k = "true";
constexpr std::string_view false_k = "false";

/** Parses the whole of @p text as a number or a boolean, answering whether every character was consumed. */
template <typename scalar_>
bool parse_entire_number(std::string_view text, scalar_& scalar) noexcept {
    if constexpr (std::is_same_v<scalar_, bool>) {
        if (text != true_k && text != false_k) return false;
        scalar = text == true_k;
        return true;
    }
    else {
        auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), scalar);
        return error == std::errc() && end == text.data() + text.size();
    }
}

/** Prints @p scalar between @p begin and @p end with a null terminator, or an empty view when it does not fit. */
template <typename scalar_>
std::string_view print_number(char* begin, char* end, scalar_ scalar) noexcept {
    std::size_t const capacity = static_cast<std::size_t>(end - begin);
    auto const [written_end, written_size] = fmt::format_to_n(begin, capacity - 1, "{}", scalar);
    if (written_size >= capacity) return {};
    *written_end = '\0';
    return {begin, written_size};
}

#pragma endregion Number Formatting

#pragma region JSON Handles

/** Document parsed into the scratch of a call, which owns all of its memory. */
struct json_t {

    /** Document as read, or @c nullptr. */
    yyjson_doc* handle = nullptr;

    /** Mutable copy of the document, or @c nullptr until a modification needs one. */
    yyjson_mut_doc* mutable_handle = nullptr;

    explicit operator bool() const noexcept { return handle || mutable_handle; }
};

/** Value inside a parsed or a mutable document. */
struct json_value_t {

    /** Value inside a parsed document, or @c nullptr. */
    yyjson_val* handle = nullptr;

    /** Value inside a mutable document, or @c nullptr. */
    yyjson_mut_val* mutable_handle = nullptr;

    explicit operator bool() const noexcept { return handle || mutable_handle; }
};

inline void* json_allocate(void* context, std::size_t size) noexcept {
    auto [bytes, status] = arena_allocate<byte_t>(static_cast<ustore_arena_t>(context), size,
                                                  alignof(std::max_align_t));
    return bytes.data();
}

inline void* json_reallocate(void* context, void* pointer, std::size_t old_size, std::size_t size) noexcept {
    if (size <= old_size) return pointer;
    std::span<byte_t> const old_bytes {static_cast<byte_t*>(pointer), old_size};
    auto [bytes, status] = arena_grow(static_cast<ustore_arena_t>(context), old_bytes, size - old_size,
                                      alignof(std::max_align_t));
    return bytes.data();
}

/** Arena memory is released with the arena, never one allocation at a time. */
inline void json_free(void*, void*) noexcept {}

inline yyjson_alc json_allocator(ustore_arena_t arena) noexcept {
    return {json_allocate, json_reallocate, json_free, arena};
}

/** Finds @p field in @p json, as a JSON Pointer when it starts with a slash and as a top-level key otherwise, where an
 *  empty field addresses the whole document. */
inline yyjson_val* json_lookup(yyjson_val* json, std::string_view field) noexcept {
    if (field.empty()) return json;
    return field.starts_with('/') ? yyjson_ptr_getn(json, field.data(), field.size())
                                  : yyjson_obj_getn(json, field.data(), field.size());
}

/** Characters @p key takes as an RFC 6901 reference token, where a tilde and a slash each take two. */
inline std::size_t escaped_length(std::string_view key) noexcept {
    std::size_t length = key.size();
    for (char const character : key) length += character == '~' || character == '/';
    return length;
}

/** Appends @p key as an RFC 6901 reference token at @p output, answering where the token ends. */
inline char* escape_into(std::string_view key, char* output) noexcept {
    for (char const character : key) {
        if (character != '~' && character != '/') {
            *output++ = character;
            continue;
        }
        *output++ = '~';
        *output++ = character == '~' ? '0' : '1';
    }
    return output;
}

/** Spells @p field as a JSON Pointer, escaping a top-level key into @p arena. */
inline expected<std::string_view> field_pointer(std::string_view field, ustore_arena_t arena) noexcept {
    std::string_view const key = field;
    if (key.starts_with('/')) return key;

    auto [pointer, status] = arena_allocate<char>(arena, escaped_length(key) + 1);
    if (status.failed()) return status;

    pointer[0] = '/';
    char const* const end = escape_into(key, pointer.data() + 1);
    return std::string_view {pointer.data(), static_cast<std::size_t>(end - pointer.data())};
}

/** Parses a JSON document, leaving the result empty for an empty or a missing value. */
inline expected<json_t> json_parse(value_view_t bytes, ustore_arena_t arena) noexcept {
    if (bytes.empty()) return json_t {};

    yyjson_alc const allocator = json_allocator(arena);
    yyjson_read_flag const flags = YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_INF_AND_NAN;
    // yyjson writes into its input only with `YYJSON_READ_INSITU`, which is never set.
    char* const text = const_cast<char*>(bytes.c_str());
    yyjson_doc* const handle = yyjson_read_opts(text, bytes.size(), flags, &allocator, nullptr);
    if (!handle) return status_t {status_code_t::malformed_k, "Failed to parse the document"};
    return json_t {handle, nullptr};
}

/** Makes @p document mutable, copying what was parsed or starting an empty document. */
inline expected<yyjson_mut_doc*> json_mutable(json_t& document, ustore_arena_t arena) noexcept {
    if (document.mutable_handle) return document.mutable_handle;

    yyjson_alc const allocator = json_allocator(arena);
    document.mutable_handle = document.handle //
                                  ? yyjson_doc_mut_copy(document.handle, &allocator)
                                  : yyjson_mut_doc_new(&allocator);
    if (!document.mutable_handle)
        return status_t {status_code_t::out_of_memory_k, "Failed to allocate a mutable document"};
    return document.mutable_handle;
}

/** Prints @p value as JSON text into @p arena, or answers a missing value when there is none. */
inline expected<value_view_t> json_dump(json_value_t value, ustore_arena_t arena) noexcept {
    if (!value) return value_view_t {};

    yyjson_alc const allocator = json_allocator(arena);
    yyjson_write_flag const flags = YYJSON_WRITE_ALLOW_INF_AND_NAN;
    std::size_t length = 0;
    char const* const text = value.mutable_handle
                                 ? yyjson_mut_val_write_opts(value.mutable_handle, flags, &allocator, &length, nullptr)
                                 : yyjson_val_write_opts(value.handle, flags, &allocator, &length, nullptr);
    if (!text) return status_t {status_code_t::out_of_memory_k, "Failed to serialize the document"};
    return value_view_t {text, length};
}

#pragma endregion JSON Handles

#pragma region Conversions

/** How faithfully a value under a field converts into the requested type. */
enum class conversion_t {
    missing_k,
    exact_k,
    converted_k,
    collides_k,
};

/** Whether @p conversion produced a value the column holds. */
inline bool is_valid(conversion_t conversion) noexcept {
    return conversion == conversion_t::exact_k || conversion == conversion_t::converted_k;
}

/** Records @p conversion of one cell, setting the validity bit last since the other bitmaps may alias it. */
inline void record_conversion(conversion_t conversion, ustore_octet_t mask, ustore_octet_t& valid,
                              ustore_octet_t& converted, ustore_octet_t& collides) noexcept {
    ustore_octet_t const cleared = static_cast<ustore_octet_t>(~mask);
    converted = static_cast<ustore_octet_t>((converted & cleared) |
                                            (conversion == conversion_t::converted_k ? mask : 0));
    collides = static_cast<ustore_octet_t>((collides & cleared) | (conversion == conversion_t::collides_k ? mask : 0));
    valid = static_cast<ustore_octet_t>((valid & cleared) | (is_valid(conversion) ? mask : 0));
}

/** Whether @p real, with its fraction dropped, fits the range of @p scalar_, where NaN fits only a float. */
template <typename scalar_>
bool real_fits(double real) noexcept {
    if constexpr (std::is_same_v<scalar_, bool>) return true;
    else if constexpr (std::is_floating_point_v<scalar_>)
        return !std::isfinite(real) || std::abs(real) <= static_cast<double>(std::numeric_limits<scalar_>::max());
    else if constexpr (std::is_signed_v<scalar_>) {
        double const bound = std::ldexp(1.0, std::numeric_limits<scalar_>::digits);
        return real >= -bound && real < bound;
    }
    else return real > -1.0 && real < std::ldexp(1.0, std::numeric_limits<scalar_>::digits);
}

/** Whether @p integer lies inside the range of @p scalar_, which a boolean or a float always covers. */
template <typename scalar_, typename integer_>
bool integer_fits(integer_ integer) noexcept {
    if constexpr (std::is_same_v<scalar_, bool> || std::is_floating_point_v<scalar_>) return true;
    else return std::in_range<scalar_>(integer);
}

/** Converts @p value into @p scalar, where a value outside the range of the scalar collides. */
template <typename scalar_>
conversion_t json_to_scalar(yyjson_val* value, scalar_& scalar) noexcept {
    if (!value) return conversion_t::missing_k;

    switch (yyjson_get_type(value)) {
    case YYJSON_TYPE_NULL: return conversion_t::missing_k;

    case YYJSON_TYPE_BOOL:
        scalar = static_cast<scalar_>(yyjson_get_bool(value));
        return std::is_same_v<scalar_, bool> ? conversion_t::exact_k : conversion_t::converted_k;

    case YYJSON_TYPE_STR: {
        std::string_view const text {yyjson_get_str(value), yyjson_get_len(value)};
        return parse_entire_number(text, scalar) ? conversion_t::converted_k : conversion_t::collides_k;
    }

    case YYJSON_TYPE_NUM:
        switch (yyjson_get_subtype(value)) {
        case YYJSON_SUBTYPE_REAL:
            if (!real_fits<scalar_>(yyjson_get_real(value))) return conversion_t::collides_k;
            scalar = static_cast<scalar_>(yyjson_get_real(value));
            return std::is_floating_point_v<scalar_> ? conversion_t::exact_k : conversion_t::converted_k;
        case YYJSON_SUBTYPE_SINT:
            if (!integer_fits<scalar_>(yyjson_get_sint(value))) return conversion_t::collides_k;
            scalar = static_cast<scalar_>(yyjson_get_sint(value));
            return std::is_integral_v<scalar_> && std::is_signed_v<scalar_> ? conversion_t::exact_k
                                                                            : conversion_t::converted_k;
        case YYJSON_SUBTYPE_UINT:
            if (!integer_fits<scalar_>(yyjson_get_uint(value))) return conversion_t::collides_k;
            scalar = static_cast<scalar_>(yyjson_get_uint(value));
            return std::is_unsigned_v<scalar_> && !std::is_same_v<scalar_, bool> ? conversion_t::exact_k
                                                                                 : conversion_t::converted_k;
        default: return conversion_t::collides_k;
        }

    default: return conversion_t::collides_k;
    }
}

/** Converts @p value into @p text, printing numbers into @p print_buffer. */
inline conversion_t json_to_string(yyjson_val* value, printed_number_buffer_t& print_buffer,
                                   std::string_view& text) noexcept {
    if (!value) return conversion_t::missing_k;

    char* const buffer_end = print_buffer + printed_number_length_limit_k;
    switch (yyjson_get_type(value)) {
    case YYJSON_TYPE_NULL: return conversion_t::missing_k;

    case YYJSON_TYPE_BOOL: text = yyjson_get_bool(value) ? true_k : false_k; return conversion_t::converted_k;

    case YYJSON_TYPE_STR: text = {yyjson_get_str(value), yyjson_get_len(value)}; return conversion_t::exact_k;

    case YYJSON_TYPE_NUM:
        switch (yyjson_get_subtype(value)) {
        case YYJSON_SUBTYPE_REAL: text = print_number(print_buffer, buffer_end, yyjson_get_real(value)); break;
        case YYJSON_SUBTYPE_SINT: text = print_number(print_buffer, buffer_end, yyjson_get_sint(value)); break;
        case YYJSON_SUBTYPE_UINT: text = print_number(print_buffer, buffer_end, yyjson_get_uint(value)); break;
        default: return conversion_t::collides_k;
        }
        return text.empty() ? conversion_t::collides_k : conversion_t::converted_k;

    default: return conversion_t::collides_k;
    }
}

/** Wraps the fixed-width scalar stored in @p bytes into a mutable value, or returns @c nullptr for too few bytes. */
template <typename scalar_>
yyjson_mut_val* mutable_scalar(yyjson_mut_doc* document, value_view_t bytes) noexcept {
    if (bytes.size() < sizeof(scalar_)) return nullptr;
    scalar_ scalar {};
    std::memcpy(&scalar, bytes.data(), sizeof(scalar_));

    if constexpr (std::is_same_v<scalar_, bool>) return yyjson_mut_bool(document, scalar);
    else if constexpr (std::is_floating_point_v<scalar_>) return yyjson_mut_real(document, scalar);
    else if constexpr (std::is_signed_v<scalar_>) return yyjson_mut_sint(document, scalar);
    else return yyjson_mut_uint(document, scalar);
}

/** Parses @p bytes of @p type into a document, wrapping a scalar into a document of its own. */
inline expected<json_t> any_parse(value_view_t bytes, ustore_docs_field_type_t type, ustore_arena_t arena) noexcept {
    if (type == ustore_docs_field_type_json_k) return json_parse(bytes, arena);

    json_t document;
    yyjson_alc const allocator = json_allocator(arena);
    document.mutable_handle = yyjson_mut_doc_new(&allocator);
    if (!document.mutable_handle)
        return status_t {status_code_t::out_of_memory_k, "Failed to allocate a mutable document"};

    yyjson_mut_doc* const handle = document.mutable_handle;
    yyjson_mut_val* root = nullptr;
    switch (type) {
    case ustore_docs_field_type_null_k: root = yyjson_mut_null(handle); break;
    case ustore_docs_field_type_bool_k: root = mutable_scalar<bool>(handle, bytes); break;
    case ustore_docs_field_type_f64_k: root = mutable_scalar<double>(handle, bytes); break;
    case ustore_docs_field_type_f32_k: root = mutable_scalar<float>(handle, bytes); break;
    case ustore_docs_field_type_i64_k: root = mutable_scalar<std::int64_t>(handle, bytes); break;
    case ustore_docs_field_type_i32_k: root = mutable_scalar<std::int32_t>(handle, bytes); break;
    case ustore_docs_field_type_i16_k: root = mutable_scalar<std::int16_t>(handle, bytes); break;
    case ustore_docs_field_type_i8_k: root = mutable_scalar<std::int8_t>(handle, bytes); break;
    case ustore_docs_field_type_u64_k: root = mutable_scalar<std::uint64_t>(handle, bytes); break;
    case ustore_docs_field_type_u32_k: root = mutable_scalar<std::uint32_t>(handle, bytes); break;
    case ustore_docs_field_type_u16_k: root = mutable_scalar<std::uint16_t>(handle, bytes); break;
    case ustore_docs_field_type_u8_k: root = mutable_scalar<std::uint8_t>(handle, bytes); break;
    case ustore_docs_field_type_str_k: root = yyjson_mut_strncpy(handle, bytes.c_str(), bytes.size()); break;
    default: break;
    }

    if (!root) return status_t {status_code_t::unsupported_k, "Input type is not supported"};
    yyjson_mut_doc_set_root(handle, root);
    return document;
}

#pragma endregion Conversions

#pragma region Gather

/** Bytes of the scalar a column of @p type holds per document, or zero for a column of strings or of JSON. */
inline std::size_t field_size_bytes(ustore_docs_field_type_t type) noexcept {
    switch (type) {
    case ustore_docs_field_type_bool_k: return sizeof(bool);
    case ustore_docs_field_type_uuid_k: return uuid_bytes_k;
    case ustore_docs_field_type_f64_k: return sizeof(double);
    case ustore_docs_field_type_f32_k: return sizeof(float);
    case ustore_docs_field_type_f16_k: return sizeof(std::uint16_t);
    case ustore_docs_field_type_i64_k: return sizeof(std::int64_t);
    case ustore_docs_field_type_i32_k: return sizeof(std::int32_t);
    case ustore_docs_field_type_i16_k: return sizeof(std::int16_t);
    case ustore_docs_field_type_i8_k: return sizeof(std::int8_t);
    case ustore_docs_field_type_u64_k: return sizeof(std::uint64_t);
    case ustore_docs_field_type_u32_k: return sizeof(std::uint32_t);
    case ustore_docs_field_type_u16_k: return sizeof(std::uint16_t);
    case ustore_docs_field_type_u8_k: return sizeof(std::uint8_t);
    default: return 0;
    }
}

/** Converts @p value into the scalar at @p index of @p column, zeroing a cell that holds no valid value. */
template <typename scalar_>
conversion_t gather_scalar(yyjson_val* value, ustore_byte_t* column, std::size_t index) noexcept {
    scalar_ scalar {};
    conversion_t const conversion = json_to_scalar(value, scalar);
    if (!is_valid(conversion)) scalar = {};
    std::memcpy(column + index * sizeof(scalar_), &scalar, sizeof(scalar_));
    return conversion;
}

/** Smallest magnitude that rounds past the largest finite half-precision float. */
constexpr float half_overflow_k = 65520.0f;

/** Encodes a finite @p single below @c half_overflow_k as IEEE 754 binary16, rounding to nearest. */
inline std::uint16_t half_of(float single) noexcept {
    auto const bits = std::bit_cast<std::uint32_t>(single);
    auto const sign = static_cast<std::uint16_t>((bits >> 16) & 0x8000u);
    int const exponent = static_cast<int>((bits >> 23) & 0xffu) - 127 + 15;
    std::uint32_t mantissa = bits & 0x7fffffu;

    if (((bits >> 23) & 0xffu) == 0xffu) return static_cast<std::uint16_t>(sign | 0x7c00u | (mantissa ? 0x200u : 0u));
    if (exponent >= 1) {
        auto const half = static_cast<std::uint16_t>(sign | (exponent << 10) | (mantissa >> 13));
        return static_cast<std::uint16_t>(half + ((mantissa >> 12) & 1u));
    }
    if (exponent < -10) return sign;

    // Subnormal halves shift the implicit leading bit into the mantissa
    mantissa |= 0x800000u;
    auto const shift = static_cast<unsigned>(14 - exponent);
    auto const half = static_cast<std::uint16_t>(sign | (mantissa >> shift));
    return static_cast<std::uint16_t>(half + ((mantissa >> (shift - 1)) & 1u));
}

/** Converts @p value into the half-precision float at @p index of @p column, where a magnitude past its range
 *  collides. */
inline conversion_t gather_half(yyjson_val* value, ustore_byte_t* column, std::size_t index) noexcept {
    float single = 0;
    conversion_t conversion = json_to_scalar(value, single);
    if (is_valid(conversion) && std::isfinite(single) && std::abs(single) >= half_overflow_k)
        conversion = conversion_t::collides_k;
    std::uint16_t const half = is_valid(conversion) ? half_of(single) : 0;
    std::memcpy(column + index * sizeof(half), &half, sizeof(half));
    return conversion;
}

/** Parses canonical UUID text such as "123e4567-e89b-12d3-a456-426614174000" into the bytes at @p index of @p column,
 *  in the order they are written. */
inline conversion_t gather_uuid(yyjson_val* value, ustore_byte_t* column, std::size_t index) noexcept {
    std::uint8_t parsed[uuid_bytes_k] = {};
    ustore_byte_t* const cell = column + index * uuid_bytes_k;
    std::memset(cell, 0, uuid_bytes_k);

    if (!value || yyjson_is_null(value)) return conversion_t::missing_k;
    if (!yyjson_is_str(value) || yyjson_get_len(value) != uuid_text_length_k) return conversion_t::collides_k;

    char const* const text = yyjson_get_str(value);
    std::size_t parsed_count = 0;
    for (std::size_t position = 0; position != uuid_text_length_k; position += 2) {
        if (position == 8 || position == 13 || position == 18 || position == 23) {
            if (text[position] != '-') return conversion_t::collides_k;
            ++position;
        }
        auto const [end, failure] = std::from_chars(text + position, text + position + 2, parsed[parsed_count], 16);
        if (failure != std::errc() || end != text + position + 2) return conversion_t::collides_k;
        ++parsed_count;
    }
    std::memcpy(cell, parsed, uuid_bytes_k);
    return conversion_t::converted_k;
}

#pragma endregion Gather

#pragma region Keys

/** Reads @p text as a UUID in its canonical 8-4-4-4-12 hexadecimal form into @p uuid, answering whether it is one. */
inline bool parse_uuid(std::string_view text, ustore_uuid_t& uuid) noexcept {
    if (text.size() != uuid_text_length_k) return false;
    std::size_t byte_index = 0;
    for (std::size_t index = 0; index != text.size();) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (text[index++] != '-') return false;
            continue;
        }
        char const* const pair = text.data() + index;
        auto const [end, parse_error] = std::from_chars(pair, pair + 2, uuid.bytes[byte_index++], 16);
        if (parse_error != std::errc {} || end != pair + 2) return false;
        index += 2;
    }
    return true;
}

/** Reads the key of every one of @p documents from its @p id_field into @p keys: a non-negative integer, spelled as
 *  the key of that integer, or a UUID in canonical text. */
template <typename documents_type_>
status_t extract_keys(documents_type_ const& documents, std::string_view id_field, ustore_arena_t arena,
                      std::span<ustore_key_t> keys) noexcept {
    for (std::size_t index = 0; index != keys.size(); ++index) {
        auto [document, status] = json_parse(value_view_t {documents[index]}, arena);
        if (status.failed()) return status;
        yyjson_val* const identifier = json_lookup(yyjson_doc_get_root(document.handle), id_field);
        if (yyjson_is_uint(identifier)) {
            keys[index] = utilities::key_from_integer(yyjson_get_uint(identifier));
            continue;
        }
        bool const is_uuid = yyjson_is_str(identifier) &&
                             parse_uuid({yyjson_get_str(identifier), yyjson_get_len(identifier)}, keys[index].uuid);
        if (!is_uuid)
            return {status_code_t::malformed_k, "The id field holds neither a non-negative integer nor a UUID"};
    }
    return {};
}

#pragma endregion Keys

} // namespace unum::ustore::docs
