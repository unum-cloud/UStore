/**
 *  @file src_stub/pages.cpp
 *  @author Ash Vardanian
 *  @date November 16, 2026
 *  @brief The pages calls over the store of @c lib.hpp: every page an element tree under a root
 *      keyed by the page, stored in a binary form of the stand-in's own, imported from and rendered
 *      into XML or text.
 *
 *  The tree follows the character data as the engine's does: an element's text before its first
 *  child is its own run, every later run becomes a text element without a name, and the root holds
 *  no text of its own, so a fragment's leading text lands as an element of its own. Rendering
 *  escapes the five XML specials in text and attributes alike, writes an element without content as
 *  @c <name/>, and renders an element without a name as its content alone. Filters of scans and
 *  cursors answer an XPath subset: absolute and descendant steps by name or wildcard, each narrowed
 *  by attribute presence or value.
 */
#include "keyed.hpp"

using namespace unum::ustore::stub;

namespace unum::ustore::stub {

#pragma region Tree

/** What an element is: one with a tag, the root included, a run of text, or a comment. */
enum class element_kind_t : std::uint8_t { element_k = 0, text_k = 1, comment_k = 2 };

/** One element: its kind, its key, its tag, empty for the root, its attributes in order, its own
 *  text run, and its children in order. */
struct element_t {
    element_kind_t kind = element_kind_t::element_k;
    ustore_key_t key {};
    std::string tag;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::string text;
    std::vector<element_t> children;
};

/** Mints a time-ordered key for an element inserted without one. */
ustore_key_t mint_element_key() {
    ustore_key_t key {};
    std::array<std::uint8_t, 16> const uuid = mint_uuid_v7();
    std::memcpy(key.uuid.bytes, uuid.data(), uuid.size());
    return key;
}

/** Leads every stored page, so bytes of another form are refused as malformed. */
constexpr std::string_view page_signature_k = "USTPAGE1";

void encode_text(std::string& out, std::string_view text) {
    std::uint64_t const length = text.size();
    out.append(reinterpret_cast<char const*>(&length), sizeof(length));
    out.append(text);
}

void encode_element(std::string& out, element_t const& element) {
    out.push_back(static_cast<char>(element.kind));
    out.append(reinterpret_cast<char const*>(element.key.uuid.bytes), 16);
    encode_text(out, element.tag);
    std::uint64_t const attributes = element.attributes.size();
    out.append(reinterpret_cast<char const*>(&attributes), sizeof(attributes));
    for (auto const& [name, value] : element.attributes) encode_text(out, name), encode_text(out, value);
    encode_text(out, element.text);
    std::uint64_t const children = element.children.size();
    out.append(reinterpret_cast<char const*>(&children), sizeof(children));
    for (element_t const& child : element.children) encode_element(out, child);
}

std::string encode_page(element_t const& root) {
    std::string out(page_signature_k);
    encode_element(out, root);
    return out;
}

bool decode_text(std::string_view& in, std::string& text) {
    std::uint64_t length = 0;
    if (in.size() < sizeof(length)) return false;
    std::memcpy(&length, in.data(), sizeof(length));
    in.remove_prefix(sizeof(length));
    if (length > in.size()) return false;
    text.assign(in.data(), static_cast<std::size_t>(length));
    in.remove_prefix(static_cast<std::size_t>(length));
    return true;
}

bool decode_count(std::string_view& in, std::uint64_t& count) {
    if (in.size() < sizeof(count)) return false;
    std::memcpy(&count, in.data(), sizeof(count));
    in.remove_prefix(sizeof(count));
    return count <= in.size();
}

bool decode_element(std::string_view& in, element_t& element, std::size_t depth = 0) {
    if (depth > 4096 || in.size() < 17) return false;
    auto const kind = static_cast<std::uint8_t>(in.front());
    if (kind > static_cast<std::uint8_t>(element_kind_t::comment_k)) return false;
    element.kind = static_cast<element_kind_t>(kind);
    std::memcpy(element.key.uuid.bytes, in.data() + 1, 16);
    in.remove_prefix(17);
    std::uint64_t attributes = 0, children = 0;
    if (!decode_text(in, element.tag) || !decode_count(in, attributes)) return false;
    element.attributes.resize(static_cast<std::size_t>(attributes));
    for (auto& [name, value] : element.attributes)
        if (!decode_text(in, name) || !decode_text(in, value)) return false;
    if (!decode_text(in, element.text) || !decode_count(in, children)) return false;
    element.children.resize(static_cast<std::size_t>(children));
    for (element_t& child : element.children)
        if (!decode_element(in, child, depth + 1)) return false;
    return true;
}

status_t decode_page(std::string_view stored, element_t& root) {
    root = {};
    if (!stored.starts_with(page_signature_k)) return {ustore_status_malformed_k, "A stored page does not decode"};
    stored.remove_prefix(page_signature_k.size());
    if (!decode_element(stored, root) || !stored.empty())
        return {ustore_status_malformed_k, "A stored page does not decode"};
    return {};
}

/** The element of @p root keyed @p key, its parent and its position there, or null. */
element_t* find_element(element_t& root, ustore_key_t const& key, element_t** parent = nullptr,
                        std::size_t* position = nullptr) {
    if (std::memcmp(root.key.uuid.bytes, key.uuid.bytes, 16) == 0) return &root;
    for (std::size_t index = 0; index != root.children.size(); ++index) {
        element_t& child = root.children[index];
        if (std::memcmp(child.key.uuid.bytes, key.uuid.bytes, 16) == 0) {
            if (parent) *parent = &root;
            if (position) *position = index;
            return &child;
        }
        if (element_t* const found = find_element(child, key, parent, position)) return found;
    }
    return nullptr;
}

/** Whether @p key names @p element or one of its descendants. */
bool holds_key(element_t const& element, ustore_key_t const& key) {
    if (std::memcmp(element.key.uuid.bytes, key.uuid.bytes, 16) == 0) return true;
    for (element_t const& child : element.children)
        if (holds_key(child, key)) return true;
    return false;
}

#pragma endregion Tree

#pragma region XML

/** What a run of character data lands under: the root, which keeps no text of its own,
 *  or an element. */
enum class holder_t : bool { root_k, element_k };

/** A strict reader of XML fragments: elements, attributes, text, the five entities and
 *  numeric references, CDATA, comments, processing instructions, and a DOCTYPE without
 *  an internal subset. */
class xml_parser_t {
    std::string_view text_;
    std::size_t at_ = 0;

    static bool name_start(char character) noexcept {
        return std::isalpha(static_cast<unsigned char>(character)) || character == '_' || character == ':' ||
               static_cast<unsigned char>(character) >= 0x80;
    }
    static bool name_char(char character) noexcept {
        return name_start(character) || std::isdigit(static_cast<unsigned char>(character)) || character == '-' ||
               character == '.';
    }
    void skip_space() noexcept {
        while (at_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[at_]))) ++at_;
    }
    bool name(std::string& out) {
        if (at_ >= text_.size() || !name_start(text_[at_])) return false;
        std::size_t const begin = at_;
        while (at_ < text_.size() && name_char(text_[at_])) ++at_;
        out.assign(text_.substr(begin, at_ - begin));
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
    bool entity(std::string& out) {
        std::size_t const end = text_.find(';', at_);
        if (end == std::string_view::npos) return false;
        std::string_view const body = text_.substr(at_ + 1, end - at_ - 1);
        at_ = end + 1;
        if (body == "lt") return out.push_back('<'), true;
        if (body == "gt") return out.push_back('>'), true;
        if (body == "amp") return out.push_back('&'), true;
        if (body == "quot") return out.push_back('"'), true;
        if (body == "apos") return out.push_back('\''), true;
        if (body.size() < 2 || body.front() != '#') return false;
        bool const hex = body[1] == 'x' || body[1] == 'X';
        std::string_view const digits = body.substr(hex ? 2 : 1);
        std::uint32_t code = 0;
        auto const [last, error] = std::from_chars(digits.data(), digits.data() + digits.size(), code, hex ? 16 : 10);
        if (error != std::errc() || last != digits.data() + digits.size()) return false;
        // Only the characters XML allows may be referenced
        bool const allowed = code == 0x9 || code == 0xA || code == 0xD || (code >= 0x20 && code <= 0xD7FF) ||
                             (code >= 0xE000 && code <= 0xFFFD) || (code >= 0x10000 && code <= 0x10FFFF);
        if (!allowed) return false;
        append_utf8(out, code);
        return true;
    }

    /** Reads character data up to the next markup into @p out. */
    bool characters(std::string& out) {
        while (at_ < text_.size() && text_[at_] != '<') {
            if (text_.substr(at_, 3) == "]]>") return false;
            if (text_[at_] == '&') {
                if (!entity(out)) return false;
            }
            else out.push_back(text_[at_++]);
        }
        return true;
    }
    bool attribute_value(std::string& out) {
        if (at_ >= text_.size() || (text_[at_] != '"' && text_[at_] != '\'')) return false;
        char const quote = text_[at_++];
        while (at_ < text_.size() && text_[at_] != quote) {
            if (text_[at_] == '<') return false;
            if (text_[at_] == '&') {
                if (!entity(out)) return false;
            }
            else out.push_back(text_[at_++]);
        }
        if (at_ >= text_.size()) return false;
        ++at_;
        return true;
    }

    /** Reads the attributes of a start tag into @p element up to its @c / or @c >, refusing a
     *  repeated one. */
    bool attributes(element_t& element) {
        while (true) {
            skip_space();
            if (at_ >= text_.size()) return false;
            if (text_[at_] == '/' || text_[at_] == '>') return true;
            std::string attribute, value;
            if (!name(attribute)) return false;
            skip_space();
            if (at_ >= text_.size() || text_[at_] != '=') return false;
            ++at_;
            skip_space();
            if (!attribute_value(value) || has_attribute(element, attribute)) return false;
            element.attributes.emplace_back(std::move(attribute), std::move(value));
        }
    }
    static bool has_attribute(element_t const& element, std::string_view name) noexcept {
        for (auto const& existing : element.attributes)
            if (existing.first == name) return true;
        return false;
    }

    /** Skips a comment, a processing instruction or a DOCTYPE at the cursor, answering whether
     *  one was there. */
    status_t skip_markup(bool& skipped) {
        skipped = true;
        std::string_view const rest = text_.substr(at_);
        auto skip_to = [&](std::string_view terminator) -> status_t {
            std::size_t const end = text_.find(terminator, at_);
            if (end == std::string_view::npos) return {ustore_status_malformed_k, "A markup does not parse"};
            at_ = end + terminator.size();
            return {};
        };
        if (rest.starts_with("<!--")) return skip_to("-->");
        if (rest.starts_with("<?")) return skip_to("?>");
        if (rest.starts_with("<!DOCTYPE")) {
            std::size_t const end = text_.find('>', at_);
            if (end == std::string_view::npos) return {ustore_status_malformed_k, "A markup does not parse"};
            if (text_.substr(at_, end - at_).find('[') != std::string_view::npos)
                return {ustore_status_unsupported_k, "A DOCTYPE carries an internal subset"};
            at_ = end + 1;
            return {};
        }
        skipped = false;
        return {};
    }

    /** Reads the content of @p parent up to its closing tag, or the end of a fragment when it is
     *  the top, where the root keeps no text of its own. */
    status_t content(element_t& parent, std::string const* closing, std::size_t depth, holder_t holder) {
        bool const root = holder == holder_t::root_k;
        status_t const malformed {ustore_status_malformed_k, "A markup does not parse"};
        if (depth > 4096) return malformed;
        std::string run;
        // A run before the first child is the element's own text, and every later one
        // a text element
        auto flush = [&] {
            if (run.empty()) return;
            if (parent.children.empty() && !root) parent.text += run;
            else {
                element_t text;
                text.kind = element_kind_t::text_k;
                text.key = mint_element_key();
                text.text = std::move(run);
                parent.children.push_back(std::move(text));
            }
            run.clear();
        };
        while (at_ < text_.size()) {
            if (text_[at_] != '<') {
                if (!characters(run)) return malformed;
                continue;
            }
            if (text_.substr(at_).starts_with("<!--")) {
                std::size_t const end = text_.find("-->", at_ + 4);
                if (end == std::string_view::npos) return malformed;
                flush();
                element_t comment;
                comment.kind = element_kind_t::comment_k;
                comment.key = mint_element_key();
                comment.text.assign(text_.substr(at_ + 4, end - at_ - 4));
                parent.children.push_back(std::move(comment));
                at_ = end + 3;
                continue;
            }
            bool skipped = false;
            if (status_t const status = skip_markup(skipped); status.failed()) return status;
            if (skipped) continue;
            if (text_.substr(at_).starts_with("<![CDATA[")) {
                std::size_t const end = text_.find("]]>", at_);
                if (end == std::string_view::npos) return malformed;
                run.append(text_.substr(at_ + 9, end - at_ - 9));
                at_ = end + 3;
                continue;
            }
            if (text_.substr(at_).starts_with("</")) {
                if (!closing) return malformed;
                at_ += 2;
                std::string closed;
                if (!name(closed) || closed != *closing) return malformed;
                skip_space();
                if (at_ >= text_.size() || text_[at_] != '>') return malformed;
                ++at_;
                flush();
                return {};
            }
            flush();
            ++at_;
            element_t child;
            child.key = mint_element_key();
            if (!name(child.tag)) return malformed;
            if (!attributes(child)) return malformed;
            if (text_[at_] == '/') {
                if (text_.substr(at_, 2) != "/>") return malformed;
                at_ += 2;
            }
            else {
                ++at_;
                std::string const tag = child.tag;
                if (status_t const status = content(child, &tag, depth + 1, holder_t::element_k); status.failed())
                    return status;
            }
            parent.children.push_back(std::move(child));
        }
        if (closing) return malformed;
        flush();
        return {};
    }

  public:
    explicit xml_parser_t(std::string_view text) noexcept : text_(text) {}

    /** Parses a whole fragment into the children of @p holder, which keeps no text of its own. */
    status_t parse(element_t& holder) { return content(holder, nullptr, 0, holder_t::root_k); }
};

void escape_xml(std::string& out, std::string_view text) {
    for (char const character : text) {
        switch (character) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out.push_back(character);
        }
    }
}

/** Renders @p element as XML: a named one as a tag around its content, or a tag alone without
 *  any, and one without a name, a text element or the root, as its content alone. */
void render_xml(std::string& out, element_t const& element) {
    if (element.kind == element_kind_t::text_k) return escape_xml(out, element.text);
    if (element.kind == element_kind_t::comment_k) {
        out += "<!--";
        out += element.text;
        out += "-->";
        return;
    }
    bool const named = !element.tag.empty();
    if (named) {
        out.push_back('<');
        out += element.tag;
        for (auto const& [name, value] : element.attributes) {
            out.push_back(' ');
            out += name;
            out += "=\"";
            escape_xml(out, value);
            out.push_back('"');
        }
        if (element.text.empty() && element.children.empty()) {
            out += "/>";
            return;
        }
        out.push_back('>');
    }
    escape_xml(out, element.text);
    for (element_t const& child : element.children) render_xml(out, child);
    if (named) {
        out += "</";
        out += element.tag;
        out.push_back('>');
    }
}

void render_text(std::string& out, element_t const& element) {
    if (element.kind == element_kind_t::comment_k) return;
    out += element.text;
    for (element_t const& child : element.children) render_text(out, child);
}

/** Renders @p element in @p encoding. */
void render(std::string& out, element_t const& element, ustore_page_encoding_t encoding) {
    if (encoding == ustore_page_encoding_text_k) render_text(out, element);
    else render_xml(out, element);
}

/** Refuses an encoding no constant names. */
bool refuse_unknown_encoding(call_t& call, ustore_page_encoding_t encoding) noexcept {
    auto const bits = std::bit_cast<std::underlying_type_t<ustore_page_encoding_t>>(encoding);
    if (bits > ustore_page_encoding_json_k) return call.stop(ustore_status_invalid_argument_k, "Unknown encoding");
    return false;
}

/** Refuses an encoding the stand-in, as the engine, does not import or render yet: HTML,
 *  Markdown and JSON. */
bool refuse_unserved_encoding(call_t& call, ustore_page_encoding_t encoding) noexcept {
    auto const bits = std::bit_cast<std::underlying_type_t<ustore_page_encoding_t>>(encoding);
    if (bits != ustore_page_encoding_xml_k && bits != ustore_page_encoding_text_k)
        return call.stop(ustore_status_unsupported_k, "HTML, Markdown and JSON are not imported or rendered yet");
    return false;
}

/** Parses @p markup in @p encoding into the children and text of @p holder. */
status_t parse_markup(std::string_view markup, ustore_page_encoding_t encoding, element_t& holder) {
    if (encoding == ustore_page_encoding_text_k) {
        if (markup.empty()) return {};
        element_t text;
        text.kind = element_kind_t::text_k;
        text.key = mint_element_key();
        text.text.assign(markup);
        holder.children.push_back(std::move(text));
        return {};
    }
    return xml_parser_t(markup).parse(holder);
}

#pragma endregion XML

#pragma region Filters

/** One step of the XPath subset: a child or descendant axis, a name or a wildcard,
 *  and attribute tests. */
struct xpath_step_t {
    bool descendants = false;
    std::string name;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<bool> valued;
};

/** Whether @p character may continue a name of the XPath subset. */
inline bool xpath_name_character(char character) noexcept {
    return std::isalnum(static_cast<unsigned char>(character)) || character == '_' || character == '-' ||
           character == ':' || character == '.';
}

/** Parses one bracketed attribute test, @c [@name] or @c [@name='value'], at @p at into @p step. */
bool parse_attribute_test(std::string_view text, std::size_t& at, xpath_step_t& step) {
    ++at;
    if (at >= text.size() || text[at] != '@') return false;
    std::size_t const begin = ++at;
    while (at < text.size() && xpath_name_character(text[at])) ++at;
    if (at == begin) return false;
    std::string attribute(text.substr(begin, at - begin));
    std::string value;
    bool const valued = at < text.size() && text[at] == '=';
    if (valued) {
        ++at;
        if (at >= text.size() || (text[at] != '\'' && text[at] != '"')) return false;
        char const quote = text[at++];
        std::size_t const end = text.find(quote, at);
        if (end == std::string_view::npos) return false;
        value.assign(text.substr(at, end - at));
        at = end + 1;
    }
    if (at >= text.size() || text[at] != ']') return false;
    ++at;
    step.attributes.emplace_back(std::move(attribute), std::move(value));
    step.valued.push_back(valued);
    return true;
}

bool parse_xpath(std::string_view text, std::vector<xpath_step_t>& steps) {
    steps.clear();
    std::size_t at = 0;
    auto is_name = [](char character) {
        return std::isalnum(static_cast<unsigned char>(character)) || character == '_' || character == '-' ||
               character == ':' || character == '.';
    };
    while (at < text.size()) {
        if (text[at] != '/') return false;
        xpath_step_t step;
        ++at;
        if (at < text.size() && text[at] == '/') step.descendants = true, ++at;
        if (at < text.size() && text[at] == '*') step.name = "*", ++at;
        else {
            std::size_t const begin = at;
            while (at < text.size() && is_name(text[at])) ++at;
            if (at == begin) return false;
            step.name.assign(text.substr(begin, at - begin));
        }
        while (at < text.size() && text[at] == '[')
            if (!parse_attribute_test(text, at, step)) return false;
        steps.push_back(std::move(step));
    }
    return !steps.empty();
}

bool step_matches(xpath_step_t const& step, element_t const& element) {
    if (step.name != "*" && step.name != element.tag) return false;
    for (std::size_t index = 0; index != step.attributes.size(); ++index) {
        bool found = false;
        for (auto const& [name, value] : element.attributes)
            if (name == step.attributes[index].first && (!step.valued[index] || value == step.attributes[index].second))
                found = true;
        if (!found) return false;
    }
    return true;
}

void collect_descendants(element_t const& element, std::vector<element_t const*>& out) {
    for (element_t const& child : element.children) out.push_back(&child), collect_descendants(child, out);
}

/** Appends to @p next the children or descendants of @p node that @p step matches. */
void select_step(xpath_step_t const& step, element_t const& node, std::vector<element_t const*>& next) {
    std::vector<element_t const*> candidates;
    if (step.descendants) collect_descendants(node, candidates);
    else
        for (element_t const& child : node.children) candidates.push_back(&child);
    for (element_t const* candidate : candidates)
        if (step_matches(step, *candidate)) next.push_back(candidate);
}

/** Whether a page whose root is @p root holds a node @p steps select. */
bool xpath_selects(std::vector<xpath_step_t> const& steps, element_t const& root) {
    std::vector<element_t const*> context {&root};
    for (xpath_step_t const& step : steps) {
        std::vector<element_t const*> next;
        for (element_t const* node : context) select_step(step, *node, next);
        if (next.empty()) return false;
        context = std::move(next);
    }
    return true;
}

bool xpath_keeps(entry_t const& entry, void const* state) {
    element_t root;
    if (decode_page(entry.value, root).failed()) return false;
    return xpath_selects(*static_cast<std::vector<xpath_step_t> const*>(state), root);
}

bool compile_xpath(call_t& call, ustore_bytes_view_t filter, verdict_t& verdict) {
    verdict = {};
    std::string_view const text = text_of(filter);
    if (text.empty()) return true;
    auto steps = std::make_shared<std::vector<xpath_step_t>>();
    if (!parse_xpath(text, *steps)) return call.fail(ustore_status_invalid_argument_k, "The filter does not compile");
    verdict.keeps = &xpath_keeps;
    verdict.state = std::move(steps);
    return true;
}

#pragma endregion Filters

#pragma region Edits

/** Whether a page edit names elements, as every edit but an import does. */
template <typename arguments_type_>
concept edits_elements_concept = requires(arguments_type_ const& arguments) { arguments.elements; };

/** Whether a page edit names the elements it edits, or may leave them out, as an import and a
 *  range removal do. */
enum class elements_t : bool { optional_k, required_k };

/** The checks every page edit shares before it claims anything. */
template <typename arguments_type_>
bool check_page_edit(call_t& call, arguments_type_* arguments, elements_t elements) {
    bool const needs_elements = elements == elements_t::required_k;
    if (refuse_database(call, arguments->database)) return false;
    std::size_t const count = arguments->count;
    if (refuse_collections(call, arguments->collections, count)) return false;
    if (count && !arguments->keys.data)
        return call.fail(ustore_status_invalid_argument_k, "The call needs the keys of the pages");
    if constexpr (edits_elements_concept<arguments_type_>)
        if (needs_elements && count && !arguments->elements.data)
            return call.fail(ustore_status_invalid_argument_k, "The call needs the elements it edits");
    if (count > 0xFFFFFFFFull) return call.fail(ustore_status_invalid_argument_k, "One call edits past 4 Gi tasks");
    return true;
}

/** Checks the choices, the transaction and the collections of a page edit, refuses a read-only
 *  database and an @p encoding not imported yet, then claims the executor and the transaction. */
template <typename arguments_type_>
bool claim_page_edit(call_t& call, arguments_type_* arguments, ustore_page_encoding_t const* encoding = nullptr) {
    if (refuse_write_choices(call, arguments->transaction, arguments->durability, arguments->watch) ||
        refuse_write_transaction(call, arguments->transaction, arguments->database) ||
        !pre_resolve(call, arguments->database, arguments->transaction, arguments->collections, arguments->count,
                     ustore_modality_pages_k) ||
        refuse_read_only(call, arguments->database) || (encoding && refuse_unserved_encoding(call, *encoding)))
        return false;
    return call.hold_executor(arguments->executor) && hold_transaction(call, arguments->transaction);
}

/**
 *  @brief Replays the edits of a call over the pages they name, each page decoded once and
 *      written back once, the tasks of one call applying in order, so a later task sees what an
 *      earlier one minted.
 *
 *  @p edit takes the index of a task, the root of its page and a flag it sets once it removed
 *  the whole page, which a later task of the call then finds missing, and records its refusals
 *  into @p call.
 */
template <typename arguments_type_, typename edit_type_>
void edit_pages(call_t& call, arguments_type_* arguments, edit_type_&& edit) {
    std::size_t const count = arguments->count;
    write_through(
        call, arguments->database, arguments->transaction, arguments->watch, arguments->durability,
        [&](reader_t const& reader, writer_t& writer) {
            std::map<std::pair<std::string, std::string>, element_t> pages;
            std::set<std::pair<std::string, std::string>> removed;
            std::vector<std::pair<collection_t const*, std::string>> order;
            for (std::size_t index = 0; index != count; ++index) {
                collection_t const* const collection = reader.find(
                    bytes_of(strided_element(arguments->collections.data, arguments->collections.stride, index)));
                if (!collection) return call.refuse(ustore_status_not_found_k, "A collection was dropped meanwhile");
                ustore_key_t const& key = strided_element(arguments->keys.data, arguments->keys.stride, index);
                std::pair<std::string, std::string> const place {bytes_of(collection->id), bytes_of(key)};
                if (removed.count(place))
                    return call.refuse(ustore_status_not_found_k, "An edited page does not exist");
                auto found = pages.find(place);
                if (found == pages.end()) {
                    std::string const* const stored = reader.get(*collection, place.second);
                    if (!stored) return call.refuse(ustore_status_not_found_k, "An edited page does not exist");
                    element_t root;
                    if (call.failed_with(decode_page(*stored, root))) return;
                    found = pages.emplace(place, std::move(root)).first;
                    order.emplace_back(collection, place.second);
                }
                bool gone = false;
                edit(index, found->second, gone);
                if (call.failed()) return;
                if (gone) removed.insert(place);
            }
            for (auto const& [collection, key] : order) {
                std::pair<std::string, std::string> const place {bytes_of(collection->id), key};
                if (removed.count(place)) writer.remove(*collection, key);
                else writer.put(*collection, key, encode_page(pages[place]), ustore_change_change_k);
            }
        });
}

/** The element a parent key names: the root for nil, or a live element of the element
 *  kind, else null. */
element_t* parent_of(element_t& root, ustore_key_t const& key) {
    element_t* const parent = is_nil(key) ? &root : find_element(root, key);
    return parent && parent->kind == element_kind_t::element_k ? parent : nullptr;
}

/** Where an element lands among @p parent's children: past @p anchor, or the front for a nil
 *  or foreign anchor. */
std::size_t landing_position(element_t const& parent, ustore_key_t const& anchor) {
    for (std::size_t index = 0; !is_nil(anchor) && index != parent.children.size(); ++index)
        if (std::memcmp(parent.children[index].key.uuid.bytes, anchor.uuid.bytes, 16) == 0) return index + 1;
    return 0;
}

/** Whether any key of @p fragment is live in @p root. */
bool collides(element_t const& root, element_t const& fragment) {
    if (holds_key(root, fragment.key)) return true;
    for (element_t const& child : fragment.children)
        if (collides(root, child)) return true;
    return false;
}

#pragma endregion Edits

status_t import_page(std::string_view markup, ustore_key_t const& page_key, std::string& stored) {
    element_t root;
    root.key = page_key;
    if (status_t const status = parse_markup(markup, ustore_page_encoding_xml_k, root); status.failed()) return status;
    stored = encode_page(root);
    return {};
}

} // namespace unum::ustore::stub

/** What @c ustore_pages_cursor_t names. */
struct ustore_pages_cursor_handle_t {
    keyed_cursor_t cursor;
};

void ustore_pages_import(ustore_pages_import_t* arguments) {
    call_t call(arguments->outcome);
    if (!check_page_edit(call, arguments, elements_t::optional_k)) return;
    std::size_t const count = arguments->count;
    strings_in_t values;
    if (!read_strings(call, arguments->values, count, values)) return;
    if (count && !values.given) return call.refuse(ustore_status_invalid_argument_k, "No values were provided");
    if (refuse_unknown_encoding(call, arguments->encoding) || !claim_page_edit(call, arguments, &arguments->encoding))
        return;
    std::vector<std::string> stored(count);
    std::size_t total = 0, parsed = 0;
    for (std::size_t index = 0; index != count; ++index) total += values[index].size();
    for (std::size_t index = 0; index != count; ++index) {
        element_t root;
        root.key = strided_element(arguments->keys.data, arguments->keys.stride, index);
        if (call.failed_with(parse_markup(values[index], arguments->encoding, root))) return;
        stored[index] = encode_page(root);
        parsed += values[index].size();
        if (!carries_on(arguments->progress, arguments->progress_state, parsed, total))
            return call.refuse(ustore_status_cancelled_k, "The progress callback cancelled the import");
    }
    write_through(
        call, arguments->database, arguments->transaction, arguments->watch, arguments->durability,
        [&](reader_t const& reader, writer_t& writer) {
            for (std::size_t index = 0; index != count; ++index) {
                collection_t const* const collection = reader.find(
                    bytes_of(strided_element(arguments->collections.data, arguments->collections.stride, index)));
                if (!collection) return call.refuse(ustore_status_not_found_k, "A collection was dropped meanwhile");
                writer.put(*collection, bytes_of(strided_element(arguments->keys.data, arguments->keys.stride, index)),
                           std::move(stored[index]));
            }
        });
}

void ustore_pages_insert(ustore_pages_insert_t* arguments) {
    call_t call(arguments->outcome);
    if (arguments->effective_keys) *arguments->effective_keys = nullptr;
    if (!check_page_edit(call, arguments, elements_t::optional_k)) return;
    std::size_t const count = arguments->count;
    strings_in_t values;
    if (!arguments->arena) return call.refuse(ustore_status_invalid_argument_k, "The call needs an arena");
    if (!read_strings(call, arguments->values, count, values)) return;
    if (count && !values.given) return call.refuse(ustore_status_invalid_argument_k, "No values were provided");
    if (refuse_unknown_encoding(call, arguments->encoding) || !claim_page_edit(call, arguments, &arguments->encoding))
        return;
    ustore_arena_handle_t* const arena = claim_arena(
        call, arguments->arena,
        {arguments->collections.data, arguments->keys.data, arguments->elements.data, arguments->parents.data,
         arguments->anchors.data, first_input(arguments->values), data_input(arguments->values)});
    if (!arena) return;
    std::vector<ustore_key_t> effective(count);
    auto task_key = [](ustore_keys_view_t keys, std::size_t index) {
        return keys.data ? strided_element(keys.data, keys.stride, index) : ustore_key_t {};
    };
    // An edit naming a removed parent or a live key is skipped, as the engine's fold skips it
    edit_pages(call, arguments, [&](std::size_t index, element_t& root, bool&) {
        element_t holder;
        if (call.failed_with(parse_markup(values[index], arguments->encoding, holder)) || holder.children.empty())
            return;
        ustore_key_t const given = task_key(arguments->elements, index);
        if (!is_nil(given)) holder.children.front().key = given;
        effective[index] = holder.children.front().key;
        element_t* const parent = parent_of(root, task_key(arguments->parents, index));
        if (!parent) return;
        for (element_t const& node : holder.children)
            if (collides(root, node)) return;
        std::size_t const position = landing_position(*parent, task_key(arguments->anchors, index));
        parent->children.insert(parent->children.begin() + static_cast<std::ptrdiff_t>(position),
                                std::make_move_iterator(holder.children.begin()),
                                std::make_move_iterator(holder.children.end()));
    });
    if (call.failed() || !arguments->effective_keys) return;
    auto* const keys = carve<ustore_key_t>(call, *arena, count);
    if (call.failed()) return;
    std::copy(effective.begin(), effective.end(), keys);
    *arguments->effective_keys = keys;
}

void ustore_pages_remove(ustore_pages_remove_t* arguments) {
    call_t call(arguments->outcome);
    bool const ranges = arguments->end_keys.data != nullptr;
    if (!check_page_edit(call, arguments, ranges ? elements_t::optional_k : elements_t::required_k)) return;
    if (ranges && arguments->elements.data)
        return call.refuse(ustore_status_invalid_argument_k, "A range removes whole pages, naming no elements");
    if (!claim_page_edit(call, arguments)) return;
    if (ranges)
        return write_through(call, arguments->database, arguments->transaction, arguments->watch, arguments->durability,
                             [&](reader_t const& reader, writer_t& writer) {
                                 remove_keyed(call, reader, writer, arguments->collections, arguments->keys,
                                              arguments->end_keys, arguments->count, ustore_modality_pages_k);
                             });
    edit_pages(call, arguments, [&](std::size_t index, element_t& root, bool& gone) {
        ustore_key_t const& element = strided_element(arguments->elements.data, arguments->elements.stride, index);
        element_t* parent = nullptr;
        std::size_t position = 0;
        // A nil element removes the whole page, and the page key empties only its root
        if (is_nil(element)) {
            gone = true;
            return;
        }
        element_t* const found = find_element(root, element, &parent, &position);
        if (!found) return;
        if (!parent) {
            root.children.clear(), root.text.clear(), root.attributes.clear();
            return;
        }
        parent->children.erase(parent->children.begin() + static_cast<std::ptrdiff_t>(position));
    });
}

void ustore_pages_move(ustore_pages_move_t* arguments) {
    call_t call(arguments->outcome);
    if (!check_page_edit(call, arguments, elements_t::required_k) || !claim_page_edit(call, arguments)) return;
    auto task_key = [](ustore_keys_view_t keys, std::size_t index) {
        return keys.data ? strided_element(keys.data, keys.stride, index) : ustore_key_t {};
    };
    edit_pages(call, arguments, [&](std::size_t index, element_t& root, bool&) {
        ustore_key_t const element = task_key(arguments->elements, index);
        element_t* old_parent = nullptr;
        std::size_t old_position = 0;
        element_t* const found = find_element(root, element, &old_parent, &old_position);
        ustore_key_t const anchor = task_key(arguments->anchors, index);
        if (!found || !old_parent || std::memcmp(anchor.uuid.bytes, element.uuid.bytes, 16) == 0) return;
        ustore_key_t const parent_key = task_key(arguments->parents, index);
        element_t const* const parent_found = parent_of(root, parent_key);
        if (!parent_found || holds_key(*found, is_nil(parent_key) ? root.key : parent_key)) return;
        element_t moved = std::move(*found);
        old_parent->children.erase(old_parent->children.begin() + static_cast<std::ptrdiff_t>(old_position));
        // The parent is found again, as the removal above may have shifted it among its siblings
        element_t* const parent = parent_of(root, parent_key);
        std::size_t const position = landing_position(*parent, anchor);
        parent->children.insert(parent->children.begin() + static_cast<std::ptrdiff_t>(position), std::move(moved));
    });
}

void ustore_pages_splice(ustore_pages_splice_t* arguments) {
    call_t call(arguments->outcome);
    if (!check_page_edit(call, arguments, elements_t::required_k)) return;
    strings_in_t values;
    if (!read_strings(call, arguments->values, arguments->count, values) || !claim_page_edit(call, arguments)) return;
    edit_pages(call, arguments, [&](std::size_t index, element_t& root, bool&) {
        element_t* const element = find_element(
            root, strided_element(arguments->elements.data, arguments->elements.stride, index));
        if (!element) return;
        std::size_t const start = smaller_of<std::size_t>(
            arguments->text_starts.data
                ? strided_element(arguments->text_starts.data, arguments->text_starts.stride, index)
                : 0,
            element->text.size());
        std::size_t const length = smaller_of<std::size_t>(
            arguments->text_lengths.data
                ? strided_element(arguments->text_lengths.data, arguments->text_lengths.stride, index)
                : 0,
            element->text.size() - start);
        element->text.replace(start, length, values.given ? values[index] : std::string_view {});
    });
}

void ustore_pages_set_attributes(ustore_pages_set_attributes_t* arguments) {
    call_t call(arguments->outcome);
    if (!check_page_edit(call, arguments, elements_t::required_k)) return;
    strings_in_t fields, values;
    if (!read_strings(call, arguments->fields, arguments->count, fields) ||
        !read_strings(call, arguments->values, arguments->count, values))
        return;
    if (arguments->count && (!fields.given || !values.given))
        return call.refuse(ustore_status_invalid_argument_k, "The call needs the attributes and their values");
    if (!claim_page_edit(call, arguments)) return;
    edit_pages(call, arguments, [&](std::size_t index, element_t& root, bool&) {
        element_t* const element = find_element(
            root, strided_element(arguments->elements.data, arguments->elements.stride, index));
        if (!element || element->kind != element_kind_t::element_k) return;
        for (auto& [name, value] : element->attributes)
            if (name == fields[index]) {
                value.assign(values[index]);
                return;
            }
        element->attributes.emplace_back(std::string(fields[index]), std::string(values[index]));
    });
}

void ustore_pages_remove_attributes(ustore_pages_remove_attributes_t* arguments) {
    call_t call(arguments->outcome);
    if (!check_page_edit(call, arguments, elements_t::required_k)) return;
    strings_in_t fields;
    if (!read_strings(call, arguments->fields, arguments->count, fields)) return;
    if (arguments->count && !fields.given)
        return call.refuse(ustore_status_invalid_argument_k, "The call needs the attributes");
    if (!claim_page_edit(call, arguments)) return;
    edit_pages(call, arguments, [&](std::size_t index, element_t& root, bool&) {
        element_t* const element = find_element(
            root, strided_element(arguments->elements.data, arguments->elements.stride, index));
        if (!element || element->kind != element_kind_t::element_k) return;
        auto& attributes = element->attributes;
        for (auto it = attributes.begin(); it != attributes.end(); ++it)
            if (it->first == fields[index]) {
                attributes.erase(it);
                return;
            }
    });
}

void ustore_pages_read(ustore_pages_read_t* arguments) {
    call_t call(arguments->outcome);
    if (arguments->presences) *arguments->presences = nullptr;
    if (arguments->lengths) *arguments->lengths = nullptr;
    if (arguments->values) *arguments->values = {};
    if (refuse_database(call, arguments->database)) return;
    std::size_t const count = arguments->count;
    if (!arguments->arena) return call.refuse(ustore_status_invalid_argument_k, "The call needs an arena");
    if (refuse_collections(call, arguments->collections, count)) return;
    if (count && !arguments->keys.data) return call.refuse(ustore_status_invalid_argument_k, "No keys were provided");
    if (refuse_unknown_encoding(call, arguments->outputs_encoding)) return;
    view_spec_t spec;
    std::vector<collection_t const*> resolved;
    if (!open_listing(call, *arguments, ustore_modality_pages_k, spec, resolved)) return;
    // Presences alone render nothing, so only the renderings refuse an encoding not rendered yet
    if ((arguments->values || arguments->lengths) && refuse_unserved_encoding(call, arguments->outputs_encoding))
        return;
    if (!call.hold_executor(arguments->executor)) return;
    ustore_arena_handle_t* const arena = claim_arena(
        call, arguments->arena, {arguments->collections.data, arguments->keys.data, arguments->elements.data});
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
            element_t root;
            if (call.failed_with(decode_page(*stored, root))) return;
            ustore_key_t const element_key = arguments->elements.data
                                                 ? strided_element(arguments->elements.data, arguments->elements.stride,
                                                                   index)
                                                 : ustore_key_t {};
            element_t const* const element = is_nil(element_key) ? &root : find_element(root, element_key);
            if (!element) continue;
            render(values[index], *element, arguments->outputs_encoding);
            present[index] = true;
        }
    }
    catch (std::bad_alloc const&) {
        return call.refuse(ustore_status_out_of_memory_k, "Out of memory while rendering");
    }
    write_values(call, *arena, values, present, arguments->presences, arguments->lengths, arguments->values);
}

void ustore_pages_scan(ustore_pages_scan_t* arguments) {
    call_t call(arguments->outcome);
    if (!check_scan(call, arguments)) return;
    verdict_t verdict;
    if (!compile_xpath(call, arguments->filter, verdict)) return;
    scan_keyed(call, arguments, ustore_modality_pages_k, nullptr, verdict);
}

void ustore_pages_cursor_init(ustore_pages_cursor_init_t* arguments) {
    call_t call(arguments->outcome);
    if (arguments->cursor) *arguments->cursor = nullptr;
    if (refuse_database(call, arguments->database)) return;
    if (!arguments->cursor)
        return call.refuse(ustore_status_invalid_argument_k, "The call needs an output for the cursor");
    if (refuse_direction(call, arguments->direction) || refuse_predicate(call, arguments->predicate)) return;
    auto* const cursor = new (std::nothrow) ustore_pages_cursor_handle_t {};
    if (!cursor) return call.refuse(ustore_status_out_of_memory_k, "Failed to allocate the cursor");
    if (!compile_xpath(call, arguments->filter, cursor->cursor.verdict) ||
        !open_keyed_cursor(call, arguments, ustore_modality_pages_k, cursor->cursor))
        return delete cursor;
    *arguments->cursor = cursor;
}

void ustore_pages_cursor_next(ustore_pages_cursor_next_t* arguments) {
    call_t call(arguments->outcome);
    keyed_cursor_t* const cursor = arguments->cursor ? &arguments->cursor->cursor : nullptr;
    if (refuse_unknown_encoding(call, arguments->outputs_encoding)) return;
    ustore_arena_handle_t* const arena = open_pull(call, arguments, cursor);
    if (!arena) return;
    if (arguments->values && refuse_unserved_encoding(call, arguments->outputs_encoding)) return;
    std::vector<entry_t> listed;
    if (!pull_keyed(call, arguments, *cursor, listed)) return;
    ustore_strings_view_t values {};
    if (arguments->values && !listed.empty()) {
        std::vector<std::string> texts(listed.size());
        for (std::size_t index = 0; index != listed.size(); ++index) {
            element_t root;
            if (call.failed_with(decode_page(listed[index].value, root))) return;
            render(texts[index], root, arguments->outputs_encoding);
        }
        if (!write_tape(call, *arena, texts, values)) return;
    }
    if (!write_page_keys(call, arguments, *arena, listed)) return;
    if (arguments->values) *arguments->values = values;
}

void ustore_pages_cursor_free(ustore_pages_cursor_t cursor) {
    if (!cursor) return;
    close_keyed_cursor(cursor->cursor);
    delete cursor;
}

void ustore_pages_sample(ustore_pages_sample_t* arguments) { sample_keyed(arguments, ustore_modality_pages_k); }

void ustore_pages_measure(ustore_pages_measure_t* arguments) { measure_keyed(arguments, ustore_modality_pages_k); }
