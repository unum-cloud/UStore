# UStore Python SDK

Current implementation relies on [PyBind11](https://github.com/pybind/pybind11).
It's feature-rich, but not the most performant, supporting:

- Named Collections
- ACID Transactions
- Single & Batch Operations
- Tensors support via [Buffer Protocol](https://docs.python.org/3/c-api/buffer.html)
- [NetworkX][networkx]-like interface for Graphs
- [Pandas][pandas]-like interface for Document collections
- [FAISS][faiss]-like interface for Vector collections

Using it can be as easy as:

```python
import ustore.ucset as ustore
# import ustore.udisk as ustore
# import ustore.leveldb as ustore
# import ustore.rocksdb as ustore

db = ustore.DataBase()
main_collection = db.main
archive_collection = db['archive']

main_collection[42] = 'purpose of life'.encode()
archive_collection[0] = db[42]
del main_collection[42]
assert len(archive_collection[0]) == 15
```

All familiar Pythonic stuff!

## Operators vs Methods

Every operator call ("dunder" method) maps to a certain method.
Latter are more flexible, as they allow additional arguments.

```python
main_collection[42] = binary_string
main_collection.set(42, binary_string)
```

Similarly, if you want to erase values:

```python
del main_collection[42]
main_collection.pop(42)
```

Look them up:

```python
main_collection[42]
main_collection.get(42)
```

Or check existence:

```python
42 in main_collection
main_collection.has_key(42)
```

As you may have noticed, all function names are identical to `dict` methods.
Even those which were removed in Python 3.
Resemblance doesn't stop there.
All of these functions are valid:

```python
len(archive_collection)
iter(archive_collection)
archive_collection.keys
archive_collection.items
archive_collection.clear()
archive_collection.remove()
```

Those methods seem self-explanatory.
One thing to note, the `.main` collection can't be removed, only cleared.

## Transactions

Similar to Python ORM tools, transactions scope can be controlled manually, or with context managers.

```python
with ustore.Transaction(db) as txn:
    txn.main[42] = binary_string # Not the same as `db.main[42]`
```

You can configure teh transaction behavior with additional arguments.

```python
txn = ustore.Transaction(db, begin=True, watch=True, flush_writes=False, snapshot=False)
```

Transactions that conflict with each other, will fail, raising an exception.
For a more fine-grained control over snapshots and consistency of updates and reads - refer to this manual.

## Batch Operations

If you want to update or read multiple entries at once, you need to pack multiple keys and multiple values into a container.
Most Python API, like Pandas, expect arguments to be Python lists.
We support those, but recommend using `tuple`-s instead, as it minimizes the number of memory allocations and accelerates traversals.
Following lines will produce identical behavior:

```python
main_collection[[42, 43, 44]] = ['a'.encode(), 'b'.encode(), 'c'.encode()]
main_collection[(42, 43, 44)] = ('a'.encode(), 'b'.encode(), 'c'.encode())
main_collection[[42, 43, 44]] = ('a'.encode(), 'b'.encode(), 'c'.encode())
main_collection[(42, 43, 44)] = ['a'.encode(), 'b'.encode(), 'c'.encode()]
```

Aside from native Python classes, we also support NumPy Array.
Best of all, we support PyArrow representations.

```python
import pyarrow as pa
keys = pa.array([1000, 2000], type=pa.int64())
strings: pa.StringArray = pa.array(['some', 'text'])
main_collection[keys] = strings
```

If you are exchanging representations like this between UStore and any other runtime, we will entirely avoid copying data.
This method is recommended for higher performance.

## Converting Collections

You can convert a collection handle into a structured binding:

```python
main_collection.graph # for NetworkX-like feel
main_collection.docs # for `dict`-like values, that we map into JSONs
main_collection.table # for Pandas experience when working with docs
main_collection.vectors # for high-dimensional vector values and k-ANN
main_collection.paths # for string keys
```

### Graphs: NetworkX

With over 10'000 stars on GitHub NetworkX might just be the most popular network-science package across all languages.
We adapt part of its interface for labeled directed graphs to preserve familiar look.
But if NetworkX hardly scales beyond a thousand nodes, we aim to support persistent memory Graphs, that can span tens of Terabytes.

```python
import numpy as np

# Let's build a random sparse graph:
g = db.main.graph
node_ids = np.arange(1_000_000)
source_ids = np.random.choice(node_ids, 10_000_000)
target_ids = np.random.choice(node_ids, 10_000_000)
g.add_edges_from(source_ids, target_ids)

# Let's export an adjacency matrix for a small subset of nodes:
subset_size = 1_000
subset_ids = np.random.choice(node_ids, subset_size)
adjacency_matrix = np.zeros((subset_size, subset_size), dtype=bool)
for i, subset_id in enumerate(subset_ids):
    neighbors_ids = g[subset_id]
    for j, subset_id in enumerate(subset_ids):
        adjacency_matrix[i, j] = subset_id in neighbors_ids
```

This allows you to very large graphs in parts, that fit into memory.
Need vertex degrees to compute the normalized **Laplacian**?

```python
normalizer = np.diag(np.power(adjacency_matrix.sum(axis=0), -0.5))
laplacian_matrix = np.identity(subset_size) - normalizer @ adjacency_matrix @ normalizer
```

Want to **Page-Rank**?

```python
d = 0.85
tolerance = 1e-2
max_iterations = 100
weight = adjacency_matrix / adjacency_matrix.sum(axis=0)
page_rank = np.ones(subset_size).reshape(subset_size, 1) * 1. / subset_size

for _ in range(max_iterations):
    old_page_rank = page_rank[:]
    page_rank = d * weight @ page_rank + (1 - d) / subset_size
    shift = np.absolute(page_rank - old_page_rank).sum()
    if shift < tolerance:
        break
```

Want to build a **Knowledge Graph** using a 1000 "worker" processes reasoning on the same graph representation, computing different metrics and performing updates?
You can't do that in NetworkX, but you can in UStore!

```python
import ustore.flight_client as remote

db = remote.DataBase('remote.server.ip.address:38709')
controversial_articles_category: int = ...
is_controversial = lambda text: 'scandal' in text

with ustore.Transaction(db) as txn:
    descriptions_collection = txn['descriptions']
    articles_ids = descriptions_collection.sample(100)
    articles = descriptions_collection[articles_ids]
    controversial_ids = [article_id for article_id, article in zip(articles_ids, articles) if is_controversial(article)]

    g = txn['categories'].graph
    g.add_edges_from(controversial_ids, [controversial_articles_category] * len(controversial_ids))
```

We also support Attributed Graphs.
You only need to specify the names of document collections, where the attributes will be stored or pulled from:

```python
g = ustore.Network(db, 'graph', 'node_attributes', 'edge_attributes')
source_ids = np.arange(100)
target_ids = np.arange(1, 101)
edge_ids = np.arange(100)
g.add_edges_from(source_ids, target_ids, edge_ids, weight=2)
assert g.degree(
    [100, 1, 2, 0], 
    weight='weight',
) == [(100, 2), (1, 4), (2, 4), (0, 2)]
```

---

Primary functions:

- `.order()`, `.number_of_nodes()`, `len`: to estimate size.
- `.nodes()`, `.has_node()`, `in`, `nbunch_iter()`: to work with nodes.
- `.edges()`, `.in_edges()`, `.out_edges()`: to iterate through edges.
- `.has_edge()`, `.get_edge_data()`: to check for specific edges.
- `.degree[]`, `.in_degree[]`, `.out_degree[]`: to get node degrees.
- `.__getitem__()`, `.successors()`, `.predecessors()`: to traverse the graph.
- `.add_edge()`, `.add_edges_from()`: to add edges.
- `.remove_edge()`, `.remove_edges_from()`: to remove edges.
- `.clear_edges()`, `.clear()`: to clear the graph.

Our next milestones for Graphs are:

- attributes in vertices and edges,
- easier subgraph extraction methods: `.subgraph()`, `.edge_subgraph()`,
- imports and exports from files: `.write_adjlist()`,
- integrating [CuGraph][cugraph] for GPU acceleration.

### Tables: Pandas

The original Pandas interface has over a hundred functions.
We don't yet support the full range, but the implemented ones include:

- `[]` to select a subset of columns.
- `.loc[]` to select a subset or a subrange of rows.
- `.head()` to take the first few rows.
- `.tail()` to take the last few rows.
- `.update()` to in-place join another table.

Once you have selected your range:

- `.astype()`: to cast the contents.
- `.df` to materialize the view.
- `.to_arrow()`: to export into Arrow Table.

From there, its a piece of cake.
Pass it to Pandas, Modin, Arrow, Spark, CuDF, Dask, Ray or any other package of your choosing.
Let's try those together to window-scan a flexible-schema collection, sampling some fields into PyArrow `DataFrame`:

```python
TODO:
```

> TODO: [Comprehensive overview of tabular processing tools in Python](https://unum.cloud/post/).

We are now bridging UStore with [CuDF][cudf] for GPU acceleration.

### Vectors: FAISS

For k-Approximate Nearest Neighbors Search, we have separate class of collections, that look like FAISS, the most commonly used k-ANN library today.
Using it is trivial:

```python
import numpy as np
keys = np.mat()
vectors = np.mat()

main_collection.add(keys, vectors)
results = main_collection.query(keys[0])
```

## Sampling

For Machine Learning applications we provide DBMS-level samplers.
This means, that your `DataLoader` doesn't need to fetch all the data into memory, shuffle it and split into batches.
We can prepare batches for you on the fly.

```python
keys_batch = main_collection.sample(1_000)
values_batch = main_collection[keys_batch]
```

For Tabular ML it may look like this:

```python
rows_batch = main_collection.sample(1_000)
values_batch = main_collection.docs.table[['name', 'age']].loc[rows_batch]
```

For ML on Graphs it may look like this:

```python
vertices_batch = main_collection.sample(1_000)
subgraphs_batch = [main_collection.subgraph(v).matrix() for v in vertices_batch]
```

## Performance

A number of faster alternatives to PyBind11 exist.
In the future bindings might be reimplemented with the slimmer NanoBind or natively as CPython modules.
We are not considering Swig or Boost.Python.

[networkx]: https://networkx.org
[pandas]: https://pandas.pydata.org
[faiss]: https://faiss.org
[cudf]: https://github.com/rapidsai/cudf
[cugraph]: https://github.com/rapidsai/cugraph
