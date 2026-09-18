//! Binary values - the Rust twin of `include/ustore/blobs.h`.
//!
//! Every verb takes a batch, and one operation is a batch of one. Inputs are borrowed slices the
//! strided ABI reads in place, so nothing is staged on the way in; outputs land in the [`Arena`] the
//! call borrows, so nothing is copied on the way out.

use std::marker::PhantomData;
use std::ptr;

use crate::db::ffi::{
    ustore_arena_t, ustore_byte_t, ustore_bytes_cptr_t, ustore_collection_t, ustore_database_t, ustore_enum_t,
    ustore_error_t, ustore_key_t, ustore_length_t, ustore_octet_t, ustore_size_t, ustore_snapshot_t,
    ustore_transaction_t,
};
use crate::db::{
    check, described_length, exported_slice, first_count, missing_length, Arena, Collection, Collections, Durability,
    Key, Result, View,
};

// region: Raw Declarations

/// One-to-one mirror of `include/ustore/blobs.h`.
mod ffi {
    #![allow(non_camel_case_types)]

    use super::*;

    #[repr(C)]
    pub struct ustore_write_t {
        pub db: ustore_database_t,
        pub error: *mut ustore_error_t,
        pub transaction: ustore_transaction_t,
        pub arena: *mut ustore_arena_t,
        pub options: ustore_enum_t,
        pub tasks_count: ustore_size_t,
        pub collections: *const ustore_collection_t,
        pub collections_stride: ustore_size_t,
        pub keys: *const ustore_key_t,
        pub keys_stride: ustore_size_t,
        pub presences: *const ustore_octet_t,
        pub offsets: *const ustore_length_t,
        pub offsets_stride: ustore_size_t,
        pub lengths: *const ustore_length_t,
        pub lengths_stride: ustore_size_t,
        pub values: *const ustore_bytes_cptr_t,
        pub values_stride: ustore_size_t,
    }

    #[repr(C)]
    pub struct ustore_read_t {
        pub db: ustore_database_t,
        pub error: *mut ustore_error_t,
        pub transaction: ustore_transaction_t,
        pub snapshot: ustore_snapshot_t,
        pub arena: *mut ustore_arena_t,
        pub options: ustore_enum_t,
        pub tasks_count: ustore_size_t,
        pub collections: *const ustore_collection_t,
        pub collections_stride: ustore_size_t,
        pub keys: *const ustore_key_t,
        pub keys_stride: ustore_size_t,
        pub presences: *mut *mut ustore_octet_t,
        pub offsets: *mut *mut ustore_length_t,
        pub lengths: *mut *mut ustore_length_t,
        pub values: *mut *mut ustore_byte_t,
    }

    #[repr(C)]
    pub struct ustore_scan_t {
        pub db: ustore_database_t,
        pub error: *mut ustore_error_t,
        pub transaction: ustore_transaction_t,
        pub snapshot: ustore_snapshot_t,
        pub arena: *mut ustore_arena_t,
        pub options: ustore_enum_t,
        pub tasks_count: ustore_size_t,
        pub collections: *const ustore_collection_t,
        pub collections_stride: ustore_size_t,
        pub start_keys: *const ustore_key_t,
        pub start_keys_stride: ustore_size_t,
        pub count_limits: *const ustore_length_t,
        pub count_limits_stride: ustore_size_t,
        pub offsets: *mut *mut ustore_length_t,
        pub counts: *mut *mut ustore_length_t,
        pub keys: *mut *mut ustore_key_t,
    }

    #[repr(C)]
    pub struct ustore_sample_t {
        pub db: ustore_database_t,
        pub error: *mut ustore_error_t,
        pub transaction: ustore_transaction_t,
        pub snapshot: ustore_snapshot_t,
        pub arena: *mut ustore_arena_t,
        pub options: ustore_enum_t,
        pub tasks_count: ustore_size_t,
        pub collections: *const ustore_collection_t,
        pub collections_stride: ustore_size_t,
        pub count_limits: *const ustore_length_t,
        pub count_limits_stride: ustore_size_t,
        pub offsets: *mut *mut ustore_length_t,
        pub counts: *mut *mut ustore_length_t,
        pub keys: *mut *mut ustore_key_t,
    }

    #[repr(C)]
    pub struct ustore_measure_t {
        pub db: ustore_database_t,
        pub error: *mut ustore_error_t,
        pub transaction: ustore_transaction_t,
        pub snapshot: ustore_snapshot_t,
        pub arena: *mut ustore_arena_t,
        pub options: ustore_enum_t,
        pub tasks_count: ustore_size_t,
        pub collections: *const ustore_collection_t,
        pub collections_stride: ustore_size_t,
        pub start_keys: *const ustore_key_t,
        pub start_keys_stride: ustore_size_t,
        pub end_keys: *const ustore_key_t,
        pub end_keys_stride: ustore_size_t,
        pub min_cardinalities: *mut *mut ustore_size_t,
        pub max_cardinalities: *mut *mut ustore_size_t,
        pub min_value_bytes: *mut *mut ustore_size_t,
        pub max_value_bytes: *mut *mut ustore_size_t,
        pub min_space_usages: *mut *mut ustore_size_t,
        pub max_space_usages: *mut *mut ustore_size_t,
    }

    extern "C" {
        pub fn ustore_write(arguments: *mut ustore_write_t);
        pub fn ustore_read(arguments: *mut ustore_read_t);
        pub fn ustore_scan(arguments: *mut ustore_scan_t);
        pub fn ustore_sample(arguments: *mut ustore_sample_t);
        pub fn ustore_measure(arguments: *mut ustore_measure_t);
    }
}

// endregion: Raw Declarations

// region: Inputs

/// One key and the bytes stored under it, laid out so a slice of them feeds the strided ABI as it
/// stands - one array for the keys, one for the pointers, one for the lengths.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct Entry<'value> {
    pub(crate) key: Key,
    pub(crate) value: *const u8,
    pub(crate) length: ustore_length_t,
    marker: PhantomData<&'value [u8]>,
}

impl<'value> Entry<'value> {
    /// Pairs a key with the bytes to store under it, refusing a value past the 32-bit length the ABI
    /// carries.
    pub fn new(key: Key, value: &'value [u8]) -> Result<Self> {
        let length = described_length(value.len(), "the value is longer than the ABI can describe")?;
        Ok(Entry {
            key,
            value: value.as_ptr(),
            length,
            marker: PhantomData,
        })
    }

    /// The key this entry stores under.
    pub fn key(&self) -> Key {
        self.key
    }

    /// Names a key to remove, which a batch mixes freely with the entries that carry values.
    ///
    /// The ABI reads a null value pointer as a removal, so no bitset of present values is needed and
    /// nothing is staged; an empty slice still points somewhere, and writes an empty value.
    pub fn removal(key: Key) -> Self {
        Entry {
            key,
            value: ptr::null(),
            length: 0,
            marker: PhantomData,
        }
    }

    /// The bytes this entry stores, or `None` for an entry that removes its key.
    pub fn value(&self) -> Option<&'value [u8]> {
        if self.value.is_null() {
            return None;
        }
        // SAFETY: the pointer and the length came from one borrowed slice alive for `'value`.
        Some(unsafe { exported_slice(self.value, self.length as usize) })
    }
}

// endregion: Inputs

// region: Outputs

/// The values one read exported, addressed by the position of their key in the batch.
///
/// The run borrows the arena it was exported into, so the next call through that arena - which
/// would overwrite it - cannot be made while it lives.
#[derive(Clone, Copy, Debug)]
pub struct Values<'arena> {
    offsets: *const ustore_length_t,
    lengths: *const ustore_length_t,
    tape: *const ustore_byte_t,
    count: usize,
    marker: PhantomData<&'arena Arena>,
}

impl<'arena> Values<'arena> {
    /// The run a read of `count` values exported, or an empty one when the read never reached an engine.
    pub(crate) fn exported(
        offsets: *const ustore_length_t,
        lengths: *const ustore_length_t,
        tape: *const ustore_byte_t,
        count: usize,
    ) -> Self {
        Values {
            offsets,
            lengths,
            tape,
            count: if lengths.is_null() { 0 } else { count },
            marker: PhantomData,
        }
    }

    /// How many keys the read covered.
    pub fn len(&self) -> usize {
        self.count
    }

    /// Whether the read covered no keys at all.
    pub fn is_empty(&self) -> bool {
        self.count == 0
    }

    /// The value of the key at `index`, or `None` for a key the database does not hold.
    pub fn get(&self, index: usize) -> Option<&'arena [u8]> {
        if index >= self.count {
            return None;
        }
        // SAFETY: a successful read exports one length and one offset per key into the arena, which
        // stays borrowed for `'arena`, and every present value sits inside the tape they address.
        unsafe {
            let length = *self.lengths.add(index);
            if length == missing_length() {
                return None;
            }
            let offset = *self.offsets.add(index) as usize;
            Some(exported_slice(self.tape.add(offset), length as usize))
        }
    }

    /// Every value in the order its key was asked for.
    pub fn iter(&self) -> impl Iterator<Item = Option<&'arena [u8]>> + '_ {
        let values = *self;
        (0..values.count).map(move |index| values.get(index))
    }
}

/// The bounds one measurement reported over a range of keys, each a lower and an upper estimate.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub struct Measurement {
    /// Lower bound on the entries in the range.
    pub min_entries: u64,
    /// Upper bound on the entries in the range.
    pub max_entries: u64,
    /// Lower bound on the bytes of values in the range.
    pub min_value_bytes: u64,
    /// Upper bound on the bytes of values in the range.
    pub max_value_bytes: u64,
    /// Lower bound on the disk space the range takes.
    pub min_space_usage: u64,
    /// Upper bound on the disk space the range takes.
    pub max_space_usage: u64,
}

// endregion: Outputs

// region: Verbs

impl View<'_> {
    /// Upserts a batch of values, removing the keys whose entries carry none.
    ///
    /// The batch spans as many collections as it has entries, and a bare [`Collection`] gives every
    /// entry the same one. A transaction flushes at its commit, so [`Durability::Flushed`] is refused
    /// inside one.
    pub fn write<'collections>(
        &self,
        arena: &mut Arena,
        collections: impl Into<Collections<'collections>>,
        entries: &[Entry<'_>],
        durability: Durability,
    ) -> Result<()> {
        if entries.is_empty() {
            return Ok(());
        }
        let collections = collections.into();
        let (collections, collections_stride) = collections.strided(entries.len())?;
        let stride = size_of::<Entry<'_>>() as ustore_size_t;
        let first = &entries[0];
        let mut error = ustore_error_t::default();
        let mut arguments = ffi::ustore_write_t {
            db: self.database(),
            error: &mut error,
            transaction: self.transaction(),
            arena: arena.slot(),
            options: self.options() | durability.to_raw(),
            tasks_count: entries.len() as ustore_size_t,
            collections,
            collections_stride,
            keys: &first.key,
            keys_stride: stride,
            presences: ptr::null(),
            offsets: ptr::null(),
            offsets_stride: 0,
            lengths: &first.length,
            lengths_stride: stride,
            values: &first.value,
            values_stride: stride,
        };
        // SAFETY: the entries and the collections outlive the call, and the members of every entry sit
        // `stride` bytes apart, which is what `#[repr(C)]` on `Entry` promises.
        unsafe { ffi::ustore_write(&mut arguments) };
        check(error)
    }

    /// Removes a batch of keys, whether or not the database holds them.
    pub fn erase<'collections>(
        &self,
        arena: &mut Arena,
        collections: impl Into<Collections<'collections>>,
        keys: &[Key],
        durability: Durability,
    ) -> Result<()> {
        if keys.is_empty() {
            return Ok(());
        }
        let collections = collections.into();
        let (collections, collections_stride) = collections.strided(keys.len())?;
        let mut error = ustore_error_t::default();
        let mut arguments = ffi::ustore_write_t {
            db: self.database(),
            error: &mut error,
            transaction: self.transaction(),
            arena: arena.slot(),
            options: self.options() | durability.to_raw(),
            tasks_count: keys.len() as ustore_size_t,
            collections,
            collections_stride,
            keys: keys.as_ptr(),
            keys_stride: size_of::<Key>() as ustore_size_t,
            presences: ptr::null(),
            offsets: ptr::null(),
            offsets_stride: 0,
            lengths: ptr::null(),
            lengths_stride: 0,
            values: ptr::null(),
            values_stride: 0,
        };
        // SAFETY: the keys and the collections outlive the call, and a null `values` is how the ABI
        // spells a removal.
        unsafe { ffi::ustore_write(&mut arguments) };
        check(error)
    }

    /// Reads a batch of keys into the arena, one collection per key or one for all of them.
    pub fn read<'arena, 'collections>(
        &self,
        arena: &'arena mut Arena,
        collections: impl Into<Collections<'collections>>,
        keys: &[Key],
    ) -> Result<Values<'arena>> {
        let mut offsets: *mut ustore_length_t = ptr::null_mut();
        let mut lengths: *mut ustore_length_t = ptr::null_mut();
        let mut tape: *mut ustore_byte_t = ptr::null_mut();
        if keys.is_empty() {
            return Ok(Values::exported(offsets, lengths, tape, 0));
        }
        let collections = collections.into();
        let (collections, collections_stride) = collections.strided(keys.len())?;
        let mut error = ustore_error_t::default();
        let mut arguments = ffi::ustore_read_t {
            db: self.database(),
            error: &mut error,
            transaction: self.transaction(),
            snapshot: self.snapshot(),
            arena: arena.slot(),
            options: self.options(),
            tasks_count: keys.len() as ustore_size_t,
            collections,
            collections_stride,
            keys: keys.as_ptr(),
            keys_stride: size_of::<Key>() as ustore_size_t,
            presences: ptr::null_mut(),
            offsets: &mut offsets,
            lengths: &mut lengths,
            values: &mut tape,
        };
        // SAFETY: the keys, the collections and the outputs outlive the call.
        unsafe { ffi::ustore_read(&mut arguments) };
        check(error)?;
        Ok(Values::exported(offsets, lengths, tape, keys.len()))
    }

    /// Lists up to `limit` keys of one collection from `start`, which is how a walk paginates.
    pub fn scan<'arena>(
        &self,
        arena: &'arena mut Arena,
        collection: Collection,
        start: Key,
        limit: u32,
    ) -> Result<&'arena [Key]> {
        let mut counts: *mut ustore_length_t = ptr::null_mut();
        let mut keys: *mut ustore_key_t = ptr::null_mut();
        let mut error = ustore_error_t::default();
        let mut arguments = ffi::ustore_scan_t {
            db: self.database(),
            error: &mut error,
            transaction: self.transaction(),
            snapshot: self.snapshot(),
            arena: arena.slot(),
            options: self.options(),
            tasks_count: 1,
            collections: &collection.0,
            collections_stride: 0,
            start_keys: &start,
            start_keys_stride: 0,
            count_limits: &limit,
            count_limits_stride: 0,
            offsets: ptr::null_mut(),
            counts: &mut counts,
            keys: &mut keys,
        };
        // SAFETY: the inputs and the outputs outlive the call.
        unsafe { ffi::ustore_scan(&mut arguments) };
        check(error)?;
        // SAFETY: a successful scan exports one count and that many keys into the arena, which stays
        // borrowed for as long as the slice does.
        Ok(unsafe { exported_slice(keys, first_count(counts)) })
    }

    /// Draws up to `limit` keys uniformly at random from one collection.
    pub fn sample<'arena>(
        &self,
        arena: &'arena mut Arena,
        collection: Collection,
        limit: u32,
    ) -> Result<&'arena [Key]> {
        let mut counts: *mut ustore_length_t = ptr::null_mut();
        let mut keys: *mut ustore_key_t = ptr::null_mut();
        let mut error = ustore_error_t::default();
        let mut arguments = ffi::ustore_sample_t {
            db: self.database(),
            error: &mut error,
            transaction: self.transaction(),
            snapshot: self.snapshot(),
            arena: arena.slot(),
            options: self.options(),
            tasks_count: 1,
            collections: &collection.0,
            collections_stride: 0,
            count_limits: &limit,
            count_limits_stride: 0,
            offsets: ptr::null_mut(),
            counts: &mut counts,
            keys: &mut keys,
        };
        // SAFETY: the inputs and the outputs outlive the call.
        unsafe { ffi::ustore_sample(&mut arguments) };
        check(error)?;
        // SAFETY: a successful sample exports one count and that many keys into the arena, which
        // stays borrowed for as long as the slice does.
        Ok(unsafe { exported_slice(keys, first_count(counts)) })
    }

    /// Estimates the entries and the space a range of keys takes in one collection.
    pub fn measure(&self, arena: &mut Arena, collection: Collection, start: Key, end: Key) -> Result<Measurement> {
        let mut min_entries: *mut ustore_size_t = ptr::null_mut();
        let mut max_entries: *mut ustore_size_t = ptr::null_mut();
        let mut min_value_bytes: *mut ustore_size_t = ptr::null_mut();
        let mut max_value_bytes: *mut ustore_size_t = ptr::null_mut();
        let mut min_space_usages: *mut ustore_size_t = ptr::null_mut();
        let mut max_space_usages: *mut ustore_size_t = ptr::null_mut();
        let mut error = ustore_error_t::default();
        let mut arguments = ffi::ustore_measure_t {
            db: self.database(),
            error: &mut error,
            transaction: self.transaction(),
            snapshot: self.snapshot(),
            arena: arena.slot(),
            options: self.options(),
            tasks_count: 1,
            collections: &collection.0,
            collections_stride: 0,
            start_keys: &start,
            start_keys_stride: 0,
            end_keys: &end,
            end_keys_stride: 0,
            min_cardinalities: &mut min_entries,
            max_cardinalities: &mut max_entries,
            min_value_bytes: &mut min_value_bytes,
            max_value_bytes: &mut max_value_bytes,
            min_space_usages: &mut min_space_usages,
            max_space_usages: &mut max_space_usages,
        };
        // SAFETY: the inputs and the outputs outlive the call.
        unsafe { ffi::ustore_measure(&mut arguments) };
        check(error)?;
        // The estimates are read here, before the call that would overwrite the arena holding them.
        Ok(Measurement {
            min_entries: first_estimate(min_entries),
            max_entries: first_estimate(max_entries),
            min_value_bytes: first_estimate(min_value_bytes),
            max_value_bytes: first_estimate(max_value_bytes),
            min_space_usage: first_estimate(min_space_usages),
            max_space_usage: first_estimate(max_space_usages),
        })
    }
}

/// The estimate one measured column reported for its single range.
fn first_estimate(column: *const ustore_size_t) -> u64 {
    match column.is_null() {
        true => 0,
        // SAFETY: a successful measurement writes one estimate per column into the arena.
        false => unsafe { *column },
    }
}

// endregion: Verbs

// region: Tests

#[cfg(test)]
mod tests {
    use super::*;
    use crate::db::{Collection, Collections, Configuration, Database, Removed, Status};

    /// A database holding `alpha`, `beta` and `gamma` under the keys one, two and three.
    fn populated() -> (Database, Arena) {
        let database = Database::open(Configuration::in_memory()).unwrap();
        let mut arena = Arena::new();
        let entries = [
            Entry::new(1, b"alpha").unwrap(),
            Entry::new(2, b"beta").unwrap(),
            Entry::new(3, b"gamma").unwrap(),
        ];
        database
            .head()
            .write(&mut arena, Collection::main(), &entries, Durability::Buffered)
            .unwrap();
        (database, arena)
    }

    #[test]
    fn batches_write_and_read_back_in_order() {
        let (database, mut arena) = populated();
        let values = database
            .head()
            .read(&mut arena, Collection::main(), &[3, 1, 2])
            .unwrap();
        assert_eq!(values.len(), 3);
        assert!(!values.is_empty());
        let read: Vec<Option<&[u8]>> = values.iter().collect();
        assert_eq!(
            read,
            vec![
                Some(b"gamma".as_slice()),
                Some(b"alpha".as_slice()),
                Some(b"beta".as_slice())
            ]
        );
        // A key past the batch is absent rather than a panic.
        assert_eq!(values.get(3), None);
    }

    #[test]
    fn reads_report_a_missing_key_apart_from_an_empty_value() {
        let database = Database::open(Configuration::in_memory()).unwrap();
        let mut arena = Arena::new();
        let entries = [Entry::new(1, b"").unwrap()];
        database
            .head()
            .write(&mut arena, Collection::main(), &entries, Durability::Buffered)
            .unwrap();

        let values = database.head().read(&mut arena, Collection::main(), &[1, 2]).unwrap();
        assert_eq!(values.get(0), Some(b"".as_slice()), "an empty value is present");
        assert_eq!(values.get(1), None, "a key nobody wrote is missing");
    }

    #[test]
    fn empty_batches_reach_no_engine() {
        let (database, mut arena) = populated();
        database
            .head()
            .write(&mut arena, Collection::main(), &[], Durability::Buffered)
            .unwrap();
        database
            .head()
            .erase(&mut arena, Collection::main(), &[], Durability::Buffered)
            .unwrap();
        let values = database.head().read(&mut arena, Collection::main(), &[]).unwrap();
        assert!(values.is_empty());
        assert_eq!(values.get(0), None);
    }

    #[test]
    fn erasing_removes_every_key_it_names() {
        let (database, mut arena) = populated();
        database
            .head()
            .erase(&mut arena, Collection::main(), &[1, 3], Durability::Buffered)
            .unwrap();
        let values = database
            .head()
            .read(&mut arena, Collection::main(), &[1, 2, 3])
            .unwrap();
        assert_eq!(values.get(0), None);
        assert_eq!(values.get(1), Some(b"beta".as_slice()));
        assert_eq!(values.get(2), None);
    }

    #[test]
    fn entries_borrow_what_they_carry() {
        let value = [1u8, 2, 3];
        let entry = Entry::new(42, &value).unwrap();
        assert_eq!(entry.key(), 42);
        assert_eq!(entry.value(), Some(value.as_slice()));
        assert_eq!(Entry::new(42, b"").unwrap().value(), Some(b"".as_slice()));
        assert_eq!(Entry::removal(42).value(), None);
    }

    #[test]
    fn writes_land_in_the_collection_they_name() {
        let database = Database::open(Configuration::in_memory()).unwrap();
        let mut arena = Arena::new();
        let named = database.create_collection(&mut arena, "people").unwrap();
        let entries = [Entry::new(1, b"in the named one").unwrap()];
        database
            .head()
            .write(&mut arena, named, &entries, Durability::Buffered)
            .unwrap();

        let there = database.head().read(&mut arena, named, &[1]).unwrap();
        assert_eq!(there.get(0), Some(b"in the named one".as_slice()));
        let elsewhere = database.head().read(&mut arena, Collection::main(), &[1]).unwrap();
        assert_eq!(elsewhere.get(0), None);

        // Dropping the collection takes its contents with it.
        database.clear_collection(named, Removed::Entries).unwrap();
        assert_eq!(database.head().read(&mut arena, named, &[1]).unwrap().get(0), None);
    }

    #[test]
    fn scans_walk_in_key_order_and_paginate() {
        let (database, mut arena) = populated();
        let first_page = database.head().scan(&mut arena, Collection::main(), 0, 2).unwrap();
        assert_eq!(first_page, &[1, 2]);

        let resume = first_page[first_page.len() - 1] + 1;
        let second_page = database.head().scan(&mut arena, Collection::main(), resume, 2).unwrap();
        assert_eq!(second_page, &[3]);

        // A walk starting past the last key finds nothing.
        assert!(database
            .head()
            .scan(&mut arena, Collection::main(), 99, 2)
            .unwrap()
            .is_empty());
    }

    #[test]
    fn scans_refuse_a_limit_of_nothing_to_walk() {
        let (database, mut arena) = populated();
        let found = database.head().scan(&mut arena, Collection::main(), 0, 0).unwrap();
        assert!(found.is_empty(), "a zero limit walks nothing");
    }

    #[test]
    fn samples_draw_from_what_the_collection_holds() {
        let (database, mut arena) = populated();
        let drawn = database.head().sample(&mut arena, Collection::main(), 2).unwrap();
        assert_eq!(drawn.len(), 2);
        for key in drawn {
            assert!((1..=3).contains(key), "{key} is not one of the written keys");
        }
    }

    #[test]
    fn measurements_bound_what_a_range_holds() {
        let (database, mut arena) = populated();
        let measured = database.head().measure(&mut arena, Collection::main(), 1, 3).unwrap();
        assert!(measured.min_entries <= 3 && measured.max_entries >= measured.min_entries);
        assert!(measured.max_value_bytes >= measured.min_value_bytes);
        assert!(measured.max_space_usage >= measured.min_space_usage);
    }

    #[test]
    fn one_batch_spans_several_collections() {
        let database = Database::open(Configuration::in_memory()).unwrap();
        let mut arena = Arena::new();
        let people = database.create_collection(&mut arena, "people").unwrap();
        let places = database.create_collection(&mut arena, "places").unwrap();

        let entries = [Entry::new(1, b"Ada").unwrap(), Entry::new(1, b"Kent").unwrap()];
        let written = [people, places];
        database
            .head()
            .write(&mut arena, written.as_slice(), &entries, Durability::Buffered)
            .unwrap();

        let values = database
            .head()
            .read(&mut arena, Collections::Each(&written), &[1, 1])
            .unwrap();
        assert_eq!(values.get(0), Some(b"Ada".as_slice()));
        assert_eq!(values.get(1), Some(b"Kent".as_slice()));

        // One collection for the whole batch is the same call with the ergonomic argument.
        let inside = database.head().read(&mut arena, people, &[1]).unwrap();
        assert_eq!(inside.get(0), Some(b"Ada".as_slice()));

        database
            .head()
            .erase(&mut arena, written.as_slice(), &[1, 1], Durability::Buffered)
            .unwrap();
        let erased = database
            .head()
            .read(&mut arena, Collections::Each(&written), &[1, 1])
            .unwrap();
        assert_eq!((erased.get(0), erased.get(1)), (None, None));
    }

    #[test]
    fn a_batch_of_collections_matches_the_batch_of_tasks() {
        let (database, mut arena) = populated();
        let one = [Collection::main()];
        let failure = database
            .head()
            .read(&mut arena, Collections::Each(&one), &[1, 2])
            .unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);

        let entries = [Entry::new(1, b"alpha").unwrap(), Entry::new(2, b"beta").unwrap()];
        let failure = database
            .head()
            .write(&mut arena, Collections::Each(&one), &entries, Durability::Buffered)
            .unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);
    }

    #[test]
    fn one_batch_mixes_values_and_removals() {
        let (database, mut arena) = populated();
        let entries = [
            Entry::new(1, b"rewritten").unwrap(),
            Entry::removal(2),
            Entry::new(4, b"").unwrap(),
        ];
        database
            .head()
            .write(&mut arena, Collection::main(), &entries, Durability::Buffered)
            .unwrap();

        let values = database
            .head()
            .read(&mut arena, Collection::main(), &[1, 2, 3, 4])
            .unwrap();
        assert_eq!(values.get(0), Some(b"rewritten".as_slice()));
        assert_eq!(values.get(1), None, "an entry without a value removes its key");
        assert_eq!(values.get(2), Some(b"gamma".as_slice()));
        assert_eq!(values.get(3), Some(b"".as_slice()), "an empty value is not a removal");
    }

    #[test]
    fn an_unknown_collection_reads_as_empty_and_drops_as_missing() {
        let (database, mut arena) = populated();
        // A read addresses a window of keys, so an identifier nobody created simply holds nothing.
        let values = database.head().read(&mut arena, Collection(4242), &[1]).unwrap();
        assert_eq!(values.get(0), None);

        // Dropping one changes the catalog, which refuses an identifier it never handed out.
        let failure = database
            .clear_collection(Collection(4242), Removed::CollectionAndEntries)
            .unwrap_err();
        assert_eq!(failure.status, Status::NotFound);
    }
}

// endregion: Tests
