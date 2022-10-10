/**
 * @file graph.h
 * @author Ashot Vardanian
 * @date 27 Jun 2022
 *
 * @brief C bindings collections of relations.
 * It essentially extends "ukv.h", to store @b Graphs.
 * Unlike raw values and docs collections, this is an index
 * and the data is transformed into @b Multi-Way @b Inverted-Index.
 *
 * Edges are represented as triplets: (first ID, second ID, edge ID), where the
 * last argument is optional. Multiple edges between same vertices are possible,
 * potentially forming a @b Directed-Multi-Graph, but only if you set edge IDs.
 * Every vertex ID is mapped to an entire list of relations that it forms.
 *
 * @section Supported Graph Kinds
 * 1. @b Undirected (Multi) Graph over vertices of the same collection: (movies.graph)
 * 2. @b Directed (Multi) Graph over vertices of the same collection: (movies.digraph)
 * 3. @b Joining (Multi) Graph linking two different collections: (movies->people.digraph)
 * In the last one, you can't choose directions at the level of edges, only collections.
 * In any one of those collections, storing metadata (a dictionary per each vertex/edge ID)
 * is @b optional. In theory, you may want to store metadata in a different DB, but that
 * would mean loosing ACID guarantees.
 *
 * @section Linking keys across collections
 * It's impossible to foresee every higher-level usage pattern, so certain
 * things are left for users to decide. Generally, if you would have graphs
 * with a lot of metadata, one could structure them as a set of following collections:
 *      * objs.docs
 *      * objs.graph
 * Or if it's a bipartite graph of `person_t` and `movie_t`, like in
 * recommendation systems, one could imagine:
 *      * people.docs
 *      * movies.docs
 *      * people->movies.digraph
 * Which means that in every edge the first ID will be a person and target
 * will be a movie. If you want to keep edge directed in an opposite way, add:
 *      * movies->people.digraph
 *
 * @section Hyper-Graphs
 * If working with Hyper-Graphs (multiple vertices linked by one edge), you are expected
 * to use Undirected Graphs, with vertices and hyper-edges mixed together. You would be
 * differentiating them not by parent collection, but by stored metadata at runtime.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ukv/db.h"

/*********************************************************/
/*****************   Structures & Consts  ****************/
/*********************************************************/

extern ukv_key_t ukv_default_edge_id_k;

/**
 * @brief Every vertex can be either a source or a target in a Directed Graph.
 * When working with undirected graphs, this argument is irrelevant and
 * should be set to `ukv_vertex_role_any_k`. With directed graphs, where
 * source and target can belong to different collections its @b crucial
 * that members of each collection are fixed to be either only sources
 * or only edges.
 */
enum ukv_vertex_role_t {
    ukv_vertex_role_unknown_k = 0,
    ukv_vertex_source_k = 1,
    ukv_vertex_target_k = 2,
    ukv_vertex_role_any_k = 3,
};

typedef uint32_t ukv_vertex_degree_t;
extern ukv_vertex_degree_t ukv_vertex_degree_missing_k;

/*********************************************************/
/*****************	 Primary Functions	  ****************/
/*********************************************************/

/**
 * @brief Finds and extracts all the related edges and
 * neighbor IDs for the provided vertices set. Can also be
 * used to retrieve vertex degrees.
 *
 * @param[in] roles     The roles of passed @p `vertices` in wanted edges.
 * @param[in] options   To request vertex degrees, add the
 *                      @c `ukv_option_read_lengths_k` flag.
 *
 * @section Output Form
 * Similar to `ukv_read`, this function exports a tape-like data
 * to minimize memory copies and colocate the relevant data in the
 * global address space.
 *
 * Every edge will be represented by three @c `ukv_key_t`s:
 * source, target and edge IDs respectively. It's not very
 * space-efficient, but will simplify the iteration over the
 * data in higher-level functions.
 *
 * Missing nodes will be exported with a "degree" set
 * to `ukv_vertex_degree_missing_k`.
 *
 * @section Output Order
 * When only source or target roles are requested, a subsequence of edges
 * related to the same input vertex ID will be sorted by the neighbor ID.
 * When both are requested, first outgoing edges will arrive, sorted by targets.
 * Then the incoming edges, sorted by the source.
 *
 * @section Checking Entity Existence
 * To check if a node or edge is present - a simpler query is possible.
 * The `ukv_read(..., ukv_option_read_lengths_k...)` call will retrieve
 * the length of the entry and if a node is present, it will never be equal
 * to `ukv_vertex_degree_missing_k`. For edges, you will have to check the
 * collection that stores the metadata of the edges.
 */

typedef struct ukv_graph_find_edges_t {

    ukv_database_t const db;
    ukv_error_t* error;
    ukv_transaction_t const transaction = NULL;
    ukv_arena_t* arena = NULL;
    ukv_options_t const options = ukv_options_default_k;
    ukv_size_t const tasks_count = 1;

    // Inputs:
    ukv_collection_t const* collections = NULL;
    ukv_size_t collections_stride = 0;

    ukv_key_t const* vertices_ids = NULL;
    ukv_size_t vertices_stride = 0;

    ukv_vertex_role_t const* roles = NULL;
    ukv_size_t roles_stride = 0;

    ukv_vertex_degree_t** degrees_per_vertex = NULL;
    ukv_key_t** edges_per_vertex = NULL;

} ukv_graph_find_edges_t;

void ukv_graph_find_edges(ukv_graph_find_edges_t*);

/**
 * @brief Inserts edges between provided vertices.
 *
 * @param[in] edge_ids  Optional edge identifiers.
 *                      Necessary for edges storing a lot of metadata.
 *                      Normal graphs would use `ukv_default_edge_id_k`.
 */

typedef struct ukv_graph_upsert_edges_t { //

    ukv_database_t const db;
    ukv_error_t* error;
    ukv_transaction_t const transaction = NULL;
    ukv_arena_t* arena = NULL;
    ukv_options_t const options = ukv_options_default_k;
    ukv_size_t const tasks_count = 1;

    // Inputs:
    ukv_collection_t const* collections = NULL;
    ukv_size_t collections_stride = 0;

    ukv_key_t const* edges_ids = NULL;
    ukv_size_t edges_stride = 0;

    ukv_key_t const* sources_ids = NULL;
    ukv_size_t sources_stride = 0;

    ukv_key_t const* targets_ids = NULL;
    ukv_size_t targets_stride = 0;

} ukv_graph_upsert_edges_t;

void ukv_graph_upsert_edges(ukv_graph_upsert_edges_t*);

/**
 * @brief Removed edges between provided vertices.
 *
 * @param[in] edge_ids  Optional edge identifiers.
 *                      Normal graphs would use `ukv_default_edge_id_k`.
 *                      If NULL is provided for a multi-graph with a deletion
 *                      request, all edges between mentioned nodes will be removed.
 */

typedef struct ukv_graph_remove_edges_t { //

    ukv_database_t const db;
    ukv_error_t* error;
    ukv_transaction_t const transaction = NULL;
    ukv_arena_t* arena = NULL;
    ukv_options_t const options = ukv_options_default_k;
    ukv_size_t const tasks_count = 1;

    // Inputs:
    ukv_collection_t const* collections = NULL;
    ukv_size_t collections_stride = 0;

    ukv_key_t const* edges_ids = NULL;
    ukv_size_t edges_stride = 0;

    ukv_key_t const* sources_ids = NULL;
    ukv_size_t sources_stride = 0;

    ukv_key_t const* targets_ids = NULL;
    ukv_size_t targets_stride = 0;

} ukv_graph_remove_edges_t;

void ukv_graph_remove_edges(ukv_graph_remove_edges_t*);

/**
 * @brief Removes vertices and all connected edges from the graph.
 * @param roles[in] Needed only for @b Joining graphs.
 */

typedef struct ukv_graph_remove_vertices_t { //

    ukv_database_t const db;
    ukv_error_t* error;
    ukv_transaction_t const transaction = NULL;
    ukv_arena_t* arena = NULL;
    ukv_options_t const options = ukv_options_default_k;
    ukv_size_t const tasks_count = 1;

    // Inputs:
    ukv_collection_t const* collections = NULL;
    ukv_size_t collections_stride = 0;

    ukv_key_t const* vertices_ids = NULL;
    ukv_size_t vertices_stride = 0;

    ukv_vertex_role_t const* roles = NULL;
    ukv_size_t roles_stride = 0;

} ukv_graph_remove_vertices_t;

void ukv_graph_remove_vertices(ukv_graph_remove_vertices_t*);

#ifdef __cplusplus
} /* end extern "C" */
#endif
