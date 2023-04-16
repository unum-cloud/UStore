# Baseline Python implementation of Louvain Community detection algorithm,
# similar to the one in NetworkX, designed to operate with both in-memory
# graphs and the ones persisted in UStore.
from typing import Mapping, Tuple

import networkx as nx


def _node_affiliation(graph, partition: Mapping[int, int], node: int) -> Tuple[int, int]:
    """
    Simultaneously computes the degree (without self-edges) between `node` and
    members of `node_community` as well as `neighbor_community`.
    """

    degrees = {}
    for neighbor in graph[node]:
        if neighbor != node:
            degrees[partition[neighbor]] = degrees.get(
                partition[neighbor], 0) + 1
    return degrees


def _node_affiliation_weighted(graph, partition: Mapping[int, int], node: int) -> Tuple[int, int]:
    """
    Simultaneously computes the weighted degree (without self-edges) between `node` and
    members of `node_community` (without counting self-edge) as well as `neighbor_community`.
    """

    degrees = {}
    for neighbor, data in graph[node].items():
        if neighbor != node:
            degrees[partition[neighbor]] = degrees.get(
                partition[neighbor], 0) + data.get('weight', 1)
    return degrees


def _one_level(graph, edges_weight_sum: float, is_weighted: bool) \
    -> tuple[
        Mapping[int, int],
        Mapping[int, float],
        Mapping[int, float],
        bool
]:
    """
    Compute one level communities of graph.
    """

    partition = {}
    degrees = {}
    # For every partition, stores the accumulated number of degrees of all of its members
    community_degrees = {}
    # For every partition, stores the accumulated number of degrees between all of its members
    community_in_degrees = {}

    if is_weighted:
        for node in graph.nodes:
            community_in_degrees[node] = 0
            degrees[node] = graph.degree(weight='weight')[node]
            partition[node] = node
    else:
        for node in graph.nodes:
            community_in_degrees[node] = 0
            degrees[node] = graph.degree[node]
            partition[node] = node

    community_degrees = degrees.copy()
    affiliation_func = _node_affiliation_weighted if is_weighted else _node_affiliation

    improvement = False
    modified = 1
    while modified:
        modified = 0
        for node in graph.nodes:
            node_degree = degrees[node]
            node_community = partition[node]
            best_mod = 0
            best_com = node_community
            node_com_tot_degree = community_degrees[node_community]

            degree_in_coms = affiliation_func(graph, partition, node)
            node_in_node_com_degree = degree_in_coms.get(node_community, 0)

            for neighbor in graph[node]:

                neighbor_community = partition[neighbor]
                if node_community == neighbor_community:
                    continue

                neighbor_com_tot_degree = community_degrees[neighbor_community]
                node_in_neighbor_com_degree = degree_in_coms[neighbor_community]

                delta_modularity = (1 / edges_weight_sum) * (node_in_neighbor_com_degree - node_in_node_com_degree) + \
                    (node_degree / (2 * (edges_weight_sum ** 2))) * \
                    (node_com_tot_degree - node_degree - neighbor_com_tot_degree)

                if delta_modularity > best_mod:
                    best_mod = delta_modularity
                    best_com = neighbor_community

            if best_com != node_community:
                community_degrees[node_community] -= node_degree
                node_in_best_com_degree = degree_in_coms[best_com]
                community_degrees[best_com] += node_degree
                community_in_degrees[node_community] -= node_in_node_com_degree
                community_in_degrees[best_com] += node_in_best_com_degree
                partition[node] = best_com
                modified += 1
                improvement = True

            # TODO: remove this awkward logic to assume NetworkX Graph here
            # But still we need to add weights for self-edges in community_in
            if is_weighted:
                edge_weight = graph.get_edge_data(node, node)
                community_in_degrees[best_com] += edge_weight.get(
                    'weight', 1) if edge_weight is not None else 0
            else:
                community_in_degrees[best_com] += 1 if graph.has_edge(
                    node, node) else 0

    return partition, community_degrees, community_in_degrees, improvement


def _gen_graph(graph, partition: Mapping[int, int]):
    """
    Create super node graph, where nodes are communities and edges are degrees between communities.
    """

    G = nx.Graph()
    G.add_nodes_from(partition.values())

    for node1, node2 in graph.edges:
        com1 = partition[node1]
        com2 = partition[node2]
        w_prec = G.get_edge_data(com1, com2, {'weight': 0}).get('weight', 1)
        G.add_edge(com1, com2, **{'weight': w_prec + 1})

    return G


def _gen_graph_from_weighted(graph, partition: Mapping[int, int]):
    """
    Create super node graph from weighted graph, where nodes are communities and edges are degrees between communities.
    """

    G = nx.Graph()
    G.add_nodes_from(partition.values())

    for node1, node2, data in graph.edges(data=True):
        com1 = partition[node1]
        com2 = partition[node2]
        edge_weight = data.get('weight', 1)
        w_prec = G.get_edge_data(com1, com2, {'weight': 0}).get('weight', 1)
        G.add_edge(com1, com2, **{'weight': w_prec + edge_weight})

    return G


def modularity(
        partition: Mapping[int, int],
        community_degrees: Mapping[int, float],
        community_in_degrees: Mapping[int, float],
        degree_sum: float) -> float:
    """
    Compute the modularity of a partition of a graph
    """

    m = degree_sum / 2
    norm = 1 / m
    res = 0.
    for com in set(partition.values()):
        res += norm * (community_in_degrees[com] -
                       community_degrees[com] * community_degrees[com] / (2*m))
    return res


def best_partition(graph, min_mod_growth: float = 0.0000001) -> Mapping[int, int]:
    """
    Compute the partition of the graph nodes which maximises the modularity.
    """

    partitions = []
    count_edges = graph.number_of_edges()
    partition, community_degrees, community_in_degrees, improvement = _one_level(
        graph, count_edges, False)
    if not improvement:
        return partition
    mod = modularity(partition, community_degrees,
                     community_in_degrees, 2 * count_edges)

    partitions.insert(0, partition)
    graph = _gen_graph(graph, partition)

    while improvement:
        edges_weight_sum = graph.size(weight='weight')
        partition, community_degrees, community_in_degrees, improvement = _one_level(
            graph, edges_weight_sum, True)
        new_mod = modularity(partition, community_degrees,
                             community_in_degrees, 2 * edges_weight_sum)

        if not improvement or new_mod - mod <= min_mod_growth:
            break

        mod = new_mod
        partitions.insert(0, partition)
        graph = _gen_graph_from_weighted(graph, partition)

    partition = partitions[0]
    for index in range(1, len(partitions)):
        for node, com in partitions[index].items():
            partitions[index][node] = partition[com]
        partition = partitions[index]

    return partition
