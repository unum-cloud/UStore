/**
 *  @file include/ustore/docs/json_paths.hpp
 *  @author Ash Vardanian
 *  @date October 16, 2026
 *  @brief Document fields translated into JSONPath and extracted by StringZilla's subdocuments engine in one pass over
 *      a batch of documents, into typed values the reads and gathers of documents convert, with no other parser.
 *
 *  A field is a JSON Pointer when it starts with a slash and a top-level key otherwise, and an
 *  empty field addresses the whole document. Each field compiles into one JSONPath,
 *  <tt>$['a']['b']</tt>, where a token spelling an array index becomes the union <tt>['0',0]</tt>,
 *  as a pointer resolves it. One engine compiles the paths of every field of a call, entering only
 *  the first member of each name as a pointer does, and one @c sz_subdocuments_find pass over the
 *  documents the cells read locates the node of every cell. The first byte of each node tells what
 *  it holds, strings then unescape in one batch, and numbers parse as yyjson parses them inside a
 *  document.
 *
 *  A document the engine finds malformed fails the call, comments and NaN or Infinity among it, though a container no
 *  path enters is skipped unread. A value read as JSON prints canonically at the top level, and a container prints
 *  minified with its strings and numbers as written.
 */

#pragma once

#include <cstddef> // `std::size_t`, `std::max_align_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::memset`

#include <charconv>    // `std::to_chars`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`

#include <smashtable/basic_hash_table.hpp> // `st::hash_map`, `st::assume_reserved`
#include <smashtable/basic_vector.hpp>     // `st::basic_vector`
#include <stringzilla/find.h>              // `sz_byteset_init`, `sz_byteset_add_u8`
#include <stringzilla/strings.h>           // `sz_strings_span_t`, `sz_strings_from_string_views`
#include <stringzilla/subdocuments.h>      // `sz_subdocuments_engine_init`, `sz_subdocuments_find`

#include "ustore/docs/json_values.hpp"      // `json_cell_t`, `print_number`
#include "ustore/utilities/ram_buffers.hpp" // `arena_allocate`, `arena_grow`, `stringzilla_kernels_t`
#include "ustore/utilities/types.hpp"       // `value_view_t`, `byte_t`

namespace unum::ustore::docs {

using utilities::arena_allocator;
using utilities::growth_status;
using utilities::stringzilla_allocator;
using utilities::stringzilla_kernels_t;

#pragma region StringZilla Bridges

/** Views @p text as StringZilla's pointer and length. */
inline sz_string_view_t stringzilla_view(std::string_view text) noexcept {
    sz_string_view_t view;
    view.start = text.data();
    view.length = text.size();
    return view;
}

#pragma endregion StringZilla Bridges

#pragma region Field Translation

/** Largest array index a JSONPath index selector spells, 2⁵³ - 1. */
constexpr std::uint64_t json_path_index_limit_k = (std::uint64_t {1} << 53) - 1;

/** Appends @p bytes to @p path. */
inline status_t append_to_path(st::basic_vector<char, arena_allocator<char>>& path, std::string_view bytes) noexcept {
    std::size_t const offset = path.size();
    if (status_t const status = growth_status(path.resize(offset + bytes.size())); status.failed()) return status;
    if (!bytes.empty()) std::memcpy(path.data() + offset, bytes.data(), bytes.size());
    return {};
}

/** Appends @p key unescaped from an RFC 6901 token when @p pointer, as a single-quoted RFC 9535 name. */
inline status_t append_quoted_name(st::basic_vector<char, arena_allocator<char>>& path, std::string_view key,
                                   bool pointer) noexcept {
    if (status_t const status = append_to_path(path, "'"); status.failed()) return status;
    constexpr char hex_digits_k[] = "0123456789abcdef";
    for (std::size_t index = 0; index != key.size(); ++index) {
        char character = key[index];
        if (pointer && character == '~') character = key[++index] == '0' ? '~' : '/';
        auto const byte = static_cast<unsigned char>(character);
        char escaped[6] = {'\\', character};
        std::size_t escaped_length = 2;
        if (byte < 0x20) {
            escaped[1] = 'u', escaped[2] = '0', escaped[3] = '0';
            escaped[4] = hex_digits_k[byte >> 4], escaped[5] = hex_digits_k[byte & 15];
            escaped_length = 6;
        }
        else if (character != '\'' && character != '\\') escaped[0] = character, escaped_length = 1;
        if (status_t const status = append_to_path(path, {escaped, escaped_length}); status.failed()) return status;
    }
    return append_to_path(path, "'");
}

/** Whether @p token is an array index as yyjson's pointer reads one: @c 0, or digits without a leading zero. */
inline bool is_index_token(std::string_view token, std::uint64_t& index) noexcept {
    if (token.empty() || token.size() > 16 || (token[0] == '0' && token.size() > 1)) return false;
    index = 0;
    for (char const character : token) {
        if (character < '0' || character > '9') return false;
        index = index * 10 + static_cast<std::uint64_t>(character - '0');
    }
    return index <= json_path_index_limit_k;
}

/** Whether every tilde of the JSON Pointer @p field escapes a tilde or a slash, without which yyjson resolves it to
 *  nothing. */
inline bool is_valid_pointer(std::string_view field) noexcept {
    for (std::size_t index = 0; index != field.size(); ++index)
        if (field[index] == '~' && (index + 1 == field.size() || (field[index + 1] != '0' && field[index + 1] != '1')))
            return false;
    return true;
}

/** Spells into @p path the JSONPath @p field resolves through, the root alone for an empty field, answering false for
 *  an invalid JSON Pointer, which no document holds. */
inline expected<bool> json_path_of(std::string_view field,
                                   st::basic_vector<char, arena_allocator<char>>& path) noexcept {
    if (status_t const status = growth_status(path.resize(0)); status.failed()) return status;
    if (status_t const status = append_to_path(path, "$"); status.failed()) return status;
    if (field.empty()) return true;
    if (!field.starts_with('/')) {
        if (status_t const status = append_to_path(path, "["); status.failed()) return status;
        if (status_t const status = append_quoted_name(path, field, false); status.failed()) return status;
        if (status_t const status = append_to_path(path, "]"); status.failed()) return status;
        return true;
    }
    if (!is_valid_pointer(field)) return false;

    for (std::size_t begin = 1; begin <= field.size();) {
        std::size_t end = field.find('/', begin);
        if (end == std::string_view::npos) end = field.size();
        std::string_view const token = field.substr(begin, end - begin);
        if (status_t const status = append_to_path(path, "["); status.failed()) return status;
        if (status_t const status = append_quoted_name(path, token, true); status.failed()) return status;
        if (std::uint64_t index = 0; is_index_token(token, index)) {
            char printed[24];
            std::string_view const digits = print_number(printed, printed + sizeof(printed), index);
            if (status_t const status = append_to_path(path, ","); status.failed()) return status;
            if (status_t const status = append_to_path(path, digits); status.failed()) return status;
        }
        if (status_t const status = append_to_path(path, "]"); status.failed()) return status;
        begin = end + 1;
    }
    return true;
}

#pragma endregion Field Translation

#pragma region Extraction

/** The documents of a read: a tape, the presence bits or @c nullptr when all are present, and their count. */
struct json_documents_t {

    /** The bytes of every document, missing ones empty. */
    ustore_strings_view_t tape {};

    /** Presence bits, addressed as Apache Arrow does, or @c nullptr. */
    ustore_octet_t const* presences = nullptr;

    /** Number of documents. */
    std::size_t count = 0;

    /** The bytes of document @p index, empty when it is missing. */
    std::string_view operator[](std::size_t index) const noexcept {
        if (presences && !((presences[index / 8] >> (index % 8)) & 1u)) return {};
        if (tape.layout == ustore_strings_u64_tape_k) {
            std::uint64_t const* const offsets = tape.data.u64_tape.offsets;
            return {reinterpret_cast<char const*>(tape.data.u64_tape.data) + offsets[index],
                    static_cast<std::size_t>(offsets[index + 1] - offsets[index])};
        }
        std::uint32_t const* const offsets = tape.data.u32_tape.offsets;
        return {reinterpret_cast<char const*>(tape.data.u32_tape.data) + offsets[index],
                static_cast<std::size_t>(offsets[index + 1] - offsets[index])};
    }
};

/** A document and a field, by index, whose value a cell holds. */
struct field_cell_t {
    std::size_t document = 0;
    std::size_t field = 0;
};

/** The value of every cell a read extracts, as columns indexed by cell. */
struct json_cells_t {

    /** What every cell holds, @c json_token_t::missing_k where its field is missing. */
    std::span<json_token_t> tokens;

    /** The bytes of every cell: a string's body unescaped, any other value as its document spells it. */
    std::span<std::string_view> bytes;

    /** The number every number cell spells. */
    std::span<json_number_t> numbers;

    /** The value of cell @p index. */
    json_cell_t operator[](std::size_t index) const noexcept {
        return json_cell_t {tokens[index], bytes[index], numbers[index]};
    }
};

/** A compiled subdocuments engine, freed with the call that compiled it. */
class subdocuments_engine_t {
    sz_subdocuments_engine_t engine_ {};
    bool compiled_ = false;

  public:
    subdocuments_engine_t() noexcept = default;
    subdocuments_engine_t(subdocuments_engine_t const&) = delete;
    subdocuments_engine_t& operator=(subdocuments_engine_t const&) = delete;
    ~subdocuments_engine_t() noexcept {
        if (compiled_) sz_subdocuments_engine_free(&engine_, nullptr);
    }

    /** Compiles the JSONPath @p paths, entering only the first member of a name as a JSON Pointer does, with blocks
     *  of @p allocator for @p capabilities. */
    sz_status_t compile(sz_strings_view_t const& paths, sz_allocator_t& allocator,
                        sz_capability_t capabilities) noexcept {
        sz_size_t malformed_index = 0;
        sz_status_t const status = sz_subdocuments_engine_init(&engine_, &paths, sz_subdocuments_json_k,
                                                               sz_subdocuments_duplicate_names_first_k,
                                                               &malformed_index, capabilities, &allocator, nullptr);
        compiled_ = status == sz_success_k;
        return status;
    }

    /** Finds the node every path selects in every one of @p documents, as @c sz_subdocuments_find does. */
    sz_status_t find(sz_strings_view_t const& documents, std::span<sz_match_t> matches,
                     std::span<sz_size_t> matches_offsets) noexcept {
        sz_size_t malformed_index = 0;
        return sz_subdocuments_find(&engine_, &documents, matches.data(), matches.size(), matches_offsets.data(),
                                    &malformed_index, nullptr);
    }
};

/** The status of a failed StringZilla call of a read, @p failed being what the read was doing. */
inline status_t stringzilla_failure(sz_status_t status, char const* failed) noexcept {
    if (status == sz_bad_alloc_k) return {status_code_t::out_of_memory_k, failed};
    if (status == sz_invalid_syntax_k || status == sz_invalid_utf8_k)
        return {status_code_t::malformed_k, "Failed to parse the document"};
    return {status_code_t::internal_k, failed};
}

/** The bytes of string @p index of @p tape, which an unescape wrote. */
inline std::string_view tape_string(sz_strings_span_t const& tape, std::size_t index) noexcept {
    if (tape.layout == sz_strings_u64_tape_k) {
        sz_u64_t const* const offsets = tape.data.u64_tape.offsets;
        return {tape.data.u64_tape.data + offsets[index],
                static_cast<std::size_t>(offsets[index + 1] - offsets[index])};
    }
    sz_u32_t const* const offsets = tape.data.u32_tape.offsets;
    return {tape.data.u32_tape.data + offsets[index], static_cast<std::size_t>(offsets[index + 1] - offsets[index])};
}

/**
 *  @brief Extracts the value of every cell, as a JSON Pointer lookup over its parsed document would find it, through
 *      one subdocuments pass over the documents the cells read.
 *
 *  @param[in] documents The documents the cells address, read in place.
 *  @param[in] fields The fields the cells address, each a JSON Pointer, a top-level key, or empty for the document.
 *  @param[in] cells_count How many cells there are.
 *  @param[in] cell_of Called as <tt>cell_of(index)</tt>, answering the @c field_cell_t of cell @p index.
 *  @param[in] kernels The StringZilla kernels the handle looked up once at open.
 *  @param[in] scratch Holds the engine, the columns and every value, until it rewinds.
 *  @param[out] cells The value of every cell, missing where a document or its field is.
 *  @retval malformed_k A document a cell reads breaks RFC 8259, or a string's escapes do not decode.
 */
template <typename cell_of_type_>
status_t extract_fields(json_documents_t const& documents, std::span<std::string_view const> fields,
                        std::size_t cells_count, cell_of_type_ const& cell_of, stringzilla_kernels_t const& kernels,
                        ustore_arena_t scratch, json_cells_t& cells) noexcept {
    auto [tokens, tokens_status] = arena_allocate<json_token_t>(scratch, cells_count);
    if (tokens_status.failed()) return tokens_status;
    auto [bytes, bytes_status] = arena_allocate<std::string_view>(scratch, cells_count);
    if (bytes_status.failed()) return bytes_status;
    auto [numbers, numbers_status] = arena_allocate<json_number_t>(scratch, cells_count);
    if (numbers_status.failed()) return numbers_status;
    for (std::size_t cell_index = 0; cell_index != cells_count; ++cell_index)
        tokens[cell_index] = json_token_t::missing_k, bytes[cell_index] = {}, numbers[cell_index] = {};
    cells = json_cells_t {tokens, bytes, numbers};
    if (!cells_count) return {};
    if (documents.tape.layout != ustore_strings_u32_tape_k && documents.tape.layout != ustore_strings_u64_tape_k)
        return {status_code_t::invalid_argument_k, "Documents arrive on a tape"};
    std::size_t const documents_count = documents.count;
    constexpr std::size_t no_index_k = static_cast<std::size_t>(-1);

    // Every field compiles into one path, which fields spelling the same path share
    auto [path_of_field, map_status] = arena_allocate<std::size_t>(scratch, fields.size());
    if (map_status.failed()) return map_status;
    st::basic_vector<sz_string_view_t, arena_allocator<sz_string_view_t>> paths {{scratch}};
    st::hash_map<std::string_view, std::size_t, st::default_hash_t, st::equal_to_t, arena_allocator<std::byte>>
        known_paths {{}, {}, arena_allocator<std::byte> {scratch}};
    st::basic_vector<char, arena_allocator<char>> path {{scratch}};
    for (std::size_t field_index = 0; field_index != fields.size(); ++field_index) {
        auto [valid, path_status] = json_path_of(fields[field_index], path);
        if (path_status.failed()) return path_status;
        path_of_field[field_index] = no_index_k;
        if (!valid) continue;
        std::string_view const spelled {path.begin(), path.end()};
        known_paths.probe_to_find(spelled,
                                  [&](auto const& slot) noexcept { path_of_field[field_index] = slot.value(); });
        if (path_of_field[field_index] != no_index_k) continue;
        // The scratch path is respelled for every field, so a new path moves into bytes of its own
        auto [kept, kept_status] = arena_allocate<char>(scratch, spelled.size());
        if (kept_status.failed()) return kept_status;
        std::memcpy(kept.data(), spelled.data(), spelled.size());
        std::string_view const stable {kept.begin(), kept.end()};
        if (known_paths.reserve_more(1) == decltype(known_paths)::reserve_result_t::failed_k)
            return {status_code_t::out_of_memory_k, "No room to compile another path"};
        known_paths.emplace(stable, std::size_t {paths.size()}, st::assume_reserved);
        path_of_field[field_index] = paths.size();
        if (status_t const status = growth_status(paths.push_back(stringzilla_view(stable))); status.failed())
            return status;
    }

    // Only the documents some cell reads are walked, so a missing or empty one asks the engine nothing
    auto [walked_of_document, walked_status] = arena_allocate<std::size_t>(scratch, documents_count);
    if (walked_status.failed()) return walked_status;
    for (std::size_t& walked : walked_of_document) walked = no_index_k;
    st::basic_vector<sz_string_view_t, arena_allocator<sz_string_view_t>> walked {{scratch}};
    for (std::size_t cell_index = 0; cell_index != cells_count; ++cell_index) {
        field_cell_t const cell = cell_of(cell_index);
        if (path_of_field[cell.field] == no_index_k || walked_of_document[cell.document] != no_index_k) continue;
        std::string_view const document = documents[cell.document];
        if (document.empty()) continue;
        walked_of_document[cell.document] = walked.size();
        if (status_t const status = growth_status(walked.push_back(stringzilla_view(document))); status.failed())
            return status;
    }
    if (walked.empty()) return {};

    sz_allocator_t allocator = stringzilla_allocator(scratch);
    subdocuments_engine_t engine;
    sz_strings_view_t paths_view {};
    sz_strings_from_string_views(paths.data(), paths.size(), &paths_view);
    if (sz_status_t const compiled = engine.compile(paths_view, allocator, kernels.capabilities);
        compiled != sz_success_k)
        return stringzilla_failure(compiled, "The fields compile into no path the engine takes");

    // With first members only, a field path selects at most one node of a document
    std::size_t const slots_count = walked.size() * paths.size();
    auto [matches, matches_status] = arena_allocate<sz_match_t>(scratch, slots_count);
    if (matches_status.failed()) return matches_status;
    auto [matches_offsets, offsets_status] = arena_allocate<sz_size_t>(scratch, walked.size() + 1);
    if (offsets_status.failed()) return offsets_status;
    sz_strings_view_t walked_view {};
    sz_strings_from_string_views(walked.data(), walked.size(), &walked_view);
    if (sz_status_t const found = engine.find(walked_view, matches, matches_offsets); found != sz_success_k)
        return stringzilla_failure(found, "The engine could not walk the documents");
    std::size_t const matches_count = static_cast<std::size_t>(matches_offsets[walked.size()]);
    if (matches_count > slots_count) return {status_code_t::internal_k, "A field path selected more than one node"};
    auto [match_of_slot, slots_status] = arena_allocate<std::size_t>(scratch, slots_count);
    if (slots_status.failed()) return slots_status;
    for (std::size_t& match : match_of_slot) match = no_index_k;
    for (std::size_t match_index = 0; match_index != matches_count; ++match_index) {
        sz_match_t const& match = matches[match_index];
        match_of_slot[match.haystack_index * paths.size() + match.needle_index] = match_index;
    }

    // Every cell takes its token, strings keeping their quotes until one batched unescape below
    st::basic_vector<sz_string_view_t, arena_allocator<sz_string_view_t>> escaped {{scratch}};
    st::basic_vector<std::size_t, arena_allocator<std::size_t>> string_cells {{scratch}};
    for (std::size_t cell_index = 0; cell_index != cells_count; ++cell_index) {
        field_cell_t const cell = cell_of(cell_index);
        std::size_t const path_index = path_of_field[cell.field];
        std::size_t const walked_index = path_index == no_index_k ? no_index_k : walked_of_document[cell.document];
        if (walked_index == no_index_k) continue;
        std::size_t const match_index = match_of_slot[walked_index * paths.size() + path_index];
        if (match_index == no_index_k) continue;
        sz_match_t const& match = matches[match_index];
        std::string_view const token = documents[cell.document].substr(match.byte_offset, match.byte_length);
        json_cell_t const classified = json_classify(token);
        if (classified.token == json_token_t::malformed_k)
            return {status_code_t::malformed_k, "Failed to parse the document"};
        tokens[cell_index] = classified.token, bytes[cell_index] = classified.bytes,
        numbers[cell_index] = classified.number;
        if (classified.token != json_token_t::string_k) continue;
        if (status_t const status = growth_status(
                escaped.push_back(stringzilla_view(token.substr(1, token.size() - 2))));
            status.failed())
            return status;
        if (status_t const status = growth_status(string_cells.push_back(std::size_t {cell_index})); status.failed())
            return status;
    }
    if (escaped.empty()) return {};

    sz_strings_view_t escaped_view {};
    sz_strings_from_string_views(escaped.data(), escaped.size(), &escaped_view);
    sz_size_t malformed_index = 0;
    // RFC 8259 strings hold well-formed UTF-8 and no raw control byte, which writes refuse and reads check alike
    if (sz_status_t const status = kernels.utf8_count_strings(&escaped_view, nullptr, escaped.size(), nullptr,
                                                              &malformed_index, nullptr);
        status != sz_success_k)
        return stringzilla_failure(status, "The strings of the fields are not UTF-8");
    sz_byteset_t controls;
    sz_byteset_init(&controls);
    for (unsigned control = 0; control != 0x20; ++control) sz_byteset_add_u8(&controls, static_cast<sz_u8_t>(control));
    for (sz_string_view_t const& body : escaped) {
        sz_cptr_t control = nullptr;
        if (sz_status_t const status = kernels.find_byteset(body.start, body.length, &controls, &control, nullptr);
            status != sz_success_k)
            return stringzilla_failure(status, "The strings of the fields could not be checked");
        if (control) return {status_code_t::malformed_k, "Failed to parse the document"};
    }
    sz_strings_span_t unescaped {};
    if (sz_status_t const status = kernels.strings_unescape(&escaped_view, nullptr, escaped.size(), sz_escape_json_k,
                                                            &allocator, &unescaped, &malformed_index, nullptr);
        status != sz_success_k)
        return stringzilla_failure(status, "The strings of the fields did not unescape");
    for (std::size_t string_index = 0; string_index != string_cells.size(); ++string_index)
        bytes[string_cells[string_index]] = tape_string(unescaped, string_index);
    return {};
}

/** Appends @p bytes to @p text. */
inline status_t append_text(st::basic_vector<char, arena_allocator<char>>& text, std::string_view bytes) noexcept {
    std::size_t const offset = text.size();
    if (status_t const status = growth_status(text.resize(offset + bytes.size())); status.failed()) return status;
    if (!bytes.empty()) std::memcpy(text.data() + offset, bytes.data(), bytes.size());
    return {};
}

/** Appends the JSON @p json to @p text without the blank space between its tokens, its strings and numbers as
 *  written. */
inline status_t append_minified(st::basic_vector<char, arena_allocator<char>>& text, std::string_view json) noexcept {
    std::size_t begin = 0;
    for (std::size_t position = 0; position != json.size(); ++position) {
        char const character = json[position];
        if (character == '"') {
            // A string runs to the first quote no backslash escapes
            for (++position; position != json.size() && json[position] != '"'; ++position)
                position += json[position] == '\\';
            if (position == json.size()) break;
            continue;
        }
        if (character != ' ' && character != '\t' && character != '\n' && character != '\r') continue;
        if (status_t const status = append_text(text, json.substr(begin, position - begin)); status.failed())
            return status;
        begin = position + 1;
    }
    return append_text(text, json.substr(begin < json.size() ? begin : json.size()));
}

/** Appends @p body to @p text as a JSON string, escaped as yyjson writes one. */
inline status_t append_json_string(st::basic_vector<char, arena_allocator<char>>& text,
                                   std::string_view body) noexcept {
    constexpr char hex_digits_k[] = "0123456789ABCDEF";
    if (status_t const status = append_text(text, "\""); status.failed()) return status;
    std::size_t begin = 0;
    for (std::size_t position = 0; position != body.size(); ++position) {
        auto const byte = static_cast<unsigned char>(body[position]);
        char escaped[6] = {'\\', static_cast<char>(byte), 0, 0, 0, 0};
        std::size_t escaped_length = 2;
        switch (byte) {
        case '"':
        case '\\': break;
        case '\b': escaped[1] = 'b'; break;
        case '\t': escaped[1] = 't'; break;
        case '\n': escaped[1] = 'n'; break;
        case '\f': escaped[1] = 'f'; break;
        case '\r': escaped[1] = 'r'; break;
        default:
            if (byte >= 0x20) continue;
            escaped[1] = 'u', escaped[2] = '0', escaped[3] = '0';
            escaped[4] = hex_digits_k[byte >> 4], escaped[5] = hex_digits_k[byte & 15];
            escaped_length = 6;
        }
        if (status_t const status = append_text(text, body.substr(begin, position - begin)); status.failed())
            return status;
        if (status_t const status = append_text(text, {escaped, escaped_length}); status.failed()) return status;
        begin = position + 1;
    }
    if (status_t const status = append_text(text, body.substr(begin)); status.failed()) return status;
    return append_text(text, "\"");
}

/** Prints @p real into @p buffer as yyjson writes a double: the shortest digits that read back, in plain notation
 *  with a fraction while the decimal point falls within 21 digits and 6 zeros, and as an exponent otherwise. */
inline std::string_view print_json_real(double real, printed_number_buffer_t& buffer) noexcept {
    char scientific[printed_number_length_limit_k];
    auto const [end, failure] = std::to_chars(scientific, scientific + sizeof(scientific), real,
                                              std::chars_format::scientific);
    if (failure != std::errc()) return {};
    std::string_view const printed {scientific, static_cast<std::size_t>(end - scientific)};
    std::size_t const exponent_position = printed.find('e');
    bool const negative = printed.front() == '-';
    char digits[printed_number_length_limit_k];
    std::size_t digits_count = 0;
    for (char const character : printed.substr(negative, exponent_position - negative))
        if (character != '.') digits[digits_count++] = character;
    int exponent = 0;
    std::from_chars(printed.data() + exponent_position + 1 + (printed[exponent_position + 1] == '+'),
                    printed.data() + printed.size(), exponent);
    while (digits_count > 1 && digits[digits_count - 1] == '0') --digits_count;

    char* output = buffer;
    if (negative) *output++ = '-';
    if (digits_count == 1 && digits[0] == '0') {
        std::memcpy(output, "0.0", 3);
        return {buffer, static_cast<std::size_t>(output + 3 - buffer)};
    }
    int const point = exponent + 1;
    if (point > -6 && point <= 21) {
        if (point <= 0) {
            *output++ = '0', *output++ = '.';
            for (int zero = 0; zero != -point; ++zero) *output++ = '0';
            std::memcpy(output, digits, digits_count), output += digits_count;
        }
        else if (static_cast<std::size_t>(point) >= digits_count) {
            std::memcpy(output, digits, digits_count), output += digits_count;
            for (std::size_t zero = digits_count; zero != static_cast<std::size_t>(point); ++zero) *output++ = '0';
            *output++ = '.', *output++ = '0';
        }
        else {
            std::memcpy(output, digits, static_cast<std::size_t>(point)), output += point;
            *output++ = '.';
            std::memcpy(output, digits + point, digits_count - point), output += digits_count - point;
        }
    }
    else {
        *output++ = digits[0];
        if (digits_count > 1)
            *output++ = '.', std::memcpy(output, digits + 1, digits_count - 1), output += digits_count - 1;
        *output++ = 'e';
        if (exponent < 0) *output++ = '-', exponent = -exponent;
        output = std::to_chars(output, buffer + sizeof(buffer), exponent).ptr;
    }
    return {buffer, static_cast<std::size_t>(output - buffer)};
}

/** Prints @p value as JSON text into @p scratch: a scalar canonically, a container minified, nothing for a missing
 *  value. */
inline expected<value_view_t> json_text_of(json_cell_t const& value, ustore_arena_t scratch) noexcept {
    st::basic_vector<char, arena_allocator<char>> text {{scratch}};
    printed_number_buffer_t printed;
    status_t status;
    switch (value.token) {
    case json_token_t::missing_k: return value_view_t {};
    case json_token_t::null_k: status = append_text(text, "null"); break;
    case json_token_t::false_k: status = append_text(text, false_k); break;
    case json_token_t::true_k: status = append_text(text, true_k); break;
    case json_token_t::string_k: status = append_json_string(text, value.bytes); break;
    case json_token_t::object_k:
    case json_token_t::array_k: status = append_minified(text, value.bytes); break;
    case json_token_t::number_k:
        if (double const* const real = std::get_if<double>(&value.number))
            status = append_text(text, print_json_real(*real, printed));
        else if (std::int64_t const* const signed_integer = std::get_if<std::int64_t>(&value.number))
            status = append_text(text, print_number(printed, printed + sizeof(printed), *signed_integer));
        else
            status = append_text(
                text, print_number(printed, printed + sizeof(printed), std::get<std::uint64_t>(value.number)));
        break;
    default: return status_t {status_code_t::malformed_k, "Failed to parse the document"};
    }
    if (status.failed()) return status;
    return value_view_t {std::as_bytes(std::span<char const> {text})};
}

#pragma endregion Extraction

} // namespace unum::ustore::docs
