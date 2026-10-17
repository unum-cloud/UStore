/**
 *  @file include/ustore/graph/edge_block.hpp
 *  @author Ash Vardanian
 *  @date December 1, 2022
 *  @brief Vertices, labeled edges and their half-edges, and the sorted edge blocks holding the half-edges of one
 *      vertex as parallel arrays.
 *
 *  Vertices and edges are named by the 16-byte keys of the C ABI, and the nil key names nothing. An edge carries
 *  its identifier, its @c first and @c second vertices, weight, label and direction, and both endpoints store it
 *  as a half-edge: the edge's identifier, weight and label, the other vertex, and the role it plays. A vertex's
 *  half-edges split into edge blocks of 16 to 512 half-edges, 128 at best, with every half-edge to one neighbor
 *  in the same block, so a small update rewrites one block rather than the whole adjacency list.
 */

#pragma once

#include <cmath>   // `std::abs`
#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `std::int32_t`, `std::uint8_t`, `std::uint16_t`

#include <algorithm> // `std::is_sorted`, `std::lower_bound`, `std::max`, `std::min`
#include <compare>   // `std::strong_ordering`
#include <limits>    // `std::numeric_limits`
#include <span>      // `std::span`

#include <fmt/format.h>

#include <smashtable/basic_flat_set.hpp> // `st::basic_flat_set`
#include <smashtable/basic_vector.hpp>   // `st::basic_vector`, `st::assume_reserved`

#include "ustore/blobs/nvme_key_slot.hpp" // `limit`
#include "ustore/utilities/algorithm.hpp" // `position_t`, `find_in_sorted`, `indexed_iterator`, `indexed_range`
#include "ustore/utilities/nvme_ring.hpp" // `bytes_span_t`, `serializable_concept`, `save_fields`, `load_fields`
#include "ustore/utilities/types.hpp"     // `status_t`, `status_code_t`, `expected`, `key_after`, `uuid_to_text`

namespace unum::ustore::graph {

using utilities::bytes_span_t;
using utilities::bytes_view_t;
using utilities::expected;
using utilities::find_in_sorted;
using utilities::indexed_iterator;
using utilities::indexed_range;
using utilities::position_t;
using utilities::serializable_concept;
using utilities::status_code_t;
using utilities::status_t;

#pragma region Vertices

/** The identifier of a vertex or an edge: the 16-byte key of the C ABI, where the nil key names nothing. */
using id_entity_t = ustore_key_t;

/** The label of a vertex or an edge, where a negative one is unknown. */
struct label_t {
    std::int32_t raw = -1;

    friend constexpr auto operator<=>(label_t const&, label_t const&) noexcept = default;
    constexpr bool is_unknown() const noexcept { return raw < 0; }
};

/** A vertex: its identifier, which alone orders and compares it, its weight and its label. */
struct vertex_t {
    id_entity_t id {};
    float weight = 1;
    label_t label;

    friend constexpr bool operator==(vertex_t const& lhs, vertex_t const& rhs) noexcept { return lhs.id == rhs.id; }
    friend constexpr auto operator<=>(vertex_t const& lhs, vertex_t const& rhs) noexcept { return lhs.id <=> rhs.id; }

    constexpr void clear() noexcept {
        id = {};
        label.raw = 0;
        weight = 1;
    }
};

using vertices_set_t = st::basic_flat_set<vertex_t>;

/** Key of one chunk of the half-edges of an entity, ordered by entity and then by the chunk's first neighbor. */
struct id_chunk_t {
    id_entity_t entity {};
    id_entity_t chunk {};

    friend constexpr auto operator<=>(id_chunk_t const&, id_chunk_t const&) noexcept = default;

    constexpr explicit operator bool() const noexcept {
        return entity != utilities::smallest_key_k && chunk != utilities::smallest_key_k;
    }
    std::byte const* data() const noexcept { return reinterpret_cast<std::byte const*>(this); }
    constexpr std::size_t size() const noexcept { return sizeof(id_chunk_t); }

    constexpr void clear() noexcept {
        entity = {};
        chunk = {};
    }

    static constexpr id_chunk_t min(id_entity_t entity_id) noexcept { return {entity_id, utilities::smallest_key_k}; }
    static constexpr id_chunk_t max(id_entity_t entity_id) noexcept { return {entity_id, utilities::largest_key_k}; }
    static constexpr id_chunk_t min() noexcept { return {utilities::smallest_key_k, utilities::smallest_key_k}; }
    static constexpr id_chunk_t max() noexcept { return {utilities::largest_key_k, utilities::largest_key_k}; }
};

static_assert(sizeof(id_chunk_t) == (sizeof(id_entity_t) * 2));

/** The canonical text of @p key, for the formatters below. */
struct printed_key_t {
    char text[utilities::uuid_text_length_k] = {};

    explicit printed_key_t(id_entity_t const& key) noexcept { utilities::uuid_to_text(key.uuid, text); }
    std::string_view view() const noexcept { return {text, utilities::uuid_text_length_k}; }
};

/** Formats a chunk identifier as `id<entity | chunk>`. */
struct id_chunk_formatter_t {

    constexpr auto parse(fmt::format_parse_context& context) { return context.begin(); }

    template <typename context_>
    auto format(id_chunk_t const& chunk_id, context_& context) const {
        return fmt::format_to(context.out(), "id<{} | {}>", printed_key_t {chunk_id.entity}.view(),
                              printed_key_t {chunk_id.chunk}.view());
    }
};

#pragma endregion Vertices

#pragma region Edges

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
    id_entity_t id {};
    id_entity_t first {};
    id_entity_t second {};
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

    constexpr explicit operator bool() const noexcept {
        return id != utilities::smallest_key_k && first != utilities::smallest_key_k &&
               second != utilities::smallest_key_k;
    }
    constexpr bool is_loop() const noexcept { return first == second; }
    constexpr bool contains(id_entity_t vertex_id) const noexcept { return first == vertex_id || second == vertex_id; }
    constexpr bool is_between(id_entity_t first_vertex_id, id_entity_t second_vertex_id) const noexcept {
        return (first == first_vertex_id && second == second_vertex_id) ||
               (second == first_vertex_id && first == second_vertex_id);
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

/** One end of a labeled edge, as the vertex at that end stores it: the edge, the other vertex and the role. */
struct labeled_half_edge_t {
    id_entity_t edge_id {};
    id_entity_t neighbor_id {};
    float edge_weight = 0;
    label_t edge_label;
    role_t role;

    constexpr labeled_half_edge_t() noexcept = default;

    constexpr explicit labeled_half_edge_t(labeled_edge_t const& edge, id_entity_t member) noexcept
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
        return fmt::format_to(context.out(), "edge<{}, {}{}{}>", printed_key_t {edge.id}.view(),
                              printed_key_t {edge.first}.view(), is_directed ? "->" : "-",
                              printed_key_t {edge.second}.view());
    }
};

#pragma endregion Edges

#pragma region Adjacency Span

/** Weighted edge between two vertices of an adjacency span. */
template <typename weight_, typename vertex_id_>
struct adjacency_edge {
    vertex_id_ first;
    vertex_id_ second;
    weight_ weight;
};

/**
 *  @brief Adjacency list of a weighted directed sparse graph, viewed as three parallel arrays.
 *  @tparam weight_ Type of the edge weights.
 *  @tparam vertex_id_ Type of the vertex identifiers.
 */
template <typename weight_, typename vertex_id_>
class adjacency_span {
  public:
    constexpr adjacency_span() noexcept = default;
    constexpr adjacency_span(std::span<vertex_id_> first_ids, std::span<vertex_id_> second_ids,
                             std::span<weight_> edge_weights) noexcept
        : firsts_(first_ids), seconds_(second_ids), weights_(edge_weights) {}

    constexpr std::size_t size() const noexcept { return firsts_.size(); }
    constexpr std::span<vertex_id_ const> firsts() const noexcept { return firsts_; }
    constexpr std::span<vertex_id_ const> seconds() const noexcept { return seconds_; }
    constexpr std::span<weight_ const> weights() const noexcept { return weights_; }

    constexpr adjacency_edge<weight_ const&, vertex_id_ const&> operator[](std::size_t index) const noexcept {
        return {firsts_[index], seconds_[index], weights_[index]};
    }
    constexpr adjacency_edge<weight_&, vertex_id_&> operator[](std::size_t index) noexcept {
        return {firsts_[index], seconds_[index], weights_[index]};
    }

  private:
    std::span<vertex_id_> firsts_;
    std::span<vertex_id_> seconds_;
    std::span<weight_> weights_;
};

using adjacency_edge_t = adjacency_edge<float, id_entity_t>;
using adjacency_span_t = adjacency_span<float const, id_entity_t const>;

#pragma endregion Adjacency Span

#pragma region Edge Block

/** Size class of an edge block, quantized so small inserts and removals rarely rewrite the registry. */
enum class edge_block_size_t : std::uint8_t { normal_k = 0, too_small_k = 1, too_big_k = 2, empty_k = 3 };

struct edge_block_specs_t {
    id_entity_t first_neighbor {};
    id_entity_t last_neighbor {};
    edge_block_size_t size = edge_block_size_t::normal_k;
};

struct edge_block_t;

/**
 *  @brief Reads one half-edge of an edge block at a time, joining its parallel arrays by index.
 *
 *  The arrays hold no half-edge to point at, so the reader builds one per index and the block it
 *  reads from must outlive it.
 */
class edge_block_reader {
  public:
    using value_type = labeled_half_edge_t;

    edge_block_reader() = default;

    /** A reader over @p block, which must outlive it. */
    edge_block_reader(edge_block_t const& block) noexcept : block_(&block) {}

    /** The half-edge at @p index, built from the block's parallel arrays. */
    labeled_half_edge_t operator[](std::size_t index) const;

  private:

    /** The edge block every index is read from, owned by whoever the reader came from. */
    edge_block_t const* block_ = nullptr;
};

/** Walks the half-edges of one edge block. */
using edge_block_iterator_t = indexed_iterator<edge_block_reader>;

/** A run of one edge block's half-edges, between two offsets. */
using edge_block_range_t = indexed_range<edge_block_reader>;

struct edge_block_t {

    using value_type = labeled_half_edge_t;
    using citerator_t = edge_block_iterator_t;
    using crange_t = edge_block_range_t;

    /** Half-edges below which an edge block asks to be merged with the one after it. */
    static constexpr std::size_t min_count_k = 16;

    /** Half-edges a split aims every new edge block at. */
    static constexpr std::size_t optimal_count_k = 128;

    /** Half-edges above which an edge block is split rather than grown. */
    static constexpr std::size_t max_count_k = 512;

    template <typename element_>
    using compressed = st::basic_vector<element_>;

#pragma region Members
    compressed<id_entity_t> neighbor_ids;
    compressed<id_entity_t> edge_ids;
    compressed<label_t> edge_labels;
    compressed<role_t> roles_in_half_edges;
    st::basic_vector<float> edge_weights;
#pragma endregion Members

#pragma region Interface Functions

    /** The bytes @ref save writes: the half-edge count, then the parallel arrays unless the edge block is empty. */
    std::size_t storage_requirements() const noexcept;
    status_t save(bytes_span_t destination) const noexcept;
    status_t load(bytes_view_t source) noexcept;
    operator edge_block_specs_t() const;

    labeled_half_edge_t at_offset(std::size_t) const;
    labeled_half_edge_t operator[](std::size_t offset) const { return at_offset(offset); }

    /** Finds the half-edge to @p neighbor_id with @p edge_label, returning its insert position and whether it
     *  exists. */
    position_t find(id_entity_t neighbor_id, label_t edge_label) const;

    /** Adds @p half_edge, or overwrites the one to its neighbor carrying its label, answering where it landed. */
    expected<position_t> upsert(labeled_half_edge_t const& half_edge) noexcept;

    position_t remove(labeled_half_edge_t const&);
    position_t remove_to(id_entity_t, label_t = {});

    citerator_t begin() const;
    citerator_t end() const;
    citerator_t begin_of(id_entity_t neighbor_id) const;
    citerator_t end_of(id_entity_t neighbor_id) const;

    crange_t relations_with(id_entity_t neighbor_id) const;

    bool correct_order() const;
    void clear();

    std::size_t size() const noexcept { return neighbor_ids.size(); }
    bool empty() const noexcept { return neighbor_ids.empty(); }

    /** The offset one past the last half-edge of a chunk of at most @p limit that starts at @p first. */
    std::size_t split_end(std::size_t first, std::size_t limit) const noexcept;

    /** Appends the half-edges of @p source between @p first and @p last, which order after every one held. */
    status_t take_from(edge_block_t const& source, std::size_t first, std::size_t last) noexcept;

#pragma endregion Interface Functions
};

inline labeled_half_edge_t edge_block_reader::operator[](std::size_t index) const { return block_->at_offset(index); }

static_assert(serializable_concept<edge_block_t>);

using chunk_len_t = std::uint16_t;
static_assert(std::numeric_limits<chunk_len_t>::max() > edge_block_t::max_count_k);

#pragma region Parsing

inline std::size_t edge_block_t::storage_requirements() const noexcept {
    if (empty()) return sizeof(chunk_len_t);
    return utilities::fields_storage_requirements(chunk_len_t {}, neighbor_ids, edge_ids, edge_labels,
                                                  roles_in_half_edges, edge_weights);
}

inline status_t edge_block_t::save(bytes_span_t destination) const noexcept {
    if (size() > std::numeric_limits<chunk_len_t>::max())
        return status_t {status_code_t::capacity_exhausted_k, "Data is too big"};
    chunk_len_t const length = static_cast<chunk_len_t>(size());
    if (!length) return utilities::save(length, destination);
    return utilities::save_fields(destination, length, neighbor_ids, edge_ids, edge_labels, roles_in_half_edges,
                                  edge_weights);
}

inline status_t edge_block_t::load(bytes_view_t source) noexcept {
    chunk_len_t length = 0;
    if (status_t const status = utilities::load(length, source); status.failed()) return status;
    if (!length) {
        clear();
        return status_t {};
    }
    return utilities::load_fields(source.subspan(sizeof(length)), neighbor_ids, edge_ids, edge_labels,
                                  roles_in_half_edges, edge_weights);
}

#pragma endregion Parsing

inline void edge_block_t::clear() {
    neighbor_ids.clear();
    edge_ids.clear();
    edge_labels.clear();
    roles_in_half_edges.clear();
    edge_weights.clear();
}

inline edge_block_t::citerator_t edge_block_t::begin() const { return citerator_t {*this, 0}; }

inline edge_block_t::citerator_t edge_block_t::end() const { return citerator_t {*this, size()}; }

inline edge_block_t::citerator_t edge_block_t::begin_of(id_entity_t neighbor_id) const {
    std::size_t const neighbor_offset = std::lower_bound(neighbor_ids.begin(), neighbor_ids.end(), neighbor_id) -
                                        neighbor_ids.begin();
    return citerator_t {*this, neighbor_offset};
}

inline edge_block_t::citerator_t edge_block_t::end_of(id_entity_t neighbor_id) const {
    std::size_t const next_offset = std::lower_bound(neighbor_ids.begin(), neighbor_ids.end(),
                                                     utilities::key_after(neighbor_id)) -
                                    neighbor_ids.begin();
    return citerator_t {*this, next_offset};
}

inline edge_block_t::crange_t edge_block_t::relations_with(id_entity_t neighbor_id) const {
    return crange_t {begin_of(neighbor_id), end_of(neighbor_id)};
}

inline position_t edge_block_t::find(id_entity_t neighbor_id, label_t edge_label) const {

    position_t position = find_in_sorted(neighbor_ids, neighbor_id);
    if (!position.exists) return position;

    // Look for a half-edge to the same neighbor with the same label.
    for (; position.offset < size(); position.offset++) {
        if (neighbor_ids[position.offset] > neighbor_id) return {position.offset, false};

        if (edge_labels[position.offset] > edge_label) return {position.offset, false};
        if (edge_labels[position.offset] == edge_label) return {position.offset, true};
    }
    return {position.offset, false};
}

inline expected<position_t> edge_block_t::upsert(labeled_half_edge_t const& half_edge) noexcept {

    position_t const position = find(half_edge.neighbor_id, half_edge.edge_label);
    if (position.exists) {
        neighbor_ids[position.offset] = half_edge.neighbor_id;
        edge_ids[position.offset] = half_edge.edge_id;
        edge_labels[position.offset] = half_edge.edge_label;
        roles_in_half_edges[position.offset] = half_edge.role;
        edge_weights[position.offset] = half_edge.edge_weight;
        return position;
    }

    // The five arrays are one half-edge split by column, so the room for all of it comes before any of it.
    std::size_t const wanted = size() + 1;
    if (neighbor_ids.reserve(wanted) != st::status_t::success_k ||
        edge_ids.reserve(wanted) != st::status_t::success_k || edge_labels.reserve(wanted) != st::status_t::success_k ||
        roles_in_half_edges.reserve(wanted) != st::status_t::success_k ||
        edge_weights.reserve(wanted) != st::status_t::success_k)
        return status_t {status_code_t::out_of_memory_k, "No room for another half-edge in an edge block"};

    [[maybe_unused]] st::status_t const neighbor = neighbor_ids.insert(st::assume_reserved, position.offset,
                                                                       id_entity_t {half_edge.neighbor_id});
    [[maybe_unused]] st::status_t const edge = edge_ids.insert(st::assume_reserved, position.offset,
                                                               id_entity_t {half_edge.edge_id});
    [[maybe_unused]] st::status_t const label = edge_labels.insert(st::assume_reserved, position.offset,
                                                                   label_t {half_edge.edge_label});
    [[maybe_unused]] st::status_t const role = roles_in_half_edges.insert(st::assume_reserved, position.offset,
                                                                          role_t {half_edge.role});
    [[maybe_unused]] st::status_t const weight = edge_weights.insert(st::assume_reserved, position.offset,
                                                                     float {half_edge.edge_weight});
    return position;
}

inline position_t edge_block_t::remove_to(id_entity_t neighbor_id, label_t edge_label) {

    position_t position = find(neighbor_id, edge_label);
    if (position.exists) {
        neighbor_ids.erase(position.offset);
        edge_ids.erase(position.offset);
        edge_labels.erase(position.offset);
        roles_in_half_edges.erase(position.offset);
        edge_weights.erase(position.offset);
    }
    return position;
}

inline position_t edge_block_t::remove(labeled_half_edge_t const& half_edge) {
    return remove_to(half_edge.neighbor_id, half_edge.edge_label);
}

inline edge_block_t::operator edge_block_specs_t() const {
    edge_block_specs_t result;
    std::size_t const count = neighbor_ids.size();
    if (count) {
        result.first_neighbor = neighbor_ids.front();
        result.last_neighbor = neighbor_ids.back();

        result.size = edge_block_size_t::normal_k;
        if (count < min_count_k) result.size = edge_block_size_t::too_small_k;
        else if (count > max_count_k) result.size = edge_block_size_t::too_big_k;
    }
    else { result.size = edge_block_size_t::empty_k; }
    return result;
}

inline bool edge_block_t::correct_order() const { return std::is_sorted(neighbor_ids.begin(), neighbor_ids.end()); }

inline labeled_half_edge_t edge_block_t::at_offset(std::size_t index) const {
    labeled_half_edge_t half_edge;
    half_edge.neighbor_id = neighbor_ids[index];
    half_edge.edge_id = edge_ids[index];
    half_edge.edge_label = edge_labels[index];
    half_edge.role = roles_in_half_edges[index];
    half_edge.edge_weight = edge_weights[index];
    return half_edge;
}

inline std::size_t edge_block_t::split_end(std::size_t first, std::size_t limit) const noexcept {
    std::size_t end = first + limit;
    if (end >= size()) return size();
    // Every half-edge to one neighbor belongs to one chunk, so the cut slides past the neighbor it landed on.
    while (end != size() && neighbor_ids[end] == neighbor_ids[end - 1]) ++end;
    return end;
}

inline status_t edge_block_t::take_from(edge_block_t const& source, std::size_t first, std::size_t last) noexcept {
    std::size_t const taken = size() + (last - first);
    if (neighbor_ids.reserve(taken) != st::status_t::success_k || edge_ids.reserve(taken) != st::status_t::success_k ||
        edge_labels.reserve(taken) != st::status_t::success_k ||
        roles_in_half_edges.reserve(taken) != st::status_t::success_k ||
        edge_weights.reserve(taken) != st::status_t::success_k)
        return status_t {status_code_t::out_of_memory_k, "No room for the half-edges of a chunk"};

    for (std::size_t offset = first; offset != last; ++offset) {
        [[maybe_unused]] st::status_t const neighbor = neighbor_ids.push_back(
            st::assume_reserved, id_entity_t {source.neighbor_ids[offset]});
        [[maybe_unused]] st::status_t const edge = edge_ids.push_back(st::assume_reserved,
                                                                      id_entity_t {source.edge_ids[offset]});
        [[maybe_unused]] st::status_t const label = edge_labels.push_back(st::assume_reserved,
                                                                          label_t {source.edge_labels[offset]});
        [[maybe_unused]] st::status_t const role = roles_in_half_edges.push_back(
            st::assume_reserved, role_t {source.roles_in_half_edges[offset]});
        [[maybe_unused]] st::status_t const weight = edge_weights.push_back(st::assume_reserved,
                                                                            float {source.edge_weights[offset]});
    }
    return {};
}

#pragma endregion Edge Block

} // namespace unum::ustore::graph

template <>
struct fmt::formatter<unum::ustore::graph::id_chunk_t> : unum::ustore::graph::id_chunk_formatter_t {};

template <>
struct fmt::formatter<unum::ustore::graph::labeled_edge_t> : unum::ustore::graph::labeled_edge_formatter_t {};
