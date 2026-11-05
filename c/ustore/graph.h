/**
 *  @file c/ustore/graph.h
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
 *  from @c ustore_blobs_read on the same collection. On a VRAM executor every output lands in the
 *  call's device arena as it is, and each key of the edges exports as its own Arrow column through
 *  @c ustore_keys_view_to_arrow, strided by three keys, which copies it dense on the device.
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

    /** Graph collections. */
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
 *
 *  @par Statuses:
 *  @c ustore_status_invalid_argument_k means the @c arena, @c collections, @c vertices or degrees
 *  output is missing, the edges are asked without their offsets, the @c view was made by neither
 *  view constructor or belongs to another engine, a watching read goes through a staged
 *  transaction, or a collection holds another modality.
 *
 *  - @c ustore_status_device_k The arena lives where another executor computes.
 *  - @c ustore_status_contended_k The arena or the transaction the @c view reads through serves
 *    another call, the executor's deferred table is full, a callback blocks on the executor running
 *    it, or an NVMe value keeps changing size across reads.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL or an NVMe transfer was cancelled.
 *  - @c ustore_status_not_found_k A collection does not exist or was dropped meanwhile.
 *  - @c ustore_status_malformed_k A stored value is no vertex.
 *  - @c ustore_status_capacity_exhausted_k Every shared NVMe channel is taken.
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

    /** Graph collections. */
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
 *
 *  @par Statuses:
 *  - @c ustore_status_contended_k The @c transaction serves another call, the executor's deferred
 *    table is full, a callback blocks on the executor running it, or an NVMe value keeps changing
 *    size across reads.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL or an NVMe transfer was cancelled.
 *  - @c ustore_status_read_only_k The database was opened read-only.
 *  - @c ustore_status_invalid_argument_k The @c collections, @c sources or @c targets are missing,
 *    the @c transaction belongs to another engine or is staged, the @c durability or @c watch is
 *    unknown, a flushed write names a @c transaction, @c ustore_watch_none_k comes without one on
 *    RocksDB and LevelDB, or a collection holds another modality.
 *  - @c ustore_status_unsupported_k The executor computes on a device.
 *  - @c ustore_status_not_found_k A collection does not exist or was dropped meanwhile.
 *  - @c ustore_status_malformed_k A stored value is no vertex, or a vertex outgrows what its NVMe
 *    collection admits.
 *  - @c ustore_status_write_conflict_k Concurrent commits kept conflicting, or a staged RocksDB
 *    transaction holds a vertex.
 *  - @c ustore_status_capacity_exhausted_k The changes outgrow the NVMe transaction's buffer, or
 *    every shared NVMe channel is taken.
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

    /** Graph collections. */
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
 *
 *  @par Statuses:
 *  - @c ustore_status_contended_k The @c transaction serves another call, the executor's deferred
 *    table is full, a callback blocks on the executor running it, or an NVMe value keeps changing
 *    size across reads.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL or an NVMe transfer was cancelled.
 *  - @c ustore_status_read_only_k The database was opened read-only.
 *  - @c ustore_status_invalid_argument_k The @c collections, @c sources or @c targets are missing,
 *    the @c transaction belongs to another engine or is staged, the @c durability or @c watch is
 *    unknown, a flushed remove names a @c transaction, @c ustore_watch_none_k comes without one on
 *    RocksDB and LevelDB, or a collection holds another modality.
 *  - @c ustore_status_unsupported_k The executor computes on a device.
 *  - @c ustore_status_not_found_k A collection does not exist or was dropped meanwhile.
 *  - @c ustore_status_malformed_k A stored value is no vertex, or a vertex outgrows what its NVMe
 *    collection admits.
 *  - @c ustore_status_write_conflict_k Concurrent commits kept conflicting, or a staged RocksDB
 *    transaction holds a vertex.
 *  - @c ustore_status_capacity_exhausted_k The changes outgrow the NVMe transaction's buffer, or
 *    every shared NVMe channel is taken.
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

    /** Graph collections. */
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
 *
 *  @par Statuses:
 *  - @c ustore_status_contended_k The @c transaction serves another call, the executor's deferred
 *    table is full, a callback blocks on the executor running it, or an NVMe value keeps changing
 *    size across reads.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL or an NVMe transfer was cancelled.
 *  - @c ustore_status_read_only_k The database was opened read-only.
 *
 *  @c ustore_status_invalid_argument_k means the @c collections or @c vertices are missing, the
 *  @c transaction belongs to another engine or is staged, the @c durability or @c watch is
 *  unknown, a flushed write names a @c transaction, @c ustore_watch_none_k comes without one on
 *  RocksDB and LevelDB, or a collection holds another modality.
 *
 *  - @c ustore_status_unsupported_k The executor computes on a device.
 *  - @c ustore_status_not_found_k A collection does not exist or was dropped meanwhile.
 *  - @c ustore_status_malformed_k A vertex outgrows what its NVMe collection admits.
 *  - @c ustore_status_write_conflict_k Concurrent commits kept conflicting, or a staged RocksDB
 *    transaction holds a vertex.
 *  - @c ustore_status_capacity_exhausted_k The changes outgrow the NVMe transaction's buffer, or
 *    every shared NVMe channel is taken.
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

    /** Graph collections. */
    ustore_collections_view_t collections;

    /** Vertices to remove, or the inclusive starts of ranges when @c end_vertices is given. */
    ustore_keys_view_t vertices;

    /** Exclusive ends of the ranges of vertices to remove with their edges, or a zeroed view to
     *  remove @c vertices alone. */
    ustore_keys_view_t end_vertices;

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
 *
 *  @par Statuses:
 *  - @c ustore_status_contended_k The @c transaction serves another call, the executor's deferred
 *    table is full, a callback blocks on the executor running it, or an NVMe value keeps changing
 *    size across reads.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL or an NVMe transfer was cancelled.
 *  - @c ustore_status_read_only_k The database was opened read-only.
 *
 *  @c ustore_status_invalid_argument_k means the @c collections or @c vertices are missing, the
 *  @c transaction belongs to another engine or is staged, the @c durability or @c watch is
 *  unknown, a flushed remove names a @c transaction, @c ustore_watch_none_k comes without one on
 *  RocksDB and LevelDB, or a collection holds another modality.
 *
 *  - @c ustore_status_unsupported_k The executor computes on a device.
 *  - @c ustore_status_not_found_k A collection does not exist or was dropped meanwhile.
 *  - @c ustore_status_malformed_k A stored value is no vertex, or a vertex outgrows what its NVMe
 *    collection admits.
 *  - @c ustore_status_write_conflict_k Concurrent commits kept conflicting, or a staged RocksDB
 *    transaction holds a vertex.
 *  - @c ustore_status_capacity_exhausted_k The changes outgrow the NVMe transaction's buffer, or
 *    every shared NVMe channel is taken.
 */
USTORE_EXPORT void ustore_graph_remove_vertices(ustore_graph_remove_vertices_t* arguments);

/**
 *  @brief The arguments of @c ustore_graph_scan_vertices, which lists up to @c wanted vertices from a start key in
 *      every graph collection.
 *
 *  Edges are not exported, so @c ustore_graph_read_edges follows the scan. Consistent iterators over concurrent state
 *  are costly, so scans paginate instead.
 */
typedef struct ustore_graph_scan_vertices_t {

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

    /** Number of scans. */
    ustore_size_t count;

    /** Graph collections. */
    ustore_collections_view_t collections;

    /** Vertex every scan starts from, inclusive, where @c NULL data starts at the first vertex in
     *  the @c direction. */
    ustore_keys_view_t start_keys;

    /** Key every scan stops before, exclusive, or a zeroed view for no bound. */
    ustore_keys_view_t end_keys;

    /** Ascending, the default, or descending from the start key. */
    ustore_scan_direction_t direction;

    /** Optional verdict on the listed vertices, asked on the host once per page as its @c cost says. */
    ustore_predicate_t predicate;

    /** Output for @c count plus one offsets into @c keys, the last one closing the final scan, inside @c arena. */
    ustore_length_t const** offsets;

    /** Output for the found vertices, scan after scan, inside @c arena. */
    ustore_key_t const** keys;

    /** Rewound by this call, then holding every output it writes; @c NULL is refused. */
    ustore_arena_t arena;

    /** Most vertices every scan returns. */
    ustore_size_t wanted;

    /** In what order the vertices come. */
    ustore_scan_mode_t mode;

    /** Optional callback reporting the scans finished. */
    ustore_progress_callback_t progress;

    /** Opaque pointer passed to @c progress. */
    void* progress_state;

} ustore_graph_scan_vertices_t;

/**
 *  @brief Scans a batch of vertex ranges.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  @c ustore_status_invalid_argument_k means the @c arena, @c collections or an output is missing,
 *  the @c mode, @c direction or a predicate cost is unknown, the @c view was made by neither view
 *  constructor or belongs to another engine, a watching scan goes through a staged transaction, or
 *  a collection holds another modality.
 *
 *  - @c ustore_status_device_k The arena lives where another executor computes.
 *  - @c ustore_status_contended_k The arena or the transaction the @c view reads through serves
 *    another call, the executor's deferred table is full, a callback blocks on the executor running
 *    it, or an NVMe value keeps changing size across reads.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL or an NVMe transfer was cancelled.
 *  - @c ustore_status_not_found_k A collection does not exist or was dropped meanwhile.
 *  - @c ustore_status_capacity_exhausted_k Every shared NVMe channel is taken.
 */
USTORE_EXPORT void ustore_graph_scan_vertices(ustore_graph_scan_vertices_t* arguments);

/**
 *  @brief An opaque handle to a walk over the vertex keys of one collection, which every pull resumes past the last key
 *      the pull before it handed out, as @c ustore_blobs_cursor_t walks.
 *
 *  It holds its database open, and the transaction or snapshot it reads through,
 *  until @c ustore_graph_vertices_cursor_free, and serves one pull at a time, so a
 *  second pull naming it finishes @c ustore_status_contended_k.
 */
typedef struct ustore_graph_vertices_cursor_handle_t* ustore_graph_vertices_cursor_t;

/** The arguments of @c ustore_graph_vertices_cursor_init. */
typedef struct ustore_graph_vertices_cursor_init_t {

    /** Output for the new cursor, which the caller frees with @c ustore_graph_vertices_cursor_free, or @c NULL on
     *  failure. */
    ustore_graph_vertices_cursor_t* cursor;

    /** An open database. */
    ustore_database_t database;

    /** Executor of the kind every pull runs on, which picks the side the walk computes on, where @c NULL walks on host
     *  executors; a pull on another kind of executor reports @c ustore_status_device_k. */
    ustore_executor_t executor;

    /** Output for the outcome, final on return. */
    ustore_outcome_t* outcome;

    /** What the walk sees, where @c NULL is the head, read pull by pull. */
    ustore_view_t view;

    /** Graph collection whose vertices to walk. */
    ustore_collection_id_t collection;

    /** Key the walk starts from, inclusive. */
    ustore_key_t start_key;

    /** Key the walk stops before, exclusive, or @c NULL for no bound. */
    ustore_key_t const* end_key;

    /** Ascending, the default, or descending from the start key. */
    ustore_scan_direction_t direction;

    /** Optional verdict on the keys of every page, asked as its @c cost says; host executors only. */
    ustore_predicate_t predicate;

} ustore_graph_vertices_cursor_init_t;

/**
 *  @brief Opens a walk over the vertex keys of one collection, reading nothing until its
 *      first pull.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c cursor output is @c NULL, the @c view was made by
 *    neither view constructor or reads another database, or a predicate cost is unknown.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL.
 */
USTORE_EXPORT void ustore_graph_vertices_cursor_init(ustore_graph_vertices_cursor_init_t* arguments);

/** The arguments of @c ustore_graph_vertices_cursor_next, which pulls the next page of a walk, in one
 *  call. */
typedef struct ustore_graph_vertices_cursor_next_t {

    /** The database the cursor was opened on. */
    ustore_database_t database;

    /** Executor that runs the call, where @c NULL runs it on the calling thread. */
    ustore_executor_t executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_deferred_k. */
    ustore_outcome_t* outcome;

    /** Whether the call returns final, the default, or once submitted. */
    ustore_completion_t completion;

    /** The cursor to advance, which a pull that fails or is cancelled leaves where it was. */
    ustore_graph_vertices_cursor_t cursor;

    /** Output for the keys of the page in their order, inside @c arena. */
    ustore_key_t const** keys;

    /** Output for the keys written, zero once the walk is exhausted; zeroed on failure. */
    ustore_size_t* written;

    /** Rewound by this call, then holding every output it writes; @c NULL is refused. */
    ustore_arena_t arena;

    /** Most keys the page holds, where a page of fewer exhausts the walk. */
    ustore_size_t wanted;

    /** Optional callback reporting the one pull, which may cancel it before the cursor advances. */
    ustore_progress_callback_t progress;

    /** Opaque pointer passed to @c progress. */
    void* progress_state;

} ustore_graph_vertices_cursor_next_t;

/**
 *  @brief Pulls the next page of a walk.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c cursor, the @c arena or the @c written output
 *    is @c NULL, the cursor walks another database, a watching pull goes through a staged
 *    transaction, or the collection holds another modality.
 *  - @c ustore_status_device_k The arena lives where another executor computes, a cursor opened
 *    for a device is pulled on host threads, or a host cursor is pulled on a device.
 *  - @c ustore_status_contended_k The arena, the cursor or the transaction it reads through serves
 *    another call, the executor's deferred table is full, a callback blocks on the executor running
 *    it, or an NVMe value keeps changing size across reads.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL or an NVMe transfer was cancelled.
 *  - @c ustore_status_not_found_k The collection does not exist or was dropped meanwhile.
 *  - @c ustore_status_capacity_exhausted_k Every shared NVMe channel is taken.
 */
USTORE_EXPORT void ustore_graph_vertices_cursor_next(ustore_graph_vertices_cursor_next_t* arguments);

/**
 *  @brief Waits until every deferred call naming the cursor finishes, then frees it, giving back the holds it kept on
 *      its database and on the transaction or snapshot it reads through.
 *  @param[in,out] cursor The cursor to be freed; @c NULL is safe.
 */
USTORE_EXPORT void ustore_graph_vertices_cursor_free(ustore_graph_vertices_cursor_t cursor);

/** The arguments of @c ustore_graph_sample_vertices, which draws up to @c wanted vertices uniformly
 *  at random from every graph collection. */
typedef struct ustore_graph_sample_vertices_t {

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

    /** Number of samples. */
    ustore_size_t count;

    /** Collections to sample. */
    ustore_collections_view_t collections;

    /** Output in @c arena for @c count plus one offsets into @c keys, the last closing them. */
    ustore_length_t const** offsets;

    /** Output for the sampled keys, sample after sample, inside @c arena. */
    ustore_key_t const** keys;

    /** Rewound by this call, then holding every output it writes; @c NULL is refused. */
    ustore_arena_t arena;

    /** Most keys every sample draws. */
    ustore_size_t wanted;

} ustore_graph_sample_vertices_t;

/**
 *  @brief Samples vertices from a batch of collections.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c arena, @c collections or an output is missing,
 *    the @c view was made by neither view constructor or belongs to another engine, a watching
 *    sample goes through a staged transaction, or a collection holds another modality.
 *  - @c ustore_status_device_k The arena lives where another executor computes.
 *  - @c ustore_status_contended_k The arena or the transaction the @c view reads through serves
 *    another call, the executor's deferred table is full, or a callback blocks on the executor
 *    running it.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL or an NVMe transfer was cancelled.
 *  - @c ustore_status_unsupported_k The executor computes on a device.
 *  - @c ustore_status_not_found_k A collection does not exist or was dropped meanwhile.
 *  - @c ustore_status_capacity_exhausted_k Every shared NVMe channel is taken.
 */
USTORE_EXPORT void ustore_graph_sample_vertices(ustore_graph_sample_vertices_t* arguments);

/** The arguments of @c ustore_graph_measure_vertices, which estimates the vertices and the space
 *  inside key ranges, every output holding one bound per range. */
typedef struct ustore_graph_measure_vertices_t {

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

    /** Number of measured ranges. */
    ustore_size_t count;

    /** Collections to measure. */
    ustore_collections_view_t collections;

    /** Inclusive start of every range, @c NULL data starting at the first key in @c direction. */
    ustore_keys_view_t start_keys;

    /** Key every range stops before, exclusive, or a zeroed view for no bound. */
    ustore_keys_view_t end_keys;

    /** Ascending, the default, or descending from the start key. */
    ustore_scan_direction_t direction;

    /** Optional output for the lower bound on the vertices in every range, inside @c arena. */
    ustore_size_t const** min_cardinalities;

    /** Optional output for the upper bound on the vertices in every range, inside @c arena. */
    ustore_size_t const** max_cardinalities;

    /** Optional output for the lower bound on the value bytes in every range, inside @c arena. */
    ustore_size_t const** min_value_bytes;

    /** Optional output for the upper bound on the value bytes in every range, inside @c arena. */
    ustore_size_t const** max_value_bytes;

    /** Optional output for the lower bound on the disk space every range takes, inside @c arena. */
    ustore_size_t const** min_space_usages;

    /** Optional output for the upper bound on the disk space every range takes, inside @c arena. */
    ustore_size_t const** max_space_usages;

    /** Rewound by this call, then holding every output it writes; @c NULL is refused. */
    ustore_arena_t arena;

} ustore_graph_measure_vertices_t;

/**
 *  @brief Estimates the vertices and the space inside a batch of key ranges.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_device_k The arena lives where another executor computes.
 *  - @c ustore_status_contended_k The arena or the transaction the @c view reads through serves
 *    another call, the executor's deferred table is full, or a callback blocks on the executor
 *    running it.
 *  - @c ustore_status_uninitialized_k The @c database is @c NULL or an NVMe transfer was cancelled.
 *  - @c ustore_status_unsupported_k The executor computes on a device.
 *  - @c ustore_status_not_found_k A collection does not exist or was dropped meanwhile.
 *  - @c ustore_status_capacity_exhausted_k Every shared NVMe channel is taken.
 *
 *  @c ustore_status_invalid_argument_k means the @c arena or the @c collections are missing,
 *  the @c direction is unknown, the @c view was made by neither view constructor or belongs
 *  to another engine, a watching measure goes through a staged transaction, or a collection
 *  holds another modality.
 */
USTORE_EXPORT void ustore_graph_measure_vertices(ustore_graph_measure_vertices_t* arguments);

#ifdef __cplusplus
}
#endif

#endif // UNUM_USTORE_GRAPH_H
