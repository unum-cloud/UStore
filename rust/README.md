# UStore for Rust

## Installation

```sh
cargo add ustore
```

The crate links the engine library CMake builds and consumes it through the C ABI in `c/ustore/`.
`build.rs` compiles no C or C++ of its own: it configures CMake, installs the libraries into `OUT_DIR` and links `ustore_static`, the archive that carries the objects of the small libraries it builds on, with the archives of the engines its features enable.
`USTORE_LIBRARY_DIR` names a directory already holding those archives, such as the `lib/` of a prefix CMake installed with `USTORE_BUNDLE_SIBLINGS=OFF`, and skips the CMake build.

The in-memory `ram` engine always builds, the default `rocksdb` and `leveldb` features add theirs to the one library, and the `nvme` feature adds the NVMe engine when asked for.
Without a feature, its engine's `Database::open_*` fails with `Status::Unsupported`.

```sh
cargo test                                        # RAM, RocksDB and LevelDB
cargo test --no-default-features --features nvme  # RAM and NVMe alone
cargo test --all-features                         # every engine, and documents read into `serde` types
```

The crate re-exports the two it builds on: `jsontape`, which parses documents, and `stringtape`, whose tapes carry batches of values in and whose views hand them out.

## Quickstart

A `Database` owns its directory, and a `Collection` addresses one collection in it by name, as the Python binding does with `db['movies']`.

```rust
use ustore::{jsontape, Arena, Database, Durability, Edge, Key, RAMOptions, Role};

let database = Database::init_ram(&RAMOptions::default())?;
let [avatar, titanic] = [1u64, 2].map(Key::from);

database.collection("movies")?.docs().set(avatar, &jsontape::parse(br#"{"title":"Avatar","year":2009}"#)?)?;
database.collection("posters")?.blobs().set(avatar, &png_bytes)?;
database.collection("similar")?.graph().add_edge(avatar, titanic)?;

let mut movies = database.collection("movies")?;
let description = movies.docs().get(avatar)?.expect("written above");
assert_eq!(description["year"].as_u64(), Some(2009));

let mut posters = database.collection("posters")?;
let thumbnail: Option<Vec<u8>> = posters.blobs().get(avatar)?;

let mut similar = database.collection("similar")?;
let recommendations: Vec<Edge> = similar.graph().read_edges(&[avatar], Role::Source)?;

let mut arena = Arena::new()?;
let mut pictures = database.collection("pictures")?;
let images = pictures.sample(1000, &mut arena)?;
let tape = images.values().tape();

let mut transaction = database.transaction()?;
transaction.collection("movies")?.docs().set(titanic, &jsontape::parse(br#"{"title":"Titanic","year":1997}"#)?)?;
transaction.collection("similar")?.graph().add_edge(titanic, avatar)?;
transaction.collection("posters")?.blobs().set(titanic, &png_bytes)?;
transaction.commit(Durability::Buffered)?;
```

A collection resolves its name through its view once, and every verb goes through a view of one modality: `blobs`, `docs` or `graph`.
The first write through a view creates a missing collection holding that view's modality, a read of a missing one fails with `Status::NotFound`, and a view of another modality fails with `Status::InvalidArgument`.
`sample` draws keys at random and reads their values through the same view, so a sample holds both.

A collection keeps no arena.
A call with no arena to pass, such as `get`, `contains`, the document reads and the graph lookups, makes a one-off arena through `ustore_arena_init`, copies what it read out of it, and frees it with the call: slower than reusing an arena, and stateless.
The batch reads, `Blobs::read` and `Collection::sample`, take the `Arena` they land in and answer views into it.
Writes land `Durability::Buffered`, and every call through a collection runs on the calling thread.
`Transaction::collection` reads and writes inside the transaction, while its name resolves at the head.
Collections are created and dropped nowhere but at the head, so a first write from inside a transaction creates the collection there at once, and a name never joins the transaction's conflicts.
Its collections borrow the transaction, so they are no longer used by the time `Transaction::commit` takes it mutably.

## Receivers & Deferred Calls

A call addresses the head of the database, a transaction, or a snapshot, and never two of them.
`On` is that view: a read passes the `ustore_view_t` it holds, and a write passes its transaction and whether that watches the write.
A view is `Copy` and borrows the database, transaction or snapshot it came from, which is its `Parent` type, so a view of a snapshot has no verb that writes, and only a view of a transaction stages and commits.

The receiver also decides where a verb runs and how it completes, while the verb and its arguments stay the same:

- `database.head().read(..)` runs on the calling thread.
- `database.head().on(&executor).read(..)` blocks on the executor, after the calls submitted there before it.
- `database.head().deferred(&executor).read(..)` submits the call and answers a `Deferred`.

`Transaction::on` and `Transaction::deferred` are the same for the view of a transaction watching its keys, and `Snapshot::on` and `Snapshot::deferred` for the view of a snapshot.

```rust
use ustore::Watch;

let mut transaction = database.transaction()?;
transaction.view(Watch::Keys).write(people, &entries, Durability::Buffered)?;
let read_stamp = transaction.stage()?;
let commit_stamp = transaction.commit(Durability::Flushed)?;

let snapshot = database.snapshot()?;
let pinned = snapshot.view().read(people, &[ada], &mut arena)?;
assert!(commit_stamp.0 <= snapshot.read_stamp().0);
```

`Watch` says whether the calls of that view record the keys they touch.
It changes what a call does only at `Isolation::Snapshot`, where `Watch::None` lets a transaction commit over a concurrent writer instead of conflicting with it; a strictly serializable transaction puts every read it makes into the one serial order whatever the view says.
Only a transaction watches anything, so the head of a database and a snapshot carry no such argument.

A transaction conflicts rather than blocks, and reports `Status::WriteConflict`, `ReadConflict` or `PhantomConflict` at its commit.
It keeps its contents whatever the outcome, so `Transaction::reset` discards them and reopens it on the newest state for another try.
`Transaction::stage` is the first phase of a two-phase commit and answers a `ReadStamp`, the stamp the transaction reads at and its commit validates against.
`Transaction::commit` answers a `CommitStamp`, the commit's position in commit order, which every snapshot whose `Snapshot::read_stamp` is at or above it sees.
The two are distinct types, so neither is compared with the other by accident.
`Transaction::rollback` withdraws a stage while keeping the reads and writes, so the transaction can stage again.
A snapshot is a handle the database hands out, pinning the state it was taken at until it is dropped, and every view at it borrows it; `Snapshot::save` writes it out as a database of its own.

`On::deferred` makes a `DeferredOn`, whose verbs return once their call is submitted and answer a `Deferred`.
Every verb keeps its name and its arguments, the transaction verbs `stage`, `commit`, `rollback` and `reset`, `save`, `clear`, `compact` and `collection_drop` included, while the inputs it takes must be owned, since the library reads them after the verb returns.
A deferred view takes keys or edges in an array or a `Vec`, keys beside a `BytesTape` of values or a `CharsTape` of JSON, paths in a `CharsTape`, columns in a `Vec<Column<'static>>`, names and save paths as a `String`, and `'static` collections, roles, fields, names and progress handlers, and a borrow of any of them does not compile.
It borrows the arena as a blocking view does, so the result of a deferred read borrows it too.

```rust
use ustore::stringtape::BytesTape;

let deferred = database.head().deferred(&executor);
let names: BytesTape<u32> = [b"Ada".as_slice(), b"Grace"].into_iter().collect();
let (keys, names) = deferred.write(people, (vec![ada, grace], names), Durability::Buffered)?.await?;

let reading = deferred.read(people, vec![ada, grace], &mut arena)?;
let values = reading.await?; // or: reading.wait()?
assert_eq!(values.get(0), Some(b"Ada".as_slice()));

// submit four scans, each into an arena of its own, then await each
let mut arenas = (0..4).map(|_| Arena::new_in(&executor)).collect::<Result<Vec<_>, _>>()?;
let scans = arenas.iter_mut().zip(starts)
    .map(|(arena, start)| deferred.scan(people, start, 1000, arena)).collect::<Result<Vec<_>, _>>()?;
for scan in scans { consume(scan.await?); }

transaction.deferred(&executor).write(people, (keys, names), Durability::Buffered)?.await?;
let commit_stamp = transaction.deferred(&executor).commit(Durability::Buffered)?.await?;
```

A `Deferred` is `IntoFuture`, so `.await` runs inside an `async` block on any executor of async tasks.
The crate brings none and needs none beyond `std`, where a dozen lines drive one:

```rust
use std::future::Future;
use std::pin::pin;
use std::task::{Context, Poll, Waker};

fn block_on<Output>(task: impl Future<Output = Output>) -> Output {
    let mut task = pin!(task);
    let mut context = Context::from_waker(Waker::noop());
    loop {
        if let Poll::Ready(output) = task.as_mut().poll(&mut context) {
            return output;
        }
    }
}

let values = block_on(async { deferred.read(people, vec![ada], &mut arena)?.await })?;
```

`Deferred::status` reads the call's status through `ustore_outcome_status` without blocking: `Err(Status::Deferred)` while the call is not final, `Ok(())` once it succeeded, and its failure otherwise.
`Deferred::wait` blocks until the call is final through `ustore_outcome_wait`.
Every poll of an awaited `Deferred` reads the status the same way, and one not yet final asks to be polled again, since the library has no wake-up, while `wait` blocks without spinning.
Once final, a call hands back what the same blocking call would: a read its `Values` borrowing the arena, writes and removals their inputs as they are, a stage its `ReadStamp`, and a commit its `CommitStamp`.
A call the binding refuses before it reaches the library, such as a tape holding fewer values than keys, fails at the verb itself.
A call the library refuses at submission, such as one finding the executor's deferred table full with `Status::Contended`, is final at once and fails at `.await` or `wait`.
A transaction serves one call at a time, so a call on a transaction serving an unfinished deferred one answers `Status::Contended` as well.

The crate starts no thread and keeps no count of its own.
A deferred call allocates one box holding its outcome, owned inputs and output slots until it is final, while the argument struct is not among them, since submission copies it.
Dropping an unfinished `Deferred` waits for the call, then frees the box, and a panic its progress handler raised resurfaces from `wait`, `.await`, or a drop outside unwinding.
A forgotten one leaks the box rather than lending the library memory it would reuse, and freeing the database, transaction, snapshot or arena the call names waits in C for every deferred call naming it.
`Deferred` is `Send` whenever its inputs are, so a scoped thread waits off the calling thread, as `std::thread::scope(|scope| scope.spawn(move || reading.wait()).join())` does.

## Executors

A view without an executor runs every call on the calling thread.
An `Executor` is the threads that run calls, shared by any number of databases and views, and is `Send` and `Sync`.
Calls on one executor start in submission order, and a blocking call runs alone: after every call submitted before it finishes, and before any submitted after it starts.

```rust
use ustore::{Arena, Executor, Mood, RocksDBOptions};

let executor = Executor::for_ram(&[], 8, 256)?;           // memory domains, threads, deferred_limit
let near = Executor::for_ram(&[0], 0, 0)?;                // 7/8 of the cores local to NUMA node 0
let shared = unsafe { Executor::on_pool(&[], pool, 0)? }; // borrows a ForkUnion pool's threads
let people = Database::open_rocksdb(&RocksDBOptions { executor: Some(&executor), ..RocksDBOptions::at("/var/lib/people") })?;
let mut arena = Arena::new_in(&executor)?;
let (names, _) = people.head().on(&executor).collection_find("names", &mut arena)?;
let values = people.head().on(&executor).read(names, &[ada, grace], &mut arena)?;
executor.drain(Mood::Parked)?;
```

`Executor::for_ram` spawns the threads for the RAM, RocksDB and LevelDB engines across the compute domains local to the memory domains it names, NUMA nodes as the operating system numbers them, where an empty list derives every domain the process may allocate on.
The scratch of each thread and every arena the executor makes live on those domains, and an identifier no domain carries is refused with `Status::InvalidArgument`.
Zero threads take 7/8 of the cores those domains carry, leaving the rest to the driver thread, to blocking calls and to the background work of RocksDB and LevelDB, and a zero `deferred_limit` takes 256.
`Executor::on_pool` borrows the threads of a ForkUnion `fu_pool_t`, so several libraries in one process share them, and is `unsafe`, since the pool must outlive the executor.
`Executor::for_nvme` spawns the threads of the NVMe engine, one I/O ring apiece, and its `IOOptions` tell every database opened on it how to move bytes: the transport, the channels and their buffers.
`Executor::for_vram` drives one CUDA device through one stream, its own or one the caller lends, failing with `Status::Unsupported` in a library built without CUDA, and is `unsafe`, since a lent stream must outlive the executor; `NVMEOptions::compactions` places an NVMe database's merges on it.
`On::on` names the executor and keeps the view blocking, so its verbs answer exactly as they do on the calling thread.
`Executor::drain` waits for every call submitted before the drain began and leaves the threads in `Mood::Parked` or `Mood::Spinning` whenever they next go idle, and dropping the executor drains it.

Opening a database blocks, and `RAMOptions`, `RocksDBOptions`, `LevelDBOptions` and `NVMEOptions` name the executor the open runs on, where `None` opens on the calling thread.
The NVMe engine reads the `IOOptions` of its executor once, at the open, so the executor may be dropped before the database.

## Arenas

Every output whose length scales with the call, such as values, presence bits, offsets, keys, degrees, edges, bounds, names and columns, lands in an `Arena`, and the verb answers a view over it without copying.
`Arena::new_in(&executor)` places one where that executor computes, decided once, and `Arena::new()` makes host pages for the calling thread; both call `ustore_arena_init`, and dropping one calls `ustore_arena_free`.
A call rewinds its arena and the arena keeps its high-water mark, so a steady loop allocates once.
`Arena::describe` reports where the outputs live, and a view over device memory is refused with `Status::Device` rather than read on the host.
A read through `&mut arena` hands back a result that borrows it, so a value read out of an arena can neither outlive it nor survive the call that would overwrite it.
Every verb takes its arena last, after its inputs and the config that shapes the work, such as the `BlobsScanConfig` of a scan.
A config converts from its most common knob, so `scan(people, ada, 100, &mut arena)` wants 100 keys, and `BlobsScanConfig { matching, ..BlobsScanConfig::from(100) }` reaches every other knob.
Writes export nothing and take no arena, and fixed-size reports, such as stamps, a `Measurement`, a `Description` or a resolved collection, come back as plain values.

The library rewinds an arena on every call through it, so results from one call stop being readable the moment the next one runs.
That rule is the `&mut Arena` every verb takes, blocking or deferred: while a result is alive the arena stays mutably borrowed, and the call that would overwrite it does not compile.

```rust
let values = database.head().read(people, &[ada], &mut arena)?;
let others = database.head().read(people, &[grace], &mut arena)?; // refused while `values` lives
println!("{:?}", values.get(0));

let (mut found, mut fetched) = (Arena::new()?, Arena::new()?);     // two live results, two arenas
for &key in database.head().scan(people, ada, 100, &mut found)?.keys {
    consume(database.head().read(people, &[key], &mut fetched)?);  // rewinds fetched, never found
}
```

Reuse the arena by letting the result fall out of scope: the next call rewinds it and keeps its memory.
To keep one result while making the next, pass the next call another arena.
An arena serves one call at a time, which is why it is `Send` and never shared, and why deferred calls submitted together take one arena each.
The library never frees an arena under a call: dropping one waits for the calls writing into it, and a call naming an arena that serves another call finishes with `Status::Contended`, so even a forgotten `Deferred` leaves it safe.

## Keys

A `Key` is a `UUID`, sixteen bytes ordered as their bytes, and converts from `[u8; 16]`, from a `UUID`, or from a `u64` or `u128` spelled big-endian, so keys made from integers order as the integers.
`Key::next` is the key right after one in their order, where a scan resumes past it, and `key_unknown` is the key that stands for no key, every bit of it set.
A `CollectionID` is a UUIDv7 minted when a collection is created, and the nil one names none, as a database holds no collection until one is created.
A batch of keys is anything that reads as `&[Key]`: an array, a `Vec`, or a borrow of either.

```rust
let [ada, grace, kent] = [1u64, 2, 3].map(Key::from);
assert!(ada < grace && grace.next() == kent);
println!("{kent}"); // 00000000-0000-0000-0000-000000000003
```

## Errors

Every fallible call answers `Result<T, Error>`, where the error carries the `Status` the C ABI reported and the static message beside it, so nothing allocates on the failure path.
A failure the binding catches before the call crosses the boundary reports the status the engine would have used.
`Status` carries the C enumerators' values and leaves success to `Result`, so an `Error` never carries `Status::Deferred`, which only `Deferred::status` reports, and a status a newer library added reads as `Status::Internal`.
`Status`, `Mood`, `Durability`, `Watch`, `DropMode`, `Isolation`, `Transport`, `Engine`, `Modality`, `Role` and `Modification` print through `Display` and parse through `FromStr` the spellings the C tables own, such as `"not-found"` or `"strict-serializable"`, so nothing is compared as a string.
`ustore::version()` names the library the program runs against, such as `"1.0.0"`.

```rust
match database.head().collection_find("nowhere", &mut arena) {
    Err(failure) if failure.status == ustore::Status::NotFound => println!("{}: {failure}", failure.status),
    other => { other?; }
}
```

## Engines & Lifecycle

The constructor names the engine and what it does with a directory: `init_ram` starts an empty database in memory alone, `open_<engine>` serves a directory in place, and `view_rocksdb` serves one read-only.
An engine the build left out fails to open with `Status::Unsupported`.

```rust
use ustore::{DropMode, Engine, Isolation, LevelDBOptions, RAMOptions, RocksDBOptions};

let scratch = Database::init_ram(&RAMOptions { isolation: Isolation::Snapshot, ..RAMOptions::default() })?;
let imaged = Database::open_ram("/var/lib/imaged", &RAMOptions::default())?;
let people = Database::open_rocksdb(&RocksDBOptions::at("/var/lib/people"))?;
let replica = Database::view_rocksdb(&RocksDBOptions::at("/var/lib/replica"))?;
let places = Database::open_leveldb(&LevelDBOptions::at("/var/lib/places"))?;

let description = people.describe()?;
assert_eq!(description.engine, Engine::RocksDB);
println!("{} keys in {} bytes on disk", description.keys, description.disk_bytes);

people.clear(DropMode::Values)?;  // keeps every key, emptying its value
people.compact(None)?;            // reclaims what overwritten and removed data still takes
people.snapshot()?.save("/var/lib/people-copy")?;
```

`open_ram` loads the image its directory holds, if any, and rewrites it whole on a flushed commit and on close.
A database `view_rocksdb` opened reads the state its directory held at open and refuses every call that would change it with `Status::ReadOnly`: writes, removes, transactions, collection creates and drops, clears and compactions, while snapshots and their saves still serve.
`describe` answers the engine, the isolation transactions see, the keys across every collection, and the bytes the data takes in memory and on disk, where every size is the engine's estimate and zero where it keeps none.

`clear` empties every collection at once, as `collection_drop` empties one: `DropMode::Pairs` keeps the collections and their names, `DropMode::Collection` drops every collection with its name, and `DropMode::Values` keeps the keys where the engine can.
The RAM and RocksDB engines refuse to drop the collections while a snapshot is open, with `Status::InvalidArgument`, since the snapshot belongs to its holder.
`compact` reclaims space one collection at a time: RocksDB and LevelDB flush and compact their trees, and the RAM engine frees the versions no reader needs and rewrites its image.
`Snapshot::save` writes the snapshot out as a new database of the same engine, holding exactly the collections and names the snapshot records, which that engine's open then serves.
RocksDB and LevelDB create the path, while the RAM engine writes its image into the existing directory the path names.
`clear` and `compact` are verbs of the head, and `save` of a snapshot, so each runs on an executor or defers as every other verb does.

## Progress & Cancellation

The configs of `compact`, `save` and the scans carry an optional progress handler, an `FnMut(finished, total) -> ControlFlow<()>`, called with the units finished so far and the units the call finishes if it runs to the end.
Answering `ControlFlow::Break(())` cancels the call, which fails with `Status::Cancelled` and publishes nothing.

```rust
use std::ops::ControlFlow;
use std::time::{Duration, Instant};

let deadline = Instant::now() + Duration::from_secs(30);
let mut before_the_deadline = move |_finished: usize, _total: usize| match Instant::now() < deadline {
    true => ControlFlow::Continue(()),
    false => ControlFlow::Break(()),
};
let config = SaveConfig { path: "/var/lib/people-copy", progress: Some(&mut before_the_deadline) };
match snapshot.save(config) {
    Err(failure) if failure.status == ustore::Status::Cancelled => println!("the copy was removed"),
    other => other?,
}
```

The handler arrives on whichever thread advanced the work, never on two at once and never after the call is final, so it is `FnMut` and `Send`, and carries a counter or a deadline without a lock of its own.
A save counts the bytes of the keys and values it wrote, and a cancelled one removes what it wrote, as a failed one does.
A compaction counts the bytes of the collections compacted where the engine measures them, as RocksDB does, and one unit per collection elsewhere, and a cancelled one stops between collections, keeping what it compacted.
A panic inside the handler cancels the call as well, since unwinding out of the C frame would abort, and resurfaces once the call is final: from the blocking verb, or from `wait`, `.await` or the drop of a `Deferred`.
A deferred view takes only a `'static` handler, since the library calls it after the verb returns.

## Collections & Names

Collections are created under a name and the modality they hold, resolved by name once and addressed by identifier from then on, and dropping one forgets its name.
Each engine keeps the names in the system collection `_names` and every collection's modality and name in `_collections`, which no identifier reaches, so a name starting with `_` is refused with `Status::InvalidArgument`.
A name is read through a view as a value is, so a watching transaction conflicts with a concurrent create or drop of a name it resolved, while a listing describes the database and reads the view's snapshot or the head.

```rust
use ustore::DropMode;

let people = database.collection_create("people", Modality::Blobs)?;
let (same, modality) = database.head().collection_find("people", &mut arena)?;
let every = database.head().collection_list(&mut arena)?;

for name in database.head().collection_names("pe", None, 100, &mut arena)?.iter() {
    println!("{name}");
}

database.collection_drop(people, DropMode::Collection)?;
```

A database holds no collection until one is created, and `Database::collection_drop` empties a collection of its pairs or values with `DropMode::Pairs` or `DropMode::Values`, or drops it whole with `DropMode::Collection`.
Walks paginate rather than hold an iterator over concurrent state: a cursor resumes past the last key it handed out, a scan from `Key::next` of the last key it found, and a name walk from a name.

The ABI carries a collection per task, so a batch spans as many collections as it has entries.
Every batch verb takes a `Collections`, and a bare `CollectionID` converts into `Collections::Every`, which is the common case written the short way.

```rust
use ustore::Collections;

let spanned = [people, places];
let entries = [Entry::new(ada, b"Ada"), Entry::new(ada, b"Kent")];
database.head().write(spanned.as_slice(), &entries, Durability::Buffered)?;

let values = database.head().read(Collections::Each(&spanned), &[ada, ada], &mut arena)?;
database.head().write(people, &entries, Durability::Buffered)?; // one for all of them
```

A `Collections::Each` whose length is not the length of the batch is refused with `Status::InvalidArgument` before the call crosses the boundary.

## Binary Values

Beneath the collections, every verb takes a batch, names its collections by identifier, and hands back what its call produced.

```rust
use ustore::{Arena, CollectionID, RAMOptions, Database, Durability, Entry, Key, Matching, Modality};

let database = Database::init_ram(&RAMOptions::default())?;
let mut arena = Arena::new()?;

let [ada, grace, kent] = [1u64, 2, 3].map(Key::from);
let people = database.collection_create("people", Modality::Blobs)?;
let entries = [Entry::new(ada, b"Ada"), Entry::new(grace, b"Grace")];
database.head().write(people, &entries, Durability::Buffered)?;

let values = database.head().read(people, &[ada, grace, kent], &mut arena)?;
assert_eq!(values.get(0), Some(b"Ada".as_slice()));
assert_eq!(values.get(2), None);
```

An `Entry` pairs a key with borrowed bytes, laid out so a slice of them crosses as it stands: the keys as one strided run, and the values through an accessor over the same entries, so nothing is staged on the way in.
A write also takes owned keys beside a `stringtape::BytesTape<u32>` or `BytesTape<u64>` of their values, one more offset than keys into one run of bytes, which the library reads as it stands.
A read hands back a `Values`, which addresses the tape in the arena rather than copying out of it, and reports a key the database does not hold as `None`.
`Values::tape` hands the whole batch out as a `Tape` over that tape: one more offset than values, and the bytes between them, 32-bit while the bytes fit in 4 GiB, as Arrow lays out `binary`, and 64-bit beyond, as it lays out `large_binary`.
A missing value is empty there, and `Values::presences` is the bitmap of the keys found, as Arrow reads a validity buffer.

A write carries values and nothing else, so an empty slice writes an empty value, and `On::remove` removes keys with their values in a batch of its own.
A removal therefore never hides inside a batch of writes.

```rust
let entries = [Entry::new(ada, b"rewritten"), Entry::new(kent, b"")];
database.head().write(people, &entries, Durability::Buffered)?;
database.head().remove(people, &[grace], Durability::Buffered)?;

let first = database.head().scan(people, Key::from(0u64), 100, &mut arena)?.keys;
let measured = database.head().measure(people, ada..kent, &mut arena)?;
let largest = KeyRange { direction: ScanDirection::Descending, ..KeyRange::default() };
let last = database.head().scan(people, largest, 10, &mut arena)?.keys;
let headers = database.head().read_slices(people, &[ada, kent], ..64, &mut arena)?;
database.head().remove_range(people, ada..kent, Durability::Buffered)?;
```

`On::remove` removes whole documents the same way, while a field goes with a merge that carries a JSON `null` for it.
`scan` lists up to the count it wants from a start key, `sample` draws keys at random, and `measure` bounds the keys and the bytes a range of keys holds.
Scans, cursors and measures take a `KeyRange`: a start key, inclusive, an end key, exclusive, and a `ScanDirection`, where a key alone walks up from it and `start..end` walks up between them.
`read_slices` reads one byte range of every value, and `remove_range`, `docs_remove_range` and `graph_remove_vertices_range` remove every key of a range in one atomic call.
`docs_sample`, `pages_sample` and `graph_sample_vertices` draw keys from the other modalities, as `docs_measure`, `pages_measure` and `graph_measure_vertices` measure them.

A `Cursor` walks one collection page by page, each pull landing in the arena it is given, with the values of its keys when it asks for them:

```rust
let mut cursor = database.head().on(&executor).cursor(people, Key::from(0u64), Matching::default())?;
loop {
    let page = cursor.next_pairs(1024, &mut arena)?;
    for (index, key) in page.keys().iter().enumerate() {
        println!("{key}: {:?}", page.get(index));
    }
    if page.keys().len() < 1024 { break; }
}
```

Every pull resumes past the last key the pull before it handed out, so a key written behind the walk is never handed out and one written ahead of it is.
`next` pulls the keys alone, and `next_pairs` reads their values in the same call, where a key removed at the head between the listing and the read of one pull reads as an empty value.
A page shorter than the count it wanted exhausts the walk for good, so every pull after it hands out nothing, even once more keys are written, and a pull that fails leaves the cursor where it was.
A cursor opens from a blocking view, sees what that view sees pull by pull, and pulls on its executor; it holds the database open, and the transaction or snapshot it reads through, until it is dropped.

`Database::changes_cursor` opens a feed of the changes committed to one collection after a `CommitStamp`, oldest first, pinning that stamp as a snapshot does:

```rust
let mut feed = database.changes_cursor(people, stamp)?;
let changes = feed.next_pairs(1024, &mut arena)?;
for (index, (key, kind)) in changes.keys().iter().zip(changes.kinds()).enumerate() {
    println!("{key} {kind} at {:?}: {:?}", changes.stamps()[index], changes.value(index));
}
```

Every pull resumes past the last change the one before it handed out, and an empty one reached the newest commit.
A `ChangeKind` is a write, a removal or a partial change, and a range removal comes as the removal of every key it removed.
A feed whose history is already gone is refused with `Status::WriteConflict`, and LevelDB, which keeps no stamps, refuses it with `Status::Unsupported`.

## Documents

Documents are JSON text, which the engine parses as it writes them.
`Collection::docs` parses what it reads through JsonTape: `get` hands out a `jsontape::Json` parsed out of the one-off arena it read through, `get_json` hands out the text itself as a `String`, and `set` writes a `jsontape::Json`.
With the `serde` feature, `get_as` deserializes a document into any `DeserializeOwned` type.
`ustore::Json` names JSON text in the batch verbs below, so JsonTape's types are always spelled with their `jsontape::` prefix.

```rust
let mut movies = database.collection("movies")?;
movies.docs().set(avatar, &jsontape::parse(br#"{"title":"Avatar","year":2009}"#)?)?;
let title = movies.docs().get(avatar)?.and_then(|movie| movie["title"].as_str().map(str::to_owned));
let text: Option<String> = movies.docs().get_json(avatar)?;
let fields: Option<BTreeMap<String, jsontape::Json>> = movies.docs().get_as(avatar)?;
```

The batch verbs carry documents as text both ways and parse nothing on the Rust side.
A field is a top-level key or an RFC 6901 pointer, which crosses as the bytes of a `&str`, so a `"/user/name"` literal crosses as it stands, and an empty field addresses the whole document.
`docs_write` writes whole documents or fields inside them, and `docs_read` reads either, as `ustore_docs_write` and `ustore_docs_read` do.

```rust
use ustore::{Column, Document, DocumentEntry, FieldAddress, FieldEntry, Json, KeyedBy, Modification, Text};

let documents = [DocumentEntry::<Json>::new(ada, r#"{"name":"Ada","born":1815}"#)];
database.head().docs_write(people, &documents, Modification::Upsert, Durability::Buffered)?;

let born = [DocumentEntry::<i64>::new(Key::from(5u64), &1815)];
database.head().docs_write(people, &born, Modification::Upsert, Durability::Buffered)?;

let keyed = [Document::new(r#"{"_id":2,"name":"Grace"}"#)];
database.head().docs_write(people, KeyedBy("_id", &keyed), Modification::Insert, Durability::Buffered)?;

let died = [FieldEntry::<i64>::new(ada, "died", &1852)];
database.head().docs_write(people, &died, Modification::Upsert, Durability::Buffered)?;
let family = [FieldEntry::<Json>::new(ada, "/family", r#"{"father":"Byron"}"#)];
database.head().docs_write(people, &family, Modification::Upsert, Durability::Buffered)?;
let unborn = [DocumentEntry::<Json>::new(ada, r#"{"born":null}"#)];
database.head().docs_write(people, &unborn, Modification::Merge, Durability::Buffered)?;

let whole = database.head().docs_read(people, [ada, grace], Json, &mut arena)?;
let names = database.head().docs_read(people, &[FieldAddress::new(ada, "name")], Text, &mut arena)?;
```

A write carries one format for the whole batch, named by the type of its entries: a scalar from `f64` down to `u8` or `bool`, `Text` for a JSON string, or `Json` for any JSON value.
The same formats write a whole document, which is then one number, one boolean or one string rather than an object.
`KeyedBy` writes documents that carry their keys under a field of their own: a non-negative integer, spelled as `Key::from` spells it, or a UUID in canonical text.
Mixing two formats in one batch does not compile, and every entry carries a value: `On::remove` removes whole documents, and a merge carrying a JSON `null` removes a field.
`Modification` chooses between an upsert, an update, an insert, an RFC 6902 patch and an RFC 7386 merge.
Reads hand back the same `Values` a binary read does, of keys for whole documents or of `FieldAddress` values for fields, exported in the format named before the arena, `Json` or printed `Text`, with a missing document or field reported as `None`.

Every document verb also takes owned inputs, which a deferred view needs.
Whole documents go as keys beside a `stringtape::CharsTape` of JSON texts, or as the tape alone inside a `KeyedBy`, and field writes as keys and a `CharsTape` of paths beside a tape of JSON values.
A field read takes keys beside a `CharsTape` of paths, and a gather a `Vec<Column<'static>>`, whose `Column::new` takes a `&str` or a `String`.

```rust
use ustore::stringtape::CharsTape;

let deferred = database.head().deferred(&executor);
let texts: CharsTape<u64> = [r#"{"name":"Ada"}"#, r#"{"name":"Grace"}"#].into_iter().collect();
deferred.docs_write(people, (vec![ada, grace], texts), Modification::Upsert, Durability::Buffered)?.wait()?;

let paths: CharsTape<u32> = ["name", "name"].into_iter().collect();
let names = deferred.docs_read(people, (vec![ada, grace], paths), Text, &mut arena)?.await?;
assert_eq!(names.get(1), Some(b"Grace".as_slice()));
```

A gist lists the unique paths to the leaves of a batch of documents as `&str` pointers, and a gather converts fields into typed columns, a row per key:

```rust
let paths = database.head().docs_gist(people, [ada, grace], &mut arena)?;

let columns = [Column::new::<u64>("born"), Column::new::<Text>("name")];
let gathered = database.head().docs_gather(people, &[ada, grace], &columns, &mut other_arena)?;
let born: &[u64] = gathered.scalars::<u64>(0)?;
let valid = gathered.validities(0)?.unwrap().get(1);
let name = gathered.strings(1)?.get(0);
```

`Columns` borrow their arena and hold the columns they were asked for, borrowed or owned, and hand out scalar columns as slices straight out of the arena, and string columns as a `StringColumn` each, whose `Text` strings come without the null terminator the arena keeps after them.
Beside the scalars a gather converts into, `Half` is a column of IEEE 754 binary16 bit patterns the caller reinterprets and `UUID` a column of sixteen bytes parsed from canonical text; neither is a format a write carries, and a value that will not fit either leaves its cell zero and sets the collision bit of its row.
Every column carries three bitmaps: validity for a value found and converted, conversions for a value whose meaning changed on the way, such as text parsed into a number, and collisions for a value of a type that would not convert.
Asking for a column in a type other than the one it was gathered in is refused with `Status::InvalidArgument`.
Paths from one arena address the fields a gather reads through another.

## Graphs

A graph collection, created with `Modality::Graph`, indexes every edge from both of its vertices, and an `Edge` is laid out as the source, target and identifier keys the ABI puts on its tapes.
A slice of edges therefore crosses as three strided columns, and the tape a lookup exports reads back as `&[Edge]` without a copy.

`Collection::graph` adds and removes one edge at a time under the default identifier, and looks up a batch of vertices in one role through a one-off arena, copying the edges or the degrees out of it:

```rust
let mut similar = database.collection("similar")?;
similar.graph().add_edge(avatar, titanic)?;
let recommendations: Vec<Edge> = similar.graph().read_edges(&[avatar], Role::Source)?;
let degrees: Vec<Degree> = similar.graph().degrees(&[avatar, titanic], Role::Any)?;
similar.graph().remove_edge(avatar, titanic)?;
```

The batch verbs, named as the C ABI names them, take edges with identifiers of their own, vertices, and a role per vertex:

```rust
use ustore::{Edge, Role, Roles};

let follows = database.collection_create("follows", Modality::Graph)?;
let [first, second, third, fourth, seventh] = [1u64, 2, 3, 4, 7].map(Key::from);
let edges = [Edge::new(first, second), Edge::new(first, third), Edge::with_id(second, third, seventh)];
database.head().graph_write_edges(follows, &edges, Durability::Buffered)?;
database.head().graph_write_vertices(follows, [fourth], Durability::Buffered)?;

let vertices = [first, third];
let found = database.head().graph_read_edges(follows, &vertices, Roles::Every(Role::Any), &mut arena)?;
for (vertex, run) in vertices.iter().zip(found.iter()) {
    for edge in run.unwrap_or_default() {
        println!("{vertex} meets {:?}", edge.neighbor_of(*vertex));
    }
}

let roles = Roles::Each(&[Role::Source, Role::Target]);
let degrees = database.head().graph_degrees(follows, [first, second], roles, &mut arena)?;
database.head().graph_remove_edges(follows, [Edge::with_id(second, third, seventh)], Durability::Buffered)?;
database.head().graph_remove_vertices(follows, [first], Roles::Every(Role::Any), Durability::Buffered)?;
```

`Neighborhoods` holds the degree of every vertex, the edges of all of them on one tape, and one more offset than vertices bounding the run of each, where a vertex the graph does not hold spans none, and its iterator splits the tape at those offsets.
Within one role the edges of a vertex come sorted by neighbor, and with both roles the outgoing edges precede the incoming ones.
`graph_degrees` asks for the degrees alone, exporting no edges.
`Roles::Every` gives the whole batch one role and `Roles::Each` gives every vertex its own, which must match the batch in length.
Every graph verb takes owned edges and vertices as well, so a deferred view serves each of them.

## What This Crate Does Not Expose

These C entry points also take a deferred completion, but the crate calls them blocking, whatever receiver you hold:

- `ustore_database_init_ram`, `ustore_database_view_rocksdb` and the `ustore_database_open_*` family, whose options name the executor the open runs on.
- `ustore_database_describe` and `ustore_collection_create`.
- `ustore_transaction_init` for a new transaction, and `ustore_snapshot_init`.
- `ustore_blobs_cursor_next`, which runs on the executor of the view that opened the cursor, and opens only from a blocking one.

Every call through a `Collection`, and `Executor::drain`, block as well.

The crate binds none of these:

- The strings cursor, which `export_arrow` drives, and the range removals of `ustore_pages_remove`.
- The progress of a cursor pull, which reports a single unit once its page is out.
- The lengths a read writes beside its values, which the tape's offsets already carry.

## Design

The crate is a thin layer over the C ABI in `c/ustore/`, and every rule below follows from that.

- __Verbs are named as in C.__
  A C verb drops only what its receiver already names, so `ustore_docs_write` is `docs_write` on a view and `ustore_collection_find` is `collection_find`, while the blob verbs, which reach documents as well, keep their bare names.
  The constructors keep C's lifecycle names: `init_ram` starts empty, `open_<engine>` serves a directory in place, and `view_rocksdb` serves one read-only.
- __Blocking verbs return `Result` directly.__
  `database.head().read(people, &[ada, grace], &mut arena)?` takes no executor, completion or call value, and nothing about asynchrony reaches a caller who never asks for it.
- __`On<'view, Parent, Completion>` makes the receiver a type, not a flag.__
  What every C argument struct puts before its inputs, the view, the executor and the completion, is chosen by the receiver a verb is called on: `database.head()`, `transaction.view(watch)` or `snapshot.view()`, then `.on(&executor)` or `.deferred(&executor)`, where `transaction.on(&executor)` and `transaction.deferred(&executor)` watch the keys.
  Each verb is written once, generic over the completion, so a `DeferredOn` offers the same verbs with the same arguments and answers a `Deferred` instead of the result.
  Opening a database has no view yet, so its options name the executor, and the open blocks.
- __A call the view cannot serve does not compile.__
  The parent, a `Database`, a `Transaction` or a `Snapshot`, decides the verbs beyond the reads: only a transaction stages, commits, rolls back and resets, only the head clears, compacts and drops collections, only a snapshot saves, and nothing writes through a snapshot.
- __Ragged inputs cross as they lie.__
  A slice of entries lends its keys as a strided run and its values through an accessor over the same records, and owned values cross as a tape of 32-bit or 64-bit offsets, so nothing is staged on the way in.
- __Arenas are lent and reused, never shared or hidden.__
  Executors make them through `ustore_arena_init`, and every verb whose outputs scale with the call borrows one as `&mut Arena`, blocking or deferred, while its result borrows it in turn, so the borrow checker retires stale views at compile time.
  No handle keeps an arena of its own.
- __Shorthand makes one-off arenas.__
  A `Collection` call with no arena to pass, such as `get` or `contains`, makes an arena for that call, copies its result out, and frees it: slower than reuse, and stateless.
- __A `DeferredOn` refuses borrowed inputs at compile time.__
  The library reads a deferred call's inputs after the verb returns, so the `Accepts` bound admits only owned ones, and passing `&[Key]` fails with "a deferred view needs owned inputs".
  The arena stays a borrow, which is safe because `ustore_arena_free` waits for the calls writing into it.
- __Deferred calls use the language's own idioms.__
  `status` polls through `ustore_outcome_status`, `wait` blocks through `ustore_outcome_wait`, and a `Deferred` is `IntoFuture`, so `.await` polls the status, asking to be polled again while the call is not final, since the library has no wake-up.
- __No life of its own.__
  The crate starts no threads and keeps no counts or queues, and needs no async runtime; releasing a `Deferred` rides `Drop`, which waits for the call first.
- __Nothing costs anything until it is used.__
  A blocking call allocates no box, a deferred one allocates one, and an arena allocates only when a call writes into it.
- __Statuses and modes are enums.__
  `Status`, `Watch`, `Durability`, `DropMode`, `Mood`, `Isolation`, `Transport`, `Engine`, `Modality`, `Role` and `Modification` mirror the C enumerators value for value, and spell themselves through the C name tables, so nothing is compared as a string.
- __Handlers cancel, and their panics resurface.__
  A progress handler is a closure lent for one call, and a panic inside it cancels the call rather than unwinding through C, then resurfaces once the call is final.
- __Borrowed pools are `unsafe`.__
  `Executor::on_pool` takes a raw `fu_pool_t`, which must outlive the executor, and safe Rust has no way to check that, so it is the one `unsafe` constructor.

Every verb of a view defers, each on owned inputs, and borrows its arena as a blocking verb does:

- __Values:__ `write` on keys beside a `BytesTape`, `remove` on keys, and `read`, `scan`, `sample` and `measure`.
- __Documents:__ `docs_write` on keys beside a `CharsTape` of JSON, on a `KeyedBy` of a tape, or on keys and a `CharsTape` of paths beside a `CharsTape` of values, `docs_read` on keys or on keys beside a `CharsTape` of paths, `docs_gather` on a `Vec<Column<'static>>`, and `docs_gist` on keys.
- __Graphs:__ `graph_write_edges` and `graph_remove_edges` on edges, `graph_write_vertices` and `graph_remove_vertices` on keys, and `graph_read_edges` and `graph_degrees`.
- __Collections:__ `collection_find` on a `String`, `collection_names` and `collection_list`, and `collection_drop` at the head.
- __Databases, transactions and snapshots:__ `clear` at the head, and `compact` there with a `'static` progress handler or none, `stage`, `commit`, `rollback` and `reset` of a transaction, and `save` of a snapshot on a `String`, with a progress handler as `compact` takes one.
