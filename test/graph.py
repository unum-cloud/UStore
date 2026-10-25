"""Graphs over the C ABI: edge and vertex writes and removals, degrees and edge reads in every role."""

import asyncio
import gc
from uuid import UUID

import numpy as np
import pytest
from capabilities import BATCH_SIZES, NEEDS_SNAPSHOTS, NEEDS_TRANSACTIONS, key, keys

import ustore
from ustore import Modality, Role

ROLES = [Role.SOURCE, Role.TARGET, Role.ANY]

EDGE_IDENTIFIERS = {
    "default": lambda count: None,
    "list": lambda count: list(range(100, 100 + count)),
    "numpy": lambda count: np.arange(100, 100 + count, dtype=np.int64),
}

# 1 → 2, 1 → 3, 2 → 3, 3 → 1
TRIANGLE = ([1, 1, 2, 3], [2, 3, 3, 1])


def expected_edges(sources: list[int], targets: list[int], identifiers: list[UUID], vertex: int, role: Role):
    """The triples a read of `vertex` answers: outgoing by target, then incoming by source."""
    triples = list(zip(keys(sources), keys(targets), identifiers, strict=True))
    outgoing = sorted((edge for edge in triples if edge[0] == key(vertex)), key=lambda edge: (edge[1], edge[2]))
    incoming = sorted((edge for edge in triples if edge[1] == key(vertex)), key=lambda edge: (edge[0], edge[2]))
    return {Role.SOURCE: outgoing, Role.TARGET: incoming, Role.ANY: outgoing + incoming}[role]


# region Edges


@pytest.mark.parametrize("role", ROLES)
@pytest.mark.parametrize("identifiers", EDGE_IDENTIFIERS)
def test_read_edges_answers_triples_per_vertex(view, arena, graph, role: Role, identifiers: str):
    sources, targets = TRIANGLE
    given = EDGE_IDENTIFIERS[identifiers](len(sources))
    view.graph_write_edges(sources, targets, edges=given, collection=graph)
    stored = [ustore.edge_id_default] * len(sources) if given is None else keys(int(edge) for edge in given)

    found = view.graph_read_edges([1, 2, 3], arena, role=role, collection=graph)
    assert isinstance(found, ustore.Neighborhoods)
    assert len(found) == 3
    for index, vertex in enumerate([1, 2, 3]):
        expected = expected_edges(sources, targets, stored, vertex, role)
        assert list(found[index]) == expected
        assert found.degrees[index] == len(expected)
    assert np.asarray(found).shape == (sum(found.degrees), 3, 16)


@pytest.mark.parametrize("count", [count for count in BATCH_SIZES if count])
def test_a_chain_of_edges(view, arena, graph, count: int):
    sources = np.arange(count, dtype=np.int64)
    targets = sources + 1
    view.graph_write_edges(sources, targets, collection=graph)
    degrees = view.graph_degrees(np.arange(count + 1, dtype=np.int64), arena, collection=graph)
    assert degrees == (1, *([2] * (count - 1)), 1)
    assert view.graph_degrees([0, count], arena, role=Role.SOURCE, collection=graph) == (1, 0)
    assert view.graph_degrees([0, count], arena, role=Role.TARGET, collection=graph) == (0, 1)
    assert view.graph_degrees([count + 1], arena, collection=graph) == (None,)


@pytest.mark.parametrize("repeats", [1, 2, 3])
def test_writing_an_edge_again_changes_nothing(view, arena, graph, repeats: int):
    for _ in range(repeats):
        view.graph_write_edges([1], [2], collection=graph)
    assert view.graph_degrees([1, 2], arena, collection=graph) == (1, 1)


def test_edge_identifiers_tell_parallel_edges_apart(view, arena, graph):
    view.graph_write_edges([1, 1, 1], [2, 2, 2], edges=[7, 8, 7], collection=graph)
    assert view.graph_read_edges([1], arena, role=Role.SOURCE, collection=graph)[0] == (
        (key(1), key(2), key(7)),
        (key(1), key(2), key(8)),
    )


@pytest.mark.parametrize("identifiers", ["default", "list"])
def test_remove_edges_keeps_the_vertices(view, arena, graph, identifiers: str):
    sources, targets = TRIANGLE
    given = EDGE_IDENTIFIERS[identifiers](len(sources))
    view.graph_write_edges(sources, targets, edges=given, collection=graph)
    view.graph_remove_edges(sources[:2], targets[:2], edges=None if given is None else given[:2], collection=graph)
    assert view.graph_degrees([1, 2, 3], arena, role=Role.SOURCE, collection=graph) == (0, 1, 1)
    assert None not in view.graph_degrees([1, 2, 3], arena, collection=graph)


def test_removing_by_pair_spares_edges_with_identifiers(view, arena, graph):
    view.graph_write_edges([1, 1], [2, 2], edges=[ustore.edge_id_default, 5], collection=graph)
    view.graph_remove_edges([1], [2], collection=graph)
    assert view.graph_read_edges([1], arena, role=Role.SOURCE, collection=graph)[0] == ((key(1), key(2), key(5)),)


def test_self_loops_count_in_both_roles(view, arena, graph):
    view.graph_write_edges([4], [4], collection=graph)
    assert view.graph_degrees([4], arena, role=Role.SOURCE, collection=graph) == (1,)
    assert view.graph_degrees([4], arena, role=Role.ANY, collection=graph) == (2,)


def test_a_missing_vertex_spans_no_edges(view, arena, graph):
    view.graph_write_edges([1, 3, 3], [2, 4, 5], edges=[10, 11, 12], collection=graph)
    found = view.graph_read_edges([1, 404, 3], arena, role=Role.SOURCE, collection=graph)
    assert found.degrees == (1, None, 2)
    assert found[1] is None
    assert found[2] == ((key(3), key(4), key(11)), (key(3), key(5), key(12)))
    assert [bytes(edge[0]) for edge in np.asarray(found)] == [key(1).bytes, key(3).bytes, key(3).bytes]


def test_edge_views_outlive_their_tape(view, arena, graph):
    view.graph_write_edges([1], [2], edges=[3], collection=graph)
    held = memoryview(view.graph_read_edges([1], arena, collection=graph))
    gc.collect()
    assert (held.format, held.shape) == ("B", (1, 3, 16))
    assert held.tobytes() == key(1).bytes + key(2).bytes + key(3).bytes


def test_empty_reads(view, arena, graph):
    found = view.graph_read_edges([], arena, collection=graph)
    assert len(found) == 0
    assert np.asarray(found).shape == (0, 3, 16)
    assert view.graph_degrees([], arena, collection=graph) == ()


# endregion Edges

# region Vertices


@pytest.mark.parametrize("count", BATCH_SIZES)
def test_write_vertices_without_edges(view, arena, graph, count: int):
    vertices = list(range(count))
    view.graph_write_vertices(vertices, collection=graph)
    assert None not in view.graph_degrees(vertices, arena, collection=graph)
    assert view.graph_degrees(vertices, arena, collection=graph) == (0,) * count


def test_writing_a_vertex_keeps_its_edges(view, arena, graph):
    view.graph_write_edges([1], [2], collection=graph)
    view.graph_write_vertices([1, 2], collection=graph)
    assert view.graph_degrees([1, 2], arena, collection=graph) == (1, 1)


@pytest.mark.parametrize("removed", [[1], [2], [1, 3], [1, 2, 3]])
def test_remove_vertices_removes_their_edges(view, arena, graph, removed: list[int]):
    sources, targets = TRIANGLE
    view.graph_write_edges(sources, targets, collection=graph)
    view.graph_remove_vertices(removed, collection=graph)
    remaining = [
        (source, target) for source, target in zip(sources, targets, strict=True) if not {source, target} & set(removed)
    ]
    for vertex in (1, 2, 3):
        assert (view.graph_degrees([vertex], arena, collection=graph)[0] is not None) == (vertex not in removed)
        if vertex not in removed:
            expected = sorted(target for source, target in remaining if source == vertex)
            found = view.graph_read_edges([vertex], arena, role=Role.SOURCE, collection=graph)[0]
            assert [edge[1] for edge in found] == keys(expected)


@pytest.mark.parametrize("stored", [b"abc", b'{"name": "Ada Lovelace", "born": 1815}'], ids=["blob", "document"])
def test_a_collection_of_values_refuses_the_graph_calls(view, blobs, arena, stored: bytes):
    """An engine storing vertices as values finds a value that is not one, and one keeping graphs apart refuses."""
    view.set(1, stored, collection=blobs)
    with pytest.raises((ustore.MalformedError, ustore.InvalidArgumentError)):
        view.graph_read_edges([1], arena, collection=blobs)
    with pytest.raises((ustore.MalformedError, ustore.InvalidArgumentError)):
        view.graph_write_edges([1], [2], collection=blobs)
    with pytest.raises((ustore.MalformedError, ustore.InvalidArgumentError)):
        view.graph_remove_vertices([1], collection=blobs)


# endregion Vertices

# region Collections


def test_graphs_in_named_collections_stay_apart(database, arena):
    database.collection_create("first", modality=Modality.GRAPH)
    database.collection_create("second", modality=Modality.GRAPH)
    database.graph_write_edges([1], [2], collection="first")
    database.graph_write_edges([1], [3], collection="second")
    first = ((key(1), key(2), ustore.edge_id_default),)
    second = ((key(1), key(3), ustore.edge_id_default),)
    assert database.graph_read_edges([1], arena, collection="first")[0] == first
    assert database.graph_read_edges([1], arena, collection="second")[0] == second
    assert database.graph_degrees([3], arena, collection="first") == (None,)


# endregion Collections

# region Transactions Across Graphs


@NEEDS_TRANSACTIONS
@NEEDS_SNAPSHOTS
def test_one_commit_updates_several_graphs(database, arena):
    """A follow turns into a block and a purchase lands in a third graph, all visible at once or not at all."""
    for name in ("follows", "blocks", "purchases"):
        database.collection_create(name, modality=Modality.GRAPH)
    database.graph_write_edges([1], [2], collection="follows")
    with database.snapshot() as before:
        with database.transaction() as staged:
            staged.graph_remove_edges([1], [2], collection="follows")
            staged.graph_write_edges([1], [2], collection="blocks")
            staged.graph_write_edges([1], [7], edges=[70], collection="purchases")
            assert database.graph_degrees([1], arena, role=Role.SOURCE, collection="follows") == (1,)
            assert database.graph_degrees([1], arena, collection="blocks") == (None,)

        blocked = ((key(1), key(2), ustore.edge_id_default),)
        assert database.graph_degrees([1], arena, role=Role.SOURCE, collection="follows") == (0,)
        assert database.graph_read_edges([1], arena, collection="blocks")[0] == blocked
        assert database.graph_read_edges([1], arena, collection="purchases")[0] == ((key(1), key(7), key(70)),)
        assert before.graph_degrees([1], arena, role=Role.SOURCE, collection="follows") == (1,)
        assert before.graph_degrees([1], arena, collection="blocks") == (None,)
        assert before.graph_degrees([1], arena, collection="purchases") == (None,)


@NEEDS_TRANSACTIONS
def test_a_conflict_in_one_graph_discards_the_other(database, arena):
    """A transaction writing two graphs loses both when another commit adds the same vertex to one of them first."""
    for name in ("follows", "purchases"):
        database.collection_create(name, modality=Modality.GRAPH)
    first = database.transaction()
    second = database.transaction()
    try:
        first.graph_write_edges([1], [2], collection="follows")
        first.graph_write_edges([1], [7], collection="purchases")
        second.graph_write_edges([3], [7], collection="purchases")
        second.commit()
        with pytest.raises(ustore.ConflictError):
            first.commit()
    finally:
        first.close()
        second.close()
    assert database.graph_degrees([1, 2], arena, collection="follows") == (None, None)
    assert database.graph_degrees([1, 3, 7], arena, collection="purchases") == (None, 1, 1)


@NEEDS_TRANSACTIONS
def test_a_write_to_one_graph_depends_on_a_read_of_another(database, arena):
    """A recommendation drawn from a follow that another commit removed conflicts, though it lands in another graph."""
    for name in ("follows", "recommendations"):
        database.collection_create(name, modality=Modality.GRAPH)
    database.graph_write_edges([1], [2], collection="follows")
    staged = database.transaction()
    try:
        assert staged.graph_degrees([1], arena, role=Role.SOURCE, collection="follows") == (1,)
        staged.graph_write_edges([2], [1], collection="recommendations")
        database.graph_remove_edges([1], [2], collection="follows")
        with pytest.raises(ustore.ConflictError):
            staged.commit()
    finally:
        staged.close()
    assert database.graph_degrees([2], arena, collection="recommendations") == (None,)


# endregion Transactions Across Graphs

# region Deferred Calls


def test_a_deferred_graph_write_edges_lands_once_waited(view, arena, graph, later):
    assert later.graph_write_edges(*TRIANGLE, collection=graph).wait() is None
    assert view.graph_degrees([1, 2, 3], arena, collection=graph) == (3, 2, 3)


def test_a_deferred_graph_remove_edges_lands_once_waited(view, arena, graph, later):
    view.graph_write_edges(*TRIANGLE, collection=graph)
    assert later.graph_remove_edges([1], [2], collection=graph).wait() is None
    assert view.graph_degrees([1, 2], arena, role=Role.SOURCE, collection=graph) == (1, 1)


def test_a_deferred_graph_read_edges_answers_once_waited(view, arena, graph, later):
    view.graph_write_edges(*TRIANGLE, collection=graph)
    found = later.graph_read_edges([1, 2, 3, 4], arena, role=Role.SOURCE, collection=graph).wait()
    blocking = view.graph_read_edges([1, 2, 3, 4], ustore.Arena(), role=Role.SOURCE, collection=graph)
    assert [found[index] for index in range(4)] == [blocking[index] for index in range(4)]
    assert found.degrees == (2, 1, 1, None)


def test_a_deferred_graph_degrees_answers_once_waited(view, arena, graph, later):
    view.graph_write_edges(*TRIANGLE, collection=graph)
    assert later.graph_degrees([1, 2, 3, 4], arena, role=Role.TARGET, collection=graph).wait() == (1, 1, 2, None)


def test_a_deferred_graph_write_vertices_lands_once_waited(view, arena, graph, later):
    assert later.graph_write_vertices([5, 6], collection=graph).wait() is None
    assert view.graph_degrees([5, 6, 7], arena, collection=graph) == (0, 0, None)


def test_a_deferred_graph_remove_vertices_lands_once_waited(view, arena, graph, later):
    view.graph_write_edges(*TRIANGLE, collection=graph)
    assert later.graph_remove_vertices([3], collection=graph).wait() is None
    assert view.graph_degrees([1, 2, 3], arena, collection=graph) == (1, 1, None)


def test_await_graph_calls_under_asyncio(arena, graph, later):
    async def main() -> tuple[int | None, ...]:
        await later.graph_write_edges(*TRIANGLE, collection=graph)
        return await later.graph_degrees([1, 2, 3], arena, collection=graph)

    assert asyncio.run(main()) == (3, 2, 3)


# endregion Deferred Calls

# region Refusals


@pytest.mark.parametrize(
    ("call", "failure"),
    [
        (lambda view: view.graph_write_edges([1, 2], [3], collection="graph"), ValueError),
        (lambda view: view.graph_write_edges([1], [2], edges=[1, 2], collection="graph"), ValueError),
        (lambda view: view.graph_write_edges([1], [2], [3], collection="graph"), TypeError),
        (lambda view: view.graph_read_edges([1], ustore.Arena(), role=len(Role), collection="graph"), ValueError),
        (lambda view: view.graph_degrees([1], ustore.Arena(), role="any", collection="graph"), TypeError),
        (lambda view: view.graph_remove_vertices([1.5], collection="graph"), TypeError),
        (lambda view: view.graph_write_vertices([1], roles=Role.ANY, collection="graph"), TypeError),
    ],
    ids=["widths", "identifier-width", "positional-identifiers", "role-value", "role-type", "vertex-type", "keyword"],
)
def test_binding_refusals(view, graph, call, failure: type):
    with pytest.raises(failure):
        call(view)


# endregion Refusals
