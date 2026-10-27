//! The names of collections, which the collection calls of `c/ustore/db.h` record and resolve.
//!
//! Names resolve in two phases: a name maps onto a collection once, and every other call addresses
//! the collection by its identifier. Each engine keeps the names apart from every collection, so no
//! identifier reaches them.
//!
//! File: rust/strings.rs
//! Author: Ash Vardanian
//! Date: September 16, 2026

use std::iter::Copied;
use std::ptr;
use std::slice;

use crate::db::ffi::{
    self, ustore_completion_t, ustore_database_t, ustore_enum_t, ustore_octet_t, ustore_outcome_t, ustore_size_t,
    ustore_strings_view_t,
};
use crate::db::sealed::{Accepts, Calling};
use crate::db::{check, fail, Address, Arena, CollectionID, Modality, Result, Status, Strings, View};

// region: Outputs

/// The names of collections one listing found, in the order of their text.
///
/// The run borrows the arena it was exported into, and every name in it was checked for UTF-8 once,
/// when the listing returned.
#[derive(Clone, Debug, Default)]
pub struct Names<'arena> {
    names: Vec<&'arena str>,
}

impl<'arena> Names<'arena> {
    /// How many names the listing kept, never above the count it wanted.
    pub fn len(&self) -> usize {
        self.names.len()
    }

    /// Whether the listing kept nothing.
    pub fn is_empty(&self) -> bool {
        self.names.is_empty()
    }

    /// The name at `index`, or `None` past the end of the listing.
    pub fn get(&self, index: usize) -> Option<&'arena str> {
        self.names.get(index).copied()
    }

    /// Every name the listing kept, in the order of its text.
    pub fn iter(&self) -> Copied<slice::Iter<'_, &'arena str>> {
        self.names.iter().copied()
    }
}

impl<'names, 'arena> IntoIterator for &'names Names<'arena> {
    type Item = &'arena str;
    type IntoIter = Copied<slice::Iter<'names, &'arena str>>;

    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}

// endregion: Outputs

// region: Names

/// Creates a collection under `name`, which no other collection may carry, answering its identifier.
pub(crate) fn create_named(database: ustore_database_t, name: &str, modality: Modality) -> Result<CollectionID> {
    let mut collection = CollectionID::default();
    let mut outcome = ustore_outcome_t::default();
    let mut arguments = ffi::ustore_collection_create_t {
        database,
        executor: ptr::null_mut(),
        outcome: &mut outcome,
        completion: ustore_completion_t::Blocking,
        name: name.as_bytes().into(),
        modality: modality as ustore_enum_t,
        collection: &mut collection,
    };
    // SAFETY: the name and the output outlive the call, which is final on return.
    unsafe { ffi::ustore_collection_create(&mut arguments) };
    check(outcome)?;
    Ok(collection)
}

impl<Parent, Completion: crate::db::sealed::Completion> View<'_, Parent, Completion> {
    /// The collection `name` addresses and the modality it holds, which [`crate::Database::collection_create`]
    /// bound, resolved through the arena.
    ///
    /// Collections are created and dropped only at the head, so names are read at the view's snapshot, or at the
    /// head for a transaction or the head itself, and never join a transaction's conflicts.
    ///
    /// Calls `ustore_collection_find`.
    pub fn collection_find<'arena, Name: AsRef<str>>(
        &self,
        name: Name,
        arena: &'arena mut Arena,
    ) -> Completion::Answer<CollectionFind<'arena, Name>>
    where
        Completion: Accepts<Name>,
    {
        let verb = CollectionFind {
            name,
            arena,
            offsets: [0; 2],
            presences: Address::default(),
            collections: Address::default(),
            modalities: Address::default(),
        };
        Completion::answer(self, verb)
    }

    /// Lists up to `wanted` collection names under `prefix` into the arena, in the order of their text, from `start`
    /// on where one is given.
    ///
    /// An engine lists its collections in an order of its own, so the names are sorted here. A listing
    /// describes the database rather than a transaction, so it reads the view's snapshot or the head.
    ///
    /// Calls `ustore_collection_list`.
    pub fn collection_names<'arena, Name: AsRef<str>>(
        &self,
        prefix: Name,
        start: Option<Name>,
        wanted: usize,
        arena: &'arena mut Arena,
    ) -> Completion::Answer<CollectionNames<'arena, Name>>
    where
        Completion: Accepts<Name>,
    {
        let verb = CollectionNames {
            prefix,
            start,
            arena,
            wanted,
            written: 0,
            names: ustore_strings_view_t::default(),
        };
        Completion::answer(self, verb)
    }
}

/// A lookup of a collection by its name; see [`View::collection_find`].
#[derive(Debug)]
pub struct CollectionFind<'arena, Name> {
    name: Name,
    arena: &'arena mut Arena,
    /// The two offsets of the one-name tape the arguments point at.
    offsets: [u64; 2],
    presences: Address<ustore_octet_t>,
    collections: Address<CollectionID>,
    modalities: Address<ustore_enum_t>,
}

impl<Name: AsRef<str>> Calling for CollectionFind<'_, Name> {
    type Arguments = ffi::ustore_collection_find_t;
    type Output = (CollectionID, Modality);

    fn arguments<Parent, Completion>(
        &mut self,
        view: &View<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let name = self.name.as_ref();
        self.offsets = [0, name.len() as u64];
        Ok(Some(ffi::ustore_collection_find_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            snapshot: view.snapshot(),
            count: 1,
            names: ustore_strings_view_t::u64_tape(name.as_ptr(), self.offsets.as_ptr()),
            presences: &mut self.presences.0,
            collections: &mut self.collections.0,
            modalities: &mut self.modalities.0,
            arena: self.arena.handle,
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_collection_find(arguments)
    }

    fn finish(self) -> Result<(CollectionID, Modality)> {
        // SAFETY: a successful lookup of one name exported its presence bit, identifier and modality into the arena,
        // which this verb still borrows.
        let (presence, collection, modality) = unsafe { (*self.presences.0, *self.collections.0, *self.modalities.0) };
        if presence & 1 == 0 {
            return Err(fail(Status::NotFound, "no collection answers to that name"));
        }
        match Modality::from_raw(modality) {
            Some(modality) => Ok((collection, modality)),
            None => Err(fail(Status::Malformed, "the engine reported an unknown modality")),
        }
    }
}

/// A listing of collection names; see [`View::collection_names`].
#[derive(Debug)]
pub struct CollectionNames<'arena, Name> {
    prefix: Name,
    start: Option<Name>,
    arena: &'arena mut Arena,
    wanted: usize,
    written: ustore_size_t,
    names: ustore_strings_view_t,
}

impl<Name: AsRef<str>> CollectionNames<'_, Name> {
    /// The names under the prefix from the start on, sorted and cut at `wanted`, refusing one that is not UTF-8.
    ///
    /// # Safety
    /// The listing succeeded, and the arena it exported into stays alive and untouched for `'arena`.
    unsafe fn listed<'arena>(&self) -> Result<Names<'arena>> {
        // A collection without a name has an empty one on the tape.
        let listed = Strings::exported(self.names, self.written as usize)?;
        let (prefix, start) = (self.prefix.as_ref(), self.start.as_ref().map(AsRef::as_ref));
        let mut names = Vec::new();
        for bytes in (0..listed.len()).filter_map(|index| listed.get(index)) {
            if bytes.is_empty() {
                continue;
            }
            let name = std::str::from_utf8(bytes).map_err(|_| fail(Status::Malformed, "a stored name is not UTF-8"))?;
            if name.starts_with(prefix) && start.is_none_or(|start| name >= start) {
                names.push(name);
            }
        }
        names.sort_unstable();
        names.truncate(self.wanted);
        Ok(Names { names })
    }
}

impl<'arena, Name: AsRef<str>> Calling for CollectionNames<'arena, Name> {
    type Arguments = ffi::ustore_collection_list_t;
    type Output = Names<'arena>;

    fn arguments<Parent, Completion>(
        &mut self,
        view: &View<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        Ok(Some(ffi::ustore_collection_list_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            snapshot: view.snapshot(),
            collections: ptr::null_mut(),
            modalities: ptr::null_mut(),
            names: &mut self.names,
            written: &mut self.written,
            arena: self.arena.handle,
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_collection_list(arguments)
    }

    fn finish(self) -> Result<Names<'arena>> {
        // SAFETY: the listing succeeded into the arena, which stays borrowed for `'arena`.
        unsafe { self.listed() }
    }
}

// endregion: Names

// region: Tests

#[cfg(test)]
mod tests {
    use super::*;
    use crate::db::{Database, DropMode, Durability, Executor, RAMOptions, Watch};

    /// A database holding three named collections.
    fn three_named() -> (Database, Arena) {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        for name in ["people", "places", "planets"] {
            database.collection_create(name, Modality::Blobs).unwrap();
        }
        (database, Arena::new().unwrap())
    }

    /// Drops the collection `name` addresses, together with its name.
    fn drop_named(database: &Database, name: &str, arena: &mut Arena) {
        let (collection, _) = database.head().collection_find(name, arena).unwrap();
        database.collection_drop(collection, DropMode::Collection).unwrap();
    }

    #[test]
    fn names_walk_in_the_order_of_their_text() {
        let (database, mut arena) = three_named();
        let names = database.head().collection_names("", None, 10, &mut arena).unwrap();
        assert_eq!(names.len(), 3);
        assert!(!names.is_empty());
        let walked: Vec<&str> = names.iter().collect();
        assert_eq!(walked, vec!["people", "places", "planets"]);
        assert_eq!(names.get(3), None);
    }

    #[test]
    fn names_walk_under_a_prefix_and_resume_from_a_start() {
        let (database, mut arena) = three_named();
        let under = database.head().collection_names("pl", None, 10, &mut arena).unwrap();
        assert_eq!(under.iter().collect::<Vec<&str>>(), vec!["places", "planets"]);

        let page = database.head().collection_names("", None, 2, &mut arena).unwrap();
        assert_eq!(page.len(), 2);
        let resume = page.get(1).unwrap().to_string();
        let rest = database
            .head()
            .collection_names("", Some(resume.as_str()), 2, &mut arena)
            .unwrap();
        assert_eq!(rest.get(0), Some("places"), "a walk resumes at the start it is given");

        // A prefix nothing shares walks nothing.
        let none = database.head().collection_names("zz", None, 10, &mut arena).unwrap();
        assert!(none.is_empty());
    }

    #[test]
    fn a_dropped_name_leaves_the_listing() {
        let (database, mut arena) = three_named();
        drop_named(&database, "places", &mut arena);
        let names = database.head().collection_names("", None, 10, &mut arena).unwrap();
        assert_eq!(names.iter().collect::<Vec<&str>>(), vec!["people", "planets"]);
    }

    #[test]
    fn names_follow_the_view_they_are_read_through() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new().unwrap();
        let snapshot = database.snapshot().unwrap();
        let people = database.collection_create("people", Modality::Blobs).unwrap();

        // The snapshot predates the binding, so only the head resolves the name.
        assert_eq!(
            database.head().collection_find("people", &mut arena).unwrap(),
            (people, Modality::Blobs)
        );
        assert_eq!(
            snapshot
                .view()
                .collection_find("people", &mut arena)
                .unwrap_err()
                .status,
            Status::NotFound
        );

        // A transaction resolves names at the head, so it reaches a collection created after it began, and a name
        // it found missing never fails its commit.
        let mut transaction = database.transaction().unwrap();
        assert!(transaction
            .view(Watch::Keys)
            .collection_find("planets", &mut arena)
            .is_err());
        let planets = database.collection_create("planets", Modality::Blobs).unwrap();
        assert_eq!(
            transaction
                .view(Watch::Keys)
                .collection_find("planets", &mut arena)
                .unwrap(),
            (planets, Modality::Blobs)
        );
        transaction.commit(Durability::Buffered).unwrap();
    }

    #[test]
    fn a_taken_name_is_refused_and_a_dropped_one_is_free() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new().unwrap();
        let first = database.collection_create("people", Modality::Blobs).unwrap();
        let taken = database.collection_create("people", Modality::Blobs).unwrap_err();
        assert_eq!(taken.status, Status::AlreadyExists);
        drop_named(&database, "people", &mut arena);
        assert_eq!(
            database
                .head()
                .collection_find("people", &mut arena)
                .unwrap_err()
                .status,
            Status::NotFound
        );
        let second = database.collection_create("people", Modality::Blobs).unwrap();
        assert_ne!(first, second);

        // Names starting with `_` belong to the system collections, which no lookup or listing reaches.
        let reserved = database.collection_create("_people", Modality::Blobs).unwrap_err();
        assert_eq!(reserved.status, Status::InvalidArgument);
        for system in ["_names", "_collections"] {
            let lookup = database.head().collection_find(system, &mut arena).unwrap_err();
            assert_eq!(lookup.status, Status::NotFound);
        }
        let names = database.head().collection_names("", None, 10, &mut arena).unwrap();
        assert_eq!(names.iter().collect::<Vec<&str>>(), vec!["people"]);
    }

    #[test]
    fn names_resolve_through_a_deferred_view() {
        let (database, mut arena) = three_named();
        let executor = Executor::for_ram(&[], 1, 0).unwrap();
        let deferred = database.head().deferred(&executor);
        let found = deferred
            .collection_find(String::from("places"), &mut arena)
            .unwrap()
            .wait()
            .unwrap();
        assert_eq!(found, database.head().collection_find("places", &mut arena).unwrap());
        let missing = deferred
            .collection_find("nowhere", &mut arena)
            .unwrap()
            .wait()
            .unwrap_err();
        assert_eq!(missing.status, Status::NotFound);
    }

    #[test]
    fn names_walk_through_a_deferred_view() {
        let (database, mut arena) = three_named();
        let executor = Executor::for_ram(&[], 1, 0).unwrap();
        let deferred = database.head().deferred(&executor);
        let listing = deferred
            .collection_names(String::from("pl"), None, 10, &mut arena)
            .unwrap()
            .wait()
            .unwrap();
        assert_eq!(listing.iter().collect::<Vec<&str>>(), vec!["places", "planets"]);
        let resumed = deferred
            .collection_names("", Some("places"), 1, &mut arena)
            .unwrap()
            .wait()
            .unwrap();
        assert_eq!(resumed.iter().collect::<Vec<&str>>(), vec!["places"]);
    }
}

// endregion: Tests
