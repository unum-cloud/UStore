//! Binary values - the Rust twin of `c/ustore/blobs.h`.
//!
//! Every verb takes a batch, and one operation is a batch of one. Keys cross as strided runs the ABI reads in place,
//! and values as a tape or through an accessor over the entries carrying them, so nothing is staged on the way in;
//! outputs land in the [`Arena`] the call borrows, so nothing is copied on the way out. A blocking view takes borrowed
//! or owned inputs, while a deferred one takes only owned ones, and both borrow the arena.

use std::ffi::c_void;
use std::marker::PhantomData;
use std::ptr;

use stringtape::BytesTape;

use crate::db::ffi::{
    ustore_arena_t, ustore_bytes_view_t, ustore_collection_id_t, ustore_collections_view_t, ustore_completion_t,
    ustore_database_t, ustore_durability_t, ustore_executor_t, ustore_key_t, ustore_keys_view_t, ustore_length_t,
    ustore_octet_t, ustore_outcome_t, ustore_progress_callback_t, ustore_scan_mode_t, ustore_size_t,
    ustore_strided_view_t, ustore_strings_view_t, ustore_transaction_t, ustore_view_t, ustore_watch_t,
};
use crate::db::sealed::{Accepts, Calling, TapeOffset, Writable};
use crate::db::{
    check, exported_slice, fail, Address, Arena, CollectionID, Collections, Durability, Key, Result, Rows, Status,
    Strings, Tape, View,
};
use crate::docs::Bitmap;

// region: Raw Declarations

/// One-to-one mirror of `c/ustore/blobs.h`.
mod ffi {
    #![allow(non_camel_case_types)]

    use super::*;

    #[repr(C)]
    pub struct ustore_blobs_write_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub transaction: ustore_transaction_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub keys: ustore_keys_view_t,
        pub values: ustore_strings_view_t,
        pub durability: ustore_durability_t,
        pub watch: ustore_watch_t,
    }

    #[repr(C)]
    pub struct ustore_blobs_remove_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub transaction: ustore_transaction_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub keys: ustore_keys_view_t,
        pub durability: ustore_durability_t,
        pub watch: ustore_watch_t,
    }

    #[repr(C)]
    pub struct ustore_blobs_read_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub view: ustore_view_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub keys: ustore_keys_view_t,
        pub presences: *mut *const ustore_octet_t,
        pub lengths: *mut *const ustore_length_t,
        pub values: *mut ustore_strings_view_t,
        pub arena: ustore_arena_t,
    }

    #[repr(C)]
    pub struct ustore_blobs_scan_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub view: ustore_view_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub start_keys: ustore_keys_view_t,
        pub offsets: *mut *const ustore_length_t,
        pub keys: *mut *const ustore_key_t,
        pub arena: ustore_arena_t,
        pub wanted: ustore_size_t,
        pub mode: ustore_scan_mode_t,
        pub progress: ustore_progress_callback_t,
        pub progress_state: *mut c_void,
    }

    #[repr(C)]
    pub struct ustore_blobs_cursor_handle_t {
        _private: [u8; 0],
    }

    pub type ustore_blobs_cursor_t = *mut ustore_blobs_cursor_handle_t;

    #[repr(C)]
    pub struct ustore_blobs_cursor_init_t {
        pub cursor: *mut ustore_blobs_cursor_t,
        pub database: ustore_database_t,
        pub outcome: *mut ustore_outcome_t,
        pub view: ustore_view_t,
        pub collection: ustore_collection_id_t,
        pub start_key: ustore_key_t,
    }

    #[repr(C)]
    pub struct ustore_blobs_cursor_next_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub cursor: ustore_blobs_cursor_t,
        pub keys: *mut *const ustore_key_t,
        pub values: *mut ustore_strings_view_t,
        pub written: *mut ustore_size_t,
        pub arena: ustore_arena_t,
        pub wanted: ustore_size_t,
        pub progress: ustore_progress_callback_t,
        pub progress_state: *mut c_void,
    }

    #[repr(C)]
    pub struct ustore_blobs_sample_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub view: ustore_view_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub offsets: *mut *const ustore_length_t,
        pub keys: *mut *const ustore_key_t,
        pub arena: ustore_arena_t,
        pub wanted: ustore_size_t,
    }

    #[repr(C)]
    pub struct ustore_blobs_measure_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub view: ustore_view_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub start_keys: ustore_keys_view_t,
        pub end_keys: ustore_keys_view_t,
        pub min_cardinalities: *mut *const ustore_size_t,
        pub max_cardinalities: *mut *const ustore_size_t,
        pub min_value_bytes: *mut *const ustore_size_t,
        pub max_value_bytes: *mut *const ustore_size_t,
        pub min_space_usages: *mut *const ustore_size_t,
        pub max_space_usages: *mut *const ustore_size_t,
        pub arena: ustore_arena_t,
    }

    extern "C" {
        pub fn ustore_blobs_write(arguments: *mut ustore_blobs_write_t);
        pub fn ustore_blobs_remove(arguments: *mut ustore_blobs_remove_t);
        pub fn ustore_blobs_read(arguments: *mut ustore_blobs_read_t);
        pub fn ustore_blobs_scan(arguments: *mut ustore_blobs_scan_t);
        pub fn ustore_blobs_cursor_init(arguments: *mut ustore_blobs_cursor_init_t);
        pub fn ustore_blobs_cursor_next(arguments: *mut ustore_blobs_cursor_next_t);
        pub fn ustore_blobs_cursor_free(cursor: ustore_blobs_cursor_t);
        pub fn ustore_blobs_sample(arguments: *mut ustore_blobs_sample_t);
        pub fn ustore_blobs_measure(arguments: *mut ustore_blobs_measure_t);
    }
}

// endregion: Raw Declarations

// region: Inputs

/// One key and the bytes stored under it, laid out so a slice of them crosses as it stands: the keys as one strided
/// run, and the values through an accessor over the same entries.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct Entry<'value> {
    key: Key,
    value: ustore_bytes_view_t,
    marker: PhantomData<&'value [u8]>,
}

impl<'value> Entry<'value> {
    /// Pairs a key with the bytes to store under it.
    pub fn new(key: Key, value: &'value [u8]) -> Self {
        Entry {
            key,
            value: value.into(),
            marker: PhantomData,
        }
    }

    /// The key this entry stores under.
    pub fn key(&self) -> Key {
        self.key
    }

    /// The bytes this entry stores.
    pub fn value(&self) -> &'value [u8] {
        // SAFETY: the view came from one borrowed slice alive for `'value`.
        unsafe { exported_slice(self.value.data, self.value.length as usize) }
    }
}

/// Keeps the inputs of a write closed over the forms this module names.
mod sealed {
    use super::*;

    /// The keys and values one write carries: borrowed entries, or owned keys beside an owned tape of their values.
    pub trait Pairs {
        /// The keys and the values of the pairs, pointing into `self`, or `None` for no pairs at all.
        fn pairs(&self) -> Result<Option<PairsView>>;
    }

    /// The keys and the values a write reads its pairs from.
    pub struct PairsView {
        pub count: usize,
        pub keys: ustore_keys_view_t,
        pub values: ustore_strings_view_t,
    }
}

use sealed::{Pairs, PairsView};

impl Pairs for &[Entry<'_>] {
    fn pairs(&self) -> Result<Option<PairsView>> {
        let Some(first) = self.first() else {
            return Ok(None);
        };
        Ok(Some(PairsView {
            count: self.len(),
            keys: ustore_strided_view_t::inside::<Entry<'_>>(&first.key),
            values: ustore_strings_view_t::inside::<Entry<'_>>(&first.value),
        }))
    }
}

impl<const COUNT: usize> Pairs for &[Entry<'_>; COUNT] {
    fn pairs(&self) -> Result<Option<PairsView>> {
        self.as_slice().pairs()
    }
}

/// Keys beside a tape of their values, one more offset than keys into one run of bytes, which a deferred write owns.
impl<Keys: AsRef<[Key]>, Offset: TapeOffset> Pairs for (Keys, BytesTape<Offset>) {
    fn pairs(&self) -> Result<Option<PairsView>> {
        let (keys, tape) = self;
        let keys = keys.as_ref();
        if keys.len() != tape.len() {
            return Err(fail(Status::InvalidArgument, "the tape holds a value per key"));
        }
        if keys.is_empty() {
            return Ok(None);
        }
        let (bytes, offsets) = tape.arrow_slices();
        Ok(Some(PairsView {
            count: keys.len(),
            keys: ustore_strided_view_t::packed(keys),
            values: Offset::tape(bytes, offsets),
        }))
    }
}

// endregion: Inputs

// region: Outputs

/// The values one read exported, addressed by the position of their key in the batch.
///
/// The presences, the offsets and the bytes all live in the arena, so the run borrows it, and the next call through it
/// cannot overwrite them while the run lives.
#[derive(Clone, Copy, Debug)]
pub struct Values<'arena> {
    presences: Bitmap<'arena>,
    values: Strings<'arena>,
}

impl<'arena> Values<'arena> {
    /// The run a read of `count` tasks exported into an arena: a presence bit per task, and the tape of the values.
    ///
    /// # Safety
    /// A successful read of `count` tasks exported its presences and the tape `values` into an arena alive for
    /// `'arena`, or `count` is zero.
    pub(crate) unsafe fn exported(
        count: usize,
        presences: Address<ustore_octet_t>,
        values: ustore_strings_view_t,
    ) -> Result<Self> {
        if count == 0 {
            return Ok(Values {
                presences: Bitmap::new(&[], 0),
                values: Strings::empty(),
            });
        }
        Ok(Values {
            presences: Bitmap::new(exported_slice(presences.0, count.div_ceil(8)), count),
            values: Strings::exported(values, count)?,
        })
    }

    /// How many keys the read covered.
    pub fn len(&self) -> usize {
        self.presences.len()
    }

    /// Whether the read covered no keys at all.
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// The value of the key at `index`, or `None` for a key the database does not hold.
    pub fn get(&self, index: usize) -> Option<&'arena [u8]> {
        match self.presences.get(index) {
            true => self.values.get(index),
            false => None,
        }
    }

    /// Every value in the order its key was asked for.
    pub fn iter(&self) -> Rows<Values<'arena>, Option<&'arena [u8]>> {
        Rows::new(*self, self.len(), Values::get)
    }

    /// Which keys the read found, one bit per key as Arrow reads a validity buffer.
    pub fn presences(&self) -> Bitmap<'arena> {
        self.presences
    }

    /// Every value on the one tape the read exported, without a copy.
    ///
    /// A missing value is empty on the tape, and [`Values::presences`] tells it apart from an empty one.
    pub fn tape(&self) -> Tape<'arena> {
        self.values.tape()
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
    /// Lower bound on the keys in the range.
    pub min_cardinality: usize,
    /// Upper bound on the keys in the range.
    pub max_cardinality: usize,
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

impl<Parent: Writable, Completion: crate::db::sealed::Completion> View<'_, Parent, Completion> {
    /// Upserts a batch of values, where an empty value is still a value and [`View::remove`] removes keys.
    ///
    /// The entries are borrowed [`Entry`] values, or owned keys beside a `stringtape::BytesTape` of their values with
    /// 32-bit or 64-bit offsets, which a deferred write needs, and the call hands them back. The batch spans as many
    /// collections as it has entries, and a bare [`CollectionID`] gives every entry the same one. A transaction
    /// flushes at its commit, so [`Durability::Flushed`] is refused inside one.
    ///
    /// Calls `ustore_blobs_write`.
    pub fn write<'collections, Entries: Pairs>(
        &self,
        collections: impl Into<Collections<'collections>>,
        entries: Entries,
        durability: Durability,
    ) -> Completion::Answer<Write<'collections, Entries>>
    where
        Completion: Accepts<Collections<'collections>> + Accepts<Entries>,
    {
        let verb = Write {
            collections: collections.into(),
            entries,
            durability,
        };
        Completion::answer(self, verb)
    }

    /// Removes a batch of keys with their values, whether or not the database holds them, and hands the keys back.
    ///
    /// Calls `ustore_blobs_remove`.
    pub fn remove<'collections, Keys: AsRef<[Key]>>(
        &self,
        collections: impl Into<Collections<'collections>>,
        keys: Keys,
        durability: Durability,
    ) -> Completion::Answer<Remove<'collections, Keys>>
    where
        Completion: Accepts<Collections<'collections>> + Accepts<Keys>,
    {
        let verb = Remove {
            collections: collections.into(),
            keys,
            durability,
        };
        Completion::answer(self, verb)
    }
}

impl<Parent, Completion: crate::db::sealed::Completion> View<'_, Parent, Completion> {
    /// Reads a batch of keys into the arena, one collection per key or one for all of them, and hands back the
    /// [`Values`].
    ///
    /// Calls `ustore_blobs_read`.
    pub fn read<'arena, 'collections, Keys: AsRef<[Key]>>(
        &self,
        collections: impl Into<Collections<'collections>>,
        keys: Keys,
        arena: &'arena mut Arena,
    ) -> Completion::Answer<Read<'arena, 'collections, Keys>>
    where
        Completion: Accepts<Collections<'collections>> + Accepts<Keys>,
    {
        let verb = Read {
            collections: collections.into(),
            keys,
            arena,
            presences: Address::default(),
            values: ustore_strings_view_t::default(),
        };
        Completion::answer(self, verb)
    }

    /// Lists up to `wanted` keys of one collection from `start` into the arena, while a [`Cursor`] walks one with its
    /// values.
    ///
    /// Calls `ustore_blobs_scan`.
    pub fn scan<'arena>(
        &self,
        collection: CollectionID,
        start: Key,
        wanted: usize,
        arena: &'arena mut Arena,
    ) -> Completion::Answer<Scan<'arena>> {
        let verb = Scan {
            collection,
            start,
            arena,
            wanted,
            found: Found::default(),
        };
        Completion::answer(self, verb)
    }

    /// Draws up to `wanted` keys uniformly at random from one collection into the arena.
    ///
    /// Calls `ustore_blobs_sample`.
    pub fn sample<'arena>(
        &self,
        collection: CollectionID,
        wanted: usize,
        arena: &'arena mut Arena,
    ) -> Completion::Answer<Sample<'arena>> {
        let verb = Sample {
            collection,
            arena,
            wanted,
            found: Found::default(),
        };
        Completion::answer(self, verb)
    }

    /// Estimates the keys and the space a range of keys takes in one collection, through the arena its bounds land in.
    ///
    /// Calls `ustore_blobs_measure`.
    pub fn measure<'arena>(
        &self,
        collection: CollectionID,
        start: Key,
        end: Key,
        arena: &'arena mut Arena,
    ) -> Completion::Answer<Measure<'arena>> {
        let verb = Measure {
            collection,
            start,
            end,
            arena,
            bounds: Default::default(),
        };
        Completion::answer(self, verb)
    }
}

impl<'view, Parent> View<'view, Parent> {
    /// Opens a walk over the keys of one collection from `start`, reading nothing until its first pull, which then
    /// sees what this view sees, pull by pull, on this view's executor.
    ///
    /// The cursor holds the database open, and the transaction or snapshot this view reads through, until it is
    /// dropped.
    ///
    /// Calls `ustore_blobs_cursor_init`.
    pub fn cursor(&self, collection: CollectionID, start: Key) -> Result<Cursor<'view>> {
        let mut cursor = Cursor {
            handle: ptr::null_mut(),
            database: self.database(),
            executor: self.executor(),
            marker: PhantomData,
        };
        let mut outcome = ustore_outcome_t::default();
        let mut arguments = ffi::ustore_blobs_cursor_init_t {
            cursor: &mut cursor.handle,
            database: self.database(),
            outcome: &mut outcome,
            view: self.view(),
            collection,
            start_key: start,
        };
        // SAFETY: every pointer in `arguments` outlives the call, which is final on return.
        unsafe { ffi::ustore_blobs_cursor_init(&mut arguments) };
        check(outcome).map(|()| cursor)
    }

    /// Which of `keys` one collection holds, exporting no value.
    pub(crate) fn contains<'arena>(
        &self,
        collection: CollectionID,
        keys: &[Key],
        arena: &'arena mut Arena,
    ) -> Result<Bitmap<'arena>> {
        let mut presences: *const ustore_octet_t = ptr::null();
        let mut outcome = ustore_outcome_t::default();
        let mut arguments = ffi::ustore_blobs_read_t {
            database: self.database(),
            executor: self.executor(),
            outcome: &mut outcome,
            completion: ustore_completion_t::Blocking,
            view: self.view(),
            count: keys.len() as ustore_size_t,
            collections: ustore_strided_view_t::repeated(&collection),
            keys: ustore_strided_view_t::packed(keys),
            presences: &mut presences,
            lengths: ptr::null_mut(),
            values: ptr::null_mut(),
            arena: arena.handle,
        };
        // SAFETY: the keys, the collection and the output outlive the call, which is final on return.
        unsafe { ffi::ustore_blobs_read(&mut arguments) };
        check(outcome)?;
        // SAFETY: a successful read exported a presence bit per key into the arena, which stays borrowed for `'arena`.
        let presences = unsafe { exported_slice(presences, keys.len().div_ceil(8)) };
        Ok(Bitmap::new(presences, keys.len()))
    }
}

/// An upsert of a batch of values; see [`View::write`].
#[derive(Debug)]
pub struct Write<'collections, Entries> {
    collections: Collections<'collections>,
    entries: Entries,
    durability: Durability,
}

impl<Entries: Pairs> Calling for Write<'_, Entries> {
    type Arguments = ffi::ustore_blobs_write_t;
    type Output = Entries;

    fn arguments<Parent, Completion>(
        &mut self,
        view: &View<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let Some(pairs) = self.entries.pairs()? else {
            return Ok(None);
        };
        Ok(Some(ffi::ustore_blobs_write_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            transaction: view.transaction(),
            count: pairs.count as ustore_size_t,
            collections: self.collections.view(pairs.count)?,
            keys: pairs.keys,
            values: pairs.values,
            durability: self.durability,
            watch: view.watch(),
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_blobs_write(arguments)
    }

    fn finish(self) -> Result<Entries> {
        Ok(self.entries)
    }
}

/// A removal of a batch of keys; see [`View::remove`].
#[derive(Debug)]
pub struct Remove<'collections, Keys> {
    collections: Collections<'collections>,
    keys: Keys,
    durability: Durability,
}

impl<Keys: AsRef<[Key]>> Calling for Remove<'_, Keys> {
    type Arguments = ffi::ustore_blobs_remove_t;
    type Output = Keys;

    fn arguments<Parent, Completion>(
        &mut self,
        view: &View<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let keys = self.keys.as_ref();
        if keys.is_empty() {
            return Ok(None);
        }
        Ok(Some(ffi::ustore_blobs_remove_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            transaction: view.transaction(),
            count: keys.len() as ustore_size_t,
            collections: self.collections.view(keys.len())?,
            keys: ustore_strided_view_t::packed(keys),
            durability: self.durability,
            watch: view.watch(),
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_blobs_remove(arguments)
    }

    fn finish(self) -> Result<Keys> {
        Ok(self.keys)
    }
}

/// A read of a batch of keys; see [`View::read`].
#[derive(Debug)]
pub struct Read<'arena, 'collections, Keys> {
    collections: Collections<'collections>,
    keys: Keys,
    arena: &'arena mut Arena,
    presences: Address<ustore_octet_t>,
    values: ustore_strings_view_t,
}

impl<'arena, Keys: AsRef<[Key]>> Calling for Read<'arena, '_, Keys> {
    type Arguments = ffi::ustore_blobs_read_t;
    type Output = Values<'arena>;

    fn arguments<Parent, Completion>(
        &mut self,
        view: &View<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let keys = self.keys.as_ref();
        if keys.is_empty() {
            return Ok(None);
        }
        Ok(Some(ffi::ustore_blobs_read_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            view: view.view(),
            count: keys.len() as ustore_size_t,
            collections: self.collections.view(keys.len())?,
            keys: ustore_strided_view_t::packed(keys),
            presences: &mut self.presences.0,
            lengths: ptr::null_mut(),
            values: &mut self.values,
            arena: self.arena.handle,
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_blobs_read(arguments)
    }

    fn finish(self) -> Result<Values<'arena>> {
        // SAFETY: the read succeeded over every key, and the arena stays borrowed for `'arena`.
        unsafe { Values::exported(self.keys.as_ref().len(), self.presences, self.values) }
    }
}

/// Where one scan or sample exports the two offsets around its keys, and the keys between them.
#[derive(Debug, Default)]
struct Found {
    offsets: Address<ustore_length_t>,
    keys: Address<Key>,
}

impl Found {
    /// The keys a successful scan or sample of one task exported.
    ///
    /// # Safety
    /// The call succeeded, exporting two offsets and the keys between them into an arena alive for `'arena`.
    unsafe fn keys<'arena>(&self) -> &'arena [Key] {
        let offsets = exported_slice(self.offsets.0, 2);
        let (begin, end) = (offsets[0] as usize, offsets[1] as usize);
        exported_slice(self.keys.0.wrapping_add(begin), end - begin)
    }
}

/// A walk over the keys of one collection; see [`View::scan`].
#[derive(Debug)]
pub struct Scan<'arena> {
    collection: CollectionID,
    start: Key,
    arena: &'arena mut Arena,
    wanted: usize,
    found: Found,
}

impl<'arena> Calling for Scan<'arena> {
    type Arguments = ffi::ustore_blobs_scan_t;
    type Output = &'arena [Key];

    fn arguments<Parent, Completion>(
        &mut self,
        view: &View<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        Ok(Some(ffi::ustore_blobs_scan_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            view: view.view(),
            count: 1,
            collections: ustore_strided_view_t::repeated(&self.collection),
            start_keys: ustore_strided_view_t::repeated(&self.start),
            offsets: &mut self.found.offsets.0,
            keys: &mut self.found.keys.0,
            arena: self.arena.handle,
            wanted: self.wanted as ustore_size_t,
            mode: ustore_scan_mode_t::Ordered,
            progress: None,
            progress_state: ptr::null_mut(),
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_blobs_scan(arguments)
    }

    fn finish(self) -> Result<&'arena [Key]> {
        // SAFETY: the scan succeeded, and the arena stays borrowed for `'arena`.
        Ok(unsafe { self.found.keys() })
    }
}

/// A random draw of keys from one collection; see [`View::sample`].
#[derive(Debug)]
pub struct Sample<'arena> {
    collection: CollectionID,
    arena: &'arena mut Arena,
    wanted: usize,
    found: Found,
}

impl<'arena> Calling for Sample<'arena> {
    type Arguments = ffi::ustore_blobs_sample_t;
    type Output = &'arena [Key];

    fn arguments<Parent, Completion>(
        &mut self,
        view: &View<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        Ok(Some(ffi::ustore_blobs_sample_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            view: view.view(),
            count: 1,
            collections: ustore_strided_view_t::repeated(&self.collection),
            offsets: &mut self.found.offsets.0,
            keys: &mut self.found.keys.0,
            arena: self.arena.handle,
            wanted: self.wanted as ustore_size_t,
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_blobs_sample(arguments)
    }

    fn finish(self) -> Result<&'arena [Key]> {
        // SAFETY: the sample succeeded, and the arena stays borrowed for `'arena`.
        Ok(unsafe { self.found.keys() })
    }
}

/// An estimate of what a range of keys holds; see [`View::measure`].
#[derive(Debug)]
pub struct Measure<'arena> {
    collection: CollectionID,
    start: Key,
    end: Key,
    arena: &'arena mut Arena,
    /// Where the six bounds land, in the order [`Measurement`] lists them.
    bounds: [Address<ustore_size_t>; 6],
}

impl Calling for Measure<'_> {
    type Arguments = ffi::ustore_blobs_measure_t;
    type Output = Measurement;

    fn arguments<Parent, Completion>(
        &mut self,
        view: &View<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let [min_cardinalities, max_cardinalities, min_value_bytes, max_value_bytes, min_space_usages, max_space_usages] =
            &mut self.bounds;
        Ok(Some(ffi::ustore_blobs_measure_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            view: view.view(),
            count: 1,
            collections: ustore_strided_view_t::repeated(&self.collection),
            start_keys: ustore_strided_view_t::repeated(&self.start),
            end_keys: ustore_strided_view_t::repeated(&self.end),
            min_cardinalities: &mut min_cardinalities.0,
            max_cardinalities: &mut max_cardinalities.0,
            min_value_bytes: &mut min_value_bytes.0,
            max_value_bytes: &mut max_value_bytes.0,
            min_space_usages: &mut min_space_usages.0,
            max_space_usages: &mut max_space_usages.0,
            arena: self.arena.handle,
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_blobs_measure(arguments)
    }

    fn finish(self) -> Result<Measurement> {
        // SAFETY: a successful measurement of one range exported every bound it was asked for into the arena.
        let [min_cardinality, max_cardinality, min_value_bytes, max_value_bytes, min_space_usage, max_space_usage] =
            self.bounds.map(|bound| unsafe { *bound.0 });
        Ok(Measurement {
            min_cardinality: min_cardinality as usize,
            max_cardinality: max_cardinality as usize,
            min_value_bytes,
            max_value_bytes,
            min_space_usage,
            max_space_usage,
        })
    }
}

// endregion: Verbs

// region: Cursors

/// A walk over the keys of one collection, which every pull resumes past the last key the pull before it handed out,
/// so a key written behind the walk is never handed out and one written ahead of it is; see [`View::cursor`].
///
/// A page shorter than `wanted` exhausts the walk for good, so every pull after it hands out nothing, even once more
/// keys are written. A pull that fails leaves the cursor where it was.
///
/// Mirrors `ustore_blobs_cursor_t`; dropping it calls `ustore_blobs_cursor_free`.
#[derive(Debug)]
pub struct Cursor<'view> {
    handle: ffi::ustore_blobs_cursor_t,
    database: ustore_database_t,
    executor: ustore_executor_t,
    marker: PhantomData<&'view ()>,
}

impl Cursor<'_> {
    /// Pulls up to `wanted` next keys into `arena`, fewer once the walk nears its end and none past it.
    ///
    /// Calls `ustore_blobs_cursor_next`.
    pub fn next<'arena>(&mut self, wanted: usize, arena: &'arena mut Arena) -> Result<&'arena [Key]> {
        self.pull(wanted, arena, ptr::null_mut())
    }

    /// Pulls up to `wanted` next keys with their values into `arena` in one call, where a key removed at the head
    /// between the listing and the read of one pull reads as an empty value.
    ///
    /// Calls `ustore_blobs_cursor_next`.
    pub fn next_pairs<'arena>(&mut self, wanted: usize, arena: &'arena mut Arena) -> Result<Page<'arena>> {
        let mut values = ustore_strings_view_t::default();
        let keys = self.pull(wanted, arena, &mut values)?;
        // SAFETY: the pull succeeded, writing a value per key as a tape into the arena borrowed for `'arena`.
        let values = unsafe { Strings::exported(values, keys.len())? };
        Ok(Page { keys, values })
    }

    /// Pulls a page of keys into `arena`, with their values into `values` unless it is null.
    fn pull<'arena>(
        &mut self,
        wanted: usize,
        arena: &'arena mut Arena,
        values: *mut ustore_strings_view_t,
    ) -> Result<&'arena [Key]> {
        let mut keys: *const ustore_key_t = ptr::null();
        let mut written: ustore_size_t = 0;
        let mut outcome = ustore_outcome_t::default();
        let mut arguments = ffi::ustore_blobs_cursor_next_t {
            database: self.database,
            executor: self.executor,
            outcome: &mut outcome,
            completion: ustore_completion_t::Blocking,
            cursor: self.handle,
            keys: &mut keys,
            values,
            written: &mut written,
            arena: arena.handle,
            wanted: wanted as ustore_size_t,
            progress: None,
            progress_state: ptr::null_mut(),
        };
        // SAFETY: every pointer in `arguments` outlives the call, which is final on return.
        unsafe { ffi::ustore_blobs_cursor_next(&mut arguments) };
        check(outcome)?;
        // SAFETY: a successful pull wrote `written` keys into the arena, which stays borrowed for `'arena`.
        Ok(unsafe { exported_slice(keys, written as usize) })
    }
}

impl Drop for Cursor<'_> {
    fn drop(&mut self) {
        // SAFETY: the handle came from a successful init, or is the null a failed one leaves, which the free ignores,
        // and is freed once, giving back the holds the cursor kept.
        unsafe { ffi::ustore_blobs_cursor_free(self.handle) };
    }
}

/// The keys one pull of a [`Cursor`] handed out, with their values on one tape in the arena; see
/// [`Cursor::next_pairs`].
#[derive(Clone, Copy, Debug)]
pub struct Page<'arena> {
    keys: &'arena [Key],
    values: Strings<'arena>,
}

impl<'arena> Page<'arena> {
    /// The keys of the page, in their order.
    pub fn keys(&self) -> &'arena [Key] {
        self.keys
    }

    /// The value of the key at `index`, or `None` past the end of the page.
    pub fn get(&self, index: usize) -> Option<&'arena [u8]> {
        self.values.get(index)
    }

    /// Every value of the page on the one tape the pull exported, without a copy.
    pub fn tape(&self) -> Tape<'arena> {
        self.values.tape()
    }
}

// endregion: Cursors

// region: Tests

#[cfg(test)]
mod tests {
    use super::*;
    use crate::db::{CollectionID, Collections, Database, DropMode, Executor, Modality, RAMOptions, Status, UUID};
    use crate::testing::{created, key, keys};

    /// A database whose blobs collection holds `alpha`, `beta` and `gamma` under the keys one, two and three.
    fn populated() -> (Database, CollectionID, Arena) {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let blobs = created(&database, Modality::Blobs);
        let entries = [
            Entry::new(key(1), b"alpha"),
            Entry::new(key(2), b"beta"),
            Entry::new(key(3), b"gamma"),
        ];
        database.head().write(blobs, &entries, Durability::Buffered).unwrap();
        (database, blobs, Arena::new().unwrap())
    }

    /// A tape holding `values` in order.
    fn tape<const COUNT: usize>(values: [&[u8]; COUNT]) -> BytesTape<u64> {
        values.into_iter().collect()
    }

    #[test]
    fn batches_write_and_read_back_in_order() {
        let (database, blobs, mut arena) = populated();
        let values = database.head().read(blobs, keys([3, 1, 2]), &mut arena).unwrap();
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
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new().unwrap();
        let blobs = created(&database, Modality::Blobs);
        let entries = [Entry::new(key(1), b"")];
        database.head().write(blobs, &entries, Durability::Buffered).unwrap();

        let values = database.head().read(blobs, keys([1, 2]), &mut arena).unwrap();
        assert_eq!(values.get(0), Some(b"".as_slice()), "an empty value is present");
        assert_eq!(values.get(1), None, "a key nobody wrote is missing");
    }

    #[test]
    fn empty_batches_reach_no_engine() {
        let (database, blobs, mut arena) = populated();
        let head = database.head();
        head.write(blobs, &[], Durability::Buffered).unwrap();
        head.write(blobs, ([], tape([])), Durability::Buffered).unwrap();
        head.remove(blobs, [], Durability::Buffered).unwrap();
        let values = head.read(blobs, [], &mut arena).unwrap();
        assert!(values.is_empty());
        assert_eq!(values.get(0), None);
        assert!(values.tape().is_empty() && values.presences().is_empty());
    }

    #[test]
    fn removals_take_every_key_they_name() {
        let (database, blobs, mut arena) = populated();
        database
            .head()
            .remove(blobs, keys([1, 3]), Durability::Buffered)
            .unwrap();
        let values = database.head().read(blobs, keys([1, 2, 3]), &mut arena).unwrap();
        assert_eq!(values.get(0), None);
        assert_eq!(values.get(1), Some(b"beta".as_slice()));
        assert_eq!(values.get(2), None);
    }

    #[test]
    fn tapes_of_either_width_write_a_value_per_key() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new().unwrap();
        let blobs = created(&database, Modality::Blobs);
        let written = database
            .head()
            .write(
                blobs,
                (keys([1, 2, 3]), tape([b"alpha", b"", b"gamma"])),
                Durability::Buffered,
            )
            .unwrap();
        assert_eq!(written.1.len(), 3, "a write hands its inputs back");
        let narrow: BytesTape<u32> = [b"delta".as_slice()].into_iter().collect();
        database
            .head()
            .write(blobs, (keys([4]), narrow), Durability::Buffered)
            .unwrap();

        // A read whose bytes fit in 4 GiB answers a tape of 32-bit offsets.
        let values = database.head().read(blobs, keys([1, 2, 3, 4]), &mut arena).unwrap();
        let read: Vec<Option<&[u8]>> = values.iter().collect();
        assert_eq!(
            read,
            [Some(b"alpha".as_slice()), Some(b""), Some(b"gamma"), Some(b"delta")]
        );
        assert!(matches!(values.tape(), Tape::U32(tape) if tape.len() == 4));

        let failure = database
            .head()
            .write(blobs, (keys([1, 2]), tape([b"alpha"])), Durability::Buffered)
            .unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);
    }

    #[test]
    fn verbs_block_or_defer_on_an_executor() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let executor = Executor::for_ram(&[], 2, 0).unwrap();
        let blocking = database.head().on(&executor);
        let deferred = database.head().deferred(&executor);
        let blobs = created(&database, Modality::Blobs);
        let pairs = (keys([1, 2, 3]), tape([b"alpha", b"beta", b"gamma"]));

        // Writes and removals hand their inputs back, whichever way the call finishes.
        let pairs = blocking.write(blobs, pairs, Durability::Buffered).unwrap();
        let (written, _) = deferred
            .write(blobs, pairs, Durability::Buffered)
            .unwrap()
            .wait()
            .unwrap();
        assert_eq!(written, keys([1, 2, 3]));
        let removed = blocking.remove(blobs, keys([3]), Durability::Buffered).unwrap();
        let removed = deferred
            .remove(blobs, removed, Durability::Buffered)
            .unwrap()
            .wait()
            .unwrap();
        assert_eq!(removed, keys([3]));

        // Reads borrow one arena on either view, and each call rewinds it for the next.
        let mut arena = Arena::new_in(&executor).unwrap();
        let read = blocking.read(blobs, keys([1, 3]), &mut arena).unwrap();
        assert_eq!(read.get(0), Some(b"alpha".as_slice()));
        let read = deferred
            .read(blobs, vec![key(2), key(3)], &mut arena)
            .unwrap()
            .wait()
            .unwrap();
        assert_eq!(read.get(0), Some(b"beta".as_slice()));
        assert_eq!(read.get(1), None);

        let scanned = deferred.scan(blobs, key(2), 10, &mut arena).unwrap().wait().unwrap();
        assert_eq!(scanned, &keys([2]));
        assert_eq!(blocking.scan(blobs, key(0), 1, &mut arena).unwrap(), &keys([1]));
        let sampled = deferred.sample(blobs, 10, &mut arena).unwrap().wait().unwrap();
        assert_eq!(sampled.len(), 2);
        assert_eq!(blocking.sample(blobs, 1, &mut arena).unwrap().len(), 1);
        let measured = deferred
            .measure(blobs, key(1), key(3), &mut arena)
            .unwrap()
            .wait()
            .unwrap();
        assert!(measured.max_cardinality >= measured.min_cardinality);
        let measured = blocking.measure(blobs, key(1), key(3), &mut arena).unwrap();
        assert!(measured.max_value_bytes >= measured.min_value_bytes);
    }

    #[test]
    fn a_refused_deferred_call_never_reaches_the_library() {
        let (database, blobs, mut arena) = populated();
        let executor = Executor::for_ram(&[], 1, 0).unwrap();
        static ONE_ALONE: [CollectionID; 1] = [CollectionID(UUID([0; 16]))];
        let refused = database
            .head()
            .deferred(&executor)
            .read(Collections::Each(&ONE_ALONE), keys([1, 2]), &mut arena)
            .unwrap_err();
        assert_eq!(refused.status, Status::InvalidArgument, "a collection per task");

        // An empty batch reaches no engine and is final at once.
        let empty = database.head().deferred(&executor).read(blobs, [], &mut arena).unwrap();
        assert_eq!(empty.status(), Ok(()));
        assert!(empty.wait().unwrap().is_empty());
    }

    #[test]
    fn entries_borrow_what_they_carry() {
        let value = [1u8, 2, 3];
        let entry = Entry::new(key(42), &value);
        assert_eq!(entry.key(), key(42));
        assert_eq!(entry.value(), value.as_slice());
        assert_eq!(Entry::new(key(42), b"").value(), b"".as_slice());
    }

    #[test]
    fn writes_land_in_the_collection_they_name() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new().unwrap();
        let named = database.collection_create("people", Modality::Blobs).unwrap();
        let other = created(&database, Modality::Blobs);
        let entries = [Entry::new(key(1), b"in the named one")];
        database.head().write(named, &entries, Durability::Buffered).unwrap();

        let there = database.head().read(named, keys([1]), &mut arena).unwrap();
        assert_eq!(there.get(0), Some(b"in the named one".as_slice()));
        let elsewhere = database.head().read(other, keys([1]), &mut arena).unwrap();
        assert_eq!(elsewhere.get(0), None);

        // Dropping the collection takes its contents with it.
        database.collection_drop(named, DropMode::Pairs).unwrap();
        assert_eq!(database.head().read(named, keys([1]), &mut arena).unwrap().get(0), None);
    }

    #[test]
    fn scans_walk_in_key_order_and_paginate() {
        let (database, blobs, mut arena) = populated();
        let first_page = database.head().scan(blobs, key(0), 2, &mut arena).unwrap();
        assert_eq!(first_page, &keys([1, 2]));

        let resume = first_page[first_page.len() - 1].next();
        let second_page = database.head().scan(blobs, resume, 2, &mut arena).unwrap();
        assert_eq!(second_page, &keys([3]));

        // A walk starting past the last key finds nothing.
        assert!(database.head().scan(blobs, key(99), 2, &mut arena).unwrap().is_empty());
    }

    #[test]
    fn scans_refuse_a_limit_of_nothing_to_walk() {
        let (database, blobs, mut arena) = populated();
        let found = database.head().scan(blobs, key(0), 0, &mut arena).unwrap();
        assert!(found.is_empty(), "wanting nothing walks nothing");
    }

    #[test]
    fn cursors_resume_past_the_last_key_until_a_short_page() {
        let (database, blobs, mut arena) = populated();
        let head = database.head();
        let mut cursor = head.cursor(blobs, key(0)).unwrap();
        let page = cursor.next_pairs(2, &mut arena).unwrap();
        assert_eq!(page.keys(), &keys([1, 2]));
        let values = (page.get(0), page.get(1), page.get(2));
        assert_eq!(values, (Some(b"alpha".as_slice()), Some(b"beta".as_slice()), None));

        // A key written behind the walk is never handed out, and one written ahead of it is.
        let late = [Entry::new(key(0), b"behind"), Entry::new(key(4), b"ahead")];
        head.write(blobs, &late, Durability::Buffered).unwrap();
        assert_eq!(cursor.next(2, &mut arena).unwrap(), &keys([3, 4]));

        // A page shorter than wanted exhausts the walk for good.
        assert!(cursor.next(2, &mut arena).unwrap().is_empty());
        let later = [Entry::new(key(5), b"later")];
        head.write(blobs, &later, Durability::Buffered).unwrap();
        assert!(cursor.next(2, &mut arena).unwrap().is_empty());
    }

    #[test]
    fn cursors_read_through_the_snapshot_they_were_opened_at() {
        let (database, blobs, mut arena) = populated();
        let executor = Executor::for_ram(&[], 1, 0).unwrap();
        let snapshot = database.snapshot().unwrap();
        let mut cursor = snapshot.on(&executor).cursor(blobs, key(0)).unwrap();
        database.head().remove(blobs, keys([2]), Durability::Buffered).unwrap();

        let page = cursor.next_pairs(10, &mut arena).unwrap();
        assert_eq!(page.keys(), &keys([1, 2, 3]));
        assert_eq!(page.get(1), Some(b"beta".as_slice()));
        assert!(matches!(page.tape(), Tape::U32(tape) if tape.len() == 3));
    }

    #[test]
    fn samples_draw_from_what_the_collection_holds() {
        let (database, blobs, mut arena) = populated();
        let drawn = database.head().sample(blobs, 2, &mut arena).unwrap();
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
        let (database, blobs, mut arena) = populated();
        let measured = database.head().measure(blobs, key(1), key(3), &mut arena).unwrap();
        assert!(measured.min_cardinality <= 3 && measured.max_cardinality >= measured.min_cardinality);
        assert!(measured.max_value_bytes >= measured.min_value_bytes);
        assert!(measured.max_space_usage >= measured.min_space_usage);
    }

    #[test]
    fn one_batch_spans_several_collections() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new().unwrap();
        let people = database.collection_create("people", Modality::Blobs).unwrap();
        let places = database.collection_create("places", Modality::Blobs).unwrap();

        let entries = [Entry::new(key(1), b"Ada"), Entry::new(key(1), b"Kent")];
        let written = [people, places];
        database
            .head()
            .write(written.as_slice(), &entries, Durability::Buffered)
            .unwrap();

        let values = database
            .head()
            .read(Collections::Each(&written), keys([1, 1]), &mut arena)
            .unwrap();
        assert_eq!(values.get(0), Some(b"Ada".as_slice()));
        assert_eq!(values.get(1), Some(b"Kent".as_slice()));

        // One collection for the whole batch is the same call with the ergonomic argument.
        let inside = database.head().read(people, keys([1]), &mut arena).unwrap();
        assert_eq!(inside.get(0), Some(b"Ada".as_slice()));

        database
            .head()
            .remove(written.as_slice(), keys([1, 1]), Durability::Buffered)
            .unwrap();
        let removed = database
            .head()
            .read(Collections::Each(&written), keys([1, 1]), &mut arena)
            .unwrap();
        assert_eq!((removed.get(0), removed.get(1)), (None, None));
    }

    #[test]
    fn a_batch_of_collections_matches_the_batch_of_tasks() {
        let (database, blobs, mut arena) = populated();
        let one = [blobs];
        let failure = database
            .head()
            .read(Collections::Each(&one), keys([1, 2]), &mut arena)
            .unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);

        let entries = [Entry::new(key(1), b"alpha"), Entry::new(key(2), b"beta")];
        let failure = database
            .head()
            .write(Collections::Each(&one), &entries, Durability::Buffered)
            .unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);
    }

    #[test]
    fn an_unknown_collection_is_refused() {
        let (database, _, mut arena) = populated();
        // Every call resolves its collections first, so an identifier nobody created reaches nothing.
        let failure = database
            .head()
            .read(CollectionID::from(4242u64), keys([1]), &mut arena)
            .unwrap_err();
        assert_eq!(failure.status, Status::NotFound);

        let failure = database
            .collection_drop(CollectionID::from(4242u64), DropMode::Collection)
            .unwrap_err();
        assert_eq!(failure.status, Status::NotFound);
    }
}

// endregion: Tests
