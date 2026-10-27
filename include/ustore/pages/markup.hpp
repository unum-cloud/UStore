/**
 *  @file include/ustore/pages/markup.hpp
 *  @author Ash Vardanian
 *  @date October 16, 2026
 *  @brief Markup in and out of element trees: XML and plain text imported through an own tokenizer and rendered back,
 *      and XPath spans over rendered XML through StringZilla's subdocuments.
 *
 *  The tokenizer reads elements, attributes, character data, CDATA sections and comments, skips the
 *  XML declaration, processing instructions and a DOCTYPE without an internal subset, and refuses
 *  what well-formed XML forbids as @c status_code_t::malformed_k, as <tt>c/ustore/pages.h</tt>
 *  answers a fragment that does not parse: unbalanced tags, a repeated attribute, a <tt>&lt;</tt>
 *  inside an attribute value, <tt>]]&gt;</tt> inside character data, and any <tt>&amp;</tt>
 *  starting neither one of the five predefined entities nor a reference to a character XML allows.
 *  Those are decoded in one batch per document by StringZilla's HTML unescape.
 *
 *  The tree follows the character data: an element's text before its first child is its own text run, every later run
 *  becomes a text element, CDATA joins the run around it, and the root holds no text of its own, so a fragment's
 *  leading text lands as an element of its own. Attribute values keep their whitespace unnormalized.
 *
 *  Rendering escapes <tt>&amp;</tt>, <tt>&lt;</tt> and <tt>&gt;</tt> in text and <tt>&amp;</tt>,
 *  <tt>&lt;</tt> and <tt>"</tt> in attributes, writes elements without content as
 *  <tt>&lt;name/&gt;</tt>, and renders an element without a name, such as the root, as its content
 *  alone. Importing a rendering renders it again byte for byte. HTML, Markdown and the JSON tree
 *  answer @c status_code_t::unsupported_k for now.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`
#include <cstring> // `std::memchr`, `std::memcpy`

#include <memory>      // `std::allocator_traits`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`
#include <stringzilla/strings.h>       // `sz_strings_unescape_best`
#include <stringzilla/subdocuments.h>  // `sz_subdocuments_engine_init`, `sz_subdocuments_find`

#include "ustore/pages/element_tree.hpp" // `element_tree`, `element_key_t`, `placement_t`

namespace unum::ustore::pages {

/** The encoding a page or a fragment is imported from or rendered into, numbered as @c ustore_page_encoding_t. */
enum class page_encoding_t : std::uint8_t {

    /** HTML, unsupported until the parser decision lands. */
    html_k = 0,

    /** XML, with every element and attribute kept as written. */
    xml_k = 1,

    /** Markdown, unsupported for now. */
    markdown_k = 2,

    /** The text of every run in document order. */
    text_k = 3,

    /** The element tree as JSON, unsupported for now. */
    json_k = 4,
};

/** Where an element's rendering spans in its output, from its <tt>&lt;</tt> through its end tag. */
struct element_span_t {

    /** The element rendered. */
    element_index_t element = no_element_k;

    /** Offset of its first byte in the output. */
    std::size_t begin = 0;

    /** Offset past its last byte in the output. */
    std::size_t end = 0;
};

#pragma region StringZilla Bridge

/** Serves StringZilla's allocations from a rebound copy of @p allocator_type_, aligned for the offsets it stores. */
template <typename allocator_type_>
class stringzilla_allocator {
    struct alignas(16) chunk_t {
        std::byte bytes[16];
    };
    using chunks_allocator_t = typename std::allocator_traits<allocator_type_>::template rebind_alloc<chunk_t>;
    chunks_allocator_t allocator_;

    static std::size_t chunks(sz_size_t bytes) noexcept { return (bytes + sizeof(chunk_t) - 1) / sizeof(chunk_t); }
    static void* allocate(sz_size_t bytes, void* handle, sz_stream_t) noexcept {
        return static_cast<stringzilla_allocator*>(handle)->allocator_.allocate(chunks(bytes));
    }
    static void free(void* pointer, sz_size_t bytes, void* handle, sz_stream_t) noexcept {
        if (pointer)
            static_cast<stringzilla_allocator*>(handle)->allocator_.deallocate(static_cast<chunk_t*>(pointer),
                                                                               chunks(bytes));
    }
    static void* reallocate(void* pointer, sz_size_t old_bytes, sz_size_t new_bytes, void* handle,
                            sz_stream_t stream) noexcept {
        void* const grown = allocate(new_bytes, handle, stream);
        if (!grown) return nullptr;
        if (pointer) std::memcpy(grown, pointer, old_bytes < new_bytes ? old_bytes : new_bytes);
        free(pointer, old_bytes, handle, stream);
        return grown;
    }

  public:
    explicit stringzilla_allocator(allocator_type_ allocator) noexcept : allocator_(allocator) {}
    stringzilla_allocator(stringzilla_allocator const&) = delete;
    stringzilla_allocator& operator=(stringzilla_allocator const&) = delete;

    /** The C allocator handing requests to this object, which must outlive it. */
    sz_allocator_t c_allocator() noexcept {
        sz_allocator_t result;
        result.allocate = &allocate;
        result.reallocate = &reallocate;
        result.free = &free;
        result.handle = this;
        return result;
    }
};

/** A strings view over an array of StringZilla views, read through the accessor layout. */
inline sz_strings_view_t strings_of(sz_string_view_t const* views, std::size_t count) noexcept {
    sz_strings_view_t strings;
    strings.layout = sz_strings_accessor_k;
    strings.count = count;
    strings.data.accessor.handle = views;
    strings.data.accessor.get = [](void const* handle, sz_size_t const* indices, sz_size_t length,
                                   sz_string_view_t* outputs) {
        sz_string_view_t const* const views = static_cast<sz_string_view_t const*>(handle);
        for (sz_size_t index = 0; index != length; ++index) outputs[index] = views[indices[index]];
    };
    return strings;
}

/** The StringZilla view of @p text. */
inline sz_string_view_t stringzilla_view(std::string_view text) noexcept {
    sz_string_view_t view;
    view.start = text.data();
    view.length = text.size();
    return view;
}

/** The string @p index of a tape StringZilla wrote. */
inline std::string_view tape_string(sz_strings_span_t const& tape, std::size_t index) noexcept {
    if (tape.layout == sz_strings_u32_tape_k)
        return {tape.data.u32_tape.data + tape.data.u32_tape.offsets[index],
                std::size_t(tape.data.u32_tape.offsets[index + 1] - tape.data.u32_tape.offsets[index])};
    return {tape.data.u64_tape.data + tape.data.u64_tape.offsets[index],
            std::size_t(tape.data.u64_tape.offsets[index + 1] - tape.data.u64_tape.offsets[index])};
}

/** The pages status of a StringZilla failure. */
inline status_t stringzilla_status(sz_status_t status, char const* message) noexcept {
    switch (status) {
    case sz_success_k: return {};
    case sz_bad_alloc_k: return {status_code_t::out_of_memory_k, message};
    case sz_invalid_syntax_k: return {status_code_t::malformed_k, message};
    default: return {status_code_t::unsupported_k, message};
    }
}

#pragma endregion

#pragma region XML Tokenizer

/** Whether @p code is a character XML 1.0 allows a document to carry. */
constexpr bool xml_character(std::uint32_t code) noexcept {
    return code == 0x9 || code == 0xA || code == 0xD || (code >= 0x20 && code <= 0xD7FF) ||
           (code >= 0xE000 && code <= 0xFFFD) || (code >= 0x10000 && code <= 0x10FFFF);
}

/** Whether every <tt>&amp;</tt> of @p run starts one of the five predefined entities or a character
 *  reference to a character XML allows, as well-formed XML without a DOCTYPE subset needs. */
constexpr bool references_well_formed(std::string_view run) noexcept {
    for (std::size_t at = run.find('&'); at != std::string_view::npos; at = run.find('&', at + 1)) {
        std::size_t const end = run.find(';', at + 1);
        if (end == std::string_view::npos) return false;
        std::string_view const name = run.substr(at + 1, end - at - 1);
        at = end;
        if (name == "amp" || name == "lt" || name == "gt" || name == "quot" || name == "apos") continue;
        if (name.size() < 2 || name[0] != '#') return false;
        bool const hexadecimal = name[1] == 'x';
        std::string_view const digits = name.substr(hexadecimal ? 2 : 1);
        if (digits.empty() || digits.size() > 8) return false;
        std::uint32_t code = 0;
        for (char digit : digits) {
            std::uint32_t value = 16;
            if (digit >= '0' && digit <= '9') value = std::uint32_t(digit - '0');
            else if (hexadecimal && digit >= 'a' && digit <= 'f') value = std::uint32_t(digit - 'a' + 10);
            else if (hexadecimal && digit >= 'A' && digit <= 'F') value = std::uint32_t(digit - 'A' + 10);
            if (value >= (hexadecimal ? 16u : 10u)) return false;
            code = code * (hexadecimal ? 16 : 10) + value;
        }
        if (!xml_character(code)) return false;
    }
    return true;
}

/** What a token of XML is. */
enum class xml_token_kind_t : std::uint8_t {

    /** A start tag, <tt>&lt;name ...&gt;</tt>. */
    open_k = 0,

    /** An empty-element tag, <tt>&lt;name .../&gt;</tt>. */
    empty_k = 1,

    /** An end tag, <tt>&lt;/name&gt;</tt>. */
    close_k = 2,

    /** Character data between markup. */
    text_k = 3,

    /** The content of a CDATA section, taken as written. */
    cdata_k = 4,

    /** The body of a comment. */
    comment_k = 5,
};

/** One token: its kind, the bytes of its name or content, its attributes, and its unescaped string if any. */
struct xml_token_t {

    /** What the token is. */
    xml_token_kind_t kind = xml_token_kind_t::text_k;

    /** Offset and length in the document of a tag's name, or of character data, CDATA or a comment's body. */
    std::uint32_t offset = 0;
    std::uint32_t length = 0;

    /** The run of the tag's attributes among the tokenizer's attributes. */
    std::uint32_t first_attribute = 0;
    std::uint32_t attributes_count = 0;

    /** The string of the unescaped character data in the tokenizer's tape, or @c no_element_k when
     *  nothing was escaped. */
    std::uint32_t unescaped = no_element_k;
};

/** One attribute of a tag: the bytes of its name and of its value between the quotes. */
struct xml_attribute_token_t {

    /** Offset and length of the name in the document. */
    std::uint32_t name_offset = 0;
    std::uint32_t name_length = 0;

    /** Offset and length of the value between its quotes in the document. */
    std::uint32_t value_offset = 0;
    std::uint32_t value_length = 0;

    /** The string of the unescaped value in the tokenizer's tape, or @c no_element_k when nothing was escaped. */
    std::uint32_t unescaped = no_element_k;
};

/**
 *  @brief Splits XML into tokens in one pass, then decodes every escaped run in one StringZilla batch.
 *
 *  The tokens view the document, which must outlive them, and the unescaped runs view a tape the tokenizer owns.
 */
template <typename allocator_type_>
class xml_tokenizer {
    template <typename value_type_>
    using rebound = typename std::allocator_traits<allocator_type_>::template rebind_alloc<value_type_>;

    struct name_t {
        std::uint32_t offset;
        std::uint32_t length;
    };

    allocator_type_ allocator_;
    std::string_view document_;
    st::basic_vector<xml_token_t, rebound<xml_token_t>> tokens_;
    st::basic_vector<xml_attribute_token_t, rebound<xml_attribute_token_t>> attributes_;
    stringzilla_allocator<allocator_type_> bridge_;
    sz_allocator_t c_allocator_ {};
    sz_strings_span_t tape_ {};

    static bool is_blank(char character) noexcept {
        return character == ' ' || character == '\t' || character == '\n' || character == '\r';
    }
    static bool ends_name(char character) noexcept {
        return is_blank(character) || character == '/' || character == '>' || character == '=' || character == '<' ||
               character == '"' || character == '\'';
    }
    std::size_t find(std::string_view needle, std::size_t from) const noexcept {
        std::size_t const found = document_.find(needle, from);
        return found == std::string_view::npos ? document_.size() : found;
    }
    std::size_t name_end(std::size_t from) const noexcept {
        while (from < document_.size() && !ends_name(document_[from])) ++from;
        return from;
    }
    std::size_t skip_blanks(std::size_t from) const noexcept {
        while (from < document_.size() && is_blank(document_[from])) ++from;
        return from;
    }
    bool escaped(std::size_t offset, std::size_t length) const noexcept {
        return length && std::memchr(document_.data() + offset, '&', length);
    }
    status_t push(xml_token_kind_t kind, std::size_t offset, std::size_t length) noexcept {
        xml_token_t token;
        token.kind = kind, token.offset = std::uint32_t(offset), token.length = std::uint32_t(length);
        token.first_attribute = std::uint32_t(attributes_.size());
        return growth_status(tokens_.push_back(std::move(token)));
    }

    status_t split() noexcept {
        status_t const malformed {status_code_t::malformed_k, "The XML is malformed"};
        st::basic_vector<name_t, rebound<name_t>> open {rebound<name_t>(allocator_)};
        std::size_t const size = document_.size();
        std::size_t at = 0;
        while (at < size) {
            if (document_[at] != '<') {
                std::size_t const next = find("<", at);
                if (document_.substr(at, next - at).find("]]>") != std::string_view::npos) return malformed;
                if (status_t status = push(xml_token_kind_t::text_k, at, next - at); status.failed()) return status;
                at = next;
                continue;
            }
            std::string_view const rest = document_.substr(at);
            if (rest.starts_with("<!--")) {
                std::size_t const end = find("-->", at + 4);
                if (end == size) return malformed;
                if (status_t status = push(xml_token_kind_t::comment_k, at + 4, end - at - 4); status.failed())
                    return status;
                at = end + 3;
            }
            else if (rest.starts_with("<![CDATA[")) {
                std::size_t const end = find("]]>", at + 9);
                if (end == size) return malformed;
                if (status_t status = push(xml_token_kind_t::cdata_k, at + 9, end - at - 9); status.failed())
                    return status;
                at = end + 3;
            }
            else if (rest.starts_with("<?")) {
                std::size_t const end = find("?>", at + 2);
                if (end == size) return malformed;
                at = end + 2;
            }
            else if (rest.starts_with("<!")) {
                std::size_t const end = find(">", at + 2);
                if (end == size) return malformed;
                // Entities declared in an internal subset would need expanding, as StringZilla's XPath refuses too
                if (document_.substr(at, end - at).find('[') != std::string_view::npos)
                    return {status_code_t::unsupported_k, "A DOCTYPE with an internal subset is unsupported"};
                at = end + 1;
            }
            else if (rest.starts_with("</")) {
                std::size_t const end = name_end(at + 2);
                std::size_t const closing = skip_blanks(end);
                if (end == at + 2 || closing == size || document_[closing] != '>' || open.empty()) return malformed;
                name_t const expected_name = open.back();
                if (document_.substr(expected_name.offset, expected_name.length) !=
                    document_.substr(at + 2, end - at - 2))
                    return malformed;
                open.pop_back();
                if (status_t status = push(xml_token_kind_t::close_k, at + 2, end - at - 2); status.failed())
                    return status;
                at = closing + 1;
            }
            else {
                std::size_t const end = name_end(at + 1);
                if (end == at + 1) return malformed;
                if (status_t status = push(xml_token_kind_t::open_k, at + 1, end - at - 1); status.failed())
                    return status;
                std::size_t cursor = skip_blanks(end);
                for (;;) {
                    if (cursor == size) return malformed;
                    if (document_[cursor] == '>') {
                        ++cursor;
                        if (status_t status = growth_status(
                                open.push_back(name_t {std::uint32_t(at + 1), std::uint32_t(end - at - 1)}));
                            status.failed())
                            return status;
                        break;
                    }
                    if (document_[cursor] == '/') {
                        if (cursor + 1 == size || document_[cursor + 1] != '>') return malformed;
                        cursor += 2;
                        tokens_.back().kind = xml_token_kind_t::empty_k;
                        break;
                    }
                    std::size_t const name_stop = name_end(cursor);
                    if (name_stop == cursor) return malformed;
                    std::size_t equals = skip_blanks(name_stop);
                    if (equals == size || document_[equals] != '=') return malformed;
                    std::size_t const quote = skip_blanks(equals + 1);
                    if (quote == size || (document_[quote] != '"' && document_[quote] != '\'')) return malformed;
                    std::size_t const value_end = document_.find(document_[quote], quote + 1);
                    if (value_end == std::string_view::npos) return malformed;
                    if (document_.substr(quote + 1, value_end - quote - 1).find('<') != std::string_view::npos)
                        return malformed;
                    xml_attribute_token_t attribute;
                    attribute.name_offset = std::uint32_t(cursor),
                    attribute.name_length = std::uint32_t(name_stop - cursor);
                    attribute.value_offset = std::uint32_t(quote + 1);
                    attribute.value_length = std::uint32_t(value_end - quote - 1);
                    if (status_t status = growth_status(attributes_.push_back(std::move(attribute))); status.failed())
                        return status;
                    ++tokens_.back().attributes_count;
                    cursor = skip_blanks(value_end + 1);
                }
                at = cursor;
            }
        }
        if (!open.empty()) return malformed;
        return {};
    }

    status_t unescape(sz_capability_t capabilities) noexcept {
        st::basic_vector<sz_string_view_t, rebound<sz_string_view_t>> runs {rebound<sz_string_view_t>(allocator_)};
        auto enlist = [&](std::uint32_t offset, std::uint32_t length, std::uint32_t& unescaped) noexcept -> status_t {
            if (!escaped(offset, length)) return {};
            if (!references_well_formed(document_.substr(offset, length)))
                return {status_code_t::malformed_k, "The XML references an undeclared entity or a forbidden character"};
            unescaped = std::uint32_t(runs.size());
            return growth_status(runs.push_back(sz_string_view_t {document_.data() + offset, length}));
        };
        for (xml_token_t& token : tokens_)
            if (token.kind == xml_token_kind_t::text_k)
                if (status_t status = enlist(token.offset, token.length, token.unescaped); status.failed())
                    return status;
        for (xml_attribute_token_t& attribute : attributes_)
            if (status_t status = enlist(attribute.value_offset, attribute.value_length, attribute.unescaped);
                status.failed())
                return status;
        if (runs.empty()) return {};
        sz_strings_view_t const strings = strings_of(runs.data(), runs.size());
        sz_size_t malformed_index = 0;
        return stringzilla_status(
            sz_strings_unescape_best(&strings, nullptr, runs.size(), sz_escape_html_k, &c_allocator_, &tape_,
                                     &malformed_index, capabilities, nullptr),
            "Failed to unescape XML");
    }

  public:
    explicit xml_tokenizer(allocator_type_ allocator = {}) noexcept
        : allocator_(allocator), tokens_(rebound<xml_token_t>(allocator)),
          attributes_(rebound<xml_attribute_token_t>(allocator)), bridge_(allocator),
          c_allocator_(bridge_.c_allocator()) {}
    xml_tokenizer(xml_tokenizer const&) = delete;
    xml_tokenizer& operator=(xml_tokenizer const&) = delete;
    ~xml_tokenizer() noexcept { sz_strings_span_free(&tape_, &c_allocator_, nullptr); }

    /** Splits @p document into tokens and decodes its escaped runs, refusing documents past 4 GiB. */
    status_t tokenize(std::string_view document, sz_capability_t capabilities) noexcept {
        if (document.size() > ~std::uint32_t(0)) return {status_code_t::invalid_argument_k, "XML past 4 GiB"};
        document_ = document;
        tokens_.clear(), attributes_.clear();
        if (status_t status = split(); status.failed()) return status;
        return unescape(capabilities);
    }

    std::span<xml_token_t const> tokens() const noexcept { return {tokens_.begin(), tokens_.end()}; }
    xml_attribute_token_t const& attribute(std::uint32_t index) const noexcept { return attributes_[index]; }

    /** The name of an element token, or the raw content of a comment or CDATA token. */
    std::string_view raw(xml_token_t const& token) const noexcept {
        return document_.substr(token.offset, token.length);
    }

    /** The character data of a text or CDATA token, decoded. */
    std::string_view content(xml_token_t const& token) const noexcept {
        return token.unescaped == no_element_k ? raw(token) : tape_string(tape_, token.unescaped);
    }

    std::string_view attribute_name(xml_attribute_token_t const& attribute) const noexcept {
        return document_.substr(attribute.name_offset, attribute.name_length);
    }
    std::string_view attribute_value(xml_attribute_token_t const& attribute) const noexcept {
        return attribute.unescaped == no_element_k ? document_.substr(attribute.value_offset, attribute.value_length)
                                                   : tape_string(tape_, attribute.unescaped);
    }
};

#pragma endregion

#pragma region Import

/**
 *  @brief Replaces @p tree with @p xml parsed under a root keyed @p root_key, every element placed at @p placement.
 *  @param[in] xml The document to parse.
 *  @param[in] root_key The key of the root element.
 *  @param[in] placement Where every element is placed.
 *  @param[in] mint Called once per element in document order for its key, so an insertion can hand out the given
 *      key first and minted ones after.
 *  @param[out] tree The element tree, replacing what it held.
 *  @param[in] capabilities What the CPU runs, which bounds the kernel picked.
 */
template <typename allocator_type_, typename mint_type_>
status_t import_xml(std::string_view xml, element_key_t const& root_key, placement_t placement, mint_type_&& mint,
                    element_tree<allocator_type_>& tree, sz_capability_t capabilities) noexcept {
    using indices_allocator_t = typename std::allocator_traits<allocator_type_>::template rebind_alloc<element_index_t>;
    using chars_allocator_t = typename std::allocator_traits<allocator_type_>::template rebind_alloc<char>;
    xml_tokenizer<allocator_type_> tokenizer(tree.allocator());
    if (status_t status = tokenizer.tokenize(xml, capabilities); status.failed()) return status;
    if (status_t status = tree.reset(root_key, placement); status.failed()) return status;

    st::basic_vector<element_index_t, indices_allocator_t> open {indices_allocator_t(tree.allocator())};
    st::basic_vector<char, chars_allocator_t> pending {chars_allocator_t(tree.allocator())};
    if (status_t status = growth_status(open.push_back(tree.root())); status.failed()) return status;

    auto flush = [&]() noexcept -> status_t {
        if (pending.empty()) return {};
        std::string_view const text(pending.begin(), pending.end());
        element_index_t const holder = open.back();
        status_t status;
        if (holder != tree.root() && tree[holder].first_child == no_element_k && tree.text(holder).empty())
            status = tree.splice(holder, 0, 0, text);
        else status = tree.append(holder, mint(), element_kind_t::text_k, {}, text, placement).status();
        pending.clear();
        return status;
    };

    for (xml_token_t const& token : tokenizer.tokens()) {
        switch (token.kind) {
        case xml_token_kind_t::text_k:
        case xml_token_kind_t::cdata_k: {
            std::string_view const text = token.kind == xml_token_kind_t::text_k ? tokenizer.content(token)
                                                                                 : tokenizer.raw(token);
            std::size_t const offset = pending.size();
            if (status_t status = growth_status(pending.resize(offset + text.size())); status.failed()) return status;
            if (!text.empty()) std::memcpy(pending.data() + offset, text.data(), text.size());
            break;
        }
        case xml_token_kind_t::open_k:
        case xml_token_kind_t::empty_k: {
            if (status_t status = flush(); status.failed()) return status;
            auto element = tree.append(open.back(), mint(), element_kind_t::element_k, tokenizer.raw(token), {},
                                       placement);
            if (!element) return element.status();
            for (std::uint32_t index = 0; index != token.attributes_count; ++index) {
                xml_attribute_token_t const& attribute = tokenizer.attribute(token.first_attribute + index);
                std::string_view const name = tokenizer.attribute_name(attribute);
                if (tree.find_attribute(*element, name) != no_element_k)
                    return {status_code_t::malformed_k, "An XML element repeats an attribute"};
                if (status_t status = tree.set_attribute(*element, name, tokenizer.attribute_value(attribute));
                    status.failed())
                    return status;
            }
            if (token.kind == xml_token_kind_t::open_k)
                if (status_t status = growth_status(open.push_back(element_index_t(*element))); status.failed())
                    return status;
            break;
        }
        case xml_token_kind_t::close_k:
            if (status_t status = flush(); status.failed()) return status;
            open.pop_back();
            break;
        case xml_token_kind_t::comment_k: {
            if (status_t status = flush(); status.failed()) return status;
            auto comment = tree.append(open.back(), mint(), element_kind_t::comment_k, {}, tokenizer.raw(token),
                                       placement);
            if (!comment) return comment.status();
            break;
        }
        }
    }
    return flush();
}

/** Replaces @p tree with a root keyed @p root_key holding @p text as one text element, unless it is empty. */
template <typename allocator_type_, typename mint_type_>
status_t import_text(std::string_view text, element_key_t const& root_key, placement_t placement, mint_type_&& mint,
                     element_tree<allocator_type_>& tree) noexcept {
    if (status_t status = tree.reset(root_key, placement); status.failed()) return status;
    if (text.empty()) return {};
    return tree.append(tree.root(), mint(), element_kind_t::text_k, {}, text, placement).status();
}

/** Replaces @p tree with @p markup in @p encoding, unescaping through the StringZilla kernels
 *  @p capabilities allow, answering @c status_code_t::unsupported_k for HTML, Markdown and JSON. */
template <typename allocator_type_, typename mint_type_>
status_t import_markup(page_encoding_t encoding, std::string_view markup, element_key_t const& root_key,
                       placement_t placement, mint_type_&& mint, element_tree<allocator_type_>& tree,
                       sz_capability_t capabilities) noexcept {
    switch (encoding) {
    case page_encoding_t::xml_k: return import_xml(markup, root_key, placement, mint, tree, capabilities);
    case page_encoding_t::text_k: return import_text(markup, root_key, placement, mint, tree);
    default: return {status_code_t::unsupported_k, "Only XML and text import for now"};
    }
}

#pragma endregion

#pragma region Render

/** Appends @p bytes to @p output. */
template <typename output_allocator_type_>
status_t append_bytes(st::basic_vector<char, output_allocator_type_>& output, std::string_view bytes) noexcept {
    std::size_t const offset = output.size();
    if (status_t status = growth_status(output.resize(offset + bytes.size())); status.failed()) return status;
    if (!bytes.empty()) std::memcpy(output.data() + offset, bytes.data(), bytes.size());
    return {};
}

/** Appends @p bytes to @p output with <tt>&amp;</tt>, <tt>&lt;</tt> and <tt>&gt;</tt> escaped, and
 *  <tt>"</tt> too inside an attribute. */
template <typename output_allocator_type_>
status_t append_escaped(st::basic_vector<char, output_allocator_type_>& output, std::string_view bytes,
                        bool inside_attribute) noexcept {
    std::size_t run = 0;
    for (std::size_t index = 0; index != bytes.size(); ++index) {
        std::string_view replacement;
        switch (bytes[index]) {
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = inside_attribute ? std::string_view {} : "&gt;"; break;
        case '"': replacement = inside_attribute ? "&quot;" : std::string_view {}; break;
        default: break;
        }
        if (replacement.empty()) continue;
        if (status_t status = append_bytes(output, bytes.substr(run, index - run)); status.failed()) return status;
        if (status_t status = append_bytes(output, replacement); status.failed()) return status;
        run = index + 1;
    }
    return append_bytes(output, bytes.substr(run));
}

/** Renders the subtree of @p from as XML, calling @p open and @p close around every element's bytes. */
template <typename tree_allocator_type_, typename output_allocator_type_, typename open_type_, typename close_type_>
status_t render_xml_with(element_tree<tree_allocator_type_> const& tree, element_index_t from,
                         st::basic_vector<char, output_allocator_type_>& output, open_type_&& open,
                         close_type_&& close) noexcept {
    if (from == no_element_k) return {status_code_t::not_found_k, "No element to render"};
    auto content_of = [&](element_index_t index) noexcept {
        return tree[index].first_child != no_element_k || !tree.text(index).empty();
    };
    element_index_t at = from;
    for (;;) {
        element_t const& element = tree[at];
        if (status_t status = open(at, output.size()); status.failed()) return status;
        status_t status;
        if (element.kind == element_kind_t::text_k) status = append_escaped(output, tree.text(at), false);
        else if (element.kind == element_kind_t::comment_k) {
            status = append_bytes(output, "<!--");
            if (status.succeeded()) status = append_bytes(output, tree.text(at));
            if (status.succeeded()) status = append_bytes(output, "-->");
        }
        else {
            std::string_view const name = tree.name(at);
            if (!name.empty()) {
                status = append_bytes(output, "<");
                if (status.succeeded()) status = append_bytes(output, name);
                for (element_index_t attribute = element.first_attribute;
                     status.succeeded() && attribute != no_element_k; attribute = tree.attribute(attribute).next) {
                    status = append_bytes(output, " ");
                    if (status.succeeded()) status = append_bytes(output, tree.string(tree.attribute(attribute).name));
                    if (status.succeeded()) status = append_bytes(output, "=\"");
                    if (status.succeeded())
                        status = append_escaped(output, tree.string(tree.attribute(attribute).value), true);
                    if (status.succeeded()) status = append_bytes(output, "\"");
                }
                if (status.succeeded()) status = append_bytes(output, content_of(at) ? ">" : "/>");
            }
            if (status.succeeded()) status = append_escaped(output, tree.text(at), false);
        }
        if (status.failed()) return status;
        if (element.first_child != no_element_k) {
            at = element.first_child;
            continue;
        }
        for (;;) {
            if (tree[at].kind == element_kind_t::element_k && !tree.name(at).empty() && content_of(at)) {
                if (status_t closing = append_bytes(output, "</"); closing.failed()) return closing;
                if (status_t closing = append_bytes(output, tree.name(at)); closing.failed()) return closing;
                if (status_t closing = append_bytes(output, ">"); closing.failed()) return closing;
            }
            close(output.size());
            if (at == from) return {};
            if (tree[at].next_sibling != no_element_k) {
                at = tree[at].next_sibling;
                break;
            }
            at = tree[at].parent;
        }
    }
}

/** Appends the subtree of @p from to @p output as XML. */
template <typename tree_allocator_type_, typename output_allocator_type_>
status_t render_xml(element_tree<tree_allocator_type_> const& tree, element_index_t from,
                    st::basic_vector<char, output_allocator_type_>& output) noexcept {
    return render_xml_with(
        tree, from, output, [](element_index_t, std::size_t) noexcept -> status_t { return {}; },
        [](std::size_t) noexcept {});
}

/** Appends the subtree of @p from to @p output as XML, and the span of every element to @p spans in preorder. */
template <typename tree_allocator_type_, typename output_allocator_type_, typename spans_allocator_type_>
status_t render_xml(element_tree<tree_allocator_type_> const& tree, element_index_t from,
                    st::basic_vector<char, output_allocator_type_>& output,
                    st::basic_vector<element_span_t, spans_allocator_type_>& spans) noexcept {
    using stack_allocator_t = typename std::allocator_traits<spans_allocator_type_>::template rebind_alloc<std::size_t>;
    st::basic_vector<std::size_t, stack_allocator_t> open {stack_allocator_t(tree.allocator())};
    return render_xml_with(
        tree, from, output,
        [&](element_index_t element, std::size_t begin) noexcept -> status_t {
            if (status_t status = growth_status(open.push_back(spans.size())); status.failed()) return status;
            return growth_status(spans.push_back(element_span_t {element, begin, begin}));
        },
        [&](std::size_t end) noexcept {
            spans[open.back()].end = end;
            open.pop_back();
        });
}

/** Appends the text of every element and text element under @p from to @p output in document order,
 *  comments dropped. */
template <typename tree_allocator_type_, typename output_allocator_type_>
status_t render_text(element_tree<tree_allocator_type_> const& tree, element_index_t from,
                     st::basic_vector<char, output_allocator_type_>& output) noexcept {
    if (from == no_element_k) return {status_code_t::not_found_k, "No element to render"};
    for (element_index_t at = from; at != no_element_k; at = tree.next_in_preorder(at, from))
        if (tree[at].kind != element_kind_t::comment_k)
            if (status_t status = append_bytes(output, tree.text(at)); status.failed()) return status;
    return {};
}

/** Appends the subtree of @p from to @p output in @p encoding, answering
 *  @c status_code_t::unsupported_k for HTML, Markdown and JSON. */
template <typename tree_allocator_type_, typename output_allocator_type_>
status_t render_markup(element_tree<tree_allocator_type_> const& tree, element_index_t from, page_encoding_t encoding,
                       st::basic_vector<char, output_allocator_type_>& output) noexcept {
    switch (encoding) {
    case page_encoding_t::xml_k: return render_xml(tree, from, output);
    case page_encoding_t::text_k: return render_text(tree, from, output);
    default: return {status_code_t::unsupported_k, "Only XML and text render for now"};
    }
}

#pragma endregion

#pragma region XPath

/**
 *  @brief Every node of @p xml that one of @p paths selects, as StringZilla's XPath subset defines them, through the
 *      best kernel of <tt>stringzilla/subdocuments.h</tt> that @p capabilities allow.
 *  @param[in] xml The document, which needs one root element.
 *  @param[in] paths The paths to select by.
 *  @param[out] matches The matches by start offset, then by path, each spanning an element through its end tag, an
 *      attribute value, or a text node.
 *  @param[in] capabilities What the CPU runs, which bounds the kernel picked.
 *  @return @c status_code_t::malformed_k for a path outside the subset or malformed XML, which needs one root element.
 */
template <typename allocator_type_>
status_t find_xpath(std::string_view xml, std::span<std::string_view const> paths,
                    st::basic_vector<sz_match_t, allocator_type_>& matches, sz_capability_t capabilities) noexcept {
    using views_allocator_t = typename std::allocator_traits<allocator_type_>::template rebind_alloc<sz_string_view_t>;
    allocator_type_ allocator = matches.get_allocator();
    st::basic_vector<sz_string_view_t, views_allocator_t> views {views_allocator_t(allocator)};
    if (status_t status = growth_status(views.reserve(paths.size())); status.failed()) return status;
    for (std::string_view path : paths) (void)views.push_back(st::assume_reserved, stringzilla_view(path));
    sz_string_view_t const document = stringzilla_view(xml);
    sz_strings_view_t const vocabulary = strings_of(views.data(), views.size());
    sz_strings_view_t const documents = strings_of(&document, 1);

    stringzilla_allocator<allocator_type_> bridge(allocator);
    sz_allocator_t c_allocator = bridge.c_allocator();
    sz_subdocuments_engine_t engine;
    sz_size_t malformed_index = 0;
    if (status_t status = stringzilla_status(
            sz_subdocuments_engine_init(&engine, &vocabulary, sz_subdocuments_xml_k, sz_subdocuments_options_none_k,
                                        &malformed_index, capabilities, &c_allocator, nullptr),
            "An XPath is outside the supported subset");
        status.failed())
        return status;
    sz_size_t offsets[2] = {0, 0};
    status_t status = stringzilla_status(
        sz_subdocuments_find(&engine, &documents, nullptr, 0, offsets, &malformed_index, nullptr),
        "The XML is malformed");
    if (status.succeeded()) status = growth_status(matches.resize(offsets[1]));
    if (status.succeeded() && offsets[1])
        status = stringzilla_status(sz_subdocuments_find(&engine, &documents, matches.data(), matches.size(), offsets,
                                                         &malformed_index, nullptr),
                                    "The XML is malformed");
    sz_subdocuments_engine_free(&engine, nullptr);
    return status;
}

/** The element whose rendering starts at @p offset among @p spans in preorder, or @c no_element_k. */
inline element_index_t element_starting(std::span<element_span_t const> spans, std::size_t offset) noexcept {
    std::size_t low = 0, high = spans.size();
    while (low < high) {
        std::size_t const middle = low + (high - low) / 2;
        if (spans[middle].begin < offset) low = middle + 1;
        else high = middle;
    }
    // Text and nameless elements may start where their first child does, so the deepest one of that start wins
    element_index_t found = no_element_k;
    for (; low != spans.size() && spans[low].begin == offset; ++low) found = spans[low].element;
    return found;
}

#pragma endregion

} // namespace unum::ustore::pages
