/**
 *  @file include/ustore/strings/string_keys.hpp
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief The keys a strings collection and the names of collections file their entries under, owning their bytes,
 *      with their order and hash, and the 16-byte heads that search them.
 *
 *  A store whose cells hold 16-byte keys, as NVMe runs do, files a string under its head, its leading 15 bytes and
 *  its length byte, and keeps the bytes past the head, its tail, beside the cells. Heads order as their strings do
 *  wherever they differ, so a search compares heads first and tails only between equal heads.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`

#include <string_view> // `std::string_view`
#include <utility>     // `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`
#include <smashtable/shared.hpp>       // `st::expected`
#include <stringzilla/stringzilla.hpp> // `sz::string_view_t`

#include "ustore/utilities/types.hpp" // `ustore_byte_t`, `ustore_key_t`

namespace unum::ustore::strings {

/** A value, or the bytes of a string, owned so it outlives the call that wrote it. */
using blob_t = st::basic_vector<ustore_byte_t>;

/** A string of a strings collection, or the name of a collection, owning its bytes. */
struct string_key_t {

    /** Bytes of the string. */
    blob_t text;

    /** The bytes of the string. */
    std::string_view view() const noexcept { return {reinterpret_cast<char const*>(text.data()), text.size()}; }

    /** A copy owning bytes of its own, or why they could not be allocated, which the store asks of a key it
     *  records. */
    st::expected<string_key_t> copy() const noexcept {
        st::expected<blob_t> copied = text.copy();
        if (!copied) return copied.status();
        return string_key_t {std::move(*copied)};
    }
};

/** Orders strings by their bytes, unsigned. */
struct string_key_order_t {
    bool operator()(string_key_t const& first, string_key_t const& second) const noexcept {
        return sz::string_view_t {first.view()} < sz::string_view_t {second.view()};
    }
};

/** Hashes a string through StringZilla, spreading the strings of a collection over every partition. */
struct string_key_hash_t {
    std::size_t operator()(string_key_t const& stored) const noexcept {
        return sz::string_view_t {stored.view()}.hash();
    }
};

#pragma region Heads

/** Bytes of a head that hold the leading bytes of a string, leaving the last one for its length. */
inline constexpr std::size_t head_prefix_bytes_k = sizeof(ustore_key_t) - 1;

/** The length byte of the head every string longer than @c head_prefix_bytes_k shares with its neighbors. */
inline constexpr unsigned char long_head_length_k = static_cast<unsigned char>(head_prefix_bytes_k + 1);

/**
 *  @brief The 16-byte head of @p text: its leading bytes padded with zeros, then its length, capped one past them.
 *
 *  Heads order as their strings do wherever they differ, and equal heads below the cap are equal strings, so a search
 *  compares heads first and the bytes past them, the @c tail_of, only between equal heads.
 */
constexpr ustore_key_t head_of(std::string_view text) noexcept {
    ustore_key_t head {};
    for (std::size_t index = 0; index != head_prefix_bytes_k && index != text.size(); ++index)
        head.uuid.bytes[index] = static_cast<std::uint8_t>(text[index]);
    head.uuid.bytes[head_prefix_bytes_k] = text.size() <= head_prefix_bytes_k ? static_cast<std::uint8_t>(text.size())
                                                                              : long_head_length_k;
    return head;
}

/** The bytes of @p text past the leading ones its head holds, empty for a string its head holds whole. */
constexpr std::string_view tail_of(std::string_view text) noexcept {
    return text.size() > head_prefix_bytes_k ? text.substr(head_prefix_bytes_k) : std::string_view {};
}

#pragma endregion Heads

} // namespace unum::ustore::strings
