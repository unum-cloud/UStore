//! Collections addressed by name, as the Python binding indexes a database: `db['movies'].docs[avatar]`.
//!
//! A [`Collection`] is the identifier a name answers to at the head, or at its view's snapshot, and
//! the view it reads and writes through. [`Database::collection`] only finds, failing with
//! [`Status::NotFound`] for a name no collection carries, while [`Database::find_or_create`]
//! creates a missing one at the head, where collections are created. A view of another modality
//! fails with [`Status::InvalidArgument`].
//!
//! A collection keeps no arena. A call with no arena to pass reads through a one-off arena, which
//! the [`Owned`] value it answers keeps: slower than reusing an arena, and stateless. Batch reads
//! take the [`Arena`] they land in instead.
//!
//! ```no_run
//! use ustore::{jsontape, Arena, Database, Key, Modality, RAMOptions, Role};
//!
//! let database = Database::init_ram(&RAMOptions::default())?;
//! let [avatar, titanic] = [1u64, 2].map(Key::from);
//!
//! let avatar_document = jsontape::parse(br#"{"title":"Avatar","year":2009}"#)?;
//! database.find_or_create("movies", Modality::Docs)?.docs().set(avatar, &avatar_document)?;
//! database.find_or_create("posters", Modality::Blobs)?.blobs().set(avatar, b"\x89PNG")?;
//! database.find_or_create("similar", Modality::Graph)?.graph().add_edge(avatar, titanic)?;
//!
//! let movies = database.collection("movies")?;
//! let description = movies.docs().get(avatar)?.expect("the movie was written above");
//! assert_eq!(description["year"].as_u64(), Some(2009));
//!
//! let similar = database.collection("similar")?;
//! let recommendations = similar.graph().read_edges(&[avatar], Role::Source)?;
//! assert_eq!(recommendations[0].target, titanic);
//!
//! let mut arena = Arena::new()?;
//! let posters = database.collection("posters")?;
//! let poster = posters.blobs().get(avatar)?.expect("the poster was written above");
//! assert_eq!(&*poster, b"\x89PNG");
//! let batch = posters.blobs().read(&[avatar, titanic], &mut arena)?;
//! assert_eq!(batch.get(0), Some(b"\x89PNG".as_slice()));
//!
//! let mut transaction = database.transaction()?;
//! transaction.collection("posters")?.blobs().set(titanic, b"\x89PNG")?;
//! transaction.commit(Durability::Buffered)?;
//! # use ustore::Durability;
//! # Ok::<(), Box<dyn std::error::Error>>(())
//! ```
//!
//! A batch read through a collection borrows the arena it landed in, so the next read into that arena does not compile
//! while the values live:
//!
//! ```compile_fail,E0499
//! use ustore::{Arena, Database, Key, Modality, RAMOptions};
//!
//! let database = Database::init_ram(&RAMOptions::default())?;
//! let posters = database.find_or_create("posters", Modality::Blobs)?;
//! let mut arena = Arena::new()?;
//! let first = posters.blobs().read(&[Key::from(1u64)], &mut arena)?;
//! let second = posters.blobs().read(&[Key::from(2u64)], &mut arena)?;
//! assert_eq!(first.get(0), second.get(0));
//! # Ok::<(), ustore::Error>(())
//! ```
//!
//! A collection of a transaction borrows the transaction, so no call goes through it after the commit:
//!
//! ```compile_fail
//! use ustore::{Database, Durability, Key, Modality, RAMOptions};
//!
//! let database = Database::init_ram(&RAMOptions::default())?;
//! database.find_or_create("posters", Modality::Blobs)?;
//! let mut transaction = database.transaction()?;
//! let posters = transaction.collection("posters")?;
//! transaction.commit(Durability::Buffered)?;
//! posters.blobs().set(Key::from(1u64), b"\x89PNG")?;
//! # Ok::<(), ustore::Error>(())
//! ```
//!
//! File: rust/collection.rs
//! Author: Ash Vardanian
//! Date: October 7, 2026

use std::fmt;
use std::ops::Deref;

use crate::arrow::{self, ArrowArrayStream, IngestOrder};
use crate::blobs::{BlobsVerbs, Entry, Values};
use crate::db::sealed::Writable;
use crate::db::{
    Arena, CollectionID, Database, Durability, Error, Key, Modality, On, Result, Status, Transaction, Watch,
    exported_slice, fail,
};
use crate::docs::{DocsVerbs, DocumentEntry, Json, Modification};
use crate::graph::{Degree, Edge, GraphVerbs, Role};
use crate::strings::{StringsVerbs, create_named};

// region: Collections

impl Database {
    /// The collection `name` addresses at the head, refusing a name no collection carries with
    /// [`Status::NotFound`].
    pub fn collection(&self, name: &str) -> Result<Collection<'_>> {
        Collection::find(self.head(), name)
    }

    /// The collection `name` addresses at the head, created to hold `modality` when no collection
    /// carries the name.
    ///
    /// Calls `ustore_collection_find`, then `ustore_collection_create` for a missing one.
    pub fn find_or_create(&self, name: &str, modality: Modality) -> Result<Collection<'_>> {
        match self.collection(name) {
            Err(failure) if failure.status == Status::NotFound => {}
            found => return found,
        }
        match create_named(self.handle(), name, modality) {
            Ok(collection) => Ok(Collection { view: self.head(), collection, modality }),
            // Another writer created it in between, perhaps holding another modality.
            Err(failure) if failure.status == Status::AlreadyExists => self.collection(name),
            Err(failure) => Err(failure),
        }
    }
}

impl<'db> Transaction<'db> {
    /// The collection `name` addresses, resolved at the head, inside this transaction, which
    /// watches what it touches.
    ///
    /// The collection borrows the transaction, so no call reaches it past [`Transaction::commit`].
    pub fn collection(&self, name: &str) -> Result<Collection<'_, Transaction<'db>>> {
        Collection::find(self.view(Watch::Keys), name)
    }
}

/// A collection a name addresses, which reads and writes through one view of its `Parent`, the head
/// of a [`Database`] or a [`Transaction`].
///
/// Every verb goes through a view of one modality: [`Collection::blobs`], [`Collection::docs`] or
/// [`Collection::graph`]. A call with no arena to pass reads through a one-off arena that the
/// [`Owned`] value it answers keeps, while a batch read borrows the [`Arena`] it is given.
/// Writes land [`Durability::Buffered`].
#[derive(Debug)]
pub struct Collection<'view, Parent = Database> {
    view: On<'view, Parent>,
    collection: CollectionID,
    modality: Modality,
}

impl<'view, Parent: Writable> Collection<'view, Parent> {
    /// The collection `name` answers to through `view`, resolved through a one-off arena.
    fn find(view: On<'view, Parent>, name: &str) -> Result<Self> {
        let (collection, modality) = view.collection_find(name, &mut Arena::new()?)?;
        Ok(Collection { view, collection, modality })
    }

    /// The binary values of the collection, as `db['posters'].blobs` addresses them.
    pub fn blobs(&self) -> Blobs<'_, 'view, Parent> {
        Blobs { collection: self }
    }

    /// The JSON documents of the collection, as `db['movies'].docs` addresses them.
    pub fn docs(&self) -> Docs<'_, 'view, Parent> {
        Docs { collection: self }
    }

    /// The edges of the collection, as `db['similar'].graph` addresses them.
    pub fn graph(&self) -> Graph<'_, 'view, Parent> {
        Graph { collection: self }
    }

    /// Draws up to `wanted` keys of a blobs collection uniformly at random into `arena`, each with its value read
    /// through the same view.
    pub fn sample<'arena>(&self, wanted: usize, arena: &'arena mut Arena<'_>) -> Result<Sample<'arena>> {
        let collection = self.holding(Modality::Blobs)?;
        let keys = self.view.sample(collection, wanted, &mut *arena)?.to_vec();
        let values = self.view.read(collection, &keys, arena)?;
        Ok(Sample { keys, values })
    }

    /// The collection, when a view of `modality` may reach what it holds.
    fn holding(&self, modality: Modality) -> Result<CollectionID> {
        if self.modality == modality {
            return Ok(self.collection);
        }
        let message = match modality {
            Modality::Blobs => "the collection holds no blobs",
            Modality::Docs => "the collection holds no documents",
            _ => "the collection holds no graph",
        };
        Err(fail(Status::InvalidArgument, message))
    }

    /// The whole collection as one Arrow stream through the view, whose batches outlive the stream; a head view
    /// reads a snapshot the stream holds until dropped.
    ///
    /// Every pull runs the cursor of the collection's modality into an arena of its own, which the exporters of
    /// `c/ustore/arrow.h` hand out as the batch's columns, and which the batch frees once released.
    pub fn export_arrow(&self) -> Result<ArrowArrayStream> {
        arrow::export(&self.view, self.collection, self.modality)
    }

    /// Writes every batch of `stream` through the writes of the collection's modality, answering the rows written.
    ///
    /// The converters of `c/ustore/arrow.h` read every batch's columns in place, and the last write pays
    /// `durability`. The call takes the stream over and releases it, even on failure.
    pub fn import_arrow(&self, stream: ArrowArrayStream, durability: Durability) -> Result<usize> {
        arrow::import(&self.view, self.collection, self.modality, stream, durability)
    }
}

impl Collection<'_, Database> {
    /// Lands every batch of `stream` as one commit of its own at the head, answering the rows landed.
    ///
    /// Appends every batch to one ingest of `ustore_ingest_init`, which `ustore_ingest_finish` lands. The call takes
    /// the stream over and releases it, even on failure.
    pub fn ingest_arrow(&self, stream: ArrowArrayStream, order: IngestOrder, durability: Durability) -> Result<usize> {
        arrow::ingest(self.view.database(), self.collection, self.modality, stream, order, durability)
    }
}

/// What one shorthand call read through a one-off arena, which it keeps, readable for as long as it
/// lives, with no copy: the bytes of a value, the text of a document, or a run of edges or degrees.
///
/// Moving it moves only the arena's handle, so the values stay where the call wrote them.
pub struct Owned<Value = u8> {
    arena: Arena<'static>,
    start: *const Value,
    length: usize,
}

impl<Value> Deref for Owned<Value> {
    type Target = [Value];

    fn deref(&self) -> &[Value] {
        let _ = &self.arena;
        // SAFETY: the run lives in host pages of the arena this value owns, which nothing rewinds.
        unsafe { exported_slice(self.start, self.length) }
    }
}

impl<Value: fmt::Debug> fmt::Debug for Owned<Value> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Debug::fmt(&**self, formatter)
    }
}

// SAFETY: the run is only ever read, and the arena it lives in moves between threads freely.
unsafe impl<Value: Sync> Send for Owned<Value> {}
unsafe impl<Value: Sync> Sync for Owned<Value> {}

// endregion: Collections

// region: Views

/// The binary values of one collection.
///
/// Every verb consumes the view; [`Collection::blobs`] hands out the next one.
#[derive(Debug)]
pub struct Blobs<'collection, 'view, Parent = Database> {
    collection: &'collection Collection<'view, Parent>,
}

impl<Parent: Writable> Blobs<'_, '_, Parent> {
    /// The value under `key`, read into a one-off arena it keeps, or `None` for a key the
    /// collection does not hold.
    pub fn get(self, key: Key) -> Result<Option<Owned>> {
        let mut arena = Arena::new()?;
        let value = self.read(&[key], &mut arena)?.get(0).map(|value| (value.as_ptr(), value.len()));
        Ok(value.map(|(start, length)| Owned { arena, start, length }))
    }

    /// The values under `keys` in order, read into `arena`, which [`Values::tape`] hands out as one tape.
    pub fn read<'arena>(self, keys: &[Key], arena: &'arena mut Arena<'_>) -> Result<Values<'arena>> {
        let collection = self.collection.holding(Modality::Blobs)?;
        self.collection.view.read(collection, keys, arena)
    }

    /// Whether the collection holds `key`, exporting no value into the one-off arena it reads through.
    pub fn contains(self, key: Key) -> Result<bool> {
        let collection = self.collection.holding(Modality::Blobs)?;
        Ok(self.collection.view.contains(collection, &[key], &mut Arena::new()?)?.get(0))
    }

    /// Stores `value` under `key`.
    pub fn set(self, key: Key, value: &[u8]) -> Result<()> {
        let collection = self.collection.holding(Modality::Blobs)?;
        let entries = [Entry::new(key, value)];
        self.collection.view.write(collection, &entries, Durability::Buffered)?;
        Ok(())
    }

    /// Removes `key`, whether or not the collection holds it.
    pub fn remove(self, key: Key) -> Result<()> {
        let collection = self.collection.holding(Modality::Blobs)?;
        self.collection.view.remove(collection, [key], Durability::Buffered)?;
        Ok(())
    }
}

/// The JSON documents of one collection, parsed through JsonTape.
///
/// Every verb consumes the view; [`Collection::docs`] hands out the next one.
#[derive(Debug)]
pub struct Docs<'collection, 'view, Parent = Database> {
    collection: &'collection Collection<'view, Parent>,
}

impl<Parent: Writable> Docs<'_, '_, Parent> {
    /// The document under `key`, parsed out of the one-off arena it was read into, or `None` for a key without one.
    pub fn get(self, key: Key) -> Result<Option<jsontape::Json>> {
        self.parsed(key, |text| jsontape::parse(text).map_err(not_json))
    }

    /// The document under `key` as the JSON text the engine holds, in the one-off arena it keeps,
    /// or `None` for a key without one.
    pub fn get_json(self, key: Key) -> Result<Option<Owned>> {
        let collection = self.collection.holding(Modality::Docs)?;
        let mut arena = Arena::new()?;
        let Some(text) = self.collection.view.docs_read(collection, [key], Json, &mut arena)?.get(0) else {
            return Ok(None);
        };
        if std::str::from_utf8(text).is_err() {
            return Err(fail(Status::Malformed, "the document is not valid JSON"));
        }
        let (start, length) = (text.as_ptr(), text.len());
        Ok(Some(Owned { arena, start, length }))
    }

    /// The document under `key` deserialized into `Value`, or `None` for a key without one.
    #[cfg(feature = "serde")]
    pub fn get_as<Value: serde::de::DeserializeOwned>(self, key: Key) -> Result<Option<Value>> {
        self.parsed(key, |text| {
            let document = jsontape::parse(text).map_err(not_json)?;
            jsontape::from_value(&document).map_err(|_| fail(Status::Malformed, "the document does not fit that type"))
        })
    }

    /// Stores `document` under `key`.
    pub fn set(self, key: Key, document: &jsontape::Json) -> Result<()> {
        let collection = self.collection.holding(Modality::Docs)?;
        let text = document.to_string();
        let entries = [DocumentEntry::<Json>::new(key, &text)];
        self.collection.view.docs_write(collection, &entries, Modification::Upsert, Durability::Buffered)?;
        Ok(())
    }

    /// Removes the document under `key`, whether or not the collection holds one.
    pub fn remove(self, key: Key) -> Result<()> {
        let collection = self.collection.holding(Modality::Docs)?;
        self.collection.view.docs_remove(collection, [key], Durability::Buffered)?;
        Ok(())
    }

    /// What `parse` makes of the text of the document under `key` while the one-off arena it was read into lives, or
    /// `None` for a key without one.
    fn parsed<Output>(self, key: Key, parse: impl FnOnce(&[u8]) -> Result<Output>) -> Result<Option<Output>> {
        let collection = self.collection.holding(Modality::Docs)?;
        let mut arena = Arena::new()?;
        let documents = self.collection.view.docs_read(collection, [key], Json, &mut arena)?;
        documents.get(0).map(parse).transpose()
    }
}

/// The error a failed parse reports: memory the heap refused, or text that is not JSON.
fn not_json(failure: jsontape::JsonError) -> Error {
    match failure {
        jsontape::JsonError::Allocation => fail(Status::OutOfMemory, "the document could not be parsed in memory"),
        _ => fail(Status::Malformed, "the document is not valid JSON"),
    }
}

/// The edges of one collection, each under the default identifier.
///
/// Every verb consumes the view; [`Collection::graph`] hands out the next one.
#[derive(Debug)]
pub struct Graph<'collection, 'view, Parent = Database> {
    collection: &'collection Collection<'view, Parent>,
}

impl<Parent: Writable> Graph<'_, '_, Parent> {
    /// Adds an edge from `source` to `target`.
    pub fn add_edge(self, source: Key, target: Key) -> Result<()> {
        let collection = self.collection.holding(Modality::Graph)?;
        let edges = [Edge::new(source, target)];
        self.collection.view.graph_write_edges(collection, edges, Durability::Buffered)?;
        Ok(())
    }

    /// Removes the edge from `source` to `target`, whether or not the graph holds it.
    pub fn remove_edge(self, source: Key, target: Key) -> Result<()> {
        let collection = self.collection.holding(Modality::Graph)?;
        let edges = [Edge::new(source, target)];
        self.collection.view.graph_remove_edges(collection, edges, Durability::Buffered)?;
        Ok(())
    }

    /// The edges of every vertex in `role`, one run after another, in the one-off arena it keeps,
    /// as `db['similar'].graph.read_edges(avatar)` lists them.
    pub fn read_edges(self, vertices: &[Key], role: Role) -> Result<Owned<Edge>> {
        let collection = self.collection.holding(Modality::Graph)?;
        let mut arena = Arena::new()?;
        let edges = self.collection.view.graph_read_edges(collection, vertices, role, &mut arena)?.edges();
        let (start, length) = (edges.as_ptr(), edges.len());
        Ok(Owned { arena, start, length })
    }

    /// The degree of every vertex in `role`, in the one-off arena it keeps, which exports no edges.
    pub fn degrees(self, vertices: &[Key], role: Role) -> Result<Owned<Degree>> {
        let collection = self.collection.holding(Modality::Graph)?;
        let mut arena = Arena::new()?;
        let degrees = self.collection.view.graph_degrees(collection, vertices, role, &mut arena)?;
        let (start, length) = (degrees.as_ptr(), degrees.len());
        Ok(Owned { arena, start, length })
    }
}

// endregion: Views

// region: Samples

/// Keys drawn at random from a collection, with the values read under them through the same view into an arena.
#[derive(Clone, Debug)]
pub struct Sample<'arena> {
    keys: Vec<Key>,
    values: Values<'arena>,
}

impl<'arena> Sample<'arena> {
    /// The keys drawn, in the order of their values.
    pub fn keys(&self) -> &[Key] {
        &self.keys
    }

    /// The value under every key, where a key removed between the draw and the read is missing.
    pub fn values(&self) -> Values<'arena> {
        self.values
    }
}

// endregion: Samples

// region: Tests

#[cfg(test)]
mod tests {
    use super::*;
    use crate::db::RAMOptions;
    use crate::testing::{key, keys};

    /// The JSON document `text` spells.
    fn document(text: &str) -> jsontape::Json {
        jsontape::parse(text.as_bytes()).unwrap()
    }

    #[test]
    fn movies_read_back_as_documents() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let avatar = key(1);
        let written = document(r#"{"title":"Avatar","year":2009}"#);
        let movies = database.find_or_create("movies", Modality::Docs).unwrap();
        movies.docs().set(avatar, &written).unwrap();

        let description = movies.docs().get(avatar).unwrap().unwrap();
        assert_eq!(description["title"].as_str(), Some("Avatar"));
        assert_eq!(description["year"].as_u64(), Some(2009));
        let text = movies.docs().get_json(avatar).unwrap().unwrap();
        assert_eq!(&*text, br#"{"title":"Avatar","year":2009}"#);
        assert!(movies.docs().get(key(2)).unwrap().is_none());

        movies.docs().remove(avatar).unwrap();
        assert!(movies.docs().get(avatar).unwrap().is_none());

        // The database-level verb created the collection to hold documents.
        let (_, modality) = database.head().collection_find("movies", &mut Arena::new().unwrap()).unwrap();
        assert_eq!(modality, Modality::Docs);
    }

    #[test]
    fn posters_read_back_as_blobs_and_batch_into_one_tape() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new().unwrap();
        let [avatar, titanic, missing] = keys([1, 2, 3]);
        let posters = database.find_or_create("posters", Modality::Blobs).unwrap();
        posters.blobs().set(avatar, b"\x89PNG avatar").unwrap();
        posters.blobs().set(titanic, b"").unwrap();

        assert_eq!(posters.blobs().get(avatar).unwrap().as_deref(), Some(b"\x89PNG avatar".as_slice()));
        assert!(posters.blobs().contains(titanic).unwrap());
        assert!(!posters.blobs().contains(missing).unwrap());

        // A missing value is empty on the tape, and the presences tell it apart from an empty one.
        let values = posters.blobs().read(&[avatar, missing, titanic], &mut arena).unwrap();
        let tape = values.tape();
        assert_eq!(tape.len(), 3);
        assert_eq!(tape.get(0), Some(b"\x89PNG avatar".as_slice()));
        assert_eq!((tape.get(1), tape.get(2)), (Some(b"".as_slice()), Some(b"".as_slice())));
        let presences = values.presences();
        assert!(presences.get(0) && !presences.get(1) && presences.get(2));

        posters.blobs().remove(avatar).unwrap();
        assert!(posters.blobs().get(avatar).unwrap().is_none());
    }

    #[test]
    fn similar_movies_come_from_the_graph() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let [avatar, titanic, aliens] = keys([1, 2, 3]);
        let similar = database.find_or_create("similar", Modality::Graph).unwrap();
        similar.graph().add_edge(avatar, titanic).unwrap();
        similar.graph().add_edge(avatar, aliens).unwrap();

        let recommendations = similar.graph().read_edges(&[avatar], Role::Source).unwrap();
        assert_eq!(*recommendations, [Edge::new(avatar, titanic), Edge::new(avatar, aliens)]);

        similar.graph().remove_edge(avatar, titanic).unwrap();
        assert_eq!(*similar.graph().degrees(&[avatar, titanic], Role::Any).unwrap(), [1, 0]);
    }

    #[test]
    fn pictures_sample_with_their_values() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new().unwrap();
        let pictures = database.find_or_create("pictures", Modality::Blobs).unwrap();
        for (index, picture) in [b"first", b"third"].into_iter().enumerate() {
            pictures.blobs().set(key(index as u64), picture).unwrap();
        }

        let images = pictures.sample(1000, &mut arena).unwrap();
        assert_eq!(images.keys().len(), 2, "a sample draws only what is there");
        for (drawn, image) in images.keys().iter().zip(images.values().iter()) {
            let expected: &[u8] = if *drawn == key(0) { b"first" } else { b"third" };
            assert_eq!(image, Some(expected));
        }
    }

    #[test]
    fn a_transaction_writes_every_modality_at_its_commit() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let [alice, bob, phone] = keys([1, 2, 3]);
        database.find_or_create("payments", Modality::Docs).unwrap();
        database.find_or_create("connections", Modality::Graph).unwrap();
        database.find_or_create("posters", Modality::Blobs).unwrap();
        let mut transaction = database.transaction().unwrap();
        transaction.collection("payments").unwrap().docs().set(alice, &document(r#"{"amount":10}"#)).unwrap();
        transaction.collection("connections").unwrap().graph().add_edge(alice, bob).unwrap();
        transaction.collection("posters").unwrap().blobs().set(phone, b"\x89PNG").unwrap();

        // The contents land with the commit.
        let head_posters = database.collection("posters").unwrap();
        assert!(head_posters.blobs().get(phone).unwrap().is_none());
        transaction.commit(Durability::Buffered).unwrap();

        assert_eq!(head_posters.blobs().get(phone).unwrap().as_deref(), Some(b"\x89PNG".as_slice()));
        let head_payments = database.collection("payments").unwrap();
        let payment = head_payments.docs().get(alice).unwrap().unwrap();
        assert_eq!(payment["amount"].as_u64(), Some(10));
        let head_connections = database.collection("connections").unwrap();
        assert_eq!(*head_connections.graph().degrees(&[alice], Role::Source).unwrap(), [1]);
    }

    #[test]
    fn a_view_of_another_modality_is_refused() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let avatar = key(1);
        let movies = database.find_or_create("movies", Modality::Docs).unwrap();
        movies.docs().set(avatar, &document("{}")).unwrap();

        let failure = movies.blobs().set(avatar, b"bytes").unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);
        let failure = movies.graph().read_edges(&[avatar], Role::Any).unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);

        // A collection created to hold blobs refuses documents the same way.
        database.collection_create("posters", Modality::Blobs).unwrap();
        let posters = database.collection("posters").unwrap();
        let failure = posters.docs().set(avatar, &document("[]")).unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);
    }

    /// A name only finds, inside a transaction too, and only the database-level verb creates.
    #[test]
    fn a_name_finds_and_creates_nothing() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new().unwrap();
        assert_eq!(database.collection("nowhere").unwrap_err().status, Status::NotFound);
        assert_eq!(database.transaction().unwrap().collection("nowhere").unwrap_err().status, Status::NotFound);
        let lookup = database.head().collection_find("nowhere", &mut arena).unwrap_err();
        assert_eq!(lookup.status, Status::NotFound);

        let created = database.find_or_create("somewhere", Modality::Blobs).unwrap();
        let found = database.find_or_create("somewhere", Modality::Docs).unwrap();
        assert_eq!(found.collection, created.collection, "a second call finds what the first created");
        assert_eq!(found.docs().get(key(1)).unwrap_err().status, Status::InvalidArgument);
    }

    /// A value read through a one-off arena stays where the call wrote it, however often it moves.
    #[test]
    fn owned_values_keep_their_arena_through_moves() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let posters = database.find_or_create("posters", Modality::Blobs).unwrap();
        posters.blobs().set(key(1), b"\x89PNG").unwrap();
        let value = posters.blobs().get(key(1)).unwrap().unwrap();
        let start = value.as_ptr();
        let moved = [value];
        assert_eq!((moved[0].as_ptr(), &*moved[0]), (start, b"\x89PNG".as_slice()));
    }

    #[test]
    #[cfg(feature = "serde")]
    fn documents_deserialize_into_the_type_asked_for() {
        use std::collections::BTreeMap;

        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let movies = database.find_or_create("movies", Modality::Docs).unwrap();
        movies.docs().set(key(1), &document(r#"{"sequels":4}"#)).unwrap();

        let fields = movies.docs().get_as::<BTreeMap<String, u64>>(key(1)).unwrap().unwrap();
        assert_eq!(fields.get("sequels"), Some(&4));
        let failure = movies.docs().get_as::<Vec<String>>(key(1)).unwrap_err();
        assert_eq!(failure.status, Status::Malformed);
    }

    /// The Quickstart of `rust/README.md`, line by line, where the README's `main` is this test,
    /// its imports name `crate` in place of `ustore`, since a crate's own tests reach it by that
    /// name, and its return type spells `std::result::Result`, since this module's `Result` is the
    /// crate's own.
    #[test]
    fn readme_quickstart() -> std::result::Result<(), Box<dyn std::error::Error>> {
        use crate::{Database, Durability, Edge, Key, Modality, RAMOptions, Role, jsontape};

        let database = Database::init_ram(&RAMOptions::default())?;
        let [avatar, titanic] = [1u64, 2].map(Key::from);
        let document = jsontape::parse(br#"{"title":"Avatar","year":2009}"#)?;

        for (name, modality) in [("posters", Modality::Blobs), ("movies", Modality::Docs), ("similar", Modality::Graph)]
        {
            database.find_or_create(name, modality)?;
        }

        // One transaction writes a blob, a document and an edge, and commits all three or none.
        let mut transaction = database.transaction()?;
        transaction.collection("posters")?.blobs().set(avatar, b"\x89PNG")?;
        transaction.collection("movies")?.docs().set(avatar, &document)?;
        transaction.collection("similar")?.graph().add_edge(avatar, titanic)?;
        transaction.commit(Durability::Buffered)?;

        // Reads at the head see the commit: the poster, the year and the one edge leaving Avatar.
        let posters = database.collection("posters")?;
        assert_eq!(posters.blobs().get(avatar)?.as_deref(), Some(b"\x89PNG".as_slice()));
        let movies = database.collection("movies")?;
        let movie = movies.docs().get(avatar)?.expect("committed above");
        assert_eq!(movie["year"].as_u64(), Some(2009));
        let similar = database.collection("similar")?;
        let edges = similar.graph().read_edges(&[avatar], Role::Source)?;
        assert_eq!(*edges, [Edge::new(avatar, titanic)]);
        Ok(())
    }
}

// endregion: Tests
