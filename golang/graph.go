// Graph collections: edges, vertices, scans, walks, samples and measurements, mirroring graph.h.
//
// File: golang/graph.go
// Author: Ash Vardanian
// Date: November 18, 2026

package ustore

import "unsafe"

/*
#cgo noescape ustore_graph_read_edges
#cgo nocallback ustore_graph_read_edges
#cgo noescape ustore_graph_write_edges
#cgo nocallback ustore_graph_write_edges
#cgo noescape ustore_graph_remove_edges
#cgo nocallback ustore_graph_remove_edges
#cgo noescape ustore_graph_write_vertices
#cgo nocallback ustore_graph_write_vertices
#cgo noescape ustore_graph_remove_vertices
#cgo nocallback ustore_graph_remove_vertices
#cgo noescape ustore_graph_scan_vertices
#cgo noescape ustore_graph_vertices_cursor_init
#cgo noescape ustore_graph_vertices_cursor_next
#cgo noescape ustore_graph_sample_vertices
#cgo nocallback ustore_graph_sample_vertices
#cgo noescape ustore_graph_measure_vertices
#cgo nocallback ustore_graph_measure_vertices
#cgo noescape ustore_vertex_role_from_name
#cgo nocallback ustore_vertex_role_from_name

#include "shim.h"
*/
import "C"

// region: Edges and Roles

// Edge is a source, a target and an edge identifier, laid out as the three keys the library puts on
// its tapes, so a slice of edges crosses as three strided columns.
type Edge struct {
	Source Key
	Target Key
	ID     Key
}

// EdgeIDDefault is the identifier of an edge that was given none, every bit of it set.
var EdgeIDDefault = KeyUnknown

// Neighbor reports the end of the edge other than vertex.
func (edge Edge) Neighbor(vertex Key) Key {
	if edge.Source == vertex {
		return edge.Target
	}
	return edge.Source
}

// VertexRole is the role a vertex plays in the edges a call names.
type VertexRole int32

const (
	// VertexRoleUnknown is no role.
	VertexRoleUnknown VertexRole = C.ustore_vertex_role_unknown_k
	// VertexRoleSource is the vertex an edge leaves.
	VertexRoleSource VertexRole = C.ustore_vertex_role_source_k
	// VertexRoleTarget is the vertex an edge enters.
	VertexRoleTarget VertexRole = C.ustore_vertex_role_target_k
	// VertexRoleAny is either end of an edge.
	VertexRoleAny VertexRole = C.ustore_vertex_role_any_k
)

// String renders the role as the library spells it.
func (role VertexRole) String() string {
	return C.GoString(C.ustore_vertex_role_name(C.ustore_vertex_role_t(role)))
}

// VertexRoleFromName reads a role back from the spelling [VertexRole.String] renders.
func VertexRoleFromName(name string) (VertexRole, error) {
	var role C.ustore_vertex_role_t
	err := named(C.ustore_vertex_role_from_name(view(name), &role))
	return VertexRole(role), err
}

// Roles names the role of every vertex in a batch: a [VertexRole] the whole batch shares, or
// [VertexRoles], one per vertex; nil stands for [VertexRoleAny].
type Roles interface{ roles() }

func (VertexRole) roles() {}

// VertexRoles names one role per vertex of a batch.
type VertexRoles []VertexRole

func (VertexRoles) roles() {}

// roles passes the role every vertex shares from the frame's inputs, or one per vertex in place.
func (call *call) roles(inputs *inputs, roles Roles) C.ustore_vertex_roles_view_t {
	switch roles := roles.(type) {
	case VertexRole:
		inputs.role = C.ustore_vertex_role_t(roles)
		return C.ustore_vertex_roles_view_t{data: &inputs.role}
	case VertexRoles:
		return C.ustore_vertex_roles_view_t{
			data:   (*C.ustore_vertex_role_t)(call.pinSlice(unsafe.Pointer(unsafe.SliceData(roles)), len(roles))),
			stride: C.ustore_size_t(unsafe.Sizeof(VertexRole(0))),
		}
	}
	return C.ustore_vertex_roles_view_t{}
}

// perVertex reports how many roles roles names one by one, zero for a shared one.
func perVertex(roles Roles) int {
	each, _ := roles.(VertexRoles)
	return len(each)
}

// edges passes a slice of edges as the identifiers, sources and targets columns, each strided by
// three keys.
func (call *call) edges(edges []Edge) (identifiers, sources, targets C.ustore_keys_view_t) {
	if len(edges) == 0 {
		return
	}
	first := (*Edge)(call.pinSlice(unsafe.Pointer(unsafe.SliceData(edges)), len(edges)))
	stride := C.ustore_size_t(unsafe.Sizeof(Edge{}))
	identifiers = C.ustore_keys_view_t{data: (*C.ustore_key_t)(unsafe.Pointer(&first.ID)), stride: stride}
	sources = C.ustore_keys_view_t{data: (*C.ustore_key_t)(unsafe.Pointer(&first.Source)), stride: stride}
	targets = C.ustore_keys_view_t{data: (*C.ustore_key_t)(unsafe.Pointer(&first.Target)), stride: stride}
	return
}

// endregion: Edges and Roles

// region: Edges

// Neighborhoods is the degree and the edges of every vertex a read named, readable until the arena
// serves another call.
type Neighborhoods struct {
	degrees *C.ustore_vertex_degree_t
	offsets *C.ustore_length_t
	edges   *C.ustore_key_t
	count   uint
	arena   arenaWrite
}

// Len reports how many vertices the call read.
func (neighborhoods Neighborhoods) Len() uint { return neighborhoods.count }

// vertexDegreeMissing mirrors ustore_vertex_degree_missing_k, a static constant cgo cannot link.
const vertexDegreeMissing = 0xFFFFFFFF

// Degree reports how many edges vertex index connects to, and false for a missing vertex.
func (neighborhoods Neighborhoods) Degree(index uint) (uint32, bool) {
	neighborhoods.arena.mustOpen()
	defer neighborhoods.arena.close()
	degree := uint32(at(neighborhoods.degrees, index))
	return degree, degree != vertexDegreeMissing
}

// Edges views the edges of vertex index in the arena: with both roles the outgoing ones first,
// sorted by target, then the incoming ones, sorted by source.
func (neighborhoods Neighborhoods) Edges(index uint) []Edge {
	neighborhoods.arena.mustOpen()
	defer neighborhoods.arena.close()
	begin, end := at(neighborhoods.offsets, index), at(neighborhoods.offsets, index+1)
	if neighborhoods.edges == nil || begin == end {
		return nil
	}
	return unsafe.Slice((*Edge)(unsafe.Add(unsafe.Pointer(neighborhoods.edges), uintptr(begin)*unsafe.Sizeof(Edge{}))), end-begin)
}

// GraphReadEdges reads the degree and the edges of every vertex in its roles into arena.
func (on reads[Handle]) GraphReadEdges(collections Collections, vertices []Key, roles Roles, arena *Arena) (Neighborhoods, error) {
	var call callWithResult[Neighborhoods]
	return block(&call, on.graphReadEdges(&call, C.ustore_completion_blocking_k, collections, vertices, roles, arena))
}

// GraphReadEdges submits [DatabaseOn.GraphReadEdges].
func (on deferredReads[Handle]) GraphReadEdges(collections Collections, vertices []Key, roles Roles, arena *Arena) (*Deferred[Neighborhoods], error) {
	deferred := new(Deferred[Neighborhoods])
	return submitted(deferred, on.graphReadEdges(&deferred.call, C.ustore_completion_deferred_k, collections, vertices, roles, arena))
}

func (receiver storeReceiver[Handle]) graphReadEdges(call *callWithResult[Neighborhoods], completion C.ustore_completion_t, collections Collections, vertices []Key, roles Roles, arena *Arena) error {
	database, executor, outputs, err := receiver.lowerInto(arena)
	if err != nil {
		return err
	}
	if err := shape(collections, len(vertices), perVertex(roles)); err != nil {
		return err
	}
	arguments, inputs := stage[C.ustore_graph_read_edges_t](call)
	call.result.arena = call.stamp(arena, outputs)
	call.result.count = uint(len(vertices))
	*arguments = C.ustore_graph_read_edges_t{
		database:           database,
		executor:           executor,
		outcome:            call.outcome(),
		completion:         completion,
		view:               receiver.view(),
		count:              C.ustore_size_t(len(vertices)),
		collections:        call.collections(inputs, collections),
		vertices:           call.keys(vertices),
		roles:              call.roles(inputs, roles),
		degrees_per_vertex: &call.result.degrees,
		edges_offsets:      &call.result.offsets,
		edges_per_vertex:   &call.result.edges,
		arena:              outputs,
	}
	C.ustore_graph_read_edges(arguments)
	return nil
}

// GraphWriteEdges inserts edges, and their vertices where missing.
func (on writes[Handle]) GraphWriteEdges(collections Collections, edges []Edge, config *WriteConfig) error {
	var call callWithResult[struct{}]
	_, err := block(&call, on.graphWriteEdges(&call, C.ustore_completion_blocking_k, collections, edges, config))
	return err
}

// GraphWriteEdges submits [DatabaseOn.GraphWriteEdges].
func (on deferredWrites[Handle]) GraphWriteEdges(collections Collections, edges []Edge, config *WriteConfig) (*Deferred[struct{}], error) {
	deferred := new(Deferred[struct{}])
	return submitted(deferred, on.graphWriteEdges(&deferred.call, C.ustore_completion_deferred_k, collections, edges, config))
}

func (receiver storeReceiver[Handle]) graphWriteEdges(call *callWithResult[struct{}], completion C.ustore_completion_t, collections Collections, edges []Edge, config *WriteConfig) error {
	database, executor, err := receiver.lower()
	if err != nil {
		return err
	}
	if err := shape(collections, len(edges)); err != nil {
		return err
	}
	options := configOf(config)
	arguments, inputs := stage[C.ustore_graph_write_edges_t](call)
	identifiers, sources, targets := call.edges(edges)
	*arguments = C.ustore_graph_write_edges_t{
		database:    database,
		executor:    executor,
		outcome:     call.outcome(),
		completion:  completion,
		transaction: receiver.transaction(),
		count:       C.ustore_size_t(len(edges)),
		collections: call.collections(inputs, collections),
		edges:       identifiers,
		sources:     sources,
		targets:     targets,
		durability:  C.ustore_durability_t(options.Durability),
		watch:       C.ustore_watch_t(receiver.watch),
	}
	C.ustore_graph_write_edges(arguments)
	return nil
}

// GraphRemoveEdges removes edges, keeping their vertices.
func (on writes[Handle]) GraphRemoveEdges(collections Collections, edges []Edge, config *WriteConfig) error {
	var call callWithResult[struct{}]
	_, err := block(&call, on.graphRemoveEdges(&call, C.ustore_completion_blocking_k, collections, edges, config))
	return err
}

// GraphRemoveEdges submits [DatabaseOn.GraphRemoveEdges].
func (on deferredWrites[Handle]) GraphRemoveEdges(collections Collections, edges []Edge, config *WriteConfig) (*Deferred[struct{}], error) {
	deferred := new(Deferred[struct{}])
	return submitted(deferred, on.graphRemoveEdges(&deferred.call, C.ustore_completion_deferred_k, collections, edges, config))
}

func (receiver storeReceiver[Handle]) graphRemoveEdges(call *callWithResult[struct{}], completion C.ustore_completion_t, collections Collections, edges []Edge, config *WriteConfig) error {
	database, executor, err := receiver.lower()
	if err != nil {
		return err
	}
	if err := shape(collections, len(edges)); err != nil {
		return err
	}
	options := configOf(config)
	arguments, inputs := stage[C.ustore_graph_remove_edges_t](call)
	identifiers, sources, targets := call.edges(edges)
	*arguments = C.ustore_graph_remove_edges_t{
		database:    database,
		executor:    executor,
		outcome:     call.outcome(),
		completion:  completion,
		transaction: receiver.transaction(),
		count:       C.ustore_size_t(len(edges)),
		collections: call.collections(inputs, collections),
		edges:       identifiers,
		sources:     sources,
		targets:     targets,
		durability:  C.ustore_durability_t(options.Durability),
		watch:       C.ustore_watch_t(receiver.watch),
	}
	C.ustore_graph_remove_edges(arguments)
	return nil
}

// endregion: Edges

// region: Vertices

// GraphWriteVertices inserts vertices unless present.
func (on writes[Handle]) GraphWriteVertices(collections Collections, vertices []Key, config *WriteConfig) error {
	var call callWithResult[struct{}]
	_, err := block(&call, on.graphWriteVertices(&call, C.ustore_completion_blocking_k, collections, vertices, config))
	return err
}

// GraphWriteVertices submits [DatabaseOn.GraphWriteVertices].
func (on deferredWrites[Handle]) GraphWriteVertices(collections Collections, vertices []Key, config *WriteConfig) (*Deferred[struct{}], error) {
	deferred := new(Deferred[struct{}])
	return submitted(deferred, on.graphWriteVertices(&deferred.call, C.ustore_completion_deferred_k, collections, vertices, config))
}

func (receiver storeReceiver[Handle]) graphWriteVertices(call *callWithResult[struct{}], completion C.ustore_completion_t, collections Collections, vertices []Key, config *WriteConfig) error {
	database, executor, err := receiver.lower()
	if err != nil {
		return err
	}
	if err := shape(collections, len(vertices)); err != nil {
		return err
	}
	options := configOf(config)
	arguments, inputs := stage[C.ustore_graph_write_vertices_t](call)
	*arguments = C.ustore_graph_write_vertices_t{
		database:    database,
		executor:    executor,
		outcome:     call.outcome(),
		completion:  completion,
		transaction: receiver.transaction(),
		count:       C.ustore_size_t(len(vertices)),
		collections: call.collections(inputs, collections),
		vertices:    call.keys(vertices),
		durability:  C.ustore_durability_t(options.Durability),
		watch:       C.ustore_watch_t(receiver.watch),
	}
	C.ustore_graph_write_vertices(arguments)
	return nil
}

// GraphRemoveVerticesConfig carries the optional arguments of [DatabaseOn.GraphRemoveVertices].
type GraphRemoveVerticesConfig struct {
	// EndVertices turns every vertex into the inclusive start of a range ending before its end
	// vertex, one per task, or removes the vertices alone when empty.
	EndVertices []Key
	// Roles are the roles of the vertices, needed only by joining graphs.
	Roles Roles
	// Durability is how durable the removals are once the call finishes at the head.
	Durability Durability
}

// GraphRemoveVertices removes vertices with every edge they connect to, or every vertex of a range
// per task given end vertices.
func (on writes[Handle]) GraphRemoveVertices(collections Collections, vertices []Key, config *GraphRemoveVerticesConfig) error {
	var call callWithResult[struct{}]
	_, err := block(&call, on.graphRemoveVertices(&call, C.ustore_completion_blocking_k, collections, vertices, config))
	return err
}

// GraphRemoveVertices submits [DatabaseOn.GraphRemoveVertices].
func (on deferredWrites[Handle]) GraphRemoveVertices(collections Collections, vertices []Key, config *GraphRemoveVerticesConfig) (*Deferred[struct{}], error) {
	deferred := new(Deferred[struct{}])
	return submitted(deferred, on.graphRemoveVertices(&deferred.call, C.ustore_completion_deferred_k, collections, vertices, config))
}

func (receiver storeReceiver[Handle]) graphRemoveVertices(call *callWithResult[struct{}], completion C.ustore_completion_t, collections Collections, vertices []Key, config *GraphRemoveVerticesConfig) error {
	database, executor, err := receiver.lower()
	if err != nil {
		return err
	}
	options := configOf(config)
	if err := shape(collections, len(vertices), len(options.EndVertices), perVertex(options.Roles)); err != nil {
		return err
	}
	arguments, inputs := stage[C.ustore_graph_remove_vertices_t](call)
	*arguments = C.ustore_graph_remove_vertices_t{
		database:     database,
		executor:     executor,
		outcome:      call.outcome(),
		completion:   completion,
		transaction:  receiver.transaction(),
		count:        C.ustore_size_t(len(vertices)),
		collections:  call.collections(inputs, collections),
		vertices:     call.keys(vertices),
		end_vertices: call.keys(options.EndVertices),
		roles:        call.roles(inputs, options.Roles),
		durability:   C.ustore_durability_t(options.Durability),
		watch:        C.ustore_watch_t(receiver.watch),
	}
	C.ustore_graph_remove_vertices(arguments)
	return nil
}

// endregion: Vertices

// region: Scans

// GraphScanConfig carries the optional arguments of [DatabaseOn.GraphScanVertices].
type GraphScanConfig struct {
	// StartKeys are the inclusive starts of the scans, one per task, or the first vertex in Direction
	// when empty.
	StartKeys []Key
	// EndKeys are the vertices every scan stops before, one per task, or no bound when empty.
	EndKeys []Key
	// Direction is which way every scan goes from its start.
	Direction ScanDirection
	// Predicate admits or passes over the listed vertices.
	Predicate Admitter
	// Mode is in what order the vertices come.
	Mode ScanMode
	// Progress reports the scans finished, and may cancel the call.
	Progress ProgressCallback
}

// GraphScanVertices lists up to wanted vertices of a range per task into arena, one task per
// collection collections names.
func (on reads[Handle]) GraphScanVertices(collections Collections, wanted uint, arena *Arena, config *GraphScanConfig) (Scanned, error) {
	var call callWithResult[Scanned]
	return block(&call, on.graphScanVertices(&call, C.ustore_completion_blocking_k, collections, wanted, arena, config))
}

// GraphScanVertices submits [DatabaseOn.GraphScanVertices].
func (on deferredReads[Handle]) GraphScanVertices(collections Collections, wanted uint, arena *Arena, config *GraphScanConfig) (*Deferred[Scanned], error) {
	deferred := new(Deferred[Scanned])
	return submitted(deferred, on.graphScanVertices(&deferred.call, C.ustore_completion_deferred_k, collections, wanted, arena, config))
}

func (receiver storeReceiver[Handle]) graphScanVertices(call *callWithResult[Scanned], completion C.ustore_completion_t, collections Collections, wanted uint, arena *Arena, config *GraphScanConfig) error {
	database, executor, outputs, err := receiver.lowerInto(arena)
	if err != nil {
		return err
	}
	options := configOf(config)
	count := tasks(collections)
	if err := shape(collections, count, len(options.StartKeys), len(options.EndKeys)); err != nil {
		return err
	}
	arguments, inputs := stage[C.ustore_graph_scan_vertices_t](call)
	call.result.arena = call.stamp(arena, outputs)
	call.result.count = uint(count)
	progress, state := call.progress(options.Progress)
	*arguments = C.ustore_graph_scan_vertices_t{
		database:       database,
		executor:       executor,
		outcome:        call.outcome(),
		completion:     completion,
		view:           receiver.view(),
		count:          C.ustore_size_t(count),
		collections:    call.collections(inputs, collections),
		start_keys:     call.keys(options.StartKeys),
		end_keys:       call.keys(options.EndKeys),
		direction:      C.ustore_scan_direction_t(options.Direction),
		predicate:      call.predicate(options.Predicate),
		offsets:        &call.result.offsets,
		keys:           &call.result.keys,
		arena:          outputs,
		wanted:         C.ustore_size_t(wanted),
		mode:           C.ustore_scan_mode_t(options.Mode),
		progress:       progress,
		progress_state: state,
	}
	C.ustore_graph_scan_vertices(arguments)
	return nil
}

// endregion: Scans

// region: Cursors

// GraphVerticesCursorConfig carries the optional arguments of [DatabaseOn.GraphVerticesCursorInit].
type GraphVerticesCursorConfig struct {
	// EndKey is the vertex the walk stops before, or nil for no bound.
	EndKey *Key
	// Direction is which way the walk goes from its start.
	Direction ScanDirection
	// Predicate admits or passes over the vertices of every page.
	Predicate Admitter
}

// GraphVerticesCursor walks the vertices of one graph collection page by page, every pull resuming
// past the last vertex the pull before it handed out.
type GraphVerticesCursor struct {
	GraphVerticesCursorOn
	walk
	handle C.ustore_graph_vertices_cursor_t
}

// GraphVerticesCursorOn is the receiver whose pulls block until their call is final: on the
// calling goroutine when embedded in a [GraphVerticesCursor], or on an executor when
// [GraphVerticesCursorOn.On] makes it.
type GraphVerticesCursorOn struct {
	childReceiver[C.ustore_graph_vertices_cursor_t, C.ustore_database_t]
}

// On is the receiver whose pulls block on executor.
func (on GraphVerticesCursorOn) On(executor *Executor) GraphVerticesCursorOn {
	on.executor = executor
	return on
}

// GraphVerticesCursorInit opens a walk over the vertices of collection from startKey, reading
// nothing until its first pull and blocking whatever receiver it is called on.
func (on reads[Handle]) GraphVerticesCursorInit(collection CollectionID, startKey Key, config *GraphVerticesCursorConfig) (*GraphVerticesCursor, error) {
	database, executor, err := on.lower()
	if err != nil {
		return nil, err
	}
	options := configOf(config)
	cursor := new(GraphVerticesCursor)
	var call callWithResult[C.ustore_graph_vertices_cursor_t]
	arguments, inputs := stage[C.ustore_graph_vertices_cursor_init_t](&call)
	*arguments = C.ustore_graph_vertices_cursor_init_t{
		cursor:     call.result,
		database:   database,
		executor:   executor,
		outcome:    call.outcome(),
		view:       on.view(),
		collection: rawCollection(collection),
		start_key:  raw(startKey),
		end_key:    call.key(inputs, options.EndKey),
		direction:  C.ustore_scan_direction_t(options.Direction),
		predicate:  cursor.admits(options.Predicate),
	}
	C.ustore_graph_vertices_cursor_init(arguments)
	if cursor.handle, err = block(&call, nil); err != nil {
		cursor.end()
		return nil, err
	}
	cursor.database = database
	cursor.childReceiver = childReceiver[C.ustore_graph_vertices_cursor_t, C.ustore_database_t]{receiver: receiver[C.ustore_graph_vertices_cursor_t]{handle: &cursor.handle}, parent: &cursor.database}
	return cursor, nil
}

// NextKeys pulls up to wanted vertices of the walk into arena.
func (on GraphVerticesCursorOn) NextKeys(wanted uint, arena *Arena) (Page, error) {
	cursor, database, executor, outputs, err := on.lowerInto(arena)
	if err != nil {
		return Page{}, err
	}
	var call callWithResult[Page]
	arguments := open[C.ustore_graph_vertices_cursor_next_t](&call)
	*arguments = C.ustore_graph_vertices_cursor_next_t{
		database: database,
		executor: executor,
		outcome:  call.outcome(),
		cursor:   cursor,
		keys:     &call.result.keys,
		written:  &call.result.written,
		arena:    outputs,
		wanted:   C.ustore_size_t(wanted),
	}
	C.ustore_graph_vertices_cursor_next(arguments)
	return block(&call, nil)
}

// Close frees the walk, giving back the holds it kept. It is idempotent.
func (cursor *GraphVerticesCursor) Close() error {
	if cursor == nil || cursor.handle == nil {
		return nil
	}
	C.ustore_graph_vertices_cursor_free(cursor.handle)
	cursor.handle = nil
	cursor.end()
	return nil
}

// endregion: Cursors

// region: Samples and Measures

// GraphSampleVertices draws up to wanted vertices uniformly at random into arena, one task per
// collection collections names.
func (on reads[Handle]) GraphSampleVertices(collections Collections, wanted uint, arena *Arena) (Scanned, error) {
	var call callWithResult[Scanned]
	return block(&call, on.graphSampleVertices(&call, C.ustore_completion_blocking_k, collections, wanted, arena))
}

// GraphSampleVertices submits [DatabaseOn.GraphSampleVertices].
func (on deferredReads[Handle]) GraphSampleVertices(collections Collections, wanted uint, arena *Arena) (*Deferred[Scanned], error) {
	deferred := new(Deferred[Scanned])
	return submitted(deferred, on.graphSampleVertices(&deferred.call, C.ustore_completion_deferred_k, collections, wanted, arena))
}

func (receiver storeReceiver[Handle]) graphSampleVertices(call *callWithResult[Scanned], completion C.ustore_completion_t, collections Collections, wanted uint, arena *Arena) error {
	database, executor, outputs, err := receiver.lowerInto(arena)
	if err != nil {
		return err
	}
	count := tasks(collections)
	arguments, inputs := stage[C.ustore_graph_sample_vertices_t](call)
	call.result.arena = call.stamp(arena, outputs)
	call.result.count = uint(count)
	*arguments = C.ustore_graph_sample_vertices_t{
		database:    database,
		executor:    executor,
		outcome:     call.outcome(),
		completion:  completion,
		view:        receiver.view(),
		count:       C.ustore_size_t(count),
		collections: call.collections(inputs, collections),
		offsets:     &call.result.offsets,
		keys:        &call.result.keys,
		arena:       outputs,
		wanted:      C.ustore_size_t(wanted),
	}
	C.ustore_graph_sample_vertices(arguments)
	return nil
}

// GraphMeasureVertices estimates the vertices and the space inside a range per task into arena, one
// task per collection collections names.
func (on reads[Handle]) GraphMeasureVertices(collections Collections, arena *Arena, config *MeasureConfig) (Measurement, error) {
	var call callWithResult[Measurement]
	return block(&call, on.graphMeasureVertices(&call, C.ustore_completion_blocking_k, collections, arena, config))
}

// GraphMeasureVertices submits [DatabaseOn.GraphMeasureVertices].
func (on deferredReads[Handle]) GraphMeasureVertices(collections Collections, arena *Arena, config *MeasureConfig) (*Deferred[Measurement], error) {
	deferred := new(Deferred[Measurement])
	return submitted(deferred, on.graphMeasureVertices(&deferred.call, C.ustore_completion_deferred_k, collections, arena, config))
}

func (receiver storeReceiver[Handle]) graphMeasureVertices(call *callWithResult[Measurement], completion C.ustore_completion_t, collections Collections, arena *Arena, config *MeasureConfig) error {
	database, executor, outputs, err := receiver.lowerInto(arena)
	if err != nil {
		return err
	}
	options := configOf(config)
	count := tasks(collections)
	if err := shape(collections, count, len(options.StartKeys), len(options.EndKeys)); err != nil {
		return err
	}
	arguments, inputs := stage[C.ustore_graph_measure_vertices_t](call)
	call.result.arena = call.stamp(arena, outputs)
	call.result.count = uint(count)
	*arguments = C.ustore_graph_measure_vertices_t{
		database:          database,
		executor:          executor,
		outcome:           call.outcome(),
		completion:        completion,
		view:              receiver.view(),
		count:             C.ustore_size_t(count),
		collections:       call.collections(inputs, collections),
		start_keys:        call.keys(options.StartKeys),
		end_keys:          call.keys(options.EndKeys),
		direction:         C.ustore_scan_direction_t(options.Direction),
		min_cardinalities: &call.result.minCardinalities,
		max_cardinalities: &call.result.maxCardinalities,
		min_value_bytes:   &call.result.minValueBytes,
		max_value_bytes:   &call.result.maxValueBytes,
		min_space_usages:  &call.result.minSpaceUsages,
		max_space_usages:  &call.result.maxSpaceUsages,
		arena:             outputs,
	}
	C.ustore_graph_measure_vertices(arguments)
	return nil
}

// endregion: Samples and Measures
