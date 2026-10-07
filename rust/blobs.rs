//! Binary values - the Rust twin of `include/ustore/blobs.h`.
//!
//! Every verb takes a batch, and one operation is a batch of one. Inputs are borrowed slices the
//! strided ABI reads in place, so nothing is staged on the way in; outputs land in the [`Arena`] the
//! call borrows, so nothing is copied on the way out.

use std::ffi::c_void;
use std::marker::PhantomData;
use std::ptr;

use crate::db::ffi::{
    ustore_arena_t, ustore_byte_t, ustore_bytes_cptr_t, ustore_collection_id_t, ustore_completion_t, ustore_database_t,
    ustore_durability_t, ustore_executor_t, ustore_key_t, ustore_length_t, ustore_octet_t, ustore_outcome_t,
    ustore_progress_callback_t, ustore_scan_mode_t, ustore_size_t, ustore_transaction_t, ustore_view_t, ustore_watch_t,
};
use crate::db::{
    check, described_length, exported_slice, fail, grown, Arena, CollectionID, Collections, Durability, Key, Result,
    Rows, Status, View,
};
use crate::docs::Bitmap;

// region: Raw Declarations

/// One-to-one mirror of `include/ustore/blobs.h`.
mod ffi {
    #![allow(non_camel_case_types)]

    use super::*;

    #[repr(C)]
    pub struct ustore_write_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub transaction: ustore_transaction_t,
        pub count: ustore_size_t,
        pub collections: *const ustore_collection_id_t,
        pub collections_stride: ustore_size_t,
        pub keys: *const ustore_key_t,
        pub keys_stride: ustore_size_t,
        pub starts: *const ustore_length_t,
        pub starts_stride: ustore_size_t,
        pub lengths: *const ustore_length_t,
        pub lengths_stride: ustore_size_t,
        pub values: *const ustore_bytes_cptr_t,
        pub values_stride: ustore_size_t,
        pub durability: ustore_durability_t,
        pub watch: ustore_watch_t,
    }

    #[repr(C)]
    pub struct ustore_remove_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub transaction: ustore_transaction_t,
        pub count: ustore_size_t,
        pub collections: *const ustore_collection_id_t,
        pub collections_stride: ustore_size_t,
        pub keys: *const ustore_key_t,
        pub keys_stride: ustore_size_t,
        pub durability: ustore_durability_t,
        pub watch: ustore_watch_t,
    }

    #[repr(C)]
    pub struct ustore_read_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub view: ustore_view_t,
        pub count: ustore_size_t,
        pub collections: *const ustore_collection_id_t,
        pub collections_stride: ustore_size_t,
        pub keys: *const ustore_key_t,
        pub keys_stride: ustore_size_t,
        pub presences: *mut ustore_octet_t,
        pub offsets: *mut ustore_length_t,
        pub values: *mut *mut ustore_byte_t,
        pub arena: *mut ustore_arena_t,
    }

    #[repr(C)]
    pub struct ustore_scan_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub view: ustore_view_t,
        pub count: ustore_size_t,
        pub collections: *const ustore_collection_id_t,
        pub collections_stride: ustore_size_t,
        pub start_keys: *const ustore_key_t,
        pub start_keys_stride: ustore_size_t,
        pub count_limits: *const ustore_length_t,
        pub count_limits_stride: ustore_size_t,
        pub offsets: *mut ustore_length_t,
        pub keys: *mut ustore_key_t,
        pub mode: ustore_scan_mode_t,
        pub progress: ustore_progress_callback_t,
        pub progress_state: *mut c_void,
    }

    #[repr(C)]
    pub struct ustore_sample_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub view: ustore_view_t,
        pub count: ustore_size_t,
        pub collections: *const ustore_collection_id_t,
        pub collections_stride: ustore_size_t,
        pub count_limits: *const ustore_length_t,
        pub count_limits_stride: ustore_size_t,
        pub offsets: *mut ustore_length_t,
        pub keys: *mut ustore_key_t,
    }

    #[repr(C)]
    pub struct ustore_measure_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub view: ustore_view_t,
        pub count: ustore_size_t,
        pub collections: *const ustore_collection_id_t,
        pub collections_stride: ustore_size_t,
        pub start_keys: *const ustore_key_t,
        pub start_keys_stride: ustore_size_t,
        pub end_keys: *const ustore_key_t,
        pub end_keys_stride: ustore_size_t,
        pub min_cardinalities: *mut ustore_size_t,
        pub min_cardinalities_stride: ustore_size_t,
        pub max_cardinalities: *mut ustore_size_t,
        pub max_cardinalities_stride: ustore_size_t,
        pub min_value_bytes: *mut ustore_size_t,
        pub min_value_bytes_stride: ustore_size_t,
        pub max_value_bytes: *mut ustore_size_t,
        pub max_value_bytes_stride: ustore_size_t,
        pub min_space_usages: *mut ustore_size_t,
        pub min_space_usages_stride: ustore_size_t,
        pub max_space_usages: *mut ustore_size_t,
        pub max_space_usages_stride: ustore_size_t,
    }

    extern "C" {
        pub fn ustore_write(arguments: *mut ustore_write_t);
        pub fn ustore_remove(arguments: *mut ustore_remove_t);
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
    /// Pairs a key with the bytes to store under it, refusing a value whose length the ABI reserves for
    /// a missing one.
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

    /// The bytes this entry stores.
    pub fn value(&self) -> &'value [u8] {
        // SAFETY: the pointer and the length came from one borrowed slice alive for `'value`.
        unsafe { exported_slice(self.value, self.length as usize) }
    }
}

// endregion: Inputs

// region: Outputs

/// The values one read exported, addressed by the position of their key in the batch.
///
/// The presences and the offsets live in the arena's spans and the bytes behind its handle, so the run borrows the
/// arena, and the next call through it cannot overwrite them while the run lives.
#[derive(Clone, Copy, Debug)]
pub struct Values<'arena> {
    presences: &'arena [ustore_octet_t],
    offsets: &'arena [ustore_length_t],
    tape: &'arena [ustore_byte_t],
}

impl<'arena> Values<'arena> {
    /// The run of a read that covered no keys.
    pub(crate) fn empty() -> Self {
        Values {
            presences: &[],
            offsets: &[0],
            tape: &[],
        }
    }

    /// The run a read exported: a presence bit per key, one more offset than keys, and the bytes they address.
    ///
    /// # Safety
    /// `offsets` is not empty and never decreases, and `tape` addresses the bytes up to the last of them, alive for
    /// `'arena`.
    pub(crate) unsafe fn exported(
        presences: &'arena [ustore_octet_t],
        offsets: &'arena [ustore_length_t],
        tape: *const ustore_byte_t,
    ) -> Self {
        let length = offsets[offsets.len() - 1] as usize;
        Values {
            presences,
            offsets,
            tape: exported_slice(tape, length),
        }
    }

    /// How many keys the read covered.
    pub fn len(&self) -> usize {
        self.offsets.len() - 1
    }

    /// Whether the read covered no keys at all.
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// The value of the key at `index`, or `None` for a key the database does not hold.
    pub fn get(&self, index: usize) -> Option<&'arena [u8]> {
        if !self.presences().get(index) {
            return None;
        }
        let begin = self.offsets[index] as usize;
        let end = self.offsets[index + 1] as usize;
        self.tape.get(begin..end)
    }

    /// Every value in the order its key was asked for.
    pub fn iter(&self) -> Rows<Values<'arena>, Option<&'arena [u8]>> {
        Rows::new(*self, self.len(), Values::get)
    }

    /// Which keys the read found, one bit per key as Arrow reads a validity buffer.
    pub fn presences(&self) -> Bitmap<'arena> {
        Bitmap::new(self.presences, self.len())
    }

    /// Every value on one Arrow-style tape of `len() + 1` offsets into the bytes the read exported, without a copy.
    ///
    /// A missing value is empty on the tape, and [`Values::presences`] tells it apart from an empty one.
    pub fn tape(&self) -> stringtape::BytesTapeViewU64<'arena> {
        // SAFETY: the offsets never decrease, and the last one closes the tape of bytes they address.
        unsafe { stringtape::BytesTapeViewU64::from_raw_parts(self.tape, self.offsets) }
    }
}

impl<'arena> IntoIterator for Values<'arena> {
    type Item = Option<&'arena [u8]>;
    type IntoIter = Rows<Values<'arena>, Option<&'arena [u8]>>;

    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}

impl<'arena> IntoIterator for &Values<'arena> {
    type Item = Option<&'arena [u8]>;
    type IntoIter = Rows<Values<'arena>, Option<&'arena [u8]>>;

    fn into_iter(self) -> Self::IntoIter {
        self.iter()
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
    /// Upserts a batch of values, where an empty value is still a value and [`View::remove`] removes keys.
    ///
    /// The batch spans as many collections as it has entries, and a bare [`CollectionID`] gives every
    /// entry the same one. A transaction flushes at its commit, so [`Durability::Flushed`] is refused
    /// inside one.
    pub fn write<'collections>(
        &self,
        collections: impl Into<Collections<'collections>>,
        entries: &[Entry<'_>],
        durability: Durability,
    ) -> Result<()> {
        let transaction = self.transaction()?;
        let Some(first) = entries.first() else {
            return Ok(());
        };
        let collections = collections.into();
        let (collections, collections_stride) = collections.strided(entries.len())?;
        let stride = size_of::<Entry<'_>>() as ustore_size_t;
        let mut outcome = ustore_outcome_t::default();
        let mut arguments = ffi::ustore_write_t {
            database: self.database(),
            executor: ptr::null_mut(),
            outcome: &mut outcome,
            completion: ustore_completion_t::Blocking,
            transaction,
            count: entries.len() as ustore_size_t,
            collections,
            collections_stride,
            keys: &first.key,
            keys_stride: stride,
            starts: ptr::null(),
            starts_stride: 0,
            lengths: &first.length,
            lengths_stride: stride,
            values: &first.value,
            values_stride: stride,
            durability,
            watch: self.watch(),
        };
        // SAFETY: the entries and the collections outlive the call, and the members of every entry sit
        // `stride` bytes apart, which is what `#[repr(C)]` on `Entry` promises.
        unsafe { ffi::ustore_write(&mut arguments) };
        check(outcome)
    }

    /// Removes a batch of keys with their values, whether or not the database holds them.
    pub fn remove<'collections>(
        &self,
        collections: impl Into<Collections<'collections>>,
        keys: &[Key],
        durability: Durability,
    ) -> Result<()> {
        let transaction = self.transaction()?;
        if keys.is_empty() {
            return Ok(());
        }
        let collections = collections.into();
        let (collections, collections_stride) = collections.strided(keys.len())?;
        let mut outcome = ustore_outcome_t::default();
        let mut arguments = ffi::ustore_remove_t {
            database: self.database(),
            executor: ptr::null_mut(),
            outcome: &mut outcome,
            completion: ustore_completion_t::Blocking,
            transaction,
            count: keys.len() as ustore_size_t,
            collections,
            collections_stride,
            keys: keys.as_ptr(),
            keys_stride: size_of::<Key>() as ustore_size_t,
            durability,
            watch: self.watch(),
        };
        // SAFETY: the keys and the collections outlive the call.
        unsafe { ffi::ustore_remove(&mut arguments) };
        check(outcome)
    }

    /// Reads a batch of keys into the arena, one collection per key or one for all of them.
    pub fn read<'arena, 'collections>(
        &self,
        arena: &'arena mut Arena,
        collections: impl Into<Collections<'collections>>,
        keys: &[Key],
    ) -> Result<Values<'arena>> {
        if keys.is_empty() {
            return Ok(Values::empty());
        }
        let collections = collections.into();
        let (collections, collections_stride) = collections.strided(keys.len())?;
        let Arena {
            handle,
            presences,
            offsets,
            ..
        } = arena;
        let presences = grown(presences, keys.len().div_ceil(8))?;
        let offsets = grown(offsets, keys.len() + 1)?;
        let mut tape: *mut ustore_byte_t = ptr::null_mut();
        let mut outcome = ustore_outcome_t::default();
        let mut arguments = ffi::ustore_read_t {
            database: self.database(),
            executor: ptr::null_mut(),
            outcome: &mut outcome,
            completion: ustore_completion_t::Blocking,
            view: self.view(),
            count: keys.len() as ustore_size_t,
            collections,
            collections_stride,
            keys: keys.as_ptr(),
            keys_stride: size_of::<Key>() as ustore_size_t,
            presences: presences.as_mut_ptr(),
            offsets: offsets.as_mut_ptr(),
            values: &mut tape,
            arena: handle,
        };
        // SAFETY: the keys, the collections and the outputs outlive the call.
        unsafe { ffi::ustore_read(&mut arguments) };
        check(outcome)?;
        // SAFETY: a successful read fills a presence bit and an offset per key, closes the tape with one more
        // offset, and lands the tape behind the handle, all of it borrowed for `'arena`.
        Ok(unsafe { Values::exported(presences, offsets, tape) })
    }

    /// Which of `keys` one collection holds, exporting no value.
    pub(crate) fn contains<'arena>(
        &self,
        arena: &'arena mut Arena,
        collection: CollectionID,
        keys: &[Key],
    ) -> Result<Bitmap<'arena>> {
        let presences = grown(&mut arena.presences, keys.len().div_ceil(8))?;
        let mut outcome = ustore_outcome_t::default();
        let mut arguments = ffi::ustore_read_t {
            database: self.database(),
            executor: ptr::null_mut(),
            outcome: &mut outcome,
            completion: ustore_completion_t::Blocking,
            view: self.view(),
            count: keys.len() as ustore_size_t,
            collections: &collection,
            collections_stride: 0,
            keys: keys.as_ptr(),
            keys_stride: size_of::<Key>() as ustore_size_t,
            presences: presences.as_mut_ptr(),
            offsets: ptr::null_mut(),
            values: ptr::null_mut(),
            arena: ptr::null_mut(),
        };
        // SAFETY: the keys, the collection and the output outlive the call, which exports nothing to an arena.
        unsafe { ffi::ustore_read(&mut arguments) };
        check(outcome)?;
        Ok(Bitmap::new(presences, keys.len()))
    }

    /// Lists up to `limit` keys of one collection from `start`, which is how a walk paginates.
    pub fn scan<'arena>(
        &self,
        arena: &'arena mut Arena,
        collection: CollectionID,
        start: Key,
        limit: u64,
    ) -> Result<&'arena [Key]> {
        let (offsets, keys) = found_spans(arena, limit)?;
        let mut outcome = ustore_outcome_t::default();
        let mut arguments = ffi::ustore_scan_t {
            database: self.database(),
            executor: ptr::null_mut(),
            outcome: &mut outcome,
            completion: ustore_completion_t::Blocking,
            view: self.view(),
            count: 1,
            collections: &collection,
            collections_stride: 0,
            start_keys: &start,
            start_keys_stride: 0,
            count_limits: &limit,
            count_limits_stride: 0,
            offsets: offsets.as_mut_ptr(),
            keys: keys.as_mut_ptr(),
            mode: ustore_scan_mode_t::Ordered,
            progress: None,
            progress_state: ptr::null_mut(),
        };
        // SAFETY: the inputs and the outputs outlive the call.
        unsafe { ffi::ustore_scan(&mut arguments) };
        check(outcome)?;
        Ok(found(offsets, keys))
    }

    /// Draws up to `limit` keys uniformly at random from one collection.
    pub fn sample<'arena>(
        &self,
        arena: &'arena mut Arena,
        collection: CollectionID,
        limit: u64,
    ) -> Result<&'arena [Key]> {
        let (offsets, keys) = found_spans(arena, limit)?;
        let mut outcome = ustore_outcome_t::default();
        let mut arguments = ffi::ustore_sample_t {
            database: self.database(),
            executor: ptr::null_mut(),
            outcome: &mut outcome,
            completion: ustore_completion_t::Blocking,
            view: self.view(),
            count: 1,
            collections: &collection,
            collections_stride: 0,
            count_limits: &limit,
            count_limits_stride: 0,
            offsets: offsets.as_mut_ptr(),
            keys: keys.as_mut_ptr(),
        };
        // SAFETY: the inputs and the outputs outlive the call.
        unsafe { ffi::ustore_sample(&mut arguments) };
        check(outcome)?;
        Ok(found(offsets, keys))
    }

    /// Estimates the entries and the space a range of keys takes in one collection.
    pub fn measure(&self, collection: CollectionID, start: Key, end: Key) -> Result<Measurement> {
        let mut measured = Measurement::default();
        let mut outcome = ustore_outcome_t::default();
        let mut arguments = ffi::ustore_measure_t {
            database: self.database(),
            executor: ptr::null_mut(),
            outcome: &mut outcome,
            completion: ustore_completion_t::Blocking,
            view: self.view(),
            count: 1,
            collections: &collection,
            collections_stride: 0,
            start_keys: &start,
            start_keys_stride: 0,
            end_keys: &end,
            end_keys_stride: 0,
            min_cardinalities: &mut measured.min_entries,
            min_cardinalities_stride: size_of::<u64>() as ustore_size_t,
            max_cardinalities: &mut measured.max_entries,
            max_cardinalities_stride: size_of::<u64>() as ustore_size_t,
            min_value_bytes: &mut measured.min_value_bytes,
            min_value_bytes_stride: size_of::<u64>() as ustore_size_t,
            max_value_bytes: &mut measured.max_value_bytes,
            max_value_bytes_stride: size_of::<u64>() as ustore_size_t,
            min_space_usages: &mut measured.min_space_usage,
            min_space_usages_stride: size_of::<u64>() as ustore_size_t,
            max_space_usages: &mut measured.max_space_usage,
            max_space_usages_stride: size_of::<u64>() as ustore_size_t,
        };
        // SAFETY: the inputs and the outputs outlive the call.
        unsafe { ffi::ustore_measure(&mut arguments) };
        check(outcome)?;
        Ok(measured)
    }
}

/// The two offsets and the `limit` keys one scan or sample fills, carved from the arena's spans.
fn found_spans(arena: &mut Arena, limit: u64) -> Result<(&mut [ustore_length_t], &mut [Key])> {
    let limit =
        usize::try_from(limit).map_err(|_| fail(Status::OutOfMemory, "the limit outsizes the address space"))?;
    let Arena { offsets, keys, .. } = arena;
    Ok((grown(offsets, 2)?, grown(keys, limit)?))
}

/// The keys one scan or sample found, between the two offsets it filled.
fn found<'arena>(offsets: &[ustore_length_t], keys: &'arena [Key]) -> &'arena [Key] {
    &keys[offsets[0] as usize..offsets[1] as usize]
}

// endregion: Verbs

// region: Tests

#[cfg(test)]
mod tests {
    use super::*;
    use crate::db::{CollectionID, Collections, Database, Modality, RAMOptions, Removed, Status};
    use crate::testing::{key, keys};

    /// A database holding `alpha`, `beta` and `gamma` under the keys one, two and three.
    fn populated() -> (Database, Arena) {
        let database = Database::open_ram(&RAMOptions::default()).unwrap();
        let entries = [
            Entry::new(key(1), b"alpha").unwrap(),
            Entry::new(key(2), b"beta").unwrap(),
            Entry::new(key(3), b"gamma").unwrap(),
        ];
        database
            .head()
            .write(CollectionID::main(), &entries, Durability::Buffered)
            .unwrap();
        (database, Arena::new())
    }

    #[test]
    fn batches_write_and_read_back_in_order() {
        let (database, mut arena) = populated();
        let values = database
            .head()
            .read(&mut arena, CollectionID::main(), &keys([3, 1, 2]))
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
        // The rows also walk backwards, know their count, and serve a `for` loop over a borrow.
        assert_eq!(values.iter().len(), 3);
        assert_eq!(values.iter().next_back(), Some(Some(b"beta".as_slice())));
        let mut lengths = 0;
        for value in &values {
            lengths += value.map_or(0, <[u8]>::len);
        }
        assert_eq!(lengths, "gammaalphabeta".len());
    }

    #[test]
    fn reads_report_a_missing_key_apart_from_an_empty_value() {
        let database = Database::open_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new();
        let entries = [Entry::new(key(1), b"").unwrap()];
        database
            .head()
            .write(CollectionID::main(), &entries, Durability::Buffered)
            .unwrap();

        let values = database
            .head()
            .read(&mut arena, CollectionID::main(), &keys([1, 2]))
            .unwrap();
        assert_eq!(values.get(0), Some(b"".as_slice()), "an empty value is present");
        assert_eq!(values.get(1), None, "a key nobody wrote is missing");
    }

    #[test]
    fn empty_batches_reach_no_engine() {
        let (database, mut arena) = populated();
        database
            .head()
            .write(CollectionID::main(), &[], Durability::Buffered)
            .unwrap();
        database
            .head()
            .remove(CollectionID::main(), &[], Durability::Buffered)
            .unwrap();
        let values = database.head().read(&mut arena, CollectionID::main(), &[]).unwrap();
        assert!(values.is_empty());
        assert_eq!(values.get(0), None);
        assert!(values.tape().is_empty() && values.presences().is_empty());
    }

    #[test]
    fn removals_take_every_key_they_name() {
        let (database, mut arena) = populated();
        database
            .head()
            .remove(CollectionID::main(), &keys([1, 3]), Durability::Buffered)
            .unwrap();
        let values = database
            .head()
            .read(&mut arena, CollectionID::main(), &keys([1, 2, 3]))
            .unwrap();
        assert_eq!(values.get(0), None);
        assert_eq!(values.get(1), Some(b"beta".as_slice()));
        assert_eq!(values.get(2), None);
    }

    #[test]
    fn entries_borrow_what_they_carry() {
        let value = [1u8, 2, 3];
        let entry = Entry::new(key(42), &value).unwrap();
        assert_eq!(entry.key(), key(42));
        assert_eq!(entry.value(), value.as_slice());
        assert_eq!(Entry::new(key(42), b"").unwrap().value(), b"".as_slice());
    }

    #[test]
    fn writes_land_in_the_collection_they_name() {
        let database = Database::open_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new();
        let named = database.create_collection("people", Modality::Any).unwrap();
        let entries = [Entry::new(key(1), b"in the named one").unwrap()];
        database.head().write(named, &entries, Durability::Buffered).unwrap();

        let there = database.head().read(&mut arena, named, &keys([1])).unwrap();
        assert_eq!(there.get(0), Some(b"in the named one".as_slice()));
        let elsewhere = database
            .head()
            .read(&mut arena, CollectionID::main(), &keys([1]))
            .unwrap();
        assert_eq!(elsewhere.get(0), None);

        // Dropping the collection takes its contents with it.
        database.clear_collection(named, Removed::Entries).unwrap();
        assert_eq!(
            database.head().read(&mut arena, named, &keys([1])).unwrap().get(0),
            None
        );
    }

    #[test]
    fn scans_walk_in_key_order_and_paginate() {
        let (database, mut arena) = populated();
        let first_page = database
            .head()
            .scan(&mut arena, CollectionID::main(), key(0), 2)
            .unwrap();
        assert_eq!(first_page, &keys([1, 2]));

        let resume = first_page[first_page.len() - 1].next();
        let second_page = database
            .head()
            .scan(&mut arena, CollectionID::main(), resume, 2)
            .unwrap();
        assert_eq!(second_page, &keys([3]));

        // A walk starting past the last key finds nothing.
        assert!(database
            .head()
            .scan(&mut arena, CollectionID::main(), key(99), 2)
            .unwrap()
            .is_empty());
    }

    #[test]
    fn scans_refuse_a_limit_of_nothing_to_walk() {
        let (database, mut arena) = populated();
        let found = database
            .head()
            .scan(&mut arena, CollectionID::main(), key(0), 0)
            .unwrap();
        assert!(found.is_empty(), "a zero limit walks nothing");
    }

    #[test]
    fn samples_draw_from_what_the_collection_holds() {
        let (database, mut arena) = populated();
        let drawn = database.head().sample(&mut arena, CollectionID::main(), 2).unwrap();
        assert_eq!(drawn.len(), 2);
        for drawn_key in drawn {
            assert!(
                (key(1)..=key(3)).contains(drawn_key),
                "{drawn_key} is not one of the written keys"
            );
        }
    }

    #[test]
    fn measurements_bound_what_a_range_holds() {
        let (database, _) = populated();
        let measured = database.head().measure(CollectionID::main(), key(1), key(3)).unwrap();
        assert!(measured.min_entries <= 3 && measured.max_entries >= measured.min_entries);
        assert!(measured.max_value_bytes >= measured.min_value_bytes);
        assert!(measured.max_space_usage >= measured.min_space_usage);
    }

    #[test]
    fn one_batch_spans_several_collections() {
        let database = Database::open_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new();
        let people = database.create_collection("people", Modality::Any).unwrap();
        let places = database.create_collection("places", Modality::Any).unwrap();

        let entries = [
            Entry::new(key(1), b"Ada").unwrap(),
            Entry::new(key(1), b"Kent").unwrap(),
        ];
        let written = [people, places];
        database
            .head()
            .write(written.as_slice(), &entries, Durability::Buffered)
            .unwrap();

        let values = database
            .head()
            .read(&mut arena, Collections::Each(&written), &keys([1, 1]))
            .unwrap();
        assert_eq!(values.get(0), Some(b"Ada".as_slice()));
        assert_eq!(values.get(1), Some(b"Kent".as_slice()));

        // One collection for the whole batch is the same call with the ergonomic argument.
        let inside = database.head().read(&mut arena, people, &keys([1])).unwrap();
        assert_eq!(inside.get(0), Some(b"Ada".as_slice()));

        database
            .head()
            .remove(written.as_slice(), &keys([1, 1]), Durability::Buffered)
            .unwrap();
        let removed = database
            .head()
            .read(&mut arena, Collections::Each(&written), &keys([1, 1]))
            .unwrap();
        assert_eq!((removed.get(0), removed.get(1)), (None, None));
    }

    #[test]
    fn a_batch_of_collections_matches_the_batch_of_tasks() {
        let (database, mut arena) = populated();
        let one = [CollectionID::main()];
        let failure = database
            .head()
            .read(&mut arena, Collections::Each(&one), &keys([1, 2]))
            .unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);

        let entries = [
            Entry::new(key(1), b"alpha").unwrap(),
            Entry::new(key(2), b"beta").unwrap(),
        ];
        let failure = database
            .head()
            .write(Collections::Each(&one), &entries, Durability::Buffered)
            .unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);
    }

    #[test]
    fn an_unknown_collection_is_refused() {
        let (database, mut arena) = populated();
        // Every call resolves its collections first, so an identifier nobody created reaches nothing.
        let failure = database
            .head()
            .read(&mut arena, CollectionID::from(4242u64), &keys([1]))
            .unwrap_err();
        assert_eq!(failure.status, Status::NotFound);

        let failure = database
            .clear_collection(CollectionID::from(4242u64), Removed::CollectionAndEntries)
            .unwrap_err();
        assert_eq!(failure.status, Status::NotFound);
    }
}

// endregion: Tests
