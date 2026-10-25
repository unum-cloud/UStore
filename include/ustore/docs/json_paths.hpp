/**
 *  @file include/ustore/docs/json_paths.hpp
 *  @author Ash Vardanian
 *  @date October 16, 2026
 *  @brief Document fields translated into JSONPath and located by StringZilla's subdocuments engine in one pass over
 *      a batch of documents, resolving into the values yyjson would have found.
 *
 *  A field is a JSON Pointer when it starts with a slash and a top-level key otherwise, and an empty field addresses
 *  the whole document. Each field compiles into a chain of JSONPath prefixes, `$['a']`, `$['a']['b']` and so on,
 *  where a token spelling an array index becomes the union `['0',0]`, as yyjson's pointer resolves it. One engine
 *  compiles the chains of every field of a call, and one `sz_subdocuments_find` pass over the documents fills a span
 *  table of the first match per document and path.
 *
 *  yyjson resolves a pointer through the first member with each key, so a field resolves to the first match of its
 *  last prefix only while that match lies inside the first match of every shorter prefix, which keeps duplicate keys
 *  resolving as they did. Scalars convert from their spans with yyjson's own number reader, strings unescape in one
 *  batch through `sz_strings_unescape_best`, and the result is a @c yyjson_val per cell, so the conversions of
 *  `ustore/docs/json_values.hpp` run unchanged.
 *
 *  Whatever the engine cannot read as yyjson does, a malformed batch, a selected value yyjson would refuse, or a layout
 *  it does not take, makes @c resolve_fields answer false, and the caller parses with yyjson instead, which reproduces
 *  its errors too. Lenient documents take that route: the engine refuses NaN and Infinity where it walks, and every
 *  comment, as it refuses a `/` outside strings even inside the containers it skips.
 */

#pragma once

#include <cstddef> // `std::size_t`, `std::max_align_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::memset`

#include <span>        // `std::span`
#include <string_view> // `std::string_view`

#include <yyjson.h> // `yyjson_val`, `yyjson_read_number`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`
#include <stringzilla/find.h>          // `sz_find_byteset_best`
#include <stringzilla/strings.h>       // `sz_strings_unescape_best`
#include <stringzilla/subdocuments.h>  // `sz_subdocuments_engine_init`, `sz_subdocuments_find`
#include <stringzilla/utf8_runes.h>    // `sz_utf8_count_strings_best`

#include "ustore/docs/json_values.hpp"      // `json_parse`
#include "ustore/utilities/ram_buffers.hpp" // `arena_allocate`, `arena_grow`, `arena_allocator`, `growing_strings`
#include "ustore/utilities/types.hpp"       // `value_view_t`, `byte_t`

namespace unum::ustore::docs {

using utilities::arena_allocator;
using utilities::bytes_of;
using utilities::growing_strings;
using utilities::growth_status;
using utilities::string_of;

#pragma region StringZilla Bridges

inline void* stringzilla_allocate(sz_size_t bytes, void* handle, sz_stream_t) noexcept {
    auto [block, status] = arena_allocate<byte_t>(static_cast<ustore_arena_t>(handle), bytes,
                                                  alignof(std::max_align_t));
    return status.failed() ? nullptr : block.data();
}

inline void* stringzilla_reallocate(void* pointer, sz_size_t old_bytes, sz_size_t new_bytes, void* handle,
                                    sz_stream_t) noexcept {
    if (new_bytes <= old_bytes) return pointer;
    std::span<byte_t> const old_block {static_cast<byte_t*>(pointer), old_bytes};
    auto [block, status] = arena_grow(static_cast<ustore_arena_t>(handle), old_block, new_bytes - old_bytes,
                                      alignof(std::max_align_t));
    return status.failed() ? nullptr : block.data();
}

/** Arena memory is released with the arena, never one block at a time. */
inline void stringzilla_free(void*, sz_size_t, void*, sz_stream_t) noexcept {}

/** A StringZilla allocator serving every block from @p arena, which frees them all when it rewinds. */
inline sz_allocator_t stringzilla_allocator(ustore_arena_t arena) noexcept {
    return {stringzilla_allocate, stringzilla_reallocate, stringzilla_free, arena};
}

/** Views @p text as StringZilla's pointer and length. */
inline sz_string_view_t stringzilla_view(std::string_view text) noexcept {
    sz_string_view_t view;
    view.start = text.data();
    view.length = text.size();
    return view;
}

/** The first byte of @p text inside @p set at or after @p offset, or the length of @p text without one, found with
 *  the StringZilla kernels @p capabilities allow. */
inline std::size_t find_byte_of(std::string_view text, std::size_t offset, sz_byteset_t const& set,
                                sz_capability_t capabilities) noexcept {
    sz_cptr_t match = nullptr;
    sz_find_byteset_best(text.data() + offset, text.size() - offset, &set, &match, capabilities, nullptr);
    return match ? static_cast<std::size_t>(match - text.data()) : text.size();
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

/** Whether @p token is an array index as yyjson's pointer reads one: `0`, or digits without a leading zero. */
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

/** Paths up to which a new one is compared against every earlier one, so shared prefixes compile once. */
constexpr std::size_t json_paths_deduplicated_k = 256;

/**
 *  @brief Appends to @p chain the indices into @p paths of the JSONPath prefixes @p field resolves through, one per
 *      token, or of the root alone for an empty field, adding to @p paths those it does not hold yet.
 *
 *  A field no document holds, an invalid JSON Pointer, appends nothing. @p path is the scratch each prefix grows in.
 */
inline status_t append_json_paths(std::string_view field, growing_strings<arena_allocator<byte_t>>& paths,
                                  st::basic_vector<char, arena_allocator<char>>& path,
                                  st::basic_vector<std::size_t, arena_allocator<std::size_t>>& chain) noexcept {
    if (status_t const status = growth_status(path.resize(0)); status.failed()) return status;
    if (status_t const status = append_to_path(path, "$"); status.failed()) return status;
    auto const push_path = [&]() noexcept -> status_t {
        std::string_view const spelled {path.begin(), path.end()};
        std::size_t const known = paths.size() <= json_paths_deduplicated_k ? paths.size() : 0;
        for (std::size_t index = 0; index != known; ++index)
            if (string_of(paths[index]) == spelled) return growth_status(chain.push_back(std::size_t {index}));
        if (status_t const status = paths.push_back(bytes_of(spelled)); status.failed()) return status;
        return growth_status(chain.push_back(paths.size() - 1));
    };

    if (field.empty()) return push_path();
    if (!field.starts_with('/')) {
        if (status_t const status = append_to_path(path, "["); status.failed()) return status;
        if (status_t const status = append_quoted_name(path, field, false); status.failed()) return status;
        if (status_t const status = append_to_path(path, "]"); status.failed()) return status;
        return push_path();
    }
    if (!is_valid_pointer(field)) return {};

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
        if (status_t const status = push_path(); status.failed()) return status;
        begin = end + 1;
    }
    return {};
}

#pragma endregion Field Translation

#pragma region Span Table

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

/** How a string body reads: whether it holds escapes, and whether yyjson would refuse its raw control bytes. */
enum class string_body_t { plain_k, escaped_k, refused_k };

/** Classifies the body of a string, between its quotes, with the StringZilla kernels @p capabilities allow. */
inline string_body_t classify_string_body(std::string_view body, sz_capability_t capabilities) noexcept {
    // Every control byte and `\`
    constexpr sz_byteset_t special_k {{0x00000000FFFFFFFFull, 0x0000000010000000ull, 0, 0}};
    string_body_t kind = string_body_t::plain_k;
    for (std::size_t position = find_byte_of(body, 0, special_k, capabilities); position != body.size();
         position = find_byte_of(body, position, special_k, capabilities)) {
        if (body[position] != '\\') return string_body_t::refused_k;
        kind = string_body_t::escaped_k;
        position += 2;
        if (position >= body.size()) break;
    }
    return kind;
}

/** Reads the number spelled by @p text into @p value with yyjson's own reader, answering whether all of it was one. */
inline bool read_json_number(std::string_view text, yyjson_val& value, ustore_arena_t scratch) noexcept {
    char local[64];
    char* terminated = local;
    if (text.size() >= sizeof(local)) {
        auto [block, status] = arena_allocate<char>(scratch, text.size() + 1);
        if (status.failed()) return false;
        terminated = block.data();
    }
    std::memcpy(terminated, text.data(), text.size());
    terminated[text.size()] = '\0';
    char const* const end = yyjson_read_number(terminated, &value, YYJSON_READ_ALLOW_INF_AND_NAN, nullptr, nullptr);
    return end == terminated + text.size();
}

/** Where the first match of one path lies in one document, with a zero length for none. */
struct json_span_t {
    std::size_t offset = 0;
    std::size_t length = 0;
};

/** Answers whether StringZilla's call succeeded. */
inline bool succeeded(sz_status_t status) noexcept { return status == sz_success_k; }

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

    /** Compiles the JSONPath @p paths with blocks of @p allocator for the kernels @p capabilities allow, answering
     *  whether they compiled. */
    bool compile(sz_strings_view_t const& paths, sz_allocator_t& allocator, sz_capability_t capabilities) noexcept {
        sz_size_t malformed_index = 0;
        compiled_ = succeeded(sz_subdocuments_engine_init(&engine_, &paths, sz_subdocuments_json_k, &malformed_index,
                                                          capabilities, &allocator, nullptr));
        return compiled_;
    }

    /** Finds every match in @p documents, as @c sz_subdocuments_find does. */
    sz_status_t find(sz_strings_view_t const& documents, std::span<sz_match_t> matches,
                     std::span<sz_size_t> offsets) noexcept {
        sz_size_t malformed_index = 0;
        return sz_subdocuments_find(&engine_, &documents, matches.data(), matches.size(), offsets.data(),
                                    &malformed_index, nullptr);
    }
};

/**
 *  @brief Resolves the value of every cell, as `json_lookup` over its parsed document would, through one subdocuments
 *      pass over @p documents.
 *
 *  @param[in] documents The documents the cells address, read in place.
 *  @param[in] fields The fields the cells address, each a JSON Pointer, a top-level key, or empty for the document.
 *  @param[in] cells_count How many cells there are.
 *  @param[in] cell_of Called as `cell_of(index)`, answering the @c field_cell_t of cell @p index.
 *  @param[in] containers_parsed Whether objects and arrays parse with yyjson, or only carry their type.
 *  @param[in] scratch Holds the engine, the span table and every value, until it rewinds.
 *  @param[in] capabilities The StringZilla kernels the running CPU allows, which `src/` probes once.
 *  @param[out] values One value per cell, @c nullptr where the field is missing.
 *  @return Whether every cell resolved; false asks the caller to parse the documents with yyjson instead.
 */
template <typename cell_of_type_>
bool resolve_fields(json_documents_t const& documents, std::span<std::string_view const> fields,
                    std::size_t cells_count, cell_of_type_ const& cell_of, bool containers_parsed,
                    ustore_arena_t scratch, sz_capability_t capabilities, std::span<yyjson_val*> values) noexcept {
    if (!cells_count) return true;
    if (documents.tape.layout != ustore_strings_u32_tape_k && documents.tape.layout != ustore_strings_u64_tape_k)
        return false;
    std::size_t const documents_count = documents.count;

    // Every field compiles into its chain of prefixes, one path per token, which fields sharing a prefix share
    auto [chains_begins, begins_status] = arena_allocate<std::size_t>(scratch, fields.size() + 1);
    if (begins_status.failed()) return false;
    growing_strings<arena_allocator<byte_t>> paths {scratch};
    st::basic_vector<char, arena_allocator<char>> path {{scratch}};
    st::basic_vector<std::size_t, arena_allocator<std::size_t>> chains {{scratch}};
    for (std::size_t field_index = 0; field_index != fields.size(); ++field_index) {
        chains_begins[field_index] = chains.size();
        if (append_json_paths(fields[field_index], paths, path, chains).failed()) return false;
    }
    chains_begins[fields.size()] = chains.size();
    std::size_t const paths_count = paths.size();

    // A dense table past a few spans per cell would cost more than parsing, as a read of many fields may ask
    if (documents_count * paths_count > 16 * (cells_count + paths_count) + 65536) return false;

    // The tape is read in place when every document is present, and through views of it otherwise, where missing
    // documents stand in as `null`, which no field but the root matches
    bool all_present = true;
    for (std::size_t document_index = 0; all_present && document_index != documents_count; ++document_index)
        all_present = !documents[document_index].empty();
    sz_strings_view_t walked {};
    walked.count = documents_count;
    if (all_present && documents.tape.layout == ustore_strings_u64_tape_k) {
        walked.layout = sz_strings_u64_tape_k;
        walked.data.u64_tape.data = reinterpret_cast<sz_cptr_t>(documents.tape.data.u64_tape.data);
        walked.data.u64_tape.offsets = documents.tape.data.u64_tape.offsets;
    }
    else if (all_present) {
        walked.layout = sz_strings_u32_tape_k;
        walked.data.u32_tape.data = reinterpret_cast<sz_cptr_t>(documents.tape.data.u32_tape.data);
        walked.data.u32_tape.offsets = documents.tape.data.u32_tape.offsets;
    }
    else {
        auto [views, views_status] = arena_allocate<sz_string_view_t>(scratch, documents_count);
        if (views_status.failed()) return false;
        constexpr std::string_view stand_in_k = "null";
        for (std::size_t document_index = 0; document_index != documents_count; ++document_index) {
            std::string_view const document = documents[document_index];
            views[document_index] = stringzilla_view(document.empty() ? stand_in_k : document);
        }
        sz_strings_from_string_views(views.data(), documents_count, &walked);
    }

    // One pass finds the first match of every path in every document, sized for one match per pair
    sz_allocator_t allocator = stringzilla_allocator(scratch);
    subdocuments_engine_t engine;
    ustore_strings_view_t const paths_tape = paths.tape();
    sz_strings_view_t paths_view {};
    paths_view.layout = sz_strings_u64_tape_k;
    paths_view.count = paths_count;
    paths_view.data.u64_tape.data = reinterpret_cast<sz_cptr_t>(paths_tape.data.u64_tape.data);
    paths_view.data.u64_tape.offsets = paths_tape.data.u64_tape.offsets;
    if (!engine.compile(paths_view, allocator, capabilities)) return false;

    auto [matches_offsets, offsets_status] = arena_allocate<sz_size_t>(scratch, documents_count + 1);
    if (offsets_status.failed()) return false;
    auto [matches, matches_status] = arena_allocate<sz_match_t>(scratch, documents_count * paths_count + 64);
    if (matches_status.failed()) return false;
    if (!succeeded(engine.find(walked, matches, matches_offsets))) return false;
    std::size_t const matches_count = matches_offsets[documents_count];
    if (matches_count > matches.size()) {
        auto [all_matches, all_status] = arena_allocate<sz_match_t>(scratch, matches_count);
        if (all_status.failed()) return false;
        matches = all_matches;
        if (!succeeded(engine.find(walked, matches, matches_offsets))) return false;
    }

    auto [table, table_status] = arena_allocate<json_span_t>(scratch, documents_count * paths_count);
    if (table_status.failed()) return false;
    std::memset(static_cast<void*>(table.data()), 0, table.size_bytes());
    for (sz_match_t const& match : matches.first(matches_count)) {
        json_span_t& first = table[match.haystack_index * paths_count + match.needle_index];
        if (!first.length) first = {match.byte_offset, match.byte_length};
    }

    // Scalars become values of their own, and strings view their bodies until the escaped ones unescape
    auto [synthetic, synthetic_status] = arena_allocate<yyjson_val>(scratch, cells_count);
    if (synthetic_status.failed()) return false;
    auto [bodies, bodies_status] = arena_allocate<sz_string_view_t>(scratch, cells_count);
    if (bodies_status.failed()) return false;
    auto [escaped_cells, escaped_status] = arena_allocate<std::size_t>(scratch, cells_count);
    if (escaped_status.failed()) return false;
    std::size_t bodies_count = 0, escaped_count = 0;

    for (std::size_t cell_index = 0; cell_index != cells_count; ++cell_index) {
        field_cell_t const cell = cell_of(cell_index);
        values[cell_index] = nullptr;
        if (!all_present && documents[cell.document].empty()) continue;

        std::size_t const* const chain = chains.data() + chains_begins[cell.field];
        std::size_t const chain_length = chains_begins[cell.field + 1] - chains_begins[cell.field];
        json_span_t const* const row = table.data() + cell.document * paths_count;
        if (!chain_length || !row[chain[0]].length) continue;
        json_span_t span = row[chain[0]];
        bool found = true;
        for (std::size_t link = 1; found && link != chain_length; ++link) {
            json_span_t const inner = row[chain[link]];
            found = inner.length && inner.offset < span.offset + span.length;
            span = inner;
        }
        if (!found) continue;

        std::string_view const text = documents[cell.document].substr(span.offset, span.length);
        yyjson_val& value = synthetic[cell_index];
        value = yyjson_val {};
        switch (text.front()) {
        case '{':
        case '[':
            if (containers_parsed) {
                auto [parsed, parsed_status] = json_parse(text, scratch);
                if (parsed_status.failed()) return false;
                values[cell_index] = yyjson_doc_get_root(parsed.handle);
                continue;
            }
            value.tag = text.front() == '{' ? YYJSON_TYPE_OBJ : YYJSON_TYPE_ARR;
            break;
        case '"': {
            if (text.size() < 2 || text.back() != '"') return false;
            std::string_view const body = text.substr(1, text.size() - 2);
            string_body_t const kind = classify_string_body(body, capabilities);
            if (kind == string_body_t::refused_k) return false;
            bodies[bodies_count++] = stringzilla_view(body);
            if (kind == string_body_t::escaped_k) escaped_cells[escaped_count++] = cell_index;
            yyjson_set_strn(&value, body.data(), body.size());
            break;
        }
        default:
            if (text == "true" || text == "false") yyjson_set_bool(&value, text == "true");
            else if (text == "null") yyjson_set_null(&value);
            else if (!read_json_number(text, value, scratch)) return false;
            break;
        }
        values[cell_index] = &value;
    }

    // yyjson refuses ill-formed UTF-8 in any string, escaped or not
    if (bodies_count) {
        sz_strings_view_t bodies_view {};
        sz_strings_from_string_views(bodies.data(), bodies_count, &bodies_view);
        sz_size_t malformed_index = 0;
        if (!succeeded(sz_utf8_count_strings_best(&bodies_view, nullptr, bodies_count, nullptr, &malformed_index,
                                                  capabilities, nullptr)))
            return false;
    }

    // The escaped strings unescape in one batch, and their values move onto the unescaped tape
    if (escaped_count) {
        for (std::size_t escaped_index = 0; escaped_index != escaped_count; ++escaped_index) {
            yyjson_val const& value = synthetic[escaped_cells[escaped_index]];
            bodies[escaped_index] = {yyjson_get_str(&value), yyjson_get_len(&value)};
        }
        sz_strings_view_t escaped_view {};
        sz_strings_from_string_views(bodies.data(), escaped_count, &escaped_view);
        sz_strings_span_t unescaped {};
        sz_size_t malformed_index = 0;
        if (!succeeded(sz_strings_unescape_best(&escaped_view, nullptr, escaped_count, sz_escape_json_k, &allocator,
                                                &unescaped, &malformed_index, capabilities, nullptr)))
            return false;
        for (std::size_t escaped_index = 0; escaped_index != escaped_count; ++escaped_index) {
            std::size_t begin = 0, end = 0;
            char const* data = nullptr;
            if (unescaped.layout == sz_strings_u64_tape_k) {
                data = unescaped.data.u64_tape.data;
                begin = unescaped.data.u64_tape.offsets[escaped_index];
                end = unescaped.data.u64_tape.offsets[escaped_index + 1];
            }
            else {
                data = unescaped.data.u32_tape.data;
                begin = unescaped.data.u32_tape.offsets[escaped_index];
                end = unescaped.data.u32_tape.offsets[escaped_index + 1];
            }
            yyjson_set_strn(&synthetic[escaped_cells[escaped_index]], data + begin, end - begin);
        }
    }
    return true;
}

#pragma endregion Span Table

} // namespace unum::ustore::docs
