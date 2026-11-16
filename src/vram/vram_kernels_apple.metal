/**
 *  @file src/vram/vram_kernels_apple.metal
 *  @author Ash Vardanian
 *  @date November 16, 2026
 *  @brief The kernels of Apple devices in MSL: the tape, the documents gather and the two-pass
 *      select, which @c vram_entries_apple.cpp compiles on each device at its first launch.
 *
 *  The Metal twin of @c vram_kernels_nvidia.cu , kernel for kernel and under the same names, each
 *  running the body of the @c .cuh step of that name, restated here as MSL includes no host header.
 *  A kernel takes its CUDA parameters packed in order as one struct at @c metal_arguments_slot_k ,
 *  every record laid out as its host struct and every pointer, embedded ones too, a host address,
 *  which @ref ustore_metal_pointer reaches through the block table the launch binds beside it.
 *  Rows, counts and indices are 32-bit, and byte offsets into a tape or a page stay 64-bit, as they
 *  pass 4 GiB. The host splices @c vram_apple_shared.h ahead of this source.
 *
 *  Metal has no 64-bit atomics, so the chained scan of the CUDA compactor becomes the two-pass
 *  select: a count per threadgroup tile, one threadgroup scanning the counts, and a write at each
 *  offset, in the same order. Metal has no @c double either, so a real converts through integer
 *  steps rounding once to nearest, ties to even, as the one IEEE 754 step of @c exact_real does.
 */
#include <metal_stdlib>
using namespace metal;

#pragma region Blocks

/** One live block of the device as the host writes the block table, @c metal_block_bytes_k each. */
struct ustore_metal_block_t {
    ulong host_begin;
    ulong host_end;
    ulong device_begin;
    ulong reserved;
};

/** The device pointer to the bytes at host address @p host , found among the @p count @p blocks
 *  sorted by host address, or null for a null address or one no block holds. */
template <typename type_>
inline device type_* ustore_metal_pointer(device ustore_metal_block_t const* blocks, uint count, ulong host) {
    if (host == 0 || count == 0) return nullptr;
    uint low = 0;
    for (uint high = count; high - low > 1;) {
        uint const middle = low + (high - low) / 2;
        if (blocks[middle].host_begin <= host) low = middle;
        else high = middle;
    }
    ustore_metal_block_t const block = blocks[low];
    if (host < block.host_begin || host >= block.host_end) return nullptr;
    return reinterpret_cast<device type_*>(block.device_begin + (host - block.host_begin));
}

#pragma endregion Blocks

namespace unum {
namespace ustore {

/** The block table a launch binds, which reaches every host address its arguments carry. */
struct blocks_t {
    device ustore_metal_block_t const* table;
    uint count;
};

/** The device pointer to the bytes at host address @p host . */
template <typename type_>
inline device type_* reach(blocks_t blocks, ulong host) {
    return ustore_metal_pointer<type_>(blocks.table, blocks.count, host);
}

#pragma region Block Scans

/** Threads of every threadgroup, as @c tape_block_threads_k and @c gather_block_threads_k . */
constant constexpr uint block_threads_k = 256;

/** Lanes of an Apple SIMD-group. */
constant constexpr uint simd_lanes_k = 32;

/** SIMD-groups of a threadgroup, each leaving its total for the threadgroup's scan. */
constant constexpr uint block_simds_k = block_threads_k / simd_lanes_k;

/** What one thread's value scans to across its threadgroup, as @c block_scanned_t . */
template <typename value_type_>
struct block_scanned {
    value_type_ before;
    value_type_ total;
};

/** Scans @p value across a threadgroup of @c block_threads_k threads through @p simd_totals ;
 *  every thread calls it from uniform control flow, and it ends in a barrier. */
inline block_scanned<uint> block_exclusive(uint value, uint lane, threadgroup uint* simd_totals) {
    uint const prefix = simd_prefix_exclusive_sum(value);
    if (lane % simd_lanes_k == simd_lanes_k - 1) simd_totals[lane / simd_lanes_k] = prefix + value;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    block_scanned<uint> scanned = {prefix, 0};
    for (uint simd = 0; simd != block_simds_k; ++simd) {
        uint const simd_total = simd_totals[simd];
        scanned.before += simd < lane / simd_lanes_k ? simd_total : 0;
        scanned.total += simd_total;
    }
    // A second scan rewrites the totals, so no thread may still be reading them then
    threadgroup_barrier(mem_flags::mem_threadgroup);
    return scanned;
}

/** Scans the 64-bit @p value as the 32-bit @c block_exclusive does; Metal's SIMD scan takes no
 *  64-bit word, so it scans the two halves, counting the carries out of the low one apart. */
inline block_scanned<ulong> block_exclusive(ulong value, uint lane, threadgroup ulong* simd_totals) {
    uint2 const halves = as_type<uint2>(value);
    uint const low_prefix = simd_prefix_exclusive_sum(halves.x);
    uint const carries = simd_prefix_exclusive_sum(uint(low_prefix + halves.x < low_prefix));
    uint const high_prefix = simd_prefix_exclusive_sum(halves.y);
    ulong const prefix = as_type<ulong>(uint2(low_prefix, high_prefix + carries));
    if (lane % simd_lanes_k == simd_lanes_k - 1) simd_totals[lane / simd_lanes_k] = prefix + value;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    block_scanned<ulong> scanned = {prefix, 0};
    for (uint simd = 0; simd != block_simds_k; ++simd) {
        ulong const simd_total = simd_totals[simd];
        scanned.before += simd < lane / simd_lanes_k ? simd_total : 0;
        scanned.total += simd_total;
    }
    // A second scan rewrites the totals, so no thread may still be reading them then
    threadgroup_barrier(mem_flags::mem_threadgroup);
    return scanned;
}

#pragma endregion Block Scans

#pragma region Tapes

/** One string a tape copies, as the host's @c tape_source_t , its bytes at a host address. */
struct tape_source_t {
    ulong data;
    ulong length;
};

/** A run of consecutive sources one threadgroup of a gather copies, as @c tape_segment_t . */
struct tape_segment_t {
    uint first_source;
    uint sources_count;
    ulong first_byte;
};

/** Fixed-width rows a stride apart and the dense run they copy into, as @c strided_rows_t . */
struct strided_rows_t {
    ulong source;
    ulong stride;
    ulong row_bytes;
    ulong rows;
    ulong dense;
};

/** The parameters of @c ustore_tape_offsets_u32 and @c ustore_tape_offsets_u64 , in order. */
struct tape_offsets_arguments_t {
    ulong sources;
    ulong count;
    uint sources_per_segment;
    ulong offsets;
    ulong segments;
};

/** The parameters of @c ustore_tape_gather , in order. */
struct tape_gather_arguments_t {
    ulong sources;
    ulong segments;
    ulong bytes;
};

/** Scans the lengths of @p count sources into @p offsets on one threadgroup, then cuts the sources
 *  into segments of @p sources_per_segment each, as @c tape_offsets_block does. */
template <typename offset_type_>
inline void tape_offsets_block(device tape_source_t const* sources, uint count, uint sources_per_segment,
                               device offset_type_* offsets, device tape_segment_t* segments, uint lane,
                               threadgroup offset_type_* simd_totals) {
    offset_type_ carried = 0;
    for (uint base = 0; base < count; base += block_threads_k) {
        uint const index = base + lane;
        offset_type_ const length = index < count ? offset_type_(sources[index].length) : offset_type_(0);
        block_scanned<offset_type_> const scan = block_exclusive(length, lane, simd_totals);
        if (index < count) offsets[index] = carried + scan.before;
        carried += scan.total;
    }
    if (lane == 0) offsets[count] = carried;
    threadgroup_barrier(mem_flags::mem_device);
    uint const segments_count = count / sources_per_segment + (count % sources_per_segment != 0 ? 1 : 0);
    for (uint segment_index = lane; segment_index < segments_count; segment_index += block_threads_k) {
        uint const first = segment_index * sources_per_segment;
        uint const left = count - first;
        tape_segment_t segment;
        segment.first_source = first;
        segment.sources_count = left < sources_per_segment ? left : sources_per_segment;
        segment.first_byte = ulong(offsets[first]);
        segments[segment_index] = segment;
    }
}

#pragma endregion Tapes

#pragma region Cell Numbers

/** How faithfully a value under a field converts, as @c conversion_t . */
enum class conversion_t : uchar { missing_k, exact_k, converted_k, collides_k };

/** Whether @p conversion produced a value the column holds. */
inline bool is_valid(conversion_t conversion) {
    return conversion == conversion_t::exact_k || conversion == conversion_t::converted_k;
}

/** Records @p conversion of one cell, setting validity last, as @c record_conversion . */
inline void record_conversion(conversion_t conversion, uchar mask, thread uchar& valid, thread uchar& converted,
                              thread uchar& collides) {
    uchar const cleared = uchar(~mask);
    converted = uchar((converted & cleared) | (conversion == conversion_t::converted_k ? mask : 0));
    collides = uchar((collides & cleared) | (conversion == conversion_t::collides_k ? mask : 0));
    valid = uchar((valid & cleared) | (is_valid(conversion) ? mask : 0));
}

/** What one field of one document holds, told by its token's first byte, as @c json_token_t . */
enum class json_token_t : uchar {
    missing_k = 0,
    null_k = 1,
    false_k = 2,
    true_k = 3,
    number_k = 4,
    string_k = 5,
    object_k = 6,
    array_k = 7,
    malformed_k = 8,
};

/** The bytes of one token inside a page, the @c std::string_view of the host steps. */
struct token_t {
    device char const* data;
    uint size;
};

/** The empty token of a missing cell. */
inline token_t no_token() { return {nullptr, 0}; }

/** The bytes of @p token from @p first on, @p size of them. */
inline token_t token_slice(token_t token, uint first, uint size) { return {token.data + first, size}; }

/** What @p token holds, told by its first byte, as @c json_token_of . */
inline json_token_t json_token_of(token_t token) {
    if (token.size == 0) return json_token_t::malformed_k;
    switch (token.data[0]) {
    case '"': return json_token_t::string_k;
    case '{': return json_token_t::object_k;
    case '[': return json_token_t::array_k;
    case 't': return json_token_t::true_k;
    case 'f': return json_token_t::false_k;
    case 'n': return json_token_t::null_k;
    default: return json_token_t::number_k;
    }
}

/** The bits of the float @c half_overflow_k , past which a magnitude rounds beyond any half. */
constant constexpr uint half_overflow_bits_k = 0x477FF000u;

/** Encodes the finite float of @p bits below @c half_overflow_bits_k as a half, as @c half_of . */
inline ushort half_of(uint bits) {
    ushort const sign = ushort((bits >> 16) & 0x8000u);
    int const exponent = int((bits >> 23) & 0xffu) - 127 + 15;
    uint mantissa = bits & 0x7fffffu;
    if (((bits >> 23) & 0xffu) == 0xffu) return ushort(sign | 0x7c00u | (mantissa ? 0x200u : 0u));
    if (exponent >= 1) {
        ushort const half = ushort(sign | (uint(exponent) << 10) | (mantissa >> 13));
        return ushort(half + ((mantissa >> 12) & 1u));
    }
    if (exponent < -10) return sign;
    // Subnormal halves shift the implicit leading bit into the mantissa
    mantissa |= 0x800000u;
    uint const shift = uint(14 - exponent);
    ushort const half = ushort(sign | (mantissa >> shift));
    return ushort(half + ((mantissa >> (shift - 1)) & 1u));
}

/** How a decimal reads, as @c decimal_reading_t . */
enum class decimal_reading_t : uchar { malformed_k, integer_k, fraction_k, too_precise_k };

/** A decimal's significant digits as one integer and their power of ten, as @c decimal_t . */
struct decimal_t {
    decimal_reading_t reading;
    bool negative;
    ulong significand;
    int exponent;
};

/** Significant digits a 64-bit significand holds whatever they are. */
constant constexpr uint decimal_digits_max_k = 19;

/** A magnitude past which an exponent's digits stop mattering. */
constant constexpr int decimal_exponent_max_k = 1000000;

/** The spellings a decimal reads, as @c decimal_grammar_t . */
enum class decimal_grammar_t : uchar { json_k, from_chars_k };

/** Whether @p character is a decimal digit. */
inline bool is_digit(char character) { return character >= '0' && character <= '9'; }

/** The malformed decimal. */
inline decimal_t malformed_decimal() { return {decimal_reading_t::malformed_k, false, 0, 0}; }

/** Reads the whole of @p text as a decimal of @p grammar , as @c read_decimal . */
inline decimal_t read_decimal(token_t text, decimal_grammar_t grammar) {
    bool const json = grammar == decimal_grammar_t::json_k;
    decimal_t decimal = malformed_decimal();
    uint position = 0;
    decimal.negative = position != text.size && text.data[position] == '-';
    position += decimal.negative ? 1 : 0;
    // JSON spells a zero integer part alone, never with leading zeros
    if (json && text.size - position > 1 && text.data[position] == '0' && is_digit(text.data[position + 1]))
        return decimal;

    // Leading zeros carry no digit, and digits past 64 bits only mark the decimal too precise
    uint significant = 0, integer_digits = 0, fraction_digits = 0;
    for (; position != text.size && is_digit(text.data[position]); ++position, ++integer_digits) {
        if (!decimal.significand && text.data[position] == '0') continue;
        if (significant++ < decimal_digits_max_k)
            decimal.significand = decimal.significand * 10 + ulong(text.data[position] - '0');
        else ++decimal.exponent;
    }
    bool const pointed = position != text.size && text.data[position] == '.';
    position += pointed ? 1 : 0;
    for (; pointed && position != text.size && is_digit(text.data[position]); ++position, ++fraction_digits) {
        if (!decimal.significand && text.data[position] == '0') {
            --decimal.exponent;
            continue;
        }
        if (significant++ >= decimal_digits_max_k) continue;
        decimal.significand = decimal.significand * 10 + ulong(text.data[position] - '0');
        --decimal.exponent;
    }
    if (!integer_digits && !fraction_digits) return malformed_decimal();
    if (json && (!integer_digits || (pointed && !fraction_digits))) return malformed_decimal();

    bool const scaled = position != text.size && (text.data[position] == 'e' || text.data[position] == 'E');
    if (scaled) {
        ++position;
        bool const negative_exponent = position != text.size && text.data[position] == '-';
        position += position != text.size && (text.data[position] == '-' || text.data[position] == '+') ? 1 : 0;
        uint const first = position;
        int written = 0;
        for (; position != text.size && is_digit(text.data[position]); ++position)
            if (written < decimal_exponent_max_k) written = written * 10 + (text.data[position] - '0');
        if (position == first) return malformed_decimal();
        decimal.exponent += negative_exponent ? -written : written;
    }
    if (position != text.size) return malformed_decimal();
    decimal.reading = significant > decimal_digits_max_k ? decimal_reading_t::too_precise_k
                      : pointed || scaled                ? decimal_reading_t::fraction_k
                                                         : decimal_reading_t::integer_k;
    return decimal;
}

/** A 128-bit integer as two 64-bit halves, holding the exact product or quotient a real rounds. */
struct wide_t {
    ulong high;
    ulong low;
};

/** Bits @p value spans, zero for zero, through the 32-bit @c clz . */
inline uint bits_of(ulong value) {
    uint const high = uint(value >> 32);
    return high ? 64 - clz(high) : 32 - clz(uint(value));
}

/** Bits @p value spans, zero for zero. */
inline uint bits_of(wide_t value) { return value.high ? 64 + bits_of(value.high) : bits_of(value.low); }

/** Bit @p bit of @p value . */
inline bool bit_of(wide_t value, uint bit) {
    return ((bit < 64 ? value.low >> bit : value.high >> (bit - 64)) & 1ul) != 0;
}

/** Whether any of the @p count lowest bits of @p value is set. */
inline bool any_below(wide_t value, uint count) {
    if (count == 0) return false;
    if (count < 64) return (value.low << (64 - count)) != 0;
    return value.low != 0 || (count > 64 && (value.high << (128 - count)) != 0);
}

/** @p value shifted right by @p shift , below 128. */
inline wide_t shift_right(wide_t value, uint shift) {
    if (shift == 0) return value;
    if (shift >= 64) return {0, value.high >> (shift - 64)};
    return {value.high >> shift, (value.low >> shift) | (value.high << (64 - shift))};
}

/** @p value shifted left by @p shift , below 128, as wide as it gets. */
inline wide_t shift_left(ulong value, uint shift) {
    if (shift == 0) return {0, value};
    if (shift >= 64) return {value << (shift - 64), 0};
    return {value >> (64 - shift), value << shift};
}

/** The exact product of @p first and @p second , through 32-bit halves. */
inline wide_t multiply_wide(ulong first, ulong second) {
    ulong const first_low = first & 0xFFFFFFFFul, first_high = first >> 32;
    ulong const second_low = second & 0xFFFFFFFFul, second_high = second >> 32;
    ulong const low_low = first_low * second_low, low_high = first_low * second_high;
    ulong const high_low = first_high * second_low, high_high = first_high * second_high;
    ulong const middle = (low_low >> 32) + (low_high & 0xFFFFFFFFul) + (high_low & 0xFFFFFFFFul);
    return {high_high + (low_high >> 32) + (high_low >> 32) + (middle >> 32),
            (middle << 32) | (low_low & 0xFFFFFFFFul)};
}

/** The quotient of @p dividend by @p divisor below 2^63, a bit at a time, leaving @p remainder . */
inline wide_t divide_wide(wide_t dividend, ulong divisor, thread ulong& remainder) {
    wide_t quotient = {0, 0};
    remainder = 0;
    for (uint step = 0; step != 128; ++step) {
        uint const bit = 127 - step;
        remainder = remainder << 1 | (bit_of(dividend, bit) ? 1ul : 0ul);
        if (remainder < divisor) continue;
        remainder -= divisor;
        if (bit < 64) quotient.low |= 1ul << bit;
        else quotient.high |= 1ul << (bit - 64);
    }
    return quotient;
}

/** A finite real as @c significand times two to @c scale , the significand filling the bits of
 *  its precision or zero, which only the sign tells apart from minus zero. */
struct real_t {
    bool negative;
    ulong significand;
    int scale;
};

/** Rounds the nonzero @p value , with the @p remainder of the division it came from below it, to
 *  its @p digits leading bits once to nearest, ties to even, adding the power of two they lose to
 *  @p scale . */
inline ulong round_wide(wide_t value, ulong remainder, uint digits, thread int& scale) {
    uint const bits = bits_of(value);
    if (bits <= digits) {
        scale -= int(digits - bits);
        return value.low << (digits - bits);
    }
    uint const shift = bits - digits;
    ulong kept = shift_right(value, shift).low;
    bool const half = bit_of(value, shift - 1);
    bool const rest = remainder != 0 || any_below(value, shift - 1);
    if (half && (rest || (kept & 1ul))) ++kept;
    scale += int(shift);
    if (kept >> digits) kept >>= 1, ++scale;
    return kept;
}

/** The decimal @p decimal as a real of @p digits significand bits, 53 or 24, rounded once
 *  to nearest where its significand and power of ten up to @p power_max are both exact, as
 *  @c exact_real does, raising @p refused for any other. A power of ten is that power of
 *  two times five to it, and five to 22 fits 64 bits, so every product is exact and every
 *  quotient keeps 74 bits. */
inline real_t exact_real(decimal_t decimal, uint digits, uint power_max, thread uint& refused) {
    real_t real = {decimal.negative, 0, 0};
    if (!decimal.significand) return real;
    uint const power = uint(decimal.exponent < 0 ? -decimal.exponent : decimal.exponent);
    if (decimal.reading == decimal_reading_t::too_precise_k || decimal.significand > (1ul << digits) ||
        power > power_max) {
        refused = 1;
        return {false, 0, 0};
    }
    ulong five = 1;
    for (uint step = 0; step != power; ++step) five *= 5;
    ulong remainder = 0;
    wide_t value;
    if (decimal.exponent >= 0) value = multiply_wide(decimal.significand, five), real.scale = int(power);
    else {
        uint const shift = 127 - bits_of(decimal.significand);
        value = divide_wide(shift_left(decimal.significand, shift), five, remainder);
        real.scale = -int(shift) - int(power);
    }
    real.significand = round_wide(value, remainder, digits, real.scale);
    return real;
}

/** @p real of 53 significand bits rounded to @p digits , as a double cast to a float rounds. */
inline real_t narrow_real(real_t real, uint digits) {
    if (!real.significand) return real;
    real.significand = round_wide({0, real.significand}, 0, digits, real.scale);
    return real;
}

/** The integer @p magnitude as a real of @p digits significand bits, as a cast of an integer to a
 *  float or a double rounds it. */
inline real_t integer_real(ulong magnitude, uint digits) {
    real_t real = {false, 0, 0};
    if (!magnitude) return real;
    real.significand = round_wide({0, magnitude}, 0, digits, real.scale);
    return real;
}

/** The bits of @p real of 53 significand bits as a double; every exact real is normal. */
inline ulong double_bits(real_t real) {
    ulong const sign = real.negative ? 1ul << 63 : 0ul;
    if (!real.significand) return sign;
    return sign | (ulong(real.scale + 52 + 1023) << 52) | (real.significand & ((1ul << 52) - 1));
}

/** The bits of @p real of 24 significand bits as a float; every exact real is a normal float, as
 *  none passes 2^53 times 10^22 nor falls below 10^-22. */
inline uint float_bits(real_t real) {
    uint const sign = real.negative ? 1u << 31 : 0u;
    if (!real.significand) return sign;
    return sign | (uint(real.scale + 23 + 127) << 23) | uint(real.significand & ((1ul << 23) - 1));
}

/** An integer of 64 bits and a sign, as a cell spells it. */
struct integer_t {
    bool negative;
    ulong magnitude;
};

/** The number kind a cell spells, as @c cell_number_kind_t . */
enum class cell_number_kind_t : uchar { unsigned_k, signed_k, real_k, refused_k };

/** The number one cell spells, as @c cell_number_t , an integer as its magnitude and sign. */
struct cell_number_t {
    cell_number_kind_t kind;
    integer_t integer;
    real_t real;
};

/** Reads @p text as decimal digits after an optional minus into @p magnitude , answering whether it
 *  spelled an integer within 64 bits, as @c read_magnitude . */
inline bool read_magnitude(token_t text, thread ulong& magnitude, thread bool& negative) {
    uint position = 0;
    negative = position != text.size && text.data[position] == '-';
    position += negative ? 1 : 0;
    if (position == text.size) return false;
    magnitude = 0;
    for (; position != text.size; ++position) {
        if (!is_digit(text.data[position])) return false;
        ulong const digit = ulong(text.data[position] - '0');
        if (magnitude > (0xFFFFFFFFFFFFFFFFul - digit) / 10) return false;
        magnitude = magnitude * 10 + digit;
    }
    return true;
}

/** Reads the number @p token as yyjson does, refusing what one rounding misreads, as
 *  @c read_cell_number . */
inline cell_number_t read_cell_number(token_t token) {
    cell_number_t number = {cell_number_kind_t::refused_k, {false, 0}, {false, 0, 0}};
    decimal_t const decimal = read_decimal(token, decimal_grammar_t::json_k);
    if (decimal.reading == decimal_reading_t::malformed_k) return number;
    if (decimal.reading != decimal_reading_t::fraction_k) {
        ulong magnitude = 0;
        bool negative = false;
        // An integer past 64 bits reads as a real, which takes the host's rounding
        if (!read_magnitude(token, magnitude, negative)) return number;
        if (!negative) {
            number.kind = cell_number_kind_t::unsigned_k, number.integer.magnitude = magnitude;
            return number;
        }
        if (magnitude > 1ul << 63) return number;
        number.kind = cell_number_kind_t::signed_k, number.integer = {magnitude != 0, magnitude};
        return number;
    }
    uint refused = 0;
    real_t const real = exact_real(decimal, 53, 22, refused);
    if (refused) return number;
    number.kind = cell_number_kind_t::real_k, number.real = real;
    return number;
}

#pragma endregion Cell Numbers

#pragma region Cell Strings

/** Whether @p text is well-formed UTF-8, as @c is_utf8 . */
inline bool is_utf8(token_t text) {
    for (uint position = 0; position != text.size;) {
        uchar const lead = uchar(text.data[position]);
        uint length = 0, rune = 0, rune_min = 0;
        if (lead < 0x80) {
            ++position;
            continue;
        }
        if ((lead & 0xE0) == 0xC0) length = 2, rune = lead & 0x1F, rune_min = 0x80;
        else if ((lead & 0xF0) == 0xE0) length = 3, rune = lead & 0x0F, rune_min = 0x800;
        else if ((lead & 0xF8) == 0xF0) length = 4, rune = lead & 0x07, rune_min = 0x10000;
        else return false;
        if (text.size - position < length) return false;
        for (uint index = 1; index != length; ++index) {
            uchar const continuation = uchar(text.data[position + index]);
            if ((continuation & 0xC0) != 0x80) return false;
            rune = rune << 6 | (continuation & 0x3F);
        }
        if (rune < rune_min || rune > 0x10FFFF || (rune >= 0xD800 && rune <= 0xDFFF)) return false;
        position += length;
    }
    return true;
}

/** The value of the hexadecimal digit @p digit , or 16 for any other, as @c hex_digit . */
inline uint hex_digit(char digit) {
    char const lowered = char(digit | 0x20);
    if (is_digit(digit)) return uint(digit - '0');
    if (lowered >= 'a' && lowered <= 'f') return uint(lowered - 'a' + 10);
    return 16;
}

/** Four hexadecimal digits of @p text from @p first , or a value past U+10FFFF, as @c hex_unit . */
inline uint hex_unit(token_t text, uint first) {
    uint unit = 0;
    for (uint index = 0; index != 4; ++index) {
        uint const digit = hex_digit(text.data[first + index]);
        if (digit == 16) return 0xFFFFFFFFu;
        unit = unit << 4 | digit;
    }
    return unit;
}

/** Decodes the unit of the string body @p body at @p read , a byte or one escape, into @p encoded ,
 *  answering the bytes it encodes or zero, as @c unescape_unit . */
inline uint unescape_unit(token_t body, thread uint& read, thread char* encoded) {
    char byte = body.data[read++];
    if (uchar(byte) < 0x20) return 0;
    if (byte != '\\') return encoded[0] = byte, 1;
    if (read == body.size) return 0;
    switch (byte = body.data[read++]) {
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
    if (body.size - read < 4) return 0;
    uint rune = hex_unit(body, read);
    read += 4;
    if (rune > 0xFFFF || (rune >= 0xDC00 && rune <= 0xDFFF)) return 0;
    if (rune >= 0xD800 && rune <= 0xDBFF) {
        if (body.size - read < 6 || body.data[read] != '\\' || body.data[read + 1] != 'u') return 0;
        uint const low = hex_unit(body, read + 2);
        if (low < 0xDC00 || low > 0xDFFF) return 0;
        read += 6;
        rune = 0x10000 + ((rune - 0xD800) << 10) + (low - 0xDC00);
    }
    if (rune < 0x80) return encoded[0] = char(rune), 1;
    if (rune < 0x800) return encoded[0] = char(0xC0 | rune >> 6), encoded[1] = char(0x80 | (rune & 0x3F)), 2;
    if (rune < 0x10000)
        return encoded[0] = char(0xE0 | rune >> 12), encoded[1] = char(0x80 | (rune >> 6 & 0x3F)),
               encoded[2] = char(0x80 | (rune & 0x3F)), 3;
    return encoded[0] = char(0xF0 | rune >> 18), encoded[1] = char(0x80 | (rune >> 12 & 0x3F)),
           encoded[2] = char(0x80 | (rune >> 6 & 0x3F)), encoded[3] = char(0x80 | (rune & 0x3F)), 4;
}

/** Bytes written into a text output, or counted where it has no target, as @c text_writer_t . */
struct text_writer_t {
    device char* target;
    uint length;
};

/** Writes @p byte into @p writer . */
inline void put(thread text_writer_t& writer, char byte) {
    if (writer.target) writer.target[writer.length] = byte;
    ++writer.length;
}

/** Writes the literal of @p kind , @c null , @c true or @c false , into @p writer . */
inline void put_literal(thread text_writer_t& writer, json_token_t kind) {
    if (kind == json_token_t::null_k) put(writer, 'n'), put(writer, 'u'), put(writer, 'l'), put(writer, 'l');
    else if (kind == json_token_t::true_k) put(writer, 't'), put(writer, 'r'), put(writer, 'u'), put(writer, 'e');
    else put(writer, 'f'), put(writer, 'a'), put(writer, 'l'), put(writer, 's'), put(writer, 'e');
}

/** Writes @p byte as StringZilla escapes it into a JSON string, as @c put_escaped . */
inline void put_escaped(thread text_writer_t& writer, char byte) {
    uchar const unit = uchar(byte);
    switch (byte) {
    case '"': return put(writer, '\\'), put(writer, '"');
    case '\\': return put(writer, '\\'), put(writer, '\\');
    case '\b': return put(writer, '\\'), put(writer, 'b');
    case '\f': return put(writer, '\\'), put(writer, 'f');
    case '\n': return put(writer, '\\'), put(writer, 'n');
    case '\r': return put(writer, '\\'), put(writer, 'r');
    case '\t': return put(writer, '\\'), put(writer, 't');
    default: break;
    }
    if (unit >= 0x20) return put(writer, byte);
    put(writer, '\\'), put(writer, 'u'), put(writer, '0'), put(writer, '0');
    uint const high = unit >> 4, low = unit & 15;
    put(writer, char(high < 10 ? '0' + high : 'a' + high - 10));
    put(writer, char(low < 10 ? '0' + low : 'a' + low - 10));
}

/** How a string body leaves an unescape, as @c unescaped_as_t . */
enum class unescaped_as_t : uchar { bytes_k, json_k };

/** Writes the JSON string body @p body unescaped into @p writer , as it decodes or re-escaped as
 *  JSON, answering false where it does not decode, as @c put_unescaped . */
inline bool put_unescaped(thread text_writer_t& writer, token_t body, unescaped_as_t as) {
    if (!is_utf8(body)) return false;
    char encoded[4] = {0, 0, 0, 0};
    for (uint read = 0; read != body.size;) {
        uint const encoded_length = unescape_unit(body, read, encoded);
        if (!encoded_length) return false;
        for (uint index = 0; index != encoded_length; ++index)
            if (as == unescaped_as_t::json_k) put_escaped(writer, encoded[index]);
            else put(writer, encoded[index]);
    }
    return true;
}

/** Writes @p magnitude in decimal digits, as @c put_integer . */
inline void put_integer(thread text_writer_t& writer, ulong magnitude) {
    char digits[20];
    uint count = 0;
    do digits[count++] = char('0' + magnitude % 10), magnitude /= 10;
    while (magnitude);
    while (count) put(writer, digits[--count]);
}

/** Writes the body of the string of @p json opening before @p position as it is, answering the
 *  position of its closing quote, or the end where none closes it. */
inline uint put_string_body(thread text_writer_t& writer, token_t json, uint position) {
    // A string runs to the first quote no backslash escapes
    for (; position != json.size && json.data[position] != '"'; ++position) {
        put(writer, json.data[position]);
        if (json.data[position] == '\\' && position + 1 != json.size) put(writer, json.data[++position]);
    }
    return position;
}

/** Writes @p json without the blank space between its tokens, as @c put_minified . */
inline void put_minified(thread text_writer_t& writer, token_t json) {
    for (uint position = 0; position != json.size; ++position) {
        char const character = json.data[position];
        if (character == '"') {
            put(writer, character);
            position = put_string_body(writer, json, position + 1);
            if (position == json.size) return;
            put(writer, '"');
            continue;
        }
        if (character != ' ' && character != '\t' && character != '\n' && character != '\r') put(writer, character);
    }
}

/** The body of the string token @p token , between its quotes. */
inline token_t string_body(token_t token) {
    return token.size < 2 ? no_token() : token_slice(token, 1, token.size - 2);
}

/** Whether the string body @p body reads as its bytes, as @c is_plain_body . */
inline bool is_plain_body(token_t body) {
    for (uint index = 0; index != body.size; ++index)
        if (body.data[index] == '\\' || uchar(body.data[index]) < 0x20) return false;
    return is_utf8(body);
}

#pragma endregion Cell Strings

#pragma region Cell Conversions

/** How a scalar column converts: as a boolean, a float of @c bits , or an integer of @c bits . */
enum class scalar_family_t : uchar { boolean_k, real_k, signed_k, unsigned_k };

/** The family and width of one scalar column, the template argument of the host steps. */
struct scalar_kind_t {
    scalar_family_t family;
    uint bits;
};

/** The bits a column of @p kind holds for the literal @p literal , as a cast of a @c bool . */
inline ulong scalar_of_literal(scalar_kind_t kind, json_token_t literal) {
    bool const value = literal == json_token_t::true_k;
    if (kind.family != scalar_family_t::real_k || !value) return value ? 1ul : 0ul;
    return kind.bits == 64 ? 0x3FF0000000000000ul : 0x3F800000ul;
}

/** Converts @p integer into the bits @p raw of @p kind , answering whether it fits, as
 *  @c integer_fits_cell and a cast. */
inline bool scalar_of_integer(scalar_kind_t kind, integer_t integer, thread ulong& raw) {
    bool const negative = integer.negative;
    ulong const magnitude = integer.magnitude;
    switch (kind.family) {
    case scalar_family_t::boolean_k: raw = magnitude != 0 ? 1ul : 0ul; return true;
    case scalar_family_t::real_k: {
        real_t real = integer_real(magnitude, kind.bits == 64 ? 53 : 24);
        real.negative = negative && magnitude != 0;
        raw = kind.bits == 64 ? double_bits(real) : ulong(float_bits(real));
        return true;
    }
    case scalar_family_t::signed_k: {
        ulong const bound = 1ul << (kind.bits - 1);
        if (negative ? magnitude > bound : magnitude >= bound) return false;
        break;
    }
    case scalar_family_t::unsigned_k:
        if (negative || (kind.bits < 64 && magnitude >= 1ul << kind.bits)) return false;
        break;
    }
    raw = negative ? ~magnitude + 1 : magnitude;
    return true;
}

/** Converts the exact real @p real of 53 significand bits into the bits @p raw of @p kind ,
 *  answering whether it fits, as @c real_fits_cell and a cast truncating toward zero. */
inline bool scalar_of_real(scalar_kind_t kind, real_t real, thread ulong& raw) {
    switch (kind.family) {
    case scalar_family_t::boolean_k: raw = real.significand != 0 ? 1ul : 0ul; return true;
    case scalar_family_t::real_k:
        raw = kind.bits == 64 ? double_bits(real) : ulong(float_bits(narrow_real(real, 24)));
        return true;
    default: break;
    }
    ulong whole = 0;
    bool fraction = false;
    if (real.significand && real.scale >= 0) {
        if (53 + uint(real.scale) > 64) return false;
        whole = real.significand << uint(real.scale);
    }
    else if (real.significand) {
        uint const dropped = uint(-real.scale);
        whole = dropped < 64 ? real.significand >> dropped : 0;
        fraction = dropped < 64 ? (real.significand & ((1ul << dropped) - 1)) != 0 : true;
    }
    if (kind.family == scalar_family_t::signed_k) {
        ulong const bound = 1ul << (kind.bits - 1);
        if (real.negative ? whole > bound || (whole == bound && fraction) : whole >= bound) return false;
    }
    else if (real.negative ? whole != 0 : kind.bits < 64 && whole >= 1ul << kind.bits) return false;
    raw = real.negative ? ~whole + 1 : whole;
    return true;
}

/** Converts the cell number @p number into bits @p raw of @p kind , as @c cell_number_scalar . */
inline conversion_t cell_number_scalar(cell_number_t number, scalar_kind_t kind, thread ulong& raw,
                                       thread uint& refused) {
    ulong converted = 0;
    switch (number.kind) {
    case cell_number_kind_t::real_k:
        if (!scalar_of_real(kind, number.real, converted)) return conversion_t::collides_k;
        raw = converted;
        return kind.family == scalar_family_t::real_k ? conversion_t::exact_k : conversion_t::converted_k;
    case cell_number_kind_t::signed_k:
    case cell_number_kind_t::unsigned_k: {
        if (!scalar_of_integer(kind, number.integer, converted)) return conversion_t::collides_k;
        raw = converted;
        scalar_family_t const exact_family = number.kind == cell_number_kind_t::signed_k ? scalar_family_t::signed_k
                                                                                         : scalar_family_t::unsigned_k;
        return kind.family == exact_family ? conversion_t::exact_k : conversion_t::converted_k;
    }
    default: return refused = 1, conversion_t::missing_k;
    }
}

/** Reads @p text whole as @c std::from_chars reads a scalar of @p kind , or a boolean's literal,
 *  into @p raw , raising @p refused for a real only the host rounds, as @c read_text_scalar . */
inline bool read_text_scalar(token_t text, scalar_kind_t kind, thread ulong& raw, thread uint& refused) {
    if (kind.family == scalar_family_t::boolean_k) {
        bool const spells_true = text.size == 4 && text.data[0] == 't' && text.data[1] == 'r' && text.data[2] == 'u' &&
                                 text.data[3] == 'e';
        bool const spells_false = text.size == 5 && text.data[0] == 'f' && text.data[1] == 'a' && text.data[2] == 'l' &&
                                  text.data[3] == 's' && text.data[4] == 'e';
        if (!spells_true && !spells_false) return false;
        raw = spells_true ? 1ul : 0ul;
        return true;
    }
    if (kind.family == scalar_family_t::real_k) {
        uint const lead = text.size != 0 && text.data[0] == '-' ? 1 : 0;
        // Infinities and NaNs read through the host's parser
        if (lead != text.size) {
            char const lowered = char(text.data[lead] | 0x20);
            if (lowered == 'i' || lowered == 'n') return refused = 1, false;
        }
        decimal_t const decimal = read_decimal(text, decimal_grammar_t::from_chars_k);
        if (decimal.reading == decimal_reading_t::malformed_k) return false;
        bool const single = kind.bits == 32;
        real_t const real = exact_real(decimal, single ? 24 : 53, single ? 10 : 22, refused);
        raw = single ? ulong(float_bits(real)) : double_bits(real);
        return !refused;
    }
    ulong magnitude = 0;
    bool negative = false;
    if (!read_magnitude(text, magnitude, negative)) return false;
    if (negative && kind.family == scalar_family_t::unsigned_k) return false;
    return scalar_of_integer(kind, {negative && magnitude != 0, magnitude}, raw);
}

/** Converts the cell @p token into the bits @p raw of @p kind , zero wherever it holds no value,
 *  as @c cell_scalar . */
inline conversion_t cell_scalar(token_t token, scalar_kind_t kind, thread ulong& raw, thread uint& refused) {
    raw = 0;
    if (token.size == 0) return conversion_t::missing_k;
    json_token_t const token_kind = json_token_of(token);
    switch (token_kind) {
    case json_token_t::null_k: return conversion_t::missing_k;
    case json_token_t::false_k:
    case json_token_t::true_k:
        raw = scalar_of_literal(kind, token_kind);
        return kind.family == scalar_family_t::boolean_k ? conversion_t::exact_k : conversion_t::converted_k;
    case json_token_t::string_k: {
        token_t const body = string_body(token);
        if (!is_plain_body(body)) return refused = 1, conversion_t::missing_k;
        ulong read = 0;
        if (read_text_scalar(body, kind, read, refused)) return raw = read, conversion_t::converted_k;
        return conversion_t::collides_k;
    }
    case json_token_t::number_k: return cell_number_scalar(read_cell_number(token), kind, raw, refused);
    default: return conversion_t::collides_k;
    }
}

/** Converts the cell @p token into a half as @c cell_half , where a magnitude past it collides. */
inline conversion_t cell_half(token_t token, thread ushort& half, thread uint& refused) {
    ulong single = 0;
    conversion_t conversion = cell_scalar(token, {scalar_family_t::real_k, 32}, single, refused);
    // A cell's float is finite: JSON spells no infinity, and a string spelling one reads on host
    if (is_valid(conversion) && (uint(single) & 0x7FFFFFFFu) >= half_overflow_bits_k)
        conversion = conversion_t::collides_k;
    half = is_valid(conversion) ? half_of(uint(single)) : ushort(0);
    return conversion;
}

/** Bytes of a UUID, and of its canonical text. */
constant constexpr uint uuid_bytes_k = 16;
constant constexpr uint uuid_text_length_k = 36;

/** Converts the cell @p token , canonical UUID text, into @p uuid , zeroed wherever it holds none,
 *  as @c cell_uuid . */
inline conversion_t cell_uuid(token_t token, thread uchar* uuid, thread uint& refused) {
    for (uint index = 0; index != uuid_bytes_k; ++index) uuid[index] = 0;
    if (token.size == 0) return conversion_t::missing_k;
    json_token_t const kind = json_token_of(token);
    if (kind == json_token_t::null_k) return conversion_t::missing_k;
    if (kind == json_token_t::number_k && read_cell_number(token).kind == cell_number_kind_t::refused_k)
        return refused = 1, conversion_t::missing_k;
    if (kind != json_token_t::string_k) return conversion_t::collides_k;
    token_t const text = string_body(token);
    if (!is_plain_body(text)) return refused = 1, conversion_t::missing_k;
    if (text.size != uuid_text_length_k) return conversion_t::collides_k;
    uchar parsed[uuid_bytes_k];
    uint parsed_count = 0;
    for (uint position = 0; position != uuid_text_length_k; position += 2) {
        if (position == 8 || position == 13 || position == 18 || position == 23) {
            if (text.data[position] != '-') return conversion_t::collides_k;
            ++position;
        }
        uint const high = hex_digit(text.data[position]), low = hex_digit(text.data[position + 1]);
        if (high == 16 || low == 16) return conversion_t::collides_k;
        parsed[parsed_count++] = uchar(high << 4 | low);
    }
    for (uint index = 0; index != uuid_bytes_k; ++index) uuid[index] = parsed[index];
    return conversion_t::converted_k;
}

/** How a text output prints its cells, as @c cell_text_t numbers them. */
enum class cell_text_t : uint { column_text_k, column_binary_k, field_text_k, field_json_k };

/** Writes the cell @p token into @p writer as @p text prints it, answering its conversion, raising
 *  @p refused where only the host prints it, as @c cell_text . */
inline conversion_t cell_text(token_t token, cell_text_t text, thread text_writer_t& writer, thread uint& refused) {
    bool const column = text == cell_text_t::column_text_k || text == cell_text_t::column_binary_k;
    json_token_t const kind = token.size == 0 ? json_token_t::missing_k : json_token_of(token);
    conversion_t conversion = conversion_t::collides_k;
    switch (kind) {
    case json_token_t::missing_k: conversion = conversion_t::missing_k; break;
    case json_token_t::null_k:
        conversion = conversion_t::missing_k;
        if (text == cell_text_t::field_json_k) put_literal(writer, kind);
        break;
    case json_token_t::false_k:
    case json_token_t::true_k:
        conversion = text == cell_text_t::field_json_k ? conversion_t::exact_k : conversion_t::converted_k;
        put_literal(writer, kind);
        break;
    case json_token_t::string_k: {
        conversion = conversion_t::exact_k;
        unescaped_as_t const as = text == cell_text_t::field_json_k ? unescaped_as_t::json_k : unescaped_as_t::bytes_k;
        if (as == unescaped_as_t::json_k) put(writer, '"');
        if (!put_unescaped(writer, string_body(token), as)) return refused = 1, conversion_t::missing_k;
        if (as == unescaped_as_t::json_k) put(writer, '"');
        break;
    }
    case json_token_t::number_k: {
        cell_number_t const number = read_cell_number(token);
        if (number.kind == cell_number_kind_t::real_k || number.kind == cell_number_kind_t::refused_k)
            return refused = 1, conversion_t::missing_k;
        if (number.integer.negative) put(writer, '-');
        put_integer(writer, number.integer.magnitude);
        conversion = conversion_t::converted_k;
        break;
    }
    case json_token_t::object_k:
    case json_token_t::array_k:
        if (!column) put_minified(writer, token);
        break;
    default: return refused = 1, conversion_t::missing_k;
    }
    if (text == cell_text_t::column_text_k) put(writer, '\0');
    return conversion;
}

#pragma endregion Cell Conversions

#pragma region Gather

/** The path a row reads where its field compiles into none, as @c no_cell_path_k . */
constant constexpr uint no_cell_path_k = 0xFFFFFFFFu;

/** Rows one lane of a gather converts, one byte of every bitmap, as @c gather_lane_rows_k . */
constant constexpr uint gather_lane_rows_k = 8;

/** The field types of scalar columns, as @c ustore_docs_field_type_t numbers them. */
constant constexpr uint field_type_bool_k = 11;
constant constexpr uint field_type_uuid_k = 12;
constant constexpr uint field_type_f16_k = 40;
constant constexpr uint field_type_f32_k = 41;
constant constexpr uint field_type_f64_k = 42;
constant constexpr uint field_type_i8_k = 20;
constant constexpr uint field_type_i16_k = 21;
constant constexpr uint field_type_i32_k = 22;
constant constexpr uint field_type_i64_k = 23;
constant constexpr uint field_type_u8_k = 30;
constant constexpr uint field_type_u16_k = 31;
constant constexpr uint field_type_u32_k = 32;
constant constexpr uint field_type_u64_k = 33;

/** One match a walk stored: its haystack, its path, and the bytes it spans, 64-bit each. */
struct gather_match_t {
    ulong haystack_index;
    ulong needle_index;
    ulong byte_offset;
    ulong byte_length;
};

/** The documents of a device gather and the matches its walks stored, as @c gather_page_t . */
struct gather_page_t {
    ulong bytes;
    ulong offsets;
    ulong matches;
    ulong found;
    uint paths_count;
    uint rows;
    ulong refused;
};

/** One scalar column of a device gather, as the host's @c gather_scalars_t . */
struct gather_scalars_t {
    ulong bitmaps;
    ulong scalars;
    ulong bitmap_stride;
    uint type;
    uint path;
};

/** One text output of a device gather or field read, as the host's @c gather_texts_t . */
struct gather_texts_t {
    cell_text_t text;
    uint path;
    ulong paths;
    ulong bitmaps;
    ulong bitmap_stride;
    ulong presences;
    ulong lengths;
    ulong measured;
    ulong offsets;
    ulong bytes;
};

/** A text column whose every row ends in a null byte and the column without them, as the host's
 *  @c terminated_texts . */
template <typename offset_type_>
struct terminated_texts {
    ulong bytes;
    ulong offsets;
    ulong bare_bytes;
    ulong bare_offsets;
    ulong rows;
};

/** The parameters of @c ustore_docs_gather_scalars , in order. */
struct docs_scalars_arguments_t {
    gather_page_t page;
    gather_scalars_t column;
};

/** The parameters of @c ustore_docs_measure_texts and @c ustore_docs_write_texts , in order. */
struct docs_texts_arguments_t {
    gather_page_t page;
    gather_texts_t texts;
};

/** The parameters of @c ustore_docs_pack_bools , in order. */
struct docs_pack_bools_arguments_t {
    ulong bools;
    ulong rows;
    ulong packed;
};

/** A @ref gather_page_t its kernel reached. */
struct page_view_t {
    device char const* bytes;
    device ulong const* offsets;
    device gather_match_t const* matches;
    device uint const* found;
    uint paths_count;
    uint rows;
    device atomic_uint* refused;
};

/** A @ref gather_scalars_t its kernel reached. */
struct scalars_view_t {
    device uchar* bitmaps;
    device uchar* scalars;
    ulong bitmap_stride;
    uint type;
    uint path;
};

/** A @ref gather_texts_t its kernel reached. */
struct texts_view_t {
    cell_text_t text;
    uint path;
    device uint const* paths;
    device uchar* bitmaps;
    ulong bitmap_stride;
    device uchar* presences;
    device ulong* lengths;
    device tape_source_t* measured;
    device ulong const* offsets;
    device char* bytes;
};

/** A @ref terminated_texts its kernel reached. */
template <typename offset_type_>
struct terminated_view {
    device uchar const* bytes;
    device offset_type_ const* offsets;
    device uchar* bare_bytes;
    device offset_type_* bare_offsets;
    uint rows;
};

/** Reaches the pointers of @p page through @p blocks . */
inline page_view_t reach_page(constant gather_page_t& page, blocks_t blocks) {
    page_view_t view;
    view.bytes = reach<char const>(blocks, page.bytes), view.offsets = reach<ulong const>(blocks, page.offsets);
    view.matches = reach<gather_match_t const>(blocks, page.matches);
    view.found = reach<uint const>(blocks, page.found);
    view.paths_count = page.paths_count, view.rows = page.rows;
    view.refused = reach<atomic_uint>(blocks, page.refused);
    return view;
}

/** Reaches the pointers of @p column through @p blocks . */
inline scalars_view_t reach_scalars(constant gather_scalars_t& column, blocks_t blocks) {
    scalars_view_t view;
    view.bitmaps = reach<uchar>(blocks, column.bitmaps), view.scalars = reach<uchar>(blocks, column.scalars);
    view.bitmap_stride = column.bitmap_stride, view.type = column.type, view.path = column.path;
    return view;
}

/** Reaches the pointers of @p texts through @p blocks . */
inline texts_view_t reach_texts(constant gather_texts_t& texts, blocks_t blocks) {
    texts_view_t view;
    view.text = texts.text, view.path = texts.path, view.paths = reach<uint const>(blocks, texts.paths);
    view.bitmaps = reach<uchar>(blocks, texts.bitmaps), view.bitmap_stride = texts.bitmap_stride;
    view.presences = reach<uchar>(blocks, texts.presences), view.lengths = reach<ulong>(blocks, texts.lengths);
    view.measured = reach<tape_source_t>(blocks, texts.measured);
    view.offsets = reach<ulong const>(blocks, texts.offsets), view.bytes = reach<char>(blocks, texts.bytes);
    return view;
}

/** Reaches the pointers of @p texts through @p blocks . */
template <typename offset_type_>
inline terminated_view<offset_type_> reach_terminated(constant terminated_texts<offset_type_>& texts, blocks_t blocks) {
    terminated_view<offset_type_> view;
    view.bytes = reach<uchar const>(blocks, texts.bytes);
    view.offsets = reach<offset_type_ const>(blocks, texts.offsets);
    view.bare_bytes = reach<uchar>(blocks, texts.bare_bytes);
    view.bare_offsets = reach<offset_type_>(blocks, texts.bare_offsets);
    view.rows = uint(texts.rows);
    return view;
}

/** The token of @p row 's document under @p path , empty where document or field is missing, as
 *  @c gather_page_t::cell . */
inline token_t page_cell(thread page_view_t const& page, uint row, uint path) {
    if (path == no_cell_path_k) return no_token();
    device gather_match_t const* const slots = page.matches + row * page.paths_count;
    for (uint slot = 0; slot != page.found[row]; ++slot)
        if (slots[slot].needle_index == path)
            return {page.bytes + page.offsets[row] + slots[slot].byte_offset, uint(slots[slot].byte_length)};
    return no_token();
}

/** The path @p row of @p texts reads, as @c gather_texts_t::path_of . */
inline uint text_path_of(thread texts_view_t const& texts, uint row) {
    return texts.paths ? texts.paths[row] : texts.path;
}

/** Writes the @p bytes low bytes of @p raw at @p row of @p scalars , as @c put_scalar . */
inline void put_scalar(device uchar* scalars, uint row, ulong raw, uint bytes) {
    for (uint index = 0; index != bytes; ++index) scalars[row * bytes + index] = uchar(raw >> (8 * index));
}

/** The kind a scalar column of field type @p type converts as, answering false for any other. */
inline bool scalar_kind_of(uint type, thread scalar_kind_t& kind) {
    switch (type) {
    case field_type_bool_k: kind = {scalar_family_t::boolean_k, 8}; return true;
    case field_type_f64_k: kind = {scalar_family_t::real_k, 64}; return true;
    case field_type_f32_k: kind = {scalar_family_t::real_k, 32}; return true;
    case field_type_i64_k: kind = {scalar_family_t::signed_k, 64}; return true;
    case field_type_i32_k: kind = {scalar_family_t::signed_k, 32}; return true;
    case field_type_i16_k: kind = {scalar_family_t::signed_k, 16}; return true;
    case field_type_i8_k: kind = {scalar_family_t::signed_k, 8}; return true;
    case field_type_u64_k: kind = {scalar_family_t::unsigned_k, 64}; return true;
    case field_type_u32_k: kind = {scalar_family_t::unsigned_k, 32}; return true;
    case field_type_u16_k: kind = {scalar_family_t::unsigned_k, 16}; return true;
    case field_type_u8_k: kind = {scalar_family_t::unsigned_k, 8}; return true;
    default: return false;
    }
}

/** Converts the rows of @p column from @p first_row on, eight or as many as remain, into its
 *  scalars and one byte of each bitmap, answering whether one is only the host's, as
 *  @c gather_scalar_rows . */
inline uint gather_scalar_rows(thread page_view_t const& page, thread scalars_view_t const& column, uint first_row) {
    scalar_kind_t kind = {scalar_family_t::boolean_k, 8};
    bool const scalar = scalar_kind_of(column.type, kind);
    if (!scalar && column.type != field_type_f16_k && column.type != field_type_uuid_k) return 1;
    uchar valid = 0, converted = 0, collides = 0;
    uint refused = 0;
    uint const end = page.rows - first_row < gather_lane_rows_k ? page.rows : first_row + gather_lane_rows_k;
    for (uint row = first_row; row != end; ++row) {
        token_t const token = page_cell(page, row, column.path);
        conversion_t conversion;
        if (column.type == field_type_uuid_k) {
            uchar uuid[uuid_bytes_k];
            conversion = cell_uuid(token, uuid, refused);
            for (uint index = 0; index != uuid_bytes_k; ++index)
                column.scalars[row * uuid_bytes_k + index] = uuid[index];
        }
        else if (column.type == field_type_f16_k) {
            ushort half = 0;
            conversion = cell_half(token, half, refused);
            put_scalar(column.scalars, row, half, 2);
        }
        else {
            ulong raw = 0;
            conversion = cell_scalar(token, kind, raw, refused);
            put_scalar(column.scalars, row, raw, kind.bits / 8);
        }
        record_conversion(conversion, uchar(1u << (row - first_row)), valid, converted, collides);
    }
    uint const slot = first_row / gather_lane_rows_k;
    column.bitmaps[slot] = valid;
    column.bitmaps[column.bitmap_stride + slot] = converted;
    column.bitmaps[2 * column.bitmap_stride + slot] = collides;
    return refused;
}

/** Measures the rows of @p texts from @p first_row on, eight or as many as remain, as
 *  @c measure_text_rows . */
inline uint measure_text_rows(thread page_view_t const& page, thread texts_view_t const& texts, uint first_row) {
    uchar valid = 0, converted = 0, collides = 0, present = 0;
    uint refused = 0;
    uint const end = page.rows - first_row < gather_lane_rows_k ? page.rows : first_row + gather_lane_rows_k;
    for (uint row = first_row; row != end; ++row) {
        token_t const token = page_cell(page, row, text_path_of(texts, row));
        text_writer_t writer = {nullptr, 0};
        conversion_t const conversion = cell_text(token, texts.text, writer, refused);
        uchar const mask = uchar(1u << (row - first_row));
        record_conversion(conversion, mask, valid, converted, collides);
        present = uchar(present | (token.size == 0 ? 0 : mask));
        texts.measured[row].length = writer.length;
        if (texts.lengths) texts.lengths[row] = writer.length;
    }
    uint const slot = first_row / gather_lane_rows_k;
    if (texts.bitmaps) {
        texts.bitmaps[slot] = valid;
        texts.bitmaps[texts.bitmap_stride + slot] = converted;
        texts.bitmaps[2 * texts.bitmap_stride + slot] = collides;
    }
    if (texts.presences) texts.presences[slot] = present;
    return refused;
}

/** Packs eight booleans of @p bools from @p first_row on into a byte of bits, as @c pack_bools . */
inline uchar pack_bools(device uchar const* bools, uint rows, uint first_row) {
    uchar packed = 0;
    uint const end = rows - first_row < gather_lane_rows_k ? rows : first_row + gather_lane_rows_k;
    for (uint row = first_row; row != end; ++row) packed = uchar(packed | (bools[row] ? 1u << (row - first_row) : 0u));
    return packed;
}

/** Copies row @p row of @p texts without its null byte, writing its offset, and after the last row
 *  the closing one, as @c drop_terminator . */
template <typename offset_type_>
inline void drop_terminator(thread terminated_view<offset_type_> const& texts, uint row) {
    offset_type_ const first = texts.offsets[0];
    offset_type_ const begin = texts.offsets[row], end = texts.offsets[row + 1];
    offset_type_ const bare_begin = offset_type_(begin - first - row);
    texts.bare_offsets[row] = bare_begin;
    if (row + 1 == texts.rows) texts.bare_offsets[texts.rows] = offset_type_(end - first - offset_type_(texts.rows));
    uint const length = end > begin ? uint(end - begin) - 1 : 0;
    device uchar const* const source = texts.bytes + begin;
    device uchar* const target = texts.bare_bytes + bare_begin;
    for (uint byte = 0; byte != length; ++byte) target[byte] = source[byte];
}

#pragma endregion Gather

#pragma region Select

/** The flags a two-pass select keeps and how it tiles them; no host struct mirrors it, as the CUDA
 *  chain it replaces takes its arguments one by one. */
struct select_arguments_t {

    /** The flags, a bit or a byte each. */
    ulong flags;

    /** Items flagged. */
    uint count;

    /** Items each threadgroup of the counting and writing passes takes. */
    uint tile;

    /** Tiles of the items, one threadgroup each, @c count divided by @c tile and rounded up. */
    uint tiles;

    /** Bits of one flag: 8 for a byte each as the merge ranks flag, 1 for a bitmap, the first item
     *  in the lowest bit, as the matching kernels mark. */
    uint flag_bits;

    /** One count per tile and the total after them, which the scan turns into offsets. */
    ulong counts;

    /** The index of every flagged item, in order, as many as the total counts. */
    ulong selected;
};

/** Whether item @p index of @p flags is flagged. */
inline bool flagged(device uchar const* flags, uint flag_bits, uint index) {
    return flag_bits == 1 ? (flags[index / 8] >> (index % 8) & 1u) != 0 : flags[index] != 0;
}

/** The first item of tile @p tile and the one past its last. */
inline uint2 select_tile(constant select_arguments_t& arguments, uint tile) {
    uint const first = min(tile * arguments.tile, arguments.count);
    return uint2(first, first + min(arguments.count - first, arguments.tile));
}

#pragma endregion Select

} // namespace ustore
} // namespace unum

/*  The header the host splices ahead declares the utilities, which the kernels name unqualified. */
namespace unum {
namespace ustore {
namespace utilities {}
} // namespace ustore
} // namespace unum

using namespace unum::ustore;
using namespace unum::ustore::utilities;

#pragma region Tape Kernels

/** Scans the lengths of the sources into the offsets and cuts the segments, on one threadgroup,
 *  as CUDA's @c ustore_tape_offsets_u32 . */
kernel void ustore_tape_offsets_u32(constant tape_offsets_arguments_t& arguments [[buffer(metal_arguments_slot_k)]],
                                    device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                    constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                    uint lane [[thread_index_in_threadgroup]]) {
    threadgroup uint simd_totals[block_simds_k];
    blocks_t const blocks = {table, table_count};
    tape_offsets_block(reach<tape_source_t const>(blocks, arguments.sources), uint(arguments.count),
                       arguments.sources_per_segment, reach<uint>(blocks, arguments.offsets),
                       reach<tape_segment_t>(blocks, arguments.segments), lane, simd_totals);
}

/** Scans the lengths of the sources into 64-bit offsets and cuts the segments, on one
 *  threadgroup, as CUDA's @c ustore_tape_offsets_u64 . */
kernel void ustore_tape_offsets_u64(constant tape_offsets_arguments_t& arguments [[buffer(metal_arguments_slot_k)]],
                                    device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                    constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                    uint lane [[thread_index_in_threadgroup]]) {
    threadgroup ulong simd_totals[block_simds_k];
    blocks_t const blocks = {table, table_count};
    tape_offsets_block(reach<tape_source_t const>(blocks, arguments.sources), uint(arguments.count),
                       arguments.sources_per_segment, reach<ulong>(blocks, arguments.offsets),
                       reach<tape_segment_t>(blocks, arguments.segments), lane, simd_totals);
}

/** Copies the strings of every segment into the tape's bytes, one threadgroup per segment, its
 *  threads writing neighbouring bytes together, as CUDA's @c ustore_tape_gather . */
kernel void ustore_tape_gather(constant tape_gather_arguments_t& arguments [[buffer(metal_arguments_slot_k)]],
                               device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                               constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                               uint group [[threadgroup_position_in_grid]], uint lane [[thread_index_in_threadgroup]],
                               uint threads [[threads_per_threadgroup]]) {
    blocks_t const blocks = {table, table_count};
    device tape_source_t const* const sources = reach<tape_source_t const>(blocks, arguments.sources);
    tape_segment_t const segment = reach<tape_segment_t const>(blocks, arguments.segments)[group];
    device uchar* written = reach<uchar>(blocks, arguments.bytes) + segment.first_byte;
    for (uint index = 0; index != segment.sources_count; ++index) {
        tape_source_t const source = sources[segment.first_source + index];
        uint const length = uint(source.length);
        device uchar const* const data = reach<uchar const>(blocks, source.data);
        for (uint byte = lane; byte < length; byte += threads) written[byte] = data[byte];
        written += length;
    }
}

/** Copies fixed-width rows a stride apart into one dense run, one row per thread, as CUDA's
 *  @c ustore_tape_densify . */
kernel void ustore_tape_densify(constant strided_rows_t& rows [[buffer(metal_arguments_slot_k)]],
                                device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                uint row [[thread_position_in_grid]]) {
    if (row >= rows.rows) return;
    blocks_t const blocks = {table, table_count};
    // A row may start past 4 GiB, though its own bytes are a 32-bit count
    device uchar const* const source = reach<uchar const>(blocks, rows.source) + ulong(row) * rows.stride;
    device uchar* const dense = reach<uchar>(blocks, rows.dense) + ulong(row) * rows.row_bytes;
    uint const row_bytes = uint(rows.row_bytes);
    for (uint byte = 0; byte != row_bytes; ++byte) dense[byte] = source[byte];
}

#pragma endregion Tape Kernels

#pragma region Gather Kernels

/** Converts the rows of one scalar column of a gather, eight per lane, as CUDA's
 *  @c ustore_docs_gather_scalars . */
kernel void ustore_docs_gather_scalars(constant docs_scalars_arguments_t& arguments [[buffer(metal_arguments_slot_k)]],
                                       device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                       constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                       uint slot [[thread_position_in_grid]]) {
    uint const first_row = slot * gather_lane_rows_k;
    if (first_row >= arguments.page.rows) return;
    blocks_t const blocks = {table, table_count};
    page_view_t const page = reach_page(arguments.page, blocks);
    scalars_view_t const column = reach_scalars(arguments.column, blocks);
    if (gather_scalar_rows(page, column, first_row)) atomic_fetch_or_explicit(page.refused, 1u, memory_order_relaxed);
}

/** Measures the rows of one text output of a gather or a read, eight per lane, as CUDA's
 *  @c ustore_docs_measure_texts . */
kernel void ustore_docs_measure_texts(constant docs_texts_arguments_t& arguments [[buffer(metal_arguments_slot_k)]],
                                      device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                      constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                      uint slot [[thread_position_in_grid]]) {
    uint const first_row = slot * gather_lane_rows_k;
    if (first_row >= arguments.page.rows) return;
    blocks_t const blocks = {table, table_count};
    page_view_t const page = reach_page(arguments.page, blocks);
    texts_view_t const texts = reach_texts(arguments.texts, blocks);
    if (measure_text_rows(page, texts, first_row)) atomic_fetch_or_explicit(page.refused, 1u, memory_order_relaxed);
}

/** Writes the rows of one measured and scanned text output, one per lane, as CUDA's
 *  @c ustore_docs_write_texts . */
kernel void ustore_docs_write_texts(constant docs_texts_arguments_t& arguments [[buffer(metal_arguments_slot_k)]],
                                    device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                    constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                    uint row [[thread_position_in_grid]]) {
    if (row >= arguments.page.rows) return;
    blocks_t const blocks = {table, table_count};
    page_view_t const page = reach_page(arguments.page, blocks);
    texts_view_t const texts = reach_texts(arguments.texts, blocks);
    uint refused = 0;
    text_writer_t writer = {texts.bytes + texts.offsets[row], 0};
    cell_text(page_cell(page, row, text_path_of(texts, row)), texts.text, writer, refused);
}

/** Packs a boolean column into Arrow's bits, eight rows per lane, as CUDA's
 *  @c ustore_docs_pack_bools . */
kernel void ustore_docs_pack_bools(constant docs_pack_bools_arguments_t& arguments [[buffer(metal_arguments_slot_k)]],
                                   device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                   constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                   uint slot [[thread_position_in_grid]]) {
    uint const rows = uint(arguments.rows);
    uint const first_row = slot * gather_lane_rows_k;
    if (first_row >= rows) return;
    blocks_t const blocks = {table, table_count};
    reach<uchar>(blocks, arguments.packed)[slot] = pack_bools(reach<uchar const>(blocks, arguments.bools), rows,
                                                              first_row);
}

/** Copies a text column of 32-bit offsets without the null byte ending each row, a row per lane,
 *  as CUDA's @c ustore_docs_drop_terminators_u32 . */
kernel void ustore_docs_drop_terminators_u32(constant terminated_texts<uint>& texts [[buffer(metal_arguments_slot_k)]],
                                             device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                             constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                             uint row [[thread_position_in_grid]]) {
    if (row >= texts.rows) return;
    blocks_t const blocks = {table, table_count};
    drop_terminator(reach_terminated(texts, blocks), row);
}

/** Copies a text column of 64-bit offsets without the null byte ending each row, a row per lane,
 *  as CUDA's @c ustore_docs_drop_terminators_u64 . */
kernel void ustore_docs_drop_terminators_u64(constant terminated_texts<ulong>& texts [[buffer(metal_arguments_slot_k)]],
                                             device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                             constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                             uint row [[thread_position_in_grid]]) {
    if (row >= texts.rows) return;
    blocks_t const blocks = {table, table_count};
    drop_terminator(reach_terminated(texts, blocks), row);
}

#pragma endregion Gather Kernels

#pragma region Select Kernels

/** Counts the flagged items of one tile per threadgroup, the first pass of the select replacing
 *  the chain of @c vram_scan.cuh . */
kernel void ustore_select_count_apple(constant select_arguments_t& arguments [[buffer(metal_arguments_slot_k)]],
                                      device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                      constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                      uint tile [[threadgroup_position_in_grid]],
                                      uint lane [[thread_index_in_threadgroup]]) {
    threadgroup uint simd_totals[block_simds_k];
    blocks_t const blocks = {table, table_count};
    device uchar const* const flags = reach<uchar const>(blocks, arguments.flags);
    uint2 const range = select_tile(arguments, tile);
    uint kept = 0;
    for (uint index = range.x + lane; index < range.y; index += block_threads_k)
        kept += flagged(flags, arguments.flag_bits, index) ? 1 : 0;
    block_scanned<uint> const scan = block_exclusive(kept, lane, simd_totals);
    if (lane == 0) reach<uint>(blocks, arguments.counts)[tile] = scan.total;
}

/** Scans the tiles' counts in place into the items flagged before each, and the total after the
 *  last, on one threadgroup, the middle pass of the select. */
kernel void ustore_select_scan_apple(constant select_arguments_t& arguments [[buffer(metal_arguments_slot_k)]],
                                     device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                     constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                     uint lane [[thread_index_in_threadgroup]]) {
    threadgroup uint simd_totals[block_simds_k];
    blocks_t const blocks = {table, table_count};
    device uint* const counts = reach<uint>(blocks, arguments.counts);
    uint carried = 0;
    for (uint base = 0; base < arguments.tiles; base += block_threads_k) {
        uint const index = base + lane;
        uint const count = index < arguments.tiles ? counts[index] : 0;
        block_scanned<uint> const scan = block_exclusive(count, lane, simd_totals);
        if (index < arguments.tiles) counts[index] = carried + scan.before;
        carried += scan.total;
    }
    if (lane == 0) counts[arguments.tiles] = carried;
}

/** Writes the index of every flagged item of one tile per threadgroup, in the order of the items,
 *  from its tile's scanned count on, the last pass of the select. */
kernel void ustore_select_write_apple(constant select_arguments_t& arguments [[buffer(metal_arguments_slot_k)]],
                                      device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                      constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                      uint tile [[threadgroup_position_in_grid]],
                                      uint lane [[thread_index_in_threadgroup]]) {
    threadgroup uint simd_totals[block_simds_k];
    blocks_t const blocks = {table, table_count};
    device uchar const* const flags = reach<uchar const>(blocks, arguments.flags);
    device uint* const selected = reach<uint>(blocks, arguments.selected);
    uint2 const range = select_tile(arguments, tile);
    uint before = reach<uint const>(blocks, arguments.counts)[tile];
    for (uint base = range.x; base < range.y; base += block_threads_k) {
        uint const index = base + lane;
        bool const kept = index < range.y && flagged(flags, arguments.flag_bits, index);
        block_scanned<uint> const scan = block_exclusive(kept ? 1u : 0u, lane, simd_totals);
        if (kept) selected[before + scan.before] = index;
        before += scan.total;
    }
}

#pragma endregion Select Kernels
