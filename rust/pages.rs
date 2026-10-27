//! Pages - the Rust twin of the cursors of `c/ustore/pages.h`.
//!
//! The pages modality is reached through its walk alone: a [`PagesCursor`] lists the keys of one pages collection,
//! keeping those its filter, an XPath in StringZilla's subset, matches, with the pages rendered when asked.

use std::marker::PhantomData;
use std::ptr;

use crate::blobs::Page;
use crate::db::ffi::{
    ustore_arena_t, ustore_bytes_view_t, ustore_collection_id_t, ustore_completion_t, ustore_database_t, ustore_enum_t,
    ustore_executor_t, ustore_key_t, ustore_outcome_t, ustore_predicate_t, ustore_progress_callback_t, ustore_size_t,
    ustore_strings_view_t, ustore_view_t,
};
use crate::db::{check, exported_slice, Arena, CollectionID, Key, Matched, Matching, Result, Strings, View};

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

// region: Raw Declarations

/// One-to-one mirror of the cursors of `c/ustore/pages.h`.
mod ffi {
    #![allow(non_camel_case_types)]

    use std::ffi::c_void;

    use super::{
        ustore_arena_t, ustore_bytes_view_t, ustore_collection_id_t, ustore_completion_t, ustore_database_t,
        ustore_enum_t, ustore_executor_t, ustore_key_t, ustore_outcome_t, ustore_predicate_t,
        ustore_progress_callback_t, ustore_size_t, ustore_strings_view_t, ustore_view_t,
    };

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

    extern "C" {
        pub fn ustore_pages_cursor_init(arguments: *mut ustore_pages_cursor_init_t);
        pub fn ustore_pages_cursor_next(arguments: *mut ustore_pages_cursor_next_t);
        pub fn ustore_pages_cursor_free(cursor: ustore_pages_cursor_t);
    }
}

// endregion: Raw Declarations

// region: Cursors

/// A walk over the keys of one pages collection, which every pull resumes past the last key the pull before it handed out, as a
/// [`crate::Cursor`] walks blobs, keeping the keys its [`Matching`] admits.
///
/// Mirrors `ustore_pages_cursor_t`; dropping it calls `ustore_pages_cursor_free`.
#[derive(Debug)]
pub struct PagesCursor<'view> {
    handle: ffi::ustore_pages_cursor_t,
    database: ustore_database_t,
    executor: ustore_executor_t,
    matched: Box<Matched<'view>>,
    marker: PhantomData<&'view ()>,
}

impl<'view, Parent> View<'view, Parent> {
    /// Opens a walk over the keys of one pages collection from `start`, compiling what `matching` names once, whose predicate lives
    /// as long as the cursor.
    ///
    /// Calls `ustore_pages_cursor_init`.
    pub fn pages_cursor(
        &self,
        collection: CollectionID,
        start: Key,
        matching: Matching<'view>,
    ) -> Result<PagesCursor<'view>> {
        let mut cursor = PagesCursor {
            handle: ptr::null_mut(),
            database: self.database(),
            executor: self.executor(),
            matched: Box::new(Matched::new(matching)),
            marker: PhantomData,
        };
        let mut outcome = ustore_outcome_t::default();
        let mut arguments = ffi::ustore_pages_cursor_init_t {
            cursor: &mut cursor.handle,
            database: self.database(),
            executor: self.executor(),
            outcome: &mut outcome,
            view: self.view(),
            collection,
            start_key: start,
            filter: cursor.matched.filter(),
            predicate: cursor.matched.predicate(),
        };
        // SAFETY: every pointer in `arguments` outlives the call, which is final on return.
        unsafe { ffi::ustore_pages_cursor_init(&mut arguments) };
        check(outcome).map(|()| cursor)
    }
}

impl PagesCursor<'_> {
    /// Pulls up to `wanted` next keys into `arena`, fewer once the walk nears its end and none past it.
    ///
    /// Calls `ustore_pages_cursor_next`.
    pub fn next<'arena>(&mut self, wanted: usize, arena: &'arena mut Arena) -> Result<&'arena [Key]> {
        self.pull(wanted, arena, ptr::null_mut(), PageEncoding::Xml)
    }

    /// Pulls up to `wanted` next keys with their pages rendered as `encoding` names into `arena` in one call, rendered as `encoding` names.
    ///
    /// Calls `ustore_pages_cursor_next`.
    pub fn next_pairs<'arena>(
        &mut self,
        wanted: usize,
        arena: &'arena mut Arena,
        encoding: PageEncoding,
    ) -> Result<Page<'arena>> {
        let mut values = ustore_strings_view_t::default();
        let keys = self.pull(wanted, arena, &mut values, encoding)?;
        // SAFETY: the pull succeeded, writing a value per key as a tape into the arena borrowed for `'arena`.
        let values = unsafe { Strings::exported(values, keys.len())? };
        Ok(Page::new(keys, values))
    }

    /// Pulls a page of keys into `arena`, with their values into `values` unless it is null.
    fn pull<'arena>(
        &mut self,
        wanted: usize,
        arena: &'arena mut Arena,
        values: *mut ustore_strings_view_t,
        encoding: PageEncoding,
    ) -> Result<&'arena [Key]> {
        let mut keys: *const ustore_key_t = ptr::null();
        let mut written: ustore_size_t = 0;
        let mut outcome = ustore_outcome_t::default();
        let mut arguments = ffi::ustore_pages_cursor_next_t {
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
            outputs_encoding: encoding as ustore_enum_t,
            progress: None,
            progress_state: ptr::null_mut(),
        };
        // SAFETY: every pointer in `arguments` outlives the call, which is final on return.
        unsafe { ffi::ustore_pages_cursor_next(&mut arguments) };
        self.matched.raise();
        check(outcome)?;
        // SAFETY: a successful pull wrote `written` keys into the arena, which stays borrowed for `'arena`.
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
