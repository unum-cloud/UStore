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
//! use ustore::{Arena, Collection, Configuration, Database, Durability, Entry};
//!
//! let database = Database::open(Configuration::in_memory())?;
//! let mut arena = Arena::new();
//!
//! let people = database.create_collection(&mut arena, "people")?;
//! let entries = [Entry::new(1, b"Ada")?, Entry::new(2, b"Grace")?];
//! database.head().write(&mut arena, people, &entries, Durability::Buffered)?;
//!
//! let values = database.head().read(&mut arena, people, &[1, 2])?;
//! assert_eq!(values.get(0), Some(b"Ada".as_slice()));
//! # Ok::<(), ustore::Error>(())
//! ```
//!
//! The engine resets an arena on every call through it, so a second call would overwrite the run the
//! first one exported. That is the borrow the verbs take, and it does not compile:
//!
//! ```compile_fail
//! use ustore::{Arena, Collection, Configuration, Database};
//!
//! let database = Database::open(Configuration::in_memory())?;
//! let mut arena = Arena::new();
//! let values = database.head().read(&mut arena, Collection::main(), &[1])?;
//! let overwritten = database.head().read(&mut arena, Collection::main(), &[2])?;
//! assert_eq!(values.get(0), None);
//! # Ok::<(), ustore::Error>(())
//! ```

pub mod blobs;
pub mod db;
pub mod strings;

#[cfg(test)]
mod testing;

pub use blobs::{Entry, Measurement, Values};
pub use db::{
    key_unknown, Arena, Collection, Configuration, Database, Durability, Error, Isolation, Key, Metadata, Removed,
    Result, SequenceNumber, Snapshot, SnapshotId, Status, Transaction, View,
};
pub use strings::Names;
