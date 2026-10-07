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
cargo test --all-features                         # every engine, and documents read into `serde` types
```

The crate re-exports the two it builds on: `jsontape`, which parses documents, and `stringtape`, whose views hand out batches of values.

## Quickstart

A `Database` owns its directory, and a `Collection` addresses one collection in it by name, as the Python binding does with `db['movies']`.

```rust
use ustore::{jsontape, Database, Durability, Key, RAMOptions, Role};

let database = Database::open_ram(&RAMOptions::default())?;
let [avatar, titanic] = [1u64, 2].map(Key::from);

database.collection("movies")?.docs().set(avatar, &jsontape::parse(br#"{"title":"Avatar","year":2009}"#)?)?;
database.collection("posters")?.blobs().set(avatar, &png_bytes)?;
database.collection("similar")?.graph().add_edge(avatar, titanic)?;

let mut movies = database.collection("movies")?;
let description = movies.docs().get(avatar)?.expect("written above");
assert_eq!(description.get("year").as_u64(), Some(2009));

let mut posters = database.collection("posters")?;
let thumbnail: Option<&[u8]> = posters.blobs().get(avatar)?;

let mut similar = database.collection("similar")?;
let recommendations = similar.graph().find_edges(&[avatar], Role::Source)?;

let mut pictures = database.collection("pictures")?;
let images = pictures.sample(1000)?;
let tape = images.values().tape();

let mut transaction = database.transaction()?;
transaction.collection("movies")?.docs().set(titanic, &jsontape::parse(br#"{"title":"Titanic","year":1997}"#)?)?;
transaction.collection("similar")?.graph().add_edge(titanic, avatar)?;
transaction.collection("posters")?.blobs().set(titanic, &png_bytes)?;
transaction.commit(Durability::Buffered)?;
```

A collection resolves its name through its view once, and every verb goes through a view of one modality: `blobs`, `docs` or `graph`.
The first write through a view creates a missing collection holding that view's modality, a read of a missing one fails with `Status::NotFound`, and a view of another modality fails with `Status::InvalidArgument`, while a collection created with `Modality::Any` takes every view.
`sample` draws keys at random and reads their values through the same view, so a sample holds both.

A collection owns the `Arena` its reads export into, and each view consumes itself on its one call, so a value it reads borrows the collection and the next call through that collection waits until the value is gone.
Writes land `Durability::Buffered`.
`Transaction::collection` reads and writes inside the transaction, while its name resolves at the head.
Collections are created and dropped nowhere but at the head, so a first write from inside a transaction creates the collection there at once, and a name never joins the transaction's conflicts.
Its collections borrow the transaction, so they are no longer used by the time `Transaction::commit` takes it mutably.

## Batches And Arenas

Beneath the collections, every verb takes a batch and names its collections by identifier, and every read lands its results in an `Arena`, the scratch the caller owns.
The library sizes the tapes of values, which land behind the arena's handle, while the presence bits, offsets, keys and degrees a batch bounds land in spans the arena keeps beside it; both grow to their high-water mark, so a steady loop allocates once.
Every read takes the arena mutably and hands back a result that borrows it, so a value read out of an arena can neither outlive it nor survive the call that would overwrite it.
Writes export nothing and take no arena.

```rust
use ustore::{Arena, CollectionID, RAMOptions, Database, Durability, Entry, Key, Modality};

let database = Database::open_ram(&RAMOptions::default())?;
let mut arena = Arena::new();

let [ada, grace, kent] = [1u64, 2, 3].map(Key::from);
let people = database.create_collection("people", Modality::Any)?;
let entries = [Entry::new(ada, b"Ada")?, Entry::new(grace, b"Grace")?];
database.head().write(people, &entries, Durability::Buffered)?;

let values = database.head().read(&mut arena, people, &[ada, grace, kent])?;
assert_eq!(values.get(0), Some(b"Ada".as_slice()));
assert_eq!(values.get(2), None);
```

A `Key` is a `UUID`, sixteen bytes ordered as their bytes, and converts from `[u8; 16]`, from a `UUID`, or from a `u64` or `u128` spelled big-endian, so keys made from integers order as the integers.
A `CollectionID` is a UUIDv7 the catalog mints when a collection is created.

An `Entry` pairs a key with borrowed bytes in the layout the strided ABI reads, so a batch of them crosses the boundary with no staging buffer.
A read hands back a `Values`, which addresses the tape in the arena rather than copying out of it, and reports a key the database does not hold as `None`.
`Values::tape` hands the whole batch out as a `stringtape::BytesTapeViewU64` over that tape: one more 64-bit offset than values, and the bytes between them, as Arrow lays out `large_binary`.
A missing value is empty there, and `Values::presences` is the bitmap of the keys found, as Arrow reads a validity buffer.

## One Batch, Many Collections

The ABI carries a collection per task, so a batch spans as many collections as it has entries.
Every batch verb takes a `Collections`, and a bare `CollectionID` converts into `Collections::Every`, which is the common case written the short way.

```rust
use ustore::Collections;

let spanned = [people, places];
let entries = [Entry::new(ada, b"Ada")?, Entry::new(ada, b"Kent")?];
database.head().write(spanned.as_slice(), &entries, Durability::Buffered)?;

let values = database.head().read(&mut arena, Collections::Each(&spanned), &[ada, ada])?;
database.head().write(people, &entries, Durability::Buffered)?; // one for all of them
```

A `Collections::Each` whose length is not the length of the batch is refused with `Status::InvalidArgument` before the call crosses the boundary.

## Writes And Removals

A write carries values and nothing else, so an empty slice writes an empty value, and `View::remove` removes keys with their values in a batch of its own.
A removal therefore never hides inside a batch of writes.

```rust
let entries = [Entry::new(ada, b"rewritten")?, Entry::new(kent, b"")?];
database.head().write(people, &entries, Durability::Buffered)?;
database.head().remove(people, &[grace], Durability::Buffered)?;
```

`View::remove` removes whole documents the same way, while a field goes with a merge that carries a JSON `null` for it.

## Arenas Are The Borrow

The library rewinds an arena on every call through it, so results from one call stop being readable the moment the next one runs.
That rule is the `&mut Arena` every read takes: while a result is alive the arena stays mutably borrowed, and the call that would overwrite it does not compile.

```rust
let values = database.head().read(&mut arena, people, &[ada])?;
let others = database.head().read(&mut arena, people, &[grace])?; // refused while `values` lives
println!("{:?}", values.get(0));
```

Reuse the arena by letting the result fall out of scope: the next call rewinds it and keeps its memory, and dropping the arena frees it.
One arena serves one thread of calls, which is why it is `Send` and never shared.

## One Call, One View

A call addresses the head of the database, a transaction, or a snapshot, and never two of them.
`View` is that view: a read passes the `ustore_view_t` it holds, and a write passes its transaction and whether that watches the write, so a write through a snapshot is refused with `Status::InvalidArgument`.
A view is `Copy` and borrows the database, transaction or snapshot it came from.

```rust
use ustore::Conflicts;

let mut transaction = database.transaction()?;
transaction.view(Conflicts::Watched).write(people, &entries, Durability::Buffered)?;
transaction.commit(Durability::Flushed)?;

let snapshot = database.snapshot()?;
let pinned = snapshot.view().read(&mut arena, people, &[ada])?;
```

`Conflicts` says whether the calls of that view watch what they touch.
It changes what a call does only at `Isolation::Snapshot`, where `Conflicts::Unwatched` lets a transaction commit over a concurrent writer instead of conflicting with it; a strictly serializable transaction puts every read it makes into the one serial order whatever the view says.
Only a transaction watches anything, so the head of a database and a snapshot carry no such argument.

A transaction conflicts rather than blocks, and reports `Status::WriteConflict`, `ReadConflict` or `PhantomConflict` at its commit.
It keeps its contents whatever the outcome, so `Transaction::reset` discards them and reopens it on the newest state for another try.
`Transaction::stage` is the first phase of a two-phase commit, and both phases report the stamp they took; `Transaction::rollback` withdraws a stage while keeping the reads and writes, so the transaction can stage again.
A snapshot is a handle the database hands out, pinning the state it was taken at until it is dropped, and every view at it borrows it; `Snapshot::export` writes it out as a database of its own.

## Collections And Names

Collections are created under a name and the modality they hold, resolved by name once and addressed by identifier from then on, and dropping one forgets its name.
Each engine keeps the names apart from every collection, so no identifier reaches them.
A name is read through a view as a value is, so a watching transaction conflicts with a concurrent create or drop of a name it resolved, while a listing describes the database and reads the view's snapshot or the head.

```rust
let people = database.create_collection("people", Modality::Any)?;
let (same, modality) = database.head().collection("people")?;
let every = database.head().collections(&mut arena)?;

for name in database.head().collection_names(&mut arena, "pe", None, 100)?.iter() {
    println!("{name}");
}

database.drop_collection("people")?;
```

`CollectionID::main` is the collection every database holds, which `Database::clear_collection` may empty but nothing may drop.
Walks paginate rather than hold an iterator over concurrent state: a scan resumes from `Key::next` of the last key it found, and a name walk from a name.

## Documents

Documents are JSON text, which the engine parses as it writes them.
`Collection::docs` parses what it reads through JsonTape: `get` hands out a `jsontape::BoundJsonView` over the text in the arena without copying it, `get_json` hands out the text itself, and `set` writes a `jsontape::Json`.
With the `serde` feature, `get_as` deserializes a document into any `DeserializeOwned` type.
`ustore::Json` names JSON text in the batch verbs below, so JsonTape's types are always spelled with their `jsontape::` prefix.

```rust
let mut movies = database.collection("movies")?;
movies.docs().set(avatar, &jsontape::parse(br#"{"title":"Avatar","year":2009}"#)?)?;
let title = movies.docs().get(avatar)?.and_then(|movie| movie.get("title").as_str().map(str::to_owned));
let text: Option<&str> = movies.docs().get_json(avatar)?;
let fields: Option<BTreeMap<String, jsontape::Json>> = movies.docs().get_as(avatar)?;
```

The batch verbs carry documents as text both ways and parse nothing on the Rust side.
A field is a top-level key or an RFC 6901 pointer the engine reads up to its null terminator, which is why every field is a `&CStr`: a `c"/user/name"` literal crosses the boundary as it stands.

```rust
use ustore::{Column, Document, DocumentEntry, FieldAddress, FieldEntry, Json, Modification, Text};

let documents = [DocumentEntry::<Json>::new(ada, r#"{"name":"Ada","born":1815}"#)?];
database.head().write_documents(people, &documents, Modification::Upsert, Durability::Buffered)?;

let born = [DocumentEntry::<i64>::new(Key::from(5u64), &1815)?];
database.head().write_documents(people, &born, Modification::Upsert, Durability::Buffered)?;

let keyed = [Document::new(r#"{"_id":2,"name":"Grace"}"#)?];
database.head().write_documents_by_id(people, c"_id", &keyed, Modification::Insert, Durability::Buffered)?;

let died = [FieldEntry::<i64>::new(ada, c"died", &1852)?];
database.head().write_fields(people, &died, Modification::Upsert, Durability::Buffered)?;
let family = [FieldEntry::<Json>::new(ada, c"/family", r#"{"father":"Byron"}"#)?];
database.head().write_fields(people, &family, Modification::Upsert, Durability::Buffered)?;
let unborn = [DocumentEntry::<Json>::new(ada, r#"{"born":null}"#)?];
database.head().write_documents(people, &unborn, Modification::Merge, Durability::Buffered)?;

let whole = database.head().read_documents(&mut arena, people, &[ada, grace])?;
let names = database.head().read_fields::<Text>(&mut arena, people, &[FieldAddress::new(ada, c"name")])?;
```

A write carries one format for the whole batch, named by the type of its entries: a scalar from `f64` down to `u8` or `bool`, `Text` for a JSON string, or `Json` for any JSON value.
The same formats write a whole document, which is then one number, one boolean or one string rather than an object.
Mixing two formats in one batch does not compile, and every entry carries a value: `View::remove` removes whole documents, and a merge carrying a JSON `null` removes a field.
`Modification` chooses between an upsert, an update, an insert, an RFC 6902 patch and an RFC 7386 merge.
Reads hand back the same `Values` a binary read does, exported as `Json` or as printed `Text`, with a missing document or field reported as `None`.

A gist lists the unique paths to the leaves of a batch of documents as `&CStr` pointers, and a gather converts fields into typed columns, a row per key:

```rust
let paths = database.head().gist(&mut arena, people, &[ada, grace])?;

let columns = [Column::new::<u64>(c"born"), Column::new::<Text>(c"name")];
let table = database.head().gather(&mut other_arena, people, &[ada, grace], &columns)?;
let born: &[u64] = table.scalars::<u64>(0)?;
let valid = table.validities(0).unwrap().get(1);
let name = table.strings(1)?.get(0);
```

A `Table` borrows its arena and the columns it was asked for, and hands out scalar columns as slices straight out of the arena.
Beside the scalars a gather converts into, `Half` is a column of IEEE 754 binary16 bit patterns the caller reinterprets and `UUID` a column of sixteen bytes parsed from canonical text; neither is a format a write carries, and a value that will not fit either leaves its cell zero and sets the collision bit of its row.
Every column carries three bitmaps: validity for a value found and converted, conversions for a value whose meaning changed on the way, such as text parsed into a number, and collisions for a value of a type that would not convert.
Asking for a column in a type other than the one it was gathered in is refused with `Status::InvalidArgument`.
Paths from one arena address the fields a gather reads through another.

## Graphs

A graph collection, created with `Modality::Graph`, indexes every edge from both of its vertices, and an `Edge` is laid out as the source, target and identifier keys the ABI puts on its tapes.
A slice of edges therefore crosses as three strided columns, and the tape a lookup exports reads back as `&[Edge]` without a copy.

`Collection::graph` adds and removes one edge at a time under the default identifier, and looks up a batch of vertices in one role:

```rust
let mut similar = database.collection("similar")?;
similar.graph().add_edge(avatar, titanic)?;
let recommendations = similar.graph().find_edges(&[avatar], Role::Source)?;
println!("{:?}", recommendations.edges());
let degrees = similar.graph().degrees(&[avatar, titanic], Role::Any)?;
println!("{degrees:?}");
similar.graph().remove_edge(avatar, titanic)?;
```

The batch verbs take edges with identifiers of their own, vertices, and a role per vertex:

```rust
use ustore::{Edge, Role, Roles};

let follows = database.create_collection("follows", Modality::Graph)?;
let [first, second, third, fourth, seventh] = [1u64, 2, 3, 4, 7].map(Key::from);
let edges = [Edge::new(first, second), Edge::new(first, third), Edge::with_id(second, third, seventh)];
database.head().upsert_edges(follows, &edges, Durability::Buffered)?;
database.head().upsert_vertices(follows, &[fourth], Durability::Buffered)?;

let vertices = [first, third];
let found = database.head().find_edges(&mut arena, follows, &vertices, Roles::Every(Role::Any))?;
for (vertex, run) in vertices.iter().zip(found.iter()) {
    for edge in run.unwrap_or_default() {
        println!("{vertex} meets {:?}", edge.neighbor_of(*vertex));
    }
}

let degrees = database.head().degrees(&mut arena, follows, &[first, second], Roles::Each(&[Role::Source, Role::Target]))?;
database.head().remove_edges(follows, &[Edge::with_id(second, third, seventh)], Durability::Buffered)?;
database.head().remove_vertices(follows, &[first], Roles::Every(Role::Any), Durability::Buffered)?;
```

`Neighborhoods` holds the degree of every vertex and the edges of all of them on one tape, and its iterator splits the tape into one run per vertex.
Within one role the edges of a vertex come sorted by neighbor, and with both roles the outgoing edges precede the incoming ones.
`degrees` asks for the degrees alone, exporting no edges.
`Roles::Every` gives the whole batch one role and `Roles::Each` gives every vertex its own, which must match the batch in length.

## Errors

Every fallible call answers `Result<T, Error>`, where the error carries the `Status` the C ABI reported and the static message beside it, so nothing allocates on the failure path.
`Status::name` spells a status as the library does, such as `"not-found"`.

```rust
match database.head().collection("nowhere") {
    Err(failure) if failure.status == ustore::Status::NotFound => println!("{}: {failure}", failure.status.name()),
    other => { other?; }
}
```

## What The Crate Covers

Opening and closing, engine capabilities and control requests, collections by identifier and by name, binary reads, writes, removals, scans, samples and measurements, documents with whole and field writes, reads, gists and gathers, graphs with edge and vertex upserts, removals and lookups, transactions with staging, rollback of a stage, reset and commit, and snapshots with export.
Batches span several collections, write whole documents in every format the engine parses, and gather half-precision and UUID columns.
Collections by name read and write blobs, JsonTape documents and graph edges one at a time, hand out batches of values as StringTape tapes, and sample keys with their values.
Every call runs on the calling thread and returns with its outcome final, so the crate binds no executors and no deferred completion.
