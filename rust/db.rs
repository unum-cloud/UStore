//! Databases, views, transactions, snapshots and arenas - the Rust twin of `include/ustore/db.h`.
//!
//! Every call addresses exactly one view of the data: the head of the database, a transaction, or a
//! snapshot. [`View`] is that view, so the pairing the C ABI refuses cannot be spelled here. Results
//! land in a caller-owned [`Arena`] that the next call reuses, which is why a call borrows it mutably
//! and every exported result borrows it back.

use std::ffi::{c_char, CStr, CString};
use std::fmt;
use std::marker::PhantomData;
use std::ptr;

// region: Raw Declarations

/// One-to-one mirror of `include/ustore/db.h`.
pub(crate) mod ffi {
    #![allow(non_camel_case_types)]

    use std::ffi::{c_char, c_int, c_void};

    pub type ustore_database_t = *mut c_void;
    pub type ustore_transaction_t = *mut c_void;
    pub type ustore_arena_t = *mut c_void;
    pub type ustore_snapshot_t = u64;
    pub type ustore_collection_t = u64;
    pub type ustore_key_t = i64;
    pub type ustore_byte_t = u8;
    pub type ustore_char_t = c_char;
    pub type ustore_length_t = u32;
    pub type ustore_size_t = u64;
    pub type ustore_octet_t = u8;
    pub type ustore_sequence_number_t = u64;
    pub type ustore_str_view_t = *const c_char;
    pub type ustore_bytes_cptr_t = *const u8;

    /// The `ustore_status_t`, `ustore_options_t`, `ustore_drop_mode_t` and `ustore_metadata_t`
    /// enumerations, each of which lowers to the platform `int`.
    pub type ustore_enum_t = c_int;

    pub const USTORE_OPTIONS_DEFAULT_K: ustore_enum_t = 0;
    pub const USTORE_OPTION_WRITE_FLUSH_K: ustore_enum_t = 1 << 1;

    pub const USTORE_SUPPORTS_TRANSACTIONS_K: ustore_enum_t = 1;
    pub const USTORE_SUPPORTS_COLLECTIONS_K: ustore_enum_t = 2;
    pub const USTORE_SUPPORTS_SNAPSHOTS_K: ustore_enum_t = 4;

    /// The outcome of one call: a status, and a static message that is null exactly on success.
    #[repr(C)]
    #[derive(Clone, Copy)]
    pub struct ustore_error_t {
        pub status: ustore_enum_t,
        pub message: *const c_char,
    }

    impl Default for ustore_error_t {
        fn default() -> Self {
            ustore_error_t {
                status: 0,
                message: core::ptr::null(),
            }
        }
    }

    #[repr(C)]
    pub struct ustore_database_init_t {
        pub config: ustore_str_view_t,
        pub db: *mut ustore_database_t,
        pub error: *mut ustore_error_t,
    }

    #[repr(C)]
    pub struct ustore_get_metadata_t {
        pub db: ustore_database_t,
        pub error: *mut ustore_error_t,
        pub metadata: *mut ustore_enum_t,
    }

    #[repr(C)]
    pub struct ustore_database_control_t {
        pub db: ustore_database_t,
        pub arena: *mut ustore_arena_t,
        pub error: *mut ustore_error_t,
        pub request: ustore_str_view_t,
        pub response: *mut ustore_str_view_t,
    }

    #[repr(C)]
    pub struct ustore_snapshot_list_t {
        pub db: ustore_database_t,
        pub error: *mut ustore_error_t,
        pub arena: *mut ustore_arena_t,
        pub options: ustore_enum_t,
        pub count: *mut ustore_size_t,
        pub ids: *mut *mut ustore_snapshot_t,
    }

    #[repr(C)]
    pub struct ustore_snapshot_create_t {
        pub db: ustore_database_t,
        pub error: *mut ustore_error_t,
        pub id: *mut ustore_snapshot_t,
    }

    #[repr(C)]
    pub struct ustore_snapshot_export_t {
        pub db: ustore_database_t,
        pub error: *mut ustore_error_t,
        pub id: ustore_snapshot_t,
        pub path: ustore_str_view_t,
    }

    #[repr(C)]
    pub struct ustore_snapshot_drop_t {
        pub db: ustore_database_t,
        pub error: *mut ustore_error_t,
        pub id: ustore_snapshot_t,
    }

    #[repr(C)]
    pub struct ustore_collection_list_t {
        pub db: ustore_database_t,
        pub error: *mut ustore_error_t,
        pub transaction: ustore_transaction_t,
        pub snapshot: ustore_snapshot_t,
        pub arena: *mut ustore_arena_t,
        pub options: ustore_enum_t,
        pub count: *mut ustore_size_t,
        pub ids: *mut *mut ustore_collection_t,
    }

    #[repr(C)]
    pub struct ustore_collection_create_t {
        pub db: ustore_database_t,
        pub error: *mut ustore_error_t,
        pub config: ustore_str_view_t,
        pub id: *mut ustore_collection_t,
    }

    #[repr(C)]
    pub struct ustore_collection_drop_t {
        pub db: ustore_database_t,
        pub error: *mut ustore_error_t,
        pub id: ustore_collection_t,
        pub mode: ustore_enum_t,
    }

    #[repr(C)]
    pub struct ustore_transaction_init_t {
        pub db: ustore_database_t,
        pub error: *mut ustore_error_t,
        pub options: ustore_enum_t,
        pub transaction: *mut ustore_transaction_t,
    }

    #[repr(C)]
    pub struct ustore_transaction_stage_t {
        pub db: ustore_database_t,
        pub error: *mut ustore_error_t,
        pub transaction: ustore_transaction_t,
        pub options: ustore_enum_t,
        pub sequence_number: *mut ustore_sequence_number_t,
    }

    #[repr(C)]
    pub struct ustore_transaction_commit_t {
        pub db: ustore_database_t,
        pub error: *mut ustore_error_t,
        pub transaction: ustore_transaction_t,
        pub options: ustore_enum_t,
        pub sequence_number: *mut ustore_sequence_number_t,
    }

    extern "C" {
        pub static ustore_collection_main_k: ustore_collection_t;
        pub static ustore_collection_names_k: ustore_collection_t;
        pub static ustore_length_missing_k: ustore_length_t;
        pub static ustore_key_unknown_k: ustore_key_t;

        pub fn ustore_database_init(arguments: *mut ustore_database_init_t);
        pub fn ustore_get_metadata(arguments: *mut ustore_get_metadata_t);
        pub fn ustore_database_control(arguments: *mut ustore_database_control_t);

        pub fn ustore_snapshot_list(arguments: *mut ustore_snapshot_list_t);
        pub fn ustore_snapshot_create(arguments: *mut ustore_snapshot_create_t);
        pub fn ustore_snapshot_export(arguments: *mut ustore_snapshot_export_t);
        pub fn ustore_snapshot_drop(arguments: *mut ustore_snapshot_drop_t);

        pub fn ustore_collection_list(arguments: *mut ustore_collection_list_t);
        pub fn ustore_collection_create(arguments: *mut ustore_collection_create_t);
        pub fn ustore_collection_drop(arguments: *mut ustore_collection_drop_t);

        pub fn ustore_transaction_init(arguments: *mut ustore_transaction_init_t);
        pub fn ustore_transaction_stage(arguments: *mut ustore_transaction_stage_t);
        pub fn ustore_transaction_commit(arguments: *mut ustore_transaction_commit_t);

        pub fn ustore_arena_free(arena: ustore_arena_t);
        pub fn ustore_transaction_free(transaction: ustore_transaction_t);
        pub fn ustore_database_free(database: ustore_database_t);
    }
}

// endregion: Raw Declarations

// region: Errors

/// What went wrong, in the vocabulary of `ustore_status_t`.
#[repr(i32)]
#[derive(Clone, Copy, PartialEq, Eq, Debug, Hash)]
#[non_exhaustive]
pub enum Status {
    /// An allocation could not be satisfied.
    OutOfMemory = 1,
    /// An argument is null, contradicts another one, or carries unaccepted options.
    InvalidArgument = 2,
    /// The call needs a database or transaction that was never opened.
    Uninitialized = 3,
    /// The engine or build does not implement what the call asks for.
    Unsupported = 4,
    /// A file or directory could not be opened, read, written, or closed.
    Io = 5,
    /// Stored or supplied data could not be decoded.
    Malformed = 6,
    /// A snapshot, collection, or document the call addresses does not exist.
    NotFound = 7,
    /// A snapshot, collection, or document the call creates already exists.
    AlreadyExists = 8,
    /// A concurrent writer changed a key this transaction wrote.
    WriteConflict = 9,
    /// A concurrent writer changed a key this transaction read.
    ReadConflict = 10,
    /// A concurrent writer inserted into a range this transaction scanned.
    PhantomConflict = 11,
    /// A hard design ceiling was reached.
    CapacityExhausted = 12,
    /// A device could not be selected, or its memory could not be claimed.
    Device = 13,
    /// The engine failed in a way no other status names.
    Internal = 14,
    /// A status a newer library reported and this binding has no name for.
    Unrecognized = -1,
}

impl Status {
    fn from_raw(code: ffi::ustore_enum_t) -> Self {
        match code {
            1 => Status::OutOfMemory,
            2 => Status::InvalidArgument,
            3 => Status::Uninitialized,
            4 => Status::Unsupported,
            5 => Status::Io,
            6 => Status::Malformed,
            7 => Status::NotFound,
            8 => Status::AlreadyExists,
            9 => Status::WriteConflict,
            10 => Status::ReadConflict,
            11 => Status::PhantomConflict,
            12 => Status::CapacityExhausted,
            13 => Status::Device,
            14 => Status::Internal,
            _ => Status::Unrecognized,
        }
    }
}

/// A failure the library reported, or the binding caught before the call, with a static message so
/// nothing allocates on the failure path.
#[derive(Clone, Copy, PartialEq, Eq)]
#[non_exhaustive]
pub struct Error {
    /// The identity of the failure, shared with the C ABI.
    pub status: Status,
    /// The detail the status alone cannot carry.
    pub message: &'static str,
}

impl fmt::Display for Error {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.message)
    }
}

impl fmt::Debug for Error {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "Error({:?}, {:?})", self.status, self.message)
    }
}

impl std::error::Error for Error {}

/// The result of every fallible call in this crate.
pub type Result<Value> = std::result::Result<Value, Error>;

/// Turns the error a call wrote into a `Result`.
pub(crate) fn check(error: ffi::ustore_error_t) -> Result<()> {
    if error.status == 0 {
        return Ok(());
    }
    Err(Error {
        status: Status::from_raw(error.status),
        // SAFETY: a failed call writes a static null-terminated message, or null.
        message: unsafe { static_string(error.message) },
    })
}

/// Reports a failure the binding caught before the call crossed the boundary, under the status the
/// engine would have used had the ABI let it see the same thing.
pub(crate) fn fail(status: Status, message: &'static str) -> Error {
    Error { status, message }
}

/// Borrows a static null-terminated string the library owns.
unsafe fn static_string(raw: *const c_char) -> &'static str {
    if raw.is_null() {
        return "";
    }
    CStr::from_ptr(raw).to_str().unwrap_or("")
}

// endregion: Errors

// region: Handles and Constants

/// The identifier of a value within one collection.
pub type Key = ffi::ustore_key_t;

/// The order in which transactions applied, as a commit or a stage reports it.
pub type SequenceNumber = ffi::ustore_sequence_number_t;

/// An identifier of a collection, unique within its database but not stable across restarts.
#[repr(transparent)]
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub struct Collection(pub ffi::ustore_collection_t);

impl Collection {
    /// The collection every database holds, which may be cleared but never dropped.
    pub fn main() -> Self {
        // SAFETY: the library defines the constant once and never writes it.
        Collection(unsafe { ffi::ustore_collection_main_k })
    }

    /// The reserved collection the strings modality maps names onto identifiers in.
    pub(crate) fn names() -> Self {
        // SAFETY: the library defines the constant once and never writes it.
        Collection(unsafe { ffi::ustore_collection_names_k })
    }
}

/// An identifier of a point-in-time view of the whole database.
#[repr(transparent)]
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub struct SnapshotId(pub ffi::ustore_snapshot_t);

/// The length the ABI reports for a missing value.
pub(crate) fn missing_length() -> ffi::ustore_length_t {
    // SAFETY: the library defines the constant once and never writes it.
    unsafe { ffi::ustore_length_missing_k }
}

/// The key that stands for no key.
pub fn key_unknown() -> Key {
    // SAFETY: the library defines the constant once and never writes it.
    unsafe { ffi::ustore_key_unknown_k }
}

/// Whether a write reaches the drive before the call returns.
///
/// A transaction flushes at its commit, so `Flushed` on a transaction's write is refused.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub enum Durability {
    /// The engine decides when the bytes reach the drive.
    #[default]
    Buffered,
    /// The call returns once the bytes are on the drive, at a cost that varies by engine.
    Flushed,
}

impl Durability {
    pub(crate) fn to_raw(self) -> ffi::ustore_enum_t {
        match self {
            Durability::Buffered => ffi::USTORE_OPTIONS_DEFAULT_K,
            Durability::Flushed => ffi::USTORE_OPTION_WRITE_FLUSH_K,
        }
    }
}

/// What dropping a collection removes.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Hash)]
pub enum Removed {
    /// The collection handle together with all of its contents.
    CollectionAndEntries = 0,
    /// Every key and value, keeping the collection.
    Entries = 1,
    /// Every value, keeping the keys.
    Values = 2,
}

// endregion: Handles and Constants

// region: Arenas

/// Temporary memory the engine exports results into, which the next call through it reuses.
///
/// Every batch verb takes one mutably and hands back a result borrowing it, so the borrow checker
/// both keeps a result from outliving its arena and stops the call that would overwrite it.
#[derive(Debug)]
pub struct Arena {
    handle: ffi::ustore_arena_t,
}

impl Arena {
    /// An arena holding nothing, which the first call that uses it grows.
    pub fn new() -> Self {
        Arena {
            handle: ptr::null_mut(),
        }
    }

    /// Releases everything exported into the arena, keeping it usable.
    pub fn reset(&mut self) {
        // SAFETY: the handle came from the library, and null is explicitly safe to free.
        unsafe { ffi::ustore_arena_free(self.handle) };
        self.handle = ptr::null_mut();
    }

    /// The slot the calls grow in place.
    pub(crate) fn slot(&mut self) -> *mut ffi::ustore_arena_t {
        &mut self.handle
    }
}

impl Default for Arena {
    fn default() -> Self {
        Arena::new()
    }
}

impl Drop for Arena {
    fn drop(&mut self) {
        // SAFETY: the handle came from the library, is freed once, and null is safe to free.
        unsafe { ffi::ustore_arena_free(self.handle) };
    }
}

// SAFETY: an arena is caller-owned scratch the library never registers anywhere, so it may be
// handed to another thread outright; every verb needs it mutably, so two cannot share one.
unsafe impl Send for Arena {}

// endregion: Arenas

// region: Configuration

/// The isolation an engine delivers to the transactions of a database.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub enum Isolation {
    /// Reads and writes both take part in one serial order.
    #[default]
    StrictSerializable,
    /// A transaction reads the snapshot it began at and conflicts only on what it wrote.
    Snapshot,
}

impl Isolation {
    fn name(self) -> &'static str {
        match self {
            Isolation::StrictSerializable => "strict_serializable",
            Isolation::Snapshot => "snapshot",
        }
    }
}

/// What a database opens as: where it keeps its data, and what its transactions promise.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
#[non_exhaustive]
pub struct Configuration<'text> {
    /// The directory holding the data, which must exist; an empty one keeps the database in memory.
    pub directory: &'text str,
    /// The isolation the transactions of this database run at.
    pub isolation: Isolation,
}

impl<'text> Configuration<'text> {
    /// A database that lives in memory and persists nothing.
    pub fn in_memory() -> Self {
        Configuration {
            directory: "",
            isolation: Isolation::StrictSerializable,
        }
    }

    /// A database whose data lives in an existing directory.
    pub fn at(directory: &'text str) -> Self {
        Configuration {
            directory,
            isolation: Isolation::StrictSerializable,
        }
    }

    /// The same configuration at another isolation.
    pub fn with_isolation(self, isolation: Isolation) -> Self {
        Configuration { isolation, ..self }
    }

    /// The JSON the C ABI takes, which is the one allocation opening a database forces.
    fn to_json(self) -> Result<CString> {
        let mut text = String::from("{\"isolation\":\"");
        text.push_str(self.isolation.name());
        text.push('"');
        if !self.directory.is_empty() {
            text.push_str(",\"directory\":\"");
            for character in self.directory.chars() {
                match character {
                    '"' | '\\' => {
                        text.push('\\');
                        text.push(character);
                    }
                    control if control.is_control() => {
                        return Err(fail(Status::Malformed, "the directory holds a control character"))
                    }
                    plain => text.push(plain),
                }
            }
            text.push('"');
        }
        text.push('}');
        CString::new(text).map_err(|_| fail(Status::Malformed, "the directory holds a null character"))
    }
}

// endregion: Configuration

// region: Databases

/// What an engine supports, as `ustore_get_metadata` reports it.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Hash)]
pub struct Metadata {
    bits: ffi::ustore_enum_t,
}

impl Metadata {
    /// Whether the engine supports transactions.
    pub fn supports_transactions(self) -> bool {
        self.bits & ffi::USTORE_SUPPORTS_TRANSACTIONS_K != 0
    }

    /// Whether the engine supports collections beyond the main one.
    pub fn supports_collections(self) -> bool {
        self.bits & ffi::USTORE_SUPPORTS_COLLECTIONS_K != 0
    }

    /// Whether the engine supports snapshots.
    pub fn supports_snapshots(self) -> bool {
        self.bits & ffi::USTORE_SUPPORTS_SNAPSHOTS_K != 0
    }
}

/// An open embedded database, which owns its directory until it is dropped.
///
/// No two processes may open one directory at once, and the handle is shared across threads freely;
/// the transactions taken from it are not.
#[derive(Debug)]
pub struct Database {
    handle: ffi::ustore_database_t,
}

// SAFETY: `db.h` promises the handle is safe to share across threads from open until free, and
// nothing in this binding keeps per-handle state of its own.
unsafe impl Send for Database {}
unsafe impl Sync for Database {}

impl Database {
    /// Opens an embedded database, in memory or in the directory the configuration names.
    pub fn open(configuration: Configuration<'_>) -> Result<Self> {
        let config = configuration.to_json()?;
        let mut handle: ffi::ustore_database_t = ptr::null_mut();
        let mut error = ffi::ustore_error_t::default();
        let mut arguments = ffi::ustore_database_init_t {
            config: config.as_ptr(),
            db: &mut handle,
            error: &mut error,
        };
        // SAFETY: every pointer in `arguments` outlives the call, and the config stays alive with it.
        unsafe { ffi::ustore_database_init(&mut arguments) };
        check(error)?;
        match handle.is_null() {
            true => Err(fail(Status::Internal, "the library returned no database and no error")),
            false => Ok(Database { handle }),
        }
    }

    /// What the engine behind this database supports.
    pub fn metadata(&self) -> Result<Metadata> {
        let mut bits: ffi::ustore_enum_t = 0;
        let mut error = ffi::ustore_error_t::default();
        let mut arguments = ffi::ustore_get_metadata_t {
            db: self.handle,
            error: &mut error,
            metadata: &mut bits,
        };
        // SAFETY: every pointer in `arguments` outlives the call.
        unsafe { ffi::ustore_get_metadata(&mut arguments) };
        check(error)?;
        Ok(Metadata { bits })
    }

    /// Sends a free-form request to the engine - "clear", "reset", "compact", "info" or "usage" -
    /// and borrows its answer out of the arena.
    pub fn control<'arena>(&self, arena: &'arena mut Arena, request: &str) -> Result<&'arena str> {
        let request =
            CString::new(request).map_err(|_| fail(Status::Malformed, "the request holds a null character"))?;
        let mut response: ffi::ustore_str_view_t = ptr::null();
        let mut error = ffi::ustore_error_t::default();
        let mut arguments = ffi::ustore_database_control_t {
            db: self.handle,
            arena: arena.slot(),
            error: &mut error,
            request: request.as_ptr(),
            response: &mut response,
        };
        // SAFETY: every pointer in `arguments` outlives the call, the request included.
        unsafe { ffi::ustore_database_control(&mut arguments) };
        check(error)?;
        if response.is_null() {
            return Err(fail(
                Status::Internal,
                "the engine answered nothing and reported no error",
            ));
        }
        // SAFETY: a successful control writes a null-terminated answer into the arena, which stays
        // borrowed for as long as the returned text does.
        let answer = unsafe { CStr::from_ptr(response) };
        answer
            .to_str()
            .map_err(|_| fail(Status::Malformed, "the engine answered text that is not UTF-8"))
    }

    /// The head of the database, where writes land and reads see the latest committed state.
    pub fn head(&self) -> View<'_> {
        View {
            database: self.handle,
            transaction: ptr::null_mut(),
            snapshot: 0,
            marker: PhantomData,
        }
    }

    /// Begins a transaction over this database, which conflicts rather than blocks.
    pub fn transaction(&self) -> Result<Transaction<'_>> {
        let mut handle: ffi::ustore_transaction_t = ptr::null_mut();
        begin(self.handle, &mut handle)?;
        Ok(Transaction { database: self, handle })
    }

    /// Captures a point-in-time view of the database, released when it is dropped.
    pub fn snapshot(&self) -> Result<Snapshot<'_>> {
        let mut id: ffi::ustore_snapshot_t = 0;
        let mut error = ffi::ustore_error_t::default();
        let mut arguments = ffi::ustore_snapshot_create_t {
            db: self.handle,
            error: &mut error,
            id: &mut id,
        };
        // SAFETY: every pointer in `arguments` outlives the call.
        unsafe { ffi::ustore_snapshot_create(&mut arguments) };
        check(error)?;
        Ok(Snapshot {
            database: self,
            id: SnapshotId(id),
        })
    }

    /// Every snapshot the database holds, exported into the arena.
    pub fn snapshots<'arena>(&self, arena: &'arena mut Arena) -> Result<&'arena [SnapshotId]> {
        let mut count: ffi::ustore_size_t = 0;
        let mut ids: *mut ffi::ustore_snapshot_t = ptr::null_mut();
        let mut error = ffi::ustore_error_t::default();
        let mut arguments = ffi::ustore_snapshot_list_t {
            db: self.handle,
            error: &mut error,
            arena: arena.slot(),
            options: ffi::USTORE_OPTIONS_DEFAULT_K,
            count: &mut count,
            ids: &mut ids,
        };
        // SAFETY: every pointer in `arguments` outlives the call.
        unsafe { ffi::ustore_snapshot_list(&mut arguments) };
        check(error)?;
        // SAFETY: a successful listing writes `count` identifiers into the arena, which stays
        // borrowed for as long as the slice does; `SnapshotId` is a transparent wrapper of one.
        Ok(unsafe { exported_slice(ids.cast::<SnapshotId>(), count as usize) })
    }

    /// Creates an empty collection and binds `name` to it, refusing a name already in use.
    pub fn create_collection(&self, arena: &mut Arena, name: &str) -> Result<Collection> {
        if crate::strings::read_name(self.head(), arena, name)?.is_some() {
            return Err(fail(Status::AlreadyExists, "a collection already answers to that name"));
        }
        let mut id: ffi::ustore_collection_t = 0;
        let mut error = ffi::ustore_error_t::default();
        let mut arguments = ffi::ustore_collection_create_t {
            db: self.handle,
            error: &mut error,
            config: ptr::null(),
            id: &mut id,
        };
        // SAFETY: every pointer in `arguments` outlives the call.
        unsafe { ffi::ustore_collection_create(&mut arguments) };
        check(error)?;
        crate::strings::write_name(self.head(), arena, name, Some(&id.to_ne_bytes()))?;
        Ok(Collection(id))
    }

    /// Drops the collection `name` addresses, together with its contents and its name.
    pub fn drop_collection(&self, arena: &mut Arena, name: &str) -> Result<()> {
        let collection = self.head().collection(arena, name)?;
        self.clear_collection(collection, Removed::CollectionAndEntries)?;
        crate::strings::write_name(self.head(), arena, name, None)
    }

    /// Removes the contents of a collection, or the collection itself, keeping every name bound.
    pub fn clear_collection(&self, collection: Collection, removed: Removed) -> Result<()> {
        let mut error = ffi::ustore_error_t::default();
        let mut arguments = ffi::ustore_collection_drop_t {
            db: self.handle,
            error: &mut error,
            id: collection.0,
            mode: removed as ffi::ustore_enum_t,
        };
        // SAFETY: every pointer in `arguments` outlives the call.
        unsafe { ffi::ustore_collection_drop(&mut arguments) };
        check(error)
    }

    /// The raw handle the modality verbs pass across the boundary.
    pub(crate) fn handle(&self) -> ffi::ustore_database_t {
        self.handle
    }
}

impl Drop for Database {
    fn drop(&mut self) {
        // SAFETY: the handle came from a successful open and is freed once.
        unsafe { ffi::ustore_database_free(self.handle) };
    }
}

/// Borrows `count` results the engine exported into an arena, reporting an empty run as an empty
/// slice rather than dereferencing the null an engine may leave behind.
///
/// # Safety
/// `first` addresses `count` initialized values that stay alive for `'arena`.
pub(crate) unsafe fn exported_slice<'arena, Value>(first: *const Value, count: usize) -> &'arena [Value] {
    match count == 0 || first.is_null() {
        true => &[],
        false => std::slice::from_raw_parts(first, count),
    }
}

// endregion: Databases

// region: Views

/// The one view a call addresses: the head of a database, a transaction, or a snapshot.
///
/// Every read, write and scan hangs off this type, so a call naming two views at once - which the C
/// ABI refuses - cannot be written.
#[derive(Clone, Copy, Debug)]
pub struct View<'view> {
    database: ffi::ustore_database_t,
    transaction: ffi::ustore_transaction_t,
    snapshot: ffi::ustore_snapshot_t,
    marker: PhantomData<&'view Database>,
}

impl<'view> View<'view> {
    /// The database every call through this view reaches.
    pub(crate) fn database(&self) -> ffi::ustore_database_t {
        self.database
    }

    /// The transaction this view reads and writes through, or null.
    pub(crate) fn transaction(&self) -> ffi::ustore_transaction_t {
        self.transaction
    }

    /// The snapshot this view reads at, or zero.
    pub(crate) fn snapshot(&self) -> ffi::ustore_snapshot_t {
        self.snapshot
    }

    /// Every collection this view holds, the main one excluded, exported into the arena.
    pub fn collections<'arena>(&self, arena: &'arena mut Arena) -> Result<&'arena [Collection]> {
        let mut count: ffi::ustore_size_t = 0;
        let mut ids: *mut ffi::ustore_collection_t = ptr::null_mut();
        let mut error = ffi::ustore_error_t::default();
        let mut arguments = ffi::ustore_collection_list_t {
            db: self.database,
            error: &mut error,
            transaction: self.transaction,
            snapshot: self.snapshot,
            arena: arena.slot(),
            options: ffi::USTORE_OPTIONS_DEFAULT_K,
            count: &mut count,
            ids: &mut ids,
        };
        // SAFETY: every pointer in `arguments` outlives the call.
        unsafe { ffi::ustore_collection_list(&mut arguments) };
        check(error)?;
        // SAFETY: a successful listing writes `count` identifiers into the arena, which stays
        // borrowed for as long as the slice does; `Collection` is a transparent wrapper of one.
        Ok(unsafe { exported_slice(ids.cast::<Collection>(), count as usize) })
    }
}

// endregion: Views

// region: Transactions

/// Begins a transaction, or resets the one the slot already holds.
fn begin(database: ffi::ustore_database_t, handle: &mut ffi::ustore_transaction_t) -> Result<()> {
    let mut error = ffi::ustore_error_t::default();
    let mut arguments = ffi::ustore_transaction_init_t {
        db: database,
        error: &mut error,
        options: ffi::USTORE_OPTIONS_DEFAULT_K,
        transaction: handle,
    };
    // SAFETY: every pointer in `arguments` outlives the call.
    unsafe { ffi::ustore_transaction_init(&mut arguments) };
    check(error)
}

/// An optimistic transaction grouping reads and writes across collections and modalities.
///
/// Not thread-safe, which the missing `Send` and `Sync` carry, and freed before its database closes,
/// which the borrow of the database carries.
#[derive(Debug)]
pub struct Transaction<'db> {
    database: &'db Database,
    handle: ffi::ustore_transaction_t,
}

impl<'db> Transaction<'db> {
    /// The view every call inside this transaction addresses.
    pub fn view(&self) -> View<'_> {
        View {
            database: self.database.handle(),
            transaction: self.handle,
            snapshot: 0,
            marker: PhantomData,
        }
    }

    /// Stages the writes as the first phase of a two-phase commit, reporting the sequence number
    /// the stage took.
    pub fn stage(&mut self) -> Result<SequenceNumber> {
        let mut sequence_number: SequenceNumber = 0;
        let mut error = ffi::ustore_error_t::default();
        let mut arguments = ffi::ustore_transaction_stage_t {
            db: self.database.handle(),
            error: &mut error,
            transaction: self.handle,
            options: ffi::USTORE_OPTIONS_DEFAULT_K,
            sequence_number: &mut sequence_number,
        };
        // SAFETY: every pointer in `arguments` outlives the call.
        unsafe { ffi::ustore_transaction_stage(&mut arguments) };
        check(error)?;
        Ok(sequence_number)
    }

    /// Commits the writes, reporting the sequence number they applied at.
    ///
    /// A conflict is reported rather than retried, and the transaction keeps its contents either
    /// way, so a caller may replay them after [`Transaction::rollback`].
    pub fn commit(&mut self, durability: Durability) -> Result<SequenceNumber> {
        let mut sequence_number: SequenceNumber = 0;
        let mut error = ffi::ustore_error_t::default();
        let mut arguments = ffi::ustore_transaction_commit_t {
            db: self.database.handle(),
            error: &mut error,
            transaction: self.handle,
            options: durability.to_raw(),
            sequence_number: &mut sequence_number,
        };
        // SAFETY: every pointer in `arguments` outlives the call.
        unsafe { ffi::ustore_transaction_commit(&mut arguments) };
        check(error)?;
        Ok(sequence_number)
    }

    /// Discards the writes and reopens the transaction on the newest state, ready for another try.
    pub fn rollback(&mut self) -> Result<()> {
        begin(self.database.handle(), &mut self.handle)
    }
}

impl Drop for Transaction<'_> {
    fn drop(&mut self) {
        // SAFETY: the handle came from a successful begin, is freed once, and outlives no view -
        // every view borrows this transaction.
        unsafe { ffi::ustore_transaction_free(self.handle) };
    }
}

// endregion: Transactions

// region: Snapshots

/// A point-in-time view of the whole database, released when it is dropped.
#[derive(Debug)]
pub struct Snapshot<'db> {
    database: &'db Database,
    id: SnapshotId,
}

impl<'db> Snapshot<'db> {
    /// The identifier `Database::snapshots` reports this snapshot under.
    pub fn id(&self) -> SnapshotId {
        self.id
    }

    /// The view every call at this snapshot addresses, which no write may go through.
    pub fn view(&self) -> View<'_> {
        View {
            database: self.database.handle(),
            transaction: ptr::null_mut(),
            snapshot: self.id.0,
            marker: PhantomData,
        }
    }

    /// Writes this snapshot into an existing directory as a database of its own.
    pub fn export(&self, path: &str) -> Result<()> {
        let path = CString::new(path).map_err(|_| fail(Status::Malformed, "the path holds a null character"))?;
        let mut error = ffi::ustore_error_t::default();
        let mut arguments = ffi::ustore_snapshot_export_t {
            db: self.database.handle(),
            error: &mut error,
            id: self.id.0,
            path: path.as_ptr(),
        };
        // SAFETY: every pointer in `arguments` outlives the call, the path included.
        unsafe { ffi::ustore_snapshot_export(&mut arguments) };
        check(error)
    }
}

impl Drop for Snapshot<'_> {
    fn drop(&mut self) {
        let mut error = ffi::ustore_error_t::default();
        let mut arguments = ffi::ustore_snapshot_drop_t {
            db: self.database.handle(),
            error: &mut error,
            id: self.id.0,
        };
        // SAFETY: every pointer in `arguments` outlives the call; the only failure a drop can meet
        // is a snapshot the engine already released, which leaves nothing to do.
        unsafe { ffi::ustore_snapshot_drop(&mut arguments) };
    }
}

// endregion: Snapshots

// region: Tests

#[cfg(test)]
mod tests {
    use super::*;
    use crate::testing::temporary_directory;

    #[test]
    fn databases_open_in_memory_and_report_their_engine() {
        let database = Database::open(Configuration::in_memory()).unwrap();
        let metadata = database.metadata().unwrap();
        assert!(metadata.supports_transactions());
        assert!(metadata.supports_collections());
        assert!(metadata.supports_snapshots());

        let mut arena = Arena::new();
        let described = database.control(&mut arena, "info").unwrap();
        assert!(described.contains("smashtable"), "{described}");
    }

    #[test]
    fn databases_open_at_a_directory_and_keep_what_they_held() {
        let directory = temporary_directory("database_reopens");
        let configuration = Configuration::at(directory.path());
        let mut arena = Arena::new();

        let database = Database::open(configuration).unwrap();
        let entries = [crate::blobs::Entry::new(7, b"alpha").unwrap()];
        database
            .head()
            .write(&mut arena, Collection::main(), &entries, Durability::Flushed)
            .unwrap();
        drop(database);

        let reopened = Database::open(configuration).unwrap();
        let values = reopened.head().read(&mut arena, Collection::main(), &[7]).unwrap();
        assert_eq!(values.get(0), Some(b"alpha".as_slice()));
    }

    #[test]
    fn databases_refuse_a_directory_that_is_not_there() {
        let failure = Database::open(Configuration::at("/nonexistent/ustore/directory")).unwrap_err();
        assert_eq!(failure.status, Status::NotFound);
    }

    #[test]
    fn configurations_escape_what_they_carry() {
        let quoted = Configuration::at("/tmp/say \"hello\"\\now").to_json().unwrap();
        assert_eq!(
            quoted.to_str().unwrap(),
            "{\"isolation\":\"strict_serializable\",\"directory\":\"/tmp/say \\\"hello\\\"\\\\now\"}"
        );
        assert_eq!(
            Configuration::in_memory()
                .with_isolation(Isolation::Snapshot)
                .to_json()
                .unwrap()
                .to_str()
                .unwrap(),
            "{\"isolation\":\"snapshot\"}"
        );
        // A path the JSON cannot carry is refused rather than smuggled through.
        assert_eq!(
            Configuration::at("/tmp/a\nb").to_json().unwrap_err().status,
            Status::Malformed
        );
    }

    #[test]
    fn databases_open_at_either_isolation() {
        for isolation in [Isolation::StrictSerializable, Isolation::Snapshot] {
            let database = Database::open(Configuration::in_memory().with_isolation(isolation)).unwrap();
            assert!(database.metadata().unwrap().supports_transactions());
        }
    }

    #[test]
    fn engines_refuse_a_request_they_do_not_know() {
        let database = Database::open(Configuration::in_memory()).unwrap();
        let mut arena = Arena::new();
        let failure = database.control(&mut arena, "levitate").unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);
        assert!(!failure.message.is_empty());
    }

    #[test]
    fn collections_are_created_listed_and_dropped_by_name() {
        let database = Database::open(Configuration::in_memory()).unwrap();
        let mut arena = Arena::new();

        let people = database.create_collection(&mut arena, "people").unwrap();
        let places = database.create_collection(&mut arena, "places").unwrap();
        assert_ne!(people, places);
        assert_eq!(database.head().collection(&mut arena, "people").unwrap(), people);

        let listed = database.head().collections(&mut arena).unwrap();
        assert_eq!(listed.len(), 2, "the main collection is never listed");
        assert!(listed.contains(&people) && listed.contains(&places));

        // A name is bound once, and a name nobody bound addresses nothing.
        assert_eq!(
            database.create_collection(&mut arena, "people").unwrap_err().status,
            Status::AlreadyExists
        );
        assert_eq!(
            database.head().collection(&mut arena, "nowhere").unwrap_err().status,
            Status::NotFound
        );

        database.drop_collection(&mut arena, "people").unwrap();
        assert_eq!(
            database.head().collection(&mut arena, "people").unwrap_err().status,
            Status::NotFound
        );
        assert_eq!(database.head().collections(&mut arena).unwrap().len(), 1);
    }

    #[test]
    fn the_main_collection_clears_but_never_drops() {
        let database = Database::open(Configuration::in_memory()).unwrap();
        let mut arena = Arena::new();
        let entries = [crate::blobs::Entry::new(1, b"alpha").unwrap()];
        database
            .head()
            .write(&mut arena, Collection::main(), &entries, Durability::Buffered)
            .unwrap();

        let failure = database
            .clear_collection(Collection::main(), Removed::CollectionAndEntries)
            .unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);

        database.clear_collection(Collection::main(), Removed::Entries).unwrap();
        let values = database.head().read(&mut arena, Collection::main(), &[1]).unwrap();
        assert_eq!(values.get(0), None);
    }

    #[test]
    fn transactions_commit_what_they_wrote() {
        let database = Database::open(Configuration::in_memory()).unwrap();
        let mut arena = Arena::new();
        let mut transaction = database.transaction().unwrap();
        let entries = [crate::blobs::Entry::new(1, b"alpha").unwrap()];
        transaction
            .view()
            .write(&mut arena, Collection::main(), &entries, Durability::Buffered)
            .unwrap();

        // Nothing is visible at the head until the commit lands.
        let before = database.head().read(&mut arena, Collection::main(), &[1]).unwrap();
        assert_eq!(before.get(0), None);

        let staged = transaction.stage().unwrap();
        let committed = transaction.commit(Durability::Buffered).unwrap();
        assert!(committed >= staged);

        let after = database.head().read(&mut arena, Collection::main(), &[1]).unwrap();
        assert_eq!(after.get(0), Some(b"alpha".as_slice()));
    }

    #[test]
    fn transactions_roll_back_and_run_again() {
        let database = Database::open(Configuration::in_memory()).unwrap();
        let mut arena = Arena::new();
        let mut transaction = database.transaction().unwrap();
        let abandoned = [crate::blobs::Entry::new(2, b"abandoned").unwrap()];
        transaction
            .view()
            .write(&mut arena, Collection::main(), &abandoned, Durability::Buffered)
            .unwrap();
        transaction.rollback().unwrap();

        let kept = [crate::blobs::Entry::new(2, b"kept").unwrap()];
        transaction
            .view()
            .write(&mut arena, Collection::main(), &kept, Durability::Buffered)
            .unwrap();
        transaction.commit(Durability::Buffered).unwrap();

        let values = database.head().read(&mut arena, Collection::main(), &[2]).unwrap();
        assert_eq!(values.get(0), Some(b"kept".as_slice()));
    }

    #[test]
    fn transactions_refuse_a_flush_of_their_own() {
        let database = Database::open(Configuration::in_memory()).unwrap();
        let mut arena = Arena::new();
        let transaction = database.transaction().unwrap();
        let entries = [crate::blobs::Entry::new(3, b"alpha").unwrap()];
        let failure = transaction
            .view()
            .write(&mut arena, Collection::main(), &entries, Durability::Flushed)
            .unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);
    }

    #[test]
    fn transactions_conflict_rather_than_block() {
        let database = Database::open(Configuration::in_memory()).unwrap();
        let mut arena = Arena::new();
        let mut first = database.transaction().unwrap();
        let mut second = database.transaction().unwrap();

        let mine = [crate::blobs::Entry::new(4, b"mine").unwrap()];
        let yours = [crate::blobs::Entry::new(4, b"yours").unwrap()];
        first
            .view()
            .write(&mut arena, Collection::main(), &mine, Durability::Buffered)
            .unwrap();
        second
            .view()
            .write(&mut arena, Collection::main(), &yours, Durability::Buffered)
            .unwrap();
        first.commit(Durability::Buffered).unwrap();

        let failure = second.commit(Durability::Buffered).unwrap_err();
        assert!(
            matches!(
                failure.status,
                Status::WriteConflict | Status::ReadConflict | Status::PhantomConflict
            ),
            "{failure:?}"
        );
    }

    #[test]
    fn snapshots_hold_the_state_they_were_taken_at() {
        let database = Database::open(Configuration::in_memory()).unwrap();
        let mut arena = Arena::new();
        let before = [crate::blobs::Entry::new(5, b"before").unwrap()];
        database
            .head()
            .write(&mut arena, Collection::main(), &before, Durability::Buffered)
            .unwrap();

        let snapshot = database.snapshot().unwrap();
        let after = [crate::blobs::Entry::new(5, b"after").unwrap()];
        database
            .head()
            .write(&mut arena, Collection::main(), &after, Durability::Buffered)
            .unwrap();

        let pinned = snapshot.view().read(&mut arena, Collection::main(), &[5]).unwrap();
        assert_eq!(pinned.get(0), Some(b"before".as_slice()));
        let latest = database.head().read(&mut arena, Collection::main(), &[5]).unwrap();
        assert_eq!(latest.get(0), Some(b"after".as_slice()));

        let listed = database.snapshots(&mut arena).unwrap();
        assert!(listed.contains(&snapshot.id()));

        let identifier = snapshot.id();
        drop(snapshot);
        assert!(!database.snapshots(&mut arena).unwrap().contains(&identifier));
    }

    #[test]
    fn snapshots_export_a_database_of_their_own() {
        let database = Database::open(Configuration::in_memory()).unwrap();
        let mut arena = Arena::new();
        let entries = [crate::blobs::Entry::new(6, b"exported").unwrap()];
        database
            .head()
            .write(&mut arena, Collection::main(), &entries, Durability::Buffered)
            .unwrap();

        let directory = temporary_directory("snapshot_exports");
        let snapshot = database.snapshot().unwrap();
        snapshot.export(directory.path()).unwrap();
        assert_eq!(
            snapshot.export("/nonexistent/ustore/directory").unwrap_err().status,
            Status::NotFound
        );

        let exported = Database::open(Configuration::at(directory.path())).unwrap();
        let values = exported.head().read(&mut arena, Collection::main(), &[6]).unwrap();
        assert_eq!(values.get(0), Some(b"exported".as_slice()));
    }

    #[test]
    fn statuses_map_and_tolerate_newer_libraries() {
        let written = ffi::ustore_error_t {
            status: 7,
            message: c"No such collection".as_ptr(),
        };
        let failure = check(written).unwrap_err();
        assert_eq!(failure.status, Status::NotFound);
        assert_eq!(failure.message, "No such collection");
        assert_eq!(failure.to_string(), "No such collection");

        let newer = ffi::ustore_error_t {
            status: 4242,
            message: ptr::null(),
        };
        assert_eq!(check(newer).unwrap_err().status, Status::Unrecognized);
        assert!(check(ffi::ustore_error_t::default()).is_ok());
    }

    #[test]
    fn constants_come_from_the_linked_engine() {
        assert_eq!(Collection::main().0, 0);
        assert_ne!(Collection::names().0, Collection::main().0);
        assert_eq!(missing_length(), u32::MAX);
        assert_ne!(key_unknown(), 0);
    }
}

// endregion: Tests
