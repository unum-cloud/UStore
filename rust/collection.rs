//! Collections addressed by name, as the Python binding indexes a database: `db['movies'].docs[avatar]`.
//!
//! A [`Collection`] resolves its name once, at the head or at its view's snapshot, and reads and writes through its
//! view. Its first write creates a missing collection holding the modality of the view that wrote it, a read of a
//! missing one fails with [`Status::NotFound`], and a view of another modality fails with
//! [`Status::InvalidArgument`], unless the collection holds [`Modality::Any`].
//!
//! A collection keeps no arena. A call with no arena to pass makes a one-off arena, copies what it read out of it,
//! and frees it: slower than reusing an arena, and stateless. Batch reads take the [`Arena`] they land in instead.
//!
//! ```no_run
//! use ustore::{jsontape, Arena, Database, Durability, Key, RAMOptions, Role};
//!
//! let database = Database::init_ram(&RAMOptions::default())?;
//! let [avatar, titanic] = [1u64, 2].map(Key::from);
//!
//! let avatar_document = jsontape::parse(br#"{"title":"Avatar","year":2009}"#)?;
//! database.collection("movies")?.docs().set(avatar, &avatar_document)?;
//! database.collection("posters")?.blobs().set(avatar, b"\x89PNG")?;
//! database.collection("similar")?.graph().add_edge(avatar, titanic)?;
//!
//! let mut movies = database.collection("movies")?;
//! let description = movies.docs().get(avatar)?.expect("the movie was written above");
//! assert_eq!(description["year"].as_u64(), Some(2009));
//!
//! let mut similar = database.collection("similar")?;
//! let recommendations = similar.graph().read_edges(&[avatar], Role::Source)?;
//! assert_eq!(recommendations[0].target, titanic);
//!
//! let mut arena = Arena::new()?;
//! let posters = database.collection("posters")?.blobs().read(&[avatar, titanic], &mut arena)?;
//! assert_eq!(posters.get(0), Some(b"\x89PNG".as_slice()));
//!
//! let mut transaction = database.transaction()?;
//! transaction.collection("posters")?.blobs().set(titanic, b"\x89PNG")?;
//! transaction.commit(Durability::Buffered)?;
//! # Ok::<(), Box<dyn std::error::Error>>(())
//! ```
//!
//! A batch read through a collection borrows the arena it landed in, so the next read into that arena does not compile
//! while the values live:
//!
//! ```compile_fail,E0499
//! use ustore::{Arena, Database, Key, RAMOptions};
//!
//! let database = Database::init_ram(&RAMOptions::default())?;
//! let mut posters = database.collection("posters")?;
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
//! use ustore::{Database, Durability, Key, RAMOptions};
//!
//! let database = Database::init_ram(&RAMOptions::default())?;
//! let mut transaction = database.transaction()?;
//! let mut posters = transaction.collection("posters")?;
//! transaction.commit(Durability::Buffered)?;
//! posters.blobs().set(Key::from(1u64), b"\x89PNG")?;
//! # Ok::<(), ustore::Error>(())
//! ```

use crate::blobs::{Entry, Values};
use crate::db::sealed::Writable;
use crate::db::{
    fail, Arena, CollectionID, Database, Durability, Error, Key, Modality, Result, Status, Transaction, View, Watch,
};
use crate::docs::{DocumentEntry, Json, Modification};
use crate::graph::{Degree, Edge, Role, Roles};
use crate::strings::create_named;

// region: Collections

impl Database {
    /// The collection `name` addresses at the head, resolved now and created by its first write.
    pub fn collection(&self, name: &str) -> Result<Collection<'_>> {
        Collection::new(self.head(), name)
    }
}

impl<'db> Transaction<'db> {
    /// The collection `name` addresses inside this transaction, which watches what it touches.
    ///
    /// The name resolves at the head, where a first write creates a missing collection at once, since collections are
    /// created nowhere else. The collection borrows the transaction, so no call goes through it after
    /// [`Transaction::commit`].
    pub fn collection(&self, name: &str) -> Result<Collection<'_, Transaction<'db>>> {
        Collection::new(self.view(Watch::Keys), name)
    }
}

/// A collection addressed by its name, which reads and writes through one view of its `Parent`, the head of a
/// [`Database`] or a [`Transaction`].
///
/// Every verb goes through a view of one modality: [`Collection::blobs`], [`Collection::docs`] or
/// [`Collection::graph`]. A call with no arena to pass reads through a one-off arena and hands back what it read
/// copied out of it, while a batch read borrows the [`Arena`] it is given. Writes land [`Durability::Buffered`].
#[derive(Debug)]
pub struct Collection<'view, Parent = Database> {
    view: View<'view, Parent>,
    name: String,
    resolved: Option<(CollectionID, Modality)>,
}

impl<'view, Parent: Writable> Collection<'view, Parent> {
    fn new(view: View<'view, Parent>, name: &str) -> Result<Self> {
        let mut collection = Collection {
            view,
            name: name.to_owned(),
            resolved: None,
        };
        collection.find()?;
        Ok(collection)
    }

    /// The binary values of the collection, as `db['posters'].blobs` addresses them.
    pub fn blobs(&mut self) -> Blobs<'_, 'view, Parent> {
        Blobs { collection: self }
    }

    /// The JSON documents of the collection, as `db['movies'].docs` addresses them.
    pub fn docs(&mut self) -> Docs<'_, 'view, Parent> {
        Docs { collection: self }
    }

    /// The edges of the collection, as `db['similar'].graph` addresses them.
    pub fn graph(&mut self) -> Graph<'_, 'view, Parent> {
        Graph { collection: self }
    }

    /// Draws up to `wanted` keys uniformly at random into `arena`, each with its value read through the same view.
    pub fn sample<'arena>(&mut self, wanted: usize, arena: &'arena mut Arena) -> Result<Sample<'arena>> {
        let collection = self.existing(Modality::Any)?;
        let keys = self.view.sample(collection, wanted, arena)?.to_vec();
        let values = self.view.read(collection, &keys, arena)?;
        Ok(Sample { keys, values })
    }

    /// The identifier and the modality the name answers to through the view, or `None` while nobody has created it,
    /// resolved through a one-off arena.
    fn find(&mut self) -> Result<Option<(CollectionID, Modality)>> {
        if self.resolved.is_none() {
            self.resolved = match self.view.collection_find(&self.name, &mut Arena::new()?) {
                Ok(found) => Some(found),
                Err(failure) if failure.status == Status::NotFound => None,
                Err(failure) => return Err(failure),
            };
        }
        Ok(self.resolved)
    }

    /// The collection a view of `modality` reads, refusing a missing one.
    fn existing(&mut self, modality: Modality) -> Result<CollectionID> {
        match self.find()? {
            Some(found) => accepted(found, modality),
            None => Err(fail(Status::NotFound, "no collection answers to that name")),
        }
    }

    /// The collection a view of `modality` writes, creating a missing one to hold `modality`.
    fn writable(&mut self, modality: Modality) -> Result<CollectionID> {
        if let Some(found) = self.find()? {
            return accepted(found, modality);
        }
        match create_named(self.view.database(), &self.name, modality) {
            Ok(created) => {
                self.resolved = Some((created, modality));
                Ok(created)
            }
            // Another writer created it in between, perhaps holding another modality.
            Err(failure) if failure.status == Status::AlreadyExists => self.existing(modality),
            Err(failure) => Err(failure),
        }
    }
}

/// The collection `found` names, when a view of `modality` may reach what it holds.
fn accepted((collection, held): (CollectionID, Modality), modality: Modality) -> Result<CollectionID> {
    if held == modality || held == Modality::Any || modality == Modality::Any {
        return Ok(collection);
    }
    let message = match modality {
        Modality::Blobs => "the collection holds no blobs",
        Modality::Docs => "the collection holds no documents",
        _ => "the collection holds no graph",
    };
    Err(fail(Status::InvalidArgument, message))
}

// endregion: Collections

// region: Views

/// The binary values of one collection.
///
/// Every verb consumes the view; [`Collection::blobs`] hands out the next one.
#[derive(Debug)]
pub struct Blobs<'collection, 'view, Parent = Database> {
    collection: &'collection mut Collection<'view, Parent>,
}

impl<Parent: Writable> Blobs<'_, '_, Parent> {
    /// The value under `key`, copied out of a one-off arena, or `None` for a key the collection does not hold.
    pub fn get(self, key: Key) -> Result<Option<Vec<u8>>> {
        Ok(self.read(&[key], &mut Arena::new()?)?.get(0).map(<[u8]>::to_vec))
    }

    /// The values under `keys` in order, read into `arena`, which [`Values::tape`] hands out as one tape.
    pub fn read<'arena>(self, keys: &[Key], arena: &'arena mut Arena) -> Result<Values<'arena>> {
        let collection = self.collection.existing(Modality::Blobs)?;
        self.collection.view.read(collection, keys, arena)
    }

    /// Whether the collection holds `key`, exporting no value into the one-off arena it reads through.
    pub fn contains(self, key: Key) -> Result<bool> {
        let collection = self.collection.existing(Modality::Blobs)?;
        Ok(self
            .collection
            .view
            .contains(collection, &[key], &mut Arena::new()?)?
            .get(0))
    }

    /// Stores `value` under `key`, creating the collection to hold blobs when it is missing.
    pub fn set(self, key: Key, value: &[u8]) -> Result<()> {
        let collection = self.collection.writable(Modality::Blobs)?;
        let entries = [Entry::new(key, value)];
        self.collection.view.write(collection, &entries, Durability::Buffered)?;
        Ok(())
    }

    /// Removes `key`, whether or not the collection holds it.
    pub fn remove(self, key: Key) -> Result<()> {
        let collection = self.collection.existing(Modality::Blobs)?;
        self.collection.view.remove(collection, [key], Durability::Buffered)?;
        Ok(())
    }
}

/// The JSON documents of one collection, parsed through JsonTape.
///
/// Every verb consumes the view; [`Collection::docs`] hands out the next one.
#[derive(Debug)]
pub struct Docs<'collection, 'view, Parent = Database> {
    collection: &'collection mut Collection<'view, Parent>,
}

impl<Parent: Writable> Docs<'_, '_, Parent> {
    /// The document under `key`, parsed out of the one-off arena it was read into, or `None` for a key without one.
    pub fn get(self, key: Key) -> Result<Option<jsontape::Json>> {
        self.parsed(key, |text| jsontape::parse(text).map_err(not_json))
    }

    /// The document under `key` as the JSON text the engine holds, or `None` for a key without one.
    pub fn get_json(self, key: Key) -> Result<Option<String>> {
        self.parsed(key, |text| match std::str::from_utf8(text) {
            Ok(text) => Ok(text.to_owned()),
            Err(_) => Err(fail(Status::Malformed, "the document is not valid JSON")),
        })
    }

    /// The document under `key` deserialized into `Value`, or `None` for a key without one.
    #[cfg(feature = "serde")]
    pub fn get_as<Value: serde::de::DeserializeOwned>(self, key: Key) -> Result<Option<Value>> {
        self.parsed(key, |text| {
            let document = jsontape::parse(text).map_err(not_json)?;
            jsontape::from_value(&document).map_err(|_| fail(Status::Malformed, "the document does not fit that type"))
        })
    }

    /// Stores `document` under `key`, creating the collection to hold documents when it is missing.
    pub fn set(self, key: Key, document: &jsontape::Json) -> Result<()> {
        let collection = self.collection.writable(Modality::Docs)?;
        let text = document.to_string();
        let entries = [DocumentEntry::<Json>::new(key, &text)];
        self.collection
            .view
            .docs_write(collection, &entries, Modification::Upsert, Durability::Buffered)?;
        Ok(())
    }

    /// Removes the document under `key`, whether or not the collection holds one.
    pub fn remove(self, key: Key) -> Result<()> {
        let collection = self.collection.existing(Modality::Docs)?;
        self.collection.view.remove(collection, [key], Durability::Buffered)?;
        Ok(())
    }

    /// What `parse` makes of the text of the document under `key` while the one-off arena it was read into lives, or
    /// `None` for a key without one.
    fn parsed<Output>(self, key: Key, parse: impl FnOnce(&[u8]) -> Result<Output>) -> Result<Option<Output>> {
        let collection = self.collection.existing(Modality::Docs)?;
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
    collection: &'collection mut Collection<'view, Parent>,
}

impl<Parent: Writable> Graph<'_, '_, Parent> {
    /// Adds an edge from `source` to `target`, creating the collection as a graph when it is missing.
    pub fn add_edge(self, source: Key, target: Key) -> Result<()> {
        let collection = self.collection.writable(Modality::Graph)?;
        let edges = [Edge::new(source, target)];
        self.collection
            .view
            .graph_write_edges(collection, edges, Durability::Buffered)?;
        Ok(())
    }

    /// Removes the edge from `source` to `target`, whether or not the graph holds it.
    pub fn remove_edge(self, source: Key, target: Key) -> Result<()> {
        let collection = self.collection.existing(Modality::Graph)?;
        let edges = [Edge::new(source, target)];
        self.collection
            .view
            .graph_remove_edges(collection, edges, Durability::Buffered)?;
        Ok(())
    }

    /// The edges of every vertex in `role`, one run after another, copied out of a one-off arena, as
    /// `db['similar'].graph.read_edges(avatar)` lists them.
    pub fn read_edges(self, vertices: &[Key], role: Role) -> Result<Vec<Edge>> {
        let collection = self.collection.existing(Modality::Graph)?;
        let mut arena = Arena::new()?;
        let found = self
            .collection
            .view
            .graph_read_edges(collection, vertices, Roles::Every(role), &mut arena)?;
        Ok(found.edges().to_vec())
    }

    /// The degree of every vertex in `role`, copied out of a one-off arena that exports no edges.
    pub fn degrees(self, vertices: &[Key], role: Role) -> Result<Vec<Degree>> {
        let collection = self.collection.existing(Modality::Graph)?;
        let mut arena = Arena::new()?;
        let degrees = self
            .collection
            .view
            .graph_degrees(collection, vertices, Roles::Every(role), &mut arena)?;
        Ok(degrees.to_vec())
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
        let mut movies = database.collection("movies").unwrap();
        movies.docs().set(avatar, &written).unwrap();

        let description = movies.docs().get(avatar).unwrap().unwrap();
        assert_eq!(description["title"].as_str(), Some("Avatar"));
        assert_eq!(description["year"].as_u64(), Some(2009));
        assert_eq!(
            movies.docs().get_json(avatar).unwrap().as_deref(),
            Some(r#"{"title":"Avatar","year":2009}"#)
        );
        assert!(movies.docs().get(key(2)).unwrap().is_none());

        movies.docs().remove(avatar).unwrap();
        assert!(movies.docs().get(avatar).unwrap().is_none());

        // The first write created the collection to hold documents.
        let (_, modality) = database
            .head()
            .collection_find("movies", &mut Arena::new().unwrap())
            .unwrap();
        assert_eq!(modality, Modality::Docs);
    }

    #[test]
    fn posters_read_back_as_blobs_and_batch_into_one_tape() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new().unwrap();
        let [avatar, titanic, missing] = keys([1, 2, 3]);
        let mut posters = database.collection("posters").unwrap();
        posters.blobs().set(avatar, b"\x89PNG avatar").unwrap();
        posters.blobs().set(titanic, b"").unwrap();

        assert_eq!(posters.blobs().get(avatar).unwrap(), Some(b"\x89PNG avatar".to_vec()));
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
        assert_eq!(posters.blobs().get(avatar).unwrap(), None);
    }

    #[test]
    fn similar_movies_come_from_the_graph() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let [avatar, titanic, aliens] = keys([1, 2, 3]);
        let mut similar = database.collection("similar").unwrap();
        similar.graph().add_edge(avatar, titanic).unwrap();
        similar.graph().add_edge(avatar, aliens).unwrap();

        let recommendations = similar.graph().read_edges(&[avatar], Role::Source).unwrap();
        assert_eq!(recommendations, [Edge::new(avatar, titanic), Edge::new(avatar, aliens)]);

        similar.graph().remove_edge(avatar, titanic).unwrap();
        assert_eq!(similar.graph().degrees(&[avatar, titanic], Role::Any).unwrap(), [1, 0]);
    }

    #[test]
    fn pictures_sample_with_their_values() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new().unwrap();
        let mut pictures = database.collection("pictures").unwrap();
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
        let mut transaction = database.transaction().unwrap();
        transaction
            .collection("payments")
            .unwrap()
            .docs()
            .set(alice, &document(r#"{"amount":10}"#))
            .unwrap();
        transaction
            .collection("connections")
            .unwrap()
            .graph()
            .add_edge(alice, bob)
            .unwrap();
        transaction
            .collection("posters")
            .unwrap()
            .blobs()
            .set(phone, b"\x89PNG")
            .unwrap();

        // The collections exist at the head at once, while their contents land with the commit.
        let mut head_posters = database.collection("posters").unwrap();
        assert_eq!(head_posters.blobs().get(phone).unwrap(), None);
        transaction.commit(Durability::Buffered).unwrap();

        assert_eq!(head_posters.blobs().get(phone).unwrap(), Some(b"\x89PNG".to_vec()));
        let mut head_payments = database.collection("payments").unwrap();
        let payment = head_payments.docs().get(alice).unwrap().unwrap();
        assert_eq!(payment["amount"].as_u64(), Some(10));
        let mut head_connections = database.collection("connections").unwrap();
        assert_eq!(head_connections.graph().degrees(&[alice], Role::Source).unwrap(), [1]);
    }

    #[test]
    fn a_view_of_another_modality_is_refused() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let avatar = key(1);
        let mut movies = database.collection("movies").unwrap();
        movies.docs().set(avatar, &document("{}")).unwrap();

        let failure = movies.blobs().set(avatar, b"bytes").unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);
        let failure = movies.graph().read_edges(&[avatar], Role::Any).unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);

        // A collection holding anything takes documents and blobs alike.
        database.collection_create("anything", Modality::Any).unwrap();
        let mut anything = database.collection("anything").unwrap();
        anything.docs().set(avatar, &document("[]")).unwrap();
        assert_eq!(anything.blobs().get(avatar).unwrap(), Some(b"[]".to_vec()));
    }

    #[test]
    fn a_read_of_a_missing_collection_creates_nothing() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new().unwrap();
        let mut nowhere = database.collection("nowhere").unwrap();
        assert_eq!(nowhere.docs().get_json(key(1)).unwrap_err().status, Status::NotFound);
        assert_eq!(nowhere.blobs().contains(key(1)).unwrap_err().status, Status::NotFound);
        assert_eq!(nowhere.sample(10, &mut arena).unwrap_err().status, Status::NotFound);

        let lookup = database.head().collection_find("nowhere", &mut arena).unwrap_err();
        assert_eq!(lookup.status, Status::NotFound);
    }

    #[test]
    #[cfg(feature = "serde")]
    fn documents_deserialize_into_the_type_asked_for() {
        use std::collections::BTreeMap;

        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let mut movies = database.collection("movies").unwrap();
        movies.docs().set(key(1), &document(r#"{"sequels":4}"#)).unwrap();

        let fields = movies.docs().get_as::<BTreeMap<String, u64>>(key(1)).unwrap().unwrap();
        assert_eq!(fields.get("sequels"), Some(&4));
        let failure = movies.docs().get_as::<Vec<String>>(key(1)).unwrap_err();
        assert_eq!(failure.status, Status::Malformed);
    }
}

// endregion: Tests
