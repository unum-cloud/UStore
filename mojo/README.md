# UStore for Mojo

The Mojo package binds the C ABI in `c/ustore/`, opening the engine's shared library with `dlopen` rather than linking it, so a consumer builds with no linker flags of its own.
It needs Mojo 1.2 through pixi, on Linux x86-64, Linux AArch64 or macOS on Apple silicon.
It binds every exported verb but the Arrow exchange: databases, snapshots, transactions, collections, change feeds and ingests, the verbs of blobs, strings, documents, graphs and pages, and the executors and arenas they run on and write into.

## Installation

Let Pixi build the package from source, with no channel to add and no linker flag to pass; the engine arrives as its run dependency, in the environment prefix `mojo build` bakes as runpath.

```toml
[workspace]
preview = ["pixi-build"]

[dependencies]
ustore_mojo = { git = "https://github.com/unum-cloud/UStore", subdirectory = "mojo", tag = "v1.0.0-rc.1" }
```

From a checkout, `pixi run test` builds the engine through the tree's own CMake and runs `mojo/test.mojo` against in-memory databases.
On Linux the engine builds against conda-forge's glibc 2.39 sysroot, whose kernel headers carry the `io_uring` flags the NVMe engine needs.

## Quickstart

```mojo
from ustore import Arena, CollectionsView, Database, Executor, Key, KeysView, Library, Modality, StringsView

def main() raises:
    var library = Library()
    var db = Database.init_ram(Executor(library))
    var people = db.collection_create("people", Modality.BLOBS)

    var keys: List[Key] = [Key(1), Key(2)]
    var bytes = "AdaGrace".as_bytes()  # two values end to end
    var offsets: List[UInt32] = [0, 3, 8]  # one more offset than values
    db.write(CollectionsView.repeating(people), KeysView.of(keys), StringsView.of(bytes, offsets))

    var arena = Arena(library)
    var values = db.read(CollectionsView.repeating(people), KeysView.of(keys), arena)
    print(values.present(1), len(values.values()[1]))
```

A `Database` owns its engine; an `Executor` owns the threads or rings a call runs on; an `Arena` owns the outputs a call writes.
`Library()` opens the engine once, and every handle created through it keeps it mapped.

## Receivers & Deferred Calls

The receiver chooses where a verb runs, what it sees and how it completes, and the verb stays the same.
A database, transaction or snapshot runs a verb on the calling thread, `handle.on(executor)` blocks on an executor, and `handle.deferred(executor)` submits it there and answers a `Deferred`:

```mojo
db.write(collections, keys, values)                       # the head, on the calling thread
db.on(executor).write(collections, keys, values)          # blocking, after every earlier submission
var call = db.deferred(executor).read(collections, keys, arena)  # Deferred[...]
print(call.wait().present(0))
```

`On` and `DeferredOn` offer the same verbs with the same arguments, and the blocking verbs are written once, in the `BlobsVerbs`, `StringsVerbs`, `DocsVerbs`, `GraphVerbs`, `PagesVerbs` and `DatabaseVerbs` traits every blocking receiver conforms to.
The verb names follow the C ones: blobs verbs are unprefixed, as `write` and `scan`, and the other modalities carry theirs, as `strings_scan`, `docs_gather`, `graph_read_edges` and `pages_insert`.
Reads see the head of a database, a transaction, or a snapshot; writes join the receiver's transaction, recording their keys unless it came from `txn.unwatched()`, and a snapshot's receiver, which has none, writes at the head.
`stage`, `commit`, `rollback` and `reset` act on the receiver's transaction and `save` on its snapshot, and the engine answers `Status.UNINITIALIZED` for a receiver without one.

A `Deferred` owns its outcome and outputs until the call is final, and borrows the arena and every input it was handed, which Mojo keeps alive for as long as the `Deferred` is; freeing the database, the executor or a transaction waits for the calls naming them.
Mojo has no runtime of its own to await on, so `wait` is the whole interface: it blocks in `ustore_outcome_wait` until the call is final, then answers the result or raises the failure, as many times as it is asked.
`status` reads the outcome without blocking, and destroying an unfinished `Deferred` waits for it.

## Keys

Every verb takes a batch, and its shortest packed input sets how many tasks it carries, so the library never reads past a view; a repeated element fits any batch.
Fixed-width inputs are strided views, `KeysView`, `CollectionsView`, `LengthsView` and `VertexRolesView`, made from a span with `.of(...)` or from one element repeated for every task with `.repeating(...)`.
Ragged inputs are `StringsView` tapes of 32-bit or 64-bit offsets, the Apache Arrow layout a read writes, so a tape read back is written again as it is.
Every view tracks the origin of what it borrows, so the data outlives the call that reads it.

## Arenas

Every output whose length only the call knows, values, keys, offsets, bounds, columns and edges, lands in an `Arena` you pass, and the verb answers a view over it without copying.
`Arena(executor)` places it where that executor computes, decided once, and `Arena(library)` makes host pages for the calling thread.
Each call rewinds its arena, so the previous call's views die and a steady loop allocates once; reading a stale one raises `Status.INVALID_ARGUMENT`.
To keep a result past the next call, pass that call another arena, and give every deferred call its own.

## Progress & Cancellation

Scans take their start keys and how many keys every scan keeps, plus an optional config for the end keys, the direction, the patterns or filter, the predicate and the progress handler:

```mojo
def stop_at_once(finished: Int, total: Int, seen: Pointer[Int, MutAnyOrigin]) -> Continuation:
    seen[] = finished
    return Continuation.CANCEL

def admit_even(keys: Span[Key, MutAnyOrigin], admissions: Span[Admission, MutAnyOrigin], state: Pointer[Int, MutAnyOrigin]):
    for index in range(len(keys)):
        admissions[index] = Admission.ADMIT if keys[index].uuid.bytes[15] % 2 == 0 else Admission.REJECT

var seen = 0
var config = BlobsScanConfig(predicate=Predicate.of[admit_even](Pointer(to=seen)))
var runs = db.scan(CollectionsView.repeating(people), KeysView.repeating(start), 100, arena, config)
_ = db.scan(collections, starts, 100, arena, BlobsScanConfig(progress=Progress.of[stop_at_once](Pointer(to=seen))))  # raises Status.CANCELLED
```

Handlers are compile-time parameters, since a capturing closure cannot cross into C, and their state crosses beside them as a pointer that must outlive the call.
Progress handlers return a value, which Mojo 1.2 is confirmed to pass back on Linux x86-64; Linux AArch64 and macOS rest on the CI matrix.

## Binary Values

A cursor walks one collection page by page, every pull resuming past the last key the pull before it handed out:

```mojo
var cursor = db.on(executor).cursor(people, Key(0))  # walks on the kind of executor it was opened on
var page = cursor.next(executor, arena, 1000)        # keys and their values, zero once exhausted
```

`db.changes_cursor(collection, since)` opens a feed of the changes committed after a stamp, and `db.ingest(collection)` lands every appended row as one commit of its own.

## Errors

Every fallible entry point raises `Error` and nothing else, since Mojo allows one error type per function.
An `Error` carries a `Status` shared with the C interface:

```mojo
try:
    db.collection_drop(CollectionID())
except error:
    if error.status == Status.NOT_FOUND:
        pass
    print(error)  # UStore: not-found [ustore_collection_drop]: ...
```

An `Error` keeps the engine mapped while it lives, since its name and message are the engine's own static text.

## What This Binding Does Not Expose

The five Arrow exports of `c/ustore/arrow.h` stay unbound, as Mojo has no Arrow arrays to hand them.
The accessor layout of a strings batch, which reads strings through a C callback, is not exposed: Mojo passes the two tape layouts.

## Design

Blocking stays simple: a verb on a database or on `db.on(executor)` reads as it would if asynchrony did not exist.
Deferred calls are symmetric, the same verbs on another receiver, observed only through the outcome's status word, and release rides Mojo's own destructor.
Nothing costs anything until used: the binding starts no threads, keeps no counts and caches nothing, so a call is one argument structure built on the stack and one indirect call.
