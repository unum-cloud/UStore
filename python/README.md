# Python SDK

The bindings are pure CPython in C11, sitting directly on the C ABI in `c/ustore/`, with no pybind11, NanoBind or SWIG.
The native core is one extension, `ustore.lib`, and the `ustore` package re-exports it.
Every verb is a batch, reached identically through a `Database`, a `Transaction` and a `Snapshot`, which reads only, so a single operation is a batch of one.

## Installation

```sh
git submodule update --init --recursive
pip install .
python -c "import ustore; print(ustore.__version__, ustore.version(), ustore.engines_compiled())"
```

`__version__` names the package, pre-release included, while `ustore.version()` answers `ustore_version_name`, the version of the library the module runs against.

`pyproject.toml` builds through `scikit-build-core`, which drives the project's own CMake tree, so there is no `setup.py`.
The wheel compiles the RAM, RocksDB and LevelDB engines, and leaves NVMe out until its sources build again.
An engine is left out through the standard `CMAKE_ARGS` or through scikit-build config settings, never through variables of UStore's own:

```sh
CMAKE_ARGS="-DUSTORE_WITH_ROCKSDB=OFF -DUSTORE_WITH_LEVELDB=OFF" pip install .
pip install . --config-settings=cmake.define.USTORE_WITH_LEVELDB=OFF
```

`cibuildwheel` builds wheels for CPython 3.12, 3.13 and 3.14, and for the free-threaded 3.14t, where importing `ustore` keeps the GIL disabled.
`engines_compiled()` names the engines a build carries, each opened through its own class method: `Database.open_ram()`, `open_rocksdb()`, `open_leveldb()` or `open_nvme()`, and `view_rocksdb()` for a read-only RocksDB database.
`open_ram(directory)` persists the database as one image in that directory, while `open_ram()` keeps it in memory alone.

## Quickstart

Every verb whose outputs scale with the call takes a `ustore.Arena` as its last positional argument and answers results viewing it:

```python
import numpy as np
import ustore
from ustore.graph_view import DiGraph
from ustore.table_view import Table

with ustore.Database.open_ram() as database:
    arena = ustore.Arena()                              # host pages for calls on the calling thread
    blobs = database.collection_create("blobs", modality=ustore.Modality.BLOBS)  # none exists until created
    database.write([1, 2], [b"a", b"bb"], collection=blobs)
    values = database.read([1, 2], arena, collection=blobs)  # Values, indexed as memoryviews over the arena
    database.scan_batch([1, 100], 16, arena, collection=blobs)  # Keys, rewinding the arena: `values` goes stale
    with database.cursor(collection=blobs) as cursor:   # a walk over one collection, page by page
        while page := cursor.next(1024, arena, values=True):
            list(page), page.values                     # UUID keys and their Values, in one call per page

    people = database.collection_create("people", modality=ustore.Modality.DOCS)
    database.docs_write([10, 11], ['{"name": "Lex", "tweets": 2221}', '{"name": "Joe"}'], collection=people)
    database.docs_gist([10, 11], arena, collection=people)  # ("/name", "/tweets")
    types = [ustore.FieldType.STR, ustore.FieldType.I64]
    columns = database.docs_gather([10, 11], ["name", "tweets"], types, arena, collection=people)
    columns[1], columns.validities(1)                   # memoryview of "q", bitmap bytes
    prices = np.array([1.5, 2.5], dtype=np.float64)
    database.docs_write([10, 11], prices, field="price", type=ustore.FieldType.F64, collection=people)

    follows = database.collection_create("follows", modality=ustore.Modality.GRAPH)
    database.graph_write_edges(np.array([1, 2]), np.array([2, 3]), collection=follows)
    neighborhoods = database.graph_read_edges([1, 2, 3], arena, role=ustore.Role.ANY, collection=follows)
    np.asarray(neighborhoods)                           # (edges, 3, 16) bytes of source, target, edge

    database.get(1, collection=blobs), 1 in database["blobs"].blobs  # one-off arena copies: b"a", True
    graph = DiGraph(database, "graph", nodes="nodes", edges="edges")
    graph.add_edge(1, 2, weight=3)
    list(graph.edges(data=True))                        # [(UUID(int=1), UUID(int=2), {"weight": 3})]
    Table(database, people).astype({"tweets": "int64"}).to_pandas(page=1 << 16)
```

A document or a vertex is a value like any other, so documents, graphs and binary values each keep collections of their own.

## Receivers & Deferred Calls

A receiver carries what every argument struct of the C ABI puts before its inputs: the view, the executor and the completion.
A `Database`, `Transaction` or `Snapshot` runs its calls on the calling thread, `on(executor)` answers a `ustore.View` whose calls block on that executor, and `deferred(executor)` answers a `ustore.DeferredView` whose calls submit there and answer a `ustore.Deferred`.
The receiver's type alone chooses the completion, so all three offer the same verbs with the same arguments, and receivers never change after they are made, so threads share them freely.
Constructors address no database yet, so no receiver offers them: `open_ram`, `open_rocksdb`, `view_rocksdb` and `open_leveldb` take an `executor=` keyword and block on it, as they block on the calling thread without one.
In a coroutine, `await` a deferred call; anywhere else, `wait()` blocks the thread with its state detached:

```python
import asyncio


async def main(directory, keys, key, payloads):
    with ustore.Executor.for_ram(8, deferred_limit=256) as executor:
        database = ustore.Database.open_rocksdb(directory, executor=executor)  # blocks on the executor
        arena = ustore.Arena(executor)                       # placed where the executor computes
        values = database.on(executor).read(keys, arena, collection="movies")  # blocking, after earlier submissions
        poster = database.on(executor)["movies"].blobs[key]  # shorthand, copied out of a one-off arena

        later = database.deferred(executor)
        reading = later.read(keys, arena, collection="movies")  # ustore.Deferred, submitted now, holding the arena
        fetching = later["movies"].blobs[key]                # a Deferred owning its one-off arena
        reading.status                                       # ustore.Status.DEFERRED until final
        values = await reading                               # or: reading.wait()
        poster = await fetching                              # a missing key raises KeyError

        transaction = database.transaction(ustore.Watch.KEYS)
        await transaction.deferred(executor).write(keys, payloads, collection="movies")
        peek = transaction.unwatched().read(keys, arena, collection="movies")  # recorded for no conflict
        read_stamp = transaction.stage()
        commit_stamp = await transaction.deferred(executor).commit(ustore.Durability.FLUSHED)
        transaction.close()

        with database.snapshot() as snapshot:
            old = snapshot.read(keys, arena, collection="movies")
            old_poster = snapshot["movies"].blobs[key]
            snapshot.read_stamp                              # sees every commit stamp at or below it
        executor.drain(ustore.Mood.SPINNING)
        database.close()
```

`await` hands `wait()` to `loop.run_in_executor`, so the thread it blocks belongs to asyncio, and `status` polls through `ustore_outcome_status` without waiting.
Arguments the binding refuses raise at the call, while the engine's refusals raise at `await` or `wait()`, as many times as they are asked.
A collection name resolves on the calling thread when the call is made, and a mapping view creates a missing collection there too, so a deferred call never waits behind earlier ones to learn its collection.
Python's `in` must answer a bool, an assignment and `del` discard what they answer, and the table view and most graph methods chain several calls, so through a deferred view they raise `TypeError` naming the verb to call instead, such as `contains` for `in`.

```python
async def fan_out(database, executor, keys, key, sources, targets):
    later, types = database.deferred(executor), [ustore.FieldType.STR, ustore.FieldType.I64]
    gathering = later.docs_gather(keys, ["name", "tweets"], types, ustore.Arena(executor), collection="people")
    linking = later.graph_write_edges(sources, targets, collection="follows")
    present = later.contains(key, collection="movies")       # Deferred[bool], since `key in ...` must answer a bool
    document = later["people"].docs[key]                     # Deferred of the parsed document
    edges = later["follows"].graph.read_edges(keys)          # Deferred[Neighborhoods] over a one-off arena
    return await gathering, await linking, await present, await document, await edges
```

A `Deferred` keeps the outcome, the inputs the library borrows, the output slots and its arena, since submission copies the argument struct and lets it go.
It holds its arena as a blocking call does, until it is waited on or dropped, and dropping an unfinished one waits for its call.
The collector frees a cycle through a finished one, and leaves a cycle through an unfinished one for a later collection, never clearing what the library still reads.
A transaction serves one call at a time, so a second call while a deferred one runs raises `ContendedError`, as does a submission that finds the executor's deferred table full or an arena serving another call.
Closing a database, transaction or snapshot, or dropping an arena, waits in C for the deferred calls naming it.

## Executors

An executor owns host threads, and a database owns its data, so one executor serves any number of databases.
`Executor.for_ram(threads=0, *, memory_domains=None, deferred_limit=0)` spawns the threads of the RAM, RocksDB and LevelDB engines near the NUMA nodes `memory_domains` names, every node the process may allocate on for `None`.
It keeps the scratch of each thread on that thread's node and every arena it makes on the first node named, and refuses a node no machine carries with `InvalidArgumentError`.
Zero threads take 7/8 of the cores those nodes carry, leaving the rest to blocking calls, which run on their callers' threads, and to the engines' own background flushes and compactions.
A zero `deferred_limit` takes the library's table size:

```python
executor = ustore.Executor.for_ram(8, memory_domains=[0], deferred_limit=256)
executor.drain(ustore.Mood.SPINNING)                    # waits, then keeps idle threads hot
arena = ustore.Arena(executor)                          # outputs of calls on this executor land here
```

`Executor.on_pool(pool, /, *, memory_domains=None, deferred_limit=0)` borrows a ForkUnion `fu_pool_t` another library already runs, given as the integer address `ctypes` or `cffi` hands over, so one process keeps one set of threads; the pool must outlive the executor.
`Executor.for_nvme(threads=0, *, transport=Transport.AUTOMATIC, vram=None, deferred_limit=0, **sizes)` makes the I/O rings of the NVMe engine, and tells every database `open_nvme` opens on it how to move bytes: the transport, the channels and their buffers, each size keyword naming a field of `ustore_nvme_options_t`.
`Executor.for_vram(devices=None, *, scratch_limit=0, deferred_limit=0)` drives CUDA devices, which an NVMe executor given it as `vram` compacts on, and raises `UnsupportedError` in a build without CUDA.

Calls on one executor start in submission order, and a blocking call runs alone: after every call submitted before it finishes.
A deferred submission that finds the table full finishes with `Status.CONTENDED` and raises `ContendedError` once awaited.
Used as a context manager, an executor waits for its calls and closes on exit, after which a receiver naming it raises `UninitializedError`.
Calling `ustore.Executor()` raises a `TypeError` naming the factories, since what an executor spawns cannot be derived from nothing.

The extension declares `Py_MOD_GIL_NOT_USED`, so free-threaded builds run without the interpreter lock.
Critical sections fence the few fields the binding mutates: an arena's exports and generation, a closed executor's handle, a cursor's handle while a pull holds it, a deferred call's answer, and the identifier a collection resolved.
A ragged input that is not already a tape is borrowed into views the call holds before it is submitted, so the accessor the executor's threads call reads those views alone, never a Python container the caller may change meanwhile.

## Progress & Cancellation

`scan`, `scan_batch`, a cursor's `next`, `compact` and a snapshot's `save` take `progress=`, a callable of the units the call finished and the units it finishes in all.
The units are the verb's own: scans for a scan, one pull for a cursor, bytes of keys and values for a save, and bytes or collections for a compaction, as the engine measures them.
The library calls it from whichever thread runs the call, never two at once and never after the call is final, so a deferred call reports from the executor's threads, and the binding attaches the interpreter there itself, taking the GIL where there is one.
An answer of `None` or a truthy one carries on, and any other falsy answer cancels: the call unwinds, publishes nothing and raises `CancelledError`.
A cancelled save removes what it wrote, a cancelled pull leaves its cursor where it was, and a cancelled compaction stops between collections, keeping what it compacted.
An exception inside the callable cancels the call the same way, and re-raises from it in place of `CancelledError`, or from `wait()` and `await` for a deferred call.

```python
stop = threading.Event()


def report(finished: int, total: int) -> bool:
    print(f"{finished} of {total}")
    return not stop.is_set()


database.compact(progress=report)
with database.snapshot() as snapshot:
    snapshot.save(directory, progress=report)           # reports on the calling thread
    snapshot.deferred(executor).save(elsewhere, progress=report).wait()  # and here from the executor's
```

## Arenas

Every output whose length only the call knows, such as values, keys, columns, edges, degrees, bounds and names, lands in a `ustore.Arena` you pass, and the verb answers a view over it without copying.
`ustore.Arena(executor)` places it where that executor computes, decided once, and `ustore.Arena()` makes host pages for calls on the calling thread.
Each call rewinds its arena, so the previous call's views go stale and a steady loop allocates once.
To keep a result past the next call, or to read one while making the next, pass that call another arena.
An arena serves one call at a time, so each deferred call needs its own.
A NumPy array or `memoryview` over an output pins its arena as a `memoryview` pins a `bytearray`, so the next call naming that arena raises `ContendedError` until the export is released.
A stale result raises `UninitializedError` when indexed, exported or asked for its `lengths`, `counts` or `degrees`.

```python
arena = ustore.Arena()
for batch in batches:
    values = database.read(batch, arena, collection="movies")  # rewinds: the last batch's values are gone
    total += sum(length or 0 for length in values.lengths)
held = database.read(keys, arena, collection="movies")[0]  # a memoryview, pinning the arena
database.read(keys, ustore.Arena(), collection="movies")  # another arena serves the next call meanwhile
held.release()                                          # and this one serves calls again
```

`read` and `docs_read` answer `ustore.Values`, `scan`, `scan_batch`, `sample` and `sample_batch` answer `ustore.Keys`, a cursor's `next` answers a `ustore.Page`, `docs_gather` answers `ustore.Columns`, and `graph_read_edges` answers `ustore.Neighborhoods`.
`measure`, `docs_gist`, `graph_degrees`, `collection_find`, `collection_list` and `collection_names` take an arena too, and copy their small answers out of it.

Shorthand has no place for an arena, so it makes a one-off arena through `ustore_arena_init` for every call, copies what it read into Python objects and frees the arena with the call.
That covers `get`, `contains`, `key in view[name].blobs`, `view[name].blobs[key]`, `.docs[key]` and `.docs_json[key]`, `Collection.sample`, and the NetworkX and Pandas views.
Each such call pays for an arena and a copy, so a loop reaches for the verbs with an arena of its own instead.

## Keys

A key is a `uuid.UUID`, its text, sixteen raw bytes, or a non-negative integer read as `uuid.UUID(int=n)`, so integer keys order as integers do.
Keys, vertices and collection IDs come back as `uuid.UUID`, and a key batch passes without a copy as a contiguous array of 16-byte records, such as NumPy's `V16` or an (n, 16) `uint8` array, which is also how `np.asarray` reads a `Keys` or `Neighborhoods` tape.
A buffer of 64-bit integers converts into keys one by one, and any other sequence converts element by element.

## Errors

Every failure raises a subclass of `ustore.Error`, itself a `RuntimeError`, and each carries the status the C ABI reported as its `status` attribute, a `ustore.Status` member:

```python
transaction = database.transaction(ustore.Watch.KEYS)
try:
    transaction.set(key, value)
    transaction.commit(ustore.Durability.FLUSHED)
except ustore.ConflictError as error:
    assert error.status in (ustore.Status.WRITE_CONFLICT, ustore.Status.READ_CONFLICT)
finally:
    transaction.close()
```

`SUCCESS` and `DEFERRED` raise nothing, and every other member has its own class, which also derives the built-in naming the same fault where one exists:

- `OUT_OF_MEMORY` → `OutOfMemoryError`, a `MemoryError`
- `INVALID_ARGUMENT` → `InvalidArgumentError`, a `ValueError`
- `UNINITIALIZED` → `UninitializedError`
- `UNSUPPORTED` → `UnsupportedError`, a `NotImplementedError`
- `IO` → `InputOutputError`, an `OSError`
- `MALFORMED` → `MalformedError`, a `ValueError`
- `NOT_FOUND` → `NotFoundError`, a `KeyError`
- `ALREADY_EXISTS` → `AlreadyExistsError`, a `ValueError`
- `WRITE_CONFLICT` → `WriteConflictError`, a `ConflictError`
- `READ_CONFLICT` → `ReadConflictError`, a `ConflictError`
- `PHANTOM_CONFLICT` → `PhantomConflictError`, a `ConflictError`
- `CAPACITY_EXHAUSTED` → `CapacityExhaustedError`, an `OverflowError`
- `DEVICE` → `DeviceError`
- `INTERNAL` → `InternalError`
- `CONTENDED` → `ContendedError`
- `CANCELLED` → `CancelledError`
- `READ_ONLY` → `ReadOnlyError`

So `except KeyError` catches a missing collection, and `except ConflictError` is the one clause a retry loop needs for the three conflicts.
A database `view_rocksdb` opened raises `ReadOnlyError` from every call that would change it: writes, removes, transactions, collection creates and drops, clears and compactions.
A failure the binding catches before the boundary raises the class the engine would have raised for the same fault: a closed database, transaction, snapshot or executor raises `UninitializedError`, a view of a collection holding another modality raises `InvalidArgumentError`, and a count of values that disagrees with the count of keys raises `InvalidArgumentError` too.
An argument of the wrong Python type raises `TypeError`, and a snapshot refuses a write with one.

## Binary Values

`write`, `set`, `read`, `get`, `contains` and `remove` address binary values by key, while `scan`, `sample` and `measure` walk them, each in the blobs collection its `collection=` names.
A write takes one bytes-like value per key, one value broadcast to every key, or a tape, and borrows every value for the length of the call rather than copying it:

```python
posters = database.collection_create("posters", modality=ustore.Modality.BLOBS)
database.write(keys, [b"a", b"bb", b"ccc"], collection=posters)  # one value per key, read by an accessor
database.write(keys, b"same", collection=posters)       # one value every key repeats
database.write(keys, pyarrow.array([b"a", b"bb", b"ccc"]), collection=posters)  # an Arrow binary array, a u32 tape
database.write(keys, (data, np.array([0, 1, 3, 6], dtype=np.uint64)), collection=posters)  # bytes and offsets, u64
database.write(other_keys, database.read(keys, arena, collection=posters), collection=posters)  # a read, as a tape
```

A tape is an Arrow `binary`, `large_binary`, `string` or `large_string` array, or a `(data, offsets)` tuple of buffers whose offsets are unsigned 32-bit or 64-bit integers, one more than the values, climbing within the data.
It reaches the C ABI as the u32 or u64 tape it already is, so nothing is copied or gathered, and an Arrow null is refused, since `remove` removes keys.
A read answers the tape the engine wrote, with 32-bit offsets while its bytes fit in 4 GiB and 64-bit ones beyond, and `Values` exports it through the Arrow C data interface as `binary` or `large_binary` to match.

`scan(start, wanted, arena)` lists up to `wanted` keys from `start` upwards.
`scan_batch(starts, wanted, arena)` runs one scan per start in one call, and `sample_batch(count, wanted, arena)` draws `count` samples, all from the one collection `collection=` names, every one held to the same `wanted`, which is what the C ABI takes; `scan` and `sample` remain the single forms.
`measure(start, end, arena)` answers a `Measurement` of the bounds on the entries, the bytes of their values and the space they take, without walking the range.

`cursor(start=0, *, collection)` opens a walk over the keys of one collection, and its `next(wanted, arena, *, values=False)` pulls the next page into the arena that pull names, with the values of its keys in the same call when asked:

```python
arena = ustore.Arena()
with database.cursor(collection="movies") as cursor:   # reads nothing until its first pull
    while page := cursor.next(4096, arena, values=True):
        for key, poster in zip(page, page.values):      # UUIDs and memoryviews over the arena
            consume(key, poster)
```

A pull resumes past the last key the one before it handed out, so a key written behind the walk never comes and one written ahead of it does.
A page shorter than `wanted` exhausts the walk for good: every later pull answers an empty page, which is falsy, whatever is written since.
A key removed at the head between the listing and the read of one pull reads as an empty value, and a pull that fails or is cancelled leaves the cursor where it was.
The cursor holds its database open, and the transaction or snapshot it reads through, until it closes, so a snapshot may close before the walk ends.
Every pull blocks, on the receiver's executor or the calling thread, so a deferred view opens no cursor, and a cursor serves one pull at a time.
`ustore.keys.scan_keys(view, collection, modality=...)` yields every key through the scans of that modality, `scan`, `docs_scan` or `graph_scan_vertices`, and one arena it keeps for the walk, which the NetworkX and Pandas views build on.

## Documents

`docs_write`, `docs_read`, `docs_gist` and `docs_gather` address JSON documents, whole or a field inside each, where a field is a top-level key or a JSON Pointer such as `/user/name`.
A document is JSON text or bytes, a tape of them, or a dict, list or number the binding serializes, and `Collection.docs` takes and answers Python objects outright.
A fixed-width `type` given a contiguous buffer writes a column of scalars, one per key, so a NumPy column of floats or integers reaches the store without being printed as JSON and parsed back.
A gather answers the same shapes in reverse: an `f16` column carries the `e` format, which NumPy reads as `float16` without a copy, and a `uuid` column carries sixteen bytes per document.
`Table.to_pandas` takes a `page`, converting that many rows at a time so only one page of gathered columns is ever live.

## Graphs

`graph_write_edges`, `graph_remove_edges`, `graph_write_vertices` and `graph_remove_vertices` edit a graph collection, while `graph_read_edges` answers `Neighborhoods` and `graph_degrees` the degree of every vertex, with `None` where a vertex is absent.
An edge is a source, a target and an optional identifier, so distinct identifiers let a pair of vertices share several edges, and one without an identifier takes `ustore.edge_id_default`.
`ustore.graph_view.DiGraph` and `Graph` mirror NetworkX over these verbs and over document collections holding the attributes of vertices and edges.

## Collections, Modes & Stamps

Every C enum a caller meets is an `enum.IntEnum` of `ustore`: `Status`, `Mood`, `Watch`, `Durability`, `DropMode`, `ScanMode`, `Modality`, `Isolation`, `Transport`, `Engine`, `Role`, `FieldType` and `Modification`.
The module builds each one at import through the C `_name` functions, valued as the enumerators and named by their canonical spellings in upper case, dashes as underscores, so "strict-serializable" is `Isolation.STRICT_SERIALIZABLE` and "read-only" is `Status.READ_ONLY`.
A verb takes any `int` that is an enumerator, refusing a bool, which would read as zero or one, and text.
`Error.status` and `Deferred.status` are `Status` members, `measure()` answers a `Measurement` struct sequence, `database.capabilities` a `Capabilities` one of `transactions` and `snapshots`, and `Columns.types` holds `FieldType` members.

`describe()` answers a `Description` struct sequence of the `capabilities`, the `engine`, the `isolation`, and the `keys`, `memory_bytes` and `disk_bytes` the engine estimates, zero where it keeps no estimate.
`clear(mode)` empties every collection at once, as `collection_drop` empties one, and `compact()` reclaims the space overwritten and removed data takes, one collection at a time.
A snapshot's `save(path)` writes it out as a new database of the same engine, holding exactly the collections and names the snapshot records:

```python
database.describe().engine                              # ustore.Engine.ROCKSDB
database.clear(ustore.DropMode.PAIRS)                   # every pair goes, the collections and their names stay
database.compact(progress=report)
with database.snapshot() as snapshot:
    snapshot.save(directory)                            # RocksDB and LevelDB create it, RAM writes into it
```

Every other field of the C ABI's argument structs that a caller chooses, such as `collection`, `durability` or `role`, is a keyword, while a verb whose only option is a mode, such as `transaction(watch)`, `commit(durability)`, `drain(mood)` or `collection_drop(collection, mode)`, takes it by position too.
No collection exists until `collection_create(name, modality=...)` makes it to hold one modality, and names starting with `_` are reserved for the system collections, which no listing shows.
Every verb names its collection as a `uuid.UUID` identifier or a name resolved on the calling thread, and `database["movies"]` answers a `Collection` whose `blobs`, `docs`, `graph` and `table` views create it on their first write.

```python
database.set(key, value, collection="movies", durability=ustore.Durability.FLUSHED)
database.collection_drop("movies", ustore.DropMode.PAIRS)
```

Stamps are plain `int`s of two kinds.
`stage()` answers the read stamp, the stamp a transaction reads at and its commit validates against.
`commit()` answers the commit stamp, the commit's position in commit order, which every snapshot whose read stamp is at or above it sees.
`Snapshot.read_stamp` is the stamp a snapshot reads at, so `before.read_stamp < commit_stamp <= after.read_stamp` for snapshots taken around a commit.
Staging reserves no position in commit order, so a read stamp is never greater than the commit stamp that follows it.

## Layout

- `lib.c`: the module glue of `ustore.lib`.
- `lib.h`: declarations every translation unit shares, with the arena and ragged-input rules.
- `types.c`: failures, enums, arenas, ragged inputs and result tapes, executors and deferred calls.
- `db.c`: databases, collections, snapshots, transactions and views.
- `blobs.c`: binary values and their cursors.
- `docs.c`: JSON documents and typed column gathers.
- `graph.c`: graph edges and vertices.
- `strings.c`: collection names, resolved onto identifiers and listed in text order.
- `collection.c`: collections addressed by name, and the mapping views each answers.
- `CMakeLists.txt`: the `ustore_python` target, installed into the `ustore` package.
- `ustore/__init__.py`: the package, building the enums and re-exporting `ustore.lib`.
- `ustore/lib.pyi`: type stubs for the extension.
- `ustore/keys.py`: walks every key of a collection through a cursor.
- `ustore/graph_view.py`: NetworkX-compatible graphs over the graph and document verbs.
- `ustore/table_view.py`: a Pandas table view over document collections.
- `ustore/py.typed`: the PEP 561 marker that ships the stubs.

Each modality joins as one C file beside `lib.c`, with its verbs declared in `lib.h` and stubbed in `ustore/lib.pyi`.
The suites live in `test/` as `<topic>.py` beside the C++ ones, and benchmarks in `bench/`.

## What This Binding Does Not Expose

- The strings and pages verbs, strings cursors among them, which Python and Rust gain together.
- Deferred constructors: an open takes `executor=` and blocks on it, so a database exists once it answers.
- Deferred cursors: every pull blocks, so a deferred view opens none.
- `ustore_status_name` and the other `_name` and `_from_name` functions: the module builds every enum through them at import, so `Status["WRITE_CONFLICT"]` parses a name and no string ever reaches a verb.

## Design

The binding is a thin layer over the C ABI in `c/ustore/`, and every rule below follows from that.

- __Blocking calls stay as simple as the language allows.__
  `db.read(keys, arena)` takes no executor, completion or view argument and returns its result; nothing about asynchrony reaches a caller who never asks for it.
- __The receiver chooses, the verb stays.__
  What every C argument struct puts before its inputs, the view, the executor and the completion, is chosen by the object a verb is called on: `db.on(executor)`, `db.deferred(executor)`, `snapshot`, `txn.unwatched()`.
  A deferred view offers every verb a blocking view does, with the same arguments, and answers a `ustore.Deferred` instead of the result.
  Opening has no database yet, so the constructors take `executor=` and block on it, and the NVMe engine, whose init keeps the executor it is handed, opens on the calling thread alone.
- __Every verb is one C call, so every verb defers.__
  Through `db.deferred(executor)`, these answer a `Deferred`:
  - `read`, `get`, `contains`, `write`, `set`, `remove`, `scan`, `scan_batch`, `sample`, `sample_batch` and `measure`;
  - `docs_read`, `docs_write`, `docs_gist` and `docs_gather`;
  - `graph_read_edges`, `graph_degrees`, `graph_write_edges`, `graph_remove_edges`, `graph_write_vertices` and `graph_remove_vertices`;
  - `collection_find`, `collection_list`, `commit`, `stage` and a snapshot's `save`;
  - `clear`, `compact` and `describe` of a database;
  - `view[name].blobs[key]`, `view[name].docs[key]`, `view[name].docs_json[key]`, `view[name].scan`, `view[name].sample_keys`, and `has_node` and `read_edges` of `view[name].graph`.

  What Python answers itself, or what takes several dependent calls, cannot be one `Deferred`.
  So `in`, an assignment, `del`, `view[name].sample`, `view[name].table` and every other method of `view[name].graph` raise `TypeError` naming the verb to call instead.
  A cursor's pulls each block, so a deferred view opens none.
- __Deferred calls use the language's own idioms.__
  `status` polls through `ustore_outcome_status`, `wait` blocks through `ustore_outcome_wait`, and `await` hands `wait()` to the running event loop's default executor.
- __Ragged inputs keep their layout.__
  An Arrow array or a pair of NumPy buffers passes as the u32 or u64 tape it is, and anything else is borrowed into views the call pins at submission, which the C accessor reads on the executor's threads.
- __Arenas are lent and reused, never hidden.__
  Every output whose length scales with the call lands in an arena the caller passes, and no handle keeps one of its own.
  An exported buffer pins its arena until released, and a view over a rewound one raises `UninitializedError`.
- __Shorthand is slower, verbs reuse an arena.__
  Syntax with no place for an arena makes a one-off arena per call and copies out of it, which costs an allocation and a copy that a verb handed a reused arena never pays.
- __Deferral costs nothing until used.__
  A blocking call allocates no handle and keeps its call on the stack; a deferred call allocates one `Deferred` that owns every input, output slot and arena reference the library reads until the call finishes.
- __The binding has no life of its own.__
  It starts no threads, keeps no queues and counts no deferred calls, transactions, snapshots or cursors.
  The C executor owns every thread, freeing a database, transaction, snapshot or arena waits in C for the deferred calls naming it, a transaction, snapshot or cursor holds its database open in C, so `db.close()` never refuses, and `await` borrows the event loop's own executor.
  A progress callable runs on whichever thread the library reports from, attaching the interpreter there itself.
- __Statuses and modes are enums.__
  `ustore.Status`, `ustore.Mood`, `ustore.Watch`, `ustore.Durability` and the rest mirror the C enumerators value for value, so nothing is ever compared as a string.
- __Language idioms over parallel APIs.__
  Mapping sugar, context managers, the buffer protocol, struct sequences and Arrow export work on every receiver that can honour them.
