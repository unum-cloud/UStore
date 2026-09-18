//! JSON documents - the Rust twin of `include/ustore/docs.h`.
//!
//! The ABI carries documents as JSON text both ways and parses them inside the engine, so nothing here
//! parses JSON. A field is a top-level key or an RFC 6901 pointer the engine reads up to its null
//! terminator, which is why every field is a `&CStr`: a `c"/user/name"` literal crosses as it stands.
//!
//! ```no_run
//! use ustore::{Arena, Column, Configuration, Database, DocumentEntry, Durability, FieldAddress, FieldEntry};
//! use ustore::{Json, Modification, Text};
//!
//! let database = Database::open(Configuration::in_memory())?;
//! let mut arena = Arena::new();
//! let people = database.create_collection(&mut arena, "people")?;
//!
//! let documents = [DocumentEntry::<Json>::new(1, r#"{"name":"Ada","born":1815}"#)?];
//! database.head().write_documents(&mut arena, people, &documents, Modification::Upsert, Durability::Buffered)?;
//! let died = [FieldEntry::<i64>::new(1, c"died", &1852)?];
//! database.head().write_fields(&mut arena, people, &died, Modification::Upsert, Durability::Buffered)?;
//!
//! let names = database.head().read_fields::<Text>(&mut arena, people, &[FieldAddress::new(1, c"name")])?;
//! assert_eq!(names.get(0), Some(b"Ada".as_slice()));
//!
//! let columns = [Column::new::<i64>(c"born"), Column::new::<Text>(c"name")];
//! let table = database.head().gather(&mut arena, people, &[1], &columns)?;
//! assert_eq!(table.scalars::<i64>(0)?, &[1815]);
//! # Ok::<(), ustore::Error>(())
//! ```
//!
//! One batch writes one format, so a batch of fields mixing two does not compile:
//!
//! ```compile_fail
//! use ustore::{FieldEntry, Json};
//!
//! let fields = [FieldEntry::<i64>::new(1, c"age", &36)?, FieldEntry::<Json>::new(1, c"name", "\"Ada\"")?];
//! # Ok::<(), ustore::Error>(())
//! ```
//!
//! A batch of whole documents carries one format too, so the same mixture is refused there:
//!
//! ```compile_fail
//! use ustore::{DocumentEntry, Json};
//!
//! let documents = [DocumentEntry::<Json>::new(1, "{}")?, DocumentEntry::<i64>::new(2, &1815)?];
//! # Ok::<(), ustore::Error>(())
//! ```
//!
//! Neither [`Half`] nor [`Uuid`] is a format a write carries, so a document written as one does not
//! compile either, however it reads back in a gathered column:
//!
//! ```compile_fail
//! use ustore::{DocumentEntry, Half};
//!
//! let documents = [DocumentEntry::<Half>::new(1, &Half(0x3e00))?];
//! # Ok::<(), ustore::Error>(())
//! ```
//!
//! A gathered table borrows the arena like any other result, so the call that would overwrite its columns
//! does not compile while it lives:
//!
//! ```compile_fail
//! use ustore::{Arena, Collection, Column, Configuration, Database, Text};
//!
//! let database = Database::open(Configuration::in_memory())?;
//! let mut arena = Arena::new();
//! let columns = [Column::new::<Text>(c"name")];
//! let table = database.head().gather(&mut arena, Collection::main(), &[1], &columns)?;
//! let paths = database.head().gist(&mut arena, Collection::main(), &[1])?;
//! assert_eq!(table.rows(), paths.len());
//! # Ok::<(), ustore::Error>(())
//! ```

use std::ffi::{c_char, c_int, CStr};
use std::marker::PhantomData;
use std::ptr;

use crate::blobs::Values;
use crate::db::ffi::{
    ustore_arena_t, ustore_byte_t, ustore_bytes_cptr_t, ustore_char_t, ustore_collection_t, ustore_database_t,
    ustore_enum_t, ustore_error_t, ustore_key_t, ustore_length_t, ustore_octet_t, ustore_size_t, ustore_snapshot_t,
    ustore_str_view_t, ustore_transaction_t,
};
use crate::db::{
    check, described_length, exported_slice, fail, Arena, Collections, Durability, Key, Result, Status, View,
};

// region: Raw Declarations

/// One-to-one mirror of `include/ustore/docs.h`.
mod ffi {
    #![allow(non_camel_case_types)]

    use super::*;

    #[repr(C)]
    pub struct ustore_docs_write_t {
        pub db: ustore_database_t,
        pub error: *mut ustore_error_t,
        pub transaction: ustore_transaction_t,
        pub arena: *mut ustore_arena_t,
        pub options: ustore_enum_t,
        pub tasks_count: ustore_size_t,
        pub type_: ustore_enum_t,
        pub modification: ustore_enum_t,
        pub collections: *const ustore_collection_t,
        pub collections_stride: ustore_size_t,
        pub keys: *const ustore_key_t,
        pub keys_stride: ustore_size_t,
        pub fields: *const ustore_str_view_t,
        pub fields_stride: ustore_size_t,
        pub presences: *const ustore_octet_t,
        pub offsets: *const ustore_length_t,
        pub offsets_stride: ustore_size_t,
        pub lengths: *const ustore_length_t,
        pub lengths_stride: ustore_size_t,
        pub values: *const ustore_bytes_cptr_t,
        pub values_stride: ustore_size_t,
        pub id_field: ustore_str_view_t,
    }

    #[repr(C)]
    pub struct ustore_docs_read_t {
        pub db: ustore_database_t,
        pub error: *mut ustore_error_t,
        pub transaction: ustore_transaction_t,
        pub snapshot: ustore_snapshot_t,
        pub arena: *mut ustore_arena_t,
        pub options: ustore_enum_t,
        pub type_: ustore_enum_t,
        pub tasks_count: ustore_size_t,
        pub collections: *const ustore_collection_t,
        pub collections_stride: ustore_size_t,
        pub keys: *const ustore_key_t,
        pub keys_stride: ustore_size_t,
        pub fields: *const ustore_str_view_t,
        pub fields_stride: ustore_size_t,
        pub presences: *mut *mut ustore_octet_t,
        pub offsets: *mut *mut ustore_length_t,
        pub lengths: *mut *mut ustore_length_t,
        pub values: *mut *mut ustore_byte_t,
    }

    #[repr(C)]
    pub struct ustore_docs_gist_t {
        pub db: ustore_database_t,
        pub error: *mut ustore_error_t,
        pub transaction: ustore_transaction_t,
        pub snapshot: ustore_snapshot_t,
        pub arena: *mut ustore_arena_t,
        pub options: ustore_enum_t,
        pub docs_count: ustore_size_t,
        pub collections: *const ustore_collection_t,
        pub collections_stride: ustore_size_t,
        pub keys: *const ustore_key_t,
        pub keys_stride: ustore_size_t,
        pub fields_count: *mut ustore_size_t,
        pub offsets: *mut *mut ustore_length_t,
        pub fields: *mut *mut ustore_char_t,
    }

    #[repr(C)]
    pub struct ustore_docs_gather_t {
        pub db: ustore_database_t,
        pub error: *mut ustore_error_t,
        pub transaction: ustore_transaction_t,
        pub snapshot: ustore_snapshot_t,
        pub arena: *mut ustore_arena_t,
        pub options: ustore_enum_t,
        pub docs_count: ustore_size_t,
        pub fields_count: ustore_size_t,
        pub collections: *const ustore_collection_t,
        pub collections_stride: ustore_size_t,
        pub keys: *const ustore_key_t,
        pub keys_stride: ustore_size_t,
        pub fields: *const ustore_str_view_t,
        pub fields_stride: ustore_size_t,
        pub types: *const ustore_enum_t,
        pub types_stride: ustore_size_t,
        pub columns_validities: *mut *mut *mut ustore_octet_t,
        pub columns_conversions: *mut *mut *mut ustore_octet_t,
        pub columns_collisions: *mut *mut *mut ustore_octet_t,
        pub columns_scalars: *mut *mut *mut ustore_byte_t,
        pub columns_offsets: *mut *mut *mut ustore_length_t,
        pub columns_lengths: *mut *mut *mut ustore_length_t,
        pub joined_strings: *mut *mut ustore_byte_t,
    }

    extern "C" {
        pub fn ustore_docs_write(arguments: *mut ustore_docs_write_t);
        pub fn ustore_docs_read(arguments: *mut ustore_docs_read_t);
        pub fn ustore_docs_gist(arguments: *mut ustore_docs_gist_t);
        pub fn ustore_docs_gather(arguments: *mut ustore_docs_gather_t);
    }
}

// endregion: Raw Declarations

// region: Formats

/// Keeps the format traits closed over the types this module names.
mod sealed {
    /// A format with a `ustore_doc_field_type_t` of its own.
    pub trait Format {
        /// The `ustore_doc_field_type_t` this format crosses the ABI as.
        const TYPE: std::ffi::c_int;
    }
}

use sealed::Format;

/// A fixed-width value a gathered column converts into.
pub trait Scalar: Format + Copy {}

/// A scalar a write carries into a whole document or into one of its fields.
///
/// [`Half`] and [`Uuid`] are gathered but never written: the engine builds a document from the bytes
/// of a scalar and has no JSON spelling for either.
pub trait WritableScalar: Scalar {}

/// JSON text: a whole value written into a field, or a field read back as the JSON it holds.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Hash)]
pub struct Json;

/// A string: a field written as a JSON string, or a value read and gathered as printed text.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Hash)]
pub struct Text;

/// A gathered column of printed text without null terminators.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Hash)]
pub struct Binary;

/// A gathered half-precision float, held as the two bytes of its IEEE 754 binary16 encoding.
///
/// The crate reinterprets nothing: a value whose magnitude will not fit sets the collision bit of its
/// row and leaves the cell zero.
#[repr(transparent)]
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub struct Half(pub u16);

/// A gathered UUID, sixteen bytes in the order canonical text writes them.
///
/// The engine parses the canonical thirty-six character form; anything else sets the collision bit of
/// its row and leaves the cell zero.
#[repr(transparent)]
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub struct Uuid(pub [u8; 16]);

impl Format for f64 {
    const TYPE: c_int = 42;
}
impl Format for f32 {
    const TYPE: c_int = 41;
}
impl Format for Half {
    const TYPE: c_int = 40;
}
impl Format for i64 {
    const TYPE: c_int = 23;
}
impl Format for i32 {
    const TYPE: c_int = 22;
}
impl Format for i16 {
    const TYPE: c_int = 21;
}
impl Format for i8 {
    const TYPE: c_int = 20;
}
impl Format for u64 {
    const TYPE: c_int = 33;
}
impl Format for u32 {
    const TYPE: c_int = 32;
}
impl Format for u16 {
    const TYPE: c_int = 31;
}
impl Format for u8 {
    const TYPE: c_int = 30;
}
impl Format for bool {
    const TYPE: c_int = 11;
}
impl Format for Uuid {
    const TYPE: c_int = 12;
}
impl Format for Json {
    const TYPE: c_int = 0;
}
impl Format for Text {
    const TYPE: c_int = 51;
}
impl Format for Binary {
    const TYPE: c_int = 50;
}

impl Scalar for f64 {}
impl Scalar for f32 {}
impl Scalar for Half {}
impl Scalar for i64 {}
impl Scalar for i32 {}
impl Scalar for i16 {}
impl Scalar for i8 {}
impl Scalar for u64 {}
impl Scalar for u32 {}
impl Scalar for u16 {}
impl Scalar for u8 {}
impl Scalar for bool {}
impl Scalar for Uuid {}

impl WritableScalar for f64 {}
impl WritableScalar for f32 {}
impl WritableScalar for i64 {}
impl WritableScalar for i32 {}
impl WritableScalar for i16 {}
impl WritableScalar for i8 {}
impl WritableScalar for u64 {}
impl WritableScalar for u32 {}
impl WritableScalar for u16 {}
impl WritableScalar for u8 {}
impl WritableScalar for bool {}

/// The format of the values one field write carries, which the whole batch shares.
pub trait FieldFormat: Format {
    /// What a field of this format is written from.
    type Value: ?Sized;

    /// The bytes the ABI reads for `value`.
    fn bytes(value: &Self::Value) -> &[u8];
}

impl<Value: WritableScalar> FieldFormat for Value {
    type Value = Value;

    fn bytes(value: &Value) -> &[u8] {
        // SAFETY: every `WritableScalar` is a primitive without padding, so each of its bytes is initialized.
        unsafe { std::slice::from_raw_parts((value as *const Value).cast::<u8>(), size_of::<Value>()) }
    }
}

impl FieldFormat for Json {
    type Value = str;

    fn bytes(value: &str) -> &[u8] {
        value.as_bytes()
    }
}

impl FieldFormat for Text {
    type Value = str;

    fn bytes(value: &str) -> &[u8] {
        value.as_bytes()
    }
}

/// The format a field read exports its values in.
pub trait ReadFormat: Format {}

impl ReadFormat for Json {}
impl ReadFormat for Text {}

/// The format a gathered column converts its values into.
pub trait ColumnFormat: Format {}

impl<Value: Scalar> ColumnFormat for Value {}
impl ColumnFormat for Text {}
impl ColumnFormat for Binary {}

/// How a write combines what it carries with what the document already holds.
#[repr(i32)]
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub enum Modification {
    /// Replaces the document or the field, creating it when missing.
    #[default]
    Upsert = 0,
    /// Replaces the document or the field, which must exist.
    Update = 1,
    /// Creates the document or the field, which must not exist.
    Insert = 2,
    /// Applies the carried RFC 6902 patch.
    Patch = 3,
    /// Applies the carried RFC 7386 merge patch.
    Merge = 4,
}

// endregion: Formats

// region: Inputs

/// A document and the key it is stored under, laid out so a slice feeds the strided ABI - one array
/// for the keys, one for the pointers, one for the lengths.
///
/// A whole document is written in one format the batch shares: [`Json`] for the text the engine parses,
/// [`Text`] for a document that is one JSON string, or any [`WritableScalar`] for a document that is
/// one number or one boolean.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct DocumentEntry<'value, Format: FieldFormat> {
    key: Key,
    value: *const u8,
    length: ustore_length_t,
    marker: PhantomData<&'value Format::Value>,
}

impl<'value, Format: FieldFormat> DocumentEntry<'value, Format> {
    /// Pairs a key with the document to store under it, refusing one past the 32-bit length the ABI
    /// carries.
    pub fn new(key: Key, value: &'value Format::Value) -> Result<Self> {
        let bytes = Format::bytes(value);
        Ok(DocumentEntry {
            key,
            value: bytes.as_ptr(),
            length: described_length(bytes.len(), "the document is longer than the ABI can describe")?,
            marker: PhantomData,
        })
    }

    /// Names a key whose document a batch removes, which it mixes freely with the entries that carry one.
    pub fn removal(key: Key) -> Self {
        DocumentEntry {
            key,
            value: ptr::null(),
            length: 0,
            marker: PhantomData,
        }
    }

    /// The key this entry stores under.
    pub fn key(&self) -> Key {
        self.key
    }
}

/// A document carrying its own key under an id field, laid out so a slice feeds the strided ABI.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct Document<'text> {
    text: *const u8,
    length: ustore_length_t,
    marker: PhantomData<&'text str>,
}

impl<'text> Document<'text> {
    /// Borrows the JSON text of a document, refusing one past the 32-bit length the ABI carries.
    pub fn new(text: &'text str) -> Result<Self> {
        Ok(Document {
            text: text.as_ptr(),
            length: described_length(text.len(), "the document is longer than the ABI can describe")?,
            marker: PhantomData,
        })
    }
}

/// A field of one document and the value written into it, laid out so a slice feeds the strided ABI.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct FieldEntry<'value, Format: FieldFormat> {
    key: Key,
    field: *const c_char,
    value: *const u8,
    length: ustore_length_t,
    marker: PhantomData<&'value Format::Value>,
}

impl<'value, Format: FieldFormat> FieldEntry<'value, Format> {
    /// Pairs a field with the value to write into it, refusing one past the 32-bit length the ABI carries.
    pub fn new(key: Key, field: &'value CStr, value: &'value Format::Value) -> Result<Self> {
        let bytes = Format::bytes(value);
        Ok(FieldEntry {
            key,
            field: field.as_ptr(),
            value: bytes.as_ptr(),
            length: described_length(bytes.len(), "the value is longer than the ABI can describe")?,
            marker: PhantomData,
        })
    }

    /// Names a field to remove from its document, whatever modification the batch applies.
    pub fn removal(key: Key, field: &'value CStr) -> Self {
        FieldEntry {
            key,
            field: field.as_ptr(),
            value: ptr::null(),
            length: 0,
            marker: PhantomData,
        }
    }
}

/// A field of one document to read, laid out so a slice feeds the strided ABI.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct FieldAddress<'field> {
    key: Key,
    field: *const c_char,
    marker: PhantomData<&'field CStr>,
}

impl<'field> FieldAddress<'field> {
    /// Addresses `field` of the document under `key`.
    pub fn new(key: Key, field: &'field CStr) -> Self {
        FieldAddress {
            key,
            field: field.as_ptr(),
            marker: PhantomData,
        }
    }
}

/// A field to gather from every document and the type to convert it into, laid out so a slice feeds
/// the strided ABI.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct Column<'field> {
    field: *const c_char,
    format: c_int,
    marker: PhantomData<&'field CStr>,
}

impl<'field> Column<'field> {
    /// Gathers `field` converted into `Format`.
    pub fn new<Format: ColumnFormat>(field: &'field CStr) -> Self {
        Column {
            field: field.as_ptr(),
            format: Format::TYPE,
            marker: PhantomData,
        }
    }
}

// endregion: Inputs

// region: Outputs

/// The unique paths to the leaves of the documents one gist inspected, in the order they were found.
#[derive(Clone, Copy, Debug)]
pub struct Paths<'arena> {
    offsets: *const ustore_length_t,
    tape: *const ustore_char_t,
    count: usize,
    marker: PhantomData<&'arena Arena>,
}

impl<'arena> Paths<'arena> {
    /// How many unique paths the gist found.
    pub fn len(&self) -> usize {
        self.count
    }

    /// Whether the gist found no leaves at all.
    pub fn is_empty(&self) -> bool {
        self.count == 0
    }

    /// The JSON Pointer at `index`, ready to address a field, or `None` past the end.
    pub fn get(&self, index: usize) -> Option<&'arena CStr> {
        if index >= self.count {
            return None;
        }
        // SAFETY: a successful gist exports `count` plus one offsets into one tape, each path between two
        // of them closed by its null terminator, all of it alive while the arena stays borrowed.
        let bytes = unsafe {
            let begin = *self.offsets.add(index) as usize;
            let end = *self.offsets.add(index + 1) as usize;
            exported_slice(self.tape.add(begin).cast::<u8>(), end.saturating_sub(begin))
        };
        CStr::from_bytes_with_nul(bytes).ok()
    }

    /// Every path in the order the gist found it.
    pub fn iter(&self) -> impl Iterator<Item = &'arena CStr> + 'arena {
        let paths = *self;
        (0..paths.count).filter_map(move |index| paths.get(index))
    }
}

/// One bit per gathered row, least significant bit first.
#[derive(Clone, Copy, Debug)]
pub struct Bitmap<'arena> {
    bytes: &'arena [u8],
    count: usize,
}

impl<'arena> Bitmap<'arena> {
    /// How many rows the bitmap covers.
    pub fn len(&self) -> usize {
        self.count
    }

    /// Whether the bitmap covers no rows.
    pub fn is_empty(&self) -> bool {
        self.count == 0
    }

    /// Whether the bit of `row` is set, which a row past the end never is.
    pub fn get(&self, row: usize) -> bool {
        row < self.count && self.bytes[row / 8] & (1 << (row % 8)) != 0
    }

    /// The bytes of the bitmap, as Arrow reads a validity buffer.
    pub fn as_bytes(&self) -> &'arena [u8] {
        self.bytes
    }
}

/// A gathered column of strings, whose value in a row is absent unless the row is valid.
#[derive(Clone, Copy, Debug)]
pub struct Strings<'arena> {
    validities: Bitmap<'arena>,
    offsets: *const ustore_length_t,
    lengths: *const ustore_length_t,
    joined: *const ustore_byte_t,
    marker: PhantomData<&'arena Arena>,
}

impl<'arena> Strings<'arena> {
    /// How many rows the column holds.
    pub fn len(&self) -> usize {
        self.validities.len()
    }

    /// Whether the column holds no rows.
    pub fn is_empty(&self) -> bool {
        self.validities.is_empty()
    }

    /// The string of `row`, or `None` for a row whose field was missing or would not convert.
    pub fn get(&self, row: usize) -> Option<&'arena [u8]> {
        if !self.validities.get(row) {
            return None;
        }
        // SAFETY: a valid row lies below the row count, for which a successful gather exported one offset
        // and one length into the joined strings, all of it alive while the arena stays borrowed.
        unsafe {
            let offset = *self.offsets.add(row) as usize;
            let length = *self.lengths.add(row) as usize;
            Some(exported_slice(self.joined.add(offset), length))
        }
    }

    /// Every row in order.
    pub fn iter(&self) -> impl Iterator<Item = Option<&'arena [u8]>> + 'arena {
        let strings = *self;
        (0..strings.len()).map(move |row| strings.get(row))
    }
}

/// The columns one gather exported, a row per document and a column per requested field.
///
/// A value is valid when it was found and converted, converted when the conversion changed its meaning,
/// such as text parsed into a number, and colliding when a value of an inconvertible type sits there.
#[derive(Clone, Copy, Debug)]
pub struct Table<'arena> {
    columns: &'arena [Column<'arena>],
    rows: usize,
    validities: *const *const ustore_octet_t,
    conversions: *const *const ustore_octet_t,
    collisions: *const *const ustore_octet_t,
    scalars: *const *const ustore_byte_t,
    offsets: *const *const ustore_length_t,
    lengths: *const *const ustore_length_t,
    joined: *const ustore_byte_t,
}

impl<'arena> Table<'arena> {
    /// How many documents the gather read.
    pub fn rows(&self) -> usize {
        self.rows
    }

    /// How many fields the gather read.
    pub fn columns(&self) -> usize {
        self.columns.len()
    }

    /// Which rows of `column` hold a value, or `None` past the last column.
    pub fn validities(&self, column: usize) -> Option<Bitmap<'arena>> {
        self.bitmap(self.validities, column)
    }

    /// Which rows of `column` changed their meaning on conversion, or `None` past the last column.
    pub fn conversions(&self, column: usize) -> Option<Bitmap<'arena>> {
        self.bitmap(self.conversions, column)
    }

    /// Which rows of `column` hold a value that would not convert, or `None` past the last column.
    pub fn collisions(&self, column: usize) -> Option<Bitmap<'arena>> {
        self.bitmap(self.collisions, column)
    }

    /// The values of a scalar column, one per row, with zero or a partial conversion in invalid rows.
    pub fn scalars<Value: Scalar>(&self, column: usize) -> Result<&'arena [Value]> {
        self.requested::<Value>(column)?;
        if self.rows == 0 {
            return Ok(&[]);
        }
        // SAFETY: a successful gather exports one column address per field and writes every row of a
        // column of the requested type, aligned for any scalar, which stays alive while the arena stays
        // borrowed; a C++ `bool` holds only zero or one.
        Ok(unsafe { exported_slice((*self.scalars.add(column)).cast::<Value>(), self.rows) })
    }

    /// The values of a column gathered as [`Text`] or [`Binary`].
    pub fn strings(&self, column: usize) -> Result<Strings<'arena>> {
        let format = self.requested_format(column)?;
        if format != Text::TYPE && format != Binary::TYPE {
            return Err(fail(Status::InvalidArgument, "the column was not gathered as strings"));
        }
        let validities = self
            .bitmap(self.validities, column)
            .unwrap_or(Bitmap { bytes: &[], count: 0 });
        if self.rows == 0 {
            return Ok(Strings {
                validities,
                offsets: ptr::null(),
                lengths: ptr::null(),
                joined: ptr::null(),
                marker: PhantomData,
            });
        }
        // SAFETY: a successful gather exports one offsets and one lengths address per field.
        let (offsets, lengths) = unsafe { (*self.offsets.add(column), *self.lengths.add(column)) };
        Ok(Strings {
            validities,
            offsets,
            lengths,
            joined: self.joined,
            marker: PhantomData,
        })
    }

    /// The format `column` was requested in, refusing a column the gather never read.
    fn requested_format(&self, column: usize) -> Result<c_int> {
        match self.columns.get(column) {
            Some(request) => Ok(request.format),
            None => Err(fail(Status::InvalidArgument, "the table has no such column")),
        }
    }

    /// Refuses `column` unless it was requested as `Value`.
    fn requested<Value: Scalar>(&self, column: usize) -> Result<()> {
        match self.requested_format(column)? == Value::TYPE {
            true => Ok(()),
            false => Err(fail(Status::InvalidArgument, "the column was gathered as another type")),
        }
    }

    /// One bitmap of `bitmaps` for `column`, or `None` past the last column.
    fn bitmap(&self, bitmaps: *const *const ustore_octet_t, column: usize) -> Option<Bitmap<'arena>> {
        if column >= self.columns.len() {
            return None;
        }
        if self.rows == 0 {
            return Some(Bitmap { bytes: &[], count: 0 });
        }
        // SAFETY: a successful gather exports one bitmap address per field, each covering every row.
        let bytes = unsafe { exported_slice(*bitmaps.add(column), self.rows.div_ceil(8)) };
        Some(Bitmap {
            bytes,
            count: self.rows,
        })
    }
}

// endregion: Outputs

// region: Verbs

impl View<'_> {
    /// Writes whole documents under the keys their entries carry, removing the keys whose entries
    /// carry none.
    ///
    /// The batch writes one format, which is [`Json`] for the text the engine parses and a
    /// [`WritableScalar`] or [`Text`] for a document that is one scalar. A transaction flushes at its
    /// commit, so [`Durability::Flushed`] is refused inside one.
    pub fn write_documents<'collections, Format: FieldFormat>(
        &self,
        arena: &mut Arena,
        collections: impl Into<Collections<'collections>>,
        documents: &[DocumentEntry<'_, Format>],
        modification: Modification,
        durability: Durability,
    ) -> Result<()> {
        let Some(first) = documents.first() else {
            return Ok(());
        };
        let collections = collections.into();
        let (collections, collections_stride) = collections.strided(documents.len())?;
        let stride = size_of::<DocumentEntry<'_, Format>>() as ustore_size_t;
        let mut error = ustore_error_t::default();
        let mut arguments = ffi::ustore_docs_write_t {
            db: self.database(),
            error: &mut error,
            transaction: self.transaction(),
            arena: arena.slot(),
            options: self.options() | durability.to_raw(),
            tasks_count: documents.len() as ustore_size_t,
            type_: Format::TYPE,
            modification: modification as ustore_enum_t,
            collections,
            collections_stride,
            keys: &first.key,
            keys_stride: stride,
            fields: ptr::null(),
            fields_stride: 0,
            presences: ptr::null(),
            offsets: ptr::null(),
            offsets_stride: 0,
            lengths: &first.length,
            lengths_stride: stride,
            values: &first.value,
            values_stride: stride,
            id_field: ptr::null(),
        };
        // SAFETY: the documents and the collections outlive the call, and the members of every entry sit
        // `stride` bytes apart, which is what `#[repr(C)]` on `DocumentEntry` promises.
        unsafe { ffi::ustore_docs_write(&mut arguments) };
        check(error)
    }

    /// Writes whole JSON documents under the integer key each one holds in `id_field`.
    pub fn write_documents_by_id<'collections>(
        &self,
        arena: &mut Arena,
        collections: impl Into<Collections<'collections>>,
        id_field: &CStr,
        documents: &[Document<'_>],
        modification: Modification,
        durability: Durability,
    ) -> Result<()> {
        let Some(first) = documents.first() else {
            return Ok(());
        };
        let collections = collections.into();
        let (collections, collections_stride) = collections.strided(documents.len())?;
        let stride = size_of::<Document<'_>>() as ustore_size_t;
        let mut error = ustore_error_t::default();
        let mut arguments = ffi::ustore_docs_write_t {
            db: self.database(),
            error: &mut error,
            transaction: self.transaction(),
            arena: arena.slot(),
            options: self.options() | durability.to_raw(),
            tasks_count: documents.len() as ustore_size_t,
            type_: Json::TYPE,
            modification: modification as ustore_enum_t,
            collections,
            collections_stride,
            keys: ptr::null(),
            keys_stride: 0,
            fields: ptr::null(),
            fields_stride: 0,
            presences: ptr::null(),
            offsets: ptr::null(),
            offsets_stride: 0,
            lengths: &first.length,
            lengths_stride: stride,
            values: &first.text,
            values_stride: stride,
            id_field: id_field.as_ptr(),
        };
        // SAFETY: the documents and the id field outlive the call, and the fields of every document sit
        // `stride` bytes apart, which is what `#[repr(C)]` on `Document` promises.
        unsafe { ffi::ustore_docs_write(&mut arguments) };
        check(error)
    }

    /// Writes values into fields of existing documents, or removes the fields whose entries carry none.
    pub fn write_fields<'collections, Format: FieldFormat>(
        &self,
        arena: &mut Arena,
        collections: impl Into<Collections<'collections>>,
        fields: &[FieldEntry<'_, Format>],
        modification: Modification,
        durability: Durability,
    ) -> Result<()> {
        let Some(first) = fields.first() else {
            return Ok(());
        };
        let collections = collections.into();
        let (collections, collections_stride) = collections.strided(fields.len())?;
        let stride = size_of::<FieldEntry<'_, Format>>() as ustore_size_t;
        let mut error = ustore_error_t::default();
        let mut arguments = ffi::ustore_docs_write_t {
            db: self.database(),
            error: &mut error,
            transaction: self.transaction(),
            arena: arena.slot(),
            options: self.options() | durability.to_raw(),
            tasks_count: fields.len() as ustore_size_t,
            type_: Format::TYPE,
            modification: modification as ustore_enum_t,
            collections,
            collections_stride,
            keys: &first.key,
            keys_stride: stride,
            fields: &first.field,
            fields_stride: stride,
            presences: ptr::null(),
            offsets: ptr::null(),
            offsets_stride: 0,
            lengths: &first.length,
            lengths_stride: stride,
            values: &first.value,
            values_stride: stride,
            id_field: ptr::null(),
        };
        // SAFETY: the entries, their fields and their values outlive the call, the members of every entry
        // sit `stride` bytes apart as `#[repr(C)]` promises, and a null value is how the ABI spells a removal.
        unsafe { ffi::ustore_docs_write(&mut arguments) };
        check(error)
    }

    /// Reads whole documents as the JSON text they were written in.
    pub fn read_documents<'arena, 'collections>(
        &self,
        arena: &'arena mut Arena,
        collections: impl Into<Collections<'collections>>,
        keys: &[Key],
    ) -> Result<Values<'arena>> {
        let tasks = StridedTasks {
            count: keys.len(),
            keys: keys.as_ptr(),
            keys_stride: size_of::<Key>() as ustore_size_t,
            fields: ptr::null(),
            fields_stride: 0,
        };
        self.read_tasks(arena, collections.into(), tasks, Json::TYPE)
    }

    /// Reads fields of documents in `Format`, reporting a missing document or field as absent.
    pub fn read_fields<'arena, 'collections, Format: ReadFormat>(
        &self,
        arena: &'arena mut Arena,
        collections: impl Into<Collections<'collections>>,
        fields: &[FieldAddress<'_>],
    ) -> Result<Values<'arena>> {
        let Some(first) = fields.first() else {
            return Ok(Values::exported(ptr::null(), ptr::null(), ptr::null(), 0));
        };
        let stride = size_of::<FieldAddress<'_>>() as ustore_size_t;
        let tasks = StridedTasks {
            count: fields.len(),
            keys: &first.key,
            keys_stride: stride,
            fields: &first.field,
            fields_stride: stride,
        };
        self.read_tasks(arena, collections.into(), tasks, Format::TYPE)
    }

    /// Lists the unique paths to the leaves of the documents under `keys`.
    pub fn gist<'arena, 'collections>(
        &self,
        arena: &'arena mut Arena,
        collections: impl Into<Collections<'collections>>,
        keys: &[Key],
    ) -> Result<Paths<'arena>> {
        let collections = collections.into();
        let (collections, collections_stride) = collections.strided(keys.len())?;
        let mut count: ustore_size_t = 0;
        let mut offsets: *mut ustore_length_t = ptr::null_mut();
        let mut tape: *mut ustore_char_t = ptr::null_mut();
        let mut error = ustore_error_t::default();
        let mut arguments = ffi::ustore_docs_gist_t {
            db: self.database(),
            error: &mut error,
            transaction: self.transaction(),
            snapshot: self.snapshot(),
            arena: arena.slot(),
            options: self.options(),
            docs_count: keys.len() as ustore_size_t,
            collections,
            collections_stride,
            keys: keys.as_ptr(),
            keys_stride: size_of::<Key>() as ustore_size_t,
            fields_count: &mut count,
            offsets: &mut offsets,
            fields: &mut tape,
        };
        // SAFETY: the keys, the collections and every output outlive the call.
        unsafe { ffi::ustore_docs_gist(&mut arguments) };
        check(error)?;
        Ok(Paths {
            offsets,
            tape,
            count: if offsets.is_null() { 0 } else { count as usize },
            marker: PhantomData,
        })
    }

    /// Reads `columns` of the documents under `keys` into typed columns, a row per key.
    pub fn gather<'arena, 'collections>(
        &self,
        arena: &'arena mut Arena,
        collections: impl Into<Collections<'collections>>,
        keys: &[Key],
        columns: &'arena [Column<'arena>],
    ) -> Result<Table<'arena>> {
        let collections = collections.into();
        let (collections, collections_stride) = collections.strided(keys.len())?;
        let mut validities: *mut *mut ustore_octet_t = ptr::null_mut();
        let mut conversions: *mut *mut ustore_octet_t = ptr::null_mut();
        let mut collisions: *mut *mut ustore_octet_t = ptr::null_mut();
        let mut scalars: *mut *mut ustore_byte_t = ptr::null_mut();
        let mut offsets: *mut *mut ustore_length_t = ptr::null_mut();
        let mut lengths: *mut *mut ustore_length_t = ptr::null_mut();
        let mut joined: *mut ustore_byte_t = ptr::null_mut();
        let stride = size_of::<Column<'_>>() as ustore_size_t;
        let (fields, types) = match columns.first() {
            Some(first) => (
                &first.field as *const ustore_str_view_t,
                &first.format as *const ustore_enum_t,
            ),
            None => (ptr::null(), ptr::null()),
        };
        let mut error = ustore_error_t::default();
        let mut arguments = ffi::ustore_docs_gather_t {
            db: self.database(),
            error: &mut error,
            transaction: self.transaction(),
            snapshot: self.snapshot(),
            arena: arena.slot(),
            options: self.options(),
            docs_count: keys.len() as ustore_size_t,
            fields_count: columns.len() as ustore_size_t,
            collections,
            collections_stride,
            keys: keys.as_ptr(),
            keys_stride: size_of::<Key>() as ustore_size_t,
            fields,
            fields_stride: stride,
            types,
            types_stride: stride,
            columns_validities: &mut validities,
            columns_conversions: &mut conversions,
            columns_collisions: &mut collisions,
            columns_scalars: &mut scalars,
            columns_offsets: &mut offsets,
            columns_lengths: &mut lengths,
            joined_strings: &mut joined,
        };
        // SAFETY: the keys, the collections, the columns with their fields, and every output outlive the
        // call, and the members of every column sit `stride` bytes apart as `#[repr(C)]` promises.
        unsafe { ffi::ustore_docs_gather(&mut arguments) };
        check(error)?;
        Ok(Table {
            columns,
            rows: if validities.is_null() { 0 } else { keys.len() },
            validities: validities.cast(),
            conversions: conversions.cast(),
            collisions: collisions.cast(),
            scalars: scalars.cast(),
            offsets: offsets.cast(),
            lengths: lengths.cast(),
            joined,
        })
    }

    /// Reads the documents or the fields `tasks` address, exporting them as `format`.
    fn read_tasks<'arena>(
        &self,
        arena: &'arena mut Arena,
        collections: Collections<'_>,
        tasks: StridedTasks,
        format: c_int,
    ) -> Result<Values<'arena>> {
        let (collections, collections_stride) = collections.strided(tasks.count)?;
        let mut offsets: *mut ustore_length_t = ptr::null_mut();
        let mut lengths: *mut ustore_length_t = ptr::null_mut();
        let mut tape: *mut ustore_byte_t = ptr::null_mut();
        let mut error = ustore_error_t::default();
        let mut arguments = ffi::ustore_docs_read_t {
            db: self.database(),
            error: &mut error,
            transaction: self.transaction(),
            snapshot: self.snapshot(),
            arena: arena.slot(),
            options: self.options(),
            type_: format,
            tasks_count: tasks.count as ustore_size_t,
            collections,
            collections_stride,
            keys: tasks.keys,
            keys_stride: tasks.keys_stride,
            fields: tasks.fields,
            fields_stride: tasks.fields_stride,
            presences: ptr::null_mut(),
            offsets: &mut offsets,
            lengths: &mut lengths,
            values: &mut tape,
        };
        // SAFETY: whatever the tasks address and every output outlive the call.
        unsafe { ffi::ustore_docs_read(&mut arguments) };
        check(error)?;
        Ok(Values::exported(offsets, lengths, tape, tasks.count))
    }
}

/// The keys and fields of a read, as borrowed strided runs that stay alive for the call.
struct StridedTasks {
    count: usize,
    keys: *const ustore_key_t,
    keys_stride: ustore_size_t,
    fields: *const ustore_str_view_t,
    fields_stride: ustore_size_t,
}

// endregion: Verbs

// region: Tests

#[cfg(test)]
mod tests {
    use super::*;
    use crate::blobs::Entry;
    use crate::db::{Collection, Collections, Configuration, Conflicts, Database};

    /// A database holding Ada under key one and Grace under key two.
    fn populated() -> (Database, Arena) {
        let database = Database::open(Configuration::in_memory()).unwrap();
        let mut arena = Arena::new();
        let documents = [
            DocumentEntry::<Json>::new(1, r#"{"name":"Ada","born":1815,"languages":["English","French"]}"#).unwrap(),
            DocumentEntry::<Json>::new(2, r#"{"name":"Grace","born":"1906","navy":{"rank":"Rear Admiral"}}"#).unwrap(),
        ];
        database
            .head()
            .write_documents(
                &mut arena,
                Collection::main(),
                &documents,
                Modification::Upsert,
                Durability::Buffered,
            )
            .unwrap();
        (database, arena)
    }

    /// The text of the field at `index`, or `None` when it is absent.
    fn text<'arena>(values: &Values<'arena>, index: usize) -> Option<&'arena str> {
        values.get(index).map(|bytes| std::str::from_utf8(bytes).unwrap())
    }

    #[test]
    fn whole_documents_read_back_as_written() {
        let (database, mut arena) = populated();
        let documents = database
            .head()
            .read_documents(&mut arena, Collection::main(), &[2, 1, 3])
            .unwrap();
        assert_eq!(documents.len(), 3);
        assert_eq!(
            text(&documents, 0),
            Some(r#"{"name":"Grace","born":"1906","navy":{"rank":"Rear Admiral"}}"#)
        );
        assert!(text(&documents, 1).unwrap().starts_with(r#"{"name":"Ada""#));
        assert_eq!(documents.get(2), None, "a key nobody wrote holds no document");
    }

    #[test]
    fn documents_refuse_text_that_is_not_json() {
        let database = Database::open(Configuration::in_memory()).unwrap();
        let mut arena = Arena::new();
        let documents = [DocumentEntry::<Json>::new(1, "{\"name\":").unwrap()];
        let failure = database
            .head()
            .write_documents(
                &mut arena,
                Collection::main(),
                &documents,
                Modification::Upsert,
                Durability::Buffered,
            )
            .unwrap_err();
        assert_eq!(failure.status, Status::Malformed);
    }

    #[test]
    fn documents_insert_once_and_update_in_place() {
        let (database, mut arena) = populated();
        let again = [DocumentEntry::<Json>::new(1, r#"{"name":"Someone else"}"#).unwrap()];
        let failure = database
            .head()
            .write_documents(
                &mut arena,
                Collection::main(),
                &again,
                Modification::Insert,
                Durability::Buffered,
            )
            .unwrap_err();
        assert_eq!(failure.status, Status::AlreadyExists);

        let fresh = [DocumentEntry::<Json>::new(3, r#"{"name":"Katherine"}"#).unwrap()];
        database
            .head()
            .write_documents(
                &mut arena,
                Collection::main(),
                &fresh,
                Modification::Insert,
                Durability::Buffered,
            )
            .unwrap();

        let merge = [DocumentEntry::<Json>::new(3, r#"{"born":1918}"#).unwrap()];
        database
            .head()
            .write_documents(
                &mut arena,
                Collection::main(),
                &merge,
                Modification::Merge,
                Durability::Buffered,
            )
            .unwrap();
        let patch = [DocumentEntry::<Json>::new(3, r#"[{"op":"add","path":"/surname","value":"Johnson"}]"#).unwrap()];
        database
            .head()
            .write_documents(
                &mut arena,
                Collection::main(),
                &patch,
                Modification::Patch,
                Durability::Buffered,
            )
            .unwrap();

        let fields = [
            FieldAddress::new(3, c"name"),
            FieldAddress::new(3, c"born"),
            FieldAddress::new(3, c"surname"),
        ];
        let read = database
            .head()
            .read_fields::<Text>(&mut arena, Collection::main(), &fields)
            .unwrap();
        assert_eq!(text(&read, 0), Some("Katherine"), "a merge keeps what it does not name");
        assert_eq!(text(&read, 1), Some("1918"));
        assert_eq!(text(&read, 2), Some("Johnson"));
    }

    #[test]
    fn a_patch_that_does_not_apply_is_refused() {
        let (database, mut arena) = populated();
        let patch = [DocumentEntry::<Json>::new(1, r#"[{"op":"remove","path":"/nowhere"}]"#).unwrap()];
        let failure = database
            .head()
            .write_documents(
                &mut arena,
                Collection::main(),
                &patch,
                Modification::Patch,
                Durability::Buffered,
            )
            .unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument, "{failure:?}");

        // An update or a patch replaces what exists, so a missing document refuses both.
        for modification in [Modification::Update, Modification::Patch] {
            let modifier = [DocumentEntry::<Json>::new(42, r#"[]"#).unwrap()];
            let failure = database
                .head()
                .write_documents(
                    &mut arena,
                    Collection::main(),
                    &modifier,
                    modification,
                    Durability::Buffered,
                )
                .unwrap_err();
            assert_eq!(failure.status, Status::NotFound, "{modification:?}");
        }
    }

    #[test]
    fn documents_carry_their_keys_under_an_id_field() {
        let database = Database::open(Configuration::in_memory()).unwrap();
        let mut arena = Arena::new();
        let documents = [
            Document::new(r#"{"_id":7,"name":"Ada"}"#).unwrap(),
            Document::new(r#"{"_id":9,"name":"Grace"}"#).unwrap(),
        ];
        database
            .head()
            .write_documents_by_id(
                &mut arena,
                Collection::main(),
                c"_id",
                &documents,
                Modification::Upsert,
                Durability::Buffered,
            )
            .unwrap();
        let names = database
            .head()
            .read_fields::<Text>(
                &mut arena,
                Collection::main(),
                &[FieldAddress::new(9, c"name"), FieldAddress::new(7, c"name")],
            )
            .unwrap();
        assert_eq!(text(&names, 0), Some("Grace"));
        assert_eq!(text(&names, 1), Some("Ada"));

        let unkeyed = [Document::new(r#"{"_id":"seven"}"#).unwrap()];
        let failure = database
            .head()
            .write_documents_by_id(
                &mut arena,
                Collection::main(),
                c"_id",
                &unkeyed,
                Modification::Upsert,
                Durability::Buffered,
            )
            .unwrap_err();
        assert_eq!(failure.status, Status::Malformed);
    }

    #[test]
    fn fields_write_every_format_and_read_back_as_text_or_json() {
        let (database, mut arena) = populated();
        let head = database.head();
        let main = Collection::main();
        let upsert = Modification::Upsert;
        let buffered = Durability::Buffered;

        let died = 1852i64;
        head.write_fields(
            &mut arena,
            main,
            &[FieldEntry::<i64>::new(1, c"died", &died).unwrap()],
            upsert,
            buffered,
        )
        .unwrap();
        let height = 1.65f64;
        head.write_fields(
            &mut arena,
            main,
            &[FieldEntry::<f64>::new(1, c"height", &height).unwrap()],
            upsert,
            buffered,
        )
        .unwrap();
        let countess = true;
        head.write_fields(
            &mut arena,
            main,
            &[FieldEntry::<bool>::new(1, c"countess", &countess).unwrap()],
            upsert,
            buffered,
        )
        .unwrap();
        let title = [FieldEntry::<Text>::new(1, c"title", "Countess of Lovelace").unwrap()];
        head.write_fields(&mut arena, main, &title, upsert, buffered).unwrap();
        let nested = [
            FieldEntry::<Json>::new(1, c"/family", r#"{"father":"Byron"}"#).unwrap(),
            FieldEntry::<Json>::new(2, c"/navy/rank", r#""Commodore""#).unwrap(),
        ];
        head.write_fields(&mut arena, main, &nested, upsert, buffered).unwrap();

        let fields = [
            FieldAddress::new(1, c"died"),
            FieldAddress::new(1, c"height"),
            FieldAddress::new(1, c"countess"),
            FieldAddress::new(1, c"title"),
            FieldAddress::new(1, c"/family/father"),
            FieldAddress::new(2, c"/navy/rank"),
            FieldAddress::new(1, c"/languages/1"),
        ];
        let texts = head.read_fields::<Text>(&mut arena, main, &fields).unwrap();
        assert_eq!(text(&texts, 0), Some("1852"));
        assert_eq!(text(&texts, 1), Some("1.65"));
        assert_eq!(text(&texts, 2), Some("true"));
        assert_eq!(text(&texts, 3), Some("Countess of Lovelace"));
        assert_eq!(text(&texts, 4), Some("Byron"));
        assert_eq!(text(&texts, 5), Some("Commodore"));
        assert_eq!(text(&texts, 6), Some("French"));

        let json = head
            .read_fields::<Json>(
                &mut arena,
                main,
                &[FieldAddress::new(1, c"title"), FieldAddress::new(1, c"family")],
            )
            .unwrap();
        assert_eq!(text(&json, 0), Some(r#""Countess of Lovelace""#));
        assert_eq!(text(&json, 1), Some(r#"{"father":"Byron"}"#));
    }

    #[test]
    fn fields_are_removed_by_entries_without_values() {
        let (database, mut arena) = populated();
        let removal = [FieldEntry::<Json>::removal(1, c"born")];
        database
            .head()
            .write_fields(
                &mut arena,
                Collection::main(),
                &removal,
                Modification::Upsert,
                Durability::Buffered,
            )
            .unwrap();
        let born = database
            .head()
            .read_fields::<Text>(&mut arena, Collection::main(), &[FieldAddress::new(1, c"born")])
            .unwrap();
        assert_eq!(born.get(0), None);

        // A field already gone does not resolve a second time.
        let failure = database
            .head()
            .write_fields(
                &mut arena,
                Collection::main(),
                &removal,
                Modification::Upsert,
                Durability::Buffered,
            )
            .unwrap_err();
        assert_eq!(failure.status, Status::NotFound);
    }

    #[test]
    fn fields_refuse_a_document_or_a_path_that_is_not_there() {
        let (database, mut arena) = populated();
        let orphan = [FieldEntry::<Text>::new(42, c"name", "Nobody").unwrap()];
        let failure = database
            .head()
            .write_fields(
                &mut arena,
                Collection::main(),
                &orphan,
                Modification::Upsert,
                Durability::Buffered,
            )
            .unwrap_err();
        assert_eq!(failure.status, Status::NotFound);

        let deep = [FieldEntry::<Text>::new(1, c"/missing/deeper", "value").unwrap()];
        let failure = database
            .head()
            .write_fields(
                &mut arena,
                Collection::main(),
                &deep,
                Modification::Update,
                Durability::Buffered,
            )
            .unwrap_err();
        assert_eq!(failure.status, Status::NotFound);
    }

    #[test]
    fn reads_report_missing_documents_and_fields_as_absent() {
        let (database, mut arena) = populated();
        let fields = [
            FieldAddress::new(1, c"name"),
            FieldAddress::new(1, c"nowhere"),
            FieldAddress::new(42, c"name"),
        ];
        let read = database
            .head()
            .read_fields::<Json>(&mut arena, Collection::main(), &fields)
            .unwrap();
        assert_eq!(text(&read, 0), Some(r#""Ada""#));
        assert_eq!(read.get(1), None);
        assert_eq!(read.get(2), None);
    }

    #[test]
    fn fields_of_a_value_that_is_not_json_are_refused() {
        let database = Database::open(Configuration::in_memory()).unwrap();
        let mut arena = Arena::new();
        let blob = [Entry::new(1, b"not a document").unwrap()];
        database
            .head()
            .write(&mut arena, Collection::main(), &blob, Durability::Buffered)
            .unwrap();
        let failure = database
            .head()
            .read_fields::<Text>(&mut arena, Collection::main(), &[FieldAddress::new(1, c"name")])
            .unwrap_err();
        assert_eq!(failure.status, Status::Malformed);
    }

    #[test]
    fn gists_list_every_leaf_once() {
        let (database, mut arena) = populated();
        let paths = database
            .head()
            .gist(&mut arena, Collection::main(), &[1, 2, 1, 42])
            .unwrap();
        let mut listed: Vec<&str> = paths.iter().map(|path| path.to_str().unwrap()).collect();
        listed.sort_unstable();
        assert_eq!(
            listed,
            vec!["/born", "/languages/0", "/languages/1", "/name", "/navy/rank"]
        );
        assert_eq!(paths.len(), 5);
        assert!(paths.get(5).is_none());
    }

    #[test]
    fn gists_refuse_a_path_past_the_length_they_export() {
        let database = Database::open(Configuration::in_memory()).unwrap();
        let mut arena = Arena::new();
        let document = format!("{{\"{}\":1}}", "k".repeat(600));
        let documents = [DocumentEntry::<Json>::new(1, &document).unwrap()];
        database
            .head()
            .write_documents(
                &mut arena,
                Collection::main(),
                &documents,
                Modification::Upsert,
                Durability::Buffered,
            )
            .unwrap();
        let failure = database.head().gist(&mut arena, Collection::main(), &[1]).unwrap_err();
        assert_eq!(failure.status, Status::CapacityExhausted);
    }

    #[test]
    fn gathers_convert_fields_into_typed_columns() {
        let (database, mut arena) = populated();
        let columns = [
            Column::new::<u64>(c"born"),
            Column::new::<f32>(c"born"),
            Column::new::<Text>(c"name"),
            Column::new::<Binary>(c"/navy/rank"),
            Column::new::<bool>(c"navy"),
            Column::new::<u8>(c"/languages/0"),
        ];
        let table = database
            .head()
            .gather(&mut arena, Collection::main(), &[1, 2, 42], &columns)
            .unwrap();
        assert_eq!((table.rows(), table.columns()), (3, 6));

        let born = table.scalars::<u64>(0).unwrap();
        assert_eq!(&born[..2], &[1815, 1906]);
        let validities = table.validities(0).unwrap();
        assert_eq!(
            (validities.get(0), validities.get(1), validities.get(2)),
            (true, true, false)
        );
        let conversions = table.conversions(0).unwrap();
        assert!(!conversions.get(0), "a positive integer is unsigned as it stands");
        assert!(conversions.get(1), "a string parsed into an integer is a conversion");
        assert_eq!(validities.len(), 3);
        assert_eq!(validities.as_bytes().len(), 1);

        assert_eq!(&table.scalars::<f32>(1).unwrap()[..2], &[1815.0, 1906.0]);

        let names = table.strings(2).unwrap();
        assert_eq!(names.len(), 3);
        assert_eq!(names.get(0), Some(b"Ada".as_slice()));
        assert_eq!(names.get(1), Some(b"Grace".as_slice()));
        assert_eq!(names.get(2), None);
        assert_eq!(names.iter().filter(Option::is_some).count(), 2);

        let ranks = table.strings(3).unwrap();
        assert_eq!(ranks.get(0), None);
        assert_eq!(ranks.get(1), Some(b"Rear Admiral".as_slice()));

        // An object cannot become a boolean, nor a word a byte.
        let navy = table.collisions(4).unwrap();
        assert!(!navy.get(0) && navy.get(1));
        assert!(!table.validities(4).unwrap().get(1));
        assert!(table.collisions(5).unwrap().get(0));
        assert_eq!(table.scalars::<bool>(4).unwrap().len(), 3);
    }

    #[test]
    fn tables_refuse_a_column_read_as_something_else() {
        let (database, mut arena) = populated();
        let columns = [Column::new::<i64>(c"born"), Column::new::<Text>(c"name")];
        let table = database
            .head()
            .gather(&mut arena, Collection::main(), &[1], &columns)
            .unwrap();
        assert_eq!(table.scalars::<u64>(0).unwrap_err().status, Status::InvalidArgument);
        assert_eq!(table.scalars::<i64>(1).unwrap_err().status, Status::InvalidArgument);
        assert_eq!(table.strings(0).unwrap_err().status, Status::InvalidArgument);
        assert_eq!(table.scalars::<i64>(2).unwrap_err().status, Status::InvalidArgument);
        assert!(table.validities(2).is_none());
    }

    #[test]
    fn gist_paths_address_the_fields_a_gather_reads() {
        let (database, mut paths_arena) = populated();
        let mut table_arena = Arena::new();
        let paths = database
            .head()
            .gist(&mut paths_arena, Collection::main(), &[1])
            .unwrap();
        let columns: Vec<Column<'_>> = paths.iter().map(Column::new::<Text>).collect();
        let table = database
            .head()
            .gather(&mut table_arena, Collection::main(), &[1], &columns)
            .unwrap();
        assert_eq!(table.columns(), 4);
        for column in 0..table.columns() {
            assert!(table.validities(column).unwrap().get(0));
        }
    }

    #[test]
    fn whole_documents_are_written_as_scalars_and_strings() {
        let database = Database::open(Configuration::in_memory()).unwrap();
        let mut arena = Arena::new();
        let head = database.head();
        let main = Collection::main();
        let upsert = Modification::Upsert;
        let buffered = Durability::Buffered;

        let born = 1815i64;
        head.write_documents(
            &mut arena,
            main,
            &[DocumentEntry::<i64>::new(1, &born).unwrap()],
            upsert,
            buffered,
        )
        .unwrap();
        let height = 1.65f64;
        head.write_documents(
            &mut arena,
            main,
            &[DocumentEntry::<f64>::new(2, &height).unwrap()],
            upsert,
            buffered,
        )
        .unwrap();
        let countess = true;
        head.write_documents(
            &mut arena,
            main,
            &[DocumentEntry::<bool>::new(3, &countess).unwrap()],
            upsert,
            buffered,
        )
        .unwrap();
        head.write_documents(
            &mut arena,
            main,
            &[DocumentEntry::<Text>::new(4, "Countess of Lovelace").unwrap()],
            upsert,
            buffered,
        )
        .unwrap();

        // A document that is one scalar reads back as the JSON that scalar prints as.
        let documents = head.read_documents(&mut arena, main, &[1, 2, 3, 4]).unwrap();
        assert_eq!(text(&documents, 0), Some("1815"));
        assert_eq!(text(&documents, 1), Some("1.65"));
        assert_eq!(text(&documents, 2), Some("true"));
        assert_eq!(text(&documents, 3), Some(r#""Countess of Lovelace""#));
    }

    #[test]
    fn one_batch_of_documents_mixes_writes_and_removals() {
        let (database, mut arena) = populated();
        let batch = [
            DocumentEntry::<Json>::new(1, r#"{"name":"Someone else"}"#).unwrap(),
            DocumentEntry::<Json>::removal(2),
        ];
        database
            .head()
            .write_documents(
                &mut arena,
                Collection::main(),
                &batch,
                Modification::Upsert,
                Durability::Buffered,
            )
            .unwrap();
        let documents = database
            .head()
            .read_documents(&mut arena, Collection::main(), &[1, 2])
            .unwrap();
        assert_eq!(text(&documents, 0), Some(r#"{"name":"Someone else"}"#));
        assert_eq!(documents.get(1), None, "an entry without a document removes its key");
    }

    #[test]
    fn one_batch_of_documents_spans_several_collections() {
        let database = Database::open(Configuration::in_memory()).unwrap();
        let mut arena = Arena::new();
        let people = database.create_collection(&mut arena, "people").unwrap();
        let places = database.create_collection(&mut arena, "places").unwrap();
        let spanned = [people, places];

        let documents = [
            DocumentEntry::<Json>::new(1, r#"{"name":"Ada"}"#).unwrap(),
            DocumentEntry::<Json>::new(1, r#"{"name":"Kent"}"#).unwrap(),
        ];
        database
            .head()
            .write_documents(
                &mut arena,
                spanned.as_slice(),
                &documents,
                Modification::Upsert,
                Durability::Buffered,
            )
            .unwrap();

        let fields = [FieldAddress::new(1, c"name"), FieldAddress::new(1, c"name")];
        let names = database
            .head()
            .read_fields::<Text>(&mut arena, Collections::Each(&spanned), &fields)
            .unwrap();
        assert_eq!((text(&names, 0), text(&names, 1)), (Some("Ada"), Some("Kent")));

        let columns = [Column::new::<Text>(c"name")];
        let table = database
            .head()
            .gather(&mut arena, Collections::Each(&spanned), &[1, 1], &columns)
            .unwrap();
        assert_eq!(table.strings(0).unwrap().get(1), Some(b"Kent".as_slice()));

        // A run of collections that is not one per task is refused before the call.
        let failure = database
            .head()
            .gather(&mut arena, Collections::Each(&spanned), &[1], &columns)
            .unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);
    }

    #[test]
    fn gathers_fill_halves_and_uuids() {
        let database = Database::open(Configuration::in_memory()).unwrap();
        let mut arena = Arena::new();
        let documents = [
            DocumentEntry::<Json>::new(
                1,
                r#"{"weight":1.5,"id":"123e4567-e89b-12d3-a456-426614174000","huge":70000.0}"#,
            )
            .unwrap(),
            DocumentEntry::<Json>::new(2, r#"{"weight":0.0,"id":"not a uuid","huge":1.0}"#).unwrap(),
        ];
        database
            .head()
            .write_documents(
                &mut arena,
                Collection::main(),
                &documents,
                Modification::Upsert,
                Durability::Buffered,
            )
            .unwrap();

        let columns = [
            Column::new::<Half>(c"weight"),
            Column::new::<Uuid>(c"id"),
            Column::new::<Half>(c"huge"),
        ];
        let table = database
            .head()
            .gather(&mut arena, Collection::main(), &[1, 2], &columns)
            .unwrap();

        // A half is two bytes the caller reinterprets; 1.5 is sign zero, exponent fifteen, mantissa one half.
        let weights = table.scalars::<Half>(0).unwrap();
        assert_eq!(weights, &[Half(0x3e00), Half(0)]);
        assert!(table.validities(0).unwrap().get(0) && table.validities(0).unwrap().get(1));

        let identifiers = table.scalars::<Uuid>(1).unwrap();
        assert_eq!(
            identifiers[0],
            Uuid([0x12, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x12, 0xd3, 0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x00])
        );
        assert_eq!(
            identifiers[1],
            Uuid::default(),
            "text that is no UUID leaves its cell zero"
        );
        assert!(table.collisions(1).unwrap().get(1));

        // A magnitude past the range of a half collides rather than rounding to infinity.
        assert!(table.collisions(2).unwrap().get(0));
        assert_eq!(table.scalars::<Half>(2).unwrap()[0], Half(0));
        assert_eq!(
            table.scalars::<f32>(0).unwrap_err().status,
            Status::InvalidArgument,
            "a half column is not a float column"
        );
    }

    #[test]
    fn empty_batches_reach_no_engine() {
        let (database, mut arena) = populated();
        let head = database.head();
        head.write_documents::<Json>(
            &mut arena,
            Collection::main(),
            &[],
            Modification::Upsert,
            Durability::Buffered,
        )
        .unwrap();
        head.write_documents_by_id(
            &mut arena,
            Collection::main(),
            c"_id",
            &[],
            Modification::Upsert,
            Durability::Buffered,
        )
        .unwrap();
        head.write_fields::<Json>(
            &mut arena,
            Collection::main(),
            &[],
            Modification::Upsert,
            Durability::Buffered,
        )
        .unwrap();
        assert!(head
            .read_documents(&mut arena, Collection::main(), &[])
            .unwrap()
            .is_empty());
        assert!(head
            .read_fields::<Text>(&mut arena, Collection::main(), &[])
            .unwrap()
            .is_empty());
        assert!(head.gist(&mut arena, Collection::main(), &[]).unwrap().is_empty());

        let columns = [Column::new::<i64>(c"born"), Column::new::<Text>(c"name")];
        let table = head.gather(&mut arena, Collection::main(), &[], &columns).unwrap();
        assert_eq!(table.rows(), 0);
        assert!(table.scalars::<i64>(0).unwrap().is_empty());
        assert!(table.strings(1).unwrap().is_empty());
        assert!(table.validities(0).unwrap().is_empty());
        assert_eq!(
            head.gather(&mut arena, Collection::main(), &[1], &[])
                .unwrap()
                .columns(),
            0
        );
    }

    #[test]
    fn transactions_hold_their_field_writes_until_commit() {
        let (database, mut arena) = populated();
        let mut transaction = database.transaction().unwrap();
        let rank = [FieldEntry::<Text>::new(2, c"/navy/rank", "Commodore").unwrap()];
        transaction
            .view(Conflicts::Watched)
            .write_fields(
                &mut arena,
                Collection::main(),
                &rank,
                Modification::Update,
                Durability::Buffered,
            )
            .unwrap();
        let flushed = transaction
            .view(Conflicts::Watched)
            .write_fields(
                &mut arena,
                Collection::main(),
                &rank,
                Modification::Update,
                Durability::Flushed,
            )
            .unwrap_err();
        assert_eq!(flushed.status, Status::InvalidArgument);

        let address = [FieldAddress::new(2, c"/navy/rank")];
        let inside = transaction
            .view(Conflicts::Watched)
            .read_fields::<Text>(&mut arena, Collection::main(), &address)
            .unwrap();
        assert_eq!(text(&inside, 0), Some("Commodore"));
        let outside = database
            .head()
            .read_fields::<Text>(&mut arena, Collection::main(), &address)
            .unwrap();
        assert_eq!(text(&outside, 0), Some("Rear Admiral"));

        transaction.commit(Durability::Buffered).unwrap();
        let committed = database
            .head()
            .read_fields::<Text>(&mut arena, Collection::main(), &address)
            .unwrap();
        assert_eq!(text(&committed, 0), Some("Commodore"));
    }

    #[test]
    fn scalars_cross_as_their_native_bytes() {
        assert_eq!(<i64 as FieldFormat>::bytes(&-2), &(-2i64).to_ne_bytes());
        assert_eq!(<f32 as FieldFormat>::bytes(&1.5), &1.5f32.to_ne_bytes());
        assert_eq!(<bool as FieldFormat>::bytes(&true), &[1]);
        assert_eq!(<Json as FieldFormat>::bytes("[]"), b"[]");
        assert_eq!(size_of::<FieldEntry<'_, Json>>(), 32);
        assert_eq!(size_of::<Column<'_>>(), 16);
    }
}

// endregion: Tests
