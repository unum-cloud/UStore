# UStore for Mojo

The Mojo package binds the C ABI in `c/ustore/`, opening the engine's shared library with `dlopen` rather than linking it, so a consumer builds with no linker flags of its own.
It needs Mojo 1.2 through pixi, on Linux x86-64, Linux AArch64 or macOS on Apple silicon.
It binds every exported verb, the Arrow exchange included:

- `Library`: the engine, opened once and kept mapped by every handle made through it.
- `Database`, `Transaction` and `Snapshot`: the head, a group of changes, and a point in time, with the same verbs.
- `Executor` and `Deferred`: the threads or rings a call runs on, and a call submitted there.
- `Arena`: where every output whose length scales with the call lands.

## Installation

Let Pixi build the package from source, with no channel to add and no linker flag to pass; the engine arrives as its run dependency, in the environment prefix `mojo build` bakes as runpath.

```toml
[workspace]
preview = ["pixi-build"]

[dependencies]
ustore_mojo = { git = "https://github.com/unum-cloud/UStore", subdirectory = "mojo", tag = "v1.0.0-rc.1" }
```

From a checkout, `pixi run test` builds the engine through the tree's own CMake and runs `mojo/test.mojo` against in-memory databases.

## Quickstart

The program opens a database in memory, writes a blob, a document and an edge in one transaction, and reads them back.
Every verb takes batches as views, and `repeating` makes a view of one element that fits a batch of any size.

```mojo
from ustore import Arena, CollectionsView, Database, DocsReadConfig, Executor, Key, KeysView, Library, Modality, StringsView

def main() raises:
    var library = Library()
    var db = Database.init_ram(Executor(library))
    var posters = CollectionsView.repeating(db.collection_create("posters", Modality.BLOBS))
    var movies = CollectionsView.repeating(db.collection_create("movies", Modality.DOCS))
    var similar = CollectionsView.repeating(db.collection_create("similar", Modality.GRAPH))
    var avatar = Key(1)
    var titanic = Key(2)

    # One transaction writes a blob, a document and an edge, and commits all three or none
    var poster = "PNG".as_bytes()
    var movie = '{"title":"Avatar","year":2009}'.as_bytes()
    var poster_offsets: List[UInt32] = [0, 3]
    var movie_offsets: List[UInt32] = [0, UInt32(len(movie))]
    var transaction = db.transaction()
    transaction.write(posters, KeysView.repeating(avatar), StringsView.of(poster, poster_offsets))
    transaction.docs_write(movies, KeysView.repeating(avatar), StringsView.of(movie, movie_offsets))
    transaction.graph_write_edges(similar, KeysView.repeating(avatar), KeysView.repeating(titanic))
    _ = transaction.commit()

    var arena = Arena(library)  # each read rewinds it, so each answer is read before the next call
    var found = db.read(posters, KeysView.repeating(avatar), arena).present(0)
    var degree = db.graph_read_edges(similar, KeysView.repeating(avatar), arena).degrees()[0]
    var field = "/year".as_bytes()
    var field_offsets: List[UInt32] = [0, 5]
    var config = DocsReadConfig(fields=StringsView.of(field, field_offsets))
    var fields = db.docs_read(movies, KeysView.repeating(avatar), arena, config)
    print(found, String(StringSlice(unsafe_from_utf8=fields.values()[0])), degree)  # True 2009 1
```

## Collections

A database holds no collection until `collection_create` makes one under a name and the `Modality` it holds, answering its `CollectionID`.
Every verb names its collections as a `CollectionsView`, one identifier per task or one repeated for every task, so a batch spans as many collections as it has entries.
`collection_find` resolves a tape of names into identifiers and modalities in one call, `collection_list` exports every collection, and `collection_drop` empties or drops one as its `DropMode` says.

```mojo
var people = db.collection_create("people", Modality.BLOBS)
var names = "people".as_bytes()
var name_offsets: List[UInt32] = [0, 6]
var found = db.collection_find(StringsView.of(names, name_offsets), arena)
print(found.present(0), found.collections()[0] == people, found.modalities()[0] == Modality.BLOBS)
print(len(db.collection_list(arena)))
db.collection_drop(people, DropMode.PAIRS)  # empties it, keeping its name
```

## Reads and Writes

`write` takes keys and a `StringsView` tape of values, Apache Arrow's layout of one more offset than values over the bytes end to end, and a write lands its whole batch or none of it.
`read` answers `Values` in the arena, whose `present`, `lengths` and `values` read presence bits, lengths and the tape, which a later write takes back as it is.
`remove` removes keys, or every key from each up to its end key, in one atomic call, so an empty value is still a value.

```mojo
var keys: List[Key] = [Key(1), Key(2)]
var bytes = "AdaGrace".as_bytes()  # two values end to end
var offsets: List[UInt32] = [0, 3, 8]  # one more offset than values
db.write(CollectionsView.repeating(people), KeysView.of(keys), StringsView.of(bytes, offsets))

var values = db.read(CollectionsView.repeating(people), KeysView.of(keys), arena)
print(values.present(1), Int(values.lengths()[1]), len(values.values()[1]))  # True 5 5
db.remove(CollectionsView.repeating(people), KeysView.repeating(Key(2)))
```

The strings modality keys values by text through `strings_write`, `strings_read` and `strings_scan`, and pages edit ordered documents through `pages_insert` and the other `pages_` verbs.

## Scans

`scan` takes start keys and how many keys every scan keeps, plus an optional `BlobsScanConfig` of end keys, a direction, patterns, a predicate and a progress handler.
`sample` draws keys at random, `measure` bounds the keys and bytes of a range without walking it, and `cursor` opens a walk whose `next` pulls a page, with its values, past the last key the pull before it handed out.

```mojo
var start = Key(0)
var runs = db.scan(CollectionsView.repeating(people), KeysView.repeating(start), 100, arena)
print(len(runs.run(0)))
var cursor = db.on(executor).cursor(people, Key(0))  # walks on the kind of executor it was opened on
var page = cursor.next(executor, arena, 1000)        # keys and their values, none once exhausted
print(len(page.keys()))
```

`db.changes_cursor(collection, since)` opens a feed of the changes committed after a stamp, and `db.ingest(collection)` lands every appended row as one commit of its own.

### Progress and Cancellation

Handlers are compile-time parameters, since a capturing closure cannot cross into C, and their state crosses beside them as a pointer that must outlive the call.
A progress handler answering `Continuation.CANCEL` stops the call, which raises `Status.CANCELLED`, and a predicate admits or rejects every key a scan meets.

```mojo
def stop_at_once(finished: Int, total: Int, seen: Pointer[Int, MutAnyOrigin]) -> Continuation:
    seen[] = finished
    return Continuation.CANCEL

def admit_even(keys: Span[Key, MutAnyOrigin], admissions: Span[Admission, MutAnyOrigin], state: Pointer[Int, MutAnyOrigin]):
    for index in range(len(keys)):
        admissions[index] = Admission.ADMIT if keys[index].uuid.bytes[15] % 2 == 0 else Admission.REJECT

var seen = 0
var even = BlobsScanConfig(predicate=Predicate.of[admit_even](Pointer(to=seen)))
var evens = db.scan(CollectionsView.repeating(people), KeysView.repeating(start), 100, arena, even)
var cancelling = BlobsScanConfig(progress=Progress.of[stop_at_once](Pointer(to=seen)))
_ = db.scan(CollectionsView.repeating(people), KeysView.repeating(start), 100, arena, cancelling)  # raises
```

Progress handlers return a value, which Mojo 1.2 is confirmed to pass back on Linux x86-64; Linux AArch64 and macOS rest on the CI matrix.

## Transactions

`db.transaction()` answers a `Transaction`, which offers every verb a database does, its reads and writes grouped so the commit lands all of them or none.
Its writes and reads record their keys, so a commit fails with a conflict status if another writer changed them since, while `txn.unwatched()` answers a view that records nothing.
`stage` runs the first phase of a two-phase commit and answers the read stamp, `commit` answers the commit stamp, `rollback` withdraws a stage, and `reset` starts over on the same handle.

```mojo
var pending: List[Key] = [Key(3), Key(4)]
var transaction = db.transaction()
transaction.write(CollectionsView.repeating(people), KeysView.of(pending), StringsView.of(bytes, offsets))
print(db.read(CollectionsView.repeating(people), KeysView.of(pending), arena).present(0))  # False until the commit
var peek = transaction.unwatched().read(CollectionsView.repeating(people), KeysView.of(pending), arena)
var read_stamp = transaction.stage()
var commit_stamp = transaction.commit(Durability.FLUSHED)
print(read_stamp.value <= commit_stamp.value)
```

A transaction is released when it goes out of scope, waiting for the deferred calls naming it.

## Snapshots

`db.snapshot()` pins the whole database at one point and answers a `Snapshot`, whose verbs read at that state and whose `read_stamp` is the stamp it reads at.
It holds its database open until it goes out of scope, and `save` writes it out as a new database of the same engine, holding exactly the collections and names it records.

```mojo
var snapshot = db.snapshot()
db.write(CollectionsView.repeating(people), KeysView.repeating(Key(9)), StringsView.of(bytes, offsets))
print(snapshot.read(CollectionsView.repeating(people), KeysView.repeating(Key(9)), arena).present(0))  # False
print(snapshot.read_stamp.value >= commit_stamp.value)  # it sees that commit
snapshot.save(directory)
```

## Documents

`docs_write` writes JSON documents, or one field inside each, as a `DocsWriteConfig` names, and `docs_read` reads them back whole or by the fields of a `DocsReadConfig`, where a field is a top-level key or a JSON Pointer.
`docs_gist` lists the unique paths to the leaves of a batch of documents, and `docs_gather` converts fields into typed columns, a row per key, each with its validity, conversion and collision bits.

```mojo
var movies = db.collection_create("movies", Modality.DOCS)
var documents = '{"title":"Alien","year":1979}'.as_bytes()
var document_offsets: List[UInt32] = [0, UInt32(len(documents))]
db.docs_write(CollectionsView.repeating(movies), KeysView.repeating(Key(1)), StringsView.of(documents, document_offsets))

var paths = "/year".as_bytes()
var path_offsets: List[UInt32] = [0, 5]
var types: List[FieldType] = [FieldType.I64]
var columns = db.docs_gather(
    CollectionsView.repeating(movies), KeysView.repeating(Key(1)), StringsView.of(paths, path_offsets), types, arena
)
print(columns.column(0).valid(0), columns.column(0).scalars[DType.int64]()[0])  # True 1979
```

## Graphs

A graph collection, created with `Modality.GRAPH`, indexes every edge from both of its vertices.
`graph_write_edges` takes sources and targets, and edge identifiers where several edges join one pair, `graph_read_edges` answers `Edges` of every vertex in its role, three keys per edge, and `graph_remove_edges` and `graph_remove_vertices` take them away.

```mojo
var graph = db.collection_create("graph", Modality.GRAPH)
db.graph_write_edges(CollectionsView.repeating(graph), KeysView.repeating(Key(1)), KeysView.repeating(Key(2)))
var vertices: List[Key] = [Key(1), Key(2)]
var edges = db.graph_read_edges(CollectionsView.repeating(graph), KeysView.of(vertices), arena)
print(Int(edges.degrees()[0]), edges.edges(0)[1] == Key(2))  # 1 True
var role = VertexRole.SOURCE
var leaving = db.graph_read_edges(
    CollectionsView.repeating(graph), KeysView.of(vertices), arena, roles=VertexRolesView.repeating(role)
)
print(Int(leaving.degrees()[1]))  # 0, as vertex 2 leaves by no edge
```

## Receivers & Deferred Calls

The receiver chooses where a verb runs, what it sees and how it completes, and the verb stays the same, as [the C rules](https://github.com/unum-cloud/UStore/blob/main/c/README.md#receivers--deferred-calls) describe in full.
A database, transaction or snapshot runs a verb on the calling thread, `handle.on(executor)` blocks on an executor, and `handle.deferred(executor)` submits there and answers a `Deferred`.
A `Deferred` owns its outcome and outputs and views its arena and inputs, which Mojo's origins keep alive for as long as it is; `wait` blocks in `ustore_outcome_wait` and answers the result or raises, `status` polls, and destroying an unfinished one waits for it.

```mojo
db.write(collections, keys, values)                       # the head, on the calling thread
db.on(executor).write(collections, keys, values)          # blocking, on the executor
var call = db.deferred(executor).read(collections, keys, arena)  # Deferred[...]
print(call.wait().present(0))
```

## Executors

An executor owns the threads or rings calls run on, and serves one call at a time, so a call on one serving another raises `Status.CONTENDED`.
`Executor(library)` owns nothing and runs every verb on the calling thread, `Executor.for_ram` spawns host threads near the NUMA nodes its `RamOptions` name, `Executor.for_nvme` spawns the NVMe engine's rings, and `Executor.for_vram` drives one GPU.
An executor is freed once it and every device arena it made go out of scope, and freeing it waits for the calls naming it, as [the C rules](https://github.com/unum-cloud/UStore/blob/main/c/README.md#executors) describe.

```mojo
var domains: List[Int32] = [0]
var executor = Executor.for_ram(library, RamOptions.on(domains), threads=8, cpu_class_ceiling=CPUClass.UNKNOWN)
var db = Database.init_ram(executor)
var arena = Arena(executor)  # placed where the executor computes
_ = db.on(executor).read(CollectionsView.repeating(people), KeysView.of(keys), arena)
executor.drain(Mood.SPINNING)  # waits, then keeps its threads hot
```

## Batches and Arenas

Every verb takes a batch, and its shortest packed input sets how many tasks it holds, so the library never reads past a view.
Fixed-width inputs are strided views, `KeysView`, `CollectionsView`, `LengthsView` and `VertexRolesView`, made from a span with `.of(...)` or from one element repeated for every task with `.repeating(...)`, and ragged inputs are `StringsView` tapes of 32-bit or 64-bit offsets.
Every output whose length only the call knows lands in an `Arena`, and each call rewinds it, so the previous call's views die and reading one raises `Status.INVALID_ARGUMENT`, as [the C rules](https://github.com/unum-cloud/UStore/blob/main/c/README.md#batches-and-arenas) describe.

```mojo
var arena = Arena(library)  # host pages for the calling thread
var first = db.read(CollectionsView.repeating(people), KeysView.of(keys), arena)
var other = Arena(library)
var second = db.read(CollectionsView.repeating(people), KeysView.of(keys), other)  # `first` stays readable
_ = db.read(CollectionsView.repeating(people), KeysView.of(keys), arena)
_ = first.present(0)  # raises: its arena served a later call
```

Every view tracks the origin of what it views, so the data outlives the call that reads it, and every deferred call takes an arena of its own.

Outputs export in place through the Arrow C data interface: `keys_to_arrow`, `tape_to_arrow` and `column_to_arrow` answer an `ArrowExport` whose schema and array addresses any Arrow consumer imports, and `keys_from_arrow` and `strings_from_arrow` read an Arrow column back as a call's input.

```mojo
var scanned = db.scan(CollectionsView.repeating(people), KeysView.repeating(Key(0)), 10, arena)
var exported = keys_to_arrow(KeysView.of(scanned.keys()), arena)  # fixed_size_binary(16), arrow.uuid
var keys = keys_from_arrow(arena, exported.schema_address(), exported.array_address())
_ = exported^  # Mojo frees at last use, so the export outlives every view of its buffers
```

## Errors

Every fallible entry point raises `Error` and nothing else, since Mojo allows one error type per function, and an `Error` holds a `Status` shared with the C interface.
An `Error` keeps the engine mapped while it lives, since its name and message are the engine's own static text.

```mojo
try:
    db.collection_drop(CollectionID())
except error:
    if error.status == Status.NOT_FOUND:
        pass
    print(error)  # UStore: not-found [ustore_collection_drop]: ...
```

## Platform Notes

### Engines

`Database.init_ram` starts a database in memory alone, and `Database.open_ram`, `open_rocksdb`, `open_leveldb` and `open_nvme` serve a directory or a manifest, each opening blocking on the executor it is handed.
The RAM and NVMe opens take a `cpu_class_ceiling`, the newest `CPUClass` the database's kernels may run as, intersected with the machine.
`describe`, `clear`, `compact` and `flush` act on the whole database through any receiver, and a `DatabaseCompactPlan` keeps a cancelled compaction open until `compact_plan_finish`.
On Linux the engine builds against conda-forge's glibc 2.39 sysroot, whose kernel headers hold the `io_uring` flags the NVMe engine needs.

### What This Binding Does Not Expose

On x86-64 `tape_to_arrow` raises `Status.UNSUPPORTED`, since that ABI passes the 24-byte strings view on the stack, where a Mojo call cannot place it.
The accessor layout of a strings batch, which reads strings through a C callback, is not exposed: Mojo passes the two tape layouts.

### Design

- __Blocking stays simple.__
  A verb on a database or on `db.on(executor)` reads as it would if asynchrony did not exist.
- __Deferred calls are symmetric.__
  The same verbs run on another receiver, observed only through the outcome's status word, and release rides Mojo's own destructor.
- __Nothing costs anything until used.__
  The binding starts no threads, keeps no counts and caches nothing, so a call is one argument structure built on the stack and one indirect call.
