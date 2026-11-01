/**
 *  @file include/ustore/pages/page_edits.hpp
 *  @author Ash Vardanian
 *  @date October 16, 2026
 *  @brief The edit record of every page call of <tt>c/ustore/pages.h</tt> and its serialized delta form.
 *
 *  An edit is one task of one call, stamped with its commit and its position inside it. It names
 *  its element, parent and anchor by key, and carries what it needs to apply without reading
 *  anything else: an import or an insertion carries its parsed fragment in the dense form of
 *  <tt>element_tree.hpp</tt>, keys already minted, so applying never parses markup and replaying always
 *  lands the same keys.
 *
 *  A delta is a chain of edits back to back, oldest first, each little-endian. Every edit starts with a modification
 *  byte, the commit stamp, the task sequence and the element key. Insertions and moves add the parent and anchor keys,
 *  splices the start and length of the replaced text, and attribute edits the name behind a 32-bit length. Imports,
 *  insertions, splices and set attributes end with their value behind a 32-bit length.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <span>        // `std::span`
#include <string_view> // `std::string_view`

#include "ustore/pages/element_tree.hpp" // `element_key_t`, `placement_t`, `page_view_t`

namespace unum::ustore::pages {

/** The kind of an edit, named after the call that makes it and numbered as its serialized form spells it. */
enum class modification_t : std::uint8_t {

    /** Replaces the whole page with a parsed tree, closing every older edit. */
    import_k = 0,

    /** Grafts a parsed fragment under a parent after an anchor. */
    insert_k = 1,

    /** Removes an element with its subtree, empties the root, or removes the page for a Nil element. */
    remove_k = 2,

    /** Moves an element under a parent after an anchor, keeping its key. */
    move_k = 3,

    /** Replaces a run of an element's text with new bytes. */
    splice_k = 4,

    /** Sets an attribute of an element, appending it when missing. */
    set_attributes_k = 5,

    /** Removes an attribute of an element. */
    remove_attributes_k = 6,
};

/** One edit, viewing the bytes of its field and value wherever they live. */
struct edit_t {

    /** What the edit does. */
    modification_t modification = modification_t::import_k;

    /** The commit stamp and the task inside the commit. */
    placement_t placement {};

    /** The element edited, the first one inserted, the page key of an import, or Nil to remove the page. */
    element_key_t element {};

    /** The parent of an insertion or a move, where Nil stands for the root. */
    element_key_t parent {};

    /** The anchor of an insertion or a move, where Nil stands for the front. */
    element_key_t anchor {};

    /** Byte of the text where a splice starts. */
    std::uint64_t text_start = 0;

    /** Bytes of text a splice replaces. */
    std::uint64_t text_length = 0;

    /** The attribute name of setting or removing an attribute. */
    std::string_view field {};

    /** The dense fragment of an import or an insertion, the new bytes of a splice, or an attribute value. */
    std::span<byte_t const> value {};
};

/** Whether edits of @p modification carry a parent and an anchor. */
constexpr bool carries_position(modification_t modification) noexcept {
    return modification == modification_t::insert_k || modification == modification_t::move_k;
}

/** Whether edits of @p modification carry an attribute name. */
constexpr bool carries_field(modification_t modification) noexcept {
    return modification == modification_t::set_attributes_k || modification == modification_t::remove_attributes_k;
}

/** Whether edits of @p modification carry a value. */
constexpr bool carries_value(modification_t modification) noexcept {
    return modification == modification_t::import_k || modification == modification_t::insert_k ||
           modification == modification_t::splice_k || modification == modification_t::set_attributes_k;
}

/** Whether @p edit makes every older edit of its page irrelevant: an import, or the removal of the page. */
constexpr bool closes(edit_t const& edit) noexcept {
    return edit.modification == modification_t::import_k ||
           (edit.modification == modification_t::remove_k && edit.element.is_nil());
}

/** The view of a bytes run as characters. */
inline std::string_view as_chars(std::span<byte_t const> bytes) noexcept {
    return {reinterpret_cast<char const*>(bytes.data()), bytes.size()};
}

/** The view of characters as a bytes run. */
inline std::span<byte_t const> as_bytes(std::string_view chars) noexcept {
    return {reinterpret_cast<byte_t const*>(chars.data()), chars.size()};
}

#pragma region Constructors

/** An import of the dense @p page, whose root key is the page key. */
inline edit_t import_edit(placement_t placement, element_key_t page, std::span<byte_t const> tree) noexcept {
    edit_t edit;
    edit.modification = modification_t::import_k, edit.placement = placement, edit.element = page, edit.value = tree;
    return edit;
}

/** An insertion of the dense @p fragment, whose root's children land under @p parent after @p anchor. */
inline edit_t insert_edit(placement_t placement, element_key_t first, element_key_t parent, element_key_t anchor,
                          std::span<byte_t const> fragment) noexcept {
    edit_t edit;
    edit.modification = modification_t::insert_k, edit.placement = placement, edit.element = first;
    edit.parent = parent, edit.anchor = anchor, edit.value = fragment;
    return edit;
}

/** A removal of @p element with its subtree, or of the page for Nil. */
inline edit_t remove_edit(placement_t placement, element_key_t element) noexcept {
    edit_t edit;
    edit.modification = modification_t::remove_k, edit.placement = placement, edit.element = element;
    return edit;
}

/** A move of @p element under @p parent after @p anchor. */
inline edit_t move_edit(placement_t placement, element_key_t element, element_key_t parent,
                        element_key_t anchor) noexcept {
    edit_t edit;
    edit.modification = modification_t::move_k, edit.placement = placement, edit.element = element;
    edit.parent = parent, edit.anchor = anchor;
    return edit;
}

/** A splice replacing @p length bytes of the text of @p element from @p start with @p bytes. */
inline edit_t splice_edit(placement_t placement, element_key_t element, std::uint64_t start, std::uint64_t length,
                          std::string_view bytes) noexcept {
    edit_t edit;
    edit.modification = modification_t::splice_k, edit.placement = placement, edit.element = element;
    edit.text_start = start, edit.text_length = length, edit.value = as_bytes(bytes);
    return edit;
}

/** Setting the attribute @p field of @p element to @p value. */
inline edit_t set_attribute_edit(placement_t placement, element_key_t element, std::string_view field,
                                 std::string_view value) noexcept {
    edit_t edit;
    edit.modification = modification_t::set_attributes_k, edit.placement = placement, edit.element = element;
    edit.field = field, edit.value = as_bytes(value);
    return edit;
}

/** Removing the attribute @p field of @p element. */
inline edit_t remove_attribute_edit(placement_t placement, element_key_t element, std::string_view field) noexcept {
    edit_t edit;
    edit.modification = modification_t::remove_attributes_k, edit.placement = placement, edit.element = element;
    edit.field = field;
    return edit;
}

#pragma endregion Constructors

#pragma region Serialized Form

/** Bytes every edit starts with: modification, stamp, sequence and element key. */
inline constexpr std::size_t edit_header_bytes_k = 1 + 8 + 4 + 16;

/** Bytes of the serialized form of @p edit. */
constexpr std::size_t encoded_size(edit_t const& edit) noexcept {
    std::size_t size = edit_header_bytes_k;
    if (carries_position(edit.modification)) size += 32;
    if (edit.modification == modification_t::splice_k) size += 16;
    if (carries_field(edit.modification)) size += 4 + edit.field.size();
    if (carries_value(edit.modification)) size += 4 + edit.value.size();
    return size;
}

/** Writes @p edit into @p target, which holds at least @c encoded_size of it, returning the bytes written. */
inline std::size_t encode_into(edit_t const& edit, byte_t* target) noexcept {
    byte_t* at = target;
    *at++ = byte_t(edit.modification);
    store_u64(at, edit.placement.stamp), at += 8;
    store_u32(at, edit.placement.sequence), at += 4;
    store_key(at, edit.element), at += 16;
    if (carries_position(edit.modification)) {
        store_key(at, edit.parent), at += 16;
        store_key(at, edit.anchor), at += 16;
    }
    if (edit.modification == modification_t::splice_k) {
        store_u64(at, edit.text_start), at += 8;
        store_u64(at, edit.text_length), at += 8;
    }
    if (carries_field(edit.modification)) {
        store_u32(at, std::uint32_t(edit.field.size())), at += 4;
        if (!edit.field.empty()) std::memcpy(at, edit.field.data(), edit.field.size()), at += edit.field.size();
    }
    if (carries_value(edit.modification)) {
        store_u32(at, std::uint32_t(edit.value.size())), at += 4;
        if (!edit.value.empty()) std::memcpy(at, edit.value.data(), edit.value.size()), at += edit.value.size();
    }
    return std::size_t(at - target);
}

/**
 *  @brief Reads the edit at the front of @p bytes, viewing its field and value inside @p bytes.
 *  @param[in] bytes The encoded edits, the first of which is read.
 *  @param[out] consumed Bytes the edit took, on success.
 *  @return The edit, or @c status_code_t::malformed_k for a truncated record, an unknown modification, or a field or
 *      value past 4 GiB.
 */
inline expected<edit_t> decode_edit(std::span<byte_t const> bytes, std::size_t& consumed) noexcept {
    status_t const truncated {status_code_t::malformed_k, "An edit record is truncated"};
    if (bytes.size() < edit_header_bytes_k) return truncated;
    byte_t const* at = bytes.data();
    byte_t const* const end = bytes.data() + bytes.size();
    edit_t edit;
    std::uint8_t const modification = std::uint8_t(*at++);
    if (modification > std::uint8_t(modification_t::remove_attributes_k))
        return status_t {status_code_t::malformed_k, "An edit record has an unknown modification"};
    edit.modification = modification_t(modification);
    edit.placement.stamp = load_u64(at), at += 8;
    edit.placement.sequence = load_u32(at), at += 4;
    edit.element = load_key(at), at += 16;
    if (carries_position(edit.modification)) {
        if (end - at < 32) return truncated;
        edit.parent = load_key(at), at += 16;
        edit.anchor = load_key(at), at += 16;
    }
    if (edit.modification == modification_t::splice_k) {
        if (end - at < 16) return truncated;
        edit.text_start = load_u64(at), at += 8;
        edit.text_length = load_u64(at), at += 8;
    }
    if (carries_field(edit.modification)) {
        if (end - at < 4) return truncated;
        std::uint32_t const length = load_u32(at);
        at += 4;
        if (std::size_t(end - at) < length) return truncated;
        edit.field = {reinterpret_cast<char const*>(at), length}, at += length;
    }
    if (carries_value(edit.modification)) {
        if (end - at < 4) return truncated;
        std::uint32_t const length = load_u32(at);
        at += 4;
        if (std::size_t(end - at) < length) return truncated;
        edit.value = {at, length}, at += length;
    }
    consumed = std::size_t(at - bytes.data());
    return edit;
}

/** Walks a chain of edits oldest first, viewing each inside the chain. */
class edits_reader_t {
    std::span<byte_t const> chain_;
    std::size_t offset_ = 0;

  public:
    explicit edits_reader_t(std::span<byte_t const> chain) noexcept : chain_(chain) {}

    /** Whether every edit was read. */
    bool done() const noexcept { return offset_ == chain_.size(); }

    /** Reads the next edit, which must exist. */
    expected<edit_t> next() noexcept {
        std::size_t consumed = 0;
        auto edit = decode_edit(chain_.subspan(offset_), consumed);
        if (edit) offset_ += consumed;
        return edit;
    }
};

/** Calls @p callback with every edit of @p chain oldest first, stopping at the first status it fails with. */
template <typename callback_type_>
status_t for_each_edit(std::span<byte_t const> chain, callback_type_&& callback) noexcept {
    edits_reader_t reader(chain);
    while (!reader.done()) {
        auto edit = reader.next();
        if (!edit) return edit.status();
        if (status_t status = callback(*edit); status.failed()) return status;
    }
    return {};
}

/** The chain of @p edits in a block of exactly its size from @p allocator, which the caller frees with that size. */
template <typename allocator_type_>
expected<std::span<byte_t const>> encode_edits(std::span<edit_t const> edits, allocator_type_& allocator) noexcept {
    std::size_t size = 0;
    for (edit_t const& edit : edits) {
        if (edit.field.size() > ~std::uint32_t(0) || edit.value.size() > ~std::uint32_t(0))
            return status_t {status_code_t::invalid_argument_k, "An edit carries more than 4 GiB"};
        size += encoded_size(edit);
    }
    byte_t* const block = size ? allocator.allocate(size) : nullptr;
    if (size && !block) return status_t {status_code_t::out_of_memory_k, "Failed to allocate a delta"};
    std::size_t offset = 0;
    for (edit_t const& edit : edits) offset += encode_into(edit, block + offset);
    return std::span<byte_t const>(block, size);
}

/** The keys an import or an insertion lands, every element of its fragment besides the root, in preorder. */
template <typename callback_type_>
status_t for_each_minted_key(edit_t const& edit, callback_type_&& callback) noexcept {
    if (edit.modification != modification_t::import_k && edit.modification != modification_t::insert_k) return {};
    auto view = page_view_t::make(edit.value);
    if (!view) return view.status();
    for (std::uint32_t position = 1; position < view->size(); ++position) callback(view->record(position).key);
    return {};
}

#pragma endregion Serialized Form

} // namespace unum::ustore::pages
