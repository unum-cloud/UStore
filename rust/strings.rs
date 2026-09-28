//! Collections keyed by variable-length strings - the Rust twin of `include/ustore/strings.h`.
//!
//! Names resolve in two phases: a string maps onto a value once through this modality, and every
//! other call addresses the collection by the identifier that value holds. The library keeps every
//! binding in its reserved catalog of names, so a name never reaches an engine.

use std::ffi::c_char;
use std::marker::PhantomData;
use std::ptr;

use crate::db::ffi::{
    ustore_arena_t, ustore_char_t, ustore_collection_t, ustore_database_t, ustore_enum_t, ustore_error_t,
    ustore_length_t, ustore_octet_t, ustore_size_t, ustore_snapshot_t, ustore_str_view_t, ustore_transaction_t,
};
use crate::db::{check, described_length, fail, first_count, Arena, Collection, Result, Status, View};

// region: Raw Declarations

/// The calls of `include/ustore/strings.h` this crate makes, mirrored field for field.
mod ffi {
    #![allow(non_camel_case_types)]

    use super::*;

    #[repr(C)]
    pub struct ustore_strings_scan_t {
        pub db: ustore_database_t,
        pub error: *mut ustore_error_t,
        pub transaction: ustore_transaction_t,
        pub snapshot: ustore_snapshot_t,
        pub arena: *mut ustore_arena_t,
        pub options: ustore_enum_t,
        pub tasks_count: ustore_size_t,
        pub collections: *const ustore_collection_t,
        pub collections_stride: ustore_size_t,
        pub prefixes: *const ustore_str_view_t,
        pub prefixes_stride: ustore_size_t,
        pub prefixes_lengths: *const ustore_length_t,
        pub prefixes_lengths_stride: ustore_size_t,
        pub start_strings: *const ustore_str_view_t,
        pub start_strings_stride: ustore_size_t,
        pub start_strings_lengths: *const ustore_length_t,
        pub start_strings_lengths_stride: ustore_size_t,
        pub count_limits: *const ustore_length_t,
        pub count_limits_stride: ustore_size_t,
        pub offsets: *mut *mut ustore_length_t,
        pub counts: *mut *mut ustore_length_t,
        pub strings_offsets: *mut *mut ustore_length_t,
        pub strings: *mut *mut ustore_char_t,
    }

    #[repr(C)]
    pub struct ustore_named_collections_find_t {
        pub db: ustore_database_t,
        pub error: *mut ustore_error_t,
        pub snapshot: ustore_snapshot_t,
        pub arena: *mut ustore_arena_t,
        pub options: ustore_enum_t,
        pub tasks_count: ustore_size_t,
        pub names: *const ustore_str_view_t,
        pub names_stride: ustore_size_t,
        pub names_lengths: *const ustore_length_t,
        pub names_lengths_stride: ustore_size_t,
        pub presences: *mut *mut ustore_octet_t,
        pub ids: *mut *mut ustore_collection_t,
        pub modalities: *mut *mut ustore_enum_t,
    }

    #[repr(C)]
    pub struct ustore_named_collections_create_t {
        pub db: ustore_database_t,
        pub error: *mut ustore_error_t,
        pub arena: *mut ustore_arena_t,
        pub name: ustore_str_view_t,
        pub name_length: ustore_length_t,
        pub modality: ustore_enum_t,
        pub id: *mut ustore_collection_t,
    }

    #[repr(C)]
    pub struct ustore_named_collections_drop_t {
        pub db: ustore_database_t,
        pub error: *mut ustore_error_t,
        pub arena: *mut ustore_arena_t,
        pub name: ustore_str_view_t,
        pub name_length: ustore_length_t,
    }

    extern "C" {
        pub fn ustore_strings_scan(arguments: *mut ustore_strings_scan_t);
        pub fn ustore_named_collections_find(arguments: *mut ustore_named_collections_find_t);
        pub fn ustore_named_collections_create(arguments: *mut ustore_named_collections_create_t);
        pub fn ustore_named_collections_drop(arguments: *mut ustore_named_collections_drop_t);
    }
}

// endregion: Raw Declarations

// region: Outputs

/// The names one walk of the catalog found, in the order of their text.
///
/// The run borrows the arena it was exported into, and every name in it was checked for UTF-8 once,
/// when the walk returned.
#[derive(Clone, Copy, Debug)]
pub struct Names<'arena> {
    offsets: *const ustore_length_t,
    tape: *const c_char,
    count: usize,
    marker: PhantomData<&'arena Arena>,
}

impl<'arena> Names<'arena> {
    /// How many names the walk found, never above the limit it was given.
    pub fn len(&self) -> usize {
        self.count
    }

    /// Whether the walk found nothing.
    pub fn is_empty(&self) -> bool {
        self.count == 0
    }

    /// The name at `index`, or `None` past the end of the walk.
    pub fn get(&self, index: usize) -> Option<&'arena str> {
        if index >= self.count {
            return None;
        }
        // SAFETY: the walk exported `count` plus one offsets into one tape, and every name between
        // two of them was checked for UTF-8 before this run was handed out.
        unsafe { Some(std::str::from_utf8_unchecked(self.bytes(index))) }
    }

    /// Every name the walk found, in the order of its text.
    pub fn iter(&self) -> impl Iterator<Item = &'arena str> + '_ {
        let names = *self;
        (0..names.count).filter_map(move |index| names.get(index))
    }

    /// The bytes of the name at `index`.
    ///
    /// # Safety
    /// `index` is below `count`, and the arena still holds the tape and its offsets.
    unsafe fn bytes(&self, index: usize) -> &'arena [u8] {
        let begin = *self.offsets.add(index) as usize;
        let end = *self.offsets.add(index + 1) as usize;
        match end <= begin {
            true => &[],
            false => std::slice::from_raw_parts(self.tape.add(begin).cast::<u8>(), end - begin),
        }
    }
}

// endregion: Outputs

// region: Names

/// Creates a collection under `name`, which no other collection may carry, answering its identifier.
pub(crate) fn create_named(database: ustore_database_t, arena: &mut Arena, name: &str) -> Result<Collection> {
    let text = name.as_ptr().cast::<c_char>();
    let text_length = described_length(name.len(), "the name is longer than the ABI can describe")?;
    let mut id: ustore_collection_t = 0;
    let mut error = ustore_error_t::default();
    let mut arguments = ffi::ustore_named_collections_create_t {
        db: database,
        error: &mut error,
        arena: arena.slot(),
        name: text,
        name_length: text_length,
        modality: crate::db::ffi::USTORE_MODALITY_ANY_K,
        id: &mut id,
    };
    // SAFETY: the name outlives the call, whose length spares it a null terminator, and so does the output.
    unsafe { ffi::ustore_named_collections_create(&mut arguments) };
    check(error)?;
    Ok(Collection(id))
}

/// Drops the collection `name` addresses, together with the name.
pub(crate) fn drop_named(database: ustore_database_t, arena: &mut Arena, name: &str) -> Result<()> {
    let text = name.as_ptr().cast::<c_char>();
    let text_length = described_length(name.len(), "the name is longer than the ABI can describe")?;
    let mut error = ustore_error_t::default();
    let mut arguments = ffi::ustore_named_collections_drop_t {
        db: database,
        error: &mut error,
        arena: arena.slot(),
        name: text,
        name_length: text_length,
    };
    // SAFETY: the name outlives the call, whose length spares it a null terminator.
    unsafe { ffi::ustore_named_collections_drop(&mut arguments) };
    check(error)
}

impl View<'_> {
    /// The collection `name` addresses, which [`crate::Database::create_collection`] bound.
    ///
    /// Names resolve at the view's snapshot or at the head, never inside a transaction, so a lookup
    /// never joins the conflicts of the work that follows it.
    pub fn collection(&self, arena: &mut Arena, name: &str) -> Result<Collection> {
        let text = name.as_ptr().cast::<c_char>();
        let text_length = described_length(name.len(), "the name is longer than the ABI can describe")?;
        let mut presences: *mut ustore_octet_t = ptr::null_mut();
        let mut ids: *mut ustore_collection_t = ptr::null_mut();
        let mut error = ustore_error_t::default();
        let mut arguments = ffi::ustore_named_collections_find_t {
            db: self.database(),
            error: &mut error,
            snapshot: self.snapshot(),
            arena: arena.slot(),
            options: 0,
            tasks_count: 1,
            names: &text,
            names_stride: 0,
            names_lengths: &text_length,
            names_lengths_stride: 0,
            presences: &mut presences,
            ids: &mut ids,
            modalities: ptr::null_mut(),
        };
        // SAFETY: the name outlives the call, whose length spares it a null terminator, and every
        // output pointer outlives it too.
        unsafe { ffi::ustore_named_collections_find(&mut arguments) };
        check(error)?;
        // SAFETY: a successful lookup exports one presence bit and one identifier into the arena.
        match unsafe { *presences & 1 } {
            0 => Err(fail(Status::NotFound, "no collection answers to that name")),
            _ => Ok(Collection(unsafe { *ids })),
        }
    }

    /// Lists up to `limit` collection names under `prefix`, resuming from `start` where one is given.
    pub fn collection_names<'arena>(
        &self,
        arena: &'arena mut Arena,
        prefix: &str,
        start: Option<&str>,
        limit: u32,
    ) -> Result<Names<'arena>> {
        let prefix_text = prefix.as_ptr().cast::<c_char>();
        let prefix_length = described_length(prefix.len(), "the prefix is longer than the ABI can describe")?;
        let start_text = start.map_or(ptr::null(), |text| text.as_ptr().cast::<c_char>());
        let start_length = match start {
            Some(text) => described_length(text.len(), "the start is longer than the ABI can describe")?,
            None => 0,
        };
        let catalog = Collection::names();
        let mut counts: *mut ustore_length_t = ptr::null_mut();
        let mut offsets: *mut ustore_length_t = ptr::null_mut();
        let mut tape: *mut ustore_char_t = ptr::null_mut();
        let mut error = ustore_error_t::default();
        let mut arguments = ffi::ustore_strings_scan_t {
            db: self.database(),
            error: &mut error,
            transaction: self.transaction(),
            snapshot: self.snapshot(),
            arena: arena.slot(),
            options: self.options(),
            tasks_count: 1,
            collections: &catalog.0,
            collections_stride: 0,
            prefixes: &prefix_text,
            prefixes_stride: 0,
            prefixes_lengths: &prefix_length,
            prefixes_lengths_stride: 0,
            start_strings: match start {
                Some(_) => &start_text as *const ustore_str_view_t,
                None => ptr::null(),
            },
            start_strings_stride: 0,
            start_strings_lengths: match start {
                Some(_) => &start_length as *const ustore_length_t,
                None => ptr::null(),
            },
            start_strings_lengths_stride: 0,
            count_limits: &limit,
            count_limits_stride: 0,
            offsets: ptr::null_mut(),
            counts: &mut counts,
            strings_offsets: &mut offsets,
            strings: &mut tape,
        };
        // SAFETY: the prefix, the start and every output outlive the call.
        unsafe { ffi::ustore_strings_scan(&mut arguments) };
        check(error)?;

        // A successful walk exports that many names inside one tape the offsets close, all of it
        // alive for as long as the arena stays borrowed.
        let names = Names {
            offsets,
            tape,
            count: first_count(counts),
            marker: PhantomData,
        };
        for index in 0..names.count {
            // SAFETY: `index` is below the count the walk reported, and the tape is still borrowed.
            let bytes = unsafe { names.bytes(index) };
            if std::str::from_utf8(bytes).is_err() {
                return Err(fail(Status::Malformed, "a stored name is not UTF-8"));
            }
        }
        Ok(names)
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
        let mut arena = Arena::new();
        for name in ["people", "places", "planets"] {
            database.create_collection(&mut arena, name).unwrap();
        }
        (database, arena)
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
        let people = database.create_collection(&mut arena, "people").unwrap();

        // The snapshot predates the binding, so only the head resolves the name.
        assert_eq!(database.head().collection(&mut arena, "people").unwrap(), people);
        assert_eq!(
            snapshot.view().collection(&mut arena, "people").unwrap_err().status,
            Status::NotFound
        );

        // A transaction resolves names at the head, so a name created after it began is found.
        let mut transaction = database.transaction().unwrap();
        let planets = database.create_collection(&mut arena, "planets").unwrap();
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
        let first = database.create_collection(&mut arena, "people").unwrap();
        let taken = database.create_collection(&mut arena, "people").unwrap_err();
        assert_eq!(taken.status, Status::AlreadyExists);
        database.drop_collection(&mut arena, "people").unwrap();
        assert_eq!(
            database.head().collection(&mut arena, "people").unwrap_err().status,
            Status::NotFound
        );
        let second = database.create_collection(&mut arena, "people").unwrap();
        assert_ne!(first, second);
    }
}

// endregion: Tests
