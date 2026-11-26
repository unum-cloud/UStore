# UStore for Rust

The crate binds the C ABI in `c/ustore/`, linking the engine library its build script makes through the project's own CMake, and adds no C or C++ of its own.
It needs Rust 1.100 and a C++20 toolchain for the engine, and it brings no async runtime, so a `Deferred` is a `Future` any executor of async tasks polls, or blocks through `wait`.
Every verb is a batch, and every blocking verb returns a `Result`:

- `Database`, `Transaction` and `Snapshot`: the head, a group of changes, and a point in time.
- `Collection`: a collection by name, with `blobs`, `docs` and `graph` views of one key at a time.
- `Arena`: where outputs land, held by the result that views them.
- `Executor` and `Deferred`: the threads a call runs on, and a call deferred there.

## Installation

```sh
cargo add ustore
```

The in-memory `ram` engine always builds, the default `rocksdb` and `leveldb` features add theirs, `nvme` adds the NVMe engine, and `serde` reads documents into `serde` types.
Without a feature, its engine's `Database::open_*` fails with `Status::Unsupported`.
`USTORE_LIBRARY_DIR` names a directory already holding the engine archives, such as the `lib/` of a prefix CMake installed, and skips the CMake build.
From a checkout, the suites run per feature set:

```sh
cargo test                                        # RAM, RocksDB and LevelDB
cargo test --all-features                         # every engine, and documents read into `serde` types
```

## Quickstart

The program opens a database in memory, writes a blob, a document and an edge in one transaction, and reads them back.
`Database::collection` finds a collection by name, and `Database::find_or_create` creates a missing one to hold a modality.

```rust
use ustore::{jsontape, Database, Durability, Edge, Key, Modality, RAMOptions, Role};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let database = Database::init_ram(&RAMOptions::default())?;
    let [avatar, titanic] = [1u64, 2].map(Key::from);
    let document = jsontape::parse(br#"{"title":"Avatar","year":2009}"#)?;
    for (name, modality) in [("posters", Modality::Blobs), ("movies", Modality::Docs), ("similar", Modality::Graph)] {
        database.find_or_create(name, modality)?;
    }

    // One transaction writes a blob, a document and an edge, and commits all three or none.
    let mut transaction = database.transaction()?;
    transaction.collection("posters")?.blobs().set(avatar, b"\x89PNG")?;
    transaction.collection("movies")?.docs().set(avatar, &document)?;
    transaction.collection("similar")?.graph().add_edge(avatar, titanic)?;
    transaction.commit(Durability::Buffered)?;

    // Reads at the head see the commit: the poster, the year and the one edge leaving Avatar.
    let posters = database.collection("posters")?;
    assert_eq!(posters.blobs().get(avatar)?.as_deref(), Some(b"\x89PNG".as_slice()));
    let movies = database.collection("movies")?;
    let movie = movies.docs().get(avatar)?.expect("committed above");
    assert_eq!(movie["year"].as_u64(), Some(2009));
    let similar = database.collection("similar")?;
    let edges = similar.graph().read_edges(&[avatar], Role::Source)?;
    assert_eq!(*edges, [Edge::new(avatar, titanic)]);
    Ok(())
}
```

Shorthand such as `get` makes a one-off arena per call and answers an `Owned` value that keeps it, dereferencing to the bytes in place, while the batch verbs below take an `Arena` and answer views into it.
The verbs of each modality come from a trait, `BlobsVerbs`, `StringsVerbs`, `DocsVerbs`, `GraphVerbs` or `PagesVerbs`, which every receiver implements, so `use ustore::*` brings them all.

## Collections

Collections are created under a name and the modality they hold, resolved by name once and addressed by `CollectionID` from then on, and a database holds none until one is created.
A name starting with `_` is refused with `Status::InvalidArgument`, as the engines keep their own system collections there.
`collection_find` answers the identifier and modality of a name, `collection_list` every identifier, and `collection_names` a page of names from a prefix.
`Database::collection_drop` empties a collection of its pairs or values with `DropMode::Pairs` or `DropMode::Values`, or drops it whole with `DropMode::Collection`.
The ABI holds a collection per task, so every batch verb takes a `Collections`: a bare `CollectionID` converts into `Collections::Every`, and `Collections::Each` names one per entry.

```rust
use ustore::{BlobsVerbs, Collections, DropMode, Modality, StringsVerbs};

let people = database.collection_create("people", Modality::Blobs)?;
let (same, modality) = database.collection_find("people", &mut arena)?;
for name in database.collection_names("pe", None, 100, &mut arena)?.iter() {
    println!("{name}");
}
let spanned = [people, places];
let values = database.read(Collections::Each(&spanned), &[ada, ada], &mut arena)?;
database.collection_drop(people, DropMode::Collection)?;
```

## Reads and Writes

An `Entry` pairs a key with bytes the caller keeps, laid out so a slice of them crosses as it stands: the keys as one strided run, and the values through an accessor over the same entries.
A write also takes owned keys beside a `stringtape::BytesTape<u32>` or `BytesTape<u64>`, which the library reads as it stands.
A read hands back `Values`, which addresses the tape in the arena rather than copying out of it, reports a missing key as `None`, and exports the whole batch through `Values::tape` in Arrow's `binary` or `large_binary` layout.
A write holds values and nothing else, so an empty slice writes an empty value, and `remove` and `remove_range` remove keys in batches of their own.

```rust
use ustore::{Arena, BlobsVerbs, Durability, Entry, Key, Modality};

let mut arena = Arena::new()?;
let [ada, grace, kent] = [1u64, 2, 3].map(Key::from);
let entries = [Entry::new(ada, b"Ada"), Entry::new(grace, b"Grace")];
database.write(people, &entries, Durability::Buffered)?;

let values = database.read(people, &[ada, grace, kent], &mut arena)?;
assert_eq!(values.get(0), Some(b"Ada".as_slice()));
assert_eq!(values.get(2), None);
let headers = database.read_slices(people, &[ada, grace], ..2, &mut arena)?;
database.remove(people, &[grace], Durability::Buffered)?;
```

## Scans

Scans, cursors and measures take a `KeyRange` of a start key, inclusive, an end key, exclusive, and a `ScanDirection`, where a key alone walks up from it and `start..end` walks up between them.
`scan` lists up to the count it wants, `sample` draws keys at random, and `measure` bounds the keys and bytes a range holds without walking it; `docs_sample`, `pages_sample` and `graph_sample_vertices` serve the other modalities.
A `Cursor` walks one collection page by page, each pull resuming past the last key the one before it handed out, and a page shorter than wanted exhausts it for good.
`DocsCursor::next` pulls `Batches` of rows instead, each its keys and the `Columns` the cursor gathers, one per executor task that found rows, none once the walk is exhausted.
`Database::changes_cursor` opens a feed of the changes committed after a `CommitStamp`, oldest first, pinning that stamp as a snapshot does.
A cursor keeps no executor: `cursor.next_pairs(..)` pulls on the calling thread, and `cursor.on(&executor).next_pairs(..)` blocks on the executor named for that pull.

A `Matching` narrows a scan or a cursor by patterns, a filter, and a `Predicate` viewing a handler: `Predicate::each` answers one key at a time and is asked cheaply, while `Predicate::batch` fills a page of `Admission`s and is asked once per page unless told `.cheap()`.

```rust
use ustore::{BlobsScanConfig, KeyRange, Matching, Predicate, ScanDirection};

let first = database.scan(people, Key::from(0u64), 100, &mut arena)?.keys;
let measured = database.measure(people, ada..kent, &mut arena)?;
let largest = KeyRange { direction: ScanDirection::Descending, ..KeyRange::default() };
let last = database.scan(people, largest, 10, &mut arena)?.keys;
let even = |key: Key| key.0.0[15] % 2 == 0;
let matching = Matching { predicate: Some(Predicate::each(&even)), ..Matching::default() };
let evens = database.scan(people, ada, BlobsScanConfig { matching, ..BlobsScanConfig::from(100) }, &mut arena)?;

let mut cursor = database.head().on(&executor).cursor(people, Key::from(0u64), Matching::default())?;
loop {
    let page = cursor.on(&executor).next_pairs(1024, &mut arena)?;
    if page.keys().len() < 1024 { break; }
}
```

### Progress and Cancellation

The configs of `compact`, `save`, `pages_import` and the scans hold an optional `Progress`, which `Progress::new(&mut handler)` makes from any `FnMut(finished, total) -> ControlFlow<()> + Send` through a trampoline made for its type.
The handler runs on whichever thread advances the work, never on two at once and never after the call is final, and a deferred call needs a `'static` one.
Answering `ControlFlow::Break(())` cancels the call, which fails with `Status::Cancelled` and publishes nothing, a cancelled save removing what it wrote.
A handler must not panic, since unwinding out of the library's `extern "C"` frame aborts the process.

```rust
use std::ops::ControlFlow;
use std::time::{Duration, Instant};

let deadline = Instant::now() + Duration::from_secs(30);
let mut before_the_deadline = move |_finished: usize, _total: usize| match Instant::now() < deadline {
    true => ControlFlow::Continue(()),
    false => ControlFlow::Break(()),
};
let config = SaveConfig { path: "/var/lib/people-copy", progress: Some(Progress::new(&mut before_the_deadline)) };
match snapshot.save(config) {
    Err(failure) if failure.status == ustore::Status::Cancelled => println!("the copy was removed"),
    other => other?,
}
```

## Transactions

A transaction groups reads and writes across collections and modalities, and commits all of them or none.
`transaction.view(Watch::Keys)` records the keys its calls touch, so the commit reports `Status::WriteConflict`, `ReadConflict` or `PhantomConflict` if another writer changed them, while `Watch::None` records nothing.
A transaction conflicts rather than blocks, and keeps its contents whatever the outcome, so `Transaction::reset` reopens it on the newest state for another try.
`stage` answers a `ReadStamp`, the stamp the transaction reads at and its commit validates against, and `commit` a `CommitStamp`, its position in commit order; the two are distinct types, so neither is compared with the other by accident.
`Transaction::collection` reads and writes inside the transaction, while its name resolves at the head.
The transaction itself runs every verb on the calling thread, watching the keys they touch.

```rust
use ustore::Watch;

let mut transaction = database.transaction()?;
transaction.write(people, &entries, Durability::Buffered)?;
let peek = transaction.view(Watch::None).read(people, &[ada], &mut arena)?;
let read_stamp = transaction.stage()?;
let commit_stamp = transaction.commit(Durability::Flushed)?;
```

`Transaction::rollback` withdraws a stage while keeping the reads and writes, so the transaction can stage again.

## Snapshots

A snapshot pins the state it was taken at until it is dropped, and every view at it holds it, so a view of a snapshot has no verb that writes.
`Snapshot::read_stamp` is the stamp it reads at, which every commit whose `CommitStamp` is at or below it shows.
`Snapshot::save` writes it out as a new database of the same engine, holding exactly the collections and names it records, where RocksDB and LevelDB create the path and the RAM engine writes its image into the directory the path names.

```rust
let snapshot = database.snapshot()?;
let pinned = snapshot.read(people, &[ada], &mut arena)?;
assert_eq!(pinned.get(0), Some(b"Ada".as_slice()));
assert!(commit_stamp.0 <= snapshot.read_stamp().0);
snapshot.save("/var/lib/people-copy")?;
```

## Documents

`Collection::docs` parses what it reads through JsonTape: `get` hands out a `jsontape::Json`, `get_json` the text itself as an `Owned` value, and `set` writes a `jsontape::Json`, while with the `serde` feature `get_as` deserializes into any `DeserializeOwned` type.
The batch verbs hold documents as text both ways and parse nothing on the Rust side.
A field is a top-level key or an RFC 6901 pointer, a write holds one format for the whole batch, named by the type of its entries, and `Modification` chooses between an upsert, an update, an insert, an RFC 6902 patch and an RFC 7386 merge.
A gather converts fields into typed columns, a row per key, handing out scalar columns as slices straight out of the arena beside their validity, conversion and collision bitmaps.

```rust
use ustore::{Column, DocsVerbs, DocumentEntry, FieldAddress, FieldEntry, Json, Modification, Text};

let documents = [DocumentEntry::<Json>::new(ada, r#"{"name":"Ada","born":1815}"#)];
database.docs_write(people, &documents, Modification::Upsert, Durability::Buffered)?;
let died = [FieldEntry::<i64>::new(ada, "died", &1852)];
database.docs_write(people, &died, Modification::Upsert, Durability::Buffered)?;

let names = database.docs_read(people, &[FieldAddress::new(ada, "name")], Text, &mut arena)?;
let paths = database.docs_gist(people, [ada], &mut other_arena)?;
let columns = [Column::new::<u64>("born"), Column::new::<Text>("name")];
let gathered = database.docs_gather(people, &[ada], &columns, &mut arena)?;
let born: &[u64] = gathered.scalars::<u64>(0)?;
```

## Graphs

A graph collection, created with `Modality::Graph`, indexes every edge from both of its vertices, and an `Edge` is laid out as the source, target and identifier keys the ABI puts on its tapes, so a slice of edges crosses as three strided columns.
`graph_read_edges` answers `Neighborhoods`, the degree of every vertex and its edges on one tape, outgoing ones before incoming ones, and `graph_degrees` the degrees alone.
A bare `Role` gives the whole batch one role, as `Roles::Every` does, and a slice of them, as `Roles::Each`, gives every vertex its own.

```rust
use ustore::{Edge, GraphVerbs, Role};

let follows = database.collection_create("follows", Modality::Graph)?;
let [first, second, third, seventh] = [1u64, 2, 3, 7].map(Key::from);
let edges = [Edge::new(first, second), Edge::new(first, third), Edge::with_id(second, third, seventh)];
database.graph_write_edges(follows, &edges, Durability::Buffered)?;

let vertices = [first, third];
let found = database.graph_read_edges(follows, &vertices, Role::Any, &mut arena)?;
for (vertex, run) in vertices.iter().zip(found.iter()) {
    for edge in run.unwrap_or_default() {
        println!("{vertex} meets {:?}", edge.neighbor_of(*vertex));
    }
}
let roles = [Role::Source, Role::Target];
let degrees = database.graph_degrees(follows, &vertices, roles.as_slice(), &mut arena)?;
database.graph_remove_vertices(follows, [first], Role::Any, Durability::Buffered)?;
```

## Receivers & Deferred Calls

The receiver a verb is called on chooses what it sees, where it runs and how it completes, while the verb stays, as [the C rules](https://github.com/unum-cloud/UStore/blob/main/c/README.md#receivers--deferred-calls) describe in full.
A `Database`, a `Transaction` or a `Snapshot` runs the verb on the calling thread, `.on(&executor)` blocks on an executor, and `.deferred(&executor)` answers a `Deferred`.
A `Deferred` is the call's own `Future`: it submits when first polled or waited on, the way Rust futures start, at the address it is pinned at, so its outcome, arguments and inputs live inline.
Refusals the binding catches arrive through the `?` that makes the call, and the library's own arrive at `.await` or `wait`.

```rust
use ustore::*;

async fn read_both(east: &Database, west: &Database, east_executor: &Executor, west_executor: &Executor,
                   people: CollectionID, keys: Vec<Key>) -> Result<(usize, usize)> {
    let mut east_arena = Arena::new_in(east_executor)?;
    let mut west_arena = Arena::new_in(west_executor)?;
    let east_call = east.head().deferred(east_executor).read(people, keys.clone(), &mut east_arena)?;
    let west_call = west.head().deferred(west_executor).read(people, keys, &mut west_arena)?;
    let (east_values, west_values) = futures::join!(east_call, west_call); // or tokio::join!, both pin in place
    Ok((east_values?.len(), west_values?.len()))
}

fn main() -> Result<()> {
    let database = Database::init_ram(&RAMOptions::default())?;
    let people = database.collection_create("people", Modality::Blobs)?;
    database.write(people, &[Entry::new(ada, b"Ada")], Durability::Buffered)?;     // the calling thread
    let executor = Executor::for_ram(&[], 8)?;
    let mut arena = Arena::new_in(&executor)?;                                       // Arena<'_> holds the executor
    let local = database.read(people, &[ada], &mut Arena::new()?)?;                  // the calling thread
    let pooled = database.head().on(&executor).read(people, &[ada], &mut arena)?;   // blocks on the executor
    let call = database.head().deferred(&executor).read(people, vec![ada], &mut arena)?; // Deferred<'_, Read<..>>
    let values = call.wait()?;                                                       // or, inside async: call.await?
    let both = futures::executor::block_on(read_both(&east, &west, &east_executor, &west_executor, people, keys))?;
    Ok(())
}
```

Overlapping work in synchronous code goes through `block_on` of a join, as above, or through a scoped thread that waits on the call, since a `Deferred` never awaited or waited on never runs.
The compiler holds the lifetime rules: a deferred verb takes only owned or `'static` inputs, predicates and progress handlers, since the library reads them after the call is submitted, its result views the arena as a blocking one does, the arena holds the executor that made it, and dropping a submitted `Deferred` waits for its call.
Passing `&[Key]` to a deferred view fails to compile with "a deferred view needs owned inputs".

| Path                     | Allocations |
| :----------------------- | ----------: |
| Executor or arena made   |           0 |
| Blocking call            |           0 |
| Deferred submit and wait |           0 |
| Each callback            |           0 |

## Executors

An `Executor` is the threads that run calls, shared by any number of databases and views, `Send` and `Sync`, and serving one call at a time, so a call submitted while another holds it answers `Status::Contended`.
`Executor::for_ram` spawns threads near the NUMA nodes it names, every node for an empty list, where zero threads take all but one in eight of their cores.
`Executor::for_nvme` spawns the NVMe engine's rings near the memory domains it names, or near the drives under its paths, and `Executor::for_vram` drives the GPUs of one vendor it names, or every GPU of the first vendor for none, one stream each, which it opens and destroys, while `Executor::for_vram_on_streams` adopts the caller's streams instead.
Opening a database blocks, on the executor its options name or the calling thread, dropping a database writes nothing, so `flush` makes its buffered commits durable first, and dropping an executor waits as `Executor::drain` does, as [the C rules](https://github.com/unum-cloud/UStore/blob/main/c/README.md#executors) describe.

```rust
use ustore::{Arena, Executor, Mood, RocksDBOptions};

let executor = Executor::for_ram(&[0], 8)?;            // memory domains, threads
let people = Database::open_rocksdb(&RocksDBOptions { executor: Some(&executor), ..RocksDBOptions::at("/var/lib/people") })?;
let mut arena = Arena::new_in(&executor)?;
let (names, _) = people.head().on(&executor).collection_find("names", &mut arena)?;
let values = people.head().on(&executor).read(names, &[ada, grace], &mut arena)?;
executor.drain(Mood::Parked)?;
```

## Batches and Arenas

Every output whose length scales with the call lands in an `Arena`, and the verb answers a view over it, as [the C rules](https://github.com/unum-cloud/UStore/blob/main/c/README.md#batches-and-arenas) describe.
`Arena::new_in(&executor)` places one where that executor computes, holding the executor so the arena never outlives it, and `Arena::new()` on host pages, and a call rewinds it, keeping its high-water mark, so a steady loop allocates once.
Every verb takes `&mut Arena` last and its result views it, so the call that would overwrite a live result does not compile; to keep one result while making the next, pass the next call another arena.
A `Key` is sixteen bytes ordered as their bytes, converting from `[u8; 16]`, a `UUID`, or a `u64` or `u128` spelled big-endian, and a batch of keys is anything that reads as `&[Key]`.

```rust
let values = database.read(people, &[ada], &mut arena)?;
let others = database.read(people, &[grace], &mut arena)?; // refused while `values` lives
println!("{:?}", values.get(0));

let (mut found, mut fetched) = (Arena::new()?, Arena::new()?); // two live results, two arenas
for &key in database.scan(people, ada, 100, &mut found)?.keys {
    consume(database.read(people, &[key], &mut fetched)?);     // rewinds fetched, never found
}
```

An arena is `Send` and serves one call at a time, so deferred calls in flight together take one each.

## Errors

Every fallible call answers `Result<T, Error>`, where the error holds the `Status` the C ABI reported and the static message beside it, so nothing allocates on the failure path.
`Status` leaves success to `Result`, so an `Error` never holds `Status::Deferred`, and a status a newer library added reads as `Status::Internal`.
`Status`, `Durability`, `Watch`, `Modality` and the other enums print through `Display` and parse through `FromStr` the spellings the C tables own, such as `"not-found"` or `"strict-serializable"`.

```rust
match database.collection_find("nowhere", &mut arena) {
    Err(failure) if failure.status == ustore::Status::NotFound => println!("{}: {failure}", failure.status),
    other => { other?; }
}
println!("{}", ustore::version()); // the library the program runs against, such as "1.0.0"
```

## Apache Arrow

The converters of `c/ustore/arrow.h` view an Arrow column as a call's input in place, and export what a call wrote into an arena as a column in place, living until the arena's next call:

```rust
let columns = [Column::new::<u64>("born"), Column::new::<Text>("name")];
let gathered = database.docs_gather(people, &[ada], &columns, &mut arena)?;
let exported = gathered.to_arrow()?; // one `ArrowExport` per column, viewing the arena
let names = &exported[1];            // `names.schema()` and `names.array()` address any Arrow consumer

let posters = database.collection("posters")?;
let stream = posters.export_arrow()?; // keys as `arrow.uuid` and values as `binary`, a batch per pull
let copies = database.find_or_create("copies", Modality::Blobs)?;
copies.import_arrow(stream, Durability::Buffered)?; // every batch's columns read in place
```

`Collection::export_arrow` writes keys and strings through `keys_view_to_arrow` and `strings_view_to_arrow`, `import_arrow` and `ingest_arrow` read them through `keys_view_from_arrow` and `strings_view_from_arrow`, and `Columns::to_arrow` is `docs_column_view_to_arrow`.
`Values::tape` views a read's values in Arrow's `binary` or `large_binary` layout without a copy.

## Platform Notes

### Engines

`Database::init_ram` starts an empty database in memory alone, `open_ram` serves a directory as one image, `open_rocksdb`, `open_leveldb` and `open_nvme` serve a directory in place, and `view_rocksdb` and `view_nvme` serve one read-only, refusing every change with `Status::ReadOnly`.
`RAMOptions` and `NVMEOptions` take a `cpu_class_ceiling`, the newest `CPUClass` the database's kernels may run as, intersected with the machine, and `describe` reports the class picked as `cpu_class` beside the engine, the isolation and the sizes it estimates.
`clear` empties every collection at once, and `compact` reclaims space through the phases its engine runs, where a `CompactPlan` keeps a stopped compaction open until `compact_plan_finish`.

```rust
use ustore::{DropMode, Engine, Isolation, RAMOptions, RocksDBOptions};

let scratch = Database::init_ram(&RAMOptions { isolation: Isolation::Snapshot, ..RAMOptions::default() })?;
let people = Database::open_rocksdb(&RocksDBOptions::at("/var/lib/people"))?;
assert_eq!(people.describe()?.engine, Engine::RocksDB);
people.clear(DropMode::Values)?;  // keeps every key, emptying its value
people.compact(CompactConfig::default())?; // reclaims what overwritten and removed data still takes
```

### Left Unbound

- Deferred opens, describes, collection creates, transaction and snapshot inits, and cursor pulls: these block whatever receiver you hold, as does every call through a `Collection`.
- The strings cursor, which `export_arrow` drives, and the range removals of `ustore_pages_remove`.
- The progress of a cursor pull, and the lengths a read writes beside its values, which the tape's offsets hold.

### Design

- __`On<'view, Parent, Completion>` makes the receiver a type, not a flag.__
  Each verb is written once, as a provided method of its modality's trait, which every handle and `On` implements, and the parent decides what compiles: only a transaction commits, only the head clears and compacts, and nothing writes through a snapshot.
- __Ragged inputs cross as they lie.__
  A slice of entries passes its keys as a strided run and its values through an accessor over the same records, and owned values cross as a tape, so nothing is staged on the way in.
- __The compiler retires stale views.__
  Every verb takes its arena as `&mut Arena`, blocking or deferred, and no handle keeps an arena of its own.
- __No life of its own.__
  The crate starts no threads and keeps no counts or queues, no call allocates on the Rust side, deferred ones included, callbacks cross as the C struct their handler shape lowers to, and releasing a `Deferred` rides `Drop`, which waits for a submitted call first.
