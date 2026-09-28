//! The names of collections, which the collection calls of `include/ustore/db.h` record and resolve.
//!
//! Names resolve in two phases: a name maps onto a collection once, and every other call addresses
//! the collection by its identifier. Each engine keeps the names apart from every collection, so no
//! identifier reaches them.

use std::ffi::c_char;
use std::ptr;

use crate::db::ffi::{
    self, ustore_char_t, ustore_collection_id_t, ustore_database_t, ustore_error_t, ustore_length_t, ustore_octet_t,
    ustore_size_t,
};
use crate::db::{check, described_length, exported_slice, fail, Arena, CollectionID, Modality, Result, Status, View};

// region: Outputs

/// The names of collections one listing found, in the order of their text.
///
/// The run borrows the arena it was exported into, and every name in it was checked for UTF-8 once,
/// when the listing returned.
#[derive(Clone, Debug)]
pub struct Names<'arena> {
    names: Vec<&'arena str>,
}

impl<'arena> Names<'arena> {
    /// How many names the listing kept, never above the limit it was given.
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
    pub fn iter(&self) -> impl Iterator<Item = &'arena str> + '_ {
        self.names.iter().copied()
    }
}

// endregion: Outputs

// region: Names

/// Creates a collection under `name`, which no other collection may carry, answering its identifier.
pub(crate) fn create_named(database: ustore_database_t, name: &str, modality: Modality) -> Result<CollectionID> {
    let text_length = described_length(name.len(), "the name is longer than the ABI can describe")?;
    let mut collection = CollectionID::default();
    let mut error = ustore_error_t::default();
    let mut arguments = ffi::ustore_collection_create_t {
        db: database,
        error: &mut error,
        name: name.as_ptr().cast::<c_char>(),
        name_length: text_length,
        modality: match modality {
            Modality::Any => ffi::USTORE_MODALITY_ANY_K,
            Modality::Blobs => ffi::USTORE_MODALITY_BLOBS_K,
            Modality::Docs => ffi::USTORE_MODALITY_DOCS_K,
            Modality::Graph => ffi::USTORE_MODALITY_GRAPH_K,
            Modality::Strings => ffi::USTORE_MODALITY_STRINGS_K,
            Modality::Pages => ffi::USTORE_MODALITY_PAGES_K,
        },
        collection: &mut collection,
    };
    // SAFETY: the name outlives the call, whose length spares it a null terminator, and so does the output.
    unsafe { ffi::ustore_collection_create(&mut arguments) };
    check(error)?;
    Ok(collection)
}

impl View<'_> {
    /// The collection `name` addresses, which [`crate::Database::create_collection`] bound.
    ///
    /// Names resolve at the view's snapshot or at the head, never inside a transaction, so a lookup
    /// never joins the conflicts of the work that follows it.
    pub fn collection(&self, arena: &mut Arena, name: &str) -> Result<CollectionID> {
        let text = name.as_ptr().cast::<c_char>();
        let text_length = described_length(name.len(), "the name is longer than the ABI can describe")?;
        let mut presences: *mut ustore_octet_t = ptr::null_mut();
        let mut ids: *mut ustore_collection_id_t = ptr::null_mut();
        let mut error = ustore_error_t::default();
        let mut arguments = ffi::ustore_collection_find_t {
            db: self.database(),
            error: &mut error,
            snapshot: self.snapshot(),
            arena: arena.slot(),
            options: ffi::USTORE_OPTIONS_DEFAULT_K,
            tasks_count: 1,
            names: &text,
            names_stride: 0,
            names_lengths: &text_length,
            names_lengths_stride: 0,
            presences: &mut presences,
            collections: &mut ids,
            modalities: ptr::null_mut(),
        };
        // SAFETY: the name outlives the call, whose length spares it a null terminator, and every
        // output pointer outlives it too.
        unsafe { ffi::ustore_collection_find(&mut arguments) };
        check(error)?;
        // SAFETY: a successful lookup exports one presence bit and one identifier into the arena.
        match unsafe { *presences & 1 } {
            0 => Err(fail(Status::NotFound, "no collection answers to that name")),
            _ => Ok(unsafe { *ids }),
        }
    }

    /// Lists up to `limit` collection names under `prefix`, in the order of their text, from `start`
    /// on where one is given.
    ///
    /// An engine lists its collections in an order of its own, so the names are sorted here, and the
    /// listing reads them at the view's snapshot or at the head, as a lookup does.
    pub fn collection_names<'arena>(
        &self,
        arena: &'arena mut Arena,
        prefix: &str,
        start: Option<&str>,
        limit: u64,
    ) -> Result<Names<'arena>> {
        let mut count: ustore_size_t = 0;
        let mut offsets: *mut ustore_length_t = ptr::null_mut();
        let mut tape: *mut ustore_char_t = ptr::null_mut();
        let mut error = ustore_error_t::default();
        let mut arguments = ffi::ustore_collection_list_t {
            db: self.database(),
            error: &mut error,
            transaction: ptr::null_mut(),
            snapshot: self.snapshot(),
            arena: arena.slot(),
            options: ffi::USTORE_OPTIONS_DEFAULT_K,
            count: &mut count,
            collections: ptr::null_mut(),
            modalities: ptr::null_mut(),
            names_offsets: &mut offsets,
            names: &mut tape,
        };
        // SAFETY: every output outlives the call.
        unsafe { ffi::ustore_collection_list(&mut arguments) };
        check(error)?;

        // A successful listing exports one more offset than collections into one tape, all of it alive
        // for as long as the arena stays borrowed, where a collection without a name has an empty one.
        let mut names = Vec::new();
        for index in 0..count as usize {
            // SAFETY: `index` is below the count the listing reported, and the tape is still borrowed.
            let bytes: &'arena [u8] = unsafe {
                let begin = *offsets.add(index) as usize;
                let end = *offsets.add(index + 1) as usize;
                exported_slice(tape.add(begin).cast::<u8>(), end - begin)
            };
            if bytes.is_empty() {
                continue;
            }
            let name = std::str::from_utf8(bytes).map_err(|_| fail(Status::Malformed, "a stored name is not UTF-8"))?;
            if name.starts_with(prefix) && start.is_none_or(|start| name >= start) {
                names.push(name);
            }
        }
        names.sort_unstable();
        names.truncate(limit as usize);
        Ok(Names { names })
    }
}

// endregion: Names

// region: Tests

#[cfg(test)]
mod tests {
    use super::*;
    use crate::db::{Conflicts, Database, Durability, RAMOptions};

    /// A database whose catalog holds three named collections.
    fn cataloged() -> (Database, Arena) {
        let database = Database::open_ram(&RAMOptions::default()).unwrap();
        for name in ["people", "places", "planets"] {
            database.create_collection(name, Modality::Any).unwrap();
        }
        (database, Arena::new())
    }

    #[test]
    fn names_walk_in_the_order_of_their_text() {
        let (database, mut arena) = cataloged();
        let names = database.head().collection_names(&mut arena, "", None, 10).unwrap();
        assert_eq!(names.len(), 3);
        assert!(!names.is_empty());
        let walked: Vec<&str> = names.iter().collect();
        assert_eq!(walked, vec!["people", "places", "planets"]);
        assert_eq!(names.get(3), None);
    }

    #[test]
    fn names_walk_under_a_prefix_and_resume_from_a_start() {
        let (database, mut arena) = cataloged();
        let under = database.head().collection_names(&mut arena, "pl", None, 10).unwrap();
        assert_eq!(under.iter().collect::<Vec<&str>>(), vec!["places", "planets"]);

        let page = database.head().collection_names(&mut arena, "", None, 2).unwrap();
        assert_eq!(page.len(), 2);
        let resume = page.get(1).unwrap().to_string();
        let rest = database
            .head()
            .collection_names(&mut arena, "", Some(&resume), 2)
            .unwrap();
        assert_eq!(rest.get(0), Some("places"), "a walk resumes at the start it is given");

        // A prefix nothing shares walks nothing.
        let none = database.head().collection_names(&mut arena, "zz", None, 10).unwrap();
        assert!(none.is_empty());
    }

    #[test]
    fn a_dropped_name_leaves_the_catalog() {
        let (database, mut arena) = cataloged();
        database.drop_collection(&mut arena, "places").unwrap();
        let names = database.head().collection_names(&mut arena, "", None, 10).unwrap();
        assert_eq!(names.iter().collect::<Vec<&str>>(), vec!["people", "planets"]);
    }

    #[test]
    fn the_catalog_follows_the_view_it_is_read_through() {
        let database = Database::open_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new();
        let snapshot = database.snapshot().unwrap();
        let people = database.create_collection("people", Modality::Any).unwrap();

        // The snapshot predates the binding, so only the head resolves the name.
        assert_eq!(database.head().collection(&mut arena, "people").unwrap(), people);
        assert_eq!(
            snapshot.view().collection(&mut arena, "people").unwrap_err().status,
            Status::NotFound
        );

        // A transaction resolves names at the head, so a name created after it began is found.
        let mut transaction = database.transaction().unwrap();
        let planets = database.create_collection("planets", Modality::Any).unwrap();
        assert_eq!(
            transaction
                .view(Conflicts::Watched)
                .collection(&mut arena, "planets")
                .unwrap(),
            planets
        );
        transaction.commit(Durability::Buffered).unwrap();
    }

    #[test]
    fn a_taken_name_is_refused_and_a_dropped_one_is_free() {
        let database = Database::open_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new();
        let first = database.create_collection("people", Modality::Any).unwrap();
        let taken = database.create_collection("people", Modality::Any).unwrap_err();
        assert_eq!(taken.status, Status::AlreadyExists);
        database.drop_collection(&mut arena, "people").unwrap();
        assert_eq!(
            database.head().collection(&mut arena, "people").unwrap_err().status,
            Status::NotFound
        );
        let second = database.create_collection("people", Modality::Any).unwrap();
        assert_ne!(first, second);
    }
}

// endregion: Tests
