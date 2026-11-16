/**
 *  @file src_stub/arrow.cpp
 *  @author Ash Vardanian
 *  @date November 16, 2026
 *  @brief The Arrow converters and exporters over the stand-in's host arenas: keys and tapes read
 *      in place, and the outputs a call wrote exported as columns that read its arena in place.
 *
 *  Every arena lives on host pages, so every column exports as @c ARROW_DEVICE_CPU. What
 *  Arrow lays out unlike the arena, strided keys, a tape of the other width, packed booleans
 *  and NUL-terminated strings, is copied behind the call's outputs under the arena's claim,
 *  as the engine does.
 */
#include <limits> // `std::numeric_limits` - a count whose bytes wrap

#include "lib.hpp"

using namespace unum::ustore::stub;

namespace unum::ustore::stub {

/** The format of a column, or empty for one missing or released. */
std::string_view format_of(ArrowSchema const* schema, ArrowArray const* array) noexcept {
    if (!schema || !array || !schema->release || !array->release || !schema->format) return {};
    return schema->format;
}

/** Refuses a column of the right format that is dictionary-encoded, holds a null, or carries
 *  fewer than @p buffers buffers, reading only its lengths and pointers. */
ustore_status_t check_column(ArrowSchema const& schema, ArrowArray const& array, std::int64_t buffers) noexcept {
    if (schema.dictionary || array.dictionary) return ustore_status_unsupported_k;
    if (array.length < 0 || array.offset < 0 || array.n_buffers < buffers || !array.buffers)
        return ustore_status_malformed_k;
    // A count of -1 is unknown, so only a missing validity buffer rules nulls out
    if (array.null_count > 0 || (array.null_count < 0 && array.buffers[0])) return ustore_status_invalid_argument_k;
    return ustore_status_success_k;
}

/** Where an exported column of no rows points its buffers, as consumers may read the one offset
 *  of an empty tape. */
constexpr std::uint64_t no_rows_k[1] {};

/** The metadata tagging a column as the @c arrow.uuid extension: the count of pairs, then each
 *  key and value behind its length, every count a little-endian 32-bit integer. */
constexpr auto uuid_metadata_k = [] {
    std::array<char, 96> bytes {};
    std::size_t at = 0;
    auto append_count = [&](std::uint32_t count) {
        for (unsigned index = 0; index != 4; ++index) bytes[at++] = static_cast<char>((count >> (8 * index)) & 0xFF);
    };
    auto append_text = [&](std::string_view text) {
        append_count(static_cast<std::uint32_t>(text.size()));
        for (char character : text) bytes[at++] = character;
    };
    append_count(2);
    append_text("ARROW:extension:name"), append_text("arrow.uuid");
    append_text("ARROW:extension:metadata"), append_text("");
    return bytes;
}();

/** The buffers an exported column points at, which its @c release frees with it. */
struct exported_column_t {
    std::array<void const*, 3> buffers {};
};

void release_exported_schema(ArrowSchema* schema) noexcept { schema->release = nullptr; }

void release_exported_column(ArrowArray* array) noexcept {
    delete static_cast<exported_column_t*>(array->private_data);
    array->release = nullptr;
}

/** Exports @p rows rows of @p buffers, laid out as @p format says, into @p schema and @p array. */
ustore_status_t export_column(std::size_t rows, std::array<void const*, 3> buffers, std::int64_t buffers_count,
                              char const* format, char const* metadata, ArrowSchema* schema,
                              ArrowDeviceArray* array) noexcept {
    auto* const column = new (std::nothrow) exported_column_t {buffers};
    if (!column) return ustore_status_out_of_memory_k;
    *schema = {};
    schema->format = format, schema->name = "", schema->metadata = metadata;
    schema->release = &release_exported_schema;
    *array = {};
    array->array.length = static_cast<std::int64_t>(rows);
    // A validity buffer leaves the count of nulls unknown, as no export counts them
    array->array.null_count = buffers[0] ? -1 : 0;
    array->array.n_buffers = buffers_count;
    array->array.buffers = column->buffers.data();
    array->array.release = &release_exported_column;
    array->array.private_data = column;
    array->device_id = -1;
    array->device_type = ARROW_DEVICE_CPU;
    return ustore_status_success_k;
}

/** Whether the @p bytes at @p data lie inside one block of @p arena. */
bool arena_holds(ustore_arena_handle_t const& arena, void const* data, std::size_t bytes) noexcept {
    if (!bytes) return true;
    if (!data) return false;
    auto const* const first = static_cast<std::byte const*>(data);
    for (auto const& block : arena.blocks)
        if (first >= block.data && first <= block.data + block.size &&
            bytes <= static_cast<std::size_t>(block.data + block.size - first))
            return true;
    return false;
}

/** Whether the @p count + 1 offsets of @p width bytes at @p offsets lie inside @p arena,
 *  refusing a count whose bytes wrap around to the empty run an arena holds anywhere. */
bool offsets_held(ustore_arena_handle_t const& arena, void const* offsets, std::size_t count,
                  std::size_t width) noexcept {
    if (count >= (std::numeric_limits<std::size_t>::max)() / width) return false;
    return arena_holds(arena, offsets, (count + 1) * width);
}

/** Whether the offsets of the @p count strings of a tape and the bytes they span lie
 *  inside @p arena. */
bool arena_holds_tape(ustore_arena_handle_t const& arena, ustore_strings_view_t const& strings,
                      std::size_t count) noexcept {
    bool const wide = strings.layout == ustore_strings_u64_tape_k;
    void const* const offsets = wide ? static_cast<void const*>(strings.data.u64_tape.offsets)
                                     : strings.data.u32_tape.offsets;
    ustore_byte_t const* const data = wide ? strings.data.u64_tape.data : strings.data.u32_tape.data;
    if (!offsets_held(arena, offsets, count, wide ? 8 : 4)) return false;
    std::uint64_t const first = wide ? strings.data.u64_tape.offsets[0] : strings.data.u32_tape.offsets[0];
    std::uint64_t const end = wide ? strings.data.u64_tape.offsets[count] : strings.data.u32_tape.offsets[count];
    return end >= first && (end == first || arena_holds(arena, data + first, end - first));
}

/** The Arrow format of a gathered @p type column of @p layout, or null for a type no column has. */
char const* docs_column_format(ustore_docs_field_type_t type, ustore_strings_layout_t layout) noexcept {
    bool const wide = layout == ustore_strings_u64_tape_k;
    switch (type) {
    case ustore_docs_field_type_bool_k: return "b";
    case ustore_docs_field_type_uuid_k: return "w:16";
    case ustore_docs_field_type_f64_k: return "g";
    case ustore_docs_field_type_f32_k: return "f";
    case ustore_docs_field_type_f16_k: return "e";
    case ustore_docs_field_type_i64_k: return "l";
    case ustore_docs_field_type_i32_k: return "i";
    case ustore_docs_field_type_i16_k: return "s";
    case ustore_docs_field_type_i8_k: return "c";
    case ustore_docs_field_type_u64_k: return "L";
    case ustore_docs_field_type_u32_k: return "I";
    case ustore_docs_field_type_u16_k: return "S";
    case ustore_docs_field_type_u8_k: return "C";
    case ustore_docs_field_type_binary_k: return wide ? "Z" : "z";
    case ustore_docs_field_type_string_k: return wide ? "U" : "u";
    default: return nullptr;
    }
}

/** Copies the offsets of a tape into the other width behind the outputs of @p arena, counting
 *  from its first byte. */
template <typename converted_type_, typename source_type_>
converted_type_* convert_offsets(ustore_arena_handle_t& arena, source_type_ const* source, std::size_t rows) noexcept {
    auto* const converted = reinterpret_cast<converted_type_*>(arena.allocate((rows + 1) * sizeof(converted_type_)));
    if (!converted) return nullptr;
    for (std::size_t index = 0; index != rows + 1; ++index)
        converted[index] = static_cast<converted_type_>(source[index] - source[0]);
    return converted;
}

/** Copies the @p rows strings of a gathered string column into @p arena as a tape of the same
 *  width without the NUL ending every row, refusing a row that lacks it. */
template <typename offset_type_>
ustore_status_t drop_terminators(ustore_arena_handle_t& arena, offset_type_ const* offsets, ustore_byte_t const* bytes,
                                 std::size_t rows, std::array<void const*, 3>& buffers) noexcept {
    for (std::size_t row = 0; row != rows; ++row)
        if (offsets[row + 1] <= offsets[row] || bytes[offsets[row + 1] - 1] != 0)
            return ustore_status_invalid_argument_k;
    auto* const dropped = reinterpret_cast<offset_type_*>(arena.allocate((rows + 1) * sizeof(offset_type_)));
    auto* const copied = reinterpret_cast<ustore_byte_t*>(arena.allocate(offsets[rows] - offsets[0]));
    if (!dropped || !copied) return ustore_status_out_of_memory_k;
    offset_type_ written = 0;
    for (std::size_t row = 0; row != rows; ++row) {
        offset_type_ const length = offsets[row + 1] - offsets[row] - 1;
        dropped[row] = written;
        std::memcpy(copied + written, bytes + offsets[row], static_cast<std::size_t>(length));
        written += length;
    }
    dropped[rows] = written;
    buffers[1] = dropped, buffers[2] = copied;
    return ustore_status_success_k;
}

} // namespace unum::ustore::stub

ustore_status_t ustore_keys_view_from_arrow(ArrowSchema const* schema, ArrowArray const* array,
                                            ustore_keys_view_t* keys, ustore_size_t* count) {
    std::string_view const format = format_of(schema, array);
    if (!keys || !count || format.empty()) return ustore_status_invalid_argument_k;
    if (format != "w:16") return ustore_status_unsupported_k;
    if (ustore_status_t const checked = check_column(*schema, *array, 2); checked != ustore_status_success_k)
        return checked;
    auto const* const data = static_cast<ustore_key_t const*>(array->buffers[1]);
    if (!data && array->length) return ustore_status_malformed_k;
    *keys = {data ? data + array->offset : nullptr, sizeof(ustore_key_t)};
    *count = static_cast<ustore_size_t>(array->length);
    return ustore_status_success_k;
}

ustore_status_t ustore_strings_view_from_arrow(ArrowSchema const* schema, ArrowArray const* array,
                                               ustore_strings_view_t* strings, ustore_size_t* count) {
    std::string_view const format = format_of(schema, array);
    if (!strings || !count || format.empty()) return ustore_status_invalid_argument_k;
    bool const narrow = format == "z" || format == "u";
    if (!narrow && format != "Z" && format != "U") return ustore_status_unsupported_k;
    if (ustore_status_t const checked = check_column(*schema, *array, 3); checked != ustore_status_success_k)
        return checked;
    void const* const offsets = array->buffers[1];
    if (!offsets) return ustore_status_malformed_k;
    auto const* const bytes = static_cast<ustore_byte_t const*>(array->buffers[2]);
    *strings = {};
    if (narrow) {
        strings->layout = ustore_strings_u32_tape_k;
        strings->data.u32_tape = {bytes, static_cast<std::uint32_t const*>(offsets) + array->offset};
    }
    else {
        strings->layout = ustore_strings_u64_tape_k;
        strings->data.u64_tape = {bytes, static_cast<std::uint64_t const*>(offsets) + array->offset};
    }
    *count = static_cast<ustore_size_t>(array->length);
    return ustore_status_success_k;
}

ustore_status_t ustore_keys_view_to_arrow(ustore_keys_view_t keys, ustore_size_t count, ustore_arena_t arena,
                                          ArrowSchema* schema, ArrowDeviceArray* array) {
    if (!arena || !schema || !array || (count && !keys.data)) return ustore_status_invalid_argument_k;
    std::size_t const rows = static_cast<std::size_t>(count);
    void const* data = count ? static_cast<void const*>(keys.data) : no_rows_k;
    if (count > 1 && keys.stride != sizeof(ustore_key_t)) {
        // Strided keys, like the triples of edges, are copied dense behind the call's outputs
        // under the claim
        if (!arena->claim.take(arena)) return ustore_status_contended_k;
        auto* const dense = reinterpret_cast<ustore_key_t*>(arena->allocate(rows * sizeof(ustore_key_t)));
        for (std::size_t index = 0; dense && index != rows; ++index)
            dense[index] = strided_element(keys.data, keys.stride, index);
        arena->claim.release();
        if (!dense) return ustore_status_out_of_memory_k;
        data = dense;
    }
    else if (!arena_holds(*arena, keys.data, rows * sizeof(ustore_key_t))) return ustore_status_invalid_argument_k;
    return export_column(rows, {nullptr, data, nullptr}, 2, "w:16", uuid_metadata_k.data(), schema, array);
}

ustore_status_t ustore_strings_view_to_arrow(ustore_strings_view_t strings, ustore_size_t count,
                                             ustore_strings_layout_t layout, ustore_arena_t arena, ArrowSchema* schema,
                                             ArrowDeviceArray* array) {
    if (!arena || !schema || !array) return ustore_status_invalid_argument_k;
    bool const wide = strings.layout == ustore_strings_u64_tape_k;
    if (!wide && strings.layout != ustore_strings_u32_tape_k) return ustore_status_invalid_argument_k;
    if (layout != ustore_strings_u32_tape_k && layout != ustore_strings_u64_tape_k)
        return ustore_status_invalid_argument_k;
    void const* const offsets = wide ? static_cast<void const*>(strings.data.u64_tape.offsets)
                                     : strings.data.u32_tape.offsets;
    ustore_byte_t const* data = wide ? strings.data.u64_tape.data : strings.data.u32_tape.data;
    if (count && !offsets) return ustore_status_invalid_argument_k;
    char const* const format = layout == ustore_strings_u64_tape_k ? "Z" : "z";
    std::size_t const rows = static_cast<std::size_t>(count);
    if (!count || layout == strings.layout) {
        if (count && !arena_holds_tape(*arena, strings, rows)) return ustore_status_invalid_argument_k;
        void const* const exported_offsets = count ? offsets : no_rows_k;
        void const* const exported_data = data ? static_cast<void const*>(data) : no_rows_k;
        return export_column(rows, {nullptr, exported_offsets, exported_data}, 3, format, nullptr, schema, array);
    }
    if (!offsets_held(*arena, offsets, rows, wide ? 8 : 4)) return ustore_status_invalid_argument_k;
    std::uint64_t const first = wide ? strings.data.u64_tape.offsets[0] : strings.data.u32_tape.offsets[0];
    std::uint64_t const end = wide ? strings.data.u64_tape.offsets[rows] : strings.data.u32_tape.offsets[rows];
    if (!wide || end - first <= 0xFFFFFFFFull) {
        // A tape of the other width has its offsets converted behind the call's outputs, counted
        // from its first byte
        if (!arena->claim.take(arena)) return ustore_status_contended_k;
        void const* converted =
            wide
                ? static_cast<void const*>(convert_offsets<std::uint32_t>(*arena, strings.data.u64_tape.offsets, rows))
                : static_cast<void const*>(convert_offsets<std::uint64_t>(*arena, strings.data.u32_tape.offsets, rows));
        arena->claim.release();
        if (!converted) return ustore_status_out_of_memory_k;
        data = data ? data + first : data;
        void const* const exported_data = data ? static_cast<void const*>(data) : no_rows_k;
        return export_column(rows, {nullptr, converted, exported_data}, 3, format, nullptr, schema, array);
    }
    return ustore_status_capacity_exhausted_k;
}

ustore_status_t ustore_docs_column_view_to_arrow(ustore_docs_column_view_t const* column, ustore_size_t count,
                                                 ustore_docs_field_type_t type, ustore_arena_t arena,
                                                 ArrowSchema* schema, ArrowDeviceArray* array) {
    if (!column || !arena || !schema || !array) return ustore_status_invalid_argument_k;
    if (!docs_column_format(type, ustore_strings_u64_tape_k)) return ustore_status_invalid_argument_k;
    std::size_t const rows = static_cast<std::size_t>(count);
    bool const texts = type == ustore_docs_field_type_binary_k || type == ustore_docs_field_type_string_k;
    if (rows && !arena_holds(*arena, column->validities, (rows + 7) / 8)) return ustore_status_invalid_argument_k;
    if (rows && !texts && !column->scalars) return ustore_status_invalid_argument_k;
    if (rows && texts && !arena_holds_tape(*arena, column->strings, rows)) return ustore_status_invalid_argument_k;

    // Every column takes the claim, as a boolean or text one copies its conversion past the outputs
    if (!arena->claim.take(arena)) return ustore_status_contended_k;
    std::array<void const*, 3> buffers {column->validities, column->scalars, nullptr};
    ustore_status_t status = ustore_status_success_k;
    if (rows && type == ustore_docs_field_type_bool_k) {
        auto* const packed = reinterpret_cast<ustore_octet_t*>(arena->allocate((rows + 7) / 8));
        if (!packed) status = ustore_status_out_of_memory_k;
        for (std::size_t row = 0; packed && row != rows; ++row)
            if (column->scalars[row]) set_bit(packed, row);
        buffers[1] = packed;
    }
    else if (rows && texts) {
        bool const wide = column->strings.layout == ustore_strings_u64_tape_k;
        ustore_byte_t const* const bytes = wide ? column->strings.data.u64_tape.data
                                                : column->strings.data.u32_tape.data;
        if (type == ustore_docs_field_type_binary_k)
            buffers[1] = wide ? static_cast<void const*>(column->strings.data.u64_tape.offsets)
                              : column->strings.data.u32_tape.offsets,
            buffers[2] = bytes;
        // Strings drop the NUL ending every row, copied into a tape of the same width
        else if (wide) status = drop_terminators(*arena, column->strings.data.u64_tape.offsets, bytes, rows, buffers);
        else status = drop_terminators(*arena, column->strings.data.u32_tape.offsets, bytes, rows, buffers);
    }
    arena->claim.release();
    if (status != ustore_status_success_k) return status;
    if (!rows) buffers = {nullptr, no_rows_k, texts ? no_rows_k : nullptr};
    char const* const format = docs_column_format(type, column->strings.layout);
    char const* const metadata = type == ustore_docs_field_type_uuid_k ? uuid_metadata_k.data() : nullptr;
    return export_column(rows, buffers, texts ? 3 : 2, format, metadata, schema, array);
}
