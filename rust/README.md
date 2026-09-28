# UStore for Rust

## Installation

```sh
cargo add ustore
```

The crate links the engine libraries CMake builds and consumes them through the C ABI in `include/ustore/`.
`build.rs` compiles no C or C++ of its own: it configures CMake, installs the static libraries into `OUT_DIR` and links them.

The in-memory `ram` engine always builds, and the default `rocksdb`, `leveldb` and `nvme` features add theirs to the one library.
Without a feature, its engine's `Database::open_*` fails with `Status::Unsupported`.

```sh
cargo test                                        # every engine
cargo test --no-default-features --features nvme  # RAM and NVMe alone
```

## Quickstart

A `Database` owns its directory, and an `Arena` is the scratch the engine exports results into.
Every batch verb takes the arena mutably and hands back a result that borrows it, so a value read out of an arena can neither outlive it nor survive the call that would overwrite it.

```rust
use ustore::{Arena, Collection, RAMOptions, Database, Durability, Entry};

let database = Database::open_ram(&RAMOptions::default())?;
let mut arena = Arena::new();

let people = database.create_collection(&mut arena, "people")?;
let entries = [Entry::new(1, b"Ada")?, Entry::new(2, b"Grace")?, Entry::removal(3)];
database.head().write(&mut arena, people, &entries, Durability::Buffered)?;

let values = database.head().read(&mut arena, people, &[1, 2])?;
assert_eq!(values.get(0), Some(b"Ada".as_slice()));
```

An `Entry` pairs a key with borrowed bytes in the layout the strided ABI reads, so a batch of them crosses the boundary with no staging buffer.
A read hands back a `Values`, which addresses the tape in the arena rather than copying out of it, and reports a key the database does not hold as `None`.

## One Batch, Many Collections

The ABI carries a collection per task, so a batch spans as many collections as it has entries.
Every batch verb takes a `Collections`, and a bare `Collection` converts into `Collections::Every`, which is the common case written the short way.

```rust
use ustore::Collections;

let spanned = [people, places];
let entries = [Entry::new(1, b"Ada")?, Entry::new(1, b"Kent")?];
database.head().write(&mut arena, spanned.as_slice(), &entries, Durability::Buffered)?;

let values = database.head().read(&mut arena, Collections::Each(&spanned), &[1, 1])?;
database.head().write(&mut arena, people, &entries, Durability::Buffered)?; // one for all of them
```

A `Collections::Each` whose length is not the length of the batch is refused with `Status::InvalidArgument` before the call crosses the boundary.

## Writes And Removals In One Batch

The ABI reads a null value pointer as a removal of its key, so an entry that carries no value removes it, and one batch mixes the two freely.
Nothing is staged and no bitset of present values is allocated: `Entry::removal` is the same sixteen bytes on the same three strided columns, with a null in the pointer one.

```rust
let entries = [Entry::new(1, b"rewritten")?, Entry::removal(2), Entry::new(3, b"")?];
database.head().write(&mut arena, people, &entries, Durability::Buffered)?;
```

An empty slice still points somewhere, so `Entry::new(3, b"")` writes an empty value rather than removing a key; `Entry::value` answers `None` for a removal and `Some` for an empty value.
`DocumentEntry::removal` and `FieldEntry::removal` say the same thing for a whole document and for a field, and `View::erase` stays the way to remove a run of bare keys without building entries for them.

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
use ustore::Conflicts;

let mut transaction = database.transaction()?;
transaction.view(Conflicts::Watched).write(&mut arena, people, &entries, Durability::Buffered)?;
transaction.commit(Durability::Flushed)?;

let snapshot = database.snapshot()?;
let pinned = snapshot.view().read(&mut arena, people, &[1])?;
```

`Conflicts` says whether the calls of that view watch what they touch.
It changes what a call does only at `Isolation::Snapshot`, where `Conflicts::Unwatched` lets a transaction commit over a concurrent writer instead of conflicting with it; a strictly serializable transaction puts every read it makes into the one serial order whatever the view says.
Only a transaction watches anything, so the head of a database and a snapshot carry no such argument.

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
use ustore::{Column, Document, DocumentEntry, FieldAddress, FieldEntry, Json, Modification, Text};

let documents = [DocumentEntry::<Json>::new(1, r#"{"name":"Ada","born":1815}"#)?];
database.head().write_documents(&mut arena, people, &documents, Modification::Upsert, Durability::Buffered)?;

let born = [DocumentEntry::<i64>::new(5, &1815)?];
database.head().write_documents(&mut arena, people, &born, Modification::Upsert, Durability::Buffered)?;

let keyed = [Document::new(r#"{"_id":2,"name":"Grace"}"#)?];
database.head().write_documents_by_id(&mut arena, people, c"_id", &keyed, Modification::Insert, Durability::Buffered)?;

let died = [FieldEntry::<i64>::new(1, c"died", &1852)?];
database.head().write_fields(&mut arena, people, &died, Modification::Upsert, Durability::Buffered)?;
let family = [FieldEntry::<Json>::new(1, c"/family", r#"{"father":"Byron"}"#)?, FieldEntry::<Json>::removal(1, c"born")];
database.head().write_fields(&mut arena, people, &family, Modification::Upsert, Durability::Buffered)?;

let whole = database.head().read_documents(&mut arena, people, &[1, 2])?;
let names = database.head().read_fields::<Text>(&mut arena, people, &[FieldAddress::new(1, c"name")])?;
```

A write carries one format for the whole batch, named by the type of its entries: a scalar from `f64` down to `u8` or `bool`, `Text` for a JSON string, or `Json` for any JSON value.
The same formats write a whole document, which is then one number, one boolean or one string rather than an object.
Mixing two formats in one batch does not compile, and an entry built with `FieldEntry::removal` or `DocumentEntry::removal` removes its field or its document.
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
Beside the scalars a gather converts into, `Half` is a column of IEEE 754 binary16 bit patterns the caller reinterprets and `Uuid` a column of sixteen bytes parsed from canonical text; neither is a format a write carries, and a value that will not fit either leaves its cell zero and sets the collision bit of its row.
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

Opening and closing, engine capabilities and control requests, collections by identifier and by name, binary reads, writes, erases, scans, samples and measurements, documents with whole and field writes, reads, gists and gathers, graphs with edge and vertex upserts, removals and lookups, transactions with staging, commit and rollback, and snapshots with export.
Batches span several collections, mix writes with removals, write whole documents in every format the engine parses, and gather half-precision and UUID columns.

Two options of `ustore_options_t` stay unbound, and deliberately.
`ustore_option_dont_discard_memory_k` exists so the results of one call survive the next and can be passed back into it, which is exactly the borrow that makes a result safe here: an exported slice is sound because the arena stays mutably borrowed for as long as it lives, and a call made without the flag afterwards would release the pages under it.
A second `Arena` gives the same thing with the compiler still checking it.
`ustore_option_read_shared_memory_k` fixes the pages an arena is filled from, and the engine replaces the whole chain, leaking what it held, when a later call asks for the other kind — which a write must, since it refuses the flag outright.
No single arena can therefore serve both a shared read and a write, and an anonymous shared mapping is reachable only by a child across `fork`, which the crate offers no way to arrange.
