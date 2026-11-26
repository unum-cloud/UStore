# UStore for Python

The bindings are pure CPython in C11, sitting directly on the C ABI in `c/ustore/`, with no pybind11, NanoBind or SWIG.
The native core is one extension, `ustore.lib`, which the `ustore` package re-exports, and it needs CPython 3.12 or newer, free-threaded builds included.
Every verb is a batch, so a single operation is a batch of one:

- `Database`, `Transaction` and `Snapshot`: the head, a group of changes, and a point in time, with the same reads.
- `Collection`: a collection by name, whose `blobs`, `docs`, `graph` and `table` views take Python objects.
- `Arena`: where the batch verbs land their outputs, answered as `Values`, `Keys`, `Columns` and `Neighborhoods`.
- `Executor` and `Deferred`: the threads a call runs on, and a call submitted there, which `await` or `wait()` finishes.

## Installation

```sh
pip install ustore
python -c "import ustore; print(ustore.__version__, ustore.version(), ustore.engines_compiled())"
```

From a checkout, `pip install .` builds through `scikit-build-core` and the project's own CMake, and `CMAKE_ARGS="-DUSTORE_WITH_LEVELDB=OFF"` leaves an engine out.
The wheels hold the RAM, RocksDB and LevelDB engines for CPython 3.12 through 3.15 and the free-threaded 3.14t and 3.15t.
The suite runs from the repository root:

```sh
pip install --group test . && python -X faulthandler -m pytest -x
```

## Quickstart

The program opens a database in memory, writes a blob, a document and an edge in one transaction, and reads them back.
The mapping views copy each answer out of a one-off arena, while the batch verbs, such as `read`, answer views into an arena they are handed.

```python
import ustore

with ustore.Database.open_ram() as database:
    database.collection_create("posters", modality=ustore.Modality.BLOBS)  # brackets only find
    database.collection_create("movies", modality=ustore.Modality.DOCS)
    database.collection_create("similar", modality=ustore.Modality.GRAPH)
    with database.transaction() as transaction:  # a clean exit commits all three writes, or none
        transaction["posters"].blobs[1] = b"\x89PNG"
        transaction["movies"].docs[1] = {"title": "Avatar", "year": 2009}
        transaction["similar"].graph.add_edge(1, 2)

    assert database["posters"].blobs[1] == b"\x89PNG"
    assert database["movies"].docs[1] == {"title": "Avatar", "year": 2009}
    assert database["similar"].graph.has_edge(1, 2)

    arena = ustore.Arena()  # batch verbs answer views into an arena each call reuses
    posters = database.read([1, 2], arena, collection="posters")
    assert bytes(posters[0]) == b"\x89PNG" and posters[1] is None
    print(database.describe().engine.name)
```

## Collections

No collection exists until `collection_create(name, modality=...)` makes it to hold one modality.
Every verb names its collection through `collection=`, as a `uuid.UUID` identifier or as a name resolved on the calling thread when the call is made.
`database["movies"]` answers a `Collection`, whose `blobs`, `docs`, `graph` and `table` views each address one modality.
Brackets only find, so every call through a missing name raises `NotFoundError`, and a view of another modality raises `InvalidArgumentError`.
Names starting with `_` are reserved for the system collections, which no listing shows.

```python
posters = database.collection_create("posters", modality=ustore.Modality.BLOBS)
arena = ustore.Arena()
assert database.collection_find("posters", arena) == posters
database.collection_names(arena, after="p", limit=16)   # ("posters",)
database.collection_list(arena)                         # every identifier
database.collection_drop("posters", ustore.DropMode.PAIRS)  # empties it, keeping its name
```

`collection_drop` with `DropMode.COLLECTION`, the default, forgets the name before the collection goes.

## Reads and Writes

`write`, `set`, `read`, `get`, `contains` and `remove` address binary values by key.
A write takes one bytes-like value per key, one value broadcast to every key, or a tape, and views every value for the length of the call rather than copying it.
A tape is an Arrow `binary`, `large_binary`, `string` or `large_string` array, or a `(data, offsets)` tuple of buffers, and reaches the C ABI as the tape it already is.
A read answers the tape the engine wrote, with `None` for a missing key.
`read(keys, arena, offset=16, limit=64)` reads a slice of every value, and `remove(keys, ends=ends)` removes ranges in one atomic call.

```python
keys = [1, 2, 3]
database.write(keys, [b"a", b"bb", b"ccc"], collection=posters)  # one value per key
database.write(keys, b"same", collection=posters)       # one value every key repeats
database.write(keys, pyarrow.array([b"a", b"bb", b"ccc"]), collection=posters)  # an Arrow array, as a u32 tape
values = database.read(keys, arena, collection=posters)  # memoryviews over the arena
database.write([4, 5, 6], values, collection=posters)   # a read writes back as the tape it is
database.set(7, b"one", collection=posters, durability=ustore.Durability.FLUSHED)
database.remove([7], collection=posters)
```

## Scans

`scan(start, wanted, arena)` lists up to `wanted` keys from `start` upwards, `end=` stops it before a key, and `direction=ScanDirection.DESCENDING` walks down instead.
`scan_batch` runs one scan per start in one call, `sample` and `sample_batch` draw keys at random, and `measure(start, end, arena)` answers a `Measurement` of bounds on the entries, value bytes and space in a range, without walking it.
`docs_scan`, `graph_scan_vertices` and `pages_scan` list the other modalities, with `_sample` and `_measure` beside them.
`cursor(start, collection=...)` opens a walk whose `next(wanted, arena, values=True)` pulls a page with its values, resuming past the last key it handed out, until a short page ends it for good.
The cursor keeps no executor: `next` runs on the calling thread, and `cursor.on(executor).next(...)` blocks one pull on an executor.
`docs_cursor(fields=..., columns_types=...)` pulls lists of `(Page, Columns)` batches, one per executor task that found rows.

```python
from ustore import ScanDirection

database.scan(10, 8, arena, collection=posters, end=30)                          # keys in [10, 30)
database.scan(None, 8, arena, collection=posters, direction=ScanDirection.DESCENDING)  # the 8 largest keys
with database.cursor(collection=posters) as cursor:     # reads nothing until its first pull
    while page := cursor.next(4096, arena, values=True):
        for key, poster in zip(page, page.values):       # UUIDs and memoryviews over the arena
            consume(key, poster)
```

`changes_cursor(since, collection=...)` opens a feed of the changes committed after a commit stamp, oldest first, whose pulls answer `Changes` of keys, values, stamps and `ChangeKind` members.

### Predicates

Scans and cursors take `predicate=`, which keeps the listed keys it admits.
A plain callable is asked about one `uuid.UUID` at a time and is cheap, so it runs before patterns or filters read any value.
`ustore.Batch(handler)` is asked once per page about the keys patterns and filters kept, for handlers that pay per call, and `cost=PredicateCost.CHEAP` moves it ahead of them.

```python
database.scan(None, 100, arena, collection=posters, predicate=lambda key: key.int % 2 == 0)


def even(keys, admissions):  # a (count, 16) view of key bytes, and a (count,) view to write 1 into
    for row in range(len(admissions)):
        admissions[row] = keys[row, 15] % 2 == 0


database.scan(None, 100, arena, collection=posters, predicate=ustore.Batch(even))
database.scan(None, 100, arena, collection=posters, predicate=ustore.Batch(even, cost=ustore.PredicateCost.CHEAP))
```

Both views are valid only for the call, and the admissions start as rejections.
An exception inside a predicate goes to `sys.unraisablehook` and rejects the rest of its page.
The library may ask a predicate from several of the executor's threads at once, each entry taking the interpreter, so a handler keeps no state it changes.

### Progress and Cancellation

`scan`, `scan_batch`, a cursor's `next`, `compact` and a snapshot's `save` take `progress=`, a callable of the units finished and the units in all.
The library calls it from whichever thread runs the call, never two at once and never after the call is final, attaching the interpreter there itself.
A falsy answer other than `None` cancels the call, which publishes nothing and raises `CancelledError`, and an exception inside the callable cancels it too and re-raises in its place.

```python
stop = threading.Event()


def report(finished: int, total: int) -> bool:
    print(f"{finished} of {total}")
    return not stop.is_set()


database.compact(progress=report)
```

## Transactions

A `Transaction` groups reads and writes across collections and modalities, and commits all of them or none.
Used as a context manager, it commits on a clean exit and drops the staged work on an exception.
`database.transaction(Watch.KEYS)` records the keys it reads, so its commit raises a `ConflictError` if a concurrent writer touched them, and `transaction.unwatched()` answers a view whose reads record nothing.
`stage()` runs the first phase of a two-phase commit and answers the read stamp, `commit()` answers the commit stamp, `rollback()` withdraws a stage, and `reset()` starts over on the same handle.
Indexing a transaction by name answers a `Collection` whose writes stage there.

```python
transaction = database.transaction(ustore.Watch.KEYS)
transaction.write(keys, [b"a", b"bb", b"ccc"], collection="movies")
peek = transaction.unwatched().read(keys, arena, collection="movies")  # recorded for no conflict
read_stamp = transaction.stage()
commit_stamp = transaction.commit(ustore.Durability.FLUSHED)
transaction.close()
assert read_stamp <= commit_stamp
```

## Snapshots

A `Snapshot` pins the whole database at one point, offers every read a `Database` does, and refuses writes with `TypeError`.
Its `read_stamp` is the stamp it reads at, so `before.read_stamp < commit_stamp <= after.read_stamp` for snapshots taken around a commit.
`save(path)` writes it out as a new database of the same engine, holding exactly the collections and names it records, where RocksDB and LevelDB create the path and the RAM engine writes its image into it.

```python
with database.snapshot() as snapshot:
    old = snapshot.read(keys, arena, collection="movies")
    old_poster = snapshot["movies"].blobs[1]
    assert snapshot.read_stamp >= commit_stamp
    snapshot.save(directory, progress=report)
```

A snapshot holds its database open in C until it closes, so `database.close()` never refuses.

## Documents

`docs_write`, `docs_read`, `docs_gist` and `docs_gather` address JSON documents, whole or a field inside each, where a field is a top-level key or a JSON Pointer such as `/user/name`.
A document is JSON text or bytes, a tape of them, or a dict, list or number the binding serializes, while `Collection.docs` takes and answers Python objects outright.
A fixed-width `type` given a contiguous buffer writes a column of scalars, one per key, so a NumPy column reaches the store without being printed as JSON and parsed back.
A gather answers `Columns`, each a buffer NumPy reads without a copy beside its validity, conversion and collision bitmaps.

```python
people = database.collection_create("people", modality=ustore.Modality.DOCS)
database.docs_write([10, 11], ['{"name": "Lex", "tweets": 2221}', '{"name": "Joe"}'], collection=people)
database.docs_write([10, 11], np.array([1.5, 2.5]), field="price", type=ustore.FieldType.F64, collection=people)
names = database.docs_read([10, 11], arena, field="name", outputs_type=ustore.FieldType.STRING, collection=people)
database.docs_gist([10, 11], arena, collection=people)  # ("/name", "/tweets", "/price")
types = [ustore.FieldType.STRING, ustore.FieldType.I64]
columns = database.docs_gather([10, 11], ["name", "tweets"], types, arena, collection=people)
columns[1], columns.validities(1)                       # memoryview of "q", bitmap bytes
```

`ustore.table_view.Table` reads document collections as a Pandas frame, converting `page` rows at a time.

## Graphs

`graph_write_edges`, `graph_remove_edges`, `graph_write_vertices` and `graph_remove_vertices` edit a graph collection.
`graph_read_edges` answers `Neighborhoods`, one run of source, target and edge triples per vertex, and `graph_degrees` the degree of every vertex, with `None` where a vertex is absent.
An edge is a source, a target and an optional identifier, so distinct identifiers let a pair of vertices share several edges, and one without an identifier takes `ustore.edge_id_default`.
`ustore.graph_view.DiGraph` and `Graph` mirror NetworkX over these verbs and over document collections holding the attributes of vertices and edges.

```python
from ustore.graph_view import DiGraph

follows = database.collection_create("follows", modality=ustore.Modality.GRAPH)
database.graph_write_edges(np.array([1, 2]), np.array([2, 3]), collection=follows)
neighborhoods = database.graph_read_edges([1, 2, 3], arena, role=ustore.Role.ANY, collection=follows)
np.asarray(neighborhoods)                               # (edges, 3, 16) bytes of source, target, edge
database.graph_degrees([1, 2, 3], arena, role=ustore.Role.SOURCE, collection=follows)  # (1, 1, 0)

for name, modality in [("graph", ustore.Modality.GRAPH), ("nodes", ustore.Modality.DOCS), ("edges", ustore.Modality.DOCS)]:
    database.collection_create(name, modality=modality)   # a view only finds collections by name
graph = DiGraph(database, "graph", nodes="nodes", edges="edges")
graph.add_edge(1, 2, weight=3)
list(graph.edges(data=True))                            # [(UUID(int=1), UUID(int=2), {"weight": 3})]
```

## Receivers & Deferred Calls

The receiver a verb is called on chooses where it runs and how it completes, while the verb and its arguments stay the same, as [the C rules](https://github.com/unum-cloud/UStore/blob/main/c/README.md#receivers--deferred-calls) describe in full.
A `Database`, `Transaction` or `Snapshot` runs its calls on the calling thread, `on(executor)` blocks on that executor, and `deferred(executor)` submits there and answers a `ustore.Deferred`.
In a coroutine, `await` a deferred call, which polls its status and re-polls on the next turn of the running loop through `loop.call_soon`, never sleeping and parking no thread; anywhere else, `wait()` blocks the thread with its state detached.
A `Deferred` holds its inputs, output slots and arena until the call finishes, and dropping an unfinished one waits for it.

```python
async def main(database, executor, keys, key):
    arena = ustore.Arena(executor)                       # placed where the executor computes
    values = database.on(executor).read(keys, arena, collection="movies")  # blocking, on the executor
    later = database.deferred(executor)
    reading = later.read(keys, arena, collection="movies")  # submitted at once, holding the arena
    reading.status                                       # ustore.Status.DEFERRED until final
    values = await reading                               # or: reading.wait()
    poster = await later["movies"].blobs[key]            # a missing key raises KeyError
```

Calls on two executors run together, and `asyncio.gather` awaits both:

```python
async def fan_out(database, left, right, keys):
    reading = database.deferred(left).read(keys, ustore.Arena(left), collection="movies")
    scanning = database.deferred(right).scan(None, 100, ustore.Arena(right), collection="movies")
    return await asyncio.gather(reading, scanning)       # both submitted before either is awaited
```

Python's `in`, assignments, `del` and the table view take several calls or must answer at once, so through a deferred view they raise `TypeError` naming the verb to call instead.

## Executors

An executor owns threads, and a database owns its data, so one executor serves any number of databases, one call at a time; a second call while one runs raises `ContendedError` once awaited.
`Executor.for_ram(threads=0, *, memory_domains=None, scratch_limit=0)` spawns threads near the NUMA nodes it names, where zero threads take all but one in eight of their cores, leaving the rest to blocking calls and the engines' background work, and each thread's scratch stops at `scratch_limit` bytes, 16 GiB for zero.
`Executor.for_nvme` makes the I/O rings of the NVMe engine, and `Executor.for_vram(devices=[...])` drives every named GPU of one vendor, one stream each, or every GPU of the first vendor for none.
Used as a context manager, an executor waits for its calls and closes on exit, and progress callbacks on its threads take the GIL as they enter Python.

```python
with ustore.Executor.for_ram(8, memory_domains=[0]) as executor:
    database = ustore.Database.open_rocksdb(directory, executor=executor)  # the open blocks on it
    arena = ustore.Arena(executor)
    values = database.on(executor).read(keys, arena, collection="movies")
    executor.drain(ustore.Mood.SPINNING)                # waits, then keeps idle threads hot
    database.close()
```

[The C rules](https://github.com/unum-cloud/UStore/blob/main/c/README.md#executors) state how executors place their threads, scratch and arenas.

## Batches and Arenas

Every output whose length only the call knows lands in a `ustore.Arena` you pass, and the verb answers a view over it without copying, as [the C rules](https://github.com/unum-cloud/UStore/blob/main/c/README.md#batches-and-arenas) describe.
Each call rewinds its arena, so the previous call's views read whatever it then holds, undetected, and a steady loop allocates once; to keep a result past the next call, pass that call another arena.
A `memoryview` over an output pins the arena until released, and the result types export DLPack too.
A key is a `uuid.UUID`, its text, sixteen raw bytes, or a non-negative integer read as `uuid.UUID(int=n)`, and a batch passes without a copy as a contiguous array of 16-byte records or converts from 64-bit integers.

```python
arena = ustore.Arena()
for batch in batches:
    values = database.read(batch, arena, collection="movies")  # rewinds: the last batch's values are gone
    total += sum(length or 0 for length in values.lengths)
held = database.read(keys, arena, collection="movies")[0]  # a memoryview, pinning the arena
database.read(keys, ustore.Arena(), collection="movies")  # another arena serves the next call meanwhile
held.release()                                          # and this one serves calls again
```

Shorthand such as `get`, `contains` and `view[name].blobs[key]` makes a one-off arena per call and copies out of it, so a loop reaches for the verbs with an arena of its own.

## Errors

Every failure raises a subclass of `ustore.Error`, itself a `RuntimeError`, holding the status the C ABI reported as its `status` attribute, a `ustore.Status` member.
Each status has its own class, which also derives the built-in naming the same fault: `NotFoundError` is a `KeyError`, `InvalidArgumentError` a `ValueError`, `UnsupportedError` a `NotImplementedError`, `InputOutputError` an `OSError`, and `CapacityExhaustedError` an `OverflowError`.
`WriteConflictError`, `ReadConflictError` and `PhantomConflictError` share `ConflictError`, the one clause a retry loop needs.
A closed handle raises `UninitializedError`, and an argument of the wrong Python type raises `TypeError`.

```python
transaction = database.transaction(ustore.Watch.KEYS)
try:
    transaction.set(key, value, collection="movies")
    transaction.commit(ustore.Durability.FLUSHED)
except ustore.ConflictError as error:
    assert error.status in (ustore.Status.WRITE_CONFLICT, ustore.Status.READ_CONFLICT, ustore.Status.PHANTOM_CONFLICT)
finally:
    transaction.close()
```

Every C enum is an `enum.IntEnum` built at import through the C `_name` functions, so "strict-serializable" is `Isolation.STRICT_SERIALIZABLE`.

## Apache Arrow

Outputs speak the Arrow PyCapsule protocol, exporting in place until their arena rewinds, and `import_arrow` and `ingest_arrow` read any object that speaks it back into a collection:

```python
values = database.read([1, 2, 3], arena, collection=posters)
pyarrow.array(values)  # `binary`, null where a key is missing
types = [ustore.FieldType.STRING, ustore.FieldType.I64]
columns = database.docs_gather([10, 11], ["name", "tweets"], types, arena, collection=people)
pyarrow.array(columns).field("name")  # ["Lex", "Joe"], one struct array of the columns

table = pyarrow.table(database["posters"])  # the whole collection as a stream, keys as `arrow.uuid`
database.import_arrow(table, collection="copies")  # every batch's columns read in place
database.ingest_arrow(table, collection="copies", order="sorted")  # or landed as one commit
```

`Values.__arrow_c_array__` exports a read as `binary` or `large_binary`, `Columns.__arrow_c_array__` a gather through `ustore_docs_column_view_to_arrow`, and `Collection.__arrow_c_stream__` the collection through `ustore_keys_view_to_arrow` and `ustore_strings_view_to_arrow`.
`import_arrow` and `ingest_arrow` read keys and strings through `ustore_keys_view_from_arrow` and `ustore_strings_view_from_arrow`, from an object answering `__arrow_c_stream__` or `__arrow_c_array__`.

## Platform Notes

### Engines

`engines_compiled()` names the engines a build holds, each opened through its own class method: `Database.open_ram()`, `open_rocksdb()`, `open_leveldb()` or `open_nvme()`, and `view_rocksdb()` or `view_nvme()` for a read-only RocksDB or NVMe database, which raises `ReadOnlyError` from every change.
`open_ram(directory)` persists the database as one image there on every `flush()`, since closing writes nothing, while `open_ram()` keeps it in memory alone, and `open_nvme(manifest)` names the database by its manifest or the raw address of its first slot, such as `/dev/nvme0n1@64GiB`.
`open_ram`, `open_rocksdb`, `view_rocksdb` and `open_leveldb` take `memory_limit=`, and the RocksDB and LevelDB opens `background_threads=`, the threads of the pool the database owns for its flushes and compactions.
`open_ram` and `open_nvme` take `cpu_class_ceiling=`, the newest `CPUClass` the database's kernels may run as, where a class the CPU lacks narrows the pick, `UnsupportedError` comes only when no compiled class fits, and `describe().cpu_class` reports the class picked.
`describe()`, `clear(mode)` and `compact()` report on, empty and reclaim every collection at once, and `on(executor).compact(plan=ustore.CompactPlan())` leaves a stopped compaction open for the next call to continue.
The extension declares `Py_MOD_GIL_NOT_USED`, so free-threaded builds run without the interpreter lock, and critical sections fence the few fields the binding mutates.

### Left Unbound

- The strings cursor, which `export_arrow` drives.
- Deferred constructors and cursors: an open takes `executor=` and blocks on it, and every pull blocks, on the calling thread or on the executor `on(executor)` names.
- The `_name` functions themselves, which build the enums at import, so no string ever reaches a verb.

### Design

- __The receiver chooses, the verb stays.__
  `db.read(keys, arena)` takes no executor or completion, and `db.on(executor)`, `db.deferred(executor)`, `snapshot` and `txn.unwatched()` choose where it runs and what it sees.
- __Ragged inputs keep their layout.__
  An Arrow array or a pair of NumPy buffers passes as the tape it is, and anything else is wrapped in views the call pins at submission.
- __Arenas are the caller's, never hidden.__
  No handle keeps an arena of its own, and shorthand pays an allocation and a copy that a verb handed a reused arena never pays.
- __No life of its own.__
  The binding starts no threads and keeps no queues or counts, and `await` polls instead of parking a thread.
