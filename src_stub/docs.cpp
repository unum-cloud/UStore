/**
 *  @file src_stub/docs.cpp
 *  @author Ash Vardanian
 *  @date November 16, 2026
 *  @brief The documents calls over the store of @c lib.hpp, with a strict RFC 8259 tree, RFC 6901
 *      pointers, RFC 6902 patches, RFC 7386 merges and a JSONPath subset of its own, as the engine
 *      reads them through yyjson.
 *
 *  A whole JSON upsert stores the bytes as written once they parse, and every other write parses,
 *  modifies and prints the document compactly. The filters of scans and cursors answer a JSONPath
 *  of dot and bracket members, indices, wildcards, recursive descent and filters comparing a
 *  relative path with a literal, or a bare root filter.
 */
#include <cmath> // `std::isfinite`, `std::ldexp`

#include <charconv> // `std::from_chars`, `std::to_chars`
#include <limits>   // `std::numeric_limits`

#include "keyed.hpp"

using namespace unum::ustore::stub;

namespace unum::ustore::stub {

#pragma region Tree

struct json_member_t;

/** One JSON value, its members kept in the order they were written. */
struct json_t {
    enum class kind_t : std::uint8_t { null_k, boolean_k, number_k, string_k, array_k, object_k };
    enum class number_t : std::uint8_t { unsigned_k, signed_k, real_k };

    kind_t kind = kind_t::null_k;
    bool boolean = false;
    number_t number = number_t::unsigned_k;
    std::uint64_t unsigned_value = 0;
    std::int64_t signed_value = 0;
    double real_value = 0;
    std::string text;
    std::vector<json_t> items;
    std::vector<json_member_t> members;

    bool is_object() const noexcept { return kind == kind_t::object_k; }
    bool is_array() const noexcept { return kind == kind_t::array_k; }

    json_t* member(std::string_view name) noexcept;
    json_t const* member(std::string_view name) const noexcept { return const_cast<json_t*>(this)->member(name); }
    double as_real() const noexcept {
        switch (number) {
        case number_t::unsigned_k: return static_cast<double>(unsigned_value);
        case number_t::signed_k: return static_cast<double>(signed_value);
        default: return real_value;
        }
    }
};

/** One member of an object: its key and its value. */
struct json_member_t {
    std::string first;
    json_t second;
};

json_t* json_t::member(std::string_view name) noexcept {
    for (auto& [key, value] : members)
        if (key == name) return &value;
    return nullptr;
}

inline bool operator==(json_t const& first, json_t const& second) noexcept {
    if (first.kind != second.kind) return false;
    switch (first.kind) {
    case json_t::kind_t::null_k: return true;
    case json_t::kind_t::boolean_k: return first.boolean == second.boolean;
    case json_t::kind_t::number_k:
        if (first.number != json_t::number_t::real_k && second.number != json_t::number_t::real_k) {
            bool const first_negative = first.number == json_t::number_t::signed_k && first.signed_value < 0;
            bool const second_negative = second.number == json_t::number_t::signed_k && second.signed_value < 0;
            if (first_negative != second_negative) return false;
            auto const magnitude = [](json_t const& value) {
                return value.number == json_t::number_t::unsigned_k ? value.unsigned_value
                                                                    : static_cast<std::uint64_t>(value.signed_value);
            };
            return magnitude(first) == magnitude(second);
        }
        return first.as_real() == second.as_real();
    case json_t::kind_t::string_k: return first.text == second.text;
    case json_t::kind_t::array_k: return first.items == second.items;
    case json_t::kind_t::object_k:
        if (first.members.size() != second.members.size()) return false;
        for (auto const& [key, value] : first.members) {
            json_t const* const other = second.member(key);
            if (!other || !(*other == value)) return false;
        }
        return true;
    }
    return false;
}

json_t make_unsigned(std::uint64_t value) {
    json_t json;
    json.kind = json_t::kind_t::number_k, json.number = json_t::number_t::unsigned_k, json.unsigned_value = value;
    return json;
}
json_t make_signed(std::int64_t value) {
    if (value >= 0) return make_unsigned(static_cast<std::uint64_t>(value));
    json_t json;
    json.kind = json_t::kind_t::number_k, json.number = json_t::number_t::signed_k, json.signed_value = value;
    return json;
}
json_t make_real(double value) {
    json_t json;
    json.kind = json_t::kind_t::number_k, json.number = json_t::number_t::real_k, json.real_value = value;
    return json;
}
json_t make_string(std::string_view value) {
    json_t json;
    json.kind = json_t::kind_t::string_k, json.text.assign(value);
    return json;
}

/** The boolean JSON spells as @p spelling, "true" or "false". */
json_t make_boolean(std::string_view spelling) {
    json_t json;
    json.kind = json_t::kind_t::boolean_k, json.boolean = spelling == "true";
    return json;
}
#pragma endregion Tree

#pragma region Parsing

/** A strict RFC 8259 reader: no comments, no trailing commas, no non-finite literals. */
class json_parser_t {
    std::string_view text_;
    std::size_t at_ = 0;
    std::size_t depth_ = 0;

    void skip() noexcept {
        while (at_ < text_.size() &&
               (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\n' || text_[at_] == '\r'))
            ++at_;
    }
    bool literal(std::string_view word) noexcept {
        if (text_.substr(at_, word.size()) != word) return false;
        at_ += word.size();
        return true;
    }
    static void append_utf8(std::string& out, std::uint32_t code) {
        if (code < 0x80) out.push_back(static_cast<char>(code));
        else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    bool hex4(std::uint32_t& code) noexcept {
        if (at_ + 4 > text_.size()) return false;
        code = 0;
        for (std::size_t index = 0; index != 4; ++index) {
            char const digit = text_[at_++];
            code <<= 4;
            if (digit >= '0' && digit <= '9') code |= static_cast<std::uint32_t>(digit - '0');
            else if (digit >= 'a' && digit <= 'f') code |= static_cast<std::uint32_t>(digit - 'a' + 10);
            else if (digit >= 'A' && digit <= 'F') code |= static_cast<std::uint32_t>(digit - 'A' + 10);
            else return false;
        }
        return true;
    }

    /** Reads the code point of a @c \\u escape, pairing surrogates, and appends it as UTF-8. */
    bool code_point(std::string& out) {
        std::uint32_t code = 0;
        if (!hex4(code)) return false;
        if (code >= 0xDC00 && code < 0xE000) return false;
        if (code >= 0xD800 && code < 0xDC00) {
            std::uint32_t low = 0;
            if (!literal("\\u") || !hex4(low) || low < 0xDC00 || low >= 0xE000) return false;
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
        }
        append_utf8(out, code);
        return true;
    }
    bool string(std::string& out) {
        if (at_ >= text_.size() || text_[at_] != '"') return false;
        ++at_;
        while (at_ < text_.size()) {
            char const character = text_[at_++];
            if (character == '"') return true;
            if (static_cast<unsigned char>(character) < 0x20) return false;
            if (character != '\\') {
                out.push_back(character);
                continue;
            }
            if (at_ >= text_.size()) return false;
            switch (text_[at_++]) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u':
                if (!code_point(out)) return false;
                break;
            default: return false;
            }
        }
        return false;
    }
    bool number(json_t& out) {
        std::size_t const begin = at_;
        if (at_ < text_.size() && text_[at_] == '-') ++at_;
        if (at_ >= text_.size()) return false;
        if (text_[at_] == '0') ++at_;
        else if (text_[at_] >= '1' && text_[at_] <= '9')
            while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') ++at_;
        else return false;
        bool real = false;
        if (at_ < text_.size() && text_[at_] == '.') {
            real = true, ++at_;
            std::size_t const digits = at_;
            while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') ++at_;
            if (at_ == digits) return false;
        }
        if (at_ < text_.size() && (text_[at_] == 'e' || text_[at_] == 'E')) {
            real = true, ++at_;
            if (at_ < text_.size() && (text_[at_] == '+' || text_[at_] == '-')) ++at_;
            std::size_t const digits = at_;
            while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') ++at_;
            if (at_ == digits) return false;
        }
        std::string_view const spelling = text_.substr(begin, at_ - begin);
        out.kind = json_t::kind_t::number_k;
        char const* const first = spelling.data();
        char const* const last = first + spelling.size();
        if (!real) {
            if (spelling.front() == '-') {
                std::int64_t value = 0;
                if (std::from_chars(first, last, value).ec == std::errc()) {
                    out = make_signed(value);
                    return true;
                }
            }
            else {
                std::uint64_t value = 0;
                if (std::from_chars(first, last, value).ec == std::errc()) {
                    out = make_unsigned(value);
                    return true;
                }
            }
        }
        double value = 0;
        auto const [end, error] = std::from_chars(first, last, value);
        if (end != last || (error != std::errc() && error != std::errc::result_out_of_range)) return false;
        if (!std::isfinite(value)) return false;
        out = make_real(value);
        return true;
    }
    bool value(json_t& out) {
        if (++depth_ > 1024) return false;
        skip();
        if (at_ >= text_.size()) return false;
        bool parsed = false;
        switch (text_[at_]) {
        case 'n': parsed = literal("null"), out.kind = json_t::kind_t::null_k; break;
        case 't': parsed = literal("true"), out = make_boolean("true"); break;
        case 'f': parsed = literal("false"), out = make_boolean("false"); break;
        case '"': out.kind = json_t::kind_t::string_k, parsed = string(out.text); break;
        case '[': {
            ++at_, out.kind = json_t::kind_t::array_k;
            skip();
            if (at_ < text_.size() && text_[at_] == ']') {
                ++at_, parsed = true;
                break;
            }
            while (true) {
                out.items.emplace_back();
                if (!value(out.items.back())) return false;
                skip();
                if (at_ >= text_.size()) return false;
                if (text_[at_] == ',') {
                    ++at_;
                    continue;
                }
                if (text_[at_] != ']') return false;
                ++at_, parsed = true;
                break;
            }
            break;
        }
        case '{': {
            ++at_, out.kind = json_t::kind_t::object_k;
            skip();
            if (at_ < text_.size() && text_[at_] == '}') {
                ++at_, parsed = true;
                break;
            }
            while (true) {
                skip();
                std::string key;
                if (!string(key)) return false;
                skip();
                if (at_ >= text_.size() || text_[at_] != ':') return false;
                ++at_;
                out.members.emplace_back(std::move(key), json_t {});
                if (!value(out.members.back().second)) return false;
                skip();
                if (at_ >= text_.size()) return false;
                if (text_[at_] == ',') {
                    ++at_;
                    continue;
                }
                if (text_[at_] != '}') return false;
                ++at_, parsed = true;
                break;
            }
            break;
        }
        default: parsed = number(out); break;
        }
        --depth_;
        return parsed;
    }

  public:
    explicit json_parser_t(std::string_view text) noexcept : text_(text) {}

    /** Parses the whole text as one value, answering false for anything else. */
    bool parse(json_t& out) {
        if (!value(out)) return false;
        skip();
        return at_ == text_.size();
    }
};

/** Parses @p text into @p out, or refuses it malformed. */
status_t parse_json(std::string_view text, json_t& out) {
    out = {};
    if (text.empty()) return {ustore_status_malformed_k, "An empty value is no JSON document"};
    if (!json_parser_t(text).parse(out)) return {ustore_status_malformed_k, "Failed to parse the document"};
    return {};
}

#pragma endregion Parsing

#pragma region Printing

void print_string(std::string& out, std::string_view text) {
    out.push_back('"');
    for (char const character : text) {
        switch (character) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20) {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(character));
                out += escaped;
            }
            else out.push_back(character);
        }
    }
    out.push_back('"');
}

/** Prints @p real as yyjson does: the shortest spelling that reads back, with a fraction
 *  or an exponent. */
std::string print_real(double real) {
    char buffer[64];
    auto const [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), real);
    std::string spelling(buffer, error == std::errc() ? end : buffer);
    if (auto const plus = spelling.find("e+"); plus != std::string::npos) spelling.erase(plus + 1, 1);
    if (spelling.find_first_of(".e") == std::string::npos) spelling += ".0";
    return spelling;
}

/** Prints @p json compactly, refusing a non-finite number, which JSON cannot spell. */
bool print_json(json_t const& json, std::string& out) {
    switch (json.kind) {
    case json_t::kind_t::null_k: out += "null"; return true;
    case json_t::kind_t::boolean_k: out += json.boolean ? "true" : "false"; return true;
    case json_t::kind_t::number_k:
        switch (json.number) {
        case json_t::number_t::unsigned_k: out += std::to_string(json.unsigned_value); return true;
        case json_t::number_t::signed_k: out += std::to_string(json.signed_value); return true;
        default:
            if (!std::isfinite(json.real_value)) return false;
            out += print_real(json.real_value);
            return true;
        }
    case json_t::kind_t::string_k: print_string(out, json.text); return true;
    case json_t::kind_t::array_k:
        out.push_back('[');
        for (std::size_t index = 0; index != json.items.size(); ++index) {
            if (index) out.push_back(',');
            if (!print_json(json.items[index], out)) return false;
        }
        out.push_back(']');
        return true;
    case json_t::kind_t::object_k:
        out.push_back('{');
        for (std::size_t index = 0; index != json.members.size(); ++index) {
            if (index) out.push_back(',');
            print_string(out, json.members[index].first);
            out.push_back(':');
            if (!print_json(json.members[index].second, out)) return false;
        }
        out.push_back('}');
        return true;
    }
    return false;
}

status_t dump_json(json_t const& json, std::string& out) {
    out.clear();
    if (!print_json(json, out)) return {ustore_status_malformed_k, "JSON spells no NaN or Infinity"};
    return {};
}

#pragma endregion Printing

#pragma region Pointers

/** The reference tokens of an RFC 6901 pointer, unescaped, or false for a malformed one. */
bool pointer_tokens(std::string_view pointer, std::vector<std::string>& tokens) {
    tokens.clear();
    if (pointer.empty()) return true;
    if (pointer.front() != '/') return false;
    pointer.remove_prefix(1);
    while (true) {
        std::size_t const slash = pointer.find('/');
        std::string_view const raw = pointer.substr(0, slash);
        std::string token;
        for (std::size_t index = 0; index != raw.size(); ++index) {
            if (raw[index] != '~') {
                token.push_back(raw[index]);
                continue;
            }
            if (index + 1 == raw.size() || (raw[index + 1] != '0' && raw[index + 1] != '1')) return false;
            token.push_back(raw[++index] == '0' ? '~' : '/');
        }
        tokens.push_back(std::move(token));
        if (slash == std::string_view::npos) return true;
        pointer.remove_prefix(slash + 1);
    }
}

/** The tokens a field addresses: a JSON Pointer when it starts with a slash, and one
 *  top-level key otherwise. */
bool field_tokens(std::string_view field, std::vector<std::string>& tokens) {
    if (field.empty() || field.front() == '/') return pointer_tokens(field, tokens);
    tokens.assign(1, std::string(field));
    return true;
}

/** The array index @p token spells, or false. */
bool array_index(std::string const& token, std::size_t& index) noexcept {
    if (token.empty() || (token.size() > 1 && token.front() == '0')) return false;
    auto const [end, error] = std::from_chars(token.data(), token.data() + token.size(), index);
    return error == std::errc() && end == token.data() + token.size();
}

json_t* resolve(json_t& root, std::vector<std::string> const& tokens, std::size_t count) {
    json_t* node = &root;
    for (std::size_t position = 0; position != count; ++position) {
        std::string const& token = tokens[position];
        if (node->is_object()) node = node->member(token);
        else if (node->is_array()) {
            std::size_t index = 0;
            if (!array_index(token, index) || index >= node->items.size()) return nullptr;
            node = &node->items[index];
        }
        else return nullptr;
        if (!node) return nullptr;
    }
    return node;
}

json_t const* lookup(json_t const& root, std::string_view field) {
    std::vector<std::string> tokens;
    if (!field_tokens(field, tokens)) return nullptr;
    return resolve(const_cast<json_t&>(root), tokens, tokens.size());
}

/** Whether a modification creates the objects missing on its path, as a set does, or needs them,
 *  as a patch does. */
enum class parents_t : bool { required_k, created_k };

/** The element of @p array its @p token indexes, or null for a scalar, an object or an index past
 *  its end. */
json_t* array_element(json_t& array, std::string const& token) noexcept {
    std::size_t index = 0;
    if (!array.is_array() || !array_index(token, index) || index >= array.items.size()) return nullptr;
    return &array.items[index];
}

/** The member @p token of @p object, created empty when missing and @p parents allow. */
json_t* member_or_created(json_t& object, std::string const& token, parents_t parents) {
    if (json_t* const found = object.member(token)) return found;
    if (parents != parents_t::created_k) return nullptr;
    object.members.emplace_back(token, json_t {});
    json_t* const created = &object.members.back().second;
    created->kind = json_t::kind_t::object_k;
    return created;
}

/** Where a modification of @p tokens lands: the parent, created as objects where missing when @p
 *  create, and the last token. */
json_t* parent_of(json_t& root, std::vector<std::string> const& tokens, parents_t parents) {
    bool const create = parents == parents_t::created_k;
    json_t* node = &root;
    for (std::size_t position = 0; position + 1 < tokens.size(); ++position) {
        std::string const& token = tokens[position];
        if (node->kind == json_t::kind_t::null_k && create) node->kind = json_t::kind_t::object_k;
        node = node->is_object() ? member_or_created(*node, token, parents) : array_element(*node, token);
        if (!node) return nullptr;
    }
    return node;
}

/** Adds @p value at @p tokens as RFC 6902 does, inserting into arrays, creating missing parents
 *  when @p create. */
bool pointer_add(json_t& root, std::vector<std::string> const& tokens, json_t value, parents_t parents) {
    bool const create = parents == parents_t::created_k;
    if (tokens.empty()) {
        root = std::move(value);
        return true;
    }
    json_t* const parent = parent_of(root, tokens, parents);
    if (!parent) return false;
    std::string const& last = tokens.back();
    if (parent->kind == json_t::kind_t::null_k && create) parent->kind = json_t::kind_t::object_k;
    if (parent->is_object()) {
        if (json_t* const existing = parent->member(last)) *existing = std::move(value);
        else parent->members.emplace_back(last, std::move(value));
        return true;
    }
    if (!parent->is_array()) return false;
    if (last == "-") {
        parent->items.push_back(std::move(value));
        return true;
    }
    std::size_t index = 0;
    if (!array_index(last, index) || index > parent->items.size()) return false;
    parent->items.insert(parent->items.begin() + static_cast<std::ptrdiff_t>(index), std::move(value));
    return true;
}

/** Sets @p value at @p tokens, replacing an array element in place, creating missing parents. */
bool pointer_set(json_t& root, std::vector<std::string> const& tokens, json_t value) {
    if (tokens.empty()) {
        root = std::move(value);
        return true;
    }
    if (json_t* const existing = resolve(root, tokens, tokens.size())) {
        *existing = std::move(value);
        return true;
    }
    return pointer_add(root, tokens, std::move(value), parents_t::created_k);
}

bool pointer_replace(json_t& root, std::vector<std::string> const& tokens, json_t value) {
    json_t* const existing = resolve(root, tokens, tokens.size());
    if (!existing) return false;
    *existing = std::move(value);
    return true;
}

/** Erases the first member @p name of @p object, moving its value into @p removed when given. */
bool erase_member(json_t& object, std::string const& name, json_t* removed = nullptr) {
    for (auto it = object.members.begin(); it != object.members.end(); ++it) {
        if (it->first != name) continue;
        if (removed) *removed = std::move(it->second);
        object.members.erase(it);
        return true;
    }
    return false;
}

bool pointer_remove(json_t& root, std::vector<std::string> const& tokens, json_t* removed = nullptr) {
    if (tokens.empty()) return false;
    json_t* const parent = resolve(root, tokens, tokens.size() - 1);
    if (!parent) return false;
    std::string const& last = tokens.back();
    if (parent->is_object()) return erase_member(*parent, last, removed);
    if (!parent->is_array()) return false;
    std::size_t index = 0;
    if (!array_index(last, index) || index >= parent->items.size()) return false;
    if (removed) *removed = std::move(parent->items[index]);
    parent->items.erase(parent->items.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

#pragma endregion Pointers

#pragma region Patches

/** Applies an RFC 7386 merge patch to @p target. */
void merge_patch(json_t& target, json_t const& patch) {
    if (!patch.is_object()) {
        target = patch;
        return;
    }
    if (!target.is_object()) target = {}, target.kind = json_t::kind_t::object_k;
    for (auto const& [key, value] : patch.members) {
        if (value.kind == json_t::kind_t::null_k) {
            erase_member(target, key);
            continue;
        }
        json_t* existing = target.member(key);
        if (!existing) {
            target.members.emplace_back(key, json_t {});
            existing = &target.members.back().second;
        }
        merge_patch(*existing, value);
    }
}

/** Applies an RFC 6902 patch to @p target, answering false where an operation does not apply. */
bool json_patch(json_t& target, json_t const& patch) {
    if (!patch.is_array()) return false;
    json_t result = target;
    for (json_t const& operation : patch.items) {
        if (!operation.is_object()) return false;
        json_t const* const op = operation.member("op");
        json_t const* const path = operation.member("path");
        if (!op || op->kind != json_t::kind_t::string_k || !path || path->kind != json_t::kind_t::string_k)
            return false;
        std::vector<std::string> tokens;
        if (!pointer_tokens(path->text, tokens)) return false;
        json_t const* const value = operation.member("value");
        json_t const* const from = operation.member("from");
        std::vector<std::string> from_tokens;
        if (from && (from->kind != json_t::kind_t::string_k || !pointer_tokens(from->text, from_tokens))) return false;
        if (op->text == "add") {
            if (!value || !pointer_add(result, tokens, *value, parents_t::required_k)) return false;
        }
        else if (op->text == "remove") {
            if (!pointer_remove(result, tokens)) return false;
        }
        else if (op->text == "replace") {
            if (!value || !pointer_replace(result, tokens, *value)) return false;
        }
        else if (op->text == "move") {
            json_t moved;
            if (!from || !pointer_remove(result, from_tokens, &moved) ||
                !pointer_add(result, tokens, std::move(moved), parents_t::required_k))
                return false;
        }
        else if (op->text == "copy") {
            json_t const* const source = from ? resolve(result, from_tokens, from_tokens.size()) : nullptr;
            if (!source) return false;
            json_t copied = *source;
            if (!pointer_add(result, tokens, std::move(copied), parents_t::required_k)) return false;
        }
        else if (op->text == "test") {
            json_t const* const tested = resolve(result, tokens, tokens.size());
            if (!value || !tested || !(*tested == *value)) return false;
        }
        else return false;
    }
    target = std::move(result);
    return true;
}

#pragma endregion Patches

#pragma region Conversions

/** How a value converts into a column or a read: missing, as it is, changed in meaning, or
 *  not at all. */
enum class conversion_t { missing_k, exact_k, converted_k, collides_k };

template <typename scalar_type_>
bool parse_entire(std::string_view text, scalar_type_& scalar) noexcept {
    if constexpr (std::is_same_v<scalar_type_, bool>) {
        if (text != "true" && text != "false") return false;
        scalar = text == "true";
        return true;
    }
    else {
        auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), scalar);
        return error == std::errc() && end == text.data() + text.size();
    }
}

template <typename scalar_type_>
bool real_fits(double real) noexcept {
    if constexpr (std::is_same_v<scalar_type_, bool>) return true;
    else if constexpr (std::is_floating_point_v<scalar_type_>)
        return !std::isfinite(real) ||
               std::abs(real) <= static_cast<double>((std::numeric_limits<scalar_type_>::max)());
    else if constexpr (std::is_signed_v<scalar_type_>) {
        double const bound = std::ldexp(1.0, std::numeric_limits<scalar_type_>::digits);
        return real >= -bound && real < bound;
    }
    else return real > -1.0 && real < std::ldexp(1.0, std::numeric_limits<scalar_type_>::digits);
}

template <typename scalar_type_, typename integer_type_>
bool integer_fits(integer_type_ integer) noexcept {
    if constexpr (std::is_same_v<scalar_type_, bool> || std::is_floating_point_v<scalar_type_>) return true;
    else return std::in_range<scalar_type_>(integer);
}

/** Converts @p value, null when missing, into @p scalar as the engine's gathers do. */
template <typename scalar_type_>
conversion_t to_scalar(json_t const* value, scalar_type_& scalar) noexcept {
    if (!value || value->kind == json_t::kind_t::null_k) return conversion_t::missing_k;
    switch (value->kind) {
    case json_t::kind_t::boolean_k:
        scalar = static_cast<scalar_type_>(value->boolean);
        return std::is_same_v<scalar_type_, bool> ? conversion_t::exact_k : conversion_t::converted_k;
    case json_t::kind_t::string_k:
        return parse_entire(value->text, scalar) ? conversion_t::converted_k : conversion_t::collides_k;
    case json_t::kind_t::number_k:
        switch (value->number) {
        case json_t::number_t::real_k:
            if (!real_fits<scalar_type_>(value->real_value)) return conversion_t::collides_k;
            scalar = static_cast<scalar_type_>(value->real_value);
            return std::is_floating_point_v<scalar_type_> ? conversion_t::exact_k : conversion_t::converted_k;
        case json_t::number_t::signed_k:
            if (!integer_fits<scalar_type_>(value->signed_value)) return conversion_t::collides_k;
            scalar = static_cast<scalar_type_>(value->signed_value);
            return std::is_integral_v<scalar_type_> && std::is_signed_v<scalar_type_> ? conversion_t::exact_k
                                                                                      : conversion_t::converted_k;
        default:
            if (!integer_fits<scalar_type_>(value->unsigned_value)) return conversion_t::collides_k;
            scalar = static_cast<scalar_type_>(value->unsigned_value);
            return std::is_unsigned_v<scalar_type_> && !std::is_same_v<scalar_type_, bool> ? conversion_t::exact_k
                                                                                           : conversion_t::converted_k;
        }
    default: return conversion_t::collides_k;
    }
}

/** Converts @p value, null when missing, into the text a string read or column prints. */
conversion_t to_text(json_t const* value, std::string& text) {
    if (!value || value->kind == json_t::kind_t::null_k) return conversion_t::missing_k;
    switch (value->kind) {
    case json_t::kind_t::boolean_k: text = value->boolean ? "true" : "false"; return conversion_t::converted_k;
    case json_t::kind_t::string_k: text = value->text; return conversion_t::exact_k;
    case json_t::kind_t::number_k:
        text.clear();
        print_json(*value, text);
        // A real prints in its shortest spelling, which a string column keeps without
        // the forced fraction
        if (value->number == json_t::number_t::real_k && text.ends_with(".0")) text.resize(text.size() - 2);
        return conversion_t::converted_k;
    default: return conversion_t::collides_k;
    }
}

/** Reads the 16 bytes a canonical UUID spells, or false. */
bool parse_uuid(std::string_view text, std::uint8_t (&bytes)[16]) noexcept {
    if (text.size() != 36) return false;
    std::size_t out = 0;
    for (std::size_t index = 0; index < text.size();) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (text[index] != '-') return false;
            ++index;
            continue;
        }
        auto const nibble = [](char digit) -> int {
            if (digit >= '0' && digit <= '9') return digit - '0';
            if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
            if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
            return -1;
        };
        int const high = nibble(text[index]), low = nibble(text[index + 1]);
        if (high < 0 || low < 0) return false;
        bytes[out++] = static_cast<std::uint8_t>(high * 16 + low);
        index += 2;
    }
    return out == 16;
}

/** The IEEE half-precision bits closest to @p value. */
std::uint16_t to_half(float value) noexcept {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    std::uint32_t const sign = (bits >> 16) & 0x8000u;
    std::int32_t exponent = static_cast<std::int32_t>((bits >> 23) & 0xFF) - 127 + 15;
    std::uint32_t mantissa = bits & 0x7FFFFFu;
    if (((bits >> 23) & 0xFF) == 0xFF) return static_cast<std::uint16_t>(sign | 0x7C00u | (mantissa ? 0x200u : 0));
    if (exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7C00u);
    if (exponent <= 0) {
        if (exponent < -10) return static_cast<std::uint16_t>(sign);
        mantissa |= 0x800000u;
        std::uint32_t const shift = static_cast<std::uint32_t>(14 - exponent);
        std::uint32_t half = mantissa >> shift;
        if ((mantissa >> (shift - 1)) & 1u) ++half;
        return static_cast<std::uint16_t>(sign | half);
    }
    std::uint32_t half = sign | (static_cast<std::uint32_t>(exponent) << 10) | (mantissa >> 13);
    if (mantissa & 0x1000u) ++half;
    return static_cast<std::uint16_t>(half);
}

/** Wraps the fixed-width scalar @p bytes hold into a value, or false for too few bytes. */
template <typename scalar_type_>
bool scalar_value(std::string_view bytes, json_t& out) noexcept {
    if (bytes.size() < sizeof(scalar_type_)) return false;
    scalar_type_ scalar {};
    std::memcpy(&scalar, bytes.data(), sizeof(scalar_type_));
    if constexpr (std::is_same_v<scalar_type_, bool>) out = make_boolean(scalar ? "true" : "false");
    else if constexpr (std::is_floating_point_v<scalar_type_>) out = make_real(scalar);
    else if constexpr (std::is_signed_v<scalar_type_>) out = make_signed(scalar);
    else out = make_unsigned(scalar);
    return true;
}

/** Reads a value written as @p type into a tree, as the engine's writes do; JSON parses,
 *  and scalars wrap. */
status_t value_of_type(std::string_view bytes, ustore_docs_field_type_t type, json_t& out) {
    if (type == ustore_docs_field_type_json_k) return parse_json(bytes, out);
    out = {};
    bool made = false;
    switch (type) {
    case ustore_docs_field_type_null_k: made = true; break;
    case ustore_docs_field_type_bool_k: made = scalar_value<bool>(bytes, out); break;
    case ustore_docs_field_type_f64_k: made = scalar_value<double>(bytes, out); break;
    case ustore_docs_field_type_f32_k: made = scalar_value<float>(bytes, out); break;
    case ustore_docs_field_type_i64_k: made = scalar_value<std::int64_t>(bytes, out); break;
    case ustore_docs_field_type_i32_k: made = scalar_value<std::int32_t>(bytes, out); break;
    case ustore_docs_field_type_i16_k: made = scalar_value<std::int16_t>(bytes, out); break;
    case ustore_docs_field_type_i8_k: made = scalar_value<std::int8_t>(bytes, out); break;
    case ustore_docs_field_type_u64_k: made = scalar_value<std::uint64_t>(bytes, out); break;
    case ustore_docs_field_type_u32_k: made = scalar_value<std::uint32_t>(bytes, out); break;
    case ustore_docs_field_type_u16_k: made = scalar_value<std::uint16_t>(bytes, out); break;
    case ustore_docs_field_type_u8_k: made = scalar_value<std::uint8_t>(bytes, out); break;
    case ustore_docs_field_type_string_k: out = make_string(bytes), made = true; break;
    default: break;
    }
    if (!made) return {ustore_status_unsupported_k, "Input type is not supported"};
    return {};
}

#pragma endregion Conversions

#pragma region Filters

/** One step of a JSONPath: a member, an index, a wildcard, a recursive descent or a filter. */
struct path_step_t {
    enum class kind_t : std::uint8_t { member_k, index_k, wildcard_k, descendants_k, filter_k };
    kind_t kind = kind_t::member_k;
    std::string name;
    std::int64_t index = 0;

    /** For a recursive descent, what it selects among the descendants; for a
     *  filter, the expression. */
    std::shared_ptr<path_step_t> inner;
    std::shared_ptr<struct filter_t> filter;
};

/** A filter expression over the current node @c @. */
struct filter_t {
    enum class kind_t : std::uint8_t { and_k, or_k, not_k, exists_k, compare_k, literal_k };
    kind_t kind = kind_t::exists_k;
    std::shared_ptr<filter_t> left, right;
    std::vector<path_step_t> path;
    std::string op;
    json_t literal;
    bool literal_right = true;
};

/** Whether a path starts at the root @c $ or at the current node @c @, which a
 *  filter's comparison reads. */
enum class path_start_t : bool { root_k, current_k };

/** Parses the JSONPath subset into steps, refusing anything it does not spell. */
class path_parser_t {
    std::string_view text_;
    std::size_t at_ = 0;

    void skip() noexcept {
        while (at_ < text_.size() && text_[at_] == ' ') ++at_;
    }
    bool eat(std::string_view token) noexcept {
        skip();
        if (text_.substr(at_, token.size()) != token) return false;
        at_ += token.size();
        return true;
    }
    bool name(std::string& out) {
        std::size_t const begin = at_;
        while (at_ < text_.size() && (std::isalnum(static_cast<unsigned char>(text_[at_])) || text_[at_] == '_' ||
                                      text_[at_] == '-' || static_cast<unsigned char>(text_[at_]) >= 0x80))
            ++at_;
        out.assign(text_.substr(begin, at_ - begin));
        return !out.empty();
    }
    bool quoted(std::string& out) {
        skip();
        if (at_ >= text_.size() || (text_[at_] != '\'' && text_[at_] != '"')) return false;
        char const quote = text_[at_++];
        while (at_ < text_.size() && text_[at_] != quote) {
            if (text_[at_] == '\\' && at_ + 1 < text_.size()) ++at_;
            out.push_back(text_[at_++]);
        }
        if (at_ >= text_.size()) return false;
        ++at_;
        return true;
    }
    bool literal(json_t& out) {
        skip();
        std::string text;
        if (at_ < text_.size() && (text_[at_] == '\'' || text_[at_] == '"')) {
            if (!quoted(text)) return false;
            out = make_string(text);
            return true;
        }
        if (eat("true")) return out = make_boolean("true"), true;
        if (eat("false")) return out = make_boolean("false"), true;
        if (eat("null")) return out = json_t {}, true;
        std::size_t const begin = at_;
        while (at_ < text_.size() && (std::isdigit(static_cast<unsigned char>(text_[at_])) || text_[at_] == '-' ||
                                      text_[at_] == '+' || text_[at_] == '.' || text_[at_] == 'e' || text_[at_] == 'E'))
            ++at_;
        return json_parser_t(text_.substr(begin, at_ - begin)).parse(out) && out.kind == json_t::kind_t::number_k;
    }
    bool relative(std::vector<path_step_t>& steps) {
        if (!eat("@")) return false;
        return segments(steps, path_start_t::current_k);
    }
    bool operand(filter_t& out) {
        skip();
        if (at_ < text_.size() && text_[at_] == '@') {
            out.kind = filter_t::kind_t::exists_k;
            return relative(out.path);
        }
        out.kind = filter_t::kind_t::literal_k;
        return literal(out.literal);
    }
    bool comparison(std::shared_ptr<filter_t>& out) {
        out = std::make_shared<filter_t>();
        skip();
        if (eat("!")) {
            out->kind = filter_t::kind_t::not_k;
            return unary(out->left);
        }
        if (eat("(")) {
            if (!disjunction(out)) return false;
            return eat(")");
        }
        filter_t left;
        if (!operand(left)) return false;
        skip();
        for (std::string_view op : {"==", "!=", "<=", ">=", "<", ">"}) {
            if (!eat(op)) continue;
            filter_t right;
            if (!operand(right)) return false;
            // A comparison names one relative path and one literal, in either order
            if (left.kind == filter_t::kind_t::literal_k && right.kind == filter_t::kind_t::exists_k) {
                out->path = std::move(right.path), out->literal = std::move(left.literal), out->literal_right = false;
            }
            else if (left.kind == filter_t::kind_t::exists_k && right.kind == filter_t::kind_t::literal_k) {
                out->path = std::move(left.path), out->literal = std::move(right.literal);
            }
            else return false;
            out->kind = filter_t::kind_t::compare_k, out->op = op;
            return true;
        }
        if (left.kind != filter_t::kind_t::exists_k) return false;
        *out = std::move(left);
        return true;
    }
    bool unary(std::shared_ptr<filter_t>& out) { return comparison(out); }
    bool conjunction(std::shared_ptr<filter_t>& out) {
        if (!unary(out)) return false;
        while (eat("&&")) {
            auto combined = std::make_shared<filter_t>();
            combined->kind = filter_t::kind_t::and_k, combined->left = out;
            if (!unary(combined->right)) return false;
            out = combined;
        }
        return true;
    }
    bool disjunction(std::shared_ptr<filter_t>& out) {
        if (!conjunction(out)) return false;
        while (eat("||")) {
            auto combined = std::make_shared<filter_t>();
            combined->kind = filter_t::kind_t::or_k, combined->left = out;
            if (!conjunction(combined->right)) return false;
            out = combined;
        }
        return true;
    }
    bool filter_step(path_step_t& step) {
        step.kind = path_step_t::kind_t::filter_k;
        bool const parenthesized = eat("(");
        if (!disjunction(step.filter)) return false;
        return !parenthesized || eat(")");
    }
    bool bracket(path_step_t& step) {
        skip();
        if (eat("*")) return step.kind = path_step_t::kind_t::wildcard_k, eat("]");
        if (eat("?")) return filter_step(step) && eat("]");
        if (at_ < text_.size() && (text_[at_] == '\'' || text_[at_] == '"')) {
            step.kind = path_step_t::kind_t::member_k;
            return quoted(step.name) && eat("]");
        }
        std::size_t const begin = at_;
        if (at_ < text_.size() && text_[at_] == '-') ++at_;
        while (at_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[at_]))) ++at_;
        std::string_view const digits = text_.substr(begin, at_ - begin);
        if (digits.empty() || digits == "-") return false;
        step.kind = path_step_t::kind_t::index_k;
        std::from_chars(digits.data(), digits.data() + digits.size(), step.index);
        return eat("]");
    }
    bool segments(std::vector<path_step_t>& steps, path_start_t start) {
        while (true) {
            skip();
            if (at_ >= text_.size()) return true;
            path_step_t step;
            if (text_.substr(at_, 2) == "..") {
                at_ += 2;
                auto inner = std::make_shared<path_step_t>();
                bool const read = eat("*")   ? (inner->kind = path_step_t::kind_t::wildcard_k, true)
                                  : eat("[") ? bracket(*inner)
                                             : name(inner->name);
                if (!read) return false;
                step.kind = path_step_t::kind_t::descendants_k, step.inner = inner;
            }
            else if (text_[at_] == '.') {
                ++at_;
                if (eat("*")) step.kind = path_step_t::kind_t::wildcard_k;
                else if (!name(step.name)) return false;
            }
            else if (text_[at_] == '[') {
                ++at_;
                if (!bracket(step)) return false;
            }
            else return start == path_start_t::current_k;
            steps.push_back(std::move(step));
        }
    }

  public:
    explicit path_parser_t(std::string_view text) noexcept : text_(text) {}

    /** Parses a whole path, or a bare root filter as one filter step over the root. */
    bool parse(std::vector<path_step_t>& steps, bool& root_filter) {
        skip();
        root_filter = false;
        if (eat("?")) {
            path_step_t step;
            if (!filter_step(step)) return false;
            steps.push_back(std::move(step));
            root_filter = true;
            skip();
            return at_ == text_.size();
        }
        if (!eat("$")) return false;
        if (!segments(steps, path_start_t::root_k)) return false;
        skip();
        return at_ == text_.size();
    }
};

/** A compiled filter: a path whose selection must be non-empty for a document to pass. */
struct compiled_path_t {
    std::vector<path_step_t> steps;
    bool root_filter = false;
};

bool evaluate(filter_t const& filter, json_t const& current);

/** Appends to @p out the nodes @p step selects from @p node. */
void select_step(path_step_t const& step, json_t const& node, std::vector<json_t const*>& out) {
    switch (step.kind) {
    case path_step_t::kind_t::member_k:
        if (json_t const* const found = node.is_object() ? node.member(step.name) : nullptr) out.push_back(found);
        return;
    case path_step_t::kind_t::index_k: {
        if (!node.is_array()) return;
        std::int64_t const size = static_cast<std::int64_t>(node.items.size());
        std::int64_t const index = step.index < 0 ? size + step.index : step.index;
        if (index >= 0 && index < size) out.push_back(&node.items[static_cast<std::size_t>(index)]);
        return;
    }
    case path_step_t::kind_t::wildcard_k:
        for (json_t const& item : node.items) out.push_back(&item);
        for (auto const& [key, value] : node.members) out.push_back(&value);
        return;
    case path_step_t::kind_t::descendants_k: {
        select_step(*step.inner, node, out);
        for (json_t const& item : node.items) select_step(step, item, out);
        for (auto const& [key, value] : node.members) select_step(step, value, out);
        return;
    }
    case path_step_t::kind_t::filter_k:
        for (json_t const& item : node.items)
            if (evaluate(*step.filter, item)) out.push_back(&item);
        for (auto const& [key, value] : node.members)
            if (evaluate(*step.filter, value)) out.push_back(&value);
        return;
    }
}

std::vector<json_t const*> select(std::vector<path_step_t> const& steps, json_t const& root) {
    std::vector<json_t const*> nodes {&root};
    for (path_step_t const& step : steps) {
        std::vector<json_t const*> next;
        for (json_t const* node : nodes) select_step(step, *node, next);
        nodes = std::move(next);
    }
    return nodes;
}

/** Compares @p value with @p literal as JSONPath does: numbers and strings order, everything
 *  else only equals. */
bool compare(json_t const& value, std::string const& op, json_t const& literal) {
    int order = 0;
    bool ordered = false;
    if (value.kind == json_t::kind_t::number_k && literal.kind == json_t::kind_t::number_k) {
        double const first = value.as_real(), second = literal.as_real();
        order = first < second ? -1 : first > second ? 1 : 0, ordered = true;
    }
    else if (value.kind == json_t::kind_t::string_k && literal.kind == json_t::kind_t::string_k)
        order = value.text.compare(literal.text), order = order < 0 ? -1 : order > 0 ? 1 : 0, ordered = true;
    bool const equal = ordered ? order == 0 : value == literal;
    if (op == "==") return equal;
    if (op == "!=") return !equal;
    if (!ordered) return false;
    if (op == "<") return order < 0;
    if (op == "<=") return order <= 0;
    if (op == ">") return order > 0;
    return order >= 0;
}

bool evaluate(filter_t const& filter, json_t const& current) {
    switch (filter.kind) {
    case filter_t::kind_t::and_k: return evaluate(*filter.left, current) && evaluate(*filter.right, current);
    case filter_t::kind_t::or_k: return evaluate(*filter.left, current) || evaluate(*filter.right, current);
    case filter_t::kind_t::not_k: return !evaluate(*filter.left, current);
    case filter_t::kind_t::exists_k: return !select(filter.path, current).empty();
    case filter_t::kind_t::compare_k:
        for (json_t const* node : select(filter.path, current)) {
            std::string const op = filter.literal_right ? filter.op
                                   : filter.op == "<"   ? ">"
                                   : filter.op == ">"   ? "<"
                                   : filter.op == "<="  ? ">="
                                   : filter.op == ">="  ? "<="
                                                        : filter.op;
            if (compare(*node, op, filter.literal)) return true;
        }
        return false;
    case filter_t::kind_t::literal_k: return filter.literal.kind != json_t::kind_t::null_k;
    }
    return false;
}

/** Whether the document @p value holds passes @p path; one that does not parse never does. */
bool path_keeps(entry_t const& entry, void const* state) {
    auto const& path = *static_cast<compiled_path_t const*>(state);
    json_t document;
    if (parse_json(entry.value, document).failed()) return false;
    if (path.root_filter) return evaluate(*path.steps.front().filter, document);
    return !select(path.steps, document).empty();
}

/** Compiles @p filter into @p verdict, refusing one that does not spell the subset; a zeroed
 *  view keeps every key. */
bool compile_filter(call_t& call, ustore_bytes_view_t filter, verdict_t& verdict) {
    verdict = {};
    std::string_view const text = text_of(filter);
    if (text.empty()) return true;
    try {
        auto compiled = std::make_shared<compiled_path_t>();
        if (!path_parser_t(text).parse(compiled->steps, compiled->root_filter))
            return call.fail(ustore_status_invalid_argument_k, "The filter does not compile");
        verdict.keeps = &path_keeps;
        verdict.state = std::move(compiled);
    }
    catch (std::bad_alloc const&) {
        return call.fail(ustore_status_out_of_memory_k, "Out of memory while compiling the filter");
    }
    return true;
}

#pragma endregion Filters

#pragma region Writes

/** Reads the key a document's @p id_field holds: a non-negative integer as its 128-bit
 *  big-endian bytes, or a canonical UUID. */
status_t extract_key(std::string_view document, std::string_view id_field, ustore_key_t& key) {
    json_t parsed;
    if (status_t const status = parse_json(document, parsed); status.failed()) return status;
    json_t const* const id = lookup(parsed, id_field);
    key = {};
    if (id && id->kind == json_t::kind_t::number_k && id->number == json_t::number_t::unsigned_k) {
        for (unsigned index = 0; index != 8; ++index)
            key.uuid.bytes[15 - index] = static_cast<std::uint8_t>(id->unsigned_value >> (8 * index));
        return {};
    }
    if (id && id->kind == json_t::kind_t::string_k && parse_uuid(id->text, key.uuid.bytes)) return {};
    return {ustore_status_malformed_k, "An id field is neither an unsigned integer nor a UUID"};
}

/** One task of a docs write, in the order of the call. */
struct document_task_t {
    std::string collection;
    std::string key;
    std::string_view field;
    std::string_view value;
};

/** Writes a whole upserted document into @p verbatim: as written when it comes as JSON, and printed
 *  from its scalar otherwise. */
status_t write_whole(std::string_view value, ustore_docs_field_type_t type, std::string& verbatim) {
    json_t parsed;
    if (type != ustore_docs_field_type_json_k) {
        if (status_t const status = value_of_type(value, type, parsed); status.failed()) return status;
        return dump_json(parsed, verbatim);
    }
    if (status_t const status = parse_json(value, parsed); status.failed()) return status;
    verbatim.assign(value);
    return {};
}

/** Applies @p modifier to the whole of @p tree as @p modification says: a merge, a patch or a
 *  replacement. */
status_t modify_whole(json_t& tree, json_t modifier, ustore_docs_modification_t modification) {
    if (modification == ustore_docs_modify_merge_k) merge_patch(tree, modifier);
    else if (modification != ustore_docs_modify_patch_k) tree = std::move(modifier);
    else if (!json_patch(tree, modifier))
        return {ustore_status_invalid_argument_k, "The patch does not apply to the document"};
    return {};
}

/** Applies @p modifier at @p field of @p tree as @p modification says, merging into or patching
 *  what sits there, or adding, replacing or setting it. */
status_t modify_field(json_t& tree, std::string_view field, json_t modifier, ustore_docs_modification_t modification) {
    status_t const unresolved {ustore_status_not_found_k, "The field path does not resolve inside the document"};
    std::vector<std::string> tokens;
    if (!field_tokens(field, tokens)) return unresolved;
    json_t replacement = std::move(modifier);
    bool const merges = modification == ustore_docs_modify_merge_k;
    if (merges || modification == ustore_docs_modify_patch_k) {
        json_t const* const existing = resolve(tree, tokens, tokens.size());
        json_t const patch = std::move(replacement);
        replacement = existing ? *existing : json_t {};
        if (merges) merge_patch(replacement, patch);
        else if (!existing || !json_patch(replacement, patch))
            return {ustore_status_invalid_argument_k, "The modifier does not apply to the field"};
    }
    bool applied = false;
    switch (modification) {
    case ustore_docs_modify_insert_k:
        applied = pointer_add(tree, tokens, std::move(replacement), parents_t::created_k);
        break;
    case ustore_docs_modify_update_k: applied = pointer_replace(tree, tokens, std::move(replacement)); break;
    default: applied = pointer_set(tree, tokens, std::move(replacement)); break;
    }
    return applied ? status_t {} : unresolved;
}

/** Whether the document a task modifies is stored, as the replay has it so far. */
enum class presence_t : bool { missing_k, present_k };

/** Refuses a modification of a document in @p presence that the @p field, empty for the whole
 *  document, and the @p modification need present or missing. */
status_t check_presence(presence_t presence, std::string_view field, ustore_docs_modification_t modification) noexcept {
    bool const exists = presence == presence_t::present_k, whole = field.empty();
    if (!exists && !whole) return {ustore_status_not_found_k, "Modified field belongs to a missing document"};
    if (!exists && (modification == ustore_docs_modify_update_k || modification == ustore_docs_modify_patch_k))
        return {ustore_status_not_found_k, "Updated or patched document is missing"};
    if (exists && whole && modification == ustore_docs_modify_insert_k)
        return {ustore_status_already_exists_k, "Inserted document already exists"};
    return {};
}

/** Replays the tasks of one document over @p stored, null for a missing one, into @p result. */
status_t replay(std::string const* stored, std::vector<document_task_t const*> const& tasks,
                ustore_docs_field_type_t type, ustore_docs_modification_t modification, std::string& result,
                bool& exists, bool& changed) {
    exists = stored != nullptr;
    changed = false;
    json_t tree;
    bool parsed = false;
    std::string verbatim = exists ? *stored : std::string {};
    for (document_task_t const* task : tasks) {
        bool const whole = task->field.empty();
        if (whole && modification == ustore_docs_modify_upsert_k) {
            if (status_t const status = write_whole(task->value, type, verbatim); status.failed()) return status;
            exists = true, parsed = false;
            continue;
        }
        presence_t const presence = exists ? presence_t::present_k : presence_t::missing_k;
        if (status_t const status = check_presence(presence, task->field, modification); status.failed()) return status;
        // The tree parses before the first modification and stays until a whole write replaces it
        if (!parsed) tree = {};
        if (!parsed && exists)
            if (status_t const status = parse_json(verbatim, tree); status.failed()) return status;
        parsed = true;
        json_t modifier;
        if (status_t const status = value_of_type(task->value, type, modifier); status.failed()) return status;
        status_t const modified = whole ? modify_whole(tree, std::move(modifier), modification)
                                        : modify_field(tree, task->field, std::move(modifier), modification);
        if (modified.failed()) return modified;
        changed = changed || !whole || modification == ustore_docs_modify_merge_k ||
                  modification == ustore_docs_modify_patch_k;
        exists = true;
    }
    if (parsed) return dump_json(tree, result);
    result = std::move(verbatim);
    return {};
}

#pragma endregion Writes

/** Collects into @p paths the unique paths to the leaves under @p node, extending @p prefix,
 *  in discovery order. */
status_t gist_paths(json_t const& node, std::string& prefix, std::set<std::string>& seen,
                    std::vector<std::string>& paths) {
    constexpr std::size_t path_length_limit_k = 512;
    std::size_t const length = prefix.size();
    if (node.is_object() || node.is_array()) {
        std::size_t index = 0;
        auto descend = [&](std::string_view token, json_t const& child) -> status_t {
            prefix.push_back('/');
            for (char const character : token) {
                if (character == '~') prefix += "~0";
                else if (character == '/') prefix += "~1";
                else prefix.push_back(character);
            }
            if (prefix.size() + 1 >= path_length_limit_k)
                return {ustore_status_capacity_exhausted_k, "Path is too long"};
            status_t const status = gist_paths(child, prefix, seen, paths);
            prefix.resize(length);
            return status;
        };
        for (auto const& [key, value] : node.members)
            if (status_t const status = descend(key, value); status.failed()) return status;
        for (json_t const& item : node.items)
            if (status_t const status = descend(std::to_string(index++), item); status.failed()) return status;
        return {};
    }
    if (seen.insert(prefix).second) paths.push_back(prefix);
    return {};
}

/** Bytes one cell of a scalar column of @p type takes, or zero for a text column and for a type
 *  no column has. */
std::size_t column_width(ustore_docs_field_type_t type) noexcept {
    switch (type) {
    case ustore_docs_field_type_bool_k:
    case ustore_docs_field_type_i8_k:
    case ustore_docs_field_type_u8_k: return 1;
    case ustore_docs_field_type_f16_k:
    case ustore_docs_field_type_i16_k:
    case ustore_docs_field_type_u16_k: return 2;
    case ustore_docs_field_type_f32_k:
    case ustore_docs_field_type_i32_k:
    case ustore_docs_field_type_u32_k: return 4;
    case ustore_docs_field_type_f64_k:
    case ustore_docs_field_type_i64_k:
    case ustore_docs_field_type_u64_k: return 8;
    case ustore_docs_field_type_uuid_k: return 16;
    default: return 0;
    }
}

bool is_column_type(ustore_docs_field_type_t type) noexcept {
    return column_width(type) || type == ustore_docs_field_type_binary_k || type == ustore_docs_field_type_string_k ||
           type == ustore_docs_field_type_json_k;
}

template <typename scalar_type_>
conversion_t gather_scalar(json_t const* value, std::byte* cell) noexcept {
    scalar_type_ scalar {};
    conversion_t const conversion = to_scalar(value, scalar);
    if (conversion != conversion_t::exact_k && conversion != conversion_t::converted_k) scalar = {};
    std::memcpy(cell, &scalar, sizeof(scalar));
    return conversion;
}

/** Converts @p value into the cell of a scalar column of @p type. */
conversion_t gather_cell(json_t const* value, ustore_docs_field_type_t type, std::byte* cell) noexcept {
    switch (type) {
    case ustore_docs_field_type_bool_k: return gather_scalar<bool>(value, cell);
    case ustore_docs_field_type_i8_k: return gather_scalar<std::int8_t>(value, cell);
    case ustore_docs_field_type_u8_k: return gather_scalar<std::uint8_t>(value, cell);
    case ustore_docs_field_type_i16_k: return gather_scalar<std::int16_t>(value, cell);
    case ustore_docs_field_type_u16_k: return gather_scalar<std::uint16_t>(value, cell);
    case ustore_docs_field_type_i32_k: return gather_scalar<std::int32_t>(value, cell);
    case ustore_docs_field_type_u32_k: return gather_scalar<std::uint32_t>(value, cell);
    case ustore_docs_field_type_f32_k: return gather_scalar<float>(value, cell);
    case ustore_docs_field_type_i64_k: return gather_scalar<std::int64_t>(value, cell);
    case ustore_docs_field_type_u64_k: return gather_scalar<std::uint64_t>(value, cell);
    case ustore_docs_field_type_f64_k: return gather_scalar<double>(value, cell);
    case ustore_docs_field_type_f16_k: {
        float real = 0;
        conversion_t conversion = to_scalar(value, real);
        // A magnitude past the range of a half collides rather than rounding to infinity
        constexpr float half_max_k = 65504.0f;
        bool const finite = std::isfinite(real);
        if (finite && (real > half_max_k || real < -half_max_k)) conversion = conversion_t::collides_k;
        bool const valid = conversion == conversion_t::exact_k || conversion == conversion_t::converted_k;
        std::uint16_t const half = valid ? to_half(real) : 0;
        std::memcpy(cell, &half, 2);
        return conversion;
    }
    case ustore_docs_field_type_uuid_k: {
        if (!value || value->kind == json_t::kind_t::null_k) return conversion_t::missing_k;
        std::uint8_t bytes[16] = {};
        if (value->kind != json_t::kind_t::string_k || !parse_uuid(value->text, bytes)) return conversion_t::collides_k;
        std::memcpy(cell, bytes, 16);
        return conversion_t::converted_k;
    }
    default: return conversion_t::collides_k;
    }
}

/** Lays out one column of @p stored.size() rows per field of @p fields into @p arena, converted
 *  into its entry of @p types, from the documents @p stored holds, empty where missing; the empty
 *  field of a JSON column is the whole document as stored. */
ustore_docs_column_view_t* write_columns(call_t& call, ustore_arena_handle_t& arena,
                                         std::vector<std::string> const& stored,
                                         std::vector<std::string_view> const& fields,
                                         ustore_docs_field_type_t const* types) {
    std::size_t const count = stored.size(), fields_count = fields.size();
    std::vector<json_t> documents(count);
    for (std::size_t row = 0; row != count; ++row)
        if (!stored[row].empty() && call.failed_with(parse_json(stored[row], documents[row]))) return nullptr;
    auto* const columns = carve<ustore_docs_column_view_t>(call, arena, fields_count);
    for (std::size_t field = 0; field != fields_count && !call.failed(); ++field) {
        ustore_docs_field_type_t const type = types[field];
        std::size_t const bitmap_bytes = (count + 7) / 8;
        auto* const validities = carve<ustore_octet_t>(call, arena, bitmap_bytes);
        auto* const conversions = carve<ustore_octet_t>(call, arena, bitmap_bytes);
        auto* const collisions = carve<ustore_octet_t>(call, arena, bitmap_bytes);
        std::size_t const width = column_width(type);
        auto* const scalars = width ? carve<std::byte>(call, arena, width * count) : nullptr;
        std::vector<std::string> texts(width ? 0 : count);
        if (call.failed()) return nullptr;
        for (std::size_t row = 0; row != count; ++row) {
            json_t const* const value = stored[row].empty() ? nullptr : lookup(documents[row], fields[field]);
            conversion_t conversion = conversion_t::missing_k;
            if (width) conversion = gather_cell(value, type, scalars + row * width);
            else if (type == ustore_docs_field_type_json_k && value && fields[field].empty())
                texts[row] = stored[row], conversion = conversion_t::exact_k;
            else if (type == ustore_docs_field_type_json_k && value)
                conversion = print_json(*value, texts[row]) ? conversion_t::exact_k : conversion_t::collides_k;
            else if (value && (value->is_object() || value->is_array())) conversion = conversion_t::collides_k;
            else conversion = to_text(value, texts[row]);
            bool const valid = conversion == conversion_t::exact_k || conversion == conversion_t::converted_k;
            if (!valid && !width) texts[row].clear();
            if (valid) set_bit(validities, row);
            if (conversion == conversion_t::converted_k) set_bit(conversions, row);
            if (conversion == conversion_t::collides_k) set_bit(collisions, row);
        }
        ustore_docs_column_view_t& column = columns[field];
        column.validities = validities, column.conversions = conversions, column.collisions = collisions;
        column.scalars = reinterpret_cast<ustore_byte_t const*>(scalars);
        if (!width && count &&
            !write_tape(call, arena, texts, column.strings,
                        type == ustore_docs_field_type_string_k ? string_end_t::terminated_k : string_end_t::open_k))
            return nullptr;
    }
    return call.failed() ? nullptr : columns;
}

status_t check_document(std::string_view text) {
    json_t parsed;
    return parse_json(text, parsed);
}

} // namespace unum::ustore::stub

/** What @c ustore_docs_cursor_t names: the walk, and the columns every pull gathers, copied at
 *  init. */
struct ustore_docs_cursor_handle_t {
    keyed_cursor_t cursor;
    std::vector<std::string> field_names;
    std::vector<std::string_view> fields;
    std::vector<ustore_docs_field_type_t> types;
};

void ustore_docs_write(ustore_docs_write_t* arguments) {
    if (submitted<&ustore_docs_write>(arguments)) return;
    call_t call(arguments->outcome);
    if (refuse_database(call, arguments->database)) return;
    std::size_t const count = arguments->count;
    strings_in_t fields, values;
    if (refuse_collections(call, arguments->collections, count)) return;
    if (!read_strings(call, arguments->fields, count, fields) || !read_strings(call, arguments->values, count, values))
        return;
    if (count && !values.given) return call.refuse(ustore_status_invalid_argument_k, "No values were provided");
    std::string_view const id_field = text_of(arguments->id_field);
    if (count && !arguments->keys.data && id_field.empty())
        return call.refuse(ustore_status_invalid_argument_k, "Documents without keys need an id field");
    if (std::string_view(ustore_docs_modification_name(arguments->modification)) == "unrecognized")
        return call.refuse(ustore_status_invalid_argument_k, "Unknown modification");
    if (refuse_write_choices(call, arguments->transaction, arguments->durability, arguments->watch) ||
        refuse_write_transaction(call, arguments->transaction, arguments->database) ||
        !pre_resolve(call, arguments->database, arguments->transaction, arguments->collections, count,
                     ustore_modality_docs_k) ||
        refuse_read_only(call, arguments->database))
        return;
    if (std::string_view(ustore_docs_field_type_name(arguments->type)) == "unrecognized")
        return call.refuse(ustore_status_unsupported_k, "Input type is not supported");
    if (!call.hold_executor(arguments->executor) || !hold_transaction(call, arguments->transaction)) return;
    if (!count) return;

    std::vector<document_task_t> tasks(count);
    try {
        for (std::size_t index = 0; index != count; ++index) {
            document_task_t& task = tasks[index];
            task.collection = bytes_of(
                strided_element(arguments->collections.data, arguments->collections.stride, index));
            task.field = fields.given ? fields[index] : std::string_view {};
            task.value = values[index];
            ustore_key_t key {};
            if (arguments->keys.data) key = strided_element(arguments->keys.data, arguments->keys.stride, index);
            else if (call.failed_with(extract_key(values[index], id_field, key))) return;
            task.key = bytes_of(key);
        }
    }
    catch (std::bad_alloc const&) {
        return call.refuse(ustore_status_out_of_memory_k, "Out of memory while writing");
    }
    write_through(call, arguments->database, arguments->transaction, arguments->watch, arguments->durability,
                  [&](reader_t const& reader, writer_t& writer) {
                      // Every task of one document replays in call order over the
                      // document read once
                      std::map<std::pair<std::string, std::string>, std::vector<document_task_t const*>> grouped;
                      for (document_task_t const& task : tasks) grouped[{task.collection, task.key}].push_back(&task);
                      bool const watched = writer.watched;
                      for (auto const& [place, group] : grouped) {
                          collection_t const* const collection = reader.find(place.first);
                          if (!collection)
                              return call.refuse(ustore_status_not_found_k, "A collection was dropped meanwhile");
                          // A transaction watches the documents it rewrites, whatever the caller
                          // asked of the writes
                          bool const reads = group.size() > 1 || !group.front()->field.empty() ||
                                             arguments->modification != ustore_docs_modify_upsert_k;
                          std::string const* const stored = reads ? reader.get(*collection, place.second)
                                                                  : reader.peek(*collection, place.second);
                          std::string result;
                          bool exists = false;
                          bool changed = false;
                          if (call.failed_with(replay(stored, group, arguments->type, arguments->modification, result,
                                                      exists, changed)))
                              return;
                          writer.watched = watched || reads;
                          writer.put(*collection, place.second, std::move(result),
                                     changed ? ustore_change_change_k : ustore_change_write_k);
                          writer.watched = watched;
                      }
                  });
}

void ustore_docs_read(ustore_docs_read_t* arguments) {
    if (submitted<&ustore_docs_read>(arguments)) return;
    call_t call(arguments->outcome);
    if (arguments->presences) *arguments->presences = nullptr;
    if (arguments->lengths) *arguments->lengths = nullptr;
    if (arguments->values) *arguments->values = {};
    if (refuse_database(call, arguments->database)) return;
    std::size_t const count = arguments->count;
    strings_in_t fields;
    if (!arguments->arena) return call.refuse(ustore_status_invalid_argument_k, "The call needs an arena");
    if (refuse_collections(call, arguments->collections, count)) return;
    if (count && !arguments->keys.data) return call.refuse(ustore_status_invalid_argument_k, "No keys were provided");
    if (!read_strings(call, arguments->fields, count, fields)) return;
    view_spec_t spec;
    std::vector<collection_t const*> resolved;
    if (!open_listing(call, *arguments, ustore_modality_docs_k, spec, resolved)) return;
    if (arguments->outputs_type != ustore_docs_field_type_json_k &&
        arguments->outputs_type != ustore_docs_field_type_string_k)
        return call.refuse(ustore_status_unsupported_k, "Documents read only as JSON or strings");
    if (!call.hold_executor(arguments->executor)) return;
    ustore_arena_handle_t* const arena = claim_arena(call, arguments->arena,
                                                     {arguments->collections.data, arguments->keys.data,
                                                      first_input(arguments->fields), data_input(arguments->fields)});
    if (!arena || !hold_view(call, spec)) return;
    std::vector<std::string> values(count);
    std::vector<bool> present(count, false);
    try {
        std::shared_lock lock(arguments->database->mutex);
        reader_t const reader = reader_of(arguments->database, spec, arguments->database->head);
        for (std::size_t index = 0; index != count; ++index) {
            collection_t const* const collection = reader.find(bytes_of(resolved[index]->id));
            if (!collection) return call.refuse(ustore_status_not_found_k, "A collection was dropped meanwhile");
            std::string const* const stored = reader.get(
                *collection, bytes_of(strided_element(arguments->keys.data, arguments->keys.stride, index)));
            if (!stored) continue;
            std::string_view const field = fields.given ? fields[index] : std::string_view {};
            if (field.empty() && arguments->outputs_type == ustore_docs_field_type_json_k) {
                values[index] = *stored, present[index] = true;
                continue;
            }
            json_t document;
            if (call.failed_with(parse_json(*stored, document))) return;
            json_t const* const value = lookup(document, field);
            if (!value) continue;
            if (arguments->outputs_type == ustore_docs_field_type_json_k) {
                print_json(*value, values[index]);
                present[index] = true;
                continue;
            }
            if (value->kind == json_t::kind_t::object_k || value->kind == json_t::kind_t::array_k) {
                print_json(*value, values[index]);
                present[index] = true;
                continue;
            }
            conversion_t const conversion = to_text(value, values[index]);
            present[index] = conversion == conversion_t::exact_k || conversion == conversion_t::converted_k;
        }
    }
    catch (std::bad_alloc const&) {
        return call.refuse(ustore_status_out_of_memory_k, "Out of memory while reading");
    }
    write_values(call, *arena, values, present, arguments->presences, arguments->lengths, arguments->values);
}

void ustore_docs_gist(ustore_docs_gist_t* arguments) {
    if (submitted<&ustore_docs_gist>(arguments)) return;
    call_t call(arguments->outcome);
    if (arguments->fields) *arguments->fields = {};
    if (arguments->written) *arguments->written = 0;
    if (refuse_database(call, arguments->database)) return;
    std::size_t const count = arguments->count;
    if (!arguments->arena) return call.refuse(ustore_status_invalid_argument_k, "The call needs an arena");
    if (refuse_collections(call, arguments->collections, count)) return;
    if ((count && !arguments->keys.data) || !arguments->fields || !arguments->written)
        return call.refuse(ustore_status_invalid_argument_k, "The call needs keys and outputs");
    view_spec_t spec;
    std::vector<collection_t const*> resolved;
    if (!open_listing(call, *arguments, ustore_modality_docs_k, spec, resolved)) return;
    if (!call.hold_executor(arguments->executor)) return;
    ustore_arena_handle_t* const arena = claim_arena(call, arguments->arena,
                                                     {arguments->collections.data, arguments->keys.data});
    if (!arena || !hold_view(call, spec)) return;
    std::set<std::string> seen;
    std::vector<std::string> paths;
    try {
        std::shared_lock lock(arguments->database->mutex);
        reader_t const reader = reader_of(arguments->database, spec, arguments->database->head);
        for (std::size_t index = 0; index != count; ++index) {
            collection_t const* const collection = reader.find(bytes_of(resolved[index]->id));
            if (!collection) return call.refuse(ustore_status_not_found_k, "A collection was dropped meanwhile");
            std::string const* const stored = reader.get(
                *collection, bytes_of(strided_element(arguments->keys.data, arguments->keys.stride, index)));
            if (!stored) continue;
            json_t document;
            if (call.failed_with(parse_json(*stored, document))) return;
            std::string prefix;
            if (call.failed_with(gist_paths(document, prefix, seen, paths))) return;
        }
    }
    catch (std::bad_alloc const&) {
        return call.refuse(ustore_status_out_of_memory_k, "Out of memory while listing paths");
    }
    ustore_strings_view_t tape {};
    if (!paths.empty() && !write_tape(call, *arena, paths, tape)) return;
    *arguments->fields = tape;
    *arguments->written = paths.size();
}

void ustore_docs_gather(ustore_docs_gather_t* arguments) {
    if (submitted<&ustore_docs_gather>(arguments)) return;
    call_t call(arguments->outcome);
    if (arguments->columns) *arguments->columns = nullptr;
    if (refuse_database(call, arguments->database)) return;
    std::size_t const count = arguments->count, fields_count = arguments->fields_count;
    strings_in_t fields;
    if (!arguments->arena) return call.refuse(ustore_status_invalid_argument_k, "The call needs an arena");
    if (refuse_collections(call, arguments->collections, count)) return;
    if ((count && !arguments->keys.data) || !arguments->columns)
        return call.refuse(ustore_status_invalid_argument_k, "The call needs keys and an output for the columns");
    if (!read_strings(call, arguments->fields, fields_count, fields)) return;
    if (fields_count && (!fields.given || !arguments->outputs_types))
        return call.refuse(ustore_status_invalid_argument_k,
                           "Need the fields to gather and the types of their columns");
    for (std::size_t field = 0; field != fields_count; ++field)
        if (!is_column_type(arguments->outputs_types[field]))
            return call.refuse(ustore_status_invalid_argument_k, "A column type is unknown");
    view_spec_t spec;
    std::vector<collection_t const*> resolved;
    if (!open_listing(call, *arguments, ustore_modality_docs_k, spec, resolved)) return;
    if (!call.hold_executor(arguments->executor)) return;
    ustore_arena_handle_t* const arena = claim_arena(
        call, arguments->arena,
        {arguments->collections.data, arguments->keys.data, first_input(arguments->fields),
         data_input(arguments->fields), arguments->outputs_types});
    if (!arena || !hold_view(call, spec)) return;
    std::vector<std::string> stored(count);
    try {
        std::shared_lock lock(arguments->database->mutex);
        reader_t const reader = reader_of(arguments->database, spec, arguments->database->head);
        for (std::size_t index = 0; index != count; ++index) {
            collection_t const* const collection = reader.find(bytes_of(resolved[index]->id));
            if (!collection) return call.refuse(ustore_status_not_found_k, "A collection was dropped meanwhile");
            std::string const* const found = reader.get(
                *collection, bytes_of(strided_element(arguments->keys.data, arguments->keys.stride, index)));
            if (found) stored[index] = *found;
        }
    }
    catch (std::bad_alloc const&) {
        return call.refuse(ustore_status_out_of_memory_k, "Out of memory while gathering");
    }
    ustore_docs_column_view_t* const columns = write_columns(call, *arena, stored, fields.items,
                                                             arguments->outputs_types);
    if (!columns) return;
    *arguments->columns = columns;
}

void ustore_docs_remove(ustore_docs_remove_t* arguments) {
    if (submitted<&ustore_docs_remove>(arguments)) return;
    call_t call(arguments->outcome);
    if (refuse_database(call, arguments->database)) return;
    std::size_t const count = arguments->count;
    if (refuse_collections(call, arguments->collections, count)) return;
    if (refuse_write_keys(call, arguments->keys, count)) return;
    if (refuse_write_choices(call, arguments->transaction, arguments->durability, arguments->watch) ||
        refuse_write_transaction(call, arguments->transaction, arguments->database) ||
        !pre_resolve(call, arguments->database, arguments->transaction, arguments->collections, count,
                     ustore_modality_docs_k) ||
        refuse_read_only(call, arguments->database))
        return;
    if (!call.hold_executor(arguments->executor) || !hold_transaction(call, arguments->transaction)) return;
    write_through(call, arguments->database, arguments->transaction, arguments->watch, arguments->durability,
                  [&](reader_t const& reader, writer_t& writer) {
                      remove_keyed(call, reader, writer, arguments->collections, arguments->keys, arguments->end_keys,
                                   count, ustore_modality_docs_k);
                  });
}

void ustore_docs_scan(ustore_docs_scan_t* arguments) {
    if (submitted<&ustore_docs_scan>(arguments)) return;
    call_t call(arguments->outcome);
    if (!check_scan(call, arguments)) return;
    verdict_t verdict;
    if (!compile_filter(call, arguments->filter, verdict)) return;
    scan_keyed(call, arguments, ustore_modality_docs_k, nullptr, verdict);
}

void ustore_docs_cursor_init(ustore_docs_cursor_init_t* arguments) {
    call_t call(arguments->outcome);
    if (arguments->cursor) *arguments->cursor = nullptr;
    if (refuse_database(call, arguments->database)) return;
    if (!arguments->cursor)
        return call.refuse(ustore_status_invalid_argument_k, "The call needs an output for the cursor");
    if (refuse_direction(call, arguments->direction) || refuse_predicate(call, arguments->predicate)) return;
    std::size_t const fields_count = arguments->fields_count;
    strings_in_t fields;
    if (!read_strings(call, arguments->fields, fields_count, fields)) return;
    if (fields_count && (!fields.given || !arguments->columns_types))
        return call.refuse(ustore_status_invalid_argument_k,
                           "Need the fields to gather and the types of their columns");
    for (std::size_t field = 0; field != fields_count; ++field)
        if (!is_column_type(arguments->columns_types[field]))
            return call.refuse(ustore_status_invalid_argument_k, "A column type is unknown");
    auto* const cursor = new (std::nothrow) ustore_docs_cursor_handle_t {};
    if (!cursor) return call.refuse(ustore_status_out_of_memory_k, "Failed to allocate the cursor");
    try {
        cursor->field_names.assign(fields.items.begin(), fields.items.end());
        for (std::string const& name : cursor->field_names) cursor->fields.emplace_back(name);
        cursor->types.assign(arguments->columns_types, arguments->columns_types + fields_count);
    }
    catch (std::bad_alloc const&) {
        delete cursor;
        return call.refuse(ustore_status_out_of_memory_k, "Failed to copy the cursor's columns");
    }
    if (!compile_filter(call, arguments->filter, cursor->cursor.verdict) ||
        !open_keyed_cursor(call, arguments, ustore_modality_docs_k, cursor->cursor))
        return delete cursor;
    *arguments->cursor = cursor;
}

void ustore_docs_cursor_next(ustore_docs_cursor_next_t* arguments) {
    if (submitted<&ustore_docs_cursor_next>(arguments)) return;
    call_t call(arguments->outcome);
    if (arguments->batches) *arguments->batches = nullptr;
    if (arguments->batches_count) *arguments->batches_count = 0;
    if (refuse_database(call, arguments->database)) return;
    if (!arguments->cursor || !arguments->arena || !arguments->batches || !arguments->batches_count)
        return call.refuse(ustore_status_invalid_argument_k, "The call needs a cursor, an arena and outputs");
    keyed_cursor_t& cursor = arguments->cursor->cursor;
    if (cursor.database != arguments->database)
        return call.refuse(ustore_status_invalid_argument_k, "The cursor walks another database");
    call.name(&cursor.named_calls);
    if (!call.hold_executor(arguments->executor)) return;
    ustore_arena_handle_t* const arena = claim_arena(call, arguments->arena);
    if (!arena || !call.claim(&cursor.claim, "Another call pulls the cursor")) return;
    std::vector<entry_t> listed;
    if (!pull_keyed(call, arguments, cursor, listed) || listed.empty()) return;
    // The one batch of a pull holds every listed row, as the stub runs every call on
    // the calling thread
    std::vector<std::string> stored;
    try {
        for (entry_t const& entry : listed) stored.push_back(entry.value);
    }
    catch (std::bad_alloc const&) {
        return call.refuse(ustore_status_out_of_memory_k, "Out of memory while pulling");
    }
    auto* const keys = carve<ustore_key_t>(call, *arena, listed.size());
    auto* const batch = carve<ustore_docs_batch_view_t>(call, *arena, 1);
    if (call.failed()) return;
    for (std::size_t index = 0; index != listed.size(); ++index) keys[index] = entry_key(listed[index]);
    ustore_docs_column_view_t const* columns = nullptr;
    if (!arguments->cursor->fields.empty()) {
        columns = write_columns(call, *arena, stored, arguments->cursor->fields, arguments->cursor->types.data());
        if (!columns) return;
    }
    *batch = {listed.size(), keys, columns};
    *arguments->batches = batch;
    *arguments->batches_count = 1;
}

void ustore_docs_cursor_free(ustore_docs_cursor_t cursor) {
    if (!cursor) return;
    close_keyed_cursor(cursor->cursor);
    delete cursor;
}

void ustore_docs_sample(ustore_docs_sample_t* arguments) {
    if (submitted<&ustore_docs_sample>(arguments)) return;
    sample_keyed(arguments, ustore_modality_docs_k);
}

void ustore_docs_measure(ustore_docs_measure_t* arguments) {
    if (submitted<&ustore_docs_measure>(arguments)) return;
    measure_keyed(arguments, ustore_modality_docs_k);
}
