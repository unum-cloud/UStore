/**
 * @file modality_graph.cpp
 * @author Ashot Vardanian
 *
 * @brief Graph implementation using fast integer compression.
 * Sits on top of any see "ustore.h"-compatible system.
 *
 * For every vertex this implementation stores:
 * - inbound degree
 * - output degree
 * - inbound neighborships: neighbor ID + edge ID
 * - outbound neighborships: neighbor ID + edge ID
 */

#include <numeric>  // `std::accumulate`
#include <optional> // `std::optional`
#include <limits>   // `std::numeric_limits`

#include "ustore/ustore.hpp"
#include "helpers/linked_memory.hpp" // `linked_memory_lock_t`
#include "helpers/algorithm.hpp"     // `equal_subrange`

/*********************************************************/
/*****************	 C++ Implementation	  ****************/
/*********************************************************/

using namespace unum::ustore;
using namespace unum;

ustore_key_t ustore_default_edge_id_k = std::numeric_limits<ustore_key_t>::max();
ustore_vertex_degree_t ustore_vertex_degree_missing_k = std::numeric_limits<ustore_vertex_degree_t>::max();

constexpr std::size_t bytes_in_degrees_header_k = 2 * sizeof(ustore_vertex_degree_t);

struct updated_entry_t : public collection_key_t {
    ustore_bytes_ptr_t content = nullptr;
    ustore_length_t length = ustore_length_missing_k;
    ustore_vertex_degree_t degree_delta = 0;
    inline operator value_view_t() const noexcept { return {content, length}; }
};

ptr_range_gt<neighborship_t const> neighbors( //
    ustore_vertex_degree_t const* degrees,
    ustore_key_t const* neighborships,
    ustore_vertex_role_t role = ustore_vertex_role_any_k) {
    auto ships = reinterpret_cast<neighborship_t const*>(neighborships);

    switch (role) {
    case ustore_vertex_source_k: return {ships, ships + degrees[0]};
    case ustore_vertex_target_k: return {ships + degrees[0], ships + degrees[0] + degrees[1]};
    case ustore_vertex_role_any_k: return {ships, ships + degrees[0] + degrees[1]};
    case ustore_vertex_role_unknown_k: return {};
    }
    __builtin_unreachable();
}

ptr_range_gt<neighborship_t const> neighbors(value_view_t bytes, ustore_vertex_role_t role = ustore_vertex_role_any_k) {
    // Handle missing vertices
    if (bytes.size() < bytes_in_degrees_header_k)
        return {};

    auto degrees = reinterpret_cast<ustore_vertex_degree_t const*>(bytes.begin());
    return neighbors(degrees, reinterpret_cast<ustore_key_t const*>(degrees + 2), role);
}

struct neighborhood_t {
    ustore_key_t center = 0;
    ptr_range_gt<neighborship_t const> targets;
    ptr_range_gt<neighborship_t const> sources;

    neighborhood_t() = default;
    neighborhood_t(neighborhood_t const&) = default;
    neighborhood_t(neighborhood_t&&) = default;

    /**
     * @brief Parses the a single `value_view_t` chunk
     * from the output of `ustore_graph_find_edges()`.
     */
    inline neighborhood_t(ustore_key_t center_vertex, value_view_t bytes) noexcept {
        center = center_vertex;
        targets = neighbors(bytes, ustore_vertex_source_k);
        sources = neighbors(bytes, ustore_vertex_target_k);
    }

    inline neighborhood_t(ustore_key_t center_vertex,
                          ustore_vertex_degree_t const* degrees,
                          ustore_key_t const* neighborships) noexcept {
        center = center_vertex;
        targets = neighbors(degrees, neighborships, ustore_vertex_source_k);
        sources = neighbors(degrees, neighborships, ustore_vertex_target_k);
    }

    inline std::size_t size() const noexcept { return targets.size() + sources.size(); }

    inline edge_t operator[](std::size_t i) const noexcept {
        edge_t result;
        if (i > targets.size()) {
            i -= targets.size();
            result.source_id = center;
            result.target_id = targets[i].neighbor_id;
            result.id = targets[i].edge_id;
        }
        else {
            result.source_id = sources[i].neighbor_id;
            result.target_id = center;
            result.id = sources[i].edge_id;
        }
        return result;
    }

    inline edges_view_t outgoing_edges() const& {
        edges_view_t edges;
        edges.source_ids = {{&center, 0}, targets.size()};
        edges.target_ids = targets.strided().members(&neighborship_t::neighbor_id);
        edges.edge_ids = targets.strided().members(&neighborship_t::edge_id);
        return edges;
    }

    inline edges_view_t incoming_edges() const& {
        edges_view_t edges;
        edges.source_ids = sources.strided().members(&neighborship_t::neighbor_id);
        edges.target_ids = {{&center, 0}, sources.size()};
        edges.edge_ids = sources.strided().members(&neighborship_t::edge_id);
        return edges;
    }

    inline ptr_range_gt<neighborship_t const> outgoing_to(ustore_key_t target) const noexcept {
        return equal_subrange(targets, target);
    }

    inline ptr_range_gt<neighborship_t const> incoming_from(ustore_key_t source) const noexcept {
        return equal_subrange(sources, source);
    }

    inline neighborship_t const* outgoing_to(ustore_key_t target, ustore_key_t edge_id) const noexcept {
        auto r = equal_subrange(targets, neighborship_t {target, edge_id});
        return r.size() ? r.begin() : nullptr;
    }

    inline neighborship_t const* incoming_from(ustore_key_t source, ustore_key_t edge_id) const noexcept {
        auto r = equal_subrange(sources, neighborship_t {source, edge_id});
        return r.size() ? r.begin() : nullptr;
    }

    inline ptr_range_gt<neighborship_t const> only(ustore_vertex_role_t role) const noexcept {
        switch (role) {
        case ustore_vertex_source_k: return targets;
        case ustore_vertex_target_k: return sources;
        default: return {};
        }
    }

    /**
     * @return true  If the node is present in the graph.
     *               The neighborhood may be empty.
     */
    inline explicit operator bool() const noexcept { return sources && targets; }
};

struct neighborhoods_iterator_t {
    strided_iterator_gt<ustore_key_t const> centers_;
    ustore_vertex_degree_t const* degrees_per_vertex_ = nullptr;
    ustore_key_t const* neighborships_per_vertex_ = nullptr;

    neighborhoods_iterator_t(strided_iterator_gt<ustore_key_t const> centers,
                             ustore_vertex_degree_t const* degrees_per_vertex,
                             ustore_key_t const* neighborships_per_vertex) noexcept
        : centers_(centers), degrees_per_vertex_(degrees_per_vertex),
          neighborships_per_vertex_(neighborships_per_vertex) {}

    inline neighborhood_t operator*() const noexcept {
        return {*centers_, degrees_per_vertex_, neighborships_per_vertex_};
    }
    inline neighborhoods_iterator_t operator++(int) const noexcept {
        return {
            centers_++,
            degrees_per_vertex_ + 2u,
            neighborships_per_vertex_ + (degrees_per_vertex_[0] + degrees_per_vertex_[1]) * 2u,
        };
    }

    inline neighborhoods_iterator_t& operator++() noexcept {
        ++centers_;
        neighborships_per_vertex_ += (degrees_per_vertex_[0] + degrees_per_vertex_[1]) * 2u;
        degrees_per_vertex_ += 2u;
        return *this;
    }

    inline bool operator==(neighborhoods_iterator_t const& other) const noexcept { return centers_ == other.centers_; }
    inline bool operator!=(neighborhoods_iterator_t const& other) const noexcept { return centers_ != other.centers_; }
};

struct neighborhoods_t {
    strided_range_gt<ustore_key_t const> centers_;
    ustore_vertex_degree_t const* degrees_per_vertex_ = nullptr;
    ustore_key_t const* neighborships_per_vertex_ = nullptr;

    neighborhoods_t(strided_range_gt<ustore_key_t const> centers,
                    ustore_vertex_degree_t const* degrees_per_vertex,
                    ustore_key_t const* neighborships_per_vertex) noexcept
        : centers_(centers), degrees_per_vertex_(degrees_per_vertex),
          neighborships_per_vertex_(neighborships_per_vertex) {}

    inline neighborhoods_iterator_t begin() const noexcept {
        return {centers_.begin(), degrees_per_vertex_, neighborships_per_vertex_};
    }
    inline neighborhoods_iterator_t end() const noexcept {
        return {centers_.end(), degrees_per_vertex_ + centers_.size() * 2u, nullptr};
    }
    inline std::size_t size() const noexcept { return centers_.size(); }
};

void count_inserts_into_entry( //
    updated_entry_t& entry,
    ustore_vertex_role_t role,
    ustore_key_t neighbor_id,
    ustore_key_t edge_id) {

    auto ship = neighborship_t {neighbor_id, edge_id};
    if (entry.length > bytes_in_degrees_header_k) {
        auto neighbors_range = neighbors(entry, role);
        auto it = std::lower_bound(neighbors_range.begin(), neighbors_range.end(), ship);
        if (it != neighbors_range.end())
            if (*it == ship)
                return;
    }

    ++entry.degree_delta;
}

/**
 * @return true  If such an entry didn't exist and was added.
 * @return false In every other case.
 */
void insert_into_entry( //
    updated_entry_t& entry,
    ustore_vertex_role_t role,
    ustore_key_t neighbor_id,
    ustore_key_t edge_id) {

    auto ship = neighborship_t {neighbor_id, edge_id};
    auto degrees = reinterpret_cast<ustore_vertex_degree_t*>(entry.content);
    auto ships = reinterpret_cast<neighborship_t*>(degrees + 2);
    if (entry.length < bytes_in_degrees_header_k || entry.length == ustore_length_missing_k) {
        degrees[role != ustore_vertex_target_k] = 0;
        degrees[role == ustore_vertex_target_k] = 1;
        ships[0] = ship;
        entry.length += bytes_in_degrees_header_k;
        entry.length += sizeof(neighborship_t);
    }
    else {
        auto neighbors_range = neighbors(entry, role);
        auto it = std::lower_bound(neighbors_range.begin(), neighbors_range.end(), ship);
        if (it != neighbors_range.end())
            if (*it == ship)
                return;

        trivial_insert(ships, degrees[0] + degrees[1], it - ships, &ship, &ship + 1);
        degrees[role == ustore_vertex_target_k] += 1;
        entry.length += sizeof(neighborship_t);
    }
}

/**
 * @return true  If a matching entry was found and deleted.
 * @return false In every other case.
 */
void erase_from_entry(updated_entry_t& entry,
                      ustore_vertex_role_t role,
                      ustore_key_t neighbor_id,
                      std::optional<ustore_key_t> edge_id = {}) {

    if (entry.length < bytes_in_degrees_header_k || entry.length == ustore_length_missing_k)
        return;

    std::size_t off = 0;
    std::size_t len = 0;

    auto degrees = reinterpret_cast<ustore_vertex_degree_t*>(entry.content);
    auto ships = reinterpret_cast<neighborship_t*>(degrees + 2);
    auto neighbors_range = neighbors(entry, role);
    if (edge_id) {
        auto ship = neighborship_t {neighbor_id, *edge_id};
        auto it = std::lower_bound(neighbors_range.begin(), neighbors_range.end(), ship);
        if (it == neighbors_range.end() || *it != ship)
            return;

        off = it - ships;
        len = 1;
    }
    else {
        auto pair = std::equal_range(neighbors_range.begin(), neighbors_range.end(), neighbor_id);
        if (pair.first == neighbors_range.end() || pair.first == pair.second)
            return;

        off = pair.first - ships;
        len = pair.second - pair.first;
    }

    trivial_erase(ships, degrees[0] + degrees[1], off, len);
    degrees[role == ustore_vertex_target_k] -= len;
    entry.degree_delta += len;
    entry.length -= sizeof(neighborship_t) * len;
}

template <bool export_center_ak = true, bool export_neighbor_ak = true, bool export_edge_ak = true>
void export_edge_tuples( //
    ustore_database_t const c_db,
    ustore_transaction_t const c_transaction,
    ustore_snapshot_t const c_snapshot,
    ustore_size_t const c_vertices_count,

    ustore_collection_t const* c_collections,
    ustore_size_t const c_collections_stride,

    ustore_key_t const* c_vertices,
    ustore_size_t const c_vertices_stride,

    ustore_vertex_role_t const* c_roles,
    ustore_size_t const c_roles_stride,

    ustore_options_t const c_options,

    ustore_vertex_degree_t** c_degrees_per_vertex,
    ustore_key_t** c_neighborships_per_vertex,

    linked_memory_lock_t& arena,
    ustore_error_t* c_error) {

    // Even if we need just the node degrees, we can't limit ourselves to just entry lengths.
    // Those may be compressed. We need to read the first bytes to parse the degree of the node.
    ustore_bytes_ptr_t c_found_values {};
    ustore_length_t* c_found_offsets {};
    ustore_read_t read {};
    read.db = c_db;
    read.error = c_error;
    read.transaction = c_transaction;
    read.snapshot = c_snapshot;
    read.arena = arena;
    read.options = c_options;
    read.tasks_count = c_vertices_count;
    read.collections = c_collections;
    read.collections_stride = c_collections_stride;
    read.keys = c_vertices;
    read.keys_stride = c_vertices_stride;
    read.offsets = &c_found_offsets;
    read.values = &c_found_values;

    ustore_read(&read);
    return_if_error_m(c_error);

    joined_blobs_t values {c_vertices_count, c_found_offsets, c_found_values};
    strided_iterator_gt<ustore_collection_t const> collections {c_collections, c_collections_stride};
    strided_range_gt<ustore_key_t const> vertices {{c_vertices, c_vertices_stride}, c_vertices_count};
    strided_iterator_gt<ustore_vertex_role_t const> roles {c_roles, c_roles_stride};
    constexpr std::size_t tuple_size_k = export_center_ak + export_neighbor_ak + export_edge_ak;

    find_edges_t find_edges {collections, vertices.begin(), roles, c_vertices_count};

    // Estimate the amount of memory we will need for the arena
    std::size_t count_ids = 0;
    if constexpr (tuple_size_k != 0) {
        joined_blobs_iterator_t values_it = values.begin();
        for (ustore_size_t i = 0; i != c_vertices_count; ++i, ++values_it) {
            value_view_t value = *values_it;
            count_ids += neighbors(value, find_edges[i].role).size();
        }
        count_ids *= tuple_size_k;
    }

    // Export into arena
    auto ids = arena.alloc_or_dummy(count_ids, c_error, c_neighborships_per_vertex);
    return_if_error_m(c_error);
    auto degrees = arena.alloc_or_dummy(c_vertices_count, c_error, c_degrees_per_vertex);
    return_if_error_m(c_error);

    std::size_t passed_ids = 0;
    joined_blobs_iterator_t values_it = values.begin();
    for (std::size_t i = 0; i != c_vertices_count; ++i, ++values_it) {
        value_view_t value = *values_it;
        find_edge_t find_edge = find_edges[i];

        // Some values may be missing
        if (!value) {
            degrees[i] = ustore_vertex_degree_missing_k;
            continue;
        }

        ustore_vertex_degree_t degree = 0;
        if (find_edge.role & ustore_vertex_source_k) {
            auto ns = neighbors(value, ustore_vertex_source_k);
            if constexpr (tuple_size_k != 0)
                for (neighborship_t n : ns) {
                    if constexpr (export_center_ak)
                        ids[passed_ids + 0] = find_edge.vertex_id;
                    if constexpr (export_neighbor_ak)
                        ids[passed_ids + export_center_ak] = n.neighbor_id;
                    if constexpr (export_edge_ak)
                        ids[passed_ids + export_center_ak + export_neighbor_ak] = n.edge_id;
                    passed_ids += tuple_size_k;
                }
            degree += static_cast<ustore_vertex_degree_t>(ns.size());
        }
        if (find_edge.role & ustore_vertex_target_k) {
            auto ns = neighbors(value, ustore_vertex_target_k);
            if constexpr (tuple_size_k != 0)
                for (neighborship_t n : ns) {
                    if constexpr (export_neighbor_ak)
                        ids[passed_ids + 0] = n.neighbor_id;
                    if constexpr (export_center_ak)
                        ids[passed_ids + export_neighbor_ak] = find_edge.vertex_id;
                    if constexpr (export_edge_ak)
                        ids[passed_ids + export_center_ak + export_neighbor_ak] = n.edge_id;
                    passed_ids += tuple_size_k;
                }
            degree += static_cast<ustore_vertex_degree_t>(ns.size());
        }
        degrees[i] = degree;
    }
}

void pull_and_link_for_updates( //
    ustore_database_t const c_db,
    ustore_transaction_t const c_transaction,
    strided_range_gt<updated_entry_t> unique_entries,
    ustore_options_t const c_options,
    linked_memory_lock_t& arena,
    ustore_error_t* c_error) {

    // Fetch the existing entries
    ustore_bytes_ptr_t found_binary_begin = nullptr;
    ustore_length_t* found_binary_offs = nullptr;
    ustore_size_t unique_count = static_cast<ustore_size_t>(unique_entries.size());
    auto collections = unique_entries.immutable().members(&updated_entry_t::collection);
    auto keys = unique_entries.immutable().members(&updated_entry_t::key);
    auto opts = c_transaction ? ustore_options_t(c_options & ~ustore_option_transaction_dont_watch_k) : c_options;
    ustore_read_t read {};
    read.db = c_db;
    read.error = c_error;
    read.transaction = c_transaction;
    read.arena = arena;
    read.options = opts;
    read.tasks_count = unique_count;
    read.collections = collections.begin().get();
    read.collections_stride = collections.begin().stride();
    read.keys = keys.begin().get();
    read.keys_stride = keys.begin().stride();
    read.offsets = &found_binary_offs;
    read.values = &found_binary_begin;

    ustore_read(&read);
    return_if_error_m(c_error);

    // Link the response buffer to `unique_entries`
    joined_blobs_t found_binaries {unique_count, found_binary_offs, found_binary_begin};
    for (std::size_t i = 0; i != unique_count; ++i) {
        auto found_binary = found_binaries[i];
        unique_entries[i].content = ustore_bytes_ptr_t(found_binary.data());
        unique_entries[i].length =
            found_binary ? static_cast<ustore_length_t>(found_binary.size()) : ustore_length_missing_k;
    }
}

template <bool erase_ak>
void update_neighborhoods( //
    ustore_database_t const c_db,
    ustore_transaction_t const c_transaction,
    ustore_size_t const c_tasks_count,

    ustore_collection_t const* c_collections,
    ustore_size_t const c_collections_stride,

    ustore_key_t const* c_edges_ids,
    ustore_size_t const c_edges_stride,

    ustore_key_t const* c_sources_ids,
    ustore_size_t const c_sources_stride,

    ustore_key_t const* c_targets_ids,
    ustore_size_t const c_targets_stride,

    ustore_options_t const c_options,

    linked_memory_lock_t& arena,
    ustore_error_t* c_error) {

    strided_iterator_gt<ustore_collection_t const> edge_collections {c_collections, c_collections_stride};
    strided_iterator_gt<ustore_key_t const> edges_ids {c_edges_ids, c_edges_stride};
    strided_iterator_gt<ustore_key_t const> sources_ids {c_sources_ids, c_sources_stride};
    strided_iterator_gt<ustore_key_t const> targets_ids {c_targets_ids, c_targets_stride};

    // Fetch all the data related to touched vertices, and deduplicate them
    auto unique_entries = arena.alloc<updated_entry_t>(c_tasks_count * 2, c_error);
    return_if_error_m(c_error);
    std::fill(unique_entries.begin(), unique_entries.end(), updated_entry_t {});
    for (ustore_size_t i = 0; i != c_tasks_count; ++i)
        unique_entries[i].collection = edge_collections[i], unique_entries[i].key = sources_ids[i];
    for (ustore_size_t i = 0; i != c_tasks_count; ++i)
        unique_entries[c_tasks_count + i].collection = edge_collections[i],
                                       unique_entries[c_tasks_count + i].key = targets_ids[i];

    // Lets put all the unique IDs in the beginning of the range,
    // and then refill the tail with replicas
    auto unique_count = sort_and_deduplicate(unique_entries.begin(), unique_entries.end());
    unique_entries = {unique_entries.begin(), unique_count};

    // Fetch the existing entries
    auto unique_strided = unique_entries.strided();
    pull_and_link_for_updates(c_db, c_transaction, unique_strided, c_options, arena, c_error);
    return_if_error_m(c_error);

    // Define our primary for-loop
    auto for_each_task = [&](auto entry_role_target_edge_callback) {
        for (std::size_t i = 0; i != c_tasks_count; ++i) {
            auto collection = edge_collections[i];
            auto source_id = sources_ids[i];
            auto target_id = targets_ids[i];
            auto edge_id = edges_ids ? edges_ids[i] : ustore_key_unknown_k;
            auto source_idx = offset_in_sorted(unique_entries, collection_key_t {collection, source_id});
            auto target_idx = offset_in_sorted(unique_entries, collection_key_t {collection, target_id});
            entry_role_target_edge_callback(unique_entries[source_idx], ustore_vertex_source_k, target_id, edge_id);
            entry_role_target_edge_callback(unique_entries[target_idx], ustore_vertex_target_k, source_id, edge_id);
        }
    };

    if constexpr (erase_ak)
        for_each_task(&erase_from_entry);
    else {
        // Unlike erasing, which can reuse the memory, her we need three passes:
        // 1. estimating final size
        for_each_task(&count_inserts_into_entry);
        // 2. reallocating into bigger buffers
        for (std::size_t i = 0; i != unique_count; ++i) {
            auto& unique_entry = unique_entries[i];
            auto bytes_present = unique_entry.length != ustore_length_missing_k ? unique_entry.length : 0;
            auto bytes_for_relations = unique_entry.degree_delta * sizeof(neighborship_t);
            auto bytes_for_degrees = bytes_present > bytes_in_degrees_header_k ? 0 : bytes_in_degrees_header_k;
            auto new_size = bytes_present + bytes_for_relations + bytes_for_degrees;
            auto new_buffer = arena.alloc<byte_t>(new_size, c_error);
            return_if_error_m(c_error);
            std::memcpy(new_buffer.begin(), unique_entry.content, bytes_present);

            unique_entry.content = (ustore_bytes_ptr_t)new_buffer.begin();
            // No need to grow `length` here, we will update in `insert_into_entry` later
            unique_entry.length = bytes_present;
        }
        // 3. performing insertions
        for_each_task(&insert_into_entry);
    }

    // Some of the requested updates may have been completely useless, like:
    // > upserting an existing relation.
    // > removing a missing relation.
    // So we can further optimize by cancelling those writes.
    std::partition(unique_entries.begin(), unique_entries.end(), std::mem_fn(&updated_entry_t::degree_delta));

    // Dump the data back to disk!
    auto collections = unique_strided.immutable().members(&updated_entry_t::collection);
    auto keys = unique_strided.immutable().members(&updated_entry_t::key);
    auto contents = unique_strided.immutable().members(&updated_entry_t::content);
    auto lengths = unique_strided.immutable().members(&updated_entry_t::length);

    ustore_write_t write {};
    write.db = c_db;
    write.error = c_error;
    write.transaction = c_transaction;
    write.arena = arena;
    write.options = c_options;
    write.tasks_count = unique_count;
    write.collections = collections.begin().get();
    write.collections_stride = collections.begin().stride();
    write.keys = keys.begin().get();
    write.keys_stride = keys.begin().stride();
    write.lengths = lengths.begin().get();
    write.lengths_stride = lengths.begin().stride();
    write.values = contents.begin().get();
    write.values_stride = contents.begin().stride();

    ustore_write(&write);
}

void ustore_graph_find_edges(ustore_graph_find_edges_t* c_ptr) {

    ustore_graph_find_edges_t& c = *c_ptr;
    if (!c.tasks_count)
        return;

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    bool only_degrees = !c.edges_per_vertex;
    auto func = only_degrees //
                    ? &export_edge_tuples<false, false, false>
                    : &export_edge_tuples<true, true, true>;
    return func( //
        c.db,
        c.transaction,
        c.snapshot,
        c.tasks_count,
        c.collections,
        c.collections_stride,
        c.vertices,
        c.vertices_stride,
        c.roles,
        c.roles_stride,
        c.options,
        c.degrees_per_vertex,
        c.edges_per_vertex,
        arena,
        c.error);
}

void ustore_graph_upsert_edges(ustore_graph_upsert_edges_t* c_ptr) {

    ustore_graph_upsert_edges_t& c = *c_ptr;
    if (!c.tasks_count)
        return;

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    return update_neighborhoods<false>( //
        c.db,
        c.transaction,
        c.tasks_count,
        c.collections,
        c.collections_stride,
        c.edges_ids,
        c.edges_stride,
        c.sources_ids,
        c.sources_stride,
        c.targets_ids,
        c.targets_stride,
        c.options,
        arena,
        c.error);
}

void ustore_graph_remove_edges(ustore_graph_remove_edges_t* c_ptr) {

    ustore_graph_remove_edges_t& c = *c_ptr;
    if (!c.tasks_count)
        return;

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    return update_neighborhoods<true>( //
        c.db,
        c.transaction,
        c.tasks_count,
        c.collections,
        c.collections_stride,
        c.edges_ids,
        c.edges_stride,
        c.sources_ids,
        c.sources_stride,
        c.targets_ids,
        c.targets_stride,
        c.options,
        arena,
        c.error);
}

void ustore_graph_upsert_vertices(ustore_graph_upsert_vertices_t* c_ptr) {

    ustore_graph_upsert_vertices_t& c = *c_ptr;
    if (!c.tasks_count)
        return;

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    ustore_length_t* c_found_lengths {};
    ustore_read_t read {};
    read.db = c.db;
    read.error = c.error;
    read.transaction = c.transaction;
    read.arena = arena;
    read.options = c.options;
    read.tasks_count = c.tasks_count;
    read.collections = c.collections;
    read.collections_stride = c.collections_stride;
    read.keys = c.vertices;
    read.keys_stride = c.vertices_stride;
    read.lengths = &c_found_lengths;

    ustore_read(&read);
    return_if_error_m(c.error);

    std::size_t idx = 0;
    auto vertices_to_upsert = arena.alloc<ustore_key_t>(c.tasks_count, c.error);
    strided_range_gt<ustore_key_t const> vertices {{c.vertices, c.vertices_stride}, c.tasks_count};
    for (std::size_t i = 0; i != c.tasks_count; ++i) {
        if (c_found_lengths[i] == ustore_length_missing_k) {
            vertices_to_upsert[idx] = vertices[i];
            ++idx;
        }
    }

    ustore_length_t length {};
    value_view_t empty_value {""};
    ustore_write_t write {};
    write.db = c.db;
    write.error = c.error;
    write.transaction = c.transaction;
    write.arena = arena;
    write.tasks_count = idx;
    write.collections = c.collections;
    write.collections_stride = c.collections_stride;
    write.keys = vertices_to_upsert.begin();
    write.keys_stride = sizeof(ustore_key_t);
    write.lengths = &length;
    write.values = empty_value.member_ptr();

    ustore_write(&write);
}

void ustore_graph_remove_vertices(ustore_graph_remove_vertices_t* c_ptr) {

    ustore_graph_remove_vertices_t& c = *c_ptr;
    if (!c.tasks_count)
        return;

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    strided_iterator_gt<ustore_collection_t const> vertex_collections {c.collections, c.collections_stride};
    strided_range_gt<ustore_key_t const> vertices {{c.vertices, c.vertices_stride}, c.tasks_count};
    strided_iterator_gt<ustore_vertex_role_t const> vertex_roles {c.roles, c.roles_stride};

    // Initially, just retrieve the bare minimum information about the vertices
    ustore_vertex_degree_t* degrees_per_vertex = nullptr;
    ustore_key_t* neighbors_per_vertex = nullptr;
    export_edge_tuples<false, true, false>( //
        c.db,
        c.transaction,
        0,
        c.tasks_count,
        c.collections,
        c.collections_stride,
        c.vertices,
        c.vertices_stride,
        c.roles,
        c.roles_stride,
        c.options,
        &degrees_per_vertex,
        &neighbors_per_vertex,
        arena,
        c.error);
    return_if_error_m(c.error);

    // Enumerate the opposite ends, from which that same reference must be removed.
    // Here all the keys will be in the sorted order.
    auto unique_count = std::accumulate(degrees_per_vertex, degrees_per_vertex + c.tasks_count, c.tasks_count);
    auto unique_entries = arena.alloc<updated_entry_t>(unique_count, c.error);
    return_if_error_m(c.error);
    std::fill(unique_entries.begin(), unique_entries.end(), updated_entry_t {});

    // Sorting the tasks would help us faster locate them in the future.
    // We may also face repetitions when connected vertices are removed.
    {
        auto planned_entries = unique_entries.begin();
        for (std::size_t i = 0; i != c.tasks_count; ++i) {
            auto collection = planned_entries->collection = vertex_collections[i];
            planned_entries->key = vertices[i];
            ++planned_entries;
            for (std::size_t j = 0; j != degrees_per_vertex[i]; ++j, ++neighbors_per_vertex, ++planned_entries)
                planned_entries->collection = collection, planned_entries->key = *neighbors_per_vertex;
        }
        unique_count = sort_and_deduplicate(unique_entries.begin(), planned_entries);
        unique_entries = {unique_entries.begin(), unique_count};
    }

    // Fetch the opposite ends, from which that same reference must be removed.
    // Here all the keys will be in the sorted order.
    auto unique_strided = unique_entries.strided();
    pull_and_link_for_updates(c.db, c.transaction, unique_strided, c.options, arena, c.error);
    return_if_error_m(c.error);

    // From every opposite end - remove a match, and only then - the content itself
    for (std::size_t i = 0; i != unique_strided.size(); ++i) {
        auto vertex_collection = vertex_collections[i];
        auto vertex_id = vertices[i];
        auto vertex_role = vertex_roles ? vertex_roles[i] : ustore_vertex_role_any_k;

        auto vertex_idx = offset_in_sorted(unique_entries, collection_key_t {vertex_collection, vertex_id});
        updated_entry_t& vertex_value = unique_entries[vertex_idx];

        for (neighborship_t n : neighbors(vertex_value, vertex_role)) {
            auto neighbor_idx = offset_in_sorted(unique_entries, collection_key_t {vertex_collection, n.neighbor_id});
            updated_entry_t& neighbor_value = unique_entries[neighbor_idx];
            if (vertex_role == ustore_vertex_role_any_k) {
                erase_from_entry(neighbor_value, ustore_vertex_source_k, vertex_id);
                erase_from_entry(neighbor_value, ustore_vertex_target_k, vertex_id);
            }
            else
                erase_from_entry(neighbor_value, invert(vertex_role), vertex_id);
        }

        vertex_value.content = nullptr;
        vertex_value.length = ustore_length_missing_k;
    }

    // Now we will go through all the explicitly deleted vertices
    auto collections = unique_strided.immutable().members(&updated_entry_t::collection);
    auto keys = unique_strided.immutable().members(&updated_entry_t::key);
    auto lengths = unique_strided.immutable().members(&updated_entry_t::length);
    auto contents = unique_strided.immutable().members(&updated_entry_t::content);

    ustore_write_t write {};
    write.db = c.db;
    write.error = c.error;
    write.transaction = c.transaction;
    write.arena = arena;
    write.options = c.options;
    write.tasks_count = unique_count;
    write.collections = collections.begin().get();
    write.collections_stride = collections.begin().stride();
    write.keys = keys.begin().get();
    write.keys_stride = keys.begin().stride();
    write.lengths = lengths.begin().get();
    write.lengths_stride = lengths.begin().stride();
    write.values = contents.begin().get();
    write.values_stride = contents.begin().stride();

    ustore_write(&write);
}