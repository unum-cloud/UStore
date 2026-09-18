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

## Documents

Documents are JSON text, and the C ABI parses them inside the engine, so the crate never parses JSON itself.
A field is a top-level key or an RFC 6901 pointer the engine reads up to its null terminator, which is why every field is a `&CStr`: a `c"/user/name"` literal crosses the boundary as it stands.

```rust
use ustore::{Column, Document, FieldAddress, FieldEntry, Json, Modification, Text};

let documents = [Entry::new(1, br#"{"name":"Ada","born":1815}"#)?];
database.head().write_documents(&mut arena, people, &documents, Modification::Upsert, Durability::Buffered)?;

let keyed = [Document::new(r#"{"_id":2,"name":"Grace"}"#)?];
database.head().write_documents_by_id(&mut arena, people, c"_id", &keyed, Modification::Insert, Durability::Buffered)?;

let died = [FieldEntry::<i64>::new(1, c"died", &1852)?];
database.head().write_fields(&mut arena, people, &died, Modification::Upsert, Durability::Buffered)?;
let family = [FieldEntry::<Json>::new(1, c"/family", r#"{"father":"Byron"}"#)?, FieldEntry::<Json>::removal(1, c"born")];
database.head().write_fields(&mut arena, people, &family, Modification::Upsert, Durability::Buffered)?;

let whole = database.head().read_documents(&mut arena, people, &[1, 2])?;
let names = database.head().read_fields::<Text>(&mut arena, people, &[FieldAddress::new(1, c"name")])?;
```

A field write carries one format for the whole batch, named by the type of its entries: a scalar from `f64` down to `u8` or `bool`, `Text` for a JSON string, or `Json` for any JSON value.
Mixing two formats in one batch does not compile, and an entry built with `FieldEntry::removal` removes its field.
`Modification` chooses between an upsert, an update, an insert, an RFC 6902 patch and an RFC 7386 merge.
Reads hand back the same `Values` a binary read does, exported as `Json` or as printed `Text`, with a missing document or field reported as `None`.

A gist lists the unique paths to the leaves of a batch of documents as `&CStr` pointers, and a gather converts fields into typed columns, a row per key:

```rust
let paths = database.head().gist(&mut arena, people, &[1, 2])?;

let columns = [Column::new::<u64>(c"born"), Column::new::<Text>(c"name")];
let table = database.head().gather(&mut other_arena, people, &[1, 2], &columns)?;
let born: &[u64] = table.scalars::<u64>(0)?;
let valid = table.validities(0).unwrap().get(1);
let name = table.strings(1)?.get(0);
```

A `Table` borrows its arena and the columns it was asked for, and hands out scalar columns as slices straight out of the arena.
Every column carries three bitmaps: validity for a value found and converted, conversions for a value whose meaning changed on the way, such as text parsed into a number, and collisions for a value of a type that would not convert.
Asking for a column in a type other than the one it was gathered in is refused with `Status::InvalidArgument`.
Paths from one arena address the fields a gather reads through another.

## Graphs

A graph collection stores every vertex as its degrees and sorted edges, and an `Edge` is laid out as the source, target and identifier keys the ABI puts on its tapes.
A slice of edges therefore crosses as three strided columns, and the tape a lookup exports reads back as `&[Edge]` without a copy.

```rust
use ustore::{Edge, Role, Roles};

let follows = database.create_collection(&mut arena, "follows")?;
let edges = [Edge::new(1, 2), Edge::new(1, 3), Edge::with_id(2, 3, 7)];
database.head().upsert_edges(&mut arena, follows, &edges, Durability::Buffered)?;
database.head().upsert_vertices(&mut arena, follows, &[4], Durability::Buffered)?;

let found = database.head().find_edges(&mut arena, follows, &[1, 3], Roles::Every(Role::Any))?;
for (vertex, run) in [1, 3].iter().zip(found.iter()) {
    for edge in run.unwrap_or_default() {
        println!("{vertex} meets {:?}", edge.neighbor_of(*vertex));
    }
}

let degrees = database.head().degrees(&mut arena, follows, &[1, 2], Roles::Each(&[Role::Source, Role::Target]))?;
database.head().remove_edges(&mut arena, follows, &[Edge::with_id(2, 3, 7)], Durability::Buffered)?;
database.head().remove_vertices(&mut arena, follows, &[1], Roles::Every(Role::Any), Durability::Buffered)?;
```

`Neighborhoods` holds the degree of every vertex and the edges of all of them on one tape, and its iterator splits the tape into one run per vertex.
Within one role the edges of a vertex come sorted by neighbor, and with both roles the outgoing edges precede the incoming ones.
`degrees` asks for the degrees alone, exporting no edges.
`Roles::Every` gives the whole batch one role and `Roles::Each` gives every vertex its own, which must match the batch in length.

## Errors

Every fallible call answers `Result<T, Error>`, where the error carries the `Status` the C ABI reported and the static message beside it, so nothing allocates on the failure path.

```rust
match database.head().collection(&mut arena, "nowhere") {
    Err(failure) if failure.status == ustore::Status::NotFound => println!("{failure}"),
    other => { other?; }
}
```

## What The Crate Covers

Opening and closing, engine metadata and control requests, collections by identifier and by name, binary reads, writes, erases, scans, samples and measurements, documents with whole and field writes, reads, gists and gathers, graphs with edge and vertex upserts, removals and lookups, transactions with staging, commit and rollback, and snapshots with export.
Batches spanning several collections at once, the half-precision and UUID gather columns, and the shared-memory and unwatched-read options are not bound yet.
