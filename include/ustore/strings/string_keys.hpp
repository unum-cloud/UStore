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

#include <cstddef> // `std::byte`, `std::size_t`

#include <string_view> // `std::string_view`
#include <utility>     // `std::move`

#include <smashtable/row_layout.hpp>   // `st::key128_t`
#include <smashtable/shared.hpp>       // `st::expected`
#include <stringzilla/stringzilla.hpp> // `sz::string_view_t`

#include "ustore/utilities/ram_buffers.hpp" // `strings_kit_concept`
#include "ustore/utilities/types.hpp"       // `blob_t`

namespace unum::ustore::strings {

using utilities::blob_t;

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

/** Hashes a string through the kit's StringZilla kernel, spreading the strings of a collection
 *  over every partition. */
template <utilities::strings_kit_concept strings_kit_type_>
struct string_key_hash {
    std::size_t operator()(string_key_t const& stored) const noexcept {
        std::string_view const text = stored.view();
        sz_u64_t hashed = 0;
        // An empty string may view a null pointer, which the hash kernels refuse.
        if (!text.empty()) strings_kit_type_::hash(text.data(), text.size(), 0, &hashed, nullptr);
        return static_cast<std::size_t>(hashed);
    }
};

#pragma region Heads

/** Bytes of a head that hold the leading bytes of a string, leaving the last one for its length. */
inline constexpr std::size_t head_prefix_bytes_k = sizeof(st::key128_t) - 1;

/** The length byte of the head every string longer than @c head_prefix_bytes_k shares with its neighbors. */
inline constexpr unsigned char long_head_length_k = static_cast<unsigned char>(head_prefix_bytes_k + 1);

/**
 *  @brief The 16-byte head of @p text: its leading bytes padded with zeros, then its length, capped one past them.
 *
 *  Heads order as their strings do wherever they differ, and equal heads below the cap are equal strings, so a search
 *  compares heads first and the bytes past them, the @c tail_of, only between equal heads.
 */
constexpr st::key128_t head_of(std::string_view text) noexcept {
    std::byte bytes[sizeof(st::key128_t)] {};
    for (std::size_t index = 0; index != head_prefix_bytes_k && index != text.size(); ++index)
        bytes[index] = static_cast<std::byte>(text[index]);
    bytes[head_prefix_bytes_k] = static_cast<std::byte>(text.size() <= head_prefix_bytes_k ? text.size()
                                                                                           : long_head_length_k);
    return st::key128_t::from_bytes(bytes);
}

/** The length byte of @p head, its last byte, which the low word holds lowest since words read big-endian. */
constexpr unsigned char head_length_of(st::key128_t const& head) noexcept {
    return static_cast<unsigned char>(head.low & 0xFF);
}

/** The bytes of @p text past the leading ones its head holds, empty for a string its head holds whole. */
constexpr std::string_view tail_of(std::string_view text) noexcept {
    return text.size() > head_prefix_bytes_k ? text.substr(head_prefix_bytes_k) : std::string_view {};
}

#pragma endregion Heads

} // namespace unum::ustore::strings
