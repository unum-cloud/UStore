/**
 *  @file include/ustore/graph.h
 *  @author Ash Vardanian
 *  @date June 27, 2022
 *  @brief The binary interface for graph collections, stored as an inverted index from every vertex
 *      to its edges.
 *
 *  An edge is a source, a target and an optional edge identifier, so distinct identifiers let a pair
 *  of vertices share several edges. One collection holds an undirected or a directed graph over its
 *  own vertices, or a joining graph between two collections whose direction is fixed per collection.
 *  Metadata of vertices and edges lives in separate document collections under the same transaction,
 *  such as people and movies next to a people to movies graph. A hypergraph mixes vertices and
 *  hyperedges in one undirected graph and tells them apart by their metadata.
 */

#pragma once

#include "ustore/db.h"

#ifdef __cplusplus
extern "C" {
#endif

#pragma region Constants

/** The edge identifier of an edge that was given none, every bit of it set like @c ustore_key_unknown_k. */
extern ustore_key_t const ustore_default_edge_id_k;

/** The role a vertex plays in an edge, where undirected graphs use @c ustore_vertex_role_any_k.
 *  In a joining graph the members of each collection must stay either sources or targets. */
typedef enum ustore_vertex_role_t {

    /** No role. */
    ustore_vertex_role_unknown_k = 0,

    /** The vertex an edge leaves. */
    ustore_vertex_source_k = 1,

    /** The vertex an edge enters. */
    ustore_vertex_target_k = 2,

    /** Either end of an edge. */
    ustore_vertex_role_any_k = 3,

} ustore_vertex_role_t;

/** The number of edges a vertex connects to. */
typedef uint32_t ustore_vertex_degree_t;

/** The degree reported for a missing vertex. */
extern ustore_vertex_degree_t const ustore_vertex_degree_missing_k;

#pragma endregion Constants

#pragma region Edges

/**
 *  @brief The arguments of @c ustore_graph_find_edges, which exports the degree and the edges of every vertex.
 *
 *  Every edge takes three keys on the tape: the source, the target and the edge identifier. Edges of
 *  one vertex in one role come sorted by neighbor; with both roles the outgoing edges, sorted by
 *  target, precede the incoming ones, sorted by source. Whether a vertex exists is cheaper to ask
 *  from @c ustore_read on the same collection.
 */
typedef struct ustore_graph_find_edges_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_pending_k. */
    ustore_outcome_t* outcome;

    /** Whether the call blocks or returns once queued; zero blocks. */
    ustore_completion_t completion;

    /** What the read sees, where @c NULL is the head. */
    ustore_view_t view;

    /** Number of vertices. */
    ustore_size_t count;

    /** Graph collections, where @c NULL stands for the main collection. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Vertices whose edges are found. */
    ustore_key_t const* vertices;

    /** Bytes between consecutive vertices, where zero repeats the first one. */
    ustore_size_t vertices_stride;

    /** Roles the vertices play in the wanted edges. */
    ustore_vertex_role_t const* roles;

    /** Bytes between consecutive roles, where zero repeats the first one. */
    ustore_size_t roles_stride;

    /** Output for the degree of every vertex, with @c ustore_vertex_degree_missing_k for missing ones. */
    ustore_vertex_degree_t* degrees_per_vertex;

    /** Bytes between consecutive output degrees, never zero. */
    ustore_size_t degrees_per_vertex_stride;

    /** Optional output for the tape of edges, three keys per edge, inside @c arena. */
    ustore_key_t** edges_per_vertex;

    /** Memory for @c edges_per_vertex: reused when it points to an arena, created when it points to @c NULL. */
    ustore_arena_t* arena;

} ustore_graph_find_edges_t;

/**
 *  @brief Finds the edges connected to a batch of vertices.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_graph_find_edges(ustore_graph_find_edges_t* arguments);

/** The arguments of @c ustore_graph_upsert_edges. */
typedef struct ustore_graph_upsert_edges_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_pending_k. */
    ustore_outcome_t* outcome;

    /** Whether the call blocks or returns once queued; zero blocks. */
    ustore_completion_t completion;

    /** Transaction the edits join, where @c NULL writes to the head. */
    ustore_transaction_t transaction;

    /** Number of edges. */
    ustore_size_t count;

    /** Graph collections, where @c NULL stands for the main collection. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Identifiers of the edges. */
    ustore_key_t const* edges;

    /** Bytes between consecutive edge identifiers, where zero repeats the first one. */
    ustore_size_t edges_stride;

    /** Sources of the edges. */
    ustore_key_t const* sources;

    /** Bytes between consecutive sources, where zero repeats the first one. */
    ustore_size_t sources_stride;

    /** Targets of the edges. */
    ustore_key_t const* targets;

    /** Bytes between consecutive targets, where zero repeats the first one. */
    ustore_size_t targets_stride;

    /** How durable the edits are once the call completes at the head; a transaction's commit decides its own. */
    ustore_durability_t durability;

    /** Whether the @c transaction records the vertices it reads and writes for conflicts. */
    ustore_watch_t watch;

} ustore_graph_upsert_edges_t;

/**
 *  @brief Inserts a batch of edges between existing or new vertices.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_graph_upsert_edges(ustore_graph_upsert_edges_t* arguments);

/** The arguments of @c ustore_graph_remove_edges. */
typedef struct ustore_graph_remove_edges_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_pending_k. */
    ustore_outcome_t* outcome;

    /** Whether the call blocks or returns once queued; zero blocks. */
    ustore_completion_t completion;

    /** Transaction the edits join, where @c NULL writes to the head. */
    ustore_transaction_t transaction;

    /** Number of edges. */
    ustore_size_t count;

    /** Graph collections, where @c NULL stands for the main collection. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Identifiers of the edges. */
    ustore_key_t const* edges;

    /** Bytes between consecutive edge identifiers, where zero repeats the first one. */
    ustore_size_t edges_stride;

    /** Sources of the edges. */
    ustore_key_t const* sources;

    /** Bytes between consecutive sources, where zero repeats the first one. */
    ustore_size_t sources_stride;

    /** Targets of the edges. */
    ustore_key_t const* targets;

    /** Bytes between consecutive targets, where zero repeats the first one. */
    ustore_size_t targets_stride;

    /** How durable the edits are once the call completes at the head; a transaction's commit decides its own. */
    ustore_durability_t durability;

    /** Whether the @c transaction records the vertices it reads and writes for conflicts. */
    ustore_watch_t watch;

} ustore_graph_remove_edges_t;

/**
 *  @brief Removes a batch of edges.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_graph_remove_edges(ustore_graph_remove_edges_t* arguments);

#pragma endregion Edges

#pragma region Vertices

/** The arguments of @c ustore_graph_upsert_vertices. */
typedef struct ustore_graph_upsert_vertices_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_pending_k. */
    ustore_outcome_t* outcome;

    /** Whether the call blocks or returns once queued; zero blocks. */
    ustore_completion_t completion;

    /** Transaction the edits join, where @c NULL writes to the head. */
    ustore_transaction_t transaction;

    /** Number of vertices. */
    ustore_size_t count;

    /** Graph collections, where @c NULL stands for the main collection. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Vertices to insert unless present. */
    ustore_key_t const* vertices;

    /** Bytes between consecutive vertices, where zero repeats the first one. */
    ustore_size_t vertices_stride;

    /** How durable the edits are once the call completes at the head; a transaction's commit decides its own. */
    ustore_durability_t durability;

    /** Whether the @c transaction records the vertices it reads and writes for conflicts. */
    ustore_watch_t watch;

} ustore_graph_upsert_vertices_t;

/**
 *  @brief Inserts a batch of vertices without edges, keeping those already present.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_graph_upsert_vertices(ustore_graph_upsert_vertices_t* arguments);

/** The arguments of @c ustore_graph_remove_vertices. */
typedef struct ustore_graph_remove_vertices_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_pending_k. */
    ustore_outcome_t* outcome;

    /** Whether the call blocks or returns once queued; zero blocks. */
    ustore_completion_t completion;

    /** Transaction the edits join, where @c NULL writes to the head. */
    ustore_transaction_t transaction;

    /** Number of vertices. */
    ustore_size_t count;

    /** Graph collections, where @c NULL stands for the main collection. */
    ustore_collection_id_t const* collections;

    /** Bytes between consecutive collections, where zero repeats the first one. */
    ustore_size_t collections_stride;

    /** Vertices to remove. */
    ustore_key_t const* vertices;

    /** Bytes between consecutive vertices, where zero repeats the first one. */
    ustore_size_t vertices_stride;

    /** Roles of the vertices, needed only by joining graphs. */
    ustore_vertex_role_t const* roles;

    /** Bytes between consecutive roles, where zero repeats the first one. */
    ustore_size_t roles_stride;

    /** How durable the edits are once the call completes at the head; a transaction's commit decides its own. */
    ustore_durability_t durability;

    /** Whether the @c transaction records the vertices it reads and writes for conflicts. */
    ustore_watch_t watch;

} ustore_graph_remove_vertices_t;

/**
 *  @brief Removes a batch of vertices together with every edge they belong to.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_graph_remove_vertices(ustore_graph_remove_vertices_t* arguments);

#pragma endregion Vertices

#ifdef __cplusplus
}
#endif
