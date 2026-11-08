//! JSON documents - the Rust twin of `c/ustore/docs.h`.
//!
//! The ABI carries documents as JSON text both ways and parses them inside the engine, so nothing here
//! parses JSON. A field is a top-level key or an RFC 6901 pointer, which crosses as the bytes of a `&str`
//! with no terminator, so a `"/user/name"` literal crosses as it stands, and an empty field addresses the
//! whole document.
//!
//! ```no_run
//! use ustore::{Arena, Column, RAMOptions, Database, DocumentEntry, Durability, FieldAddress, FieldEntry};
//! use ustore::{Json, Key, Modality, Modification, Text};
//!
//! let database = Database::init_ram(&RAMOptions::default())?;
//! let mut arena = Arena::new()?;
//! let people = database.collection_create("people", Modality::Docs)?;
//! let ada = Key::from(1u64);
//!
//! let documents = [DocumentEntry::<Json>::new(ada, r#"{"name":"Ada","born":1815}"#)];
//! database.head().docs_write(people, &documents, Modification::Upsert, Durability::Buffered)?;
//! let died = [FieldEntry::<i64>::new(ada, "died", &1852)];
//! database.head().docs_write(people, &died, Modification::Upsert, Durability::Buffered)?;
//!
//! let names = database.head().docs_read(people, &[FieldAddress::new(ada, "name")], Text, &mut arena)?;
//! assert_eq!(names.get(0), Some(b"Ada".as_slice()));
//!
//! let columns = [Column::new::<i64>("born"), Column::new::<Text>("name")];
//! let gathered = database.head().docs_gather(people, &[ada], &columns, &mut arena)?;
//! assert_eq!(gathered.scalars::<i64>(0)?, &[1815]);
//! # Ok::<(), ustore::Error>(())
//! ```
//!
//! One batch writes one format, so a batch of fields mixing two does not compile:
//!
//! ```compile_fail
//! use ustore::{FieldEntry, Json, Key};
//!
//! let ada = Key::from(1u64);
//! let fields = [FieldEntry::<i64>::new(ada, "age", &36), FieldEntry::<Json>::new(ada, "name", "\"Ada\"")];
//! ```
//!
//! A batch of whole documents carries one format too, so the same mixture is refused there:
//!
//! ```compile_fail
//! use ustore::{DocumentEntry, Json, Key};
//!
//! let [ada, born] = [1u64, 2].map(Key::from);
//! let documents = [DocumentEntry::<Json>::new(ada, "{}"), DocumentEntry::<i64>::new(born, &1815)];
//! ```
//!
//! Neither [`Half`] nor [`UUID`] is a format a write carries, so a document written as one does not
//! compile either, however it reads back in a gathered column:
//!
//! ```compile_fail
//! use ustore::{DocumentEntry, Half, Key};
//!
//! let documents = [DocumentEntry::<Half>::new(Key::from(1u64), &Half(0x3e00))];
//! ```
//!
//! Gathered columns borrow the arena like any other result, so the call that would overwrite them does not compile
//! while they live:
//!
//! ```compile_fail,E0499
//! use ustore::{Arena, Column, RAMOptions, Database, Key, Modality, Text};
//!
//! let database = Database::init_ram(&RAMOptions::default())?;
//! let mut arena = Arena::new()?;
//! let people = database.collection_create("people", Modality::Docs)?;
//! let columns = [Column::new::<Text>("name")];
//! let ada = Key::from(1u64);
//! let gathered = database.head().docs_gather(people, &[ada], &columns, &mut arena)?;
//! let paths = database.head().docs_gist(people, [ada], &mut arena)?;
//! assert_eq!(gathered.rows(), paths.len());
//! # Ok::<(), ustore::Error>(())
//! ```
//!
//! File: rust/docs.rs
//! Author: Ash Vardanian
//! Date: September 18, 2026

use std::borrow::Cow;
use std::ffi::{c_char, c_int, c_void};
use std::fmt;
use std::marker::PhantomData;
use std::ops::Range;
use std::ptr;
use std::str::FromStr;

use stringtape::CharsTape;

use crate::arrow::{ffi as arrow_ffi, ArrowDeviceArray, ArrowExport, ArrowSchema};
use crate::blobs::{Bounds, Found, Measurement, Page, Values};
use crate::db::ffi::{
    ustore_arena_t, ustore_byte_t, ustore_bytes_view_t, ustore_collection_id_t, ustore_collections_view_t,
    ustore_completion_t, ustore_database_t, ustore_durability_t, ustore_enum_t, ustore_executor_t, ustore_key_t,
    ustore_keys_view_t, ustore_length_t, ustore_octet_t, ustore_outcome_t, ustore_predicate_t,
    ustore_progress_callback_t, ustore_scan_direction_t, ustore_scan_mode_t, ustore_size_t, ustore_strided_view_t,
    ustore_strings_view_t, ustore_transaction_t, ustore_view_t, ustore_watch_t,
};
use crate::db::sealed::{Accepts, Calling, TapeOffset, Writable};
use crate::db::{
    check, exported_slice, fail, host_arena, read_spelling, spelling, Address, Arena, CollectionID, Collections,
    Deferring, Durability, Error, Key, KeyRange, Matched, Matching, On, Progress, ProgressCallback, Result, Rows,
    ScanMode, Status, Strings, UUID,
};

// region: Raw Declarations

/// One-to-one mirror of `c/ustore/docs.h`.
pub(crate) mod ffi {
    #![allow(non_camel_case_types)]

    use super::*;

    #[repr(C)]
    pub struct ustore_docs_write_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub transaction: ustore_transaction_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub keys: ustore_keys_view_t,
        pub id_field: ustore_bytes_view_t,
        pub fields: ustore_strings_view_t,
        pub values: ustore_strings_view_t,
        pub type_: ustore_enum_t,
        pub modification: ustore_enum_t,
        pub durability: ustore_durability_t,
        pub watch: ustore_watch_t,
    }

    #[repr(C)]
    pub struct ustore_docs_remove_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub transaction: ustore_transaction_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub keys: ustore_keys_view_t,
        pub end_keys: ustore_keys_view_t,
        pub durability: ustore_durability_t,
        pub watch: ustore_watch_t,
    }

    #[repr(C)]
    pub struct ustore_docs_read_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub view: ustore_view_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub keys: ustore_keys_view_t,
        pub fields: ustore_strings_view_t,
        pub presences: *mut *const ustore_octet_t,
        pub lengths: *mut *const ustore_length_t,
        pub values: *mut ustore_strings_view_t,
        pub arena: ustore_arena_t,
        pub outputs_type: ustore_enum_t,
    }

    #[repr(C)]
    pub struct ustore_docs_gist_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub view: ustore_view_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub keys: ustore_keys_view_t,
        pub fields: *mut ustore_strings_view_t,
        pub written: *mut ustore_size_t,
        pub arena: ustore_arena_t,
    }

    #[repr(C)]
    #[derive(Clone, Copy, Debug)]
    pub struct ustore_docs_column_view_t {
        pub validities: *const ustore_octet_t,
        pub conversions: *const ustore_octet_t,
        pub collisions: *const ustore_octet_t,
        pub scalars: *const ustore_byte_t,
        pub strings: ustore_strings_view_t,
    }

    #[repr(C)]
    pub struct ustore_docs_gather_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub view: ustore_view_t,
        pub count: ustore_size_t,
        pub collections: ustore_collections_view_t,
        pub keys: ustore_keys_view_t,
        pub fields_count: ustore_size_t,
        pub fields: ustore_strings_view_t,
        pub columns: *mut *const ustore_docs_column_view_t,
        pub arena: ustore_arena_t,
        pub outputs_types: *const ustore_enum_t,
    }

    #[repr(C)]
    pub struct ustore_docs_cursor_handle_t {
        _private: [u8; 0],
    }

    pub type ustore_docs_cursor_t = *mut ustore_docs_cursor_handle_t;

    #[repr(C)]
    pub struct ustore_docs_cursor_init_t {
        pub cursor: *mut ustore_docs_cursor_t,
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
    pub struct ustore_docs_cursor_next_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub cursor: ustore_docs_cursor_t,
        pub keys: *mut *const ustore_key_t,
        pub values: *mut ustore_strings_view_t,
        pub written: *mut ustore_size_t,
        pub arena: ustore_arena_t,
        pub wanted: ustore_size_t,
        pub progress: ustore_progress_callback_t,
        pub progress_state: *mut c_void,
    }

    #[repr(C)]
    pub struct ustore_docs_scan_t {
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
    pub struct ustore_docs_sample_t {
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
    pub struct ustore_docs_measure_t {
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

    extern "C" {
        pub fn ustore_docs_cursor_init(arguments: *mut ustore_docs_cursor_init_t);
        pub fn ustore_docs_cursor_next(arguments: *mut ustore_docs_cursor_next_t);
        pub fn ustore_docs_cursor_free(cursor: ustore_docs_cursor_t);
        pub fn ustore_docs_write(arguments: *mut ustore_docs_write_t);
        pub fn ustore_docs_remove(arguments: *mut ustore_docs_remove_t);
        pub fn ustore_docs_read(arguments: *mut ustore_docs_read_t);
        pub fn ustore_docs_gist(arguments: *mut ustore_docs_gist_t);
        pub fn ustore_docs_gather(arguments: *mut ustore_docs_gather_t);
        pub fn ustore_docs_scan(arguments: *mut ustore_docs_scan_t);
        pub fn ustore_docs_sample(arguments: *mut ustore_docs_sample_t);
        pub fn ustore_docs_measure(arguments: *mut ustore_docs_measure_t);
        pub fn ustore_docs_modification_name(modification: ustore_enum_t) -> *const c_char;
        pub fn ustore_docs_field_type_name(field_type: ustore_enum_t) -> *const c_char;
        pub fn ustore_docs_field_type_from_name(name: ustore_bytes_view_t) -> ustore_enum_t;
        pub fn ustore_docs_modification_from_name(name: ustore_bytes_view_t) -> ustore_enum_t;
    }
}

// endregion: Raw Declarations

// region: Formats

/// Keeps the formats and the inputs of the verbs closed over the types this module names.
pub(crate) mod sealed {
    use super::*;

    /// A format with a `ustore_docs_field_type_t` of its own.
    pub trait Format {
        /// The `ustore_docs_field_type_t` this format crosses the ABI as.
        const TYPE: c_int;
    }

    /// What one docs write carries: whole documents under their keys, documents keyed by a field of their own, or
    /// values written into fields, each borrowed or owned.
    pub trait Writes {
        /// The tasks of the batch, pointing into `self`, or `None` for no documents.
        fn tasks(&self) -> Result<Option<WriteTasks>>;
    }

    /// What one docs read addresses: whole documents under keys, or fields inside them, borrowed or owned.
    pub trait Addresses {
        /// The keys and fields of the batch, pointing into `self`.
        fn tasks(&self) -> Result<ReadTasks>;
    }

    /// The keys, fields and values of a whole-document or a field write, and the format its values cross in.
    pub struct WriteTasks {
        pub count: usize,
        pub keys: ustore_keys_view_t,
        pub id_field: ustore_bytes_view_t,
        pub fields: ustore_strings_view_t,
        pub values: ustore_strings_view_t,
        pub format: c_int,
    }

    /// The keys and fields of a read, where a zeroed run of fields reads whole documents.
    pub struct ReadTasks {
        pub count: usize,
        pub keys: ustore_keys_view_t,
        pub fields: ustore_strings_view_t,
    }
}

use sealed::{Addresses, Format, ReadTasks, WriteTasks, Writes};

/// A fixed-width value a gathered column converts into.
pub trait Scalar: Format + Copy {}

/// A scalar a write carries into a whole document or into one of its fields.
///
/// [`Half`] and [`UUID`] are gathered but never written: the engine builds a document from the bytes
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
/// A gathered [`UUID`] parses the canonical thirty-six character form, in the order that text writes the bytes;
/// anything else sets the collision bit of its row and leaves the cell zero.
impl Format for UUID {
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
impl Scalar for UUID {}

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

impl Modification {
    fn from_raw(raw: ustore_enum_t) -> Option<Self> {
        match raw {
            0 => Some(Modification::Upsert),
            1 => Some(Modification::Update),
            2 => Some(Modification::Insert),
            3 => Some(Modification::Patch),
            4 => Some(Modification::Merge),
            _ => None,
        }
    }
}

impl fmt::Display for Modification {
    /// Calls `ustore_docs_modification_name`.
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(spelling(ffi::ustore_docs_modification_name, *self as ustore_enum_t))
    }
}

impl FromStr for Modification {
    type Err = Error;

    /// Calls `ustore_docs_modification_from_name`.
    fn from_str(text: &str) -> Result<Self> {
        read_spelling(
            text,
            ffi::ustore_docs_modification_from_name,
            ffi::ustore_docs_modification_name,
            Modification::from_raw,
        )
    }
}

/// The type a field crosses as, which every format above names one of.
#[repr(i32)]
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub enum FieldType {
    /// JSON text.
    #[default]
    Json = 0,
    /// The JSON null.
    Null = 10,
    /// A boolean.
    Bool = 11,
    /// A 128-bit UUID.
    UUID = 12,
    /// A 64-bit float.
    F64 = 42,
    /// A 32-bit float.
    F32 = 41,
    /// A 16-bit float.
    F16 = 40,
    /// A signed 64-bit integer.
    I64 = 23,
    /// A signed 32-bit integer.
    I32 = 22,
    /// A signed 16-bit integer.
    I16 = 21,
    /// A signed 8-bit integer.
    I8 = 20,
    /// An unsigned 64-bit integer.
    U64 = 33,
    /// An unsigned 32-bit integer.
    U32 = 32,
    /// An unsigned 16-bit integer.
    U16 = 31,
    /// An unsigned 8-bit integer.
    U8 = 30,
    /// The text of a value without a null terminator.
    Binary = 50,
    /// A value printed as text.
    String = 51,
}

impl FieldType {
    fn from_raw(raw: ustore_enum_t) -> Option<Self> {
        [
            FieldType::Json,
            FieldType::Null,
            FieldType::Bool,
            FieldType::UUID,
            FieldType::F64,
            FieldType::F32,
            FieldType::F16,
            FieldType::I64,
            FieldType::I32,
            FieldType::I16,
            FieldType::I8,
            FieldType::U64,
            FieldType::U32,
            FieldType::U16,
            FieldType::U8,
            FieldType::Binary,
            FieldType::String,
        ]
        .into_iter()
        .find(|field_type| *field_type as ustore_enum_t == raw)
    }
}

impl fmt::Display for FieldType {
    /// Calls `ustore_docs_field_type_name`.
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(spelling(ffi::ustore_docs_field_type_name, *self as ustore_enum_t))
    }
}

impl FromStr for FieldType {
    type Err = Error;

    /// Calls `ustore_docs_field_type_from_name`.
    fn from_str(text: &str) -> Result<Self> {
        read_spelling(
            text,
            ffi::ustore_docs_field_type_from_name,
            ffi::ustore_docs_field_type_name,
            FieldType::from_raw,
        )
    }
}

// endregion: Formats

// region: Inputs

/// A document and the key it is stored under, laid out so a slice of them crosses as it stands: the keys as one
/// strided run, and the documents through an accessor over the same entries.
///
/// A whole document is written in one format the batch shares: [`Json`] for the text the engine parses,
/// [`Text`] for a document that is one JSON string, or any [`WritableScalar`] for a document that is
/// one number or one boolean.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct DocumentEntry<'value, Format: FieldFormat> {
    key: Key,
    value: ustore_bytes_view_t,
    marker: PhantomData<&'value Format::Value>,
}

impl<'value, Format: FieldFormat> DocumentEntry<'value, Format> {
    /// Pairs a key with the document to store under it.
    pub fn new(key: Key, value: &'value Format::Value) -> Self {
        DocumentEntry {
            key,
            value: Format::bytes(value).into(),
            marker: PhantomData,
        }
    }

    /// The key this entry stores under.
    pub fn key(&self) -> Key {
        self.key
    }
}

/// A document carrying its own key under an id field, which [`KeyedBy`] names, laid out so a slice of them crosses
/// through an accessor.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct Document<'text> {
    text: ustore_bytes_view_t,
    marker: PhantomData<&'text str>,
}

impl<'text> Document<'text> {
    /// Borrows the JSON text of a document.
    pub fn new(text: &'text str) -> Self {
        Document {
            text: text.as_bytes().into(),
            marker: PhantomData,
        }
    }
}

/// Documents carrying their keys under one field of their own, as a docs write takes them: borrowed [`Document`]
/// values, or an owned `stringtape::CharsTape` of JSON texts, which a deferred write needs beside an owned or
/// `'static` field.
#[derive(Clone, Debug)]
pub struct KeyedBy<Field, Texts>(
    /// The field holding the key of every document, such as "_id": a non-negative integer, spelled as
    /// [`Key::from`] spells it, or a UUID in canonical text.
    pub Field,
    /// The documents.
    pub Texts,
);

/// A field of one document and the value written into it, laid out so a slice of them crosses as it stands.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct FieldEntry<'value, Format: FieldFormat> {
    key: Key,
    field: ustore_bytes_view_t,
    value: ustore_bytes_view_t,
    marker: PhantomData<&'value Format::Value>,
}

impl<'value, Format: FieldFormat> FieldEntry<'value, Format> {
    /// Pairs a field with the value to write into it.
    pub fn new(key: Key, field: &'value str, value: &'value Format::Value) -> Self {
        FieldEntry {
            key,
            field: field.as_bytes().into(),
            value: Format::bytes(value).into(),
            marker: PhantomData,
        }
    }
}

/// A field of one document to read, laid out so a slice of them crosses as it stands.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct FieldAddress<'field> {
    key: Key,
    field: ustore_bytes_view_t,
    marker: PhantomData<&'field str>,
}

impl<'field> FieldAddress<'field> {
    /// Addresses `field` of the document under `key`.
    pub fn new(key: Key, field: &'field str) -> Self {
        FieldAddress {
            key,
            field: field.as_bytes().into(),
            marker: PhantomData,
        }
    }
}

/// A field to gather from every document, borrowed or owned, and the type to convert it into.
#[derive(Clone, Debug)]
pub struct Column<'field> {
    field: Cow<'field, str>,
    format: c_int,
}

impl<'field> Column<'field> {
    /// Gathers `field`, a `&str` or an owned `String`, converted into `Format`.
    pub fn new<Format: ColumnFormat>(field: impl Into<Cow<'field, str>>) -> Self {
        Column {
            field: field.into(),
            format: Format::TYPE,
        }
    }
}

/// Writes the view of the field of every column `indices` names out of the slice of [`Column`]s at `handle`.
unsafe extern "C" fn column_fields(
    handle: *const c_void,
    indices: *const ustore_size_t,
    count: ustore_size_t,
    views: *mut ustore_bytes_view_t,
) {
    let columns = handle.cast::<Column<'static>>();
    for slot in 0..count as usize {
        let column = &*columns.add(*indices.add(slot) as usize);
        *views.add(slot) = column.field.as_bytes().into();
    }
}

/// The view of a tape of `count` strings, refusing a tape of another length.
fn counted_tape<Offset: TapeOffset>(tape: &CharsTape<Offset>, count: usize) -> Result<ustore_strings_view_t> {
    if tape.len() != count {
        return Err(fail(Status::InvalidArgument, "the tape holds a string per key"));
    }
    let (bytes, offsets) = tape.arrow_slices();
    Ok(Offset::tape(bytes, offsets))
}

impl<Format: FieldFormat> Writes for &[DocumentEntry<'_, Format>] {
    fn tasks(&self) -> Result<Option<WriteTasks>> {
        Ok(self.first().map(|first| WriteTasks {
            count: self.len(),
            keys: ustore_strided_view_t::inside::<DocumentEntry<'_, Format>>(&first.key),
            id_field: ustore_bytes_view_t::default(),
            fields: ustore_strings_view_t::default(),
            values: ustore_strings_view_t::inside::<DocumentEntry<'_, Format>>(&first.value),
            format: Format::TYPE,
        }))
    }
}

impl<Format: FieldFormat, const COUNT: usize> Writes for &[DocumentEntry<'_, Format>; COUNT] {
    fn tasks(&self) -> Result<Option<WriteTasks>> {
        self.as_slice().tasks()
    }
}

/// Keys beside a tape of their JSON texts, which a deferred write owns.
impl<Keys: AsRef<[Key]>, Offset: TapeOffset> Writes for (Keys, CharsTape<Offset>) {
    fn tasks(&self) -> Result<Option<WriteTasks>> {
        let keys = self.0.as_ref();
        let values = counted_tape(&self.1, keys.len())?;
        Ok((!keys.is_empty()).then(|| WriteTasks {
            count: keys.len(),
            keys: ustore_strided_view_t::packed(keys),
            id_field: ustore_bytes_view_t::default(),
            fields: ustore_strings_view_t::default(),
            values,
            format: Json::TYPE,
        }))
    }
}

impl<Field: AsRef<str>> Writes for KeyedBy<Field, &[Document<'_>]> {
    fn tasks(&self) -> Result<Option<WriteTasks>> {
        let KeyedBy(field, documents) = self;
        Ok(documents.first().map(|first| WriteTasks {
            count: documents.len(),
            keys: ustore_strided_view_t::default(),
            id_field: field.as_ref().as_bytes().into(),
            fields: ustore_strings_view_t::default(),
            values: ustore_strings_view_t::inside::<Document<'_>>(&first.text),
            format: Json::TYPE,
        }))
    }
}

impl<Field: AsRef<str>, const COUNT: usize> Writes for KeyedBy<Field, &[Document<'_>; COUNT]> {
    fn tasks(&self) -> Result<Option<WriteTasks>> {
        KeyedBy(self.0.as_ref(), self.1.as_slice()).tasks()
    }
}

/// A tape of JSON texts keyed by a field of their own, which a deferred write owns.
impl<Field: AsRef<str>, Offset: TapeOffset> Writes for KeyedBy<Field, CharsTape<Offset>> {
    fn tasks(&self) -> Result<Option<WriteTasks>> {
        let KeyedBy(field, texts) = self;
        let (bytes, offsets) = texts.arrow_slices();
        Ok((!texts.is_empty()).then(|| WriteTasks {
            count: texts.len(),
            keys: ustore_strided_view_t::default(),
            id_field: field.as_ref().as_bytes().into(),
            fields: ustore_strings_view_t::default(),
            values: Offset::tape(bytes, offsets),
            format: Json::TYPE,
        }))
    }
}

impl<Format: FieldFormat> Writes for &[FieldEntry<'_, Format>] {
    fn tasks(&self) -> Result<Option<WriteTasks>> {
        Ok(self.first().map(|first| WriteTasks {
            count: self.len(),
            keys: ustore_strided_view_t::inside::<FieldEntry<'_, Format>>(&first.key),
            id_field: ustore_bytes_view_t::default(),
            fields: ustore_strings_view_t::inside::<FieldEntry<'_, Format>>(&first.field),
            values: ustore_strings_view_t::inside::<FieldEntry<'_, Format>>(&first.value),
            format: Format::TYPE,
        }))
    }
}

impl<Format: FieldFormat, const COUNT: usize> Writes for &[FieldEntry<'_, Format>; COUNT] {
    fn tasks(&self) -> Result<Option<WriteTasks>> {
        self.as_slice().tasks()
    }
}

/// Keys and a tape of paths beside a tape of the JSON values written into them, which a deferred write owns.
impl<Keys: AsRef<[Key]>, PathsOffset: TapeOffset, ValuesOffset: TapeOffset> Writes
    for (Keys, CharsTape<PathsOffset>, CharsTape<ValuesOffset>)
{
    fn tasks(&self) -> Result<Option<WriteTasks>> {
        let keys = self.0.as_ref();
        let fields = counted_tape(&self.1, keys.len())?;
        let values = counted_tape(&self.2, keys.len())?;
        Ok((!keys.is_empty()).then(|| WriteTasks {
            count: keys.len(),
            keys: ustore_strided_view_t::packed(keys),
            id_field: ustore_bytes_view_t::default(),
            fields,
            values,
            format: Json::TYPE,
        }))
    }
}

/// The tasks reading the whole documents under `keys`.
fn whole_documents(keys: &[Key]) -> ReadTasks {
    ReadTasks {
        count: keys.len(),
        keys: ustore_strided_view_t::packed(keys),
        fields: ustore_strings_view_t::default(),
    }
}

impl Addresses for &[Key] {
    fn tasks(&self) -> Result<ReadTasks> {
        Ok(whole_documents(self))
    }
}

impl<const COUNT: usize> Addresses for &[Key; COUNT] {
    fn tasks(&self) -> Result<ReadTasks> {
        Ok(whole_documents(self.as_slice()))
    }
}

/// Keys of whole documents, which a deferred read owns.
impl<const COUNT: usize> Addresses for [Key; COUNT] {
    fn tasks(&self) -> Result<ReadTasks> {
        Ok(whole_documents(self))
    }
}

/// Keys of whole documents, which a deferred read owns.
impl Addresses for Vec<Key> {
    fn tasks(&self) -> Result<ReadTasks> {
        Ok(whole_documents(self))
    }
}

impl Addresses for &[FieldAddress<'_>] {
    fn tasks(&self) -> Result<ReadTasks> {
        let Some(first) = self.first() else {
            return Ok(ReadTasks {
                count: 0,
                keys: ustore_strided_view_t::default(),
                fields: ustore_strings_view_t::default(),
            });
        };
        Ok(ReadTasks {
            count: self.len(),
            keys: ustore_strided_view_t::inside::<FieldAddress<'_>>(&first.key),
            fields: ustore_strings_view_t::inside::<FieldAddress<'_>>(&first.field),
        })
    }
}

impl<const COUNT: usize> Addresses for &[FieldAddress<'_>; COUNT] {
    fn tasks(&self) -> Result<ReadTasks> {
        self.as_slice().tasks()
    }
}

/// Keys beside a tape of the paths read from them, which a deferred read owns.
impl<Keys: AsRef<[Key]>, Offset: TapeOffset> Addresses for (Keys, CharsTape<Offset>) {
    fn tasks(&self) -> Result<ReadTasks> {
        let keys = self.0.as_ref();
        Ok(ReadTasks {
            count: keys.len(),
            keys: ustore_strided_view_t::packed(keys),
            fields: counted_tape(&self.1, keys.len())?,
        })
    }
}

/// The document keys a scan keeps, what admits them and how it reports; see [`On::docs_scan`].
pub struct DocsScanConfig<'config> {
    /// Keys kept, at most.
    pub wanted: usize,
    /// Admits only the documents its filter and predicate approve; every document by default.
    pub matching: Matching<'config>,
    /// In what order the keys come.
    pub mode: ScanMode,
    /// Reports the scans finished and stops wherever it says to.
    pub progress: Option<&'config mut ProgressCallback<'config>>,
}

impl From<usize> for DocsScanConfig<'_> {
    fn from(wanted: usize) -> Self {
        DocsScanConfig {
            wanted,
            matching: Matching::default(),
            mode: ScanMode::Ordered,
            progress: None,
        }
    }
}

/// The document keys a sample draws; see [`On::docs_sample`].
#[derive(Clone, Copy, Debug)]
pub struct DocsSampleConfig {
    /// Keys drawn, at most.
    pub wanted: usize,
}

impl From<usize> for DocsSampleConfig {
    fn from(wanted: usize) -> Self {
        DocsSampleConfig { wanted }
    }
}

/// The typed columns a gather reads; see [`On::docs_gather`].
#[derive(Clone, Copy, Debug)]
pub struct DocsGatherConfig<Requested> {
    /// The fields and the format each lands in, borrowed or owned.
    pub columns: Requested,
}

impl<Requested> From<Requested> for DocsGatherConfig<Requested> {
    fn from(columns: Requested) -> Self {
        DocsGatherConfig { columns }
    }
}

/// What admits the keys a cursor hands out; see [`On::docs_cursor`].
#[derive(Debug, Default)]
pub struct DocsCursorConfig<'config> {
    /// Admits only the documents its filter and predicate approve; every document by default.
    pub matching: Matching<'config>,
}

impl<'config> From<Matching<'config>> for DocsCursorConfig<'config> {
    fn from(matching: Matching<'config>) -> Self {
        DocsCursorConfig { matching }
    }
}

impl Accepts<DocsScanConfig<'static>> for Deferring {}

// endregion: Inputs

// region: Outputs

/// The unique paths to the leaves of the documents one gist inspected, in the order they were found.
#[derive(Clone, Copy, Debug)]
pub struct Paths<'arena> {
    paths: Strings<'arena>,
}

impl<'arena> Paths<'arena> {
    /// The `count` paths a gist exported as the tape `fields`, refusing one that is not UTF-8.
    ///
    /// # Safety
    /// A successful gist exported `count` paths as the tape `fields`, which stays alive for `'arena`.
    unsafe fn exported(count: ustore_size_t, fields: ustore_strings_view_t) -> Result<Self> {
        let paths = Strings::exported(fields, count as usize)?;
        for index in 0..paths.len() {
            let path = paths.get(index).unwrap_or_default();
            std::str::from_utf8(path).map_err(|_| fail(Status::Malformed, "a path is not UTF-8"))?;
        }
        Ok(Paths { paths })
    }

    /// How many unique paths the gist found.
    pub fn len(&self) -> usize {
        self.paths.len()
    }

    /// Whether the gist found no leaves at all.
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// The JSON Pointer at `index`, ready to address a field, or `None` past the end.
    pub fn get(&self, index: usize) -> Option<&'arena str> {
        // SAFETY: every path was checked for UTF-8 when the gist returned.
        self.paths
            .get(index)
            .map(|path| unsafe { std::str::from_utf8_unchecked(path) })
    }

    /// Every path in the order the gist found it.
    pub fn iter(&self) -> impl Iterator<Item = &'arena str> + 'arena {
        let paths = *self;
        (0..paths.len()).filter_map(move |index| paths.get(index))
    }
}

/// One bit per row of a batch, least significant bit first.
#[derive(Clone, Copy, Debug)]
pub struct Bitmap<'arena> {
    bytes: &'arena [u8],
    count: usize,
}

impl<'arena> Bitmap<'arena> {
    /// The bitmap of `count` rows in `bytes`, which hold at least one bit per row.
    pub(crate) fn new(bytes: &'arena [u8], count: usize) -> Self {
        Bitmap { bytes, count }
    }

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
pub struct StringColumn<'arena> {
    validities: Bitmap<'arena>,
    strings: Strings<'arena>,
    /// Bytes closing every string past its text: the null terminator of [`Text`], and none of [`Binary`].
    terminator: usize,
}

impl<'arena> StringColumn<'arena> {
    /// How many rows the column holds.
    pub fn len(&self) -> usize {
        self.validities.len()
    }

    /// Whether the column holds no rows.
    pub fn is_empty(&self) -> bool {
        self.validities.is_empty()
    }

    /// The string of `row` without its terminator, or `None` for a row whose field was missing or would not convert.
    pub fn get(&self, row: usize) -> Option<&'arena [u8]> {
        if !self.validities.get(row) {
            return None;
        }
        let string = self.strings.get(row)?;
        string.get(..string.len().saturating_sub(self.terminator))
    }

    /// Every row in order.
    pub fn iter(&self) -> Rows<StringColumn<'arena>, Option<&'arena [u8]>> {
        Rows::new(*self, self.len(), StringColumn::get)
    }
}

impl<'arena> IntoIterator for StringColumn<'arena> {
    type Item = Option<&'arena [u8]>;
    type IntoIter = Rows<StringColumn<'arena>, Option<&'arena [u8]>>;

    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}

impl<'arena> IntoIterator for &StringColumn<'arena> {
    type Item = Option<&'arena [u8]>;
    type IntoIter = Rows<StringColumn<'arena>, Option<&'arena [u8]>>;

    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}

/// The columns one gather exported, a row per document and a column per requested field, holding the requests that
/// name the format of each.
///
/// A value is valid when it was found and converted, converted when the conversion changed its meaning,
/// such as text parsed into a number, and colliding when a value of an inconvertible type sits there.
#[derive(Clone, Copy, Debug)]
pub struct Columns<'arena, Requested> {
    requested: Requested,
    rows: usize,
    /// The arena holding the columns, borrowed for `'arena`.
    arena: ustore_arena_t,
    /// One view per requested column, or none for a gather that reached no engine.
    views: &'arena [ffi::ustore_docs_column_view_t],
}

impl<'arena, 'field, Requested: AsRef<[Column<'field>]>> Columns<'arena, Requested> {
    /// How many documents the gather read.
    pub fn rows(&self) -> usize {
        self.rows
    }

    /// How many fields the gather read.
    pub fn columns(&self) -> usize {
        self.requested.as_ref().len()
    }

    /// Which rows of `column` hold a value, or `None` past the last column.
    pub fn validities(&self, column: usize) -> Result<Option<Bitmap<'arena>>> {
        self.bitmap(column, |view| view.validities)
    }

    /// Which rows of `column` changed their meaning on conversion, or `None` past the last column.
    pub fn conversions(&self, column: usize) -> Result<Option<Bitmap<'arena>>> {
        self.bitmap(column, |view| view.conversions)
    }

    /// Which rows of `column` hold a value that would not convert, or `None` past the last column.
    pub fn collisions(&self, column: usize) -> Result<Option<Bitmap<'arena>>> {
        self.bitmap(column, |view| view.collisions)
    }

    /// The values of a scalar column, one per row, with zero or a partial conversion in invalid rows.
    pub fn scalars<Value: Scalar>(&self, column: usize) -> Result<&'arena [Value]> {
        if self.requested_format(column)? != Value::TYPE {
            return Err(fail(Status::InvalidArgument, "the column was gathered as another type"));
        }
        let Some(view) = self.views.get(column) else {
            return Ok(&[]);
        };
        host_arena(self.arena)?;
        // SAFETY: a successful gather writes every row of a column of the requested type into the arena, which stays
        // borrowed; a C++ `bool` holds only zero or one.
        Ok(unsafe { exported_slice(view.scalars.cast::<Value>(), self.rows) })
    }

    /// The values of a column gathered as [`Text`] or [`Binary`].
    pub fn strings(&self, column: usize) -> Result<StringColumn<'arena>> {
        let terminator = match self.requested_format(column)? {
            Text::TYPE => 1,
            Binary::TYPE => 0,
            _ => return Err(fail(Status::InvalidArgument, "the column was not gathered as strings")),
        };
        let validities = self
            .bitmap(column, |view| view.validities)?
            .unwrap_or(Bitmap { bytes: &[], count: 0 });
        let strings = match self.views.get(column) {
            // SAFETY: a successful gather exports the strings of every row of a string column as a tape in the arena,
            // which stays borrowed.
            Some(view) => unsafe { Strings::exported(view.strings, self.rows)? },
            None => Strings::empty(),
        };
        Ok(StringColumn {
            validities,
            strings,
            terminator,
        })
    }

    /// Every column as one Arrow array beside its schema, in request order, or none for a gather
    /// that reached no engine.
    ///
    /// Scalars and [`Binary`] columns export in place, and [`Text`] ones copied without their
    /// terminators. The columns of a device arena stay on the device, as
    /// [`ArrowDeviceArray::device_type`] says. Each export borrows the arena, so the call that
    /// would rewind it does not compile while one lives:
    ///
    /// ```compile_fail,E0499
    /// use ustore::{Arena, Column, RAMOptions, Database, Key, Modality, Text};
    ///
    /// let database = Database::init_ram(&RAMOptions::default())?;
    /// let mut arena = Arena::new()?;
    /// let people = database.collection_create("people", Modality::Docs)?;
    /// let columns = [Column::new::<Text>("name")];
    /// let ada = Key::from(1u64);
    /// let exported = database.head().docs_gather(people, &[ada], &columns, &mut arena)?.to_arrow()?;
    /// database.head().docs_gist(people, [ada], &mut arena)?;
    /// drop(exported);
    /// # Ok::<(), ustore::Error>(())
    /// ```
    ///
    /// Calls `ustore_docs_column_view_to_arrow`.
    pub fn to_arrow(&self) -> Result<Vec<ArrowExport<'arena>>> {
        let mut exported = Vec::new();
        exported
            .try_reserve_exact(self.views.len())
            .map_err(|_| fail(Status::OutOfMemory, "the column exports could not be staged"))?;
        for (column, request) in (0..self.views.len()).zip(self.requested.as_ref()) {
            let mut schema = ArrowSchema::empty();
            let mut array = ArrowDeviceArray::empty();
            // SAFETY: the view is an address in the borrowed arena, which the library reads
            // wherever it lives.
            let status = unsafe {
                arrow_ffi::ustore_docs_column_view_to_arrow(
                    self.views.as_ptr().add(column),
                    self.rows as ustore_size_t,
                    request.format,
                    self.arena,
                    &mut schema,
                    &mut array,
                )
            };
            check(ustore_outcome_t {
                status,
                message: c"failed to export a gathered column".as_ptr(),
            })?;
            exported.push(ArrowExport {
                schema,
                array,
                marker: PhantomData,
            });
        }
        Ok(exported)
    }

    /// The format `column` was requested in, refusing a column the gather never read.
    fn requested_format(&self, column: usize) -> Result<c_int> {
        match self.requested.as_ref().get(column) {
            Some(request) => Ok(request.format),
            None => Err(fail(Status::InvalidArgument, "the table has no such column")),
        }
    }

    /// The bitmap of `column` that `bits` picks out of its view, or `None` past the last column.
    fn bitmap(
        &self,
        column: usize,
        bits: fn(&ffi::ustore_docs_column_view_t) -> *const ustore_octet_t,
    ) -> Result<Option<Bitmap<'arena>>> {
        if column >= self.columns() {
            return Ok(None);
        }
        let bytes = match self.views.get(column) {
            Some(view) => {
                host_arena(self.arena)?;
                // SAFETY: a successful gather exports every bitmap of a column covering every row,
                // in the arena.
                unsafe { exported_slice(bits(view), self.rows.div_ceil(8)) }
            }
            None => &[],
        };
        Ok(Some(Bitmap {
            bytes,
            count: self.rows,
        }))
    }
}

// endregion: Outputs

// region: Verbs

impl<Parent: Writable, Completion: crate::db::sealed::Completion> On<'_, Parent, Completion> {
    /// Writes whole documents or values into fields inside them, where [`On::docs_remove`] removes
    /// documents and a merge with a JSON `null` removes a field, and hands what it wrote back.
    ///
    /// The batch is one of:
    /// - borrowed [`DocumentEntry`] values of one format, which is [`Json`] for the text the engine parses and a
    ///   [`WritableScalar`] or [`Text`] for a document that is one scalar;
    /// - [`KeyedBy`] documents, carrying their keys under a field of their own;
    /// - borrowed [`FieldEntry`] values of one format, writing into fields of existing documents;
    /// - the owned forms a deferred write needs: keys beside a `stringtape::CharsTape` of JSON texts, keys and a tape
    ///   of paths beside a tape of JSON values, or a tape of JSON texts keyed by a field.
    ///
    /// A transaction flushes at its commit, so [`Durability::Flushed`] is refused inside one.
    ///
    /// Calls `ustore_docs_write`.
    pub fn docs_write<'collections, Documents: Writes>(
        &self,
        collections: impl Into<Collections<'collections>>,
        documents: Documents,
        modification: Modification,
        durability: Durability,
    ) -> Completion::Answer<DocsWrite<'collections, Documents>>
    where
        Completion: Accepts<Collections<'collections>> + Accepts<Documents>,
    {
        let verb = DocsWrite {
            collections: collections.into(),
            documents,
            modification,
            durability,
        };
        Completion::answer(self, verb)
    }

    /// Removes a batch of whole documents, whether or not the collections hold them, and hands the keys back.
    ///
    /// Calls `ustore_docs_remove`.
    pub fn docs_remove<'collections, Keys: AsRef<[Key]>>(
        &self,
        collections: impl Into<Collections<'collections>>,
        keys: Keys,
        durability: Durability,
    ) -> Completion::Answer<DocsRemove<'collections, Keys>>
    where
        Completion: Accepts<Collections<'collections>> + Accepts<Keys>,
    {
        let verb = DocsRemove {
            collections: collections.into(),
            keys,
            durability,
        };
        Completion::answer(self, verb)
    }

    /// Removes every document of one collection from `range.start`, inclusive, to `range.end`,
    /// exclusive, in one atomic call.
    pub fn docs_remove_range(
        &self,
        collection: CollectionID,
        range: Range<Key>,
        durability: Durability,
    ) -> Completion::Answer<DocsRemoveRange> {
        let verb = DocsRemoveRange {
            collection,
            range,
            durability,
        };
        Completion::answer(self, verb)
    }
}

impl<Parent, Completion: crate::db::sealed::Completion> On<'_, Parent, Completion> {
    /// Lists up to `wanted` document keys of one collection's `range` into the arena, keeping those
    /// whose document the filter of `matching` admits and the keys its predicate admits.
    ///
    /// Calls `ustore_docs_scan`.
    pub fn docs_scan<'config, 'arena>(
        &self,
        collection: CollectionID,
        range: impl Into<KeyRange>,
        config: impl Into<DocsScanConfig<'config>>,
        arena: &'arena mut Arena,
    ) -> Completion::Answer<DocsScan<'arena, 'config>>
    where
        Completion: Accepts<DocsScanConfig<'config>>,
    {
        let range = range.into();
        let DocsScanConfig {
            wanted,
            matching,
            mode,
            progress,
        } = config.into();
        let verb = DocsScan {
            collection,
            start: range.start_key(),
            range,
            arena,
            wanted,
            matched: Matched::new(matching),
            mode,
            progress: Progress::new(progress),
            found: Found::default(),
        };
        Completion::answer(self, verb)
    }

    /// Draws up to `wanted` document keys uniformly at random from one collection into the arena.
    pub fn docs_sample<'arena>(
        &self,
        collection: CollectionID,
        config: impl Into<DocsSampleConfig>,
        arena: &'arena mut Arena,
    ) -> Completion::Answer<DocsSample<'arena>> {
        let verb = DocsSample {
            collection,
            arena,
            wanted: config.into().wanted,
            found: Found::default(),
        };
        Completion::answer(self, verb)
    }

    /// Estimates the documents and the space a range of keys takes in one collection, through the
    /// arena its bounds land in.
    pub fn docs_measure<'arena>(
        &self,
        collection: CollectionID,
        range: impl Into<KeyRange>,
        arena: &'arena mut Arena,
    ) -> Completion::Answer<DocsMeasure<'arena>> {
        let range = range.into();
        let verb = DocsMeasure {
            collection,
            start: range.start_key(),
            range,
            arena,
            bounds: Bounds::default(),
        };
        Completion::answer(self, verb)
    }

    /// Reads whole documents or fields inside them into the arena in `format`, [`Json`] or [`Text`], reporting a
    /// missing document or field as absent.
    ///
    /// The batch is keys of whole documents, borrowed [`FieldAddress`] values, or owned keys beside a
    /// `stringtape::CharsTape` of paths, a path per key, which a deferred read needs.
    ///
    /// Calls `ustore_docs_read`.
    pub fn docs_read<'arena, 'collections, Targets: Addresses, Format: ReadFormat>(
        &self,
        collections: impl Into<Collections<'collections>>,
        targets: Targets,
        format: Format,
        arena: &'arena mut Arena,
    ) -> Completion::Answer<DocsRead<'arena, 'collections, Targets>>
    where
        Completion: Accepts<Collections<'collections>> + Accepts<Targets>,
    {
        let _ = format;
        let verb = DocsRead {
            collections: collections.into(),
            targets,
            arena,
            format: Format::TYPE,
            count: 0,
            presences: Address::default(),
            values: ustore_strings_view_t::default(),
        };
        Completion::answer(self, verb)
    }

    /// Lists the unique paths to the leaves of the documents under `keys` into the arena.
    ///
    /// Calls `ustore_docs_gist`.
    pub fn docs_gist<'arena, 'collections, Keys: AsRef<[Key]>>(
        &self,
        collections: impl Into<Collections<'collections>>,
        keys: Keys,
        arena: &'arena mut Arena,
    ) -> Completion::Answer<DocsGist<'arena, 'collections, Keys>>
    where
        Completion: Accepts<Collections<'collections>> + Accepts<Keys>,
    {
        let verb = DocsGist {
            collections: collections.into(),
            keys,
            arena,
            written: 0,
            fields: ustore_strings_view_t::default(),
        };
        Completion::answer(self, verb)
    }

    /// Reads `columns` of the documents under `keys` into typed columns in the arena, a row per key.
    ///
    /// The answered [`Columns`] hold the requested columns, borrowed or owned, while a deferred gather needs owned
    /// keys and owned columns, whose paths are owned or `'static`.
    ///
    /// Calls `ustore_docs_gather`.
    pub fn docs_gather<'arena, 'collections, Keys: AsRef<[Key]>, Requested>(
        &self,
        collections: impl Into<Collections<'collections>>,
        keys: Keys,
        config: impl Into<DocsGatherConfig<Requested>>,
        arena: &'arena mut Arena,
    ) -> Completion::Answer<DocsGather<'arena, 'collections, Keys, Requested>>
    where
        Completion: Accepts<Collections<'collections>> + Accepts<Keys> + Accepts<Requested>,
        DocsGather<'arena, 'collections, Keys, Requested>: Calling,
    {
        let verb = DocsGather {
            collections: collections.into(),
            keys,
            columns: config.into().columns,
            arena,
            formats: Vec::new(),
            views: Address::default(),
        };
        Completion::answer(self, verb)
    }
}

/// A write of whole documents or of fields inside them; see [`On::docs_write`].
#[derive(Debug)]
pub struct DocsWrite<'collections, Documents> {
    collections: Collections<'collections>,
    documents: Documents,
    modification: Modification,
    durability: Durability,
}

impl<Documents: Writes> Calling for DocsWrite<'_, Documents> {
    type Arguments = ffi::ustore_docs_write_t;
    type Output = Documents;

    fn arguments<Parent, Completion>(
        &mut self,
        view: &On<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let Some(tasks) = self.documents.tasks()? else {
            return Ok(None);
        };
        Ok(Some(ffi::ustore_docs_write_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            transaction: view.transaction(),
            count: tasks.count as ustore_size_t,
            collections: self.collections.view(tasks.count)?,
            keys: tasks.keys,
            id_field: tasks.id_field,
            fields: tasks.fields,
            values: tasks.values,
            type_: tasks.format,
            modification: self.modification as ustore_enum_t,
            durability: self.durability,
            watch: view.watch(),
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_docs_write(arguments)
    }

    fn finish(self) -> Result<Documents> {
        Ok(self.documents)
    }
}

/// A removal of a batch of whole documents; see [`On::docs_remove`].
#[derive(Debug)]
pub struct DocsRemove<'collections, Keys> {
    collections: Collections<'collections>,
    keys: Keys,
    durability: Durability,
}

impl<Keys: AsRef<[Key]>> Calling for DocsRemove<'_, Keys> {
    type Arguments = ffi::ustore_docs_remove_t;
    type Output = Keys;

    fn arguments<Parent, Completion>(
        &mut self,
        view: &On<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let keys = self.keys.as_ref();
        if keys.is_empty() {
            return Ok(None);
        }
        Ok(Some(ffi::ustore_docs_remove_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            transaction: view.transaction(),
            count: keys.len() as ustore_size_t,
            collections: self.collections.view(keys.len())?,
            keys: ustore_strided_view_t::packed(keys),
            end_keys: ustore_keys_view_t::default(),
            durability: self.durability,
            watch: view.watch(),
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_docs_remove(arguments)
    }

    fn finish(self) -> Result<Keys> {
        Ok(self.keys)
    }
}

/// A removal of one range of documents; see [`On::docs_remove_range`].
#[derive(Debug)]
pub struct DocsRemoveRange {
    collection: CollectionID,
    range: Range<Key>,
    durability: Durability,
}

impl Calling for DocsRemoveRange {
    type Arguments = ffi::ustore_docs_remove_t;
    type Output = ();

    fn arguments<Parent, Completion>(
        &mut self,
        view: &On<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        Ok(Some(ffi::ustore_docs_remove_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            transaction: view.transaction(),
            count: 1,
            collections: ustore_strided_view_t::repeated(&self.collection),
            keys: ustore_strided_view_t::repeated(&self.range.start),
            end_keys: ustore_strided_view_t::repeated(&self.range.end),
            durability: self.durability,
            watch: view.watch(),
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_docs_remove(arguments)
    }

    fn finish(self) -> Result<()> {
        Ok(())
    }
}

/// A walk over the document keys of one collection; see [`On::docs_scan`].
#[derive(Debug)]
pub struct DocsScan<'arena, 'matching> {
    collection: CollectionID,
    start: Key,
    range: KeyRange,
    arena: &'arena mut Arena,
    wanted: usize,
    matched: Matched<'matching>,
    mode: ScanMode,
    progress: Progress<'matching>,
    found: Found,
}

impl<'arena> Calling for DocsScan<'arena, '_> {
    type Arguments = ffi::ustore_docs_scan_t;
    type Output = &'arena [Key];

    fn arguments<Parent, Completion>(
        &mut self,
        view: &On<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let (progress, progress_state) = self.progress.lowered();
        Ok(Some(ffi::ustore_docs_scan_t {
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
            filter: self.matched.filter(),
            predicate: self.matched.predicate(),
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
        ffi::ustore_docs_scan(arguments)
    }

    fn raise(&mut self) {
        self.matched.raise();
        self.progress.raise();
    }

    fn finish(self) -> Result<&'arena [Key]> {
        self.arena.host()?;
        // SAFETY: the scan succeeded, and the arena stays borrowed for `'arena`.
        Ok(unsafe { self.found.keys() })
    }
}

/// A random draw of document keys from one collection; see [`On::docs_sample`].
#[derive(Debug)]
pub struct DocsSample<'arena> {
    collection: CollectionID,
    arena: &'arena mut Arena,
    wanted: usize,
    found: Found,
}

impl<'arena> Calling for DocsSample<'arena> {
    type Arguments = ffi::ustore_docs_sample_t;
    type Output = &'arena [Key];

    fn arguments<Parent, Completion>(
        &mut self,
        view: &On<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        Ok(Some(ffi::ustore_docs_sample_t {
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
        ffi::ustore_docs_sample(arguments)
    }

    fn finish(self) -> Result<&'arena [Key]> {
        self.arena.host()?;
        // SAFETY: the sample succeeded, and the arena stays borrowed for `'arena`.
        Ok(unsafe { self.found.keys() })
    }
}

/// An estimate of what a range of documents holds; see [`On::docs_measure`].
#[derive(Debug)]
pub struct DocsMeasure<'arena> {
    collection: CollectionID,
    start: Key,
    range: KeyRange,
    arena: &'arena mut Arena,
    bounds: Bounds,
}

impl Calling for DocsMeasure<'_> {
    type Arguments = ffi::ustore_docs_measure_t;
    type Output = Measurement;

    fn arguments<Parent, Completion>(
        &mut self,
        view: &On<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let [min_cardinalities, max_cardinalities, min_value_bytes, max_value_bytes, min_space_usages, max_space_usages] =
            &mut self.bounds.0;
        Ok(Some(ffi::ustore_docs_measure_t {
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
        ffi::ustore_docs_measure(arguments)
    }

    fn finish(self) -> Result<Measurement> {
        self.arena.host()?;
        // SAFETY: the measurement of one range succeeded.
        Ok(unsafe { self.bounds.measurement() })
    }
}

/// A read of whole documents or of fields inside them; see [`On::docs_read`].
#[derive(Debug)]
pub struct DocsRead<'arena, 'collections, Targets> {
    collections: Collections<'collections>,
    targets: Targets,
    arena: &'arena mut Arena,
    format: c_int,
    /// How many documents or fields the read addressed, once its arguments are filled.
    count: usize,
    presences: Address<ustore_octet_t>,
    values: ustore_strings_view_t,
}

impl<'arena, Targets: Addresses> Calling for DocsRead<'arena, '_, Targets> {
    type Arguments = ffi::ustore_docs_read_t;
    type Output = Values<'arena>;

    fn arguments<Parent, Completion>(
        &mut self,
        view: &On<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let tasks = self.targets.tasks()?;
        let collections = self.collections.view(tasks.count)?;
        self.count = tasks.count;
        if tasks.count == 0 {
            return Ok(None);
        }
        Ok(Some(ffi::ustore_docs_read_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            view: view.view(),
            count: tasks.count as ustore_size_t,
            collections,
            keys: tasks.keys,
            fields: tasks.fields,
            presences: &mut self.presences.0,
            lengths: ptr::null_mut(),
            values: &mut self.values,
            arena: self.arena.handle,
            outputs_type: self.format,
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_docs_read(arguments)
    }

    fn finish(self) -> Result<Values<'arena>> {
        self.arena.host()?;
        // SAFETY: the read succeeded over every task, and the arena stays borrowed for `'arena`.
        unsafe { Values::exported(self.count, self.presences, self.values) }
    }
}

/// A listing of the paths to the leaves of documents; see [`On::docs_gist`].
#[derive(Debug)]
pub struct DocsGist<'arena, 'collections, Keys> {
    collections: Collections<'collections>,
    keys: Keys,
    arena: &'arena mut Arena,
    written: ustore_size_t,
    fields: ustore_strings_view_t,
}

impl<'arena, Keys: AsRef<[Key]>> Calling for DocsGist<'arena, '_, Keys> {
    type Arguments = ffi::ustore_docs_gist_t;
    type Output = Paths<'arena>;

    fn arguments<Parent, Completion>(
        &mut self,
        view: &On<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let keys = self.keys.as_ref();
        Ok(Some(ffi::ustore_docs_gist_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            view: view.view(),
            count: keys.len() as ustore_size_t,
            collections: self.collections.view(keys.len())?,
            keys: ustore_strided_view_t::packed(keys),
            fields: &mut self.fields,
            written: &mut self.written,
            arena: self.arena.handle,
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_docs_gist(arguments)
    }

    fn finish(self) -> Result<Paths<'arena>> {
        self.arena.host()?;
        // SAFETY: the gist succeeded, and the arena stays borrowed for `'arena`.
        unsafe { Paths::exported(self.written, self.fields) }
    }
}

/// A conversion of fields into typed columns; see [`On::docs_gather`].
#[derive(Debug)]
pub struct DocsGather<'arena, 'collections, Keys, Requested> {
    collections: Collections<'collections>,
    keys: Keys,
    columns: Requested,
    arena: &'arena mut Arena,
    /// The type of every column, packed as the arguments read them until the call is final.
    formats: Vec<c_int>,
    views: Address<ffi::ustore_docs_column_view_t>,
}

impl<'arena, 'field, Keys: AsRef<[Key]>, Requested: AsRef<[Column<'field>]>> Calling
    for DocsGather<'arena, '_, Keys, Requested>
{
    type Arguments = ffi::ustore_docs_gather_t;
    type Output = Columns<'arena, Requested>;

    fn arguments<Parent, Completion>(
        &mut self,
        view: &On<'_, Parent, Completion>,
        outcome: *mut ustore_outcome_t,
        completion: ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let (keys, columns) = (self.keys.as_ref(), self.columns.as_ref());
        let collections = self.collections.view(keys.len())?;
        if keys.is_empty() || columns.is_empty() {
            return Ok(None);
        }
        self.formats
            .try_reserve_exact(columns.len())
            .map_err(|_| fail(Status::OutOfMemory, "the column types could not be staged"))?;
        self.formats.extend(columns.iter().map(|column| column.format));
        Ok(Some(ffi::ustore_docs_gather_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            view: view.view(),
            count: keys.len() as ustore_size_t,
            collections,
            keys: ustore_strided_view_t::packed(keys),
            fields_count: columns.len() as ustore_size_t,
            fields: ustore_strings_view_t::accessor(column_fields, columns.as_ptr().cast()),
            columns: &mut self.views.0,
            arena: self.arena.handle,
            outputs_types: self.formats.as_ptr(),
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_docs_gather(arguments)
    }

    fn finish(self) -> Result<Columns<'arena, Requested>> {
        let fields = self.columns.as_ref().len();
        // SAFETY: the gather succeeded into the arena, which stays borrowed for `'arena`, or reached no engine and left
        // the views null.
        let views = unsafe { exported_slice(self.views.0, fields) };
        Ok(Columns {
            rows: self.keys.as_ref().len(),
            arena: self.arena.handle,
            requested: self.columns,
            views,
        })
    }
}

// endregion: Verbs

// region: Tests

#[cfg(test)]
mod tests {
    use std::ffi::CStr;
    use std::slice;

    use super::*;
    use crate::blobs::Entry;
    use crate::db::{CollectionID, Collections, Database, Executor, Modality, RAMOptions, Watch};
    use crate::testing::{created, key, keys};

    /// A database whose docs collection holds Ada under key one and Grace under key two.
    fn populated() -> (Database, CollectionID, Arena) {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let docs = created(&database, Modality::Docs);
        let documents = [
            DocumentEntry::<Json>::new(key(1), r#"{"name":"Ada","born":1815,"languages":["English","French"]}"#),
            DocumentEntry::<Json>::new(
                key(2),
                r#"{"name":"Grace","born":"1906","navy":{"rank":"Rear Admiral"}}"#,
            ),
        ];
        database
            .head()
            .docs_write(docs, &documents, Modification::Upsert, Durability::Buffered)
            .unwrap();
        (database, docs, Arena::new().unwrap())
    }

    #[test]
    fn filters_narrow_documents_cursors() {
        let (database, docs, mut arena) = populated();
        let graces = Matching {
            filter: Some(r#"?@.name == "Grace""#),
            ..Matching::default()
        };
        let head = database.head();
        let mut cursor = head.docs_cursor(docs, key(0), graces).unwrap();
        let page = cursor.next_pairs(8, &mut arena).unwrap();
        assert_eq!(page.keys(), &keys([2]));
        assert!(std::str::from_utf8(page.get(0).unwrap())
            .unwrap()
            .contains("Rear Admiral"));
        assert!(cursor.next(8, &mut arena).unwrap().is_empty());
    }

    #[test]
    fn document_ranges_sample_measure_and_remove() {
        let (database, docs, mut arena) = populated();
        let head = database.head();
        assert_eq!(head.docs_sample(docs, 8, &mut arena).unwrap().len(), 2);
        let scanned = head.docs_scan(docs, key(0), 8, &mut arena).unwrap();
        assert_eq!(scanned, &keys([1, 2]));
        let graces = Matching {
            filter: Some(r#"?@.name == "Grace""#),
            ..Matching::default()
        };
        assert_eq!(
            head.docs_scan(
                docs,
                key(0),
                DocsScanConfig {
                    matching: graces,
                    ..DocsScanConfig::from(8)
                },
                &mut arena
            )
            .unwrap(),
            &keys([2])
        );
        let measured = head.docs_measure(docs, key(1)..key(2), &mut arena).unwrap();
        assert!(measured.min_cardinality <= 1 && measured.max_cardinality >= measured.min_cardinality);
        head.docs_remove_range(docs, key(1)..key(2), Durability::Buffered)
            .unwrap();
        let mut cursor = head.docs_cursor(docs, key(0), Matching::default()).unwrap();
        assert_eq!(cursor.next(8, &mut arena).unwrap(), &keys([2]));
    }

    /// The text of the field at `index`, or `None` when it is absent.
    fn text<'arena>(values: &Values<'arena>, index: usize) -> Option<&'arena str> {
        values.get(index).map(|bytes| std::str::from_utf8(bytes).unwrap())
    }

    #[test]
    fn whole_documents_read_back_as_written() {
        let (database, docs, mut arena) = populated();
        let grace = r#"{"name":"Grace","born":"1906","navy":{"rank":"Rear Admiral"}}"#;
        let documents = database
            .head()
            .docs_read(docs, keys([2, 1, 3]), Json, &mut arena)
            .unwrap();
        assert_eq!(documents.len(), 3);
        assert_eq!(text(&documents, 0), Some(grace));
        assert!(text(&documents, 1).unwrap().starts_with(r#"{"name":"Ada""#));
        assert_eq!(documents.get(2), None, "a key nobody wrote holds no document");

        // An empty field addresses the whole document, as the empty JSON Pointer does.
        let whole = database
            .head()
            .docs_read(docs, &[FieldAddress::new(key(2), "")], Json, &mut arena)
            .unwrap();
        assert_eq!(text(&whole, 0), Some(grace));
    }

    #[test]
    fn documents_refuse_text_that_is_not_json() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let docs = created(&database, Modality::Docs);
        let documents = [DocumentEntry::<Json>::new(key(1), "{\"name\":")];
        let failure = database
            .head()
            .docs_write(docs, &documents, Modification::Upsert, Durability::Buffered)
            .unwrap_err();
        assert_eq!(failure.status, Status::Malformed);
    }

    #[test]
    fn documents_insert_once_and_update_in_place() {
        let (database, docs, mut arena) = populated();
        let again = [DocumentEntry::<Json>::new(key(1), r#"{"name":"Someone else"}"#)];
        let failure = database
            .head()
            .docs_write(docs, &again, Modification::Insert, Durability::Buffered)
            .unwrap_err();
        assert_eq!(failure.status, Status::AlreadyExists);

        let fresh = [DocumentEntry::<Json>::new(key(3), r#"{"name":"Katherine"}"#)];
        database
            .head()
            .docs_write(docs, &fresh, Modification::Insert, Durability::Buffered)
            .unwrap();

        let merge = [DocumentEntry::<Json>::new(key(3), r#"{"born":1918}"#)];
        database
            .head()
            .docs_write(docs, &merge, Modification::Merge, Durability::Buffered)
            .unwrap();
        let patch = [DocumentEntry::<Json>::new(
            key(3),
            r#"[{"op":"add","path":"/surname","value":"Johnson"}]"#,
        )];
        database
            .head()
            .docs_write(docs, &patch, Modification::Patch, Durability::Buffered)
            .unwrap();

        let fields = [
            FieldAddress::new(key(3), "name"),
            FieldAddress::new(key(3), "born"),
            FieldAddress::new(key(3), "surname"),
        ];
        let read = database.head().docs_read(docs, &fields, Text, &mut arena).unwrap();
        assert_eq!(text(&read, 0), Some("Katherine"), "a merge keeps what it does not name");
        assert_eq!(text(&read, 1), Some("1918"));
        assert_eq!(text(&read, 2), Some("Johnson"));
    }

    #[test]
    fn a_patch_that_does_not_apply_is_refused() {
        let (database, docs, _) = populated();
        let patch = [DocumentEntry::<Json>::new(
            key(1),
            r#"[{"op":"remove","path":"/nowhere"}]"#,
        )];
        let failure = database
            .head()
            .docs_write(docs, &patch, Modification::Patch, Durability::Buffered)
            .unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument, "{failure:?}");

        // An update or a patch replaces what exists, so a missing document refuses both.
        for modification in [Modification::Update, Modification::Patch] {
            let modifier = [DocumentEntry::<Json>::new(key(42), r#"[]"#)];
            let failure = database
                .head()
                .docs_write(docs, &modifier, modification, Durability::Buffered)
                .unwrap_err();
            assert_eq!(failure.status, Status::NotFound, "{modification:?}");
        }
    }

    #[test]
    fn documents_carry_their_keys_under_an_id_field() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let docs = created(&database, Modality::Docs);
        let mut arena = Arena::new().unwrap();
        let documents = [
            Document::new(r#"{"_id":7,"name":"Ada"}"#),
            Document::new(r#"{"_id":9,"name":"Grace"}"#),
        ];
        database
            .head()
            .docs_write(
                docs,
                KeyedBy("_id", &documents),
                Modification::Upsert,
                Durability::Buffered,
            )
            .unwrap();
        let names = database
            .head()
            .docs_read(
                docs,
                &[FieldAddress::new(key(9), "name"), FieldAddress::new(key(7), "name")],
                Text,
                &mut arena,
            )
            .unwrap();
        assert_eq!(text(&names, 0), Some("Grace"));
        assert_eq!(text(&names, 1), Some("Ada"));

        let unkeyed = [Document::new(r#"{"_id":"seven"}"#)];
        let failure = database
            .head()
            .docs_write(
                docs,
                KeyedBy("_id", &unkeyed),
                Modification::Upsert,
                Durability::Buffered,
            )
            .unwrap_err();
        assert_eq!(failure.status, Status::Malformed);
    }

    #[test]
    fn fields_write_every_format_and_read_back_as_text_or_json() {
        let (database, docs, mut arena) = populated();
        let head = database.head();
        let upsert = Modification::Upsert;
        let buffered = Durability::Buffered;

        let died = [FieldEntry::<i64>::new(key(1), "died", &1852)];
        head.docs_write(docs, &died, upsert, buffered).unwrap();
        let height = [FieldEntry::<f64>::new(key(1), "height", &1.65)];
        head.docs_write(docs, &height, upsert, buffered).unwrap();
        let countess = [FieldEntry::<bool>::new(key(1), "countess", &true)];
        head.docs_write(docs, &countess, upsert, buffered).unwrap();
        let title = [FieldEntry::<Text>::new(key(1), "title", "Countess of Lovelace")];
        head.docs_write(docs, &title, upsert, buffered).unwrap();
        let nested = [
            FieldEntry::<Json>::new(key(1), "/family", r#"{"father":"Byron"}"#),
            FieldEntry::<Json>::new(key(2), "/navy/rank", r#""Commodore""#),
        ];
        head.docs_write(docs, &nested, upsert, buffered).unwrap();

        let fields = [
            FieldAddress::new(key(1), "died"),
            FieldAddress::new(key(1), "height"),
            FieldAddress::new(key(1), "countess"),
            FieldAddress::new(key(1), "title"),
            FieldAddress::new(key(1), "/family/father"),
            FieldAddress::new(key(2), "/navy/rank"),
            FieldAddress::new(key(1), "/languages/1"),
        ];
        let texts = head.docs_read(docs, &fields, Text, &mut arena).unwrap();
        assert_eq!(text(&texts, 0), Some("1852"));
        assert_eq!(text(&texts, 1), Some("1.65"));
        assert_eq!(text(&texts, 2), Some("true"));
        assert_eq!(text(&texts, 3), Some("Countess of Lovelace"));
        assert_eq!(text(&texts, 4), Some("Byron"));
        assert_eq!(text(&texts, 5), Some("Commodore"));
        assert_eq!(text(&texts, 6), Some("French"));

        let json = head
            .docs_read(
                docs,
                &[FieldAddress::new(key(1), "title"), FieldAddress::new(key(1), "family")],
                Json,
                &mut arena,
            )
            .unwrap();
        assert_eq!(text(&json, 0), Some(r#""Countess of Lovelace""#));
        assert_eq!(text(&json, 1), Some(r#"{"father":"Byron"}"#));
    }

    #[test]
    fn fields_are_removed_by_merging_a_null() {
        let (database, docs, mut arena) = populated();
        let removal = [DocumentEntry::<Json>::new(key(1), r#"{"born":null}"#)];
        database
            .head()
            .docs_write(docs, &removal, Modification::Merge, Durability::Buffered)
            .unwrap();
        let born = database
            .head()
            .docs_read(docs, &[FieldAddress::new(key(1), "born")], Text, &mut arena)
            .unwrap();
        assert_eq!(born.get(0), None);
    }

    #[test]
    fn fields_refuse_a_document_or_a_path_that_is_not_there() {
        let (database, docs, _) = populated();
        let orphan = [FieldEntry::<Text>::new(key(42), "name", "Nobody")];
        let failure = database
            .head()
            .docs_write(docs, &orphan, Modification::Upsert, Durability::Buffered)
            .unwrap_err();
        assert_eq!(failure.status, Status::NotFound);

        let deep = [FieldEntry::<Text>::new(key(1), "/missing/deeper", "value")];
        let failure = database
            .head()
            .docs_write(docs, &deep, Modification::Update, Durability::Buffered)
            .unwrap_err();
        assert_eq!(failure.status, Status::NotFound);
    }

    #[test]
    fn reads_report_missing_documents_and_fields_as_absent() {
        let (database, docs, mut arena) = populated();
        let fields = [
            FieldAddress::new(key(1), "name"),
            FieldAddress::new(key(1), "nowhere"),
            FieldAddress::new(key(42), "name"),
        ];
        let read = database.head().docs_read(docs, &fields, Json, &mut arena).unwrap();
        assert_eq!(text(&read, 0), Some(r#""Ada""#));
        assert_eq!(read.get(1), None);
        assert_eq!(read.get(2), None);
    }

    #[test]
    fn a_collection_of_values_refuses_the_document_verbs() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new().unwrap();
        let values = created(&database, Modality::Blobs);
        let blob = [Entry::new(key(1), b"not a document")];
        database.head().write(values, &blob, Durability::Buffered).unwrap();
        let failure = database
            .head()
            .docs_read(values, &[FieldAddress::new(key(1), "name")], Text, &mut arena)
            .unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);
        let documents = [DocumentEntry::<Json>::new(key(1), "{}")];
        let failure = database
            .head()
            .docs_write(values, &documents, Modification::Upsert, Durability::Buffered)
            .unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);
    }

    #[test]
    fn gists_list_every_leaf_once() {
        let (database, docs, mut arena) = populated();
        let paths = database
            .head()
            .docs_gist(docs, keys([1, 2, 1, 42]), &mut arena)
            .unwrap();
        let mut listed: Vec<&str> = paths.iter().collect();
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
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let docs = created(&database, Modality::Docs);
        let mut arena = Arena::new().unwrap();
        let document = format!("{{\"{}\":1}}", "k".repeat(600));
        let documents = [DocumentEntry::<Json>::new(key(1), &document)];
        database
            .head()
            .docs_write(docs, &documents, Modification::Upsert, Durability::Buffered)
            .unwrap();
        let failure = database.head().docs_gist(docs, keys([1]), &mut arena).unwrap_err();
        assert_eq!(failure.status, Status::CapacityExhausted);
    }

    #[test]
    fn gathered_columns_export_in_place() {
        let (database, docs, mut arena) = populated();
        let columns = [Column::new::<u64>("born"), Column::new::<Binary>("name")];
        let gathered = database
            .head()
            .docs_gather(docs, &keys([1, 2, 42]), &columns, &mut arena)
            .unwrap();
        let exported = gathered.to_arrow().unwrap();
        assert_eq!(exported.len(), 2);
        let born = exported[0].array();
        assert_eq!((born.device_type, born.array.length), (ArrowDeviceArray::CPU, 3));
        // SAFETY: a scalar column on the host holds validities and values.
        let values = unsafe { *born.array.buffers.add(1) };
        assert_eq!(values.cast(), gathered.scalars::<u64>(0).unwrap().as_ptr());
        let validities = unsafe { *born.array.buffers };
        assert_eq!(
            validities.cast(),
            gathered.validities(0).unwrap().unwrap().as_bytes().as_ptr()
        );
    }

    #[test]
    fn gathered_text_exports_without_terminators() {
        let (database, docs, mut arena) = populated();
        let columns = [Column::new::<Text>("name")];
        let gathered = database
            .head()
            .docs_gather(docs, &keys([1, 2]), &columns, &mut arena)
            .unwrap();
        let exported = gathered.to_arrow().unwrap();
        let (schema, array) = (exported[0].schema(), exported[0].array());
        // SAFETY: a string column on the host holds validities, offsets and bytes, as its schema
        // says.
        unsafe {
            assert_eq!(CStr::from_ptr(schema.format), c"u");
            let offsets = *array.array.buffers.add(1) as *const u32;
            let bytes = *array.array.buffers.add(2) as *const u8;
            let first = slice::from_raw_parts(bytes, *offsets.add(1) as usize);
            assert_eq!(Some(first), gathered.strings(0).unwrap().get(0));
        }
    }

    #[test]
    fn gathers_convert_fields_into_typed_columns() {
        let (database, docs, mut arena) = populated();
        let columns = [
            Column::new::<u64>("born"),
            Column::new::<f32>("born"),
            Column::new::<Text>("name"),
            Column::new::<Binary>("/navy/rank"),
            Column::new::<bool>("navy"),
            Column::new::<u8>("/languages/0"),
        ];
        let gathered = database
            .head()
            .docs_gather(docs, &keys([1, 2, 42]), &columns, &mut arena)
            .unwrap();
        assert_eq!((gathered.rows(), gathered.columns()), (3, 6));

        let born = gathered.scalars::<u64>(0).unwrap();
        assert_eq!(&born[..2], &[1815, 1906]);
        let validities = gathered.validities(0).unwrap().unwrap();
        assert_eq!(
            (validities.get(0), validities.get(1), validities.get(2)),
            (true, true, false)
        );
        let conversions = gathered.conversions(0).unwrap().unwrap();
        assert!(!conversions.get(0), "a positive integer is unsigned as it stands");
        assert!(conversions.get(1), "a string parsed into an integer is a conversion");
        assert_eq!(validities.len(), 3);
        assert_eq!(validities.as_bytes().len(), 1);

        assert_eq!(&gathered.scalars::<f32>(1).unwrap()[..2], &[1815.0, 1906.0]);

        let names = gathered.strings(2).unwrap();
        assert_eq!(names.len(), 3);
        assert_eq!(names.get(0), Some(b"Ada".as_slice()));
        assert_eq!(names.get(1), Some(b"Grace".as_slice()));
        assert_eq!(names.get(2), None);
        assert_eq!(names.iter().filter(Option::is_some).count(), 2);

        let ranks = gathered.strings(3).unwrap();
        assert_eq!(ranks.get(0), None);
        assert_eq!(ranks.get(1), Some(b"Rear Admiral".as_slice()));

        // An object cannot become a boolean, nor a word a byte.
        let navy = gathered.collisions(4).unwrap().unwrap();
        assert!(!navy.get(0) && navy.get(1));
        assert!(!gathered.validities(4).unwrap().unwrap().get(1));
        assert!(gathered.collisions(5).unwrap().unwrap().get(0));
        assert_eq!(gathered.scalars::<bool>(4).unwrap().len(), 3);
    }

    #[test]
    fn columns_refuse_to_be_read_as_something_else() {
        let (database, docs, mut arena) = populated();
        let columns = [Column::new::<i64>("born"), Column::new::<Text>("name")];
        let gathered = database
            .head()
            .docs_gather(docs, &keys([1]), &columns, &mut arena)
            .unwrap();
        assert_eq!(gathered.scalars::<u64>(0).unwrap_err().status, Status::InvalidArgument);
        assert_eq!(gathered.scalars::<i64>(1).unwrap_err().status, Status::InvalidArgument);
        assert_eq!(gathered.strings(0).unwrap_err().status, Status::InvalidArgument);
        assert_eq!(gathered.scalars::<i64>(2).unwrap_err().status, Status::InvalidArgument);
        assert!(gathered.validities(2).unwrap().is_none());
    }

    #[test]
    fn gist_paths_address_the_fields_a_gather_reads() {
        let (database, docs, mut paths_arena) = populated();
        let mut columns_arena = Arena::new().unwrap();
        let paths = database.head().docs_gist(docs, keys([1]), &mut paths_arena).unwrap();
        let columns: Vec<Column<'_>> = paths.iter().map(Column::new::<Text>).collect();
        let gathered = database
            .head()
            .docs_gather(docs, &keys([1]), &columns, &mut columns_arena)
            .unwrap();
        assert_eq!(gathered.columns(), 4);
        for column in 0..gathered.columns() {
            assert!(gathered.validities(column).unwrap().unwrap().get(0));
        }
    }

    #[test]
    fn whole_documents_are_written_as_scalars_and_strings() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let docs = created(&database, Modality::Docs);
        let mut arena = Arena::new().unwrap();
        let head = database.head();
        let upsert = Modification::Upsert;
        let buffered = Durability::Buffered;

        let born = [DocumentEntry::<i64>::new(key(1), &1815)];
        head.docs_write(docs, &born, upsert, buffered).unwrap();
        let height = [DocumentEntry::<f64>::new(key(2), &1.65)];
        head.docs_write(docs, &height, upsert, buffered).unwrap();
        let countess = [DocumentEntry::<bool>::new(key(3), &true)];
        head.docs_write(docs, &countess, upsert, buffered).unwrap();
        let title = [DocumentEntry::<Text>::new(key(4), "Countess of Lovelace")];
        head.docs_write(docs, &title, upsert, buffered).unwrap();

        // A document that is one scalar reads back as the JSON that scalar prints as.
        let documents = head.docs_read(docs, keys([1, 2, 3, 4]), Json, &mut arena).unwrap();
        assert_eq!(text(&documents, 0), Some("1815"));
        assert_eq!(text(&documents, 1), Some("1.65"));
        assert_eq!(text(&documents, 2), Some("true"));
        assert_eq!(text(&documents, 3), Some(r#""Countess of Lovelace""#));
    }

    #[test]
    fn one_batch_of_documents_spans_several_collections() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new().unwrap();
        let people = database.collection_create("people", Modality::Docs).unwrap();
        let places = database.collection_create("places", Modality::Docs).unwrap();
        let spanned = [people, places];

        let documents = [
            DocumentEntry::<Json>::new(key(1), r#"{"name":"Ada"}"#),
            DocumentEntry::<Json>::new(key(1), r#"{"name":"Kent"}"#),
        ];
        database
            .head()
            .docs_write(
                spanned.as_slice(),
                &documents,
                Modification::Upsert,
                Durability::Buffered,
            )
            .unwrap();

        let fields = [FieldAddress::new(key(1), "name"), FieldAddress::new(key(1), "name")];
        let names = database
            .head()
            .docs_read(Collections::Each(&spanned), &fields, Text, &mut arena)
            .unwrap();
        assert_eq!((text(&names, 0), text(&names, 1)), (Some("Ada"), Some("Kent")));

        let columns = [Column::new::<Text>("name")];
        let gathered = database
            .head()
            .docs_gather(Collections::Each(&spanned), &keys([1, 1]), &columns, &mut arena)
            .unwrap();
        assert_eq!(gathered.strings(0).unwrap().get(1), Some(b"Kent".as_slice()));

        // A run of collections that is not one per task is refused before the call.
        let failure = database
            .head()
            .docs_gather(Collections::Each(&spanned), &keys([1]), &columns, &mut arena)
            .unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);
    }

    #[test]
    fn gathers_fill_halves_and_uuids() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let docs = created(&database, Modality::Docs);
        let mut arena = Arena::new().unwrap();
        let documents = [
            DocumentEntry::<Json>::new(
                key(1),
                r#"{"weight":1.5,"id":"123e4567-e89b-12d3-a456-426614174000","huge":70000.0}"#,
            ),
            DocumentEntry::<Json>::new(key(2), r#"{"weight":0.0,"id":"not a uuid","huge":1.0}"#),
        ];
        database
            .head()
            .docs_write(docs, &documents, Modification::Upsert, Durability::Buffered)
            .unwrap();

        let columns = [
            Column::new::<Half>("weight"),
            Column::new::<UUID>("id"),
            Column::new::<Half>("huge"),
        ];
        let gathered = database
            .head()
            .docs_gather(docs, &keys([1, 2]), &columns, &mut arena)
            .unwrap();

        // A half is two bytes the caller reinterprets; 1.5 is sign zero, exponent fifteen, mantissa one half.
        let weights = gathered.scalars::<Half>(0).unwrap();
        assert_eq!(weights, &[Half(0x3e00), Half(0)]);
        assert!(gathered.validities(0).unwrap().unwrap().get(0) && gathered.validities(0).unwrap().unwrap().get(1));

        let identifiers = gathered.scalars::<UUID>(1).unwrap();
        assert_eq!(
            identifiers[0],
            UUID([0x12, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x12, 0xd3, 0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x00])
        );
        assert_eq!(
            identifiers[1],
            UUID::default(),
            "text that is no UUID leaves its cell zero"
        );
        assert!(gathered.collisions(1).unwrap().unwrap().get(1));

        // A magnitude past the range of a half collides rather than rounding to infinity.
        assert!(gathered.collisions(2).unwrap().unwrap().get(0));
        assert_eq!(gathered.scalars::<Half>(2).unwrap()[0], Half(0));
        assert_eq!(
            gathered.scalars::<f32>(0).unwrap_err().status,
            Status::InvalidArgument,
            "a half column is not a float column"
        );
    }

    #[test]
    fn empty_batches_reach_no_engine() {
        let (database, docs, mut arena) = populated();
        let head = database.head();
        let (upsert, buffered) = (Modification::Upsert, Durability::Buffered);
        let no_documents: &[DocumentEntry<'_, Json>] = &[];
        head.docs_write(docs, no_documents, upsert, buffered).unwrap();
        let no_texts: &[Document<'_>] = &[];
        head.docs_write(docs, KeyedBy("_id", no_texts), upsert, buffered)
            .unwrap();
        let no_fields: &[FieldEntry<'_, Json>] = &[];
        head.docs_write(docs, no_fields, upsert, buffered).unwrap();
        assert!(head.docs_read(docs, [], Json, &mut arena).unwrap().is_empty());
        let no_addresses: &[FieldAddress<'_>] = &[];
        assert!(head.docs_read(docs, no_addresses, Text, &mut arena).unwrap().is_empty());
        assert!(head.docs_gist(docs, [], &mut arena).unwrap().is_empty());

        let columns = [Column::new::<i64>("born"), Column::new::<Text>("name")];
        let gathered = head.docs_gather(docs, &[], &columns, &mut arena).unwrap();
        assert_eq!(gathered.rows(), 0);
        assert!(gathered.scalars::<i64>(0).unwrap().is_empty());
        assert!(gathered.strings(1).unwrap().is_empty());
        assert!(gathered.validities(0).unwrap().unwrap().is_empty());
        assert_eq!(
            head.docs_gather(docs, &keys([1]), &[], &mut arena).unwrap().columns(),
            0
        );
    }

    #[test]
    fn transactions_hold_their_field_writes_until_commit() {
        let (database, docs, mut arena) = populated();
        let mut transaction = database.transaction().unwrap();
        let rank = [FieldEntry::<Text>::new(key(2), "/navy/rank", "Commodore")];
        transaction
            .view(Watch::Keys)
            .docs_write(docs, &rank, Modification::Update, Durability::Buffered)
            .unwrap();
        let flushed = transaction
            .view(Watch::Keys)
            .docs_write(docs, &rank, Modification::Update, Durability::Flushed)
            .unwrap_err();
        assert_eq!(flushed.status, Status::InvalidArgument);

        let address = [FieldAddress::new(key(2), "/navy/rank")];
        let inside = transaction
            .view(Watch::Keys)
            .docs_read(docs, &address, Text, &mut arena)
            .unwrap();
        assert_eq!(text(&inside, 0), Some("Commodore"));
        let outside = database.head().docs_read(docs, &address, Text, &mut arena).unwrap();
        assert_eq!(text(&outside, 0), Some("Rear Admiral"));

        transaction.commit(Durability::Buffered).unwrap();
        let committed = database.head().docs_read(docs, &address, Text, &mut arena).unwrap();
        assert_eq!(text(&committed, 0), Some("Commodore"));
    }

    #[test]
    fn documents_and_gists_block_or_defer_on_an_executor() {
        let (database, docs, _) = populated();
        let executor = Executor::for_ram(&[], 2, 0).unwrap();
        let mut arena = Arena::new_in(&executor).unwrap();
        let blocking = database.head().on(&executor);
        let deferred = database.head().deferred(&executor);

        let read = blocking.docs_read(docs, keys([1]), Json, &mut arena).unwrap();
        assert!(text(&read, 0).unwrap().starts_with(r#"{"name":"Ada""#));
        let read = deferred
            .docs_read(docs, vec![key(2), key(3)], Json, &mut arena)
            .unwrap()
            .wait()
            .unwrap();
        assert!(text(&read, 0).unwrap().starts_with(r#"{"name":"Grace""#));
        assert_eq!(read.get(1), None);

        assert_eq!(blocking.docs_gist(docs, keys([1]), &mut arena).unwrap().len(), 4);
        let paths = deferred
            .docs_gist(docs, keys([1, 2]), &mut arena)
            .unwrap()
            .wait()
            .unwrap();
        assert_eq!(paths.len(), 5);
    }

    #[test]
    fn whole_documents_write_through_a_deferred_view() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let docs = created(&database, Modality::Docs);
        let executor = Executor::for_ram(&[], 1, 0).unwrap();
        let deferred = database.head().deferred(&executor);
        let (upsert, buffered) = (Modification::Upsert, Durability::Buffered);

        let texts: CharsTape<u64> = [r#"{"name":"Ada"}"#, r#"{"name":"Grace"}"#].into_iter().collect();
        let writing = deferred.docs_write(docs, (vec![key(1), key(2)], texts), upsert, buffered);
        let (written, _) = writing.unwrap().wait().unwrap();
        assert_eq!(written, keys([1, 2]), "a write hands its inputs back");

        let mut arena = Arena::new().unwrap();
        let documents = database.head().docs_read(docs, keys([2, 1]), Json, &mut arena).unwrap();
        assert_eq!(text(&documents, 0), Some(r#"{"name":"Grace"}"#));
        assert_eq!(text(&documents, 1), Some(r#"{"name":"Ada"}"#));

        let texts: CharsTape<u64> = ["{}"].into_iter().collect();
        let refused = deferred.docs_write(docs, (keys([1, 2]), texts), upsert, buffered);
        assert_eq!(
            refused.unwrap_err().status,
            Status::InvalidArgument,
            "the tape holds a value per key"
        );
    }

    #[test]
    fn documents_by_id_write_through_a_deferred_view() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let docs = created(&database, Modality::Docs);
        let executor = Executor::for_ram(&[], 1, 0).unwrap();
        let texts: CharsTape<u64> = [r#"{"_id":7,"name":"Ada"}"#].into_iter().collect();
        database
            .head()
            .deferred(&executor)
            .docs_write(
                docs,
                KeyedBy(String::from("_id"), texts),
                Modification::Upsert,
                Durability::Buffered,
            )
            .unwrap()
            .wait()
            .unwrap();

        let mut arena = Arena::new().unwrap();
        let names = database
            .head()
            .docs_read(docs, &[FieldAddress::new(key(7), "name")], Text, &mut arena)
            .unwrap();
        assert_eq!(text(&names, 0), Some("Ada"));
    }

    #[test]
    fn fields_write_through_a_deferred_view() {
        let (database, docs, mut arena) = populated();
        let executor = Executor::for_ram(&[], 1, 0).unwrap();
        let paths: CharsTape<u32> = ["died", "/navy/rank"].into_iter().collect();
        let values: CharsTape<u64> = ["1852", r#""Commodore""#].into_iter().collect();
        database
            .head()
            .deferred(&executor)
            .docs_write(
                docs,
                (keys([1, 2]), paths, values),
                Modification::Upsert,
                Durability::Buffered,
            )
            .unwrap()
            .wait()
            .unwrap();

        let fields = [
            FieldAddress::new(key(1), "died"),
            FieldAddress::new(key(2), "/navy/rank"),
        ];
        let read = database.head().docs_read(docs, &fields, Text, &mut arena).unwrap();
        assert_eq!((text(&read, 0), text(&read, 1)), (Some("1852"), Some("Commodore")));
    }

    #[test]
    fn fields_read_through_a_deferred_view() {
        let (database, docs, mut arena) = populated();
        let executor = Executor::for_ram(&[], 1, 0).unwrap();
        let deferred = database.head().deferred(&executor);

        let paths: CharsTape<u64> = ["name", "/navy/rank"].into_iter().collect();
        let read = deferred
            .docs_read(docs, (vec![key(1), key(2)], paths), Text, &mut arena)
            .unwrap()
            .wait()
            .unwrap();
        assert_eq!(text(&read, 0), Some("Ada"));
        assert_eq!(text(&read, 1), Some("Rear Admiral"));

        let refused = deferred.docs_read(docs, (keys([1]), CharsTape::<u64>::default()), Json, &mut arena);
        assert_eq!(refused.unwrap_err().status, Status::InvalidArgument, "a path per key");
    }

    #[test]
    fn gathers_run_through_a_deferred_view() {
        let (database, docs, mut arena) = populated();
        let executor = Executor::for_ram(&[], 1, 0).unwrap();
        let columns = vec![Column::new::<u64>("born"), Column::new::<Text>(String::from("name"))];
        let gathered = database
            .head()
            .deferred(&executor)
            .docs_gather(docs, keys([1, 2]), columns, &mut arena)
            .unwrap()
            .wait()
            .unwrap();
        assert_eq!(gathered.rows(), 2);
        assert_eq!(gathered.scalars::<u64>(0).unwrap(), &[1815, 1906]);
        assert_eq!(gathered.strings(1).unwrap().get(1), Some(b"Grace".as_slice()));
    }

    #[test]
    fn scalars_cross_as_their_native_bytes() {
        assert_eq!(<i64 as FieldFormat>::bytes(&-2), &(-2i64).to_ne_bytes());
        assert_eq!(<f32 as FieldFormat>::bytes(&1.5), &1.5f32.to_ne_bytes());
        assert_eq!(<bool as FieldFormat>::bytes(&true), &[1]);
        assert_eq!(<Json as FieldFormat>::bytes("[]"), b"[]");
    }
}

// endregion: Tests

// region: Cursors

/// A walk over the keys of one documents collection, which every pull resumes past the last key the pull before it handed out, as a
/// [`crate::Cursor`] walks blobs, keeping the keys its [`Matching`] admits.
///
/// Mirrors `ustore_docs_cursor_t`; dropping it calls `ustore_docs_cursor_free`.
#[derive(Debug)]
pub struct DocsCursor<'view> {
    handle: ffi::ustore_docs_cursor_t,
    database: ustore_database_t,
    executor: ustore_executor_t,
    matched: Box<Matched<'view>>,
    marker: PhantomData<&'view ()>,
}

impl<'view, Parent> On<'view, Parent> {
    /// Opens a walk over the keys of one documents collection's `range`, compiling what `matching`
    /// names once, whose predicate lives as long as the cursor.
    ///
    /// Calls `ustore_docs_cursor_init`.
    pub fn docs_cursor(
        &self,
        collection: CollectionID,
        range: impl Into<KeyRange>,
        config: impl Into<DocsCursorConfig<'view>>,
    ) -> Result<DocsCursor<'view>> {
        let range: KeyRange = range.into();
        let mut cursor = DocsCursor {
            handle: ptr::null_mut(),
            database: self.database(),
            executor: self.executor(),
            matched: Box::new(Matched::new(config.into().matching)),
            marker: PhantomData,
        };
        let mut outcome = ustore_outcome_t::default();
        let mut arguments = ffi::ustore_docs_cursor_init_t {
            cursor: &mut cursor.handle,
            database: self.database(),
            executor: self.executor(),
            outcome: &mut outcome,
            view: self.view(),
            collection,
            start_key: range.start_key(),
            end_key: range.end.as_ref().map_or(ptr::null(), ptr::from_ref),
            direction: range.direction,
            filter: cursor.matched.filter(),
            predicate: cursor.matched.predicate(),
        };
        // SAFETY: every pointer in `arguments` outlives the call, which is final on return.
        unsafe { ffi::ustore_docs_cursor_init(&mut arguments) };
        check(outcome).map(|()| cursor)
    }
}

impl DocsCursor<'_> {
    /// Pulls up to `wanted` next keys into `arena`, fewer once the walk nears its end and none past it.
    ///
    /// Calls `ustore_docs_cursor_next`.
    pub fn next<'arena>(&mut self, wanted: usize, arena: &'arena mut Arena) -> Result<&'arena [Key]> {
        self.pull(wanted, arena, ptr::null_mut())
    }

    /// Pulls up to `wanted` next keys with their whole documents into `arena` in one call.
    ///
    /// Calls `ustore_docs_cursor_next`.
    pub fn next_pairs<'arena>(&mut self, wanted: usize, arena: &'arena mut Arena) -> Result<Page<'arena>> {
        let mut values = ustore_strings_view_t::default();
        let keys = self.pull(wanted, arena, &mut values)?;
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
    ) -> Result<&'arena [Key]> {
        let mut keys: *const ustore_key_t = ptr::null();
        let mut written: ustore_size_t = 0;
        let mut outcome = ustore_outcome_t::default();
        let mut arguments = ffi::ustore_docs_cursor_next_t {
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
        unsafe { ffi::ustore_docs_cursor_next(&mut arguments) };
        self.matched.raise();
        check(outcome)?;
        arena.host()?;
        // SAFETY: a successful pull wrote `written` keys into the arena, which stays borrowed for `'arena`.
        Ok(unsafe { exported_slice(keys, written as usize) })
    }
}

impl Drop for DocsCursor<'_> {
    fn drop(&mut self) {
        // SAFETY: the handle came from a successful init, or is the null a failed one leaves, which the free ignores.
        unsafe { ffi::ustore_docs_cursor_free(self.handle) };
    }
}

// endregion: Cursors
