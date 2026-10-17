/**
 *  @file include/ustore/strings/string_keys.hpp
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief The keys a strings collection and the names of collections file their entries under, owning their bytes,
 *      with their order and hash.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <string_view> // `std::string_view`
#include <utility>     // `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`
#include <smashtable/shared.hpp>       // `st::expected`
#include <stringzilla/stringzilla.hpp> // `sz::string_view_t`

#include "ustore/utilities/types.hpp" // `ustore_byte_t`

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

} // namespace unum::ustore::strings
