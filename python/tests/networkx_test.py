import ustore.ucset as ustore
import numpy as np
import pytest
import networkx as nx


def test_line():
    net = ustore.DataBase().main.graph

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

    net.clear()


def test_triangle():
    net = ustore.DataBase().main.graph

    # 1 -> 2 -> 3 -> 1
    net.add_edge(1, 2)
    net.add_edge(2, 3)
    net.add_edge(3, 1)

    assert net.has_node(1) and net.has_node(2) and net.has_node(3)

    expected_nodes = [1, 2, 3]
    exported_nodes = []
    for node in net.nodes:
        exported_nodes.append(node)
    assert exported_nodes == expected_nodes

    assert net.number_of_edges() == 3
    assert net.number_of_edges(1, 2) == 1
    assert net.number_of_edges(2, 3) == 1
    assert net.number_of_edges(3, 1) == 1

    expected_edges = [(1, 2), (2, 3), (3, 1)]
    exported_edges = []
    for edge in net.edges:
        exported_edges.append(edge)
    assert exported_edges == expected_edges

    assert len(list(net.successors(1))) == 1
    assert len(list(net.predecessors(1))) == 1

    net.clear()


def test_triangle_batch():
    net = ustore.DataBase().main.graph

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

    net.clear()


def test_random_fill():
    net = ustore.DataBase().main.graph

    sources = np.random.randint(1000, size=100)
    targets = np.random.randint(1000, size=100)
    net.add_edges_from(sources, targets)

    for index in range(100):
        assert net.has_node(sources[index])
        assert net.has_node(targets[index])
        assert net.has_edge(sources[index], targets[index])

    net.clear()


def test_neighbors():
    db = ustore.DataBase()
    net = db.main.graph

    sources = np.arange(100)
    targets = np.arange(1, 101)
    net.add_edges_from(sources, targets)

    neighbors = net[0]
    assert len(neighbors) == 1
    assert neighbors[0] == 1

    neighbors = net[100]
    assert len(neighbors) == 1
    assert neighbors[0] == 99

    for node in range(1, 100):
        neighbors = net[node]
        assert len(neighbors) == 2
        assert neighbors[0] == node - 1
        assert neighbors[1] == node + 1

    net.clear()


def test_degree():
    db = ustore.DataBase()
    net = ustore.Network(db, 'graph', 'nodes', 'edges')

    sources = np.arange(100)
    targets = np.arange(1, 101)
    edge_ids = np.arange(100)
    net.add_edges_from(sources, targets, edge_ids, weight=2)

    degs = net.degree
    assert degs[0] == degs[100] == 1
    for node in range(1, 100):
        assert degs[node] == 2

    in_degs = net.in_degree
    assert in_degs[0] == 0
    for node in range(1, 101):
        assert in_degs[node] == 1

    out_degs = net.out_degree
    assert out_degs[101] == 0
    for node in range(100):
        assert out_degs[node] == 1

    # Batch(List, Numpy)
    assert net.degree([0, 1, 2]) == [(0, 1), (1, 2), (2, 2)]
    assert net.degree([100, 1, 2, 0], weight='weight') == [
        (100, 2), (1, 4), (2, 4), (0, 2)]

    # Whole graph
    expected_degrees = [2] * 99
    expected_degrees.append(1)
    expected_degrees.insert(0, 1)
    exported_degrees = []
    for node, deg in net.degree:
        exported_degrees.append(deg)
    assert exported_degrees == expected_degrees

    expected_degrees = [deg*2 for deg in expected_degrees]

    exported_degrees = []
    for node, deg in net.degree(weight='weight'):
        exported_degrees.append(deg)
    assert exported_degrees == expected_degrees

    net.clear()


def test_upsert_remove_nodes_batch():
    net = ustore.DataBase().main.graph

    nodes = np.arange(100)

    net.add_nodes_from(nodes)
    for node in nodes:
        assert net.has_node(node)

    net.remove_nodes_from(nodes)
    for node in nodes:
        assert not net.has_node(node)

    net.clear()


def test_remove_edges():
    net = ustore.DataBase().main.graph

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

    net.clear()


def test_remove_nodes_and_related_edges():
    net = ustore.DataBase().main.graph

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

    net.clear()


def test_clear():
    net = ustore.DataBase().main.graph

    sources = np.arange(100)
    targets = np.arange(100, 200)
    net.add_edges_from(sources, targets)

    for source, target in zip(sources, targets):
        assert net.has_node(source)
        assert net.has_node(target)
        assert net.has_edge(source, target)

    net.clear_edges()
    for source, target in zip(sources, targets):
        assert net.has_node(source)
        assert net.has_node(target)
        assert not net.has_edge(source, target)

    net.clear()
    for source, target in zip(sources, targets):
        assert not net.has_node(source)
        assert not net.has_node(target)
        assert not net.has_edge(source, target)

    net.clear()


def test_size():
    db = ustore.DataBase()
    net = ustore.Network(db, 'graph', 'nodes', 'edges')
    size = 1000
    weighted_size = 0
    for node in range(size):
        net.add_edge(node, node+1, node, weight=node)
        weighted_size += node

    assert net.size() == size
    assert net.size(weight='weight') == weighted_size

    net.add_edge(size, size+1, size, weight='str')
    with pytest.raises(Exception):
        net.size(weight='weight')


def test_nodes_attributes():
    db = ustore.DataBase()
    net = ustore.Network(db, 'graph', 'nodes')

    expected_node_data = {}
    retrieved_node_data = {}
    for i in range(10):
        if i % 2:
            net.add_node(i, id=i, name='node{}'.format(i))
            expected_node_data[i] = {'id': i, 'name': 'node{}'.format(i)}
        else:
            net.add_node(i)
            expected_node_data[i] = {}

    # Whole Data
    for node, data in net.nodes(data=True):
        retrieved_node_data[node] = data
    assert retrieved_node_data == expected_node_data

    # Custom Field
    for node, name in net.nodes(data='name'):
        assert name == ('node{}'.format(node) if node % 2 else None)

    # Custom Field With Default
    for node, id in net.nodes(data='id', default=1):
        assert (id == node if node % 2 else id == 1)

    # Batch Upsert
    net.clear()
    nodes = np.arange(100)
    net.add_nodes_from(nodes, name='node')

    for node, data in net.nodes(data=True):
        assert data == {'name': 'node'}

    net.clear()


def test_edges_attributes():
    db = ustore.DataBase()
    net = ustore.Network(db, 'graph', 'nodes', 'edges')

    for i in range(100):
        net.add_edge(i, i+1, i, weight=i) if i % 2 else net.add_edge(i, i+1, i)

    index = 0
    for node, neighbor, data in net.edges(data=True):
        assert node == index and neighbor == index+1
        assert data == ({'weight': index} if node % 2 else {})
        index += 1

    index = 0
    for node, neighbor, weight in net.edges(data='weight'):
        assert node == index and neighbor == index+1
        assert weight == (node if node % 2 else None)
        index += 1

    index = 0
    for node, neighbor, weight in net.edges(data='weight', default=1):
        assert node == index and neighbor == index+1
        assert weight == (node if node % 2 else 1)
        index += 1

    # nbunch
    assert list(net.edges(1, data=True)) == [(1, 2, {'weight': 1})]
    assert list(net.edges(2, data='weight', default=1)) == [
        (2, 3, 1)]
    assert list(net.edges(3, data='weight')) == [
        (3, 4, 3)]

    index = 0
    for node, neighbor, data in net.edges([0, 1, 2, 3], data=True):
        assert node == index and neighbor == index+1
        assert data == ({'weight': index} if node % 2 else {})
        index += 1

    index = 0
    for node, neighbor, weight in net.edges([0, 1, 2, 3], data='weight', default=1):
        assert node == index and neighbor == index+1
        assert weight == (node if node % 2 else 1)
        index += 1

    # Batch
    net.clear()
    sources = np.arange(100)
    targets = np.arange(100, 200)
    ids = np.arange(100)
    net.add_edges_from(sources, targets, ids, weight=1)

    index = 0
    for node, neighbor, data in net.edges(data=True):
        assert node == sources[index] and neighbor == targets[index]
        assert data == {'weight': 1}
        index += 1

    net.clear()


def test_get_node_attributes():
    db = ustore.DataBase()
    net = ustore.Network(db, 'graph', 'nodes')

    for node in range(1000):
        net.add_node(node, weight=node)

    weights = net.get_node_attributes('weight')
    for node in range(1000):
        assert weights[node] == node


def test_get_edge_attributes():
    db = ustore.DataBase()
    net = ustore.Network(db, 'graph', 'nodes', 'edges')

    for node in range(1000):
        net.add_edge(node, node+1, node, weight=node)

    weights = net.get_edge_attributes('weight')
    for node in range(1000):
        assert weights[(node, node+1, node)] == node

    for node in range(1000):
        assert net.get_edge_data(node, node+1) == {'weight': node}


def test_set_node_attributes():
    db = ustore.DataBase()
    net = ustore.Network(db, 'graph', 'nodes')

    net.add_node(0)
    net.add_node(1)
    net.add_node(2)

    # Set Attributes From Scalar And Name
    attributes = 1
    net.set_node_attributes(attributes, name='weight')

    index = 0
    for node, weight in net.nodes(data='weight'):
        assert node == index
        assert weight == 1
        index += 1

    # Set Attributes From Dict And Name
    attributes = {0: 'node0', 1: 'node1', 2: 'node2'}
    net.set_node_attributes(attributes, name='name')

    index = 0
    for node, name in net.nodes(data='name'):
        assert node == index
        assert name == 'node{}'.format(index)
        index += 1

    # Set Attributes From Dict of Dict
    attributes = {0: {'id': '0'}, 1: {
    }, 2: {'number': 2}}

    net.set_node_attributes(attributes)
    expected_datas = [{'name': 'node0', 'id': '0', 'weight': 1}, {
        'name': 'node1', 'weight': 1}, {'name': 'node2', 'number': 2, 'weight': 1}]
    exported_datas = []
    for node, data in net.nodes(data=True):
        exported_datas.append(data)

    assert expected_datas == exported_datas


def test_set_edge_attributes():
    db = ustore.DataBase()
    net = ustore.Network(db, relations='edges')

    net.add_edge(0, 1, 0)
    net.add_edge(1, 2, 1)
    net.add_edge(2, 3, 2)

    # Set Attributes From Scalar And Name
    attributes = 1
    net.set_edge_attributes(attributes, name='weight')

    index = 0
    for node, neighbor, weight in net.edges(data='weight'):
        assert node == index and neighbor == index+1 and weight == 1
        index += 1

    # Set Attributes From Dict And Name
    attributes = {(0, 1, 0): 'edge0', (1, 2, 1): 'edge1', (2, 3, 2): 'edge2'}
    net.set_edge_attributes(attributes, name='name')

    index = 0
    for node, neighbor, name in net.edges(data='name'):
        assert node == index and neighbor == index+1
        assert name == 'edge{}'.format(index)
        index += 1

    # Set Attributes From Dict of Dict
    attributes = {(0, 1, 0): {'id': '0'}, (1, 2, 1): {
    }, (2, 3, 2): {'number': 2}}

    net.set_edge_attributes(attributes)
    expected_datas = [{'name': 'edge0', 'id': '0', 'weight': 1}, {
        'name': 'edge1', 'weight': 1}, {'name': 'edge2', 'number': 2, 'weight': 1}]
    exported_datas = []
    for _, _, data in net.edges(data=True):
        exported_datas.append(data)

    assert expected_datas == exported_datas


def test_transaction_watch():
    db = ustore.DataBase()
    net = db.main.graph

    net.add_edge(1, 2)
    net.add_edge(2, 3)

    txn = ustore.Transaction(db)
    txn_net = txn.main.graph

    assert txn_net.has_edge(1, 2)
    net.remove_edge(1, 2)
    with pytest.raises(Exception):
        txn.commit()

    net.clear()


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
