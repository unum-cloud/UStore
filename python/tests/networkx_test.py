import ustore.ucset as ustore
import numpy as np
import pyarrow as pa
import pytest
import networkx as nx


def test_line():
    db = ustore.DataBase()
    graph = ustore.Graph(db, 'graph')
    digraph = ustore.DiGraph(db, 'digraph')

    # 1 -> 2
    assert not graph.has_node(1) and not digraph.has_node(1)
    assert not graph.has_node(2) and not digraph.has_node(2)
    assert not graph.has_edge(1, 2) and not digraph.has_edge(1, 2)

    graph.add_edge(1, 2)
    digraph.add_edge(1, 2)

    assert graph.has_node(1) and digraph.has_node(1)
    assert graph.has_node(2) and digraph.has_node(2)
    assert graph.number_of_edges(
        1, 2) == 1 and digraph.number_of_edges(1, 2) == 1
    assert graph.has_edge(1, 2) and digraph.has_edge(1, 2)
    assert graph.has_edge(2, 1) and not digraph.has_edge(2, 1)

    assert not graph.has_edge(2, 2) and not digraph.has_edge(2, 2)

    graph.remove_edge(1, 2)
    digraph.remove_edge(1, 2)
    assert graph.has_node(1) and digraph.has_node(1)
    assert graph.has_node(2) and digraph.has_node(2)

    assert not graph.has_edge(1, 2) and not digraph.has_edge(1, 2)
    graph.clear()
    digraph.clear()


def test_triangle():
    digraph = ustore.DataBase().main.digraph

    # 1 -> 2 -> 3 -> 1
    digraph.add_edge(1, 2)
    digraph.add_edge(2, 3)
    digraph.add_edge(3, 1)

    assert digraph.has_node(1) and digraph.has_node(2) and digraph.has_node(3)

    expected_nodes = [1, 2, 3]
    exported_nodes = []
    for node in digraph.nodes:
        exported_nodes.append(node)
    assert exported_nodes == expected_nodes

    assert digraph.number_of_edges() == 3
    assert digraph.number_of_edges(1, 2) == 1
    assert digraph.number_of_edges(2, 3) == 1
    assert digraph.number_of_edges(3, 1) == 1

    expected_edges = [(1, 2), (2, 3), (3, 1)]
    exported_edges = []

    for edge in digraph.edges:
        exported_edges.append(edge)
    assert exported_edges == expected_edges

    assert len(list(digraph.successors(1))) == 1
    assert len(list(digraph.predecessors(1))) == 1

    digraph.clear()


def test_triangle_batch():
    digraph = ustore.DataBase().main.digraph

    # TODO: Check why matrix casting fails!
    # edges = np.array([[1, 2], [2, 3], [3, 1]])
    # assert edges.shape[1] == 2
    # digraph.add_edges_from(edges)
    digraph.add_edges_from(np.array([1, 2, 3]), np.array([2, 3, 1]))

    assert digraph.has_node(1) and digraph.has_node(2) and digraph.has_node(3)
    assert not 4 in digraph

    assert digraph.has_edge(1, 2) and digraph.has_edge(
        2, 3) and digraph.has_edge(3, 1)
    assert not digraph.has_edge(2, 1)

    assert len(list(digraph.successors(1))) == 1
    assert len(list(digraph.predecessors(1))) == 1

    digraph.remove_edges_from(np.array([1, 2]), np.array([2, 3]))

    assert not digraph.has_edge(1, 2) and not digraph.has_edge(2, 3)
    assert digraph.has_edge(3, 1)

    digraph.clear()


def test_batch_attributes():
    db = ustore.DataBase()
    graph = ustore.Graph(db, 'graph', 'nodes', 'edges')

    source = pa.array([1, 2, 3])
    target = pa.array([2, 3, 1])
    keys = pa.array([1, 2, 3])
    weight = pa.array([0, 1, 2])
    name = pa.array(['edge0', 'edge1', 'edge2'])

    names = ['source', 'target', 'keys', 'weight', 'name']

    table = pa.Table.from_arrays(
        [source, target, keys, weight, name], names=names)
    graph.add_edges_from(table, 'source', 'target', 'keys')

    assert graph.has_node(1) and graph.has_node(2) and graph.has_node(3)
    assert not 4 in graph

    assert graph.has_edge(1, 2) and graph.has_edge(
        2, 3) and graph.has_edge(3, 1)

    index = 0
    for node, neighbor, data in graph.edges(data=True):
        assert node == source[index].as_py(
        ) and neighbor == target[index].as_py()
        assert data == {'weight': index, 'name': f'edge{index}'}
        index += 1

    graph.clear()


def test_random_fill():

    size = 100
    sources = np.random.randint(0, 1000, size)
    targets = np.random.randint(1000, 2000, size)

    graph = ustore.DataBase().main.graph
    graph.add_edges_from(sources, targets)
    for index in range(size):
        assert graph.has_node(sources[index])
        assert graph.has_node(targets[index])
        assert graph.has_edge(sources[index], targets[index])
        assert graph.has_edge(targets[index], sources[index])
    graph.clear()

    digraph = ustore.DataBase().main.digraph
    digraph.add_edges_from(sources, targets)
    for index in range(size):
        assert digraph.has_node(sources[index])
        assert digraph.has_node(targets[index])
        assert digraph.has_edge(sources[index], targets[index])
        assert not digraph.has_edge(targets[index], sources[index])
    digraph.clear()


def test_neighbors():
    db = ustore.DataBase()

    graph = db.main.graph
    graph.add_edge(1, 2)
    graph.add_edge(1, 3)
    graph.add_edge(2, 3)
    assert list(graph.neighbors(1)) == [2, 3]
    assert list(graph.neighbors(2)) == [1, 3]
    assert list(graph.neighbors(3)) == [1, 2]
    graph.clear()

    digraph = db.main.digraph
    digraph.add_edge(1, 2)
    digraph.add_edge(1, 3)
    digraph.add_edge(2, 3)
    assert list(digraph.neighbors(1)) == [2, 3]
    assert list(digraph.neighbors(2)) == [3]
    assert list(digraph.neighbors(3)) == []
    digraph.clear()


def test_degree():
    db = ustore.DataBase()
    graph = ustore.Graph(db, 'graph', relations='edges')
    digraph = ustore.DiGraph(db, 'digraph', relations='edges')

    sources = np.arange(100)
    targets = np.arange(1, 101)
    graph.add_edges_from(sources, targets, weight=2)
    digraph.add_edges_from(sources, targets, weight=2)

    g_degs = graph.degree
    dg_degs = digraph.degree
    assert g_degs[0] == g_degs[100] == dg_degs[0] == dg_degs[100] == 1
    for node in range(1, 100):
        assert g_degs[node] == dg_degs[node] == 2

    # Batch(List, Numpy)

    assert graph.degree([0, 1, 2]) == digraph.degree(
        [0, 1, 2]) == [(0, 1), (1, 2), (2, 2)]
    assert graph.degree([100, 1, 2, 0], weight='weight') == digraph.degree([100, 1, 2, 0], weight='weight') == [
        (100, 2), (1, 4), (2, 4), (0, 2)]

    # Whole graph
    expected_degrees = [2] * 99
    expected_degrees.append(1)
    expected_degrees.insert(0, 1)
    exported_degrees = []
    for node, deg in graph.degree:
        exported_degrees.append(deg)
    assert exported_degrees == expected_degrees

    expected_degrees = [deg*2 for deg in expected_degrees]

    exported_degrees = []
    for node, deg in graph.degree(weight='weight'):
        exported_degrees.append(deg)
    assert exported_degrees == expected_degrees

    in_degs = digraph.in_degree
    assert in_degs[0] == 0
    for node in range(1, 101):
        assert in_degs[node] == 1

    out_degs = digraph.out_degree
    assert out_degs[101] == 0
    for node in range(100):
        assert out_degs[node] == 1

    graph.clear()
    digraph.clear()


def test_upsert_remove_nodes_batch():
    graph = ustore.DataBase().main.graph
    nodes = np.arange(100)

    graph.add_nodes_from(nodes)
    for node in nodes:
        assert graph.has_node(node)

    graph.remove_nodes_from(nodes)
    for node in nodes:
        assert not graph.has_node(node)

    graph.clear()


def test_remove_edges():
    db = ustore.DataBase()
    graph = ustore.Graph(db, 'graph')
    digraph = ustore.DiGraph(db, 'digraph')

    sources = np.arange(100)
    targets = np.arange(100, 200)
    graph.add_edges_from(sources, targets)
    digraph.add_edges_from(sources, targets)

    for source, target in zip(sources[:50], targets[:50]):
        assert graph.has_node(source) and digraph.has_node(source)
        assert graph.has_node(target) and digraph.has_node(target)
        assert graph.has_edge(
            source, target) and digraph.has_edge(source, target)
        assert graph.has_edge(
            target, source) and not digraph.has_edge(target, source)
        graph.remove_edge(source, target)
        digraph.remove_edge(source, target)
        assert not graph.has_edge(source, target)

    for source, target in zip(sources[50:], targets[50:]):
        assert graph.has_edge(
            source, target) and digraph.has_edge(source, target)

    graph.remove_edges_from(sources[50:], targets[50:])
    digraph.remove_edges_from(sources[50:], targets[50:])
    for source, target in zip(sources[50:], targets[50:]):
        assert not graph.has_edge(
            source, target) and not digraph.has_edge(source, target)

    graph.clear()
    digraph.clear()


def test_remove_nodes_and_related_edges():
    graph = ustore.DataBase().main.graph

    sources = np.arange(100)
    targets = np.arange(100, 200)
    graph.add_edges_from(sources, targets)

    for source, target in zip(sources, targets):
        assert graph.has_node(source)
        assert graph.has_node(target)
        assert graph.has_edge(source, target)
        graph.remove_node(source)
        assert not graph.has_node(source)
        assert graph.has_node(target)
        assert not graph.has_edge(source, target)

    graph.clear()


def test_clear():
    graph = ustore.DataBase().main.graph

    sources = np.arange(100)
    targets = np.arange(100, 200)
    graph.add_edges_from(sources, targets)

    for source, target in zip(sources, targets):
        assert graph.has_node(source)
        assert graph.has_node(target)
        assert graph.has_edge(source, target)

    graph.clear_edges()
    for source, target in zip(sources, targets):
        assert graph.has_node(source)
        assert graph.has_node(target)
        assert not graph.has_edge(source, target)

    graph.clear()
    for source, target in zip(sources, targets):
        assert not graph.has_node(source)
        assert not graph.has_node(target)
        assert not graph.has_edge(source, target)

    graph.clear()


def test_size():
    db = ustore.DataBase()
    graph = ustore.MultiGraph(db, 'graph', relations='edges')
    size = 1000
    weighted_size = 0
    for node in range(size):
        graph.add_edge(node, node+1, node, weight=node)
        weighted_size += node

    assert graph.size() == size
    assert graph.size(weight='weight') == weighted_size

    graph.add_edge(size, size+1, size, weight='str')
    with pytest.raises(Exception):
        graph.size(weight='weight')


def test_nodes_attributes():
    db = ustore.DataBase()
    graph = ustore.Graph(db, 'graph', 'nodes')

    expected_node_data = {}
    retrieved_node_data = {}
    for i in range(10):
        if i % 2:
            graph.add_node(i, id=i, name='node{}'.format(i))
            expected_node_data[i] = {'id': i, 'name': 'node{}'.format(i)}
        else:
            graph.add_node(i)
            expected_node_data[i] = {}

    # Whole Data
    for node, data in graph.nodes(data=True):
        retrieved_node_data[node] = data
    assert retrieved_node_data == expected_node_data

    # Custom Field
    for node, name in graph.nodes(data='name'):
        assert name == ('node{}'.format(node) if node % 2 else None)

    # Custom Field With Default
    for node, id in graph.nodes(data='id', default=1):
        assert (id == node if node % 2 else id == 1)

    # Batch Upsert
    graph.clear()
    nodes = np.arange(100)
    graph.add_nodes_from(nodes, name='node')

    for node, data in graph.nodes(data=True):
        assert data == {'name': 'node'}

    graph.clear()


def test_edges_attributes():
    db = ustore.DataBase()
    graph = ustore.MultiGraph(db, 'graph', relations='edges')

    for i in range(100):
        graph.add_edge(
            i, i+1, i, weight=i) if i % 2 else graph.add_edge(i, i+1, i)

    index = 0
    for node, neighbor, data in graph.edges(data=True):
        assert node == index and neighbor == index+1
        assert data == ({'weight': index} if node % 2 else {})
        index += 1

    index = 0
    for node, neighbor, weight in graph.edges(data='weight'):
        assert node == index and neighbor == index+1
        assert weight == (node if node % 2 else None)
        index += 1

    index = 0
    for node, neighbor, weight in graph.edges(data='weight', default=1):
        assert node == index and neighbor == index+1
        assert weight == (node if node % 2 else 1)
        index += 1

    # nbunch
    assert list(graph.edges(1, data=True)) == [(1, 2, {'weight': 1})]
    assert list(graph.edges(2, data='weight', default=1)) == [
        (2, 3, 1)]
    assert list(graph.edges(3, data='weight')) == [
        (3, 4, 3)]

    index = 0
    for node, neighbor, data in graph.edges([0, 1, 2, 3], data=True):
        assert node == index and neighbor == index+1
        assert data == ({'weight': index} if node % 2 else {})
        index += 1

    index = 0
    for node, neighbor, weight in graph.edges([0, 1, 2, 3], data='weight', default=1):
        assert node == index and neighbor == index+1
        assert weight == (node if node % 2 else 1)
        index += 1

    # Batch
    graph.clear()
    sources = np.arange(100)
    targets = np.arange(100, 200)
    ids = np.arange(100)
    graph.add_edges_from(sources, targets, ids, weight=1)

    index = 0
    for node, neighbor, data in graph.edges(data=True):
        assert node == sources[index] and neighbor == targets[index]
        assert data == {'weight': 1}
        index += 1

    graph.clear()


def test_get_node_attributes():
    db = ustore.DataBase()
    graph = ustore.Graph(db, 'graph', 'nodes')

    for node in range(1000):
        graph.add_node(node, weight=node)

    weights = graph.get_node_attributes('weight')
    for node in range(1000):
        assert weights[node] == node


def test_get_edge_attributes():
    db = ustore.DataBase()
    graph = ustore.MultiGraph(db, 'graph', relations='edges')

    for node in range(1000):
        graph.add_edge(node, node+1, node, weight=node)

    weights = graph.get_edge_attributes('weight')
    for node in range(1000):
        assert weights[(node, node+1, node)] == node

    for node in range(1000):
        assert graph.get_edge_data(node, node+1) == {'weight': node}


def test_set_node_attributes():
    db = ustore.DataBase()
    graph = ustore.Graph(db, 'graph', 'nodes')

    graph.add_node(0)
    graph.add_node(1)
    graph.add_node(2)

    # Set Attributes From Scalar And Name
    attributes = 1
    graph.set_node_attributes(attributes, name='weight')

    index = 0
    for node, weight in graph.nodes(data='weight'):
        assert node == index
        assert weight == 1
        index += 1

    # Set Attributes From Dict And Name
    attributes = {0: 'node0', 1: 'node1', 2: 'node2'}
    graph.set_node_attributes(attributes, name='name')

    index = 0
    for node, name in graph.nodes(data='name'):
        assert node == index
        assert name == 'node{}'.format(index)
        index += 1

    # Set Attributes From Dict of Dict
    attributes = {0: {'id': '0'}, 1: {
    }, 2: {'number': 2}}

    graph.set_node_attributes(attributes)
    expected_datas = [{'name': 'node0', 'id': '0', 'weight': 1}, {
        'name': 'node1', 'weight': 1}, {'name': 'node2', 'number': 2, 'weight': 1}]
    exported_datas = []
    for node, data in graph.nodes(data=True):
        exported_datas.append(data)

    assert expected_datas == exported_datas


def test_set_edge_attributes():
    db = ustore.DataBase()
    graph = ustore.MultiGraph(db, relations='edges')

    graph.add_edge(0, 1, 0)
    graph.add_edge(1, 2, 1)
    graph.add_edge(2, 3, 2)

    # Set Attributes From Scalar And Name
    attributes = 1
    graph.set_edge_attributes(attributes, name='weight')

    index = 0
    for node, neighbor, weight in graph.edges(data='weight'):
        assert node == index and neighbor == index+1 and weight == 1
        index += 1

    # Set Attributes From Dict And Name
    attributes = {(0, 1, 0): 'edge0', (1, 2, 1): 'edge1', (2, 3, 2): 'edge2'}
    graph.set_edge_attributes(attributes, name='name')

    index = 0
    for node, neighbor, name in graph.edges(data='name'):
        assert node == index and neighbor == index+1
        assert name == 'edge{}'.format(index)
        index += 1

    # Set Attributes From Dict of Dict
    attributes = {(0, 1, 0): {'id': '0'}, (1, 2, 1): {
    }, (2, 3, 2): {'number': 2}}

    graph.set_edge_attributes(attributes)
    expected_datas = [{'name': 'edge0', 'id': '0', 'weight': 1}, {
        'name': 'edge1', 'weight': 1}, {'name': 'edge2', 'number': 2, 'weight': 1}]
    exported_datas = []
    for _, _, data in graph.edges(data=True):
        exported_datas.append(data)

    assert expected_datas == exported_datas

    graph.clear()


def test_add_remove_edges():
    db = ustore.DataBase()
    graph = ustore.Graph(db, 'graph', relations='edges')
    digraph = ustore.DiGraph(db, 'digraph', relations='di_edges')
    multigraph = ustore.MultiGraph(db, 'multigraph', relations='multi_edges')
    multidigraph = ustore.MultiDiGraph(
        db, 'multidigraph', relations='multidi_edges')

    sources_to_add = np.array([1, 2, 2, 3])
    targets_to_add = np.array([4, 5, 5, 6])

    graph.add_edges_from(sources_to_add, targets_to_add, weight=2)
    digraph.add_edges_from(sources_to_add, targets_to_add, weight=2)
    multigraph.add_edges_from(sources_to_add, targets_to_add, weight=2)
    multidigraph.add_edges_from(sources_to_add, targets_to_add, weight=2)

    assert list(graph.edges()) == [(1, 4), (2, 5), (3, 6)]
    assert list(digraph.edges()) == [(1, 4), (2, 5), (3, 6)]
    assert list(multigraph.edges()) == [(1, 4), (2, 5), (2, 5), (3, 6)]
    assert list(multidigraph.edges()) == [(1, 4), (2, 5), (2, 5), (3, 6)]

    sources_to_remove = np.array([1, 2])
    targets_to_remove = np.array([4, 5])

    graph.remove_edges_from(sources_to_remove, targets_to_remove)
    digraph.remove_edges_from(sources_to_remove, targets_to_remove)
    multigraph.remove_edges_from(sources_to_remove, targets_to_remove)
    multidigraph.remove_edges_from(sources_to_remove, targets_to_remove)

    assert list(graph.edges()) == [(3, 6)]
    assert list(digraph.edges()) == [(3, 6)]
    assert list(multigraph.edges()) == [(2, 5), (3, 6)]
    assert list(multidigraph.edges()) == [(2, 5), (3, 6)]


def test_iterate_edges():
    db = ustore.DataBase()
    graph = ustore.Graph(db, 'graph', relations='edges')
    digraph = ustore.DiGraph(db, 'digraph', relations='di_edges')
    multigraph = ustore.MultiGraph(db, 'multigraph', relations='multi_edges')
    multidigraph = ustore.MultiDiGraph(
        db, 'multidigraph', relations='multidi_edges')

    sources_to_add = np.array([1, 2, 2, 3])
    targets_to_add = np.array([4, 5, 5, 6])

    graph.add_edges_from(sources_to_add, targets_to_add, weight=2)
    digraph.add_edges_from(sources_to_add, targets_to_add, weight=2)
    multigraph.add_edges_from(sources_to_add, targets_to_add, weight=2)
    multidigraph.add_edges_from(sources_to_add, targets_to_add, weight=2)

    data = {'weight': 2}
    assert list(graph.edges()) == [(1, 4), (2, 5), (3, 6)]
    assert list(graph.edges(data=True)) == [
        (1, 4, data), (2, 5, data), (3, 6, data)]

    assert list(digraph.edges()) == [(1, 4), (2, 5), (3, 6)]
    assert list(digraph.edges(data=True)) == [
        (1, 4, data), (2, 5, data), (3, 6, data)]

    assert list(multigraph.edges()) == [(1, 4), (2, 5), (2, 5), (3, 6)]
    assert list(multigraph.edges(data=True)) == [
        (1, 4, data), (2, 5, data), (2, 5, data), (3, 6, data)]
    assert list(multigraph.edges(keys=True)) == [
        (1, 4, 0), (2, 5, 1), (2, 5, 2), (3, 6, 3)]

    assert list(multidigraph.edges()) == [(1, 4), (2, 5), (2, 5), (3, 6)]
    assert list(multidigraph.edges(data=True)) == [
        (1, 4, data), (2, 5, data), (2, 5, data), (3, 6, data)]
    assert list(multidigraph.edges(keys=True)) == [
        (1, 4, 0), (2, 5, 1), (2, 5, 2), (3, 6, 3)]

    graph.clear()
    digraph.clear()
    multigraph.clear()
    multidigraph.clear()


def test_keys():

    db = ustore.DataBase()
    graph = ustore.Graph(db, 'graph', relations='edges')
    multigraph = ustore.MultiGraph(db, 'multigraph', relations='multi_edges')

    graph.add_edge(1, 2, weight=1)
    graph.add_edge(1, 2, weight=2)
    graph.add_edge(1, 2, weight=3)

    multigraph.add_edge(1, 2, weight=1)
    multigraph.add_edge(1, 2, weight=2)
    multigraph.add_edge(1, 2, weight=3)

    assert list(graph.edges()) == [(1, 2)]
    assert list(multigraph.edges(keys=True, data=True)) == [
        (1, 2, 0, {'weight': 1}), (1, 2, 1, {'weight': 2}), (1, 2, 2, {'weight': 3})]

    multigraph.remove_edge(1, 2)
    assert list(multigraph.edges(keys=True, data=True)) == [
        (1, 2, 0, {'weight': 1}), (1, 2, 1, {'weight': 2})]

    multigraph.add_edge(1, 2, 3, weight=4)
    assert list(multigraph.edges(keys=True, data=True)) == [
        (1, 2, 0, {'weight': 1}), (1, 2, 1, {'weight': 2}), (1, 2, 3, {'weight': 4})]

    multigraph.add_edge(1, 2, weight=5)
    multigraph.add_edge(1, 2, weight=6)
    assert list(multigraph.edges(keys=True, data=True)) == [
        (1, 2, 0, {'weight': 1}), (1, 2, 1, {'weight': 2}), (1, 2, 3, {'weight': 4}), (1, 2, 4, {'weight': 5}), (1, 2, 5, {'weight': 6})]


def test_add_overwrite_edges():
    db = ustore.DataBase()

    graph = ustore.Graph(db, 'graph', relations='edges')
    graph.add_edge(1, 2, name='edge1', weight=2)
    graph.add_edge(1, 2, id=1, weight=3)
    assert list(graph.edges(data=True)) == [
        (1, 2, {'id': 1, 'name': 'edge1', 'weight': 3})]

    multigraph = ustore.MultiGraph(db, 'multigraph', relations='multi_edges')
    multigraph.add_edge(1, 2, name='edge1', weight=2)
    multigraph.add_edge(1, 2, name='edge2', weight=2)
    multigraph.add_edge(1, 2, key=1, weight=3)
    assert list(multigraph.edges(keys=True, data=True)) == [(1, 2, 0, {'name': 'edge1', 'weight': 2}), (1, 2, 1, {
        'name': 'edge2', 'weight': 3})]


def test_transaction_watch():
    db = ustore.DataBase()
    graph = db.main.graph

    graph.add_edge(1, 2)
    graph.add_edge(2, 3)

    txn = ustore.Transaction(db)
    txn_graph = txn.main.graph

    assert txn_graph.has_edge(1, 2)
    graph.remove_edge(1, 2)
    with pytest.raises(Exception):
        txn.commit()

    graph.clear()


def test_conflicting_transactions():
    db = ustore.DataBase()

    txn1 = ustore.Transaction(db)
    txn2 = ustore.Transaction(db)

    txn_net1 = txn1.main.graph
    txn_net2 = txn2.main.graph

    txn_net1.add_edge(1, 2)
    txn_net2.add_edge(2, 3)

    txn1.commit()
    with pytest.raises(Exception):
        txn2.commit()
