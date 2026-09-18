/**
 *  @file src/ram/key.hpp
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief The composite key every pair of the SmashTable engine is stored under, with its order, hash and bounds.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#include <limits> // `std::numeric_limits`

#include <smashtable/shared.hpp>

#include <ustore/db.h>
#include <ustore/types.hpp>

namespace unum::ustore::ram {

/** The identifier of the nameless main collection. */
inline constexpr ustore_collection_t main_collection_k = 0;

/** The collection reserved for the strings modality that maps names onto collection identifiers, kept
 *  above every identifier the counter hands out so the two can never collide. */
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

} // namespace unum::ustore::ram
