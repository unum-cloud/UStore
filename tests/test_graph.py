"""Graphs over the C ABI: edge and vertex upserts and removals, degrees and edge lookups in every role."""

import gc

import numpy
import pytest
import ustore
from ustore_capabilities import NEEDS_COLLECTIONS

BATCH_SIZES = [1, 16, 65, 1000]

ROLES = ["source", "target", "any"]

EDGE_IDENTIFIERS = {
    "default": lambda count: None,
    "list": lambda count: list(range(100, 100 + count)),
    "numpy": lambda count: numpy.arange(100, 100 + count, dtype=numpy.int64),
}

# 1 → 2, 1 → 3, 2 → 3, 3 → 1
TRIANGLE = ([1, 1, 2, 3], [2, 3, 3, 1])


def expected_edges(sources: list[int], targets: list[int], identifiers: list[int], vertex: int, role: str):
    """The triples a lookup of `vertex` answers: outgoing by target, then incoming by source."""
    triples = list(zip(sources, targets, identifiers, strict=True))
    outgoing = sorted((edge for edge in triples if edge[0] == vertex), key=lambda edge: (edge[1], edge[2]))
    incoming = sorted((edge for edge in triples if edge[1] == vertex), key=lambda edge: (edge[0], edge[2]))
    return {"source": outgoing, "target": incoming, "any": outgoing + incoming}[role]


# region Edges


@pytest.mark.parametrize("role", ROLES)
@pytest.mark.parametrize("identifiers", EDGE_IDENTIFIERS)
def test_find_edges_answers_triples_per_vertex(view, role: str, identifiers: str):
    sources, targets = TRIANGLE
    given = EDGE_IDENTIFIERS[identifiers](len(sources))
    view.graph_upsert_edges(sources, targets, edges=given)
    stored = [ustore.default_edge_id] * len(sources) if given is None else [int(edge) for edge in given]

    found = view.graph_find_edges([1, 2, 3], role=role)
    assert isinstance(found, ustore.Edges)
    assert len(found) == 3
    for index, vertex in enumerate([1, 2, 3]):
        expected = expected_edges(sources, targets, stored, vertex, role)
        assert found[index].shape == (len(expected), 3)
        assert [tuple(edge) for edge in found[index].tolist()] == expected
        assert found.degrees[index] == len(expected)
    assert numpy.asarray(found).shape == (sum(found.degrees), 3)


@pytest.mark.parametrize("count", BATCH_SIZES)
def test_a_chain_of_edges(view, count: int):
    sources = numpy.arange(count, dtype=numpy.int64)
    targets = sources + 1
    view.graph_upsert_edges(sources, targets)
    degrees = view.graph_degrees(numpy.arange(count + 1, dtype=numpy.int64))
    assert degrees == (1, *([2] * (count - 1)), 1)
    assert view.graph_degrees([0, count], role="source") == (1, 0)
    assert view.graph_degrees([0, count], role="target") == (0, 1)
    assert view.graph_degrees([count + 1]) == (None,)


@pytest.mark.parametrize("repeats", [1, 2, 3])
def test_upserting_an_edge_again_changes_nothing(view, repeats: int):
    for _ in range(repeats):
        view.graph_upsert_edges([1], [2])
    assert view.graph_degrees([1, 2]) == (1, 1)


def test_edge_identifiers_tell_parallel_edges_apart(view):
    view.graph_upsert_edges([1, 1, 1], [2, 2, 2], edges=[7, 8, 7])
    assert [tuple(edge) for edge in view.graph_find_edges([1], role="source")[0].tolist()] == [(1, 2, 7), (1, 2, 8)]


@pytest.mark.parametrize("identifiers", ["default", "list"])
def test_remove_edges_keeps_the_vertices(view, identifiers: str):
    sources, targets = TRIANGLE
    given = EDGE_IDENTIFIERS[identifiers](len(sources))
    view.graph_upsert_edges(sources, targets, edges=given)
    view.graph_remove_edges(sources[:2], targets[:2], edges=None if given is None else given[:2])
    assert view.graph_degrees([1, 2, 3], role="source") == (0, 1, 1)
    assert all(view.contains(vertex) for vertex in (1, 2, 3))


def test_removing_by_pair_spares_edges_with_identifiers(view):
    view.graph_upsert_edges([1, 1], [2, 2], edges=[ustore.default_edge_id, 5])
    view.graph_remove_edges([1], [2])
    assert [tuple(edge) for edge in view.graph_find_edges([1], role="source")[0].tolist()] == [(1, 2, 5)]


def test_self_loops_count_in_both_roles(view):
    view.graph_upsert_edges([4], [4])
    assert view.graph_degrees([4], role="source") == (1,)
    assert view.graph_degrees([4], role="any") == (2,)


def test_edge_views_outlive_their_tape(view):
    view.graph_upsert_edges([1], [2], edges=[3])
    held = view.graph_find_edges([1])[0]
    gc.collect()
    assert held.format == "q"
    assert held.tolist() == [[1, 2, 3]]


def test_empty_lookups(view):
    found = view.graph_find_edges([])
    assert len(found) == 0
    assert numpy.asarray(found).shape == (0, 3)
    assert view.graph_degrees([]) == ()


# endregion Edges

# region Vertices


@pytest.mark.parametrize("count", BATCH_SIZES)
def test_upsert_vertices_without_edges(view, count: int):
    vertices = list(range(count))
    view.graph_upsert_vertices(vertices)
    assert all(view.contains(vertex) for vertex in vertices)
    assert view.graph_degrees(vertices) == (0,) * count


def test_upserting_a_vertex_keeps_its_edges(view):
    view.graph_upsert_edges([1], [2])
    view.graph_upsert_vertices([1, 2])
    assert view.graph_degrees([1, 2]) == (1, 1)


@pytest.mark.parametrize("removed", [[1], [2], [1, 3], [1, 2, 3]])
def test_remove_vertices_removes_their_edges(view, removed: list[int]):
    sources, targets = TRIANGLE
    view.graph_upsert_edges(sources, targets)
    view.graph_remove_vertices(removed)
    remaining = [
        (source, target) for source, target in zip(sources, targets, strict=True) if not {source, target} & set(removed)
    ]
    for vertex in (1, 2, 3):
        assert view.contains(vertex) == (vertex not in removed)
        if vertex not in removed:
            expected = sorted(target for source, target in remaining if source == vertex)
            assert [edge[1] for edge in view.graph_find_edges([vertex], role="source")[0].tolist()] == expected


@pytest.mark.parametrize("stored", [b"abc", b'{"name": "Ada Lovelace", "born": 1815}'], ids=["blob", "document"])
def test_a_value_that_is_not_a_vertex_is_refused(view, stored: bytes):
    view.set(1, stored)
    with pytest.raises(ustore.MalformedError):
        view.graph_find_edges([1])
    with pytest.raises(ustore.MalformedError):
        view.graph_upsert_edges([1], [2])
    with pytest.raises(ustore.MalformedError):
        view.graph_remove_vertices([1])


# endregion Vertices

# region Collections


@NEEDS_COLLECTIONS
def test_graphs_in_named_collections_stay_apart(database):
    database.collection_create("first")
    database.collection_create("second")
    database.graph_upsert_edges([1], [2], collection="first")
    database.graph_upsert_edges([1], [3], collection="second")
    assert database.graph_find_edges([1], collection="first")[0].tolist() == [[1, 2, ustore.default_edge_id]]
    assert database.graph_find_edges([1], collection="second")[0].tolist() == [[1, 3, ustore.default_edge_id]]
    assert not database.contains(1)


# endregion Collections

# region Refusals


@pytest.mark.parametrize(
    ("call", "failure"),
    [
        (lambda view: view.graph_upsert_edges([1, 2], [3]), ValueError),
        (lambda view: view.graph_upsert_edges([1], [2], edges=[1, 2]), ValueError),
        (lambda view: view.graph_upsert_edges([1], [2], [3]), TypeError),
        (lambda view: view.graph_find_edges([1], role="outgoing"), ValueError),
        (lambda view: view.graph_degrees([1], role=1), TypeError),
        (lambda view: view.graph_remove_vertices(["one"]), TypeError),
        (lambda view: view.graph_upsert_vertices([1], roles="any"), TypeError),
    ],
    ids=["widths", "identifier-width", "positional-identifiers", "role-name", "role-type", "vertex-type", "keyword"],
)
def test_binding_refusals(view, call, failure: type):
    with pytest.raises(failure):
        call(view)


# endregion Refusals
