/**
 * @file modality_graph.cpp
 * @author Ashot Vardanian
 *
 * @brief Graph implementation using fast integer compression.
 * Sits on top of any @see "ukv.h"-compatible system.
 *
 * For every vertex this implementation stores:
 * * inbound degree
 * * output degree
 * * inbound neighborships: neighbor ID + edge ID
 * * outbound neighborships: neighbor ID + edge ID
 */

#include <numeric>  // `std::accumulate`
#include <optional> // `std::optional`
#include <limits>   // `std::numeric_limits`

#include "ukv/ukv.hpp"
#include "helpers/pmr.hpp"       // `stl_arena_t`
#include "helpers/algorithm.hpp" // `equal_subrange`

/*********************************************************/
/*****************	 C++ Implementation	  ****************/
/*********************************************************/

using namespace unum::ukv;
using namespace unum;

ukv_key_t ukv_default_edge_id_k = std::numeric_limits<ukv_key_t>::max();
ukv_vertex_degree_t ukv_vertex_degree_missing_k = std::numeric_limits<ukv_vertex_degree_t>::max();

constexpr std::size_t bytes_in_degrees_header_k = 2 * sizeof(ukv_vertex_degree_t);

struct updated_entry_t : public collection_key_t {
    ukv_bytes_ptr_t content = nullptr;
    ukv_length_t length = ukv_length_missing_k;
    ukv_vertex_degree_t degree_delta = 0;
    inline operator value_view_t() const noexcept { return {content, length}; }
};

ptr_range_gt<neighborship_t const> neighbors( //
    ukv_vertex_degree_t const* degrees,
    ukv_key_t const* neighborships,
    ukv_vertex_role_t role = ukv_vertex_role_any_k) {
    auto ships = reinterpret_cast<neighborship_t const*>(neighborships);

    switch (role) {
    case ukv_vertex_source_k: return {ships, ships + degrees[0]};
    case ukv_vertex_target_k: return {ships + degrees[0], ships + degrees[0] + degrees[1]};
    case ukv_vertex_role_any_k: return {ships, ships + degrees[0] + degrees[1]};
    case ukv_vertex_role_unknown_k: return {};
    }
    __builtin_unreachable();
}

ptr_range_gt<neighborship_t const> neighbors(value_view_t bytes, ukv_vertex_role_t role = ukv_vertex_role_any_k) {
    // Handle missing vertices
    if (bytes.size() < bytes_in_degrees_header_k)
        return {};

    auto degrees = reinterpret_cast<ukv_vertex_degree_t const*>(bytes.begin());
    return neighbors(degrees, reinterpret_cast<ukv_key_t const*>(degrees + 2), role);
}

struct neighborhood_t {
    ukv_key_t center = 0;
    ptr_range_gt<neighborship_t const> targets;
    ptr_range_gt<neighborship_t const> sources;

    neighborhood_t() = default;
    neighborhood_t(neighborhood_t const&) = default;
    neighborhood_t(neighborhood_t&&) = default;

    /**
     * @brief Parses the a single `value_view_t` chunk
     * from the output of `ukv_graph_find_edges`.
     */
    inline neighborhood_t(ukv_key_t center_vertex, value_view_t bytes) noexcept {
        center = center_vertex;
        targets = neighbors(bytes, ukv_vertex_source_k);
        sources = neighbors(bytes, ukv_vertex_target_k);
    }

    inline neighborhood_t(ukv_key_t center_vertex,
                          ukv_vertex_degree_t const* degrees,
                          ukv_key_t const* neighborships) noexcept {
        center = center_vertex;
        targets = neighbors(degrees, neighborships, ukv_vertex_source_k);
        sources = neighbors(degrees, neighborships, ukv_vertex_target_k);
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

    inline ptr_range_gt<neighborship_t const> outgoing_to(ukv_key_t target) const noexcept {
        return equal_subrange(targets, target);
    }

    inline ptr_range_gt<neighborship_t const> incoming_from(ukv_key_t source) const noexcept {
        return equal_subrange(sources, source);
    }

    inline neighborship_t const* outgoing_to(ukv_key_t target, ukv_key_t edge_id) const noexcept {
        auto r = equal_subrange(targets, neighborship_t {target, edge_id});
        return r.size() ? r.begin() : nullptr;
    }

    inline neighborship_t const* incoming_from(ukv_key_t source, ukv_key_t edge_id) const noexcept {
        auto r = equal_subrange(sources, neighborship_t {source, edge_id});
        return r.size() ? r.begin() : nullptr;
    }

    inline ptr_range_gt<neighborship_t const> only(ukv_vertex_role_t role) const noexcept {
        switch (role) {
        case ukv_vertex_source_k: return targets;
        case ukv_vertex_target_k: return sources;
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
    strided_iterator_gt<ukv_key_t const> centers_;
    ukv_vertex_degree_t const* degrees_per_vertex_ = nullptr;
    ukv_key_t const* neighborships_per_vertex_ = nullptr;

    neighborhoods_iterator_t(strided_iterator_gt<ukv_key_t const> centers,
                             ukv_vertex_degree_t const* degrees_per_vertex,
                             ukv_key_t const* neighborships_per_vertex) noexcept
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
    strided_range_gt<ukv_key_t const> centers_;
    ukv_vertex_degree_t const* degrees_per_vertex_ = nullptr;
    ukv_key_t const* neighborships_per_vertex_ = nullptr;

    neighborhoods_t(strided_range_gt<ukv_key_t const> centers,
                    ukv_vertex_degree_t const* degrees_per_vertex,
                    ukv_key_t const* neighborships_per_vertex) noexcept
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
    ukv_vertex_role_t role,
    ukv_key_t neighbor_id,
    ukv_key_t edge_id) {

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
    ukv_vertex_role_t role,
    ukv_key_t neighbor_id,
    ukv_key_t edge_id) {

    auto ship = neighborship_t {neighbor_id, edge_id};
    auto degrees = reinterpret_cast<ukv_vertex_degree_t*>(entry.content);
    auto ships = reinterpret_cast<neighborship_t*>(degrees + 2);
    if (entry.length < bytes_in_degrees_header_k || entry.length == ukv_length_missing_k) {
        degrees[role != ukv_vertex_target_k] = 0;
        degrees[role == ukv_vertex_target_k] = 1;
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
        degrees[role == ukv_vertex_target_k] += 1;
        entry.length += sizeof(neighborship_t);
    }
}

/**
 * @return true  If a matching entry was found and deleted.
 * @return false In every other case.
 */
void erase_from_entry(updated_entry_t& entry,
                      ukv_vertex_role_t role,
                      ukv_key_t neighbor_id,
                      std::optional<ukv_key_t> edge_id = {}) {

    if (entry.length < bytes_in_degrees_header_k || entry.length == ukv_length_missing_k)
        return;

    std::size_t off = 0;
    std::size_t len = 0;

    auto degrees = reinterpret_cast<ukv_vertex_degree_t*>(entry.content);
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
    degrees[role == ukv_vertex_target_k] -= len;
    entry.degree_delta += len;
    entry.length -= sizeof(neighborship_t) * len;
}

template <bool export_center_ak = true, bool export_neighbor_ak = true, bool export_edge_ak = true>
void export_edge_tuples( //
    ukv_database_t const c_db,
    ukv_transaction_t const c_transaction,
    ukv_size_t const c_vertices_count,

    ukv_collection_t const* c_collections,
    ukv_size_t const c_collections_stride,

    ukv_key_t const* c_vertices_ids,
    ukv_size_t const c_vertices_stride,

    ukv_vertex_role_t const* c_roles,
    ukv_size_t const c_roles_stride,

    ukv_options_t const c_options,

    ukv_vertex_degree_t** c_degrees_per_vertex,
    ukv_key_t** c_neighborships_per_vertex,

    ukv_arena_t* c_arena,
    ukv_error_t* c_error) {

    stl_arena_t arena = prepare_arena(c_arena, c_options, c_error);
    return_on_error(c_error);

    // Even if we need just the node degrees, we can't limit ourselves to just entry lengths.
    // Those may be compressed. We need to read the first bytes to parse the degree of the node.
    ukv_arena_t arena_ptr = &arena;
    ukv_bytes_ptr_t c_found_values = nullptr;
    ukv_length_t* c_found_offsets = nullptr;
    ukv_read( //
        c_db,
        c_transaction,
        c_vertices_count,
        c_collections,
        c_collections_stride,
        c_vertices_ids,
        c_vertices_stride,
        c_options,
        nullptr,
        &c_found_offsets,
        nullptr,
        &c_found_values,
        &arena_ptr,
        c_error);
    return_on_error(c_error);

    joined_bins_t values {c_vertices_count, c_found_offsets, c_found_values};
    strided_range_gt<ukv_key_t const> vertices_ids {{c_vertices_ids, c_vertices_stride}, c_vertices_count};
    strided_iterator_gt<ukv_vertex_role_t const> roles {c_roles, c_roles_stride};
    constexpr std::size_t tuple_size_k = export_center_ak + export_neighbor_ak + export_edge_ak;

    // Estimate the amount of memory we will need for the arena
    std::size_t count_ids = 0;
    if constexpr (tuple_size_k != 0) {
        joined_bins_iterator_t values_it = values.begin();
        for (ukv_size_t i = 0; i != c_vertices_count; ++i, ++values_it) {
            value_view_t value = *values_it;
            ukv_vertex_role_t role = roles[i];
            count_ids += neighbors(value, role).size();
        }
        count_ids *= tuple_size_k;
    }

    // Export into arena
    auto ids = arena.alloc_or_dummy<ukv_key_t>(count_ids, c_error, c_neighborships_per_vertex);
    return_on_error(c_error);
    auto degrees = arena.alloc_or_dummy<ukv_vertex_degree_t>(c_vertices_count, c_error, c_degrees_per_vertex);
    return_on_error(c_error);

    std::size_t passed_ids = 0;
    joined_bins_iterator_t values_it = values.begin();
    for (std::size_t i = 0; i != c_vertices_count; ++i, ++values_it) {
        value_view_t value = *values_it;
        ukv_key_t vertex_id = vertices_ids[i];
        ukv_vertex_role_t role = roles[i];

        // Some values may be missing
        if (!value) {
            degrees[i] = ukv_vertex_degree_missing_k;
            continue;
        }

        ukv_vertex_degree_t degree = 0;
        if (role & ukv_vertex_source_k) {
            auto ns = neighbors(value, ukv_vertex_source_k);
            if constexpr (tuple_size_k != 0)
                for (neighborship_t n : ns) {
                    if constexpr (export_center_ak)
                        ids[passed_ids + 0] = vertex_id;
                    if constexpr (export_neighbor_ak)
                        ids[passed_ids + export_center_ak] = n.neighbor_id;
                    if constexpr (export_edge_ak)
                        ids[passed_ids + export_center_ak + export_neighbor_ak] = n.edge_id;
                    passed_ids += tuple_size_k;
                }
            degree += static_cast<ukv_vertex_degree_t>(ns.size());
        }
        if (role & ukv_vertex_target_k) {
            auto ns = neighbors(value, ukv_vertex_target_k);
            if constexpr (tuple_size_k != 0)
                for (neighborship_t n : ns) {
                    if constexpr (export_neighbor_ak)
                        ids[passed_ids + 0] = n.neighbor_id;
                    if constexpr (export_center_ak)
                        ids[passed_ids + export_neighbor_ak] = vertex_id;
                    if constexpr (export_edge_ak)
                        ids[passed_ids + export_center_ak + export_neighbor_ak] = n.edge_id;
                    passed_ids += tuple_size_k;
                }
            degree += static_cast<ukv_vertex_degree_t>(ns.size());
        }

        degrees[i] = degree;
    }
}

void pull_and_link_for_updates( //
    ukv_database_t const c_db,
    ukv_transaction_t const c_transaction,
    strided_range_gt<updated_entry_t> unique_entries,
    ukv_options_t const c_options,
    ukv_arena_t* c_arena,
    ukv_error_t* c_error) {

    stl_arena_t arena = prepare_arena(c_arena, c_options, c_error);
    return_on_error(c_error);

    // Fetch the existing entries
    ukv_arena_t arena_ptr = &arena;
    ukv_bytes_ptr_t found_binary_begin = nullptr;
    ukv_length_t* found_binary_offs = nullptr;
    ukv_size_t unique_count = static_cast<ukv_size_t>(unique_entries.size());
    auto collections = unique_entries.immutable().members(&updated_entry_t::collection);
    auto keys = unique_entries.immutable().members(&updated_entry_t::key);
    auto opts = c_transaction ? ukv_options_t(c_options & ~ukv_option_transaction_dont_watch_k) : c_options;
    ukv_read( //
        c_db,
        c_transaction,
        unique_count,
        collections.begin().get(),
        collections.begin().stride(),
        keys.begin().get(),
        keys.begin().stride(),
        opts,
        nullptr,
        &found_binary_offs,
        nullptr,
        &found_binary_begin,
        &arena_ptr,
        c_error);
    return_on_error(c_error);

    // Link the response buffer to `unique_entries`
    joined_bins_t found_binaries {unique_count, found_binary_offs, found_binary_begin};
    for (std::size_t i = 0; i != unique_count; ++i) {
        auto found_binary = found_binaries[i];
        unique_entries[i].content = ukv_bytes_ptr_t(found_binary.data());
        unique_entries[i].length = found_binary ? static_cast<ukv_length_t>(found_binary.size()) : ukv_length_missing_k;
    }
}

template <bool erase_ak>
void update_neighborhoods( //
    ukv_database_t const c_db,
    ukv_transaction_t const c_transaction,
    ukv_size_t const c_tasks_count,

    ukv_collection_t const* c_collections,
    ukv_size_t const c_collections_stride,

    ukv_key_t const* c_edges_ids,
    ukv_size_t const c_edges_stride,

    ukv_key_t const* c_sources_ids,
    ukv_size_t const c_sources_stride,

    ukv_key_t const* c_targets_ids,
    ukv_size_t const c_targets_stride,

    ukv_options_t const c_options,

    ukv_arena_t* c_arena,
    ukv_error_t* c_error) {

    stl_arena_t arena = prepare_arena(c_arena, c_options, c_error);
    return_on_error(c_error);

    strided_iterator_gt<ukv_collection_t const> edge_collections {c_collections, c_collections_stride};
    strided_iterator_gt<ukv_key_t const> edges_ids {c_edges_ids, c_edges_stride};
    strided_iterator_gt<ukv_key_t const> sources_ids {c_sources_ids, c_sources_stride};
    strided_iterator_gt<ukv_key_t const> targets_ids {c_targets_ids, c_targets_stride};

    // Fetch all the data related to touched vertices, and deduplicate them
    auto unique_entries = arena.alloc<updated_entry_t>(c_tasks_count * 2, c_error);
    return_on_error(c_error);
    std::fill(unique_entries.begin(), unique_entries.end(), updated_entry_t {});
    for (ukv_size_t i = 0; i != c_tasks_count; ++i)
        unique_entries[i].collection = edge_collections[i], unique_entries[i].key = sources_ids[i];
    for (ukv_size_t i = 0; i != c_tasks_count; ++i)
        unique_entries[c_tasks_count + i].collection = edge_collections[i],
                                       unique_entries[c_tasks_count + i].key = targets_ids[i];

    // Lets put all the unique IDs in the beginning of the range,
    // and then refill the tail with replicas
    auto unique_count = sort_and_deduplicate(unique_entries.begin(), unique_entries.end());
    unique_entries = {unique_entries.begin(), unique_count};

    // Fetch the existing entries
    ukv_arena_t arena_ptr = &arena;
    auto unique_strided = unique_entries.strided();
    pull_and_link_for_updates(c_db, c_transaction, unique_strided, c_options, &arena_ptr, c_error);
    return_on_error(c_error);

    // Define our primary for-loop
    auto for_each_task = [&](auto entry_role_target_edge_callback) {
        for (std::size_t i = 0; i != c_tasks_count; ++i) {
            auto collection = edge_collections[i];
            auto source_id = sources_ids[i];
            auto target_id = targets_ids[i];
            auto edge_id = edges_ids ? edges_ids[i] : ukv_key_unknown_k;
            auto source_idx = offset_in_sorted(unique_entries, collection_key_t {collection, source_id});
            auto target_idx = offset_in_sorted(unique_entries, collection_key_t {collection, target_id});
            entry_role_target_edge_callback(unique_entries[source_idx], ukv_vertex_source_k, target_id, edge_id);
            entry_role_target_edge_callback(unique_entries[target_idx], ukv_vertex_target_k, source_id, edge_id);
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
            auto bytes_present = unique_entry.length != ukv_length_missing_k ? unique_entry.length : 0;
            auto bytes_for_relations = unique_entry.degree_delta * sizeof(neighborship_t);
            auto bytes_for_degrees = bytes_present > bytes_in_degrees_header_k ? 0 : bytes_in_degrees_header_k;
            auto new_size = bytes_present + bytes_for_relations + bytes_for_degrees;
            auto new_buffer = arena.alloc<byte_t>(new_size, c_error);
            return_on_error(c_error);
            std::memcpy(new_buffer.begin(), unique_entry.content, bytes_present);

            unique_entry.content = (ukv_bytes_ptr_t)new_buffer.begin();
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
    ukv_write( //
        c_db,
        c_transaction,
        unique_count,
        collections.begin().get(),
        collections.begin().stride(),
        keys.begin().get(),
        keys.begin().stride(),
        nullptr,
        nullptr,
        0,
        lengths.begin().get(),
        lengths.begin().stride(),
        contents.begin().get(),
        contents.begin().stride(),
        c_options,
        &arena_ptr,
        c_error);
}

void ukv_graph_find_edges(ukv_graph_find_edges_t* c_ptr) {

    ukv_graph_find_edges_t& c = *c_ptr;
    if (!c.tasks_count)
        return;

    bool only_degrees = !c.edges_per_vertex;
    auto func = only_degrees //
                    ? &export_edge_tuples<false, false, false>
                    : &export_edge_tuples<true, true, true>;
    return func( //
        c.db,
        c.transaction,
        c.tasks_count,
        c.collections,
        c.collections_stride,
        c.vertices_ids,
        c.vertices_stride,
        c.roles,
        c.roles_stride,
        c.options,
        c.degrees_per_vertex,
        c.edges_per_vertex,
        c.arena,
        c.error);
}

void ukv_graph_upsert_edges(ukv_graph_upsert_edges_t* c_ptr) {

    ukv_graph_upsert_edges_t& c = *c_ptr;

    if (!c.tasks_count)
        return;

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
        c.arena,
        c.error);
}

void ukv_graph_remove_edges(ukv_graph_remove_edges_t* c_ptr) {

    ukv_graph_remove_edges_t& c = *c_ptr;

    if (!c.tasks_count)
        return;

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
        c.arena,
        c.error);
}

void ukv_graph_remove_vertices(ukv_graph_remove_vertices_t* c_ptr) {

    ukv_graph_remove_vertices_t& c = *c_ptr;

    if (!c.tasks_count)
        return;

    stl_arena_t arena = prepare_arena(c.arena, c.options, c.error);
    return_on_error(c.error);

    strided_iterator_gt<ukv_collection_t const> vertex_collections {c.collections, c.collections_stride};
    strided_range_gt<ukv_key_t const> vertices_ids {{c.vertices_ids, c.vertices_stride}, c.tasks_count};
    strided_iterator_gt<ukv_vertex_role_t const> vertex_roles {c.roles, c.roles_stride};

    // Initially, just retrieve the bare minimum information about the vertices
    ukv_vertex_degree_t* degrees_per_vertex = nullptr;
    ukv_key_t* neighbors_per_vertex = nullptr;
    ukv_arena_t arena_ptr = &arena;
    export_edge_tuples<false, true, false>( //
        c.db,
        c.transaction,
        c.tasks_count,
        c.collections,
        c.collections_stride,
        c.vertices_ids,
        c.vertices_stride,
        c.roles,
        c.roles_stride,
        c.options,
        &degrees_per_vertex,
        &neighbors_per_vertex,
        &arena_ptr,
        c.error);
    return_on_error(c.error);

    // Enumerate the opposite ends, from which that same reference must be removed.
    // Here all the keys will be in the sorted order.
    auto unique_count = std::accumulate(degrees_per_vertex, degrees_per_vertex + c.tasks_count, c.tasks_count);
    auto unique_entries = arena.alloc<updated_entry_t>(unique_count, c.error);
    return_on_error(c.error);
    std::fill(unique_entries.begin(), unique_entries.end(), updated_entry_t {});

    // Sorting the tasks would help us faster locate them in the future.
    // We may also face repetitions when connected vertices are removed.
    {
        auto planned_entries = unique_entries.begin();
        for (std::size_t i = 0; i != c.tasks_count; ++i) {
            auto collection = planned_entries->collection = vertex_collections[i];
            planned_entries->key = vertices_ids[i];
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
    pull_and_link_for_updates(c.db, c.transaction, unique_strided, c.options, &arena_ptr, c.error);
    return_on_error(c.error);

    // From every opposite end - remove a match, and only then - the content itself
    for (std::size_t i = 0; i != unique_strided.size(); ++i) {
        auto vertex_collection = vertex_collections[i];
        auto vertex_id = vertices_ids[i];
        auto vertex_role = vertex_roles[i];

        auto vertex_idx = offset_in_sorted(unique_entries, collection_key_t {vertex_collection, vertex_id});
        updated_entry_t& vertex_value = unique_entries[vertex_idx];

        for (neighborship_t n : neighbors(vertex_value, vertex_role)) {
            auto neighbor_idx = offset_in_sorted(unique_entries, collection_key_t {vertex_collection, n.neighbor_id});
            updated_entry_t& neighbor_value = unique_entries[neighbor_idx];
            if (vertex_role == ukv_vertex_role_any_k) {
                erase_from_entry(neighbor_value, ukv_vertex_source_k, vertex_id);
                erase_from_entry(neighbor_value, ukv_vertex_target_k, vertex_id);
            }
            else
                erase_from_entry(neighbor_value, invert(vertex_role), vertex_id);
        }

        vertex_value.content = nullptr;
        vertex_value.length = ukv_length_missing_k;
    }

    // Now we will go through all the explicitly deleted vertices
    auto collections = unique_strided.immutable().members(&updated_entry_t::collection);
    auto keys = unique_strided.immutable().members(&updated_entry_t::key);
    auto lengths = unique_strided.immutable().members(&updated_entry_t::length);
    auto contents = unique_strided.immutable().members(&updated_entry_t::content);
    ukv_write( //
        c.db,
        c.transaction,
        unique_count,
        collections.begin().get(),
        collections.begin().stride(),
        keys.begin().get(),
        keys.begin().stride(),
        nullptr,
        nullptr,
        0,
        lengths.begin().get(),
        lengths.begin().stride(),
        contents.begin().get(),
        contents.begin().stride(),
        c.options,
        &arena_ptr,
        c.error);
}