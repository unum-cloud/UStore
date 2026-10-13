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

#ifndef UNUM_USTORE_GRAPH_H
#define UNUM_USTORE_GRAPH_H

#include "ustore/db.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The edge identifier of an edge that was given none, every bit of it set like @c ustore_key_unknown_k. */
static ustore_key_t const ustore_edge_id_default_k = {
    {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}}};

/** The role a vertex plays in an edge, where undirected graphs use @c ustore_vertex_role_any_k.
 *  In a joining graph the members of each collection must stay either sources or targets. */
typedef enum ustore_vertex_role_t {

    /** No role. */
    ustore_vertex_role_unknown_k = 0,

    /** The vertex an edge leaves. */
    ustore_vertex_role_source_k = 1,

    /** The vertex an edge enters. */
    ustore_vertex_role_target_k = 2,

    /** Either end of an edge. */
    ustore_vertex_role_any_k = 3,

} ustore_vertex_role_t;

/** The canonical spelling of a role, as @c ustore_vertex_role_source_k spells "source", or "unrecognized". */
USTORE_EXPORT char const* ustore_vertex_role_name(ustore_vertex_role_t vertex_role);

/** Reads a role back from its canonical spelling; @c ustore_vertex_role_unknown_k when unrecognized. */
USTORE_EXPORT ustore_vertex_role_t ustore_vertex_role_from_name(ustore_bytes_view_t name);

/** A strided run of roles, one per task, where @c NULL data stands for @c ustore_vertex_role_any_k. */
typedef struct ustore_vertex_roles_view_t {

    /** First role of the run. */
    ustore_vertex_role_t const* data;

    /** Bytes between consecutive roles, where zero repeats the first one. */
    ustore_size_t stride;

} ustore_vertex_roles_view_t;

/** The number of edges a vertex connects to. */
typedef uint32_t ustore_vertex_degree_t;

/** The degree reported for a missing vertex. */
static ustore_vertex_degree_t const ustore_vertex_degree_missing_k = 0xFFFFFFFFu;

/**
 *  @brief The arguments of @c ustore_graph_read_edges, which exports the degree and the edges of every vertex.
 *
 *  Every edge takes three keys on the tape: the source, the target and the edge identifier, and the
 *  edges of vertex @c i span from edge @c edges_offsets[i] up to edge @c edges_offsets[i + 1]. Edges
 *  of one vertex in one role come sorted by neighbor; with both roles the outgoing edges, sorted by
 *  target, precede the incoming ones, sorted by source. Whether a vertex exists is cheaper to ask
 *  from @c ustore_blobs_read on the same collection.
 */
typedef struct ustore_graph_read_edges_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** What the call sees, where @c NULL is the head. */
    ustore_view_t view;

    /** Number of vertices. */
    ustore_size_t count;

    /** Graph collections, where @c NULL data stands for the main collection. */
    ustore_collections_view_t collections;

    /** Vertices whose edges are read. */
    ustore_keys_view_t vertices;

    /** Roles the vertices play in the wanted edges. */
    ustore_vertex_roles_view_t roles;

    /** Output for the degree of every vertex, with @c ustore_vertex_degree_missing_k for missing ones, inside
     *  @c arena. */
    ustore_vertex_degree_t const** degrees_per_vertex;

    /** Optional output for @c count plus one offsets into @c edges_per_vertex, counted in edges, where a missing
     *  vertex spans none, inside @c arena; required with @c edges_per_vertex. */
    ustore_length_t const** edges_offsets;

    /** Optional output for the tape of edges, three keys per edge, inside @c arena. */
    ustore_key_t const** edges_per_vertex;

    /** Rewound by this call, then holding every output it writes; @c NULL is refused. */
    ustore_arena_t arena;

} ustore_graph_read_edges_t;

/**
 *  @brief Reads the edges connected to a batch of vertices.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_graph_read_edges(ustore_graph_read_edges_t* arguments);

/** The arguments of @c ustore_graph_write_edges. */
typedef struct ustore_graph_write_edges_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** Transaction the edits join, where @c NULL writes to the head. */
    ustore_transaction_t transaction;

    /** Number of edges. */
    ustore_size_t count;

    /** Graph collections, where @c NULL data stands for the main collection. */
    ustore_collections_view_t collections;

    /** Identifiers of the edges, where @c NULL data gives each @c ustore_edge_id_default_k. */
    ustore_keys_view_t edges;

    /** Sources of the edges. */
    ustore_keys_view_t sources;

    /** Targets of the edges. */
    ustore_keys_view_t targets;

    /** How durable the edits are once the call finishes at the head; a transaction's commit decides its own. */
    ustore_durability_t durability;

    /** Whether the @c transaction records the vertices it reads and writes for conflicts. */
    ustore_watch_t watch;

} ustore_graph_write_edges_t;

/**
 *  @brief Writes a batch of edges between existing or new vertices.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_graph_write_edges(ustore_graph_write_edges_t* arguments);

/** The arguments of @c ustore_graph_remove_edges. */
typedef struct ustore_graph_remove_edges_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** Transaction the edits join, where @c NULL writes to the head. */
    ustore_transaction_t transaction;

    /** Number of edges. */
    ustore_size_t count;

    /** Graph collections, where @c NULL data stands for the main collection. */
    ustore_collections_view_t collections;

    /** Identifiers of the edges, where @c NULL data gives each @c ustore_edge_id_default_k. */
    ustore_keys_view_t edges;

    /** Sources of the edges. */
    ustore_keys_view_t sources;

    /** Targets of the edges. */
    ustore_keys_view_t targets;

    /** How durable the edits are once the call finishes at the head; a transaction's commit decides its own. */
    ustore_durability_t durability;

    /** Whether the @c transaction records the vertices it reads and writes for conflicts. */
    ustore_watch_t watch;

} ustore_graph_remove_edges_t;

/**
 *  @brief Removes a batch of edges.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_graph_remove_edges(ustore_graph_remove_edges_t* arguments);

/** The arguments of @c ustore_graph_write_vertices. */
typedef struct ustore_graph_write_vertices_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** Transaction the edits join, where @c NULL writes to the head. */
    ustore_transaction_t transaction;

    /** Number of vertices. */
    ustore_size_t count;

    /** Graph collections, where @c NULL data stands for the main collection. */
    ustore_collections_view_t collections;

    /** Vertices to insert unless present. */
    ustore_keys_view_t vertices;

    /** How durable the edits are once the call finishes at the head; a transaction's commit decides its own. */
    ustore_durability_t durability;

    /** Whether the @c transaction records the vertices it reads and writes for conflicts. */
    ustore_watch_t watch;

} ustore_graph_write_vertices_t;

/**
 *  @brief Writes a batch of vertices without edges, keeping those already present.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_graph_write_vertices(ustore_graph_write_vertices_t* arguments);

/** The arguments of @c ustore_graph_remove_vertices. */
typedef struct ustore_graph_remove_vertices_t {

    /** An open database. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** Transaction the edits join, where @c NULL writes to the head. */
    ustore_transaction_t transaction;

    /** Number of vertices. */
    ustore_size_t count;

    /** Graph collections, where @c NULL data stands for the main collection. */
    ustore_collections_view_t collections;

    /** Vertices to remove. */
    ustore_keys_view_t vertices;

    /** Roles of the vertices, needed only by joining graphs. */
    ustore_vertex_roles_view_t roles;

    /** How durable the edits are once the call finishes at the head; a transaction's commit decides its own. */
    ustore_durability_t durability;

    /** Whether the @c transaction records the vertices it reads and writes for conflicts. */
    ustore_watch_t watch;

} ustore_graph_remove_vertices_t;

/**
 *  @brief Removes a batch of vertices together with every edge they belong to.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_graph_remove_vertices(ustore_graph_remove_vertices_t* arguments);

#ifdef __cplusplus
}
#endif

#endif // UNUM_USTORE_GRAPH_H
