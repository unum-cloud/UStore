"""Louvain community detection in pure Python, over a graph held in UStore and over the same graph in NetworkX.

The algorithm is the one `networkx.community.louvain_communities` runs, written against the calls both
graph types share, so the same code times three ways: NetworkX's own implementation, this baseline over
a `networkx.Graph`, and this baseline over a `ustore.graph_view.Graph` whose every read reaches the store.

Run:
    python bench/louvain.py --dataset data/snap-email/email-Eu-core.txt
    python bench/louvain.py --vertices 4096 --communities 16
"""

import argparse
import time
from collections.abc import Callable, Mapping
from pathlib import Path
from typing import Any

import networkx
import numpy as np
from networkx.algorithms.community import louvain_communities

import ustore
from ustore.graph_view import Graph


def node_affiliation(graph: Any, partition: Mapping[int, int], node: int) -> dict[int, int]:
    """The number of edges from `node` into every community its neighbours belong to, self-loops aside."""
    degrees: dict[int, int] = {}
    for neighbour in graph[node]:
        if neighbour != node:
            degrees[partition[neighbour]] = degrees.get(partition[neighbour], 0) + 1
    return degrees


def node_affiliation_weighted(graph: Any, partition: Mapping[int, int], node: int) -> dict[int, float]:
    """The weight of edges from `node` into every community its neighbours belong to, self-loops aside."""
    degrees: dict[int, float] = {}
    for neighbour, attributes in graph[node].items():
        if neighbour != node:
            degrees[partition[neighbour]] = degrees.get(partition[neighbour], 0) + attributes.get("weight", 1)
    return degrees


def one_level(
    graph: Any, edges_weight_sum: float, is_weighted: bool
) -> tuple[dict[int, int], dict[int, float], dict[int, float], bool]:
    """Moves every node into the neighbouring community that raises modularity most, until none moves."""
    degrees: dict[int, float] = dict(graph.degree(weight="weight") if is_weighted else graph.degree)
    partition = {node: node for node in degrees}
    # The summed degree of every community's members, and the summed degree between them.
    community_degrees = dict(degrees)
    community_inner_degrees = dict.fromkeys(degrees, 0.0)
    affiliation: Callable[[Any, Mapping[int, int], int], Mapping[int, float]] = (
        node_affiliation_weighted if is_weighted else node_affiliation
    )

    improved = False
    moved = 1
    while moved:
        moved = 0
        for node, node_degree in degrees.items():
            node_community = partition[node]
            best_gain = 0.0
            best_community = node_community
            node_community_degree = community_degrees[node_community]

            degrees_into_communities = affiliation(graph, partition, node)
            degree_into_own = degrees_into_communities.get(node_community, 0)

            for neighbour in graph[node]:
                neighbour_community = partition[neighbour]
                if neighbour_community == node_community:
                    continue
                gain = (degrees_into_communities[neighbour_community] - degree_into_own) / edges_weight_sum + (
                    node_degree / (2 * edges_weight_sum**2)
                ) * (node_community_degree - node_degree - community_degrees[neighbour_community])
                if gain > best_gain:
                    best_gain = gain
                    best_community = neighbour_community

            if best_community != node_community:
                community_degrees[node_community] -= node_degree
                community_degrees[best_community] += node_degree
                community_inner_degrees[node_community] -= degree_into_own
                community_inner_degrees[best_community] += degrees_into_communities[best_community]
                partition[node] = best_community
                moved += 1
                improved = True

            # A self-loop adds to the inner degree of whichever community the node ends up in.
            if is_weighted:
                loop = graph.get_edge_data(node, node)
                community_inner_degrees[best_community] += loop.get("weight", 1) if loop is not None else 0
            else:
                community_inner_degrees[best_community] += 1 if graph.has_edge(node, node) else 0

    return partition, community_degrees, community_inner_degrees, improved


def aggregate(graph: Any, partition: Mapping[int, int], is_weighted: bool) -> networkx.Graph:
    """The graph of communities, whose edge weights sum the edges between their members."""
    aggregated = networkx.Graph()
    aggregated.add_nodes_from(partition.values())
    edges = graph.edges(data=True) if is_weighted else ((source, target, {}) for source, target in graph.edges)
    for source, target, attributes in edges:
        first, second = partition[source], partition[target]
        previous = aggregated.get_edge_data(first, second, {"weight": 0}).get("weight", 1)
        aggregated.add_edge(first, second, weight=previous + attributes.get("weight", 1))
    return aggregated


def modularity(
    partition: Mapping[int, int],
    community_degrees: Mapping[int, float],
    community_inner_degrees: Mapping[int, float],
    degree_sum: float,
) -> float:
    """The modularity of a partition, from the degrees `one_level` accumulated."""
    edges_weight = degree_sum / 2
    return sum(
        (community_inner_degrees[community] - community_degrees[community] ** 2 / (2 * edges_weight)) / edges_weight
        for community in set(partition.values())
    )


def best_partition(graph: Any, min_modularity_growth: float = 1e-7) -> dict[int, int]:
    """The partition of the nodes of `graph` into communities that maximises modularity level by level."""
    edges_count = graph.number_of_edges()
    partition, community_degrees, community_inner_degrees, improved = one_level(graph, edges_count, False)
    if not improved:
        return partition
    current = modularity(partition, community_degrees, community_inner_degrees, 2 * edges_count)

    partitions = [partition]
    level = aggregate(graph, partition, False)
    while improved:
        edges_weight_sum = level.size(weight="weight")
        partition, community_degrees, community_inner_degrees, improved = one_level(level, edges_weight_sum, True)
        grown = modularity(partition, community_degrees, community_inner_degrees, 2 * edges_weight_sum)
        if not improved or grown - current <= min_modularity_growth:
            break
        current = grown
        partitions.append(partition)
        level = aggregate(level, partition, True)

    flattened = partitions[0]
    for coarser in partitions[1:]:
        flattened = {node: coarser[community] for node, community in flattened.items()}
    return flattened


def load_graph(settings: argparse.Namespace) -> networkx.Graph:
    """The edge list at `--dataset`, or a planted-partition graph generated from the other flags."""
    if settings.dataset is not None:
        return networkx.read_edgelist(settings.dataset, nodetype=int)
    size = settings.vertices // settings.communities
    outer_size = size * (settings.communities - 1)
    # Peers draw edges into a vertex too, so `degree` drawn per vertex is a pair probability of `2 * degree / peers`.
    inner_probability = min(1.0, 2 * settings.inner_degree / (size - 1))
    outer_probability = min(1.0, 2 * settings.outer_degree / outer_size) if outer_size else 0.0
    return networkx.planted_partition_graph(
        settings.communities, size, inner_probability, outer_probability, seed=settings.seed
    )


def timed(name: str, run: Callable[[], Any]) -> Any:
    start = time.perf_counter()
    result = run()
    print(f"{name}: {time.perf_counter() - start:.3f} s", flush=True)
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dataset", type=Path, help="SNAP edge list; a planted partition is generated without it")
    parser.add_argument("--engine", choices=ustore.engines_compiled(), default="ram", help="engine to open")
    parser.add_argument("--directory", type=Path, help="directory the UStore graph persists in; in memory otherwise")
    parser.add_argument("--vertices", type=int, default=4096, help="vertices of the generated graph")
    parser.add_argument("--communities", type=int, default=16, help="communities planted in the generated graph")
    parser.add_argument("--inner-degree", type=int, default=8, help="edges each vertex draws inside its community")
    parser.add_argument("--outer-degree", type=int, default=1, help="edges each vertex draws across communities")
    parser.add_argument("--seed", type=int, default=42, help="seed of the generated graph and of NetworkX's Louvain")
    settings = parser.parse_args()

    reference = load_graph(settings)
    print(f"Graph: {reference.number_of_nodes()} vertices, {reference.number_of_edges()} edges", flush=True)

    communities = timed("NetworkX louvain_communities", lambda: louvain_communities(reference, seed=settings.seed))
    print(f"  {len(communities)} communities", flush=True)
    partition = timed("Python baseline over NetworkX", lambda: best_partition(reference))
    print(f"  {len(set(partition.values()))} communities", flush=True)

    with getattr(ustore.Database, f"open_{settings.engine}")(settings.directory) as database:
        edges = np.array(reference.edges, dtype=np.int64).reshape(-1, 2)
        isolated = [node for node, degree in reference.degree if degree == 0]
        stored = Graph(database, "graph")
        graph = stored.graph_collection
        timed(
            "UStore import",
            lambda: database.graph_upsert_edges(edges[:, 0].copy(), edges[:, 1].copy(), collection=graph),
        )
        if isolated:
            database.graph_upsert_vertices(isolated, collection=graph)
        partition = timed("Python baseline over UStore", lambda: best_partition(stored))
        print(f"  {len(set(partition.values()))} communities", flush=True)


if __name__ == "__main__":
    main()
