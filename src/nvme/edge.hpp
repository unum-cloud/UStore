/**
 *  @file src/nvme/edge.hpp
 *  @author Ash Vardanian
 *  @date December 1, 2022
 *  @brief Edge and role types of the persistent graph database.
 */

#pragma once
#include <cmath>   // `std::abs`
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`

#include <algorithm> // `std::max`, `std::min`
#include <limits>    // `std::numeric_limits`

#include <fmt/format.h>

#include <smashtable/basic_flat_set.hpp> // `st::basic_flat_set`

#include "nvme/vertex.hpp" // `id_entity_t`, `label_t`

namespace unum::ustore::nvme {

/** Bitmask of the parts a vertex plays in an edge. */
struct role_t {
    std::uint8_t raw = 0;

    friend constexpr auto operator<=>(role_t, role_t) noexcept = default;
    friend constexpr bool operator&(role_t first, role_t second) noexcept { return first.raw & second.raw; }
    friend constexpr role_t operator|(role_t first, role_t second) noexcept {
        return role_t {static_cast<std::uint8_t>(first.raw | second.raw)};
    }

    constexpr bool is_unknown() const noexcept { return !raw; }
    constexpr role_t allow(role_t other) const noexcept { return role_t {static_cast<std::uint8_t>(raw | other.raw)}; }
    constexpr role_t disallow(role_t other) const noexcept {
        return role_t {static_cast<std::uint8_t>(raw & ~other.raw)};
    }
};

inline constexpr role_t edge_member_k {1 << 0};
inline constexpr role_t dedge_source_k {1 << 1};
inline constexpr role_t dedge_target_k {1 << 2};
inline constexpr role_t hedge_leaf_k {1 << 3};
inline constexpr role_t hedge_parent_k {1 << 4};
inline constexpr role_t any_role_k {std::numeric_limits<std::uint8_t>::max()};

constexpr role_t opposite(role_t role) noexcept {
    switch (role.raw) {
    case dedge_source_k.raw: return dedge_target_k;
    case dedge_target_k.raw: return dedge_source_k;
    case hedge_leaf_k.raw: return hedge_parent_k;
    case hedge_parent_k.raw: return hedge_leaf_k;
    default: return role;
    }
}

/** Whether an edge leads from its first vertex to its second one, or joins them both ways. */
enum class direction_t : bool { undirected_k, directed_k };

/** Weighted, labeled edge whose @c first and @c second endpoints follow the Boost Graph Library edge lists. */
struct labeled_edge_t {
    id_entity_t id = 0;
    id_entity_t first = 0;
    id_entity_t second = 0;
    float weight = 1;
    label_t label;
    direction_t direction = direction_t::directed_k;

    /** Compares everything but the direction. */
    static constexpr bool equals(labeled_edge_t const& lhs, labeled_edge_t const& rhs) noexcept {
        return lhs.id == rhs.id && lhs.first == rhs.first && lhs.second == rhs.second && lhs.label == rhs.label &&
               lhs.weight == rhs.weight;
    }

    friend constexpr bool operator==(labeled_edge_t const& lhs, labeled_edge_t const& rhs) noexcept {
        return equals(lhs, rhs);
    }
    friend constexpr auto operator<=>(labeled_edge_t const& lhs, labeled_edge_t const& rhs) noexcept {
        return lhs.id <=> rhs.id;
    }

    constexpr explicit operator bool() const noexcept { return id && first && second; }
    constexpr bool is_loop() const noexcept { return first == second; }
    constexpr bool contains(id_entity_t vertex_id) const noexcept {
        return (first == vertex_id) | (second == vertex_id);
    }
    constexpr bool is_between(id_entity_t first_vertex_id, id_entity_t second_vertex_id) const noexcept {
        return ((first == first_vertex_id) & (second == second_vertex_id)) |
               ((second == first_vertex_id) & (first == second_vertex_id));
    }
    constexpr id_entity_t neighbor_of(id_entity_t known_member) const noexcept {
        return first == known_member ? second : first;
    }

    constexpr void clear() noexcept { *this = labeled_edge_t {}; }

    constexpr role_t role_of(id_entity_t known_member) const noexcept {
        constexpr role_t mapping[2][2] = {
            {edge_member_k, edge_member_k},
            {dedge_target_k, dedge_source_k},
        };
        return mapping[static_cast<std::size_t>(direction)][first == known_member];
    }
};

static_assert(sizeof(labeled_edge_t) <= (sizeof(id_entity_t) * 5));

using edges_set_t = st::basic_flat_set<labeled_edge_t>;

inline bool similar(labeled_edge_t const& lhs, labeled_edge_t const& rhs) noexcept {
    return lhs.label == rhs.label && std::abs(lhs.weight - rhs.weight) < std::numeric_limits<float>::epsilon();
}

struct half_edge_t {
    id_entity_t edge_id = 0;
    id_entity_t neighbor_id = 0;
    float edge_weight = 0;
    label_t edge_label;
    role_t role;

    constexpr half_edge_t() noexcept = default;

    constexpr explicit half_edge_t(labeled_edge_t const& edge, id_entity_t member) noexcept
        : edge_id(edge.id), edge_weight(edge.weight), edge_label(edge.label) {
        bool const is_directed = edge.direction == direction_t::directed_k;
        if (edge.first == member) {
            neighbor_id = edge.second;
            role = is_directed ? dedge_source_k : edge_member_k;
        }
        else {
            neighbor_id = edge.first;
            role = is_directed ? dedge_target_k : edge_member_k;
        }
    }

    constexpr labeled_edge_t to_edge(id_entity_t vertex) const noexcept {
        labeled_edge_t edge;
        edge.id = edge_id;
        edge.label = edge_label;
        edge.weight = edge_weight;

        switch (role.raw) {
        case dedge_source_k.raw:
            edge.direction = direction_t::directed_k;
            edge.first = vertex;
            edge.second = neighbor_id;
            break;

        case dedge_target_k.raw:
            edge.direction = direction_t::directed_k;
            edge.first = neighbor_id;
            edge.second = vertex;
            break;

        default:
            edge.direction = direction_t::undirected_k;
            edge.first = std::min(vertex, neighbor_id);
            edge.second = std::max(vertex, neighbor_id);
            break;
        }
        return edge;
    }
};

/** Formats a labeled edge as `edge<id, first->second>`, or with a plain dash when undirected. */
struct labeled_edge_formatter_t {

    constexpr auto parse(fmt::format_parse_context& context) { return context.begin(); }

    template <typename context_>
    auto format(labeled_edge_t const& edge, context_& context) const {
        bool const is_directed = edge.direction == direction_t::directed_k;
        return fmt::format_to(context.out(), "edge<{}, {}{}{}>", edge.id, edge.first, is_directed ? "->" : "-",
                              edge.second);
    }
};

} // namespace unum::ustore::nvme

template <>
struct fmt::formatter<unum::ustore::nvme::labeled_edge_t> : unum::ustore::nvme::labeled_edge_formatter_t {};
