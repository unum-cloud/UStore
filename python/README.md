# Python SDK

The bindings are pure CPython in C11, sitting directly on the C ABI in `include/ustore/`, with no pybind11, NanoBind or SWIG.
The native core is one extension, `ustore.lib`, and the `ustore` package re-exports it.

## Installation

```sh
git submodule update --init --recursive
pip install .
python -c "import ustore, ustore.lib; print(ustore.lib.__version__, ustore.lib.engines_compiled())"
```

`pyproject.toml` builds through `scikit-build-core`, which drives the project's own CMake tree, so there is no `setup.py`.
The wheel compiles the SmashTable engine alone by default.
RocksDB and LevelDB are enabled through the standard `CMAKE_ARGS` or through scikit-build config settings, never through variables of UStore's own:

```sh
CMAKE_ARGS="-DUSTORE_USE_ROCKSDB=ON -DUSTORE_USE_LEVELDB=ON" pip install .
pip install . --config-settings=cmake.define.USTORE_USE_ROCKSDB=ON
```

`cibuildwheel` builds wheels for CPython 3.12, 3.13 and 3.14, and for the free-threaded 3.14t, where importing `ustore` keeps the GIL disabled.

## Layout

| Path                   | What it holds                                                             |
| :--------------------- | :------------------------------------------------------------------------ |
| `lib.c`                | The module glue of `ustore.lib`                                           |
| `lib.h`                | Declarations every translation unit shares, with the arena-ownership rule |
| `types.c`              | Failures, arenas, key and value batches, result tapes and their views     |
| `database.c`           | Databases, collections, snapshots and transactions                        |
| `blobs.c`              | Binary values                                                             |
| `docs.c`               | JSON documents and typed column gathers                                   |
| `graph.c`              | Graph edges and vertices                                                  |
| `strings.c`            | Collection names, resolved through the strings modality                   |
| `CMakeLists.txt`       | The `ustore_python` target, installed into the `ustore` package           |
| `ustore/__init__.py`   | The package, re-exporting `ustore.lib`                                    |
| `ustore/lib.pyi`       | Type stubs for the extension                                              |
| `ustore/keys.py`       | Walks every key of a collection page by page                              |
| `ustore/graph_view.py` | NetworkX-compatible graphs over the graph and document verbs              |
| `ustore/table_view.py` | A Pandas table view over document collections                             |
| `ustore/py.typed`      | The PEP 561 marker that ships the stubs                                   |

Each modality joins as one C file beside `lib.c`, with its verbs declared in `lib.h` and stubbed in `ustore/lib.pyi`.
The suites live in `tests/` as `test_*.py` beside the C++ ones, and benchmarks in `bench/`.
The extension links one engine, SmashTable unless `USTORE_PYTHON_ENGINE` names another one the tree builds.

## Interface

Every verb is a batch, reached identically through a `Database` and a `Transaction`, and every result owns the arena it was read into:

```python
import numpy, ustore
from ustore.graph_view import DiGraph
from ustore.table_view import Table

with ustore.Database() as database:
    database.write([1, 2], [b"a", b"bb"])
    values = database.read([1, 2])                      # Values, indexed as memoryviews

    people = database.collection_create("people")
    database.docs_write([10, 11], ['{"name": "Lex", "tweets": 2221}', '{"name": "Joe"}'], collection=people)
    database.docs_gist([10, 11], collection=people)     # ("/name", "/tweets")
    columns = database.docs_gather([10, 11], ["name", "tweets"], ["str", "i64"], collection=people)
    columns[1], columns.validity(1)                     # memoryview of "q", bitmap bytes

    follows = database.collection_create("follows")
    database.graph_upsert_edges(numpy.array([1, 2]), numpy.array([2, 3]), collection=follows)
    edges = database.graph_find_edges([1, 2, 3], role="any", collection=follows)
    numpy.asarray(edges)                                # (edges, 3) of source, target, edge

    graph = DiGraph(database, "graph", nodes="nodes", edges="edges")
    graph.add_edge(1, 2, weight=3)
    list(graph.edges(data=True))                        # [(1, 2, {"weight": 3})]
    Table(database, people).astype({"tweets": "int64"}).to_pandas()
```

A document or a vertex is a value like any other, so documents, graphs and binary values each keep collections of their own.
