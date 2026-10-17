/**
 *  @file include/ustore/graph/vertex_registry.hpp
 *  @author Ash Vardanian
 *  @date December 1, 2022
 *  @brief Per-vertex registry of the edge blocks holding its edges.
 */

#pragma once

#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint16_t`

#include <limits>   // `std::numeric_limits`
#include <optional> // `std::optional`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "ustore/graph/edge_block.hpp"    // `edge_block_size_t`, `edge_block_specs_t`, `id_entity_t`, `id_chunk_t`
#include "ustore/utilities/algorithm.hpp" // `position_t`, `find_in_sorted`
#include "ustore/utilities/nvme_ring.hpp" // `bytes_span_t`, `serializable_concept`
#include "ustore/utilities/types.hpp"     // `status_t`, `status_code_t`

namespace unum::ustore::graph {

/**
 *  @brief Registry of the edge blocks that belong to one vertex.
 *
 *  Splitting the half-edges of a vertex into edge blocks reduces the write amplification of small updates in big
 *  databases, and the registry lists those edge blocks as parallel arrays.
 */
struct vertex_registry_t {

    /** The array every column of this registry is held in, one entry per edge block. */
    template <typename element_>
    using compressed = st::basic_vector<element_>;

    /** The identity, the weight and the label of the vertex itself. */
    vertex_t vertex_info;

    /** The first neighbor of every edge block, ascending, which is also the suffix of that block's key. */
    compressed<id_entity_t> first_neighbors;

    /** The last neighbor of every edge block, in the order @c first_neighbors places them. */
    compressed<id_entity_t> last_neighbors;

    /** The size class of every edge block, which is what a lookup routes around a block that is too big. */
    compressed<edge_block_size_t> sizes;

    /** The bytes @ref save writes: the edge block count, the parallel arrays unless empty, then the vertex. */
    std::size_t storage_requirements() const noexcept;

    /** Writes the edge block count, the parallel arrays and the vertex into @p destination. */
    status_t save(bytes_span_t destination) const noexcept;

    /** Reads back what @ref save wrote, replacing whatever this registry held. */
    status_t load(bytes_view_t source) noexcept;

    /** Edge blocks this registry lists. */
    std::size_t size() const;

    /** Whether this registry lists no edge block, which is what a vertex with no edges holds. */
    bool empty() const;

    /** Forgets every edge block, keeping the vertex itself. */
    void clear();

    /** The bounds and the size class of the edge block at @p index. */
    edge_block_specs_t operator[](std::size_t index) const;

    /** The index of the edge block whose neighbors span the one @p chunk_id names. */
    std::optional<std::size_t> index_containing(id_chunk_t chunk_id) const;

    /** The index of the last edge block whose neighbors all order before the one @p chunk_id names. */
    std::optional<std::size_t> index_preceding(id_chunk_t chunk_id) const;

    /** The key of the edge block whose neighbors span the one @p chunk_id names. */
    std::optional<id_chunk_t> chunk_containing(id_chunk_t chunk_id) const;

    /** The key of the edge block a neighbor would join, which is none when every candidate is too big. */
    std::optional<id_chunk_t> chunk_existing_to_insert(id_chunk_t chunk_id) const;

    /** Picks a chunk through @c chunk_existing_to_insert, falling back to the requested one when none fits. */
    id_chunk_t chunk_to_insert(id_chunk_t chunk_id) const;

    /** Where the edge block @p chunk_id keys sits among the listed ones, or where it would be listed. */
    position_t find(id_chunk_t chunk_id) const;

    /** Registers @p specs under its own first neighbor, replacing the bounds of a chunk already listed there. */
    status_t upsert(edge_block_specs_t const& specs) noexcept;

    /** Forgets the chunk whose first neighbor @p chunk_id names, and answers where it was. */
    position_t erase(id_chunk_t chunk_id);
};

static_assert(serializable_concept<vertex_registry_t>);

inline position_t vertex_registry_t::find(id_chunk_t chunk_id) const {
    return find_in_sorted(first_neighbors, chunk_id.chunk);
}

inline std::optional<std::size_t> vertex_registry_t::index_containing(id_chunk_t chunk_id) const {
    position_t const at = find_in_sorted(first_neighbors, chunk_id.chunk);
    if (!at.exists && at.offset == 0) return std::nullopt;
    std::size_t const index = at.exists ? at.offset : at.offset - 1;
    if (chunk_id.chunk > last_neighbors[index]) return std::nullopt;
    return index;
}

inline std::optional<id_chunk_t> vertex_registry_t::chunk_containing(id_chunk_t chunk_id) const {
    if (std::optional<std::size_t> const index = index_containing(chunk_id); index)
        return id_chunk_t {chunk_id.entity, first_neighbors[*index]};
    return std::nullopt;
}

inline std::optional<std::size_t> vertex_registry_t::index_preceding(id_chunk_t chunk_id) const {
    position_t const at = find_in_sorted(last_neighbors, chunk_id.chunk);
    if (at.offset == 0) return std::nullopt;
    return at.offset - 1;
}

inline std::optional<id_chunk_t> vertex_registry_t::chunk_existing_to_insert(id_chunk_t chunk_id) const {
    if (first_neighbors.empty()) return std::nullopt;

    // Splitting the existing edge blocks is left to compaction.
    if (std::optional<std::size_t> const containing = index_containing(chunk_id); containing)
        return id_chunk_t {chunk_id.entity, first_neighbors[*containing]};

    // Otherwise append to the preceding chunk unless it is already too big.
    if (std::optional<std::size_t> const preceding = index_preceding(chunk_id); preceding)
        if (sizes[*preceding] != edge_block_size_t::too_big_k)
            return id_chunk_t {chunk_id.entity, first_neighbors[*preceding]};
    return std::nullopt;
}

inline id_chunk_t vertex_registry_t::chunk_to_insert(id_chunk_t chunk_id) const {
    if (std::optional<id_chunk_t> const existing = chunk_existing_to_insert(chunk_id); existing) return *existing;
    return chunk_id;
}

inline std::size_t vertex_registry_t::storage_requirements() const noexcept {
    std::size_t const arrays = empty() ? 0
                                       : utilities::fields_storage_requirements(first_neighbors, last_neighbors, sizes);
    return sizeof(std::uint16_t) + arrays + utilities::storage_requirements(vertex_info);
}

inline status_t vertex_registry_t::save(bytes_span_t destination) const noexcept {
    if (size() > std::numeric_limits<std::uint16_t>::max())
        return status_t {status_code_t::capacity_exhausted_k, "Data is too big"};
    std::uint16_t const length = static_cast<std::uint16_t>(size());
    if (!length) return utilities::save_fields(destination, length, vertex_info);
    return utilities::save_fields(destination, length, first_neighbors, last_neighbors, sizes, vertex_info);
}

inline status_t vertex_registry_t::load(bytes_view_t source) noexcept {
    std::uint16_t length = 0;
    if (status_t const status = utilities::load(length, source); status.failed()) return status;
    if (length)
        return utilities::load_fields(source.subspan(sizeof(length)), first_neighbors, last_neighbors, sizes,
                                      vertex_info);
    first_neighbors.clear();
    last_neighbors.clear();
    sizes.clear();
    return utilities::load(vertex_info, source.subspan(sizeof(length)));
}

inline edge_block_specs_t vertex_registry_t::operator[](std::size_t index) const {
    edge_block_specs_t result;
    result.first_neighbor = first_neighbors[index];
    result.last_neighbor = last_neighbors[index];
    result.size = sizes[index];
    return result;
}

inline status_t vertex_registry_t::upsert(edge_block_specs_t const& specs) noexcept {
    position_t const position = find_in_sorted(first_neighbors, specs.first_neighbor);
    if (position.exists) {
        last_neighbors[position.offset] = specs.last_neighbor;
        sizes[position.offset] = specs.size;
        return {};
    }

    // The three arrays are one record split by column, so the room for all of it comes before any of it.
    std::size_t const wanted = first_neighbors.size() + 1;
    if (first_neighbors.reserve(wanted) != st::status_t::success_k ||
        last_neighbors.reserve(wanted) != st::status_t::success_k || sizes.reserve(wanted) != st::status_t::success_k)
        return {status_code_t::out_of_memory_k, "No room for another edge block in a registry"};

    [[maybe_unused]] st::status_t const first = first_neighbors.insert(st::assume_reserved, position.offset,
                                                                       id_entity_t {specs.first_neighbor});
    [[maybe_unused]] st::status_t const last = last_neighbors.insert(st::assume_reserved, position.offset,
                                                                     id_entity_t {specs.last_neighbor});
    [[maybe_unused]] st::status_t const size = sizes.insert(st::assume_reserved, position.offset,
                                                            edge_block_size_t {specs.size});
    return {};
}

inline position_t vertex_registry_t::erase(id_chunk_t chunk_id) {
    position_t const position = find(chunk_id);
    if (position.exists) {
        first_neighbors.erase(position.offset);
        last_neighbors.erase(position.offset);
        sizes.erase(position.offset);
    }
    return position;
}

inline std::size_t vertex_registry_t::size() const { return first_neighbors.size(); }

inline bool vertex_registry_t::empty() const { return first_neighbors.empty(); }

inline void vertex_registry_t::clear() {
    first_neighbors.clear();
    last_neighbors.clear();
    sizes.clear();
}

} // namespace unum::ustore::graph
