//! Apache Arrow - the Rust twin of `c/ustore/arrow.h`: the structs of the Arrow C data and stream interfaces, and the
//! loops that move whole collections through its converters, its exporters and the ingest of `c/ustore/db.h`.
//!
//! The structs lay out as the specification fixes them, as arrow-rs's `FFI_ArrowSchema`, `FFI_ArrowArray` and
//! `FFI_ArrowArrayStream` do, so a stream converts by pointer without this crate depending on arrow-rs:
//!
//! ```ignore
//! let mut stream = database.collection("posters")?.export_arrow()?;
//! let reader = unsafe { arrow::ffi_stream::ArrowArrayStreamReader::from_raw((&mut stream as *mut ustore::ArrowArrayStream).cast())? };
//! ```
//!
//! Every struct releases itself when dropped, unless its consumer moved it out and left `release` null, as the
//! specification asks of a move.
//!
//! An export pulls the collection's cursor into one arena per batch, which the exporters pin until the batch is
//! released, so batches outlive each other and the stream. An import reads every batch's columns in place into the
//! writes of the collection's modality, and an ingest appends them to one ingest that lands as one commit.
//!
//! File: rust/arrow.rs
//! Author: Ash Vardanian
//! Date: October 29, 2026

use std::array;
use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::ptr;

use crate::blobs::ffi as blobs_ffi;
use crate::db::ffi::{
    self as db_ffi, ustore_collections_view_t, ustore_completion_t, ustore_database_t, ustore_enum_t,
    ustore_executor_t, ustore_key_t, ustore_keys_view_t, ustore_outcome_t, ustore_size_t, ustore_snapshot_t,
    ustore_status_t, ustore_strings_view_t, ustore_transaction_t, ustore_view_t, USTORE_STRINGS_U32_TAPE_K,
    USTORE_STRINGS_U64_TAPE_K,
};
use crate::db::{check, fail, Arena, CollectionID, Durability, Error, Key, Modality, Result, Status, View, Watch};
use crate::docs::ffi as docs_ffi;
use crate::docs::sealed::Format;
use crate::docs::{Json, Modification};
use crate::graph::ffi as graph_ffi;
use crate::graph::Role;
use crate::pages::ffi as pages_ffi;
use crate::pages::PageEncoding;
use crate::strings::ffi as strings_ffi;

/// The type of an Arrow array: its format, name, metadata and children, as `struct ArrowSchema` lays them out.
#[repr(C)]
#[derive(Debug)]
pub struct ArrowSchema {
    /// The format string, such as `+s` for a struct or `w:16` for 16-byte keys.
    pub format: *const c_char,
    /// The field name, or null.
    pub name: *const c_char,
    /// The binary-encoded key-value metadata, or null.
    pub metadata: *const c_char,
    /// The `ARROW_FLAG_*` bits.
    pub flags: i64,
    /// The number of children.
    pub n_children: i64,
    /// The children.
    pub children: *mut *mut ArrowSchema,
    /// The dictionary type, or null.
    pub dictionary: *mut ArrowSchema,
    /// Frees the schema, null once released or moved out.
    pub release: Option<unsafe extern "C" fn(*mut ArrowSchema)>,
    /// The producer's own state.
    pub private_data: *mut c_void,
}

/// The buffers of an Arrow array, as `struct ArrowArray` lays them out.
#[repr(C)]
#[derive(Debug)]
pub struct ArrowArray {
    /// The rows.
    pub length: i64,
    /// The nulls, or -1 when not yet counted.
    pub null_count: i64,
    /// The first row, into every buffer.
    pub offset: i64,
    /// The number of buffers.
    pub n_buffers: i64,
    /// The number of children.
    pub n_children: i64,
    /// The buffers, validity first.
    pub buffers: *mut *const c_void,
    /// The children.
    pub children: *mut *mut ArrowArray,
    /// The dictionary, or null.
    pub dictionary: *mut ArrowArray,
    /// Frees the array, null once released or moved out.
    pub release: Option<unsafe extern "C" fn(*mut ArrowArray)>,
    /// The producer's own state.
    pub private_data: *mut c_void,
}

/// A stream of Arrow record batches of one schema, as `struct ArrowArrayStream` lays it out.
#[repr(C)]
#[derive(Debug)]
pub struct ArrowArrayStream {
    /// Writes the schema every batch shares, answering 0 or an `errno` code.
    pub get_schema: Option<unsafe extern "C" fn(*mut ArrowArrayStream, *mut ArrowSchema) -> c_int>,
    /// Writes the next batch, released once the stream ends, answering 0 or an `errno` code.
    pub get_next: Option<unsafe extern "C" fn(*mut ArrowArrayStream, *mut ArrowArray) -> c_int>,
    /// The message of the last failed callback, or null.
    pub get_last_error: Option<unsafe extern "C" fn(*mut ArrowArrayStream) -> *const c_char>,
    /// Frees the stream, which every batch it answered outlives; null once released or moved out.
    pub release: Option<unsafe extern "C" fn(*mut ArrowArrayStream)>,
    /// The producer's own state.
    pub private_data: *mut c_void,
}

impl ArrowSchema {
    /// A released schema, for a producer to write into.
    pub fn empty() -> Self {
        ArrowSchema {
            format: ptr::null(),
            name: ptr::null(),
            metadata: ptr::null(),
            flags: 0,
            n_children: 0,
            children: ptr::null_mut(),
            dictionary: ptr::null_mut(),
            release: None,
            private_data: ptr::null_mut(),
        }
    }
}

impl ArrowArray {
    /// A released array, for a producer to write into.
    pub fn empty() -> Self {
        ArrowArray {
            length: 0,
            null_count: 0,
            offset: 0,
            n_buffers: 0,
            n_children: 0,
            buffers: ptr::null_mut(),
            children: ptr::null_mut(),
            dictionary: ptr::null_mut(),
            release: None,
            private_data: ptr::null_mut(),
        }
    }
}

impl ArrowArrayStream {
    /// A released stream, for a producer to write into.
    pub fn empty() -> Self {
        ArrowArrayStream {
            get_schema: None,
            get_next: None,
            get_last_error: None,
            release: None,
            private_data: ptr::null_mut(),
        }
    }

    /// The schema every batch shares.
    pub fn schema(&mut self) -> Result<ArrowSchema> {
        let mut schema = ArrowSchema::empty();
        let get_schema = self
            .get_schema
            .ok_or(fail(Status::Uninitialized, "the stream was released"))?;
        // SAFETY: a live stream answers its schema into a released one.
        let answered = unsafe { get_schema(self, &mut schema) };
        if answered != 0 {
            return Err(fail(Status::Io, "the stream failed to answer its schema"));
        }
        Ok(schema)
    }

    /// The next batch, or `None` once the stream ends.
    pub fn next_batch(&mut self) -> Result<Option<ArrowArray>> {
        let mut batch = ArrowArray::empty();
        let get_next = self
            .get_next
            .ok_or(fail(Status::Uninitialized, "the stream was released"))?;
        // SAFETY: a live stream answers its next batch into a released one.
        let answered = unsafe { get_next(self, &mut batch) };
        if answered != 0 {
            return Err(fail(Status::Io, "the stream failed to answer a batch"));
        }
        Ok(batch.release.is_some().then_some(batch))
    }

    /// The message of the last failed callback, which lives until the next call on the stream.
    pub fn last_error(&mut self) -> Option<&CStr> {
        let get_last_error = self.get_last_error?;
        // SAFETY: the stream answers a null-terminated message or null, valid until its next call.
        let message = unsafe { get_last_error(self) };
        (!message.is_null()).then(|| unsafe { CStr::from_ptr(message) })
    }
}

impl Drop for ArrowSchema {
    fn drop(&mut self) {
        if let Some(release) = self.release {
            // SAFETY: a schema with a release callback is live and released once.
            unsafe { release(self) };
        }
    }
}

impl Drop for ArrowArray {
    fn drop(&mut self) {
        if let Some(release) = self.release {
            // SAFETY: an array with a release callback is live and released once.
            unsafe { release(self) };
        }
    }
}

impl Drop for ArrowArrayStream {
    fn drop(&mut self) {
        if let Some(release) = self.release {
            // SAFETY: a stream with a release callback is live and released once.
            unsafe { release(self) };
        }
    }
}

/// In what order an ingest's rows arrive, as `ustore_ingest_order_t` spells it.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub enum IngestOrder {
    /// Keys strictly ascend across every batch, checked as each one is written into runs of its own.
    #[default]
    Sorted = 0,
    /// Keys arrive in any order and may repeat, the last row of a key winning; every batch is held until the end.
    Unsorted = 1,
}

// region: Columns

/// The columns a collection of `modality` exports and imports, by name, each with whether it is ragged.
fn columns_of(modality: Modality) -> &'static [(&'static CStr, bool)] {
    match modality {
        Modality::Graph => &[(c"source", false), (c"target", false), (c"edge", false)],
        Modality::Strings => &[(c"key", true), (c"value", false)],
        _ => &[(c"key", false), (c"value", true)],
    }
}

/// Where the columns of a modality sit among the children of a stream's schema.
#[derive(Default)]
struct Columns {
    /// Keys of blobs, documents and pages, or the keys strings map onto.
    keys: Option<usize>,
    /// Values of blobs, documents and pages, or the strings of a strings collection.
    strings: Option<usize>,
    /// The sources of edges.
    sources: Option<usize>,
    /// The targets of edges.
    targets: Option<usize>,
    /// The identifiers of edges, which a stream may leave out for the default edge.
    edges: Option<usize>,
}

/// Matches the children of `schema` to the columns of `modality` by name, refusing a missing, repeated or unknown one.
fn resolve_columns(schema: &ArrowSchema, modality: Modality) -> Result<Columns> {
    let unknown = fail(
        Status::InvalidArgument,
        "the stream carries a column its modality does not name, or names one twice",
    );
    let mut columns = Columns::default();
    for index in 0..schema.n_children.max(0) as usize {
        // SAFETY: a live schema holds `n_children` live children, each named or not.
        let name = unsafe {
            let child = &**schema.children.add(index);
            match child.name.is_null() {
                true => &[][..],
                false => CStr::from_ptr(child.name).to_bytes(),
            }
        };
        let slot = match (modality, name) {
            (Modality::Graph, b"source") => &mut columns.sources,
            (Modality::Graph, b"target") => &mut columns.targets,
            (Modality::Graph, b"edge") => &mut columns.edges,
            (Modality::Graph, _) => return Err(unknown),
            (Modality::Strings, b"key") => &mut columns.strings,
            (Modality::Strings, b"value") => &mut columns.keys,
            (_, b"key") => &mut columns.keys,
            (_, b"value") => &mut columns.strings,
            _ => return Err(unknown),
        };
        if slot.replace(index).is_some() {
            return Err(unknown);
        }
    }
    let complete = match modality {
        Modality::Graph => columns.sources.and(columns.targets),
        _ => columns.keys.and(columns.strings),
    };
    match complete {
        Some(_) => Ok(columns),
        None => Err(fail(
            Status::InvalidArgument,
            "the stream lacks a column its modality needs",
        )),
    }
}

/// The views of one batch's columns, read in place, those its modality lacks left empty.
#[derive(Clone, Copy, Default)]
struct Rows {
    count: usize,
    keys: ustore_keys_view_t,
    strings: ustore_strings_view_t,
    sources: ustore_keys_view_t,
    targets: ustore_keys_view_t,
    edges: ustore_keys_view_t,
}

/// The error a converter's `status` reports for a column of `count` rows a batch reads `needed` of.
fn converted(status: ustore_status_t, count: ustore_size_t, needed: ustore_size_t) -> Result<()> {
    let message = match status.0 {
        raw if raw == Status::Unsupported as c_int => c"a column is of another type, a dictionary or a view",
        raw if raw == Status::Malformed as c_int => c"a column misses a buffer or is shorter than its batch",
        _ => c"a column holds a null",
    };
    check(ustore_outcome_t {
        status,
        message: message.as_ptr(),
    })?;
    match count >= needed {
        true => Ok(()),
        false => Err(fail(Status::Malformed, "a column is shorter than its batch")),
    }
}

/// `keys` from their `first` on, or the missing keys a null view stands for.
fn keys_from(keys: ustore_keys_view_t, first: usize) -> ustore_keys_view_t {
    match keys.data.is_null() {
        true => keys,
        false => ustore_keys_view_t {
            data: keys.data.wrapping_add(first),
            stride: keys.stride,
        },
    }
}

/// `strings` from their `first` on, the tape's offsets advanced past the strings before.
fn strings_from(strings: ustore_strings_view_t, first: usize) -> ustore_strings_view_t {
    let mut advanced = strings;
    // SAFETY: the layout names the member of the union that holds the tape.
    unsafe {
        match strings.layout {
            USTORE_STRINGS_U32_TAPE_K if !strings.data.u32_tape.offsets.is_null() => {
                advanced.data.u32_tape.offsets = strings.data.u32_tape.offsets.add(first)
            }
            USTORE_STRINGS_U64_TAPE_K if !strings.data.u64_tape.offsets.is_null() => {
                advanced.data.u64_tape.offsets = strings.data.u64_tape.offsets.add(first)
            }
            _ => {}
        }
    }
    advanced
}

/// Reads the columns of `batch`, laid out as `schema` says, in place.
fn rows_of(schema: &ArrowSchema, batch: &ArrowArray, columns: &Columns) -> Result<Rows> {
    if batch.n_children != schema.n_children || batch.null_count > 0 || batch.offset < 0 || batch.length < 0 {
        return Err(fail(
            Status::Malformed,
            "a batch disagrees with its schema or holds a null row",
        ));
    }
    let first = batch.offset as usize;
    let needed = (batch.offset + batch.length) as ustore_size_t;
    // SAFETY: a live batch and its schema hold `n_children` live children, of which `column` is one.
    let children = |column: usize| unsafe { (&**schema.children.add(column), &**batch.children.add(column)) };
    let keys = |column: Option<usize>| -> Result<ustore_keys_view_t> {
        let mut keys = ustore_keys_view_t::default();
        if let Some(column) = column {
            let (schema, array) = children(column);
            let mut count: ustore_size_t = 0;
            // SAFETY: the converter reads only the lengths and pointers of the live column.
            let status = unsafe { ffi::ustore_keys_view_from_arrow(schema, array, &mut keys, &mut count) };
            converted(status, count, needed)?;
        }
        Ok(keys_from(keys, first))
    };
    let mut rows = Rows {
        count: batch.length as usize,
        keys: keys(columns.keys)?,
        sources: keys(columns.sources)?,
        targets: keys(columns.targets)?,
        edges: keys(columns.edges)?,
        ..Rows::default()
    };
    if let Some(column) = columns.strings {
        let (schema, array) = children(column);
        let mut count: ustore_size_t = 0;
        // SAFETY: the converter reads only the lengths and pointers of the live column.
        let status = unsafe { ffi::ustore_strings_view_from_arrow(schema, array, &mut rows.strings, &mut count) };
        converted(status, count, needed)?;
        rows.strings = strings_from(rows.strings, first);
    }
    Ok(rows)
}

// endregion: Columns

// region: Exports

/// Rows one batch of an export holds at most, a graph counting its source vertices.
const EXPORT_BATCH_ROWS: ustore_size_t = 65536;

/// The `errno` codes POSIX and the Windows C runtime share, which a stream callback answers.
const ENOMEM: c_int = 12;
const EINVAL: c_int = 22;
const EIO: c_int = 5;

/// The cursor an export walks, whose variant names the modality of its collection.
#[derive(Clone, Copy)]
enum ExportCursor {
    Blobs(blobs_ffi::ustore_blobs_cursor_t),
    Docs(docs_ffi::ustore_docs_cursor_t),
    Pages(pages_ffi::ustore_pages_cursor_t),
    Strings(strings_ffi::ustore_strings_cursor_t),
    Vertices(graph_ffi::ustore_graph_vertices_cursor_t),
}

impl ExportCursor {
    /// The cursor of `modality`, null until opened.
    fn of(modality: Modality) -> Self {
        match modality {
            Modality::Blobs => ExportCursor::Blobs(ptr::null_mut()),
            Modality::Docs => ExportCursor::Docs(ptr::null_mut()),
            Modality::Pages => ExportCursor::Pages(ptr::null_mut()),
            Modality::Strings => ExportCursor::Strings(ptr::null_mut()),
            Modality::Graph => ExportCursor::Vertices(ptr::null_mut()),
        }
    }
}

/// What an exported stream holds between pulls, which no batch points back to.
struct ExportStream {
    database: ustore_database_t,
    view: ustore_view_t,
    /// The snapshot of the head the stream took, freed after the cursor, or null for a caller's view.
    snapshot: ustore_snapshot_t,
    collection: CollectionID,
    modality: Modality,
    /// The cursor of the modality, null until opened.
    cursor: ExportCursor,
    /// Where a graph's vertices land before their edges are read into a batch's arena.
    vertices_arena: Option<Arena>,
    /// Why the last callback failed.
    failure: CString,
}

impl Drop for ExportStream {
    fn drop(&mut self) {
        // SAFETY: every handle is null or came from a successful init, and is freed once, the cursor before the
        // snapshot it reads.
        unsafe {
            match self.cursor {
                ExportCursor::Blobs(cursor) => blobs_ffi::ustore_blobs_cursor_free(cursor),
                ExportCursor::Docs(cursor) => docs_ffi::ustore_docs_cursor_free(cursor),
                ExportCursor::Pages(cursor) => pages_ffi::ustore_pages_cursor_free(cursor),
                ExportCursor::Strings(cursor) => strings_ffi::ustore_strings_cursor_free(cursor),
                ExportCursor::Vertices(cursor) => graph_ffi::ustore_graph_vertices_cursor_free(cursor),
            }
            db_ffi::ustore_snapshot_free(self.snapshot);
        }
    }
}

/// One exported schema: the root's children and the metadata they point at, freed by the root's release.
struct ExportedSchema {
    columns: [ArrowSchema; 3],
    children: [*mut ArrowSchema; 3],
    /// The metadata of the root, of a uuid column, and of the value column of pages.
    metadata: [Vec<u8>; 3],
}

/// One exported batch: the columns the exporters handed out, which its release releases.
struct ExportedBatch {
    columns: [ArrowArray; 3],
    count: usize,
    children: [*mut ArrowArray; 3],
    validity: [*const c_void; 1],
}

/// Encodes `pairs` as Arrow metadata: the count of pairs, then every key and value behind its length, each count a
/// 32-bit integer in the native byte order.
fn metadata(pairs: &[(&str, &str)]) -> Vec<u8> {
    let mut bytes = (pairs.len() as i32).to_ne_bytes().to_vec();
    for text in pairs.iter().flat_map(|&(key, value)| [key, value]) {
        bytes.extend_from_slice(&(text.len() as i32).to_ne_bytes());
        bytes.extend_from_slice(text.as_bytes());
    }
    bytes
}

unsafe extern "C" fn release_column_schema(schema: *mut ArrowSchema) {
    (*schema).release = None;
}

unsafe extern "C" fn release_schema(schema: *mut ArrowSchema) {
    drop(Box::from_raw((*schema).private_data.cast::<ExportedSchema>()));
    (*schema).release = None;
}

unsafe extern "C" fn release_batch(batch: *mut ArrowArray) {
    drop(Box::from_raw((*batch).private_data.cast::<ExportedBatch>()));
    (*batch).release = None;
}

/// The schema of a collection of `modality`: a struct of its columns, uuids tagged as `arrow.uuid`.
fn exported_schema(modality: Modality) -> ArrowSchema {
    let columns = columns_of(modality);
    let owned = Box::into_raw(Box::new(ExportedSchema {
        columns: array::from_fn(|_| ArrowSchema::empty()),
        children: [ptr::null_mut(); 3],
        metadata: [
            metadata(&[("ustore.modality", &modality.to_string())]),
            metadata(&[("ARROW:extension:name", "arrow.uuid"), ("ARROW:extension:metadata", "")]),
            metadata(&[("ustore.page_encoding", "xml")]),
        ],
    }));
    // SAFETY: the box was just leaked, and the root's release takes it back.
    unsafe {
        for (index, &(name, ragged)) in columns.iter().enumerate() {
            let column_metadata = match (ragged, modality) {
                (false, _) => (*owned).metadata[1].as_ptr(),
                (true, Modality::Pages) => (*owned).metadata[2].as_ptr(),
                (true, _) => ptr::null(),
            };
            (*owned).columns[index] = ArrowSchema {
                format: if ragged { c"z" } else { c"w:16" }.as_ptr(),
                name: name.as_ptr(),
                metadata: column_metadata.cast(),
                flags: 0,
                n_children: 0,
                children: ptr::null_mut(),
                dictionary: ptr::null_mut(),
                release: Some(release_column_schema),
                private_data: ptr::null_mut(),
            };
            (*owned).children[index] = ptr::addr_of_mut!((*owned).columns[index]);
        }
        ArrowSchema {
            format: c"+s".as_ptr(),
            name: c"".as_ptr(),
            metadata: (*owned).metadata[0].as_ptr().cast(),
            flags: 0,
            n_children: columns.len() as i64,
            children: ptr::addr_of_mut!((*owned).children).cast(),
            dictionary: ptr::null_mut(),
            release: Some(release_schema),
            private_data: owned.cast(),
        }
    }
}

impl ExportedBatch {
    /// Exports `count` keys out of `arena` as the next column.
    fn keys(&mut self, keys: ustore_keys_view_t, count: ustore_size_t, arena: &Arena) -> Result<()> {
        let mut schema = ArrowSchema::empty();
        let mut column = ffi::ArrowDeviceArray::empty();
        // SAFETY: a call wrote the keys into the arena, which the column pins until released.
        let status = unsafe { ffi::ustore_keys_view_to_arrow(keys, count, arena.handle, &mut schema, &mut column) };
        check(ustore_outcome_t {
            status,
            message: c"failed to export the keys of a batch".as_ptr(),
        })?;
        self.columns[self.count] = column.array;
        self.count += 1;
        Ok(())
    }

    /// Exports `count` strings out of `arena` as the next column, as `binary`, which the stream's schema names.
    fn strings(&mut self, strings: ustore_strings_view_t, count: ustore_size_t, arena: &Arena) -> Result<()> {
        let mut schema = ArrowSchema::empty();
        let mut column = ffi::ArrowDeviceArray::empty();
        // SAFETY: a call wrote the tape into the arena, which the column pins until released.
        let status = unsafe {
            ffi::ustore_strings_view_to_arrow(
                strings,
                count,
                USTORE_STRINGS_U32_TAPE_K,
                arena.handle,
                &mut schema,
                &mut column,
            )
        };
        check(ustore_outcome_t {
            status,
            message: c"failed to export the values of a batch".as_ptr(),
        })?;
        self.columns[self.count] = column.array;
        self.count += 1;
        Ok(())
    }

    /// The batch of `rows` rows over the exported columns, which its release releases.
    fn into_array(self: Box<Self>, rows: ustore_size_t) -> ArrowArray {
        let owned = Box::into_raw(self);
        // SAFETY: the box was just leaked, and the batch's release takes it back.
        unsafe {
            for index in 0..(*owned).count {
                (*owned).children[index] = ptr::addr_of_mut!((*owned).columns[index]);
            }
            ArrowArray {
                length: rows as i64,
                null_count: 0,
                offset: 0,
                n_buffers: 1,
                n_children: (*owned).count as i64,
                buffers: ptr::addr_of_mut!((*owned).validity).cast(),
                children: ptr::addr_of_mut!((*owned).children).cast(),
                dictionary: ptr::null_mut(),
                release: Some(release_batch),
                private_data: owned.cast(),
            }
        }
    }
}

impl ExportStream {
    /// Opens the cursor of the stream's modality over its collection.
    fn open(&mut self) -> Result<()> {
        let mut outcome = ustore_outcome_t::default();
        // SAFETY: every pointer in each `arguments` outlives its call, which is final on return.
        unsafe {
            match &mut self.cursor {
                ExportCursor::Docs(cursor) => docs_ffi::ustore_docs_cursor_init(&mut docs_ffi::ustore_docs_cursor_init_t {
                    cursor,
                    database: self.database,
                    executor: ptr::null_mut(),
                    outcome: &mut outcome,
                    view: self.view,
                    collection: self.collection,
                    start_key: Key::default(),
                    filter: Default::default(),
                    predicate: Default::default(),
                }),
                ExportCursor::Pages(cursor) => pages_ffi::ustore_pages_cursor_init(&mut pages_ffi::ustore_pages_cursor_init_t {
                    cursor,
                    database: self.database,
                    executor: ptr::null_mut(),
                    outcome: &mut outcome,
                    view: self.view,
                    collection: self.collection,
                    start_key: Key::default(),
                    filter: Default::default(),
                    predicate: Default::default(),
                }),
                ExportCursor::Strings(cursor) => {
                    strings_ffi::ustore_strings_cursor_init(&mut strings_ffi::ustore_strings_cursor_init_t {
                        cursor,
                        database: self.database,
                        executor: ptr::null_mut(),
                        outcome: &mut outcome,
                        view: self.view,
                        collection: self.collection,
                        prefix: Default::default(),
                        start_string: Default::default(),
                        patterns: Default::default(),
                        predicate: Default::default(),
                    })
                }
                ExportCursor::Vertices(cursor) => {
                    self.vertices_arena = Some(Arena::new()?);
                    graph_ffi::ustore_graph_vertices_cursor_init(&mut graph_ffi::ustore_graph_vertices_cursor_init_t {
                        cursor,
                        database: self.database,
                        executor: ptr::null_mut(),
                        outcome: &mut outcome,
                        view: self.view,
                        collection: self.collection,
                        start_key: Key::default(),
                        predicate: Default::default(),
                    })
                }
                ExportCursor::Blobs(cursor) => blobs_ffi::ustore_blobs_cursor_init(&mut blobs_ffi::ustore_blobs_cursor_init_t {
                    cursor,
                    database: self.database,
                    executor: ptr::null_mut(),
                    outcome: &mut outcome,
                    view: self.view,
                    collection: self.collection,
                    start_key: Key::default(),
                    patterns: Default::default(),
                    predicate: Default::default(),
                }),
            }
        }
        check(outcome)
    }

    /// Pulls the next batch into an arena of its own, which the last of its columns frees, or `None` once the walk
    /// ends.
    fn pull(&mut self) -> Result<Option<ArrowArray>> {
        let arena = Arena::new()?;
        let mut batch = Box::new(ExportedBatch {
            columns: array::from_fn(|_| ArrowArray::empty()),
            count: 0,
            children: [ptr::null_mut(); 3],
            validity: [ptr::null()],
        });
        let mut keys: *const ustore_key_t = ptr::null();
        let mut values = ustore_strings_view_t::default();
        let mut rows: ustore_size_t = 0;
        let mut outcome = ustore_outcome_t::default();
        // SAFETY: every pointer in each `arguments` outlives its call, which is final on return.
        unsafe {
            match self.cursor {
                ExportCursor::Vertices(cursor) => return self.pull_edges(cursor, batch, &arena),
                ExportCursor::Strings(cursor) => {
                    strings_ffi::ustore_strings_cursor_next(&mut strings_ffi::ustore_strings_cursor_next_t {
                        database: self.database,
                        executor: ptr::null_mut(),
                        outcome: &mut outcome,
                        completion: ustore_completion_t::Blocking,
                        cursor,
                        strings: &mut values,
                        values: &mut keys,
                        written: &mut rows,
                        arena: arena.handle,
                        wanted: EXPORT_BATCH_ROWS,
                        progress: None,
                        progress_state: ptr::null_mut(),
                    });
                    check(outcome)?;
                    if rows == 0 {
                        return Ok(None);
                    }
                    batch.strings(values, rows, &arena)?;
                    batch.keys(ustore_keys_view_t { data: keys, stride: 16 }, rows, &arena)?;
                    return Ok(Some(batch.into_array(rows)));
                }
                ExportCursor::Docs(cursor) => docs_ffi::ustore_docs_cursor_next(&mut docs_ffi::ustore_docs_cursor_next_t {
                    database: self.database,
                    executor: ptr::null_mut(),
                    outcome: &mut outcome,
                    completion: ustore_completion_t::Blocking,
                    cursor,
                    keys: &mut keys,
                    values: &mut values,
                    written: &mut rows,
                    arena: arena.handle,
                    wanted: EXPORT_BATCH_ROWS,
                    progress: None,
                    progress_state: ptr::null_mut(),
                }),
                ExportCursor::Pages(cursor) => pages_ffi::ustore_pages_cursor_next(&mut pages_ffi::ustore_pages_cursor_next_t {
                    database: self.database,
                    executor: ptr::null_mut(),
                    outcome: &mut outcome,
                    completion: ustore_completion_t::Blocking,
                    cursor,
                    keys: &mut keys,
                    values: &mut values,
                    written: &mut rows,
                    arena: arena.handle,
                    wanted: EXPORT_BATCH_ROWS,
                    outputs_encoding: PageEncoding::Xml as ustore_enum_t,
                    progress: None,
                    progress_state: ptr::null_mut(),
                }),
                ExportCursor::Blobs(cursor) => blobs_ffi::ustore_blobs_cursor_next(&mut blobs_ffi::ustore_blobs_cursor_next_t {
                    database: self.database,
                    executor: ptr::null_mut(),
                    outcome: &mut outcome,
                    completion: ustore_completion_t::Blocking,
                    cursor,
                    keys: &mut keys,
                    values: &mut values,
                    written: &mut rows,
                    arena: arena.handle,
                    wanted: EXPORT_BATCH_ROWS,
                    progress: None,
                    progress_state: ptr::null_mut(),
                }),
            }
        }
        check(outcome)?;
        if rows == 0 {
            return Ok(None);
        }
        batch.keys(ustore_keys_view_t { data: keys, stride: 16 }, rows, &arena)?;
        batch.strings(values, rows, &arena)?;
        Ok(Some(batch.into_array(rows)))
    }

    /// Pulls the edges leaving the next pages of vertices into `arena`, skipping pages whose vertices leave none, and
    /// exports their sources, targets and identifiers out of the triples.
    fn pull_edges(
        &mut self,
        cursor: graph_ffi::ustore_graph_vertices_cursor_t,
        mut batch: Box<ExportedBatch>,
        arena: &Arena,
    ) -> Result<Option<ArrowArray>> {
        let vertices_arena = self
            .vertices_arena
            .as_ref()
            .map_or(ptr::null_mut(), |arena| arena.handle);
        loop {
            let mut vertices: *const ustore_key_t = ptr::null();
            let mut written: ustore_size_t = 0;
            let mut outcome = ustore_outcome_t::default();
            // SAFETY: every pointer in `arguments` outlives the call, which is final on return.
            unsafe {
                graph_ffi::ustore_graph_vertices_cursor_next(&mut graph_ffi::ustore_graph_vertices_cursor_next_t {
                    database: self.database,
                    executor: ptr::null_mut(),
                    outcome: &mut outcome,
                    completion: ustore_completion_t::Blocking,
                    cursor,
                    keys: &mut vertices,
                    written: &mut written,
                    arena: vertices_arena,
                    wanted: EXPORT_BATCH_ROWS,
                    progress: None,
                    progress_state: ptr::null_mut(),
                })
            };
            check(outcome)?;
            if written == 0 {
                return Ok(None);
            }
            let role = Role::Source as ustore_enum_t;
            let mut degrees: *const graph_ffi::ustore_vertex_degree_t = ptr::null();
            let mut offsets: *const db_ffi::ustore_length_t = ptr::null();
            let mut triples: *const ustore_key_t = ptr::null();
            // SAFETY: every pointer in `arguments` outlives the call, which is final on return.
            unsafe {
                graph_ffi::ustore_graph_read_edges(&mut graph_ffi::ustore_graph_read_edges_t {
                    database: self.database,
                    executor: ptr::null_mut(),
                    outcome: &mut outcome,
                    completion: ustore_completion_t::Blocking,
                    view: self.view,
                    count: written,
                    collections: ustore_collections_view_t::repeated(&self.collection),
                    vertices: ustore_keys_view_t {
                        data: vertices,
                        stride: 16,
                    },
                    roles: db_ffi::ustore_strided_view_t::repeated(&role),
                    degrees_per_vertex: &mut degrees,
                    edges_offsets: &mut offsets,
                    edges_per_vertex: &mut triples,
                    arena: arena.handle,
                })
            };
            check(outcome)?;
            // SAFETY: a successful read wrote one more offset than vertices, or none for no edges at all.
            let edges = match offsets.is_null() {
                true => 0,
                false => unsafe { *offsets.add(written as usize) },
            };
            if edges == 0 && written == EXPORT_BATCH_ROWS {
                continue;
            }
            if edges == 0 {
                return Ok(None);
            }
            for column in 0..3 {
                let keys = ustore_keys_view_t {
                    data: triples.wrapping_add(column),
                    stride: 48,
                };
                batch.keys(keys, edges, arena)?;
            }
            return Ok(Some(batch.into_array(edges)));
        }
    }

    /// The `errno` code a stream callback answers for `failure`, which `get_last_error` then reports.
    fn failed(&mut self, failure: Error) -> c_int {
        self.failure = CString::new(failure.message).unwrap_or_default();
        match failure.status {
            Status::OutOfMemory => ENOMEM,
            Status::InvalidArgument => EINVAL,
            _ => EIO,
        }
    }
}

unsafe extern "C" fn export_get_schema(stream: *mut ArrowArrayStream, out: *mut ArrowSchema) -> c_int {
    let state = &*(*stream).private_data.cast::<ExportStream>();
    ptr::write(out, exported_schema(state.modality));
    0
}

unsafe extern "C" fn export_get_next(stream: *mut ArrowArrayStream, out: *mut ArrowArray) -> c_int {
    let state = &mut *(*stream).private_data.cast::<ExportStream>();
    match state.pull() {
        Ok(batch) => {
            ptr::write(out, batch.unwrap_or_else(ArrowArray::empty));
            0
        }
        Err(failure) => {
            ptr::write(out, ArrowArray::empty());
            state.failed(failure)
        }
    }
}

unsafe extern "C" fn export_get_last_error(stream: *mut ArrowArrayStream) -> *const c_char {
    (*(*stream).private_data.cast::<ExportStream>()).failure.as_ptr()
}

unsafe extern "C" fn export_release(stream: *mut ArrowArrayStream) {
    drop(Box::from_raw((*stream).private_data.cast::<ExportStream>()));
    (*stream).release = None;
}

/// The whole of `collection`, holding `modality`, as one stream read through `view`, or through a snapshot of the head
/// the stream takes and holds when `view` reads the head.
///
/// The stream pulls on the calling thread, since nothing ties its life to an executor's.
pub(crate) fn export<Parent>(
    view: &View<'_, Parent>,
    collection: CollectionID,
    modality: Modality,
) -> Result<ArrowArrayStream> {
    let mut state = Box::new(ExportStream {
        database: view.database(),
        view: view.view(),
        snapshot: ptr::null_mut(),
        collection,
        modality,
        cursor: ExportCursor::of(modality),
        vertices_arena: None,
        failure: CString::default(),
    });
    // A head cursor reads a key removed between its listing and its read as an empty value
    if state.view.is_null() {
        let mut outcome = ustore_outcome_t::default();
        let mut arguments = db_ffi::ustore_snapshot_init_t {
            snapshot: &mut state.snapshot,
            database: state.database,
            executor: ptr::null_mut(),
            outcome: &mut outcome,
            completion: ustore_completion_t::Blocking,
            read_stamp: ptr::null_mut(),
        };
        // SAFETY: every pointer in `arguments` outlives the call, which is final on return.
        unsafe { db_ffi::ustore_snapshot_init(&mut arguments) };
        check(outcome)?;
        // SAFETY: the snapshot was just taken, and the stream frees it after its cursor.
        state.view = unsafe { db_ffi::ustore_snapshot_view(state.snapshot) };
    }
    state.open()?;
    Ok(ArrowArrayStream {
        get_schema: Some(export_get_schema),
        get_next: Some(export_get_next),
        get_last_error: Some(export_get_last_error),
        release: Some(export_release),
        private_data: Box::into_raw(state).cast(),
    })
}

// endregion: Exports

// region: Imports

/// Rows one write of an import holds at most, halved for a write the head refuses for its size.
const IMPORT_CHUNK_ROWS: usize = 65536;

/// Where every write of an import lands: a collection through the view's database, executor and transaction.
struct Target {
    database: ustore_database_t,
    executor: ustore_executor_t,
    transaction: ustore_transaction_t,
    watch: Watch,
    collection: CollectionID,
    modality: Modality,
}

/// Writes rows `[first, first + count)` of `rows` through the writes of the target's modality.
fn write_chunk(target: &Target, rows: &Rows, first: usize, count: usize, durability: Durability) -> ustore_outcome_t {
    let mut outcome = ustore_outcome_t::default();
    let collections = ustore_collections_view_t::repeated(&target.collection);
    let keys = keys_from(rows.keys, first);
    let strings = strings_from(rows.strings, first);
    let count = count as ustore_size_t;
    // SAFETY: every view addresses `count` rows of a live batch, and every pointer outlives the call, which is final
    // on return.
    unsafe {
        match target.modality {
            Modality::Docs => docs_ffi::ustore_docs_write(&mut docs_ffi::ustore_docs_write_t {
                database: target.database,
                executor: target.executor,
                outcome: &mut outcome,
                completion: ustore_completion_t::Blocking,
                transaction: target.transaction,
                count,
                collections,
                keys,
                id_field: Default::default(),
                fields: Default::default(),
                values: strings,
                type_: <Json as Format>::TYPE,
                modification: Modification::Upsert as ustore_enum_t,
                durability,
                watch: target.watch,
            }),
            Modality::Pages => pages_ffi::ustore_pages_import(&mut pages_ffi::ustore_pages_import_t {
                database: target.database,
                executor: target.executor,
                outcome: &mut outcome,
                completion: ustore_completion_t::Blocking,
                transaction: target.transaction,
                count,
                collections,
                keys,
                values: strings,
                encoding: PageEncoding::Xml as ustore_enum_t,
                durability,
                watch: target.watch,
                progress: None,
                progress_state: ptr::null_mut(),
            }),
            Modality::Strings => strings_ffi::ustore_strings_write(&mut strings_ffi::ustore_strings_write_t {
                database: target.database,
                executor: target.executor,
                outcome: &mut outcome,
                completion: ustore_completion_t::Blocking,
                transaction: target.transaction,
                count,
                collections,
                strings,
                values: keys,
                durability,
                watch: target.watch,
            }),
            Modality::Graph => graph_ffi::ustore_graph_write_edges(&mut graph_ffi::ustore_graph_write_edges_t {
                database: target.database,
                executor: target.executor,
                outcome: &mut outcome,
                completion: ustore_completion_t::Blocking,
                transaction: target.transaction,
                count,
                collections,
                edges: keys_from(rows.edges, first),
                sources: keys_from(rows.sources, first),
                targets: keys_from(rows.targets, first),
                durability,
                watch: target.watch,
            }),
            Modality::Blobs => blobs_ffi::ustore_blobs_write(&mut blobs_ffi::ustore_blobs_write_t {
                database: target.database,
                executor: target.executor,
                outcome: &mut outcome,
                completion: ustore_completion_t::Blocking,
                transaction: target.transaction,
                count,
                collections,
                keys,
                values: strings,
                durability,
                watch: target.watch,
            }),
        }
    }
    outcome
}

/// Writes every row of `rows` in chunks, halving one the head refused for its size, each buffered but the last.
fn write_rows(target: &Target, rows: &Rows, durability: Durability) -> Result<()> {
    let mut chunk = IMPORT_CHUNK_ROWS;
    let mut first = 0;
    while first != rows.count {
        let count = chunk.min(rows.count - first);
        let last = first + count == rows.count;
        let chunk_durability = if last { durability } else { Durability::Buffered };
        let outcome = write_chunk(target, rows, first, count, chunk_durability);
        if outcome.status.0 == Status::CapacityExhausted as c_int && target.transaction.is_null() && count > 1 {
            chunk = count / 2;
            continue;
        }
        check(outcome)?;
        first += count;
    }
    Ok(())
}

/// Writes every batch of `stream` into `collection`, holding `modality`, through the writes of `view`, the last
/// paying `durability`, answering the rows written.
pub(crate) fn import<Parent>(
    view: &View<'_, Parent>,
    collection: CollectionID,
    modality: Modality,
    mut stream: ArrowArrayStream,
    durability: Durability,
) -> Result<usize> {
    let target = Target {
        database: view.database(),
        executor: view.executor(),
        transaction: view.transaction(),
        watch: view.watch(),
        collection,
        modality,
    };
    let schema = stream.schema()?;
    let columns = resolve_columns(&schema, modality)?;
    let mut written = 0;
    let mut current = stream.next_batch()?;
    while let Some(batch) = current {
        let upcoming = stream.next_batch()?;
        let rows = rows_of(&schema, &batch, &columns)?;
        let batch_durability = if upcoming.is_some() {
            Durability::Buffered
        } else {
            durability
        };
        write_rows(&target, &rows, batch_durability)?;
        written += rows.count;
        current = upcoming;
    }
    Ok(written)
}

// endregion: Imports

// region: Ingests

/// An ingest, freed with every row of an unfinished one when dropped.
struct Ingest(ffi::ustore_ingest_t);

impl Drop for Ingest {
    fn drop(&mut self) {
        // SAFETY: the handle is null or came from a successful init, and is freed once.
        unsafe { ffi::ustore_ingest_free(self.0) };
    }
}

/// Lands every batch of `stream` into `collection`, holding `modality`, as one commit of its own at the head,
/// answering the rows landed.
pub(crate) fn ingest(
    database: ustore_database_t,
    collection: CollectionID,
    modality: Modality,
    mut stream: ArrowArrayStream,
    order: IngestOrder,
    durability: Durability,
) -> Result<usize> {
    let schema = stream.schema()?;
    let columns = resolve_columns(&schema, modality)?;
    let mut ingest = Ingest(ptr::null_mut());
    let mut outcome = ustore_outcome_t::default();
    let mut init = ffi::ustore_ingest_init_t {
        ingest: &mut ingest.0,
        database,
        outcome: &mut outcome,
        collection,
        order,
    };
    // SAFETY: every pointer in `init` outlives the call, which is final on return.
    unsafe { ffi::ustore_ingest_init(&mut init) };
    check(outcome)?;
    let mut written = 0;
    while let Some(batch) = stream.next_batch()? {
        let rows = rows_of(&schema, &batch, &columns)?;
        let mut append = ffi::ustore_ingest_append_t {
            ingest: ingest.0,
            outcome: &mut outcome,
            count: rows.count as ustore_size_t,
            keys: rows.keys,
            values: rows.strings,
            sources: rows.sources,
            targets: rows.targets,
            edges: rows.edges,
        };
        // SAFETY: the views address the rows of the live batch, which the append copies before returning.
        unsafe { ffi::ustore_ingest_append(&mut append) };
        check(outcome)?;
        written += rows.count;
    }
    let mut finish = ffi::ustore_ingest_finish_t {
        ingest: ingest.0,
        outcome: &mut outcome,
        durability,
        commit_stamp: ptr::null_mut(),
    };
    // SAFETY: every pointer in `finish` outlives the call, which is final on return.
    unsafe { ffi::ustore_ingest_finish(&mut finish) };
    check(outcome)?;
    Ok(written)
}

// endregion: Ingests

// region: Raw Declarations

/// One-to-one mirror of `c/ustore/arrow.h`.
pub(crate) mod ffi {
    #![allow(non_camel_case_types)]

    use std::ffi::c_void;
    use std::ptr;

    use super::{ArrowArray, ArrowSchema, IngestOrder};
    use crate::db::ffi::{
        ustore_arena_t, ustore_collection_id_t, ustore_database_t, ustore_durability_t, ustore_enum_t, ustore_keys_view_t,
        ustore_outcome_t, ustore_size_t, ustore_stamp_t, ustore_status_t, ustore_strings_view_t,
    };

    #[repr(C)]
    pub struct ArrowDeviceArray {
        pub array: ArrowArray,
        pub device_id: i64,
        pub device_type: i32,
        pub sync_event: *mut c_void,
        pub reserved: [i64; 3],
    }

    impl ArrowDeviceArray {
        /// A released array, for an exporter to write into.
        pub fn empty() -> Self {
            ArrowDeviceArray {
                array: ArrowArray::empty(),
                device_id: 0,
                device_type: 0,
                sync_event: ptr::null_mut(),
                reserved: [0; 3],
            }
        }
    }

    #[repr(C)]
    pub struct ustore_ingest_handle_t {
        _private: [u8; 0],
    }

    pub type ustore_ingest_t = *mut ustore_ingest_handle_t;

    #[repr(C)]
    pub struct ustore_ingest_init_t {
        pub ingest: *mut ustore_ingest_t,
        pub database: ustore_database_t,
        pub outcome: *mut ustore_outcome_t,
        pub collection: ustore_collection_id_t,
        pub order: IngestOrder,
    }

    #[repr(C)]
    pub struct ustore_ingest_append_t {
        pub ingest: ustore_ingest_t,
        pub outcome: *mut ustore_outcome_t,
        pub count: ustore_size_t,
        pub keys: ustore_keys_view_t,
        pub values: ustore_strings_view_t,
        pub sources: ustore_keys_view_t,
        pub targets: ustore_keys_view_t,
        pub edges: ustore_keys_view_t,
    }

    #[repr(C)]
    pub struct ustore_ingest_finish_t {
        pub ingest: ustore_ingest_t,
        pub outcome: *mut ustore_outcome_t,
        pub durability: ustore_durability_t,
        pub commit_stamp: *mut ustore_stamp_t,
    }

    extern "C" {
        pub fn ustore_keys_view_from_arrow(
            schema: *const ArrowSchema,
            array: *const ArrowArray,
            keys: *mut ustore_keys_view_t,
            count: *mut ustore_size_t,
        ) -> ustore_status_t;
        pub fn ustore_strings_view_from_arrow(
            schema: *const ArrowSchema,
            array: *const ArrowArray,
            strings: *mut ustore_strings_view_t,
            count: *mut ustore_size_t,
        ) -> ustore_status_t;
        pub fn ustore_keys_view_to_arrow(
            keys: ustore_keys_view_t,
            count: ustore_size_t,
            arena: ustore_arena_t,
            schema: *mut ArrowSchema,
            array: *mut ArrowDeviceArray,
        ) -> ustore_status_t;
        pub fn ustore_strings_view_to_arrow(
            strings: ustore_strings_view_t,
            count: ustore_size_t,
            layout: ustore_enum_t,
            arena: ustore_arena_t,
            schema: *mut ArrowSchema,
            array: *mut ArrowDeviceArray,
        ) -> ustore_status_t;
        pub fn ustore_ingest_init(arguments: *mut ustore_ingest_init_t);
        pub fn ustore_ingest_append(arguments: *mut ustore_ingest_append_t);
        pub fn ustore_ingest_finish(arguments: *mut ustore_ingest_finish_t);
        pub fn ustore_ingest_free(ingest: ustore_ingest_t);
    }
}

// endregion: Raw Declarations

#[cfg(test)]
mod tests {
    use std::slice;

    use super::*;
    use crate::db::{Database, NVMEOptions, RAMOptions, RocksDBOptions};
    use crate::testing::{key, temporary_directory};

    /// The rows of every batch `stream` answers, each a key and its value, read through the C data interface alone.
    fn rows_of(mut stream: ArrowArrayStream) -> Vec<(Vec<u8>, Vec<u8>)> {
        let schema = stream.schema().unwrap();
        // SAFETY: an exported blobs schema is a struct of two children.
        let formats: Vec<&CStr> = (0..schema.n_children as usize)
            .map(|index| unsafe { CStr::from_ptr((**schema.children.add(index)).format) })
            .collect();
        assert_eq!(formats, [c"w:16", c"z"]);
        let mut rows = Vec::new();
        while let Some(batch) = stream.next_batch().unwrap() {
            // SAFETY: the batch carries a keys child of two buffers and a values child of three, as its schema says.
            unsafe {
                let keys = &**batch.children;
                let values = &**batch.children.add(1);
                let key_bytes = *keys.buffers.add(1) as *const [u8; 16];
                let offsets = *values.buffers.add(1) as *const u32;
                let bytes = *values.buffers.add(2) as *const u8;
                for row in 0..batch.length as usize {
                    let key = (*key_bytes.add(keys.offset as usize + row)).to_vec();
                    let first = *offsets.add(values.offset as usize + row) as usize;
                    let last = *offsets.add(values.offset as usize + row + 1) as usize;
                    rows.push((key, slice::from_raw_parts(bytes.add(first), last - first).to_vec()));
                }
            }
        }
        rows
    }

    /// Fills a `posters` collection of `database` with 50 blobs, answering it.
    fn posters(database: &Database) -> crate::Collection<'_> {
        let mut posters = database.collection("posters").unwrap();
        for index in 1..=50u64 {
            posters
                .blobs()
                .set(key(index), format!("poster-{index}").as_bytes())
                .unwrap();
        }
        posters
    }

    /// Ingests the posters of `database` into another collection, which then exports the same rows.
    fn ingests_every_row(database: &Database) {
        let mut posters = posters(database);
        database.collection_create("copies", Modality::Blobs).unwrap();
        let mut copies = database.collection("copies").unwrap();
        let exported = posters.export_arrow().unwrap();
        let landed = copies.ingest_arrow(exported, IngestOrder::Sorted, Durability::Buffered);
        assert_eq!(landed.unwrap(), 50);
        assert_eq!(
            rows_of(copies.export_arrow().unwrap()),
            rows_of(posters.export_arrow().unwrap())
        );
    }

    #[test]
    fn arrow_export_imports_into_another_collection() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let mut posters = posters(&database);
        database.collection_create("copies", Modality::Blobs).unwrap();
        let mut copies = database.collection("copies").unwrap();

        let exported = posters.export_arrow().unwrap();
        assert_eq!(copies.import_arrow(exported, Durability::Buffered).unwrap(), 50);
        let originals = rows_of(posters.export_arrow().unwrap());
        assert_eq!(originals.len(), 50);
        assert_eq!(originals[0], (key(1).0 .0.to_vec(), b"poster-1".to_vec()));
        assert_eq!(rows_of(copies.export_arrow().unwrap()), originals);
    }

    #[test]
    fn arrow_ingest_lands_every_row_on_ram_and_nvme() {
        ingests_every_row(&Database::init_ram(&RAMOptions::default()).unwrap());
        let directory = temporary_directory("arrow_ingest_nvme");
        match Database::open_nvme(&NVMEOptions::at(directory.path())) {
            Ok(database) => ingests_every_row(&database),
            Err(failure) => assert_eq!((failure.status, cfg!(feature = "nvme")), (Status::Unsupported, false)),
        }
    }

    #[test]
    fn arrow_ingest_is_unsupported_and_takes_the_stream() {
        let directory = temporary_directory("arrow_ingest_rocksdb");
        let Ok(database) = Database::open_rocksdb(&RocksDBOptions::at(directory.path())) else {
            return assert!(!cfg!(feature = "rocksdb"));
        };
        let mut posters = posters(&database);
        let exported = posters.export_arrow().unwrap();
        let refused = posters.ingest_arrow(exported, IngestOrder::Sorted, Durability::Buffered);
        assert_eq!(refused.unwrap_err().status, Status::Unsupported);
    }

    #[test]
    fn arrow_structs_match_the_specification() {
        assert_eq!(std::mem::size_of::<ArrowSchema>(), 72);
        assert_eq!(std::mem::size_of::<ArrowArray>(), 80);
        assert_eq!(std::mem::size_of::<ArrowArrayStream>(), 40);
        assert_eq!(std::mem::size_of::<ffi::ArrowDeviceArray>(), 128);
    }
}
