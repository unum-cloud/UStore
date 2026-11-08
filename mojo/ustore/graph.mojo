"""Graph collections: edges and vertices, their reads, writes, removes, scans, cursors, samples and measures.

An edge is a source, a target and an optional edge identifier, so distinct identifiers let a pair of
vertices share several edges. One collection holds an undirected or a directed graph over its own
vertices, or a joining graph between two collections.

File: mojo/ustore/graph.mojo
Author: Ash Vardanian
Date: November 8, 2026
"""

from std.ffi import c_int, c_size_t
from std.memory import stack_allocation

from ustore.blobs import KeyRuns, KeyRunsWritten, MeasureArguments, Measures, Pulled, PulledWritten, SampleArguments, _measure
from ustore.db import (
    CollectionID,
    CollectionsView,
    Durability,
    Key,
    KeysView,
    Predicate,
    ScanDirection,
    ScanMode,
    Scope,
    StridedArguments,
    StridedView,
    Watch,
    _tasks,
)
from ustore.library import Library
from ustore.types import (
    Address,
    Answer,
    Arena,
    Arguments,
    Completion,
    Deferred,
    Error,
    Executor,
    NULL_ADDRESS,
    Outcome,
    Progress,
    Unwritten,
    Verb,
    _anchor,
    _block,
    _created,
    _cross,
    _Crossing,
    _in_arena,
    _readable,
    _slot,
)

# region Vocabulary


@fieldwise_init
struct VertexRole(Equatable, ImplicitlyCopyable, TrivialRegisterPassable):
    """The role a vertex plays in an edge, `ustore_vertex_role_t`; undirected graphs use `ANY`."""

    var code: c_int
    """The raw `ustore_vertex_role_t` value the ABI exchanges."""
    comptime UNKNOWN = Self(0)
    """No role."""
    comptime SOURCE = Self(1)
    """The vertex an edge leaves."""
    comptime TARGET = Self(2)
    """The vertex an edge enters."""
    comptime ANY = Self(3)
    """Either end of an edge."""


comptime VertexRolesView[origin: ImmOrigin = ImmUntrackedOrigin] = StridedView[VertexRole, origin]
"""A strided run of roles, `ustore_vertex_roles_view_t`; zeroed stands for `ANY`."""

comptime VERTEX_DEGREE_MISSING: UInt32 = 0xFFFFFFFF
"""The degree a read reports for a missing vertex."""


# endregion Vocabulary

# region Results


@fieldwise_init
struct EdgesWritten(Defaultable, ImplicitlyCopyable):
    """The degrees, offsets and edges an edges read writes, at one stable address."""

    var degrees: Address
    var offsets: Address
    var edges: Address
    var with_edges: Bool

    def __init__(out self):
        """Nothing written, which is what a failed call leaves."""
        self = Self(NULL_ADDRESS, NULL_ADDRESS, NULL_ADDRESS, True)


@fieldwise_init
struct Edges[origin: MutOrigin](Answer, Sized):
    """The edges of a batch of vertices, three keys per edge, read in place from the arena a read borrows.

    Parameters:
        origin: The arena the edges live in, which this result borrows.
    """

    comptime Written = EdgesWritten
    var arena: Pointer[Arena, Self.origin]
    """The arena holding the edges."""
    var generation: Int
    """The arena's call the edges came from."""
    var written: EdgesWritten
    """The views the engine wrote."""
    var count: Int
    """Vertices the read carried."""

    def __init__(out self, arena: Address, generation: Int, written: EdgesWritten, count: Int):
        self.arena = Pointer[Arena, Self.origin](unsafe_from_address=arena)
        self.generation = generation
        self.written = written
        self.count = count

    def __len__(self) -> Int:
        """The vertices the read carried."""
        return self.count

    def degrees(ref self) raises Error -> Span[UInt32, origin_of(self)]:
        """The degree of every vertex, `VERTEX_DEGREE_MISSING` for a missing one."""
        _readable(self.arena, self.generation, "Edges.degrees")
        return _in_arena[UInt32, origin_of(self)](self.written.degrees, self.count)

    def offsets(ref self) raises Error -> Span[UInt64, origin_of(self)]:
        """The vertices' boundaries in edges, one more of them than vertices; empty unless edges were asked for."""
        _readable(self.arena, self.generation, "Edges.offsets")
        return _in_arena[UInt64, origin_of(self)](self.written.offsets, self.count + 1)

    def edges(ref self, vertex: Int) raises Error -> Span[Key, origin_of(self)]:
        """The edges of vertex `vertex`, each as its source, target and edge identifier."""
        var offsets = self.offsets()
        if len(offsets) == 0:
            return Span[Key, origin_of(self)]()
        var first = Int(offsets[vertex])
        return _in_arena[Key, origin_of(self)](self.written.edges + first * 48, (Int(offsets[vertex + 1]) - first) * 3)


# endregion Results

# region Configs


struct GraphScanConfig[end_origin: ImmOrigin = ImmUntrackedOrigin](ImplicitlyCopyable):
    """The bounds, order and verdict of a scan over vertices.

    Parameters:
        end_origin: The borrowed end keys.
    """

    var end_keys: KeysView[Self.end_origin]
    """Vertex every scan stops before, exclusive; zeroed bounds nothing."""
    var direction: ScanDirection
    """Ascending or descending from the start keys."""
    var mode: ScanMode
    """In what order the vertices come."""
    var predicate: Predicate
    """The caller's verdict on the listed vertices."""
    var progress: Progress
    """Reports the scans finished, and stops the call where it says to."""

    def __init__(
        out self,
        *,
        end_keys: KeysView[Self.end_origin] = KeysView[Self.end_origin](),
        direction: ScanDirection = ScanDirection.ASCENDING,
        mode: ScanMode = ScanMode.ORDERED,
        predicate: Predicate = Predicate(),
        progress: Progress = Progress(),
    ):
        """Names only the knobs that differ from the defaults."""
        self.end_keys = end_keys
        self.direction = direction
        self.mode = mode
        self.predicate = predicate
        self.progress = progress


@fieldwise_init
struct GraphCursorConfig(Defaultable, ImplicitlyCopyable):
    """The bound, order and verdict of a walk over the vertices of one collection."""

    var end_key: Optional[Key]
    """Vertex the walk stops before, exclusive; absent bounds nothing."""
    var direction: ScanDirection
    """Ascending or descending from the start key."""
    var predicate: Predicate
    """The caller's verdict on the keys of every page."""

    def __init__(out self):
        """Unbounded, ascending, admitting every vertex."""
        self = Self(None, ScanDirection.ASCENDING, Predicate())


# endregion Configs

# region Arguments


@fieldwise_init
struct GraphReadEdgesArguments(Arguments):
    """`ustore_graph_read_edges_t`."""

    comptime Written = EdgesWritten
    comptime symbol: StaticString = "ustore_graph_read_edges"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var view: Address
    var count: c_size_t
    var collections: StridedArguments
    var vertices: StridedArguments
    var roles: StridedArguments
    var degrees_per_vertex: Optional[Pointer[Address, MutUntrackedOrigin]]
    var edges_offsets: Optional[Pointer[Address, MutUntrackedOrigin]]
    var edges_per_vertex: Optional[Pointer[Address, MutUntrackedOrigin]]
    var arena: Address
    var with_edges: Bool

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[EdgesWritten, MutUntrackedOrigin], arena: Address):
        self.database = scope.database
        self.executor = scope.executor
        self.outcome = outcome
        self.completion = completion
        self.view = scope.view
        self.degrees_per_vertex = Pointer(to=written[].degrees)
        self.edges_offsets = _slot(self.with_edges, Pointer(to=written[].offsets))
        self.edges_per_vertex = _slot(self.with_edges, Pointer(to=written[].edges))
        self.arena = arena

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().graph_read_edges


@fieldwise_init
struct GraphEdgesArguments[name: StaticString](Arguments):
    """`ustore_graph_write_edges_t`, and `ustore_graph_remove_edges_t` with the same layout."""

    comptime Written = Unwritten
    comptime symbol: StaticString = Self.name
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var transaction: Address
    var count: c_size_t
    var collections: StridedArguments
    var edges: StridedArguments
    var sources: StridedArguments
    var targets: StridedArguments
    var durability: Durability
    var watch: c_int

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[Unwritten, MutUntrackedOrigin], arena: Address):
        self.database = scope.database
        self.executor = scope.executor
        self.outcome = outcome
        self.completion = completion
        self.transaction = scope.transaction
        self.watch = scope.watch.code

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        comptime if Self.name == "ustore_graph_remove_edges":
            return rebind[Verb[Self]](library.symbols().graph_remove_edges)
        else:
            return rebind[Verb[Self]](library.symbols().graph_write_edges)


@fieldwise_init
struct GraphWriteVerticesArguments(Arguments):
    """`ustore_graph_write_vertices_t`."""

    comptime Written = Unwritten
    comptime symbol: StaticString = "ustore_graph_write_vertices"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var transaction: Address
    var count: c_size_t
    var collections: StridedArguments
    var vertices: StridedArguments
    var durability: Durability
    var watch: c_int

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[Unwritten, MutUntrackedOrigin], arena: Address):
        self.database = scope.database
        self.executor = scope.executor
        self.outcome = outcome
        self.completion = completion
        self.transaction = scope.transaction
        self.watch = scope.watch.code

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().graph_write_vertices


@fieldwise_init
struct GraphRemoveVerticesArguments(Arguments):
    """`ustore_graph_remove_vertices_t`."""

    comptime Written = Unwritten
    comptime symbol: StaticString = "ustore_graph_remove_vertices"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var transaction: Address
    var count: c_size_t
    var collections: StridedArguments
    var vertices: StridedArguments
    var end_vertices: StridedArguments
    var roles: StridedArguments
    var durability: Durability
    var watch: c_int

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[Unwritten, MutUntrackedOrigin], arena: Address):
        self.database = scope.database
        self.executor = scope.executor
        self.outcome = outcome
        self.completion = completion
        self.transaction = scope.transaction
        self.watch = scope.watch.code

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().graph_remove_vertices


@fieldwise_init
struct GraphScanVerticesArguments(Arguments):
    """`ustore_graph_scan_vertices_t`."""

    comptime Written = KeyRunsWritten
    comptime symbol: StaticString = "ustore_graph_scan_vertices"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var view: Address
    var count: c_size_t
    var collections: StridedArguments
    var start_keys: StridedArguments
    var end_keys: StridedArguments
    var direction: ScanDirection
    var predicate: Predicate
    var offsets: Optional[Pointer[Address, MutUntrackedOrigin]]
    var keys: Optional[Pointer[Address, MutUntrackedOrigin]]
    var arena: Address
    var wanted: c_size_t
    var mode: ScanMode
    var progress: Progress

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[KeyRunsWritten, MutUntrackedOrigin], arena: Address):
        self.database = scope.database
        self.executor = scope.executor
        self.outcome = outcome
        self.completion = completion
        self.view = scope.view
        self.offsets = Pointer(to=written[].offsets)
        self.keys = Pointer(to=written[].keys)
        self.arena = arena

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().graph_scan_vertices


@fieldwise_init
struct GraphVerticesCursorInitArguments(ImplicitlyCopyable):
    """`ustore_graph_vertices_cursor_init_t`."""

    var cursor: Optional[Pointer[Address, MutUntrackedOrigin]]
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var view: Address
    var collection: CollectionID
    var start_key: Key
    var end_key: Address
    var direction: ScanDirection
    var predicate: Predicate


@fieldwise_init
struct GraphVerticesCursorNextArguments(Arguments):
    """`ustore_graph_vertices_cursor_next_t`."""

    comptime Written = PulledWritten
    comptime symbol: StaticString = "ustore_graph_vertices_cursor_next"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var cursor: Address
    var keys: Optional[Pointer[Address, MutUntrackedOrigin]]
    var written: Optional[Pointer[c_size_t, MutUntrackedOrigin]]
    var arena: Address
    var wanted: c_size_t
    var progress: Progress

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[PulledWritten, MutUntrackedOrigin], arena: Address):
        self.database = scope.database
        self.executor = scope.executor
        self.outcome = outcome
        self.completion = completion
        self.keys = Pointer(to=written[].keys)
        self.written = Pointer(to=written[].written)
        self.arena = arena

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().graph_vertices_cursor_next


# endregion Arguments

# region Cursors


struct GraphVerticesCursor(Movable):
    """A walk over the vertices of one graph collection, every pull resuming past the last vertex the pull
    before it handed out, freed when it goes out of scope. It holds its database open, and the transaction
    or snapshot it reads through."""

    var library: Library
    """The loaded engine."""
    var database: Address
    """The `ustore_database_t` it walks."""
    var address: Address
    """The `ustore_graph_vertices_cursor_t`."""

    def __init__(out self, *, library: Library, database: Address, address: Address):
        """Adopts a cursor the engine just opened."""
        self.library = library
        self.database = database
        self.address = address

    def __deinit__(deinit self):
        """Frees the cursor and the holds it kept. Calls `ustore_graph_vertices_cursor_free`."""
        self.library.symbols().graph_vertices_cursor_free(self.address)
        _ = self.library^

    def next(self, executor: Executor, mut arena: Arena, wanted: Int, progress: Progress = Progress()) raises Error -> Pulled[origin_of(arena)]:
        """Pulls the next page of up to `wanted` vertices. Calls `ustore_graph_vertices_cursor_next`.

        Raises:
            `Error` with `CANCELLED` when `progress` stops the pull, which leaves the cursor where it was.
        """
        return _block(
            Scope(self.library, self.database, executor.address, NULL_ADDRESS, NULL_ADDRESS, NULL_ADDRESS, Watch.KEYS),
            _Crossing[GraphVerticesCursorNextArguments, Pulled[origin_of(arena)], origin_of(arena), ImmUntrackedOrigin](
                GraphVerticesCursorNextArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, self.address, None, None, NULL_ADDRESS, c_size_t(wanted), progress
                ),
                Pointer(to=arena),
                0,
            ),
        )


# endregion Cursors

# region Verbs


trait GraphVerbs:
    """The graph verbs every blocking receiver offers, written once over the receiver's scope."""

    def _scope(self) -> Scope:
        """What the receiver addresses and the executor it runs on."""
        ...

    def graph_read_edges[
        collections_origin: ImmOrigin, vertices_origin: ImmOrigin, roles_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        vertices: KeysView[vertices_origin],
        mut arena: Arena,
        roles: VertexRolesView[roles_origin] = VertexRolesView(),
        edges: Bool = True,
    ) raises Error -> Edges[origin_of(arena)]:
        """Reads the degree of every vertex, and its edges when asked. Calls `ustore_graph_read_edges`."""
        var count = _tasks(collections.tasks(), vertices.tasks(), roles.tasks())
        return _block(
            self._scope(),
            _Crossing[GraphReadEdgesArguments, Edges[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[vertices_origin](), _anchor[roles_origin]()))](
                GraphReadEdgesArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    vertices.arguments(), roles.arguments(), None, None, None, NULL_ADDRESS, edges,
                ),
                Pointer(to=arena),
                count,
            ),
        )

    def graph_write_edges[
        collections_origin: ImmOrigin, sources_origin: ImmOrigin, targets_origin: ImmOrigin, edges_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        sources: KeysView[sources_origin],
        targets: KeysView[targets_origin],
        edges: KeysView[edges_origin] = KeysView(),
        durability: Durability = Durability.BUFFERED,
    ) raises Error:
        """Writes a batch of edges, their identifiers defaulting when `edges` is zeroed. Calls `ustore_graph_write_edges`."""
        _ = _block(
            self._scope(),
            _Crossing[GraphEdgesArguments["ustore_graph_write_edges"], Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[sources_origin](), _anchor[targets_origin](), _anchor[edges_origin]()))](
                GraphEdgesArguments["ustore_graph_write_edges"](
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), sources.tasks(), targets.tasks(), edges.tasks())), collections.arguments(),
                    edges.arguments(), sources.arguments(), targets.arguments(), durability, 0,
                ),
                None,
                0,
            ),
        )

    def graph_remove_edges[
        collections_origin: ImmOrigin, sources_origin: ImmOrigin, targets_origin: ImmOrigin, edges_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        sources: KeysView[sources_origin],
        targets: KeysView[targets_origin],
        edges: KeysView[edges_origin] = KeysView(),
        durability: Durability = Durability.BUFFERED,
    ) raises Error:
        """Removes a batch of edges. Calls `ustore_graph_remove_edges`."""
        _ = _block(
            self._scope(),
            _Crossing[GraphEdgesArguments["ustore_graph_remove_edges"], Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[sources_origin](), _anchor[targets_origin](), _anchor[edges_origin]()))](
                GraphEdgesArguments["ustore_graph_remove_edges"](
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), sources.tasks(), targets.tasks(), edges.tasks())), collections.arguments(),
                    edges.arguments(), sources.arguments(), targets.arguments(), durability, 0,
                ),
                None,
                0,
            ),
        )

    def graph_write_vertices[
        collections_origin: ImmOrigin, vertices_origin: ImmOrigin, //
    ](self, collections: CollectionsView[collections_origin], vertices: KeysView[vertices_origin], durability: Durability = Durability.BUFFERED) raises Error:
        """Inserts a batch of vertices unless present. Calls `ustore_graph_write_vertices`."""
        _ = _block(
            self._scope(),
            _Crossing[GraphWriteVerticesArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[vertices_origin]()))](
                GraphWriteVerticesArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS, c_size_t(_tasks(collections.tasks(), vertices.tasks())),
                    collections.arguments(), vertices.arguments(), durability, 0,
                ),
                None,
                0,
            ),
        )

    def graph_remove_vertices[
        collections_origin: ImmOrigin, vertices_origin: ImmOrigin, end_origin: ImmOrigin, roles_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        vertices: KeysView[vertices_origin],
        end_vertices: KeysView[end_origin] = KeysView(),
        roles: VertexRolesView[roles_origin] = VertexRolesView(),
        durability: Durability = Durability.BUFFERED,
    ) raises Error:
        """Removes a batch of vertices with their edges, or every vertex of a range per task when `end_vertices`
        are given. Calls `ustore_graph_remove_vertices`."""
        _ = _block(
            self._scope(),
            _Crossing[GraphRemoveVerticesArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[vertices_origin](), _anchor[end_origin](), _anchor[roles_origin]()))](
                GraphRemoveVerticesArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), vertices.tasks(), end_vertices.tasks(), roles.tasks())), collections.arguments(),
                    vertices.arguments(), end_vertices.arguments(), roles.arguments(), durability, 0,
                ),
                None,
                0,
            ),
        )

    def graph_scan_vertices[
        collections_origin: ImmOrigin, start_origin: ImmOrigin, end_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        start_keys: KeysView[start_origin],
        wanted: Int,
        mut arena: Arena,
        config: GraphScanConfig[end_origin] = GraphScanConfig(),
    ) raises Error -> KeyRuns[origin_of(arena)]:
        """Lists up to `wanted` vertices from a start key in every graph collection. Calls `ustore_graph_scan_vertices`."""
        var count = _tasks(collections.tasks(), start_keys.tasks(), config.end_keys.tasks())
        return _block(
            self._scope(),
            _Crossing[GraphScanVerticesArguments, KeyRuns[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[start_origin](), _anchor[end_origin]()))](
                GraphScanVerticesArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    start_keys.arguments(), config.end_keys.arguments(), config.direction, config.predicate, None, None,
                    NULL_ADDRESS, c_size_t(wanted), config.mode, config.progress,
                ),
                Pointer(to=arena),
                count,
            ),
        )

    def graph_vertices_cursor(self, collection: CollectionID, start_key: Key, config: GraphCursorConfig = GraphCursorConfig()) raises Error -> GraphVerticesCursor:
        """Opens a walk over the vertices of one graph collection, reading nothing until its first pull.
        Calls `ustore_graph_vertices_cursor_init`."""
        var scope = self._scope()
        var created = stack_allocation[1, Address]().unsafe_origin_cast[MutUntrackedOrigin]()
        created[] = NULL_ADDRESS
        var outcome = stack_allocation[1, Outcome]().unsafe_origin_cast[MutUntrackedOrigin]()
        outcome[] = Outcome()
        var end = stack_allocation[1, Key]().unsafe_origin_cast[MutUntrackedOrigin]()
        var bounded = False
        if case .Some(key) = config.end_key:
            end[] = key
            bounded = True
        _cross(
            GraphVerticesCursorInitArguments(
                created, scope.database, scope.executor, outcome, scope.view, collection, start_key,
                Int(end) if bounded else NULL_ADDRESS, config.direction, config.predicate,
            ),
            scope.library.symbols().graph_vertices_cursor_init,
        )
        return GraphVerticesCursor(
            library=scope.library, database=scope.database, address=_created(scope.library, outcome, created, "ustore_graph_vertices_cursor_init")
        )

    def graph_sample_vertices[
        collections_origin: ImmOrigin, //
    ](self, collections: CollectionsView[collections_origin], wanted: Int, mut arena: Arena) raises Error -> KeyRuns[origin_of(arena)]:
        """Draws up to `wanted` vertices uniformly at random from every graph collection. Calls `ustore_graph_sample_vertices`."""
        var count = len(collections)
        return _block(
            self._scope(),
            _Crossing[SampleArguments["ustore_graph_sample_vertices"], KeyRuns[origin_of(arena)], origin_of(arena), collections_origin](
                SampleArguments["ustore_graph_sample_vertices"](
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    None, None, NULL_ADDRESS, c_size_t(wanted),
                ),
                Pointer(to=arena),
                count,
            ),
        )

    def graph_measure_vertices[
        collections_origin: ImmOrigin, start_origin: ImmOrigin, end_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        start_keys: KeysView[start_origin],
        end_keys: KeysView[end_origin],
        mut arena: Arena,
        direction: ScanDirection = ScanDirection.ASCENDING,
    ) raises Error -> Measures[origin_of(arena)]:
        """Estimates the vertices and the space inside a batch of key ranges. Calls `ustore_graph_measure_vertices`."""
        var count = _tasks(collections.tasks(), start_keys.tasks(), end_keys.tasks())
        return _block(
            self._scope(),
            _Crossing[MeasureArguments["ustore_graph_measure_vertices"], Measures[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[start_origin](), _anchor[end_origin]()))](
                _measure["ustore_graph_measure_vertices"](count, collections.arguments(), start_keys.arguments(), end_keys.arguments(), direction),
                Pointer(to=arena),
                count,
            ),
        )


trait GraphDeferredVerbs:
    """The graph verbs every deferred receiver offers, each answering a `Deferred` at once; the arguments
    are those of `GraphVerbs`."""

    def _scope(self) -> Scope:
        """What the receiver addresses and the executor it submits to."""
        ...

    def graph_read_edges[
        collections_origin: ImmOrigin, vertices_origin: ImmOrigin, roles_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        vertices: KeysView[vertices_origin],
        mut arena: Arena,
        roles: VertexRolesView[roles_origin] = VertexRolesView(),
        edges: Bool = True,
    ) -> Deferred[
        _Crossing[GraphReadEdgesArguments, Edges[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[vertices_origin](), _anchor[roles_origin]()))]
    ]:
        """Submits `GraphVerbs.graph_read_edges`. Calls `ustore_graph_read_edges`."""
        var count = _tasks(collections.tasks(), vertices.tasks(), roles.tasks())
        return {
            self._scope(),
            _Crossing[GraphReadEdgesArguments, Edges[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[vertices_origin](), _anchor[roles_origin]()))](
                GraphReadEdgesArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    vertices.arguments(), roles.arguments(), None, None, None, NULL_ADDRESS, edges,
                ),
                Pointer(to=arena),
                count,
            ),
        }

    def graph_write_edges[
        collections_origin: ImmOrigin, sources_origin: ImmOrigin, targets_origin: ImmOrigin, edges_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        sources: KeysView[sources_origin],
        targets: KeysView[targets_origin],
        edges: KeysView[edges_origin] = KeysView(),
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[
        _Crossing[GraphEdgesArguments["ustore_graph_write_edges"], Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[sources_origin](), _anchor[targets_origin](), _anchor[edges_origin]()))]
    ]:
        """Submits `GraphVerbs.graph_write_edges`. Calls `ustore_graph_write_edges`."""
        return {
            self._scope(),
            _Crossing[GraphEdgesArguments["ustore_graph_write_edges"], Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[sources_origin](), _anchor[targets_origin](), _anchor[edges_origin]()))](
                GraphEdgesArguments["ustore_graph_write_edges"](
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), sources.tasks(), targets.tasks(), edges.tasks())), collections.arguments(),
                    edges.arguments(), sources.arguments(), targets.arguments(), durability, 0,
                ),
                None,
                0,
            ),
        }

    def graph_remove_edges[
        collections_origin: ImmOrigin, sources_origin: ImmOrigin, targets_origin: ImmOrigin, edges_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        sources: KeysView[sources_origin],
        targets: KeysView[targets_origin],
        edges: KeysView[edges_origin] = KeysView(),
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[
        _Crossing[GraphEdgesArguments["ustore_graph_remove_edges"], Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[sources_origin](), _anchor[targets_origin](), _anchor[edges_origin]()))]
    ]:
        """Submits `GraphVerbs.graph_remove_edges`. Calls `ustore_graph_remove_edges`."""
        return {
            self._scope(),
            _Crossing[GraphEdgesArguments["ustore_graph_remove_edges"], Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[sources_origin](), _anchor[targets_origin](), _anchor[edges_origin]()))](
                GraphEdgesArguments["ustore_graph_remove_edges"](
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), sources.tasks(), targets.tasks(), edges.tasks())), collections.arguments(),
                    edges.arguments(), sources.arguments(), targets.arguments(), durability, 0,
                ),
                None,
                0,
            ),
        }

    def graph_write_vertices[
        collections_origin: ImmOrigin, vertices_origin: ImmOrigin, //
    ](self, collections: CollectionsView[collections_origin], vertices: KeysView[vertices_origin], durability: Durability = Durability.BUFFERED) -> Deferred[
        _Crossing[GraphWriteVerticesArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[vertices_origin]()))]
    ]:
        """Submits `GraphVerbs.graph_write_vertices`. Calls `ustore_graph_write_vertices`."""
        return {
            self._scope(),
            _Crossing[GraphWriteVerticesArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[vertices_origin]()))](
                GraphWriteVerticesArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS, c_size_t(_tasks(collections.tasks(), vertices.tasks())),
                    collections.arguments(), vertices.arguments(), durability, 0,
                ),
                None,
                0,
            ),
        }

    def graph_remove_vertices[
        collections_origin: ImmOrigin, vertices_origin: ImmOrigin, end_origin: ImmOrigin, roles_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        vertices: KeysView[vertices_origin],
        end_vertices: KeysView[end_origin] = KeysView(),
        roles: VertexRolesView[roles_origin] = VertexRolesView(),
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[
        _Crossing[GraphRemoveVerticesArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[vertices_origin](), _anchor[end_origin](), _anchor[roles_origin]()))]
    ]:
        """Submits `GraphVerbs.graph_remove_vertices`. Calls `ustore_graph_remove_vertices`."""
        return {
            self._scope(),
            _Crossing[GraphRemoveVerticesArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[vertices_origin](), _anchor[end_origin](), _anchor[roles_origin]()))](
                GraphRemoveVerticesArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), vertices.tasks(), end_vertices.tasks(), roles.tasks())), collections.arguments(),
                    vertices.arguments(), end_vertices.arguments(), roles.arguments(), durability, 0,
                ),
                None,
                0,
            ),
        }

    def graph_scan_vertices[
        collections_origin: ImmOrigin, start_origin: ImmOrigin, end_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        start_keys: KeysView[start_origin],
        wanted: Int,
        mut arena: Arena,
        config: GraphScanConfig[end_origin] = GraphScanConfig(),
    ) -> Deferred[
        _Crossing[GraphScanVerticesArguments, KeyRuns[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[start_origin](), _anchor[end_origin]()))]
    ]:
        """Submits `GraphVerbs.graph_scan_vertices`. Calls `ustore_graph_scan_vertices`."""
        var count = _tasks(collections.tasks(), start_keys.tasks(), config.end_keys.tasks())
        return {
            self._scope(),
            _Crossing[GraphScanVerticesArguments, KeyRuns[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[start_origin](), _anchor[end_origin]()))](
                GraphScanVerticesArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    start_keys.arguments(), config.end_keys.arguments(), config.direction, config.predicate, None, None,
                    NULL_ADDRESS, c_size_t(wanted), config.mode, config.progress,
                ),
                Pointer(to=arena),
                count,
            ),
        }

    def graph_sample_vertices[
        collections_origin: ImmOrigin, //
    ](self, collections: CollectionsView[collections_origin], wanted: Int, mut arena: Arena) -> Deferred[
        _Crossing[SampleArguments["ustore_graph_sample_vertices"], KeyRuns[origin_of(arena)], origin_of(arena), collections_origin]
    ]:
        """Submits `GraphVerbs.graph_sample_vertices`. Calls `ustore_graph_sample_vertices`."""
        var count = len(collections)
        return {
            self._scope(),
            _Crossing[SampleArguments["ustore_graph_sample_vertices"], KeyRuns[origin_of(arena)], origin_of(arena), collections_origin](
                SampleArguments["ustore_graph_sample_vertices"](
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    None, None, NULL_ADDRESS, c_size_t(wanted),
                ),
                Pointer(to=arena),
                count,
            ),
        }

    def graph_measure_vertices[
        collections_origin: ImmOrigin, start_origin: ImmOrigin, end_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        start_keys: KeysView[start_origin],
        end_keys: KeysView[end_origin],
        mut arena: Arena,
        direction: ScanDirection = ScanDirection.ASCENDING,
    ) -> Deferred[
        _Crossing[MeasureArguments["ustore_graph_measure_vertices"], Measures[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[start_origin](), _anchor[end_origin]()))]
    ]:
        """Submits `GraphVerbs.graph_measure_vertices`. Calls `ustore_graph_measure_vertices`."""
        var count = _tasks(collections.tasks(), start_keys.tasks(), end_keys.tasks())
        return {
            self._scope(),
            _Crossing[MeasureArguments["ustore_graph_measure_vertices"], Measures[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[start_origin](), _anchor[end_origin]()))](
                _measure["ustore_graph_measure_vertices"](count, collections.arguments(), start_keys.arguments(), end_keys.arguments(), direction),
                Pointer(to=arena),
                count,
            ),
        }


# endregion Verbs
