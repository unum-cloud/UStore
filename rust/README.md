# UStore for Rust

## Installation

```sh
cargo add ustore
```

The crate links the engine libraries CMake builds and consumes them through the C ABI in `include/ustore/`.
`build.rs` compiles no C or C++ of its own: it configures CMake, installs the static libraries into `OUT_DIR` and links them.

SmashTable always builds and links.
The `rocksdb` and `leveldb` features build their engines beside it, linked once `c/lib.cpp` dispatches between them.

```sh
cargo test
cargo test --features rocksdb,leveldb
```

## Quickstart

A `Database` owns its directory, and an `Arena` is the scratch the engine exports results into.
Every batch verb takes the arena mutably and hands back a result that borrows it, so a value read out of an arena can neither outlive it nor survive the call that would overwrite it.

```rust
use ustore::{Arena, Collection, Configuration, Database, Durability, Entry};

let database = Database::open(Configuration::in_memory())?;
let mut arena = Arena::new();

let people = database.create_collection(&mut arena, "people")?;
let entries = [Entry::new(1, b"Ada")?, Entry::new(2, b"Grace")?];
database.head().write(&mut arena, people, &entries, Durability::Buffered)?;

let values = database.head().read(&mut arena, people, &[1, 2])?;
assert_eq!(values.get(0), Some(b"Ada".as_slice()));
```

An `Entry` pairs a key with borrowed bytes in the layout the strided ABI reads, so a batch of them crosses the boundary with no staging buffer.
A read hands back a `Values`, which addresses the tape in the arena rather than copying out of it, and reports a key the database does not hold as `None`.

## Arenas Are The Borrow

The C ABI resets an arena on every call through it, so results from one call stop being readable the moment the next one runs.
That rule is the `&mut Arena` every verb takes: while a result is alive the arena stays mutably borrowed, and the call that would overwrite it does not compile.

```rust
let values = database.head().read(&mut arena, people, &[1])?;
let others = database.head().read(&mut arena, people, &[2])?; // refused while `values` lives
println!("{:?}", values.get(0));
```

Reuse the arena by letting the result fall out of scope, or reset it outright with `Arena::reset`.
One arena serves one thread of calls, which is why it is `Send` and never shared.

## One Call, One View

A call addresses the head of the database, a transaction, or a snapshot, and never two of them.
`View` is that view, so the combination the C ABI refuses cannot be written.

```rust
let mut transaction = database.transaction()?;
transaction.view().write(&mut arena, people, &entries, Durability::Buffered)?;
transaction.commit(Durability::Flushed)?;

let snapshot = database.snapshot()?;
let pinned = snapshot.view().read(&mut arena, people, &[1])?;
```

A transaction conflicts rather than blocks, and reports `Status::WriteConflict`, `ReadConflict` or `PhantomConflict` at its commit.
It keeps its contents whatever the outcome, so `Transaction::rollback` reopens it on the newest state for another try.
`Transaction::stage` is the first phase of a two-phase commit, and both phases report the sequence number they took.
A snapshot pins the state it was taken at until it is dropped, and `Snapshot::export` writes it out as a database of its own.

## Collections And Names

Collections are created, listed and dropped by identifier, and a name is a binding in the strings modality rather than anything an engine knows.
Both halves live behind one call each.

```rust
let people = database.create_collection(&mut arena, "people")?;
let same = database.head().collection(&mut arena, "people")?;
let every = database.head().collections(&mut arena)?;

for name in database.head().collection_names(&mut arena, "pe", None, 100)?.iter() {
    println!("{name}");
}

database.drop_collection(&mut arena, "people")?;
```

`Collection::main` is the collection every database holds, which `Database::clear_collection` may empty but nothing may drop.
Walks paginate rather than hold an iterator over concurrent state: a scan resumes from a key and a name walk from a name.

## Errors

Every fallible call answers `Result<T, Error>`, where the error carries the `Status` the C ABI reported and the static message beside it, so nothing allocates on the failure path.

```rust
match database.head().collection(&mut arena, "nowhere") {
    Err(failure) if failure.status == ustore::Status::NotFound => println!("{failure}"),
    other => { other?; }
}
```

## What The Crate Covers

Opening and closing, engine metadata and control requests, collections by identifier and by name, binary reads, writes, erases, scans, samples and measurements, transactions with staging, commit and rollback, and snapshots with export.
The documents and graph modalities of `docs.h` and `graph.h`, batches spanning several collections at once, and the shared-memory and unwatched-read options are not bound yet.
