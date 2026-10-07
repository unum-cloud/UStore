//! Collections addressed by name, as the Python binding indexes a database: `db['movies'].docs[avatar]`.
//!
//! A [`Collection`] resolves its name once, at the head or at its view's snapshot, and reads and writes through its view
//! with an arena of its own. Its first write creates a missing collection holding the modality of the view that wrote it, a read of a
//! missing one fails with [`Status::NotFound`], and a view of another modality fails with
//! [`Status::InvalidArgument`], unless the collection holds [`Modality::Any`].
//!
//! ```no_run
//! use ustore::{jsontape, Database, Durability, Key, RAMOptions, Role};
//!
//! let database = Database::open_ram(&RAMOptions::default())?;
//! let [avatar, titanic] = [1u64, 2].map(Key::from);
//!
//! let avatar_document = jsontape::parse(br#"{"title":"Avatar","year":2009}"#)?;
//! database.collection("movies")?.docs().set(avatar, &avatar_document)?;
//! database.collection("posters")?.blobs().set(avatar, b"\x89PNG")?;
//! database.collection("similar")?.graph().add_edge(avatar, titanic)?;
//!
//! let mut movies = database.collection("movies")?;
//! let description = movies.docs().get(avatar)?.expect("the movie was written above");
//! assert_eq!(description.get("year").as_u64(), Some(2009));
//!
//! let mut similar = database.collection("similar")?;
//! let recommendations = similar.graph().find_edges(&[avatar], Role::Source)?;
//! assert_eq!(recommendations.edges()[0].target, titanic);
//!
//! let mut transaction = database.transaction()?;
//! transaction.collection("posters")?.blobs().set(titanic, b"\x89PNG")?;
//! transaction.commit(Durability::Buffered)?;
//! # Ok::<(), Box<dyn std::error::Error>>(())
//! ```
//!
//! A value read through a collection borrows it, so the next call through that collection does not compile while
//! the value lives:
//!
//! ```compile_fail
//! use ustore::{Database, Key, RAMOptions};
//!
//! let database = Database::open_ram(&RAMOptions::default())?;
//! let mut posters = database.collection("posters")?;
//! let first = posters.blobs().get(Key::from(1u64))?;
//! let second = posters.blobs().get(Key::from(2u64))?;
//! assert_eq!(first, second);
//! # Ok::<(), ustore::Error>(())
//! ```
//!
//! A collection of a transaction borrows the transaction, so no call goes through it after the commit:
//!
//! ```compile_fail
//! use ustore::{Database, Durability, Key, RAMOptions};
//!
//! let database = Database::open_ram(&RAMOptions::default())?;
//! let mut transaction = database.transaction()?;
//! let mut posters = transaction.collection("posters")?;
//! transaction.commit(Durability::Buffered)?;
//! posters.blobs().set(Key::from(1u64), b"\x89PNG")?;
//! # Ok::<(), ustore::Error>(())
//! ```

use crate::blobs::{Entry, Values};
use crate::db::{
    fail, Arena, CollectionID, Conflicts, Database, Durability, Error, Key, Modality, Result, Status, Transaction, View,
};
use crate::docs::{DocumentEntry, Json, Modification};
use crate::graph::{Degree, Edge, Neighborhoods, Role, Roles};
use crate::strings::create_named;

// region: Collections

impl Database {
    /// The collection `name` addresses at the head, resolved now and created by its first write.
    pub fn collection(&self, name: &str) -> Result<Collection<'_>> {
        Collection::new(self.head(), name)
    }
}

impl Transaction<'_> {
    /// The collection `name` addresses inside this transaction, which watches what it touches.
    ///
    /// The name resolves at the head, where a first write creates a missing collection at once, since collections are
    /// created nowhere else. The collection borrows the transaction, so no call goes through it after
    /// [`Transaction::commit`].
    pub fn collection(&self, name: &str) -> Result<Collection<'_>> {
        Collection::new(self.view(Conflicts::Watched), name)
    }
}

/// A collection addressed by its name, which reads and writes through one view with an arena of its own.
///
/// Every verb goes through a view of one modality: [`Collection::blobs`], [`Collection::docs`] or
/// [`Collection::graph`]. A value read through one borrows the collection, so the next call waits until that value
/// is gone, as a call through a bare [`Arena`] does. Writes land [`Durability::Buffered`].
#[derive(Debug)]
pub struct Collection<'view> {
    view: View<'view>,
    name: String,
    resolved: Option<(CollectionID, Modality)>,
    arena: Arena,
}

impl<'view> Collection<'view> {
    fn new(view: View<'view>, name: &str) -> Result<Self> {
        let mut collection = Collection {
            view,
            name: name.to_owned(),
            resolved: None,
            arena: Arena::new(),
        };
        collection.find()?;
        Ok(collection)
    }

    /// The binary values of the collection, as `db['posters'].blobs` addresses them.
    pub fn blobs(&mut self) -> Blobs<'_, 'view> {
        Blobs { collection: self }
    }

    /// The JSON documents of the collection, as `db['movies'].docs` addresses them.
    pub fn docs(&mut self) -> Docs<'_, 'view> {
        Docs { collection: self }
    }

    /// The edges of the collection, as `db['similar'].graph` addresses them.
    pub fn graph(&mut self) -> Graph<'_, 'view> {
        Graph { collection: self }
    }

    /// Draws up to `count` keys uniformly at random, each with its value read through the same view.
    pub fn sample(&mut self, count: u64) -> Result<Sample<'_>> {
        let collection = self.existing(Modality::Any)?;
        let keys = self.view.sample(&mut self.arena, collection, count)?.to_vec();
        let values = self.view.read(&mut self.arena, collection, &keys)?;
        Ok(Sample { keys, values })
    }

    /// The identifier and the modality the name answers to through the view, or `None` while nobody has created it.
    fn find(&mut self) -> Result<Option<(CollectionID, Modality)>> {
        if self.resolved.is_none() {
            self.resolved = match self.view.collection(&self.name) {
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
/// Every verb consumes the view, so a value it reads borrows the collection itself; [`Collection::blobs`] hands out
/// the next one.
#[derive(Debug)]
pub struct Blobs<'collection, 'view> {
    collection: &'collection mut Collection<'view>,
}

impl<'collection> Blobs<'collection, '_> {
    /// The value under `key`, or `None` for a key the collection does not hold.
    pub fn get(self, key: Key) -> Result<Option<&'collection [u8]>> {
        Ok(self.read(&[key])?.get(0))
    }

    /// The values under `keys` in order, which [`Values::tape`] hands out as one tape.
    pub fn read(self, keys: &[Key]) -> Result<Values<'collection>> {
        let collection = self.collection.existing(Modality::Blobs)?;
        let Collection { view, arena, .. } = self.collection;
        view.read(arena, collection, keys)
    }

    /// Whether the collection holds `key`, exporting no value.
    pub fn contains(self, key: Key) -> Result<bool> {
        let collection = self.collection.existing(Modality::Blobs)?;
        let Collection { view, arena, .. } = self.collection;
        Ok(view.contains(arena, collection, &[key])?.get(0))
    }

    /// Stores `value` under `key`, creating the collection to hold blobs when it is missing.
    pub fn set(self, key: Key, value: &[u8]) -> Result<()> {
        let collection = self.collection.writable(Modality::Blobs)?;
        let entries = [Entry::new(key, value)?];
        self.collection.view.write(collection, &entries, Durability::Buffered)
    }

    /// Removes `key`, whether or not the collection holds it.
    pub fn remove(self, key: Key) -> Result<()> {
        let collection = self.collection.existing(Modality::Blobs)?;
        self.collection.view.remove(collection, &[key], Durability::Buffered)
    }
}

/// The JSON documents of one collection, parsed through JsonTape.
///
/// Every verb consumes the view, so a document it reads borrows the collection itself; [`Collection::docs`] hands out
/// the next one.
#[derive(Debug)]
pub struct Docs<'collection, 'view> {
    collection: &'collection mut Collection<'view>,
}

impl<'collection> Docs<'collection, '_> {
    /// The document under `key`, parsed in place over the text the engine exported, or `None` for a key without one.
    pub fn get(self, key: Key) -> Result<Option<jsontape::BoundJsonView<'collection>>> {
        let Some(text) = self.text(key)? else {
            return Ok(None);
        };
        jsontape::view_bound(text).map(Some).map_err(not_json)
    }

    /// The document under `key` as the JSON text the engine holds, or `None` for a key without one.
    pub fn get_json(self, key: Key) -> Result<Option<&'collection str>> {
        let Some(text) = self.text(key)? else {
            return Ok(None);
        };
        match std::str::from_utf8(text) {
            Ok(text) => Ok(Some(text)),
            Err(_) => Err(fail(Status::Malformed, "the document is not valid JSON")),
        }
    }

    /// The document under `key` deserialized into `Value`, or `None` for a key without one.
    #[cfg(feature = "serde")]
    pub fn get_as<Value: serde::de::DeserializeOwned>(self, key: Key) -> Result<Option<Value>> {
        let Some(text) = self.text(key)? else {
            return Ok(None);
        };
        let document = jsontape::parse(text).map_err(not_json)?;
        match jsontape::from_value(&document) {
            Ok(value) => Ok(Some(value)),
            Err(_) => Err(fail(Status::Malformed, "the document does not fit that type")),
        }
    }

    /// Stores `document` under `key`, creating the collection to hold documents when it is missing.
    pub fn set(self, key: Key, document: &jsontape::Json) -> Result<()> {
        let collection = self.collection.writable(Modality::Docs)?;
        let text = document.to_string();
        let entries = [DocumentEntry::<Json>::new(key, &text)?];
        self.collection
            .view
            .write_documents(collection, &entries, Modification::Upsert, Durability::Buffered)
    }

    /// Removes the document under `key`, whether or not the collection holds one.
    pub fn remove(self, key: Key) -> Result<()> {
        let collection = self.collection.existing(Modality::Docs)?;
        self.collection.view.remove(collection, &[key], Durability::Buffered)
    }

    /// The text of the document under `key`, or `None` for a key without one.
    fn text(self, key: Key) -> Result<Option<&'collection [u8]>> {
        let collection = self.collection.existing(Modality::Docs)?;
        let Collection { view, arena, .. } = self.collection;
        Ok(view.read_documents(arena, collection, &[key])?.get(0))
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
/// Every verb consumes the view, so the edges it finds borrow the collection itself; [`Collection::graph`] hands out
/// the next one.
#[derive(Debug)]
pub struct Graph<'collection, 'view> {
    collection: &'collection mut Collection<'view>,
}

impl<'collection> Graph<'collection, '_> {
    /// Adds an edge from `source` to `target`, creating the collection as a graph when it is missing.
    pub fn add_edge(self, source: Key, target: Key) -> Result<()> {
        let collection = self.collection.writable(Modality::Graph)?;
        self.collection
            .view
            .upsert_edges(collection, &[Edge::new(source, target)], Durability::Buffered)
    }

    /// Removes the edge from `source` to `target`, whether or not the graph holds it.
    pub fn remove_edge(self, source: Key, target: Key) -> Result<()> {
        let collection = self.collection.existing(Modality::Graph)?;
        self.collection
            .view
            .remove_edges(collection, &[Edge::new(source, target)], Durability::Buffered)
    }

    /// The degree and the edges of every vertex in `role`, as `db['similar'].graph.find_edges(avatar)` lists them.
    pub fn find_edges(self, vertices: &[Key], role: Role) -> Result<Neighborhoods<'collection>> {
        let collection = self.collection.existing(Modality::Graph)?;
        let Collection { view, arena, .. } = self.collection;
        view.find_edges(arena, collection, vertices, Roles::Every(role))
    }

    /// The degree of every vertex in `role`, exporting no edges.
    pub fn degrees(self, vertices: &[Key], role: Role) -> Result<&'collection [Degree]> {
        let collection = self.collection.existing(Modality::Graph)?;
        let Collection { view, arena, .. } = self.collection;
        view.degrees(arena, collection, vertices, Roles::Every(role))
    }
}

// endregion: Views

// region: Samples

/// Keys drawn at random from a collection, with the values read under them through the same view.
#[derive(Clone, Debug)]
pub struct Sample<'collection> {
    keys: Vec<Key>,
    values: Values<'collection>,
}

impl<'collection> Sample<'collection> {
    /// The keys drawn, in the order of their values.
    pub fn keys(&self) -> &[Key] {
        &self.keys
    }

    /// The value under every key, where a key removed between the draw and the read is missing.
    pub fn values(&self) -> Values<'collection> {
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
        let database = Database::open_ram(&RAMOptions::default()).unwrap();
        let avatar = key(1);
        let written = document(r#"{"title":"Avatar","year":2009}"#);
        let mut movies = database.collection("movies").unwrap();
        movies.docs().set(avatar, &written).unwrap();

        let description = movies.docs().get(avatar).unwrap().unwrap();
        assert_eq!(description.get("title").as_str(), Some("Avatar"));
        assert_eq!(description.get("year").as_u64(), Some(2009));
        assert_eq!(
            movies.docs().get_json(avatar).unwrap(),
            Some(r#"{"title":"Avatar","year":2009}"#)
        );
        assert!(movies.docs().get(key(2)).unwrap().is_none());

        movies.docs().remove(avatar).unwrap();
        assert!(movies.docs().get(avatar).unwrap().is_none());

        // The first write created the collection to hold documents.
        let (_, modality) = database.head().collection("movies").unwrap();
        assert_eq!(modality, Modality::Docs);
    }

    #[test]
    fn posters_read_back_as_blobs_and_batch_into_one_tape() {
        let database = Database::open_ram(&RAMOptions::default()).unwrap();
        let [avatar, titanic, missing] = keys([1, 2, 3]);
        let mut posters = database.collection("posters").unwrap();
        posters.blobs().set(avatar, b"\x89PNG avatar").unwrap();
        posters.blobs().set(titanic, b"").unwrap();

        assert_eq!(posters.blobs().get(avatar).unwrap(), Some(b"\x89PNG avatar".as_slice()));
        assert!(posters.blobs().contains(titanic).unwrap());
        assert!(!posters.blobs().contains(missing).unwrap());

        // A missing value is empty on the tape, and the presences tell it apart from an empty one.
        let values = posters.blobs().read(&[avatar, missing, titanic]).unwrap();
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
        let database = Database::open_ram(&RAMOptions::default()).unwrap();
        let [avatar, titanic, aliens] = keys([1, 2, 3]);
        let mut similar = database.collection("similar").unwrap();
        similar.graph().add_edge(avatar, titanic).unwrap();
        similar.graph().add_edge(avatar, aliens).unwrap();

        let recommendations = similar.graph().find_edges(&[avatar], Role::Source).unwrap();
        assert_eq!(
            recommendations.edges(),
            &[Edge::new(avatar, titanic), Edge::new(avatar, aliens)]
        );

        similar.graph().remove_edge(avatar, titanic).unwrap();
        assert_eq!(similar.graph().degrees(&[avatar, titanic], Role::Any).unwrap(), &[1, 0]);
    }

    #[test]
    fn pictures_sample_with_their_values() {
        let database = Database::open_ram(&RAMOptions::default()).unwrap();
        let mut pictures = database.collection("pictures").unwrap();
        for (index, picture) in [b"first", b"third"].into_iter().enumerate() {
            pictures.blobs().set(key(index as u64), picture).unwrap();
        }

        let images = pictures.sample(1000).unwrap();
        assert_eq!(images.keys().len(), 2, "a sample draws only what is there");
        for (drawn, image) in images.keys().iter().zip(images.values().iter()) {
            let expected: &[u8] = if *drawn == key(0) { b"first" } else { b"third" };
            assert_eq!(image, Some(expected));
        }
    }

    #[test]
    fn a_transaction_writes_every_modality_at_its_commit() {
        let database = Database::open_ram(&RAMOptions::default()).unwrap();
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

        assert_eq!(head_posters.blobs().get(phone).unwrap(), Some(b"\x89PNG".as_slice()));
        let mut head_payments = database.collection("payments").unwrap();
        let payment = head_payments.docs().get(alice).unwrap().unwrap();
        assert_eq!(payment.get("amount").as_u64(), Some(10));
        let mut head_connections = database.collection("connections").unwrap();
        assert_eq!(head_connections.graph().degrees(&[alice], Role::Source).unwrap(), &[1]);
    }

    #[test]
    fn a_view_of_another_modality_is_refused() {
        let database = Database::open_ram(&RAMOptions::default()).unwrap();
        let avatar = key(1);
        let mut movies = database.collection("movies").unwrap();
        movies.docs().set(avatar, &document("{}")).unwrap();

        let failure = movies.blobs().set(avatar, b"bytes").unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);
        let failure = movies.graph().find_edges(&[avatar], Role::Any).unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);

        // A collection holding anything takes documents and blobs alike.
        database.create_collection("anything", Modality::Any).unwrap();
        let mut anything = database.collection("anything").unwrap();
        anything.docs().set(avatar, &document("[]")).unwrap();
        assert_eq!(anything.blobs().get(avatar).unwrap(), Some(b"[]".as_slice()));
    }

    #[test]
    fn a_read_of_a_missing_collection_creates_nothing() {
        let database = Database::open_ram(&RAMOptions::default()).unwrap();
        let mut nowhere = database.collection("nowhere").unwrap();
        assert_eq!(nowhere.docs().get_json(key(1)).unwrap_err().status, Status::NotFound);
        assert_eq!(nowhere.blobs().contains(key(1)).unwrap_err().status, Status::NotFound);
        assert_eq!(nowhere.sample(10).unwrap_err().status, Status::NotFound);

        let lookup = database.head().collection("nowhere").unwrap_err();
        assert_eq!(lookup.status, Status::NotFound);
    }

    #[test]
    #[cfg(feature = "serde")]
    fn documents_deserialize_into_the_type_asked_for() {
        use std::collections::BTreeMap;

        let database = Database::open_ram(&RAMOptions::default()).unwrap();
        let mut movies = database.collection("movies").unwrap();
        movies.docs().set(key(1), &document(r#"{"sequels":4}"#)).unwrap();

        let fields = movies.docs().get_as::<BTreeMap<String, u64>>(key(1)).unwrap().unwrap();
        assert_eq!(fields.get("sequels"), Some(&4));
        let failure = movies.docs().get_as::<Vec<String>>(key(1)).unwrap_err();
        assert_eq!(failure.status, Status::Malformed);
    }
}

// endregion: Tests
