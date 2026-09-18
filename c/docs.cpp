/**
 *  @file c/docs.cpp
 *  @author Ash Vardanian
 *  @date December 26, 2022
 *  @brief Documents modality on yyjson, running on top of any engine that implements <ustore/db.h>.
 *
 *  Documents are stored as JSON text.
 *  Whole-document reads and upserts pass straight through to the engine, while field access, conversions
 *  and modifications parse every document with yyjson, allocating from the arena of the request.
 */

#include <cmath>   // `std::abs`, `std::isfinite`, `std::ldexp`
#include <cstddef> // `std::size_t`, `std::max_align_t`
#include <cstdint> // `std::uint8_t`, `std::uint16_t`, `std::uint32_t`
#include <cstring> // `std::memcpy`, `std::memset`, `std::strlen`

#include <algorithm>    // `std::lower_bound`
#include <bit>          // `std::bit_cast`
#include <charconv>     // `std::from_chars`
#include <concepts>     // `std::invocable`
#include <limits>       // `std::numeric_limits`
#include <span>         // `std::span`
#include <string_view>  // `std::string_view`
#include <system_error> // `std::errc`
#include <type_traits>  // `std::is_same_v`
#include <utility>      // `std::exchange`, `std::in_range`

#include <fmt/format.h> // `fmt::format_to_n`
#include <yyjson.h>     // `yyjson_read_opts`, `yyjson_mut_doc`

#include <ustore/docs.h>          // `ustore_docs_write_t`, `ustore_docs_read_t`
#include <ustore/ranges_args.hpp> // `places_arg_t`, `contents_arg_t`

#include "helpers/algorithm.hpp"     // `offset_in_sorted`
#include "helpers/linked_array.hpp"  // `growing_tape_t`, `uninitialized_array`
#include "helpers/linked_memory.hpp" // `linked_memory_lock_t`

namespace unum::ustore {

#pragma region Number Formatting

/** Longest text a printed number takes, including the null terminator. */
constexpr std::size_t printed_number_length_limit_k = 32;

/** Longest path @c ustore_docs_gist exports, including the null terminator. */
constexpr std::size_t field_path_length_limit_k = 512;

/** Bytes of a UUID. */
constexpr std::size_t uuid_bytes_k = 16;

using printed_number_buffer_t = char[printed_number_length_limit_k];
using field_path_buffer_t = char[field_path_length_limit_k];

constexpr std::string_view true_k = "true";
constexpr std::string_view false_k = "false";

/** Parses the whole of @p text as a number or a boolean, answering whether every character was consumed. */
template <typename scalar_>
static bool parse_entire_number(std::string_view text, scalar_& scalar) noexcept {
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
static std::string_view print_number(char* begin, char* end, scalar_ scalar) noexcept {
    std::size_t const capacity = static_cast<std::size_t>(end - begin);
    auto const [written_end, written_size] = fmt::format_to_n(begin, capacity - 1, "{}", scalar);
    if (written_size >= capacity) return {};
    *written_end = '\0';
    return {begin, written_size};
}

#pragma endregion Number Formatting

#pragma region JSON Handles

/** Document parsed from the request arena, which owns all of its memory. */
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

static void* arena_allocate(void* context, std::size_t size) noexcept {
    ustore_error_t error {};
    linked_memory_lock_t& arena = *static_cast<linked_memory_lock_t*>(context);
    return arena.alloc<byte_t>(size, &error, alignof(std::max_align_t)).data();
}

static void* arena_reallocate(void* context, void* pointer, std::size_t old_size, std::size_t size) noexcept {
    if (size <= old_size) return pointer;
    ustore_error_t error {};
    linked_memory_lock_t& arena = *static_cast<linked_memory_lock_t*>(context);
    std::span<byte_t> const old_bytes {static_cast<byte_t*>(pointer), old_size};
    return arena.grow<byte_t>(old_bytes, size - old_size, &error, alignof(std::max_align_t)).data();
}

/** Arena memory is released with the arena, never one allocation at a time. */
static void arena_free(void*, void*) noexcept {}

static yyjson_alc arena_allocator(linked_memory_lock_t& arena) noexcept {
    return {arena_allocate, arena_reallocate, arena_free, &arena};
}

/** Finds @p field in @p json, as a JSON Pointer when it starts with a slash and as a top-level key otherwise. */
static yyjson_val* json_lookup(yyjson_val* json, ustore_str_view_t field) noexcept {
    if (!field) return json;
    return field[0] == '/' ? yyjson_ptr_get(json, field) : yyjson_obj_get(json, field);
}

/** Characters @p key takes as an RFC 6901 reference token, where a tilde and a slash each take two. */
static std::size_t escaped_length(std::string_view key) noexcept {
    std::size_t length = key.size();
    for (char const character : key) length += character == '~' || character == '/';
    return length;
}

/** Appends @p key as an RFC 6901 reference token at @p output, answering where the token ends. */
static char* escape_into(std::string_view key, char* output) noexcept {
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

/** Spells @p field as a JSON Pointer, escaping a top-level key into the arena. */
static std::string_view field_pointer(ustore_str_view_t field, linked_memory_lock_t& arena,
                                      ustore_error_t* error) noexcept {
    std::string_view const key {field};
    if (key.starts_with('/')) return key;

    auto pointer = arena.alloc<char>(escaped_length(key) + 1, error);
    if (error->status) return {};

    pointer[0] = '/';
    char const* const end = escape_into(key, pointer.data() + 1);
    return {pointer.data(), static_cast<std::size_t>(end - pointer.data())};
}

/** Parses a JSON document, leaving the result empty for an empty or a missing value. */
static json_t json_parse(value_view_t bytes, linked_memory_lock_t& arena, ustore_error_t* error) noexcept {
    json_t document;
    if (bytes.empty()) return document;

    yyjson_alc const allocator = arena_allocator(arena);
    yyjson_read_flag const flags = YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_INF_AND_NAN;
    // yyjson writes into its input only with `YYJSON_READ_INSITU`, which is never set.
    char* const text = const_cast<char*>(bytes.c_str());
    document.handle = yyjson_read_opts(text, bytes.size(), flags, &allocator, nullptr);
    if (!document.handle) *error = {ustore_error_malformed_k, "Failed to parse the document"};
    return document;
}

/** Makes @p document mutable, copying what was parsed or starting an empty document. */
static yyjson_mut_doc* json_mutable(json_t& document, linked_memory_lock_t& arena, ustore_error_t* error) noexcept {
    if (document.mutable_handle) return document.mutable_handle;

    yyjson_alc const allocator = arena_allocator(arena);
    document.mutable_handle = document.handle //
                                  ? yyjson_doc_mut_copy(document.handle, &allocator)
                                  : yyjson_mut_doc_new(&allocator);
    if (!document.mutable_handle) *error = {ustore_error_out_of_memory_k, "Failed to allocate a mutable document"};
    return document.mutable_handle;
}

/** Appends @p value to @p output as null-terminated JSON text, or appends a missing value when there is none. */
static void json_dump(json_value_t value, linked_memory_lock_t& arena, growing_tape_t& output,
                      ustore_error_t* error) noexcept {
    if (!value) {
        output.push_back(value_view_t {}, error);
        return;
    }

    yyjson_alc const allocator = arena_allocator(arena);
    yyjson_write_flag const flags = YYJSON_WRITE_ALLOW_INF_AND_NAN;
    std::size_t length = 0;
    char const* const text = value.mutable_handle
                                 ? yyjson_mut_val_write_opts(value.mutable_handle, flags, &allocator, &length, nullptr)
                                 : yyjson_val_write_opts(value.handle, flags, &allocator, &length, nullptr);
    if (!text) {
        *error = {ustore_error_out_of_memory_k, "Failed to serialize the document"};
        return;
    }
    output.push_back(value_view_t {text, length}, error);
    if (error->status) return;
    output.add_terminator(byte_t {0}, error);
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
static bool is_valid(conversion_t conversion) noexcept {
    return conversion == conversion_t::exact_k || conversion == conversion_t::converted_k;
}

/** Records @p conversion of one cell, setting the validity bit last since the other bitmaps may alias it. */
static void record_conversion(conversion_t conversion, ustore_octet_t mask, ustore_octet_t& valid,
                              ustore_octet_t& converted, ustore_octet_t& collides) noexcept {
    ustore_octet_t const cleared = static_cast<ustore_octet_t>(~mask);
    converted = static_cast<ustore_octet_t>((converted & cleared) |
                                            (conversion == conversion_t::converted_k ? mask : 0));
    collides = static_cast<ustore_octet_t>((collides & cleared) | (conversion == conversion_t::collides_k ? mask : 0));
    valid = static_cast<ustore_octet_t>((valid & cleared) | (is_valid(conversion) ? mask : 0));
}

/** Whether @p real, with its fraction dropped, fits the range of @p scalar_, where NaN fits only a float. */
template <typename scalar_>
static bool real_fits(double real) noexcept {
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
static bool integer_fits(integer_ integer) noexcept {
    if constexpr (std::is_same_v<scalar_, bool> || std::is_floating_point_v<scalar_>) return true;
    else return std::in_range<scalar_>(integer);
}

/** Converts @p value into @p scalar, where a value outside the range of the scalar collides. */
template <typename scalar_>
static conversion_t json_to_scalar(yyjson_val* value, scalar_& scalar) noexcept {
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
static conversion_t json_to_string(yyjson_val* value, printed_number_buffer_t& print_buffer,
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
static yyjson_mut_val* mutable_scalar(yyjson_mut_doc* document, value_view_t bytes) noexcept {
    if (bytes.size() < sizeof(scalar_)) return nullptr;
    scalar_ scalar {};
    std::memcpy(&scalar, bytes.data(), sizeof(scalar_));

    if constexpr (std::is_same_v<scalar_, bool>) return yyjson_mut_bool(document, scalar);
    else if constexpr (std::is_floating_point_v<scalar_>) return yyjson_mut_real(document, scalar);
    else if constexpr (std::is_signed_v<scalar_>) return yyjson_mut_sint(document, scalar);
    else return yyjson_mut_uint(document, scalar);
}

/** Parses @p bytes of @p type into a document, wrapping a scalar into a document of its own. */
static json_t any_parse(value_view_t bytes, ustore_doc_field_type_t type, linked_memory_lock_t& arena,
                        ustore_error_t* error) noexcept {
    if (type == ustore_doc_field_json_k) return json_parse(bytes, arena, error);

    json_t document;
    yyjson_alc const allocator = arena_allocator(arena);
    document.mutable_handle = yyjson_mut_doc_new(&allocator);
    if (!document.mutable_handle) {
        *error = {ustore_error_out_of_memory_k, "Failed to allocate a mutable document"};
        return document;
    }

    yyjson_mut_doc* const handle = document.mutable_handle;
    yyjson_mut_val* root = nullptr;
    switch (type) {
    case ustore_doc_field_null_k: root = yyjson_mut_null(handle); break;
    case ustore_doc_field_bool_k: root = mutable_scalar<bool>(handle, bytes); break;
    case ustore_doc_field_f64_k: root = mutable_scalar<double>(handle, bytes); break;
    case ustore_doc_field_f32_k: root = mutable_scalar<float>(handle, bytes); break;
    case ustore_doc_field_i64_k: root = mutable_scalar<std::int64_t>(handle, bytes); break;
    case ustore_doc_field_i32_k: root = mutable_scalar<std::int32_t>(handle, bytes); break;
    case ustore_doc_field_i16_k: root = mutable_scalar<std::int16_t>(handle, bytes); break;
    case ustore_doc_field_i8_k: root = mutable_scalar<std::int8_t>(handle, bytes); break;
    case ustore_doc_field_u64_k: root = mutable_scalar<std::uint64_t>(handle, bytes); break;
    case ustore_doc_field_u32_k: root = mutable_scalar<std::uint32_t>(handle, bytes); break;
    case ustore_doc_field_u16_k: root = mutable_scalar<std::uint16_t>(handle, bytes); break;
    case ustore_doc_field_u8_k: root = mutable_scalar<std::uint8_t>(handle, bytes); break;
    case ustore_doc_field_str_k: root = yyjson_mut_strncpy(handle, bytes.c_str(), bytes.size()); break;
    default: break;
    }

    if (!root) *error = {ustore_error_unsupported_k, "Input type is not supported"};
    yyjson_mut_doc_set_root(handle, root);
    return document;
}

#pragma endregion Conversions

#pragma region Modifications

/** Modifications a request applies, extending @c ustore_doc_modification_t with reads and removals. */
enum class modification_t {
    nothing_k = -1,
    remove_k = -2,
    upsert_k = ustore_doc_modify_upsert_k,
    update_k = ustore_doc_modify_update_k,
    insert_k = ustore_doc_modify_insert_k,
    patch_k = ustore_doc_modify_patch_k,
    merge_k = ustore_doc_modify_merge_k,
};

/** Adds @p value at the non-empty @p path of @p root, replacing an object member and inserting into an array. */
static bool patch_add(yyjson_mut_doc* document, yyjson_mut_val* root, std::string_view path,
                      yyjson_mut_val* value) noexcept {
    std::size_t const parent_length = path.rfind('/');
    if (parent_length == std::string_view::npos) return false;
    yyjson_mut_val const* const parent = parent_length
                                             ? yyjson_mut_ptr_getx(root, path.data(), parent_length, nullptr, nullptr)
                                             : root;
    if (yyjson_mut_is_obj(parent))
        return yyjson_mut_ptr_setx(root, path.data(), path.size(), value, document, false, nullptr, nullptr);
    return yyjson_mut_ptr_addx(root, path.data(), path.size(), value, document, false, nullptr, nullptr);
}

/** Applies the RFC 6902 @p patch to @p root in place, answering the patched root or @c nullptr on failure. */
static yyjson_mut_val* patch_value(yyjson_mut_doc* document, yyjson_mut_val* root, yyjson_mut_val* patch) noexcept {
    if (!root || !yyjson_mut_is_arr(patch)) return nullptr;

    yyjson_mut_arr_iter iterator;
    yyjson_mut_arr_iter_init(patch, &iterator);
    while (yyjson_mut_val* const operation = yyjson_mut_arr_iter_next(&iterator)) {
        yyjson_mut_val* const name_value = yyjson_mut_obj_get(operation, "op");
        yyjson_mut_val* const path_value = yyjson_mut_obj_get(operation, "path");
        if (!yyjson_mut_is_str(name_value) || !yyjson_mut_is_str(path_value)) return nullptr;
        std::string_view const name {yyjson_mut_get_str(name_value), yyjson_mut_get_len(name_value)};
        std::string_view const path {yyjson_mut_get_str(path_value), yyjson_mut_get_len(path_value)};
        yyjson_mut_val* const value = yyjson_mut_obj_get(operation, "value");
        yyjson_mut_val* const from_value = yyjson_mut_obj_get(operation, "from");
        std::string_view const from = yyjson_mut_is_str(from_value) ? std::string_view {yyjson_mut_get_str(from_value),
                                                                                        yyjson_mut_get_len(from_value)}
                                                                    : std::string_view {};

        if (name == "test") {
            yyjson_mut_val const* const target =
                path.empty() ? root : yyjson_mut_ptr_getx(root, path.data(), path.size(), nullptr, nullptr);
            if (!value || !target || !yyjson_mut_equals(value, target)) return nullptr;
            continue;
        }
        if (name == "remove") {
            if (path.empty() || !yyjson_mut_ptr_removex(root, path.data(), path.size(), nullptr, nullptr))
                return nullptr;
            continue;
        }

        // Every other operation places one value at `path`
        yyjson_mut_val* placed = nullptr;
        if (name == "add" || name == "replace") placed = value ? yyjson_mut_val_mut_copy(document, value) : nullptr;
        else if (name == "copy" && from_value) {
            yyjson_mut_val const* const source =
                from.empty() ? root : yyjson_mut_ptr_getx(root, from.data(), from.size(), nullptr, nullptr);
            placed = source ? yyjson_mut_val_mut_copy(document, source) : nullptr;
        }
        else if (name == "move" && from_value) {
            if (from == path) continue;
            placed = from.empty() ? nullptr : yyjson_mut_ptr_removex(root, from.data(), from.size(), nullptr, nullptr);
        }
        if (!placed) return nullptr;

        if (path.empty()) root = placed;
        else if (name == "replace") {
            if (!yyjson_mut_ptr_replacex(root, path.data(), path.size(), placed, nullptr, nullptr)) return nullptr;
        }
        else if (!patch_add(document, root, path, placed)) return nullptr;
    }
    return root;
}

/** Applies @p modifier to @p field of @p original, or to the whole document when @p field is @c nullptr. */
static void modify(json_t& original, yyjson_mut_val* modifier, ustore_str_view_t field, modification_t modification,
                   linked_memory_lock_t& arena, ustore_error_t* error) noexcept {

    yyjson_mut_doc* const document = json_mutable(original, arena, error);
    if (error->status) return;

    if (!field) {
        yyjson_mut_val* const root = yyjson_mut_doc_get_root(document);
        yyjson_mut_val* modified = nullptr;
        switch (modification) {
        case modification_t::merge_k: modified = yyjson_mut_merge_patch(document, root, modifier); break;
        case modification_t::patch_k: modified = patch_value(document, root, modifier); break;
        case modification_t::upsert_k:
        case modification_t::update_k:
        case modification_t::insert_k: modified = yyjson_mut_val_mut_copy(document, modifier); break;
        default: break;
        }
        if (!modified && modification == modification_t::patch_k) {
            *error = {ustore_error_invalid_argument_k, "The patch does not apply to the document"};
            return;
        }
        if (!modified) {
            *error = {ustore_error_out_of_memory_k, "Failed to modify the document"};
            return;
        }
        yyjson_mut_doc_set_root(document, modified);
        return;
    }

    std::string_view const pointer = field_pointer(field, arena, error);
    if (error->status) return;

    yyjson_mut_val* replacement = nullptr;
    switch (modification) {
    case modification_t::remove_k: break;
    case modification_t::merge_k: {
        yyjson_mut_val* const existing = yyjson_mut_doc_ptr_getn(document, pointer.data(), pointer.size());
        replacement = yyjson_mut_merge_patch(document, existing, modifier);
        break;
    }
    case modification_t::patch_k: {
        yyjson_mut_val* const existing = yyjson_mut_doc_ptr_getn(document, pointer.data(), pointer.size());
        replacement = patch_value(document, yyjson_mut_val_mut_copy(document, existing), modifier);
        break;
    }
    default: replacement = yyjson_mut_val_mut_copy(document, modifier); break;
    }
    if (!replacement && modification != modification_t::remove_k) {
        *error = {ustore_error_invalid_argument_k, "The modifier does not apply to the field"};
        return;
    }

    bool succeeded = false;
    switch (modification) {
    case modification_t::insert_k:
        succeeded = yyjson_mut_doc_ptr_addn(document, pointer.data(), pointer.size(), replacement);
        break;
    case modification_t::update_k:
        succeeded = yyjson_mut_doc_ptr_replacen(document, pointer.data(), pointer.size(), replacement) != nullptr;
        break;
    case modification_t::remove_k:
        succeeded = yyjson_mut_doc_ptr_removen(document, pointer.data(), pointer.size()) != nullptr;
        break;
    default: succeeded = yyjson_mut_doc_ptr_setn(document, pointer.data(), pointer.size(), replacement); break;
    }
    if (!succeeded) {
        *error = {ustore_error_not_found_k, "The field path does not resolve inside the document"};
        return;
    }
}

#pragma endregion Modifications

#pragma region Reading Documents

/** Receives the index of a task, its field, and the binary document it addresses. */
template <typename callback_>
concept document_callback = std::invocable<callback_&, std::size_t, ustore_str_view_t, value_view_t>;

/** Binary documents found by one read, indexed by task. */
struct found_documents_t {

    /** Offsets of the documents inside @c contents. */
    ustore_length_t* offsets = nullptr;

    /** Lengths of the documents, with @c ustore_length_missing_k for missing ones. */
    ustore_length_t* lengths = nullptr;

    /** Joined contents of the documents. */
    ustore_byte_t* contents = nullptr;

    value_view_t operator[](std::size_t index) const noexcept {
        if (lengths[index] == ustore_length_missing_k) return value_view_t {};
        return value_view_t {contents + offsets[index], lengths[index]};
    }
};

static found_documents_t read_documents(ustore_database_t database, ustore_transaction_t transaction,
                                        ustore_snapshot_t snapshot, places_arg_t const& places,
                                        ustore_options_t options, linked_memory_lock_t& arena,
                                        ustore_error_t* error) noexcept {
    found_documents_t found;
    ustore_read_t read {};
    read.db = database;
    read.error = error;
    read.transaction = transaction;
    read.snapshot = snapshot;
    read.arena = arena;
    read.options = options;
    read.tasks_count = places.count;
    read.collections = places.collections_begin.get();
    read.collections_stride = places.collections_begin.stride();
    read.keys = places.keys_begin.get();
    read.keys_stride = places.keys_begin.stride();
    read.offsets = &found.offsets;
    read.lengths = &found.lengths;
    read.values = &found.contents;
    ustore_read(&read);
    return found;
}

/**
 *  @brief Calls @p callback on the document of every task, in order.
 *
 *  Fails a field modification of a missing document, an update or a patch of a whole document that is missing,
 *  and an insertion of a whole document that already exists.
 */
template <document_callback callback_>
static void read_each_document(ustore_database_t database, ustore_transaction_t transaction, ustore_snapshot_t snapshot,
                               places_arg_t const& places, ustore_options_t options, modification_t modification,
                               linked_memory_lock_t& arena, ustore_error_t* error, callback_&& callback) noexcept {

    bool const has_fields = places.fields_begin && (!places.fields_begin.repeats() || *places.fields_begin);
    found_documents_t const found = read_documents(database, transaction, snapshot, places, options, arena, error);
    if (error->status) return;

    for (std::size_t index = 0; index != places.size(); ++index) {
        value_view_t const document = found[index];
        if (!document && has_fields && modification != modification_t::nothing_k) {
            *error = {ustore_error_not_found_k, "Modified field belongs to a missing document"};
            return;
        }
        bool const replaces_existing = modification == modification_t::update_k ||
                                       modification == modification_t::patch_k;
        if (!document && !has_fields && replaces_existing) {
            *error = {ustore_error_not_found_k, "Updated or patched document is missing"};
            return;
        }
        if (document && !has_fields && modification == modification_t::insert_k) {
            *error = {ustore_error_already_exists_k, "Inserted document already exists"};
            return;
        }
        callback(index, places.fields_begin ? places.fields_begin[index] : nullptr, document);
        if (error->status) return;
    }
}

/** Calls @p callback on the document of every task, reading every distinct document once. */
template <document_callback callback_>
static void read_distinct_documents(ustore_database_t database, ustore_transaction_t transaction,
                                    ustore_snapshot_t snapshot, places_arg_t const& places, ustore_options_t options,
                                    linked_memory_lock_t& arena, ustore_error_t* error, callback_&& callback) noexcept {

    if (all_ascending(places.keys_begin, places.count))
        return read_each_document(database, transaction, snapshot, places, options, modification_t::nothing_k, arena,
                                  error, callback);

    auto distinct = arena.alloc<collection_key_t>(places.count, error);
    if (error->status) return;
    transform_n(places, places.count, distinct.begin(), [](place_t const& place) { return place.collection_key(); });
    std::size_t const distinct_count = sort_and_deduplicate(distinct.begin(), distinct.end());
    if (distinct_count == places.count)
        return read_each_document(database, transaction, snapshot, places, options, modification_t::nothing_k, arena,
                                  error, callback);

    std::span<collection_key_t> const sorted = distinct.first(distinct_count);
    auto const sorted_strided = strided(sorted).immutable();
    places_arg_t distinct_places;
    distinct_places.collections_begin = sorted_strided.members(&collection_key_t::collection).begin();
    distinct_places.keys_begin = sorted_strided.members(&collection_key_t::key).begin();
    distinct_places.count = static_cast<ustore_size_t>(distinct_count);

    found_documents_t const found = read_documents(database, transaction, snapshot, distinct_places, options, arena,
                                                   error);
    if (error->status) return;

    for (std::size_t index = 0; index != places.size(); ++index) {
        place_t const place = places[index];
        callback(index, place.field, found[offset_in_sorted(sorted, place.collection_key())]);
        if (error->status) return;
    }
}

#pragma endregion Reading Documents

#pragma region Writing

/** Parses, modifies and writes back the documents at @p places. */
static void read_modify_write(ustore_database_t database, ustore_transaction_t transaction, places_arg_t const& places,
                              contents_arg_t const& contents, ustore_options_t options, modification_t modification,
                              ustore_doc_field_type_t type, linked_memory_lock_t& arena,
                              ustore_error_t* error) noexcept {

    growing_tape_t modified {arena};
    modified.reserve(places.size(), error);
    if (error->status) return;

    auto modify_one = [&](std::size_t index, ustore_str_view_t field, value_view_t binary_document) {
        json_t document = json_parse(binary_document, arena, error);
        if (error->status) return;

        value_view_t const content = contents[index];
        if (!content && !field) {
            modified.push_back(value_view_t {}, error);
            return;
        }

        if (!content) modify(document, nullptr, field, modification_t::remove_k, arena, error);
        else {
            json_t modifier = any_parse(content, type, arena, error);
            if (error->status) return;
            yyjson_mut_doc* const modifier_document = json_mutable(modifier, arena, error);
            if (error->status) return;
            modify(document, yyjson_mut_doc_get_root(modifier_document), field, modification, arena, error);
        }
        if (error->status) return;
        json_dump({nullptr, yyjson_mut_doc_get_root(document.mutable_handle)}, arena, modified, error);
    };

    ustore_options_t const read_options = transaction //
                                              ? ustore_options_t(options & ~ustore_option_transaction_dont_watch_k)
                                              : options;
    read_each_document(database, transaction, {}, places, read_options, modification, arena, error, modify_one);
    if (error->status) return;

    ustore_byte_t* const modified_contents = reinterpret_cast<ustore_byte_t*>(modified.contents().begin().get());
    ustore_write_t write {};
    write.db = database;
    write.error = error;
    write.transaction = transaction;
    write.arena = arena;
    write.options = options;
    write.tasks_count = places.count;
    write.collections = places.collections_begin.get();
    write.collections_stride = places.collections_begin.stride();
    write.keys = places.keys_begin.get();
    write.keys_stride = places.keys_begin.stride();
    write.offsets = modified.offsets().begin().get();
    write.offsets_stride = modified.offsets().stride();
    write.lengths = modified.lengths().begin().get();
    write.lengths_stride = modified.lengths().stride();
    write.values = &modified_contents;
    ustore_write(&write);
}

/** Reads the integer key of every document from its @p id_field into @p keys. */
static void extract_keys(contents_arg_t const& contents, ustore_str_view_t id_field, linked_memory_lock_t& arena,
                         std::span<ustore_key_t> keys, ustore_error_t* error) noexcept {
    for (std::size_t index = 0; index != contents.size(); ++index) {
        json_t const document = json_parse(contents[index], arena, error);
        if (error->status) return;
        yyjson_val* const identifier = json_lookup(yyjson_doc_get_root(document.handle), id_field);
        if (!yyjson_is_int(identifier)) {
            *error = {ustore_error_malformed_k, "The id field does not hold an integer key"};
            return;
        }
        keys[index] = yyjson_get_sint(identifier);
    }
}

#pragma endregion Writing

#pragma region Gist

/** Collects the unique paths to the leaves under @p node, extending the null-terminated @p path. */
static void gist_paths(yyjson_val* node, field_path_buffer_t& path, uninitialized_array<std::string_view>& sorted_paths,
                       growing_tape_t& exported_paths, ustore_error_t* error) noexcept {

    std::size_t const path_length = std::strlen(path);

    if (yyjson_is_obj(node)) {
        yyjson_obj_iter iterator;
        yyjson_obj_iter_init(node, &iterator);
        yyjson_val* key = nullptr;
        while ((key = yyjson_obj_iter_next(&iterator)) && !error->status) {
            std::string_view const name {yyjson_get_str(key), yyjson_get_len(key)};
            if (path_length + 1 + escaped_length(name) >= field_path_length_limit_k) {
                *error = {ustore_error_capacity_exhausted_k, "Path is too long"};
                return;
            }
            path[path_length] = '/';
            *escape_into(name, path + path_length + 1) = '\0';
            gist_paths(yyjson_obj_iter_get_val(key), path, sorted_paths, exported_paths, error);
        }
        path[path_length] = '\0';
        return;
    }

    if (yyjson_is_arr(node)) {
        std::size_t element_index = 0;
        yyjson_arr_iter iterator;
        yyjson_arr_iter_init(node, &iterator);
        yyjson_val* element = nullptr;
        while ((element = yyjson_arr_iter_next(&iterator)) && !error->status) {
            path[path_length] = '/';
            std::string_view const printed = print_number(path + path_length + 1, path + field_path_length_limit_k,
                                                          element_index++);
            if (printed.empty()) {
                *error = {ustore_error_capacity_exhausted_k, "Path is too long"};
                return;
            }
            gist_paths(element, path, sorted_paths, exported_paths, error);
        }
        path[path_length] = '\0';
        return;
    }

    std::string_view const leaf {path, path_length};
    std::size_t const position = std::lower_bound(sorted_paths.begin(), sorted_paths.end(), leaf) -
                                 sorted_paths.begin();
    if (position != sorted_paths.size() && sorted_paths[position] == leaf) return;

    value_view_t const exported = exported_paths.push_back(leaf, error);
    if (error->status) return;
    exported_paths.add_terminator(byte_t {0}, error);
    if (error->status) return;

    std::string_view const stored {exported.c_str(), exported.size()};
    sorted_paths.insert(position, &stored, &stored + 1, error);
}

#pragma endregion Gist

#pragma region Gather

/** Bytes a column of @p type takes per document: the scalar, or an offset and a length for strings. */
static std::size_t field_size_bytes(ustore_doc_field_type_t type) noexcept {
    switch (type) {
    case ustore_doc_field_bool_k: return sizeof(bool);
    case ustore_doc_field_uuid_k: return uuid_bytes_k;
    case ustore_doc_field_f64_k: return sizeof(double);
    case ustore_doc_field_f32_k: return sizeof(float);
    case ustore_doc_field_f16_k: return sizeof(std::uint16_t);
    case ustore_doc_field_i64_k: return sizeof(std::int64_t);
    case ustore_doc_field_i32_k: return sizeof(std::int32_t);
    case ustore_doc_field_i16_k: return sizeof(std::int16_t);
    case ustore_doc_field_i8_k: return sizeof(std::int8_t);
    case ustore_doc_field_u64_k: return sizeof(std::uint64_t);
    case ustore_doc_field_u32_k: return sizeof(std::uint32_t);
    case ustore_doc_field_u16_k: return sizeof(std::uint16_t);
    case ustore_doc_field_u8_k: return sizeof(std::uint8_t);
    case ustore_doc_field_bin_k:
    case ustore_doc_field_str_k: return 2 * sizeof(ustore_length_t);
    default: return 0;
    }
}

/** Bytes one column of @p type takes for @p docs_count documents, with the closing offset, aligned for any scalar. */
static std::size_t column_size_bytes(ustore_doc_field_type_t type, std::size_t docs_count) noexcept {
    std::size_t const bytes = field_size_bytes(type) * docs_count + sizeof(ustore_length_t);
    return next_multiple<std::size_t>(bytes, alignof(std::max_align_t));
}

/** Converts @p value into the scalar at @p index of @p column, zeroing a cell that holds no valid value. */
template <typename scalar_>
static conversion_t gather_scalar(yyjson_val* value, ustore_byte_t* column, std::size_t index) noexcept {
    scalar_ scalar {};
    conversion_t const conversion = json_to_scalar(value, scalar);
    if (!is_valid(conversion)) scalar = {};
    std::memcpy(column + index * sizeof(scalar_), &scalar, sizeof(scalar_));
    return conversion;
}

/** Smallest magnitude that rounds past the largest finite half-precision float. */
constexpr float half_overflow_k = 65520.0f;

/** Encodes a finite @p single below @c half_overflow_k as IEEE 754 binary16, rounding to nearest. */
static std::uint16_t half_of(float single) noexcept {
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
static conversion_t gather_half(yyjson_val* value, ustore_byte_t* column, std::size_t index) noexcept {
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
static conversion_t gather_uuid(yyjson_val* value, ustore_byte_t* column, std::size_t index) noexcept {
    constexpr std::size_t uuid_text_length_k = 36;
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

} // namespace unum::ustore

using namespace unum::ustore;

#pragma region C Interface

void ustore_docs_write(ustore_docs_write_t* request_pointer) {
    ustore_docs_write_t& request = *request_pointer;
    if (!request.tasks_count) return;

    if (!request.db) {
        *request.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    linked_memory_lock_t arena = linked_memory(request.arena, request.options, request.error);
    if (request.error->status) return;

    strided_iterator<ustore_str_view_t const> fields {request.fields, request.fields_stride};
    strided_iterator<ustore_collection_t const> collections {request.collections, request.collections_stride};
    bits_view_t presences {request.presences};
    strided_iterator<ustore_length_t const> offsets {request.offsets, request.offsets_stride};
    strided_iterator<ustore_length_t const> lengths {request.lengths, request.lengths_stride};
    strided_iterator<ustore_bytes_cptr_t const> values {request.values, request.values_stride};
    contents_arg_t contents {presences, offsets, lengths, values, request.tasks_count};

    strided_iterator<ustore_key_t const> keys {request.keys, request.keys_stride};
    if (!request.keys) {
        if (!request.values || !request.id_field) {
            *request.error = {ustore_error_invalid_argument_k, "Documents without keys need values and an id field"};
            return;
        }
        auto extracted = arena.alloc<ustore_key_t>(request.tasks_count, request.error);
        if (request.error->status) return;
        extract_keys(contents, request.id_field, arena, extracted, request.error);
        if (request.error->status) return;
        keys = {extracted.data(), sizeof(ustore_key_t)};
    }
    places_arg_t const places {collections, keys, fields, request.tasks_count};

    bool const has_fields = fields && (!fields.repeats() || *fields);
    if (has_fields || request.type != ustore_doc_field_json_k || request.modification != ustore_doc_modify_upsert_k)
        return read_modify_write(request.db, request.transaction, places, contents, request.options,
                                 static_cast<modification_t>(request.modification), request.type, arena, request.error);

    // Whole JSON documents pass through once they parse, which extracting the keys has already checked.
    for (std::size_t index = 0; request.keys && index != contents.size(); ++index) {
        [[maybe_unused]] json_t const document = json_parse(contents[index], arena, request.error);
        if (request.error->status) return;
    }

    ustore_write_t write {};
    write.db = request.db;
    write.error = request.error;
    write.transaction = request.transaction;
    write.arena = arena;
    write.options = request.options;
    write.tasks_count = request.tasks_count;
    write.collections = request.collections;
    write.collections_stride = request.collections_stride;
    write.keys = keys.get();
    write.keys_stride = keys.stride();
    write.presences = request.presences;
    write.offsets = request.offsets;
    write.offsets_stride = request.offsets_stride;
    write.lengths = request.lengths;
    write.lengths_stride = request.lengths_stride;
    write.values = request.values;
    write.values_stride = request.values_stride;
    ustore_write(&write);
}

void ustore_docs_read(ustore_docs_read_t* request_pointer) {
    ustore_docs_read_t& request = *request_pointer;
    if (!request.tasks_count) return;

    linked_memory_lock_t arena = linked_memory(request.arena, request.options, request.error);
    if (request.error->status) return;

    strided_iterator<ustore_str_view_t const> fields {request.fields, request.fields_stride};
    bool const has_fields = fields && (!fields.repeats() || *fields);
    if (!has_fields && request.type == ustore_doc_field_json_k) {
        ustore_read_t read {};
        read.db = request.db;
        read.error = request.error;
        read.transaction = request.transaction;
        read.snapshot = request.snapshot;
        read.arena = arena;
        read.options = request.options;
        read.tasks_count = request.tasks_count;
        read.collections = request.collections;
        read.collections_stride = request.collections_stride;
        read.keys = request.keys;
        read.keys_stride = request.keys_stride;
        read.presences = request.presences;
        read.offsets = request.offsets;
        read.lengths = request.lengths;
        read.values = request.values;
        return ustore_read(&read);
    }

    if (!request.db) {
        *request.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (request.type != ustore_doc_field_json_k && request.type != ustore_doc_field_str_k) {
        *request.error = {ustore_error_unsupported_k, "Documents read only as JSON or strings"};
        return;
    }

    strided_iterator<ustore_collection_t const> collections {request.collections, request.collections_stride};
    strided_iterator<ustore_key_t const> keys {request.keys, request.keys_stride};
    places_arg_t const places {collections, keys, fields, request.tasks_count};

    growing_tape_t exported {arena};
    exported.reserve(places.size(), request.error);
    if (request.error->status) return;

    auto export_one = [&](std::size_t, ustore_str_view_t field, value_view_t binary_document) {
        json_t const document = json_parse(binary_document, arena, request.error);
        if (request.error->status) return;

        yyjson_val* const value = json_lookup(yyjson_doc_get_root(document.handle), field);
        if (request.type == ustore_doc_field_json_k || !value || yyjson_is_ctn(value))
            return json_dump({value, nullptr}, arena, exported, request.error);

        printed_number_buffer_t print_buffer;
        std::string_view text;
        [[maybe_unused]] conversion_t const conversion = json_to_string(value, print_buffer, text);
        exported.push_back(text, request.error);
        if (request.error->status) return;
        exported.add_terminator(byte_t {0}, request.error);
    };

    read_distinct_documents(request.db, request.transaction, request.snapshot, places, request.options, arena,
                            request.error, export_one);
    if (request.error->status) return;

    if (request.presences) *request.presences = exported.presences().get();
    if (request.offsets) *request.offsets = exported.offsets().begin().get();
    if (request.lengths) *request.lengths = exported.lengths().begin().get();
    if (request.values) *request.values = reinterpret_cast<ustore_bytes_ptr_t>(exported.contents().begin().get());
}

void ustore_docs_gist(ustore_docs_gist_t* request_pointer) {
    ustore_docs_gist_t& request = *request_pointer;
    if (!request.docs_count) return;

    linked_memory_lock_t arena = linked_memory(request.arena, request.options, request.error);
    if (request.error->status) return;

    strided_iterator<ustore_collection_t const> collections {request.collections, request.collections_stride};
    strided_iterator<ustore_key_t const> keys {request.keys, request.keys_stride};
    places_arg_t const places {collections, keys, {}, request.docs_count};
    found_documents_t const found = read_documents(request.db, request.transaction, request.snapshot, places,
                                                   request.options, arena, request.error);
    if (request.error->status) return;

    field_path_buffer_t path = {0};
    uninitialized_array<std::string_view> sorted_paths {arena};
    growing_tape_t exported_paths {arena};
    for (std::size_t index = 0; index != request.docs_count; ++index) {
        json_t const document = json_parse(found[index], arena, request.error);
        if (request.error->status) return;
        if (!document) continue;
        gist_paths(yyjson_doc_get_root(document.handle), path, sorted_paths, exported_paths, request.error);
        if (request.error->status) return;
    }

    if (request.fields_count) *request.fields_count = static_cast<ustore_size_t>(sorted_paths.size());
    if (request.offsets) *request.offsets = exported_paths.offsets().begin().get();
    if (request.fields) *request.fields = reinterpret_cast<ustore_char_t*>(exported_paths.contents().begin().get());
}

void ustore_docs_gather(ustore_docs_gather_t* request_pointer) {
    ustore_docs_gather_t& request = *request_pointer;
    if (!request.docs_count || !request.fields_count) return;

    linked_memory_lock_t arena = linked_memory(request.arena, request.options, request.error);
    if (request.error->status) return;

    strided_iterator<ustore_collection_t const> collections {request.collections, request.collections_stride};
    strided_iterator<ustore_key_t const> keys {request.keys, request.keys_stride};
    strided_iterator<ustore_str_view_t const> fields {request.fields, request.fields_stride};
    strided_iterator<ustore_doc_field_type_t const> types {request.types, request.types_stride};
    places_arg_t const places {collections, keys, {}, request.docs_count};
    found_documents_t const found = read_documents(request.db, request.transaction, request.snapshot, places,
                                                   request.options, arena, request.error);
    if (request.error->status) return;

    // One tape holds six rows of column addresses, then the zeroed bitmaps, then the columns themselves.
    std::size_t const fields_count = request.fields_count;
    std::size_t const docs_count = request.docs_count;
    std::size_t const bitmap_bytes = divide_round_up<std::size_t>(docs_count, bits_in_byte_k);
    std::size_t const bitmaps_count = 1u + (request.columns_conversions != nullptr) +
                                      (request.columns_collisions != nullptr);
    std::size_t const address_row_bytes = sizeof(void*) * fields_count;
    std::size_t const addresses_bytes = address_row_bytes * 6;
    std::size_t const bitmaps_bytes = next_multiple<std::size_t>(bitmap_bytes * bitmaps_count * fields_count,
                                                                 alignof(std::max_align_t));
    std::size_t const columns_bytes = transform_reduce_n(
        types, fields_count, std::size_t {0},
        [docs_count](ustore_doc_field_type_t type) { return column_size_bytes(type, docs_count); });

    auto tape = arena.alloc<byte_t>(addresses_bytes + bitmaps_bytes + columns_bytes, request.error,
                                    alignof(std::max_align_t));
    if (request.error->status) return;
    std::memset(tape.data() + addresses_bytes, 0, bitmaps_bytes);

    auto const validities = reinterpret_cast<ustore_octet_t**>(tape.data());
    auto const conversions = reinterpret_cast<ustore_octet_t**>(tape.data() + address_row_bytes);
    auto const collisions = reinterpret_cast<ustore_octet_t**>(tape.data() + address_row_bytes * 2);
    auto const offsets = reinterpret_cast<ustore_length_t**>(tape.data() + address_row_bytes * 3);
    auto const lengths = reinterpret_cast<ustore_length_t**>(tape.data() + address_row_bytes * 4);
    auto const scalars = reinterpret_cast<ustore_byte_t**>(tape.data() + address_row_bytes * 5);

    auto bitmap = reinterpret_cast<ustore_octet_t*>(tape.data() + addresses_bytes);
    byte_t* column = tape.data() + addresses_bytes + bitmaps_bytes;
    for (std::size_t field_index = 0; field_index != fields_count; ++field_index) {
        validities[field_index] = std::exchange(bitmap, bitmap + bitmap_bytes);
        conversions[field_index] = request.columns_conversions ? std::exchange(bitmap, bitmap + bitmap_bytes)
                                                               : validities[field_index];
        collisions[field_index] = request.columns_collisions ? std::exchange(bitmap, bitmap + bitmap_bytes)
                                                             : validities[field_index];

        ustore_doc_field_type_t const type = types[field_index];
        bool const is_string = type == ustore_doc_field_str_k || type == ustore_doc_field_bin_k;
        offsets[field_index] = is_string ? reinterpret_cast<ustore_length_t*>(column) : nullptr;
        lengths[field_index] = is_string ? offsets[field_index] + docs_count + 1 : nullptr;
        scalars[field_index] = is_string ? nullptr : reinterpret_cast<ustore_byte_t*>(column);
        column += column_size_bytes(type, docs_count);
    }

    if (request.columns_validities) *request.columns_validities = validities;
    if (request.columns_conversions) *request.columns_conversions = conversions;
    if (request.columns_collisions) *request.columns_collisions = collisions;
    if (request.columns_offsets) *request.columns_offsets = offsets;
    if (request.columns_lengths) *request.columns_lengths = lengths;
    if (request.columns_scalars) *request.columns_scalars = scalars;

    printed_number_buffer_t print_buffer;
    uninitialized_array<char> joined {arena};
    for (std::size_t document_index = 0; document_index != docs_count; ++document_index) {
        json_t const document = json_parse(found[document_index], arena, request.error);
        if (request.error->status) return;
        yyjson_val* const root = yyjson_doc_get_root(document.handle);
        std::size_t const slot = document_index / bits_in_byte_k;
        ustore_octet_t const mask = static_cast<ustore_octet_t>(1u << (document_index % bits_in_byte_k));

        for (std::size_t field_index = 0; field_index != fields_count; ++field_index) {
            yyjson_val* const value = json_lookup(root, fields[field_index]);
            ustore_doc_field_type_t const type = types[field_index];
            ustore_byte_t* const scalar_column = scalars[field_index];
            conversion_t conversion = conversion_t::collides_k;

            switch (type) {
            case ustore_doc_field_bool_k: conversion = gather_scalar<bool>(value, scalar_column, document_index); break;
            case ustore_doc_field_uuid_k: conversion = gather_uuid(value, scalar_column, document_index); break;
            case ustore_doc_field_f64_k:
                conversion = gather_scalar<double>(value, scalar_column, document_index);
                break;
            case ustore_doc_field_f32_k: conversion = gather_scalar<float>(value, scalar_column, document_index); break;
            case ustore_doc_field_f16_k: conversion = gather_half(value, scalar_column, document_index); break;
            case ustore_doc_field_i64_k:
                conversion = gather_scalar<std::int64_t>(value, scalar_column, document_index);
                break;
            case ustore_doc_field_i32_k:
                conversion = gather_scalar<std::int32_t>(value, scalar_column, document_index);
                break;
            case ustore_doc_field_i16_k:
                conversion = gather_scalar<std::int16_t>(value, scalar_column, document_index);
                break;
            case ustore_doc_field_i8_k:
                conversion = gather_scalar<std::int8_t>(value, scalar_column, document_index);
                break;
            case ustore_doc_field_u64_k:
                conversion = gather_scalar<std::uint64_t>(value, scalar_column, document_index);
                break;
            case ustore_doc_field_u32_k:
                conversion = gather_scalar<std::uint32_t>(value, scalar_column, document_index);
                break;
            case ustore_doc_field_u16_k:
                conversion = gather_scalar<std::uint16_t>(value, scalar_column, document_index);
                break;
            case ustore_doc_field_u8_k:
                conversion = gather_scalar<std::uint8_t>(value, scalar_column, document_index);
                break;
            case ustore_doc_field_str_k:
            case ustore_doc_field_bin_k: {
                std::string_view text;
                conversion = json_to_string(value, print_buffer, text);
                offsets[field_index][document_index] = static_cast<ustore_length_t>(joined.size());
                lengths[field_index][document_index] = static_cast<ustore_length_t>(text.size());
                joined.insert(joined.size(), text.data(), text.data() + text.size(), request.error);
                if (request.error->status) return;
                if (type == ustore_doc_field_str_k) joined.push_back('\0', request.error);
                if (request.error->status) return;
                offsets[field_index][document_index + 1] = static_cast<ustore_length_t>(joined.size());
                break;
            }
            default: break;
            }

            record_conversion(conversion, mask, validities[field_index][slot], conversions[field_index][slot],
                              collisions[field_index][slot]);
        }
    }

    if (request.joined_strings) *request.joined_strings = reinterpret_cast<ustore_byte_t*>(joined.data());
}

#pragma endregion C Interface
