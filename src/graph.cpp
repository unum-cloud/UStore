/**
 *  @file src/graph.cpp
 *  @author Ash Vardanian
 *  @date January 11, 2023
 *  @brief The graph modality over the reads and writes of any engine, storing every vertex as one value of its
 *      degrees and sorted neighborships.
 *
 *  The codec of a vertex value, its degrees and its sorted neighborships, lives in `ustore/graph/vertex_values.hpp`.
 *  This file reads and writes those values through the engine's own blobs calls, and implements `c/ustore/graph.h`
 *  over them: routing every graph call to its engine, and the spelling of vertex roles.
 */

#include <cstddef> // `std::size_t`
#include <cstring> // `std::memcpy`

#include <algorithm> // `std::ranges::fill`, `std::ranges::partition`
#include <span>      // `std::span`

#include "ustore/graph.h"    // `ustore_graph_read_edges_t`, `ustore_vertex_role_t`
#include "ustore/ranges.hpp" // `strided_span`, `values_view_t`

#include "ustore/graph/vertex_values.hpp"   // `updated_entry_t`, `neighbors`, `insert_into_entry`, `erase_from_entry`
#include "ustore/utilities/ram_buffers.hpp" // `arena_allocate`

#include "engine.hpp"            // `engine_of`, `view_t`, `graph_read_edges_in_vertex_values`
#include "lib.hpp"               // `route`, `enumerator_spelling`, `name_in`, `value_in`
#include "utilities/batches.hpp" // `edges_batch_t`, `strings_arg_t`

namespace unum::ustore {
using namespace unum::ustore::utilities;
using namespace unum::ustore::graph;

/** The identifiers an edge export writes for every neighborship, next to the degree of every vertex. */
enum class edge_export_t { degrees_k, neighbors_k, edges_k };

/** Whether an update of neighborhoods inserts edges or erases them. */
enum class neighborhood_update_t { upsert_k, erase_k };

/** How many identifiers an export writes per neighborship. */
constexpr std::size_t ids_per_neighborship(edge_export_t export_kind) noexcept {
    switch (export_kind) {
    case edge_export_t::degrees_k: return 0;
    case edge_export_t::neighbors_k: return 1;
    case edge_export_t::edges_k: return 3;
    }
    return 0;
}

/** Reads the values of @p count vertices through @p view with the engine's own read, which appends to the scratch of
 *  @p view rather than rewinding it, and whose presences tell a missing vertex apart from a bare one. */
expected<values_view_t> read_vertices(ustore_database_t database, std::size_t count,
                                      ustore_collections_view_t collections, ustore_keys_view_t vertices,
                                      view_t const& view) noexcept {
    ustore_octet_t const* presences = nullptr;
    ustore_strings_view_t contents {};
    ustore_blobs_read_t read {};
    read.database = database;
    read.count = count;
    read.collections = collections;
    read.keys = vertices;
    read.presences = &presences;
    read.values = &contents;
    read.arena = view.scratch;
    if (status_t const status = engine_of(database).blobs_read(read, view); status.failed()) return status;
    return values_view_t {count, presences, contents};
}

/** Reads the vertices and exports their degrees, the offsets of their edges when asked for, and their neighbors or
 *  edges when @p export_kind_ asks for them, into @p arena alone. */
template <edge_export_t export_kind_>
status_t export_edge_tuples( //
    ustore_database_t const database, ustore_size_t const vertices_count, ustore_collections_view_t collections,
    ustore_keys_view_t vertices, ustore_vertex_roles_view_t roles, ustore_vertex_degree_t const** degrees_per_vertex,
    ustore_length_t const** edges_offsets, ustore_key_t const** neighborships_per_vertex, ustore_arena_t arena,
    view_t const& view) noexcept {

    edges_batch_t const read_edges {strided(collections), strided(vertices), strided(roles), vertices_count};
    auto [values, values_status] = read_vertices(database, vertices_count, collections, vertices, view);
    if (values_status.failed()) return values_status;
    constexpr std::size_t ids_per_neighborship_k = ids_per_neighborship(export_kind_);

    std::size_t ids_count = 0;
    for (std::size_t index = 0; index != vertices_count; ++index) {
        if (!is_vertex_value(values[index]))
            return {status_code_t::malformed_k, "A vertex holds a value that is not a graph vertex"};
        ids_count += neighbors(values[index], read_edges[index].role).size();
    }
    ids_count *= ids_per_neighborship_k;

    auto [degrees, degrees_status] = arena_allocate<ustore_vertex_degree_t>(arena, vertices_count);
    if (degrees_status.failed()) return degrees_status;
    auto [offsets, offsets_status] = arena_allocate<ustore_length_t>(arena, edges_offsets ? vertices_count + 1 : 0);
    if (offsets_status.failed()) return offsets_status;
    auto [ids, ids_status] = arena_allocate<ustore_key_t>(arena, ids_count);
    if (ids_status.failed()) return ids_status;

    std::size_t exported_ids = 0;
    std::size_t exported_edges = 0;
    for (std::size_t index = 0; index != vertices_count; ++index) {
        value_view_t const value = values[index];
        edges_task_t const read_edge = read_edges[index];
        if (edges_offsets) offsets[index] = static_cast<ustore_length_t>(exported_edges);

        if (!value) {
            degrees[index] = ustore_vertex_degree_missing_k;
            continue;
        }

        ustore_vertex_degree_t degree = 0;
        if (read_edge.role & ustore_vertex_role_source_k) {
            neighborships_view_t const outgoing = neighbors(value, ustore_vertex_role_source_k);
            if constexpr (export_kind_ == edge_export_t::edges_k) {
                for (neighborship_t const neighborship : outgoing) {
                    ids[exported_ids] = read_edge.vertex_id;
                    ids[exported_ids + 1] = neighborship.neighbor_id;
                    ids[exported_ids + 2] = neighborship.edge_id;
                    exported_ids += ids_per_neighborship_k;
                }
            }
            else if constexpr (export_kind_ == edge_export_t::neighbors_k) {
                for (neighborship_t const neighborship : outgoing) ids[exported_ids++] = neighborship.neighbor_id;
            }
            degree += static_cast<ustore_vertex_degree_t>(outgoing.size());
        }
        if (read_edge.role & ustore_vertex_role_target_k) {
            neighborships_view_t const incoming = neighbors(value, ustore_vertex_role_target_k);
            if constexpr (export_kind_ == edge_export_t::edges_k) {
                for (neighborship_t const neighborship : incoming) {
                    ids[exported_ids] = neighborship.neighbor_id;
                    ids[exported_ids + 1] = read_edge.vertex_id;
                    ids[exported_ids + 2] = neighborship.edge_id;
                    exported_ids += ids_per_neighborship_k;
                }
            }
            else if constexpr (export_kind_ == edge_export_t::neighbors_k) {
                for (neighborship_t const neighborship : incoming) ids[exported_ids++] = neighborship.neighbor_id;
            }
            degree += static_cast<ustore_vertex_degree_t>(incoming.size());
        }
        degrees[index] = degree;
        exported_edges += degree;
    }
    if (edges_offsets) offsets[vertices_count] = static_cast<ustore_length_t>(exported_edges);
    *degrees_per_vertex = degrees.data();
    if (edges_offsets) *edges_offsets = offsets.data();
    if (neighborships_per_vertex) *neighborships_per_vertex = ids.data();
    return {};
}

/** Reads the values of @p unique_entries into the scratch of @p view and points every entry at its own bytes. */
status_t pull_and_link_for_updates( //
    ustore_database_t const database, strided_span<updated_entry_t> unique_entries, view_t const& view) noexcept {

    std::size_t const unique_count = unique_entries.size();
    auto const collections = unique_entries.immutable().members(&updated_entry_t::collection);
    auto const keys = unique_entries.immutable().members(&updated_entry_t::key);

    // A transaction watches the vertices it rewrites, whatever the caller asked of the writes
    view_t watching = view;
    watching.watch = ustore_watch_keys_k;
    auto [found, status] = read_vertices(database, unique_count, {collections.data(), collections.stride()},
                                         {keys.data(), keys.stride()}, watching);
    if (status.failed()) return status;

    // The values lie in the scratch of this call, so the updates rewrite them in place
    for (std::size_t index = 0; index != unique_count; ++index) {
        value_view_t const value = found[index];
        if (!is_vertex_value(value))
            return {status_code_t::malformed_k, "A vertex holds a value that is not a graph vertex"};
        unique_entries[index].content = const_cast<ustore_byte_t*>(
            reinterpret_cast<ustore_byte_t const*>(value.data()));
        unique_entries[index].length = value ? static_cast<ustore_length_t>(value.size()) : missing_length_k;
    }
    return {};
}

/** Writes the vertex values of @p entries back through the engine's own write, in the transaction of @p view. */
status_t write_entries(ustore_database_t const database, std::span<updated_entry_t const> entries,
                       ustore_durability_t const durability, view_t const& view) noexcept {
    if (entries.empty()) return {};
    strided_span<updated_entry_t const> const written = strided(entries);
    strided_span<ustore_collection_id_t const> const collections = written.members(&updated_entry_t::collection);
    strided_span<ustore_key_t const> const keys = written.members(&updated_entry_t::key);

    // The views of the values are gathered here, as the router gathers what an accessor writes
    auto [values, values_status] = arena_allocate<ustore_bytes_view_t>(view.scratch, entries.size());
    if (values_status.failed()) return values_status;
    for (std::size_t index = 0; index != entries.size(); ++index)
        values[index] = {entries[index].content, entries[index].length};

    ustore_blobs_write_t write {};
    write.database = database;
    write.count = entries.size();
    write.collections = {collections.data(), collections.stride()};
    write.keys = {keys.data(), keys.stride()};
    write.durability = durability;
    return engine_of(database).blobs_write(write, strings_arg_t::from_views(values.data()), view);
}

/** Inserts or erases edges in the neighborships of both their ends, and writes every touched vertex back. */
template <neighborhood_update_t update_, typename arguments_type_>
status_t update_neighborhoods(arguments_type_ const& arguments, view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!arguments.sources.data || !arguments.targets.data)
        return {status_code_t::invalid_argument_k, "Need the sources and the targets of the edges"};

    ustore_arena_t const scratch = view.scratch;
    std::size_t const edges_count = arguments.count;
    strided_iterator<ustore_collection_id_t const> const edge_collections =
        arguments.collections.data ? strided(arguments.collections)
                                   : strided_iterator<ustore_collection_id_t const> {&ustore_collection_main_k, 0};
    strided_iterator<ustore_key_t const> const edges = strided(arguments.edges);
    strided_iterator<ustore_key_t const> const sources = strided(arguments.sources);
    strided_iterator<ustore_key_t const> const targets = strided(arguments.targets);

    // Both ends of every edge, deduplicated, so each touched vertex is read and written once
    auto [unique_entries, entries_status] = arena_allocate<updated_entry_t>(scratch, edges_count * 2);
    if (entries_status.failed()) return entries_status;
    std::ranges::fill(unique_entries, updated_entry_t {});
    for (std::size_t index = 0; index != edges_count; ++index) {
        unique_entries[index].collection = edge_collections[index];
        unique_entries[index].key = sources[index];
        unique_entries[edges_count + index].collection = edge_collections[index];
        unique_entries[edges_count + index].key = targets[index];
    }

    std::size_t const unique_count = sort_and_deduplicate(unique_entries.begin(), unique_entries.end());
    unique_entries = unique_entries.first(unique_count);

    if (status_t const status = pull_and_link_for_updates(arguments.database, strided(unique_entries), view);
        status.failed())
        return status;

    auto const for_each_task = [&](auto update_entry) {
        for (std::size_t index = 0; index != edges_count; ++index) {
            ustore_collection_id_t const collection = edge_collections[index];
            ustore_key_t const source_id = sources[index];
            ustore_key_t const target_id = targets[index];
            ustore_key_t const edge_id = edges ? edges[index] : ustore_key_unknown_k;
            std::size_t const source_offset = offset_in_sorted(unique_entries,
                                                               collection_key_t {collection, source_id});
            std::size_t const target_offset = offset_in_sorted(unique_entries,
                                                               collection_key_t {collection, target_id});
            update_entry(unique_entries[source_offset], ustore_vertex_role_source_k, target_id, edge_id);
            update_entry(unique_entries[target_offset], ustore_vertex_role_target_k, source_id, edge_id);
        }
    };

    if constexpr (update_ == neighborhood_update_t::erase_k) { for_each_task(&erase_from_entry); }
    else {
        // Count the growth of every vertex, reallocate each buffer once, then insert in place
        for_each_task(&count_inserts_into_entry);
        for (std::size_t index = 0; index != unique_count; ++index) {
            updated_entry_t& entry = unique_entries[index];
            std::size_t const bytes_present = entry.length != missing_length_k ? entry.length : 0;
            std::size_t const bytes_for_relations = entry.degree_delta * sizeof(neighborship_t);
            std::size_t const bytes_for_degrees = bytes_present > bytes_in_degrees_header_k ? 0
                                                                                            : bytes_in_degrees_header_k;
            auto [buffer, status] = arena_allocate<byte_t>(scratch,
                                                           bytes_present + bytes_for_relations + bytes_for_degrees);
            if (status.failed()) return status;
            if (bytes_present) std::memcpy(buffer.data(), entry.content, bytes_present);
            entry.content = reinterpret_cast<ustore_byte_t*>(buffer.data());
            entry.length = static_cast<ustore_length_t>(bytes_present);
        }
        for_each_task(&insert_into_entry);
    }

    // Vertices the update left unchanged move to the back and are not written
    auto const unchanged = std::ranges::partition(
        unique_entries, [](ustore_vertex_degree_t delta) { return delta != 0; }, &updated_entry_t::degree_delta);
    std::size_t const changed_count = static_cast<std::size_t>(unchanged.begin() - unique_entries.begin());
    return write_entries(arguments.database, unique_entries.first(changed_count), arguments.durability, view);
}

/** Removes the vertices of @p entries through the engine's own removal, in the transaction of @p view. */
status_t remove_entries(ustore_database_t const database, std::span<updated_entry_t const> entries,
                        ustore_durability_t const durability, view_t const& view) noexcept {
    if (entries.empty()) return {};
    strided_span<updated_entry_t const> const removed = strided(entries);
    strided_span<ustore_collection_id_t const> const collections = removed.members(&updated_entry_t::collection);
    strided_span<ustore_key_t const> const keys = removed.members(&updated_entry_t::key);

    ustore_blobs_remove_t remove {};
    remove.database = database;
    remove.count = entries.size();
    remove.collections = {collections.data(), collections.stride()};
    remove.keys = {keys.data(), keys.stride()};
    remove.durability = durability;
    return engine_of(database).blobs_remove(remove, view);
}

status_t graph_read_edges_in_vertex_values(ustore_graph_read_edges_t const& arguments, view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!arguments.degrees_per_vertex) return {status_code_t::invalid_argument_k, "Need an output for the degrees"};
    if (arguments.edges_per_vertex && !arguments.edges_offsets)
        return {status_code_t::invalid_argument_k, "Need an output for the offsets of the edges"};
    if (arguments.edges_per_vertex)
        return export_edge_tuples<edge_export_t::edges_k>( //
            arguments.database, arguments.count, arguments.collections, arguments.vertices, arguments.roles,
            arguments.degrees_per_vertex, arguments.edges_offsets, arguments.edges_per_vertex, arguments.arena, view);
    return export_edge_tuples<edge_export_t::degrees_k>( //
        arguments.database, arguments.count, arguments.collections, arguments.vertices, arguments.roles,
        arguments.degrees_per_vertex, arguments.edges_offsets, nullptr, arguments.arena, view);
}

status_t graph_write_edges_in_vertex_values(ustore_graph_write_edges_t const& arguments, view_t const& view) noexcept {
    return update_neighborhoods<neighborhood_update_t::upsert_k>(arguments, view);
}

status_t graph_remove_edges_in_vertex_values(ustore_graph_remove_edges_t const& arguments,
                                             view_t const& view) noexcept {
    return update_neighborhoods<neighborhood_update_t::erase_k>(arguments, view);
}

status_t graph_write_vertices_in_vertex_values(ustore_graph_write_vertices_t const& arguments,
                                               view_t const& view) noexcept {
    std::size_t const count = arguments.count;
    if (!count) return {};

    auto [missing, missing_status] = arena_allocate<updated_entry_t>(view.scratch, count);
    if (missing_status.failed()) return missing_status;

    ustore_octet_t const* presences = nullptr;
    ustore_blobs_read_t read {};
    read.database = arguments.database;
    read.count = count;
    read.collections = arguments.collections;
    read.keys = arguments.vertices;
    read.presences = &presences;
    read.arena = view.scratch;
    if (status_t const status = engine_of(arguments.database).blobs_read(read, view); status.failed()) return status;

    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    strided_iterator<ustore_key_t const> const vertices = strided(arguments.vertices);
    bits_view_t const present {presences};
    std::size_t missing_count = 0;
    for (std::size_t index = 0; index != count; ++index) {
        if (present[index]) continue;
        ustore_collection_id_t const collection = collections ? collections[index] : ustore_collection_main_k;
        missing[missing_count++] = updated_entry_t {{collection, vertices[index]}, nullptr, 0, 0};
    }
    return write_entries(arguments.database, missing.first(missing_count), arguments.durability, view);
}

status_t graph_remove_vertices_in_vertex_values(ustore_graph_remove_vertices_t const& arguments,
                                                view_t const& view) noexcept {
    if (!arguments.count) return {};

    ustore_arena_t const scratch = view.scratch;
    strided_iterator<ustore_collection_id_t const> const vertex_collections =
        arguments.collections.data ? strided(arguments.collections)
                                   : strided_iterator<ustore_collection_id_t const> {&ustore_collection_main_k, 0};
    strided_iterator<ustore_key_t const> const vertices = strided(arguments.vertices);
    strided_iterator<ustore_vertex_role_t const> const vertex_roles = strided(arguments.roles);

    // Only the neighbors of every removed vertex are needed to find the references to erase
    ustore_vertex_degree_t const* degrees = nullptr;
    ustore_key_t const* neighbors_per_vertex = nullptr;
    if (status_t const status = export_edge_tuples<edge_export_t::neighbors_k>( //
            arguments.database, arguments.count, arguments.collections, arguments.vertices, arguments.roles, &degrees,
            nullptr, &neighbors_per_vertex, scratch, view);
        status.failed())
        return status;

    auto const present_degree = [&](std::size_t index) {
        ustore_vertex_degree_t const degree = degrees[index];
        return degree == ustore_vertex_degree_missing_k ? std::size_t {0} : std::size_t {degree};
    };

    std::size_t unique_count = arguments.count;
    for (std::size_t index = 0; index != arguments.count; ++index) unique_count += present_degree(index);

    auto [unique_entries, entries_status] = arena_allocate<updated_entry_t>(scratch, unique_count);
    if (entries_status.failed()) return entries_status;
    std::ranges::fill(unique_entries, updated_entry_t {});

    // Every removed vertex and every neighbor of it, sorted and deduplicated for lookups
    updated_entry_t* planned = unique_entries.data();
    ustore_key_t const* neighbor = neighbors_per_vertex;
    for (std::size_t index = 0; index != arguments.count; ++index) {
        ustore_collection_id_t const collection = vertex_collections[index];
        planned->collection = collection;
        planned->key = vertices[index];
        ++planned;
        for (std::size_t offset = 0; offset != present_degree(index); ++offset, ++neighbor, ++planned) {
            planned->collection = collection;
            planned->key = *neighbor;
        }
    }
    unique_count = sort_and_deduplicate(unique_entries.data(), planned);
    unique_entries = unique_entries.first(unique_count);

    if (status_t const status = pull_and_link_for_updates(arguments.database, strided(unique_entries), view);
        status.failed())
        return status;

    // Erase the references every neighbor holds, then the removed vertex itself
    for (std::size_t index = 0; index != arguments.count; ++index) {
        ustore_collection_id_t const collection = vertex_collections[index];
        ustore_key_t const vertex_id = vertices[index];
        ustore_vertex_role_t const vertex_role = vertex_roles ? vertex_roles[index] : ustore_vertex_role_any_k;
        updated_entry_t& vertex_entry =
            unique_entries[offset_in_sorted(unique_entries, collection_key_t {collection, vertex_id})];

        for (neighborship_t const neighborship : neighbors(vertex_entry, vertex_role)) {
            updated_entry_t& neighbor_entry = unique_entries[offset_in_sorted(
                unique_entries, collection_key_t {collection, neighborship.neighbor_id})];
            if (vertex_role == ustore_vertex_role_any_k) {
                erase_from_entry(neighbor_entry, ustore_vertex_role_source_k, vertex_id);
                erase_from_entry(neighbor_entry, ustore_vertex_role_target_k, vertex_id);
            }
            else erase_from_entry(neighbor_entry, invert(vertex_role), vertex_id);
        }

        vertex_entry.content = nullptr;
        vertex_entry.length = missing_length_k;
    }

    // Rewritten neighbors lead, removed vertices follow, and the untouched rest is never written
    auto const removed_and_untouched = std::ranges::partition(unique_entries, [](updated_entry_t const& entry) {
        return entry.length != missing_length_k && entry.degree_delta != 0;
    });
    auto const untouched = std::ranges::partition(
        removed_and_untouched, [](updated_entry_t const& entry) { return entry.length == missing_length_k; });
    std::size_t const rewritten_count = static_cast<std::size_t>(removed_and_untouched.begin() -
                                                                 unique_entries.begin());
    std::size_t const removed_count = static_cast<std::size_t>(untouched.begin() - removed_and_untouched.begin());

    // Neighbors are rewritten before the vertices go, so a removal failing at the head leaves them for a retry
    if (status_t const status = write_entries(arguments.database, unique_entries.first(rewritten_count),
                                              arguments.durability, view);
        status.failed())
        return status;
    return remove_entries(arguments.database, unique_entries.subspan(rewritten_count, removed_count),
                          arguments.durability, view);
}

#pragma region Names

/** The canonical spelling of every vertex role. */
constexpr enumerator_spelling<ustore_vertex_role_t> vertex_role_names_k[] {
    {ustore_vertex_role_unknown_k, "unknown"},
    {ustore_vertex_role_source_k, "source"},
    {ustore_vertex_role_target_k, "target"},
    {ustore_vertex_role_any_k, "any"},
};

#pragma endregion Names

} // namespace unum::ustore

void ustore_graph_read_edges(ustore_graph_read_edges_t* arguments) { route<&engine_t::graph_read_edges>(arguments); }
void ustore_graph_write_edges(ustore_graph_write_edges_t* arguments) { route<&engine_t::graph_write_edges>(arguments); }
void ustore_graph_remove_edges(ustore_graph_remove_edges_t* arguments) {
    route<&engine_t::graph_remove_edges>(arguments);
}
void ustore_graph_write_vertices(ustore_graph_write_vertices_t* arguments) {
    route<&engine_t::graph_write_vertices>(arguments);
}
void ustore_graph_remove_vertices(ustore_graph_remove_vertices_t* arguments) {
    route<&engine_t::graph_remove_vertices>(arguments);
}

char const* ustore_vertex_role_name(ustore_vertex_role_t vertex_role) {
    return name_in(vertex_role_names_k, vertex_role);
}

ustore_vertex_role_t ustore_vertex_role_from_name(ustore_bytes_view_t name) {
    return value_in(vertex_role_names_k, name);
}
