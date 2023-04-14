
import sys
import networkx as nx
import matplotlib.cm as cm
import matplotlib.pyplot as plt

import ukv.umem as ukv
sys.path.insert(0, 'python/algorithms/')
from louvain import best_partition # autopep8: off

G = nx.karate_club_graph()
db = ukv.DataBase()
main = db.main
graph = main.graph
for v1,v2 in G.edges:
    graph.add_edge(v1,v2)

partition = best_partition(graph)
#or
partition = graph.community_louvain()

# draw the graph
pos = nx.spring_layout(G)
# color the nodes according to their partition
cmap = cm.get_cmap('viridis', max(partition.values()) + 1)
nx.draw_networkx_nodes(G, pos, partition.keys(), node_size=40,
                       cmap=cmap, node_color=list(partition.values()))
nx.draw_networkx_edges(G, pos, alpha=0.5)
plt.show()
