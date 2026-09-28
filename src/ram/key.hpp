/**
 *  @file src/ram/key.hpp
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief The composite keys the pairs of the SmashTable engine are stored under, with their orders, hashes and bounds.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#include <limits>      // `std::numeric_limits`
#include <string_view> // `std::string_view`
#include <utility>     // `std::move`

#include <smashtable/basic_vector.hpp>
#include <smashtable/shared.hpp>
#include <stringzilla/stringzilla.hpp>

#include <ustore/db.h>
#include <ustore/types.hpp>

namespace unum::ustore::ram {

/** The identifier of the nameless main collection. */
inline constexpr ustore_collection_t main_collection_k = 0;

/** The collection of the strings store holding the record of every collection name, kept above every
 *  identifier the counter hands out and never listed in the catalog, so no call reaches it. */
inline constexpr ustore_collection_t names_collection_k = std::numeric_limits<ustore_collection_t>::max() - 1;

/** The reserved collection holding one entry per collection and the counter that numbers them. */
inline constexpr ustore_collection_t catalog_collection_k = std::numeric_limits<ustore_collection_t>::max();

/** The catalog key of the counter, which sits above every entry and below @c stored_key_t::highest. */
inline constexpr ustore_key_t collection_counter_key_k = std::numeric_limits<ustore_key_t>::max() - 1;

/** A key inside a collection, and the only type that knows how keys order, hash and bound a collection. */
struct stored_key_t {

    /** Collection owning the key. */
    ustore_collection_t collection {main_collection_k};

    /** Key inside the collection. */
    ustore_key_t key {0};

    /** The key that stands for no key. */
    static constexpr ustore_key_t unknown_key_k = std::numeric_limits<ustore_key_t>::max();

    /** The first key @p collection can hold. */
    static constexpr stored_key_t lowest_in(ustore_collection_t collection) noexcept {
        return {collection, std::numeric_limits<ustore_key_t>::min()};
    }

    /** The last key @p collection can hold. */
    static constexpr stored_key_t highest_in(ustore_collection_t collection) noexcept {
        return {collection, std::numeric_limits<ustore_key_t>::max()};
    }

    /** The first key after every key of @p collection, which a walk of it stops before. */
    static constexpr stored_key_t past(ustore_collection_t collection) noexcept { return lowest_in(collection + 1); }

    /** The first key the store can hold. */
    static constexpr stored_key_t lowest() noexcept { return lowest_in(main_collection_k); }

    /** The last key the store can hold, which nothing is ever stored under. */
    static constexpr stored_key_t highest() noexcept { return highest_in(catalog_collection_k); }

    /** The catalog entry naming @p collection. */
    static constexpr stored_key_t catalog_entry(ustore_collection_t collection) noexcept {
        return {catalog_collection_k, static_cast<ustore_key_t>(collection)};
    }

    /** The catalog counter, which every created collection advances and so every concurrent creation conflicts on. */
    static constexpr stored_key_t collection_counter() noexcept {
        return {catalog_collection_k, collection_counter_key_k};
    }

    /** Whether this key belongs to the catalog, which writes through the C ABI may not touch. */
    constexpr bool is_catalog_entry() const noexcept { return collection == catalog_collection_k; }

    /** The collection a catalog entry names. */
    constexpr ustore_collection_t cataloged() const noexcept { return static_cast<ustore_collection_t>(key); }
};

/** The identifier past the last one a collection may take, so no catalog entry reaches the counter. */
inline constexpr ustore_collection_t collections_limit_k = static_cast<ustore_collection_t>(collection_counter_key_k);

/** Orders keys by collection first, so every collection is one contiguous window of the store. */
struct stored_key_order_t {
    constexpr bool operator()(stored_key_t const& first, stored_key_t const& second) const noexcept {
        if (first.collection != second.collection) return first.collection < second.collection;
        return first.key < second.key;
    }
};

/** Hashes a key as its two machine words, spreading every collection over all partitions for writer concurrency. */
struct stored_key_hash_t {
    constexpr std::size_t operator()(stored_key_t const& stored) const noexcept {
        st::hash<std::uint64_t> const mixer;
        std::uint64_t const collection_hash = mixer(stored.collection);
        return mixer(static_cast<std::uint64_t>(stored.key) ^ (collection_hash * 0x9E3779B97F4A7C15ull));
    }
};

/** A value, or the bytes of a string key, owned so it outlives the call that wrote it. */
using blob_t = st::basic_vector<ustore_byte_t>;

/** A string inside a collection keyed by strings, owning its bytes. */
struct string_key_t {

    /** Collection owning the string. */
    ustore_collection_t collection {main_collection_k};

    /** Bytes of the string. */
    blob_t text;

    /** The first key @p collection can hold, the empty string, which owns no bytes. */
    static string_key_t lowest_in(ustore_collection_t collection) noexcept { return {collection, {}}; }

    /** The first key after every key of @p collection, which a walk of it stops before. */
    static string_key_t past(ustore_collection_t collection) noexcept { return lowest_in(collection + 1); }

    /** The bytes of the string. */
    std::string_view view() const noexcept { return {reinterpret_cast<char const*>(text.data()), text.size()}; }

    /** A copy owning bytes of its own, or why they could not be allocated, which the store asks of a key it
     *  records. */
    st::expected<string_key_t> copy() const noexcept {
        st::expected<blob_t> copied = text.copy();
        if (!copied) return copied.status();
        return string_key_t {collection, std::move(*copied)};
    }
};

/** Orders string keys by collection, then by their bytes, so every collection is one contiguous window. */
struct string_key_order_t {
    bool operator()(string_key_t const& first, string_key_t const& second) const noexcept {
        if (first.collection != second.collection) return first.collection < second.collection;
        return sz::string_view_t {first.view()} < sz::string_view_t {second.view()};
    }
};

/** Hashes a string key through StringZilla, seeded by its collection, spreading every collection over all
 *  partitions. */
struct string_key_hash_t {
    std::size_t operator()(string_key_t const& stored) const noexcept {
        return sz::string_view_t {stored.view()}.hash(stored.collection);
    }
};

} // namespace unum::ustore::ram
