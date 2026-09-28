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
//! use ustore::{Arena, Collection, Collections, RAMOptions, Database, Durability, Entry};
//!
//! let database = Database::open_ram(&RAMOptions::default())?;
//! let mut arena = Arena::new();
//!
//! let people = database.create_collection(&mut arena, "people")?;
//! let places = database.create_collection(&mut arena, "places")?;
//! let entries = [Entry::new(1, b"Ada")?, Entry::new(2, b"Grace")?, Entry::removal(3)];
//! database.head().write(&mut arena, people, &entries, Durability::Buffered)?;
//!
//! let values = database.head().read(&mut arena, people, &[1, 2])?;
//! assert_eq!(values.get(0), Some(b"Ada".as_slice()));
//!
//! // A batch carries a collection per task, and a bare one gives every task the same.
//! let spanned = [people, places];
//! let across = [Entry::new(1, b"Ada")?, Entry::new(1, b"Kent")?];
//! database.head().write(&mut arena, spanned.as_slice(), &across, Durability::Buffered)?;
//! let read = database.head().read(&mut arena, Collections::Each(&spanned), &[1, 1])?;
//! assert_eq!(read.get(1), Some(b"Kent".as_slice()));
//! # Ok::<(), ustore::Error>(())
//! ```
//!
//! The engine resets an arena on every call through it, so a second call would overwrite the run the
//! first one exported. That is the borrow the verbs take, and it does not compile:
//!
//! ```compile_fail
//! use ustore::{Arena, Collection, RAMOptions, Database};
//!
//! let database = Database::open_ram(&RAMOptions::default())?;
//! let mut arena = Arena::new();
//! let values = database.head().read(&mut arena, Collection::main(), &[1])?;
//! let overwritten = database.head().read(&mut arena, Collection::main(), &[2])?;
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
    key_unknown, Arena, Capabilities, Collection, Collections, Conflicts, DataDirectory, Database, Durability, Error,
    Isolation, Key, LevelDBOptions, NVMEOptions, RAMOptions, Removed, Result, RocksDBOptions, SequenceNumber, Snapshot,
    SnapshotId, Status, Transaction, Transport, View,
};
pub use docs::{
    Binary, Bitmap, Column, ColumnFormat, Document, DocumentEntry, FieldAddress, FieldEntry, FieldFormat, Half, Json,
    Modification, Paths, ReadFormat, Scalar, Strings, Table, Text, Uuid, WritableScalar,
};
pub use graph::{default_edge_id, degree_missing, Degree, Edge, Neighborhoods, Role, Roles};
pub use strings::Names;
