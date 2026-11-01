/**
 *  @file include/ustore/pages/element_tree.hpp
 *  @author Ash Vardanian
 *  @date October 16, 2026
 *  @brief A page as an ordered tree of elements keyed by 16-byte keys, its dense serialized form, and a view that
 *      addresses subtrees of that form by element key without decoding it.
 *
 *  Every element carries a key, a kind, a name, ordered attributes and a run of text, and the order of siblings is the
 *  content. The root element's key is the page key, its name is empty, and it renders as its content only.
 *
 *  Siblings follow a replicated growable array: an element lands after its anchor, past every sibling placed later
 *  than itself, so edits applied in commit order put the newest of concurrent insertions after one anchor first, and
 *  applying them in any order converges on that same order.
 *
 *  The dense form is what the RAM and oracle engines store as one blob per page, laid out little-endian. A header of
 *  three 32-bit counts, of elements, attributes and bytes of strings, comes first. One 60-byte record per element
 *  follows in preorder, holding the key, the placement stamp and sequence, the kind, the subtree size, and the offsets
 *  and lengths of the name, the text and the attribute run. One 16-byte record per attribute holds the offsets and
 *  lengths of its name and value. A directory of 20-byte entries, a key and a preorder position each, sorted by key,
 *  precedes the strings, back to back.
 *
 *  A subtree is a contiguous run of records, so the directory and the subtree sizes address one in place.
 */

#pragma once

#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <algorithm>   // `std::sort`, `std::min`
#include <compare>     // `std::strong_ordering`
#include <memory>      // `std::allocator_traits`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::move`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/utilities/ram_buffers.hpp" // `growth_status`
#include "ustore/utilities/types.hpp"       // `byte_t`, `expected`, `status_t`, `status_code_t`

namespace unum::ustore::pages {

namespace st = ashvardanian::smashtable;

#pragma region Vocabulary

using utilities::byte_t;
using utilities::expected;
using utilities::growth_status;
using utilities::status_code_t;
using utilities::status_t;

/** The key of an element, where the page key doubles as the key of the root and all zeros is Nil. */
struct element_key_t {

    /** The 16 bytes, compared lexicographically. */
    std::uint8_t bytes[16] = {};

    constexpr bool is_nil() const noexcept {
        for (std::uint8_t byte : bytes)
            if (byte) return false;
        return true;
    }

    friend constexpr std::strong_ordering operator<=>(element_key_t const&, element_key_t const&) noexcept = default;
    friend constexpr bool operator==(element_key_t const&, element_key_t const&) noexcept = default;
};

static_assert(sizeof(element_key_t) == 16);

/** The Nil key, standing for the root as a parent and for the front as an anchor. */
inline constexpr element_key_t nil_key_k {};

/** A time-ordered key for the element @p ordinal of task @p sequence committed at @p stamp, big-endian so that bytes
 *  order as the mints did. */
constexpr element_key_t mint_key(std::uint64_t stamp, std::uint32_t sequence, std::uint32_t ordinal) noexcept {
    element_key_t key;
    for (std::size_t index = 0; index != 8; ++index) key.bytes[index] = std::uint8_t(stamp >> (56 - index * 8));
    for (std::size_t index = 0; index != 4; ++index) key.bytes[8 + index] = std::uint8_t(sequence >> (24 - index * 8));
    for (std::size_t index = 0; index != 4; ++index) key.bytes[12 + index] = std::uint8_t(ordinal >> (24 - index * 8));
    return key;
}

/** When an element took its place among its siblings: the commit stamp and the task inside that commit. */
struct placement_t {

    /** Commit stamp of the edit that placed the element. */
    std::uint64_t stamp = 0;

    /** Task of that edit inside its commit, ordering the tasks of one call. */
    std::uint32_t sequence = 0;

    friend constexpr std::strong_ordering operator<=>(placement_t const&, placement_t const&) noexcept = default;
    friend constexpr bool operator==(placement_t const&, placement_t const&) noexcept = default;
};

/** What an element is, which decides how it renders. */
enum class element_kind_t : std::uint8_t {

    /** A tagged element with attributes, its own leading text and children. */
    element_k = 0,

    /** A run of character data, with neither name, attributes nor children. */
    text_k = 1,

    /** A comment, whose text is its body. */
    comment_k = 2,
};

/** A run of bytes inside the strings of a tree. */
struct string_span_t {

    /** Bytes from the start of the strings to the run. */
    std::uint32_t offset = 0;

    /** Bytes of the run. */
    std::uint32_t length = 0;
};

/** Index of an element or an attribute inside a tree, stable until the tree is reset or decoded. */
using element_index_t = std::uint32_t;

/** The index standing for no element or no attribute. */
inline constexpr element_index_t no_element_k = ~element_index_t(0);

/** One element of a tree, linked to its parent, its siblings and its children. */
struct element_t {

    /** Identity of the element, stable across every edit. */
    element_key_t key {};

    /** When the element took its place among its siblings, which orders concurrent insertions. */
    placement_t placement {};

    /** Whether it is an element, a text run or a comment. */
    element_kind_t kind = element_kind_t::element_k;

    /** Whether a removal unlinked it, leaving the slot until the next decode. */
    bool removed = false;

    /** The links to the parent, the children and the siblings, @c no_element_k where absent. */
    element_index_t parent = no_element_k;
    element_index_t first_child = no_element_k;
    element_index_t last_child = no_element_k;
    element_index_t previous_sibling = no_element_k;
    element_index_t next_sibling = no_element_k;

    /** The first attribute in order, @c no_element_k without any. */
    element_index_t first_attribute = no_element_k;

    /** The tag, empty for the root, text runs and comments. */
    string_span_t name {};

    /** The leading text of an element, the bytes of a text run, or the body of a comment. */
    string_span_t text {};
};

/** One attribute of an element, linked to the next one in order. */
struct attribute_t {

    /** The attribute's name, unique within its element. */
    string_span_t name {};

    /** The attribute's value, unescaped. */
    string_span_t value {};

    /** The next attribute of the same element, @c no_element_k for the last. */
    element_index_t next = no_element_k;
};

#pragma endregion Vocabulary

#pragma region Little Endian

/** Writes @p value little-endian into the 4 bytes at @p target, as every integer of the dense form. */
inline void store_u32(byte_t* target, std::uint32_t value) noexcept {
    for (std::size_t index = 0; index != 4; ++index) target[index] = byte_t(value >> (index * 8));
}

/** Writes @p value little-endian into the 8 bytes at @p target. */
inline void store_u64(byte_t* target, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index != 8; ++index) target[index] = byte_t(value >> (index * 8));
}

/** Reads the little-endian 4 bytes at @p source. */
inline std::uint32_t load_u32(byte_t const* source) noexcept {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index != 4; ++index) value |= std::uint32_t(source[index]) << (index * 8);
    return value;
}

/** Reads the little-endian 8 bytes at @p source. */
inline std::uint64_t load_u64(byte_t const* source) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index != 8; ++index) value |= std::uint64_t(source[index]) << (index * 8);
    return value;
}

/** Writes the 16 bytes of @p key at @p target, in their own order. */
inline void store_key(byte_t* target, element_key_t const& key) noexcept { std::memcpy(target, key.bytes, 16); }

/** Reads the key in the 16 bytes at @p source. */
inline element_key_t load_key(byte_t const* source) noexcept {
    element_key_t key;
    std::memcpy(key.bytes, source, 16);
    return key;
}

#pragma endregion Little Endian

#pragma region Dense Form

/** Bytes of the dense form's header. */
inline constexpr std::size_t page_header_bytes_k = 12;

/** Bytes of one element record. */
inline constexpr std::size_t page_record_bytes_k = 60;

/** Bytes of one attribute record. */
inline constexpr std::size_t page_attribute_bytes_k = 16;

/** Bytes of one directory entry. */
inline constexpr std::size_t page_directory_bytes_k = 20;

/** One element record of the dense form, decoded. */
struct page_record_t {
    element_key_t key {};
    placement_t placement {};
    element_kind_t kind = element_kind_t::element_k;
    std::uint32_t subtree_size = 1;
    string_span_t name {};
    string_span_t text {};
    std::uint32_t first_attribute = 0;
    std::uint32_t attributes_count = 0;
};

/** One attribute record of the dense form, decoded. */
struct page_attribute_t {
    string_span_t name {};
    string_span_t value {};
};

/** The preorder positions <tt>[first, first + count)</tt> of a subtree inside a dense form. */
struct page_subtree_t {
    std::uint32_t first = 0;
    std::uint32_t count = 0;
};

/** A validated, read-only view of a page's dense form, addressing records and subtrees by key in place. */
class page_view_t {
    byte_t const* records_ = nullptr;
    byte_t const* attributes_ = nullptr;
    byte_t const* directory_ = nullptr;
    char const* strings_ = nullptr;
    std::uint32_t count_ = 0;
    std::uint32_t attributes_count_ = 0;
    std::uint32_t strings_length_ = 0;

    static bool spans_inside(string_span_t span, std::uint32_t length) noexcept {
        return span.offset <= length && span.length <= length - span.offset;
    }

  public:
    page_view_t() noexcept = default;

    /** Validates the sizes, links and spans of @p bytes, refusing with @c status_code_t::malformed_k. */
    static expected<page_view_t> make(std::span<byte_t const> bytes) noexcept {
        if (bytes.size() < page_header_bytes_k) return status_t {status_code_t::malformed_k, "A page lacks its header"};
        page_view_t view;
        view.count_ = load_u32(bytes.data());
        view.attributes_count_ = load_u32(bytes.data() + 4);
        view.strings_length_ = load_u32(bytes.data() + 8);
        std::uint64_t const wanted = page_header_bytes_k +
                                     std::uint64_t(view.count_) * (page_record_bytes_k + page_directory_bytes_k) +
                                     std::uint64_t(view.attributes_count_) * page_attribute_bytes_k +
                                     view.strings_length_;
        if (wanted != bytes.size() || view.count_ == 0)
            return status_t {status_code_t::malformed_k, "A page's size disagrees with its counts"};
        view.records_ = bytes.data() + page_header_bytes_k;
        view.attributes_ = view.records_ + std::size_t(view.count_) * page_record_bytes_k;
        view.directory_ = view.attributes_ + std::size_t(view.attributes_count_) * page_attribute_bytes_k;
        view.strings_ = reinterpret_cast<char const*>(view.directory_ +
                                                      std::size_t(view.count_) * page_directory_bytes_k);
        for (std::uint32_t position = 0; position != view.count_; ++position) {
            page_record_t const record = view.record(position);
            if (record.kind > element_kind_t::comment_k)
                return status_t {status_code_t::malformed_k, "A page record has an unknown kind"};
            if (record.subtree_size == 0 || record.subtree_size > view.count_ - position)
                return status_t {status_code_t::malformed_k, "A page record's subtree overruns the page"};
            if (!spans_inside(record.name, view.strings_length_) || !spans_inside(record.text, view.strings_length_))
                return status_t {status_code_t::malformed_k, "A page record's strings overrun the page"};
            if (record.first_attribute > view.attributes_count_ ||
                record.attributes_count > view.attributes_count_ - record.first_attribute)
                return status_t {status_code_t::malformed_k, "A page record's attributes overrun the page"};
            if (record.kind != element_kind_t::element_k && (record.attributes_count || record.subtree_size != 1))
                return status_t {status_code_t::malformed_k, "A text or comment record holds attributes or children"};
            std::uint32_t const directed = load_u32(view.directory_ + std::size_t(position) * page_directory_bytes_k +
                                                    16);
            if (directed >= view.count_ || !(view.record(directed).key == view.directory_key(position)))
                return status_t {status_code_t::malformed_k, "A directory entry disagrees with its record"};
            if (position && !(view.directory_key(position - 1) < view.directory_key(position)))
                return status_t {status_code_t::malformed_k, "A page directory is unsorted or repeats a key"};
        }
        if (view.record(0).subtree_size != view.count_ || view.record(0).kind != element_kind_t::element_k)
            return status_t {status_code_t::malformed_k, "A page root must be an element spanning the page"};
        for (std::uint32_t index = 0; index != view.attributes_count_; ++index) {
            page_attribute_t const attribute = view.attribute(index);
            if (!spans_inside(attribute.name, view.strings_length_) ||
                !spans_inside(attribute.value, view.strings_length_))
                return status_t {status_code_t::malformed_k, "A page attribute's strings overrun the page"};
        }
        return view;
    }

    /** Number of element records, the root included. */
    std::uint32_t size() const noexcept { return count_; }

    /** The record at preorder @p position. */
    page_record_t record(std::uint32_t position) const noexcept {
        byte_t const* source = records_ + std::size_t(position) * page_record_bytes_k;
        page_record_t record;
        record.key = load_key(source);
        record.placement.stamp = load_u64(source + 16);
        record.placement.sequence = load_u32(source + 24);
        record.kind = static_cast<element_kind_t>(source[28]);
        record.subtree_size = load_u32(source + 32);
        record.name = {load_u32(source + 36), load_u32(source + 40)};
        record.text = {load_u32(source + 44), load_u32(source + 48)};
        record.first_attribute = load_u32(source + 52);
        record.attributes_count = load_u32(source + 56);
        return record;
    }

    /** The attribute record at @p index. */
    page_attribute_t attribute(std::uint32_t index) const noexcept {
        byte_t const* source = attributes_ + std::size_t(index) * page_attribute_bytes_k;
        return {{load_u32(source), load_u32(source + 4)}, {load_u32(source + 8), load_u32(source + 12)}};
    }

    /** The bytes @p span names inside the strings. */
    std::string_view string(string_span_t span) const noexcept { return {strings_ + span.offset, span.length}; }

    /** The key of the root, which is the page key. */
    element_key_t root_key() const noexcept { return load_key(records_); }

    /** The preorder position of the element keyed @p key, or @c no_element_k. */
    std::uint32_t find(element_key_t const& key) const noexcept {
        std::uint32_t low = 0, high = count_;
        while (low < high) {
            std::uint32_t const middle = low + (high - low) / 2;
            if (directory_key(middle) < key) low = middle + 1;
            else high = middle;
        }
        if (low == count_ || !(directory_key(low) == key)) return no_element_k;
        return load_u32(directory_ + std::size_t(low) * page_directory_bytes_k + 16);
    }

    /** The preorder positions <tt>[first, first + count)</tt> of the subtree rooted at @p key, empty when missing. */
    page_subtree_t subtree(element_key_t const& key) const noexcept {
        std::uint32_t const position = find(key);
        if (position == no_element_k) return {};
        return {position, record(position).subtree_size};
    }

  private:
    element_key_t directory_key(std::uint32_t index) const noexcept {
        return load_key(directory_ + std::size_t(index) * page_directory_bytes_k);
    }
};

#pragma endregion Dense Form

#pragma region Element Tree

/**
 *  @brief A page held as linked elements, edited in place and encoded into or decoded from the dense form.
 *
 *  Elements and attributes live in growable arrays and link by index; their names and texts live in one pool of
 *  strings that splices append to, and which encoding compacts. Removed elements stay in the arrays, unlinked and
 *  flagged, until the next decode. A hashed index maps every key to its newest element.
 */
template <typename allocator_type_>
class element_tree {
  public:
    using allocator_t = allocator_type_;

  private:
    template <typename value_type_>
    using rebound = typename std::allocator_traits<allocator_t>::template rebind_alloc<value_type_>;

    allocator_t allocator_;
    st::vector<element_t, rebound<element_t>> elements_;
    st::vector<attribute_t, rebound<attribute_t>> attributes_;
    st::vector<char, rebound<char>> strings_;
    st::vector<element_index_t, rebound<element_index_t>> slots_;
    std::size_t indexed_ = 0;
    std::size_t live_ = 0;

    static std::size_t hash(element_key_t const& key) noexcept {
        std::uint64_t halves[2];
        std::memcpy(halves, key.bytes, sizeof(halves));
        std::uint64_t mixed = halves[0] ^ (halves[1] * 0x9E3779B97F4A7C15ull);
        mixed ^= mixed >> 29;
        mixed *= 0xBF58476D1CE4E5B9ull;
        return std::size_t(mixed ^ (mixed >> 32));
    }

    /** The slot holding @p key, or the empty slot where it would go. */
    std::size_t slot_of(element_key_t const& key) const noexcept {
        std::size_t const mask = slots_.size() - 1;
        std::size_t slot = hash(key) & mask;
        while (slots_[slot] != no_element_k && !(elements_[slots_[slot]].key == key)) slot = (slot + 1) & mask;
        return slot;
    }

    status_t grow_index() noexcept {
        std::size_t capacity = slots_.size() ? slots_.size() * 2 : 16;
        st::vector<element_index_t, rebound<element_index_t>> old {rebound<element_index_t>(allocator_)};
        if (status_t status = growth_status(old.swap(slots_)); status.failed()) return status;
        if (status_t status = growth_status(slots_.resize(capacity, no_element_k)); status.failed()) return status;
        for (element_index_t index : old)
            if (index != no_element_k) slots_[slot_of(elements_[index].key)] = index;
        return {};
    }

    /** Maps @p key to @p index, replacing a removed element of the same key. */
    status_t index_key(element_key_t const& key, element_index_t index) noexcept {
        if ((indexed_ + 1) * 2 > slots_.size())
            if (status_t status = grow_index(); status.failed()) return status;
        std::size_t const slot = slot_of(key);
        if (slots_[slot] == no_element_k) ++indexed_;
        slots_[slot] = index;
        return {};
    }

    status_t store(std::string_view bytes, string_span_t& span) noexcept {
        if (bytes.size() > ~std::uint32_t(0) - strings_.size())
            return {status_code_t::invalid_argument_k, "A page outgrew 4 GiB of strings"};
        span.offset = std::uint32_t(strings_.size());
        span.length = std::uint32_t(bytes.size());
        if (status_t status = growth_status(strings_.resize(strings_.size() + bytes.size())); status.failed())
            return status;
        if (!bytes.empty()) std::memcpy(strings_.data() + span.offset, bytes.data(), bytes.size());
        return {};
    }

    void attach(element_index_t index, element_index_t parent, element_index_t after) noexcept {
        element_t& element = elements_[index];
        element_t& holder = elements_[parent];
        element.parent = parent;
        element.previous_sibling = after;
        element.next_sibling = after == no_element_k ? holder.first_child : elements_[after].next_sibling;
        if (after == no_element_k) holder.first_child = index;
        else elements_[after].next_sibling = index;
        if (element.next_sibling == no_element_k) holder.last_child = index;
        else elements_[element.next_sibling].previous_sibling = index;
    }

    void detach(element_index_t index) noexcept {
        element_t& element = elements_[index];
        element_t& holder = elements_[element.parent];
        if (element.previous_sibling == no_element_k) holder.first_child = element.next_sibling;
        else elements_[element.previous_sibling].next_sibling = element.next_sibling;
        if (element.next_sibling == no_element_k) holder.last_child = element.previous_sibling;
        else elements_[element.next_sibling].previous_sibling = element.previous_sibling;
        element.previous_sibling = element.next_sibling = no_element_k;
    }

  public:
    explicit element_tree(allocator_t allocator = {}) noexcept
        : allocator_(allocator), elements_(rebound<element_t>(allocator)), attributes_(rebound<attribute_t>(allocator)),
          strings_(rebound<char>(allocator)), slots_(rebound<element_index_t>(allocator)) {}

    element_tree(element_tree&&) noexcept = default;
    element_tree& operator=(element_tree&&) noexcept = default;
    element_tree(element_tree const&) = delete;
    element_tree& operator=(element_tree const&) = delete;

    /** The allocator every array of the tree comes from. */
    allocator_t allocator() const noexcept { return allocator_; }

    /** Empties the tree down to a root element keyed @p root_key. */
    status_t reset(element_key_t const& root_key, placement_t placement = {}) noexcept {
        elements_.clear(), attributes_.clear(), strings_.clear();
        for (element_index_t& slot : slots_) slot = no_element_k;
        indexed_ = live_ = 0;
        element_t root;
        root.key = root_key;
        root.placement = placement;
        if (status_t status = growth_status(elements_.push_back(std::move(root))); status.failed()) return status;
        live_ = 1;
        return index_key(root_key, 0);
    }

    /** The root element, whose key is the page key. */
    element_index_t root() const noexcept { return elements_.empty() ? no_element_k : 0; }

    /** Whether the tree holds no root. */
    bool empty() const noexcept { return elements_.empty(); }

    /** Number of live elements, the root included. */
    std::size_t size() const noexcept { return live_; }

    /** The live element keyed @p key, or @c no_element_k. */
    element_index_t find(element_key_t const& key) const noexcept {
        if (slots_.empty()) return no_element_k;
        element_index_t const index = slots_[slot_of(key)];
        return index == no_element_k || elements_[index].removed ? no_element_k : index;
    }

    element_t const& operator[](element_index_t index) const noexcept { return elements_[index]; }
    attribute_t const& attribute(element_index_t index) const noexcept { return attributes_[index]; }
    std::string_view string(string_span_t span) const noexcept { return {strings_.data() + span.offset, span.length}; }
    std::string_view name(element_index_t index) const noexcept { return string(elements_[index].name); }
    std::string_view text(element_index_t index) const noexcept { return string(elements_[index].text); }

    /** The attribute of @p index named @p name, or @c no_element_k. */
    element_index_t find_attribute(element_index_t index, std::string_view name) const noexcept {
        for (element_index_t at = elements_[index].first_attribute; at != no_element_k; at = attributes_[at].next)
            if (string(attributes_[at].name) == name) return at;
        return no_element_k;
    }

    /** Whether @p ancestor is @p descendant or one of its ancestors. */
    bool contains(element_index_t ancestor, element_index_t descendant) const noexcept {
        for (element_index_t at = descendant; at != no_element_k; at = elements_[at].parent)
            if (at == ancestor) return true;
        return false;
    }

    /** The element after @p index in preorder, staying inside the subtree of @p within, or @c no_element_k. */
    element_index_t next_in_preorder(element_index_t index, element_index_t within) const noexcept {
        if (elements_[index].first_child != no_element_k) return elements_[index].first_child;
        for (element_index_t at = index; at != within; at = elements_[at].parent)
            if (elements_[at].next_sibling != no_element_k) return elements_[at].next_sibling;
        return no_element_k;
    }

    /**
     *  @brief Where an element placed at @p placement lands under @p parent after the anchor keyed @p anchor.
     *  @return The sibling to link after, or @c no_element_k for the front.
     *
     *  A Nil, missing or foreign anchor stands for the front. From there the element skips every
     *  sibling placed later than itself, as a replicated growable array does, so concurrent
     *  insertions after one anchor order newest first whatever order they are applied in.
     */
    element_index_t landing(element_index_t parent, element_key_t const& anchor, placement_t placement) const noexcept {
        element_index_t after = anchor.is_nil() ? no_element_k : find(anchor);
        if (after != no_element_k && elements_[after].parent != parent) after = no_element_k;
        element_index_t next = after == no_element_k ? elements_[parent].first_child : elements_[after].next_sibling;
        while (next != no_element_k && placement < elements_[next].placement)
            after = next, next = elements_[next].next_sibling;
        return after;
    }

    /**
     *  @brief Creates an element under @p parent right after @p after, or at the front for @c no_element_k.
     *  @return The new element, or @c status_code_t::already_exists_k when a live element holds @p key.
     */
    expected<element_index_t> link(element_index_t parent, element_index_t after, element_key_t const& key,
                                   element_kind_t kind, std::string_view name, std::string_view text,
                                   placement_t placement) noexcept {
        if (key.is_nil()) return status_t {status_code_t::invalid_argument_k, "An element key is Nil"};
        if (find(key) != no_element_k) return status_t {status_code_t::already_exists_k, "An element key is taken"};
        element_t element;
        element.key = key;
        element.kind = kind;
        element.placement = placement;
        if (status_t status = store(name, element.name); status.failed()) return status;
        if (status_t status = store(text, element.text); status.failed()) return status;
        element_index_t const index = element_index_t(elements_.size());
        if (status_t status = growth_status(elements_.push_back(std::move(element))); status.failed()) return status;
        if (status_t status = index_key(key, index); status.failed()) {
            elements_.pop_back();
            return status;
        }
        attach(index, parent, after);
        ++live_;
        return index;
    }

    /** Creates an element as the last child of @p parent. */
    expected<element_index_t> append(element_index_t parent, element_key_t const& key, element_kind_t kind,
                                     std::string_view name, std::string_view text, placement_t placement) noexcept {
        return link(parent, elements_[parent].last_child, key, kind, name, text, placement);
    }

    /** Sets the attribute @p name of @p index to @p value, appending it when missing. */
    status_t set_attribute(element_index_t index, std::string_view name, std::string_view value) noexcept {
        element_index_t const found = find_attribute(index, name);
        if (found != no_element_k) return store(value, attributes_[found].value);
        attribute_t attribute;
        if (status_t status = store(name, attribute.name); status.failed()) return status;
        if (status_t status = store(value, attribute.value); status.failed()) return status;
        element_index_t const added = element_index_t(attributes_.size());
        if (status_t status = growth_status(attributes_.push_back(std::move(attribute))); status.failed())
            return status;
        element_index_t* tail = &elements_[index].first_attribute;
        while (*tail != no_element_k) tail = &attributes_[*tail].next;
        *tail = added;
        return {};
    }

    /** Removes the attribute @p name of @p index, if it has one. */
    void remove_attribute(element_index_t index, std::string_view name) noexcept {
        for (element_index_t* link = &elements_[index].first_attribute; *link != no_element_k;
             link = &attributes_[*link].next)
            if (string(attributes_[*link].name) == name) {
                *link = attributes_[*link].next;
                return;
            }
    }

    /** Replaces @p length bytes of the text of @p index from @p start with @p bytes, both clamped to the text. */
    status_t splice(element_index_t index, std::uint64_t start, std::uint64_t length, std::string_view bytes) noexcept {
        string_span_t const old = elements_[index].text;
        std::uint32_t const clamped_start = std::uint32_t(std::min<std::uint64_t>(start, old.length));
        std::uint32_t const clamped_length = std::uint32_t(std::min<std::uint64_t>(length, old.length - clamped_start));
        std::uint64_t const new_length = std::uint64_t(old.length) - clamped_length + bytes.size();
        if (new_length > ~std::uint32_t(0) - strings_.size())
            return {status_code_t::invalid_argument_k, "A page outgrew 4 GiB of strings"};
        std::size_t const offset = strings_.size();
        if (status_t status = growth_status(strings_.resize(offset + new_length)); status.failed()) return status;
        char* target = strings_.data() + offset;
        char const* source = strings_.data() + old.offset;
        std::size_t const kept_tail = old.length - clamped_start - clamped_length;
        if (clamped_start) std::memcpy(target, source, clamped_start);
        if (!bytes.empty()) std::memcpy(target + clamped_start, bytes.data(), bytes.size());
        if (kept_tail)
            std::memcpy(target + clamped_start + bytes.size(), source + clamped_start + clamped_length, kept_tail);
        elements_[index].text = {std::uint32_t(offset), std::uint32_t(new_length)};
        return {};
    }

    /** Unlinks @p index with its subtree, or empties the root of children, text and attributes. */
    void remove(element_index_t index) noexcept {
        if (index == root()) {
            while (elements_[index].first_child != no_element_k) remove(elements_[index].first_child);
            elements_[index].text = {};
            elements_[index].first_attribute = no_element_k;
            return;
        }
        for (element_index_t at = index; at != no_element_k; at = next_in_preorder(at, index))
            elements_[at].removed = true, --live_;
        detach(index);
    }

    /** Unlinks @p index from its siblings, so that @c landing computes its new place without it. */
    void unlink(element_index_t index) noexcept { detach(index); }

    /** Links an unlinked @p index under @p parent right after @p after, which the caller checked for cycles. */
    void relink(element_index_t index, element_index_t parent, element_index_t after, placement_t placement) noexcept {
        elements_[index].placement = placement;
        attach(index, parent, after);
    }

    /** Whether any element of @p fragment besides its root holds a key live in this tree. */
    template <typename other_allocator_type_>
    bool collides(element_tree<other_allocator_type_> const& fragment) const noexcept {
        element_index_t const root = fragment.root();
        for (element_index_t at = fragment.next_in_preorder(root, root); at != no_element_k;
             at = fragment.next_in_preorder(at, root))
            if (find(fragment[at].key) != no_element_k) return true;
        return false;
    }

    /**
     *  @brief Copies the children of @p fragment's root under @p parent, the first right after @p after and the rest
     *      after each other, placing every copy at @p placement.
     *  @return @c status_code_t::already_exists_k, changing nothing, when a fragment key is live here.
     */
    template <typename other_allocator_type_>
    status_t graft(element_tree<other_allocator_type_> const& fragment, element_index_t parent, element_index_t after,
                   placement_t placement) noexcept {
        if (fragment.empty()) return {};
        if (collides(fragment)) return {status_code_t::already_exists_k, "A fragment key is taken"};
        element_index_t const root = fragment.root();
        // The copy of every fragment element's parent is the copy linked last at the depth above it
        st::vector<element_index_t, rebound<element_index_t>> copies {rebound<element_index_t>(allocator_)};
        if (status_t status = growth_status(copies.resize(fragment.capacity(), no_element_k)); status.failed())
            return status;
        copies[root] = parent;
        for (element_index_t at = fragment.next_in_preorder(root, root); at != no_element_k;
             at = fragment.next_in_preorder(at, root)) {
            element_t const& source = fragment[at];
            element_index_t const holder = copies[source.parent];
            element_index_t const previous = source.parent == root ? (source.previous_sibling == no_element_k
                                                                          ? after
                                                                          : copies[source.previous_sibling])
                                                                   : elements_[holder].last_child;
            auto copy = link(holder, previous, source.key, source.kind, fragment.string(source.name),
                             fragment.string(source.text), placement);
            if (!copy) return copy.status();
            copies[at] = *copy;
            for (element_index_t attribute = source.first_attribute; attribute != no_element_k;
                 attribute = fragment.attribute(attribute).next)
                if (status_t status = set_attribute(*copy, fragment.string(fragment.attribute(attribute).name),
                                                    fragment.string(fragment.attribute(attribute).value));
                    status.failed())
                    return status;
        }
        return {};
    }

    /** Number of element slots, removed ones included, which bounds every index. */
    std::size_t capacity() const noexcept { return elements_.size(); }

    /** Number of attributes and string bytes the dense form of the live tree takes. */
    void measure(std::uint32_t& count, std::uint32_t& attributes, std::uint64_t& strings) const noexcept {
        count = attributes = 0, strings = 0;
        element_index_t const root = this->root();
        for (element_index_t at = root; at != no_element_k; at = next_in_preorder(at, root)) {
            ++count;
            strings += elements_[at].name.length + elements_[at].text.length;
            for (element_index_t attribute = elements_[at].first_attribute; attribute != no_element_k;
                 attribute = attributes_[attribute].next)
                ++attributes, strings += attributes_[attribute].name.length + attributes_[attribute].value.length;
        }
    }

    /** Bytes of the dense form of the live tree. */
    std::uint64_t encoded_size() const noexcept {
        std::uint32_t count, attributes;
        std::uint64_t strings;
        measure(count, attributes, strings);
        return page_header_bytes_k + std::uint64_t(count) * (page_record_bytes_k + page_directory_bytes_k) +
               std::uint64_t(attributes) * page_attribute_bytes_k + strings;
    }

    /** Writes the dense form into @p target, exactly @c encoded_size bytes, compacting the strings. */
    status_t encode_into(std::span<byte_t> target) noexcept {
        if (empty()) return {status_code_t::invalid_argument_k, "An empty tree has no dense form"};
        std::uint32_t count, attributes_count;
        std::uint64_t strings_length;
        measure(count, attributes_count, strings_length);
        if (strings_length > ~std::uint32_t(0)) return {status_code_t::invalid_argument_k, "A page outgrew 4 GiB"};
        if (target.size() != encoded_size()) return {status_code_t::invalid_argument_k, "A target of a wrong size"};
        byte_t* const records = target.data() + page_header_bytes_k;
        byte_t* const attributes = records + std::size_t(count) * page_record_bytes_k;
        byte_t* const directory = attributes + std::size_t(attributes_count) * page_attribute_bytes_k;
        char* const strings = reinterpret_cast<char*>(directory + std::size_t(count) * page_directory_bytes_k);
        store_u32(target.data(), count), store_u32(target.data() + 4, attributes_count);
        store_u32(target.data() + 8, std::uint32_t(strings_length));

        struct directed_t {
            element_key_t key;
            std::uint32_t position;
        };
        st::vector<directed_t, rebound<directed_t>> entries {rebound<directed_t>(allocator_)};
        st::vector<std::uint32_t, rebound<std::uint32_t>> open {rebound<std::uint32_t>(allocator_)};
        if (status_t status = growth_status(entries.reserve(count)); status.failed()) return status;
        if (status_t status = growth_status(open.reserve(count)); status.failed()) return status;

        std::uint32_t position = 0, attribute_position = 0, written = 0;
        auto write_string = [&](string_span_t span, byte_t* where) noexcept {
            if (span.length) std::memcpy(strings + written, strings_.data() + span.offset, span.length);
            store_u32(where, written), store_u32(where + 4, span.length);
            written += span.length;
        };
        element_index_t const root = this->root();
        element_index_t at = root;
        for (;;) {
            element_t const& element = elements_[at];
            byte_t* const record = records + std::size_t(position) * page_record_bytes_k;
            std::memset(record, 0, page_record_bytes_k);
            store_key(record, element.key);
            store_u64(record + 16, element.placement.stamp);
            store_u32(record + 24, element.placement.sequence);
            record[28] = byte_t(element.kind);
            write_string(element.name, record + 36);
            write_string(element.text, record + 44);
            store_u32(record + 52, attribute_position);
            std::uint32_t own_attributes = 0;
            for (element_index_t attribute = element.first_attribute; attribute != no_element_k;
                 attribute = attributes_[attribute].next, ++own_attributes, ++attribute_position) {
                byte_t* const slot = attributes + std::size_t(attribute_position) * page_attribute_bytes_k;
                write_string(attributes_[attribute].name, slot);
                write_string(attributes_[attribute].value, slot + 8);
            }
            store_u32(record + 56, own_attributes);
            (void)entries.push_back(st::assume_reserved, directed_t {element.key, position});
            (void)open.push_back(st::assume_reserved, std::uint32_t(position));
            ++position;
            if (element.first_child != no_element_k) {
                at = element.first_child;
                continue;
            }
            // Climbing closes the subtree of every element whose last descendant was just written
            for (;;) {
                std::uint32_t const closed = open.back();
                open.pop_back();
                store_u32(records + std::size_t(closed) * page_record_bytes_k + 32, position - closed);
                if (at == root) break;
                if (elements_[at].next_sibling != no_element_k) {
                    at = elements_[at].next_sibling;
                    break;
                }
                at = elements_[at].parent;
            }
            if (open.empty()) break;
        }
        std::sort(entries.begin(), entries.end(),
                  [](directed_t const& first, directed_t const& second) noexcept { return first.key < second.key; });
        for (std::uint32_t index = 0; index != count; ++index) {
            store_key(directory + std::size_t(index) * page_directory_bytes_k, entries[index].key);
            store_u32(directory + std::size_t(index) * page_directory_bytes_k + 16, entries[index].position);
        }
        return {};
    }

    /** The dense form in a block of exactly its size from @p allocator, which the caller frees with that size. */
    template <typename output_allocator_type_>
    expected<std::span<byte_t const>> encode(output_allocator_type_& allocator) noexcept {
        std::uint64_t const size = encoded_size();
        byte_t* const block = allocator.allocate(std::size_t(size));
        if (!block) return status_t {status_code_t::out_of_memory_k, "Failed to allocate a page"};
        if (status_t status = encode_into({block, std::size_t(size)}); status.failed()) {
            allocator.deallocate(block, std::size_t(size));
            return status;
        }
        return std::span<byte_t const>(block, std::size_t(size));
    }

    /** Replaces the tree with the dense form in @p bytes, refusing malformed bytes with the tree left empty. */
    status_t decode(std::span<byte_t const> bytes) noexcept {
        auto view = page_view_t::make(bytes);
        if (!view) {
            elements_.clear();
            return view.status();
        }
        return decode(*view);
    }

    /** Replaces the tree with what @p view holds. */
    status_t decode(page_view_t const& view) noexcept {
        page_record_t const root_record = view.record(0);
        if (status_t status = reset(root_record.key, root_record.placement); status.failed()) return status;
        if (status_t status = store(view.string(root_record.name), elements_[0].name); status.failed()) return status;
        if (status_t status = store(view.string(root_record.text), elements_[0].text); status.failed()) return status;
        if (status_t status = growth_status(elements_.reserve(view.size())); status.failed()) return status;

        struct open_t {
            element_index_t index;
            std::uint32_t end;
        };
        st::vector<open_t, rebound<open_t>> open {rebound<open_t>(allocator_)};
        if (status_t status = growth_status(open.reserve(view.size())); status.failed()) return status;
        [[maybe_unused]] auto const pushed_root = open.push_back(st::assume_reserved,
                                                                 open_t {0, root_record.subtree_size});
        auto copy_attributes = [&](element_index_t index, page_record_t const& record) noexcept -> status_t {
            for (std::uint32_t at = 0; at != record.attributes_count; ++at) {
                page_attribute_t const attribute = view.attribute(record.first_attribute + at);
                std::string_view const name = view.string(attribute.name);
                if (find_attribute(index, name) != no_element_k)
                    return {status_code_t::malformed_k, "A page element repeats an attribute"};
                if (status_t status = set_attribute(index, name, view.string(attribute.value)); status.failed())
                    return status;
            }
            return {};
        };
        if (status_t status = copy_attributes(0, root_record); status.failed()) return elements_.clear(), status;
        for (std::uint32_t position = 1; position != view.size(); ++position) {
            while (open.back().end <= position) open.pop_back();
            page_record_t const record = view.record(position);
            if (record.subtree_size > open.back().end - position) {
                elements_.clear();
                return {status_code_t::malformed_k, "A page subtree overruns its parent"};
            }
            auto added = append(open.back().index, record.key, record.kind, view.string(record.name),
                                view.string(record.text), record.placement);
            if (!added) {
                elements_.clear();
                return added.code() == status_code_t::out_of_memory_k
                           ? added.status()
                           : status_t {status_code_t::malformed_k, "A page repeats or nils a key"};
            }
            if (status_t status = copy_attributes(*added, record); status.failed()) return elements_.clear(), status;
            if (record.subtree_size > 1) [[maybe_unused]]
                auto const pushed_child = open.push_back(st::assume_reserved,
                                                         open_t {*added, position + record.subtree_size});
        }
        return {};
    }
};

#pragma endregion Element Tree

} // namespace unum::ustore::pages
