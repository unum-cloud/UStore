//! UStore, an embedded multi-modal transactional database, over its C ABI.
//!
//! The crate links the engine library CMake builds and consumes it through the stable headers in
//! `c/ustore/`, so nothing but the vocabulary below crosses the boundary. One module per
//! header it mirrors.
//!
//! A [`Database`] owns its directory, a [`Transaction`] and a [`Snapshot`] borrow it, and every
//! result borrows the [`Arena`] it landed in, so a value read out of an arena cannot outlive it, nor
//! survive the call that would overwrite it. A [`DeferredView`] submits the same verbs to an [`Executor`]
//! instead, borrowing the arena the same way until the call is final.
//!
//! A [`Collection`] addresses one collection by name, as the Python binding does, and creates it with
//! its first write. Documents parse through JsonTape and batches of values read as StringTape tapes,
//! and both crates are re-exported here.
//!
//! ```no_run
//! use ustore::{jsontape, Arena, Database, Key, RAMOptions, Role};
//!
//! let database = Database::init_ram(&RAMOptions::default())?;
//! let [avatar, titanic] = [1u64, 2].map(Key::from);
//!
//! database.collection("movies")?.docs().set(avatar, &jsontape::parse(br#"{"title":"Avatar"}"#)?)?;
//! database.collection("posters")?.blobs().set(avatar, b"\x89PNG")?;
//! database.collection("similar")?.graph().add_edge(avatar, titanic)?;
//!
//! let mut movies = database.collection("movies")?;
//! let description = movies.docs().get(avatar)?.expect("the movie was written above");
//! assert_eq!(description["title"].as_str(), Some("Avatar"));
//!
//! let mut arena = Arena::new()?;
//! let recommendations = database.collection("similar")?.graph().read_edges(&[avatar], Role::Source)?.len();
//! let images = database.collection("posters")?.sample(1000, &mut arena)?.keys().len();
//! assert_eq!((recommendations, images), (1, 1));
//! # Ok::<(), Box<dyn std::error::Error>>(())
//! ```
//!
//! Beneath the collections, every verb takes a batch, names its collections by identifier, and answers the call's
//! result directly:
//!
//! ```no_run
//! use ustore::{Arena, CollectionID, Collections, RAMOptions, Database, Durability, Entry, Key, Modality};
//!
//! let database = Database::init_ram(&RAMOptions::default())?;
//! let mut arena = Arena::new()?;
//!
//! // Keys are UUIDs, and an integer spells as its big-endian bytes.
//! let [ada, grace, kent] = [1u64, 2, 3].map(Key::from);
//! let people = database.collection_create("people", Modality::Any)?;
//! let places = database.collection_create("places", Modality::Any)?;
//! let entries = [Entry::new(ada, b"Ada"), Entry::new(grace, b"Grace"), Entry::new(kent, b"Kent")];
//! database.head().write(people, &entries, Durability::Buffered)?;
//! database.head().remove(people, &[kent], Durability::Buffered)?;
//!
//! let values = database.head().read(people, &[ada, grace, kent], &mut arena)?;
//! assert_eq!(values.get(0), Some(b"Ada".as_slice()));
//! assert_eq!(values.get(2), None);
//!
//! // A batch carries a collection per task, and a bare one gives every task the same.
//! let spanned = [people, places];
//! let across = [Entry::new(ada, b"Ada"), Entry::new(ada, b"Kent")];
//! database.head().write(spanned.as_slice(), &across, Durability::Buffered)?;
//! let read = database.head().read(Collections::Each(&spanned), &[ada, ada], &mut arena)?;
//! assert_eq!(read.get(1), Some(b"Kent".as_slice()));
//! # Ok::<(), ustore::Error>(())
//! ```
//!
//! The receiver chooses how a call completes, and the verb stays: `.on(&executor)` blocks on an [`Executor`], and
//! `.deferred(&executor)` answers a [`Deferred`] that owns the inputs and borrows the arena until the call is final.
//! A `Deferred` is `IntoFuture`, so any executor of async tasks polls it, such as this one of a dozen `std` lines:
//!
//! ```no_run
//! use std::future::Future;
//! use std::pin::pin;
//! use std::task::{Context, Poll, Waker};
//!
//! use ustore::{Arena, Database, Durability, Entry, Executor, Key, Modality, RAMOptions, Watch};
//! use ustore::stringtape::BytesTape;
//!
//! fn block_on<Output>(task: impl Future<Output = Output>) -> Output {
//!     let mut task = pin!(task);
//!     let mut context = Context::from_waker(Waker::noop());
//!     loop {
//!         if let Poll::Ready(output) = task.as_mut().poll(&mut context) {
//!             return output;
//!         }
//!     }
//! }
//!
//! let database = Database::init_ram(&RAMOptions::default())?;
//! let executor = Executor::for_ram(&[], 8, 256)?;
//! let mut arena = Arena::new_in(&executor)?;
//! let people = database.collection_create("people", Modality::Any)?;
//! let [ada, grace] = [1u64, 2].map(Key::from);
//!
//! let names: BytesTape<u32> = [b"Ada".as_slice(), b"Grace"].into_iter().collect();
//! database.head().on(&executor).write(people, (vec![ada, grace], names), Durability::Buffered)?;
//!
//! block_on(async {
//!     let reading = database.head().deferred(&executor).read(people, vec![ada, grace], &mut arena)?;
//!     let values = reading.await?; // or: reading.wait()?
//!     assert_eq!(values.get(0), Some(b"Ada".as_slice()));
//!     Ok::<(), ustore::Error>(())
//! })?;
//!
//! let transaction = database.transaction()?;
//! let titles = [Entry::new(ada, b"Countess")];
//! transaction.view(Watch::Keys).write(people, &titles, Durability::Buffered)?;
//! let stamp = transaction.deferred(&executor).commit(Durability::Buffered)?.wait()?;
//! # Ok::<(), ustore::Error>(())
//! ```
//!
//! The library rewinds an arena on every call through it, so a second call would overwrite the run the
//! first one exported. That is the borrow the verbs take, and using a result after the next call does not compile:
//!
//! ```compile_fail,E0499
//! use ustore::{Arena, CollectionID, RAMOptions, Database, Key};
//!
//! let database = Database::init_ram(&RAMOptions::default())?;
//! let mut arena = Arena::new()?;
//! let values = database.head().read(CollectionID::main(), &[Key::from(1u64)], &mut arena)?;
//! let overwritten = database.head().read(CollectionID::main(), &[Key::from(2u64)], &mut arena)?;
//! assert_eq!(values.get(0), None);
//! # Ok::<(), ustore::Error>(())
//! ```
//!
//! The library reads a deferred call's inputs after the verb returns, so a deferred view refuses borrowed ones, while
//! it borrows the arena, whose free waits for the call:
//!
//! ```compile_fail,E0277
//! use ustore::{Arena, CollectionID, Database, Executor, Key, RAMOptions};
//!
//! let database = Database::init_ram(&RAMOptions::default())?;
//! let executor = Executor::for_ram(&[], 2, 0)?;
//! let mut arena = Arena::new_in(&executor)?;
//! let keys = [Key::from(1u64)];
//! let reading = database.head().deferred(&executor).read(CollectionID::main(), &keys, &mut arena)?;
//! # Ok::<(), ustore::Error>(())
//! ```
//!
//! The library calls a deferred call's progress handler after the verb returns as well, so a handler borrowed from a
//! local does not compile either:
//!
//! ```compile_fail,E0597
//! use std::ops::ControlFlow;
//! use ustore::{Database, Executor, RAMOptions};
//!
//! let database = Database::init_ram(&RAMOptions::default())?;
//! let executor = Executor::for_ram(&[], 2, 0)?;
//! let mut counting = |_: usize, _: usize| -> ControlFlow<()> { ControlFlow::Continue(()) };
//! let compaction = database.head().deferred(&executor).compact(Some(&mut counting))?;
//! # Ok::<(), ustore::Error>(())
//! ```
//!
//! A view's parent decides which verbs it has beyond the reads, so a call another view serves does not compile. Only a
//! transaction commits:
//!
//! ```compile_fail,E0599
//! use ustore::{Database, Durability, RAMOptions};
//!
//! let database = Database::init_ram(&RAMOptions::default())?;
//! let stamp = database.head().commit(Durability::Buffered)?;
//! # Ok::<(), ustore::Error>(())
//! ```
//!
//! Only the head clears and drops collections:
//!
//! ```compile_fail,E0599
//! use ustore::{CollectionID, Database, DropMode, RAMOptions, Watch};
//!
//! let database = Database::init_ram(&RAMOptions::default())?;
//! let transaction = database.transaction()?;
//! transaction.view(Watch::Keys).collection_drop(CollectionID::main(), DropMode::Pairs)?;
//! # Ok::<(), ustore::Error>(())
//! ```
//!
//! Nothing writes through a snapshot:
//!
//! ```compile_fail,E0599
//! use ustore::{CollectionID, Database, Durability, Key, RAMOptions};
//!
//! let database = Database::init_ram(&RAMOptions::default())?;
//! let snapshot = database.snapshot()?;
//! snapshot.view().remove(CollectionID::main(), [Key::from(1u64)], Durability::Buffered)?;
//! # Ok::<(), ustore::Error>(())
//! ```

pub mod blobs;
pub mod collection;
pub mod db;
pub mod docs;
pub mod graph;
pub mod strings;

#[cfg(test)]
mod testing;

pub use jsontape;
pub use stringtape;

// Named only so their archives link, carrying the objects the UStore archive leaves out.
extern crate forkunion as _;
extern crate stringzilla as _;

pub use blobs::{Cursor, Entry, Measurement, Page, Values};
pub use collection::{Blobs, Collection, Docs, Graph, Sample};
pub use db::{
    key_unknown, version, Arena, Blocking, CollectionID, Collections, CommitStamp, DataDirectory, Database, Deferred,
    DeferredView, Deferring, Description, DropMode, Durability, Engine, Error, Executor, Isolation, Key,
    LevelDBOptions, Modality, Mood, NVMEOptions, ProgressCallback, RAMOptions, ReadStamp, Result, RocksDBOptions,
    Snapshot, Status, Tape, Transaction, Transport, View, Wait, Watch, UUID,
};
pub use docs::{
    Binary, Bitmap, Column, ColumnFormat, Columns, Document, DocumentEntry, FieldAddress, FieldEntry, FieldFormat,
    Half, Json, KeyedBy, Modification, Paths, ReadFormat, Scalar, StringColumn, Text, WritableScalar,
};
pub use graph::{default_edge_id, degree_missing, Degree, Edge, Neighborhoods, Role, Roles};
pub use strings::Names;
