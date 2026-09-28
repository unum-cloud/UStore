//! UStore, an embedded multi-modal transactional database, over its C ABI.
//!
//! The crate links the engine library CMake builds and consumes it through the stable headers in
//! `include/ustore/`, so nothing but the vocabulary below crosses the boundary. One module per
//! header it mirrors.
//!
//! A [`Database`] owns its directory, a [`Transaction`] and a [`Snapshot`] borrow it, and every
//! result borrows the [`Arena`] the engine exported it into - so a value read out of an arena cannot
//! outlive it, nor survive the call that would overwrite it.
//!
//! ```no_run
//! use ustore::{Arena, CollectionID, Collections, RAMOptions, Database, Durability, Entry, Key, Modality};
//!
//! let database = Database::open_ram(&RAMOptions::default())?;
//! let mut arena = Arena::new();
//!
//! // Keys are UUIDs, and an integer spells as its big-endian bytes.
//! let [ada, grace, kent] = [1u64, 2, 3].map(Key::from);
//! let people = database.create_collection("people", Modality::Any)?;
//! let places = database.create_collection("places", Modality::Any)?;
//! let entries = [Entry::new(ada, b"Ada")?, Entry::new(grace, b"Grace")?, Entry::removal(kent)];
//! database.head().write(&mut arena, people, &entries, Durability::Buffered)?;
//!
//! let values = database.head().read(&mut arena, people, &[ada, grace])?;
//! assert_eq!(values.get(0), Some(b"Ada".as_slice()));
//!
//! // A batch carries a collection per task, and a bare one gives every task the same.
//! let spanned = [people, places];
//! let across = [Entry::new(ada, b"Ada")?, Entry::new(ada, b"Kent")?];
//! database.head().write(&mut arena, spanned.as_slice(), &across, Durability::Buffered)?;
//! let read = database.head().read(&mut arena, Collections::Each(&spanned), &[ada, ada])?;
//! assert_eq!(read.get(1), Some(b"Kent".as_slice()));
//! # Ok::<(), ustore::Error>(())
//! ```
//!
//! The engine resets an arena on every call through it, so a second call would overwrite the run the
//! first one exported. That is the borrow the verbs take, and it does not compile:
//!
//! ```compile_fail
//! use ustore::{Arena, CollectionID, RAMOptions, Database, Key};
//!
//! let database = Database::open_ram(&RAMOptions::default())?;
//! let mut arena = Arena::new();
//! let values = database.head().read(&mut arena, CollectionID::main(), &[Key::from(1u64)])?;
//! let overwritten = database.head().read(&mut arena, CollectionID::main(), &[Key::from(2u64)])?;
//! assert_eq!(values.get(0), None);
//! # Ok::<(), ustore::Error>(())
//! ```

pub mod blobs;
pub mod db;
pub mod docs;
pub mod graph;
pub mod strings;

#[cfg(test)]
mod testing;

pub use blobs::{Entry, Measurement, Values};
pub use db::{
    key_unknown, Arena, Capabilities, CollectionID, Collections, Conflicts, DataDirectory, Database, Durability, Error,
    Isolation, Key, LevelDBOptions, Modality, NVMEOptions, RAMOptions, Removed, Result, RocksDBOptions, Snapshot,
    Stamp, Status, Transaction, Transport, View, UUID,
};
pub use docs::{
    Binary, Bitmap, Column, ColumnFormat, Document, DocumentEntry, FieldAddress, FieldEntry, FieldFormat, Half, Json,
    Modification, Paths, ReadFormat, Scalar, Strings, Table, Text, WritableScalar,
};
pub use graph::{default_edge_id, degree_missing, Degree, Edge, Neighborhoods, Role, Roles};
pub use strings::Names;
