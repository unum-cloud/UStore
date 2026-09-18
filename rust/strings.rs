//! Collections keyed by variable-length strings - the Rust twin of `include/ustore/strings.h`.
//!
//! Names resolve in two phases: a string maps onto a value once through this modality, and every
//! other call addresses the collection by the identifier that value holds. The reserved catalog of
//! names is where this crate keeps the binding, so a name never reaches an engine.

use std::ffi::c_char;
use std::marker::PhantomData;
use std::ptr;

use crate::db::ffi::{
    ustore_arena_t, ustore_byte_t, ustore_bytes_cptr_t, ustore_char_t, ustore_collection_t, ustore_database_t,
    ustore_enum_t, ustore_error_t, ustore_length_t, ustore_octet_t, ustore_size_t, ustore_snapshot_t,
    ustore_str_view_t, ustore_transaction_t,
};
use crate::db::{check, described_length, fail, first_count, missing_length, Arena, Collection, Result, Status, View};

// region: Raw Declarations

/// One-to-one mirror of `include/ustore/strings.h`.
mod ffi {
    #![allow(non_camel_case_types)]

    use super::*;

    #[repr(C)]
    pub struct ustore_strings_write_t {
        pub db: ustore_database_t,
        pub error: *mut ustore_error_t,
        pub transaction: ustore_transaction_t,
        pub arena: *mut ustore_arena_t,
        pub options: ustore_enum_t,
        pub tasks_count: ustore_size_t,
        pub collections: *const ustore_collection_t,
        pub collections_stride: ustore_size_t,
        pub strings: *const ustore_str_view_t,
        pub strings_stride: ustore_size_t,
        pub strings_lengths: *const ustore_length_t,
        pub strings_lengths_stride: ustore_size_t,
        pub values_presences: *const ustore_octet_t,
        pub values_offsets: *const ustore_length_t,
        pub values_offsets_stride: ustore_size_t,
        pub values_lengths: *const ustore_length_t,
        pub values_lengths_stride: ustore_size_t,
        pub values: *const ustore_bytes_cptr_t,
        pub values_stride: ustore_size_t,
    }

    #[repr(C)]
    pub struct ustore_strings_read_t {
        pub db: ustore_database_t,
        pub error: *mut ustore_error_t,
        pub transaction: ustore_transaction_t,
        pub snapshot: ustore_snapshot_t,
        pub arena: *mut ustore_arena_t,
        pub options: ustore_enum_t,
        pub tasks_count: ustore_size_t,
        pub collections: *const ustore_collection_t,
        pub collections_stride: ustore_size_t,
        pub strings: *const ustore_str_view_t,
        pub strings_stride: ustore_size_t,
        pub strings_lengths: *const ustore_length_t,
        pub strings_lengths_stride: ustore_size_t,
        pub presences: *mut *mut ustore_octet_t,
        pub offsets: *mut *mut ustore_length_t,
        pub lengths: *mut *mut ustore_length_t,
        pub values: *mut *mut ustore_byte_t,
    }

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

    extern "C" {
        pub fn ustore_strings_write(arguments: *mut ustore_strings_write_t);
        pub fn ustore_strings_read(arguments: *mut ustore_strings_read_t);
        pub fn ustore_strings_scan(arguments: *mut ustore_strings_scan_t);
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

/// Reads the value a name holds in the catalog, or `None` when nothing answers to it.
pub(crate) fn read_name<'arena>(view: View<'_>, arena: &'arena mut Arena, name: &str) -> Result<Option<&'arena [u8]>> {
    let text = name.as_ptr().cast::<c_char>();
    let text_length = described_length(name.len(), "the name is longer than the ABI can describe")?;
    let catalog = Collection::names();
    let mut offsets: *mut ustore_length_t = ptr::null_mut();
    let mut lengths: *mut ustore_length_t = ptr::null_mut();
    let mut values: *mut ustore_byte_t = ptr::null_mut();
    let mut error = ustore_error_t::default();
    let mut arguments = ffi::ustore_strings_read_t {
        db: view.database(),
        error: &mut error,
        transaction: view.transaction(),
        snapshot: view.snapshot(),
        arena: arena.slot(),
        options: view.options(),
        tasks_count: 1,
        collections: &catalog.0,
        collections_stride: 0,
        strings: &text,
        strings_stride: 0,
        strings_lengths: &text_length,
        strings_lengths_stride: 0,
        presences: ptr::null_mut(),
        offsets: &mut offsets,
        lengths: &mut lengths,
        values: &mut values,
    };
    // SAFETY: the name outlives the call, whose lengths spare it a null terminator, and every
    // output pointer outlives it too.
    unsafe { ffi::ustore_strings_read(&mut arguments) };
    check(error)?;
    // SAFETY: a successful read exports one length and one offset into the arena, which stays
    // borrowed for as long as the returned bytes do.
    unsafe {
        let length = *lengths;
        if length == missing_length() {
            return Ok(None);
        }
        let offset = *offsets as usize;
        Ok(Some(std::slice::from_raw_parts(values.add(offset), length as usize)))
    }
}

/// Binds a name to `value` in the catalog, or unbinds it when no value is given.
pub(crate) fn write_name(view: View<'_>, arena: &mut Arena, name: &str, value: Option<&[u8]>) -> Result<()> {
    let text = name.as_ptr().cast::<c_char>();
    let text_length = described_length(name.len(), "the name is longer than the ABI can describe")?;
    let bytes: ustore_bytes_cptr_t = match value {
        Some(value) => value.as_ptr(),
        None => ptr::null(),
    };
    let bytes_length = match value {
        Some(value) => described_length(value.len(), "the value is longer than the ABI can describe")?,
        None => 0,
    };
    let catalog = Collection::names();
    let mut error = ustore_error_t::default();
    let mut arguments = ffi::ustore_strings_write_t {
        db: view.database(),
        error: &mut error,
        transaction: view.transaction(),
        arena: arena.slot(),
        options: view.options(),
        tasks_count: 1,
        collections: &catalog.0,
        collections_stride: 0,
        strings: &text,
        strings_stride: 0,
        strings_lengths: &text_length,
        strings_lengths_stride: 0,
        values_presences: ptr::null(),
        values_offsets: ptr::null(),
        values_offsets_stride: 0,
        values_lengths: match value {
            Some(_) => &bytes_length as *const ustore_length_t,
            None => ptr::null(),
        },
        values_lengths_stride: 0,
        values: match value {
            Some(_) => &bytes as *const ustore_bytes_cptr_t,
            None => ptr::null(),
        },
        values_stride: 0,
    };
    // SAFETY: the name and the value outlive the call, and a null `values` is how the ABI spells
    // the removal of a name.
    unsafe { ffi::ustore_strings_write(&mut arguments) };
    check(error)
}

impl View<'_> {
    /// The collection `name` addresses, which [`crate::Database::create_collection`] bound.
    pub fn collection(&self, arena: &mut Arena, name: &str) -> Result<Collection> {
        let stored =
            read_name(*self, arena, name)?.ok_or(fail(Status::NotFound, "no collection answers to that name"))?;
        let identifier: [u8; size_of::<ustore_collection_t>()] = stored
            .try_into()
            .map_err(|_| fail(Status::Malformed, "the name holds something other than a collection"))?;
        Ok(Collection(ustore_collection_t::from_ne_bytes(identifier)))
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
    use crate::db::{Configuration, Conflicts, Database, Durability};

    /// A database whose catalog holds three named collections.
    fn cataloged() -> (Database, Arena) {
        let database = Database::open(Configuration::in_memory()).unwrap();
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
    fn a_name_bound_to_something_else_is_refused() {
        let database = Database::open(Configuration::in_memory()).unwrap();
        let mut arena = Arena::new();
        write_name(database.head(), &mut arena, "people", Some(b"not an identifier")).unwrap();
        let failure = database.head().collection(&mut arena, "people").unwrap_err();
        assert_eq!(failure.status, Status::Malformed);
    }

    #[test]
    fn names_read_back_what_was_written_under_them() {
        let database = Database::open(Configuration::in_memory()).unwrap();
        let mut arena = Arena::new();
        assert_eq!(read_name(database.head(), &mut arena, "absent").unwrap(), None);

        write_name(database.head(), &mut arena, "present", Some(b"value")).unwrap();
        assert_eq!(
            read_name(database.head(), &mut arena, "present").unwrap(),
            Some(b"value".as_slice())
        );

        write_name(database.head(), &mut arena, "present", None).unwrap();
        assert_eq!(read_name(database.head(), &mut arena, "present").unwrap(), None);
    }

    #[test]
    fn the_catalog_follows_the_view_it_is_read_through() {
        let database = Database::open(Configuration::in_memory()).unwrap();
        let mut arena = Arena::new();
        let snapshot = database.snapshot().unwrap();
        let people = database.create_collection(&mut arena, "people").unwrap();

        // The snapshot predates the binding, so only the head resolves the name.
        assert_eq!(database.head().collection(&mut arena, "people").unwrap(), people);
        assert_eq!(
            snapshot.view().collection(&mut arena, "people").unwrap_err().status,
            Status::NotFound
        );

        // A transaction sees the catalog as the head has it, and its own writes on top.
        let mut transaction = database.transaction().unwrap();
        assert_eq!(
            transaction
                .view(Conflicts::Watched)
                .collection(&mut arena, "people")
                .unwrap(),
            people
        );
        write_name(
            transaction.view(Conflicts::Watched),
            &mut arena,
            "pending",
            Some(&people.0.to_ne_bytes()),
        )
        .unwrap();
        assert_eq!(
            transaction
                .view(Conflicts::Watched)
                .collection(&mut arena, "pending")
                .unwrap(),
            people
        );
        assert_eq!(
            database.head().collection(&mut arena, "pending").unwrap_err().status,
            Status::NotFound
        );
        transaction.commit(Durability::Buffered).unwrap();
        assert_eq!(database.head().collection(&mut arena, "pending").unwrap(), people);
    }
}

// endregion: Tests
