# UStore for Rust

The crate binds the C ABI in `c/ustore/`, linking the engine library its build script makes through the project's own CMake, and adds no C or C++ of its own.
It needs Rust 1.100 and a C++20 toolchain for the engine, and it brings no async runtime, so a `Deferred` awaits on any executor of async tasks or blocks through `wait`.
Every verb is a batch, and every blocking verb returns a `Result`:

- `Database`, `Transaction` and `Snapshot`: the head, a group of changes, and a point in time.
- `Collection`: a collection by name, with `blobs`, `docs` and `graph` views of one key at a time.
- `Arena`: where outputs land, borrowed by the result that views them.
- `Executor` and `Deferred`: the threads a call runs on, and a call submitted there.

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
A `Collection` resolves its name once, and the first write through a view creates the collection holding that view's modality.

```rust
use ustore::{jsontape, Database, Durability, Edge, Key, RAMOptions, Role};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let database = Database::init_ram(&RAMOptions::default())?;
    let [avatar, titanic] = [1u64, 2].map(Key::from);
    let document = jsontape::parse(br#"{"title":"Avatar","year":2009}"#)?;

    // One transaction writes a blob, a document and an edge, and commits all three or none.
    let mut transaction = database.transaction()?;
    transaction.collection("posters")?.blobs().set(avatar, b"\x89PNG")?;
    transaction.collection("movies")?.docs().set(avatar, &document)?;
    transaction.collection("similar")?.graph().add_edge(avatar, titanic)?;
    transaction.commit(Durability::Buffered)?;

    // Reads at the head see the commit: the poster, the year and the one edge leaving Avatar.
    let mut posters = database.collection("posters")?;
    assert_eq!(posters.blobs().get(avatar)?, Some(b"\x89PNG".to_vec()));
    let mut movies = database.collection("movies")?;
    let movie = movies.docs().get(avatar)?.expect("committed above");
    assert_eq!(movie["year"].as_u64(), Some(2009));
    let mut similar = database.collection("similar")?;
    let edges = similar.graph().read_edges(&[avatar], Role::Source)?;
    assert_eq!(edges, [Edge::new(avatar, titanic)]);
    Ok(())
}
```

Shorthand such as `get` makes a one-off arena per call and copies out of it, while the batch verbs below take an `Arena` and answer views into it.

## Collections

Collections are created under a name and the modality they hold, resolved by name once and addressed by `CollectionID` from then on, and a database holds none until one is created.
A name starting with `_` is refused with `Status::InvalidArgument`, as the engines keep their own system collections there.
`collection_find` answers the identifier and modality of a name, `collection_list` every identifier, and `collection_names` a page of names from a prefix.
`Database::collection_drop` empties a collection of its pairs or values with `DropMode::Pairs` or `DropMode::Values`, or drops it whole with `DropMode::Collection`.
The ABI carries a collection per task, so every batch verb takes a `Collections`: a bare `CollectionID` converts into `Collections::Every`, and `Collections::Each` names one per entry.

```rust
use ustore::{Collections, DropMode, Modality};

let people = database.collection_create("people", Modality::Blobs)?;
let (same, modality) = database.head().collection_find("people", &mut arena)?;
for name in database.head().collection_names("pe", None, 100, &mut arena)?.iter() {
    println!("{name}");
}
let spanned = [people, places];
let values = database.head().read(Collections::Each(&spanned), &[ada, ada], &mut arena)?;
database.collection_drop(people, DropMode::Collection)?;
```

## Reads and Writes

An `Entry` pairs a key with borrowed bytes, laid out so a slice of them crosses as it stands: the keys as one strided run, and the values through an accessor over the same entries.
A write also takes owned keys beside a `stringtape::BytesTape<u32>` or `BytesTape<u64>`, which the library reads as it stands.
A read hands back `Values`, which addresses the tape in the arena rather than copying out of it, reports a missing key as `None`, and exports the whole batch through `Values::tape` in Arrow's `binary` or `large_binary` layout.
A write carries values and nothing else, so an empty slice writes an empty value, and `remove` and `remove_range` remove keys in batches of their own.

```rust
use ustore::{Arena, Durability, Entry, Key, Modality};

let mut arena = Arena::new()?;
let [ada, grace, kent] = [1u64, 2, 3].map(Key::from);
let entries = [Entry::new(ada, b"Ada"), Entry::new(grace, b"Grace")];
database.head().write(people, &entries, Durability::Buffered)?;

let values = database.head().read(people, &[ada, grace, kent], &mut arena)?;
assert_eq!(values.get(0), Some(b"Ada".as_slice()));
assert_eq!(values.get(2), None);
let headers = database.head().read_slices(people, &[ada, grace], ..2, &mut arena)?;
database.head().remove(people, &[grace], Durability::Buffered)?;
```

## Scans

Scans, cursors and measures take a `KeyRange` of a start key, inclusive, an end key, exclusive, and a `ScanDirection`, where a key alone walks up from it and `start..end` walks up between them.
`scan` lists up to the count it wants, `sample` draws keys at random, and `measure` bounds the keys and bytes a range holds without walking it; `docs_sample`, `pages_sample` and `graph_sample_vertices` serve the other modalities.
A `Cursor` walks one collection page by page, each pull resuming past the last key the one before it handed out, and a page shorter than wanted exhausts it for good.
`Database::changes_cursor` opens a feed of the changes committed after a `CommitStamp`, oldest first, pinning that stamp as a snapshot does.

```rust
use ustore::{KeyRange, Matching, ScanDirection};

let first = database.head().scan(people, Key::from(0u64), 100, &mut arena)?.keys;
let measured = database.head().measure(people, ada..kent, &mut arena)?;
let largest = KeyRange { direction: ScanDirection::Descending, ..KeyRange::default() };
let last = database.head().scan(people, largest, 10, &mut arena)?.keys;

let mut cursor = database.head().on(&executor).cursor(people, Key::from(0u64), Matching::default())?;
loop {
    let page = cursor.next_pairs(1024, &mut arena)?;
    if page.keys().len() < 1024 { break; }
}
```

### Progress and Cancellation

The configs of `compact`, `save` and the scans carry an optional progress handler, an `FnMut(finished, total) -> ControlFlow<()>`, called on whichever thread advances the work, never on two at once and never after the call is final.
Answering `ControlFlow::Break(())` cancels the call, which fails with `Status::Cancelled` and publishes nothing, a cancelled save removing what it wrote.
A handler must not panic, since unwinding out of the library's `extern "C"` frame aborts the process.

```rust
use std::ops::ControlFlow;
use std::time::{Duration, Instant};

let deadline = Instant::now() + Duration::from_secs(30);
let before_the_deadline = move |_finished: usize, _total: usize| match Instant::now() < deadline {
    true => ControlFlow::Continue(()),
    false => ControlFlow::Break(()),
};
let config = SaveConfig { path: "/var/lib/people-copy", progress: Some(Box::new(before_the_deadline)) };
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

```rust
use ustore::Watch;

let mut transaction = database.transaction()?;
transaction.view(Watch::Keys).write(people, &entries, Durability::Buffered)?;
let peek = transaction.view(Watch::None).read(people, &[ada], &mut arena)?;
let read_stamp = transaction.stage()?;
let commit_stamp = transaction.commit(Durability::Flushed)?;
```

`Transaction::rollback` withdraws a stage while keeping the reads and writes, so the transaction can stage again.

## Snapshots

A snapshot pins the state it was taken at until it is dropped, and every view at it borrows it, so a view of a snapshot has no verb that writes.
`Snapshot::read_stamp` is the stamp it reads at, which every commit whose `CommitStamp` is at or below it shows.
`Snapshot::save` writes it out as a new database of the same engine, holding exactly the collections and names it records, where RocksDB and LevelDB create the path and the RAM engine writes its image into the directory the path names.

```rust
let snapshot = database.snapshot()?;
let pinned = snapshot.view().read(people, &[ada], &mut arena)?;
assert_eq!(pinned.get(0), Some(b"Ada".as_slice()));
assert!(commit_stamp.0 <= snapshot.read_stamp().0);
snapshot.save("/var/lib/people-copy")?;
```

## Documents

`Collection::docs` parses what it reads through JsonTape: `get` hands out a `jsontape::Json`, `get_json` the text itself, and `set` writes a `jsontape::Json`, while with the `serde` feature `get_as` deserializes into any `DeserializeOwned` type.
The batch verbs carry documents as text both ways and parse nothing on the Rust side.
A field is a top-level key or an RFC 6901 pointer, a write carries one format for the whole batch, named by the type of its entries, and `Modification` chooses between an upsert, an update, an insert, an RFC 6902 patch and an RFC 7386 merge.
A gather converts fields into typed columns, a row per key, handing out scalar columns as slices straight out of the arena beside their validity, conversion and collision bitmaps.

```rust
use ustore::{Column, DocumentEntry, FieldAddress, FieldEntry, Json, Modification, Text};

let documents = [DocumentEntry::<Json>::new(ada, r#"{"name":"Ada","born":1815}"#)];
database.head().docs_write(people, &documents, Modification::Upsert, Durability::Buffered)?;
let died = [FieldEntry::<i64>::new(ada, "died", &1852)];
database.head().docs_write(people, &died, Modification::Upsert, Durability::Buffered)?;

let names = database.head().docs_read(people, &[FieldAddress::new(ada, "name")], Text, &mut arena)?;
let paths = database.head().docs_gist(people, [ada], &mut other_arena)?;
let columns = [Column::new::<u64>("born"), Column::new::<Text>("name")];
let gathered = database.head().docs_gather(people, &[ada], &columns, &mut arena)?;
let born: &[u64] = gathered.scalars::<u64>(0)?;
```

## Graphs

A graph collection, created with `Modality::Graph`, indexes every edge from both of its vertices, and an `Edge` is laid out as the source, target and identifier keys the ABI puts on its tapes, so a slice of edges crosses as three strided columns.
`graph_read_edges` answers `Neighborhoods`, the degree of every vertex and its edges on one tape, outgoing ones before incoming ones, and `graph_degrees` the degrees alone.
`Roles::Every` gives the whole batch one role and `Roles::Each` gives every vertex its own.

```rust
use ustore::{Edge, Role, Roles};

let follows = database.collection_create("follows", Modality::Graph)?;
let [first, second, third, seventh] = [1u64, 2, 3, 7].map(Key::from);
let edges = [Edge::new(first, second), Edge::new(first, third), Edge::with_id(second, third, seventh)];
database.head().graph_write_edges(follows, &edges, Durability::Buffered)?;

let vertices = [first, third];
let found = database.head().graph_read_edges(follows, &vertices, Roles::Every(Role::Any), &mut arena)?;
for (vertex, run) in vertices.iter().zip(found.iter()) {
    for edge in run.unwrap_or_default() {
        println!("{vertex} meets {:?}", edge.neighbor_of(*vertex));
    }
}
database.head().graph_remove_vertices(follows, [first], Roles::Every(Role::Any), Durability::Buffered)?;
```

## Receivers & Deferred Calls

The receiver a verb is called on chooses what it sees, where it runs and how it completes, while the verb stays, as [the C rules](https://github.com/unum-cloud/UStore/blob/main/c/README.md#receivers--deferred-calls) describe in full.
`database.head()`, `transaction.view(watch)` and `snapshot.view()` run on the calling thread, `.on(&executor)` blocks on an executor, and `.deferred(&executor)` answers a `Deferred` once the call is submitted.
The borrow checker carries the lifetime rules: a deferred verb takes only owned inputs, since the library reads them after it returns, while its result borrows the arena as a blocking one does.
A `Deferred` is `IntoFuture`, so `.await` polls its status and asks to be polled again, `wait` blocks in `ustore_outcome_wait`, and dropping an unfinished one waits for its call.

```rust
use ustore::stringtape::BytesTape;

let deferred = database.head().deferred(&executor);
let names: BytesTape<u32> = [b"Ada".as_slice(), b"Grace"].into_iter().collect();
let (keys, names) = deferred.write(people, (vec![ada, grace], names), Durability::Buffered)?.await?;

let reading = deferred.read(people, vec![ada, grace], &mut arena)?;
let values = reading.await?; // or: reading.wait()?
assert_eq!(values.get(0), Some(b"Ada".as_slice()));
let commit_stamp = transaction.deferred(&executor).commit(Durability::Buffered)?.await?;
```

Passing `&[Key]` to a deferred view fails to compile with "a deferred view needs owned inputs".

## Executors

An `Executor` is the threads that run calls, shared by any number of databases and views, `Send` and `Sync`, and serving one call at a time, so a call submitted while another holds it answers `Status::Contended`.
`Executor::for_ram` spawns threads near the NUMA nodes it names, every node for an empty list, where zero threads take 7/8 of their cores.
`Executor::for_nvme` spawns the NVMe engine's rings near the memory domains it names, or near the drives under its paths, and `Executor::for_vram` drives one CUDA device.
Opening a database blocks, on the executor its options name or the calling thread, and dropping an executor waits as `Executor::drain` does, as [the C rules](https://github.com/unum-cloud/UStore/blob/main/c/README.md#executors) describe.

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
`Arena::new_in(&executor)` places one where that executor computes and `Arena::new()` on host pages, and a call rewinds it, keeping its high-water mark, so a steady loop allocates once.
Every verb takes `&mut Arena` last and its result borrows it, so the call that would overwrite a live result does not compile; to keep one result while making the next, pass the next call another arena.
A `Key` is sixteen bytes ordered as their bytes, converting from `[u8; 16]`, a `UUID`, or a `u64` or `u128` spelled big-endian, and a batch of keys is anything that reads as `&[Key]`.

```rust
let values = database.head().read(people, &[ada], &mut arena)?;
let others = database.head().read(people, &[grace], &mut arena)?; // refused while `values` lives
println!("{:?}", values.get(0));

let (mut found, mut fetched) = (Arena::new()?, Arena::new()?);     // two live results, two arenas
for &key in database.head().scan(people, ada, 100, &mut found)?.keys {
    consume(database.head().read(people, &[key], &mut fetched)?);  // rewinds fetched, never found
}
```

An arena is `Send` and never shared, so deferred calls submitted together take one each.

## Errors

Every fallible call answers `Result<T, Error>`, where the error carries the `Status` the C ABI reported and the static message beside it, so nothing allocates on the failure path.
`Status` leaves success to `Result`, so an `Error` never carries `Status::Deferred`, and a status a newer library added reads as `Status::Internal`.
`Status`, `Durability`, `Watch`, `Modality` and the other enums print through `Display` and parse through `FromStr` the spellings the C tables own, such as `"not-found"` or `"strict-serializable"`.

```rust
match database.head().collection_find("nowhere", &mut arena) {
    Err(failure) if failure.status == ustore::Status::NotFound => println!("{}: {failure}", failure.status),
    other => { other?; }
}
println!("{}", ustore::version()); // the library the program runs against, such as "1.0.0"
```

## Platform Notes

### Engines

`Database::init_ram` starts an empty database in memory alone, `open_ram` serves a directory as one image, `open_rocksdb`, `open_leveldb` and `open_nvme` serve a directory in place, and `view_rocksdb` serves one read-only, refusing every change with `Status::ReadOnly`.
`RAMOptions` and `NVMEOptions` take a `cpu_class_ceiling`, the newest `CPUClass` the database's kernels may run as, intersected with the machine, and `describe` reports the class picked as `cpu_class` beside the engine, the isolation and the sizes it estimates.
`clear` empties every collection at once, and `compact` reclaims space through the phases its engine runs, where a `CompactPlan` keeps a stopped compaction open until `compact_plan_finish`.

```rust
use ustore::{DropMode, Engine, Isolation, RAMOptions, RocksDBOptions};

let scratch = Database::init_ram(&RAMOptions { isolation: Isolation::Snapshot, ..RAMOptions::default() })?;
let people = Database::open_rocksdb(&RocksDBOptions::at("/var/lib/people"))?;
assert_eq!(people.describe()?.engine, Engine::RocksDB);
people.clear(DropMode::Values)?;  // keeps every key, emptying its value
people.compact(None)?;            // reclaims what overwritten and removed data still takes
```

### What This Crate Does Not Expose

- Deferred opens, describes, collection creates, transaction and snapshot inits, and cursor pulls: these block whatever receiver you hold, as does every call through a `Collection`.
- The strings cursor, which `export_arrow` drives, and the range removals of `ustore_pages_remove`.
- The progress of a cursor pull, and the lengths a read writes beside its values, which the tape's offsets carry.

### Design

- __`On<'view, Parent, Completion>` makes the receiver a type, not a flag.__
  Each verb is written once, generic over the completion, and the parent decides what compiles: only a transaction commits, only the head clears and compacts, and nothing writes through a snapshot.
- __Ragged inputs cross as they lie.__
  A slice of entries lends its keys as a strided run and its values through an accessor over the same records, and owned values cross as a tape, so nothing is staged on the way in.
- __The borrow checker retires stale views.__
  Every verb borrows its arena as `&mut Arena`, blocking or deferred, and no handle keeps an arena of its own.
- __No life of its own.__
  The crate starts no threads and keeps no counts or queues, a blocking call allocates nothing, and releasing a `Deferred` rides `Drop`, which waits for the call first.
