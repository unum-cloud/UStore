//! The names of collections, which the collection calls of `c/ustore/db.h` record and resolve, and
//! the strings collections of `c/ustore/strings.h`, which map every string onto one key.
//!
//! Names resolve in two phases: a name maps onto a collection once, and every other call addresses
//! the collection by its identifier. Each engine keeps the names apart from every collection, so no
//! identifier reaches them.
//!
//! Strings cross as borrowed `&str` slices, or as an owned `stringtape::CharsTape`, which a
//! deferred call needs, and come back as byte runs inside the arena, since an engine orders them by
//! their bytes and never checks UTF-8.
//!
//! File: rust/strings.rs
//! Author: Ash Vardanian
//! Date: September 16, 2026

use std::iter::Copied;
use std::ops::Range;
use std::ptr;
use std::slice;

use stringtape::CharsTape;

use crate::blobs::{Bounds, Measurement};
use crate::db::ffi::{
    self as db_ffi, ustore_completion_t, ustore_database_t, ustore_enum_t, ustore_length_t, ustore_octet_t,
    ustore_outcome_t, ustore_size_t, ustore_strided_view_t, ustore_strings_view_t,
};
use crate::db::sealed::{Accepts, Calling, TapeOffset, Writable};
use crate::db::{
    check, exported_slice, fail, string_views, Address, Arena, CollectionID, Collections, Deferring, Durability, Key,
    Matched, Matching, Modality, On, Progress, ProgressCallback, Result, Rows, ScanDirection, ScanMode, Status,
    Strings,
};
use crate::docs::Bitmap;

// region: Raw Declarations

/// One-to-one mirror of `c/ustore/strings.h`, but for its cursors, which only the Arrow export
/// drives.
pub(crate) mod ffi {
    #![allow(non_camel_case_types)]

    use std::ffi::c_void;

    use crate::db::ffi::{
        ustore_arena_t, ustore_bytes_view_t, ustore_collection_id_t, ustore_collections_view_t, ustore_completion_t,
        ustore_database_t, ustore_durability_t, ustore_executor_t, ustore_key_t, ustore_keys_view_t, ustore_length_t,
        ustore_octet_t, ustore_outcome_t, ustore_patterns_t, ustore_predicate_t, ustore_progress_callback_t,
        ustore_scan_direction_t, ustore_scan_mode_t, ustore_size_t, ustore_strings_view_t, ustore_transaction_t,
        ustore_view_t, ustore_watch_t,
    };

    #[repr(C)]
    pub struct ustore_strings_write_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub transaction: ustore_transaction_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub strings: ustore_strings_view_t,
        pub values: ustore_keys_view_t,
        pub durability: ustore_durability_t,
        pub watch: ustore_watch_t,
    }

    #[repr(C)]
    pub struct ustore_strings_remove_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub transaction: ustore_transaction_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub strings: ustore_strings_view_t,
        pub end_strings: ustore_strings_view_t,
        pub durability: ustore_durability_t,
        pub watch: ustore_watch_t,
    }

    #[repr(C)]
    pub struct ustore_strings_read_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub view: ustore_view_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub strings: ustore_strings_view_t,
        pub presences: *mut *const ustore_octet_t,
        pub values: *mut *const ustore_key_t,
        pub arena: ustore_arena_t,
    }

    #[repr(C)]
    pub struct ustore_strings_scan_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub view: ustore_view_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub prefixes: ustore_strings_view_t,
        pub start_strings: ustore_strings_view_t,
        pub end_strings: ustore_strings_view_t,
        pub direction: ustore_scan_direction_t,
        pub patterns: ustore_patterns_t,
        pub predicate: ustore_predicate_t,
        pub offsets: *mut *const ustore_length_t,
        pub strings: *mut ustore_strings_view_t,
        pub scores: *mut *const f32,
        pub arena: ustore_arena_t,
        pub wanted: ustore_size_t,
        pub mode: ustore_scan_mode_t,
        pub progress: ustore_progress_callback_t,
        pub progress_state: *mut c_void,
    }

    #[repr(C)]
    pub struct ustore_strings_sample_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub view: ustore_view_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub offsets: *mut *const ustore_length_t,
        pub strings: *mut ustore_strings_view_t,
        pub arena: ustore_arena_t,
        pub wanted: ustore_size_t,
    }

    #[repr(C)]
    pub struct ustore_strings_measure_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub view: ustore_view_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub start_strings: ustore_strings_view_t,
        pub end_strings: ustore_strings_view_t,
        pub direction: ustore_scan_direction_t,
        pub min_cardinalities: *mut *const ustore_size_t,
        pub max_cardinalities: *mut *const ustore_size_t,
        pub min_value_bytes: *mut *const ustore_size_t,
        pub max_value_bytes: *mut *const ustore_size_t,
        pub min_space_usages: *mut *const ustore_size_t,
        pub max_space_usages: *mut *const ustore_size_t,
        pub arena: ustore_arena_t,
    }

    #[repr(C)]
    pub struct ustore_strings_cursor_handle_t {
        _private: [u8; 0],
    }

    pub type ustore_strings_cursor_t = *mut ustore_strings_cursor_handle_t;

    #[repr(C)]
    pub struct ustore_strings_cursor_init_t {
        pub cursor: *mut ustore_strings_cursor_t,
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub view: ustore_view_t,
        pub collection: ustore_collection_id_t,
        pub prefix: ustore_bytes_view_t,
        pub start_string: ustore_bytes_view_t,
        pub end_string: ustore_bytes_view_t,
        pub direction: ustore_scan_direction_t,
        pub patterns: ustore_patterns_t,
        pub predicate: ustore_predicate_t,
    }

    #[repr(C)]
    pub struct ustore_strings_cursor_next_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub cursor: ustore_strings_cursor_t,
        pub strings: *mut ustore_strings_view_t,
        pub values: *mut *const ustore_key_t,
        pub written: *mut ustore_size_t,
        pub arena: ustore_arena_t,
        pub wanted: ustore_size_t,
        pub progress: ustore_progress_callback_t,
        pub progress_state: *mut c_void,
    }

    extern "C" {
        pub fn ustore_strings_write(arguments: *mut ustore_strings_write_t);
        pub fn ustore_strings_remove(arguments: *mut ustore_strings_remove_t);
        pub fn ustore_strings_read(arguments: *mut ustore_strings_read_t);
        pub fn ustore_strings_scan(arguments: *mut ustore_strings_scan_t);
        pub fn ustore_strings_sample(arguments: *mut ustore_strings_sample_t);
        pub fn ustore_strings_measure(arguments: *mut ustore_strings_measure_t);
        pub fn ustore_strings_cursor_init(arguments: *mut ustore_strings_cursor_init_t);
        pub fn ustore_strings_cursor_next(arguments: *mut ustore_strings_cursor_next_t);
        pub fn ustore_strings_cursor_free(cursor: ustore_strings_cursor_t);
    }
}

// endregion: Raw Declarations

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

// region: Strings

/// The strings a scan keeps, what admits them and how it reports; see [`On::strings_scan`].
pub struct StringsScanConfig<'config> {
    /// Strings kept, at most.
    pub wanted: usize,
    /// Admits only the strings it approves; every string by default.
    pub matching: Matching<'config>,
    /// In what order the strings come.
    pub mode: ScanMode,
    /// Reports the scans finished and stops wherever it says to.
    pub progress: Option<&'config mut ProgressCallback<'config>>,
}

impl From<usize> for StringsScanConfig<'_> {
    fn from(wanted: usize) -> Self {
        StringsScanConfig {
            wanted,
            matching: Matching::default(),
            mode: ScanMode::Ordered,
            progress: None,
        }
    }
}

/// The strings a sample draws; see [`On::strings_sample`].
#[derive(Clone, Copy, Debug)]
pub struct StringsSampleConfig {
    /// Strings drawn, at most.
    pub wanted: usize,
}

impl From<usize> for StringsSampleConfig {
    fn from(wanted: usize) -> Self {
        StringsSampleConfig { wanted }
    }
}

impl Accepts<StringsScanConfig<'static>> for Deferring {}

/// Keeps the inputs of the strings verbs closed over the forms this module names.
mod sealed {
    use super::*;

    /// The strings one batch carries: borrowed `&str` slices, or an owned tape of them.
    pub trait Texts {
        /// How many strings there are and their view, pointing into `self`, or `None` for no
        /// strings at all.
        fn texts(&self) -> Result<Option<(usize, ustore_strings_view_t)>>;
    }
}

use sealed::Texts;

impl Texts for &[&str] {
    fn texts(&self) -> Result<Option<(usize, ustore_strings_view_t)>> {
        Ok((!self.is_empty()).then(|| {
            (
                self.len(),
                ustore_strings_view_t::accessor(string_views, self.as_ptr().cast()),
            )
        }))
    }
}

impl<const COUNT: usize> Texts for &[&str; COUNT] {
    fn texts(&self) -> Result<Option<(usize, ustore_strings_view_t)>> {
        self.as_slice().texts()
    }
}

impl<Offset: TapeOffset> Texts for CharsTape<Offset> {
    fn texts(&self) -> Result<Option<(usize, ustore_strings_view_t)>> {
        let (bytes, offsets) = self.arrow_slices();
        Ok((!self.is_empty()).then(|| (self.len(), Offset::tape(bytes, offsets))))
    }
}

/// Which strings of one collection a walk covers: those starting with `prefix`, from `start`,
/// inclusive, up to `end`, exclusive, in the order of their bytes or the reverse.
///
/// A string converts into the ascending range of the strings starting with it.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub struct TextRange<'text> {
    /// What every string starts with, or `None` for every string.
    pub prefix: Option<&'text str>,
    /// The string the walk starts from, or `None` for the first one in its direction.
    pub start: Option<&'text str>,
    /// The string the walk stops before, or `None` to walk to the last one in its direction.
    pub end: Option<&'text str>,
    /// Which way the walk goes from `start`.
    pub direction: ScanDirection,
}

impl<'text> From<&'text str> for TextRange<'text> {
    fn from(prefix: &'text str) -> Self {
        TextRange {
            prefix: Some(prefix),
            ..TextRange::default()
        }
    }
}

/// The view of the one string `text` holds, or a zeroed one when there is none.
fn lent(text: &Option<&str>) -> ustore_strings_view_t {
    text.as_ref().map_or_else(ustore_strings_view_t::default, |text| {
        ustore_strings_view_t::accessor(string_views, ptr::from_ref(text).cast())
    })
}

/// The keys one read found, addressed by the position of their string in the batch.
#[derive(Clone, Copy, Debug)]
pub struct Mapped<'arena> {
    presences: Bitmap<'arena>,
    keys: &'arena [Key],
}

impl<'arena> Mapped<'arena> {
    /// How many strings the read covered.
    pub fn len(&self) -> usize {
        self.presences.len()
    }

    /// Whether the read covered no strings at all.
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// The key of the string at `index`, or `None` for a string the database does not hold.
    pub fn get(&self, index: usize) -> Option<Key> {
        match self.presences.get(index) {
            true => self.keys.get(index).copied(),
            false => None,
        }
    }

    /// Every key in the order its string was asked for.
    pub fn iter(&self) -> Rows<Mapped<'arena>, Option<Key>> {
        Rows::new(*self, self.len(), Mapped::get)
    }

    /// Which strings the read found, one bit per string as Arrow reads a validity buffer.
    pub fn presences(&self) -> Bitmap<'arena> {
        self.presences
    }
}

/// The strings one scan or sample listed, as byte runs inside the arena, and the BM25 scores of a
/// scan whose patterns carried weights.
#[derive(Clone, Copy, Debug)]
pub struct Listing<'arena> {
    strings: Strings<'arena>,
    scores: Option<&'arena [f32]>,
}

impl<'arena> Listing<'arena> {
    /// The strings a successful call of one task listed.
    ///
    /// # Safety
    /// The call succeeded, exporting two offsets and the tape `strings` into an arena alive for
    /// `'arena`.
    unsafe fn exported(
        offsets: Address<ustore_length_t>,
        strings: ustore_strings_view_t,
        scores: Option<*const f32>,
    ) -> Result<Self> {
        let offsets = exported_slice(offsets.0, 2);
        let listed = (offsets[1] - offsets[0]) as usize;
        let strings = Strings::exported(strings, offsets[1] as usize)?;
        Ok(Listing {
            strings,
            scores: scores.map(|scores| exported_slice(scores, listed)),
        })
    }

    /// How many strings the call listed.
    pub fn len(&self) -> usize {
        self.strings.len()
    }

    /// Whether the call listed nothing.
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// The string at `index`, or `None` past the end.
    pub fn get(&self, index: usize) -> Option<&'arena [u8]> {
        self.strings.get(index)
    }

    /// Every string in the order the call listed it.
    pub fn iter(&self) -> Rows<Listing<'arena>, &'arena [u8]> {
        Rows::new(*self, self.len(), |listing, index| {
            listing.get(index).unwrap_or_default()
        })
    }

    /// One score per string when the patterns carried weights.
    pub fn scores(&self) -> Option<&'arena [f32]> {
        self.scores
    }
}

impl<Parent: Writable, Completion: crate::db::sealed::Completion> On<'_, Parent, Completion> {
    /// Upserts a batch of strings and the keys they map onto, one key per string.
    ///
    /// The strings are a borrowed `&[&str]`, or an owned `stringtape::CharsTape`, which a deferred
    /// write needs beside owned keys, and the call hands both back. A bare [`CollectionID`] gives
    /// every string the same collection.
    ///
    /// Calls `ustore_strings_write`.
    pub fn strings_write<'collections, Strs: Texts, Keys: AsRef<[Key]>>(
        &self,
        collections: impl Into<Collections<'collections>>,
        strings: Strs,
        keys: Keys,
        durability: Durability,
    ) -> Completion::Answer<StringsWrite<'collections, Strs, Keys>>
    where
        Completion: Accepts<Collections<'collections>> + Accepts<Strs> + Accepts<Keys>,
    {
        let verb = StringsWrite {
            collections: collections.into(),
            strings,
            keys,
            durability,
        };
        Completion::answer(self, verb)
    }

    /// Removes a batch of strings with their keys, whether or not the database holds them, and
    /// hands the strings back.
    ///
    /// Calls `ustore_strings_remove`.
    pub fn strings_remove<'collections, Strs: Texts>(
        &self,
        collections: impl Into<Collections<'collections>>,
        strings: Strs,
        durability: Durability,
    ) -> Completion::Answer<StringsRemove<'collections, Strs>>
    where
        Completion: Accepts<Collections<'collections>> + Accepts<Strs>,
    {
        let verb = StringsRemove {
            collections: collections.into(),
            strings,
            durability,
        };
        Completion::answer(self, verb)
    }
}

impl<Parent, Completion: crate::db::sealed::Completion> On<'_, Parent, Completion> {
    /// Reads the keys a batch of strings map onto into the arena, one collection per string or one
    /// for all of them.
    ///
    /// Calls `ustore_strings_read`.
    pub fn strings_read<'arena, 'collections, Strs: Texts>(
        &self,
        collections: impl Into<Collections<'collections>>,
        strings: Strs,
        arena: &'arena mut Arena,
    ) -> Completion::Answer<StringsRead<'arena, 'collections, Strs>>
    where
        Completion: Accepts<Collections<'collections>> + Accepts<Strs>,
    {
        let verb = StringsRead {
            collections: collections.into(),
            strings,
            arena,
            count: 0,
            presences: Address::default(),
            keys: Address::default(),
        };
        Completion::answer(self, verb)
    }

    /// Lists up to `wanted` strings of one collection's `range` into the arena, keeping those its
    /// `matching` admits. Weighted patterns write one BM25 score per string beside them.
    ///
    /// Calls `ustore_strings_scan`.
    pub fn strings_scan<'config, 'arena, 'text>(
        &self,
        collection: CollectionID,
        range: impl Into<TextRange<'text>>,
        config: impl Into<StringsScanConfig<'config>>,
        arena: &'arena mut Arena,
    ) -> Completion::Answer<StringsScan<'arena, 'text, 'config>>
    where
        Completion: Accepts<TextRange<'text>> + Accepts<StringsScanConfig<'config>>,
    {
        let StringsScanConfig {
            wanted,
            matching,
            mode,
            progress,
        } = config.into();
        let verb = StringsScan {
            collection,
            range: range.into(),
            arena,
            wanted,
            matched: Matched::new(matching),
            mode,
            progress: Progress::new(progress),
            offsets: Address::default(),
            strings: ustore_strings_view_t::default(),
            scores: Address::default(),
        };
        Completion::answer(self, verb)
    }

    /// Draws up to `wanted` strings uniformly at random from one collection into the arena.
    ///
    /// Calls `ustore_strings_sample`.
    pub fn strings_sample<'arena>(
        &self,
        collection: CollectionID,
        config: impl Into<StringsSampleConfig>,
        arena: &'arena mut Arena,
    ) -> Completion::Answer<StringsSample<'arena>> {
        let verb = StringsSample {
            collection,
            arena,
            wanted: config.into().wanted,
            offsets: Address::default(),
            strings: ustore_strings_view_t::default(),
        };
        Completion::answer(self, verb)
    }

    /// Estimates the strings and the space between two strings of one collection, through the arena
    /// its bounds land in.
    ///
    /// Calls `ustore_strings_measure`.
    pub fn strings_measure<'arena, 'text>(
        &self,
        collection: CollectionID,
        range: Range<&'text str>,
        arena: &'arena mut Arena,
    ) -> Completion::Answer<StringsMeasure<'arena, 'text>>
    where
        Completion: Accepts<Range<&'text str>>,
    {
        let verb = StringsMeasure {
            collection,
            range: (Some(range.start), Some(range.end)),
            arena,
            bounds: Bounds::default(),
        };
        Completion::answer(self, verb)
    }
}

/// An upsert of a batch of strings; see [`On::strings_write`].
#[derive(Debug)]
pub struct StringsWrite<'collections, Strs, Keys> {
    collections: Collections<'collections>,
    strings: Strs,
    keys: Keys,
    durability: Durability,
}

impl<Strs: Texts, Keys: AsRef<[Key]>> Calling for StringsWrite<'_, Strs, Keys> {
    type Arguments = ffi::ustore_strings_write_t;
    type Output = (Strs, Keys);

    fn arguments<Parent, Completion>(
        &mut self,
        view: &On<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let keys = self.keys.as_ref();
        let Some((count, strings)) = self.strings.texts()? else {
            return Ok(None);
        };
        if keys.len() != count {
            return Err(fail(Status::InvalidArgument, "the batch holds a key per string"));
        }
        Ok(Some(ffi::ustore_strings_write_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            transaction: view.transaction(),
            count: count as ustore_size_t,
            collections: self.collections.view(count)?,
            strings,
            values: ustore_strided_view_t::packed(keys),
            durability: self.durability,
            watch: view.watch(),
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_strings_write(arguments)
    }

    fn finish(self) -> Result<(Strs, Keys)> {
        Ok((self.strings, self.keys))
    }
}

/// A removal of a batch of strings; see [`On::strings_remove`].
#[derive(Debug)]
pub struct StringsRemove<'collections, Strs> {
    collections: Collections<'collections>,
    strings: Strs,
    durability: Durability,
}

impl<Strs: Texts> Calling for StringsRemove<'_, Strs> {
    type Arguments = ffi::ustore_strings_remove_t;
    type Output = Strs;

    fn arguments<Parent, Completion>(
        &mut self,
        view: &On<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let Some((count, strings)) = self.strings.texts()? else {
            return Ok(None);
        };
        Ok(Some(ffi::ustore_strings_remove_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            transaction: view.transaction(),
            count: count as ustore_size_t,
            collections: self.collections.view(count)?,
            strings,
            end_strings: ustore_strings_view_t::default(),
            durability: self.durability,
            watch: view.watch(),
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_strings_remove(arguments)
    }

    fn finish(self) -> Result<Strs> {
        Ok(self.strings)
    }
}

/// A read of the keys of a batch of strings; see [`On::strings_read`].
#[derive(Debug)]
pub struct StringsRead<'arena, 'collections, Strs> {
    collections: Collections<'collections>,
    strings: Strs,
    arena: &'arena mut Arena,
    count: usize,
    presences: Address<ustore_octet_t>,
    keys: Address<Key>,
}

impl<'arena, Strs: Texts> Calling for StringsRead<'arena, '_, Strs> {
    type Arguments = ffi::ustore_strings_read_t;
    type Output = Mapped<'arena>;

    fn arguments<Parent, Completion>(
        &mut self,
        view: &On<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let Some((count, strings)) = self.strings.texts()? else {
            return Ok(None);
        };
        self.count = count;
        Ok(Some(ffi::ustore_strings_read_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            view: view.view(),
            count: count as ustore_size_t,
            collections: self.collections.view(count)?,
            strings,
            presences: &mut self.presences.0,
            values: &mut self.keys.0,
            arena: self.arena.handle,
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_strings_read(arguments)
    }

    fn finish(self) -> Result<Mapped<'arena>> {
        self.arena.host()?;
        if self.count == 0 {
            return Ok(Mapped {
                presences: Bitmap::new(&[], 0),
                keys: &[],
            });
        }
        // SAFETY: the read succeeded over every string, exporting a presence bit and a key for each
        // into the arena, which stays borrowed for `'arena`.
        unsafe {
            Ok(Mapped {
                presences: Bitmap::new(exported_slice(self.presences.0, self.count.div_ceil(8)), self.count),
                keys: exported_slice(self.keys.0, self.count),
            })
        }
    }
}

/// A walk over the strings of one collection; see [`On::strings_scan`].
#[derive(Debug)]
pub struct StringsScan<'arena, 'text, 'matching> {
    collection: CollectionID,
    range: TextRange<'text>,
    arena: &'arena mut Arena,
    wanted: usize,
    matched: Matched<'matching>,
    mode: ScanMode,
    progress: Progress<'matching>,
    offsets: Address<ustore_length_t>,
    strings: ustore_strings_view_t,
    scores: Address<f32>,
}

impl<'arena> Calling for StringsScan<'arena, '_, '_> {
    type Arguments = ffi::ustore_strings_scan_t;
    type Output = Listing<'arena>;

    fn arguments<Parent, Completion>(
        &mut self,
        view: &On<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let (progress, progress_state) = self.progress.lowered();
        Ok(Some(ffi::ustore_strings_scan_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            view: view.view(),
            count: 1,
            collections: ustore_strided_view_t::repeated(&self.collection),
            prefixes: lent(&self.range.prefix),
            start_strings: lent(&self.range.start),
            end_strings: lent(&self.range.end),
            direction: self.range.direction,
            patterns: self.matched.patterns(),
            predicate: self.matched.predicate(),
            offsets: &mut self.offsets.0,
            strings: &mut self.strings,
            scores: match self.matched.scores() {
                true => &mut self.scores.0,
                false => ptr::null_mut(),
            },
            arena: self.arena.handle,
            wanted: self.wanted as ustore_size_t,
            mode: self.mode,
            progress,
            progress_state,
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_strings_scan(arguments)
    }

    fn raise(&mut self) {
        self.matched.raise();
        self.progress.raise();
    }

    fn finish(self) -> Result<Listing<'arena>> {
        self.arena.host()?;
        // SAFETY: the scan succeeded, and the arena stays borrowed for `'arena`; a scan with
        // weighted patterns wrote one score per string into the same arena.
        unsafe {
            Listing::exported(
                self.offsets,
                self.strings,
                self.matched.scores().then_some(self.scores.0),
            )
        }
    }
}

/// A random draw of strings from one collection; see [`On::strings_sample`].
#[derive(Debug)]
pub struct StringsSample<'arena> {
    collection: CollectionID,
    arena: &'arena mut Arena,
    wanted: usize,
    offsets: Address<ustore_length_t>,
    strings: ustore_strings_view_t,
}

impl<'arena> Calling for StringsSample<'arena> {
    type Arguments = ffi::ustore_strings_sample_t;
    type Output = Listing<'arena>;

    fn arguments<Parent, Completion>(
        &mut self,
        view: &On<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        Ok(Some(ffi::ustore_strings_sample_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            view: view.view(),
            count: 1,
            collections: ustore_strided_view_t::repeated(&self.collection),
            offsets: &mut self.offsets.0,
            strings: &mut self.strings,
            arena: self.arena.handle,
            wanted: self.wanted as ustore_size_t,
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_strings_sample(arguments)
    }

    fn finish(self) -> Result<Listing<'arena>> {
        self.arena.host()?;
        // SAFETY: the sample succeeded, and the arena stays borrowed for `'arena`.
        unsafe { Listing::exported(self.offsets, self.strings, None) }
    }
}

/// An estimate of what a range of strings holds; see [`On::strings_measure`].
#[derive(Debug)]
pub struct StringsMeasure<'arena, 'text> {
    collection: CollectionID,
    range: (Option<&'text str>, Option<&'text str>),
    arena: &'arena mut Arena,
    bounds: Bounds,
}

impl Calling for StringsMeasure<'_, '_> {
    type Arguments = ffi::ustore_strings_measure_t;
    type Output = Measurement;

    fn arguments<Parent, Completion>(
        &mut self,
        view: &On<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let [min_cardinalities, max_cardinalities, min_value_bytes, max_value_bytes, min_space_usages, max_space_usages] =
            &mut self.bounds.0;
        Ok(Some(ffi::ustore_strings_measure_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            view: view.view(),
            count: 1,
            collections: ustore_strided_view_t::repeated(&self.collection),
            start_strings: lent(&self.range.0),
            end_strings: lent(&self.range.1),
            direction: ScanDirection::Ascending,
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
        ffi::ustore_strings_measure(arguments)
    }

    fn finish(self) -> Result<Measurement> {
        self.arena.host()?;
        // SAFETY: the measurement of one range succeeded.
        Ok(unsafe { self.bounds.measurement() })
    }
}

// endregion: Strings

// region: Names

/// Creates a collection under `name`, which no other collection may carry, answering its identifier.
pub(crate) fn create_named(database: ustore_database_t, name: &str, modality: Modality) -> Result<CollectionID> {
    let mut collection = CollectionID::default();
    let mut outcome = ustore_outcome_t::default();
    let mut arguments = db_ffi::ustore_collection_create_t {
        database,
        executor: ptr::null_mut(),
        outcome: &mut outcome,
        completion: ustore_completion_t::Blocking,
        name: name.as_bytes().into(),
        modality: modality as ustore_enum_t,
        collection: &mut collection,
    };
    // SAFETY: the name and the output outlive the call, which is final on return.
    unsafe { db_ffi::ustore_collection_create(&mut arguments) };
    check(outcome)?;
    Ok(collection)
}

impl<Parent, Completion: crate::db::sealed::Completion> On<'_, Parent, Completion> {
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

/// A lookup of a collection by its name; see [`On::collection_find`].
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
    type Arguments = db_ffi::ustore_collection_find_t;
    type Output = (CollectionID, Modality);

    fn arguments<Parent, Completion>(
        &mut self,
        view: &On<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let name = self.name.as_ref();
        self.offsets = [0, name.len() as u64];
        Ok(Some(db_ffi::ustore_collection_find_t {
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
        db_ffi::ustore_collection_find(arguments)
    }

    fn finish(self) -> Result<(CollectionID, Modality)> {
        self.arena.host()?;
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

/// A listing of collection names; see [`On::collection_names`].
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
    type Arguments = db_ffi::ustore_collection_list_t;
    type Output = Names<'arena>;

    fn arguments<Parent, Completion>(
        &mut self,
        view: &On<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        Ok(Some(db_ffi::ustore_collection_list_t {
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
        db_ffi::ustore_collection_list(arguments)
    }

    fn finish(self) -> Result<Names<'arena>> {
        self.arena.host()?;
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
    use crate::testing::{created, key, keys};

    /// A database holding three named collections.
    fn three_named() -> (Database, Arena) {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        for name in ["people", "places", "planets"] {
            database.collection_create(name, Modality::Blobs).unwrap();
        }
        (database, Arena::new().unwrap())
    }

    /// A strings collection mapping `apple`, `apricot` and `banana` onto the keys one, two and
    /// three.
    fn populated() -> (Database, CollectionID, Arena) {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let strings = created(&database, Modality::Strings);
        database
            .head()
            .strings_write(
                strings,
                &["apple", "apricot", "banana"],
                keys([1, 2, 3]),
                Durability::Buffered,
            )
            .unwrap();
        (database, strings, Arena::new().unwrap())
    }

    #[test]
    fn strings_read_back_their_keys_and_report_the_missing() {
        let (database, strings, mut arena) = populated();
        let mapped = database
            .head()
            .strings_read(strings, &["banana", "cherry", "apple"], &mut arena)
            .unwrap();
        assert_eq!(
            mapped.iter().collect::<Vec<_>>(),
            vec![Some(key(3)), None, Some(key(1))]
        );
        assert!(!mapped.presences().get(1));

        database
            .head()
            .strings_remove(strings, &["apple", "cherry"], Durability::Buffered)
            .unwrap();
        let mapped = database
            .head()
            .strings_read(strings, &["apple", "apricot"], &mut arena)
            .unwrap();
        assert_eq!(mapped.iter().collect::<Vec<_>>(), vec![None, Some(key(2))]);
    }

    #[test]
    fn strings_scan_by_prefix_in_either_direction_and_resume_from_a_start() {
        let (database, strings, mut arena) = populated();
        let head = database.head();
        let under = head.strings_scan(strings, "ap", 10, &mut arena).unwrap();
        assert_eq!(under.iter().collect::<Vec<_>>(), vec![b"apple".as_slice(), b"apricot"]);

        let backwards = TextRange {
            direction: ScanDirection::Descending,
            ..TextRange::default()
        };
        let listed = head.strings_scan(strings, backwards, 2, &mut arena).unwrap();
        assert_eq!(
            listed.iter().collect::<Vec<_>>(),
            vec![b"banana".as_slice(), b"apricot"]
        );

        let resumed = TextRange {
            start: Some("apricot"),
            ..TextRange::default()
        };
        let listed = head.strings_scan(strings, resumed, 10, &mut arena).unwrap();
        assert_eq!(listed.get(0), Some(b"apricot".as_slice()));
        assert_eq!(listed.len(), 2);
    }

    #[test]
    fn strings_sample_and_measure_cover_the_collection() {
        let (database, strings, mut arena) = populated();
        let head = database.head();
        assert_eq!(head.strings_sample(strings, 8, &mut arena).unwrap().len(), 3);
        let measured = head.strings_measure(strings, "a".."b", &mut arena).unwrap();
        assert!(measured.min_cardinality <= 2 && measured.max_cardinality >= 2);
    }

    #[test]
    fn strings_cross_a_deferred_view_as_an_owned_tape() {
        let (database, strings, mut arena) = populated();
        let executor = Executor::for_ram(&[], 1).unwrap();
        let deferred = database.head().deferred(&executor);
        let texts: CharsTape<u64> = ["cherry", "date"].into_iter().collect();
        deferred
            .strings_write(strings, texts, keys([4, 5]), Durability::Buffered)
            .unwrap()
            .wait()
            .unwrap();
        let texts: CharsTape<u64> = ["date", "fig"].into_iter().collect();
        let mapped = deferred
            .strings_read(strings, texts, &mut arena)
            .unwrap()
            .wait()
            .unwrap();
        assert_eq!(mapped.iter().collect::<Vec<_>>(), vec![Some(key(5)), None]);
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
        let executor = Executor::for_ram(&[], 1).unwrap();
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
        let executor = Executor::for_ram(&[], 1).unwrap();
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
