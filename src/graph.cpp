/**
 *  @file src/graph.cpp
 *  @author Ash Vardanian
 *  @date January 11, 2023
 *  @brief Implements <tt>c/ustore/graph.h</tt>: routes every graph call to the graph verbs of the engine serving its
 *      database, and spells the vertex roles.
 */

#include "ustore/graph.h" // `ustore_vertex_role_t`

#include "engine.hpp" // `engine_t`, `graph_verbs<home_t::host_k>`
#include "lib.hpp"    // `route`, `open_cursor`, `free_cursor`, `enumerator_spelling`, `name_in`, `value_in`

namespace unum::ustore {

#pragma region Names

/** The canonical spelling of every vertex role. */
constexpr enumerator_spelling<ustore_vertex_role_t> vertex_role_names_k[] {
    {ustore_vertex_role_unknown_k, "unknown"},
    {ustore_vertex_role_source_k, "source"},
    {ustore_vertex_role_target_k, "target"},
    {ustore_vertex_role_any_k, "any"},
};

#pragma endregion Names

status_t pull(engine_t const& engine, ustore_graph_vertices_cursor_next_t const& arguments,
              ustore_graph_vertices_cursor_handle_t& cursor, view_t const& view) noexcept {
    if (!arguments.keys || !arguments.written)
        return {status_code_t::invalid_argument_k, "Need outputs for the keys and the count written"};
    keyed_survivors_t survivors {arena_allocator<ustore_key_t> {view.scratch}};
    std::variant<ustore_key_t, exhausted_t> resume;
    if (status_t const status = list_cursor_keys<ustore_graph_scan_vertices_t>(
            engine, arguments.database, cursor, arguments.wanted, arguments.arena, view, survivors, resume);
        status.failed())
        return status;
    arena_rewind(arguments.arena);
    std::size_t const written = survivors.size();
    auto [keys, keys_status] = arena_copy<ustore_key_t>(arguments.arena, survivors);
    if (keys_status.failed()) return keys_status;
    if (!progress_t {arguments.progress, arguments.progress_state, 1}.advance(1)) return progress_cancelled_k;
    cursor.position = resume;
    *arguments.keys = keys.data();
    *arguments.written = written;
    return {};
}

} // namespace unum::ustore

void ustore_graph_read_edges(ustore_graph_read_edges_t* arguments) { route(arguments); }
void ustore_graph_write_edges(ustore_graph_write_edges_t* arguments) { route(arguments); }
void ustore_graph_remove_edges(ustore_graph_remove_edges_t* arguments) { route(arguments); }
void ustore_graph_write_vertices(ustore_graph_write_vertices_t* arguments) { route(arguments); }
void ustore_graph_remove_vertices(ustore_graph_remove_vertices_t* arguments) { route(arguments); }
void ustore_graph_scan_vertices(ustore_graph_scan_vertices_t* arguments) { route(arguments); }

void ustore_graph_vertices_cursor_next(ustore_graph_vertices_cursor_next_t* arguments) { route(arguments); }

void ustore_graph_vertices_cursor_init(ustore_graph_vertices_cursor_init_t* arguments) {
    auto [cursor, status] =
        open_cursor<ustore_graph_vertices_cursor_handle_t, graph_verbs, ustore_graph_scan_vertices_t>(*arguments);
    if (status.failed()) return finish(*arguments->outcome, status);
    cursor->position = arguments->start_key;
    *arguments->cursor = cursor;
    finish(*arguments->outcome, {});
}

void ustore_graph_vertices_cursor_free(ustore_graph_vertices_cursor_t cursor) { free_cursor(cursor); }

char const* ustore_vertex_role_name(ustore_vertex_role_t vertex_role) {
    return name_in(vertex_role_names_k, vertex_role);
}

ustore_vertex_role_t ustore_vertex_role_from_name(ustore_bytes_view_t name) {
    return value_in(vertex_role_names_k, name);
}
