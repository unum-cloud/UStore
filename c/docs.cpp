/**
 *  @file c/docs.cpp
 *  @author Ash Vardanian
 *  @date December 26, 2022
 *  @brief Documents modality on yyjson, running on top of any engine that implements <ustore/db.h>.
 *
 *  Documents are stored as JSON text.
 *  Whole-document reads and upserts pass straight through to the engine's own read and write, while field access,
 *  conversions and modifications parse every document with yyjson, allocating from the arena of the request.
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
#include <utility>      // `std::in_range`

#include <fmt/format.h> // `fmt::format_to_n`
#include <yyjson.h>     // `yyjson_read_opts`, `yyjson_mut_doc`

#include "ustore/docs.h"          // `ustore_docs_write_t`, `ustore_docs_read_t`
#include "ustore/ranges_args.hpp" // `places_arg_t`, `contents_arg_t`, `arena_t`

#include "engine.hpp"                // `engine_of`, `view_t`, `docs_write_in_json_values`
#include "helpers/algorithm.hpp"     // `offset_in_sorted`
#include "helpers/linked_array.hpp"  // `growing_tape_t`, `uninitialized_array`
#include "helpers/linked_memory.hpp" // `arena_allocate`, `arena_grow`

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

static void* json_allocate(void* context, std::size_t size) noexcept {
    auto [bytes, status] = arena_allocate<byte_t>(static_cast<ustore_arena_t*>(context), size,
                                                  alignof(std::max_align_t));
    return bytes.data();
}

static void* json_reallocate(void* context, void* pointer, std::size_t old_size, std::size_t size) noexcept {
    if (size <= old_size) return pointer;
    std::span<byte_t> const old_bytes {static_cast<byte_t*>(pointer), old_size};
    auto [bytes, status] = arena_grow(static_cast<ustore_arena_t*>(context), old_bytes, size - old_size,
                                      alignof(std::max_align_t));
    return bytes.data();
}

/** Arena memory is released with the arena, never one allocation at a time. */
static void json_free(void*, void*) noexcept {}

static yyjson_alc json_allocator(ustore_arena_t* arena) noexcept {
    return {json_allocate, json_reallocate, json_free, arena};
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

/** Spells @p field as a JSON Pointer, escaping a top-level key into @p arena. */
static expected<std::string_view> field_pointer(ustore_str_view_t field, ustore_arena_t* arena) noexcept {
    std::string_view const key {field};
    if (key.starts_with('/')) return key;

    auto [pointer, status] = arena_allocate<char>(arena, escaped_length(key) + 1);
    if (status.failed()) return status;

    pointer[0] = '/';
    char const* const end = escape_into(key, pointer.data() + 1);
    return std::string_view {pointer.data(), static_cast<std::size_t>(end - pointer.data())};
}

/** Parses a JSON document, leaving the result empty for an empty or a missing value. */
static expected<json_t> json_parse(value_view_t bytes, ustore_arena_t* arena) noexcept {
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
static expected<yyjson_mut_doc*> json_mutable(json_t& document, ustore_arena_t* arena) noexcept {
    if (document.mutable_handle) return document.mutable_handle;

    yyjson_alc const allocator = json_allocator(arena);
    document.mutable_handle = document.handle //
                                  ? yyjson_doc_mut_copy(document.handle, &allocator)
                                  : yyjson_mut_doc_new(&allocator);
    if (!document.mutable_handle)
        return status_t {status_code_t::out_of_memory_k, "Failed to allocate a mutable document"};
    return document.mutable_handle;
}

/** Appends @p value to @p output as JSON text, or appends a missing value when there is none. */
static status_t json_dump(json_value_t value, ustore_arena_t* arena, growing_tape_t& output) noexcept {
    if (!value) return output.push_back(value_view_t {}).status();

    yyjson_alc const allocator = json_allocator(arena);
    yyjson_write_flag const flags = YYJSON_WRITE_ALLOW_INF_AND_NAN;
    std::size_t length = 0;
    char const* const text = value.mutable_handle
                                 ? yyjson_mut_val_write_opts(value.mutable_handle, flags, &allocator, &length, nullptr)
                                 : yyjson_val_write_opts(value.handle, flags, &allocator, &length, nullptr);
    if (!text) return {status_code_t::out_of_memory_k, "Failed to serialize the document"};
    return output.push_back(value_view_t {text, length}).status();
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
static expected<json_t> any_parse(value_view_t bytes, ustore_doc_field_type_t type, ustore_arena_t* arena) noexcept {
    if (type == ustore_doc_field_json_k) return json_parse(bytes, arena);

    json_t document;
    yyjson_alc const allocator = json_allocator(arena);
    document.mutable_handle = yyjson_mut_doc_new(&allocator);
    if (!document.mutable_handle)
        return status_t {status_code_t::out_of_memory_k, "Failed to allocate a mutable document"};

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

    if (!root) return status_t {status_code_t::unsupported_k, "Input type is not supported"};
    yyjson_mut_doc_set_root(handle, root);
    return document;
}

#pragma endregion Conversions

#pragma region Modifications

/** Modifications a request applies, extending @c ustore_doc_modification_t with reads. */
enum class modification_t {
    nothing_k = -1,
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
static status_t modify(json_t& original, yyjson_mut_val* modifier, ustore_str_view_t field, modification_t modification,
                       ustore_arena_t* arena) noexcept {

    auto [document, status] = json_mutable(original, arena);
    if (status.failed()) return status;

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
        if (!modified && modification == modification_t::patch_k)
            return {status_code_t::invalid_argument_k, "The patch does not apply to the document"};
        if (!modified) return {status_code_t::out_of_memory_k, "Failed to modify the document"};
        yyjson_mut_doc_set_root(document, modified);
        return {};
    }

    auto [pointer, pointer_status] = field_pointer(field, arena);
    if (pointer_status.failed()) return pointer_status;

    yyjson_mut_val* replacement = nullptr;
    switch (modification) {
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
    if (!replacement) return {status_code_t::invalid_argument_k, "The modifier does not apply to the field"};

    bool succeeded = false;
    switch (modification) {
    case modification_t::insert_k:
        succeeded = yyjson_mut_doc_ptr_addn(document, pointer.data(), pointer.size(), replacement);
        break;
    case modification_t::update_k:
        succeeded = yyjson_mut_doc_ptr_replacen(document, pointer.data(), pointer.size(), replacement) != nullptr;
        break;
    default: succeeded = yyjson_mut_doc_ptr_setn(document, pointer.data(), pointer.size(), replacement); break;
    }
    if (!succeeded) return {status_code_t::not_found_k, "The field path does not resolve inside the document"};
    return {};
}

#pragma endregion Modifications

#pragma region Reading Documents

/** Receives the index of a task, its field, and the binary document it addresses, and answers a status. */
template <typename callback_type_>
concept document_callback =
    std::is_invocable_r_v<status_t, callback_type_&, std::size_t, ustore_str_view_t, value_view_t>;

/** Binary documents found by one read, indexed by task. */
struct found_documents_t {

    /** Bits marking the documents found. */
    ustore_octet_t* presences = nullptr;

    /** One offset into @c contents per document, and one more closing the last. */
    ustore_length_t* offsets = nullptr;

    /** Joined contents of the documents. */
    ustore_byte_t* contents = nullptr;

    value_view_t operator[](std::size_t index) const noexcept {
        if (!bits_view_t {presences}[index]) return value_view_t {};
        return value_view_t {contents + offsets[index], offsets[index + 1] - offsets[index]};
    }
};

/** Reads the documents at @p places through @p view with the engine's own read, which appends to @p arena rather
 *  than rewinding it. */
static expected<found_documents_t> read_documents(ustore_database_t database, places_arg_t const& places,
                                                  ustore_arena_t* arena, view_t const& view) noexcept {
    std::size_t const count = places.count;
    auto [presences, presences_status] = arena_allocate<ustore_octet_t>(arena, divide_round_up(count, bits_in_byte_k));
    if (presences_status.failed()) return presences_status;
    auto [offsets, offsets_status] = arena_allocate<ustore_length_t>(arena, count + 1);
    if (offsets_status.failed()) return offsets_status;

    found_documents_t found {presences.data(), offsets.data(), nullptr};
    ustore_read_t read {};
    read.database = database;
    read.count = count;
    read.collections = places.collections_begin.get();
    read.collections_stride = places.collections_begin.stride();
    read.keys = places.keys_begin.get();
    read.keys_stride = places.keys_begin.stride();
    read.presences = found.presences;
    read.offsets = found.offsets;
    read.values = &found.contents;
    read.arena = arena;
    if (status_t const status = engine_of(database).read(read, view); status.failed()) return status;
    return found;
}

/**
 *  @brief Calls @p callback on the document of every task, in order.
 *
 *  Fails a field modification of a missing document, an update or a patch of a whole document that is missing,
 *  and an insertion of a whole document that already exists.
 */
template <document_callback callback_type_>
static status_t read_each_document(ustore_database_t database, places_arg_t const& places, modification_t modification,
                                   ustore_arena_t* arena, view_t const& view, callback_type_&& callback) noexcept {

    bool const has_fields = places.fields_begin && (!places.fields_begin.repeats() || *places.fields_begin);
    auto [found, status] = read_documents(database, places, arena, view);
    if (status.failed()) return status;

    for (std::size_t index = 0; index != places.size(); ++index) {
        value_view_t const document = found[index];
        if (!document && has_fields && modification != modification_t::nothing_k)
            return {status_code_t::not_found_k, "Modified field belongs to a missing document"};
        bool const replaces_existing = modification == modification_t::update_k ||
                                       modification == modification_t::patch_k;
        if (!document && !has_fields && replaces_existing)
            return {status_code_t::not_found_k, "Updated or patched document is missing"};
        if (document && !has_fields && modification == modification_t::insert_k)
            return {status_code_t::already_exists_k, "Inserted document already exists"};
        if (status_t const handled = callback(index, places.fields_begin ? places.fields_begin[index] : nullptr,
                                              document);
            handled.failed())
            return handled;
    }
    return {};
}

/** Calls @p callback on the document of every task, reading every distinct document once. */
template <document_callback callback_type_>
static status_t read_distinct_documents(ustore_database_t database, places_arg_t const& places, ustore_arena_t* arena,
                                        view_t const& view, callback_type_&& callback) noexcept {

    if (all_ascending(places.keys_begin, places.count))
        return read_each_document(database, places, modification_t::nothing_k, arena, view, callback);

    auto [distinct, distinct_status] = arena_allocate<collection_key_t>(arena, places.count);
    if (distinct_status.failed()) return distinct_status;
    transform_n(places, places.count, distinct.begin(), [](place_t const& place) { return place.collection_key(); });
    std::size_t const distinct_count = sort_and_deduplicate(distinct.begin(), distinct.end());
    if (distinct_count == places.count)
        return read_each_document(database, places, modification_t::nothing_k, arena, view, callback);

    std::span<collection_key_t> const sorted = distinct.first(distinct_count);
    auto const sorted_strided = strided(sorted).immutable();
    places_arg_t distinct_places;
    distinct_places.collections_begin = sorted_strided.members(&collection_key_t::collection).begin();
    distinct_places.keys_begin = sorted_strided.members(&collection_key_t::key).begin();
    distinct_places.count = static_cast<ustore_size_t>(distinct_count);

    auto [found, found_status] = read_documents(database, distinct_places, arena, view);
    if (found_status.failed()) return found_status;

    for (std::size_t index = 0; index != places.size(); ++index) {
        place_t const place = places[index];
        if (status_t const handled = callback(index, place.field,
                                              found[offset_in_sorted(sorted, place.collection_key())]);
            handled.failed())
            return handled;
    }
    return {};
}

#pragma endregion Reading Documents

#pragma region Writing

/** Parses, modifies and writes back the documents at @p places through the engine's own read and write. */
static status_t read_modify_write(ustore_database_t database, places_arg_t const& places,
                                  contents_arg_t const& contents, modification_t modification,
                                  ustore_doc_field_type_t type, ustore_durability_t durability, ustore_arena_t* arena,
                                  view_t const& view) noexcept {

    growing_tape_t modified {arena};
    if (status_t const status = modified.reserve(places.size()); status.failed()) return status;

    auto modify_one = [&](std::size_t index, ustore_str_view_t field, value_view_t binary_document) noexcept {
        auto [document, document_status] = json_parse(binary_document, arena);
        if (document_status.failed()) return document_status;
        auto [modifier, modifier_status] = any_parse(contents[index], type, arena);
        if (modifier_status.failed()) return modifier_status;
        if (!modifier) return status_t {status_code_t::malformed_k, "An empty value is no JSON document"};
        auto [modifier_document, mutable_status] = json_mutable(modifier, arena);
        if (mutable_status.failed()) return mutable_status;
        if (status_t const status = modify(document, yyjson_mut_doc_get_root(modifier_document), field, modification,
                                           arena);
            status.failed())
            return status;
        return json_dump({nullptr, yyjson_mut_doc_get_root(document.mutable_handle)}, arena, modified);
    };

    // A transaction watches the documents it rewrites, whatever the caller asked of the writes
    view_t watching = view;
    watching.watch = ustore_watch_keys_k;
    if (status_t const status = read_each_document(database, places, modification, arena, watching, modify_one);
        status.failed())
        return status;

    // Every value ends where the next one starts, as the tape keeps one offset more than values
    ustore_bytes_cptr_t const modified_contents = reinterpret_cast<ustore_bytes_cptr_t>(
        modified.contents().begin().get());
    ustore_write_t write {};
    write.database = database;
    write.count = places.count;
    write.collections = places.collections_begin.get();
    write.collections_stride = places.collections_begin.stride();
    write.keys = places.keys_begin.get();
    write.keys_stride = places.keys_begin.stride();
    write.starts = modified.offsets().begin().get();
    write.starts_stride = modified.offsets().stride();
    write.values = &modified_contents;
    write.durability = durability;
    return engine_of(database).write(write, view);
}

/** Reads @p text as a UUID in its canonical 8-4-4-4-12 hexadecimal form into @p uuid, answering whether it is one. */
static bool parse_uuid(std::string_view text, ustore_uuid_t& uuid) noexcept {
    if (text.size() != 36) return false;
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

/** Reads the key of every document from its @p id_field into @p keys: a non-negative integer, spelled as the key of
 *  that integer, or a UUID in canonical text. */
static status_t extract_keys(contents_arg_t const& contents, ustore_str_view_t id_field, ustore_arena_t* arena,
                             std::span<ustore_key_t> keys) noexcept {
    for (std::size_t index = 0; index != contents.size(); ++index) {
        auto [document, status] = json_parse(contents[index], arena);
        if (status.failed()) return status;
        yyjson_val* const identifier = json_lookup(yyjson_doc_get_root(document.handle), id_field);
        if (yyjson_is_uint(identifier)) {
            keys[index] = key_from_integer(yyjson_get_uint(identifier));
            continue;
        }
        bool const is_uuid = yyjson_is_str(identifier) &&
                             parse_uuid({yyjson_get_str(identifier), yyjson_get_len(identifier)}, keys[index].uuid);
        if (!is_uuid)
            return {status_code_t::malformed_k, "The id field holds neither a non-negative integer nor a UUID"};
    }
    return {};
}

#pragma endregion Writing

#pragma region Gist

/** Collects the unique paths to the leaves under @p node, extending the null-terminated @p path. */
static status_t gist_paths(yyjson_val* node, field_path_buffer_t& path,
                           uninitialized_array<std::string_view>& sorted_paths,
                           growing_tape_t& exported_paths) noexcept {

    std::size_t const path_length = std::strlen(path);

    if (yyjson_is_obj(node)) {
        yyjson_obj_iter iterator;
        yyjson_obj_iter_init(node, &iterator);
        while (yyjson_val* const key = yyjson_obj_iter_next(&iterator)) {
            std::string_view const name {yyjson_get_str(key), yyjson_get_len(key)};
            if (path_length + 1 + escaped_length(name) >= field_path_length_limit_k)
                return {status_code_t::capacity_exhausted_k, "Path is too long"};
            path[path_length] = '/';
            *escape_into(name, path + path_length + 1) = '\0';
            if (status_t const status = gist_paths(yyjson_obj_iter_get_val(key), path, sorted_paths, exported_paths);
                status.failed())
                return status;
        }
        path[path_length] = '\0';
        return {};
    }

    if (yyjson_is_arr(node)) {
        std::size_t element_index = 0;
        yyjson_arr_iter iterator;
        yyjson_arr_iter_init(node, &iterator);
        while (yyjson_val* const element = yyjson_arr_iter_next(&iterator)) {
            path[path_length] = '/';
            std::string_view const printed = print_number(path + path_length + 1, path + field_path_length_limit_k,
                                                          element_index++);
            if (printed.empty()) return {status_code_t::capacity_exhausted_k, "Path is too long"};
            if (status_t const status = gist_paths(element, path, sorted_paths, exported_paths); status.failed())
                return status;
        }
        path[path_length] = '\0';
        return {};
    }

    std::string_view const leaf {path, path_length};
    std::size_t const position = std::lower_bound(sorted_paths.begin(), sorted_paths.end(), leaf) -
                                 sorted_paths.begin();
    if (position != sorted_paths.size() && sorted_paths[position] == leaf) return {};

    auto [exported, status] = exported_paths.push_back(leaf);
    if (status.failed()) return status;
    if (status_t const terminated = exported_paths.add_terminator(byte_t {0}); terminated.failed()) return terminated;

    std::string_view const stored {exported.c_str(), exported.size()};
    return sorted_paths.insert(position, &stored, &stored + 1);
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

/** Whether @p arguments carry the column field @p field_index of @p type writes into, where a type without one needs
 *  none. */
static bool has_column(ustore_docs_gather_t const& arguments, std::size_t field_index,
                       ustore_doc_field_type_t type) noexcept {
    if (type == ustore_doc_field_str_k || type == ustore_doc_field_bin_k)
        return arguments.columns_offsets && arguments.columns_offsets[field_index];
    return !field_size_bytes(type) || (arguments.columns_scalars && arguments.columns_scalars[field_index]);
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

#pragma region Document Calls

status_t docs_write_in_json_values(ustore_docs_write_t const& arguments, view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!arguments.values) return {status_code_t::invalid_argument_k, "No values were provided"};
    if (!arguments.keys && !arguments.id_field)
        return {status_code_t::invalid_argument_k, "Documents without keys need an id field"};

    // A write carries no arena, so the documents it parses and modifies live in scratch freed once it returns
    arena_t scratch;
    strided_iterator<ustore_str_view_t const> const fields {arguments.fields, arguments.fields_stride};
    strided_iterator<ustore_collection_id_t const> const collections {arguments.collections,
                                                                      arguments.collections_stride};
    contents_arg_t const contents {{arguments.starts, arguments.starts_stride},
                                   {arguments.lengths, arguments.lengths_stride},
                                   {arguments.values, arguments.values_stride},
                                   arguments.count};

    strided_iterator<ustore_key_t const> keys {arguments.keys, arguments.keys_stride};
    if (!arguments.keys) {
        auto [extracted, status] = arena_allocate<ustore_key_t>(scratch, arguments.count);
        if (status.failed()) return status;
        if (status_t const extracting = extract_keys(contents, arguments.id_field, scratch, extracted);
            extracting.failed())
            return extracting;
        keys = {extracted.data(), sizeof(ustore_key_t)};
    }
    places_arg_t const places {collections, keys, fields, arguments.count};

    bool const has_fields = fields && (!fields.repeats() || *fields);
    if (has_fields || arguments.type != ustore_doc_field_json_k || arguments.modification != ustore_doc_modify_upsert_k)
        return read_modify_write(arguments.database, places, contents,
                                 static_cast<modification_t>(arguments.modification), arguments.type,
                                 arguments.durability, scratch, view);

    // Whole JSON documents pass through once they parse, which extracting the keys has already checked
    for (std::size_t index = 0; arguments.keys && index != contents.size(); ++index)
        if (status_t const status = json_parse(contents[index], scratch).status(); status.failed()) return status;

    ustore_write_t write {};
    write.database = arguments.database;
    write.count = arguments.count;
    write.collections = arguments.collections;
    write.collections_stride = arguments.collections_stride;
    write.keys = keys.get();
    write.keys_stride = keys.stride();
    write.starts = arguments.starts;
    write.starts_stride = arguments.starts_stride;
    write.lengths = arguments.lengths;
    write.lengths_stride = arguments.lengths_stride;
    write.values = arguments.values;
    write.values_stride = arguments.values_stride;
    write.durability = arguments.durability;
    return engine_of(arguments.database).write(write, view);
}

status_t docs_read_in_json_values(ustore_docs_read_t const& arguments, view_t const& view) noexcept {
    if (!arguments.count) return {};

    strided_iterator<ustore_str_view_t const> const fields {arguments.fields, arguments.fields_stride};
    bool const has_fields = fields && (!fields.repeats() || *fields);
    if (!has_fields && arguments.type == ustore_doc_field_json_k) {
        ustore_read_t read {};
        read.database = arguments.database;
        read.count = arguments.count;
        read.collections = arguments.collections;
        read.collections_stride = arguments.collections_stride;
        read.keys = arguments.keys;
        read.keys_stride = arguments.keys_stride;
        read.presences = arguments.presences;
        read.offsets = arguments.offsets;
        read.values = arguments.values;
        read.arena = arguments.arena;
        return engine_of(arguments.database).read(read, view);
    }
    if (arguments.type != ustore_doc_field_json_k && arguments.type != ustore_doc_field_str_k)
        return {status_code_t::unsupported_k, "Documents read only as JSON or strings"};

    strided_iterator<ustore_collection_id_t const> const collections {arguments.collections,
                                                                      arguments.collections_stride};
    strided_iterator<ustore_key_t const> const keys {arguments.keys, arguments.keys_stride};
    places_arg_t const places {collections, keys, fields, arguments.count};

    growing_tape_t exported {arguments.arena};
    if (status_t const status = exported.reserve(places.size()); status.failed()) return status;

    // A value is present when its field is, and a missing one takes no bytes of the tape
    bits_span_t const presences {arguments.presences};
    auto export_one = [&](std::size_t index, ustore_str_view_t field, value_view_t binary_document) noexcept {
        auto [document, status] = json_parse(binary_document, arguments.arena);
        if (status.failed()) return status;

        yyjson_val* const value = json_lookup(yyjson_doc_get_root(document.handle), field);
        if (arguments.presences) presences[index] = value != nullptr;
        if (arguments.type == ustore_doc_field_json_k || !value || yyjson_is_ctn(value))
            return json_dump({value, nullptr}, arguments.arena, exported);

        printed_number_buffer_t print_buffer;
        std::string_view text;
        [[maybe_unused]] conversion_t const conversion = json_to_string(value, print_buffer, text);
        return exported.push_back(text).status();
    };
    if (status_t const status = read_distinct_documents(arguments.database, places, arguments.arena, view, export_one);
        status.failed())
        return status;

    strided_range<ustore_length_t> const exported_offsets = exported.offsets();
    if (arguments.offsets)
        for (std::size_t index = 0; index != places.size() + 1; ++index)
            arguments.offsets[index] = exported_offsets[index];
    if (arguments.values) *arguments.values = reinterpret_cast<ustore_byte_t*>(exported.contents().begin().get());
    return {};
}

status_t docs_gist_in_json_values(ustore_docs_gist_t const& arguments, view_t const& view) noexcept {
    if (!arguments.count) return {};

    strided_iterator<ustore_collection_id_t const> const collections {arguments.collections,
                                                                      arguments.collections_stride};
    strided_iterator<ustore_key_t const> const keys {arguments.keys, arguments.keys_stride};
    places_arg_t const places {collections, keys, {}, arguments.count};
    auto [found, found_status] = read_documents(arguments.database, places, arguments.arena, view);
    if (found_status.failed()) return found_status;

    field_path_buffer_t path = {0};
    uninitialized_array<std::string_view> sorted_paths {arguments.arena};
    growing_tape_t exported_paths {arguments.arena};
    for (std::size_t index = 0; index != arguments.count; ++index) {
        auto [document, document_status] = json_parse(found[index], arguments.arena);
        if (document_status.failed()) return document_status;
        if (!document) continue;
        if (status_t const status = gist_paths(yyjson_doc_get_root(document.handle), path, sorted_paths,
                                               exported_paths);
            status.failed())
            return status;
    }

    if (arguments.fields_count) *arguments.fields_count = static_cast<ustore_size_t>(sorted_paths.size());
    if (arguments.offsets) *arguments.offsets = exported_paths.offsets().begin().get();
    if (arguments.fields) *arguments.fields = reinterpret_cast<ustore_char_t*>(exported_paths.contents().begin().get());
    return {};
}

status_t docs_gather_in_json_values(ustore_docs_gather_t const& arguments, view_t const& view) noexcept {
    if (!arguments.count || !arguments.fields_count) return {};

    std::size_t const fields_count = arguments.fields_count;
    std::size_t const docs_count = arguments.count;
    strided_iterator<ustore_str_view_t const> const fields {arguments.fields, arguments.fields_stride};
    strided_iterator<ustore_doc_field_type_t const> const types {arguments.types, arguments.types_stride};
    for (std::size_t field_index = 0; field_index != fields_count; ++field_index)
        if (!has_column(arguments, field_index, types[field_index]))
            return {status_code_t::invalid_argument_k, "Need an output column for every field"};

    strided_iterator<ustore_collection_id_t const> const collections {arguments.collections,
                                                                      arguments.collections_stride};
    strided_iterator<ustore_key_t const> const keys {arguments.keys, arguments.keys_stride};
    places_arg_t const places {collections, keys, {}, docs_count};
    auto [found, found_status] = read_documents(arguments.database, places, arguments.arena, view);
    if (found_status.failed()) return found_status;

    printed_number_buffer_t print_buffer;
    uninitialized_array<char> joined {arguments.arena};
    for (std::size_t document_index = 0; document_index != docs_count; ++document_index) {
        auto [document, document_status] = json_parse(found[document_index], arguments.arena);
        if (document_status.failed()) return document_status;
        yyjson_val* const root = yyjson_doc_get_root(document.handle);
        std::size_t const slot = document_index / bits_in_byte_k;
        ustore_octet_t const mask = static_cast<ustore_octet_t>(1u << (document_index % bits_in_byte_k));

        for (std::size_t field_index = 0; field_index != fields_count; ++field_index) {
            yyjson_val* const value = json_lookup(root, fields[field_index]);
            ustore_doc_field_type_t const type = types[field_index];
            ustore_byte_t* const scalar_column = arguments.columns_scalars ? arguments.columns_scalars[field_index]
                                                                           : nullptr;
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
                ustore_length_t* const offsets = arguments.columns_offsets[field_index];
                ustore_length_t* const lengths = arguments.columns_lengths ? arguments.columns_lengths[field_index]
                                                                           : nullptr;
                offsets[document_index] = static_cast<ustore_length_t>(joined.size());
                if (lengths) lengths[document_index] = static_cast<ustore_length_t>(text.size());
                if (status_t const status = joined.insert(joined.size(), text.data(), text.data() + text.size());
                    status.failed())
                    return status;
                if (type == ustore_doc_field_str_k)
                    if (status_t const status = joined.push_back('\0'); status.failed()) return status;
                offsets[document_index + 1] = static_cast<ustore_length_t>(joined.size());
                break;
            }
            default: break;
            }

            // A bitmap the caller asked for none of records into a scratch octet
            ustore_octet_t ignored = 0;
            record_conversion(
                conversion, mask,
                arguments.columns_validities ? arguments.columns_validities[field_index][slot] : ignored,
                arguments.columns_conversions ? arguments.columns_conversions[field_index][slot] : ignored,
                arguments.columns_collisions ? arguments.columns_collisions[field_index][slot] : ignored);
        }
    }

    if (arguments.joined_strings) *arguments.joined_strings = reinterpret_cast<ustore_byte_t*>(joined.data());
    return {};
}

#pragma endregion Document Calls

} // namespace unum::ustore
