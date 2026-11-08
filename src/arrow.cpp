/**
 *  @file src/arrow.cpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief Implements <tt>c/ustore/arrow.h</tt>: Arrow columns read into views and views exported as Arrow columns that
 *      read their arena's outputs until its next call or free.
 *
 *  An export that carves behind the outputs, dense keys or offsets of another width, takes
 *  the claim as a call does, so neither a call nor another carving export touches the
 *  arena's blocks meanwhile.
 */

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::int64_t`, `std::uint32_t`, `std::uint64_t`, `std::uintptr_t`

#include <array>            // `std::array`
#include <bit>              // `std::endian`
#include <initializer_list> // `std::initializer_list`
#include <limits>           // `std::numeric_limits`
#include <new>              // `std::nothrow`
#include <span>             // `std::span`
#include <string_view>      // `std::string_view`
#include <type_traits>      // `std::is_same_v`
#include <variant>          // `std::variant`, `std::visit`

#include "ustore/arrow.h" // `ustore_keys_view_from_arrow`, `ustore_strings_view_to_arrow`

#include "ustore/docs/vram_gather.cuh" // `pack_bools`, `terminated_texts`, `drop_terminator`

#include "lib.hpp" // `take`, `release`, `arena_allocate`, `host_blocks_of`, `strided`, `gpu_runtime`, `carve`

namespace unum::ustore {

#pragma region Converters

/** The format of a column, or empty for one missing or released. */
std::string_view format_of(ArrowSchema const* schema, ArrowArray const* array) noexcept {
    if (!schema || !array || !schema->release || !array->release || !schema->format) return {};
    return schema->format;
}

/** Refuses a column of the right format that is dictionary-encoded, holds a null, or carries fewer than @p buffers
 *  buffers, reading only its lengths and pointers. */
ustore_status_t check_column(ArrowSchema const& schema, ArrowArray const& array, std::int64_t buffers) noexcept {
    if (schema.dictionary || array.dictionary) return ustore_status_unsupported_k;
    if (array.length < 0 || array.offset < 0 || array.n_buffers < buffers || !array.buffers)
        return ustore_status_malformed_k;
    // A count of -1 is unknown, so only a missing validity buffer rules nulls out
    if (array.null_count > 0 || (array.null_count < 0 && array.buffers[0])) return ustore_status_invalid_argument_k;
    return ustore_status_success_k;
}

#pragma endregion Converters

#pragma region Exporters

/** Where an exported column of no rows points its buffers, as consumers may read the one offset of an empty tape. */
inline constexpr std::uint64_t no_rows_k[1] {};

/** One key and its value in the metadata of a schema. */
struct metadata_pair_t {
    std::string_view key;
    std::string_view value;
};

/** Bytes the metadata of an exported column takes at most. */
constexpr std::size_t metadata_capacity_k = 96;

/** Encodes @p pairs as Arrow metadata: the count of pairs, then every key and value behind its length, each count a
 *  32-bit integer in the native byte order. */
consteval std::array<char, metadata_capacity_k> encode_metadata(std::initializer_list<metadata_pair_t> pairs) noexcept {
    std::array<char, metadata_capacity_k> bytes {};
    std::size_t length = 0;
    auto const append_count = [&](std::size_t count) {
        for (std::size_t byte = 0; byte != sizeof(std::int32_t); ++byte) {
            std::size_t const shift = 8 * (std::endian::native == std::endian::little ? byte : 3 - byte);
            bytes[length++] = static_cast<char>((count >> shift) & 0xFF);
        }
    };
    auto const append_text = [&](std::string_view text) {
        append_count(text.size());
        for (char const character : text) bytes[length++] = character;
    };
    append_count(pairs.size());
    for (metadata_pair_t const& pair : pairs) append_text(pair.key), append_text(pair.value);
    return bytes;
}

inline constexpr std::array<char, metadata_capacity_k> uuid_metadata_k = encode_metadata(
    {{"ARROW:extension:name", "arrow.uuid"}, {"ARROW:extension:metadata", ""}});

/** What an exported column owns: the buffers it points at. */
struct exported_column_t {
    std::array<void const*, 3> buffers {};
};

/** The device runs of @p arena, with the Arrow device type and ordinal they live on, null
 *  for host pages. */
struct device_arena_t {
    device_blocks_t const* blocks = nullptr;
    ArrowDeviceType type = ARROW_DEVICE_CPU;
    std::int64_t device = -1;
};

device_arena_t device_arena_of(ustore_arena_t arena) noexcept {
    if (cuda_blocks_t const* const cuda = std::get_if<cuda_blocks_t>(&arena->memory))
        return {&cuda->blocks, ARROW_DEVICE_CUDA, cuda->device};
    if (hip_blocks_t const* const hip = std::get_if<hip_blocks_t>(&arena->memory))
        return {&hip->blocks, ARROW_DEVICE_ROCM, hip->device};
    return {};
}

/** Whether the @p bytes at @p first lie inside one run of @p blocks, of which only the newest
 *  is partly used. */
bool device_blocks_hold(device_blocks_t const& blocks, std::uintptr_t first, std::size_t bytes) noexcept {
    for (std::uint32_t block = 0; block != blocks.count; ++block) {
        device_block_t const& run = blocks.blocks[block];
        std::size_t const used = block + 1 == blocks.count ? blocks.used : run.capacity;
        if (first >= run.address && first - run.address + bytes <= used) return true;
    }
    return false;
}

/** Whether the @p bytes at @p data lie inside one block @p arena handed out; an empty run lies anywhere. */
bool arena_holds(ustore_arena_t arena, void const* data, std::size_t bytes) noexcept {
    if (!bytes) return true;
    if (device_arena_t const device = device_arena_of(arena); device.blocks)
        return data && device_blocks_hold(*device.blocks, reinterpret_cast<std::uintptr_t>(data), bytes);
    host_blocks_t const* const blocks = host_blocks_of(arena);
    if (!blocks || !data) return false;
    std::uintptr_t const first = reinterpret_cast<std::uintptr_t>(data);
    for (arena_block_t const* block = blocks->newest; block; block = block->previous) {
        std::uintptr_t const begin = reinterpret_cast<std::uintptr_t>(block);
        if (first >= begin && first - begin + bytes <= block->used) return true;
    }
    return false;
}

/** Whether the offsets of the @p count strings of a tape and the bytes they span lie inside @p arena. */
bool arena_holds_tape(ustore_arena_t arena, ustore_strings_view_t const& strings, std::size_t count) noexcept {
    bool const wide = strings.layout == ustore_strings_u64_tape_k;
    void const* const offsets = wide ? static_cast<void const*>(strings.data.u64_tape.offsets)
                                     : strings.data.u32_tape.offsets;
    ustore_byte_t const* const data = wide ? strings.data.u64_tape.data : strings.data.u32_tape.data;
    std::size_t const width = wide ? sizeof(std::uint64_t) : sizeof(std::uint32_t);
    // The host reads no device offsets, so a device tape is held where its offsets and its
    // first byte are
    if (device_arena_of(arena).blocks)
        return arena_holds(arena, offsets, (count + 1) * width) && (!data || arena_holds(arena, data, 1));
    std::uint64_t const first = wide ? strings.data.u64_tape.offsets[0] : strings.data.u32_tape.offsets[0];
    std::uint64_t const end = wide ? strings.data.u64_tape.offsets[count] : strings.data.u32_tape.offsets[count];
    return arena_holds(arena, offsets, (count + 1) * width) && end >= first &&
           arena_holds(arena, data + first, end - first);
}

/** Copies the @p converted.size() offsets at @p source into @p converted, less @p first, in the other width. */
template <typename converted_type_, typename source_type_>
void convert_offsets(source_type_ const* source, std::span<converted_type_> converted, std::uint64_t first) noexcept {
    for (std::size_t index = 0; index != converted.size(); ++index)
        converted[index] = static_cast<converted_type_>(source[index] - first);
}

/** Converts the tape @p strings of @p rows strings on host pages into the other width behind the
 *  call's outputs in @p arena, pointing @p converted at the new offsets and moving @p data to the
 *  first byte they count from. */
ustore_status_t convert_on_host(ustore_arena_t arena, ustore_strings_view_t const& strings, std::size_t rows,
                                void const*& converted, ustore_byte_t const*& data) noexcept {
    bool const wide = strings.layout == ustore_strings_u64_tape_k;
    // A span past 32 bits is refused before its bytes are looked for, as no arena holds that
    // many of them
    std::uint64_t const first = wide ? strings.data.u64_tape.offsets[0] : 0;
    std::uint64_t const end = wide ? strings.data.u64_tape.offsets[rows] : 0;
    if (end >= first && end - first > std::numeric_limits<std::uint32_t>::max())
        return ustore_status_capacity_exhausted_k;
    if (!arena_holds_tape(arena, strings, rows)) return ustore_status_invalid_argument_k;
    if (wide) {
        auto [narrowed, carve_status] = arena_allocate<std::uint32_t>(arena, rows + 1);
        if (carve_status.failed()) return static_cast<ustore_status_t>(carve_status.code);
        convert_offsets(strings.data.u64_tape.offsets, narrowed, first);
        converted = narrowed.data();
        data += first;
        return ustore_status_success_k;
    }
    auto [widened, carve_status] = arena_allocate<std::uint64_t>(arena, rows + 1);
    if (carve_status.failed()) return static_cast<ustore_status_t>(carve_status.code);
    convert_offsets(strings.data.u32_tape.offsets, widened, 0);
    converted = widened.data();
    return ustore_status_success_k;
}

/** Converts the @p rows + 1 offsets at @p offsets on @p executor's device into @p blocks in the
 *  other width, through the executor's bounce, pointing @p converted at them and setting @p first
 *  to the offset a narrowing counts from. */
template <typename converted_type_, typename source_type_, gpu_executor_concept executor_type_>
ustore_status_t convert_through(executor_type_& executor, device_blocks_t& blocks, source_type_ const* offsets,
                                std::size_t rows, void const*& converted, std::uint64_t& first) noexcept {
    ustore_arena_t const bounce = executor.bounce();
    auto [source, source_status] = arena_allocate<source_type_>(bounce, rows + 1);
    if (source_status.failed()) return static_cast<ustore_status_t>(source_status.code);
    auto [target, target_status] = arena_allocate<converted_type_>(bounce, rows + 1);
    if (target_status.failed()) return static_cast<ustore_status_t>(target_status.code);
    status_t status = executor.pin_bounce();
    if (status.succeeded())
        status = executor.download_async(std::as_writable_bytes(source), reinterpret_cast<std::uintptr_t>(offsets));
    if (status.succeeded()) status = executor.wait();
    if (status.failed()) return static_cast<ustore_status_t>(status.code);
    first = sizeof(converted_type_) < sizeof(source_type_) ? static_cast<std::uint64_t>(source[0]) : 0;
    std::uint64_t const end = source[rows];
    if (end < first) return ustore_status_invalid_argument_k;
    if (end - first > std::numeric_limits<std::uint32_t>::max()) return ustore_status_capacity_exhausted_k;
    convert_offsets(source.data(), target, first);
    auto [carved, carved_status] = carve<converted_type_>(blocks, executor, rows + 1);
    if (carved_status.failed()) return static_cast<ustore_status_t>(carved_status.code);
    status = executor.upload_async(reinterpret_cast<std::uintptr_t>(carved.data()), std::as_bytes(target));
    if (status.succeeded()) status = executor.wait();
    if (status.failed()) return static_cast<ustore_status_t>(status.code);
    converted = carved.data();
    return ustore_status_success_k;
}

/** Spawns an executor on the device @p arena lives on for one export, as the C call names none, and
 *  hands @p visitor the executor and the arena's runs on its device. */
template <typename visitor_type_>
ustore_status_t visit_export_executor(ustore_arena_t arena, visitor_type_&& visitor) noexcept {
    device_arena_t const device = device_arena_of(arena);
    gpu_vendor_t const vendor = device.type == ARROW_DEVICE_CUDA ? gpu_vendor_t::nvidia_k : gpu_vendor_t::amd_k;
    gpu_device_t const* listed = nullptr;
    for (gpu_device_t const& candidate : gpu_devices())
        if (candidate.vendor == vendor && candidate.vendor_ordinal == static_cast<std::uint32_t>(device.device))
            listed = &candidate;
    if (!listed) return ustore_status_device_k;
    gpu_runtime_t& runtime = gpu_runtime();
    std::variant<std::monostate, cuda_executor_t, hip_executor_t> spawned;
    if (status_t const status = spawn_gpu_executor(runtime.drivers, runtime.cuda_kernels, runtime.hip_kernels, *listed,
                                                   gpu_class_of(*listed), nullptr, 0, ustore_gds_compatibility_k,
                                                   spawned);
        status.failed())
        return static_cast<ustore_status_t>(status.code);
    return std::visit(
        [&]<typename executor_type_>(executor_type_& executor) noexcept -> ustore_status_t {
            if constexpr (std::is_same_v<executor_type_, std::monostate>) return ustore_status_device_k;
            else {
                device_blocks_t* const blocks = device_blocks_of(arena, executor);
                if (!blocks) return ustore_status_device_k;
                return visitor(executor, *blocks);
            }
        },
        spawned);
}

/** Converts the device tape @p strings of @p rows strings as @c convert_on_host does, through an
 *  executor spawned on the arena's device for this export alone: the offsets come down into its
 *  bounce, convert there and go back up behind the call's outputs. */
ustore_status_t convert_on_device(ustore_arena_t arena, ustore_strings_view_t const& strings, std::size_t rows,
                                  void const*& converted, ustore_byte_t const*& data) noexcept {
    if (!arena_holds_tape(arena, strings, rows)) return ustore_status_invalid_argument_k;
    return visit_export_executor(
        arena, [&]<typename executor_type_>(executor_type_& executor, device_blocks_t& blocks) noexcept {
            std::uint64_t first = 0;
            ustore_status_t const status = strings.layout == ustore_strings_u64_tape_k
                                               ? convert_through<std::uint32_t>(executor, blocks,
                                                                                strings.data.u64_tape.offsets, rows,
                                                                                converted, first)
                                               : convert_through<std::uint64_t>(executor, blocks,
                                                                                strings.data.u32_tape.offsets, rows,
                                                                                converted, first);
            data += first;
            return status;
        });
}

/** Densely copies strided @p rows past the call's outputs in host @p arena, pointing them at it. */
ustore_status_t densify_on_host(ustore_arena_t arena, strided_rows_t& rows) noexcept {
    auto [dense, carve_status] = arena_allocate<ustore_key_t>(arena, static_cast<std::size_t>(rows.rows));
    if (carve_status.failed()) return static_cast<ustore_status_t>(carve_status.code);
    rows.dense = reinterpret_cast<std::byte*>(dense.data());
    for (std::uint64_t row = 0; row != rows.rows; ++row) densify_row(rows, row);
    return ustore_status_success_k;
}

/** Copies the strided @p rows of a device arena dense behind the call's outputs, through an
 *  executor spawned on the arena's device for this export alone, pointing them at the copy. */
ustore_status_t densify_on_device(ustore_arena_t arena, strided_rows_t& rows) noexcept {
    std::size_t const spanned = static_cast<std::size_t>((rows.rows - 1) * rows.stride + rows.row_bytes);
    if (!arena_holds(arena, rows.source, spanned)) return ustore_status_invalid_argument_k;
    return visit_export_executor(arena, [&]<typename executor_type_>(executor_type_& executor,
                                                                     device_blocks_t& blocks) noexcept {
        if (!executor.kernels()) return ustore_status_device_k;
        auto [dense, carve_status] = carve<ustore_key_t>(blocks, executor, static_cast<std::size_t>(rows.rows));
        if (carve_status.failed()) return static_cast<ustore_status_t>(carve_status.code);
        rows.dense = reinterpret_cast<std::byte*>(dense.data());
        status_t status = launch(executor, executor.kernels()->function("ustore_tape_densify"),
                                 static_cast<unsigned>(divide_round_up<std::size_t>(rows.rows, tape_block_threads_k)),
                                 tape_block_threads_k, 0u, rows);
        if (status.succeeded()) status = executor.wait();
        return static_cast<ustore_status_t>(status.code);
    });
}

void release_exported_schema(ArrowSchema* schema) noexcept { schema->release = nullptr; }

void release_exported_column(ArrowArray* array) noexcept {
    delete static_cast<exported_column_t*>(array->private_data);
    array->release = nullptr;
}

/** Exports @p rows rows of @p buffers out of @p arena, laid out as @p format says, into @p schema
 *  and @p array. */
ustore_status_t export_column(ustore_arena_t arena, std::size_t rows, std::array<void const*, 3> buffers,
                              std::int64_t buffers_count, char const* format, char const* metadata, ArrowSchema* schema,
                              ArrowDeviceArray* array) noexcept {
    auto* const column = new (std::nothrow) exported_column_t {buffers};
    if (!column) return ustore_status_out_of_memory_k;
    *schema = {.format = format,
               .name = "",
               .metadata = metadata,
               .flags = 0,
               .n_children = 0,
               .children = nullptr,
               .dictionary = nullptr,
               .release = &release_exported_schema,
               .private_data = nullptr};
    *array = {};
    // A validity buffer leaves the count of nulls unknown, as no export counts them
    array->array = {.length = static_cast<std::int64_t>(rows),
                    .null_count = buffers[0] ? -1 : 0,
                    .offset = 0,
                    .n_buffers = buffers_count,
                    .n_children = 0,
                    .buffers = column->buffers.data(),
                    .children = nullptr,
                    .dictionary = nullptr,
                    .release = &release_exported_column,
                    .private_data = column};
    // Every call filling the arena waited for its executor, so its outputs are final and no
    // event guards them
    device_arena_t const device = device_arena_of(arena);
    array->device_id = device.device;
    array->device_type = device.type;
    return ustore_status_success_k;
}

#pragma endregion Exporters

#pragma region Gathered Columns

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

/** The buffers an exported gathered column points at: validities, values or offsets, then bytes. */
using column_buffers_t = std::array<void const*, 3>;

/** Packs the @p rows booleans at @p bools into bits past the call's outputs in host @p arena. */
ustore_status_t pack_on_host(ustore_arena_t arena, ustore_byte_t const* bools, std::size_t rows,
                             void const*& packed) noexcept {
    auto [bits, bits_status] = arena_allocate<ustore_octet_t>(arena, divide_round_up(rows, bits_in_byte_k));
    if (bits_status.failed()) return static_cast<ustore_status_t>(bits_status.code);
    for (std::size_t first = 0; first < rows; first += bits_in_byte_k)
        bits[first / bits_in_byte_k] = docs::pack_bools(bools, rows, first);
    packed = bits.data();
    return ustore_status_success_k;
}

/** Copies the text tape of @p rows rows at @p offsets and @p bytes without the null byte
 *  ending every row, behind the call's outputs in the host @p arena, its offsets counted from
 *  its first row. */
template <typename offset_type_>
ustore_status_t drop_on_host(ustore_arena_t arena, offset_type_ const* offsets, ustore_byte_t const* bytes,
                             std::size_t rows, column_buffers_t& buffers) noexcept {
    auto [bare_offsets, offsets_status] = arena_allocate<offset_type_>(arena, rows + 1);
    if (offsets_status.failed()) return static_cast<ustore_status_t>(offsets_status.code);
    auto [bare_bytes, bytes_status] = arena_allocate<ustore_byte_t>(
        arena, static_cast<std::size_t>(offsets[rows] - offsets[0]) - rows);
    if (bytes_status.failed()) return static_cast<ustore_status_t>(bytes_status.code);
    docs::terminated_texts<offset_type_> const texts {bytes, offsets, bare_bytes.data(), bare_offsets.data(), rows};
    for (std::size_t row = 0; row != rows; ++row) docs::drop_terminator(texts, row);
    buffers[1] = bare_offsets.data(), buffers[2] = bare_bytes.data();
    return ustore_status_success_k;
}

/** Packs the @p rows booleans at @p bools on @p executor's device into bits cut from @p blocks. */
template <gpu_executor_concept executor_type_>
ustore_status_t pack_on_device(executor_type_& executor, device_blocks_t& blocks, ustore_byte_t const* bools,
                               std::size_t rows, void const*& packed) noexcept {
    if (!executor.kernels()) return ustore_status_device_k;
    std::size_t const bytes = divide_round_up(rows, bits_in_byte_k);
    auto [bits, bits_status] = carve<ustore_octet_t>(blocks, executor, bytes);
    if (bits_status.failed()) return static_cast<ustore_status_t>(bits_status.code);
    status_t status = launch(executor, executor.kernels()->function("ustore_docs_pack_bools"),
                             static_cast<unsigned>(divide_round_up<std::size_t>(bytes, docs::gather_block_threads_k)),
                             docs::gather_block_threads_k, 0u, bools, std::uint64_t {rows}, bits.data());
    if (status.succeeded()) status = executor.wait();
    if (status.failed()) return static_cast<ustore_status_t>(status.code);
    packed = bits.data();
    return ustore_status_success_k;
}

/** Copies the device text tape of @p rows rows at @p offsets and @p bytes without the null byte
 *  ending every row, on @p executor's device into @p blocks, learning its length from its first and
 *  last offsets through the bounce. */
template <typename offset_type_, gpu_executor_concept executor_type_>
ustore_status_t drop_on_device(executor_type_& executor, device_blocks_t& blocks, offset_type_ const* offsets,
                               ustore_byte_t const* bytes, std::size_t rows, column_buffers_t& buffers) noexcept {
    if (!executor.kernels()) return ustore_status_device_k;
    auto [ends, ends_status] = arena_allocate<offset_type_>(executor.bounce(), 2);
    if (ends_status.failed()) return static_cast<ustore_status_t>(ends_status.code);
    status_t status = executor.pin_bounce();
    if (status.succeeded())
        status = executor.download_async(std::as_writable_bytes(ends.first(1)),
                                         reinterpret_cast<std::uintptr_t>(offsets));
    if (status.succeeded())
        status = executor.download_async(std::as_writable_bytes(ends.last(1)),
                                         reinterpret_cast<std::uintptr_t>(offsets + rows));
    if (status.succeeded()) status = executor.wait();
    if (status.failed()) return static_cast<ustore_status_t>(status.code);
    if (ends[1] < ends[0] || static_cast<std::uint64_t>(ends[1] - ends[0]) < rows)
        return ustore_status_invalid_argument_k;
    auto [bare_offsets, offsets_status] = carve<offset_type_>(blocks, executor, rows + 1);
    if (offsets_status.failed()) return static_cast<ustore_status_t>(offsets_status.code);
    auto [bare_bytes, bytes_status] = carve<ustore_byte_t>(blocks, executor,
                                                           static_cast<std::size_t>(ends[1] - ends[0]) - rows);
    if (bytes_status.failed()) return static_cast<ustore_status_t>(bytes_status.code);
    docs::terminated_texts<offset_type_> const texts {bytes, offsets, bare_bytes.data(), bare_offsets.data(), rows};
    char const* const kernel = sizeof(offset_type_) == sizeof(std::uint64_t) ? "ustore_docs_drop_terminators_u64"
                                                                             : "ustore_docs_drop_terminators_u32";
    status = launch(executor, executor.kernels()->function(kernel),
                    static_cast<unsigned>(divide_round_up<std::size_t>(rows, docs::gather_block_threads_k)),
                    docs::gather_block_threads_k, 0u, texts);
    if (status.succeeded()) status = executor.wait();
    if (status.failed()) return static_cast<ustore_status_t>(status.code);
    buffers[1] = bare_offsets.data(), buffers[2] = bare_bytes.data();
    return ustore_status_success_k;
}

/** Fills @p buffers with the Arrow layout of @p column, of @p rows rows of @p type in @p arena,
 *  after checking the arena holds what it names: in place where Arrow lays it out alike, and
 *  through @p pack and @p drop otherwise. */
template <typename pack_type_, typename drop_type_>
ustore_status_t lay_out_column(ustore_docs_column_view_t const& column, std::size_t rows, ustore_docs_field_type_t type,
                               ustore_arena_t arena, pack_type_&& pack, drop_type_&& drop,
                               column_buffers_t& buffers) noexcept {
    buffers = {column.validities, column.scalars, nullptr};
    if (!arena_holds(arena, column.validities, divide_round_up(rows, bits_in_byte_k)))
        return ustore_status_invalid_argument_k;
    if (std::size_t const width = docs::field_size_bytes(type); width) {
        if (!arena_holds(arena, column.scalars, rows * width)) return ustore_status_invalid_argument_k;
        return type == ustore_docs_field_type_bool_k ? pack(column.scalars, rows, buffers[1]) : ustore_status_success_k;
    }
    ustore_strings_view_t const& strings = column.strings;
    bool const wide = strings.layout == ustore_strings_u64_tape_k;
    if (!wide && strings.layout != ustore_strings_u32_tape_k) return ustore_status_invalid_argument_k;
    if (!arena_holds_tape(arena, strings, rows)) return ustore_status_invalid_argument_k;
    ustore_byte_t const* const bytes = wide ? strings.data.u64_tape.data : strings.data.u32_tape.data;
    if (type == ustore_docs_field_type_string_k)
        return wide ? drop(strings.data.u64_tape.offsets, bytes, rows, buffers)
                    : drop(strings.data.u32_tape.offsets, bytes, rows, buffers);
    buffers[1] = wide ? static_cast<void const*>(strings.data.u64_tape.offsets) : strings.data.u32_tape.offsets;
    buffers[2] = bytes;
    return ustore_status_success_k;
}

#pragma endregion Gathered Columns

} // namespace unum::ustore

using namespace unum::ustore;

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
    *strings = narrow ? utilities::u32_tape(bytes, static_cast<std::uint32_t const*>(offsets) + array->offset)
                      : utilities::u64_tape(bytes, static_cast<std::uint64_t const*>(offsets) + array->offset);
    *count = static_cast<ustore_size_t>(array->length);
    return ustore_status_success_k;
}

ustore_status_t ustore_keys_view_to_arrow(ustore_keys_view_t keys, ustore_size_t count, ustore_arena_t arena,
                                          ArrowSchema* schema, ArrowDeviceArray* array) {
    if (!arena || !schema || !array || (count && !keys.data)) return ustore_status_invalid_argument_k;
    void const* data = count ? static_cast<void const*>(keys.data) : no_rows_k;
    if (count > 1 && keys.stride != sizeof(ustore_key_t)) {
        // Strided keys, like the triples of edges, are carved dense behind the call's outputs under the claim
        if (!take(arena->claim)) return ustore_status_contended_k;
        std::size_t const rows = static_cast<std::size_t>(count);
        strided_rows_t strided_keys {reinterpret_cast<std::byte const*>(keys.data), keys.stride, sizeof(ustore_key_t),
                                     rows, nullptr};
        ustore_status_t const status = device_arena_of(arena).blocks ? densify_on_device(arena, strided_keys)
                                                                     : densify_on_host(arena, strided_keys);
        release(arena->claim);
        if (status != ustore_status_success_k) return status;
        data = strided_keys.dense;
    }
    else if (!arena_holds(arena, keys.data, static_cast<std::size_t>(count) * sizeof(ustore_key_t)))
        return ustore_status_invalid_argument_k;
    return export_column(arena, count, {nullptr, data, nullptr}, 2, "w:16", uuid_metadata_k.data(), schema, array);
}

ustore_status_t ustore_strings_view_to_arrow(ustore_strings_view_t strings, ustore_size_t count,
                                             ustore_strings_layout_t layout, ustore_arena_t arena, ArrowSchema* schema,
                                             ArrowDeviceArray* array) {
    if (!arena || !schema || !array) return ustore_status_invalid_argument_k;
    bool const wide = strings.layout == ustore_strings_u64_tape_k;
    if (!wide && strings.layout != ustore_strings_u32_tape_k) return ustore_status_invalid_argument_k;
    if (layout != ustore_strings_u32_tape_k && layout != ustore_strings_u64_tape_k)
        return ustore_status_invalid_argument_k;
    void const* offsets = wide ? static_cast<void const*>(strings.data.u64_tape.offsets)
                               : strings.data.u32_tape.offsets;
    ustore_byte_t const* data = wide ? strings.data.u64_tape.data : strings.data.u32_tape.data;
    if (count && !offsets) return ustore_status_invalid_argument_k;
    char const* const format = layout == ustore_strings_u64_tape_k ? "Z" : "z";
    std::size_t const rows = static_cast<std::size_t>(count);
    if (!count || layout == strings.layout) {
        if (count && !arena_holds_tape(arena, strings, rows)) return ustore_status_invalid_argument_k;
        void const* const exported_offsets = count ? offsets : no_rows_k;
        void const* const exported_data = data ? static_cast<void const*>(data) : no_rows_k;
        return export_column(arena, count, {nullptr, exported_offsets, exported_data}, 3, format, nullptr, schema,
                             array);
    }

    // A tape of the other width is converted behind the call's outputs under the claim, narrowed from its first byte
    if (!take(arena->claim)) return ustore_status_contended_k;
    void const* converted = nullptr;
    ustore_status_t const status = device_arena_of(arena).blocks
                                       ? convert_on_device(arena, strings, rows, converted, data)
                                       : convert_on_host(arena, strings, rows, converted, data);
    release(arena->claim);
    if (status != ustore_status_success_k) return status;
    void const* const exported_data = data ? static_cast<void const*>(data) : no_rows_k;
    return export_column(arena, count, {nullptr, converted, exported_data}, 3, format, nullptr, schema, array);
}

ustore_status_t ustore_docs_column_view_to_arrow(ustore_docs_column_view_t const* column, ustore_size_t count,
                                                 ustore_docs_field_type_t type, ustore_arena_t arena,
                                                 ArrowSchema* schema, ArrowDeviceArray* array) {
    if (!column || !arena || !schema || !array) return ustore_status_invalid_argument_k;
    if (!docs_column_format(type, ustore_strings_u64_tape_k)) return ustore_status_invalid_argument_k;
    std::size_t const rows = static_cast<std::size_t>(count);
    bool const device = device_arena_of(arena).blocks != nullptr;
    ustore_docs_column_view_t view = device ? ustore_docs_column_view_t {} : *column;

    // Every column takes the claim, as a boolean or text one carves its conversion past the outputs
    if (!take(arena->claim)) return ustore_status_contended_k;
    column_buffers_t buffers {};
    ustore_status_t status = ustore_status_success_k;
    if (!device && rows)
        status = lay_out_column(
            view, rows, type, arena,
            [&](ustore_byte_t const* bools, std::size_t bools_count, void const*& packed) noexcept {
                return pack_on_host(arena, bools, bools_count, packed);
            },
            [&]<typename offset_type_>(offset_type_ const* offsets, ustore_byte_t const* bytes, std::size_t texts,
                                       column_buffers_t& laid_out) noexcept {
                return drop_on_host(arena, offsets, bytes, texts, laid_out);
            },
            buffers);
    if (device)
        status = visit_export_executor(
            arena, [&]<typename executor_type_>(executor_type_& executor, device_blocks_t& blocks) noexcept {
                // A view the gather left on the device comes down through the bounce first
                if (executor.device_of(column) < 0) view = *column;
                else {
                    auto [copy, copy_status] = arena_allocate<ustore_docs_column_view_t>(executor.bounce(), 1);
                    status_t loaded = copy_status;
                    if (loaded.succeeded()) loaded = executor.pin_bounce();
                    if (loaded.succeeded())
                        loaded = executor.download_async(std::as_writable_bytes(copy),
                                                         reinterpret_cast<std::uintptr_t>(column));
                    if (loaded.succeeded()) loaded = executor.wait();
                    if (loaded.failed()) return static_cast<ustore_status_t>(loaded.code);
                    view = copy[0];
                }
                if (!rows) return ustore_status_success_k;
                return lay_out_column(
                    view, rows, type, arena,
                    [&](ustore_byte_t const* bools, std::size_t bools_count, void const*& packed) noexcept {
                        return pack_on_device(executor, blocks, bools, bools_count, packed);
                    },
                    [&]<typename offset_type_>(offset_type_ const* offsets, ustore_byte_t const* bytes,
                                               std::size_t texts, column_buffers_t& laid_out) noexcept {
                        return drop_on_device(executor, blocks, offsets, bytes, texts, laid_out);
                    },
                    buffers);
            });
    release(arena->claim);
    if (status != ustore_status_success_k) return status;

    bool const texts = !docs::field_size_bytes(type);
    if (!rows) buffers = {nullptr, no_rows_k, nullptr};
    if (texts && !buffers[2]) buffers[2] = no_rows_k;
    char const* const format = docs_column_format(type, view.strings.layout);
    char const* const metadata = type == ustore_docs_field_type_uuid_k ? uuid_metadata_k.data() : nullptr;
    return export_column(arena, rows, buffers, texts ? 3 : 2, format, metadata, schema, array);
}
