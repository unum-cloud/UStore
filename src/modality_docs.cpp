/**
 * @file modality_docs.cpp
 * @author Ashot Vardanian
 *
 * @brief Document storage using "YYJSON" lib.
 * Sits on top of any @see "ukv.h"-compatible system.
 */
#include <cstdio>      // `std::snprintf`
#include <cctype>      // `std::isdigit`
#include <charconv>    // `std::to_chars`
#include <string_view> // `std::string_view`

#include <fmt/format.h> // `fmt::format_int`

#include <simdjson.h>          // Secondary internal JSON representation
#include <yyjson.h>            // Primary internal JSON representation
#include <bson.h>              // Converting from/to BSON
#include <mpack_header_only.h> // Converting from/to MsgPack

#include "ukv/docs.h"                //
#include "helpers/linked_memory.hpp" // `linked_memory_lock_t`
#include "helpers/linked_array.hpp"  // `growing_tape_t`
#include "helpers/algorithm.hpp"     // `transform_n`
#include "ukv/cpp/ranges_args.hpp"   // `places_arg_t`

/*********************************************************/
/*****************	 C++ Implementation	  ****************/
/*********************************************************/

using namespace unum::ukv;
using namespace unum;

namespace sj = simdjson;

constexpr ukv_doc_field_type_t internal_format_k = ukv_doc_field_json_k;

static constexpr char const* null_k = "null";

static constexpr char const* true_k = "true";
static constexpr char const* false_k = "false";

static constexpr const char* open_k = "{";
static constexpr const char* close_k = "}";

static constexpr const char* open_arr_k = "[";
static constexpr const char* close_arr_k = "]";

static constexpr const char* separator_k = ",";

enum class doc_modification_t {
    nothing_k = -1,
    remove_k = -2,
    upsert_k = ukv_doc_modify_upsert_k,
    update_k = ukv_doc_modify_update_k,
    insert_k = ukv_doc_modify_insert_k,
    patch_k = ukv_doc_modify_patch_k,
    merge_k = ukv_doc_modify_merge_k,
};

/// The length of buffer to be used to convert/format/print numerical values into strings.
constexpr std::size_t printed_number_length_limit_k = 32;
constexpr std::size_t field_path_len_limit_k = 512;

using printed_number_buffer_t = char[printed_number_length_limit_k];
using field_path_buffer_t = char[field_path_len_limit_k];

/*********************************************************/
/*****************	 STL Compatibility	  ****************/
/*********************************************************/

/**
 * @brief Parses `float`, `double`, `bool` or any integral type from string.
 * @return true If not the entire string was recognized as a number.
 */
template <typename at>
bool parse_entire_number(char const* begin, char const* end, at& result) {
    // Floats:
    if constexpr (std::is_same_v<at, float>) {
        char* number_end = nullptr;
        result = std::strtof(begin, &number_end);
        return end == number_end;
    }
    // Doubles:
    else if constexpr (std::is_same_v<at, double>) {
        char* number_end = nullptr;
        result = std::strtod(begin, &number_end);
        return end == number_end;
    }
    // Booleans:
    else if constexpr (std::is_same_v<at, bool>) {
        bool is_true = (end - begin) == 4 && std::equal(begin, end, true_k);
        bool is_false = (end - begin) == 5 && std::equal(begin, end, false_k);
        if (is_true | is_false) {
            result = is_true;
            return true;
        }
        else
            return false;
    }
    // Integers:
    else {
        return std::from_chars(begin, end, result).ptr == end;
    }
}

/**
 * @brief Prints a number into a string buffer. Terminates with zero character.
 * @return The string-vew until the termination character. Empty string on failure.
 */
template <typename at>
std::string_view print_number(char* begin, char* end, at scalar) {
    if constexpr (std::is_floating_point_v<at>) {
        // Parsing and dumping floating-point numbers is still not fully implemented in STL:
        //  std::to_chars_result result = std::to_chars(&print_buffer[0], print_buffer + printed_number_length_limit_k,
        //  scalar); bool fits_terminator = result.ec != std::errc() && result.ptr < print_buffer +
        //  printed_number_length_limit_k;
        // Using FMT would cause an extra dependency:
        //  auto end_ptr = fmt::format_to(print_buffer, "{}", scalar);
        //  bool fits_terminator = end_ptr < print_buffer + printed_number_length_limit_k;
        // If we use `std::snprintf`, the result will @b already be NULL-terminated:
        auto result = std::snprintf(begin, end - begin, "%f", scalar);
        return result > 0 //
                   ? std::string_view {begin, static_cast<std::size_t>(result - 1)}
                   : std::string_view {};
    }
    else {
        // `std::to_chars` won't NULL-terminate the string, but we should.
        auto result = std::to_chars(begin, end, scalar);
        if (result.ec != std::errc() || result.ptr == end)
            return {};

        *result.ptr = '\0';
        return {begin, static_cast<std::size_t>(result.ptr - begin)};
    }
}

/*********************************************************/
/*****************	 Working with JSONs	  ****************/
/*********************************************************/

struct json_t {
    yyjson_doc* handle = nullptr;
    yyjson_mut_doc* mut_handle = nullptr;

    ~json_t() noexcept {
        if (mut_handle)
            yyjson_mut_doc_free(mut_handle);
        if (handle)
            yyjson_doc_free(handle);
    }

    json_t() = default;
    json_t(json_t const&) = delete;
    json_t& operator=(json_t const&) = delete;

    json_t(json_t&& other) noexcept
        : handle(std::exchange(other.handle, nullptr)), mut_handle(std::exchange(other.mut_handle, nullptr)) {}
    json_t& operator=(json_t&& other) noexcept {
        std::swap(handle, other.handle);
        std::swap(mut_handle, other.mut_handle);
        return *this;
    }

    explicit operator bool() const noexcept { return handle || mut_handle; }
};

struct json_branch_t {
    yyjson_val* handle = nullptr;
    yyjson_mut_val* mut_handle = nullptr;

    explicit operator bool() const noexcept { return handle || mut_handle; }
    yyjson_val* punned() const noexcept { return mut_handle ? (yyjson_val*)(mut_handle) : handle; }
};

static void* json_yy_malloc(void* ctx, size_t length) noexcept {
    ukv_error_t error = nullptr;
    linked_memory_lock_t& arena = *reinterpret_cast<linked_memory_lock_t*>(ctx);
    auto result = arena.alloc<byte_t>(length + sizeof(length), &error).begin();
    if (!result)
        return result;
    std::memcpy(result, &length, sizeof(length));
    return result + sizeof(length);
}

static void* json_yy_realloc(void* ctx, void* ptr, size_t length) noexcept {
    ukv_error_t error = nullptr;
    linked_memory_lock_t& arena = *reinterpret_cast<linked_memory_lock_t*>(ctx);
    auto bytes = reinterpret_cast<byte_t*>(ptr) - sizeof(length);
    auto old_length = *reinterpret_cast<size_t*>(bytes);
    auto old_size = old_length + sizeof(length);
    auto new_size = length + sizeof(length);
    auto result = arena.grow<byte_t>({bytes, old_size}, new_size - old_size, &error).begin();
    if (!result)
        return result;
    std::memcpy(result, &length, sizeof(length));
    return result + sizeof(length);
}

static void json_yy_free(void*, void* ptr) noexcept {
}

yyjson_alc wrap_allocator(linked_memory_lock_t& arena) {
    yyjson_alc allocator;
    allocator.malloc = json_yy_malloc;
    allocator.realloc = json_yy_realloc;
    allocator.free = json_yy_free;
    allocator.ctx = &arena;
    return allocator;
}

auto simdjson_lookup(sj::ondemand::value& json, ukv_str_view_t field) noexcept {
    return !field ? json : field[0] == '/' ? json.at_pointer(field) : json[field];
}

yyjson_val* json_lookup(yyjson_val* json, ukv_str_view_t field) noexcept {
    return !field ? json : field[0] == '/' ? yyjson_get_pointer(json, field) : yyjson_obj_get(json, field);
}

yyjson_val* json_lookupn(yyjson_val* json, ukv_str_view_t field, size_t len) noexcept {
    return !field ? json : field[0] == '/' ? yyjson_get_pointern(json, field, len) : yyjson_obj_getn(json, field, len);
}

yyjson_mut_val* json_lookup(yyjson_mut_val* json, ukv_str_view_t field) noexcept {
    return !field ? json : field[0] == '/' ? yyjson_mut_get_pointer(json, field) : yyjson_mut_obj_get(json, field);
}

yyjson_mut_val* json_lookupn(yyjson_mut_val* json, ukv_str_view_t field, size_t len) noexcept {
    return !field            ? json
           : field[0] == '/' ? yyjson_mut_get_pointern(json, field, len)
                             : yyjson_mut_obj_getn(json, field, len);
}

json_t json_parse(value_view_t bytes, linked_memory_lock_t& arena, ukv_error_t* c_error) noexcept {

    if (bytes.empty())
        return {};

    json_t result;
    yyjson_alc allocator = wrap_allocator(arena);
    yyjson_read_flag flg = YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_INF_AND_NAN;
    result.handle = yyjson_read_opts((char*)bytes.data(), (size_t)bytes.size(), flg, &allocator, NULL);
    log_error_if_m(result.handle, c_error, 0, "Failed to parse document!");
    result.mut_handle = yyjson_doc_mut_copy(result.handle, &allocator);
    return result;
}

value_view_t json_dump(json_branch_t json,
                       linked_memory_lock_t& arena,
                       growing_tape_t& output,
                       ukv_error_t* c_error) noexcept {

    if (!json)
        return output.push_back(value_view_t {}, c_error);

    size_t result_length = 0;
    yyjson_write_flag flg = 0;
    yyjson_alc allocator = wrap_allocator(arena);
    char* result_begin = json.mut_handle
                             ? yyjson_mut_val_write_opts(json.mut_handle, flg, &allocator, &result_length, NULL)
                             : yyjson_val_write_opts(json.handle, flg, &allocator, &result_length, NULL);
    log_error_if_m(result_begin, c_error, 0, "Failed to serialize the document!");
    auto result = value_view_t {reinterpret_cast<byte_t const*>(result_begin), result_length};
    result = output.push_back(result, c_error);
    output.add_terminator(byte_t {0}, c_error);
    return result;
}

template <typename scalar_at>
void json_to_scalar(yyjson_val* value,
                    ukv_octet_t mask,
                    ukv_octet_t& valid,
                    ukv_octet_t& convert,
                    ukv_octet_t& collide,
                    scalar_at& scalar) noexcept {

    yyjson_type const type = yyjson_get_type(value);
    yyjson_subtype const subtype = yyjson_get_subtype(value);

    switch (type) {
    case YYJSON_TYPE_NULL:
        convert &= ~mask;
        collide &= ~mask;
        valid &= ~mask;
        break;
    case YYJSON_TYPE_NONE:
    case YYJSON_TYPE_OBJ:
    case YYJSON_TYPE_ARR:
        convert &= ~mask;
        collide |= mask;
        valid &= ~mask;
        break;

    case YYJSON_TYPE_BOOL:
        scalar = yyjson_is_true(value);
        if constexpr (std::is_same_v<scalar_at, bool>)
            convert &= ~mask;
        else
            convert |= mask;
        collide &= ~mask;
        valid |= mask;
        break;

    case YYJSON_TYPE_STR: {
        char const* str_begin = yyjson_get_str(value);
        size_t str_len = yyjson_get_len(value);
        if (parse_entire_number(str_begin, str_begin + str_len, scalar)) {
            convert |= mask;
            collide &= ~mask;
            valid |= mask;
        }
        else {
            convert &= ~mask;
            collide |= mask;
            valid &= ~mask;
        }
        break;
    }

    case YYJSON_TYPE_NUM: {

        switch (subtype) {
        case YYJSON_SUBTYPE_UINT:
            scalar = static_cast<scalar_at>(yyjson_get_uint(value));
            if constexpr (std::is_unsigned_v<scalar_at>)
                convert &= ~mask;
            else
                convert |= mask;
            collide &= ~mask;
            valid |= mask;
            break;

        case YYJSON_SUBTYPE_SINT:
            scalar = static_cast<scalar_at>(yyjson_get_sint(value));
            if constexpr (std::is_integral_v<scalar_at> && std::is_signed_v<scalar_at>)
                convert &= ~mask;
            else
                convert |= mask;
            collide &= ~mask;
            valid |= mask;
            break;

        case YYJSON_SUBTYPE_REAL:
            scalar = static_cast<scalar_at>(yyjson_get_real(value));
            if constexpr (std::is_floating_point_v<scalar_at>)
                convert &= ~mask;
            else
                convert |= mask;
            collide &= ~mask;
            valid |= mask;
            break;
        }
    }
    }
}

std::string_view json_to_string(yyjson_val* value,
                                ukv_octet_t mask,
                                ukv_octet_t& valid,
                                ukv_octet_t& convert,
                                ukv_octet_t& collide,
                                printed_number_buffer_t& print_buffer) noexcept {

    yyjson_type const type = yyjson_get_type(value);
    yyjson_subtype const subtype = yyjson_get_subtype(value);
    std::string_view result;

    switch (type) {
    case YYJSON_TYPE_NULL:
        convert &= ~mask;
        collide &= ~mask;
        valid &= ~mask;
        break;
    case YYJSON_TYPE_NONE:
    case YYJSON_TYPE_OBJ:
    case YYJSON_TYPE_ARR:
        convert &= ~mask;
        collide |= mask;
        valid &= ~mask;
        break;

    case YYJSON_TYPE_BOOL: {
        result = yyjson_is_true(value) ? std::string_view(true_k, 5) : std::string_view(false_k, 6);
        convert |= mask;
        collide &= ~mask;
        valid |= mask;
        break;
    }

    case YYJSON_TYPE_STR: {
        char const* str_begin = yyjson_get_str(value);
        size_t str_len = yyjson_get_len(value);

        result = std::string_view(str_begin, str_len);
        convert &= ~mask;
        collide &= ~mask;
        valid |= mask;
        break;
    }

    case YYJSON_TYPE_NUM: {

        switch (subtype) {
        case YYJSON_SUBTYPE_UINT:
            result = print_number(print_buffer, print_buffer + printed_number_length_limit_k, yyjson_get_uint(value));
            convert |= mask;
            collide = !result.empty() ? (collide & ~mask) : (collide | mask);
            valid = result.empty() ? (valid & ~mask) : (valid | mask);
            break;

        case YYJSON_SUBTYPE_SINT:
            result = print_number(print_buffer, print_buffer + printed_number_length_limit_k, yyjson_get_sint(value));
            convert |= mask;
            collide = !result.empty() ? (collide & ~mask) : (collide | mask);
            valid = result.empty() ? (valid & ~mask) : (valid | mask);
            break;

        case YYJSON_SUBTYPE_REAL:
            result = print_number(print_buffer, print_buffer + printed_number_length_limit_k, yyjson_get_real(value));
            convert |= mask;
            collide = !result.empty() ? (collide & ~mask) : (collide | mask);
            valid = result.empty() ? (valid & ~mask) : (valid | mask);
            break;
        default: break;
        }
    }
    default: break;
    }

    return result;
}

template <typename value_at>
std::string_view get_value( //
    value_at& value,
    ukv_doc_field_type_t field_type,
    printed_number_buffer_t& print_buffer) noexcept {

    std::string_view result;

    if (field_type == ukv_doc_field_json_k) {
        if (value.type().value() == sj::ondemand::json_type::object)
            result = value.get_object().value().raw_json();
        else if (value.type().value() == sj::ondemand::json_type::array)
            result = value.get_array().value().raw_json();
        else
            result = value.raw_json_token();
    }
    else if (field_type == ukv_doc_field_str_k) {
        auto type = value.type().value();
        switch (type) {
        case sj::ondemand::json_type::null: break;
        case sj::ondemand::json_type::object: {
            result = value.get_object().value().raw_json();
            break;
        }
        case sj::ondemand::json_type::array: {
            result = value.get_object().value().raw_json();
            break;
        }
        case sj::ondemand::json_type::boolean: {
            result = value.get_bool() ? std::string_view(true_k, 5) : std::string_view(false_k, 6);
            break;
        }
        case sj::ondemand::json_type::string: {
            result = value.get_string();
            break;
        }
        case sj::ondemand::json_type::number: {
            auto number_type = value.get_number_type().value();
            switch (number_type) {
            case sj::ondemand::number_type::signed_integer: {
                result =
                    print_number(print_buffer, print_buffer + printed_number_length_limit_k, value.get_int64().value());
                break;
            }
            case sj::ondemand::number_type::unsigned_integer: {
                result = print_number(print_buffer,
                                      print_buffer + printed_number_length_limit_k,
                                      value.get_uint64().value());
                break;
            }
            default: break;
            }
        }
        default: break;
        }
    }

    return result;
}

/*********************************************************/
/*****************	 Format Conversions	  ****************/
/*********************************************************/

using string_t = uninitialized_array_gt<char>;
struct json_state_t {
    string_t& json_str;
    ukv_error_t* c_error;

    uint32_t count;
    bool keys;
    ssize_t error_offset;
};

template <std::size_t count_ak>
void to_json_string(string_t& json_str, char (&str)[count_ak], ukv_error_t* c_error) {
    json_str.insert(json_str.size(), str, str + count_ak, c_error);
}

void to_json_string(string_t& json_str, char const* str, size_t count, ukv_error_t* c_error) {
    json_str.insert(json_str.size(), str, str + count, c_error);
}

void to_json_string(string_t& json_str, char const* str, ukv_error_t* c_error) {
    json_str.insert(json_str.size(), str, str + std::strlen(str), c_error);
}

template <typename at>
void to_json_number(string_t& json_str, at scalar, ukv_error_t* c_error) {
    printed_number_buffer_t print_buffer;
    auto result = print_number(print_buffer, print_buffer + printed_number_length_limit_k, scalar);
    json_str.insert(json_str.size(), result.data(), result.data() + result.size(), c_error);
}

static bool bson_visit_array(bson_iter_t const*, char const*, bson_t const*, void*);
static bool bson_visit_document(bson_iter_t const*, char const*, bson_t const*, void*);

static bool bson_visit_before(bson_iter_t const*, char const* key, void* data) {
    json_state_t& state = *reinterpret_cast<json_state_t*>(data);

    if (state.count)
        to_json_string(state.json_str, ", ", state.c_error);

    if (state.keys) {
        char* escaped = bson_utf8_escape_for_json(key, -1);
        if (escaped) {
            to_json_string(state.json_str, "\"", state.c_error);
            to_json_string(state.json_str, escaped, state.c_error);
            to_json_string(state.json_str, "\" : ", state.c_error);

            bson_free(escaped);
        }
        else
            return true;
    }

    state.count++;
    return false;
}
static bool bson_visit_after(bson_iter_t const*, char const*, void*) {
    return false;
}
static void bson_visit_corrupt(bson_iter_t const* iter, void* data) {
    json_state_t& state = *reinterpret_cast<json_state_t*>(data);
    state.error_offset = iter->off;
}
static bool bson_visit_double(bson_iter_t const*, char const*, double v_double, void* data) {
    json_state_t& state = *reinterpret_cast<json_state_t*>(data);
    if (v_double != v_double)
        to_json_string(state.json_str, "NaN", state.c_error);
    else if (v_double * 0 != 0)
        to_json_string(state.json_str, v_double > 0 ? "Infinity" : "-Infinity", state.c_error);
    else
        to_json_number(state.json_str, v_double, state.c_error);

    to_json_string(state.json_str, "\" }", state.c_error);

    return false;
}
static bool bson_visit_utf8(bson_iter_t const*, char const*, size_t v_utf8_len, char const* v_utf8, void* data) {
    json_state_t& state = *reinterpret_cast<json_state_t*>(data);
    char* escaped = bson_utf8_escape_for_json(v_utf8, v_utf8_len); // TODO

    if (escaped) {
        to_json_string(state.json_str, "\"", state.c_error);
        to_json_string(state.json_str, escaped, state.c_error);
        to_json_string(state.json_str, "\"", state.c_error);
        bson_free(escaped);
        return false;
    }

    return true;
}
static bool bson_visit_binary(bson_iter_t const*,
                              char const*,
                              bson_subtype_t v_subtype,
                              size_t v_binary_len,
                              uint8_t const* v_binary,
                              void* data) {
    json_state_t& state = *reinterpret_cast<json_state_t*>(data);
    char* b64 = (char*)(v_binary);

    to_json_string(state.json_str, "{ \"$binary\" : { \"base64\" : \"", state.c_error);
    to_json_string(state.json_str, b64, state.c_error);
    to_json_string(state.json_str, "\", \"subType\" : \"", state.c_error);
    to_json_number(state.json_str, static_cast<int>(v_subtype), state.c_error);
    to_json_string(state.json_str, "\" } }", state.c_error);

    return false;
}
static bool bson_visit_undefined(bson_iter_t const*, char const*, void* data) {
    json_state_t& state = *reinterpret_cast<json_state_t*>(data);
    to_json_string(state.json_str, "{ \"$undefined\" : true }", state.c_error);
    return false;
}
static bool bson_visit_oid(bson_iter_t const*, char const*, const bson_oid_t*, void* data) {
    json_state_t& state = *reinterpret_cast<json_state_t*>(data);
    log_error_m(state.c_error, 0, "Unsupported type");
    return false;
}
static bool bson_visit_bool(bson_iter_t const*, char const*, bool v_bool, void* data) {
    json_state_t& state = *reinterpret_cast<json_state_t*>(data);
    to_json_string(state.json_str, v_bool ? true_k : false_k, state.c_error);
    return false;
}
static bool bson_visit_date_time(bson_iter_t const*, char const*, int64_t msec_since_epoch, void* data) {
    json_state_t& state = *reinterpret_cast<json_state_t*>(data);
    to_json_string(state.json_str, "{ \"$date\" : { \"$numberLong\" : \"", state.c_error);
    to_json_number(state.json_str, msec_since_epoch, state.c_error);
    to_json_string(state.json_str, "\" } }", state.c_error);
    return false;
}
static bool bson_visit_null(bson_iter_t const*, char const*, void* data) {
    json_state_t& state = *reinterpret_cast<json_state_t*>(data);
    to_json_string(state.json_str, null_k, state.c_error);
    return false;
}
static bool bson_visit_regex(bson_iter_t const*, char const*, char const*, char const*, void* data) {
    json_state_t& state = *reinterpret_cast<json_state_t*>(data);
    log_error_m(state.c_error, 0, "Unsupported type");
    return false;
}
static bool bson_visit_dbpointer(bson_iter_t const*, char const*, size_t, char const*, bson_oid_t const*, void* data) {
    json_state_t& state = *reinterpret_cast<json_state_t*>(data);
    log_error_m(state.c_error, 0, "Unsupported type");
    return false;
}
static bool bson_visit_code(bson_iter_t const*, char const*, size_t, char const*, void* data) {
    json_state_t& state = *reinterpret_cast<json_state_t*>(data);
    log_error_m(state.c_error, 0, "Unsupported type");
    return false;
}
static bool bson_visit_symbol(bson_iter_t const*, char const*, size_t, char const*, void* data) {
    json_state_t& state = *reinterpret_cast<json_state_t*>(data);
    log_error_m(state.c_error, 0, "Unsupported type");
    return false;
}
static bool bson_visit_codewscope(bson_iter_t const*, char const*, size_t, char const*, bson_t const*, void* data) {
    json_state_t* state = reinterpret_cast<json_state_t*>(data);
    log_error_m(state->c_error, 0, "Unsupported type");
    return false;
}
static bool bson_visit_int32(bson_iter_t const*, char const*, int32_t v_int32, void* data) {
    json_state_t& state = *reinterpret_cast<json_state_t*>(data);
    to_json_number(state.json_str, v_int32, state.c_error);
    return false;
}
static bool bson_visit_timestamp(
    bson_iter_t const*, char const*, uint32_t v_timestamp, uint32_t v_increment, void* data) {
    json_state_t& state = *reinterpret_cast<json_state_t*>(data);

    to_json_string(state.json_str, "{ \"$timestamp\" : { \"t\" : ", state.c_error);
    to_json_number(state.json_str, v_timestamp, state.c_error);
    to_json_string(state.json_str, ", \"i\" : ", state.c_error);
    to_json_number(state.json_str, v_increment, state.c_error);
    to_json_string(state.json_str, " } }", state.c_error);

    return false;
}
static bool bson_visit_int64(bson_iter_t const*, char const*, int64_t v_int64, void* data) {
    json_state_t& state = *reinterpret_cast<json_state_t*>(data);
    to_json_number(state.json_str, v_int64, state.c_error);
    return false;
}
static bool bson_visit_maxkey(bson_iter_t const*, char const*, void* data) {
    json_state_t& state = *reinterpret_cast<json_state_t*>(data);
    to_json_string(state.json_str, "{ \"$maxKey\" : 1 }", state.c_error);
    return false;
}
static bool bson_visit_minkey(bson_iter_t const*, char const*, void* data) {
    json_state_t& state = *reinterpret_cast<json_state_t*>(data);
    to_json_string(state.json_str, "{ \"$minKey\" : 1 }", state.c_error);
    return false;
}
static void bson_visit_unsupported_type(bson_iter_t const* iter, char const*, uint32_t, void* data) {
    json_state_t& state = *reinterpret_cast<json_state_t*>(data);
    // state.error_offset = iter->off; //TODO
    return_error_m(state.c_error, "BSON unsupported type");
}
static bool bson_visit_decimal128(bson_iter_t const*, char const*, bson_decimal128_t const*, void* data) {
    json_state_t& state = *reinterpret_cast<json_state_t*>(data);
    log_error_m(state.c_error, 0, "Unsupported type");
    return false;
}

static bson_visitor_t const bson_visitor = {
    bson_visit_before,   bson_visit_after,     bson_visit_corrupt,    bson_visit_double,    bson_visit_utf8,
    bson_visit_document, bson_visit_array,     bson_visit_binary,     bson_visit_undefined, bson_visit_oid,
    bson_visit_bool,     bson_visit_date_time, bson_visit_null,       bson_visit_regex,     bson_visit_dbpointer,
    bson_visit_code,     bson_visit_symbol,    bson_visit_codewscope, bson_visit_int32,     bson_visit_timestamp,
    bson_visit_int64,    bson_visit_maxkey,    bson_visit_minkey,
};

static bool bson_visit_array(bson_iter_t const*, char const*, bson_t const* v_array, void* data) {
    json_state_t& state = *reinterpret_cast<json_state_t*>(data);
    json_state_t child_state = {state.json_str, state.c_error, 0, true, state.error_offset};
    bson_iter_t child;

    if (bson_iter_init(&child, v_array)) {
        to_json_string(child_state.json_str, open_arr_k, state.c_error);

        if (bson_iter_visit_all(&child, &bson_visitor, &child_state))
            log_error_m(state.c_error, 0, "Failed to iterate the BSON array!");

        to_json_string(child_state.json_str, close_arr_k, state.c_error);
    }
    return false;
}
static bool bson_visit_document(bson_iter_t const*, char const*, bson_t const* v_document, void* data) {
    json_state_t& state = *reinterpret_cast<json_state_t*>(data);
    json_state_t child_state = {state.json_str, state.c_error, 0, true, state.error_offset};
    bson_iter_t child;

    if (bson_iter_init(&child, v_document)) {
        to_json_string(child_state.json_str, open_k, state.c_error);

        if (bson_iter_visit_all(&child, &bson_visitor, &child_state))
            log_error_m(state.c_error, 0, "Failed to iterate the BSON document!");

        to_json_string(child_state.json_str, close_k, state.c_error);
    }
    return false;
}

// MsgPack to Json
void object_reading(mpack_reader_t& reader, string_t& builder, ukv_error_t* c_error) {
    if (mpack_reader_error(&reader) != mpack_ok) // C++20: [[unlikely]]
        return;

    mpack_tag_t tag = mpack_read_tag(&reader);

    switch (mpack_tag_type(&tag)) {
    case mpack_type_nil: {
        to_json_string(builder, null_k, c_error);
        break;
    }
    case mpack_type_bool: {
        bool v_bool = mpack_tag_bool_value(&tag);
        to_json_string(builder, v_bool ? true_k : false_k, c_error);
        break;
    }
    case mpack_type_int: {
        to_json_number(builder, mpack_tag_int_value(&tag), c_error);
        break;
    }
    case mpack_type_uint: {
        to_json_number(builder, mpack_tag_uint_value(&tag), c_error);
        break;
    }
    case mpack_type_float: {
        to_json_number(builder, mpack_tag_float_value(&tag), c_error);
        break;
    }
    case mpack_type_double: {
        to_json_number(builder, mpack_tag_double_value(&tag), c_error);
        break;
    }
    case mpack_type_str: {
        size_t length = mpack_tag_str_length(&tag);
        auto data = mpack_read_bytes_inplace(&reader, length);
        to_json_string(builder, "\"", c_error);
        to_json_string(builder, data, c_error);
        to_json_string(builder, "\"", c_error);
        mpack_done_str(&reader);
        break;
    }
    case mpack_type_bin: {
        size_t length = mpack_tag_bin_length(&tag);
        auto data = mpack_read_bytes_inplace(&reader, length);
        to_json_string(builder, data, c_error);
        break;
    }
    case mpack_type_array: {
        to_json_string(builder, open_arr_k, c_error);

        uint32_t count = mpack_tag_array_count(&tag);
        // Recursively call for kids.
        for (uint32_t i = 0; i != count; ++i) {
            object_reading(reader, builder, c_error);

            if (mpack_reader_error(&reader) != mpack_ok) // critical check!
                break;
        }
        mpack_done_array(&reader);

        to_json_string(builder, close_arr_k, c_error);
        break;
    }
    case mpack_type_map: {
        to_json_string(builder, open_k, c_error);

        uint32_t count = mpack_tag_map_count(&tag);
        for (uint32_t i = 0; i != count; ++i) {
            if (i != 0)
                to_json_string(builder, ",", c_error);

            mpack_tag_t tag = mpack_read_tag(&reader);
            uint32_t length = mpack_tag_str_length(&tag);
            const char* key = mpack_read_bytes_inplace(&reader, length);
            // Write key into json string
            to_json_string(builder, "\"", c_error);
            to_json_string(builder, key, length, c_error);
            to_json_string(builder, "\":", c_error);

            object_reading(reader, builder, c_error);
            if (mpack_reader_error(&reader) != mpack_ok) // critical check!
                break;
        }
        mpack_done_map(&reader);

        to_json_string(builder, close_k, c_error);
        break;
    }
    default: mpack_reader_flag_error(&reader, mpack_error_unsupported); break;
    }
}
bool iterate_over_mpack_data(value_view_t data, string_t& json_str, ukv_error_t* c_error) {
    mpack_reader_t reader;
    mpack_reader_init_data(&reader, data.c_str(), data.size());

    // Export all the content without any allocations
    while (reader.data != reader.end)
        object_reading(reader, json_str, c_error);

    // Cleanup the state
    mpack_reader_destroy(&reader);

    return true;
}

// Json to MsgPack
void sample_leafs(mpack_writer_t& writer, sj::simdjson_result<sj::ondemand::value> value) {
    auto type = value.type().value();
    switch (type) {
    case sj::ondemand::json_type::object: {
        mpack_build_map(&writer);
        auto object = value.get_object().value();
        auto begin = object.begin().value();
        auto end = object.end().value();
        while (begin != end) {
            auto field = *begin;
            std::string_view key = field.unescaped_key();
            mpack_write_str(&writer, key.data(), key.size());
            sample_leafs(writer, field.value().value());
            ++begin;
        }
        mpack_complete_map(&writer);
        break;
    }
    case sj::ondemand::json_type::array: {
        mpack_build_array(&writer);
        auto array = value.get_array();
        auto begin = array.begin().value();
        auto end = array.end().value();
        while (begin != end) {
            auto value = *begin;
            sample_leafs(writer, value);
            ++begin;
        }
        mpack_complete_array(&writer);
        break;
    }
    case sj::ondemand::json_type::null: {
        mpack_write_nil(&writer);
        break;
    }
    case sj::ondemand::json_type::boolean: {
        mpack_write_bool(&writer, value.get_bool().value());
        break;
    }
    case sj::ondemand::json_type::string: {
        mpack_write_str(&writer, value.get_string().value().data(), value.get_string().value().size());
        break;
    }
    case sj::ondemand::json_type::number: {
        auto number_type = value.get_number_type().value();
        switch (number_type) {
        case sj::ondemand::number_type::floating_point_number: {
            mpack_write_double(&writer, value.get_double().value());
            break;
        }
        case sj::ondemand::number_type::signed_integer: {
            mpack_write_i64(&writer, value.get_int64().value());
            break;
        }
        case sj::ondemand::number_type::unsigned_integer: {
            mpack_write_u64(&writer, value.get_uint64().value());
            break;
        }
        }
        break;
    }
    default: break;
    }
}

void json_to_mpack(sj::padded_string_view doc, string_t& output, ukv_error_t* c_error) {
    sj::ondemand::parser parser;

    output.resize(doc.size(), c_error);
    return_if_error_m(c_error);

    mpack_writer_t writer;
    mpack_writer_init(&writer, output.data(), doc.size());

    auto result = parser.iterate(doc);
    mpack_build_map(&writer);
    auto object = result.get_object().value();
    auto begin = object.begin().value();
    auto end = object.end().value();
    while (begin != end) {
        auto field = *begin;
        std::string_view key = field.unescaped_key();
        mpack_write_str(&writer, key.data(), key.size());
        sample_leafs(writer, field.value().value());
        ++begin;
    }
    mpack_complete_map(&writer);

    auto end_ptr = writer.position;
    size_t new_size = end_ptr - writer.buffer;

    output.resize(new_size, c_error);
    mpack_writer_destroy(&writer);
    return_if_error_m(c_error);
}

json_t any_parse(value_view_t bytes,
                 ukv_doc_field_type_t const field_type,
                 linked_memory_lock_t& arena,
                 ukv_error_t* c_error) noexcept {

    if (field_type == ukv_doc_field_bson_k) {
        bson_t bson;
        bool success = bson_init_static(&bson, reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size());
        // Using `bson_as_canonical_extended_json` is a bad idea, as it allocates dynamically.
        // Instead we will manually iterate over the document, using the "visitor" pattern
        bson_iter_t iter;
        string_t json(arena);
        json_state_t state {json, c_error, 0, true, -1};

        if (!bson_iter_init(&iter, &bson)) {
            *c_error = "Failed to parse the BSON document!";
            return {};
        }

        to_json_string(json, open_k, c_error);
        if (bson_iter_visit_all(&iter, &bson_visitor, &state) || state.error_offset != -1) {
            *c_error = "Failed to iterate the BSON document!";
            return {};
        }
        to_json_string(json, close_k, c_error);

        return json_parse({json.data(), json.size()}, arena, c_error);
    }

    if (field_type == ukv_doc_field_msgpack_k) {
        string_t json(arena);
        if (!iterate_over_mpack_data(bytes, json, c_error)) {
            *c_error = "Failed to parse the MsgPack document!";
            return {};
        }
        return json_parse({json.data(), json.size()}, arena, c_error);
    }

    if (field_type == ukv_doc_field_json_k)
        return json_parse(bytes, arena, c_error);

    // Wrapping binary data into a JSON object
    yyjson_alc allocator = wrap_allocator(arena);
    yyjson_mut_doc* doc = yyjson_mut_doc_new(&allocator);
    yyjson_mut_val* root = nullptr;
    switch (field_type) {
    case ukv_doc_field_null_k:
    case ukv_doc_field_uuid_k:
    case ukv_doc_field_f16_k:
    case ukv_doc_field_bin_k: *c_error = "Input type not supported";
    case ukv_doc_field_str_k: root = yyjson_mut_strn(doc, bytes.c_str(), bytes.size()); break;
    case ukv_doc_field_u8_k: root = yyjson_mut_uint(doc, *reinterpret_cast<uint8_t const*>(bytes.data())); break;
    case ukv_doc_field_u16_k: root = yyjson_mut_uint(doc, *reinterpret_cast<uint16_t const*>(bytes.data())); break;
    case ukv_doc_field_u32_k: root = yyjson_mut_uint(doc, *reinterpret_cast<uint32_t const*>(bytes.data())); break;
    case ukv_doc_field_u64_k: root = yyjson_mut_uint(doc, *reinterpret_cast<uint64_t const*>(bytes.data())); break;
    case ukv_doc_field_i8_k: root = yyjson_mut_sint(doc, *reinterpret_cast<int8_t const*>(bytes.data())); break;
    case ukv_doc_field_i16_k: root = yyjson_mut_sint(doc, *reinterpret_cast<int16_t const*>(bytes.data())); break;
    case ukv_doc_field_i32_k: root = yyjson_mut_sint(doc, *reinterpret_cast<int32_t const*>(bytes.data())); break;
    case ukv_doc_field_i64_k: root = yyjson_mut_sint(doc, *reinterpret_cast<int64_t const*>(bytes.data())); break;
    case ukv_doc_field_f32_k: root = yyjson_mut_real(doc, *reinterpret_cast<float const*>(bytes.data())); break;
    case ukv_doc_field_f64_k: root = yyjson_mut_real(doc, *reinterpret_cast<double const*>(bytes.data())); break;
    case ukv_doc_field_bool_k: root = yyjson_mut_bool(doc, *reinterpret_cast<bool const*>(bytes.data())); break;

    case ukv_doc_field_bson_k:
    case ukv_doc_field_json_k:
    case ukv_doc_field_msgpack_k: break;
    }
    yyjson_mut_doc_set_root(doc, root);
    json_t result;
    result.mut_handle = doc;
    return result;
}

value_view_t any_dump(json_branch_t json,
                      ukv_doc_field_type_t const field_type,
                      linked_memory_lock_t& arena,
                      growing_tape_t& output,
                      ukv_error_t* c_error) noexcept {

    if (field_type == ukv_doc_field_str_k) {
        ukv_octet_t dummy;
        printed_number_buffer_t print_buffer;
        auto str = json_to_string(json.punned(), 0, dummy, dummy, dummy, print_buffer);
        auto result = output.push_back(str, c_error);
        output.add_terminator(byte_t {0}, c_error);
        return result;
    }

    else if (field_type == ukv_doc_field_json_k)
        return json_dump(json, arena, output, c_error);

    *c_error = "Output type not supported!";
    return {};
}

/*********************************************************/
/*****************	 Primary Functions	  ****************/
/*********************************************************/

void modify_field( //
    yyjson_mut_doc* original_doc,
    yyjson_mut_val* modifier,
    ukv_str_view_t field,
    doc_modification_t const c_modification,
    ukv_error_t* c_error) {

    std::string_view json_ptr(field);
    auto last_key_pos = json_ptr.rfind('/');
    auto last_key_or_idx = json_ptr.substr(last_key_pos + 1);
    auto is_idx = std::all_of(last_key_or_idx.begin(), last_key_or_idx.end(), [](char c) { return std::isdigit(c); });

    yyjson_mut_val* val = json_lookupn(original_doc->root, json_ptr.data(), last_key_pos);
    return_error_if_m(val, c_error, 0, "Invalid field!");

    if (yyjson_mut_is_arr(val)) {
        return_error_if_m(is_idx || last_key_or_idx == "-", c_error, 0, "Invalid field!");
        size_t idx = 0;
        std::from_chars(last_key_or_idx.begin(), last_key_or_idx.end(), idx);
        if (c_modification == doc_modification_t::merge_k) {
            yyjson_mut_val* mergeable = yyjson_mut_arr_get(val, idx);
            return_error_if_m(mergeable, c_error, 0, "Invalid field!");
            yyjson_mut_val* merge_result = yyjson_mut_merge_patch(original_doc, mergeable, modifier);
            return_error_if_m(merge_result, c_error, 0, "Failed To Merge!");
            return_error_if_m(yyjson_mut_arr_replace(val, idx, merge_result), c_error, 0, "Failed To Merge!");
        }
        else if (c_modification == doc_modification_t::insert_k) {
            auto result = is_idx //
                              ? yyjson_mut_arr_insert(val, modifier, idx)
                              : yyjson_mut_arr_add_val(val, modifier);
            return_error_if_m(result, c_error, 0, "Failed To Insert!");
        }
        else if (c_modification == doc_modification_t::remove_k) {
            return_error_if_m(yyjson_mut_arr_remove(val, idx), c_error, 0, "Failed To Remove!");
        }
        else if (c_modification == doc_modification_t::update_k) {
            return_error_if_m(yyjson_mut_arr_replace(val, idx, modifier), c_error, 0, "Failed To Update!");
        }
        else if (c_modification == doc_modification_t::upsert_k) {
            auto result = yyjson_mut_arr_get(val, idx) //
                              ? yyjson_mut_arr_replace(val, idx, modifier) != nullptr
                              : yyjson_mut_arr_append(val, modifier);
            return_error_if_m(result, c_error, 0, "Failed To Upsert!");
        }
        else {
            return_error_m(c_error, "Invalid Modification Mode!");
        }
    }
    else if (yyjson_mut_is_obj(val)) {
        if (c_modification == doc_modification_t::merge_k) {
            yyjson_mut_val* mergeable = yyjson_mut_obj_getn(val, last_key_or_idx.data(), last_key_or_idx.size());
            yyjson_mut_val* merge_result = yyjson_mut_merge_patch(original_doc, mergeable, modifier);
            yyjson_mut_val* key = yyjson_mut_strncpy(original_doc, last_key_or_idx.data(), last_key_or_idx.size());
            yyjson_mut_obj_replace(val, key, merge_result);
        }
        else if (c_modification == doc_modification_t::insert_k) {
            yyjson_mut_val* key = yyjson_mut_strncpy(original_doc, last_key_or_idx.data(), last_key_or_idx.size());
            return_error_if_m(yyjson_mut_obj_add(val, key, modifier), c_error, 0, "Failed To Insert!");
        }
        else if (c_modification == doc_modification_t::remove_k) {
            yyjson_mut_val* key = yyjson_mut_strncpy(original_doc, last_key_or_idx.data(), last_key_or_idx.size());
            return_error_if_m(yyjson_mut_obj_remove(val, key), c_error, 0, "Failed To Insert!");
        }
        else if (c_modification == doc_modification_t::update_k) {
            yyjson_mut_val* key = yyjson_mut_strncpy(original_doc, last_key_or_idx.data(), last_key_or_idx.size());
            return_error_if_m(yyjson_mut_obj_replace(val, key, modifier), c_error, 0, "Failed To Update!");
        }
        else if (c_modification == doc_modification_t::upsert_k) {
            if (yyjson_mut_obj_get(val, last_key_or_idx.data())) {
                yyjson_mut_val* key = yyjson_mut_strncpy(original_doc, last_key_or_idx.data(), last_key_or_idx.size());
                return_error_if_m(yyjson_mut_obj_replace(val, key, modifier), c_error, 0, "Failed To Update!");
            }
            else {
                yyjson_mut_val* key = yyjson_mut_strncpy(original_doc, last_key_or_idx.data(), last_key_or_idx.size());
                return_error_if_m(yyjson_mut_obj_add(val, key, modifier), c_error, 0, "Failed To Update!");
            }
        }
        else {
            return_error_m(c_error, "Invalid Modification Mode!");
        }
    }
}

ukv_str_view_t field_concat(ukv_str_view_t field,
                            ukv_str_view_t suffix,
                            linked_memory_lock_t& arena,
                            ukv_error_t* c_error) {
    auto field_len = field ? std::strlen(field) : 0;
    auto suffix_len = suffix ? std::strlen(suffix) : 0;

    if (!(field_len | suffix_len))
        return nullptr;
    if (!field_len)
        return suffix;
    if (!suffix_len)
        return field;

    auto result = arena.alloc<char>(field_len + suffix_len + 1, c_error).begin();
    std::memcpy(result, field, field_len);
    std::memcpy(result + field_len, suffix, suffix_len);
    result[field_len + suffix_len] = '\0';
    return result;
}

void patch( //
    yyjson_mut_doc* original_doc,
    yyjson_mut_val* patch_doc,
    ukv_str_view_t field,
    linked_memory_lock_t& arena,
    ukv_error_t* c_error) {

    return_error_if_m(yyjson_mut_is_arr(patch_doc), c_error, 0, "Invalid Patch Doc!");
    yyjson_mut_val* obj;
    yyjson_mut_arr_iter arr_iter;
    yyjson_mut_arr_iter_init(patch_doc, &arr_iter);
    while ((obj = yyjson_mut_arr_iter_next(&arr_iter))) {
        return_error_if_m(yyjson_mut_is_obj(obj), c_error, 0, "Invalid Patch Doc!");
        yyjson_mut_obj_iter obj_iter;
        yyjson_mut_obj_iter_init(obj, &obj_iter);
        yyjson_mut_val* op = yyjson_mut_obj_iter_get(&obj_iter, "op");
        return_error_if_m(op, c_error, 0, "Invalid Patch Doc!");
        if (yyjson_mut_equals_str(op, "add")) {
            return_error_if_m((yyjson_mut_obj_size(obj) == 3), c_error, 0, "Invalid Patch Doc!");
            yyjson_mut_val* path = yyjson_mut_obj_iter_get(&obj_iter, "path");
            return_error_if_m(path, c_error, 0, "Invalid Patch Doc!");
            yyjson_mut_val* value = yyjson_mut_obj_iter_get(&obj_iter, "value");
            return_error_if_m(value, c_error, 0, "Invalid Patch Doc!");
            auto nested_path = field_concat(field, yyjson_mut_get_str(path), arena, c_error);
            return_if_error_m(c_error);
            nested_path ? modify_field(original_doc, value, nested_path, doc_modification_t::insert_k, c_error)
                        : yyjson_mut_doc_set_root(original_doc, value);
        }
        else if (yyjson_mut_equals_str(op, "remove")) {
            return_error_if_m((yyjson_mut_obj_size(obj) == 2), c_error, 0, "Invalid Patch Doc!");
            yyjson_mut_val* path = yyjson_mut_obj_iter_get(&obj_iter, "path");
            return_error_if_m(path, c_error, 0, "Invalid Patch Doc!");
            auto nested_path = field_concat(field, yyjson_mut_get_str(path), arena, c_error);
            return_if_error_m(c_error);
            nested_path ? modify_field(original_doc, nullptr, nested_path, doc_modification_t::remove_k, c_error)
                        : yyjson_mut_doc_set_root(original_doc, NULL);
        }
        else if (yyjson_mut_equals_str(op, "replace")) {
            return_error_if_m((yyjson_mut_obj_size(obj) == 3), c_error, 0, "Invalid Patch Doc!");
            yyjson_mut_val* path = yyjson_mut_obj_iter_get(&obj_iter, "path");
            return_error_if_m(path, c_error, 0, "Invalid Patch Doc!");
            yyjson_mut_val* value = yyjson_mut_obj_iter_get(&obj_iter, "value");
            return_error_if_m(value, c_error, 0, "Invalid Patch Doc!");
            auto nested_path = field_concat(field, yyjson_mut_get_str(path), arena, c_error);
            return_if_error_m(c_error);
            nested_path ? modify_field(original_doc, value, nested_path, doc_modification_t::update_k, c_error)
                        : yyjson_mut_doc_set_root(original_doc, value);
        }
        else if (yyjson_mut_equals_str(op, "copy")) {
            return_error_if_m((yyjson_mut_obj_size(obj) == 3), c_error, 0, "Invalid Patch Doc!");
            yyjson_mut_val* path = yyjson_mut_obj_iter_get(&obj_iter, "path");
            return_error_if_m(path, c_error, 0, "Invalid Patch Doc!");
            yyjson_mut_val* from = yyjson_mut_obj_iter_get(&obj_iter, "from");
            return_error_if_m(from, c_error, 0, "Invalid Patch Doc!");
            yyjson_mut_val* value =
                yyjson_mut_val_mut_copy(original_doc, json_lookup(original_doc->root, yyjson_mut_get_str(from)));
            return_error_if_m(value, c_error, 0, "Invalid Patch Doc!");
            auto nested_path = field_concat(field, yyjson_mut_get_str(path), arena, c_error);
            return_if_error_m(c_error);
            modify_field(original_doc, value, nested_path, doc_modification_t::upsert_k, c_error);
        }
        else if (yyjson_mut_equals_str(op, "move")) {
            return_error_if_m((yyjson_mut_obj_size(obj) == 3), c_error, 0, "Invalid Patch Doc!");
            yyjson_mut_val* path = yyjson_mut_obj_iter_get(&obj_iter, "path");
            return_error_if_m(path, c_error, 0, "Invalid Patch Doc!");
            yyjson_mut_val* from = yyjson_mut_obj_iter_get(&obj_iter, "from");
            return_error_if_m(from, c_error, 0, "Invalid Patch Doc!");
            yyjson_mut_val* value =
                yyjson_mut_val_mut_copy(original_doc, json_lookup(original_doc->root, yyjson_mut_get_str(from)));
            return_error_if_m(value, c_error, 0, "Invalid Patch Doc!");
            auto nested_from_path = field_concat(field, yyjson_mut_get_str(from), arena, c_error);
            return_if_error_m(c_error);
            modify_field(original_doc, nullptr, nested_from_path, doc_modification_t::remove_k, c_error);
            auto nested_to_path = field_concat(field, yyjson_mut_get_str(path), arena, c_error);
            return_if_error_m(c_error);
            modify_field(original_doc, value, nested_to_path, doc_modification_t::upsert_k, c_error);
        }
    }
}

void modify( //
    json_t& original,
    yyjson_mut_val* modifier,
    ukv_str_view_t field,
    doc_modification_t const c_modification,
    linked_memory_lock_t& arena,
    ukv_error_t* c_error) {

    if (!original.mut_handle) {
        original.mut_handle = yyjson_mut_doc_new(nullptr);
        original.mut_handle->root = yyjson_mut_val_mut_copy(original.mut_handle, modifier);
        return;
    }

    if (field && c_modification != doc_modification_t::patch_k) {
        modify_field(original.mut_handle, modifier, field, c_modification, c_error);
        return_error_if_m(original.mut_handle->root, c_error, 0, "Failed To Modify!");
        return;
    }

    if (c_modification == doc_modification_t::merge_k)
        original.mut_handle->root = yyjson_mut_merge_patch(original.mut_handle, original.mut_handle->root, modifier);

    else if (c_modification == doc_modification_t::patch_k)
        patch(original.mut_handle, modifier, field, arena, c_error);

    else
        original.mut_handle->root = yyjson_mut_val_mut_copy(original.mut_handle, modifier);

    return_error_if_m(original.mut_handle->root, c_error, 0, "Failed To Modify!");
}

template <typename callback_at>
void read_unique_docs( //
    ukv_database_t const c_db,
    ukv_transaction_t const c_txn,
    places_arg_t const& places,
    ukv_options_t const c_options,
    linked_memory_lock_t& arena,
    places_arg_t& unique_places,
    ukv_error_t* c_error,
    callback_at callback) noexcept {

    ukv_byte_t* found_binary_begin = nullptr;
    ukv_length_t* found_binary_offs = nullptr;
    ukv_length_t* found_binary_lens = nullptr;
    ukv_read_t read {
        .db = c_db,
        .error = c_error,
        .transaction = c_txn,
        .arena = arena,
        .options = c_options,
        .tasks_count = places.count,
        .collections = places.collections_begin.get(),
        .collections_stride = places.collections_begin.stride(),
        .keys = places.keys_begin.get(),
        .keys_stride = places.keys_begin.stride(),
        .offsets = &found_binary_offs,
        .lengths = &found_binary_lens,
        .values = &found_binary_begin,
    };

    ukv_read(&read);

    auto found_binaries = joined_blobs_t(places.count, found_binary_offs, found_binary_begin);
    auto found_binary_it = found_binaries.begin();

    ukv_length_t max_length =
        *std::max_element(found_binary_lens, found_binary_lens + places.count, [](ukv_length_t lhs, ukv_length_t rhs) {
            return (lhs < rhs) && (lhs != ukv_length_missing_k) && rhs != ukv_length_missing_k;
        });

    if (max_length == ukv_length_missing_k) {
        for (std::size_t task_idx = 0; task_idx != places.size(); ++task_idx, ++found_binary_it)
            callback(task_idx, {}, value_view_t::make_empty());
        return;
    }

    auto document = arena.alloc<byte_t>(max_length + sj::SIMDJSON_PADDING, c_error);
    return_if_error_m(c_error);

    for (std::size_t task_idx = 0; task_idx != places.size(); ++task_idx, ++found_binary_it) {
        value_view_t binary_doc = *found_binary_it;
        std::memcpy(document.begin(), binary_doc.data(), binary_doc.size());
        std::memset(document.begin() + binary_doc.size(), 0, sj::SIMDJSON_PADDING);
        ukv_str_view_t field = places.fields_begin ? places.fields_begin[task_idx] : nullptr;
        callback(task_idx, field, value_view_t(document.begin(), binary_doc.size()));
    }

    unique_places = places;
}

template <typename callback_at>
void read_modify_unique_docs( //
    ukv_database_t const c_db,
    ukv_transaction_t const c_txn,
    places_arg_t const& places,
    ukv_options_t const c_options,
    doc_modification_t const c_modification,
    linked_memory_lock_t& arena,
    places_arg_t& unique_places,
    ukv_error_t* c_error,
    callback_at callback) noexcept {

    if (c_modification == doc_modification_t::nothing_k)
        return read_unique_docs(c_db, c_txn, places, c_options, arena, unique_places, c_error, callback);

    auto has_fields = places.fields_begin && (!places.fields_begin.repeats() || *places.fields_begin);
    bool need_values =
        has_fields || c_modification == doc_modification_t::patch_k || c_modification == doc_modification_t::merge_k;

    if (need_values) {
        ukv_byte_t* found_binary_begin = nullptr;
        ukv_length_t* found_binary_offs = nullptr;
        ukv_read_t read {
            .db = c_db,
            .error = c_error,
            .transaction = c_txn,
            .arena = arena,
            .options = c_options,
            .tasks_count = places.count,
            .collections = places.collections_begin.get(),
            .collections_stride = places.collections_begin.stride(),
            .keys = places.keys_begin.get(),
            .keys_stride = places.keys_begin.stride(),
            .offsets = &found_binary_offs,
            .values = &found_binary_begin,
        };

        ukv_read(&read);
        return_if_error_m(c_error);

        auto found_binaries = joined_blobs_t(places.count, found_binary_offs, found_binary_begin);
        auto found_binary_it = found_binaries.begin();

        for (std::size_t task_idx = 0; task_idx != places.size(); ++task_idx, ++found_binary_it) {

            return_error_if_m(has_fields || c_modification != doc_modification_t::insert_k,
                              c_error,
                              0,
                              "Key Already Exists!");
            ukv_str_view_t field = places.fields_begin ? places.fields_begin[task_idx] : nullptr;
            value_view_t binary_doc = *found_binary_it;
            callback(task_idx, field, binary_doc);
            return_if_error_m(c_error);
        }
    }
    else {
        ukv_octet_t* found_presences = nullptr;
        ukv_read_t read {
            .db = c_db,
            .error = c_error,
            .transaction = c_txn,
            .arena = arena,
            .options = c_options,
            .tasks_count = places.count,
            .collections = places.collections_begin.get(),
            .collections_stride = places.collections_begin.stride(),
            .keys = places.keys_begin.get(),
            .keys_stride = places.keys_begin.stride(),
            .presences = &found_presences,
        };
        ukv_read(&read);
        return_if_error_m(c_error);

        bits_view_t presents {found_presences};
        for (std::size_t task_idx = 0; task_idx != places.size(); ++task_idx) {
            ukv_str_view_t field = places.fields_begin ? places.fields_begin[task_idx] : nullptr;
            return_error_if_m(presents[task_idx] || !has_fields, c_error, 0, "Key Not Exists!");
            return_error_if_m(!presents[task_idx] || c_modification != doc_modification_t::insert_k,
                              c_error,
                              0,
                              "Invalid Arguments!");
            callback(task_idx, field, value_view_t::make_empty());
            return_if_error_m(c_error);
        }
    }

    unique_places = places;
}

template <typename callback_at>
void read_modify_docs( //
    ukv_database_t const c_db,
    ukv_transaction_t const c_txn,
    places_arg_t const& places,
    ukv_options_t const c_options,
    doc_modification_t const c_modification,
    linked_memory_lock_t& arena,
    places_arg_t& unique_places,
    ukv_error_t* c_error,
    callback_at callback) {

    // Handle the common case of requesting the non-colliding
    // all-ascending input sequences of document IDs received
    // during scans without the sort and extra memory.
    if (all_ascending(places.keys_begin, places.count))
        return read_modify_unique_docs(c_db,
                                       c_txn,
                                       places,
                                       c_options,
                                       c_modification,
                                       arena,
                                       unique_places,
                                       c_error,
                                       callback);

    // If it's not one of the trivial consecutive lookups, we want
    // to sort & deduplicate the entries to minimize the random reads
    // from disk.
    auto unique_col_keys = arena.alloc<collection_key_t>(places.count, c_error);
    return_if_error_m(c_error);

    transform_n(places, places.count, unique_col_keys, std::mem_fn(&place_t::collection_key));
    unique_col_keys = {unique_col_keys.begin(), sort_and_deduplicate(unique_col_keys.begin(), unique_col_keys.end())};

    // There is a chance, all the entries are unique.
    // In such case, let's free-up the memory.
    if (unique_col_keys.size() == places.count)
        return read_modify_unique_docs(c_db,
                                       c_txn,
                                       places,
                                       c_options,
                                       c_modification,
                                       arena,
                                       unique_places,
                                       c_error,
                                       callback);

    // Otherwise, let's retrieve the sublist of unique docs,
    // which may be in a very different order from original.
    ukv_byte_t* found_binary_begin = nullptr;
    ukv_length_t* found_binary_offs = nullptr;
    auto unique_col_keys_strided = strided_range(unique_col_keys.begin(), unique_col_keys.end()).immutable();
    unique_places.collections_begin = unique_col_keys_strided.members(&collection_key_t::collection).begin();
    unique_places.keys_begin = unique_col_keys_strided.members(&collection_key_t::key).begin();
    unique_places.fields_begin = {};
    unique_places.count = static_cast<ukv_size_t>(unique_col_keys.size());
    ukv_read_t read {
        .db = c_db,
        .error = c_error,
        .transaction = c_txn,
        .arena = arena,
        .options = c_options,
        .tasks_count = unique_places.count,
        .collections = unique_places.collections_begin.get(),
        .collections_stride = unique_places.collections_begin.stride(),
        .keys = unique_places.keys_begin.get(),
        .keys_stride = unique_places.keys_begin.stride(),
        .offsets = &found_binary_offs,
        .values = &found_binary_begin,
    };

    ukv_read(&read);
    return_if_error_m(c_error);

    // We will later need to locate the data for every separate request.
    // Doing it in O(N) tape iterations every time is too slow.
    // Once we transform to inclusive sums, it will be O(1).
    //      inplace_inclusive_prefix_sum(found_binary_lens, found_binary_lens + found_binary_count);
    // Alternatively we can compensate it with additional memory:

    // Parse all the unique documents
    auto found_binaries = joined_blobs_t(places.count, found_binary_offs, found_binary_begin);

    // Join docs and fields with binary search
    for (std::size_t task_idx = 0; task_idx != places.size(); ++task_idx) {
        auto place = places[task_idx];
        auto parsed_idx = offset_in_sorted(unique_col_keys, place.collection_key());

        value_view_t binary_doc = found_binaries[parsed_idx];
        callback(task_idx, place.field, binary_doc);
    }
}

void read_modify_write( //
    ukv_database_t const c_db,
    ukv_transaction_t const c_txn,
    places_arg_t const& places,
    contents_arg_t const& contents,
    ukv_options_t const c_options,
    doc_modification_t const c_modification,
    ukv_doc_field_type_t const c_type,
    linked_memory_lock_t& arena,
    ukv_error_t* c_error) noexcept {

    growing_tape_t growing_tape {arena};
    growing_tape.reserve(places.size(), c_error);
    return_if_error_m(c_error);

    yyjson_alc allocator = wrap_allocator(arena);
    auto safe_callback = [&](ukv_size_t task_idx, ukv_str_view_t field, value_view_t binary_doc) {
        json_t parsed = any_parse(binary_doc, internal_format_k, arena, c_error);

        // This error is extremely unlikely, as we have previously accepted the data into the store.
        return_if_error_m(c_error);
        if (!parsed.mut_handle)
            parsed.mut_handle = yyjson_doc_mut_copy(parsed.handle, &allocator);

        json_t parsed_task = any_parse(contents[task_idx], c_type, arena, c_error);
        return_if_error_m(c_error);

        // Perform modifications
        modify(parsed, parsed_task.mut_handle->root, field, c_modification, arena, c_error);
        any_dump({.mut_handle = parsed.mut_handle->root}, internal_format_k, arena, growing_tape, c_error);
        return_if_error_m(c_error);
    };

    places_arg_t unique_places;
    auto opts = c_txn ? ukv_options_t(c_options & ~ukv_option_transaction_dont_watch_k) : c_options;
    read_modify_docs(c_db, c_txn, places, opts, c_modification, arena, unique_places, c_error, safe_callback);
    return_if_error_m(c_error);

    // By now, the tape contains concatenated updates docs:
    ukv_byte_t* tape_begin = reinterpret_cast<ukv_byte_t*>(growing_tape.contents().begin().get());
    ukv_write_t write {
        .db = c_db,
        .error = c_error,
        .transaction = c_txn,
        .arena = arena,
        .options = c_options,
        .tasks_count = unique_places.count,
        .collections = unique_places.collections_begin.get(),
        .collections_stride = unique_places.collections_begin.stride(),
        .keys = unique_places.keys_begin.get(),
        .keys_stride = unique_places.keys_begin.stride(),
        .offsets = growing_tape.offsets().begin().get(),
        .offsets_stride = growing_tape.offsets().stride(),
        .lengths = growing_tape.lengths().begin().get(),
        .lengths_stride = growing_tape.lengths().stride(),
        .values = &tape_begin,
    };

    ukv_write(&write);
}

void ukv_docs_write(ukv_docs_write_t* c_ptr) {

    ukv_docs_write_t& c = *c_ptr;
    if (!c.tasks_count)
        return;

    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    // If user wants the entire doc in the same format, as the one we use internally,
    // this request can be passed entirely to the underlying Key-Value store.
    strided_iterator_gt<ukv_str_view_t const> fields {c.fields, c.fields_stride};
    auto has_fields = fields && (!fields.repeats() || *fields);
    strided_iterator_gt<ukv_collection_t const> collections {c.collections, c.collections_stride};
    strided_iterator_gt<ukv_key_t const> keys {c.keys, c.keys_stride};
    bits_view_t presences {c.presences};
    strided_iterator_gt<ukv_length_t const> offs {c.offsets, c.offsets_stride};
    strided_iterator_gt<ukv_length_t const> lens {c.lengths, c.lengths_stride};
    strided_iterator_gt<ukv_bytes_cptr_t const> vals {c.values, c.values_stride};

    places_arg_t places {collections, keys, fields, c.tasks_count};
    contents_arg_t contents {presences, offs, lens, vals, c.tasks_count};

    if (has_fields || c.type != internal_format_k || c.modification != ukv_doc_modify_upsert_k)
        return read_modify_write(c.db,
                                 c.transaction,
                                 places,
                                 contents,
                                 c.options,
                                 static_cast<doc_modification_t>(c.modification),
                                 c.type,
                                 arena,
                                 c.error);

    // Validate JSONs Before Write
    ukv_length_t max_length = 0;
    for (std::size_t i = 0; i != contents.size(); ++i) {
        if (max_length < contents[i].size() && contents[i].size() != ukv_length_missing_k)
            max_length = contents[i].size();
    }

    auto document = arena.alloc<byte_t>(max_length + sj::SIMDJSON_PADDING, c.error);
    return_if_error_m(c.error);

    sj::dom::parser parser;
    for (std::size_t i = 0; i < contents.size(); ++i) {
        std::memcpy(document.begin(), contents[i].data(), contents[i].size());
        std::memset(document.begin() + contents[i].size(), 0, sj::SIMDJSON_PADDING);
        auto result = parser.parse((const char*)document.begin(), contents[i].size(), false);
        return_error_if_m(result.error() == sj::SUCCESS, c.error, 0, "Invalid Json!");
    }

    ukv_write_t write {
        .db = c.db,
        .error = c.error,
        .transaction = c.transaction,
        .arena = arena,
        .options = c.options,
        .tasks_count = c.tasks_count,
        .collections = c.collections,
        .collections_stride = c.collections_stride,
        .keys = c.keys,
        .keys_stride = c.keys_stride,
        .presences = c.presences,
        .offsets = c.offsets,
        .offsets_stride = c.offsets_stride,
        .lengths = c.lengths,
        .lengths_stride = c.lengths_stride,
        .values = c.values,
        .values_stride = c.values_stride,
    };

    ukv_write(&write);
}

void ukv_docs_read(ukv_docs_read_t* c_ptr) {

    ukv_docs_read_t& c = *c_ptr;
    if (!c.tasks_count)
        return;

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    // If user wants the entire doc in the same format, as the one we use internally,
    // this request can be passed entirely to the underlying Key-Value store.
    strided_iterator_gt<ukv_str_view_t const> fields {c.fields, c.fields_stride};
    auto has_fields = fields && (!fields.repeats() || *fields);
    if (!has_fields && c.type == internal_format_k) {
        ukv_read_t read {
            .db = c.db,
            .error = c.error,
            .transaction = c.transaction,
            .arena = arena,
            .options = c.options,
            .tasks_count = c.tasks_count,
            .collections = c.collections,
            .collections_stride = c.collections_stride,
            .keys = c.keys,
            .keys_stride = c.keys_stride,
            .presences = c.presences,
            .offsets = c.offsets,
            .lengths = c.lengths,
            .values = c.values,
        };
        return ukv_read(&read);
    }

    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");

    strided_iterator_gt<ukv_collection_t const> collections {c.collections, c.collections_stride};
    strided_iterator_gt<ukv_key_t const> keys {c.keys, c.keys_stride};
    places_arg_t places {collections, keys, fields, c.tasks_count};

    // Now, we need to parse all the entries to later export them into a target format.
    // Potentially sampling certain sub-fields again along the way.
    growing_tape_t growing_tape {arena};
    growing_tape.reserve(places.size(), c.error);
    return_if_error_m(c.error);
    sj::ondemand::parser parser;

    auto safe_callback = [&](ukv_size_t, ukv_str_view_t field, value_view_t binary_doc) {
        if (binary_doc.empty()) {
            growing_tape.push_back(binary_doc, c.error);
            return;
        }

        std::string_view result;
        auto padded_doc =
            sj::padded_string_view(binary_doc.c_str(), binary_doc.size(), binary_doc.size() + sj::SIMDJSON_PADDING);

        string_t output {arena};
        if (c.type == ukv_doc_field_msgpack_k) {
            json_to_mpack(padded_doc, output, c.error);
            result = {output.data(), output.size()};
        }
        else if (c.type == ukv_doc_field_bson_k) {
            bson_error_t error;
            bson_t* b = bson_new_from_json((uint8_t*)binary_doc.c_str(), -1, &error);
            result = {(const char*)bson_get_data(b), b->len};
            growing_tape.push_back(result, c.error);
            growing_tape.add_terminator(byte_t {0}, c.error);
            return_if_error_m(c.error);
            bson_clear(&b);
            return;
        }
        else {
            auto maybe_doc = parser.iterate(padded_doc);
            return_error_if_m(maybe_doc.error() == sj::SUCCESS, c.error, 0, "Fail To Parse Document!");
            printed_number_buffer_t print_buffer;
            if (maybe_doc.value().is_scalar())
                result = get_value(maybe_doc.value(), c.type, print_buffer);
            else {
                auto parsed = maybe_doc.value().get_value();
                auto branch = simdjson_lookup(parsed.value(), field);
                result = get_value(branch, c.type, print_buffer);
            }
        }
        growing_tape.push_back(result, c.error);
        growing_tape.add_terminator(byte_t {0}, c.error);
        return_if_error_m(c.error);
    };

    places_arg_t unique_places;
    read_modify_docs(c.db,
                     c.transaction,
                     places,
                     c.options,
                     doc_modification_t::nothing_k,
                     arena,
                     unique_places,
                     c.error,
                     safe_callback);

    if (c.offsets)
        *c.offsets = growing_tape.offsets().begin().get();
    if (c.lengths)
        *c.lengths = growing_tape.lengths().begin().get();
    if (c.values)
        *c.values = reinterpret_cast<ukv_byte_t*>(growing_tape.contents().begin().get());
}

/*********************************************************/
/*****************	 Tabular Exports	  ****************/
/*********************************************************/

void gist_recursively(yyjson_val* node,
                      field_path_buffer_t& path,
                      uninitialized_array_gt<std::string_view>& sorted_paths,
                      growing_tape_t& exported_paths,
                      ukv_error_t* c_error) {

    auto path_len = std::strlen(path);
    auto constexpr slash_len = 1;
    auto constexpr terminator_len = 1;

    if (yyjson_is_obj(node)) {
        yyjson_val *key, *val;
        yyjson_obj_iter iter;
        yyjson_obj_iter_init(node, &iter);
        while ((key = yyjson_obj_iter_next(&iter)) && !*c_error) {
            val = yyjson_obj_iter_get_val(key);
            char const* key_name = yyjson_get_str(key);
            size_t key_len = yyjson_get_len(key);
            if (path_len + slash_len + key_len + terminator_len >= field_path_len_limit_k) {
                *c_error = "Path is too long!";
                return;
            }

            path[path_len] = '/';
            std::memcpy(path + path_len + slash_len, key_name, key_len);
            path[path_len + slash_len + key_len] = 0;
            gist_recursively(val, path, sorted_paths, exported_paths, c_error);
        }
        path[path_len] = 0;
    }
    else if (yyjson_is_arr(node)) {
        std::size_t idx = 0;
        yyjson_val* val;
        yyjson_arr_iter iter;
        yyjson_arr_iter_init(node, &iter);
        while ((val = yyjson_arr_iter_next(&iter)) && !*c_error) {

            path[path_len] = '/';
            auto result = print_number(path + path_len + slash_len, path + field_path_len_limit_k, idx);
            if (result.empty()) {
                *c_error = "Path is too long!";
                return;
            }

            gist_recursively(val, path, sorted_paths, exported_paths, c_error);
            ++idx;
        }
        path[path_len] = 0;
    }
    else {

        std::string_view path_str = std::string_view(path, path_len);
        std::size_t idx = std::lower_bound(sorted_paths.begin(), sorted_paths.end(), path_str) - sorted_paths.begin();
        if (idx != sorted_paths.size() && sorted_paths[idx] == path_str)
            // This same path is already exported
            return;

        auto exported_path = exported_paths.push_back(path, c_error);
        return_if_error_m(c_error);
        exported_paths.add_terminator(byte_t {0}, c_error);
        return_if_error_m(c_error);

        path_str = std::string_view(exported_path.c_str(), exported_path.size());
        sorted_paths.insert(idx, &path_str, &path_str + 1, c_error);
    }
}

void ukv_docs_gist(ukv_docs_gist_t* c_ptr) {

    ukv_docs_gist_t& c = *c_ptr;
    if (!c.docs_count)
        return;

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    ukv_byte_t* found_binary_begin = nullptr;
    ukv_length_t* found_binary_offs = nullptr;
    ukv_read_t read {
        .db = c.db,
        .error = c.error,
        .transaction = c.transaction,
        .arena = arena,
        .options = c.options,
        .tasks_count = c.docs_count,
        .collections = c.collections,
        .collections_stride = c.collections_stride,
        .keys = c.keys,
        .keys_stride = c.keys_stride,
        .presences = nullptr,
        .offsets = &found_binary_offs,
        .lengths = nullptr,
        .values = &found_binary_begin,
    };

    ukv_read(&read);
    return_if_error_m(c.error);

    strided_iterator_gt<ukv_collection_t const> collections {c.collections, c.collections_stride};
    strided_iterator_gt<ukv_key_t const> keys {c.keys, c.keys_stride};

    joined_blobs_t found_binaries {c.docs_count, found_binary_offs, found_binary_begin};
    joined_blobs_iterator_t found_binary_it = found_binaries.begin();

    // Export all the elements into a heap-allocated hash-set, keeping only unique entries
    field_path_buffer_t field_name = {0};
    uninitialized_array_gt<std::string_view> sorted_paths(arena);
    growing_tape_t exported_paths(arena);
    for (ukv_size_t doc_idx = 0; doc_idx != c.docs_count; ++doc_idx, ++found_binary_it) {
        value_view_t binary_doc = *found_binary_it;
        if (!binary_doc)
            continue;

        json_t doc = any_parse(binary_doc, internal_format_k, arena, c.error);
        return_if_error_m(c.error);
        if (!doc)
            continue;

        yyjson_val* root = yyjson_doc_get_root(doc.handle);
        gist_recursively(root, field_name, sorted_paths, exported_paths, c.error);
        return_if_error_m(c.error);
    }

    if (c.fields_count)
        *c.fields_count = static_cast<ukv_size_t>(sorted_paths.size());
    if (c.offsets)
        *c.offsets = exported_paths.offsets().begin().get();
    if (c.fields)
        *c.fields = reinterpret_cast<ukv_char_t*>(exported_paths.contents().begin().get());
}

std::size_t doc_field_size_bytes(ukv_doc_field_type_t type) {
    switch (type) {
    default: return 0;
    case ukv_doc_field_null_k: return 0;
    case ukv_doc_field_bool_k: return 1;
    case ukv_doc_field_uuid_k: return 16;

    case ukv_doc_field_i8_k: return 1;
    case ukv_doc_field_i16_k: return 2;
    case ukv_doc_field_i32_k: return 4;
    case ukv_doc_field_i64_k: return 8;

    case ukv_doc_field_u8_k: return 1;
    case ukv_doc_field_u16_k: return 2;
    case ukv_doc_field_u32_k: return 4;
    case ukv_doc_field_u64_k: return 8;

    case ukv_doc_field_f16_k: return 2;
    case ukv_doc_field_f32_k: return 4;
    case ukv_doc_field_f64_k: return 8;

    // Offsets and lengths:
    case ukv_doc_field_bin_k: return 8;
    case ukv_doc_field_str_k: return 8;
    }
}

bool doc_field_is_variable_length(ukv_doc_field_type_t type) {
    switch (type) {
    case ukv_doc_field_bin_k: return true;
    case ukv_doc_field_str_k: return true;
    default: return false;
    }
}

struct column_begin_t {
    ukv_octet_t* validities;
    ukv_octet_t* conversions;
    ukv_octet_t* collisions;
    ukv_byte_t* scalars;
    ukv_length_t* str_offsets;
    ukv_length_t* str_lengths;

    template <typename scalar_at>
    inline void set(std::size_t doc_idx, yyjson_val* value) noexcept {

        ukv_octet_t mask = static_cast<ukv_octet_t>(1 << (doc_idx % CHAR_BIT));
        ukv_octet_t& valid = validities[doc_idx / CHAR_BIT];
        ukv_octet_t& convert = conversions[doc_idx / CHAR_BIT];
        ukv_octet_t& collide = collisions[doc_idx / CHAR_BIT];
        scalar_at& scalar = reinterpret_cast<scalar_at*>(scalars)[doc_idx];

        json_to_scalar(value, mask, valid, convert, collide, scalar);
    }

    inline void set_str(std::size_t doc_idx,
                        yyjson_val* value,
                        printed_number_buffer_t& print_buffer,
                        string_t& output,
                        bool with_separator,
                        bool is_last,
                        ukv_error_t* c_error) noexcept {

        ukv_octet_t mask = static_cast<ukv_octet_t>(1 << (doc_idx % CHAR_BIT));
        ukv_octet_t& valid = validities[doc_idx / CHAR_BIT];
        ukv_octet_t& convert = conversions[doc_idx / CHAR_BIT];
        ukv_octet_t& collide = collisions[doc_idx / CHAR_BIT];
        ukv_length_t& off = str_offsets[doc_idx];
        ukv_length_t& len = str_lengths[doc_idx];

        auto str = json_to_string(value, mask, valid, convert, collide, print_buffer);
        off = static_cast<ukv_length_t>(output.size());
        len = static_cast<ukv_length_t>(str.size());
        output.insert(output.size(), str.begin(), str.end(), c_error);
        return_if_error_m(c_error);
        if (with_separator)
            output.push_back('\0', c_error);
        if (is_last)
            str_offsets[doc_idx + 1] = static_cast<ukv_length_t>(output.size());
    }
};

void ukv_docs_gather(ukv_docs_gather_t* c_ptr) {

    ukv_docs_gather_t& c = *c_ptr;
    if (!c.docs_count || !c.fields_count)
        return;

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    // Retrieve the entire documents before we can sample internal fields
    ukv_byte_t* found_binary_begin = nullptr;
    ukv_length_t* found_binary_offs = nullptr;
    ukv_read_t read {
        .db = c.db,
        .error = c.error,
        .transaction = c.transaction,
        .arena = arena,
        .options = c.options,
        .tasks_count = c.docs_count,
        .collections = c.collections,
        .collections_stride = c.collections_stride,
        .keys = c.keys,
        .keys_stride = c.keys_stride,
        .offsets = &found_binary_offs,
        .values = &found_binary_begin,
    };

    ukv_read(&read);
    return_if_error_m(c.error);

    strided_iterator_gt<ukv_collection_t const> collections {c.collections, c.collections_stride};
    strided_iterator_gt<ukv_key_t const> keys {c.keys, c.keys_stride};
    strided_iterator_gt<ukv_str_view_t const> fields {c.fields, c.fields_stride};
    strided_iterator_gt<ukv_doc_field_type_t const> types {c.types, c.types_stride};

    joined_blobs_t found_binaries {c.docs_count, found_binary_offs, found_binary_begin};
    joined_blobs_iterator_t found_binary_it = found_binaries.begin();

    // Estimate the amount of memory needed to store at least scalars and columns addresses
    // TODO: Align offsets of bitmaps to 64-byte boundaries for Arrow
    // https://arrow.apache.org/docs/format/Columnar.html#buffer-alignment-and-padding
    // TODO: Align offsets of bitmaps to 64-byte boundaries for Arrow
    // https://arrow.apache.org/docs/format/Columnar.html#buffer-alignment-and-padding
    bool wants_conversions = c.columns_conversions;
    bool wants_collisions = c.columns_collisions;
    std::size_t slots_per_bitmap = divide_round_up<std::size_t>(c.docs_count, bits_in_byte_k);
    std::size_t count_bitmaps = 1ul + wants_conversions + wants_collisions;
    std::size_t bytes_per_bitmap = sizeof(ukv_octet_t) * slots_per_bitmap;
    std::size_t bytes_per_addresses_row = sizeof(void*) * c.fields_count;
    std::size_t bytes_for_addresses = bytes_per_addresses_row * 6 + sizeof(ukv_length_t);
    std::size_t bytes_for_bitmaps = bytes_per_bitmap * count_bitmaps * c.fields_count * c.fields_count;
    std::size_t bytes_per_scalars_row = transform_reduce_n(types, c.fields_count, 0ul, &doc_field_size_bytes);
    std::size_t bytes_for_scalars = bytes_per_scalars_row * c.docs_count;

    std::size_t string_columns = transform_reduce_n(types, c.fields_count, 0ul, doc_field_is_variable_length);
    bool has_string_columns = string_columns != 0;
    bool has_scalar_columns = string_columns != c.fields_count;

    // Preallocate at least a minimum amount of memory.
    // It will be organized in the following way:
    // 1. validity bitmaps for all fields
    // 2. optional conversion bitmaps for all fields
    // 3. optional collision bitmaps for all fields
    // 4. offsets of all strings
    // 5. lengths of all strings
    // 6. scalars for all fields

    auto tape = arena.alloc<byte_t>(bytes_for_addresses + bytes_for_bitmaps + bytes_for_scalars, c.error);
    byte_t* const tape_ptr = tape.begin();

    // If those pointers were not provided, we can reuse the validity bitmap
    // It will allow us to avoid extra checks later.
    // ! Still, in every sequence of updates, validity is the last bit to be set,
    // ! to avoid overwriting.
    auto first_collection_validities = reinterpret_cast<ukv_octet_t*>(tape_ptr + bytes_for_addresses);
    auto first_collection_conversions = wants_conversions //
                                            ? first_collection_validities + slots_per_bitmap * c.fields_count
                                            : first_collection_validities;
    auto first_collection_collisions = wants_collisions //
                                           ? first_collection_conversions + slots_per_bitmap * c.fields_count
                                           : first_collection_validities;
    auto first_collection_scalars = reinterpret_cast<ukv_byte_t*>(tape_ptr + bytes_for_addresses + bytes_for_bitmaps);

    // 1, 2, 3. Export validity maps addresses
    std::size_t tape_progress = 0;
    {
        auto addresses = reinterpret_cast<ukv_octet_t**>(tape_ptr + tape_progress);
        if (c.columns_validities)
            *c.columns_validities = addresses;
        for (ukv_size_t field_idx = 0; field_idx != c.fields_count; ++field_idx)
            addresses[field_idx] = first_collection_validities + field_idx * slots_per_bitmap;
        tape_progress += bytes_per_addresses_row;
    }
    if (wants_conversions) {
        auto addresses = reinterpret_cast<ukv_octet_t**>(tape_ptr + tape_progress);
        if (c.columns_conversions)
            *c.columns_conversions = addresses;
        for (ukv_size_t field_idx = 0; field_idx != c.fields_count; ++field_idx)
            addresses[field_idx] = first_collection_conversions + field_idx * slots_per_bitmap;
        tape_progress += bytes_per_addresses_row;
    }
    if (wants_collisions) {
        auto addresses = reinterpret_cast<ukv_octet_t**>(tape_ptr + tape_progress);
        if (c.columns_collisions)
            *c.columns_collisions = addresses;
        for (ukv_size_t field_idx = 0; field_idx != c.fields_count; ++field_idx)
            addresses[field_idx] = first_collection_collisions + field_idx * slots_per_bitmap;
        tape_progress += bytes_per_addresses_row;
    }

    // 4, 5, 6. Export addresses for scalars, strings offsets and strings lengths
    auto addresses_offs = reinterpret_cast<ukv_length_t**>(tape_ptr + tape_progress + bytes_per_addresses_row * 0);
    if (c.columns_offsets)
        *c.columns_offsets = addresses_offs;
    auto addresses_lens = reinterpret_cast<ukv_length_t**>(tape_ptr + tape_progress + bytes_per_addresses_row * 1);
    if (c.columns_lengths)
        *c.columns_lengths = addresses_lens;
    auto addresses_scalars = reinterpret_cast<ukv_byte_t**>(tape_ptr + tape_progress + bytes_per_addresses_row * 2);
    if (c.columns_scalars)
        *c.columns_scalars = addresses_scalars;

    {
        auto scalars_tape = first_collection_scalars;
        for (ukv_size_t field_idx = 0; field_idx != c.fields_count; ++field_idx) {
            ukv_doc_field_type_t type = types[field_idx];
            switch (type) {
            case ukv_doc_field_str_k:
            case ukv_doc_field_bin_k:
                addresses_offs[field_idx] = reinterpret_cast<ukv_length_t*>(scalars_tape);
                addresses_lens[field_idx] = addresses_offs[field_idx] + c.docs_count + 1;
                addresses_scalars[field_idx] = nullptr;
                break;
            default:
                addresses_offs[field_idx] = nullptr;
                addresses_lens[field_idx] = nullptr;
                addresses_scalars[field_idx] = reinterpret_cast<ukv_byte_t*>(scalars_tape);
                break;
            }
            scalars_tape += doc_field_size_bytes(type) * c.docs_count + sizeof(ukv_length_t);
        }
    }

    // Go though all the documents extracting and type-checking the relevant parts
    printed_number_buffer_t print_buffer;
    string_t string_tape(arena);
    for (ukv_size_t doc_idx = 0; doc_idx != c.docs_count; ++doc_idx, ++found_binary_it) {
        value_view_t binary_doc = *found_binary_it;
        json_t doc = any_parse(binary_doc, internal_format_k, arena, c.error);
        return_if_error_m(c.error);
        if (!doc)
            continue;
        yyjson_val* root = yyjson_doc_get_root(doc.handle);

        for (ukv_size_t field_idx = 0; field_idx != c.fields_count; ++field_idx) {

            // Find this field within document
            ukv_doc_field_type_t type = types[field_idx];
            ukv_str_view_t field = fields[field_idx];
            yyjson_val* found_value = json_lookup(root, field);

            column_begin_t column {
                .validities = (*c.columns_validities)[field_idx],
                .conversions = (*(c.columns_conversions ? c.columns_conversions : c.columns_validities))[field_idx],
                .collisions = (*(c.columns_collisions ? c.columns_collisions : c.columns_validities))[field_idx],
                .scalars = addresses_scalars[field_idx],
                .str_offsets = addresses_offs[field_idx],
                .str_lengths = addresses_lens[field_idx],
            };

            bool is_last = doc_idx == c.docs_count - 1;
            // Export the types
            switch (type) {

            case ukv_doc_field_bool_k: column.set<bool>(doc_idx, found_value); break;

            case ukv_doc_field_i8_k: column.set<std::int8_t>(doc_idx, found_value); break;
            case ukv_doc_field_i16_k: column.set<std::int16_t>(doc_idx, found_value); break;
            case ukv_doc_field_i32_k: column.set<std::int32_t>(doc_idx, found_value); break;
            case ukv_doc_field_i64_k: column.set<std::int64_t>(doc_idx, found_value); break;

            case ukv_doc_field_u8_k: column.set<std::uint8_t>(doc_idx, found_value); break;
            case ukv_doc_field_u16_k: column.set<std::uint16_t>(doc_idx, found_value); break;
            case ukv_doc_field_u32_k: column.set<std::uint32_t>(doc_idx, found_value); break;
            case ukv_doc_field_u64_k: column.set<std::uint64_t>(doc_idx, found_value); break;

            case ukv_doc_field_f32_k: column.set<float>(doc_idx, found_value); break;
            case ukv_doc_field_f64_k: column.set<double>(doc_idx, found_value); break;

            case ukv_doc_field_str_k:
                column.set_str(doc_idx, found_value, print_buffer, string_tape, true, is_last, c.error);
                break;
            case ukv_doc_field_bin_k:
                column.set_str(doc_idx, found_value, print_buffer, string_tape, false, is_last, c.error);
                break;

            default: break;
            }
        }
    }

    *c.joined_strings = reinterpret_cast<ukv_byte_t*>(string_tape.data());
}
