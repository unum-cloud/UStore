/**
 *  @file src_stub/graph.cpp
 *  @author Ash Vardanian
 *  @date November 16, 2026
 *  @brief The graph calls over the store of @c lib.hpp, filing every vertex and both ends of
 *      every edge as the RAM engine's 49-byte half-edges, so the entries of one vertex form
 *      one ordered run.
 */
#include "keyed.hpp"

using namespace unum::ustore::stub;

/** What @c ustore_graph_vertices_cursor_t names. */
struct ustore_graph_vertices_cursor_handle_t {
    keyed_cursor_t cursor;
};

namespace unum::ustore::stub {

/** The role of task @p index, where null data stands for either end. */
inline ustore_vertex_role_t task_role(ustore_vertex_roles_view_t roles, std::size_t index) noexcept {
    return roles.data ? strided_element(roles.data, roles.stride, index) : ustore_vertex_role_any_k;
}

/** Refuses a role no constant names among @p count tasks. */
inline bool refuse_roles(call_t& call, ustore_vertex_roles_view_t roles, std::size_t count) noexcept {
    for (std::size_t index = 0; roles.data && index != count; ++index) {
        auto const role = std::bit_cast<std::underlying_type_t<ustore_vertex_role_t>>(task_role(roles, index));
        if (role != ustore_vertex_role_source_k && role != ustore_vertex_role_target_k &&
            role != ustore_vertex_role_any_k)
            return call.stop(ustore_status_invalid_argument_k, "Unknown vertex role");
    }
    return false;
}

/** Every entry of the run @p vertex files in @p collection as @p reader sees it: its own,
 *  then its half-edges. */
inline std::vector<std::string> vertex_run(reader_t const& reader, collection_t const& collection,
                                           ustore_key_t const& vertex) {
    std::vector<std::string> run;
    std::string const prefix = bytes_of(vertex);
    std::string position = vertex_entry(vertex);
    bound_t kind = bound_t::inclusive_k;
    while (true) {
        found_entry_t const found = reader.seek(collection, position, kind, ustore_scan_ascending_k);
        if (!found.value || !std::string_view(found.key).starts_with(prefix)) break;
        // A transaction watching the vertex watches every edge it files, and its window for
        // the ones added
        reader.record(collection, found.key);
        run.push_back(found.key);
        position = found.key, kind = bound_t::exclusive_k;
    }
    reader.record_window({bytes_of(collection.id), vertex_entry(vertex), half_edge(vertex, 3, {}, {}), true});
    return run;
}

/** Counts the edges @p vertex has in @p role, appending each as its source, target and identifier
 *  to @p edges when given. */
inline ustore_vertex_degree_t read_vertex_edges(reader_t const& reader, collection_t const& collection,
                                                ustore_key_t const& vertex, ustore_vertex_role_t role,
                                                std::vector<ustore_key_t>* edges) {
    ustore_vertex_degree_t degree = 0;
    for (std::string const& entry : vertex_run(reader, collection, vertex)) {
        auto const direction = static_cast<std::uint8_t>(entry[16]);
        bool const outgoing = direction == 1, incoming = direction == 2;
        bool const wanted = (outgoing && role != ustore_vertex_role_target_k) ||
                            (incoming && role != ustore_vertex_role_source_k);
        if (!wanted) continue;
        ++degree;
        if (!edges) continue;
        ustore_key_t const neighbor = key_of(std::string_view(entry).substr(17, 16));
        edges->push_back(outgoing ? vertex : neighbor);
        edges->push_back(outgoing ? neighbor : vertex);
        edges->push_back(key_of(std::string_view(entry).substr(33, 16)));
    }
    return degree;
}

/** Stages the removal of @p vertex with every edge it belongs to, mirrored at every neighbor. */
inline void stage_vertex_removal(reader_t const& reader, writer_t& writer, collection_t const& collection,
                                 ustore_key_t const& vertex) {
    for (std::string const& entry : vertex_run(reader, collection, vertex)) {
        writer.remove(collection, entry);
        auto const direction = static_cast<std::uint8_t>(entry[16]);
        if (direction != 1 && direction != 2) continue;
        ustore_key_t const neighbor = key_of(std::string_view(entry).substr(17, 16));
        ustore_key_t const edge = key_of(std::string_view(entry).substr(33, 16));
        writer.remove(collection, half_edge(neighbor, direction == 1 ? 2 : 1, vertex, edge));
    }
}

/** The checks every graph change shares, before it claims anything. */
template <typename arguments_type_>
bool check_graph_change(call_t& call, arguments_type_* arguments) {
    if (refuse_write_choices(call, arguments->transaction, arguments->durability, arguments->watch,
                             unwatched_writes_t::admitted_k) ||
        refuse_write_transaction(call, arguments->transaction, arguments->database) ||
        !pre_resolve(call, arguments->database, arguments->transaction, arguments->collections, arguments->count,
                     ustore_modality_graph_k) ||
        refuse_read_only(call, arguments->database))
        return false;
    return call.hold_executor(arguments->executor) && hold_transaction(call, arguments->transaction);
}

/** The collection of task @p index as @p reader sees it, or null once refused. */
template <typename arguments_type_>
collection_t const* task_collection(call_t& call, reader_t const& reader, arguments_type_ const* arguments,
                                    std::size_t index) {
    collection_t const* const collection = reader.find(
        bytes_of(strided_element(arguments->collections.data, arguments->collections.stride, index)));
    if (!collection) call.refuse(ustore_status_not_found_k, "A collection was dropped meanwhile");
    return collection;
}

} // namespace unum::ustore::stub

void ustore_graph_read_edges(ustore_graph_read_edges_t* arguments) {
    if (submitted<&ustore_graph_read_edges>(arguments)) return;
    call_t call(arguments->outcome);
    if (arguments->degrees_per_vertex) *arguments->degrees_per_vertex = nullptr;
    if (arguments->edges_offsets) *arguments->edges_offsets = nullptr;
    if (arguments->edges_per_vertex) *arguments->edges_per_vertex = nullptr;
    if (refuse_database(call, arguments->database)) return;
    std::size_t const count = arguments->count;
    if (!arguments->arena) return call.refuse(ustore_status_invalid_argument_k, "The call needs an arena");
    if (refuse_collections(call, arguments->collections, count)) return;
    if ((count && !arguments->vertices.data) || !arguments->degrees_per_vertex)
        return call.refuse(ustore_status_invalid_argument_k, "The call needs vertices and the degrees output");
    if (arguments->edges_per_vertex && !arguments->edges_offsets)
        return call.refuse(ustore_status_invalid_argument_k, "Edges are read with their offsets");
    if (refuse_roles(call, arguments->roles, count)) return;
    view_spec_t spec;
    std::vector<collection_t const*> resolved;
    if (!open_listing(call, *arguments, ustore_modality_graph_k, spec, resolved)) return;
    if (!call.hold_executor(arguments->executor)) return;
    ustore_arena_handle_t* const arena = claim_arena(
        call, arguments->arena, {arguments->collections.data, arguments->vertices.data, arguments->roles.data});
    if (!arena || !hold_view(call, spec)) return;
    std::vector<ustore_vertex_degree_t> degrees(count, ustore_vertex_degree_missing_k);
    std::vector<ustore_length_t> offsets(count + 1, 0);
    std::vector<ustore_key_t> edges;
    try {
        std::shared_lock lock(arguments->database->mutex);
        reader_t const reader = reader_of(arguments->database, spec, arguments->database->head);
        for (std::size_t index = 0; index != count; ++index) {
            offsets[index + 1] = offsets[index];
            collection_t const* const collection = reader.find(bytes_of(resolved[index]->id));
            if (!collection) return call.refuse(ustore_status_not_found_k, "A collection was dropped meanwhile");
            ustore_key_t const& vertex = strided_element(arguments->vertices.data, arguments->vertices.stride, index);
            if (!reader.get(*collection, vertex_entry(vertex))) continue;
            ustore_vertex_role_t const role = task_role(arguments->roles, index);
            ustore_vertex_degree_t const degree = read_vertex_edges(reader, *collection, vertex, role,
                                                                    arguments->edges_per_vertex ? &edges : nullptr);
            degrees[index] = degree;
            offsets[index + 1] = offsets[index] + (arguments->edges_per_vertex ? degree : 0);
        }
    }
    catch (std::bad_alloc const&) {
        return call.refuse(ustore_status_out_of_memory_k, "Out of memory while reading edges");
    }
    auto* const degrees_out = carve<ustore_vertex_degree_t>(call, *arena, count);
    auto* const offsets_out = arguments->edges_offsets ? carve<ustore_length_t>(call, *arena, count + 1) : nullptr;
    auto* const edges_out = arguments->edges_per_vertex ? carve<ustore_key_t>(call, *arena, edges.size()) : nullptr;
    if (call.failed()) return;
    std::copy(degrees.begin(), degrees.end(), degrees_out);
    if (offsets_out) std::copy(offsets.begin(), offsets.end(), offsets_out);
    if (edges_out) std::copy(edges.begin(), edges.end(), edges_out);
    *arguments->degrees_per_vertex = degrees_out;
    if (arguments->edges_offsets) *arguments->edges_offsets = offsets_out;
    if (arguments->edges_per_vertex) *arguments->edges_per_vertex = edges_out;
}

void ustore_graph_write_edges(ustore_graph_write_edges_t* arguments) {
    if (submitted<&ustore_graph_write_edges>(arguments)) return;
    call_t call(arguments->outcome);
    if (refuse_database(call, arguments->database)) return;
    std::size_t const count = arguments->count;
    if (refuse_collections(call, arguments->collections, count)) return;
    if (count && (!arguments->sources.data || !arguments->targets.data))
        return call.refuse(ustore_status_invalid_argument_k, "Edges need their sources and targets");
    if (!check_graph_change(call, arguments)) return;
    write_through(
        call, arguments->database, arguments->transaction, arguments->watch, arguments->durability,
        [&](reader_t const& reader, writer_t& writer) {
            for (std::size_t index = 0; index != count; ++index) {
                collection_t const* const collection = task_collection(call, reader, arguments, index);
                if (!collection) return;
                ustore_keys_view_t const edges =
                    arguments->edges.data
                        ? ustore_keys_view_t {&strided_element(arguments->edges.data, arguments->edges.stride, index),
                                              0}
                        : ustore_keys_view_t {};
                stage_edges(reader, writer, *collection,
                            {&strided_element(arguments->sources.data, arguments->sources.stride, index), 0},
                            {&strided_element(arguments->targets.data, arguments->targets.stride, index), 0}, edges, 1);
            }
        });
}

void ustore_graph_remove_edges(ustore_graph_remove_edges_t* arguments) {
    if (submitted<&ustore_graph_remove_edges>(arguments)) return;
    call_t call(arguments->outcome);
    if (refuse_database(call, arguments->database)) return;
    std::size_t const count = arguments->count;
    if (refuse_collections(call, arguments->collections, count)) return;
    if (count && (!arguments->sources.data || !arguments->targets.data))
        return call.refuse(ustore_status_invalid_argument_k, "Edges need their sources and targets");
    if (!check_graph_change(call, arguments)) return;
    write_through(
        call, arguments->database, arguments->transaction, arguments->watch, arguments->durability,
        [&](reader_t const& reader, writer_t& writer) {
            for (std::size_t index = 0; index != count; ++index) {
                collection_t const* const collection = task_collection(call, reader, arguments, index);
                if (!collection) return;
                ustore_key_t const& source = strided_element(arguments->sources.data, arguments->sources.stride, index);
                ustore_key_t const& target = strided_element(arguments->targets.data, arguments->targets.stride, index);
                ustore_key_t const edge = arguments->edges.data
                                              ? strided_element(arguments->edges.data, arguments->edges.stride, index)
                                              : ustore_edge_id_default_k;
                writer.remove(*collection, half_edge(source, 1, target, edge));
                writer.remove(*collection, half_edge(target, 2, source, edge));
            }
        });
}

void ustore_graph_write_vertices(ustore_graph_write_vertices_t* arguments) {
    if (submitted<&ustore_graph_write_vertices>(arguments)) return;
    call_t call(arguments->outcome);
    if (refuse_database(call, arguments->database)) return;
    std::size_t const count = arguments->count;
    if (refuse_collections(call, arguments->collections, count)) return;
    if (count && !arguments->vertices.data)
        return call.refuse(ustore_status_invalid_argument_k, "No vertices were provided");
    if (!check_graph_change(call, arguments)) return;
    write_through(call, arguments->database, arguments->transaction, arguments->watch, arguments->durability,
                  [&](reader_t const& reader, writer_t& writer) {
                      for (std::size_t index = 0; index != count; ++index) {
                          collection_t const* const collection = task_collection(call, reader, arguments, index);
                          if (!collection) return;
                          std::string const entry = vertex_entry(
                              strided_element(arguments->vertices.data, arguments->vertices.stride, index));
                          if (!reader.get(*collection, entry)) writer.put(*collection, entry, {});
                      }
                  });
}

void ustore_graph_remove_vertices(ustore_graph_remove_vertices_t* arguments) {
    if (submitted<&ustore_graph_remove_vertices>(arguments)) return;
    call_t call(arguments->outcome);
    if (refuse_database(call, arguments->database)) return;
    std::size_t const count = arguments->count;
    if (refuse_collections(call, arguments->collections, count)) return;
    if (count && !arguments->vertices.data)
        return call.refuse(ustore_status_invalid_argument_k, "No vertices were provided");
    if (refuse_roles(call, arguments->roles, count) || !check_graph_change(call, arguments)) return;
    write_through(call, arguments->database, arguments->transaction, arguments->watch, arguments->durability,
                  [&](reader_t const& reader, writer_t& writer) {
                      for (std::size_t index = 0; index != count; ++index) {
                          collection_t const* const collection = task_collection(call, reader, arguments, index);
                          if (!collection) return;
                          ustore_key_t const& vertex = strided_element(arguments->vertices.data,
                                                                       arguments->vertices.stride, index);
                          if (!arguments->end_vertices.data) {
                              stage_vertex_removal(reader, writer, *collection, vertex);
                              continue;
                          }
                          ustore_key_t const& end = strided_element(arguments->end_vertices.data,
                                                                    arguments->end_vertices.stride, index);
                          walk_t walk = keyed_walk(ustore_modality_graph_k, &vertex, &end, ustore_scan_ascending_k);
                          record_walk(reader, *collection, walk);
                          std::vector<entry_t> listed;
                          fetch(reader, *collection, walk, static_cast<std::size_t>(-1), listed);
                          for (entry_t const& entry : listed)
                              stage_vertex_removal(reader, writer, *collection, key_of(entry.key));
                      }
                  });
}

void ustore_graph_scan_vertices(ustore_graph_scan_vertices_t* arguments) {
    if (submitted<&ustore_graph_scan_vertices>(arguments)) return;
    call_t call(arguments->outcome);
    if (!check_scan(call, arguments)) return;
    scan_keyed(call, arguments, ustore_modality_graph_k, nullptr, {});
}

void ustore_graph_vertices_cursor_init(ustore_graph_vertices_cursor_init_t* arguments) {
    call_t call(arguments->outcome);
    if (arguments->cursor) *arguments->cursor = nullptr;
    if (refuse_database(call, arguments->database)) return;
    if (!arguments->cursor)
        return call.refuse(ustore_status_invalid_argument_k, "The call needs an output for the cursor");
    if (refuse_direction(call, arguments->direction) || refuse_predicate(call, arguments->predicate)) return;
    auto* const cursor = new (std::nothrow) ustore_graph_vertices_cursor_handle_t {};
    if (!cursor) return call.refuse(ustore_status_out_of_memory_k, "Failed to allocate the cursor");
    if (!open_keyed_cursor(call, arguments, ustore_modality_graph_k, cursor->cursor)) return delete cursor;
    *arguments->cursor = cursor;
}

void ustore_graph_vertices_cursor_next(ustore_graph_vertices_cursor_next_t* arguments) {
    if (submitted<&ustore_graph_vertices_cursor_next>(arguments)) return;
    call_t call(arguments->outcome);
    keyed_cursor_t* const cursor = arguments->cursor ? &arguments->cursor->cursor : nullptr;
    ustore_arena_handle_t* const arena = open_pull(call, arguments, cursor);
    if (!arena) return;
    std::vector<entry_t> listed;
    if (!pull_keyed(call, arguments, *cursor, listed)) return;
    write_page_keys(call, arguments, *arena, listed);
}

void ustore_graph_vertices_cursor_free(ustore_graph_vertices_cursor_t cursor) {
    if (!cursor) return;
    close_keyed_cursor(cursor->cursor);
    delete cursor;
}

void ustore_graph_sample_vertices(ustore_graph_sample_vertices_t* arguments) {
    if (submitted<&ustore_graph_sample_vertices>(arguments)) return;
    sample_keyed(arguments, ustore_modality_graph_k);
}

void ustore_graph_measure_vertices(ustore_graph_measure_vertices_t* arguments) {
    if (submitted<&ustore_graph_measure_vertices>(arguments)) return;
    measure_keyed(arguments, ustore_modality_graph_k);
}
