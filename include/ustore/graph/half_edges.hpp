/**
 *  @file include/ustore/graph/half_edges.hpp
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief Half-edges, the entries a graph collection files every vertex and both ends of every edge under, with
 *      their order and hash.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`
#include <cstring> // `std::memcmp`

#include "ustore/utilities/types.hpp" // `uuid_hash_t`, `ustore_key_t`

namespace unum::ustore::graph {

/** Which entry of a vertex's run a half-edge is: the vertex itself, or an edge seen from one of its ends. */
enum class half_edge_direction_t : std::uint8_t {

    /** The vertex itself, with a nil neighbor and edge, present for as long as the vertex is. */
    vertex_k = 0,

    /** An edge leaving the vertex, filed under its target as the neighbor. */
    outgoing_k = 1,

    /** An edge entering the vertex, filed under its source as the neighbor. */
    incoming_k = 2,

    /** After every entry of the vertex, which a walk of the vertex stops before and nothing is filed under. */
    past_k = 3,
};

/**
 *  @brief One entry of a graph collection: a vertex, or an edge filed under one of its ends.
 *
 *  Every edge is filed twice, outgoing under its source and incoming under its target, so the entries of one
 *  vertex are one contiguous run of the set: the vertex, its outgoing edges by target, then its incoming edges
 *  by source. Adding an edge writes two entries instead of rewriting either vertex, so transactions adding
 *  different edges to one vertex never conflict over it.
 *
 *  @code{.c}
 *  struct half_edge {
 *      uint8_t vertex[16];   // offset 0
 *      uint8_t direction;    // offset 16: 0 for the vertex itself, 1 outgoing, 2 incoming
 *      uint8_t neighbor[16]; // offset 17, nil for the vertex itself
 *      uint8_t edge[16];     // offset 33, nil for the vertex itself
 *  };                        // 49 bytes, ordered bytewise
 *  @endcode
 */
struct half_edge_t {
    ustore_key_t vertex {};
    half_edge_direction_t direction {half_edge_direction_t::vertex_k};
    ustore_key_t neighbor {};
    ustore_key_t edge {};

    /** The entry recording that @p vertex exists. */
    static constexpr half_edge_t vertex_entry(ustore_key_t const& vertex) noexcept {
        return {vertex, half_edge_direction_t::vertex_k, {}, {}};
    }

    /** The first entry of @p vertex in @p direction, where a walk of that direction starts. */
    static constexpr half_edge_t first_of(ustore_key_t const& vertex, half_edge_direction_t direction) noexcept {
        return {vertex, direction, {}, {}};
    }

    friend constexpr bool operator==(half_edge_t const&, half_edge_t const&) noexcept = default;
};

static_assert(sizeof(half_edge_t) == 49, "a half-edge packs its three UUIDs around the direction byte");

/** Orders half-edges by their bytes, which groups every vertex's run and sorts it by direction, neighbor and edge. */
struct half_edge_order_t {
    bool operator()(half_edge_t const& first, half_edge_t const& second) const noexcept {
        return std::memcmp(&first, &second, sizeof(half_edge_t)) < 0;
    }
};

/** Hashes a half-edge by its vertex alone, so the run of one vertex stays in one partition. */
struct half_edge_hash_t {
    std::size_t operator()(half_edge_t const& entry) const noexcept {
        return utilities::uuid_hash_t {}(entry.vertex.uuid);
    }
};

} // namespace unum::ustore::graph
