# UStore for Go

The package binds the C ABI in `c/ustore/` through cgo, linking the engine archive `libustore_static`, and adds no logic of its own beyond two trampolines for callbacks.
It needs Go 1.24 or newer with cgo enabled and a C++20 toolchain for the engine, and it starts no goroutines, so a `Deferred` is waited on from any goroutine or polled through its status.
Every verb is a batch, and every blocking verb answers its value beside an `error` that `errors.Is` matches against a `Status`:

- `Database`, `Transaction` and `Snapshot`: the head, a group of changes, and a point in time.
- `CollectionID` and `Each`: one collection for a whole batch, or one per task.
- `Arena`: where outputs land, read through the views a verb answers until its next call.
- `Executor` and `Deferred`: the threads a call runs on, and a call submitted there.

## Installation

```sh
go get github.com/unum-cloud/ustore/golang@main
```

A commit pins a build instead of `@main`, and Go records it in `go.sum` as a pseudo-version.
The package links `libustore_static` and reads the headers from the module's own `c/`, so the archive must sit on the linker's path, such as `/usr/local/lib`, or be named through `CGO_LDFLAGS`.
From a checkout of the repository, for the current shell:

```sh
cmake -B build_release -D CMAKE_BUILD_TYPE=Release
cmake --build build_release --target ustore_static
export CGO_LDFLAGS="-L$PWD/build_release"   # or install the archive
```

```go
import ustore "github.com/unum-cloud/ustore/golang"
```

## Quickstart

The program opens a database in memory, writes a blob, a document and an edge in one transaction, and reads them back.
Collections are created by name and modality before a transaction writes into them, and every read lands in an arena.

```go
package main

import (
	"errors"
	"fmt"
	"log"

	ustore "github.com/unum-cloud/ustore/golang"
)

func main() {
	database, err := ustore.DatabaseInitRam(nil)
	if err != nil {
		log.Fatal(err)
	}
	defer database.Close()
	avatar, titanic := ustore.KeyFromUint64(1), ustore.KeyFromUint64(2)
	posters, err := database.CollectionCreate("posters", ustore.ModalityBlobs)
	if err != nil {
		log.Fatal(err)
	}
	movies, err := database.CollectionCreate("movies", ustore.ModalityDocs)
	if err != nil {
		log.Fatal(err)
	}
	similar, err := database.CollectionCreate("similar", ustore.ModalityGraph)
	if err != nil {
		log.Fatal(err)
	}

	// One transaction writes a blob, a document and an edge, and commits all three or none.
	transaction, err := database.TransactionInit()
	if err != nil {
		log.Fatal(err)
	}
	defer transaction.Close()
	err = errors.Join(
		transaction.BlobsWrite(posters, []ustore.Key{avatar}, ustore.TapeOf("\x89PNG"), nil),
		transaction.DocsWrite(movies, []ustore.Key{avatar}, ustore.TapeOf(`{"title":"Avatar","year":2009}`), nil),
		transaction.GraphWriteEdges(similar, []ustore.Edge{{Source: avatar, Target: titanic, ID: ustore.EdgeIDDefault}}, nil),
	)
	if err != nil {
		log.Fatal(err)
	}
	if _, err := transaction.Commit(ustore.DurabilityBuffered); err != nil {
		log.Fatal(err)
	}

	// Reads at the head see the commit: the poster, the year and the one edge leaving Avatar.
	arena, err := ustore.NewArena(nil)
	if err != nil {
		log.Fatal(err)
	}
	defer arena.Close()
	poster, err := database.BlobsRead(posters, []ustore.Key{avatar}, arena, nil)
	if err != nil {
		log.Fatal(err)
	}
	value, _ := poster.Get(0)
	fmt.Printf("%q ", value)
	movie, err := database.DocsRead(movies, []ustore.Key{avatar}, arena, &ustore.DocsReadConfig{Fields: ustore.TapeOf("year")})
	if err != nil {
		log.Fatal(err)
	}
	year, _ := movie.Get(0)
	fmt.Printf("%s ", year)
	edges, err := database.GraphReadEdges(similar, []ustore.Key{avatar}, ustore.VertexRoleSource, arena)
	if err != nil {
		log.Fatal(err)
	}
	fmt.Println(edges.Edges(0)[0].Target == titanic)
}
```

`golang/example_test.go` compiles and runs this program on every `go test`.

## Collections

Collections are created under a name and the modality they hold, resolved by name once and addressed by `CollectionID` from then on, and a database holds none until one is created.
A name starting with `_` is refused with `StatusInvalidArgument`, as the engines keep their own system collections there.
`CollectionFind` answers the identifier and modality of every name in a batch, and `CollectionList` every collection with its name.
`CollectionDrop` empties a collection of its pairs or values with `DropPairs` or `DropValues`, or drops it whole with `DropCollection`.
Every batch verb takes a `Collections`: a bare `CollectionID` serves every task, and `Each` names one per task.

```go
people, err := database.CollectionCreate("people", ustore.ModalityBlobs)
found, err := database.CollectionFind(ustore.TapeOf("people", "places"), arena)
id, modality, ok := found.Get(0)
values, err := database.BlobsRead(ustore.Each{people, places}, []ustore.Key{ada, ada}, arena, nil)
err = database.CollectionDrop(people, ustore.DropCollection)
```

## Reads and Writes

Keys are `[]Key`, viewed in place, and values are a `Tape`: bytes end to end around one more offset than values, as Apache Arrow lays them out.
`TapeOf` packs strings or byte slices into a fresh tape, while `U32Tape` and `U64Tape` view bytes and offsets the caller already holds, without a copy.
A read answers `Values`, which addresses the tape in the arena rather than copying out of it, reports a missing key as absent, and passes the whole batch back through `Values.Tape`, so a tape a read wrote is written again as it is.
Since every result views the arena as Go slices, a verb into an arena whose `Describe` reports `HostAccessRefused` fails with `StatusDevice` before it runs, while managed memory and Apple's shared memory read in place.
A write carries values and nothing else, so an empty value is still a value, and `BlobsRemove` removes keys, or ranges given end keys.

```go
ada, grace, kent := ustore.KeyFromUint64(1), ustore.KeyFromUint64(2), ustore.KeyFromUint64(3)
err := database.BlobsWrite(people, []ustore.Key{ada, grace}, ustore.TapeOf("Ada", "Grace"), nil)

values, err := database.BlobsRead(people, []ustore.Key{ada, grace, kent}, arena, nil)
name, ok := values.Get(0)    // "Ada", true
_, ok = values.Get(2)        // false
headers, err := database.BlobsRead(people, []ustore.Key{ada}, arena, &ustore.BlobsReadConfig{Limits: []uint64{2}})
err = database.BlobsRemove(people, []ustore.Key{grace}, nil)
```

## Scans

Scans, samples and measures run one task per collection their `Collections` names, from a start key, inclusive, towards an end key, exclusive, in a `ScanDirection`.
`BlobsScan` lists up to the count it wants, `BlobsSample` draws keys at random, and `BlobsMeasure` bounds the keys and bytes a range holds without walking it; `DocsScan`, `PagesScan`, `StringsScan` and `GraphScanVertices` serve the other modalities.
A start key of `Key{}` walks up from the first key, and `KeyUnknown` walks down from the last.
A cursor walks one collection page by page, each pull resuming past the last key the one before it handed out, and a page shorter than wanted exhausts it for good.
`ChangesCursorInit` opens a feed of the changes committed after a `CommitStamp`, oldest first, pinning that stamp as a snapshot does.
A cursor keeps no executor: `cursor.NextPairs(..)` pulls on the calling goroutine, and `cursor.On(executor).NextPairs(..)` blocks on the executor named for that pull.
A `DocsCursor` gathers the `Fields` of its config into typed columns, and its `Next` answers `DocsBatches`, one batch of keys and columns per executor task that found rows, none once the walk is exhausted.

```go
first, err := database.BlobsScan(people, 100, arena, nil)
measured, err := database.BlobsMeasure(people, arena, &ustore.MeasureConfig{StartKeys: []ustore.Key{ada}, EndKeys: []ustore.Key{kent}})
last, err := database.BlobsScan(people, 10, arena, &ustore.BlobsScanConfig{
	StartKeys: []ustore.Key{ustore.KeyUnknown}, Direction: ustore.ScanDescending,
})

cursor, err := database.On(executor).BlobsCursorInit(people, ustore.Key{}, nil)
defer cursor.Close()
for {
	page, err := cursor.On(executor).NextPairs(1024, arena)
	if err != nil || page.Len() < 1024 {
		break
	}
}

ages, err := database.DocsCursorInit(people, ustore.Key{}, &ustore.DocsCursorConfig{
	Fields: ustore.TapeOf("age"), ColumnsTypes: []ustore.DocsFieldType{ustore.DocsFieldTypeU64},
})
defer ages.Close()
batches, err := ages.On(executor).Next(1024, arena) // batches.Keys(i) and batches.Columns(i) per batch
```

### Progress and Cancellation

The configs of `Compact`, `Save`, `PagesImport` and the scans carry an optional `ProgressCallback`, called with the units finished and the units in all, never on two threads at once and never after the call is final.
Answering false cancels the call, which fails with `StatusCancelled` and publishes nothing, a cancelled save removing what it wrote.
A handler must not panic, since unwinding through the library's C frames aborts the process.

```go
deadline := time.Now().Add(30 * time.Second)
beforeTheDeadline := func(finished, total uint64) bool { return time.Now().Before(deadline) }
err := snapshot.Save("/var/lib/people-copy", beforeTheDeadline)
if errors.Is(err, ustore.StatusCancelled) {
	fmt.Println("the copy was removed")
}
```

The `Predicate` field of a scan or cursor config admits or passes over the keys it lists, asked on the host, and its type tells the library what one call costs:

- `ustore.Predicate` answers one key at a time, asked before patterns or filters read any value.
- `ustore.BatchPredicate` answers a page of keys at once, asked about the keys patterns and filters kept, for handlers that pay per call.
- `ustore.CheapBatchPredicate` answers a page of keys at once, asked before patterns or filters read any value.

```go
odd := ustore.Predicate(func(key ustore.Key) bool { return key[15]%2 == 1 })
found, err := database.BlobsScan(people, 10, arena, &ustore.BlobsScanConfig{Predicate: odd})

moderated := ustore.BatchPredicate(func(keys []ustore.Key, admissions []ustore.Admission) {
	for position, allowed := range moderation.Check(keys) { // one round trip for the whole page
		if allowed {
			admissions[position] = ustore.Admit
		}
	}
})
found, err = database.BlobsScan(people, 10, arena, &ustore.BlobsScanConfig{Predicate: moderated})
```

Admissions start as `ustore.Reject`, and both slices live only until the handler returns.

## Transactions

A transaction groups reads and writes across collections and modalities, and commits all of them or none.
Its own verbs record the keys they touch, so the commit reports `StatusWriteConflict`, `StatusReadConflict` or `StatusPhantomConflict` if another writer changed them, while `Transaction.View` with `WatchNone` records nothing.
A transaction conflicts rather than blocks, and keeps its contents whatever the outcome, so `Reset` reopens it on the newest state for another try.
`Stage` answers a `ReadStamp`, the stamp the transaction reads at and its commit validates against, and `Commit` a `CommitStamp`, its position in commit order; the two are distinct types, so neither is compared with the other by accident.
`Rollback` withdraws a stage while keeping the reads and writes, so the transaction can stage again.

```go
transaction, err := database.TransactionInit()
defer transaction.Close()
err = transaction.BlobsWrite(people, []ustore.Key{ada, grace}, ustore.TapeOf("Ada", "Grace"), nil)
peek, err := transaction.View(ustore.WatchNone).BlobsRead(people, []ustore.Key{ada}, arena, nil)
readStamp, err := transaction.Stage()
commitStamp, err := transaction.Commit(ustore.DurabilityFlushed)
```

## Snapshots

A snapshot pins the state it was taken at until it is closed, and its verbs read that state; nothing writes through it.
`Snapshot.ReadStamp` is the stamp it reads at, which every commit whose `CommitStamp` is at or below it shows.
`Save` writes it out as a new database of the same engine, holding exactly the collections and names it records.

```go
snapshot, err := database.SnapshotInit()
defer snapshot.Close()
pinned, err := snapshot.BlobsRead(people, []ustore.Key{ada}, arena, nil)
err = snapshot.Save("/var/lib/people-copy", nil)
```

## Documents

`DocsWrite` writes JSON text by default, whole documents or, with `Fields`, top-level keys and RFC 6901 pointers inside them, and `DocsModification` chooses between an upsert, an update, an insert, an RFC 6902 patch and an RFC 7386 merge.
`DocsRead` reads whole documents or fields as JSON or as strings, and `DocsGist` lists the paths to every leaf.
`DocsGather` converts fields into typed columns, a row per key, whose scalars `DocsColumnScalars` views straight in the arena beside the validity, conversion and collision bitmaps.

```go
documents := ustore.TapeOf(`{"name":"Ada","born":1815}`)
err := database.DocsWrite(people, []ustore.Key{ada}, documents, nil)
died := &ustore.DocsWriteConfig{Fields: ustore.TapeOf("died")}
err = database.DocsWrite(people, []ustore.Key{ada}, ustore.TapeOf("1852"), died)

names, err := database.DocsRead(people, []ustore.Key{ada}, arena, &ustore.DocsReadConfig{
	Fields: ustore.TapeOf("name"), OutputsType: ustore.DocsFieldTypeString,
})
paths, err := database.DocsGist(people, []ustore.Key{ada}, otherArena)
columns, err := database.DocsGather(people, []ustore.Key{ada}, ustore.TapeOf("born"),
	[]ustore.DocsFieldType{ustore.DocsFieldTypeU64}, arena)
born := ustore.DocsColumnScalars[uint64](columns.Column(0))
```

## Graphs

A graph collection, created with `ModalityGraph`, indexes every edge from both of its vertices, and an `Edge` is laid out as the source, target and identifier keys the ABI puts on its tapes, so a slice of edges crosses as three strided columns.
`GraphReadEdges` answers `Neighborhoods`, the degree of every vertex and its edges, outgoing ones before incoming ones, and `Edge.Neighbor` names the other end.
A `VertexRole` gives the whole batch one role and `VertexRoles` gives every vertex its own.

```go
follows, err := database.CollectionCreate("follows", ustore.ModalityGraph)
edges := []ustore.Edge{
	{Source: first, Target: second, ID: ustore.EdgeIDDefault},
	{Source: first, Target: third, ID: ustore.EdgeIDDefault},
	{Source: second, Target: third, ID: seventh},
}
err = database.GraphWriteEdges(follows, edges, nil)

vertices := []ustore.Key{first, third}
found, err := database.GraphReadEdges(follows, vertices, ustore.VertexRoleAny, arena)
for index, vertex := range vertices {
	for _, edge := range found.Edges(uint(index)) {
		fmt.Println(vertex, "meets", edge.Neighbor(vertex))
	}
}
err = database.GraphRemoveVertices(follows, []ustore.Key{first}, nil)
```

## Receivers & Deferred Calls

The receiver a verb is called on chooses what it sees, where it runs and how it completes, while the verb and its arguments stay.
A `Database`, a `Transaction`, a `Snapshot` and a cursor run their verbs on the calling goroutine, `On(executor)` blocks on an executor, and `Deferred(executor)`, which cursors lack, answers a `*Deferred` once the call is submitted, or the refusal the binding or the library made before anything ran.
A deferred call reads its slices and writes its arena until it is final, so neither may change until `Wait` returns.
`Wait` blocks in `ustore_outcome_wait`, `Status` polls through `ustore_outcome_status`, and a `Deferred` dropped without either is released by the collector once its call is final.

```go
deferred := database.Deferred(executor)
writing, err := deferred.BlobsWrite(people, []ustore.Key{ada, grace}, ustore.TapeOf("Ada", "Grace"), nil)
if err != nil {
	return err
}
_, err = writing.Wait()

reading, err := deferred.BlobsRead(people, []ustore.Key{ada, grace}, arena, nil)
values, err := reading.Wait() // or poll reading.Status() until it leaves ustore.StatusDeferred
committing, err := transaction.Deferred(executor).Commit(ustore.DurabilityBuffered)
```

## Executors

An `Executor` is the threads that run calls, shared by any number of databases and receivers, serving one call at a time, so a call submitted while another holds it answers `StatusContended`.
`ExecutorForRam` spawns threads near the NUMA nodes it names, every node for an empty list, where zero threads take seven eighths of their cores, and a nonzero scratch limit caps each thread's scratch.
`ExecutorForNvme` spawns the NVMe engine's rings near the memory domains it names or the media under its `Paths`, and `ExecutorForVram` drives the GPUs of one vendor, their positions in `GPUDevices` or every one for none, one stream each, which it creates and destroys or adopts from the caller and destroys.
Opening a database blocks, on the executor its config names or the calling goroutine, and closing an executor waits for the call holding it.
An arena never outlives the executor that made it, so `defer executor.Close()` comes before `defer arena.Close()`, and an arena from `NewArena(nil)` lives until it is closed.

```go
executor, err := ustore.ExecutorForRam([]int32{0}, 8, ustore.CPUClassUnknown, 0)
defer executor.Close()
people, err := ustore.DatabaseOpenRocksdb("/var/lib/people", &ustore.DatabaseOpenRocksdbConfig{Executor: executor})
arena, err := ustore.NewArena(executor)
defer arena.Close()
found, err := people.On(executor).CollectionFind(ustore.TapeOf("names"), arena)
err = executor.Drain(ustore.MoodParked)
```

## Batches and Arenas

Every output whose length scales with the call lands in an `Arena`, and the verb answers a view over it.
`NewArena(executor)` places one where that executor computes, and `NewArena(nil)` on host pages, and a call rewinds it, keeping its high-water mark, so a steady loop allocates once.
A view read after a later call took its arena fails as `Contended`.
A slice or segment an accessor returned is valid only until the arena's next call.
A result kept past the next call needs the next call to take another arena.
An input lying inside the arena a call rewinds is refused with `StatusInvalidArgument`.
A `Key` is sixteen bytes ordered as their bytes, and `KeyFromUint64` spells an integer big-endian, so integer keys order as integers.

```go
found, err := ustore.NewArena(nil)
fetched, err := ustore.NewArena(nil)
scanned, err := database.BlobsScan(people, 100, found, nil)
for _, key := range scanned.Keys(0) {
	values, err := database.BlobsRead(people, []ustore.Key{key}, fetched, nil) // rewinds fetched, never found
	consume(values, err)
}
```

## Errors

Every fallible verb answers an `error`, a `*Error` carrying the `Status` the C ABI reported and the static message beside it, so `errors.Is` matches it against a bare `Status` and `errors.As` reaches the message.
A closed handle is refused with `StatusUninitialized` before anything crosses, and a batch whose runs disagree in length with `StatusInvalidArgument`.
`Status`, `Durability`, `Watch`, `Modality` and the other enums print through `String` and parse through their `FromName` functions the spellings the C tables own, such as `"not-found"` or `"strict-serializable"`.

```go
_, err := database.CollectionCreate("people", ustore.ModalityBlobs)
if errors.Is(err, ustore.StatusAlreadyExists) {
	fmt.Println(err)
}
fmt.Println(ustore.VersionName()) // the library the program runs against, such as "1.0.0"
```

## Apache Arrow

The converters of `c/ustore/arrow.h` view an Arrow column as a call's input in place, and export what a call wrote into an arena as a column in place, and until the column is released the arena's next call and `Close` answer `StatusContended`:

```go
scanned, err := database.BlobsScan(people, 100, found, nil)
var schema ustore.ArrowSchema
var array ustore.ArrowDeviceArray
if err := ustore.KeysViewToArrow(scanned.Keys(0), found, &schema, &array); err != nil {
	return err
}
defer schema.Release()
defer array.Release()

keys, err := ustore.KeysViewFromArrow(&schema, array.Array()) // the same keys, in place
columns, err := database.DocsGather(movies, keys, ustore.TapeOf("title"), []ustore.DocsFieldType{ustore.DocsFieldTypeString}, fetched)
var titles ustore.ArrowSchema
var column ustore.ArrowDeviceArray
err = ustore.DocsColumnViewToArrow(columns.Column(0), ustore.DocsFieldTypeString, fetched, &titles, &column)
defer titles.Release()
defer column.Release()

tape, err := ustore.StringsViewFromArrow(&titles, column.Array()) // the same strings, as a Tape
err = database.BlobsWrite(people, keys, tape, nil)
```

`KeysViewFromArrow` and `StringsViewFromArrow` read keys and strings, and `KeysViewToArrow`, `StringsViewToArrow` and `DocsColumnViewToArrow` write keys, strings and gathered columns.

## Platform Notes

### Engines

`DatabaseInitRam` starts an empty database in memory alone, `DatabaseOpenRam` serves a directory as one image, `DatabaseOpenRocksdb`, `DatabaseOpenLeveldb` and `DatabaseOpenNvme` serve a directory in place, and `DatabaseViewRocksdb` and `DatabaseViewNvme` serve one read-only, refusing every change with `StatusReadOnly`.
`Close` writes nothing to disk, so `On(executor).Flush()` makes commits with `DurabilityBuffered` durable before it.
`Describe` reports the engine, the isolation, the CPU class picked and the sizes the engine estimates, `Clear` empties every collection at once, and `Compact` reclaims space through the phases its engine runs, where a `CompactPlan` keeps a stopped compaction open until `CompactPlanFinish`.
The RAM engine always links, and build tags mirror the Cargo features for the others: `ustore_rocksdb` links `librocksdb`, and on Linux `liburing` beside it, and `ustore_leveldb` links `libleveldb`.
Build the archive with the matching `USTORE_WITH_*` options and point `CGO_LDFLAGS` at the directory holding the engine archives too, as the `lib/` of a prefix CMake installed with `USTORE_INSTALL` does; the NVMe engine links nothing more.

```sh
go build -tags ustore_rocksdb,ustore_leveldb ./...
```

```go
scratch, err := ustore.DatabaseInitRam(&ustore.DatabaseInitRamConfig{Isolation: ustore.IsolationSnapshot})
people, err := ustore.DatabaseOpenRocksdb("/var/lib/people", nil)
description, err := people.Describe()
err = people.Clear(ustore.DropValues) // keeps every key, emptying its value
err = people.Compact(nil)            // reclaims what overwritten and removed data still takes
```

### Left Unbound

- Deferred opens, transaction and snapshot inits, resets, cursor inits, cursor pulls and ingests: these block whatever receiver they are called on.
- The progress of a cursor pull, and the lengths a read writes beside its values, which the tape's offsets carry.
- Batches of strings through an accessor: every input batch is a `Tape`.
- Exporting strided keys, like the columns of edges, through `KeysViewToArrow`, which takes dense keys alone.

### Design

- The receiver makes the completion a type, not a flag.
  Each verb is written once over its parent, and the parent decides what compiles: only a transaction commits, only the head clears and compacts, and nothing writes through a snapshot.
- Inputs cross as they lie.
  Keys, edges and tapes are pinned in place for the call, and a collection or a role the whole batch shares crosses from the call's own frame, so nothing is staged on the way in.
- One pinned frame per call.
  It holds the outcome, the argument structure, the fixed-width inputs and the result, and `TestCallsAllocateOnlyTheirFrames` pins the budgets below.
- No life of its own.
  The binding starts no goroutines, takes no locks and keeps no counts or queues, and releasing a `Deferred` rides `Wait`, `Close` or the collector, each of which waits for the call to be final first.

| Path | Go | Rust |
|---|---|---|
| Blocking call | 1 frame | 0 |
| Refusal before the call | 1 `Error` | 1 error |
| Deferred submit and wait | 4: frame, `Deferred`, 2 in `AddCleanup` | 0 |
| Each callback | 1 handle | 0 |
