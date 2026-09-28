"""Graphs over the C ABI: edge and vertex upserts and removals, degrees and edge lookups in every role."""

import gc
from uuid import UUID

import numpy as np
import pytest
from capabilities import NEEDS_COLLECTIONS, NEEDS_SNAPSHOTS, NEEDS_TRANSACTIONS, key, keys

import ustore

BATCH_SIZES = [1, 16, 65, 1000]

ROLES = ["source", "target", "any"]

EDGE_IDENTIFIERS = {
    "default": lambda count: None,
    "list": lambda count: list(range(100, 100 + count)),
    "numpy": lambda count: np.arange(100, 100 + count, dtype=np.int64),
}

# 1 → 2, 1 → 3, 2 → 3, 3 → 1
TRIANGLE = ([1, 1, 2, 3], [2, 3, 3, 1])


def expected_edges(sources: list[int], targets: list[int], identifiers: list[UUID], vertex: int, role: str):
    """The triples a lookup of `vertex` answers: outgoing by target, then incoming by source."""
    triples = list(zip(keys(sources), keys(targets), identifiers, strict=True))
    outgoing = sorted((edge for edge in triples if edge[0] == key(vertex)), key=lambda edge: (edge[1], edge[2]))
    incoming = sorted((edge for edge in triples if edge[1] == key(vertex)), key=lambda edge: (edge[0], edge[2]))
    return {"source": outgoing, "target": incoming, "any": outgoing + incoming}[role]


@pytest.fixture
def graph(database: ustore.Database) -> UUID:
    """A graph collection for the graph calls, as the main collection holds values."""
    return database.collection_create("graph", modality="graph")


# region Edges


@pytest.mark.parametrize("role", ROLES)
@pytest.mark.parametrize("identifiers", EDGE_IDENTIFIERS)
def test_find_edges_answers_triples_per_vertex(view, graph, role: str, identifiers: str):
    sources, targets = TRIANGLE
    given = EDGE_IDENTIFIERS[identifiers](len(sources))
    view.graph_upsert_edges(sources, targets, edges=given, collection=graph)
    stored = [ustore.default_edge_id] * len(sources) if given is None else keys(int(edge) for edge in given)

    found = view.graph_find_edges([1, 2, 3], role=role, collection=graph)
    assert isinstance(found, ustore.Edges)
    assert len(found) == 3
    for index, vertex in enumerate([1, 2, 3]):
        expected = expected_edges(sources, targets, stored, vertex, role)
        assert list(found[index]) == expected
        assert found.degrees[index] == len(expected)
    assert np.asarray(found).shape == (sum(found.degrees), 3, 16)


@pytest.mark.parametrize("count", BATCH_SIZES)
def test_a_chain_of_edges(view, graph, count: int):
    sources = np.arange(count, dtype=np.int64)
    targets = sources + 1
    view.graph_upsert_edges(sources, targets, collection=graph)
    degrees = view.graph_degrees(np.arange(count + 1, dtype=np.int64), collection=graph)
    assert degrees == (1, *([2] * (count - 1)), 1)
    assert view.graph_degrees([0, count], role="source", collection=graph) == (1, 0)
    assert view.graph_degrees([0, count], role="target", collection=graph) == (0, 1)
    assert view.graph_degrees([count + 1], collection=graph) == (None,)


@pytest.mark.parametrize("repeats", [1, 2, 3])
def test_upserting_an_edge_again_changes_nothing(view, graph, repeats: int):
    for _ in range(repeats):
        view.graph_upsert_edges([1], [2], collection=graph)
    assert view.graph_degrees([1, 2], collection=graph) == (1, 1)


def test_edge_identifiers_tell_parallel_edges_apart(view, graph):
    view.graph_upsert_edges([1, 1, 1], [2, 2, 2], edges=[7, 8, 7], collection=graph)
    assert view.graph_find_edges([1], role="source", collection=graph)[0] == (
        (key(1), key(2), key(7)),
        (key(1), key(2), key(8)),
    )


@pytest.mark.parametrize("identifiers", ["default", "list"])
def test_remove_edges_keeps_the_vertices(view, graph, identifiers: str):
    sources, targets = TRIANGLE
    given = EDGE_IDENTIFIERS[identifiers](len(sources))
    view.graph_upsert_edges(sources, targets, edges=given, collection=graph)
    view.graph_remove_edges(sources[:2], targets[:2], edges=None if given is None else given[:2], collection=graph)
    assert view.graph_degrees([1, 2, 3], role="source", collection=graph) == (0, 1, 1)
    assert all(view.contains(vertex, collection=graph) for vertex in (1, 2, 3))


def test_removing_by_pair_spares_edges_with_identifiers(view, graph):
    view.graph_upsert_edges([1, 1], [2, 2], edges=[ustore.default_edge_id, 5], collection=graph)
    view.graph_remove_edges([1], [2], collection=graph)
    assert view.graph_find_edges([1], role="source", collection=graph)[0] == ((key(1), key(2), key(5)),)


def test_self_loops_count_in_both_roles(view, graph):
    view.graph_upsert_edges([4], [4], collection=graph)
    assert view.graph_degrees([4], role="source", collection=graph) == (1,)
    assert view.graph_degrees([4], role="any", collection=graph) == (2,)


def test_edge_views_outlive_their_tape(view, graph):
    view.graph_upsert_edges([1], [2], edges=[3], collection=graph)
    held = memoryview(view.graph_find_edges([1], collection=graph))
    gc.collect()
    assert (held.format, held.shape) == ("B", (1, 3, 16))
    assert held.tobytes() == key(1).bytes + key(2).bytes + key(3).bytes


def test_empty_lookups(view, graph):
    found = view.graph_find_edges([], collection=graph)
    assert len(found) == 0
    assert np.asarray(found).shape == (0, 3, 16)
    assert view.graph_degrees([], collection=graph) == ()


# endregion Edges

# region Vertices


@pytest.mark.parametrize("count", BATCH_SIZES)
def test_upsert_vertices_without_edges(view, graph, count: int):
    vertices = list(range(count))
    view.graph_upsert_vertices(vertices, collection=graph)
    assert all(view.contains(vertex, collection=graph) for vertex in vertices)
    assert view.graph_degrees(vertices, collection=graph) == (0,) * count


def test_upserting_a_vertex_keeps_its_edges(view, graph):
    view.graph_upsert_edges([1], [2], collection=graph)
    view.graph_upsert_vertices([1, 2], collection=graph)
    assert view.graph_degrees([1, 2], collection=graph) == (1, 1)


@pytest.mark.parametrize("removed", [[1], [2], [1, 3], [1, 2, 3]])
def test_remove_vertices_removes_their_edges(view, graph, removed: list[int]):
    sources, targets = TRIANGLE
    view.graph_upsert_edges(sources, targets, collection=graph)
    view.graph_remove_vertices(removed, collection=graph)
    remaining = [
        (source, target) for source, target in zip(sources, targets, strict=True) if not {source, target} & set(removed)
    ]
    for vertex in (1, 2, 3):
        assert view.contains(vertex, collection=graph) == (vertex not in removed)
        if vertex not in removed:
            expected = sorted(target for source, target in remaining if source == vertex)
            assert [edge[1] for edge in view.graph_find_edges([vertex], role="source", collection=graph)[0]] == keys(
                expected
            )


@pytest.mark.parametrize("stored", [b"abc", b'{"name": "Ada Lovelace", "born": 1815}'], ids=["blob", "document"])
def test_a_collection_of_values_refuses_the_graph_calls(view, stored: bytes):
    """An engine storing vertices as values finds a value that is not one, and one keeping graphs apart refuses."""
    view.set(1, stored)
    with pytest.raises((ustore.MalformedError, ustore.ArgumentError)):
        view.graph_find_edges([1])
    with pytest.raises((ustore.MalformedError, ustore.ArgumentError)):
        view.graph_upsert_edges([1], [2])
    with pytest.raises((ustore.MalformedError, ustore.ArgumentError)):
        view.graph_remove_vertices([1])


# endregion Vertices

# region Collections


@NEEDS_COLLECTIONS
def test_graphs_in_named_collections_stay_apart(database):
    database.collection_create("first", modality="graph")
    database.collection_create("second", modality="graph")
    database.graph_upsert_edges([1], [2], collection="first")
    database.graph_upsert_edges([1], [3], collection="second")
    assert database.graph_find_edges([1], collection="first")[0] == ((key(1), key(2), ustore.default_edge_id),)
    assert database.graph_find_edges([1], collection="second")[0] == ((key(1), key(3), ustore.default_edge_id),)
    assert not database.contains(1)


# endregion Collections

# region Transactions Across Graphs


@NEEDS_TRANSACTIONS
@NEEDS_SNAPSHOTS
def test_one_commit_updates_several_graphs(database):
    """A follow turns into a block and a purchase lands in a third graph, all visible at once or not at all."""
    for name in ("follows", "blocks", "purchases"):
        database.collection_create(name, modality="graph")
    database.graph_upsert_edges([1], [2], collection="follows")
    with database.snapshot() as before:
        with database.transaction() as staged:
            staged.graph_remove_edges([1], [2], collection="follows")
            staged.graph_upsert_edges([1], [2], collection="blocks")
            staged.graph_upsert_edges([1], [7], edges=[70], collection="purchases")
            assert database.graph_degrees([1], role="source", collection="follows") == (1,)
            assert database.graph_degrees([1], collection="blocks") == (None,)

        assert database.graph_degrees([1], role="source", collection="follows") == (0,)
        assert database.graph_find_edges([1], collection="blocks")[0] == ((key(1), key(2), ustore.default_edge_id),)
        assert database.graph_find_edges([1], collection="purchases")[0] == ((key(1), key(7), key(70)),)
        assert database.graph_degrees([1], role="source", collection="follows", snapshot=before) == (1,)
        assert database.graph_degrees([1], collection="blocks", snapshot=before) == (None,)
        assert database.graph_degrees([1], collection="purchases", snapshot=before) == (None,)


@NEEDS_TRANSACTIONS
def test_a_conflict_in_one_graph_discards_the_other(database):
    """A transaction writing two graphs loses both when another commit adds the same vertex to one of them first."""
    for name in ("follows", "purchases"):
        database.collection_create(name, modality="graph")
    first = database.transaction()
    second = database.transaction()
    try:
        first.graph_upsert_edges([1], [2], collection="follows")
        first.graph_upsert_edges([1], [7], collection="purchases")
        second.graph_upsert_edges([3], [7], collection="purchases")
        second.commit()
        with pytest.raises(ustore.ConflictError):
            first.commit()
    finally:
        first.close()
        second.close()
    assert database.graph_degrees([1, 2], collection="follows") == (None, None)
    assert database.graph_degrees([1, 3, 7], collection="purchases") == (None, 1, 1)


@NEEDS_TRANSACTIONS
def test_a_write_to_one_graph_depends_on_a_read_of_another(database):
    """A recommendation drawn from a follow that another commit removed conflicts, though it lands in another graph."""
    for name in ("follows", "recommendations"):
        database.collection_create(name, modality="graph")
    database.graph_upsert_edges([1], [2], collection="follows")
    staged = database.transaction()
    try:
        assert staged.graph_degrees([1], role="source", collection="follows") == (1,)
        staged.graph_upsert_edges([2], [1], collection="recommendations")
        database.graph_remove_edges([1], [2], collection="follows")
        with pytest.raises(ustore.ConflictError):
            staged.commit()
    finally:
        staged.close()
    assert database.graph_degrees([2], collection="recommendations") == (None,)


# endregion Transactions Across Graphs

# region Refusals


@pytest.mark.parametrize(
    ("call", "failure"),
    [
        (lambda view: view.graph_upsert_edges([1, 2], [3]), ValueError),
        (lambda view: view.graph_upsert_edges([1], [2], edges=[1, 2]), ValueError),
        (lambda view: view.graph_upsert_edges([1], [2], [3]), TypeError),
        (lambda view: view.graph_find_edges([1], role="outgoing"), ValueError),
        (lambda view: view.graph_degrees([1], role=1), TypeError),
        (lambda view: view.graph_remove_vertices([1.5]), TypeError),
        (lambda view: view.graph_upsert_vertices([1], roles="any"), TypeError),
    ],
    ids=["widths", "identifier-width", "positional-identifiers", "role-name", "role-type", "vertex-type", "keyword"],
)
def test_binding_refusals(view, call, failure: type):
    with pytest.raises(failure):
        call(view)


# endregion Refusals
