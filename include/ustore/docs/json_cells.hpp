/**
 *  @file include/ustore/docs/json_cells.hpp
 *  @author Ash Vardanian
 *  @date November 5, 2026
 *  @brief How one field of one document converts into the row of a column or of a read, as
 *      constexpr steps the host gather shares and every device runs alike, beside the vocabulary of
 *      conversions both residencies report.
 *
 *  A cell is the JSON token a subdocuments walk located, and its first byte tells what it holds.
 *  These steps convert a cell exactly where one step of IEEE 754 arithmetic reproduces what yyjson
 *  and @c std::from_chars answer: integers within 64 bits, and decimals whose digits fit the
 *  significand under a power of ten up to 22, or 10 for a float. Every other cell they cannot
 *  answer alone raises the refusal they return, for the host to serve the call: a real past that,
 *  one printed as text, a string a number or UUID reads through an escape, ill-formed UTF-8 and
 *  malformed escapes, which the host then refuses itself.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint16_t`, `std::uint32_t`, `std::uint64_t`, `std::int64_t`

#include <array>       // `std::array`
#include <bit>         // `std::bit_cast`
#include <limits>      // `std::numeric_limits`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::is_same_v`, `std::is_floating_point_v`, `std::is_signed_v`
#include <utility>     // `std::in_range`

#include "ustore/docs.h"              // `ustore_octet_t`, `ustore_docs_field_type_t`
#include "ustore/utilities/types.hpp" // `uuid_text_length_k`

namespace unum::ustore::docs {

using utilities::uuid_text_length_k;

#pragma region Conversions

/** Bytes of a UUID. */
inline constexpr std::size_t uuid_bytes_k = 16;

/** How faithfully a value under a field converts into the requested type. */
enum class conversion_t {
    missing_k,
    exact_k,
    converted_k,
    collides_k,
};

/** Whether @p conversion produced a value the column holds. */
constexpr bool is_valid(conversion_t conversion) noexcept {
    return conversion == conversion_t::exact_k || conversion == conversion_t::converted_k;
}

/** Records @p conversion of one cell, setting validity last, as other bitmaps may alias it. */
constexpr void record_conversion(conversion_t conversion, ustore_octet_t mask, ustore_octet_t& valid,
                                 ustore_octet_t& converted, ustore_octet_t& collides) noexcept {
    ustore_octet_t const cleared = static_cast<ustore_octet_t>(~mask);
    converted = static_cast<ustore_octet_t>((converted & cleared) |
                                            (conversion == conversion_t::converted_k ? mask : 0));
    collides = static_cast<ustore_octet_t>((collides & cleared) | (conversion == conversion_t::collides_k ? mask : 0));
    valid = static_cast<ustore_octet_t>((valid & cleared) | (is_valid(conversion) ? mask : 0));
}

/** What one field of one document holds, told by the first byte of its JSON token. */
enum class json_token_t : std::uint8_t {

    /** No member of the document has the field, so the cell is null. */
    missing_k = 0,

    /** A JSON @c null. */
    null_k = 1,

    /** A JSON @c false. */
    false_k = 2,

    /** A JSON @c true. */
    true_k = 3,

    /** A JSON number. */
    number_k = 4,

    /** A JSON string. */
    string_k = 5,

    /** A JSON object, as its raw bytes. */
    object_k = 6,

    /** A JSON array, as its raw bytes. */
    array_k = 7,

    /** A token of a malformed document, or a string whose escapes do not decode, which is null. */
    malformed_k = 8,
};

/** What @p token holds, told by its first byte, every number before it is read. */
constexpr json_token_t json_token_of(std::string_view token) noexcept {
    if (token.empty()) return json_token_t::malformed_k;
    switch (token[0]) {
    case '"': return json_token_t::string_k;
    case '{': return json_token_t::object_k;
    case '[': return json_token_t::array_k;
    case 't': return json_token_t::true_k;
    case 'f': return json_token_t::false_k;
    case 'n': return json_token_t::null_k;
    default: return json_token_t::number_k;
    }
}

/** Bytes of the scalar a column of @p type holds per document, or zero for strings or JSON. */
constexpr std::size_t field_size_bytes(ustore_docs_field_type_t type) noexcept {
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

/** Smallest magnitude that rounds past the largest finite half-precision float. */
inline constexpr float half_overflow_k = 65520.0f;

/** Encodes a finite @p single below @c half_overflow_k as IEEE 754 binary16, rounded to nearest. */
constexpr std::uint16_t half_of(float single) noexcept {
    std::uint32_t const bits = std::bit_cast<std::uint32_t>(single);
    std::uint16_t const sign = static_cast<std::uint16_t>((bits >> 16) & 0x8000u);
    int const exponent = static_cast<int>((bits >> 23) & 0xffu) - 127 + 15;
    std::uint32_t mantissa = bits & 0x7fffffu;

    if (((bits >> 23) & 0xffu) == 0xffu) return static_cast<std::uint16_t>(sign | 0x7c00u | (mantissa ? 0x200u : 0u));
    if (exponent >= 1) {
        std::uint16_t const half = static_cast<std::uint16_t>(sign | (exponent << 10) | (mantissa >> 13));
        return static_cast<std::uint16_t>(half + ((mantissa >> 12) & 1u));
    }
    if (exponent < -10) return sign;

    // Subnormal halves shift the implicit leading bit into the mantissa
    mantissa |= 0x800000u;
    unsigned const shift = static_cast<unsigned>(14 - exponent);
    std::uint16_t const half = static_cast<std::uint16_t>(sign | (mantissa >> shift));
    return static_cast<std::uint16_t>(half + ((mantissa >> (shift - 1)) & 1u));
}

#pragma endregion Conversions

#pragma region Cell Numbers

/** The number kind a cell spells as yyjson reads it, or a refusal where only the host is exact. */
enum class cell_number_kind_t : std::uint8_t { unsigned_k, signed_k, real_k, refused_k };

/** The number one cell spells, in the member its @c kind names. */
struct cell_number_t {
    cell_number_kind_t kind = cell_number_kind_t::refused_k;
    std::uint64_t unsigned_integer = 0;
    std::int64_t signed_integer = 0;
    double real = 0;
};

/** The spellings a decimal reads: RFC 8259 numbers, or the general format of @c std::from_chars. */
enum class decimal_grammar_t : std::uint8_t { json_k, from_chars_k };

/** How a decimal reads: malformed, an integer without a point or an exponent, a decimal, or one
 *  with more significant digits than 64 bits hold. */
enum class decimal_reading_t : std::uint8_t { malformed_k, integer_k, fraction_k, too_precise_k };

/** A decimal's spelling in parts: its significant digits as one integer and their power of ten. */
struct decimal_t {
    decimal_reading_t reading = decimal_reading_t::malformed_k;
    bool negative = false;
    std::uint64_t significand = 0;
    std::int64_t exponent = 0;
};

/** Significant digits a 64-bit significand holds whatever they are. */
inline constexpr unsigned decimal_digits_max_k = 19;

/** A magnitude past which an exponent's digits stop mattering to any number a cell converts. */
inline constexpr std::int64_t decimal_exponent_max_k = 1000000;

/** Whether @p character is a decimal digit. */
constexpr bool is_digit(char character) noexcept { return character >= '0' && character <= '9'; }

/** Whether @p text spells @p literal bytewise, as devices link no @c memcmp or @c strlen. */
template <std::size_t length_>
constexpr bool spells(std::string_view text, char const (&literal)[length_]) noexcept {
    if (text.size() != length_ - 1) return false;
    for (std::size_t index = 0; index != text.size(); ++index)
        if (text[index] != literal[index]) return false;
    return true;
}

/** Reads the whole of @p text as a decimal of @p grammar. */
constexpr decimal_t read_decimal(std::string_view text, decimal_grammar_t grammar) noexcept {
    bool const json = grammar == decimal_grammar_t::json_k;
    decimal_t decimal;
    std::size_t position = 0;
    decimal.negative = position != text.size() && text[position] == '-';
    position += decimal.negative;
    // JSON spells a zero integer part alone, never with leading zeros
    if (json && text.size() - position > 1 && text[position] == '0' && is_digit(text[position + 1])) return decimal;

    // Leading zeros carry no digit, and digits past 64 bits only mark the decimal too precise
    unsigned significant = 0;
    std::size_t integer_digits = 0, fraction_digits = 0;
    for (; position != text.size() && is_digit(text[position]); ++position, ++integer_digits) {
        if (!decimal.significand && text[position] == '0') continue;
        if (significant++ < decimal_digits_max_k)
            decimal.significand = decimal.significand * 10 + static_cast<std::uint64_t>(text[position] - '0');
        else ++decimal.exponent;
    }
    bool const pointed = position != text.size() && text[position] == '.';
    position += pointed;
    for (; pointed && position != text.size() && is_digit(text[position]); ++position, ++fraction_digits) {
        if (!decimal.significand && text[position] == '0') {
            --decimal.exponent;
            continue;
        }
        if (significant++ >= decimal_digits_max_k) continue;
        decimal.significand = decimal.significand * 10 + static_cast<std::uint64_t>(text[position] - '0');
        --decimal.exponent;
    }
    if (!integer_digits && !fraction_digits) return decimal_t {};
    if (json && (!integer_digits || (pointed && !fraction_digits))) return decimal_t {};

    bool const scaled = position != text.size() && (text[position] == 'e' || text[position] == 'E');
    if (scaled) {
        ++position;
        bool const negative_exponent = position != text.size() && text[position] == '-';
        position += position != text.size() && (text[position] == '-' || text[position] == '+');
        std::size_t const first = position;
        std::int64_t written = 0;
        for (; position != text.size() && is_digit(text[position]); ++position)
            if (written < decimal_exponent_max_k) written = written * 10 + (text[position] - '0');
        if (position == first) return decimal_t {};
        decimal.exponent += negative_exponent ? -written : written;
    }
    if (position != text.size()) return decimal_t {};
    decimal.reading = significant > decimal_digits_max_k ? decimal_reading_t::too_precise_k
                      : pointed || scaled                ? decimal_reading_t::fraction_k
                                                         : decimal_reading_t::integer_k;
    return decimal;
}

/** Ten to the @p power as @c real_type_, exact while @c real_type_ holds it. */
template <typename real_type_>
constexpr real_type_ power_of_ten(std::int64_t power) noexcept {
    real_type_ result = 1;
    for (std::int64_t step = 0; step != power; ++step) result *= 10;
    return result;
}

/** The decimal @p decimal names as @c real_type_ rounded once to nearest, where its significand and
 *  power of ten are both exact, raising @p refused for any other. */
template <typename real_type_>
constexpr real_type_ exact_real(decimal_t const& decimal, std::uint32_t& refused) noexcept {
    constexpr bool single = std::is_same_v<real_type_, float>;
    constexpr std::uint64_t significand_max = std::uint64_t {1} << std::numeric_limits<real_type_>::digits;
    constexpr std::int64_t power_max = single ? 10 : 22;
    if (!decimal.significand) return decimal.negative ? -real_type_ {0} : real_type_ {0};
    std::int64_t const power = decimal.exponent < 0 ? -decimal.exponent : decimal.exponent;
    if (decimal.reading == decimal_reading_t::too_precise_k || decimal.significand > significand_max ||
        power > power_max) {
        refused = 1;
        return 0;
    }
    real_type_ const significand = static_cast<real_type_>(decimal.significand);
    real_type_ const scale = power_of_ten<real_type_>(power);
    real_type_ const magnitude = decimal.exponent < 0 ? significand / scale : significand * scale;
    return decimal.negative ? -magnitude : magnitude;
}

/** Reads @p text as decimal digits after an optional minus, setting @p negative, into @p magnitude,
 *  answering whether it spelled an integer within 64 bits. */
constexpr bool read_magnitude(std::string_view text, std::uint64_t& magnitude, bool& negative) noexcept {
    std::size_t position = 0;
    negative = position != text.size() && text[position] == '-';
    position += negative;
    if (position == text.size()) return false;
    magnitude = 0;
    for (; position != text.size(); ++position) {
        if (!is_digit(text[position])) return false;
        std::uint64_t const digit = static_cast<std::uint64_t>(text[position] - '0');
        if (magnitude > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return false;
        magnitude = magnitude * 10 + digit;
    }
    return true;
}

/** Reads the number @p token as yyjson does in a document, refusing what one rounding misreads. */
constexpr cell_number_t read_cell_number(std::string_view token) noexcept {
    cell_number_t number;
    decimal_t const decimal = read_decimal(token, decimal_grammar_t::json_k);
    if (decimal.reading == decimal_reading_t::malformed_k) return number;
    if (decimal.reading != decimal_reading_t::fraction_k) {
        std::uint64_t magnitude = 0;
        bool negative = false;
        // An integer past 64 bits reads as a real, which takes the host's rounding
        if (!read_magnitude(token, magnitude, negative)) return number;
        constexpr std::uint64_t signed_magnitude_max = std::uint64_t {1} << 63;
        if (!negative) {
            number.kind = cell_number_kind_t::unsigned_k, number.unsigned_integer = magnitude;
            return number;
        }
        if (magnitude > signed_magnitude_max) return number;
        number.kind = cell_number_kind_t::signed_k;
        number.signed_integer = static_cast<std::int64_t>(~magnitude + 1);
        return number;
    }
    std::uint32_t refused = 0;
    double const real = exact_real<double>(decimal, refused);
    if (refused) return number;
    number.kind = cell_number_kind_t::real_k, number.real = real;
    return number;
}

#pragma endregion Cell Numbers

#pragma region Cell Strings

/** The length a string answers for its escapes not decoding. */
inline constexpr std::size_t no_text_k = static_cast<std::size_t>(-1);

/** Whether @p text is well-formed UTF-8: no overlong form, no surrogate, nothing past U+10FFFF. */
constexpr bool is_utf8(std::string_view text) noexcept {
    for (std::size_t position = 0; position != text.size();) {
        std::uint8_t const lead = static_cast<std::uint8_t>(text[position]);
        std::size_t length = 0;
        std::uint32_t rune = 0, rune_min = 0;
        if (lead < 0x80) {
            ++position;
            continue;
        }
        if ((lead & 0xE0) == 0xC0) length = 2, rune = lead & 0x1F, rune_min = 0x80;
        else if ((lead & 0xF0) == 0xE0) length = 3, rune = lead & 0x0F, rune_min = 0x800;
        else if ((lead & 0xF8) == 0xF0) length = 4, rune = lead & 0x07, rune_min = 0x10000;
        else return false;
        if (text.size() - position < length) return false;
        for (std::size_t index = 1; index != length; ++index) {
            std::uint8_t const continuation = static_cast<std::uint8_t>(text[position + index]);
            if ((continuation & 0xC0) != 0x80) return false;
            rune = rune << 6 | (continuation & 0x3F);
        }
        if (rune < rune_min || rune > 0x10FFFF || (rune >= 0xD800 && rune <= 0xDFFF)) return false;
        position += length;
    }
    return true;
}

/** Four hexadecimal digits at @p digits, or a value past U+10FFFF when one is no digit. */
constexpr std::uint32_t hex_unit(std::string_view digits) noexcept {
    std::uint32_t unit = 0;
    for (std::size_t index = 0; index != 4; ++index) {
        char const digit = digits[index];
        char const lowered = static_cast<char>(digit | 0x20);
        if (is_digit(digit)) unit = unit << 4 | static_cast<std::uint32_t>(digit - '0');
        else if (lowered >= 'a' && lowered <= 'f') unit = unit << 4 | static_cast<std::uint32_t>(lowered - 'a' + 10);
        else return 0xFFFFFFFFu;
    }
    return unit;
}

/** Decodes the unit of the string body @p body at @p read, a byte or one escape, into @p encoded,
 *  moving @p read past it, as StringZilla's JSON unescape does; answers the bytes it encodes, or
 *  zero for a raw control byte, an unknown escape or an unpaired surrogate. */
constexpr std::size_t unescape_unit(std::string_view body, std::size_t& read, char (&encoded)[4]) noexcept {
    char byte = body[read++];
    if (static_cast<std::uint8_t>(byte) < 0x20) return 0;
    if (byte != '\\') return encoded[0] = byte, 1;
    if (read == body.size()) return 0;
    switch (byte = body[read++]) {
    case 'b': return encoded[0] = '\b', 1;
    case 'f': return encoded[0] = '\f', 1;
    case 'n': return encoded[0] = '\n', 1;
    case 'r': return encoded[0] = '\r', 1;
    case 't': return encoded[0] = '\t', 1;
    case '/':
    case '\\':
    case '"': return encoded[0] = byte, 1;
    case 'u': break;
    default: return 0;
    }
    if (body.size() - read < 4) return 0;
    std::uint32_t rune = hex_unit({body.data() + read, 4});
    read += 4;
    if (rune > 0xFFFF || (rune >= 0xDC00 && rune <= 0xDFFF)) return 0;
    if (rune >= 0xD800 && rune <= 0xDBFF) {
        if (body.size() - read < 6 || body[read] != '\\' || body[read + 1] != 'u') return 0;
        std::uint32_t const low = hex_unit({body.data() + read + 2, 4});
        if (low < 0xDC00 || low > 0xDFFF) return 0;
        read += 6;
        rune = 0x10000 + ((rune - 0xD800) << 10) + (low - 0xDC00);
    }
    if (rune < 0x80) return encoded[0] = static_cast<char>(rune), 1;
    if (rune < 0x800)
        return encoded[0] = static_cast<char>(0xC0 | rune >> 6), encoded[1] = static_cast<char>(0x80 | (rune & 0x3F)),
               2;
    if (rune < 0x10000)
        return encoded[0] = static_cast<char>(0xE0 | rune >> 12),
               encoded[1] = static_cast<char>(0x80 | (rune >> 6 & 0x3F)),
               encoded[2] = static_cast<char>(0x80 | (rune & 0x3F)), 3;
    return encoded[0] = static_cast<char>(0xF0 | rune >> 18),
           encoded[1] = static_cast<char>(0x80 | (rune >> 12 & 0x3F)),
           encoded[2] = static_cast<char>(0x80 | (rune >> 6 & 0x3F)),
           encoded[3] = static_cast<char>(0x80 | (rune & 0x3F)), 4;
}

/** Bytes written into a text output, or only counted where it has no target. */
struct text_writer_t {
    char* target = nullptr;
    std::size_t length = 0;

    constexpr void put(char byte) noexcept {
        if (target) target[length] = byte;
        ++length;
    }
    template <std::size_t length_>
    constexpr void put(char const (&literal)[length_]) noexcept {
        for (std::size_t index = 0; index + 1 < length_; ++index) put(literal[index]);
    }
};

/** Writes @p byte as StringZilla escapes it into a JSON string: a quote, a backslash and the C0
 *  controls escaped, every other byte as it is. */
constexpr void put_escaped(text_writer_t& writer, char byte) noexcept {
    constexpr char lower_digits_k[] = "0123456789abcdef";
    std::uint8_t const unit = static_cast<std::uint8_t>(byte);
    switch (byte) {
    case '"': return writer.put("\\\"");
    case '\\': return writer.put("\\\\");
    case '\b': return writer.put("\\b");
    case '\f': return writer.put("\\f");
    case '\n': return writer.put("\\n");
    case '\r': return writer.put("\\r");
    case '\t': return writer.put("\\t");
    default: break;
    }
    if (unit >= 0x20) return writer.put(byte);
    writer.put("\\u00");
    writer.put(lower_digits_k[unit >> 4]);
    writer.put(lower_digits_k[unit & 15]);
}

/** How the string body @p body leaves an unescape: as it decodes, or re-escaped as JSON text. */
enum class unescaped_as_t : std::uint8_t { bytes_k, json_k };

/** Writes the JSON string body @p body unescaped into @p writer, answering false where its escapes
 *  do not decode, a raw control byte sits in it, or its bytes are no UTF-8. */
constexpr bool put_unescaped(text_writer_t& writer, std::string_view body, unescaped_as_t as) noexcept {
    if (!is_utf8(body)) return false;
    char encoded[4] = {};
    for (std::size_t read = 0; read != body.size();) {
        std::size_t const encoded_length = unescape_unit(body, read, encoded);
        if (!encoded_length) return false;
        for (std::size_t index = 0; index != encoded_length; ++index)
            if (as == unescaped_as_t::json_k) put_escaped(writer, encoded[index]);
            else writer.put(encoded[index]);
    }
    return true;
}

/** Writes @p magnitude in decimal digits. */
constexpr void put_integer(text_writer_t& writer, std::uint64_t magnitude) noexcept {
    char digits[20] = {};
    std::size_t count = 0;
    do digits[count++] = static_cast<char>('0' + magnitude % 10), magnitude /= 10;
    while (magnitude);
    while (count) writer.put(digits[--count]);
}

/** Writes the JSON @p json without the blank space between its tokens, its strings and numbers as
 *  written, as @c append_minified does. */
constexpr void put_minified(text_writer_t& writer, std::string_view json) noexcept {
    for (std::size_t position = 0; position != json.size(); ++position) {
        char const character = json[position];
        if (character == '"') {
            writer.put(character);
            // A string runs to the first quote no backslash escapes
            for (++position; position != json.size() && json[position] != '"'; ++position) {
                writer.put(json[position]);
                if (json[position] == '\\' && position + 1 != json.size()) writer.put(json[++position]);
            }
            if (position == json.size()) return;
            writer.put('"');
            continue;
        }
        if (character != ' ' && character != '\t' && character != '\n' && character != '\r') writer.put(character);
    }
}

#pragma endregion Cell Strings

#pragma region Cell Conversions

/** The limit of @p scalar_type_ a real's integer part must stay below, as @c real_fits takes it. */
template <typename scalar_type_>
constexpr double real_bound() noexcept {
    double bound = 1;
    for (int bit = 0; bit != std::numeric_limits<scalar_type_>::digits; ++bit) bound *= 2;
    return bound;
}

/** Whether the finite @p real, with its fraction dropped, fits the range of @p scalar_type_. */
template <typename scalar_type_>
constexpr bool real_fits_cell(double real) noexcept {
    if constexpr (std::is_same_v<scalar_type_, bool>) return true;
    else if constexpr (std::is_floating_point_v<scalar_type_>) {
        constexpr double max = static_cast<double>(std::numeric_limits<scalar_type_>::max());
        return real <= max && real >= -max;
    }
    else if constexpr (std::is_signed_v<scalar_type_>)
        return real >= -real_bound<scalar_type_>() && real < real_bound<scalar_type_>();
    else return real > -1.0 && real < real_bound<scalar_type_>();
}

/** Reads @p text whole as @c std::from_chars reads a @p scalar_type_, or a boolean's literal,
 *  answering whether it spelled one, raising @p refused for a real only the host rounds. */
template <typename scalar_type_>
constexpr bool read_text_scalar(std::string_view text, scalar_type_& scalar, std::uint32_t& refused) noexcept {
    if constexpr (std::is_same_v<scalar_type_, bool>) {
        if (!spells(text, "true") && !spells(text, "false")) return false;
        scalar = spells(text, "true");
        return true;
    }
    else if constexpr (std::is_floating_point_v<scalar_type_>) {
        std::size_t const lead = !text.empty() && text[0] == '-';
        // Infinities and NaNs read through the host's parser
        if (lead != text.size()) {
            char const lowered = static_cast<char>(text[lead] | 0x20);
            if (lowered == 'i' || lowered == 'n') return refused = 1, false;
        }
        decimal_t const decimal = read_decimal(text, decimal_grammar_t::from_chars_k);
        if (decimal.reading == decimal_reading_t::malformed_k) return false;
        scalar = exact_real<scalar_type_>(decimal, refused);
        return !refused;
    }
    else {
        std::uint64_t magnitude = 0;
        bool negative = false;
        if (!read_magnitude(text, magnitude, negative)) return false;
        if (negative && !std::is_signed_v<scalar_type_>) return false;
        if (!negative) {
            if (!std::in_range<scalar_type_>(magnitude)) return false;
            scalar = static_cast<scalar_type_>(magnitude);
            return true;
        }
        constexpr std::uint64_t magnitude_max = static_cast<std::uint64_t>(std::numeric_limits<scalar_type_>::max()) +
                                                1;
        if (magnitude > magnitude_max) return false;
        scalar = static_cast<scalar_type_>(static_cast<std::int64_t>(~magnitude + 1));
        return true;
    }
}

/** The body of the string token @p token, between its quotes. */
constexpr std::string_view string_body(std::string_view token) noexcept {
    return token.size() < 2 ? std::string_view {} : std::string_view {token.data() + 1, token.size() - 2};
}

/** Whether the string body @p body reads as its bytes: well-formed UTF-8 without an escape or a raw
 *  control byte, as only such a body is a number or a UUID a device reads itself. */
constexpr bool is_plain_body(std::string_view body) noexcept {
    for (char const byte : body)
        if (byte == '\\' || static_cast<std::uint8_t>(byte) < 0x20) return false;
    return is_utf8(body);
}

/** Converts the cell @p token into @p scalar as @c gather_scalar does, a missing cell empty,
 *  raising @p refused where only the host converts it; @p scalar is zero wherever the conversion
 *  holds no value. */
template <typename scalar_type_>
constexpr conversion_t cell_scalar(std::string_view token, scalar_type_& scalar, std::uint32_t& refused) noexcept {
    scalar = {};
    if (token.empty()) return conversion_t::missing_k;
    json_token_t const kind = json_token_of(token);
    switch (kind) {
    case json_token_t::null_k: return conversion_t::missing_k;
    case json_token_t::false_k:
    case json_token_t::true_k:
        scalar = static_cast<scalar_type_>(kind == json_token_t::true_k);
        return std::is_same_v<scalar_type_, bool> ? conversion_t::exact_k : conversion_t::converted_k;
    case json_token_t::string_k: {
        std::string_view const body = string_body(token);
        if (!is_plain_body(body)) return refused = 1, conversion_t::missing_k;
        scalar_type_ read {};
        if (read_text_scalar(body, read, refused)) return scalar = read, conversion_t::converted_k;
        return conversion_t::collides_k;
    }
    case json_token_t::number_k: {
        cell_number_t const number = read_cell_number(token);
        switch (number.kind) {
        case cell_number_kind_t::real_k:
            if (!real_fits_cell<scalar_type_>(number.real)) return conversion_t::collides_k;
            scalar = static_cast<scalar_type_>(number.real);
            return std::is_floating_point_v<scalar_type_> ? conversion_t::exact_k : conversion_t::converted_k;
        case cell_number_kind_t::signed_k:
            if constexpr (!std::is_same_v<scalar_type_, bool> && !std::is_floating_point_v<scalar_type_>)
                if (!std::in_range<scalar_type_>(number.signed_integer)) return conversion_t::collides_k;
            scalar = static_cast<scalar_type_>(number.signed_integer);
            return std::is_integral_v<scalar_type_> && std::is_signed_v<scalar_type_> ? conversion_t::exact_k
                                                                                      : conversion_t::converted_k;
        case cell_number_kind_t::unsigned_k:
            if constexpr (!std::is_same_v<scalar_type_, bool> && !std::is_floating_point_v<scalar_type_>)
                if (!std::in_range<scalar_type_>(number.unsigned_integer)) return conversion_t::collides_k;
            scalar = static_cast<scalar_type_>(number.unsigned_integer);
            return std::is_unsigned_v<scalar_type_> && !std::is_same_v<scalar_type_, bool> ? conversion_t::exact_k
                                                                                           : conversion_t::converted_k;
        default: return refused = 1, conversion_t::missing_k;
        }
    }
    default: return conversion_t::collides_k;
    }
}

/** Converts the cell @p token into a half-precision float as @c gather_half does, where a magnitude
 *  past its range collides. */
constexpr conversion_t cell_half(std::string_view token, std::uint16_t& half, std::uint32_t& refused) noexcept {
    float single = 0;
    conversion_t conversion = cell_scalar(token, single, refused);
    // A cell's float is finite: JSON spells no infinity, and a string spelling one reads on host
    if (is_valid(conversion) && (single >= half_overflow_k || single <= -half_overflow_k))
        conversion = conversion_t::collides_k;
    half = is_valid(conversion) ? half_of(single) : 0;
    return conversion;
}

/** The value of the hexadecimal digit @p digit, or 16 for any other character. */
constexpr std::uint8_t hex_digit(char digit) noexcept {
    char const lowered = static_cast<char>(digit | 0x20);
    if (is_digit(digit)) return static_cast<std::uint8_t>(digit - '0');
    if (lowered >= 'a' && lowered <= 'f') return static_cast<std::uint8_t>(lowered - 'a' + 10);
    return 16;
}

/** Converts the cell @p token, canonical UUID text, into @p uuid as @c gather_uuid does, zeroed
 *  wherever it holds none. */
constexpr conversion_t cell_uuid(std::string_view token, std::array<std::uint8_t, uuid_bytes_k>& uuid,
                                 std::uint32_t& refused) noexcept {
    uuid = {};
    if (token.empty()) return conversion_t::missing_k;
    json_token_t const kind = json_token_of(token);
    if (kind == json_token_t::null_k) return conversion_t::missing_k;
    if (kind == json_token_t::number_k && read_cell_number(token).kind == cell_number_kind_t::refused_k)
        return refused = 1, conversion_t::missing_k;
    if (kind != json_token_t::string_k) return conversion_t::collides_k;
    std::string_view const text = string_body(token);
    if (!is_plain_body(text)) return refused = 1, conversion_t::missing_k;
    if (text.size() != uuid_text_length_k) return conversion_t::collides_k;
    std::array<std::uint8_t, uuid_bytes_k> parsed {};
    std::size_t parsed_count = 0;
    for (std::size_t position = 0; position != uuid_text_length_k; position += 2) {
        if (position == 8 || position == 13 || position == 18 || position == 23) {
            if (text[position] != '-') return conversion_t::collides_k;
            ++position;
        }
        std::uint8_t const high = hex_digit(text[position]), low = hex_digit(text[position + 1]);
        if (high == 16 || low == 16) return conversion_t::collides_k;
        parsed[parsed_count++] = static_cast<std::uint8_t>(high << 4 | low);
    }
    uuid = parsed;
    return conversion_t::converted_k;
}

/** How a text output prints its cells. */
enum class cell_text_t : std::uint32_t {

    /** A string column of a gather, as @c json_to_string prints, each row ending in a null byte. */
    column_text_k,

    /** A binary column of a gather, as @c json_to_string prints, every row bare. */
    column_binary_k,

    /** A read of fields as strings: containers minified, other values as a column prints them. */
    field_text_k,

    /** A read of fields as JSON, as @c json_text_of prints. */
    field_json_k,
};

/** Writes the cell @p token into @p writer as @p text prints it, answering its conversion, raising
 *  @p refused where only the host prints it, a real among them. */
constexpr conversion_t cell_text(std::string_view token, cell_text_t text, text_writer_t& writer,
                                 std::uint32_t& refused) noexcept {
    bool const column = text == cell_text_t::column_text_k || text == cell_text_t::column_binary_k;
    json_token_t const kind = token.empty() ? json_token_t::missing_k : json_token_of(token);
    conversion_t conversion = conversion_t::collides_k;
    switch (kind) {
    case json_token_t::missing_k: conversion = conversion_t::missing_k; break;
    case json_token_t::null_k:
        conversion = conversion_t::missing_k;
        if (text == cell_text_t::field_json_k) writer.put("null");
        break;
    case json_token_t::false_k:
    case json_token_t::true_k:
        conversion = text == cell_text_t::field_json_k ? conversion_t::exact_k : conversion_t::converted_k;
        if (kind == json_token_t::true_k) writer.put("true");
        else writer.put("false");
        break;
    case json_token_t::string_k: {
        conversion = conversion_t::exact_k;
        unescaped_as_t const as = text == cell_text_t::field_json_k ? unescaped_as_t::json_k : unescaped_as_t::bytes_k;
        if (as == unescaped_as_t::json_k) writer.put('"');
        if (!put_unescaped(writer, string_body(token), as)) return refused = 1, conversion_t::missing_k;
        if (as == unescaped_as_t::json_k) writer.put('"');
        break;
    }
    case json_token_t::number_k: {
        cell_number_t const number = read_cell_number(token);
        if (number.kind == cell_number_kind_t::unsigned_k) put_integer(writer, number.unsigned_integer);
        else if (number.kind == cell_number_kind_t::signed_k) {
            std::uint64_t const bits = static_cast<std::uint64_t>(number.signed_integer);
            if (number.signed_integer < 0) writer.put('-');
            put_integer(writer, number.signed_integer < 0 ? ~bits + 1 : bits);
        }
        else return refused = 1, conversion_t::missing_k;
        conversion = conversion_t::converted_k;
        break;
    }
    case json_token_t::object_k:
    case json_token_t::array_k:
        if (!column) put_minified(writer, token);
        break;
    default: return refused = 1, conversion_t::missing_k;
    }
    if (text == cell_text_t::column_text_k) writer.put('\0');
    return conversion;
}

#pragma endregion Cell Conversions

} // namespace unum::ustore::docs
