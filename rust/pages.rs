//! Pages - the Rust twin of `c/ustore/pages.h`.
//!
//! A page is an ordered tree of elements addressed by key, where the page key doubles as the key of
//! its root. Imports replace whole pages, small edits insert, remove, move, splice or set
//! attributes of elements, and reads render pages or subtrees back as markup. Scans and a
//! [`PagesCursor`] list the keys of one pages collection, keeping those its XPath filter matches.
//!
//! File: rust/pages.rs
//! Author: Ash Vardanian
//! Date: October 27, 2026

use std::ffi::c_char;
use std::fmt;
use std::marker::PhantomData;
use std::ptr;
use std::str::FromStr;

use crate::blobs::{Bounds, Found, Measurement, Page, Values};
use crate::db::ffi::{
    ustore_arena_t, ustore_bytes_view_t, ustore_collection_id_t, ustore_collections_view_t, ustore_completion_t,
    ustore_database_t, ustore_durability_t, ustore_enum_t, ustore_executor_t, ustore_key_t, ustore_keys_view_t,
    ustore_length_t, ustore_lengths_view_t, ustore_octet_t, ustore_outcome_t, ustore_predicate_t,
    ustore_progress_callback_t, ustore_scan_direction_t, ustore_scan_mode_t, ustore_size_t, ustore_strided_view_t,
    ustore_strings_view_t, ustore_transaction_t, ustore_view_t, ustore_watch_t,
};
use crate::db::sealed::{Accepts, Calling, Completion, Handles, Writable};
use crate::db::{
    Address, Arena, CollectionID, Collections, CursorOn, Deferrable, Deferring, Durability, Error, Executor, Key,
    KeyRange, Matching, On, Progress, Result, ScanMode, Status, Strings, ViewVerbs, check, executor_handle,
    exported_slice, fail, lowered_progress, read_spelling, spelling, string_views,
};

/// The encoding pages render in, as `ustore_page_encoding_t` spells it.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub enum PageEncoding {
    /// HTML, rendered back as markup.
    Html = 0,
    /// XML, with every element and attribute kept as written.
    #[default]
    Xml = 1,
    /// Markdown.
    Markdown = 2,
    /// The text of every run in document order.
    Text = 3,
    /// The element tree as JSON.
    Json = 4,
}

impl PageEncoding {
    fn from_raw(raw: ustore_enum_t) -> Option<Self> {
        match raw {
            0 => Some(PageEncoding::Html),
            1 => Some(PageEncoding::Xml),
            2 => Some(PageEncoding::Markdown),
            3 => Some(PageEncoding::Text),
            4 => Some(PageEncoding::Json),
            _ => None,
        }
    }
}

impl fmt::Display for PageEncoding {
    /// Calls `ustore_page_encoding_name`.
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(spelling(ffi::ustore_page_encoding_name, *self as ustore_enum_t))
    }
}

impl FromStr for PageEncoding {
    type Err = Error;

    /// Calls `ustore_page_encoding_from_name`.
    fn from_str(text: &str) -> Result<Self> {
        read_spelling(text, ffi::ustore_page_encoding_from_name, PageEncoding::from_raw)
    }
}

// region: Raw Declarations

/// One-to-one mirror of `c/ustore/pages.h`.
pub(crate) mod ffi {
    #![allow(non_camel_case_types)]

    use std::ffi::c_void;

    use super::*;

    #[repr(C)]
    pub struct ustore_pages_cursor_handle_t {
        _private: [u8; 0],
    }

    pub type ustore_pages_cursor_t = *mut ustore_pages_cursor_handle_t;

    #[repr(C)]
    pub struct ustore_pages_cursor_init_t {
        pub cursor: *mut ustore_pages_cursor_t,
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub view: ustore_view_t,
        pub collection: ustore_collection_id_t,
        pub start_key: ustore_key_t,
        pub end_key: *const ustore_key_t,
        pub direction: ustore_scan_direction_t,
        pub filter: ustore_bytes_view_t,
        pub predicate: ustore_predicate_t,
    }

    #[repr(C)]
    pub struct ustore_pages_cursor_next_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub cursor: ustore_pages_cursor_t,
        pub keys: *mut *const ustore_key_t,
        pub values: *mut ustore_strings_view_t,
        pub written: *mut ustore_size_t,
        pub arena: ustore_arena_t,
        pub wanted: ustore_size_t,
        pub outputs_encoding: ustore_enum_t,
        pub progress: ustore_progress_callback_t,
        pub progress_state: *mut c_void,
    }

    #[repr(C)]
    pub struct ustore_pages_import_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub transaction: ustore_transaction_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub keys: ustore_keys_view_t,
        pub values: ustore_strings_view_t,
        pub encoding: ustore_enum_t,
        pub durability: ustore_durability_t,
        pub watch: ustore_watch_t,
        pub progress: ustore_progress_callback_t,
        pub progress_state: *mut c_void,
    }

    #[repr(C)]
    pub struct ustore_pages_insert_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub transaction: ustore_transaction_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub keys: ustore_keys_view_t,
        pub elements: ustore_keys_view_t,
        pub parents: ustore_keys_view_t,
        pub anchors: ustore_keys_view_t,
        pub values: ustore_strings_view_t,
        pub encoding: ustore_enum_t,
        pub effective_keys: *mut *const ustore_key_t,
        pub arena: ustore_arena_t,
        pub durability: ustore_durability_t,
        pub watch: ustore_watch_t,
    }

    #[repr(C)]
    pub struct ustore_pages_remove_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub transaction: ustore_transaction_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub keys: ustore_keys_view_t,
        pub end_keys: ustore_keys_view_t,
        pub elements: ustore_keys_view_t,
        pub durability: ustore_durability_t,
        pub watch: ustore_watch_t,
    }

    #[repr(C)]
    pub struct ustore_pages_move_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub transaction: ustore_transaction_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub keys: ustore_keys_view_t,
        pub elements: ustore_keys_view_t,
        pub parents: ustore_keys_view_t,
        pub anchors: ustore_keys_view_t,
        pub durability: ustore_durability_t,
        pub watch: ustore_watch_t,
    }

    #[repr(C)]
    pub struct ustore_pages_splice_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub transaction: ustore_transaction_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub keys: ustore_keys_view_t,
        pub elements: ustore_keys_view_t,
        pub text_starts: ustore_lengths_view_t,
        pub text_lengths: ustore_lengths_view_t,
        pub values: ustore_strings_view_t,
        pub durability: ustore_durability_t,
        pub watch: ustore_watch_t,
    }

    #[repr(C)]
    pub struct ustore_pages_set_attributes_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub transaction: ustore_transaction_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub keys: ustore_keys_view_t,
        pub elements: ustore_keys_view_t,
        pub fields: ustore_strings_view_t,
        pub values: ustore_strings_view_t,
        pub durability: ustore_durability_t,
        pub watch: ustore_watch_t,
    }

    #[repr(C)]
    pub struct ustore_pages_remove_attributes_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub transaction: ustore_transaction_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub keys: ustore_keys_view_t,
        pub elements: ustore_keys_view_t,
        pub fields: ustore_strings_view_t,
        pub durability: ustore_durability_t,
        pub watch: ustore_watch_t,
    }

    #[repr(C)]
    pub struct ustore_pages_read_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub view: ustore_view_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub keys: ustore_keys_view_t,
        pub elements: ustore_keys_view_t,
        pub presences: *mut *const ustore_octet_t,
        pub lengths: *mut *const ustore_length_t,
        pub values: *mut ustore_strings_view_t,
        pub arena: ustore_arena_t,
        pub outputs_encoding: ustore_enum_t,
    }

    #[repr(C)]
    pub struct ustore_pages_scan_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub view: ustore_view_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub start_keys: ustore_keys_view_t,
        pub end_keys: ustore_keys_view_t,
        pub direction: ustore_scan_direction_t,
        pub filter: ustore_bytes_view_t,
        pub predicate: ustore_predicate_t,
        pub offsets: *mut *const ustore_length_t,
        pub keys: *mut *const ustore_key_t,
        pub arena: ustore_arena_t,
        pub wanted: ustore_size_t,
        pub mode: ustore_scan_mode_t,
        pub progress: ustore_progress_callback_t,
        pub progress_state: *mut c_void,
    }

    #[repr(C)]
    pub struct ustore_pages_sample_t {
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
    pub struct ustore_pages_measure_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub view: ustore_view_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub start_keys: ustore_keys_view_t,
        pub end_keys: ustore_keys_view_t,
        pub direction: ustore_scan_direction_t,
        pub min_cardinalities: *mut *const ustore_size_t,
        pub max_cardinalities: *mut *const ustore_size_t,
        pub min_value_bytes: *mut *const ustore_size_t,
        pub max_value_bytes: *mut *const ustore_size_t,
        pub min_space_usages: *mut *const ustore_size_t,
        pub max_space_usages: *mut *const ustore_size_t,
        pub arena: ustore_arena_t,
    }

    unsafe extern "C" {
        pub fn ustore_pages_cursor_init(arguments: *mut ustore_pages_cursor_init_t);
        pub fn ustore_pages_cursor_next(arguments: *mut ustore_pages_cursor_next_t);
        pub fn ustore_pages_cursor_free(cursor: ustore_pages_cursor_t);
        pub fn ustore_pages_import(arguments: *mut ustore_pages_import_t);
        pub fn ustore_pages_insert(arguments: *mut ustore_pages_insert_t);
        pub fn ustore_pages_remove(arguments: *mut ustore_pages_remove_t);
        pub fn ustore_pages_move(arguments: *mut ustore_pages_move_t);
        pub fn ustore_pages_splice(arguments: *mut ustore_pages_splice_t);
        pub fn ustore_pages_set_attributes(arguments: *mut ustore_pages_set_attributes_t);
        pub fn ustore_pages_remove_attributes(arguments: *mut ustore_pages_remove_attributes_t);
        pub fn ustore_pages_read(arguments: *mut ustore_pages_read_t);
        pub fn ustore_pages_scan(arguments: *mut ustore_pages_scan_t);
        pub fn ustore_pages_sample(arguments: *mut ustore_pages_sample_t);
        pub fn ustore_pages_measure(arguments: *mut ustore_pages_measure_t);
        pub fn ustore_page_encoding_name(encoding: ustore_enum_t) -> *const c_char;
        pub fn ustore_page_encoding_from_name(
            name: ustore_bytes_view_t,
            encoding: *mut ustore_enum_t,
        ) -> crate::db::ffi::ustore_status_t;
    }
}

// endregion: Raw Declarations

// region: Inputs

/// The markup imported pages arrive in and how the import reports, as
/// [`PagesVerbs::pages_import`] takes them.
pub struct PagesImportConfig<'config> {
    /// The encoding of the markup.
    pub encoding: PageEncoding,
    /// Reports the pages imported and stops wherever it says to.
    pub progress: Option<Progress<'config>>,
}

impl From<PageEncoding> for PagesImportConfig<'_> {
    fn from(encoding: PageEncoding) -> Self {
        PagesImportConfig { encoding, progress: None }
    }
}

/// Where inserted elements land and which keys they take; see [`PagesVerbs::pages_insert`].
#[derive(Clone, Copy, Debug)]
pub struct PagesInsertConfig<'config> {
    /// The key of the first element of every fragment; minted in every task when empty.
    pub elements: &'config [Key],
    /// The element every fragment lands under; the root in every task when empty.
    pub parents: &'config [Key],
    /// The sibling every fragment lands after; the front in every task when empty.
    pub anchors: &'config [Key],
    /// The encoding of the fragments.
    pub encoding: PageEncoding,
}

impl From<PageEncoding> for PagesInsertConfig<'_> {
    fn from(encoding: PageEncoding) -> Self {
        PagesInsertConfig { elements: &[], parents: &[], anchors: &[], encoding }
    }
}

/// What a page read returns and how; see [`PagesVerbs::pages_read`].
#[derive(Clone, Copy, Debug)]
pub struct PagesReadConfig<'config> {
    /// Only these elements of each page; every element when empty.
    pub elements: &'config [Key],
    /// The markup the fragments come back in.
    pub outputs_encoding: PageEncoding,
}

impl From<PageEncoding> for PagesReadConfig<'_> {
    fn from(outputs_encoding: PageEncoding) -> Self {
        PagesReadConfig { elements: &[], outputs_encoding }
    }
}

/// The page keys a scan keeps, what admits them and how it reports; see [`PagesVerbs::pages_scan`].
pub struct PagesScanConfig<'config> {
    /// Page keys kept, at most.
    pub wanted: usize,
    /// Admits only the pages its filter and predicate approve; every page by default.
    pub matching: Matching<'config>,
    /// In what order the page keys come.
    pub mode: ScanMode,
    /// Reports the scans finished and stops wherever it says to.
    pub progress: Option<Progress<'config>>,
}

impl From<usize> for PagesScanConfig<'_> {
    fn from(wanted: usize) -> Self {
        PagesScanConfig { wanted, matching: Matching::default(), mode: ScanMode::Ordered, progress: None }
    }
}

/// The page keys a sample draws; see [`PagesVerbs::pages_sample`].
#[derive(Clone, Copy, Debug)]
pub struct PagesSampleConfig {
    /// Page keys drawn, at most.
    pub wanted: usize,
}

impl From<usize> for PagesSampleConfig {
    fn from(wanted: usize) -> Self {
        PagesSampleConfig { wanted }
    }
}

/// What admits the page keys a cursor hands out; see [`On::pages_cursor`].
#[derive(Debug, Default)]
pub struct PagesCursorConfig<'config> {
    /// Admits only the pages its filter and predicate approve; every page by default.
    pub matching: Matching<'config>,
}

impl<'config> From<Matching<'config>> for PagesCursorConfig<'config> {
    fn from(matching: Matching<'config>) -> Self {
        PagesCursorConfig { matching }
    }
}

impl Accepts<PagesImportConfig<'static>> for Deferring {}
impl Accepts<PagesInsertConfig<'static>> for Deferring {}
impl Accepts<PagesReadConfig<'static>> for Deferring {}
impl Accepts<PagesScanConfig<'static>> for Deferring {}
impl Accepts<Vec<&'static str>> for Deferring {}
impl<const COUNT: usize> Accepts<[&'static str; COUNT]> for Deferring {}
impl Accepts<Vec<ustore_length_t>> for Deferring {}
impl<const COUNT: usize> Accepts<[ustore_length_t; COUNT]> for Deferring {}

/// The column of a batch of `count` tasks, or a view without data for an empty one, which the
/// library reads as Nil or zero in every task.
fn column<Value>(values: &[Value], count: usize) -> Result<ustore_strided_view_t<Value>> {
    match values.len() {
        0 => Ok(ustore_strided_view_t::default()),
        length if length == count => Ok(ustore_strided_view_t::packed(values)),
        _ => Err(fail(Status::InvalidArgument, "every column of a batch holds one item per task")),
    }
}

/// The strings of a batch of `count` tasks, one per task.
fn texts(texts: &[&str], count: usize) -> Result<ustore_strings_view_t> {
    match texts.len() == count {
        true => Ok(ustore_strings_view_t::accessor(string_views, texts.as_ptr().cast())),
        false => Err(fail(Status::InvalidArgument, "every column of a batch holds one item per task")),
    }
}

// endregion: Inputs

// region: Verbs

/// The verbs over pages, written once for every receiver of [`ViewVerbs`]: a handle blocks on the
/// calling thread, and an [`On`] runs on its executor, blocking or deferred.
///
/// Rust-only.
pub trait PagesVerbs<'view>: ViewVerbs<'view> {
    /// Replaces whole pages with parsed markup, one per key, minting a key for every element.
    ///
    /// Calls `ustore_pages_import`.
    fn pages_import<'collections, 'config, 'texts, Keys: AsRef<[Key]>, Markups: AsRef<[&'texts str]>>(
        self,
        collections: impl Into<Collections<'collections>>,
        keys: Keys,
        markups: Markups,
        config: impl Into<PagesImportConfig<'config>>,
        durability: Durability,
    ) -> Deferrable<'view, Self, PagesImport<'collections, 'config, Keys, Markups>>
    where
        Self::Parent: Writable,
        Self::Completion:
            Accepts<Collections<'collections>> + Accepts<Keys> + Accepts<Markups> + Accepts<PagesImportConfig<'config>>,
    {
        let PagesImportConfig { encoding, progress } = config.into();
        let verb = PagesImport { collections: collections.into(), keys, markups, encoding, progress, durability };
        <Self::Completion as Completion>::answer(&self.receiver(), verb)
    }

    /// Parses one fragment per task into elements under a parent after an anchor of the page `keys`
    /// names, and hands back the key every first element ended up with, given or minted, from the
    /// arena.
    ///
    /// Calls `ustore_pages_insert`.
    fn pages_insert<'arena, 'collections, 'config, 'texts, Keys: AsRef<[Key]>, Fragments: AsRef<[&'texts str]>>(
        self,
        collections: impl Into<Collections<'collections>>,
        keys: Keys,
        fragments: Fragments,
        config: impl Into<PagesInsertConfig<'config>>,
        arena: &'arena mut Arena<'_>,
        durability: Durability,
    ) -> Deferrable<'view, Self, PagesInsert<'arena, 'collections, 'config, Keys, Fragments>>
    where
        Self::Parent: Writable,
        Self::Completion: Accepts<Collections<'collections>>
            + Accepts<Keys>
            + Accepts<Fragments>
            + Accepts<PagesInsertConfig<'config>>,
    {
        let verb = PagesInsert {
            collections: collections.into(),
            keys,
            fragments,
            config: config.into(),
            arena,
            durability,
            effective: Address::default(),
        };
        <Self::Completion as Completion>::answer(&self.receiver(), verb)
    }

    /// Removes one element per task with its subtree, where the Nil element [`Key::default`]
    /// removes the whole page.
    ///
    /// Calls `ustore_pages_remove`.
    fn pages_remove<'collections, Keys: AsRef<[Key]>, Elements: AsRef<[Key]>>(
        self,
        collections: impl Into<Collections<'collections>>,
        keys: Keys,
        elements: Elements,
        durability: Durability,
    ) -> Deferrable<'view, Self, PagesRemove<'collections, Keys, Elements>>
    where
        Self::Parent: Writable,
        Self::Completion: Accepts<Collections<'collections>> + Accepts<Keys> + Accepts<Elements>,
    {
        let verb = PagesRemove { collections: collections.into(), keys, elements, durability };
        <Self::Completion as Completion>::answer(&self.receiver(), verb)
    }

    /// Moves one element per task with its subtree under a parent after an anchor, keeping its key,
    /// where empty `parents` stand for the root and empty `anchors` for the front.
    ///
    /// Calls `ustore_pages_move`.
    fn pages_move<'collections, Keys, Elements, Parents, Anchors>(
        self,
        collections: impl Into<Collections<'collections>>,
        keys: Keys,
        elements: Elements,
        parents: Parents,
        anchors: Anchors,
        durability: Durability,
    ) -> Deferrable<'view, Self, PagesMove<'collections, Keys, Elements, Parents, Anchors>>
    where
        Self::Parent: Writable,
        Keys: AsRef<[Key]>,
        Elements: AsRef<[Key]>,
        Parents: AsRef<[Key]>,
        Anchors: AsRef<[Key]>,
        Self::Completion: Accepts<Collections<'collections>>
            + Accepts<Keys>
            + Accepts<Elements>
            + Accepts<Parents>
            + Accepts<Anchors>,
    {
        let verb = PagesMove { collections: collections.into(), keys, elements, parents, anchors, durability };
        <Self::Completion as Completion>::answer(&self.receiver(), verb)
    }

    /// Replaces `lengths` bytes of the text of one element per task from `starts` on with `texts`,
    /// where a zero length only inserts and an empty text only removes.
    ///
    /// Calls `ustore_pages_splice`.
    #[allow(clippy::too_many_arguments)]
    fn pages_splice<'collections, 'texts, Keys, Elements, Starts, Lengths, Texts>(
        self,
        collections: impl Into<Collections<'collections>>,
        keys: Keys,
        elements: Elements,
        starts: Starts,
        lengths: Lengths,
        texts: Texts,
        durability: Durability,
    ) -> Deferrable<'view, Self, PagesSplice<'collections, Keys, Elements, Starts, Lengths, Texts>>
    where
        Self::Parent: Writable,
        Keys: AsRef<[Key]>,
        Elements: AsRef<[Key]>,
        Starts: AsRef<[ustore_length_t]>,
        Lengths: AsRef<[ustore_length_t]>,
        Texts: AsRef<[&'texts str]>,
        Self::Completion: Accepts<Collections<'collections>>
            + Accepts<Keys>
            + Accepts<Elements>
            + Accepts<Starts>
            + Accepts<Lengths>
            + Accepts<Texts>,
    {
        let verb = PagesSplice { collections: collections.into(), keys, elements, starts, lengths, texts, durability };
        <Self::Completion as Completion>::answer(&self.receiver(), verb)
    }

    /// Sets one attribute of one element per task, adding the ones it lacks.
    ///
    /// Calls `ustore_pages_set_attributes`.
    fn pages_set_attributes<'collections, 'texts, Keys, Elements, Fields, Texts>(
        self,
        collections: impl Into<Collections<'collections>>,
        keys: Keys,
        elements: Elements,
        fields: Fields,
        values: Texts,
        durability: Durability,
    ) -> Deferrable<'view, Self, PagesSetAttributes<'collections, Keys, Elements, Fields, Texts>>
    where
        Self::Parent: Writable,
        Keys: AsRef<[Key]>,
        Elements: AsRef<[Key]>,
        Fields: AsRef<[&'texts str]>,
        Texts: AsRef<[&'texts str]>,
        Self::Completion:
            Accepts<Collections<'collections>> + Accepts<Keys> + Accepts<Elements> + Accepts<Fields> + Accepts<Texts>,
    {
        let verb = PagesSetAttributes { collections: collections.into(), keys, elements, fields, values, durability };
        <Self::Completion as Completion>::answer(&self.receiver(), verb)
    }

    /// Removes one attribute of one element per task.
    ///
    /// Calls `ustore_pages_remove_attributes`.
    fn pages_remove_attributes<'collections, 'texts, Keys, Elements, Fields>(
        self,
        collections: impl Into<Collections<'collections>>,
        keys: Keys,
        elements: Elements,
        fields: Fields,
        durability: Durability,
    ) -> Deferrable<'view, Self, PagesRemoveAttributes<'collections, Keys, Elements, Fields>>
    where
        Self::Parent: Writable,
        Keys: AsRef<[Key]>,
        Elements: AsRef<[Key]>,
        Fields: AsRef<[&'texts str]>,
        Self::Completion: Accepts<Collections<'collections>> + Accepts<Keys> + Accepts<Elements> + Accepts<Fields>,
    {
        let verb = PagesRemoveAttributes { collections: collections.into(), keys, elements, fields, durability };
        <Self::Completion as Completion>::answer(&self.receiver(), verb)
    }

    /// Reads whole pages, or chosen elements of them, as markup into the arena.
    ///
    /// Calls `ustore_pages_read`.
    fn pages_read<'arena, 'collections, 'config, Keys: AsRef<[Key]>>(
        self,
        collections: impl Into<Collections<'collections>>,
        keys: Keys,
        config: impl Into<PagesReadConfig<'config>>,
        arena: &'arena mut Arena<'_>,
    ) -> Deferrable<'view, Self, PagesRead<'arena, 'collections, 'config, Keys>>
    where
        Self::Completion: Accepts<Collections<'collections>> + Accepts<Keys> + Accepts<PagesReadConfig<'config>>,
    {
        let verb = PagesRead {
            collections: collections.into(),
            keys,
            config: config.into(),
            arena,
            presences: Address::default(),
            values: ustore_strings_view_t::default(),
        };
        <Self::Completion as Completion>::answer(&self.receiver(), verb)
    }

    /// Lists up to `wanted` page keys of one collection's `range` into the arena, keeping those its
    /// `matching` admits, while [`PagesVerbs::pages_read`] renders them.
    ///
    /// Calls `ustore_pages_scan`.
    fn pages_scan<'config, 'arena>(
        self,
        collection: CollectionID,
        range: impl Into<KeyRange>,
        config: impl Into<PagesScanConfig<'config>>,
        arena: &'arena mut Arena<'_>,
    ) -> Deferrable<'view, Self, PagesScan<'arena, 'config>>
    where
        Self::Completion: Accepts<PagesScanConfig<'config>>,
    {
        let range = range.into();
        let PagesScanConfig { wanted, matching, mode, progress } = config.into();
        let verb = PagesScan {
            collection,
            start: range.start_key(),
            range,
            arena,
            wanted,
            matched: matching,
            mode,
            progress,
            found: Found::default(),
        };
        <Self::Completion as Completion>::answer(&self.receiver(), verb)
    }

    /// Draws up to `wanted` page keys uniformly at random from one pages collection into the arena.
    ///
    /// Calls `ustore_pages_sample`.
    fn pages_sample<'arena>(
        self,
        collection: CollectionID,
        config: impl Into<PagesSampleConfig>,
        arena: &'arena mut Arena<'_>,
    ) -> Deferrable<'view, Self, PagesSample<'arena>> {
        let verb = PagesSample { collection, arena, wanted: config.into().wanted, found: Found::default() };
        <Self::Completion as Completion>::answer(&self.receiver(), verb)
    }

    /// Estimates the pages and the space a range of page keys takes in one collection, through the
    /// arena its bounds land in.
    ///
    /// Calls `ustore_pages_measure`.
    fn pages_measure<'arena>(
        self,
        collection: CollectionID,
        range: impl Into<KeyRange>,
        arena: &'arena mut Arena<'_>,
    ) -> Deferrable<'view, Self, PagesMeasure<'arena>> {
        let range = range.into();
        let verb = PagesMeasure { collection, start: range.start_key(), range, arena, bounds: Bounds::default() };
        <Self::Completion as Completion>::answer(&self.receiver(), verb)
    }
}

impl<'view, Receiver: ViewVerbs<'view>> PagesVerbs<'view> for Receiver {}

/// A replacement of whole pages; see [`PagesVerbs::pages_import`].
#[derive(Debug)]
pub struct PagesImport<'collections, 'config, Keys, Markups> {
    collections: Collections<'collections>,
    keys: Keys,
    markups: Markups,
    encoding: PageEncoding,
    progress: Option<Progress<'config>>,
    durability: Durability,
}

impl<'texts, Keys: AsRef<[Key]>, Markups: AsRef<[&'texts str]>> Calling for PagesImport<'_, '_, Keys, Markups> {
    type Arguments = ffi::ustore_pages_import_t;
    type Output = ();

    fn arguments(
        &mut self,
        view: &Handles,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let keys = self.keys.as_ref();
        if keys.is_empty() {
            return Ok(None);
        }
        let (progress, progress_state) = lowered_progress(&self.progress);
        Ok(Some(ffi::ustore_pages_import_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            transaction: view.transaction(),
            count: keys.len() as ustore_size_t,
            collections: self.collections.view(keys.len())?,
            keys: ustore_strided_view_t::packed(keys),
            values: texts(self.markups.as_ref(), keys.len())?,
            encoding: self.encoding as ustore_enum_t,
            durability: self.durability,
            watch: view.watch(),
            progress,
            progress_state,
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        unsafe { ffi::ustore_pages_import(arguments) }
    }

    fn finish(self) -> Result<()> {
        Ok(())
    }
}

/// An insertion of parsed fragments; see [`PagesVerbs::pages_insert`].
#[derive(Debug)]
pub struct PagesInsert<'arena, 'collections, 'config, Keys, Fragments> {
    collections: Collections<'collections>,
    keys: Keys,
    fragments: Fragments,
    config: PagesInsertConfig<'config>,
    arena: &'arena Arena<'arena>,
    durability: Durability,
    effective: Address<Key>,
}

impl<'arena, 'texts, Keys: AsRef<[Key]>, Fragments: AsRef<[&'texts str]>> Calling
    for PagesInsert<'arena, '_, '_, Keys, Fragments>
{
    type Arguments = ffi::ustore_pages_insert_t;
    type Output = &'arena [Key];

    fn arguments(
        &mut self,
        view: &Handles,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let keys = self.keys.as_ref();
        if keys.is_empty() {
            return Ok(None);
        }
        Ok(Some(ffi::ustore_pages_insert_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            transaction: view.transaction(),
            count: keys.len() as ustore_size_t,
            collections: self.collections.view(keys.len())?,
            keys: ustore_strided_view_t::packed(keys),
            elements: column(self.config.elements, keys.len())?,
            parents: column(self.config.parents, keys.len())?,
            anchors: column(self.config.anchors, keys.len())?,
            values: texts(self.fragments.as_ref(), keys.len())?,
            encoding: self.config.encoding as ustore_enum_t,
            effective_keys: &mut self.effective.0,
            arena: self.arena.handle,
            durability: self.durability,
            watch: view.watch(),
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        unsafe { ffi::ustore_pages_insert(arguments) }
    }

    fn finish(self) -> Result<&'arena [Key]> {
        self.arena.host()?;
        // SAFETY: the insertion succeeded, writing a key per task into the arena held for
        // `'arena`.
        Ok(unsafe { exported_slice(self.effective.0, self.keys.as_ref().len()) })
    }
}

/// A removal of elements with their subtrees; see [`PagesVerbs::pages_remove`].
#[derive(Debug)]
pub struct PagesRemove<'collections, Keys, Elements> {
    collections: Collections<'collections>,
    keys: Keys,
    elements: Elements,
    durability: Durability,
}

impl<Keys: AsRef<[Key]>, Elements: AsRef<[Key]>> Calling for PagesRemove<'_, Keys, Elements> {
    type Arguments = ffi::ustore_pages_remove_t;
    type Output = ();

    fn arguments(
        &mut self,
        view: &Handles,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let keys = self.keys.as_ref();
        if keys.is_empty() {
            return Ok(None);
        }
        Ok(Some(ffi::ustore_pages_remove_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            transaction: view.transaction(),
            count: keys.len() as ustore_size_t,
            collections: self.collections.view(keys.len())?,
            keys: ustore_strided_view_t::packed(keys),
            end_keys: ustore_keys_view_t::default(),
            elements: column(self.elements.as_ref(), keys.len())?,
            durability: self.durability,
            watch: view.watch(),
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        unsafe { ffi::ustore_pages_remove(arguments) }
    }

    fn finish(self) -> Result<()> {
        Ok(())
    }
}

/// A move of elements with their subtrees; see [`PagesVerbs::pages_move`].
#[derive(Debug)]
pub struct PagesMove<'collections, Keys, Elements, Parents, Anchors> {
    collections: Collections<'collections>,
    keys: Keys,
    elements: Elements,
    parents: Parents,
    anchors: Anchors,
    durability: Durability,
}

impl<Keys, Elements, Parents, Anchors> Calling for PagesMove<'_, Keys, Elements, Parents, Anchors>
where
    Keys: AsRef<[Key]>,
    Elements: AsRef<[Key]>,
    Parents: AsRef<[Key]>,
    Anchors: AsRef<[Key]>,
{
    type Arguments = ffi::ustore_pages_move_t;
    type Output = ();

    fn arguments(
        &mut self,
        view: &Handles,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let keys = self.keys.as_ref();
        if keys.is_empty() {
            return Ok(None);
        }
        Ok(Some(ffi::ustore_pages_move_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            transaction: view.transaction(),
            count: keys.len() as ustore_size_t,
            collections: self.collections.view(keys.len())?,
            keys: ustore_strided_view_t::packed(keys),
            elements: column(self.elements.as_ref(), keys.len())?,
            parents: column(self.parents.as_ref(), keys.len())?,
            anchors: column(self.anchors.as_ref(), keys.len())?,
            durability: self.durability,
            watch: view.watch(),
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        unsafe { ffi::ustore_pages_move(arguments) }
    }

    fn finish(self) -> Result<()> {
        Ok(())
    }
}

/// A replacement of runs of text inside elements; see [`PagesVerbs::pages_splice`].
#[derive(Debug)]
pub struct PagesSplice<'collections, Keys, Elements, Starts, Lengths, Texts> {
    collections: Collections<'collections>,
    keys: Keys,
    elements: Elements,
    starts: Starts,
    lengths: Lengths,
    texts: Texts,
    durability: Durability,
}

impl<'texts, Keys, Elements, Starts, Lengths, Texts> Calling for PagesSplice<'_, Keys, Elements, Starts, Lengths, Texts>
where
    Keys: AsRef<[Key]>,
    Elements: AsRef<[Key]>,
    Starts: AsRef<[ustore_length_t]>,
    Lengths: AsRef<[ustore_length_t]>,
    Texts: AsRef<[&'texts str]>,
{
    type Arguments = ffi::ustore_pages_splice_t;
    type Output = ();

    fn arguments(
        &mut self,
        view: &Handles,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let keys = self.keys.as_ref();
        if keys.is_empty() {
            return Ok(None);
        }
        Ok(Some(ffi::ustore_pages_splice_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            transaction: view.transaction(),
            count: keys.len() as ustore_size_t,
            collections: self.collections.view(keys.len())?,
            keys: ustore_strided_view_t::packed(keys),
            elements: column(self.elements.as_ref(), keys.len())?,
            text_starts: column(self.starts.as_ref(), keys.len())?,
            text_lengths: column(self.lengths.as_ref(), keys.len())?,
            values: texts(self.texts.as_ref(), keys.len())?,
            durability: self.durability,
            watch: view.watch(),
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        unsafe { ffi::ustore_pages_splice(arguments) }
    }

    fn finish(self) -> Result<()> {
        Ok(())
    }
}

/// A setting of attributes of elements; see [`PagesVerbs::pages_set_attributes`].
#[derive(Debug)]
pub struct PagesSetAttributes<'collections, Keys, Elements, Fields, Texts> {
    collections: Collections<'collections>,
    keys: Keys,
    elements: Elements,
    fields: Fields,
    values: Texts,
    durability: Durability,
}

impl<'texts, Keys, Elements, Fields, Texts> Calling for PagesSetAttributes<'_, Keys, Elements, Fields, Texts>
where
    Keys: AsRef<[Key]>,
    Elements: AsRef<[Key]>,
    Fields: AsRef<[&'texts str]>,
    Texts: AsRef<[&'texts str]>,
{
    type Arguments = ffi::ustore_pages_set_attributes_t;
    type Output = ();

    fn arguments(
        &mut self,
        view: &Handles,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let keys = self.keys.as_ref();
        if keys.is_empty() {
            return Ok(None);
        }
        Ok(Some(ffi::ustore_pages_set_attributes_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            transaction: view.transaction(),
            count: keys.len() as ustore_size_t,
            collections: self.collections.view(keys.len())?,
            keys: ustore_strided_view_t::packed(keys),
            elements: column(self.elements.as_ref(), keys.len())?,
            fields: texts(self.fields.as_ref(), keys.len())?,
            values: texts(self.values.as_ref(), keys.len())?,
            durability: self.durability,
            watch: view.watch(),
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        unsafe { ffi::ustore_pages_set_attributes(arguments) }
    }

    fn finish(self) -> Result<()> {
        Ok(())
    }
}

/// A removal of attributes of elements; see [`PagesVerbs::pages_remove_attributes`].
#[derive(Debug)]
pub struct PagesRemoveAttributes<'collections, Keys, Elements, Fields> {
    collections: Collections<'collections>,
    keys: Keys,
    elements: Elements,
    fields: Fields,
    durability: Durability,
}

impl<'texts, Keys, Elements, Fields> Calling for PagesRemoveAttributes<'_, Keys, Elements, Fields>
where
    Keys: AsRef<[Key]>,
    Elements: AsRef<[Key]>,
    Fields: AsRef<[&'texts str]>,
{
    type Arguments = ffi::ustore_pages_remove_attributes_t;
    type Output = ();

    fn arguments(
        &mut self,
        view: &Handles,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let keys = self.keys.as_ref();
        if keys.is_empty() {
            return Ok(None);
        }
        Ok(Some(ffi::ustore_pages_remove_attributes_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            transaction: view.transaction(),
            count: keys.len() as ustore_size_t,
            collections: self.collections.view(keys.len())?,
            keys: ustore_strided_view_t::packed(keys),
            elements: column(self.elements.as_ref(), keys.len())?,
            fields: texts(self.fields.as_ref(), keys.len())?,
            durability: self.durability,
            watch: view.watch(),
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        unsafe { ffi::ustore_pages_remove_attributes(arguments) }
    }

    fn finish(self) -> Result<()> {
        Ok(())
    }
}

/// A rendering of pages or subtrees; see [`PagesVerbs::pages_read`].
#[derive(Debug)]
pub struct PagesRead<'arena, 'collections, 'config, Keys> {
    collections: Collections<'collections>,
    keys: Keys,
    config: PagesReadConfig<'config>,
    arena: &'arena Arena<'arena>,
    presences: Address<ustore_octet_t>,
    values: ustore_strings_view_t,
}

impl<'arena, Keys: AsRef<[Key]>> Calling for PagesRead<'arena, '_, '_, Keys> {
    type Arguments = ffi::ustore_pages_read_t;
    type Output = Values<'arena>;

    fn arguments(
        &mut self,
        view: &Handles,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let keys = self.keys.as_ref();
        if keys.is_empty() {
            return Ok(None);
        }
        Ok(Some(ffi::ustore_pages_read_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            view: view.view(),
            count: keys.len() as ustore_size_t,
            collections: self.collections.view(keys.len())?,
            keys: ustore_strided_view_t::packed(keys),
            elements: column(self.config.elements, keys.len())?,
            presences: &mut self.presences.0,
            lengths: ptr::null_mut(),
            values: &mut self.values,
            arena: self.arena.handle,
            outputs_encoding: self.config.outputs_encoding as ustore_enum_t,
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        unsafe { ffi::ustore_pages_read(arguments) }
    }

    fn finish(self) -> Result<Values<'arena>> {
        self.arena.host()?;
        // SAFETY: the read succeeded over every key, and the arena stays held for `'arena`.
        unsafe { Values::exported(self.keys.as_ref().len(), self.presences, self.values) }
    }
}

/// A walk over the keys of one pages collection; see [`PagesVerbs::pages_scan`].
#[derive(Debug)]
pub struct PagesScan<'arena, 'config> {
    collection: CollectionID,
    start: Key,
    range: KeyRange,
    arena: &'arena Arena<'arena>,
    wanted: usize,
    matched: Matching<'config>,
    mode: ScanMode,
    progress: Option<Progress<'config>>,
    found: Found,
}

impl<'arena> Calling for PagesScan<'arena, '_> {
    type Arguments = ffi::ustore_pages_scan_t;
    type Output = &'arena [Key];

    fn arguments(
        &mut self,
        view: &Handles,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let (progress, progress_state) = lowered_progress(&self.progress);
        Ok(Some(ffi::ustore_pages_scan_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            view: view.view(),
            count: 1,
            collections: ustore_strided_view_t::repeated(&self.collection),
            start_keys: ustore_strided_view_t::repeated(&self.start),
            end_keys: self.range.end_keys(),
            direction: self.range.direction,
            filter: self.matched.lowered_filter(),
            predicate: self.matched.lowered_predicate(),
            offsets: &mut self.found.offsets.0,
            keys: &mut self.found.keys.0,
            arena: self.arena.handle,
            wanted: self.wanted as ustore_size_t,
            mode: self.mode,
            progress,
            progress_state,
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        unsafe { ffi::ustore_pages_scan(arguments) }
    }

    fn finish(self) -> Result<&'arena [Key]> {
        self.arena.host()?;
        // SAFETY: the scan succeeded, and the arena stays held for `'arena`.
        Ok(unsafe { self.found.keys() })
    }
}

/// A random draw of page keys from one collection; see [`PagesVerbs::pages_sample`].
#[derive(Debug)]
pub struct PagesSample<'arena> {
    collection: CollectionID,
    arena: &'arena Arena<'arena>,
    wanted: usize,
    found: Found,
}

impl<'arena> Calling for PagesSample<'arena> {
    type Arguments = ffi::ustore_pages_sample_t;
    type Output = &'arena [Key];

    fn arguments(
        &mut self,
        view: &Handles,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        Ok(Some(ffi::ustore_pages_sample_t {
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
        unsafe { ffi::ustore_pages_sample(arguments) }
    }

    fn finish(self) -> Result<&'arena [Key]> {
        self.arena.host()?;
        // SAFETY: the sample succeeded, and the arena stays held for `'arena`.
        Ok(unsafe { self.found.keys() })
    }
}

/// An estimate of what a range of pages holds; see [`PagesVerbs::pages_measure`].
#[derive(Debug)]
pub struct PagesMeasure<'arena> {
    collection: CollectionID,
    start: Key,
    range: KeyRange,
    arena: &'arena Arena<'arena>,
    bounds: Bounds,
}

impl Calling for PagesMeasure<'_> {
    type Arguments = ffi::ustore_pages_measure_t;
    type Output = Measurement;

    fn arguments(
        &mut self,
        view: &Handles,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let [
            min_cardinalities,
            max_cardinalities,
            min_value_bytes,
            max_value_bytes,
            min_space_usages,
            max_space_usages,
        ] = &mut self.bounds.0;
        Ok(Some(ffi::ustore_pages_measure_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            view: view.view(),
            count: 1,
            collections: ustore_strided_view_t::repeated(&self.collection),
            start_keys: ustore_strided_view_t::repeated(&self.start),
            end_keys: self.range.end_keys(),
            direction: self.range.direction,
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
        unsafe { ffi::ustore_pages_measure(arguments) }
    }

    fn finish(self) -> Result<Measurement> {
        self.arena.host()?;
        // SAFETY: the measurement of one range succeeded.
        Ok(unsafe { self.bounds.measurement() })
    }
}

// endregion: Verbs

// region: Cursors

/// A walk over the keys of one pages collection, which every pull resumes past the last key the pull before it handed out, as a
/// [`crate::Cursor`] walks blobs, keeping the keys its [`Matching`] admits.
///
/// Mirrors `ustore_pages_cursor_t`; dropping it calls `ustore_pages_cursor_free`.
#[derive(Debug)]
pub struct PagesCursor<'view> {
    handle: ffi::ustore_pages_cursor_t,
    database: ustore_database_t,
    view: PhantomData<&'view ()>,
}

impl<'view, Parent> On<'view, Parent> {
    /// Opens a walk over the keys of one pages collection's `range`, compiling its filter once,
    /// whose predicate lives as long as the cursor.
    ///
    /// Calls `ustore_pages_cursor_init`.
    pub fn pages_cursor(
        &self,
        collection: CollectionID,
        range: impl Into<KeyRange>,
        config: impl Into<PagesCursorConfig<'view>>,
    ) -> Result<PagesCursor<'view>> {
        let range: KeyRange = range.into();
        let matching = config.into().matching;
        let mut cursor = PagesCursor { handle: ptr::null_mut(), database: self.database(), view: PhantomData };
        let mut outcome = ustore_outcome_t::default();
        let mut arguments = ffi::ustore_pages_cursor_init_t {
            cursor: &mut cursor.handle,
            database: self.database(),
            executor: self.executor(),
            outcome: &mut outcome,
            view: self.view(),
            collection,
            start_key: range.start_key(),
            end_key: range.end.as_ref().map_or(ptr::null(), ptr::from_ref),
            direction: range.direction,
            filter: matching.lowered_filter(),
            predicate: matching.lowered_predicate(),
        };
        // SAFETY: every pointer in `arguments` outlives the call, which is final on return.
        unsafe { ffi::ustore_pages_cursor_init(&mut arguments) };
        check(outcome).map(|()| cursor)
    }
}

impl PagesCursor<'_> {
    /// The receiver whose pulls block on `executor`.
    pub fn on<'cursor>(&'cursor mut self, executor: &'cursor Executor) -> CursorOn<'cursor, Self> {
        CursorOn::new(self, Some(executor))
    }

    /// Pulls up to `wanted` next keys into `arena`, fewer once the walk nears its end and none past it.
    ///
    /// Calls `ustore_pages_cursor_next`.
    ///
    /// Runs on the calling thread; [`PagesCursor::on`] names an executor for one pull.
    pub fn next<'arena>(&mut self, wanted: usize, arena: &'arena mut Arena<'_>) -> Result<&'arena [Key]> {
        CursorOn::new(self, None).next(wanted, arena)
    }

    /// Pulls up to `wanted` next keys with their pages rendered as `encoding` names into `arena` in
    /// one call.
    ///
    /// Calls `ustore_pages_cursor_next`.
    ///
    /// Runs on the calling thread; [`PagesCursor::on`] names an executor for one pull.
    pub fn next_pairs<'arena>(
        &mut self,
        wanted: usize,
        arena: &'arena mut Arena<'_>,
        encoding: PageEncoding,
    ) -> Result<Page<'arena>> {
        CursorOn::new(self, None).next_pairs(wanted, arena, encoding)
    }
}

impl CursorOn<'_, PagesCursor<'_>> {
    /// Pulls up to `wanted` next keys into `arena`, fewer near the walk's end and none past it.
    ///
    /// Calls `ustore_pages_cursor_next`.
    pub fn next<'arena>(self, wanted: usize, arena: &'arena mut Arena<'_>) -> Result<&'arena [Key]> {
        self.pull(wanted, arena, ptr::null_mut(), PageEncoding::Xml)
    }

    /// Pulls up to `wanted` next keys with their pages rendered as `encoding` names into `arena` in
    /// one call.
    ///
    /// Calls `ustore_pages_cursor_next`.
    pub fn next_pairs<'arena>(
        self,
        wanted: usize,
        arena: &'arena mut Arena<'_>,
        encoding: PageEncoding,
    ) -> Result<Page<'arena>> {
        let mut values = ustore_strings_view_t::default();
        let keys = self.pull(wanted, arena, &mut values, encoding)?;
        // SAFETY: the pull succeeded, writing a value per key as a tape into the result's arena.
        let values = unsafe { Strings::exported(values, keys.len())? };
        Ok(Page::new(keys, values))
    }

    /// Pulls a page of keys into `arena`, with their values into `values` unless it is null.
    fn pull<'arena>(
        self,
        wanted: usize,
        arena: &'arena mut Arena<'_>,
        values: *mut ustore_strings_view_t,
        encoding: PageEncoding,
    ) -> Result<&'arena [Key]> {
        let mut keys: *const ustore_key_t = ptr::null();
        let mut written: ustore_size_t = 0;
        let mut outcome = ustore_outcome_t::default();
        let mut arguments = ffi::ustore_pages_cursor_next_t {
            database: self.cursor.database,
            executor: executor_handle(self.executor),
            outcome: &mut outcome,
            completion: ustore_completion_t::Blocking,
            cursor: self.cursor.handle,
            keys: &mut keys,
            values,
            written: &mut written,
            arena: arena.handle,
            wanted: wanted as ustore_size_t,
            outputs_encoding: encoding as ustore_enum_t,
            progress: None,
            progress_state: ptr::null_mut(),
        };
        // SAFETY: every pointer in `arguments` outlives the call, which is final on return.
        unsafe { ffi::ustore_pages_cursor_next(&mut arguments) };
        check(outcome)?;
        arena.host()?;
        // SAFETY: a successful pull wrote `written` keys into the arena the result views.
        Ok(unsafe { exported_slice(keys, written as usize) })
    }
}

impl Drop for PagesCursor<'_> {
    fn drop(&mut self) {
        // SAFETY: the handle came from a successful init, or is the null a failed one leaves, which the free ignores.
        unsafe { ffi::ustore_pages_cursor_free(self.handle) };
    }
}

// endregion: Cursors

// region: Tests

#[cfg(test)]
mod tests {
    use super::*;
    use crate::db::{Database, Modality, RAMOptions};
    use crate::testing::{created, key};

    const BUFFERED: Durability = Durability::Buffered;

    /// A database whose pages collection holds `markup` as the page keyed five hundred.
    fn imported(markup: &str) -> (Database, CollectionID, Arena<'static>) {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let pages = created(&database, Modality::Pages);
        database.head().pages_import(pages, [key(500)], [markup], PageEncoding::Xml, BUFFERED).unwrap();
        (database, pages, Arena::new().unwrap())
    }

    /// The rendering of `element` of the page keyed five hundred, or of the whole page, and `None`
    /// where it is missing.
    fn rendered(
        database: &Database,
        pages: CollectionID,
        element: Option<Key>,
        arena: &mut Arena<'_>,
    ) -> Option<String> {
        let config = PagesReadConfig { elements: element.as_slice(), outputs_encoding: PageEncoding::Xml };
        let values = database.head().pages_read(pages, [key(500)], config, arena).unwrap();
        values.get(0).map(|bytes| String::from_utf8(bytes.to_vec()).unwrap())
    }

    /// Inserts `fragment` as the element keyed `element` under `parent`, the root for `None`.
    fn inserted(
        head: &On<'_>,
        pages: CollectionID,
        fragment: &str,
        element: Key,
        parent: Option<Key>,
        arena: &mut Arena<'_>,
    ) {
        let config = PagesInsertConfig {
            elements: &[element],
            parents: parent.as_slice(),
            ..PagesInsertConfig::from(PageEncoding::Xml)
        };
        let landed = head.pages_insert(pages, [key(500)], [fragment], config, arena, BUFFERED).unwrap();
        assert_eq!(landed, &[element]);
    }

    #[test]
    fn inserts_land_under_their_parents_and_render_back() {
        let (database, pages, mut arena) = imported(r#"<doc><p id="1">one</p></doc>"#);
        let head = database.head();
        inserted(&head, pages, "<h1>title</h1>", key(10), None, &mut arena);
        assert_eq!(
            rendered(&database, pages, None, &mut arena).as_deref(),
            Some(r#"<h1>title</h1><doc><p id="1">one</p></doc>"#)
        );

        let under = PagesInsertConfig { parents: &[key(10)], ..PagesInsertConfig::from(PageEncoding::Xml) };
        let minted = head.pages_insert(pages, [key(500)], ["<i>x</i>"], under, &mut arena, BUFFERED).unwrap()[0];
        assert_ne!(minted, key(0));
        assert_eq!(rendered(&database, pages, Some(key(10)), &mut arena).as_deref(), Some("<h1>title<i>x</i></h1>"));
        assert_eq!(rendered(&database, pages, Some(key(404)), &mut arena), None);
    }

    #[test]
    fn splices_and_attributes_edit_one_element() {
        let (database, pages, mut arena) = imported("<doc/>");
        let head = database.head();
        let page = [key(500)];
        inserted(&head, pages, "<h1>title</h1>", key(10), None, &mut arena);
        head.pages_splice(pages, page, [key(10)], [0u64], [5u64], ["Head"], BUFFERED).unwrap();
        head.pages_set_attributes(pages, page, [key(10)], ["class"], ["a<b"], BUFFERED).unwrap();
        assert_eq!(
            rendered(&database, pages, Some(key(10)), &mut arena).as_deref(),
            Some(r#"<h1 class="a&lt;b">Head</h1>"#)
        );
        head.pages_remove_attributes(pages, page, [key(10)], ["class"], BUFFERED).unwrap();
        assert_eq!(rendered(&database, pages, Some(key(10)), &mut arena).as_deref(), Some("<h1>Head</h1>"));
    }

    #[test]
    fn moves_keep_keys_and_removals_take_subtrees() {
        let (database, pages, mut arena) = imported("<doc/>");
        let head = database.head();
        let page = [key(500)];
        inserted(&head, pages, "<h1>title</h1>", key(10), None, &mut arena);
        inserted(&head, pages, "<i>x</i>", key(11), Some(key(10)), &mut arena);

        // Empty parents and anchors move the element to the front of the root.
        head.pages_move(pages, page, [key(11)], [], [], BUFFERED).unwrap();
        assert_eq!(rendered(&database, pages, None, &mut arena).as_deref(), Some("<i>x</i><h1>title</h1><doc/>"));

        head.pages_remove(pages, page, [key(10)], BUFFERED).unwrap();
        assert_eq!(rendered(&database, pages, Some(key(10)), &mut arena), None);
        assert_eq!(rendered(&database, pages, Some(key(11)), &mut arena).as_deref(), Some("<i>x</i>"));
        head.pages_remove(pages, page, [Key::default()], BUFFERED).unwrap();
        assert_eq!(rendered(&database, pages, None, &mut arena), None);
    }

    #[test]
    fn scans_keep_the_pages_their_filter_matches() {
        let (database, pages, mut arena) = imported("<p/>");
        let head = database.head();
        head.pages_import(pages, [key(2)], ["<p><b/></p>"], PageEncoding::Xml, BUFFERED).unwrap();
        assert_eq!(head.pages_scan(pages, key(0), 10, &mut arena).unwrap(), &[key(2), key(500)]);

        let config = PagesScanConfig {
            matching: Matching { filter: Some("//b"), ..Matching::default() },
            ..PagesScanConfig::from(10)
        };
        assert_eq!(head.pages_scan(pages, key(0), config, &mut arena).unwrap(), &[key(2)]);
    }

    #[test]
    fn encodings_read_back_from_their_spelling() {
        for encoding in
            [PageEncoding::Html, PageEncoding::Xml, PageEncoding::Markdown, PageEncoding::Text, PageEncoding::Json]
        {
            assert_eq!(encoding.to_string().parse::<PageEncoding>().unwrap(), encoding);
        }
        assert_eq!(PageEncoding::Markdown.to_string(), "markdown");
        assert!("unknown".parse::<PageEncoding>().is_err());
    }
}

// endregion: Tests
