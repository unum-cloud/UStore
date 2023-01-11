import random


def best_partition(graph, max_community_size=100, min_modularity_growth=0.0000001):

    if (max_community_size > graph.number_of_nodes()):
        max_community_size = graph.number_of_nodes()

    partition = {}
    degrees = {}
    com_tot_degree = {}
    node_com_degrees = {}
    degreview = graph.degree

    for node in graph.nodes():
        degrees[node] = degreview[node]
        partition[node] = random.randint(0, max_community_size-1)
        node_com_degrees[node] = [0] * max_community_size
    com_tot_degree = degrees.copy()

    modified = True
    while modified:
        for node in node_com_degrees.keys():
            node_com_degrees[node] = [0] * max_community_size

        links = 0
        for node, neighbor in graph.edges():
            links += 1
            node_com_degrees[node][partition[neighbor]] += 1
            node_com_degrees[neighbor][partition[node]] += 1

        modified = False
        for node, neighbor in graph.edges():
            if partition[node] == partition[neighbor]:
                continue

            community_node = partition[node]
            community_neighbor = partition[neighbor]
            deg = degrees[node]
            node_com_tot_degree = com_tot_degree[community_node]
            neighbor_com_tot_degree = com_tot_degree[community_neighbor]
            degree_in_node_com = node_com_degrees[node][community_node]
            degree_in_neighbor_com = node_com_degrees[node][community_neighbor]

            delta_modularity = (1/links) * (degree_in_neighbor_com-degree_in_node_com) - \
                (deg/(2*(links**2))) * \
                (deg + neighbor_com_tot_degree - node_com_tot_degree)

            if delta_modularity > min_modularity_growth:
                com_tot_degree[community_node] -= (deg - degree_in_node_com)
                com_tot_degree[community_neighbor] += (
                    deg - degree_in_neighbor_com)
                partition[node] = partition[neighbor]
                modified = True

    return partition
