//! UStore, an embedded multi-modal transactional database, over its C ABI.
//!
//! The crate links the engine library CMake builds and consumes it through the stable headers in
//! `c/ustore/`, so nothing but the vocabulary below crosses the boundary. One module per
//! header it mirrors.
//!
//! A [`Database`] owns its directory, a [`Transaction`] and a [`Snapshot`] hold it, and every
//! result views the [`Arena`] it landed in, so a value read out of an arena cannot outlive it, nor
//! survive the call that would overwrite it. A [`DeferredOn`] defers the same verbs to an
//! [`Executor`] instead, holding the arena the same way until the call is final.
//!
//! A [`Collection`] addresses one collection by name, as the Python binding does:
//! [`Database::collection`] finds it, and [`Database::find_or_create`] creates a missing one.
//! Documents parse through JsonTape and batches of values read as StringTape tapes, and both crates
//! are re-exported here.
//!
//! ```no_run
//! use ustore::{jsontape, Arena, Database, Key, Modality, RAMOptions, Role};
//!
//! let database = Database::init_ram(&RAMOptions::default())?;
//! let [avatar, titanic] = [1u64, 2].map(Key::from);
//!
//! let movies = database.find_or_create("movies", Modality::Docs)?;
//! movies.docs().set(avatar, &jsontape::parse(br#"{"title":"Avatar"}"#)?)?;
//! database.find_or_create("posters", Modality::Blobs)?.blobs().set(avatar, b"\x89PNG")?;
//! database.find_or_create("similar", Modality::Graph)?.graph().add_edge(avatar, titanic)?;
//!
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
//! Beneath the collections, every verb takes a batch, names its collections by identifier, and
//! answers the call's result directly. The verbs of each modality come from a trait, [`BlobsVerbs`]
//! for binary values, implemented by every receiver, so a handle runs them on the calling thread:
//!
//! ```no_run
//! use ustore::{Arena, BlobsVerbs, Collections, Database, Durability, Entry, Key, Modality, RAMOptions};
//!
//! let database = Database::init_ram(&RAMOptions::default())?;
//! let mut arena = Arena::new()?;
//!
//! // Keys are UUIDs, and an integer spells as its big-endian bytes.
//! let [ada, grace, kent] = [1u64, 2, 3].map(Key::from);
//! let people = database.collection_create("people", Modality::Blobs)?;
//! let places = database.collection_create("places", Modality::Blobs)?;
//! let entries = [Entry::new(ada, b"Ada"), Entry::new(grace, b"Grace"), Entry::new(kent, b"Kent")];
//! database.write(people, &entries, Durability::Buffered)?;
//! database.remove(people, &[kent], Durability::Buffered)?;
//!
//! let values = database.read(people, &[ada, grace, kent], &mut arena)?;
//! assert_eq!(values.get(0), Some(b"Ada".as_slice()));
//! assert_eq!(values.get(2), None);
//!
//! // A batch holds a collection per task, and a bare one gives every task the same.
//! let spanned = [people, places];
//! let across = [Entry::new(ada, b"Ada"), Entry::new(ada, b"Kent")];
//! database.write(spanned.as_slice(), &across, Durability::Buffered)?;
//! let read = database.read(Collections::Each(&spanned), &[ada, ada], &mut arena)?;
//! assert_eq!(read.get(1), Some(b"Kent".as_slice()));
//! # Ok::<(), ustore::Error>(())
//! ```
//!
//! The receiver chooses how a call completes, and the verb stays: `.on(&executor)` blocks on an
//! [`Executor`], and `.deferred(&executor)` answers a [`Deferred`] that owns the inputs and holds
//! the arena until the call is final. A `Deferred` is the call's `Future`: it submits when first
//! polled or waited on, at the address it is pinned at, so it allocates nothing, and any executor
//! of async tasks polls it, such as this one of a dozen `std` lines:
//!
//! ```no_run
//! use std::future::Future;
//! use std::pin::pin;
//! use std::task::{Context, Poll, Waker};
//!
//! use ustore::stringtape::BytesTape;
//! use ustore::{Arena, BlobsVerbs, Database, Durability, Entry, Executor, Key, Modality, RAMOptions};
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
//! let executor = Executor::for_ram(&[], 8)?;
//! let mut arena = Arena::new_in(&executor)?;
//! let people = database.collection_create("people", Modality::Blobs)?;
//! let [ada, grace] = [1u64, 2].map(Key::from);
//!
//! let names: BytesTape<u32> = [b"Ada".as_slice(), b"Grace"].into_iter().collect();
//! database.head().on(&executor).write(people, (vec![ada, grace], names), Durability::Buffered)?;
//!
//! let call = database.head().deferred(&executor).read(people, vec![ada, grace], &mut arena)?;
//! let values = block_on(async { call.await })?; // or: call.wait()?
//! assert_eq!(values.get(0), Some(b"Ada".as_slice()));
//!
//! let transaction = database.transaction()?;
//! let titles = [Entry::new(ada, b"Countess")];
//! transaction.write(people, &titles, Durability::Buffered)?;
//! let stamp = transaction.deferred(&executor).commit(Durability::Buffered)?.wait()?;
//! # Ok::<(), ustore::Error>(())
//! ```
//!
//! The library rewinds an arena on every call through it, so a second call would overwrite the run
//! the first one exported. That is the `&mut` the verbs take, and using a result after the next
//! call does not compile:
//!
//! ```compile_fail,E0499
//! use ustore::{Arena, BlobsVerbs, Database, Key, Modality, RAMOptions};
//!
//! let database = Database::init_ram(&RAMOptions::default())?;
//! let mut arena = Arena::new()?;
//! let people = database.collection_create("people", Modality::Blobs)?;
//! let values = database.read(people, &[Key::from(1u64)], &mut arena)?;
//! let overwritten = database.read(people, &[Key::from(2u64)], &mut arena)?;
//! assert_eq!(values.get(0), None);
//! # Ok::<(), ustore::Error>(())
//! ```
//!
//! The library reads a deferred call's inputs after it is submitted, so a deferred view refuses
//! viewed ones, while it holds the arena, whose free waits for the call:
//!
//! ```compile_fail,E0277
//! use ustore::{Arena, BlobsVerbs, Database, Executor, Key, Modality, RAMOptions};
//!
//! let database = Database::init_ram(&RAMOptions::default())?;
//! let executor = Executor::for_ram(&[], 2)?;
//! let mut arena = Arena::new_in(&executor)?;
//! let people = database.collection_create("people", Modality::Blobs)?;
//! let keys = [Key::from(1u64)];
//! let reading = database.head().deferred(&executor).read(people, &keys, &mut arena)?;
//! # Ok::<(), ustore::Error>(())
//! ```
//!
//! The library calls a deferred call's progress handler after it is submitted as well, so a handler
//! referencing a local does not compile either:
//!
//! ```compile_fail,E0597
//! use std::ops::ControlFlow;
//! use ustore::{CompactConfig, Database, Executor, Progress, RAMOptions};
//!
//! let database = Database::init_ram(&RAMOptions::default())?;
//! let executor = Executor::for_ram(&[], 2)?;
//! let mut counting = |_: usize, _: usize| -> ControlFlow<()> { ControlFlow::Continue(()) };
//! let config = CompactConfig { progress: Some(Progress::new(&mut counting)), ..CompactConfig::default() };
//! let compaction = database.head().deferred(&executor).compact(config)?;
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
//! use ustore::{Database, DropMode, Modality, RAMOptions, Watch};
//!
//! let database = Database::init_ram(&RAMOptions::default())?;
//! let people = database.collection_create("people", Modality::Blobs)?;
//! let transaction = database.transaction()?;
//! transaction.view(Watch::Keys).collection_drop(people, DropMode::Pairs)?;
//! # Ok::<(), ustore::Error>(())
//! ```
//!
//! Nothing writes through a snapshot:
//!
//! ```compile_fail,E0277
//! use ustore::{BlobsVerbs, Database, Durability, Key, Modality, RAMOptions};
//!
//! let database = Database::init_ram(&RAMOptions::default())?;
//! let people = database.collection_create("people", Modality::Blobs)?;
//! let snapshot = database.snapshot()?;
//! snapshot.remove(people, [Key::from(1u64)], Durability::Buffered)?;
//! # Ok::<(), ustore::Error>(())
//! ```
//!
//! File: rust/lib.rs
//! Author: Ash Vardanian
//! Date: September 16, 2026

#![warn(missing_docs)]

pub mod arrow;
pub mod blobs;
pub mod collection;
pub mod db;
pub mod docs;
pub mod graph;
pub mod pages;
pub mod strings;

#[cfg(test)]
mod testing;

pub use jsontape;
pub use stringtape;

// Named only so their archives link, holding the objects the UStore archive leaves out.
extern crate forkunion as _;
extern crate stringzilla as _;

pub use arrow::{ArrowArray, ArrowArrayStream, ArrowExport, ArrowSchema, IngestOrder};
pub use blobs::{
    BlobsCursorConfig, BlobsSampleConfig, BlobsScanConfig, BlobsVerbs, Cursor, Entry, Measurement, Page, Scanned,
    Values,
};
pub use collection::{Blobs, Collection, Docs, Graph, Owned, Sample};
pub use db::{
    Admission, Arena, ArenaDescription, Blocking, CPUClass, CaseSensitivity, ChangeKind, Changes, ChangesCursor,
    ChangesCursorConfig, CollectionID, Collections, CommitStamp, CompactConfig, CompactPlan, CursorOn, DataDirectory,
    Database, Deferred, DeferredOn, Deferring, Description, DropMode, Durability, Engine, Error, Executor, GPUClass,
    GPUDevice, GPUVendor, Gds, IOOptions, Isolation, Key, KeyRange, LevelDBOptions, Matching, Modality, Mood,
    NVMEOptions, NVMEViewOptions, On, PatternKind, Phase, PhaseProgress, Predicate, PredicateCost, Progress,
    RAMOptions, ReadStamp, Residency, Result, RocksDBOptions, SaveConfig, ScanDirection, ScanMode, Snapshot, Status,
    Tape, Transaction, Transport, UUID, ViewVerbs, Watch, gpu_devices, key_unknown, version,
};
pub use docs::{
    Batches, Binary, Bitmap, Column, ColumnFormat, Columns, DocsCursor, DocsCursorConfig, DocsGatherConfig,
    DocsSampleConfig, DocsScanConfig, DocsVerbs, Document, DocumentEntry, FieldAddress, FieldEntry, FieldFormat,
    FieldType, Half, Json, KeyedBy, Modification, Paths, ReadFormat, Scalar, StringColumn, Text, WritableScalar,
};
pub use graph::{
    Degree, Edge, GraphSampleVerticesConfig, GraphScanVerticesConfig, GraphVerbs, GraphVerticesCursor,
    GraphVerticesCursorConfig, Neighborhoods, Role, Roles, default_edge_id, degree_missing,
};
pub use pages::{
    PageEncoding, PagesCursor, PagesCursorConfig, PagesImportConfig, PagesInsertConfig, PagesReadConfig,
    PagesSampleConfig, PagesScanConfig, PagesVerbs,
};
pub use strings::{Listing, Mapped, Names, StringsSampleConfig, StringsScanConfig, StringsVerbs, TextRange};
