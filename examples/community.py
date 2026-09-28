"""Runs Louvain community detection over a graph held in a UStore collection.

Mirrors Zachary's karate club into a graph collection through the NetworkX-compatible view, then
partitions it twice: once over the edges read back out of the database, and once over the in-memory
copy.
Both counts are printed so a reader can see how closely a graph living in the database recovers the
community structure of the one in memory, and the drawing is colored by the partition of the stored
graph.
Louvain is sensitive to the order the edges arrive in, so the two counts need not match exactly.

Needs `networkx`, `matplotlib` and the `ustore` bindings.
Run with `python examples/community.py`.
"""

import matplotlib
import matplotlib.pyplot as plt
import networkx as nx

import ustore
from ustore.graph_view import Graph

GRAPH_COLLECTION = "karate"
RANDOM_SEED = 42
NODE_SIZE = 40
EDGE_ALPHA = 0.5


def mirror_karate_club(stored: Graph) -> nx.Graph:
    """Copies the karate club edges into the UStore graph collection and returns the in-memory source."""
    karate_club = nx.karate_club_graph()
    sources, targets = zip(*karate_club.edges, strict=True)
    stored.add_edges_from_arrays(sources, targets)
    return karate_club


def recover(stored: Graph) -> nx.Graph:
    """Reads every stored edge back into an in-memory graph, which is what Louvain partitions."""
    recovered = nx.Graph()
    recovered.add_nodes_from(stored.nodes)
    recovered.add_edges_from(stored.edges())
    return recovered


def partition(graph: nx.Graph) -> dict[int, int]:
    """Partitions a graph, flattening the returned community sets into a node-to-index map."""
    communities = nx.community.louvain_communities(graph, seed=RANDOM_SEED)
    return {node: index for index, community in enumerate(communities) for node in community}


def count_communities(partitioned: dict[int, int]) -> int:
    """The number of distinct communities a node-to-index partition describes."""
    return len(set(partitioned.values()))


def draw(karate_club: nx.Graph, partitioned: dict[int, int]) -> None:
    """Draws the club with each node colored by its community, seeded so repeated runs look alike."""
    positions = nx.spring_layout(karate_club, seed=RANDOM_SEED)
    nodes = list(karate_club.nodes)
    colormap = matplotlib.colormaps["viridis"].resampled(count_communities(partitioned))
    nx.draw_networkx_nodes(
        karate_club,
        positions,
        nodes,
        node_size=NODE_SIZE,
        cmap=colormap,
        node_color=[partitioned[node] for node in nodes],
    )
    nx.draw_networkx_edges(karate_club, positions, alpha=EDGE_ALPHA)
    plt.show()


def main() -> None:
    """Loads the club into the database, partitions it both ways and draws the result."""
    with ustore.Database.open_ram() as database:
        stored = Graph(database, GRAPH_COLLECTION)
        karate_club = mirror_karate_club(stored)

        stored_partition = partition(recover(stored))
        memory_partition = partition(karate_club)
        print(
            f"The stored graph holds {stored.number_of_nodes()} vertices and "
            f"{stored.number_of_edges()} edges in {count_communities(stored_partition)} communities, "
            f"the in-memory one in {count_communities(memory_partition)}"
        )

        draw(karate_club, stored_partition)


if __name__ == "__main__":
    main()
