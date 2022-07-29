import ukv.stl as ukv
nx = ukv


def line(net):
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


def triangle(net):
    # 1 -> 2 -> 3 -> 1

    net.add_edge(1, 2)
    net.add_edge(2, 3)
    net.add_edge(3, 1)

    assert net.has_node(1) and net.has_node(2) and net.has_node(3)

    assert net.number_of_edges(1, 2) == 1
    assert net.number_of_edges(2, 3) == 1
    assert net.number_of_edges(3, 1) == 1

    assert len(list(net.successors(1))) == 1
    assert len(list(net.predecessors(1))) == 1

    net.remove_edge(1, 2)
    net.remove_edge(2, 3)
    net.remove_edge(3, 1)


def batch(net):
    nodes_count = 100
    edges = []
    for i in range(nodes_count-1):
        edges.append((i, i+1))

    net.add_edges_from(edges)

    for i in range(nodes_count-1):
        assert net.has_edge(i, i+1)
        assert net.has_node(i)

    middle_index = round(len(edges) / 2)

    net.remove_edges_from(edges[:middle_index])

    for edge in edges[:middle_index]:
        assert not net.has_edge(edge[0], edge[1])

    for edge in edges[middle_index:]:
        assert net.has_edge(edge[0], edge[1])


def clear(net):
    nodes_count = 100
    edges = []
    for i in range(nodes_count-1):
        edges.append((i, i+1))

    net.add_edges_from(edges)

    assert list(net.edges()) == edges
    assert list(net.nodes) == list(range(nodes_count))

    for edge in edges:
        assert net.has_edge(edge[0], edge[1])

    net.clear_edges()

    for edge in edges:
        assert not net.has_edge(edge[0], edge[1])

    for node in range(nodes_count):
        assert net.has_node(node)

    net.clear()

    for node in range(nodes_count):
        assert not net.has_node(node)


def degree(net):
    nodes_count = 100
    edges = []
    for i in range(nodes_count):
        for j in range(i+1, nodes_count):
            edges.append((i, j))

    net.add_edges_from(edges)

    for node in range(nodes_count):
        assert net.degree[node] == nodes_count-1


def test():
    # net = ukv.DiGraph()
    db = ukv.DataBase()
    net = ukv.Network(db, '.graph')
    line(net)
    triangle(net)
    batch(net)
    net.clear()
    clear(net)
    degree(net)


# def test_named():
#     net = ukv.Network(
#         relation_name='views',
#         source_attributes='people',
#         target_attributes='movies',
#         multi=False,
#         attributed_relations=True,
#     )
#     line(net)
#     triangle(net)
#     batch(net)
#     clear(net)
#     degree(net)
