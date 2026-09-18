"""The NetworkX-compatible graph view, checked against NetworkX itself as the oracle for every graph kind."""

import networkx
import numpy
import pytest
import ustore
from ustore.graph_view import DiGraph, Graph, MultiDiGraph, MultiGraph
from ustore_capabilities import NEEDS_COLLECTIONS, NEEDS_TRANSACTIONS

GRAPH_KINDS = {
    "graph": (Graph, networkx.Graph),
    "digraph": (DiGraph, networkx.DiGraph),
    "multigraph": (MultiGraph, networkx.MultiGraph),
    "multidigraph": (MultiDiGraph, networkx.MultiDiGraph),
}

SIZES = [1, 10, 100]


def canonical(graph, edges) -> list[tuple[int, ...]]:
    """Edges sorted with an undirected edge spelled from its smaller end, so both views compare equal."""
    spelled = [(min(edge[:2]), max(edge[:2]), *edge[2:]) if not graph.is_directed() else tuple(edge) for edge in edges]
    return sorted(spelled, key=repr)


@pytest.fixture(params=GRAPH_KINDS)
def kind(request: pytest.FixtureRequest) -> str:
    return request.param


@pytest.fixture
def graphs(view, kind: str):
    """An empty view in the main collection beside an empty NetworkX graph of the same kind."""
    ours, reference = GRAPH_KINDS[kind]
    return ours(view), reference()


@pytest.fixture
def attributed(view, kind: str):
    """An empty view keeping vertex and edge attributes in collections of their own."""
    ours, _ = GRAPH_KINDS[kind]
    return ours(view, "graph", nodes="nodes", edges="edges")


def assert_same_structure(ours, reference) -> None:
    assert list(ours.nodes) == sorted(reference.nodes)
    assert ours.number_of_nodes() == reference.number_of_nodes()
    assert ours.number_of_edges() == reference.number_of_edges()
    assert canonical(ours, ours.edges()) == canonical(ours, reference.edges())
    assert sorted(ours.degree) == sorted(reference.degree)


# region Structure


@pytest.mark.parametrize(("source", "target"), [(1, 2), (2, 1), (5, 5)])
def test_one_edge(graphs, source: int, target: int):
    ours, reference = graphs
    assert not ours.has_node(source) and not ours.has_edge(source, target)

    ours.add_edge(source, target)
    reference.add_edge(source, target)
    assert_same_structure(ours, reference)
    for pair in [(source, target), (target, source), (source, source)]:
        assert ours.has_edge(*pair) == reference.has_edge(*pair)
        assert ours.number_of_edges(*pair) == reference.number_of_edges(*pair)

    ours.remove_edge(source, target)
    reference.remove_edge(source, target)
    assert_same_structure(ours, reference)
    assert ours.has_node(source) and ours.has_node(target)


def test_triangle(graphs):
    ours, reference = graphs
    for source, target in [(1, 2), (2, 3), (3, 1)]:
        ours.add_edge(source, target)
        reference.add_edge(source, target)
    assert_same_structure(ours, reference)
    assert 1 in ours and 4 not in ours
    if ours.is_directed():
        assert list(ours.successors(1)) == sorted(reference.successors(1))
        assert list(ours.predecessors(1)) == sorted(reference.predecessors(1))


@pytest.mark.parametrize("container", ["list", "numpy"])
def test_triangle_in_batches(graphs, container: str):
    ours, reference = graphs
    spell = numpy.array if container == "numpy" else list
    ours.add_edges_from_arrays(spell([1, 2, 3]), spell([2, 3, 1]))
    reference.add_edges_from([(1, 2), (2, 3), (3, 1)])
    assert_same_structure(ours, reference)

    ours.remove_edges_from_arrays(spell([1, 2]), spell([2, 3]))
    reference.remove_edges_from([(1, 2), (2, 3)])
    assert_same_structure(ours, reference)


@pytest.mark.parametrize("size", SIZES)
@pytest.mark.parametrize("seed", [0, 1])
def test_random_fill_matches_networkx(graphs, size: int, seed: int):
    ours, reference = graphs
    generator = numpy.random.default_rng(seed)
    sources = generator.integers(0, 50, size)
    targets = generator.integers(0, 50, size)
    ours.add_edges_from_arrays(sources, targets)
    reference.add_edges_from(zip(sources.tolist(), targets.tolist(), strict=True))
    assert_same_structure(ours, reference)
    for source, target in zip(sources.tolist(), targets.tolist(), strict=True):
        assert ours.has_edge(target, source) == reference.has_edge(target, source)


@pytest.mark.parametrize("node", [1, 2, 3])
def test_neighbors(graphs, node: int):
    ours, reference = graphs
    for source, target in [(1, 2), (1, 3), (2, 3)]:
        ours.add_edge(source, target)
        reference.add_edge(source, target)
    assert list(ours.neighbors(node)) == sorted(reference.neighbors(node))
    assert sorted(ours[node]) == sorted(reference[node])


def test_adding_existing_vertices_keeps_their_edges(graphs):
    ours, reference = graphs
    ours.add_edge(1, 2)
    reference.add_edge(1, 2)
    ours.add_nodes_from([1, 2, 3])
    reference.add_nodes_from([1, 2, 3])
    assert_same_structure(ours, reference)


@pytest.mark.parametrize("count", SIZES)
def test_add_and_remove_nodes_in_batches(graphs, count: int):
    ours, reference = graphs
    nodes = numpy.arange(count)
    ours.add_nodes_from(nodes)
    reference.add_nodes_from(nodes.tolist())
    assert_same_structure(ours, reference)
    ours.remove_nodes_from(nodes[::2].tolist())
    reference.remove_nodes_from(nodes[::2].tolist())
    assert_same_structure(ours, reference)


@pytest.mark.parametrize("count", SIZES)
def test_removing_nodes_removes_their_edges(graphs, count: int):
    ours, reference = graphs
    ours.add_edges_from_arrays(numpy.arange(count), numpy.arange(count, 2 * count))
    reference.add_edges_from(zip(range(count), range(count, 2 * count), strict=True))
    for source in range(0, count, 3):
        ours.remove_node(source)
        reference.remove_node(source)
    assert_same_structure(ours, reference)
    with pytest.raises(KeyError):
        ours.remove_node(-5)


def test_clear_edges_then_clear(graphs):
    ours, reference = graphs
    ours.add_edges_from_arrays(numpy.arange(20), numpy.arange(20, 40))
    reference.add_edges_from(zip(range(20), range(20, 40), strict=True))
    ours.clear_edges()
    reference.clear_edges()
    assert_same_structure(ours, reference)
    ours.clear()
    reference.clear()
    assert_same_structure(ours, reference)


def test_duplicate_edges_in_one_batch(graphs):
    ours, reference = graphs
    ours.add_edges_from_arrays([1, 2, 2, 3], [4, 5, 5, 6])
    reference.add_edges_from([(1, 4), (2, 5), (2, 5), (3, 6)])
    assert_same_structure(ours, reference)
    ours.remove_edges_from_arrays([1, 2], [4, 5])
    reference.remove_edges_from([(1, 4), (2, 5)])
    assert_same_structure(ours, reference)


@pytest.mark.parametrize("count", SIZES)
def test_degrees(graphs, count: int):
    ours, reference = graphs
    ours.add_edges_from_arrays(numpy.arange(count), numpy.arange(1, count + 1))
    reference.add_edges_from(zip(range(count), range(1, count + 1), strict=True))
    for node in (0, count // 2, count):
        assert ours.degree[node] == reference.degree[node]
        assert ours.degree(node) == reference.degree(node)
    assert list(ours.degree([0, count])) == list(reference.degree([0, count]))
    if ours.is_directed():
        assert sorted(ours.in_degree) == sorted(reference.in_degree)
        assert sorted(ours.out_degree) == sorted(reference.out_degree)
    with pytest.raises(KeyError):
        ours.degree[count + 1]


# endregion Structure

# region Attributes


@NEEDS_COLLECTIONS
@pytest.mark.parametrize("weight", [1, 2.5])
def test_weighted_degrees_and_size(attributed, kind: str, weight: float):
    reference = GRAPH_KINDS[kind][1]()
    attributed.add_edges_from_arrays(numpy.arange(10), numpy.arange(1, 11), weight=weight)
    reference.add_edges_from(zip(range(10), range(1, 11), strict=True), weight=weight)
    assert sorted(attributed.degree(weight="weight")) == sorted(reference.degree(weight="weight"))
    assert attributed.size(weight="weight") == reference.size(weight="weight")
    assert attributed.size() == reference.size()


@NEEDS_COLLECTIONS
def test_a_weight_that_is_not_a_number_is_refused(attributed):
    attributed.add_edge(1, 2, weight="heavy")
    attributed.add_edge(2, 3, weight=1)
    with pytest.raises(TypeError):
        attributed.size(weight="weight")


@NEEDS_COLLECTIONS
def test_node_attributes(attributed):
    expected = {}
    for node in range(10):
        if node % 2:
            attributed.add_node(node, id=node, name=f"node{node}")
            expected[node] = {"id": node, "name": f"node{node}"}
        else:
            attributed.add_node(node)
            expected[node] = {}

    assert dict(attributed.nodes(data=True)) == expected
    assert dict(attributed.nodes(data="name")) == {node: expected[node].get("name") for node in range(10)}
    assert dict(attributed.nodes(data="id", default=-1)) == {node: expected[node].get("id", -1) for node in range(10)}
    assert attributed.nodes[3] == expected[3]
    assert attributed.get_node_attributes("name") == {node: data["name"] for node, data in expected.items() if data}

    attributed.clear()
    attributed.add_nodes_from(numpy.arange(100), name="node")
    assert all(data == {"name": "node"} for _, data in attributed.nodes(data=True))


@NEEDS_COLLECTIONS
def test_set_node_attributes(attributed):
    attributed.add_nodes_from([0, 1, 2])
    attributed.set_node_attributes(1, name="weight")
    assert dict(attributed.nodes(data="weight")) == {0: 1, 1: 1, 2: 1}

    attributed.set_node_attributes({0: "node0", 1: "node1", 2: "node2"}, name="name")
    attributed.set_node_attributes({0: {"id": "0"}, 1: {}, 2: {"number": 2}})
    assert dict(attributed.nodes(data=True)) == {
        0: {"name": "node0", "id": "0", "weight": 1},
        1: {"name": "node1", "weight": 1},
        2: {"name": "node2", "number": 2, "weight": 1},
    }


@NEEDS_COLLECTIONS
def test_edge_attributes_match_networkx(attributed, kind: str):
    reference = GRAPH_KINDS[kind][1]()
    for node in range(20):
        attributes = {"weight": node} if node % 2 else {}
        attributed.add_edge(node, node + 1, **attributes)
        reference.add_edge(node, node + 1, **attributes)

    assert canonical(attributed, attributed.edges(data="weight")) == canonical(
        attributed, reference.edges(data="weight")
    )
    assert canonical(attributed, attributed.edges(data="weight", default=1)) == canonical(
        attributed, reference.edges(data="weight", default=1)
    )
    for nbunch in [1, 2, [0, 1, 2, 3]]:
        assert canonical(attributed, attributed.edges(nbunch, data="weight")) == canonical(
            attributed, reference.edges(nbunch, data="weight")
        )
    # A multigraph keys edges by identifiers of its own, so only the attributes compare.
    found, expected = attributed.get_edge_data(1, 2), reference.get_edge_data(1, 2)
    if attributed.is_multigraph():
        found, expected = list(found.values()), list(expected.values())
    assert found == expected
    assert attributed.get_edge_data(40, 41, default="none") == "none"


@NEEDS_COLLECTIONS
def test_edge_attributes_in_batches(attributed):
    sources = numpy.array([1, 2, 3])
    targets = numpy.array([2, 3, 1])
    attributed.add_edges_from_arrays(
        sources, targets, columns={"weight": [0, 1, 2], "name": ["edge0", "edge1", "edge2"]}
    )
    assert attributed.has_edge(1, 2) and attributed.has_edge(2, 3) and attributed.has_edge(3, 1)
    labelled = {(source, target): data for source, target, data in attributed.edges(data=True)}
    assert labelled == {
        (1, 2): {"weight": 0, "name": "edge0"},
        (2, 3): {"weight": 1, "name": "edge1"},
        (3, 1): {"weight": 2, "name": "edge2"},
    }


@NEEDS_COLLECTIONS
@pytest.mark.parametrize("kind", ["graph", "digraph"], indirect=True)
def test_set_edge_attributes_of_simple_graphs(attributed):
    for node in range(3):
        attributed.add_edge(node, node + 1)
    attributed.set_edge_attributes(1, name="weight")
    attributed.set_edge_attributes({(0, 1): "edge0", (1, 2): "edge1", (2, 3): "edge2"}, name="name")
    attributed.set_edge_attributes({(0, 1): {"id": "0"}, (1, 2): {}, (2, 3): {"number": 2}})
    assert attributed.get_edge_attributes("weight") == {(0, 1): 1, (1, 2): 1, (2, 3): 1}
    assert [data for _, _, data in attributed.edges(data=True)] == [
        {"name": "edge0", "id": "0", "weight": 1},
        {"name": "edge1", "weight": 1},
        {"name": "edge2", "number": 2, "weight": 1},
    ]


@NEEDS_COLLECTIONS
@pytest.mark.parametrize("kind", ["multigraph", "multidigraph"], indirect=True)
def test_set_edge_attributes_of_multigraphs(attributed):
    keys = [attributed.add_edge(node, node + 1) for node in range(3)]
    attributed.set_edge_attributes({(node, node + 1, key): f"edge{node}" for node, key in enumerate(keys)}, name="name")
    assert attributed.get_edge_attributes("name") == {
        (node, node + 1, key): f"edge{node}" for node, key in enumerate(keys)
    }
    assert attributed.get_edge_data(0, 1) == {keys[0]: {"name": "edge0"}}
    assert attributed.get_edge_data(0, 1, keys[0]) == {"name": "edge0"}


@NEEDS_COLLECTIONS
def test_adding_an_edge_again_merges_its_attributes(attributed):
    attributed.add_edge(1, 2, name="edge1", weight=2)
    if not attributed.is_multigraph():
        attributed.add_edge(1, 2, id=1, weight=3)
        assert list(attributed.edges(data=True)) == [(1, 2, {"id": 1, "name": "edge1", "weight": 3})]
        return
    attributed.add_edge(1, 2, name="edge2", weight=2)
    attributed.add_edge(1, 2, key=1, weight=3)
    assert list(attributed.edges(keys=True, data=True)) == [
        (1, 2, 0, {"name": "edge1", "weight": 2}),
        (1, 2, 1, {"name": "edge2", "weight": 3}),
    ]


@NEEDS_COLLECTIONS
@pytest.mark.parametrize("kind", ["multigraph", "multidigraph"], indirect=True)
def test_multigraph_keys(attributed):
    for weight in (1, 2, 3):
        attributed.add_edge(1, 2, weight=weight)
    assert list(attributed.edges(keys=True, data=True)) == [
        (1, 2, 0, {"weight": 1}),
        (1, 2, 1, {"weight": 2}),
        (1, 2, 2, {"weight": 3}),
    ]

    attributed.remove_edge(1, 2)
    attributed.add_edge(1, 2, 3, weight=4)
    attributed.add_edge(1, 2, weight=5)
    attributed.add_edge(1, 2, weight=6)
    assert list(attributed.edges(keys=True, data=True)) == [
        (1, 2, 0, {"weight": 1}),
        (1, 2, 1, {"weight": 2}),
        (1, 2, 3, {"weight": 4}),
        (1, 2, 4, {"weight": 5}),
        (1, 2, 5, {"weight": 6}),
    ]


@NEEDS_COLLECTIONS
def test_attributes_never_share_the_graph_collection(view):
    with pytest.raises(ValueError):
        Graph(view, nodes=0)


def test_attributes_need_a_collection(graphs):
    ours, _ = graphs
    with pytest.raises(ValueError):
        ours.add_edge(1, 2, weight=1)


@pytest.mark.parametrize("kind", ["graph", "digraph"], indirect=True)
def test_simple_graphs_take_no_keys(graphs):
    ours, _ = graphs
    with pytest.raises(TypeError):
        ours.add_edges_from_arrays([1], [2], [7])
    with pytest.raises(TypeError):
        list(ours.edges(keys=True))


# endregion Attributes

# region Transactions


@NEEDS_TRANSACTIONS
def test_a_watched_edge_conflicts_with_a_concurrent_removal(database, kind: str):
    ours = GRAPH_KINDS[kind][0]
    ours(database).add_edges_from_arrays([1, 2], [2, 3])

    staged = database.transaction()
    try:
        assert ours(staged).has_edge(1, 2)
        ours(database).remove_edge(1, 2)
        with pytest.raises(ustore.ConflictError):
            staged.commit()
    finally:
        staged.close()


@NEEDS_TRANSACTIONS
def test_transactions_touching_one_vertex_conflict(database, kind: str):
    ours = GRAPH_KINDS[kind][0]
    first = database.transaction()
    second = database.transaction()
    try:
        ours(first).add_edge(1, 2)
        ours(second).add_edge(2, 3)
        first.commit()
        with pytest.raises(ustore.ConflictError):
            second.commit()
    finally:
        first.close()
        second.close()
    assert ours(database).has_edge(1, 2)
    assert not ours(database).has_edge(2, 3)


# endregion Transactions
