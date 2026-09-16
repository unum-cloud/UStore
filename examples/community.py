"""Runs Louvain community detection over a graph held in a UStore collection.

Mirrors Zachary's karate club into a graph collection, then partitions it twice: once with the
engine's own `community_louvain`, and once with NetworkX over the in-memory copy.
Both counts are printed so a reader can see that a graph living in the database recovers the same
community structure as the one in memory, and the drawing is colored by the engine's partition.

Needs `networkx`, `matplotlib` and the `ustore` bindings.
Run with `python examples/community.py`.
"""

import matplotlib
import matplotlib.pyplot as plt
import networkx as nx
import ustore.smashtable as ustore

RANDOM_SEED = 42
NODE_SIZE = 40
EDGE_ALPHA = 0.5


def mirror_karate_club(graph) -> nx.Graph:
    """Copies the karate club edges into the UStore graph collection and returns the in-memory source."""
    karate_club = nx.karate_club_graph()
    for source, target in karate_club.edges:
        graph.add_edge(source, target)
    return karate_club


def partition_with_networkx(karate_club: nx.Graph) -> dict[int, int]:
    """Partitions the in-memory graph, flattening the returned community sets into a node-to-index map."""
    communities = nx.community.louvain_communities(karate_club, seed=RANDOM_SEED)
    return {node: index for index, community in enumerate(communities) for node in community}


def count_communities(partition: dict[int, int]) -> int:
    """The number of distinct communities a node-to-index partition describes."""
    return len(set(partition.values()))


def draw(karate_club: nx.Graph, partition: dict[int, int]) -> None:
    """Draws the club with each node colored by its community, seeded so repeated runs look alike."""
    positions = nx.spring_layout(karate_club, seed=RANDOM_SEED)
    nodes = list(karate_club.nodes)
    colormap = matplotlib.colormaps["viridis"].resampled(count_communities(partition))
    nx.draw_networkx_nodes(
        karate_club,
        positions,
        nodes,
        node_size=NODE_SIZE,
        cmap=colormap,
        node_color=[partition[node] for node in nodes],
    )
    nx.draw_networkx_edges(karate_club, positions, alpha=EDGE_ALPHA)
    plt.show()


def main() -> None:
    """Loads the club into the database, partitions it both ways and draws the result."""
    database = ustore.DataBase()
    graph = database.main.graph
    karate_club = mirror_karate_club(graph)

    engine_partition = graph.community_louvain()
    networkx_partition = partition_with_networkx(karate_club)
    print(
        f"Engine found {count_communities(engine_partition)} communities, "
        f"NetworkX found {count_communities(networkx_partition)}"
    )

    draw(karate_club, engine_partition)


if __name__ == "__main__":
    main()
