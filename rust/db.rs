//! Databases, views, transactions, snapshots and arenas - the Rust twin of `include/ustore/db.h`.
//!
//! Every call addresses exactly one view of the data: the head of the database, a transaction, or a
//! snapshot. [`View`] is that view, so a call naming two cannot be spelled here. Results land in a
//! caller-owned [`Arena`] that the next call reuses, which is why a call borrows it mutably and every
//! exported result borrows it back.

use std::ffi::{c_char, CStr, CString};
use std::fmt;
use std::iter::FusedIterator;
use std::marker::PhantomData;
use std::ops::Range;
use std::ptr;

// region: Raw Declarations

/// One-to-one mirror of `include/ustore/types.h` and `include/ustore/db.h`.
pub(crate) mod ffi {
    #![allow(non_camel_case_types)]

    use std::ffi::{c_char, c_int, c_void};

    /// The opaque structs the C ABI hands out pointers to, which Rust never reads through.
    #[repr(C)]
    pub struct ustore_database_handle_t {
        _private: [u8; 0],
    }
    #[repr(C)]
    pub struct ustore_transaction_handle_t {
        _private: [u8; 0],
    }
    #[repr(C)]
    pub struct ustore_snapshot_handle_t {
        _private: [u8; 0],
    }
    #[repr(C)]
    pub struct ustore_view_handle_t {
        _private: [u8; 0],
    }
    #[repr(C)]
    pub struct ustore_arena_handle_t {
        _private: [u8; 0],
    }
    #[repr(C)]
    pub struct ustore_executor_handle_t {
        _private: [u8; 0],
    }

    pub type ustore_database_t = *mut ustore_database_handle_t;
    pub type ustore_transaction_t = *mut ustore_transaction_handle_t;
    pub type ustore_snapshot_t = *mut ustore_snapshot_handle_t;
    pub type ustore_view_t = *mut ustore_view_handle_t;
    pub type ustore_arena_t = *mut ustore_arena_handle_t;
    pub type ustore_executor_t = *mut ustore_executor_handle_t;

    // The public identifiers and choices are `#[repr(C)]` twins of what the header declares, so the ABI names them
    pub type ustore_collection_id_t = super::CollectionID;
    pub type ustore_durability_t = super::Durability;
    pub type ustore_watch_t = super::Conflicts;
    pub type ustore_key_t = super::Key;
    pub type ustore_byte_t = u8;
    pub type ustore_char_t = c_char;
    pub type ustore_length_t = u64;
    pub type ustore_size_t = u64;
    pub type ustore_octet_t = u8;
    pub type ustore_stamp_t = u64;
    pub type ustore_str_view_t = *const c_char;
    pub type ustore_bytes_cptr_t = *const u8;

    /// The `ustore_drop_mode_t`, `ustore_capabilities_t`, `ustore_isolation_t`, `ustore_transport_t` and
    /// `ustore_modality_t` enumerations, each of which lowers to the platform `int`.
    pub type ustore_enum_t = c_int;

    /// The `ustore_status_t` a call stores, kept as its integer, since a value no variant of a Rust enum names is
    /// undefined behavior to read.
    #[repr(transparent)]
    #[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
    pub struct ustore_status_t(pub c_int);

    impl ustore_status_t {
        pub const SUCCESS: ustore_status_t = ustore_status_t(0);
    }

    /// The outcome of one call: a status, and a static message that is null exactly on success.
    #[repr(C)]
    #[derive(Clone, Copy)]
    pub struct ustore_outcome_t {
        pub status: ustore_status_t,
        pub message: *const c_char,
    }

    impl Default for ustore_outcome_t {
        fn default() -> Self {
            ustore_outcome_t {
                status: ustore_status_t::SUCCESS,
                message: core::ptr::null(),
            }
        }
    }

    /// Whether a call returns once done or once queued, where the binding only ever blocks.
    #[repr(C)]
    #[allow(dead_code)]
    pub enum ustore_completion_t {
        Blocking = 0,
        Deferred = 1,
    }

    /// In what order a scan hands its keys over, where the binding only ever asks for order.
    #[repr(C)]
    #[allow(dead_code)]
    pub enum ustore_scan_mode_t {
        Ordered = 0,
        Bulk = 1,
    }

    /// A progress callback answering a `ustore_continuation_t`, which the binding never registers.
    pub type ustore_progress_callback_t =
        Option<unsafe extern "C" fn(ustore_size_t, ustore_size_t, *mut c_void) -> ustore_enum_t>;

    pub const USTORE_ISOLATION_STRICT_SERIALIZABLE_K: ustore_enum_t = 0;
    pub const USTORE_ISOLATION_SNAPSHOT_K: ustore_enum_t = 1;

    pub const USTORE_TRANSPORT_AUTOMATIC_K: ustore_enum_t = 0;
    pub const USTORE_TRANSPORT_BLOCKING_K: ustore_enum_t = 1;
    pub const USTORE_TRANSPORT_LINUX_URING_K: ustore_enum_t = 2;
    pub const USTORE_TRANSPORT_LINUX_AIO_K: ustore_enum_t = 3;
    pub const USTORE_TRANSPORT_LINUX_SPDK_K: ustore_enum_t = 4;

    pub const USTORE_MODALITY_ANY_K: ustore_enum_t = 0;
    pub const USTORE_MODALITY_BLOBS_K: ustore_enum_t = 1;
    pub const USTORE_MODALITY_DOCS_K: ustore_enum_t = 2;
    pub const USTORE_MODALITY_GRAPH_K: ustore_enum_t = 3;
    pub const USTORE_MODALITY_STRINGS_K: ustore_enum_t = 4;
    pub const USTORE_MODALITY_PAGES_K: ustore_enum_t = 5;

    pub const USTORE_SUPPORTS_TRANSACTIONS_K: ustore_enum_t = 1;
    pub const USTORE_SUPPORTS_COLLECTIONS_K: ustore_enum_t = 2;
    pub const USTORE_SUPPORTS_SNAPSHOTS_K: ustore_enum_t = 4;

    #[repr(C)]
    pub struct ustore_data_directory_t {
        pub path: ustore_str_view_t,
        pub max_bytes: ustore_size_t,
    }

    #[repr(C)]
    pub struct ustore_database_init_ram_t {
        pub database: *mut ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub directory: ustore_str_view_t,
        pub isolation: ustore_enum_t,
    }

    #[repr(C)]
    pub struct ustore_database_init_rocksdb_t {
        pub database: *mut ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub directory: ustore_str_view_t,
        pub data_directories: *const ustore_data_directory_t,
        pub data_directories_count: ustore_size_t,
    }

    #[repr(C)]
    pub struct ustore_database_init_leveldb_t {
        pub database: *mut ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub directory: ustore_str_view_t,
    }

    #[repr(C)]
    pub struct ustore_database_init_nvme_t {
        pub database: *mut ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub directory: ustore_str_view_t,
        pub data_directories: *const ustore_data_directory_t,
        pub data_directories_count: ustore_size_t,
        pub transport: ustore_enum_t,
        pub memory_limit: ustore_size_t,
        pub gpu_devices: *const c_int,
        pub gpu_devices_count: ustore_size_t,
        pub gpu_memory_limit: ustore_size_t,
        pub shared_io_channels: ustore_size_t,
        pub queue_depth: ustore_size_t,
        pub request_buffer_bytes: ustore_size_t,
        pub request_buffers_count: ustore_size_t,
        pub stream_buffer_bytes: ustore_size_t,
        pub stream_buffers_count: ustore_size_t,
        pub transaction_max_bytes: ustore_size_t,
        pub transaction_max_elements: ustore_size_t,
        pub collection_concurrency_limit: ustore_size_t,
        pub collection_cache_limit: ustore_size_t,
        pub collection_write_buffer_max_bytes: ustore_size_t,
        pub collection_write_buffer_max_elements: ustore_size_t,
        pub collection_first_level_max_bytes: ustore_size_t,
        pub collection_level_enlarge_factor: ustore_size_t,
        pub collection_value_size: ustore_size_t,
        pub collection_value_max_size: ustore_size_t,
    }

    #[repr(C)]
    pub struct ustore_database_description_t {
        pub capabilities: ustore_enum_t,
    }

    #[repr(C)]
    pub struct ustore_database_describe_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub description: *mut ustore_database_description_t,
    }

    #[repr(C)]
    pub struct ustore_database_control_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub request: ustore_str_view_t,
        pub response: *mut ustore_str_view_t,
        pub arena: *mut ustore_arena_t,
        pub progress: ustore_progress_callback_t,
        pub progress_state: *mut c_void,
    }

    #[repr(C)]
    pub struct ustore_snapshot_init_t {
        pub snapshot: *mut ustore_snapshot_t,
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
    }

    #[repr(C)]
    pub struct ustore_snapshot_export_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub snapshot: ustore_snapshot_t,
        pub path: ustore_str_view_t,
        pub progress: ustore_progress_callback_t,
        pub progress_state: *mut c_void,
    }

    #[repr(C)]
    pub struct ustore_collection_list_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub snapshot: ustore_snapshot_t,
        pub count: *mut ustore_size_t,
        pub collections: *mut *mut ustore_collection_id_t,
        pub modalities: *mut *mut ustore_enum_t,
        pub names_offsets: *mut *mut ustore_length_t,
        pub names: *mut *mut ustore_char_t,
        pub arena: *mut ustore_arena_t,
    }

    #[repr(C)]
    pub struct ustore_collection_create_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub name: ustore_str_view_t,
        pub name_length: ustore_length_t,
        pub modality: ustore_enum_t,
        pub collection: *mut ustore_collection_id_t,
    }

    #[repr(C)]
    pub struct ustore_collection_find_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub snapshot: ustore_snapshot_t,
        pub count: ustore_size_t,
        pub names: *const ustore_str_view_t,
        pub names_stride: ustore_size_t,
        pub names_lengths: *const ustore_length_t,
        pub names_lengths_stride: ustore_size_t,
        pub presences: *mut ustore_octet_t,
        pub collections: *mut ustore_collection_id_t,
        pub collections_stride: ustore_size_t,
        pub modalities: *mut ustore_enum_t,
        pub modalities_stride: ustore_size_t,
    }

    #[repr(C)]
    pub struct ustore_collection_drop_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub collection: ustore_collection_id_t,
        pub mode: ustore_enum_t,
    }

    #[repr(C)]
    pub struct ustore_transaction_init_t {
        pub transaction: *mut ustore_transaction_t,
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
    }

    #[repr(C)]
    pub struct ustore_transaction_stage_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub transaction: ustore_transaction_t,
        pub stamp: *mut ustore_stamp_t,
    }

    #[repr(C)]
    pub struct ustore_transaction_commit_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub transaction: ustore_transaction_t,
        pub stamp: *mut ustore_stamp_t,
        pub durability: ustore_durability_t,
    }

    #[repr(C)]
    pub struct ustore_transaction_rollback_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub transaction: ustore_transaction_t,
    }

    extern "C" {
        pub static ustore_collection_main_k: ustore_collection_id_t;
        pub static ustore_length_missing_k: ustore_length_t;
        pub static ustore_key_unknown_k: ustore_key_t;

        pub fn ustore_status_name(status: ustore_status_t) -> *const c_char;

        pub fn ustore_database_init_ram(arguments: *mut ustore_database_init_ram_t);
        pub fn ustore_database_init_rocksdb(arguments: *mut ustore_database_init_rocksdb_t);
        pub fn ustore_database_init_leveldb(arguments: *mut ustore_database_init_leveldb_t);
        pub fn ustore_database_init_nvme(arguments: *mut ustore_database_init_nvme_t);

        pub fn ustore_database_describe(arguments: *mut ustore_database_describe_t);
        pub fn ustore_database_control(arguments: *mut ustore_database_control_t);

        pub fn ustore_snapshot_init(arguments: *mut ustore_snapshot_init_t);
        pub fn ustore_snapshot_export(arguments: *mut ustore_snapshot_export_t);
        pub fn ustore_snapshot_view(snapshot: ustore_snapshot_t) -> ustore_view_t;

        pub fn ustore_collection_list(arguments: *mut ustore_collection_list_t);
        pub fn ustore_collection_create(arguments: *mut ustore_collection_create_t);
        pub fn ustore_collection_find(arguments: *mut ustore_collection_find_t);
        pub fn ustore_collection_drop(arguments: *mut ustore_collection_drop_t);

        pub fn ustore_transaction_init(arguments: *mut ustore_transaction_init_t);
        pub fn ustore_transaction_view(transaction: ustore_transaction_t, watch: ustore_watch_t) -> ustore_view_t;
        pub fn ustore_transaction_stage(arguments: *mut ustore_transaction_stage_t);
        pub fn ustore_transaction_commit(arguments: *mut ustore_transaction_commit_t);
        pub fn ustore_transaction_rollback(arguments: *mut ustore_transaction_rollback_t);

        pub fn ustore_arena_free(arena: ustore_arena_t);
        pub fn ustore_transaction_free(transaction: ustore_transaction_t);
        pub fn ustore_snapshot_free(snapshot: ustore_snapshot_t);
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
    /// The call was submitted and has not finished.
    Pending = 15,
    /// The call found its executor or its arena busy with another call.
    Contended = 16,
    /// A progress callback stopped the call, which published nothing.
    Cancelled = 17,
    /// A status a newer library reported and this binding has no name for.
    Unrecognized = -1,
}

impl Status {
    fn from_raw(status: ffi::ustore_status_t) -> Self {
        match status.0 {
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
            15 => Status::Pending,
            16 => Status::Contended,
            17 => Status::Cancelled,
            _ => Status::Unrecognized,
        }
    }

    /// The canonical lowercase spelling the library gives this status, such as "out-of-memory", where one it has no
    /// name for spells as its fallback.
    pub fn name(self) -> &'static str {
        // SAFETY: the library answers a static null-terminated spelling for every value, unknown ones included.
        unsafe {
            static_string(ffi::ustore_status_name(ffi::ustore_status_t(
                self as ffi::ustore_enum_t,
            )))
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

/// Turns the outcome a blocking call stored into a `Result`.
pub(crate) fn check(outcome: ffi::ustore_outcome_t) -> Result<()> {
    if outcome.status == ffi::ustore_status_t::SUCCESS {
        return Ok(());
    }
    Err(Error {
        status: Status::from_raw(outcome.status),
        // SAFETY: a failed call stores a static null-terminated message, or null.
        message: unsafe { static_string(outcome.message) },
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

/// Sixteen bytes naming one thing everywhere, ordered and hashed as their bytes, and printed in the canonical
/// thirty-six character form.
///
/// An integer spells as its 128-bit big-endian bytes, as Python's `uuid.UUID(int=...)` does, so identifiers made from
/// integers order as the integers.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Default)]
pub struct UUID(pub [u8; 16]);

impl From<[u8; 16]> for UUID {
    fn from(bytes: [u8; 16]) -> Self {
        UUID(bytes)
    }
}

impl From<u128> for UUID {
    fn from(integer: u128) -> Self {
        UUID(integer.to_be_bytes())
    }
}

impl From<u64> for UUID {
    fn from(integer: u64) -> Self {
        UUID::from(u128::from(integer))
    }
}

impl fmt::Display for UUID {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        for (index, byte) in self.0.iter().enumerate() {
            if matches!(index, 4 | 6 | 8 | 10) {
                formatter.write_str("-")?;
            }
            write!(formatter, "{byte:02x}")?;
        }
        Ok(())
    }
}

impl fmt::Debug for UUID {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Display::fmt(self, formatter)
    }
}

/// The identifier of a value within one collection.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Default)]
pub struct Key(pub UUID);

impl Key {
    /// The key right after this one in their order, where a scan resuming past it starts; the last key wraps to the
    /// first.
    pub fn next(self) -> Key {
        Key(UUID::from(u128::from_be_bytes(self.0 .0).wrapping_add(1)))
    }
}

impl From<UUID> for Key {
    fn from(uuid: UUID) -> Self {
        Key(uuid)
    }
}

impl From<[u8; 16]> for Key {
    fn from(bytes: [u8; 16]) -> Self {
        Key(UUID::from(bytes))
    }
}

impl From<u128> for Key {
    fn from(integer: u128) -> Self {
        Key(UUID::from(integer))
    }
}

impl From<u64> for Key {
    fn from(integer: u64) -> Self {
        Key(UUID::from(integer))
    }
}

impl fmt::Display for Key {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Display::fmt(&self.0, formatter)
    }
}

impl fmt::Debug for Key {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Display::fmt(&self.0, formatter)
    }
}

/// The position of a commit in the order commits apply in, as a commit or a stage reports it.
pub type Stamp = ffi::ustore_stamp_t;

/// The identifier of a collection: a UUIDv7 the catalog mints once and never reuses, so it stays the same across
/// restarts and never names two collections.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Default)]
pub struct CollectionID(pub UUID);

impl CollectionID {
    /// The collection every database holds, named by the nil UUID, which may be cleared but never dropped.
    pub fn main() -> Self {
        // SAFETY: the library defines the constant once and never writes it.
        unsafe { ffi::ustore_collection_main_k }
    }
}

impl From<UUID> for CollectionID {
    fn from(uuid: UUID) -> Self {
        CollectionID(uuid)
    }
}

impl From<[u8; 16]> for CollectionID {
    fn from(bytes: [u8; 16]) -> Self {
        CollectionID(UUID::from(bytes))
    }
}

impl From<u128> for CollectionID {
    fn from(integer: u128) -> Self {
        CollectionID(UUID::from(integer))
    }
}

impl From<u64> for CollectionID {
    fn from(integer: u64) -> Self {
        CollectionID(UUID::from(integer))
    }
}

impl fmt::Display for CollectionID {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Display::fmt(&self.0, formatter)
    }
}

impl fmt::Debug for CollectionID {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Display::fmt(&self.0, formatter)
    }
}

/// The collections a batch addresses: one for every task, or one per task.
///
/// The ABI carries a collection per task, so a batch may span as many collections as it has entries;
/// [`Collections::Every`] is the common case, and every verb takes a bare [`CollectionID`] as one.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Hash)]
pub enum Collections<'collections> {
    /// Every task of the batch addresses the same collection.
    Every(CollectionID),
    /// The task at each position addresses the collection at the same position.
    Each(&'collections [CollectionID]),
}

impl From<CollectionID> for Collections<'_> {
    fn from(collection: CollectionID) -> Self {
        Collections::Every(collection)
    }
}

impl<'collections> From<&'collections [CollectionID]> for Collections<'collections> {
    fn from(collections: &'collections [CollectionID]) -> Self {
        Collections::Each(collections)
    }
}

impl Collections<'_> {
    /// The strided run of collections for `count` tasks, refusing a run of another length.
    ///
    /// The pointer addresses either the identifier inside this value or the caller's slice, so the
    /// call it feeds must run while both are alive.
    pub(crate) fn strided(&self, count: usize) -> Result<(*const ffi::ustore_collection_id_t, ffi::ustore_size_t)> {
        match self {
            Collections::Every(collection) => Ok((collection, 0)),
            Collections::Each(collections) if collections.len() == count => {
                Ok((collections.as_ptr(), size_of::<CollectionID>() as ffi::ustore_size_t))
            }
            Collections::Each(_) => Err(fail(Status::InvalidArgument, "the batch holds a collection per task")),
        }
    }
}

/// The length the ABI reports for a missing value.
pub(crate) fn missing_length() -> ffi::ustore_length_t {
    // SAFETY: the library defines the constant once and never writes it.
    unsafe { ffi::ustore_length_missing_k }
}

/// The length the ABI carries for `length` bytes, refusing one equal to the missing length.
pub(crate) fn described_length(length: usize, message: &'static str) -> Result<ffi::ustore_length_t> {
    match ffi::ustore_length_t::try_from(length) {
        Ok(described) if described != missing_length() => Ok(described),
        _ => Err(fail(Status::CapacityExhausted, message)),
    }
}

/// The key that stands for no key, every bit of it set.
pub fn key_unknown() -> Key {
    // SAFETY: the library defines the constant once and never writes it.
    unsafe { ffi::ustore_key_unknown_k }
}

/// Whether a write reaches the drive before the call returns, as `ustore_durability_t` spells it.
///
/// A transaction flushes at its commit, so `Flushed` on a transaction's write is refused.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub enum Durability {
    /// The engine decides when the bytes reach the drive.
    #[default]
    Buffered = 0,
    /// The call returns once the bytes are on the drive, at a cost that varies by engine.
    Flushed = 1,
}

/// Whether the calls of a transaction watch what they touch for conflicts, as `ustore_watch_t` spells it.
///
/// Only a transaction watches anything, so the head of a database and a snapshot never carry this, and
/// it changes what a call does only at [`Isolation::Snapshot`]: a strictly serializable transaction puts
/// every read it makes into the one serial order whatever this says.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub enum Conflicts {
    /// Every read and write the view makes is recorded, so a concurrent writer conflicts at the commit.
    #[default]
    Watched = 0,
    /// Nothing the view touches is watched, so a snapshot-isolated transaction commits over a concurrent
    /// writer instead of conflicting with it.
    Unwatched = 1,
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

/// Temporary memory results land in, which the next call through it reuses.
///
/// The library sizes the tapes of values, which land behind the handle, while the outputs a batch bounds land in
/// the spans beside it, which the binding sizes. Both grow to their high-water mark, so a steady loop allocates once. Every
/// batch verb takes one mutably and hands back a result borrowing it, so the borrow checker both keeps a result
/// from outliving its arena and stops the call that would overwrite it.
#[derive(Debug)]
pub struct Arena {
    pub(crate) handle: ffi::ustore_arena_t,
    /// Presence bits of a read, one per key.
    pub(crate) presences: Vec<ffi::ustore_octet_t>,
    /// Offsets of a read into its tape, or of a scan into its keys, one more than its tasks.
    pub(crate) offsets: Vec<ffi::ustore_length_t>,
    /// Keys a scan or a sample found, as many as its limit.
    pub(crate) keys: Vec<Key>,
    /// Degrees of the vertices a graph lookup addressed.
    pub(crate) degrees: Vec<u32>,
    /// Words the columns of a gather and their addresses are carved from, aligned for any scalar.
    pub(crate) columns: Vec<u64>,
}

impl Arena {
    /// An arena holding nothing, which the first call that uses it grows.
    pub fn new() -> Self {
        Arena {
            handle: ptr::null_mut(),
            presences: Vec::new(),
            offsets: Vec::new(),
            keys: Vec::new(),
            degrees: Vec::new(),
            columns: Vec::new(),
        }
    }

    /// The slot the calls grow in place.
    pub(crate) fn slot(&mut self) -> *mut ffi::ustore_arena_t {
        &mut self.handle
    }
}

/// The first `count` entries of `buffer`, which grows to hold them and never shrinks.
pub(crate) fn grown<Entry: Copy + Default>(buffer: &mut Vec<Entry>, count: usize) -> Result<&mut [Entry]> {
    if buffer.len() < count {
        buffer
            .try_reserve(count - buffer.len())
            .map_err(|_| fail(Status::OutOfMemory, "the arena could not grow its spans"))?;
        buffer.resize(count, Entry::default());
    }
    Ok(&mut buffer[..count])
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

/// The rows of a batch result in order, from either end, each read through the result's own `get`.
#[derive(Clone, Debug)]
pub struct Rows<Results, Row> {
    results: Results,
    rows: Range<usize>,
    row: fn(&Results, usize) -> Row,
}

impl<Results, Row> Rows<Results, Row> {
    pub(crate) fn new(results: Results, count: usize, row: fn(&Results, usize) -> Row) -> Self {
        Rows {
            results,
            rows: 0..count,
            row,
        }
    }
}

impl<Results, Row> Iterator for Rows<Results, Row> {
    type Item = Row;

    fn next(&mut self) -> Option<Row> {
        self.rows.next().map(|index| (self.row)(&self.results, index))
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        self.rows.size_hint()
    }
}

impl<Results, Row> DoubleEndedIterator for Rows<Results, Row> {
    fn next_back(&mut self) -> Option<Row> {
        self.rows.next_back().map(|index| (self.row)(&self.results, index))
    }
}

impl<Results, Row> ExactSizeIterator for Rows<Results, Row> {}

impl<Results, Row> FusedIterator for Rows<Results, Row> {}

// endregion: Arenas

// region: Options

/// What a collection holds, which an engine records beside its name and refuses the verbs of another modality by.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub enum Modality {
    /// Anything the blob and document verbs write, which the main collection holds.
    #[default]
    Any,
    /// Binary values.
    Blobs,
    /// JSON documents.
    Docs,
    /// Vertices and their edges, which only the graph verbs reach.
    Graph,
    /// Values keyed by strings.
    Strings,
    /// Ordered documents.
    Pages,
}

impl Modality {
    pub(crate) fn to_raw(self) -> ffi::ustore_enum_t {
        match self {
            Modality::Any => ffi::USTORE_MODALITY_ANY_K,
            Modality::Blobs => ffi::USTORE_MODALITY_BLOBS_K,
            Modality::Docs => ffi::USTORE_MODALITY_DOCS_K,
            Modality::Graph => ffi::USTORE_MODALITY_GRAPH_K,
            Modality::Strings => ffi::USTORE_MODALITY_STRINGS_K,
            Modality::Pages => ffi::USTORE_MODALITY_PAGES_K,
        }
    }

    /// The modality an engine reported, or `None` for one a newer library added.
    pub(crate) fn from_raw(raw: ffi::ustore_enum_t) -> Option<Self> {
        match raw {
            ffi::USTORE_MODALITY_ANY_K => Some(Modality::Any),
            ffi::USTORE_MODALITY_BLOBS_K => Some(Modality::Blobs),
            ffi::USTORE_MODALITY_DOCS_K => Some(Modality::Docs),
            ffi::USTORE_MODALITY_GRAPH_K => Some(Modality::Graph),
            ffi::USTORE_MODALITY_STRINGS_K => Some(Modality::Strings),
            ffi::USTORE_MODALITY_PAGES_K => Some(Modality::Pages),
            _ => None,
        }
    }
}

/// The isolation the in-memory engine delivers to the transactions of a database.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub enum Isolation {
    /// Reads and writes both take part in one serial order.
    #[default]
    StrictSerializable,
    /// A transaction reads the snapshot it began at and conflicts only on what it wrote.
    Snapshot,
}

/// How the NVMe engine moves bytes between its files and memory.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub enum Transport {
    /// io_uring where the kernel offers it, Linux AIO where it offers only that, and blocking calls elsewhere.
    #[default]
    Automatic,
    /// Positioned reads and writes that block the polling thread, which every platform has.
    Blocking,
    /// io_uring on unbuffered files, refused where the kernel creates no ring.
    LinuxUring,
    /// Linux AIO on unbuffered files.
    LinuxAio,
    /// SPDK's user-space driver on NVMe devices the process owns.
    LinuxSpdk,
}

/// A directory the data files may occupy, with the bytes they may take there.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub struct DataDirectory<'path> {
    /// The directory, which must exist.
    pub path: &'path str,
    /// Bytes the files may take there: zero is unlimited on RocksDB and the default volume size on NVMe.
    pub max_bytes: u64,
}

/// What [`Database::open_ram`] opens: in memory alone, or persisted as one image in a directory.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub struct RAMOptions<'path> {
    /// The directory the image persists in, which must exist, or `None` to keep the database in memory.
    pub directory: Option<&'path str>,
    /// How transactions see each other.
    pub isolation: Isolation,
}

impl<'path> RAMOptions<'path> {
    /// A database persisted as one image in an existing directory.
    pub fn at(directory: &'path str) -> Self {
        RAMOptions {
            directory: Some(directory),
            ..RAMOptions::default()
        }
    }
}

/// What [`Database::open_rocksdb`] opens.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub struct RocksDBOptions<'path> {
    /// The database's directory, which must exist.
    pub directory: &'path str,
    /// Directories the table files fill in order, or none to keep them in `directory`.
    pub data_directories: &'path [DataDirectory<'path>],
}

impl<'path> RocksDBOptions<'path> {
    /// A database in an existing directory.
    pub fn at(directory: &'path str) -> Self {
        RocksDBOptions {
            directory,
            ..RocksDBOptions::default()
        }
    }
}

/// What [`Database::open_leveldb`] opens.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub struct LevelDBOptions<'path> {
    /// The database's directory, which must exist.
    pub directory: &'path str,
}

impl<'path> LevelDBOptions<'path> {
    /// A database in an existing directory.
    pub fn at(directory: &'path str) -> Self {
        LevelDBOptions { directory }
    }
}

/// What [`Database::open_nvme`] opens, where every zero size or count keeps the engine's default.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub struct NVMEOptions<'path> {
    /// The directory of the database's own files.
    pub directory: &'path str,
    /// Directories the volumes fill, or none for one volume inside `directory`.
    pub data_directories: &'path [DataDirectory<'path>],
    /// How bytes move between the files and memory.
    pub transport: Transport,
    /// Bytes of host memory the engine may pin and cache.
    pub memory_limit: u64,
    /// CUDA devices compaction may run on.
    pub gpu_devices: &'path [i32],
    /// Bytes of device memory compaction may take on each of `gpu_devices`.
    pub gpu_memory_limit: u64,
    /// I/O channels the threads share beyond their own.
    pub shared_io_channels: u64,
    /// Submissions a channel keeps in flight.
    pub queue_depth: u64,
    /// Bytes of each request buffer of a channel.
    pub request_buffer_bytes: u64,
    /// Request buffers of each channel.
    pub request_buffers_count: u64,
    /// Bytes of each streaming buffer of a channel.
    pub stream_buffer_bytes: u64,
    /// Streaming buffers of each channel.
    pub stream_buffers_count: u64,
    /// Bytes a transaction may buffer before it must commit.
    pub transaction_max_bytes: u64,
    /// Writes a transaction may buffer before it must commit.
    pub transaction_max_elements: u64,
    /// Operations that may run on one collection at once.
    pub collection_concurrency_limit: u64,
    /// Bytes of values each collection may cache.
    pub collection_cache_limit: u64,
    /// Bytes each collection buffers before it flushes a sorted run.
    pub collection_write_buffer_max_bytes: u64,
    /// Writes each collection buffers before it flushes a sorted run.
    pub collection_write_buffer_max_elements: u64,
    /// Bytes the first level of each collection holds before it compacts into the next.
    pub collection_first_level_max_bytes: u64,
    /// How many times larger each level of a collection is than the one above it.
    pub collection_level_enlarge_factor: u64,
    /// Bytes of every value of a collection whose values share one size, or zero for varying sizes.
    pub collection_value_size: u64,
    /// Bytes of the largest value of a collection whose values vary in size.
    pub collection_value_max_size: u64,
}

impl<'path> NVMEOptions<'path> {
    /// A database in a directory, with every default.
    pub fn at(directory: &'path str) -> Self {
        NVMEOptions {
            directory,
            ..NVMEOptions::default()
        }
    }
}

/// The null-terminated copy of `path` the C ABI borrows.
fn c_path(path: &str) -> Result<CString> {
    CString::new(path).map_err(|_| fail(Status::Malformed, "a path holds a null character"))
}

/// The C view of `directories`, pointing into the null-terminated paths returned beside it.
fn c_data_directories(directories: &[DataDirectory<'_>]) -> Result<(Vec<CString>, Vec<ffi::ustore_data_directory_t>)> {
    let paths = directories
        .iter()
        .map(|directory| c_path(directory.path))
        .collect::<Result<Vec<_>>>()?;
    let entries = paths
        .iter()
        .zip(directories)
        .map(|(path, directory)| ffi::ustore_data_directory_t {
            path: path.as_ptr(),
            max_bytes: directory.max_bytes,
        })
        .collect();
    Ok((paths, entries))
}

// endregion: Options

// region: Databases

/// What an engine supports, as `ustore_database_describe` reports it.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Hash)]
pub struct Capabilities {
    bits: ffi::ustore_enum_t,
}

impl Capabilities {
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
    /// Opens the in-memory engine, which persists as one image when the options name a directory.
    pub fn open_ram(options: &RAMOptions<'_>) -> Result<Self> {
        let directory = options.directory.map(c_path).transpose()?;
        let mut handle: ffi::ustore_database_t = ptr::null_mut();
        let mut outcome = ffi::ustore_outcome_t::default();
        let mut arguments = ffi::ustore_database_init_ram_t {
            database: &mut handle,
            executor: ptr::null_mut(),
            outcome: &mut outcome,
            completion: ffi::ustore_completion_t::Blocking,
            directory: directory.as_ref().map_or(ptr::null(), |path| path.as_ptr()),
            isolation: match options.isolation {
                Isolation::StrictSerializable => ffi::USTORE_ISOLATION_STRICT_SERIALIZABLE_K,
                Isolation::Snapshot => ffi::USTORE_ISOLATION_SNAPSHOT_K,
            },
        };
        // SAFETY: every pointer in `arguments` outlives the call, the directory included.
        unsafe { ffi::ustore_database_init_ram(&mut arguments) };
        Database::opened(handle, outcome)
    }

    /// Opens the RocksDB engine, or fails with [`Status::Unsupported`] in a build without it.
    pub fn open_rocksdb(options: &RocksDBOptions<'_>) -> Result<Self> {
        let directory = c_path(options.directory)?;
        let (_paths, data_directories) = c_data_directories(options.data_directories)?;
        let mut handle: ffi::ustore_database_t = ptr::null_mut();
        let mut outcome = ffi::ustore_outcome_t::default();
        let mut arguments = ffi::ustore_database_init_rocksdb_t {
            database: &mut handle,
            executor: ptr::null_mut(),
            outcome: &mut outcome,
            completion: ffi::ustore_completion_t::Blocking,
            directory: directory.as_ptr(),
            data_directories: data_directories.as_ptr(),
            data_directories_count: data_directories.len() as ffi::ustore_size_t,
        };
        // SAFETY: every pointer in `arguments` outlives the call, the paths included.
        unsafe { ffi::ustore_database_init_rocksdb(&mut arguments) };
        Database::opened(handle, outcome)
    }

    /// Opens the LevelDB engine, or fails with [`Status::Unsupported`] in a build without it.
    pub fn open_leveldb(options: &LevelDBOptions<'_>) -> Result<Self> {
        let directory = c_path(options.directory)?;
        let mut handle: ffi::ustore_database_t = ptr::null_mut();
        let mut outcome = ffi::ustore_outcome_t::default();
        let mut arguments = ffi::ustore_database_init_leveldb_t {
            database: &mut handle,
            executor: ptr::null_mut(),
            outcome: &mut outcome,
            completion: ffi::ustore_completion_t::Blocking,
            directory: directory.as_ptr(),
        };
        // SAFETY: every pointer in `arguments` outlives the call, the directory included.
        unsafe { ffi::ustore_database_init_leveldb(&mut arguments) };
        Database::opened(handle, outcome)
    }

    /// Opens the NVMe engine, or fails with [`Status::Unsupported`] in a build without it.
    pub fn open_nvme(options: &NVMEOptions<'_>) -> Result<Self> {
        let directory = c_path(options.directory)?;
        let (_paths, data_directories) = c_data_directories(options.data_directories)?;
        let mut handle: ffi::ustore_database_t = ptr::null_mut();
        let mut outcome = ffi::ustore_outcome_t::default();
        let mut arguments = ffi::ustore_database_init_nvme_t {
            database: &mut handle,
            executor: ptr::null_mut(),
            outcome: &mut outcome,
            completion: ffi::ustore_completion_t::Blocking,
            directory: directory.as_ptr(),
            data_directories: data_directories.as_ptr(),
            data_directories_count: data_directories.len() as ffi::ustore_size_t,
            transport: match options.transport {
                Transport::Automatic => ffi::USTORE_TRANSPORT_AUTOMATIC_K,
                Transport::Blocking => ffi::USTORE_TRANSPORT_BLOCKING_K,
                Transport::LinuxUring => ffi::USTORE_TRANSPORT_LINUX_URING_K,
                Transport::LinuxAio => ffi::USTORE_TRANSPORT_LINUX_AIO_K,
                Transport::LinuxSpdk => ffi::USTORE_TRANSPORT_LINUX_SPDK_K,
            },
            memory_limit: options.memory_limit,
            gpu_devices: options.gpu_devices.as_ptr(),
            gpu_devices_count: options.gpu_devices.len() as ffi::ustore_size_t,
            gpu_memory_limit: options.gpu_memory_limit,
            shared_io_channels: options.shared_io_channels,
            queue_depth: options.queue_depth,
            request_buffer_bytes: options.request_buffer_bytes,
            request_buffers_count: options.request_buffers_count,
            stream_buffer_bytes: options.stream_buffer_bytes,
            stream_buffers_count: options.stream_buffers_count,
            transaction_max_bytes: options.transaction_max_bytes,
            transaction_max_elements: options.transaction_max_elements,
            collection_concurrency_limit: options.collection_concurrency_limit,
            collection_cache_limit: options.collection_cache_limit,
            collection_write_buffer_max_bytes: options.collection_write_buffer_max_bytes,
            collection_write_buffer_max_elements: options.collection_write_buffer_max_elements,
            collection_first_level_max_bytes: options.collection_first_level_max_bytes,
            collection_level_enlarge_factor: options.collection_level_enlarge_factor,
            collection_value_size: options.collection_value_size,
            collection_value_max_size: options.collection_value_max_size,
        };
        // SAFETY: every pointer in `arguments` outlives the call, the paths and devices included.
        unsafe { ffi::ustore_database_init_nvme(&mut arguments) };
        Database::opened(handle, outcome)
    }

    /// The database an init call opened, or the failure it reported.
    fn opened(handle: ffi::ustore_database_t, outcome: ffi::ustore_outcome_t) -> Result<Self> {
        check(outcome)?;
        match handle.is_null() {
            true => Err(fail(Status::Internal, "the library returned no database and no error")),
            false => Ok(Database { handle }),
        }
    }

    /// What the engine behind this database supports.
    pub fn capabilities(&self) -> Result<Capabilities> {
        let mut description = ffi::ustore_database_description_t { capabilities: 0 };
        let mut outcome = ffi::ustore_outcome_t::default();
        let mut arguments = ffi::ustore_database_describe_t {
            database: self.handle,
            executor: ptr::null_mut(),
            outcome: &mut outcome,
            completion: ffi::ustore_completion_t::Blocking,
            description: &mut description,
        };
        // SAFETY: every pointer in `arguments` outlives the call.
        unsafe { ffi::ustore_database_describe(&mut arguments) };
        check(outcome)?;
        Ok(Capabilities {
            bits: description.capabilities,
        })
    }

    /// Sends a free-form request to the engine - "clear", "reset", "compact", "info" or "usage" -
    /// and borrows its answer out of the arena.
    pub fn control<'arena>(&self, arena: &'arena mut Arena, request: &str) -> Result<&'arena str> {
        let request =
            CString::new(request).map_err(|_| fail(Status::Malformed, "the request holds a null character"))?;
        let mut response: ffi::ustore_str_view_t = ptr::null();
        let mut outcome = ffi::ustore_outcome_t::default();
        let mut arguments = ffi::ustore_database_control_t {
            database: self.handle,
            executor: ptr::null_mut(),
            outcome: &mut outcome,
            completion: ffi::ustore_completion_t::Blocking,
            request: request.as_ptr(),
            response: &mut response,
            arena: arena.slot(),
            progress: None,
            progress_state: ptr::null_mut(),
        };
        // SAFETY: every pointer in `arguments` outlives the call, the request included.
        unsafe { ffi::ustore_database_control(&mut arguments) };
        check(outcome)?;
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
            view: ptr::null_mut(),
            transaction: ptr::null_mut(),
            snapshot: ptr::null_mut(),
            watch: Conflicts::Watched,
            marker: PhantomData,
        }
    }

    /// Begins a transaction over this database, which conflicts rather than blocks.
    pub fn transaction(&self) -> Result<Transaction<'_>> {
        let mut handle: ffi::ustore_transaction_t = ptr::null_mut();
        begin(self.handle, &mut handle)?;
        Ok(Transaction { database: self, handle })
    }

    /// Pins a point-in-time view of the database, which it releases when dropped.
    pub fn snapshot(&self) -> Result<Snapshot<'_>> {
        let mut handle: ffi::ustore_snapshot_t = ptr::null_mut();
        let mut outcome = ffi::ustore_outcome_t::default();
        let mut arguments = ffi::ustore_snapshot_init_t {
            snapshot: &mut handle,
            database: self.handle,
            executor: ptr::null_mut(),
            outcome: &mut outcome,
            completion: ffi::ustore_completion_t::Blocking,
        };
        // SAFETY: every pointer in `arguments` outlives the call.
        unsafe { ffi::ustore_snapshot_init(&mut arguments) };
        check(outcome)?;
        match handle.is_null() {
            true => Err(fail(Status::Internal, "the library returned no snapshot and no error")),
            false => Ok(Snapshot { database: self, handle }),
        }
    }

    /// Creates an empty collection holding `modality` and binds `name` to it, refusing a name already in use.
    pub fn create_collection(&self, name: &str, modality: Modality) -> Result<CollectionID> {
        crate::strings::create_named(self.handle, name, modality)
    }

    /// Drops the collection `name` addresses, together with its contents and its name.
    pub fn drop_collection(&self, name: &str) -> Result<()> {
        let (collection, _) = self.head().collection(name)?;
        self.clear_collection(collection, Removed::CollectionAndEntries)
    }

    /// Removes the contents of a collection, or the collection itself together with its name.
    pub fn clear_collection(&self, collection: CollectionID, removed: Removed) -> Result<()> {
        let mut outcome = ffi::ustore_outcome_t::default();
        let mut arguments = ffi::ustore_collection_drop_t {
            database: self.handle,
            executor: ptr::null_mut(),
            outcome: &mut outcome,
            completion: ffi::ustore_completion_t::Blocking,
            collection,
            mode: removed as ffi::ustore_enum_t,
        };
        // SAFETY: every pointer in `arguments` outlives the call.
        unsafe { ffi::ustore_collection_drop(&mut arguments) };
        check(outcome)
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
/// Every read, write and scan hangs off this type, so a call naming two views at once cannot be written. Reads
/// pass the `ustore_view_t` it holds, writes pass its transaction and whether that watches them, and the view is
/// as cheap to copy as the handles it borrows from its parent.
#[derive(Clone, Copy, Debug)]
pub struct View<'view> {
    database: ffi::ustore_database_t,
    view: ffi::ustore_view_t,
    transaction: ffi::ustore_transaction_t,
    snapshot: ffi::ustore_snapshot_t,
    watch: Conflicts,
    marker: PhantomData<&'view Database>,
}

impl<'view> View<'view> {
    /// The database every call through this view reaches.
    pub(crate) fn database(&self) -> ffi::ustore_database_t {
        self.database
    }

    /// What every read through this view sees, null at the head.
    pub(crate) fn view(&self) -> ffi::ustore_view_t {
        self.view
    }

    /// The transaction every write through this view joins, null at the head, refusing a snapshot.
    pub(crate) fn transaction(&self) -> Result<ffi::ustore_transaction_t> {
        match self.snapshot.is_null() {
            true => Ok(self.transaction),
            false => Err(fail(Status::InvalidArgument, "nothing writes through a snapshot")),
        }
    }

    /// Whether the transaction records what the writes through this view touch.
    pub(crate) fn watch(&self) -> Conflicts {
        self.watch
    }

    /// The snapshot a listing through this view reads, null at the head and inside a transaction, whose listings
    /// read the head as well.
    pub(crate) fn snapshot(&self) -> ffi::ustore_snapshot_t {
        self.snapshot
    }

    /// Every collection the database holds at this view's snapshot or at the head, the main one excluded, exported
    /// into the arena.
    pub fn collections<'arena>(&self, arena: &'arena mut Arena) -> Result<&'arena [CollectionID]> {
        let mut count: ffi::ustore_size_t = 0;
        let mut ids: *mut CollectionID = ptr::null_mut();
        let mut outcome = ffi::ustore_outcome_t::default();
        let mut arguments = ffi::ustore_collection_list_t {
            database: self.database,
            executor: ptr::null_mut(),
            outcome: &mut outcome,
            completion: ffi::ustore_completion_t::Blocking,
            snapshot: self.snapshot,
            count: &mut count,
            collections: &mut ids,
            modalities: ptr::null_mut(),
            names_offsets: ptr::null_mut(),
            names: ptr::null_mut(),
            arena: arena.slot(),
        };
        // SAFETY: every pointer in `arguments` outlives the call.
        unsafe { ffi::ustore_collection_list(&mut arguments) };
        check(outcome)?;
        // SAFETY: a successful listing writes `count` identifiers into the arena, which stays
        // borrowed for as long as the slice does.
        Ok(unsafe { exported_slice(ids, count as usize) })
    }
}

// endregion: Views

// region: Transactions

/// Begins a transaction, or resets the one the slot already holds.
fn begin(database: ffi::ustore_database_t, handle: &mut ffi::ustore_transaction_t) -> Result<()> {
    let mut outcome = ffi::ustore_outcome_t::default();
    let mut arguments = ffi::ustore_transaction_init_t {
        transaction: handle,
        database,
        executor: ptr::null_mut(),
        outcome: &mut outcome,
        completion: ffi::ustore_completion_t::Blocking,
    };
    // SAFETY: every pointer in `arguments` outlives the call.
    unsafe { ffi::ustore_transaction_init(&mut arguments) };
    check(outcome)
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
    /// The view every call inside this transaction addresses, watching what it touches or not.
    pub fn view(&self, conflicts: Conflicts) -> View<'_> {
        View {
            database: self.database.handle(),
            // SAFETY: the transaction is open, and its view tags the handle without allocating.
            view: unsafe { ffi::ustore_transaction_view(self.handle, conflicts) },
            transaction: self.handle,
            snapshot: ptr::null_mut(),
            watch: conflicts,
            marker: PhantomData,
        }
    }

    /// Stages the writes as the first phase of a two-phase commit, reporting the stamp the stage took.
    pub fn stage(&mut self) -> Result<Stamp> {
        let mut stamp: Stamp = 0;
        let mut outcome = ffi::ustore_outcome_t::default();
        let mut arguments = ffi::ustore_transaction_stage_t {
            database: self.database.handle(),
            executor: ptr::null_mut(),
            outcome: &mut outcome,
            completion: ffi::ustore_completion_t::Blocking,
            transaction: self.handle,
            stamp: &mut stamp,
        };
        // SAFETY: every pointer in `arguments` outlives the call.
        unsafe { ffi::ustore_transaction_stage(&mut arguments) };
        check(outcome)?;
        Ok(stamp)
    }

    /// Commits the writes, reporting the stamp they were published under.
    ///
    /// A conflict is reported rather than retried, and the transaction keeps its contents either way, so a caller
    /// may stage them again after [`Transaction::rollback`] or start over after [`Transaction::reset`].
    pub fn commit(&mut self, durability: Durability) -> Result<Stamp> {
        let mut stamp: Stamp = 0;
        let mut outcome = ffi::ustore_outcome_t::default();
        let mut arguments = ffi::ustore_transaction_commit_t {
            database: self.database.handle(),
            executor: ptr::null_mut(),
            outcome: &mut outcome,
            completion: ffi::ustore_completion_t::Blocking,
            transaction: self.handle,
            stamp: &mut stamp,
            durability,
        };
        // SAFETY: every pointer in `arguments` outlives the call.
        unsafe { ffi::ustore_transaction_commit(&mut arguments) };
        check(outcome)?;
        Ok(stamp)
    }

    /// Withdraws a stage, releasing its stamp, while keeping the reads and writes to stage again.
    pub fn rollback(&mut self) -> Result<()> {
        let mut outcome = ffi::ustore_outcome_t::default();
        let mut arguments = ffi::ustore_transaction_rollback_t {
            database: self.database.handle(),
            executor: ptr::null_mut(),
            outcome: &mut outcome,
            completion: ffi::ustore_completion_t::Blocking,
            transaction: self.handle,
        };
        // SAFETY: every pointer in `arguments` outlives the call.
        unsafe { ffi::ustore_transaction_rollback(&mut arguments) };
        check(outcome)
    }

    /// Discards the reads and writes and reopens the transaction on the newest state, ready for another try.
    pub fn reset(&mut self) -> Result<()> {
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

// SAFETY: a transaction may move to another thread with its database borrow; it stays `!Sync`, so no two
// threads ever call through one at once, which is all the C interface asks of it.
unsafe impl Send for Transaction<'_> {}

// endregion: Transactions

// region: Snapshots

/// A point-in-time view of the whole database, which pins the versions its reads need until it is
/// dropped.
///
/// Every view at a snapshot borrows it, so the snapshot outlives them, and the snapshot borrows its
/// database, which it is freed before.
#[derive(Debug)]
pub struct Snapshot<'db> {
    database: &'db Database,
    handle: ffi::ustore_snapshot_t,
}

impl<'db> Snapshot<'db> {
    /// The view every call at this snapshot addresses, which no write may go through.
    pub fn view(&self) -> View<'_> {
        View {
            database: self.database.handle(),
            // SAFETY: the snapshot is pinned, and its view tags the handle without allocating.
            view: unsafe { ffi::ustore_snapshot_view(self.handle) },
            transaction: ptr::null_mut(),
            snapshot: self.handle,
            watch: Conflicts::Unwatched,
            marker: PhantomData,
        }
    }

    /// Writes this snapshot into an existing directory as a database of its own.
    pub fn export(&self, path: &str) -> Result<()> {
        let path = CString::new(path).map_err(|_| fail(Status::Malformed, "the path holds a null character"))?;
        let mut outcome = ffi::ustore_outcome_t::default();
        let mut arguments = ffi::ustore_snapshot_export_t {
            database: self.database.handle(),
            executor: ptr::null_mut(),
            outcome: &mut outcome,
            completion: ffi::ustore_completion_t::Blocking,
            snapshot: self.handle,
            path: path.as_ptr(),
            progress: None,
            progress_state: ptr::null_mut(),
        };
        // SAFETY: every pointer in `arguments` outlives the call, the path included.
        unsafe { ffi::ustore_snapshot_export(&mut arguments) };
        check(outcome)
    }
}

impl Drop for Snapshot<'_> {
    fn drop(&mut self) {
        // SAFETY: the handle came from a successful init, is freed once, and outlives no view - every
        // view borrows this snapshot.
        unsafe { ffi::ustore_snapshot_free(self.handle) };
    }
}

// SAFETY: a snapshot may move to another thread with its database borrow; it stays `!Sync`, since the C
// interface promises concurrent calls only through a database.
unsafe impl Send for Snapshot<'_> {}

// endregion: Snapshots

// region: Tests

#[cfg(test)]
mod tests {
    use super::*;
    use crate::testing::temporary_directory;
    use crate::testing::{key, keys};

    #[test]
    fn databases_open_in_memory_and_report_their_engine() {
        let database = Database::open_ram(&RAMOptions::default()).unwrap();
        let capabilities = database.capabilities().unwrap();
        assert!(capabilities.supports_transactions());
        assert!(capabilities.supports_collections());
        assert!(capabilities.supports_snapshots());

        let mut arena = Arena::new();
        let described = database.control(&mut arena, "info").unwrap();
        assert!(described.contains("ram"), "{described}");
    }

    #[test]
    fn databases_open_at_a_directory_and_keep_what_they_held() {
        let directory = temporary_directory("database_reopens");
        let options = RAMOptions::at(directory.path());
        let mut arena = Arena::new();

        let database = Database::open_ram(&options).unwrap();
        let entries = [crate::blobs::Entry::new(key(7), b"alpha").unwrap()];
        database
            .head()
            .write(CollectionID::main(), &entries, Durability::Flushed)
            .unwrap();
        drop(database);

        let reopened = Database::open_ram(&options).unwrap();
        let values = reopened
            .head()
            .read(&mut arena, CollectionID::main(), &keys([7]))
            .unwrap();
        assert_eq!(values.get(0), Some(b"alpha".as_slice()));
    }

    #[test]
    fn databases_refuse_a_directory_that_is_not_there() {
        let failure = Database::open_ram(&RAMOptions::at("/nonexistent/ustore/directory")).unwrap_err();
        assert_eq!(failure.status, Status::NotFound);
        let failure = Database::open_ram(&RAMOptions::at("/tmp/a\0b")).unwrap_err();
        assert_eq!(failure.status, Status::Malformed);
    }

    #[test]
    fn databases_open_at_either_isolation() {
        for isolation in [Isolation::StrictSerializable, Isolation::Snapshot] {
            let options = RAMOptions {
                isolation,
                ..RAMOptions::default()
            };
            assert!(Database::open_ram(&options)
                .unwrap()
                .capabilities()
                .unwrap()
                .supports_transactions());
        }
    }

    #[test]
    fn databases_open_on_every_engine_or_report_it_missing() {
        let rocksdb = temporary_directory("database_rocksdb");
        let leveldb = temporary_directory("database_leveldb");
        let nvme = temporary_directory("database_nvme");
        let opened = [
            (
                Database::open_rocksdb(&RocksDBOptions::at(rocksdb.path())),
                cfg!(feature = "rocksdb"),
            ),
            (
                Database::open_leveldb(&LevelDBOptions::at(leveldb.path())),
                cfg!(feature = "leveldb"),
            ),
            (
                Database::open_nvme(&NVMEOptions::at(nvme.path())),
                cfg!(feature = "nvme"),
            ),
        ];
        let mut arena = Arena::new();
        for (database, compiled) in opened {
            if !compiled {
                assert_eq!(database.unwrap_err().status, Status::Unsupported);
                continue;
            }
            let database = database.unwrap();
            let entries = [crate::blobs::Entry::new(key(7), b"alpha").unwrap()];
            database
                .head()
                .write(CollectionID::main(), &entries, Durability::Flushed)
                .unwrap();
            let values = database
                .head()
                .read(&mut arena, CollectionID::main(), &keys([7]))
                .unwrap();
            assert_eq!(values.get(0), Some(b"alpha".as_slice()));
        }
    }

    #[test]
    fn engines_refuse_a_request_they_do_not_know() {
        let database = Database::open_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new();
        let failure = database.control(&mut arena, "levitate").unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);
        assert!(!failure.message.is_empty());
    }

    #[test]
    fn collections_are_created_listed_and_dropped_by_name() {
        let database = Database::open_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new();

        let people = database.create_collection("people", Modality::Any).unwrap();
        let places = database.create_collection("places", Modality::Graph).unwrap();
        assert_ne!(people, places);
        assert_eq!(database.head().collection("people").unwrap(), (people, Modality::Any));
        assert_eq!(
            database.head().collection("places").unwrap(),
            (places, Modality::Graph),
            "a name resolves to the modality it was created with"
        );

        let listed = database.head().collections(&mut arena).unwrap();
        assert_eq!(listed.len(), 2, "the main collection is never listed");
        assert!(listed.contains(&people) && listed.contains(&places));

        // A name is bound once, and a name nobody bound addresses nothing.
        assert_eq!(
            database.create_collection("people", Modality::Any).unwrap_err().status,
            Status::AlreadyExists
        );
        assert_eq!(
            database.head().collection("nowhere").unwrap_err().status,
            Status::NotFound
        );

        database.drop_collection("people").unwrap();
        assert_eq!(
            database.head().collection("people").unwrap_err().status,
            Status::NotFound
        );
        assert_eq!(database.head().collections(&mut arena).unwrap().len(), 1);
    }

    #[test]
    fn the_main_collection_clears_but_never_drops() {
        let database = Database::open_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new();
        let entries = [crate::blobs::Entry::new(key(1), b"alpha").unwrap()];
        database
            .head()
            .write(CollectionID::main(), &entries, Durability::Buffered)
            .unwrap();

        let failure = database
            .clear_collection(CollectionID::main(), Removed::CollectionAndEntries)
            .unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);

        database
            .clear_collection(CollectionID::main(), Removed::Entries)
            .unwrap();
        let values = database
            .head()
            .read(&mut arena, CollectionID::main(), &keys([1]))
            .unwrap();
        assert_eq!(values.get(0), None);
    }

    #[test]
    fn transactions_commit_what_they_wrote() {
        let database = Database::open_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new();
        let mut transaction = database.transaction().unwrap();
        let entries = [crate::blobs::Entry::new(key(1), b"alpha").unwrap()];
        transaction
            .view(Conflicts::Watched)
            .write(CollectionID::main(), &entries, Durability::Buffered)
            .unwrap();

        // Nothing is visible at the head until the commit lands.
        let before = database
            .head()
            .read(&mut arena, CollectionID::main(), &keys([1]))
            .unwrap();
        assert_eq!(before.get(0), None);

        let staged = transaction.stage().unwrap();
        let committed = transaction.commit(Durability::Buffered).unwrap();
        assert!(committed >= staged);

        let after = database
            .head()
            .read(&mut arena, CollectionID::main(), &keys([1]))
            .unwrap();
        assert_eq!(after.get(0), Some(b"alpha".as_slice()));
    }

    #[test]
    fn transactions_reset_and_run_again() {
        let database = Database::open_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new();
        let mut transaction = database.transaction().unwrap();
        let abandoned = [crate::blobs::Entry::new(key(2), b"abandoned").unwrap()];
        let other = [crate::blobs::Entry::new(key(3), b"abandoned").unwrap()];
        transaction
            .view(Conflicts::Watched)
            .write(CollectionID::main(), &abandoned, Durability::Buffered)
            .unwrap();
        transaction
            .view(Conflicts::Watched)
            .write(CollectionID::main(), &other, Durability::Buffered)
            .unwrap();
        transaction.reset().unwrap();

        let kept = [crate::blobs::Entry::new(key(2), b"kept").unwrap()];
        transaction
            .view(Conflicts::Watched)
            .write(CollectionID::main(), &kept, Durability::Buffered)
            .unwrap();
        transaction.commit(Durability::Buffered).unwrap();

        let values = database
            .head()
            .read(&mut arena, CollectionID::main(), &keys([2, 3]))
            .unwrap();
        assert_eq!(values.get(0), Some(b"kept".as_slice()));
        assert_eq!(values.get(1), None, "a reset forgets every write before it");
    }

    #[test]
    fn transactions_roll_back_a_stage_and_keep_their_writes() {
        let database = Database::open_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new();
        let mut transaction = database.transaction().unwrap();
        let kept = [crate::blobs::Entry::new(key(2), b"kept").unwrap()];
        transaction
            .view(Conflicts::Watched)
            .write(CollectionID::main(), &kept, Durability::Buffered)
            .unwrap();
        transaction.stage().unwrap();
        transaction.rollback().unwrap();
        transaction.commit(Durability::Buffered).unwrap();

        let values = database
            .head()
            .read(&mut arena, CollectionID::main(), &keys([2]))
            .unwrap();
        assert_eq!(values.get(0), Some(b"kept".as_slice()));
    }

    #[test]
    fn transactions_refuse_a_flush_of_their_own() {
        let database = Database::open_ram(&RAMOptions::default()).unwrap();
        let transaction = database.transaction().unwrap();
        let entries = [crate::blobs::Entry::new(key(3), b"alpha").unwrap()];
        let failure = transaction
            .view(Conflicts::Watched)
            .write(CollectionID::main(), &entries, Durability::Flushed)
            .unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);
    }

    #[test]
    fn transactions_conflict_rather_than_block() {
        let database = Database::open_ram(&RAMOptions::default()).unwrap();
        let mut first = database.transaction().unwrap();
        let mut second = database.transaction().unwrap();

        let mine = [crate::blobs::Entry::new(key(4), b"mine").unwrap()];
        let yours = [crate::blobs::Entry::new(key(4), b"yours").unwrap()];
        first
            .view(Conflicts::Watched)
            .write(CollectionID::main(), &mine, Durability::Buffered)
            .unwrap();
        second
            .view(Conflicts::Watched)
            .write(CollectionID::main(), &yours, Durability::Buffered)
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
    fn an_unwatched_view_records_nothing_to_conflict_over() {
        // Only snapshot isolation adds a watch of its own; a strictly serializable read is ordered anyway.
        let database = Database::open_ram(&RAMOptions {
            isolation: Isolation::Snapshot,
            ..RAMOptions::default()
        })
        .unwrap();
        let mut arena = Arena::new();
        let seed = [crate::blobs::Entry::new(key(8), b"seed").unwrap()];
        database
            .head()
            .write(CollectionID::main(), &seed, Durability::Buffered)
            .unwrap();

        // A watched read is part of the serial order, so a writer that lands under it conflicts.
        let mut watched = database.transaction().unwrap();
        let read = watched
            .view(Conflicts::Watched)
            .read(&mut arena, CollectionID::main(), &keys([8]))
            .unwrap();
        assert_eq!(read.get(0), Some(b"seed".as_slice()));
        let overwritten = [crate::blobs::Entry::new(key(8), b"overwritten").unwrap()];
        database
            .head()
            .write(CollectionID::main(), &overwritten, Durability::Buffered)
            .unwrap();
        let failure = watched.commit(Durability::Buffered).unwrap_err();
        assert!(
            matches!(failure.status, Status::ReadConflict | Status::WriteConflict),
            "{failure:?}"
        );

        // The same run through an unwatched view records nothing, so the commit stands.
        let mut unwatched = database.transaction().unwrap();
        let read = unwatched
            .view(Conflicts::Unwatched)
            .read(&mut arena, CollectionID::main(), &keys([8]))
            .unwrap();
        assert_eq!(read.get(0), Some(b"overwritten".as_slice()));
        let again = [crate::blobs::Entry::new(key(8), b"again").unwrap()];
        database
            .head()
            .write(CollectionID::main(), &again, Durability::Buffered)
            .unwrap();
        unwatched.commit(Durability::Buffered).unwrap();
    }

    #[test]
    fn snapshots_hold_the_state_they_were_taken_at() {
        let database = Database::open_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new();
        let before = [crate::blobs::Entry::new(key(5), b"before").unwrap()];
        database
            .head()
            .write(CollectionID::main(), &before, Durability::Buffered)
            .unwrap();

        let snapshot = database.snapshot().unwrap();
        let after = [crate::blobs::Entry::new(key(5), b"after").unwrap()];
        database
            .head()
            .write(CollectionID::main(), &after, Durability::Buffered)
            .unwrap();

        let pinned = snapshot
            .view()
            .read(&mut arena, CollectionID::main(), &keys([5]))
            .unwrap();
        assert_eq!(pinned.get(0), Some(b"before".as_slice()));
        let latest = database
            .head()
            .read(&mut arena, CollectionID::main(), &keys([5]))
            .unwrap();
        assert_eq!(latest.get(0), Some(b"after".as_slice()));

        let failure = snapshot
            .view()
            .write(CollectionID::main(), &after, Durability::Buffered)
            .unwrap_err();
        assert_eq!(
            failure.status,
            Status::InvalidArgument,
            "nothing writes through a snapshot"
        );
    }

    #[test]
    fn snapshots_export_a_database_of_their_own() {
        let database = Database::open_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new();
        let entries = [crate::blobs::Entry::new(key(6), b"exported").unwrap()];
        database
            .head()
            .write(CollectionID::main(), &entries, Durability::Buffered)
            .unwrap();

        let directory = temporary_directory("snapshot_exports");
        let snapshot = database.snapshot().unwrap();
        snapshot.export(directory.path()).unwrap();
        assert_eq!(
            snapshot.export("/nonexistent/ustore/directory").unwrap_err().status,
            Status::NotFound
        );

        let exported = Database::open_ram(&RAMOptions::at(directory.path())).unwrap();
        let values = exported
            .head()
            .read(&mut arena, CollectionID::main(), &keys([6]))
            .unwrap();
        assert_eq!(values.get(0), Some(b"exported".as_slice()));
    }

    #[test]
    fn statuses_map_and_tolerate_newer_libraries() {
        let written = ffi::ustore_outcome_t {
            status: ffi::ustore_status_t(7),
            message: c"No such collection".as_ptr(),
        };
        let failure = check(written).unwrap_err();
        assert_eq!(failure.status, Status::NotFound);
        assert_eq!(failure.message, "No such collection");
        assert_eq!(failure.to_string(), "No such collection");
        assert_eq!(failure.status.name(), "not-found");

        let contended = ffi::ustore_outcome_t {
            status: ffi::ustore_status_t(16),
            message: c"The arena serves another call".as_ptr(),
        };
        assert_eq!(check(contended).unwrap_err().status, Status::Contended);
        assert_eq!(Status::Cancelled.name(), "cancelled");

        let newer = ffi::ustore_outcome_t {
            status: ffi::ustore_status_t(4242),
            message: ptr::null(),
        };
        assert_eq!(check(newer).unwrap_err().status, Status::Unrecognized);
        assert!(check(ffi::ustore_outcome_t::default()).is_ok());
    }

    #[test]
    fn constants_come_from_the_linked_engine() {
        assert_eq!(CollectionID::main(), CollectionID::default());
        assert_eq!(missing_length(), u64::MAX);
        assert_eq!(key_unknown(), Key::from(u128::MAX));
    }
}

// endregion: Tests
