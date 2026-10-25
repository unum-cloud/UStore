"""NetworkX-compatible graphs over the graph and document verbs of `ustore.lib`.

A graph collection holds every vertex and its edges, while two optional document collections hold
the attributes of vertices, keyed by vertex, and of edges, keyed by edge identifier. Every read goes
to the store, so a view holds nothing that could go stale, and every verb works the same over a
`Database`, a `Transaction` and a blocking `View` of either. Every call reads into a one-off
`Arena` and copies what it needs into Python objects, which costs an arena per call, while the
graph verbs of `ustore.lib` take arenas a loop reuses. Over a `DeferredView`, `has_node` and
`read_edges`, which take one call each, answer a `Deferred`, while every method built of several
calls raises TypeError. Vertices and edges are named by `uuid.UUID`, as every key is, and any
spelling of a key the binding accepts names them on the way in.

A multigraph gives every edge an identifier of its own, which is the key NetworkX reports; a simple
graph does too once it keeps edge attributes, and otherwise stores every edge under
`edge_id_default`.
"""

from __future__ import annotations

import json
from collections.abc import Buffer, Iterable, Iterator, Mapping, Sequence
from itertools import batched
from typing import Any, ClassVar
from uuid import UUID

from ustore.keys import PAGE_KEYS, KeyLike, as_key, is_one_key, scan_keys
from ustore.lib import (
    Arena,
    Database,
    Deferred,
    DeferredView,
    Modality,
    Modification,
    Neighborhoods,
    Role,
    Transaction,
    View,
    edge_id_default,
)

type Blocking = Database | Transaction | View
type Receiver = Blocking | DeferredView
type CollectionLike = UUID | str | None
type Attributes = dict[str, Any]
type Triple = tuple[UUID, UUID, UUID]
type KeysLike = Sequence[KeyLike] | Buffer


def resolve_collection(view: Receiver, collection: CollectionLike, modality: Modality) -> UUID:
    """Answers the ID a collection argument addresses, creating a named one that is missing to hold `modality`."""
    if collection is None:
        return UUID(int=0)
    if not isinstance(collection, str):
        return collection
    database = view if isinstance(view, Database) else view.database
    if database.collection_contains(collection):
        return database.collection_find(collection, Arena())
    return database.collection_create(collection, modality=modality)


def triples_of(neighborhoods: Neighborhoods, index: int) -> list[Triple]:
    """The (source, target, edge) triples one vertex of a read found."""
    found = neighborhoods[index]
    return [] if found is None else list(found)


class NodeView:
    """The vertices of a graph: iterable, sized, and callable for their attributes, as `Graph.nodes` is."""

    def __init__(self, graph: Graph) -> None:
        self.graph = graph

    def __iter__(self) -> Iterator[UUID]:
        return iter(self.graph)

    def __len__(self) -> int:
        return len(self.graph)

    def __contains__(self, node: KeyLike) -> bool:
        return node in self.graph

    def __getitem__(self, node: KeyLike) -> Attributes:
        if node not in self.graph:
            raise KeyError(node)
        return self.graph.read_attributes(self.graph.nodes_collection, [node])[0]

    def __call__(self, data: bool | str = False, default: Any = None) -> Iterator[UUID] | Iterator[tuple[UUID, Any]]:
        if data is False:
            return iter(self.graph)
        return self._with_data(data, default)

    def _with_data(self, data: bool | str, default: Any) -> Iterator[tuple[UUID, Any]]:
        for page in batched(self.graph, PAGE_KEYS):
            attributes = self.graph.read_attributes(self.graph.nodes_collection, page)
            for node, found in zip(page, attributes, strict=True):
                yield (node, found) if data is True else (node, found.get(data, default))


class EdgeView:
    """The edges of a graph: iterable, sized, and callable for keys and attributes, as `Graph.edges` is."""

    def __init__(self, graph: Graph) -> None:
        self.graph = graph

    def __iter__(self) -> Iterator[tuple[UUID, ...]]:
        """Every edge as its two ends, followed by its key in a multigraph."""
        return self(keys=self.graph.MULTIGRAPH)

    def __len__(self) -> int:
        return self.graph.number_of_edges()

    def __contains__(self, edge: tuple[KeyLike, KeyLike]) -> bool:
        return self.graph.has_edge(*edge)

    def __call__(
        self,
        nbunch: KeyLike | Iterable[KeyLike] | None = None,
        data: bool | str = False,
        keys: bool = False,
        default: Any = None,
    ) -> Iterator[tuple[Any, ...]]:
        if keys and not self.graph.MULTIGRAPH:
            raise TypeError("Only a multigraph reports edge keys")
        for page in batched(self.graph.edge_triples(nbunch), PAGE_KEYS):
            identifiers = [edge for _, _, edge in page]
            attributes = (
                self.graph.read_attributes(self.graph.edges_collection, identifiers)
                if data is not False
                else [None] * len(page)
            )
            for (source, target, edge), found in zip(page, attributes, strict=True):
                head = (source, target, edge) if keys else (source, target)
                if data is False:
                    yield head
                elif data is True:
                    yield (*head, found)
                else:
                    yield (*head, found.get(data, default))


class DegreeView:
    """Degrees of vertices in one role: indexable by vertex, iterable as pairs, callable with weights."""

    def __init__(self, graph: Graph, role: Role, weight: str | None = None, nodes: list[KeyLike] | None = None) -> None:
        self.graph = graph
        self.role = role
        self.weight = weight
        self.nodes = nodes

    def __getitem__(self, node: KeyLike) -> int | float:
        if node not in self.graph:
            raise KeyError(node)
        return next(self._degrees([node]))[1]

    def __iter__(self) -> Iterator[tuple[KeyLike, int | float]]:
        pages = batched(self.graph if self.nodes is None else self.nodes, PAGE_KEYS)
        for page in pages:
            yield from self._degrees(list(page))

    def __call__(
        self, nbunch: KeyLike | Iterable[KeyLike] | None = None, weight: str | None = None
    ) -> int | float | DegreeView:
        if nbunch is not None and is_one_key(nbunch):
            return DegreeView(self.graph, self.role, weight)[nbunch]
        return DegreeView(self.graph, self.role, weight, None if nbunch is None else list(nbunch))

    def _degrees(self, nodes: list[KeyLike]) -> Iterator[tuple[KeyLike, int | float]]:
        if self.weight is None:
            counts = self.graph.blocking.graph_degrees(
                nodes, Arena(), role=self.role, collection=self.graph.graph_collection
            )
            yield from ((node, count or 0) for node, count in zip(nodes, counts, strict=True))
            return
        found = self.graph.blocking.graph_read_edges(
            nodes, Arena(), role=self.role, collection=self.graph.graph_collection
        )
        for index, node in enumerate(nodes):
            identifiers = [edge for _, _, edge in triples_of(found, index)]
            attributes = self.graph.read_attributes(self.graph.edges_collection, identifiers)
            yield node, sum(entry.get(self.weight, 1) for entry in attributes)


class Graph:
    """An undirected graph holding at most one edge between two vertices, as `networkx.Graph` does."""

    DIRECTED: ClassVar[bool] = False
    MULTIGRAPH: ClassVar[bool] = False

    def __init__(
        self,
        view: Receiver,
        graph: UUID | str,
        *,
        nodes: UUID | str | None = None,
        edges: UUID | str | None = None,
    ) -> None:
        """Opens a graph in the graph collection `graph`, with vertex attributes in `nodes` and edge attributes in `edges`.

        Named collections that do not exist yet are created, at the head of the database, graph and documents.
        """
        self.view = view
        self.graph_collection = resolve_collection(view, graph, Modality.GRAPH)
        self.nodes_collection = None if nodes is None else resolve_collection(view, nodes, Modality.DOCS)
        self.edges_collection = None if edges is None else resolve_collection(view, edges, Modality.DOCS)
        # A graph collection holds vertices rather than documents, so attributes live in collections of their own.
        if self.graph_collection in (self.nodes_collection, self.edges_collection):
            raise ValueError("Attributes live in a collection apart from the graph")
        self.next_edge_id: int | None = None

    def is_directed(self) -> bool:
        return self.DIRECTED

    def is_multigraph(self) -> bool:
        return self.MULTIGRAPH

    @property
    def blocking(self) -> Blocking:
        """The receiver of every method built of several calls, which a deferred view cannot answer as one."""
        if isinstance(self.view, DeferredView):
            raise TypeError(
                "A graph over a deferred view answers a Deferred from has_node() and read_edges() alone; "
                "call the graph verbs, such as graph_read_edges(), on the view instead"
            )
        return self.view

    # region Attributes

    def read_attributes(self, collection: UUID | None, keys: Sequence[KeyLike]) -> list[Attributes]:
        """The attribute document of every key, an empty one where there is none."""
        if collection is None or len(keys) == 0:
            return [{} for _ in keys]
        documents = self.blocking.docs_read(keys, Arena(), collection=collection)
        return [{} if document is None else json.loads(bytes(document)) for document in documents]

    def merge_attributes(self, collection: UUID | None, keys: KeysLike, documents: Sequence[Attributes]) -> None:
        """Merges one attribute document into every key, or one per key, as RFC 7386 does."""
        if collection is None:
            raise ValueError("Attributes need a collection of their own, given when the graph is opened")
        encoded = json.dumps(documents[0]) if len(documents) == 1 else [json.dumps(entry) for entry in documents]
        self.blocking.docs_write(keys, encoded, modification=Modification.MERGE, collection=collection)

    # endregion Attributes

    # region Vertices

    def __iter__(self) -> Iterator[UUID]:
        return scan_keys(self.blocking, self.graph_collection, modality=Modality.GRAPH)

    def __len__(self) -> int:
        return sum(1 for _ in self)

    def __contains__(self, node: KeyLike) -> bool:
        return self.blocking.contains(node, collection=self.graph_collection)

    def __getitem__(self, node: KeyLike) -> dict[UUID, Any]:
        """The neighbours of a vertex with the attributes of the edges reaching them, keyed by edge in a multigraph."""
        if node not in self:
            raise KeyError(node)
        triples = list(self.edge_triples(node))
        attributes = self.read_attributes(self.edges_collection, [edge for _, _, edge in triples])
        adjacency: dict[UUID, Any] = {}
        for (_, neighbour, edge), found in zip(triples, attributes, strict=True):
            if self.MULTIGRAPH:
                adjacency.setdefault(neighbour, {})[edge] = found
            else:
                adjacency[neighbour] = found
        return adjacency

    @property
    def nodes(self) -> NodeView:
        return NodeView(self)

    def number_of_nodes(self) -> int:
        return len(self)

    def has_node(self, node: KeyLike) -> bool | Deferred[bool]:
        """Whether the vertex is present, or the `Deferred` of it over a deferred view."""
        return self.view.contains(node, collection=self.graph_collection)

    def add_node(self, node: KeyLike, **attributes: Any) -> None:
        self.add_nodes_from([node], **attributes)

    def add_nodes_from(self, nodes: KeysLike | Iterable[KeyLike], **attributes: Any) -> None:
        """Inserts vertices, merging `attributes` into every one of them."""
        keys = nodes if isinstance(nodes, (Buffer, Sequence)) else list(nodes)
        # Only absent vertices are written, since a batch whose every vertex exists is refused by the C ABI.
        lengths = self.blocking.read(keys, Arena(), collection=self.graph_collection).lengths
        missing = [key for key, length in zip(keys, lengths, strict=True) if length is None]
        if missing:
            self.blocking.graph_write_vertices(missing, collection=self.graph_collection)
        if attributes:
            self.merge_attributes(self.nodes_collection, keys, [attributes])

    def remove_node(self, node: KeyLike) -> None:
        if node not in self:
            raise KeyError(node)
        self.remove_nodes_from([node])

    def remove_nodes_from(self, nodes: Iterable[KeyLike]) -> None:
        """Removes vertices with every edge they take part in and the attributes of both."""
        keys = list(nodes)
        if self.edges_collection is not None:
            found = self.blocking.graph_read_edges(keys, Arena(), role=Role.ANY, collection=self.graph_collection)
            identifiers = [edge for index in range(len(keys)) for _, _, edge in triples_of(found, index)]
            self.blocking.docs_remove(identifiers, collection=self.edges_collection)
        self.blocking.graph_remove_vertices(keys, collection=self.graph_collection)
        if self.nodes_collection is not None:
            self.blocking.docs_remove(keys, collection=self.nodes_collection)

    # endregion Vertices

    # region Edges

    @property
    def outgoing_role(self) -> Role:
        """The role a vertex plays in the edges it reaches its neighbours by."""
        return Role.SOURCE if self.DIRECTED else Role.ANY

    def canonical_pair(self, source: KeyLike, target: KeyLike) -> tuple[UUID, UUID]:
        """The pair both orientations of an undirected edge share."""
        source, target = as_key(source), as_key(target)
        return (source, target) if self.DIRECTED else (min(source, target), max(source, target))

    def stored_between(self, sources: Sequence[KeyLike]) -> dict[tuple[UUID, UUID], list[Triple]]:
        """Every stored edge leaving `sources`, grouped by canonical pair, in stored order."""
        unique = list(dict.fromkeys(map(as_key, sources)))
        found = self.blocking.graph_read_edges(
            unique, Arena(), role=self.outgoing_role, collection=self.graph_collection
        )
        grouped: dict[tuple[UUID, UUID], list[Triple]] = {}
        for index in range(len(unique)):
            for triple in triples_of(found, index):
                entries = grouped.setdefault(self.canonical_pair(triple[0], triple[1]), [])
                # A self-loop of an undirected graph is found once leaving and once entering.
                if triple not in entries:
                    entries.append(triple)
        return grouped

    def allocate_edge_ids(self, count: int) -> list[UUID]:
        """Hands out identifiers past every one the graph already holds, counting as integers do."""
        if self.next_edge_id is None:
            held = (edge.int for _, _, edge in self.edge_triples(None) if edge != edge_id_default)
            self.next_edge_id = max(held, default=-1) + 1
        first = self.next_edge_id
        self.next_edge_id += count
        return [UUID(int=edge) for edge in range(first, first + count)]

    def add_edges_from_arrays(
        self,
        sources: Sequence[KeyLike],
        targets: Sequence[KeyLike],
        keys: Sequence[KeyLike | None] | None = None,
        *,
        columns: Mapping[str, Sequence[Any]] | None = None,
        **attributes: Any,
    ) -> list[UUID]:
        """Inserts a batch of edges, merging `attributes` into all of them and `columns` edge by edge.

        Returns:
            The identifier of every edge, in the order the edges were given.
        """
        sources = [as_key(source) for source in sources]
        targets = [as_key(target) for target in targets]
        if len(sources) != len(targets):
            raise ValueError(f"Got {len(sources)} sources and {len(targets)} targets")
        wanted = [None] * len(sources) if keys is None else [None if key is None else as_key(key) for key in keys]
        if not self.MULTIGRAPH and any(key is not None for key in wanted):
            raise TypeError("Only a multigraph takes edge keys")

        identifiers: list[UUID] = []
        written: list[Triple] = []
        if self.MULTIGRAPH:
            fresh = iter(self.allocate_edge_ids(sum(key is None for key in wanted)))
            identifiers = [next(fresh) if key is None else key for key in wanted]
            # A key the caller chose is never handed out again.
            self.next_edge_id = max([self.next_edge_id or 0, *(key.int + 1 for key in wanted if key is not None)])
            written = list(zip(sources, targets, identifiers, strict=True))
        else:
            stored = self.stored_between(sources)
            added: dict[tuple[UUID, UUID], UUID] = {}
            for source, target in zip(sources, targets, strict=True):
                pair = self.canonical_pair(source, target)
                if pair in stored:
                    identifiers.append(stored[pair][0][2])
                    continue
                if pair not in added:
                    # An edge only needs an identifier of its own once its attributes are kept.
                    keeps_attributes = self.edges_collection is not None
                    added[pair] = self.allocate_edge_ids(1)[0] if keeps_attributes else edge_id_default
                    written.append((source, target, added[pair]))
                identifiers.append(added[pair])

        if written:
            written_sources, written_targets, written_edges = zip(*written, strict=True)
            self.blocking.graph_write_edges(
                written_sources, written_targets, edges=written_edges, collection=self.graph_collection
            )
        if attributes or columns:
            documents = [
                {**attributes, **{name: values[index] for name, values in (columns or {}).items()}}
                for index in range(len(identifiers))
            ]
            self.merge_attributes(self.edges_collection, identifiers, documents if columns else documents[:1])
        return identifiers

    def add_edge(self, source: KeyLike, target: KeyLike, **attributes: Any) -> None:
        self.add_edges_from_arrays([source], [target], **attributes)

    def add_edges_from(self, ebunch: Iterable[tuple[Any, ...]], **attributes: Any) -> None:
        """Inserts (source, target) pairs, (source, target, attributes) triples, or keyed quadruples."""
        for edge in ebunch:
            source, target, *rest = edge
            key = rest[0] if self.MULTIGRAPH and rest and not isinstance(rest[0], Mapping) else None
            own = rest[-1] if rest and isinstance(rest[-1], Mapping) else {}
            self.add_edges_from_arrays([source], [target], None if key is None else [key], **{**attributes, **own})

    def remove_edges_from_arrays(
        self, sources: Sequence[KeyLike], targets: Sequence[KeyLike], keys: Sequence[KeyLike | None] | None = None
    ) -> int:
        """Removes a batch of edges, the newest one of a pair where a multigraph names no key.

        Returns:
            The number of edges removed; pairs holding no edge are skipped.
        """
        sources = [as_key(source) for source in sources]
        targets = [as_key(target) for target in targets]
        wanted = [None] * len(sources) if keys is None else [None if key is None else as_key(key) for key in keys]
        stored = self.stored_between(sources)
        removed: list[Triple] = []
        for source, target, key in zip(sources, targets, wanted, strict=True):
            remaining = [
                triple for triple in stored.get(self.canonical_pair(source, target), []) if triple not in removed
            ]
            if not self.MULTIGRAPH:
                removed.extend(remaining)
            elif key is not None:
                removed.extend(triple for triple in remaining if triple[2] == key)
            elif remaining:
                removed.append(max(remaining, key=lambda triple: triple[2]))
        if removed:
            removed_sources, removed_targets, removed_edges = zip(*removed, strict=True)
            self.blocking.graph_remove_edges(
                removed_sources, removed_targets, edges=removed_edges, collection=self.graph_collection
            )
            if self.edges_collection is not None:
                self.blocking.docs_remove(removed_edges, collection=self.edges_collection)
        return len(removed)

    def remove_edge(self, source: KeyLike, target: KeyLike) -> None:
        if self.remove_edges_from_arrays([source], [target]) == 0:
            raise KeyError((source, target))

    def remove_edges_from(self, ebunch: Iterable[tuple[KeyLike, ...]]) -> None:
        for edge in ebunch:
            key = edge[2] if self.MULTIGRAPH and len(edge) > 2 else None
            self.remove_edges_from_arrays([edge[0]], [edge[1]], None if key is None else [key])

    def edge_triples(self, nbunch: KeyLike | Iterable[KeyLike] | None) -> Iterator[Triple]:
        """Every edge once, as stored for the whole graph and oriented away from `nbunch` otherwise."""
        if nbunch is None:
            for page in batched(self, PAGE_KEYS):
                found = self.blocking.graph_read_edges(
                    page, Arena(), role=Role.SOURCE, collection=self.graph_collection
                )
                for index in range(len(page)):
                    yield from triples_of(found, index)
            return
        nodes = [as_key(nbunch)] if is_one_key(nbunch) else [as_key(node) for node in nbunch]
        found = self.blocking.graph_read_edges(
            nodes, Arena(), role=self.outgoing_role, collection=self.graph_collection
        )
        seen: set[Triple] = set()
        for index, node in enumerate(nodes):
            for source, target, edge in triples_of(found, index):
                oriented = (source, target, edge) if source == node else (target, source, edge)
                identity = (*self.canonical_pair(source, target), edge)
                if identity not in seen:
                    seen.add(identity)
                    yield oriented

    def read_edges(
        self, vertices: KeyLike | Sequence[KeyLike], role: Role = Role.ANY
    ) -> Neighborhoods | Deferred[Neighborhoods]:
        """The edges of every vertex in `role`, as the stored (source, target, edge) triples, or the `Deferred` of
        them over a deferred view, viewing a one-off arena that lives as long as they do."""
        batch = [vertices] if is_one_key(vertices) else vertices
        return self.view.graph_read_edges(batch, Arena(), role=role, collection=self.graph_collection)

    @property
    def edges(self) -> EdgeView:
        return EdgeView(self)

    def has_edge(self, source: KeyLike, target: KeyLike, key: KeyLike | None = None) -> bool:
        between = self.stored_between([source]).get(self.canonical_pair(source, target), [])
        return any(key is None or edge == as_key(key) for _, _, edge in between)

    def number_of_edges(self, source: KeyLike | None = None, target: KeyLike | None = None) -> int:
        if source is None or target is None:
            return sum(degree for _, degree in DegreeView(self, Role.SOURCE))
        return len(self.stored_between([source]).get(self.canonical_pair(source, target), []))

    def size(self, weight: str | None = None) -> int | float:
        if weight is None:
            return self.number_of_edges()
        return sum(attributes.get(weight, 1) for _, _, attributes in self.edges(data=True))

    def neighbors(self, node: KeyLike) -> Iterator[UUID]:
        """The distinct vertices an edge leads to from `node`, in ascending order."""
        return iter(sorted({target for _, target, _ in self.edge_triples(node)}))

    def get_edge_data(self, source: KeyLike, target: KeyLike, default: Any = None) -> Any:
        between = self.stored_between([source]).get(self.canonical_pair(source, target), [])
        if not between:
            return default
        return self.read_attributes(self.edges_collection, [between[0][2]])[0]

    @property
    def degree(self) -> DegreeView:
        return DegreeView(self, Role.ANY)

    def clear_edges(self) -> None:
        """Removes every edge and its attributes, keeping the vertices."""
        for page in batched(self, PAGE_KEYS):
            found = self.blocking.graph_read_edges(page, Arena(), role=Role.SOURCE, collection=self.graph_collection)
            stored = [triple for index in range(len(page)) for triple in triples_of(found, index)]
            if stored:
                sources, targets, identifiers = zip(*stored, strict=True)
                self.blocking.graph_remove_edges(sources, targets, edges=identifiers, collection=self.graph_collection)
        if self.edges_collection is not None:
            for page in batched(scan_keys(self.blocking, self.edges_collection, modality=Modality.DOCS), PAGE_KEYS):
                self.blocking.docs_remove(page, collection=self.edges_collection)
        self.next_edge_id = None

    def clear(self) -> None:
        """Removes every vertex, every edge, and the attributes of both."""
        for page in batched(self, PAGE_KEYS):
            self.blocking.graph_remove_vertices(page, collection=self.graph_collection)
        for collection in (self.nodes_collection, self.edges_collection):
            if collection is not None:
                for page in batched(scan_keys(self.blocking, collection, modality=Modality.DOCS), PAGE_KEYS):
                    self.blocking.docs_remove(page, collection=collection)
        self.next_edge_id = None

    # endregion Edges

    # region Attribute Maps

    def get_node_attributes(self, name: str) -> dict[UUID, Any]:
        return {node: found[name] for node, found in self.nodes(data=True) if name in found}

    def set_node_attributes(self, values: Any, name: str | None = None) -> None:
        """Merges `values` into vertices: one value for all of them, a value per vertex, or a document per vertex."""
        if name is None:
            nodes = list(values)
            self.merge_attributes(self.nodes_collection, nodes, [dict(values[node]) for node in nodes])
        elif isinstance(values, Mapping):
            nodes = list(values)
            self.merge_attributes(self.nodes_collection, nodes, [{name: values[node]} for node in nodes])
        else:
            for page in batched(self, PAGE_KEYS):
                self.merge_attributes(self.nodes_collection, page, [{name: values}])

    def edge_label(self, source: UUID, target: UUID, edge: UUID) -> tuple[UUID, ...]:
        """How NetworkX names an edge: with its key in a multigraph, by its ends otherwise."""
        return (source, target, edge) if self.MULTIGRAPH else (source, target)

    def edge_identifier(self, label: tuple[KeyLike, ...]) -> UUID:
        """The identifier an edge label addresses."""
        if self.MULTIGRAPH:
            return as_key(label[2])
        between = self.stored_between([label[0]]).get(self.canonical_pair(label[0], label[1]), [])
        if not between:
            raise KeyError(label)
        return between[0][2]

    def get_edge_attributes(self, name: str) -> dict[tuple[UUID, ...], Any]:
        labelled: dict[tuple[UUID, ...], Any] = {}
        for page in batched(self.edge_triples(None), PAGE_KEYS):
            attributes = self.read_attributes(self.edges_collection, [edge for _, _, edge in page])
            for (source, target, edge), found in zip(page, attributes, strict=True):
                if name in found:
                    labelled[self.edge_label(source, target, edge)] = found[name]
        return labelled

    def set_edge_attributes(self, values: Any, name: str | None = None) -> None:
        """Merges `values` into edges: one value for all of them, a value per edge, or a document per edge."""
        if name is None:
            labels = list(values)
            documents = [dict(values[label]) for label in labels]
        elif isinstance(values, Mapping):
            labels = list(values)
            documents = [{name: values[label]} for label in labels]
        else:
            identifiers = [edge for _, _, edge in self.edge_triples(None)]
            self.merge_attributes(self.edges_collection, identifiers, [{name: values}])
            return
        identifiers = [self.edge_identifier(label) for label in labels]
        self.merge_attributes(self.edges_collection, identifiers, documents)

    # endregion Attribute Maps


class DiGraph(Graph):
    """A directed graph holding at most one edge from one vertex to another, as `networkx.DiGraph` does."""

    DIRECTED = True

    def successors(self, node: KeyLike) -> Iterator[UUID]:
        return self.neighbors(node)

    def predecessors(self, node: KeyLike) -> Iterator[UUID]:
        found = self.blocking.graph_read_edges([node], Arena(), role=Role.TARGET, collection=self.graph_collection)
        return iter(sorted({source for source, _, _ in triples_of(found, 0)}))

    @property
    def in_degree(self) -> DegreeView:
        return DegreeView(self, Role.TARGET)

    @property
    def out_degree(self) -> DegreeView:
        return DegreeView(self, Role.SOURCE)


class MultiGraph(Graph):
    """An undirected graph holding any number of keyed edges between two vertices, as `networkx.MultiGraph` does."""

    MULTIGRAPH = True

    def add_edge(self, source: KeyLike, target: KeyLike, key: KeyLike | None = None, **attributes: Any) -> UUID:
        """Inserts one edge, or merges `attributes` into the one `key` names, and answers its key."""
        return self.add_edges_from_arrays([source], [target], [key], **attributes)[0]

    def remove_edge(self, source: KeyLike, target: KeyLike, key: KeyLike | None = None) -> None:
        """Removes the edge `key` names, or the newest edge between the pair."""
        if self.remove_edges_from_arrays([source], [target], [key]) == 0:
            raise KeyError((source, target, key))

    def get_edge_data(self, source: KeyLike, target: KeyLike, key: KeyLike | None = None, default: Any = None) -> Any:
        between = self.stored_between([source]).get(self.canonical_pair(source, target), [])
        identifiers = [edge for _, _, edge in between if key is None or edge == as_key(key)]
        if not identifiers:
            return default
        attributes = self.read_attributes(self.edges_collection, identifiers)
        return attributes[0] if key is not None else dict(zip(identifiers, attributes, strict=True))


class MultiDiGraph(MultiGraph, DiGraph):
    """A directed graph holding any number of keyed edges between two vertices, as `networkx.MultiDiGraph` does."""

    DIRECTED = True
    MULTIGRAPH = True
