import ukv.umem as ukv
import numpy as np
import pytest


def test_line():
    db = ukv.DataBase()
    net = db.main.graph

    # 1 -> 2
    assert not net.has_node(1)
    assert not net.has_node(2)
    assert not net.has_edge(1, 2)

    net.add_edge(1, 2)
    assert net.has_node(1)
    assert net.has_node(2)
    assert net.number_of_edges(1, 2) == 1
    assert net.has_edge(1, 2)

    assert not net.has_edge(2, 2)

    net.remove_edge(1, 2)
    assert net.has_node(1)
    assert net.has_node(2)
    assert not net.has_edge(1, 2)


def test_triangle():
    db = ukv.DataBase()
    net = db.main.graph

    # 1 -> 2 -> 3 -> 1
    net.add_edge(1, 2)
    net.add_edge(2, 3)
    net.add_edge(3, 1)

    assert net.has_node(1) and net.has_node(2) and net.has_node(3)

    expected_nodes = [1,2,3]
    exported_nodes = []
    for i, node in enumerate(net.nodes()):
        exported_nodes.append(node)
    assert exported_nodes == expected_nodes

    assert net.number_of_edges() == 3
    assert net.number_of_edges(1, 2) == 1
    assert net.number_of_edges(2, 3) == 1
    assert net.number_of_edges(3, 1) == 1

    expected_edges = [(1,2),(2,3),(3,1)]
    exported_edges = []
    for edge in net.edges():
        exported_edges.append(edge)
    assert exported_edges == expected_edges

    assert len(list(net.successors(1))) == 1
    assert len(list(net.predecessors(1))) == 1


def test_triangle_batch():
    db = ukv.DataBase()
    net = db.main.graph

    # TODO: Check why matrix casting fails!
    # edges = np.array([[1, 2], [2, 3], [3, 1]])
    # assert edges.shape[1] == 2
    # net.add_edges_from(edges)
    net.add_edges_from(np.array([1, 2, 3]), np.array([2, 3, 1]))

    assert net.has_node(1) and net.has_node(2) and net.has_node(3)
    assert not 4 in net

    assert net.has_edge(1, 2) and net.has_edge(2, 3) and net.has_edge(3, 1)
    assert not net.has_edge(2, 1)

    assert len(list(net.successors(1))) == 1
    assert len(list(net.predecessors(1))) == 1

    net.remove_edges_from(np.array([1, 2]), np.array([2, 3]))

    assert not net.has_edge(1, 2) and not net.has_edge(2, 3)
    assert net.has_edge(3, 1)


def test_random_fill():
    db = ukv.DataBase()
    net = db.main.graph

    sources = np.random.randint(1000, size=100)
    targets = np.random.randint(1000, size=100)
    net.add_edges_from(sources, targets)

    for index in range(100):
        assert net.has_node(sources[index])
        assert net.has_node(targets[index])
        assert net.has_edge(sources[index], targets[index])


def test_neighbors():
    db = ukv.DataBase()
    net = db.main.graph

    sources = np.arange(100)
    targets = np.arange(1,101)
    net.add_edges_from(sources,targets)

    neighbors = net.neighbors(0)
    assert len(neighbors) == 1
    assert neighbors[0] == 1 

    neighbors = net.neighbors(100)
    assert len(neighbors) == 1
    assert neighbors[0] == 99 

    for node in range(1,100):
        neighbors = net.neighbors(node)
        assert len(neighbors) == 2
        assert neighbors[0] == node + 1 
        assert neighbors[1] == node - 1


def test_degree():
    db = ukv.DataBase()
    net = db.main.graph

    sources = np.arange(100)
    targets = np.arange(1,101)
    net.add_edges_from(sources,targets)

    degs = net.degree
    assert degs[0] == degs[100] == 1
    for node in range(1,100):
        assert degs[node] == 2

    in_degs = net.in_degree
    assert in_degs[0] == 0
    for node in range(1,101):
        assert in_degs[node] == 1

    out_degs = net.out_degree
    assert out_degs[101] == 0
    for node in range(100):
        assert out_degs[node] == 1

    # Batch
    expected_degrees = [2] * 99
    expected_degrees.append(1)
    expected_degrees.insert(0,1)
    vertices = np.arange(101)
    degrees = degs[vertices]
    for i in vertices:
        assert degrees[i] == expected_degrees[i]


def test_upsert_remove_nodes_batch():
    db = ukv.DataBase()
    net = db.main.graph
    nodes = np.arange(100)

    net.add_nodes_from(nodes)
    for node in nodes:
        assert net.has_node(node)

    net.remove_nodes_from(nodes)
    for node in nodes:
        assert not net.has_node(node)


def test_remove_edges():
    db = ukv.DataBase()
    net = db.main.graph

    sources = np.arange(100)
    targets = np.arange(100, 200)
    net.add_edges_from(sources, targets)

    for source, target in zip(sources[:50], targets[:50]):
        assert net.has_node(source)
        assert net.has_node(target)
        assert net.has_edge(source, target)
        net.remove_edge(source, target)
        assert not net.has_edge(source, target)

    for source, target in zip(sources[50:], targets[50:]):
        assert net.has_edge(source, target)

    net.remove_edges_from(sources[50:], targets[50:])
    for source, target in zip(sources[50:], targets[50:]):
        assert not net.has_edge(source, target)


def test_remove_nodes_and_related_edges():
    db = ukv.DataBase()
    net = db.main.graph

    sources = np.arange(100)
    targets = np.arange(100, 200)
    net.add_edges_from(sources, targets)

    for source, target in zip(sources, targets):
        assert net.has_node(source)
        assert net.has_node(target)
        assert net.has_edge(source, target)
        net.remove_node(source)
        assert not net.has_node(source)
        assert net.has_node(target)
        assert not net.has_edge(source, target)


def test_transaction_watch():
    db = ukv.DataBase()
    net = db.main.graph

    net.add_edge(1, 2)
    net.add_edge(2, 3)

    txn = ukv.Transaction(db)
    txn_net = txn.main.graph

    assert txn_net.has_edge(1, 2)
    net.remove_edge(1, 2)
    with pytest.raises(Exception):
        txn.commit()


def test_conflicting_transactions():
    db = ukv.DataBase()

    txn1 = ukv.Transaction(db)
    txn2 = ukv.Transaction(db)

    txn_net1 = txn1.main.graph
    txn_net2 = txn2.main.graph

    txn_net1.add_edge(1, 2)
    txn_net2.add_edge(2, 3)

    txn1.commit()
    with pytest.raises(Exception):
        txn2.commit()
