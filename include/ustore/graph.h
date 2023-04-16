/**
 * @file graph.h
 * @author Ashot Vardanian
 * @date 27 Jun 2022
 * @addtogroup C
 *
 * @brief Binary Interface Standard for @b Graph collections.
 *
 * It essentially extends "ustore.h", to store @b Graphs.
 * Unlike raw values and docs collections, this is an index
 * and the data is transformed into @b Multi-Way @b Inverted-Index.
 *
 * Edges are represented as triplets: (first ID, second ID, edge ID), where the
 * last argument is optional. Multiple edges between same vertices are possible,
 * potentially forming a @b Directed-Multi-Graph, but only if you set edge IDs.
 * Every vertex ID is mapped to an entire list of relations that it forms.
 *
 * ## Supported Graph Kinds
 *
 * 1. @b Undirected (Multi) Graph over vertices of the same collection: (movies.graph)
 * 2. @b Directed (Multi) Graph over vertices of the same collection: (movies.digraph)
 * 3. @b Joining (Multi) Graph linking two different collections: (movies->people.digraph)
 * In the last one, you can't choose directions at the level of edges, only collections.
 * In any one of those collections, storing metadata (a dictionary per each vertex/edge ID)
 * is @b optional. In theory, you may want to store metadata in a different DB, but that
 * would mean loosing ACID guarantees.
 *
 * ## Linking keys across collections
 *
 * It's impossible to foresee every higher-level usage pattern, so certain
 * things are left for users to decide. Generally, if you would have graphs
 * with a lot of metadata, one could structure them as a set of following collections:
 *  - objs.docs
 *  - objs.graph
 *
 * Or if it's a bipartite graph of `person_t` and `movie_t`, like in
 * recommendation systems, one could imagine:
 *  - people.docs
 *  - movies.docs
 *  - people->movies.digraph
 *
 * Which means that in every edge the first ID will be a person and target
 * will be a movie. If you want to keep edge directed in an opposite way, add:
 *  - movies->people.digraph
 *
 * ## Hyper-Graphs
 *
 * If working with Hyper-Graphs (multiple vertices linked by one edge), you are expected
 * to use Undirected Graphs, with vertices and hyper-edges mixed together. You would be
 * differentiating them not by parent collection, but by stored metadata at runtime.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ustore/db.h"

/*********************************************************/
/*****************   Structures & Consts  ****************/
/*********************************************************/

extern ustore_key_t ustore_default_edge_id_k;

/**
 * @brief Every vertex can be either a source or a target in a Directed Graph.
 *
 * When working with undirected graphs, this argument is irrelevant and
 * should be set to `::ustore_vertex_role_any_k`. With directed graphs, where
 * source and target can belong to different collections its @b crucial
 * that members of each collection are fixed to be either only sources
 * or only edges.
 */
typedef enum ustore_vertex_role_t {
    ustore_vertex_role_unknown_k = 0,
    ustore_vertex_source_k = 1,
    ustore_vertex_target_k = 2,
    ustore_vertex_role_any_k = 3,
} ustore_vertex_role_t;

/**
 * @brief Type to describe the number of edges a vertex connects to.
 */
typedef uint32_t ustore_vertex_degree_t;
extern ustore_vertex_degree_t ustore_vertex_degree_missing_k;

/*********************************************************/
/*****************	 Primary Functions	  ****************/
/*********************************************************/

/**
 * @brief Finds all the edges, connected to given vertices.
 * @see `ustore_graph_find_edges()`.
 *
 * ## Output Form
 *
 * Similar to `ustore_read()`, this function exports a tape-like data
 * to minimize memory copies and colocate the relevant data in the
 * global address space.
 *
 * Every edge will be represented by @b three @c ustore_key_t's:
 * source, target and edge IDs respectively. It's not very
 * space-efficient, but will simplify the iteration over the
 * data in higher-level functions.
 *
 * Missing nodes will be exported with a "degree" set
 * to `::ustore_vertex_degree_missing_k`.
 *
 * ## Output Order
 *
 * When only source or target roles are requested, a subsequence of edges
 * related to the same input vertex ID will be sorted by the neighbor ID.
 * When both are requested:
 *
 * - First outgoing edges will arrive, sorted by targets.
 * - Then the incoming edges, sorted by the source.
 *
 * ## Checking Entity Existence
 *
 * To check if a node or edge is present - a simpler query is possible.
 * The `ustore_read()` on these same `collections` will return the presence
 * indicators for vertices. For edges, you will have to check the
 * collection that stores the metadata of the edges.
 */
typedef struct ustore_graph_find_edges_t {

    /// @name Context
    /// @{

    /** @brief Already open database instance. */
    ustore_database_t db;
    /** @brief Pointer to exported error message. */
    ustore_error_t* error;
    /** @brief The transaction in which the operation will be watched. */
    ustore_transaction_t transaction;
    /** @brief A snapshot captures a point-in-time view of the DB at the time it's created. */
    ustore_snapshot_t snapshot;
    /** @brief Reusable memory handle. */
    ustore_arena_t* arena;
    /** @brief Read options. @see `ustore_read_t`. */
    ustore_options_t options;

    /// @}
    /// @name Inputs
    /// @{

    ustore_size_t tasks_count;

    ustore_collection_t const* collections;
    ustore_size_t collections_stride;

    ustore_key_t const* vertices;
    ustore_size_t vertices_stride;

    /** @brief The roles of passed `vertices` within edges. */
    ustore_vertex_role_t const* roles;
    /** @brief Step between `roles`. */
    ustore_size_t roles_stride;

    /// @}
    /// @name Outputs
    /// @{

    ustore_vertex_degree_t** degrees_per_vertex;
    ustore_key_t** edges_per_vertex;

    /// @}

} ustore_graph_find_edges_t;

/**
 * @brief Finds all the edges, connected to given vertices.
 * @see `ustore_graph_find_edges_t`.
 */
void ustore_graph_find_edges(ustore_graph_find_edges_t*);

/**
 * @brief Inserts edges between provided vertices.
 * @see `ustore_graph_upsert_edges()`.
 */
typedef struct ustore_graph_upsert_edges_t {

    /// @name Context
    /// @{

    /** @brief Already open database instance. */
    ustore_database_t db;
    /** @brief Pointer to exported error message. */
    ustore_error_t* error;
    /** @brief The transaction in which the operation will be watched. */
    ustore_transaction_t transaction;
    /** @brief Reusable memory handle. */
    ustore_arena_t* arena;
    /** @brief Read and Write options. @see `ustore_read_t`, `ustore_write_t`. */
    ustore_options_t options;

    /// @}
    /// @name Inputs
    /// @{
    ustore_size_t tasks_count;

    ustore_collection_t const* collections;
    ustore_size_t collections_stride;

    ustore_key_t const* edges_ids;
    ustore_size_t edges_stride;

    ustore_key_t const* sources_ids;
    ustore_size_t sources_stride;

    ustore_key_t const* targets_ids;
    ustore_size_t targets_stride;

    /// @}

} ustore_graph_upsert_edges_t;

/**
 * @brief Inserts edges between provided vertices.
 * @see `ustore_graph_upsert_edges_t`.
 */
void ustore_graph_upsert_edges(ustore_graph_upsert_edges_t*);

/**
 * @brief Removed edges between provided vertices.
 * @see `ustore_graph_remove_edges()`.
 */
typedef struct ustore_graph_remove_edges_t { //

    /// @name Context
    /// @{

    /** @brief Already open database instance. */
    ustore_database_t db;
    /** @brief Pointer to exported error message. */
    ustore_error_t* error;
    /** @brief The transaction in which the operation will be watched. */
    ustore_transaction_t transaction;
    /** @brief Reusable memory handle. */
    ustore_arena_t* arena;
    /** @brief Read and Write options. @see `ustore_read_t`, `ustore_write_t`. */
    ustore_options_t options;

    /// @}
    /// @name Inputs
    /// @{

    ustore_size_t tasks_count;

    ustore_collection_t const* collections;
    ustore_size_t collections_stride;

    ustore_key_t const* edges_ids;
    ustore_size_t edges_stride;

    ustore_key_t const* sources_ids;
    ustore_size_t sources_stride;

    ustore_key_t const* targets_ids;
    ustore_size_t targets_stride;

    /// @}

} ustore_graph_remove_edges_t;

/**
 * @brief Removed edges between provided vertices.
 * @see `ustore_graph_remove_edges_t`.
 */
void ustore_graph_remove_edges(ustore_graph_remove_edges_t*);

/**
 * @brief Upsert vertices.
 * @see `ustore_graph_upsert_vertices()`.
 */
typedef struct ustore_graph_upsert_vertices_t { //

    /// @name Context
    /// @{

    /** @brief Already open database instance. */
    ustore_database_t db;
    /** @brief Pointer to exported error message. */
    ustore_error_t* error;
    /** @brief The transaction in which the operation will be watched. */
    ustore_transaction_t transaction;
    /** @brief Reusable memory handle. */
    ustore_arena_t* arena;
    /** @brief Read and Write options. @see `ustore_read_t`, `ustore_write_t`. */
    ustore_options_t options;

    /// @}
    /// @name Inputs
    /// @{

    ustore_size_t tasks_count;

    ustore_collection_t const* collections;
    ustore_size_t collections_stride;

    ustore_key_t const* vertices;
    ustore_size_t vertices_stride;

    /// @}

} ustore_graph_upsert_vertices_t;

/**
 * @brief Upsert vertices.
 * @see `ustore_graph_upsert_vertices_t`.
 */
void ustore_graph_upsert_vertices(ustore_graph_upsert_vertices_t*);

/**
 * @brief Removes vertices and all related edges from the graph.
 * @see `ustore_graph_remove_vertices()`.
 */
typedef struct ustore_graph_remove_vertices_t { //

    /// @name Context
    /// @{

    /** @brief Already open database instance. */
    ustore_database_t db;
    /** @brief Pointer to exported error message. */
    ustore_error_t* error;
    /** @brief The transaction in which the operation will be watched. */
    ustore_transaction_t transaction;
    /** @brief Reusable memory handle. */
    ustore_arena_t* arena;
    /** @brief Read and Write options. @see `ustore_read_t`, `ustore_write_t`. */
    ustore_options_t options;

    /// @}
    /// @name Inputs
    /// @{

    ustore_size_t tasks_count;

    ustore_collection_t const* collections;
    ustore_size_t collections_stride;

    ustore_key_t const* vertices;
    ustore_size_t vertices_stride;

    /** @brief Needed only for @b Joining graphs. */
    ustore_vertex_role_t const* roles;
    /** @brief Step between `roles`. */
    ustore_size_t roles_stride;

    /// @}

} ustore_graph_remove_vertices_t;

/**
 * @brief Removes vertices and all related edges from the graph.
 * @see `ustore_graph_remove_vertices_t`.
 */
void ustore_graph_remove_vertices(ustore_graph_remove_vertices_t*);

#ifdef __cplusplus
} /* end extern "C" */
#endif
