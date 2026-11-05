//! Databases, views, transactions, snapshots, arenas and executors - the Rust twin of `c/ustore/types.h` and
//! `c/ustore/db.h`.
//!
//! Every call addresses exactly one view of the data: the head of the database, a transaction, or a
//! snapshot. [`View`] is that view, so a call naming two cannot be spelled here, and its parent type names which one
//! it is, so a verb only one of them serves, such as a commit, a collection drop or a write, cannot be spelled on the
//! others. Results land in an [`Arena`] an executor makes and the next call reuses, which is why a call borrows it
//! mutably and every exported result borrows it back.
//!
//! The view also chooses how its verbs complete: every verb of a [`View`] blocks and answers what its call produced,
//! while the same verb of a [`DeferredView`] submits the call to an [`Executor`] and answers a [`Deferred`] at once.
//!
//! File: rust/db.rs
//! Author: Ash Vardanian
//! Date: September 16, 2026

use std::any::Any;
use std::ffi::{c_char, c_void, CStr};
use std::fmt;
use std::future::{Future, IntoFuture};
use std::iter::FusedIterator;
use std::marker::PhantomData;
use std::mem::ManuallyDrop;
use std::ops::{ControlFlow, Range};
use std::panic::AssertUnwindSafe;
use std::pin::Pin;
use std::ptr::{self, NonNull};
use std::str::FromStr;
use std::task::{Context, Poll};

use stringtape::{BytesTape, BytesTapeViewU32, BytesTapeViewU64, CharsTape};

use crate::docs::{Column, KeyedBy};
use crate::graph::{Edge, Roles};

// region: Raw Declarations

/// One-to-one mirror of `c/ustore/types.h` and `c/ustore/db.h`.
pub(crate) mod ffi {
    #![allow(non_camel_case_types)]

    use std::ffi::{c_char, c_int, c_void};
    use std::fmt;
    use std::ptr;

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
    pub type ustore_watch_t = super::Watch;
    pub type ustore_mood_t = super::Mood;
    pub type ustore_gds_t = super::Gds;
    pub type ustore_scan_direction_t = super::ScanDirection;
    pub type ustore_key_t = super::Key;
    pub type ustore_byte_t = u8;
    pub type ustore_length_t = u64;
    pub type ustore_size_t = u64;
    pub type ustore_octet_t = u8;
    pub type ustore_stamp_t = u64;

    /// The `ustore_drop_mode_t`, `ustore_capabilities_t`, `ustore_isolation_t`, `ustore_transport_t`,
    /// `ustore_engine_t`, `ustore_modality_t`, `ustore_continuation_t` and `ustore_strings_layout_t` enumerations,
    /// each of which lowers to the platform `int`.
    pub type ustore_enum_t = c_int;

    /// A borrowed run of bytes and its length, never null-terminated.
    #[repr(C)]
    #[derive(Clone, Copy, Debug)]
    pub struct ustore_bytes_view_t {
        pub data: *const u8,
        pub length: ustore_size_t,
    }

    impl Default for ustore_bytes_view_t {
        fn default() -> Self {
            ustore_bytes_view_t {
                data: ptr::null(),
                length: 0,
            }
        }
    }

    impl From<&[u8]> for ustore_bytes_view_t {
        fn from(bytes: &[u8]) -> Self {
            ustore_bytes_view_t {
                data: bytes.as_ptr(),
                length: bytes.len() as ustore_size_t,
            }
        }
    }

    /// The shape `ustore_keys_view_t`, `ustore_collections_view_t` and every other typed strided view shares: one
    /// element per task, `stride` bytes apart, where a zero stride repeats the first.
    #[repr(C)]
    #[derive(Clone, Copy, Debug)]
    pub struct ustore_strided_view_t<Element> {
        pub data: *const Element,
        pub stride: ustore_size_t,
    }

    impl<Element> ustore_strided_view_t<Element> {
        /// The run of `elements` packed one after another.
        pub fn packed(elements: &[Element]) -> Self {
            ustore_strided_view_t {
                data: elements.as_ptr(),
                stride: size_of::<Element>() as ustore_size_t,
            }
        }

        /// The run repeating `element` for every task.
        pub fn repeated(element: &Element) -> Self {
            ustore_strided_view_t {
                data: element,
                stride: 0,
            }
        }

        /// The run of one `Element` inside each of `records`, starting at `first` inside the first record.
        pub fn inside<Record>(first: &Element) -> Self {
            ustore_strided_view_t {
                data: first,
                stride: size_of::<Record>() as ustore_size_t,
            }
        }
    }

    impl<Element> Default for ustore_strided_view_t<Element> {
        fn default() -> Self {
            ustore_strided_view_t {
                data: ptr::null(),
                stride: 0,
            }
        }
    }

    pub type ustore_keys_view_t = ustore_strided_view_t<ustore_key_t>;
    pub type ustore_collections_view_t = ustore_strided_view_t<ustore_collection_id_t>;
    pub type ustore_lengths_view_t = ustore_strided_view_t<ustore_length_t>;

    /// Writes the views of the strings at `indices` into `views`, for a batch no tape holds.
    pub type ustore_strings_get_t = unsafe extern "C" fn(
        handle: *const c_void,
        indices: *const ustore_size_t,
        count: ustore_size_t,
        views: *mut ustore_bytes_view_t,
    );

    pub const USTORE_STRINGS_U32_TAPE_K: ustore_enum_t = 0;
    pub const USTORE_STRINGS_U64_TAPE_K: ustore_enum_t = 1;
    pub const USTORE_STRINGS_ACCESSOR_K: ustore_enum_t = 2;

    /// The accessor of a `ustore_strings_accessor_k` batch.
    #[repr(C)]
    #[derive(Clone, Copy)]
    pub struct ustore_strings_accessor_t {
        pub get: Option<ustore_strings_get_t>,
        pub handle: *const c_void,
    }

    /// The tape of a `ustore_strings_u32_tape_k` or a `ustore_strings_u64_tape_k` batch.
    #[repr(C)]
    #[derive(Clone, Copy)]
    pub struct ustore_strings_tape_t<Offset> {
        pub data: *const u8,
        pub offsets: *const Offset,
    }

    /// The `data` union of `ustore_strings_view_t`, read through the member its `layout` names.
    #[repr(C)]
    #[derive(Clone, Copy)]
    pub union ustore_strings_data_t {
        pub accessor: ustore_strings_accessor_t,
        pub u32_tape: ustore_strings_tape_t<u32>,
        pub u64_tape: ustore_strings_tape_t<u64>,
    }

    /// A ragged batch of byte strings in one of three layouts, where a zeroed view stands for an input's default.
    #[repr(C)]
    #[derive(Clone, Copy)]
    pub struct ustore_strings_view_t {
        pub layout: ustore_enum_t,
        pub data: ustore_strings_data_t,
    }

    impl ustore_strings_view_t {
        /// The strings of an Arrow tape of 32-bit offsets.
        pub fn u32_tape(data: *const u8, offsets: *const u32) -> Self {
            ustore_strings_view_t {
                layout: USTORE_STRINGS_U32_TAPE_K,
                data: ustore_strings_data_t {
                    u32_tape: ustore_strings_tape_t { data, offsets },
                },
            }
        }

        /// The strings of an Arrow tape of 64-bit offsets.
        pub fn u64_tape(data: *const u8, offsets: *const u64) -> Self {
            ustore_strings_view_t {
                layout: USTORE_STRINGS_U64_TAPE_K,
                data: ustore_strings_data_t {
                    u64_tape: ustore_strings_tape_t { data, offsets },
                },
            }
        }

        /// The strings `get` finds in `handle`.
        pub fn accessor(get: ustore_strings_get_t, handle: *const c_void) -> Self {
            ustore_strings_view_t {
                layout: USTORE_STRINGS_ACCESSOR_K,
                data: ustore_strings_data_t {
                    accessor: ustore_strings_accessor_t { get: Some(get), handle },
                },
            }
        }
    }

    impl Default for ustore_strings_view_t {
        fn default() -> Self {
            ustore_strings_view_t::u32_tape(ptr::null(), ptr::null())
        }
    }

    impl fmt::Debug for ustore_strings_view_t {
        fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
            write!(formatter, "ustore_strings_view_t({})", self.layout)
        }
    }

    // SAFETY: a view addresses an input or an output of the call that holds it, which moves between threads with
    // the call, and its accessor reads nothing but what its handle addresses.
    unsafe impl Send for ustore_strings_view_t {}

    /// The `ustore_status_t` a call stores, kept as its integer, since a value no variant of a Rust enum names is
    /// undefined behavior to read.
    #[repr(transparent)]
    #[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
    pub struct ustore_status_t(pub c_int);

    impl ustore_status_t {
        pub const SUCCESS: ustore_status_t = ustore_status_t(0);
        pub const DEFERRED: ustore_status_t = ustore_status_t(1);
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

    /// Whether a call returns final or once submitted.
    #[repr(C)]
    #[derive(Clone, Copy)]
    pub enum ustore_completion_t {
        Blocking = 0,
        Deferred = 1,
    }

    /// In what order a scan hands its keys over, where the binding only ever asks for order.
    #[repr(C)]
    #[allow(dead_code)]
    pub enum ustore_scan_mode_t {
        Ordered = 0,
        Unordered = 1,
    }

    /// A progress callback answering a `ustore_continuation_t`.
    pub type ustore_progress_callback_t =
        Option<unsafe extern "C" fn(ustore_size_t, ustore_size_t, *mut c_void) -> ustore_enum_t>;

    /// Patterns a scan keeps the values or strings matching, where a zeroed structure matches everything.
    #[repr(C)]
    #[derive(Clone, Copy, Debug)]
    pub struct ustore_patterns_t {
        pub patterns: ustore_strings_view_t,
        pub count: ustore_size_t,
        pub kind: ustore_enum_t,
        pub sensitivity: ustore_enum_t,
        pub weights: *const f32,
    }

    impl Default for ustore_patterns_t {
        fn default() -> Self {
            ustore_patterns_t {
                patterns: ustore_strings_view_t::default(),
                count: 0,
                kind: 0,
                sensitivity: 0,
                weights: ptr::null(),
            }
        }
    }

    /// A predicate admitting or rejecting a page of listed keys, writing one `ustore_admission_t` per key.
    pub type ustore_predicate_callback_t =
        Option<unsafe extern "C" fn(*const ustore_key_t, *mut ustore_enum_t, ustore_size_t, *mut c_void)>;

    /// The caller's verdict on the keys a scan lists, where a null callback admits every key.
    #[repr(C)]
    #[derive(Clone, Copy, Debug)]
    pub struct ustore_predicate_t {
        pub callback: ustore_predicate_callback_t,
        pub state: *mut c_void,
        pub cost: ustore_enum_t,
    }

    impl Default for ustore_predicate_t {
        fn default() -> Self {
            ustore_predicate_t {
                callback: None,
                state: ptr::null_mut(),
                cost: 0,
            }
        }
    }

    pub const USTORE_CONTINUE_K: ustore_enum_t = 0;
    pub const USTORE_CANCEL_K: ustore_enum_t = 1;

    pub const USTORE_CAPABILITIES_TRANSACTIONS_K: ustore_enum_t = 1;

    #[repr(C)]
    pub struct ustore_changes_cursor_handle_t {
        _private: [u8; 0],
    }

    pub type ustore_changes_cursor_t = *mut ustore_changes_cursor_handle_t;

    #[repr(C)]
    pub struct ustore_changes_cursor_init_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub collection: ustore_collection_id_t,
        pub since_stamp: ustore_stamp_t,
        pub cursor: *mut ustore_changes_cursor_t,
    }

    #[repr(C)]
    pub struct ustore_changes_cursor_next_t {
        pub cursor: ustore_changes_cursor_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub wanted: ustore_size_t,
        pub count: *mut ustore_size_t,
        pub keys: *mut *const ustore_key_t,
        pub stamps: *mut *const ustore_stamp_t,
        pub kinds: *mut *const ustore_enum_t,
        pub values: *mut ustore_strings_view_t,
        pub arena: ustore_arena_t,
    }
    pub const USTORE_CAPABILITIES_SNAPSHOTS_K: ustore_enum_t = 4;

    #[repr(C)]
    pub struct ustore_ram_options_t {
        pub memory_domains: *const i32,
        pub memory_domains_count: ustore_size_t,
    }

    #[repr(C)]
    pub struct ustore_executor_for_ram_t {
        pub executor: *mut ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub ram: ustore_ram_options_t,
        pub threads: ustore_size_t,
        pub deferred_limit: ustore_size_t,
        pub cpu_class: ustore_enum_t,
    }

    #[repr(C)]
    pub struct ustore_executor_on_pool_t {
        pub executor: *mut ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub ram: ustore_ram_options_t,
        pub pool: *mut c_void,
        pub deferred_limit: ustore_size_t,
    }

    #[repr(C)]
    pub struct ustore_nvme_options_t {
        pub transport: ustore_enum_t,
        pub shared_io_channels: ustore_size_t,
        pub queue_depth: ustore_size_t,
        pub request_buffer_bytes: ustore_size_t,
        pub request_buffers_count: ustore_size_t,
        pub stream_buffer_bytes: ustore_size_t,
        pub stream_buffers_count: ustore_size_t,
    }

    #[repr(C)]
    pub struct ustore_executor_for_nvme_t {
        pub executor: *mut ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub nvme: ustore_nvme_options_t,
        pub threads: ustore_size_t,
        pub deferred_limit: ustore_size_t,
        pub cpu_class: ustore_enum_t,
    }

    #[repr(C)]
    pub struct ustore_gpu_device_t {
        pub vendor: ustore_enum_t,
        pub vendor_ordinal: u32,
        pub gpu_class: ustore_enum_t,
        pub compute_units: u32,
        pub memory_bytes: u64,
        pub name: [c_char; 64],
    }

    #[repr(C)]
    pub struct ustore_gpu_devices_t {
        pub outcome: *mut ustore_outcome_t,
        pub devices: *mut ustore_gpu_device_t,
        pub devices_capacity: ustore_size_t,
        pub devices_count: *mut ustore_size_t,
    }

    #[repr(C)]
    pub struct ustore_executor_for_vram_t {
        pub executor: *mut ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub device: i32,
        pub stream: *mut c_void,
        pub scratch_limit: ustore_size_t,
        pub gds: ustore_gds_t,
        pub deferred_limit: ustore_size_t,
    }

    #[repr(C)]
    pub struct ustore_executor_drain_t {
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub mood: ustore_mood_t,
    }

    #[repr(C)]
    pub struct ustore_arena_init_t {
        pub arena: *mut ustore_arena_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
    }

    #[repr(C)]
    #[derive(Debug)]
    pub struct ustore_data_directory_t {
        pub path: ustore_bytes_view_t,
        pub max_bytes: ustore_size_t,
    }

    #[repr(C)]
    pub struct ustore_database_init_ram_t {
        pub database: *mut ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub isolation: ustore_enum_t,
        pub cpu_class: ustore_enum_t,
    }

    #[repr(C)]
    pub struct ustore_database_open_ram_t {
        pub database: *mut ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub directory: ustore_bytes_view_t,
        pub isolation: ustore_enum_t,
        pub cpu_class: ustore_enum_t,
    }

    #[repr(C)]
    pub struct ustore_database_open_rocksdb_t {
        pub database: *mut ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub directory: ustore_bytes_view_t,
        pub data_directories: *const ustore_data_directory_t,
        pub data_directories_count: ustore_size_t,
    }

    /// The header declares the same members under a second name.
    pub type ustore_database_view_rocksdb_t = ustore_database_open_rocksdb_t;

    #[repr(C)]
    pub struct ustore_database_open_leveldb_t {
        pub database: *mut ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub directory: ustore_bytes_view_t,
    }

    #[repr(C)]
    pub struct ustore_database_open_nvme_t {
        pub database: *mut ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub directory: ustore_bytes_view_t,
        pub data_directories: *const ustore_data_directory_t,
        pub data_directories_count: ustore_size_t,
        pub compactions: ustore_executor_t,
        pub memory_limit: ustore_size_t,
        pub transaction_max_bytes: ustore_size_t,
        pub transaction_max_deltas: ustore_size_t,
        pub collection_concurrency_limit: ustore_size_t,
        pub collection_cache_limit: ustore_size_t,
        pub collection_memory_runs_max_bytes: ustore_size_t,
        pub collection_memory_runs_max_deltas: ustore_size_t,
        pub collection_level_enlarge_factor: ustore_size_t,
        pub collection_value_size: ustore_size_t,
        pub collection_value_max_size: ustore_size_t,
        pub cpu_class: ustore_enum_t,
    }

    #[repr(C)]
    #[derive(Default)]
    pub struct ustore_database_description_t {
        pub capabilities: ustore_enum_t,
        pub engine: ustore_enum_t,
        pub isolation: ustore_enum_t,
        pub keys: ustore_size_t,
        pub memory_bytes: ustore_size_t,
        pub disk_bytes: ustore_size_t,
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
    pub struct ustore_database_clear_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub mode: ustore_enum_t,
    }

    #[repr(C)]
    pub struct ustore_database_compact_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
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
        pub read_stamp: *mut ustore_stamp_t,
    }

    #[repr(C)]
    pub struct ustore_snapshot_save_t {
        pub snapshot: ustore_snapshot_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub path: ustore_bytes_view_t,
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
        pub collections: *mut *const ustore_collection_id_t,
        pub modalities: *mut *const ustore_enum_t,
        pub names: *mut ustore_strings_view_t,
        pub written: *mut ustore_size_t,
        pub arena: ustore_arena_t,
    }

    #[repr(C)]
    pub struct ustore_collection_create_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub name: ustore_bytes_view_t,
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
        pub names: ustore_strings_view_t,
        pub presences: *mut *const ustore_octet_t,
        pub collections: *mut *const ustore_collection_id_t,
        pub modalities: *mut *const ustore_enum_t,
        pub arena: ustore_arena_t,
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
        pub read_stamp: *mut ustore_stamp_t,
    }

    #[repr(C)]
    pub struct ustore_transaction_commit_t {
        pub database: ustore_database_t,
        pub executor: ustore_executor_t,
        pub outcome: *mut ustore_outcome_t,
        pub completion: ustore_completion_t,
        pub transaction: ustore_transaction_t,
        pub commit_stamp: *mut ustore_stamp_t,
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
        pub fn ustore_status_name(status: ustore_status_t) -> *const c_char;
        pub fn ustore_status_from_name(name: ustore_bytes_view_t) -> ustore_status_t;
        pub fn ustore_outcome_wait(outcome: *mut ustore_outcome_t) -> ustore_status_t;
        pub fn ustore_outcome_status(outcome: *mut ustore_outcome_t) -> ustore_status_t;
        pub fn ustore_version_name() -> *const c_char;

        pub fn ustore_mood_name(mood: ustore_enum_t) -> *const c_char;
        pub fn ustore_mood_from_name(name: ustore_bytes_view_t) -> ustore_enum_t;
        pub fn ustore_cpu_class_name(cpu_class: ustore_enum_t) -> *const c_char;
        pub fn ustore_cpu_class_from_name(name: ustore_bytes_view_t) -> ustore_enum_t;
        pub fn ustore_durability_name(durability: ustore_enum_t) -> *const c_char;
        pub fn ustore_durability_from_name(name: ustore_bytes_view_t) -> ustore_enum_t;
        pub fn ustore_watch_name(watch: ustore_enum_t) -> *const c_char;
        pub fn ustore_watch_from_name(name: ustore_bytes_view_t) -> ustore_enum_t;
        pub fn ustore_drop_mode_name(drop_mode: ustore_enum_t) -> *const c_char;
        pub fn ustore_drop_mode_from_name(name: ustore_bytes_view_t) -> ustore_enum_t;
        pub fn ustore_scan_direction_name(scan_direction: ustore_enum_t) -> *const c_char;
        pub fn ustore_change_kind_name(change_kind: ustore_enum_t) -> *const c_char;
        pub fn ustore_change_kind_from_name(name: ustore_bytes_view_t) -> ustore_enum_t;
        pub fn ustore_scan_direction_from_name(name: ustore_bytes_view_t) -> ustore_enum_t;
        pub fn ustore_isolation_name(isolation: ustore_enum_t) -> *const c_char;
        pub fn ustore_isolation_from_name(name: ustore_bytes_view_t) -> ustore_enum_t;
        pub fn ustore_pattern_kind_name(kind: ustore_enum_t) -> *const c_char;
        pub fn ustore_pattern_kind_from_name(name: ustore_bytes_view_t) -> ustore_enum_t;
        pub fn ustore_case_sensitivity_name(sensitivity: ustore_enum_t) -> *const c_char;
        pub fn ustore_case_sensitivity_from_name(name: ustore_bytes_view_t) -> ustore_enum_t;
        pub fn ustore_predicate_cost_name(cost: ustore_enum_t) -> *const c_char;
        pub fn ustore_predicate_cost_from_name(name: ustore_bytes_view_t) -> ustore_enum_t;
        pub fn ustore_gds_name(gds: ustore_enum_t) -> *const c_char;
        pub fn ustore_gds_from_name(name: ustore_bytes_view_t) -> ustore_enum_t;
        pub fn ustore_gpu_vendor_name(vendor: ustore_enum_t) -> *const c_char;
        pub fn ustore_gpu_vendor_from_name(name: ustore_bytes_view_t) -> ustore_enum_t;
        pub fn ustore_gpu_class_name(gpu_class: ustore_enum_t) -> *const c_char;
        pub fn ustore_gpu_class_from_name(name: ustore_bytes_view_t) -> ustore_enum_t;
        pub fn ustore_transport_name(transport: ustore_enum_t) -> *const c_char;
        pub fn ustore_transport_from_name(name: ustore_bytes_view_t) -> ustore_enum_t;
        pub fn ustore_engine_name(engine: ustore_enum_t) -> *const c_char;
        pub fn ustore_engine_from_name(name: ustore_bytes_view_t) -> ustore_enum_t;
        pub fn ustore_modality_name(modality: ustore_enum_t) -> *const c_char;
        pub fn ustore_modality_from_name(name: ustore_bytes_view_t) -> ustore_enum_t;

        pub fn ustore_executor_for_ram(arguments: *mut ustore_executor_for_ram_t);
        pub fn ustore_executor_on_pool(arguments: *mut ustore_executor_on_pool_t);
        pub fn ustore_executor_for_nvme(arguments: *mut ustore_executor_for_nvme_t);
        pub fn ustore_executor_for_vram(arguments: *mut ustore_executor_for_vram_t);
        pub fn ustore_gpu_devices(arguments: *mut ustore_gpu_devices_t);
        pub fn ustore_executor_drain(arguments: *mut ustore_executor_drain_t);
        pub fn ustore_executor_free(executor: ustore_executor_t);

        pub fn ustore_arena_init(arguments: *mut ustore_arena_init_t);

        pub fn ustore_database_init_ram(arguments: *mut ustore_database_init_ram_t);
        pub fn ustore_database_open_ram(arguments: *mut ustore_database_open_ram_t);
        pub fn ustore_database_open_rocksdb(arguments: *mut ustore_database_open_rocksdb_t);
        pub fn ustore_database_view_rocksdb(arguments: *mut ustore_database_view_rocksdb_t);
        pub fn ustore_database_open_leveldb(arguments: *mut ustore_database_open_leveldb_t);
        pub fn ustore_database_open_nvme(arguments: *mut ustore_database_open_nvme_t);

        pub fn ustore_database_describe(arguments: *mut ustore_database_describe_t);
        pub fn ustore_database_clear(arguments: *mut ustore_database_clear_t);
        pub fn ustore_database_compact(arguments: *mut ustore_database_compact_t);

        pub fn ustore_snapshot_init(arguments: *mut ustore_snapshot_init_t);
        pub fn ustore_snapshot_save(arguments: *mut ustore_snapshot_save_t);
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

        pub fn ustore_changes_cursor_init(arguments: *mut ustore_changes_cursor_init_t);
        pub fn ustore_changes_cursor_next(arguments: *mut ustore_changes_cursor_next_t);
        pub fn ustore_changes_cursor_free(cursor: ustore_changes_cursor_t);
        pub fn ustore_database_free(database: ustore_database_t);
    }
}

// endregion: Raw Declarations

// region: Errors

/// What went wrong, in the vocabulary of `ustore_status_t`, whose success is left to `Result`.
///
/// An [`Error`] never carries [`Status::Deferred`], which only [`Deferred::status`] reports.
#[repr(i32)]
#[derive(Clone, Copy, PartialEq, Eq, Debug, Hash)]
#[non_exhaustive]
pub enum Status {
    /// A deferred call that has not finished; the one status that is never final.
    Deferred = 1,
    /// An allocation could not be satisfied.
    OutOfMemory = 2,
    /// The call would change a database opened read-only, as [`Database::view_rocksdb`] opens one.
    ReadOnly = 6,
    /// A file or directory could not be opened, read, written, or closed.
    Io = 7,
    /// A device could not be selected, or its memory could not be claimed.
    Device = 8,
    /// Stored or supplied data could not be decoded.
    Malformed = 9,
    /// The engine failed in a way no other status names.
    Internal = 10,
    /// The call needs a database or transaction that was never opened.
    Uninitialized = 12,
    /// The call was refused rather than waited for.
    Contended = 14,
    /// A progress callback stopped the call, which published nothing.
    Cancelled = 15,
    /// The engine or build does not implement what the call asks for.
    Unsupported = 16,
    /// An argument is null, contradicts another one, or carries unaccepted options.
    InvalidArgument = 17,
    /// A snapshot, collection, or document the call addresses does not exist.
    NotFound = 18,
    /// A snapshot, collection, or document the call creates already exists.
    AlreadyExists = 19,
    /// A concurrent writer changed a key this transaction wrote.
    WriteConflict = 20,
    /// A concurrent writer changed a key this transaction read.
    ReadConflict = 21,
    /// A concurrent writer inserted into a range this transaction scanned.
    PhantomConflict = 22,
    /// A hard design ceiling was reached.
    CapacityExhausted = 23,
}

impl Status {
    /// Lifts the C status word, reading a value this binding has no name for as `Internal`.
    fn from_raw(status: ffi::ustore_status_t) -> std::result::Result<(), Self> {
        let status = match status.0 {
            0 => return Ok(()),
            1 => Status::Deferred,
            2 => Status::OutOfMemory,
            6 => Status::ReadOnly,
            7 => Status::Io,
            8 => Status::Device,
            9 => Status::Malformed,
            12 => Status::Uninitialized,
            14 => Status::Contended,
            15 => Status::Cancelled,
            16 => Status::Unsupported,
            17 => Status::InvalidArgument,
            18 => Status::NotFound,
            19 => Status::AlreadyExists,
            20 => Status::WriteConflict,
            21 => Status::ReadConflict,
            22 => Status::PhantomConflict,
            23 => Status::CapacityExhausted,
            _ => Status::Internal,
        };
        Err(status)
    }

    /// The canonical lowercase spelling the library gives this status, such as "out-of-memory".
    fn name(self) -> &'static str {
        // SAFETY: the library answers a static null-terminated spelling for every value.
        unsafe {
            static_string(ffi::ustore_status_name(ffi::ustore_status_t(
                self as ffi::ustore_enum_t,
            )))
        }
    }
}

impl fmt::Display for Status {
    /// Calls `ustore_status_name`.
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.name())
    }
}

impl FromStr for Status {
    type Err = Error;

    /// Calls `ustore_status_from_name`, refusing "success", which names no failure.
    fn from_str(text: &str) -> Result<Self> {
        // SAFETY: the name outlives the call, which reads it without a terminator.
        let raw = unsafe { ffi::ustore_status_from_name(text.as_bytes().into()) };
        // An unknown name reads as `invalid-argument`, so only a status that spells back the same was named.
        match Status::from_raw(raw) {
            Err(status) if status.name() == text => Ok(status),
            _ => Err(fail(Status::InvalidArgument, "no status is spelled that way")),
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

/// Turns the outcome of a final call into a `Result`.
///
/// It reads the status as plainly as the message beside it, which has no accessor: the blocking call or the
/// `ustore_outcome_wait` before it returned, ordering the final store first, and nothing writes the outcome after.
pub(crate) fn check(outcome: ffi::ustore_outcome_t) -> Result<()> {
    Status::from_raw(outcome.status).map_err(|status| Error {
        status,
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

/// The canonical spelling the library gives `value` through one of its `ustore_*_name` functions.
pub(crate) fn spelling(
    name: unsafe extern "C" fn(ffi::ustore_enum_t) -> *const c_char,
    value: ffi::ustore_enum_t,
) -> &'static str {
    // SAFETY: every name function answers a static null-terminated spelling, "unrecognized" included.
    unsafe { static_string(name(value)) }
}

/// Reads a spelling back through one of the library's `ustore_*_from_name` functions, refusing one the library spells
/// otherwise, since each answers its enum's zero for a name it does not know.
pub(crate) fn read_spelling<Value>(
    text: &str,
    from_name: unsafe extern "C" fn(ffi::ustore_bytes_view_t) -> ffi::ustore_enum_t,
    name: unsafe extern "C" fn(ffi::ustore_enum_t) -> *const c_char,
    lift: fn(ffi::ustore_enum_t) -> Option<Value>,
) -> Result<Value> {
    // SAFETY: the text outlives the call, which reads it without a terminator.
    let value = unsafe { from_name(text.as_bytes().into()) };
    match spelling(name, value) == text {
        true => lift(value),
        false => None,
    }
    .ok_or(fail(Status::InvalidArgument, "no enumerator is spelled that way"))
}

/// The version of the library the program runs against, like "1.0.0".
///
/// Calls `ustore_version_name`.
pub fn version() -> &'static str {
    // SAFETY: the library answers a static null-terminated string.
    unsafe { static_string(ffi::ustore_version_name()) }
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

/// The stamp a transaction or a snapshot reads at, which a stage reports and the commit after it validates against.
#[repr(transparent)]
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug, Default)]
pub struct ReadStamp(pub ffi::ustore_stamp_t);

/// The position of a commit in commit order, visible to every snapshot whose read stamp is at or above it.
#[repr(transparent)]
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug, Default)]
pub struct CommitStamp(pub ffi::ustore_stamp_t);

/// The identifier of a collection: a UUIDv7 its create mints once and never reuses, so it stays the same across
/// restarts and never names two collections. The nil UUID names none.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Default)]
pub struct CollectionID(pub UUID);

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
    /// The run addresses either the identifier inside this value or the caller's slice, so the
    /// call it feeds must run while both are alive.
    pub(crate) fn view(&self, count: usize) -> Result<ffi::ustore_collections_view_t> {
        match self {
            Collections::Every(collection) => Ok(ffi::ustore_strided_view_t::repeated(collection)),
            Collections::Each(collections) if collections.len() == count => {
                Ok(ffi::ustore_strided_view_t::packed(collections))
            }
            Collections::Each(_) => Err(fail(Status::InvalidArgument, "the batch holds a collection per task")),
        }
    }
}

/// The key that stands for no key, every bit of it set.
pub const fn key_unknown() -> Key {
    Key(UUID([0xFF; 16]))
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

impl Durability {
    fn from_raw(raw: ffi::ustore_enum_t) -> Option<Self> {
        match raw {
            0 => Some(Durability::Buffered),
            1 => Some(Durability::Flushed),
            _ => None,
        }
    }
}

impl fmt::Display for Durability {
    /// Calls `ustore_durability_name`.
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(spelling(ffi::ustore_durability_name, *self as ffi::ustore_enum_t))
    }
}

impl FromStr for Durability {
    type Err = Error;

    /// Calls `ustore_durability_from_name`.
    fn from_str(text: &str) -> Result<Self> {
        read_spelling(
            text,
            ffi::ustore_durability_from_name,
            ffi::ustore_durability_name,
            Durability::from_raw,
        )
    }
}

/// Whether the calls of a transaction watch the keys they touch for conflicts, as `ustore_watch_t` spells it.
///
/// Only a transaction watches anything, so the head of a database and a snapshot never carry this, and
/// it changes what a call does only at [`Isolation::Snapshot`]: a strictly serializable transaction puts
/// every read it makes into the one serial order whatever this says.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub enum Watch {
    /// Every key the view reads or writes is recorded, so a concurrent writer conflicts at the commit.
    #[default]
    Keys = 0,
    /// Nothing the view touches is watched, so a snapshot-isolated transaction commits over a concurrent
    /// writer instead of conflicting with it.
    None = 1,
}

impl Watch {
    fn from_raw(raw: ffi::ustore_enum_t) -> Option<Self> {
        match raw {
            0 => Some(Watch::Keys),
            1 => Some(Watch::None),
            _ => None,
        }
    }
}

impl fmt::Display for Watch {
    /// Calls `ustore_watch_name`.
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(spelling(ffi::ustore_watch_name, *self as ffi::ustore_enum_t))
    }
}

impl FromStr for Watch {
    type Err = Error;

    /// Calls `ustore_watch_from_name`.
    fn from_str(text: &str) -> Result<Self> {
        read_spelling(
            text,
            ffi::ustore_watch_from_name,
            ffi::ustore_watch_name,
            Watch::from_raw,
        )
    }
}

/// What dropping a collection or clearing a database removes, as `ustore_drop_mode_t` spells it.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Hash)]
pub enum DropMode {
    /// The collection together with its name and every pair it holds.
    Collection = 0,
    /// Every key and value, keeping the collection.
    Pairs = 1,
    /// Every value, keeping the keys.
    Values = 2,
}

impl DropMode {
    fn from_raw(raw: ffi::ustore_enum_t) -> Option<Self> {
        match raw {
            0 => Some(DropMode::Collection),
            1 => Some(DropMode::Pairs),
            2 => Some(DropMode::Values),
            _ => None,
        }
    }
}

impl fmt::Display for DropMode {
    /// Calls `ustore_drop_mode_name`.
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(spelling(ffi::ustore_drop_mode_name, *self as ffi::ustore_enum_t))
    }
}

impl FromStr for DropMode {
    type Err = Error;

    /// Calls `ustore_drop_mode_from_name`.
    fn from_str(text: &str) -> Result<Self> {
        read_spelling(
            text,
            ffi::ustore_drop_mode_from_name,
            ffi::ustore_drop_mode_name,
            DropMode::from_raw,
        )
    }
}

/// Which way a scan, a cursor or a measure walks its range from the start key.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub enum ScanDirection {
    /// Up from the start key, smallest key first.
    #[default]
    Ascending = 0,
    /// Down from the start key, largest key first.
    Descending = 1,
}

impl ScanDirection {
    fn from_raw(raw: ffi::ustore_enum_t) -> Option<Self> {
        match raw {
            0 => Some(ScanDirection::Ascending),
            1 => Some(ScanDirection::Descending),
            _ => None,
        }
    }
}

impl fmt::Display for ScanDirection {
    /// Spells the direction as "ascending" or "descending".
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(spelling(ffi::ustore_scan_direction_name, *self as ffi::ustore_enum_t))
    }
}

impl FromStr for ScanDirection {
    type Err = Error;

    /// Reads "ascending" or "descending" back.
    fn from_str(text: &str) -> Result<Self> {
        read_spelling(
            text,
            ffi::ustore_scan_direction_from_name,
            ffi::ustore_scan_direction_name,
            ScanDirection::from_raw,
        )
    }
}

/// The keys a scan, a cursor or a measure walks: from `start`, inclusive, towards `end`, exclusive,
/// in the order `direction` names.
///
/// A key converts into the ascending range from it, and `start..end` into the one between them.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub struct KeyRange {
    /// The key the walk starts from, or `None` for the first key in its direction.
    pub start: Option<Key>,
    /// The key the walk stops before, or `None` to walk to the last key in its direction.
    pub end: Option<Key>,
    /// Which way the walk goes from `start`.
    pub direction: ScanDirection,
}

impl KeyRange {
    /// The key the walk starts from, the smallest or the largest one when no start is given.
    pub(crate) fn start_key(&self) -> Key {
        match (self.start, self.direction) {
            (Some(start), _) => start,
            (None, ScanDirection::Ascending) => Key::default(),
            (None, ScanDirection::Descending) => key_unknown(),
        }
    }

    /// The view of the end key, or a zeroed one when the walk has no end.
    pub(crate) fn end_keys(&self) -> ffi::ustore_keys_view_t {
        match &self.end {
            Some(end) => ffi::ustore_strided_view_t::repeated(end),
            None => ffi::ustore_strided_view_t::default(),
        }
    }
}

impl From<Key> for KeyRange {
    fn from(start: Key) -> Self {
        KeyRange {
            start: Some(start),
            ..KeyRange::default()
        }
    }
}

impl From<Range<Key>> for KeyRange {
    fn from(range: Range<Key>) -> Self {
        KeyRange {
            start: Some(range.start),
            end: Some(range.end),
            direction: ScanDirection::Ascending,
        }
    }
}

/// What the threads of an [`Executor`] do once it goes idle, as `ustore_mood_t` spells it.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug, Hash)]
pub enum Mood {
    /// Parked, so the next call pays a wake-up.
    Parked = 0,
    /// Spinning, trading power for the next call's latency.
    Spinning = 1,
}

impl Mood {
    fn from_raw(raw: ffi::ustore_enum_t) -> Option<Self> {
        match raw {
            0 => Some(Mood::Parked),
            1 => Some(Mood::Spinning),
            _ => None,
        }
    }
}

impl fmt::Display for Mood {
    /// Calls `ustore_mood_name`.
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(spelling(ffi::ustore_mood_name, *self as ffi::ustore_enum_t))
    }
}

impl FromStr for Mood {
    type Err = Error;

    /// Calls `ustore_mood_from_name`.
    fn from_str(text: &str) -> Result<Self> {
        read_spelling(text, ffi::ustore_mood_from_name, ffi::ustore_mood_name, Mood::from_raw)
    }
}

// endregion: Handles and Constants

// region: Arenas

/// Memory the library writes every output whose length scales with a call into, reused across calls.
///
/// An executor places it where it computes, decided once, and nothing is allocated until a call writes into it. Every
/// call rewinds and refills it, invalidating the previous call's outputs, so a steady loop allocates once. Every verb,
/// blocking or deferred, borrows it as `&mut Arena` and answers outputs that borrow it in turn, so the borrow checker
/// both keeps a result from outliving its arena and stops the call that would overwrite it.
///
/// Mirrors `ustore_arena_t`; dropping it calls `ustore_arena_free`, which waits for the calls still writing into it.
#[derive(Debug)]
pub struct Arena {
    pub(crate) handle: ffi::ustore_arena_t,
}

impl Arena {
    /// Makes an arena of host pages for calls on the calling thread.
    ///
    /// Calls `ustore_arena_init`.
    pub fn new() -> Result<Self> {
        Arena::made_by(ptr::null_mut())
    }

    /// Makes an arena where `executor` computes, serving the calls of its residency.
    ///
    /// Calls `ustore_arena_init`.
    pub fn new_in(executor: &Executor) -> Result<Self> {
        Arena::made_by(executor.handle)
    }

    /// The arena `executor` places, where null places it for the calling thread.
    fn made_by(executor: ffi::ustore_executor_t) -> Result<Self> {
        let mut arena = Arena {
            handle: ptr::null_mut(),
        };
        let mut outcome = ffi::ustore_outcome_t::default();
        let mut arguments = ffi::ustore_arena_init_t {
            arena: &mut arena.handle,
            executor,
            outcome: &mut outcome,
        };
        // SAFETY: every pointer in `arguments` outlives the call, which is final on return.
        unsafe { ffi::ustore_arena_init(&mut arguments) };
        check(outcome).map(|()| arena)
    }
}

/// An address a call holds into memory it owns, or into an output the library exports into its arena, which moves
/// between threads with the call.
#[repr(transparent)]
#[derive(Clone, Copy)]
pub(crate) struct Address<Value>(pub(crate) *const Value);

impl<Value> Default for Address<Value> {
    fn default() -> Self {
        Address(ptr::null())
    }
}

impl<Value> fmt::Debug for Address<Value> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Pointer::fmt(&self.0, formatter)
    }
}

// SAFETY: the address points into an arena or an input the same call owns, both of which move between threads freely.
unsafe impl<Value> Send for Address<Value> {}
unsafe impl<Value> Sync for Address<Value> {}

impl Drop for Arena {
    fn drop(&mut self) {
        // SAFETY: the handle came from the library and is freed once, after the calls still writing into it.
        unsafe { ffi::ustore_arena_free(self.handle) };
    }
}

// SAFETY: an arena serves one call at a time, which the `&mut` every verb takes already enforces, and the library
// ties it to no thread.
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

/// Writes the view of every string `indices` names out of a slice of `Record`s, each holding its view at the place
/// `handle` addresses inside the first, so a slice of entries lends its values without a copy.
unsafe extern "C" fn record_views<Record>(
    handle: *const c_void,
    indices: *const ffi::ustore_size_t,
    count: ffi::ustore_size_t,
    views: *mut ffi::ustore_bytes_view_t,
) {
    let first = handle.cast::<u8>();
    for slot in 0..count as usize {
        let index = *indices.add(slot) as usize;
        let view = first
            .add(index * size_of::<Record>())
            .cast::<ffi::ustore_bytes_view_t>();
        *views.add(slot) = view.read();
    }
}

impl ffi::ustore_strings_view_t {
    /// The strings a slice of `Record`s lends through an accessor, one view inside each, `first` addressing the view
    /// inside the first record.
    pub(crate) fn inside<Record>(first: &ffi::ustore_bytes_view_t) -> Self {
        ffi::ustore_strings_view_t::accessor(record_views::<Record>, ptr::from_ref(first).cast())
    }
}

impl sealed::TapeOffset for u32 {
    fn tape(bytes: &[u8], offsets: &[u32]) -> ffi::ustore_strings_view_t {
        ffi::ustore_strings_view_t::u32_tape(bytes.as_ptr(), offsets.as_ptr())
    }
}

impl sealed::TapeOffset for u64 {
    fn tape(bytes: &[u8], offsets: &[u64]) -> ffi::ustore_strings_view_t {
        ffi::ustore_strings_view_t::u64_tape(bytes.as_ptr(), offsets.as_ptr())
    }
}

/// The offsets of a tape the library wrote: 32-bit while its bytes fit in 4 GiB, and 64-bit beyond.
#[derive(Clone, Copy, Debug)]
enum Offsets<'arena> {
    U32(&'arena [u32]),
    U64(&'arena [u64]),
}

/// A batch of strings a call wrote into an arena as a tape, one more offset than strings into the bytes they split.
#[derive(Clone, Copy, Debug)]
pub(crate) struct Strings<'arena> {
    bytes: &'arena [u8],
    offsets: Offsets<'arena>,
}

impl<'arena> Strings<'arena> {
    /// The strings of a call that wrote none.
    pub(crate) fn empty() -> Self {
        Strings {
            bytes: &[],
            offsets: Offsets::U32(&[0]),
        }
    }

    /// The `count` strings a call wrote as the tape `view`, which a call with nothing to report left zeroed.
    ///
    /// # Safety
    /// A successful call wrote `view` as a tape of `count` strings into an arena alive for `'arena`, or left it zeroed.
    pub(crate) unsafe fn exported(view: ffi::ustore_strings_view_t, count: usize) -> Result<Self> {
        match view.layout {
            ffi::USTORE_STRINGS_U32_TAPE_K if !view.data.u32_tape.offsets.is_null() => {
                let offsets = exported_slice(view.data.u32_tape.offsets, count + 1);
                Ok(Strings {
                    bytes: exported_slice(view.data.u32_tape.data, offsets[count] as usize),
                    offsets: Offsets::U32(offsets),
                })
            }
            ffi::USTORE_STRINGS_U64_TAPE_K if !view.data.u64_tape.offsets.is_null() => {
                let offsets = exported_slice(view.data.u64_tape.offsets, count + 1);
                Ok(Strings {
                    bytes: exported_slice(view.data.u64_tape.data, offsets[count] as usize),
                    offsets: Offsets::U64(offsets),
                })
            }
            ffi::USTORE_STRINGS_U32_TAPE_K | ffi::USTORE_STRINGS_U64_TAPE_K => Ok(Strings::empty()),
            _ => Err(fail(Status::Internal, "the library wrote strings in no tape layout")),
        }
    }

    /// How many strings the tape holds.
    pub(crate) fn len(&self) -> usize {
        match self.offsets {
            Offsets::U32(offsets) => offsets.len() - 1,
            Offsets::U64(offsets) => offsets.len() - 1,
        }
    }

    /// The string at `index`, or `None` past the end.
    pub(crate) fn get(&self, index: usize) -> Option<&'arena [u8]> {
        let (begin, end) = match self.offsets {
            Offsets::U32(offsets) => (*offsets.get(index)? as usize, *offsets.get(index + 1)? as usize),
            Offsets::U64(offsets) => (*offsets.get(index)? as usize, *offsets.get(index + 1)? as usize),
        };
        self.bytes.get(begin..end)
    }

    /// The tape itself, without a copy.
    pub(crate) fn tape(&self) -> Tape<'arena> {
        // SAFETY: the offsets never decrease, and the last one closes the bytes they split.
        unsafe {
            match self.offsets {
                Offsets::U32(offsets) => Tape::U32(BytesTapeViewU32::from_raw_parts(self.bytes, offsets)),
                Offsets::U64(offsets) => Tape::U64(BytesTapeViewU64::from_raw_parts(self.bytes, offsets)),
            }
        }
    }
}

/// The strings a call wrote as one tape: one more offset than strings, and the bytes between them, as Arrow lays out
/// `binary` while the bytes fit in 4 GiB and `large_binary` beyond.
#[derive(Debug)]
pub enum Tape<'arena> {
    /// A tape of 32-bit offsets, as Arrow lays out `binary`.
    U32(BytesTapeViewU32<'arena>),
    /// A tape of 64-bit offsets, as Arrow lays out `large_binary`.
    U64(BytesTapeViewU64<'arena>),
}

impl Tape<'_> {
    /// How many strings the tape holds.
    pub fn len(&self) -> usize {
        match self {
            Tape::U32(tape) => tape.len(),
            Tape::U64(tape) => tape.len(),
        }
    }

    /// Whether the tape holds no strings.
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// The string at `index`, or `None` past the end.
    pub fn get(&self, index: usize) -> Option<&[u8]> {
        match self {
            Tape::U32(tape) => tape.get(index),
            Tape::U64(tape) => tape.get(index),
        }
    }
}

// endregion: Arenas

// region: Options

/// What a collection holds, which an engine records beside its name and refuses the verbs of another modality by.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Hash)]
pub enum Modality {
    /// Binary values.
    Blobs = 1,
    /// JSON documents.
    Docs = 2,
    /// Vertices and their edges, which only the graph verbs reach.
    Graph = 3,
    /// Values keyed by strings.
    Strings = 4,
    /// Ordered documents.
    Pages = 5,
}

impl Modality {
    /// The modality an engine reported, or `None` for one a newer library added.
    pub(crate) fn from_raw(raw: ffi::ustore_enum_t) -> Option<Self> {
        match raw {
            1 => Some(Modality::Blobs),
            2 => Some(Modality::Docs),
            3 => Some(Modality::Graph),
            4 => Some(Modality::Strings),
            5 => Some(Modality::Pages),
            _ => None,
        }
    }
}

impl fmt::Display for Modality {
    /// Calls `ustore_modality_name`.
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(spelling(ffi::ustore_modality_name, *self as ffi::ustore_enum_t))
    }
}

impl FromStr for Modality {
    type Err = Error;

    /// Calls `ustore_modality_from_name`.
    fn from_str(text: &str) -> Result<Self> {
        read_spelling(
            text,
            ffi::ustore_modality_from_name,
            ffi::ustore_modality_name,
            Modality::from_raw,
        )
    }
}

/// The isolation the in-memory engine delivers to the transactions of a database.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub enum Isolation {
    /// Reads and writes both take part in one serial order.
    #[default]
    StrictSerializable = 0,
    /// A transaction reads the snapshot it began at and conflicts only on what it wrote.
    Snapshot = 1,
}

impl Isolation {
    fn from_raw(raw: ffi::ustore_enum_t) -> Option<Self> {
        match raw {
            0 => Some(Isolation::StrictSerializable),
            1 => Some(Isolation::Snapshot),
            _ => None,
        }
    }
}

impl fmt::Display for Isolation {
    /// Calls `ustore_isolation_name`.
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(spelling(ffi::ustore_isolation_name, *self as ffi::ustore_enum_t))
    }
}

impl FromStr for Isolation {
    type Err = Error;

    /// Calls `ustore_isolation_from_name`.
    fn from_str(text: &str) -> Result<Self> {
        read_spelling(
            text,
            ffi::ustore_isolation_from_name,
            ffi::ustore_isolation_name,
            Isolation::from_raw,
        )
    }
}

/// How an executor of [`Executor::for_vram`] moves bytes between the drives and its device through GPUDirect Storage,
/// as `ustore_gds_t` spells it.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub enum Gds {
    /// Direct transfers where the `nvidia_fs` driver is loaded, and cuFile's compatibility mode elsewhere.
    #[default]
    Automatic = 0,
    /// Direct transfers, refused with [`Status::Device`] where the driver is absent.
    Direct = 1,
    /// cuFile's compatibility mode, which bounces through host pages it manages.
    Compatibility = 2,
}

impl Gds {
    fn from_raw(raw: ffi::ustore_enum_t) -> Option<Self> {
        match raw {
            0 => Some(Gds::Automatic),
            1 => Some(Gds::Direct),
            2 => Some(Gds::Compatibility),
            _ => None,
        }
    }
}

impl fmt::Display for Gds {
    /// Calls `ustore_gds_name`.
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(spelling(ffi::ustore_gds_name, *self as ffi::ustore_enum_t))
    }
}

impl FromStr for Gds {
    type Err = Error;

    /// Calls `ustore_gds_from_name`.
    fn from_str(text: &str) -> Result<Self> {
        read_spelling(text, ffi::ustore_gds_from_name, ffi::ustore_gds_name, Gds::from_raw)
    }
}

/// The vendor whose runtime drives a GPU, as `ustore_gpu_vendor_t` spells it, like "amd".
#[derive(Clone, Copy, PartialEq, Eq, Debug, Hash)]
pub enum GPUVendor {
    /// NVIDIA, through the CUDA driver.
    Nvidia = 1,
    /// AMD, through the HIP runtime.
    Amd = 2,
}

impl GPUVendor {
    fn from_raw(raw: ffi::ustore_enum_t) -> Option<Self> {
        match raw {
            1 => Some(GPUVendor::Nvidia),
            2 => Some(GPUVendor::Amd),
            _ => None,
        }
    }
}

impl fmt::Display for GPUVendor {
    /// Calls `ustore_gpu_vendor_name`.
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(spelling(ffi::ustore_gpu_vendor_name, *self as ffi::ustore_enum_t))
    }
}

impl FromStr for GPUVendor {
    type Err = Error;

    /// Calls `ustore_gpu_vendor_from_name`.
    fn from_str(text: &str) -> Result<Self> {
        read_spelling(
            text,
            ffi::ustore_gpu_vendor_from_name,
            ffi::ustore_gpu_vendor_name,
            GPUVendor::from_raw,
        )
    }
}

/// The generation of a GPU, as `ustore_gpu_class_t` spells it, like "nvidia-hopper".
#[derive(Clone, Copy, PartialEq, Eq, Debug, Hash)]
pub enum GPUClass {
    /// Ampere-class NVIDIA and every NVIDIA GPU without a class of its own - A100, A10 & RTX 30.
    NvidiaAmpere = 1,
    /// Ada-class NVIDIA - L4, L40S & RTX 40.
    NvidiaAda = 2,
    /// Hopper-class NVIDIA - H100 & H200.
    NvidiaHopper = 3,
    /// Blackwell-class NVIDIA - B200, B300 & GB300.
    NvidiaBlackwell = 4,
    /// Consumer Blackwell NVIDIA - RTX 50, RTX PRO 6000 & DGX Spark.
    NvidiaBlackwellRTX = 5,
    /// CDNA3 AMD - MI300X & MI325X.
    AmdCDNA3 = 6,
    /// CDNA4 AMD - MI350X & MI355X.
    AmdCDNA4 = 7,
    /// CDNA5 AMD - MI400.
    AmdCDNA5 = 8,
}

impl GPUClass {
    fn from_raw(raw: ffi::ustore_enum_t) -> Option<Self> {
        match raw {
            1 => Some(GPUClass::NvidiaAmpere),
            2 => Some(GPUClass::NvidiaAda),
            3 => Some(GPUClass::NvidiaHopper),
            4 => Some(GPUClass::NvidiaBlackwell),
            5 => Some(GPUClass::NvidiaBlackwellRTX),
            6 => Some(GPUClass::AmdCDNA3),
            7 => Some(GPUClass::AmdCDNA4),
            8 => Some(GPUClass::AmdCDNA5),
            _ => None,
        }
    }
}

impl fmt::Display for GPUClass {
    /// Calls `ustore_gpu_class_name`.
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(spelling(ffi::ustore_gpu_class_name, *self as ffi::ustore_enum_t))
    }
}

impl FromStr for GPUClass {
    type Err = Error;

    /// Calls `ustore_gpu_class_from_name`.
    fn from_str(text: &str) -> Result<Self> {
        read_spelling(
            text,
            ffi::ustore_gpu_class_from_name,
            ffi::ustore_gpu_class_name,
            GPUClass::from_raw,
        )
    }
}

/// One GPU this process can drive, as [`gpu_devices`] lists it.
#[derive(Clone, PartialEq, Eq, Debug, Hash)]
pub struct GPUDevice {
    /// The ordinal [`Executor::for_vram`] names the device by, its position in [`gpu_devices`].
    pub ordinal: usize,
    /// The vendor whose runtime drives the device.
    pub vendor: GPUVendor,
    /// The device's ordinal in its vendor's runtime, as `nvidia-smi` or `rocm-smi` list it.
    pub vendor_ordinal: u32,
    /// The class of the device.
    pub gpu_class: GPUClass,
    /// Streaming multiprocessors or compute units.
    pub compute_units: u32,
    /// Device memory in bytes.
    pub memory_bytes: u64,
    /// The vendor's own name for the device.
    pub name: String,
}

/// Every GPU this process can drive, NVIDIA devices first, then AMD, each at the ordinal [`Executor::for_vram`] names
/// it by; empty on a machine without GPUs or their runtimes.
///
/// Calls `ustore_gpu_devices`.
pub fn gpu_devices() -> Result<Vec<GPUDevice>> {
    let mut count: ffi::ustore_size_t = 0;
    let mut outcome = ffi::ustore_outcome_t::default();
    let mut counting = ffi::ustore_gpu_devices_t {
        outcome: &mut outcome,
        devices: ptr::null_mut(),
        devices_capacity: 0,
        devices_count: &mut count,
    };
    // SAFETY: every pointer in `counting` outlives the call, which is final on return.
    unsafe { ffi::ustore_gpu_devices(&mut counting) };
    check(outcome)?;
    let mut listed: Vec<ffi::ustore_gpu_device_t> = Vec::with_capacity(count as usize);
    let mut outcome = ffi::ustore_outcome_t::default();
    let mut listing = ffi::ustore_gpu_devices_t {
        outcome: &mut outcome,
        devices: listed.as_mut_ptr(),
        devices_capacity: count,
        devices_count: ptr::null_mut(),
    };
    // SAFETY: `devices` has room for `count` entries, which the call writes all of, since the listing never changes.
    unsafe {
        ffi::ustore_gpu_devices(&mut listing);
        listed.set_len(count as usize);
    }
    check(outcome)?;
    Ok(listed
        .iter()
        .enumerate()
        .filter_map(|(ordinal, device)| {
            // SAFETY: the library null-terminates the name within its 64 bytes.
            let name = unsafe { CStr::from_ptr(device.name.as_ptr()) };
            Some(GPUDevice {
                ordinal,
                vendor: GPUVendor::from_raw(device.vendor)?,
                vendor_ordinal: device.vendor_ordinal,
                gpu_class: GPUClass::from_raw(device.gpu_class)?,
                compute_units: device.compute_units,
                memory_bytes: device.memory_bytes,
                name: name.to_string_lossy().into_owned(),
            })
        })
        .collect())
}

/// The CPU generation a database confines its waits, atomics and string kernels to, where `None` takes what the CPU
/// offers. Prints and parses its stable name, like "intel-icelake".
#[derive(Clone, Copy, PartialEq, Eq, Debug, Hash)]
pub enum CPUClass {
    /// Portable kernels that run on any CPU.
    Serial = 1,
    /// x86 cores from Intel Haswell on.
    IntelHaswell = 2,
    /// x86 efficiency cores from Intel Sierra Forest on.
    IntelSierra = 3,
    /// x86 server cores from Intel Skylake on.
    IntelSkylake = 4,
    /// x86 server cores from Intel Ice Lake on.
    IntelIcelake = 5,
    /// Arm cores from AWS Graviton 2 on.
    AWSG2 = 6,
    /// Arm cores from Apple M2 on.
    AppleM2 = 7,
    /// Arm server cores from AWS Graviton 5 on.
    AWSG5 = 8,
    /// RISC-V cores with RVV, Zawrs and Zacas, from Alibaba's C930 on.
    AlibabaC930 = 9,
    /// The one class of a monoversioned build.
    Monoversioned = 10,
}

impl CPUClass {
    fn from_raw(raw: ffi::ustore_enum_t) -> Option<Self> {
        match raw {
            1 => Some(CPUClass::Serial),
            2 => Some(CPUClass::IntelHaswell),
            3 => Some(CPUClass::IntelSierra),
            4 => Some(CPUClass::IntelSkylake),
            5 => Some(CPUClass::IntelIcelake),
            6 => Some(CPUClass::AWSG2),
            7 => Some(CPUClass::AppleM2),
            8 => Some(CPUClass::AWSG5),
            9 => Some(CPUClass::AlibabaC930),
            10 => Some(CPUClass::Monoversioned),
            _ => None,
        }
    }

    /// The `ustore_cpu_class_t` of `cpu_class`, zero for `None`.
    fn to_raw(cpu_class: Option<CPUClass>) -> ffi::ustore_enum_t {
        cpu_class.map_or(0, |cpu_class| cpu_class as ffi::ustore_enum_t)
    }
}

impl fmt::Display for CPUClass {
    /// Calls `ustore_cpu_class_name`.
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(spelling(ffi::ustore_cpu_class_name, *self as ffi::ustore_enum_t))
    }
}

impl FromStr for CPUClass {
    type Err = Error;

    /// Calls `ustore_cpu_class_from_name`.
    fn from_str(text: &str) -> Result<Self> {
        read_spelling(
            text,
            ffi::ustore_cpu_class_from_name,
            ffi::ustore_cpu_class_name,
            CPUClass::from_raw,
        )
    }
}

/// How the NVMe engine moves bytes between its files and memory.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub enum Transport {
    /// io_uring where the kernel offers it, Linux AIO where it offers only that, and blocking calls elsewhere.
    #[default]
    Automatic = 0,
    /// Positioned reads and writes that block the polling thread, which every platform has.
    Blocking = 1,
    /// io_uring on unbuffered files, refused where the kernel creates no ring.
    LinuxUring = 2,
    /// Linux AIO on unbuffered files.
    LinuxAio = 3,
    /// SPDK's user-space driver on NVMe devices the process owns.
    LinuxSpdk = 4,
}

impl Transport {
    fn from_raw(raw: ffi::ustore_enum_t) -> Option<Self> {
        match raw {
            0 => Some(Transport::Automatic),
            1 => Some(Transport::Blocking),
            2 => Some(Transport::LinuxUring),
            3 => Some(Transport::LinuxAio),
            4 => Some(Transport::LinuxSpdk),
            _ => None,
        }
    }
}

impl fmt::Display for Transport {
    /// Calls `ustore_transport_name`.
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(spelling(ffi::ustore_transport_name, *self as ffi::ustore_enum_t))
    }
}

impl FromStr for Transport {
    type Err = Error;

    /// Calls `ustore_transport_from_name`.
    fn from_str(text: &str) -> Result<Self> {
        read_spelling(
            text,
            ffi::ustore_transport_from_name,
            ffi::ustore_transport_name,
            Transport::from_raw,
        )
    }
}

/// The engine serving a database, as `ustore_engine_t` spells it.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Hash)]
pub enum Engine {
    /// The in-memory engine of [`Database::init_ram`] and [`Database::open_ram`].
    RAM = 0,
    /// The RocksDB engine of [`Database::open_rocksdb`] and [`Database::view_rocksdb`].
    RocksDB = 1,
    /// The LevelDB engine of [`Database::open_leveldb`].
    LevelDB = 2,
    /// The NVMe engine of [`Database::open_nvme`].
    NVME = 3,
}

impl Engine {
    fn from_raw(raw: ffi::ustore_enum_t) -> Option<Self> {
        match raw {
            0 => Some(Engine::RAM),
            1 => Some(Engine::RocksDB),
            2 => Some(Engine::LevelDB),
            3 => Some(Engine::NVME),
            _ => None,
        }
    }
}

impl fmt::Display for Engine {
    /// Calls `ustore_engine_name`.
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(spelling(ffi::ustore_engine_name, *self as ffi::ustore_enum_t))
    }
}

impl FromStr for Engine {
    type Err = Error;

    /// Calls `ustore_engine_from_name`.
    fn from_str(text: &str) -> Result<Self> {
        read_spelling(
            text,
            ffi::ustore_engine_from_name,
            ffi::ustore_engine_name,
            Engine::from_raw,
        )
    }
}

/// A directory the data files may occupy, with the bytes they may take there.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub struct DataDirectory<'path> {
    /// The directory, which must exist; on NVMe also a raw device from a base, like
    /// `/dev/nvme0n1@64GiB`.
    pub path: &'path str,
    /// Bytes the files may take there: zero is unlimited on RocksDB and the whole medium on NVMe.
    pub max_bytes: u64,
}

/// How [`Database::init_ram`] and [`Database::open_ram`] start the in-memory engine.
#[derive(Clone, Copy, Debug, Default)]
pub struct RAMOptions<'options> {
    /// How transactions see each other.
    pub isolation: Isolation,
    /// The executor the open runs on, after the calls submitted there before it; `None` opens on the calling thread.
    pub executor: Option<&'options Executor>,
    /// The CPU class the database confines itself to, failing with [`Status::Unsupported`] where the CPU does not run
    /// it; `None` takes what the CPU offers.
    pub cpu_class: Option<CPUClass>,
}

/// What [`Database::open_rocksdb`] and [`Database::view_rocksdb`] open.
#[derive(Clone, Copy, Debug, Default)]
pub struct RocksDBOptions<'options> {
    /// The database's directory, which must exist.
    pub directory: &'options str,
    /// Directories the table files fill in order, or none to keep them in `directory`.
    pub data_directories: &'options [DataDirectory<'options>],
    /// The executor the open runs on, after the calls submitted there before it; `None` opens on the calling thread.
    pub executor: Option<&'options Executor>,
}

impl<'options> RocksDBOptions<'options> {
    /// A database in an existing directory.
    pub fn at(directory: &'options str) -> Self {
        RocksDBOptions {
            directory,
            ..RocksDBOptions::default()
        }
    }
}

/// What [`Database::open_leveldb`] opens.
#[derive(Clone, Copy, Debug, Default)]
pub struct LevelDBOptions<'options> {
    /// The database's directory, which must exist.
    pub directory: &'options str,
    /// The executor the open runs on, after the calls submitted there before it; `None` opens on the calling thread.
    pub executor: Option<&'options Executor>,
}

impl<'options> LevelDBOptions<'options> {
    /// A database in an existing directory.
    pub fn at(directory: &'options str) -> Self {
        LevelDBOptions {
            directory,
            executor: None,
        }
    }
}

/// What [`Database::open_nvme`] opens, where every zero size or count keeps the engine's default.
#[derive(Clone, Copy, Debug, Default)]
pub struct NVMEOptions<'path> {
    /// The manifest, `<database>.ustore.json` in an existing directory, or the raw address of
    /// the first slot.
    pub manifest: &'path str,
    /// Further data directories the runs land on, the one with the most free bytes taking each.
    pub data_directories: &'path [DataDirectory<'path>],
    /// The executor the open runs on, after the calls submitted there before it; `None` opens on the calling thread.
    /// The database moves bytes as one of [`Executor::for_nvme`] says, read once at the open, and with that call's
    /// defaults for any other executor and for none.
    pub executor: Option<&'path Executor>,
    /// The executor of [`Executor::for_vram`] whose device and scratch limit every merge takes, read once at the open;
    /// `None` merges on host threads.
    pub compactions: Option<&'path Executor>,
    /// Bytes of host memory the engine may pin and cache.
    pub memory_limit: u64,
    /// Bytes a transaction may buffer before it must commit.
    pub transaction_max_bytes: u64,
    /// Deltas a transaction may buffer before it must commit.
    pub transaction_max_deltas: usize,
    /// Operations that may run on one collection at once.
    pub collection_concurrency_limit: usize,
    /// Bytes of values each collection may cache.
    pub collection_cache_limit: u64,
    /// Bytes of memory runs a collection holds before the commit that crosses them flushes level 0.
    pub collection_memory_runs_max_bytes: u64,
    /// Deltas of memory runs a collection holds before the commit that crosses them flushes level 0.
    pub collection_memory_runs_max_deltas: usize,
    /// How many times larger each level of a collection is than the one above it.
    pub collection_level_enlarge_factor: usize,
    /// Bytes of every value of a collection whose values share one size, or zero for varying sizes.
    pub collection_value_size: u64,
    /// Bytes of the largest value of a collection whose values vary in size.
    pub collection_value_max_size: u64,
    /// The CPU class the database confines itself to, failing with [`Status::Unsupported`] where the CPU does not run
    /// it; `None` takes what the CPU offers.
    pub cpu_class: Option<CPUClass>,
}

impl<'path> NVMEOptions<'path> {
    /// The database a manifest names, with every default.
    pub fn at(manifest: &'path str) -> Self {
        NVMEOptions {
            manifest,
            ..NVMEOptions::default()
        }
    }
}

/// The C view of `directories`, pointing into the paths they borrow.
fn data_directories(directories: &[DataDirectory<'_>]) -> Result<Vec<ffi::ustore_data_directory_t>> {
    let mut entries = Vec::new();
    entries
        .try_reserve_exact(directories.len())
        .map_err(|_| fail(Status::OutOfMemory, "the data directories could not be staged"))?;
    entries.extend(directories.iter().map(|directory| ffi::ustore_data_directory_t {
        path: directory.path.as_bytes().into(),
        max_bytes: directory.max_bytes,
    }));
    Ok(entries)
}

/// The executor `executor` names, null for the calling thread.
fn executor_handle(executor: Option<&Executor>) -> ffi::ustore_executor_t {
    executor.map_or(ptr::null_mut(), |executor| executor.handle)
}

/// The memory domains an executor serves, borrowing `memory_domains`, where an empty list derives every one.
fn ram_options(memory_domains: &[i32]) -> ffi::ustore_ram_options_t {
    ffi::ustore_ram_options_t {
        memory_domains: memory_domains.as_ptr(),
        memory_domains_count: memory_domains.len() as ffi::ustore_size_t,
    }
}

/// How the threads of an executor of [`Executor::for_nvme`] move bytes between the drives and memory, where every
/// zero size or count keeps the engine's default.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub struct IOOptions {
    /// How bytes move between the files and memory.
    pub transport: Transport,
    /// I/O channels the threads outside the executor share.
    pub shared_io_channels: usize,
    /// Submissions a channel holds at once.
    pub queue_depth: usize,
    /// Bytes of each request buffer of a channel.
    pub request_buffer_bytes: u64,
    /// Request buffers of each channel.
    pub request_buffers_count: usize,
    /// Bytes of each streaming buffer of a channel.
    pub stream_buffer_bytes: u64,
    /// Streaming buffers of each channel.
    pub stream_buffers_count: usize,
}

/// The C view of `io`.
fn nvme_options(io: &IOOptions) -> ffi::ustore_nvme_options_t {
    ffi::ustore_nvme_options_t {
        transport: io.transport as ffi::ustore_enum_t,
        shared_io_channels: io.shared_io_channels as ffi::ustore_size_t,
        queue_depth: io.queue_depth as ffi::ustore_size_t,
        request_buffer_bytes: io.request_buffer_bytes,
        request_buffers_count: io.request_buffers_count as ffi::ustore_size_t,
        stream_buffer_bytes: io.stream_buffer_bytes,
        stream_buffers_count: io.stream_buffers_count as ffi::ustore_size_t,
    }
}

// endregion: Options

// region: Databases

/// What an open database reports about itself, as `ustore_database_describe` answers it, where every size is the
/// engine's estimate, and zero where the engine keeps none.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Hash)]
pub struct Description {
    /// The capabilities of the engine, which the `supports_` methods read.
    capabilities: ffi::ustore_enum_t,
    /// The engine serving the database.
    pub engine: Engine,
    /// How transactions see each other: what the RAM engine was started with, and [`Isolation::Snapshot`] elsewhere.
    pub isolation: Isolation,
    /// Keys across every collection.
    pub keys: usize,
    /// Bytes of memory the engine holds for the data.
    pub memory_bytes: u64,
    /// Bytes the files of the database take on disk.
    pub disk_bytes: u64,
}

impl Description {
    /// Whether the engine supports transactions.
    pub fn supports_transactions(self) -> bool {
        self.capabilities & ffi::USTORE_CAPABILITIES_TRANSACTIONS_K != 0
    }

    /// Whether the engine supports snapshots.
    pub fn supports_snapshots(self) -> bool {
        self.capabilities & ffi::USTORE_CAPABILITIES_SNAPSHOTS_K != 0
    }
}

/// An open embedded database, which owns its directory until it is dropped.
///
/// No two processes may open one directory at once, and the handle is shared across threads freely;
/// the transactions taken from it are not. Dropping it waits for every deferred call naming it to finish.
#[derive(Debug)]
pub struct Database {
    handle: ffi::ustore_database_t,
}

// SAFETY: `db.h` promises the handle is safe to share across threads from open until free, and
// nothing in this binding keeps per-handle state of its own.
unsafe impl Send for Database {}
unsafe impl Sync for Database {}

impl Database {
    /// Starts an empty database of the in-memory engine, which persists nowhere.
    ///
    /// Calls `ustore_database_init_ram`.
    pub fn init_ram(options: &RAMOptions<'_>) -> Result<Self> {
        let mut database = Database {
            handle: ptr::null_mut(),
        };
        let mut outcome = ffi::ustore_outcome_t::default();
        let mut arguments = ffi::ustore_database_init_ram_t {
            database: &mut database.handle,
            executor: executor_handle(options.executor),
            outcome: &mut outcome,
            completion: ffi::ustore_completion_t::Blocking,
            isolation: options.isolation as ffi::ustore_enum_t,
            cpu_class: CPUClass::to_raw(options.cpu_class),
        };
        // SAFETY: every pointer in `arguments` outlives the call, which is final on return.
        unsafe { ffi::ustore_database_init_ram(&mut arguments) };
        check(outcome)?;
        database.opened()
    }

    /// Serves an existing directory in place through the in-memory engine, loading the image it holds, if any, and
    /// rewriting it whole on a flushed commit and on close.
    ///
    /// Calls `ustore_database_open_ram`.
    pub fn open_ram(directory: &str, options: &RAMOptions<'_>) -> Result<Self> {
        let mut database = Database {
            handle: ptr::null_mut(),
        };
        let mut outcome = ffi::ustore_outcome_t::default();
        let mut arguments = ffi::ustore_database_open_ram_t {
            database: &mut database.handle,
            executor: executor_handle(options.executor),
            outcome: &mut outcome,
            completion: ffi::ustore_completion_t::Blocking,
            directory: directory.as_bytes().into(),
            isolation: options.isolation as ffi::ustore_enum_t,
            cpu_class: CPUClass::to_raw(options.cpu_class),
        };
        // SAFETY: every pointer in `arguments` outlives the call, which is final on return.
        unsafe { ffi::ustore_database_open_ram(&mut arguments) };
        check(outcome)?;
        database.opened()
    }

    /// Serves a directory in place through the RocksDB engine, creating the database when it holds none, or fails
    /// with [`Status::Unsupported`] in a build without the engine.
    ///
    /// Calls `ustore_database_open_rocksdb`.
    pub fn open_rocksdb(options: &RocksDBOptions<'_>) -> Result<Self> {
        Database::rocksdb(options, ffi::ustore_database_open_rocksdb)
    }

    /// Serves an existing directory in place through the RocksDB engine, read-only, or fails with
    /// [`Status::Unsupported`] in a build without the engine.
    ///
    /// The database reads the state its directory held at open, and refuses every call that would change it with
    /// [`Status::ReadOnly`]: writes, removes, transactions, collection creates and drops, clears and compactions.
    /// Snapshots and their saves still serve.
    ///
    /// Calls `ustore_database_view_rocksdb`.
    pub fn view_rocksdb(options: &RocksDBOptions<'_>) -> Result<Self> {
        Database::rocksdb(options, ffi::ustore_database_view_rocksdb)
    }

    /// The database `open` serves over `options`, mutable or read-only.
    fn rocksdb(
        options: &RocksDBOptions<'_>,
        open: unsafe extern "C" fn(*mut ffi::ustore_database_open_rocksdb_t),
    ) -> Result<Self> {
        let data_directories = data_directories(options.data_directories)?;
        let mut database = Database {
            handle: ptr::null_mut(),
        };
        let mut outcome = ffi::ustore_outcome_t::default();
        let mut arguments = ffi::ustore_database_open_rocksdb_t {
            database: &mut database.handle,
            executor: executor_handle(options.executor),
            outcome: &mut outcome,
            completion: ffi::ustore_completion_t::Blocking,
            directory: options.directory.as_bytes().into(),
            data_directories: data_directories.as_ptr(),
            data_directories_count: data_directories.len() as ffi::ustore_size_t,
        };
        // SAFETY: every pointer in `arguments` outlives the call, which is final on return.
        unsafe { open(&mut arguments) };
        check(outcome)?;
        database.opened()
    }

    /// Serves a directory in place through the LevelDB engine, creating the database when it holds none, or fails
    /// with [`Status::Unsupported`] in a build without the engine.
    ///
    /// Calls `ustore_database_open_leveldb`.
    pub fn open_leveldb(options: &LevelDBOptions<'_>) -> Result<Self> {
        let mut database = Database {
            handle: ptr::null_mut(),
        };
        let mut outcome = ffi::ustore_outcome_t::default();
        let mut arguments = ffi::ustore_database_open_leveldb_t {
            database: &mut database.handle,
            executor: executor_handle(options.executor),
            outcome: &mut outcome,
            completion: ffi::ustore_completion_t::Blocking,
            directory: options.directory.as_bytes().into(),
        };
        // SAFETY: every pointer in `arguments` outlives the call, which is final on return.
        unsafe { ffi::ustore_database_open_leveldb(&mut arguments) };
        check(outcome)?;
        database.opened()
    }

    /// Serves the database a manifest names through the NVMe engine, or fails with
    /// [`Status::Unsupported`] in a build without it.
    ///
    /// Calls `ustore_database_open_nvme`.
    pub fn open_nvme(options: &NVMEOptions<'_>) -> Result<Self> {
        let data_directories = data_directories(options.data_directories)?;
        let mut database = Database {
            handle: ptr::null_mut(),
        };
        let mut outcome = ffi::ustore_outcome_t::default();
        let mut arguments = ffi::ustore_database_open_nvme_t {
            database: &mut database.handle,
            executor: executor_handle(options.executor),
            outcome: &mut outcome,
            completion: ffi::ustore_completion_t::Blocking,
            directory: options.manifest.as_bytes().into(),
            data_directories: data_directories.as_ptr(),
            data_directories_count: data_directories.len() as ffi::ustore_size_t,
            compactions: executor_handle(options.compactions),
            memory_limit: options.memory_limit,
            transaction_max_bytes: options.transaction_max_bytes,
            transaction_max_deltas: options.transaction_max_deltas as ffi::ustore_size_t,
            collection_concurrency_limit: options.collection_concurrency_limit as ffi::ustore_size_t,
            collection_cache_limit: options.collection_cache_limit,
            collection_memory_runs_max_bytes: options.collection_memory_runs_max_bytes,
            collection_memory_runs_max_deltas: options.collection_memory_runs_max_deltas as ffi::ustore_size_t,
            collection_level_enlarge_factor: options.collection_level_enlarge_factor as ffi::ustore_size_t,
            collection_value_size: options.collection_value_size,
            collection_value_max_size: options.collection_value_max_size,
            cpu_class: CPUClass::to_raw(options.cpu_class),
        };
        // SAFETY: every pointer in `arguments` outlives the call, the paths included.
        unsafe { ffi::ustore_database_open_nvme(&mut arguments) };
        check(outcome)?;
        database.opened()
    }

    /// This database once its init succeeded, refusing a handle the library left null.
    fn opened(self) -> Result<Self> {
        match self.handle.is_null() {
            true => Err(fail(Status::Internal, "the library returned no database and no error")),
            false => Ok(self),
        }
    }

    /// The engine behind this database, what it supports and how much it holds.
    ///
    /// Calls `ustore_database_describe`.
    pub fn describe(&self) -> Result<Description> {
        let mut description = ffi::ustore_database_description_t::default();
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
        let (Some(engine), Some(isolation)) = (
            Engine::from_raw(description.engine),
            Isolation::from_raw(description.isolation),
        ) else {
            return Err(fail(
                Status::Malformed,
                "the engine described itself in values a newer library added",
            ));
        };
        Ok(Description {
            capabilities: description.capabilities,
            engine,
            isolation,
            keys: description.keys as usize,
            memory_bytes: description.memory_bytes,
            disk_bytes: description.disk_bytes,
        })
    }

    /// Empties every collection at once at the head; see [`View::clear`].
    pub fn clear(&self, mode: DropMode) -> Result<()> {
        self.head().clear(mode)
    }

    /// Reclaims the space overwritten and removed data still takes, one collection at a time; see [`View::compact`].
    pub fn compact<'progress>(&self, progress: Option<&'progress mut ProgressCallback<'progress>>) -> Result<()> {
        self.head().compact(progress)
    }

    /// The head of the database, where writes land and reads see the latest committed state.
    pub fn head(&self) -> View<'_> {
        View {
            database: self.handle,
            view: ptr::null_mut(),
            transaction: ptr::null_mut(),
            snapshot: ptr::null_mut(),
            executor: ptr::null_mut(),
            watch: Watch::Keys,
            marker: PhantomData,
        }
    }

    /// Begins a transaction over this database, which conflicts rather than blocks.
    pub fn transaction(&self) -> Result<Transaction<'_>> {
        let mut handle: ffi::ustore_transaction_t = ptr::null_mut();
        let mut outcome = ffi::ustore_outcome_t::default();
        let mut arguments = ffi::ustore_transaction_init_t {
            transaction: &mut handle,
            database: self.handle,
            executor: ptr::null_mut(),
            outcome: &mut outcome,
            completion: ffi::ustore_completion_t::Blocking,
        };
        // SAFETY: every pointer in `arguments` outlives the call.
        unsafe { ffi::ustore_transaction_init(&mut arguments) };
        check(outcome)?;
        match handle.is_null() {
            true => Err(fail(
                Status::Internal,
                "the library returned no transaction and no error",
            )),
            false => Ok(Transaction { database: self, handle }),
        }
    }

    /// Pins a point-in-time view of the database, which it releases when dropped.
    pub fn snapshot(&self) -> Result<Snapshot<'_>> {
        let mut handle: ffi::ustore_snapshot_t = ptr::null_mut();
        let mut read_stamp = ReadStamp::default();
        let mut outcome = ffi::ustore_outcome_t::default();
        let mut arguments = ffi::ustore_snapshot_init_t {
            snapshot: &mut handle,
            database: self.handle,
            executor: ptr::null_mut(),
            outcome: &mut outcome,
            completion: ffi::ustore_completion_t::Blocking,
            read_stamp: &mut read_stamp.0,
        };
        // SAFETY: every pointer in `arguments` outlives the call.
        unsafe { ffi::ustore_snapshot_init(&mut arguments) };
        check(outcome)?;
        match handle.is_null() {
            true => Err(fail(Status::Internal, "the library returned no snapshot and no error")),
            false => Ok(Snapshot {
                database: self,
                handle,
                read_stamp,
            }),
        }
    }

    /// Creates an empty collection holding `modality` and binds `name` to it, refusing a name already in use.
    ///
    /// Calls `ustore_collection_create`.
    pub fn collection_create(&self, name: &str, modality: Modality) -> Result<CollectionID> {
        crate::strings::create_named(self.handle, name, modality)
    }

    /// Removes the contents of a collection, or the collection itself together with its name.
    pub fn collection_drop(&self, collection: CollectionID, mode: DropMode) -> Result<()> {
        self.head().collection_drop(collection, mode)
    }

    /// The raw handle the modality verbs pass across the boundary.
    pub(crate) fn handle(&self) -> ffi::ustore_database_t {
        self.handle
    }
}

impl Drop for Database {
    fn drop(&mut self) {
        // SAFETY: the handle came from a successful open, or is the null a failed one leaves, which the free ignores,
        // and is freed once, after every transaction and snapshot, which borrow it, while the library waits for the
        // deferred calls naming it, forgotten ones included.
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

/// The one view a call addresses: the head of a [`Database`], a [`Transaction`], or a [`Snapshot`], which is its
/// `Parent`, with the executor its calls run on and how they complete.
///
/// Every read, write and scan hangs off this type, so a call naming two views at once cannot be written. Reads
/// pass the `ustore_view_t` it holds, writes pass its transaction and whether that watches them, and the view is
/// as cheap to copy as the handles it borrows from its parent.
///
/// The parent decides which verbs exist beyond the reads every view serves: the head and a transaction write, only
/// the head clears, compacts and drops collections, only a transaction stages, commits, rolls back and resets, and
/// only a snapshot saves.
///
/// Every verb of a blocking view returns once its call is final and answers what it produced, on the calling thread
/// or, after [`View::on`], on that executor. The same verb of a [`DeferredView`], which [`View::deferred`] makes,
/// returns once its call is submitted and answers a [`Deferred`].
#[derive(Debug)]
pub struct View<'view, Parent = Database, Completion = Blocking> {
    database: ffi::ustore_database_t,
    view: ffi::ustore_view_t,
    transaction: ffi::ustore_transaction_t,
    snapshot: ffi::ustore_snapshot_t,
    executor: ffi::ustore_executor_t,
    watch: Watch,
    marker: PhantomData<(&'view Parent, Completion)>,
}

impl<Parent, Completion> Clone for View<'_, Parent, Completion> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<Parent, Completion> Copy for View<'_, Parent, Completion> {}

/// A view whose verbs defer their calls on an executor, each answering a [`Deferred`] once submitted.
pub type DeferredView<'view, Parent = Database> = View<'view, Parent, Deferring>;

impl<'view, Parent, Completion> View<'view, Parent, Completion> {
    /// This view with every call through it running on `executor`, after the calls submitted there before it.
    pub fn on<'with>(self, executor: &'with Executor) -> View<'with, Parent, Completion>
    where
        'view: 'with,
    {
        View {
            executor: executor.handle,
            ..self
        }
    }

    /// This view with every verb through it deferred on `executor`, answering a [`Deferred`] once submitted.
    pub fn deferred<'with>(self, executor: &'with Executor) -> DeferredView<'with, Parent>
    where
        'view: 'with,
    {
        View {
            database: self.database,
            view: self.view,
            transaction: self.transaction,
            snapshot: self.snapshot,
            executor: executor.handle,
            watch: self.watch,
            marker: PhantomData,
        }
    }

    /// The database every call through this view reaches.
    pub(crate) fn database(&self) -> ffi::ustore_database_t {
        self.database
    }

    /// The executor every call through this view runs on, null for the calling thread.
    pub(crate) fn executor(&self) -> ffi::ustore_executor_t {
        self.executor
    }

    /// What every read through this view sees, null at the head.
    pub(crate) fn view(&self) -> ffi::ustore_view_t {
        self.view
    }

    /// The transaction every write through this view joins, null at the head.
    pub(crate) fn transaction(&self) -> ffi::ustore_transaction_t {
        self.transaction
    }

    /// Whether the transaction records what the writes through this view touch.
    pub(crate) fn watch(&self) -> Watch {
        self.watch
    }

    /// The snapshot a listing through this view reads, null at the head and inside a transaction, whose listings
    /// read the head as well.
    pub(crate) fn snapshot(&self) -> ffi::ustore_snapshot_t {
        self.snapshot
    }
}

impl<Parent, Completion: sealed::Completion> View<'_, Parent, Completion> {
    /// Every collection the database holds at this view's snapshot or at the head, exported into the arena.
    ///
    /// Calls `ustore_collection_list`.
    pub fn collection_list<'arena>(&self, arena: &'arena mut Arena) -> Completion::Answer<CollectionList<'arena>> {
        let verb = CollectionList {
            arena,
            written: 0,
            collections: Address::default(),
        };
        Completion::answer(self, verb)
    }
}

impl<Completion: sealed::Completion> View<'_, Database, Completion> {
    /// Removes the contents of a collection, or the collection itself together with its name, which only the head
    /// does, since collections are created and dropped nowhere else.
    ///
    /// Calls `ustore_collection_drop`.
    pub fn collection_drop(&self, collection: CollectionID, mode: DropMode) -> Completion::Answer<CollectionDrop> {
        Completion::answer(self, CollectionDrop { collection, mode })
    }

    /// Empties every collection at once, as [`View::collection_drop`] empties one: every pair, keeping the
    /// collections and their names; every collection with its name; or every value, keeping the keys where the
    /// engine can.
    ///
    /// The RAM and RocksDB engines refuse to drop the collections while a snapshot is open, with
    /// [`Status::InvalidArgument`], since the snapshot belongs to its holder.
    ///
    /// Calls `ustore_database_clear`.
    pub fn clear(&self, mode: DropMode) -> Completion::Answer<Clear> {
        Completion::answer(self, Clear { mode })
    }

    /// Reclaims the space overwritten and removed data still takes: RocksDB and LevelDB flush and compact their
    /// trees, and the RAM engine frees the versions no reader needs and rewrites its image.
    ///
    /// The work runs one collection at a time, and `progress` counts the bytes of the collections compacted where the
    /// engine measures them, as RocksDB does, and one unit per collection elsewhere. A cancelled compaction stops
    /// between collections, keeping what it compacted.
    ///
    /// Calls `ustore_database_compact`.
    pub fn compact<'progress>(
        &self,
        progress: Option<&'progress mut ProgressCallback<'progress>>,
    ) -> Completion::Answer<Compact<'progress>>
    where
        Completion: sealed::Accepts<Option<&'progress mut ProgressCallback<'progress>>>,
    {
        let verb = Compact {
            progress: Progress::new(progress),
        };
        Completion::answer(self, verb)
    }
}

/// A listing of the collections a view sees; see [`View::collection_list`].
#[derive(Debug)]
pub struct CollectionList<'arena> {
    arena: &'arena mut Arena,
    written: ffi::ustore_size_t,
    collections: Address<CollectionID>,
}

impl<'arena> sealed::Calling for CollectionList<'arena> {
    type Arguments = ffi::ustore_collection_list_t;
    type Output = &'arena [CollectionID];

    fn arguments<Parent, Completion>(
        &mut self,
        view: &View<'_, Parent, Completion>,
        outcome: *mut ffi::ustore_outcome_t,
        completion: ffi::ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        Ok(Some(ffi::ustore_collection_list_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            snapshot: view.snapshot(),
            collections: &mut self.collections.0,
            modalities: ptr::null_mut(),
            names: ptr::null_mut(),
            written: &mut self.written,
            arena: self.arena.handle,
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_collection_list(arguments)
    }

    fn finish(self) -> Result<&'arena [CollectionID]> {
        // SAFETY: a successful listing writes `written` identifiers into the arena, which stays borrowed for as long
        // as the slice does.
        Ok(unsafe { exported_slice(self.collections.0, self.written as usize) })
    }
}

/// A removal of the contents of a collection, or of the collection with its name; see [`View::collection_drop`].
#[derive(Debug)]
pub struct CollectionDrop {
    collection: CollectionID,
    mode: DropMode,
}

impl sealed::Calling for CollectionDrop {
    type Arguments = ffi::ustore_collection_drop_t;
    type Output = ();

    fn arguments<Parent, Completion>(
        &mut self,
        view: &View<'_, Parent, Completion>,
        outcome: *mut ffi::ustore_outcome_t,
        completion: ffi::ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        Ok(Some(ffi::ustore_collection_drop_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            collection: self.collection,
            mode: self.mode as ffi::ustore_enum_t,
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_collection_drop(arguments)
    }

    fn finish(self) -> Result<()> {
        Ok(())
    }
}

/// An emptying of every collection at once; see [`View::clear`].
#[derive(Debug)]
pub struct Clear {
    mode: DropMode,
}

impl sealed::Calling for Clear {
    type Arguments = ffi::ustore_database_clear_t;
    type Output = ();

    fn arguments<Parent, Completion>(
        &mut self,
        view: &View<'_, Parent, Completion>,
        outcome: *mut ffi::ustore_outcome_t,
        completion: ffi::ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        Ok(Some(ffi::ustore_database_clear_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            mode: self.mode as ffi::ustore_enum_t,
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_database_clear(arguments)
    }

    fn finish(self) -> Result<()> {
        Ok(())
    }
}

/// A compaction of every collection; see [`View::compact`].
#[derive(Debug)]
pub struct Compact<'progress> {
    progress: Progress<'progress>,
}

impl sealed::Calling for Compact<'_> {
    type Arguments = ffi::ustore_database_compact_t;
    type Output = ();

    fn arguments<Parent, Completion>(
        &mut self,
        view: &View<'_, Parent, Completion>,
        outcome: *mut ffi::ustore_outcome_t,
        completion: ffi::ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let (progress, progress_state) = self.progress.lowered();
        Ok(Some(ffi::ustore_database_compact_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            progress,
            progress_state,
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_database_compact(arguments)
    }

    fn raise(&mut self) {
        self.progress.raise()
    }

    fn finish(self) -> Result<()> {
        Ok(())
    }
}

// endregion: Views

// region: Matching

/// Whether a predicate keeps a listed key or passes over it, as `ustore_admission_t` spells it.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub enum Admission {
    /// Pass over the key.
    #[default]
    Reject = 0,
    /// Keep the key among the listed ones.
    Admit = 1,
}

/// What the patterns of a [`Matching`] are matched as, as `ustore_pattern_kind_t` spells it.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub enum PatternKind {
    /// Regular expressions, of which a value matches at least one somewhere.
    #[default]
    Regex = 0,
    /// Literal substrings, of which a value holds at least one.
    Substrings = 1,
}

/// Whether patterns compare bytes as they are or after Unicode case folding, as `ustore_case_sensitivity_t` spells it.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub enum CaseSensitivity {
    /// Bytes as they are.
    #[default]
    Cased = 0,
    /// Full Unicode case folding of both the patterns and the values.
    Uncased = 1,
}

/// Where a scan asks its predicate, as `ustore_predicate_cost_t` spells it.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub enum PredicateCost {
    /// Once per page, before patterns or filters read any value, so it narrows those reads.
    #[default]
    Cheap = 0,
    /// Once per page about the keys patterns and filters kept, for handlers that pay per call.
    Expensive = 1,
}

impl PatternKind {
    fn from_raw(raw: ffi::ustore_enum_t) -> Option<Self> {
        match raw {
            0 => Some(PatternKind::Regex),
            1 => Some(PatternKind::Substrings),
            _ => None,
        }
    }
}

impl CaseSensitivity {
    fn from_raw(raw: ffi::ustore_enum_t) -> Option<Self> {
        match raw {
            0 => Some(CaseSensitivity::Cased),
            1 => Some(CaseSensitivity::Uncased),
            _ => None,
        }
    }
}

impl PredicateCost {
    fn from_raw(raw: ffi::ustore_enum_t) -> Option<Self> {
        match raw {
            0 => Some(PredicateCost::Cheap),
            1 => Some(PredicateCost::Expensive),
            _ => None,
        }
    }
}

impl fmt::Display for PatternKind {
    /// Calls `ustore_pattern_kind_name`.
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(spelling(ffi::ustore_pattern_kind_name, *self as ffi::ustore_enum_t))
    }
}

impl FromStr for PatternKind {
    type Err = Error;

    /// Calls `ustore_pattern_kind_from_name`.
    fn from_str(text: &str) -> Result<Self> {
        read_spelling(
            text,
            ffi::ustore_pattern_kind_from_name,
            ffi::ustore_pattern_kind_name,
            PatternKind::from_raw,
        )
    }
}

impl fmt::Display for CaseSensitivity {
    /// Calls `ustore_case_sensitivity_name`.
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(spelling(ffi::ustore_case_sensitivity_name, *self as ffi::ustore_enum_t))
    }
}

impl FromStr for CaseSensitivity {
    type Err = Error;

    /// Calls `ustore_case_sensitivity_from_name`.
    fn from_str(text: &str) -> Result<Self> {
        read_spelling(
            text,
            ffi::ustore_case_sensitivity_from_name,
            ffi::ustore_case_sensitivity_name,
            CaseSensitivity::from_raw,
        )
    }
}

impl fmt::Display for PredicateCost {
    /// Calls `ustore_predicate_cost_name`.
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(spelling(ffi::ustore_predicate_cost_name, *self as ffi::ustore_enum_t))
    }
}

impl FromStr for PredicateCost {
    type Err = Error;

    /// Calls `ustore_predicate_cost_from_name`.
    fn from_str(text: &str) -> Result<Self> {
        read_spelling(
            text,
            ffi::ustore_predicate_cost_from_name,
            ffi::ustore_predicate_cost_name,
            PredicateCost::from_raw,
        )
    }
}

/// A caller's verdict on one page of listed keys, writing one [`Admission`] per key, every one starting as
/// [`Admission::Reject`].
///
/// It runs on whichever thread runs the call, never on two at once within one call, so it owes `Send` and may be
/// `FnMut`. A panic inside it rejects the rest of the call's keys and resurfaces once the call is final.
///
/// Mirrors `ustore_predicate_callback_t`.
pub type PredicateCallback<'handler> = dyn FnMut(&[Key], &mut [Admission]) + Send + 'handler;

/// What a scan or a cursor narrows its keys by, every part left empty to keep every key: patterns over the values of
/// blobs, a JSONPath or XPath filter over documents and pages, and a predicate over the keys.
///
/// Mirrors the `patterns`, `filter` and `predicate` fields of `ustore_blobs_scan_t` and its kin.
#[derive(Default)]
pub struct Matching<'matching> {
    /// The patterns every listed value matches one of, compiled once per call or once per cursor.
    pub patterns: &'matching [&'matching str],
    /// What the patterns are matched as.
    pub pattern_kind: PatternKind,
    /// Whether matching folds case.
    pub sensitivity: CaseSensitivity,
    /// One weight per substring pattern, which turns on BM25 scores.
    pub weights: Option<&'matching [f32]>,
    /// The JSONPath, root filter or XPath every listed document or page matches.
    pub filter: Option<&'matching str>,
    /// The caller's verdict on the keys of every page.
    pub predicate: Option<&'matching mut PredicateCallback<'matching>>,
    /// Where the predicate is asked.
    pub predicate_cost: PredicateCost,
}

impl fmt::Debug for Matching<'_> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("Matching")
            .field("patterns", &self.patterns)
            .field("pattern_kind", &self.pattern_kind)
            .field("sensitivity", &self.sensitivity)
            .field("weights", &self.weights)
            .field("filter", &self.filter)
            .field("predicate", &self.predicate.is_some())
            .field("predicate_cost", &self.predicate_cost)
            .finish()
    }
}

/// The matching one call or cursor lends the library, and the first panic its predicate raised, which resurfaces once
/// the call is final, since unwinding out of an `extern "C"` frame aborts.
pub(crate) struct Matched<'matching> {
    matching: Matching<'matching>,
    panic: Option<Box<dyn Any + Send>>,
}

impl<'matching> Matched<'matching> {
    pub(crate) fn new(matching: Matching<'matching>) -> Self {
        Matched { matching, panic: None }
    }

    /// The patterns as the ABI takes them, viewing what `self` borrows.
    pub(crate) fn patterns(&self) -> ffi::ustore_patterns_t {
        let patterns = self.matching.patterns;
        if patterns.is_empty() {
            return ffi::ustore_patterns_t::default();
        }
        ffi::ustore_patterns_t {
            patterns: ffi::ustore_strings_view_t::accessor(string_views, patterns.as_ptr().cast()),
            count: patterns.len() as ffi::ustore_size_t,
            kind: self.matching.pattern_kind as ffi::ustore_enum_t,
            sensitivity: self.matching.sensitivity as ffi::ustore_enum_t,
            weights: self.matching.weights.map_or(ptr::null(), <[f32]>::as_ptr),
        }
    }

    /// The filter as the ABI takes it, viewing what `self` borrows.
    pub(crate) fn filter(&self) -> ffi::ustore_bytes_view_t {
        self.matching
            .filter
            .map_or_else(ffi::ustore_bytes_view_t::default, |filter| filter.as_bytes().into())
    }

    /// Whether the patterns carry weights, so a scan writes scores.
    pub(crate) fn scores(&self) -> bool {
        self.matching.weights.is_some()
    }

    /// The predicate as the ABI takes it, addressing `self`, which stays in place until the call or the cursor ends.
    pub(crate) fn predicate(&mut self) -> ffi::ustore_predicate_t {
        match self.matching.predicate {
            Some(_) => ffi::ustore_predicate_t {
                callback: Some(predicate_trampoline),
                state: ptr::from_mut(self).cast(),
                cost: self.matching.predicate_cost as ffi::ustore_enum_t,
            },
            None => ffi::ustore_predicate_t::default(),
        }
    }

    /// Re-raises the first panic the predicate raised, once the call is final.
    pub(crate) fn raise(&mut self) {
        if let Some(panic) = self.panic.take() {
            std::panic::resume_unwind(panic)
        }
    }
}

impl fmt::Debug for Matched<'_> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("Matched")
            .field("matching", &self.matching)
            .finish_non_exhaustive()
    }
}

/// Writes the view of every `&str` that `indices` names out of the slice `handle` addresses.
unsafe extern "C" fn string_views(
    handle: *const c_void,
    indices: *const ffi::ustore_size_t,
    count: ffi::ustore_size_t,
    views: *mut ffi::ustore_bytes_view_t,
) {
    let texts = handle.cast::<&str>();
    for slot in 0..count as usize {
        let text = *texts.add(*indices.add(slot) as usize);
        *views.add(slot) = text.as_bytes().into();
    }
}

/// Runs the predicate of the [`Matched`] at `state` over one page, rejecting every key on a panic.
///
/// # Safety
/// `state` addresses the `Matched` of the call or cursor the predicate was passed to, which nothing else reaches until
/// the callback returns, and `keys` and `admissions` hold `count` entries each.
unsafe extern "C" fn predicate_trampoline(
    keys: *const ffi::ustore_key_t,
    admissions: *mut ffi::ustore_enum_t,
    count: ffi::ustore_size_t,
    state: *mut c_void,
) {
    let matched = &mut *state.cast::<Matched<'_>>();
    let count = count as usize;
    // SAFETY: the library wrote a rejection into every admission, a valid `Admission` of the same width.
    let admissions = std::slice::from_raw_parts_mut(admissions.cast::<Admission>(), count);
    let Some(handler) = matched.matching.predicate.as_mut() else {
        return;
    };
    if matched.panic.is_some() {
        return;
    }
    let keys = std::slice::from_raw_parts(keys, count);
    if let Err(panic) = std::panic::catch_unwind(AssertUnwindSafe(|| handler(keys, &mut *admissions))) {
        admissions.fill(Admission::Reject);
        matched.panic = Some(panic);
    }
}

// endregion: Matching

// region: Progress

/// A periodic report of the units a call finished and the units it finishes if it runs to the end, where
/// `ControlFlow::Break` cancels the call, which then fails with [`Status::Cancelled`] and publishes nothing.
///
/// It runs on whichever thread advanced the work, never on two at once and never after the call is final, so it owes
/// `Send` and may be `FnMut`. A panic inside it cancels the call as well, and resurfaces once the call is final.
///
/// Mirrors `ustore_progress_callback_t`.
pub type ProgressCallback<'handler> = dyn FnMut(usize, usize) -> ControlFlow<()> + Send + 'handler;

/// The progress handler one call lends the library, and the first panic it raised, which resurfaces once the call is
/// final, since unwinding out of an `extern "C"` frame aborts.
pub(crate) struct Progress<'handler> {
    handler: Option<&'handler mut ProgressCallback<'handler>>,
    panic: Option<Box<dyn Any + Send>>,
}

impl<'handler> Progress<'handler> {
    pub(crate) fn new(handler: Option<&'handler mut ProgressCallback<'handler>>) -> Self {
        Progress { handler, panic: None }
    }

    /// The callback and state a reporting verb passes, addressing `self`, which stays in place until the call is
    /// final, or nulls without a handler.
    pub(crate) fn lowered(&mut self) -> (ffi::ustore_progress_callback_t, *mut c_void) {
        match self.handler {
            Some(_) => (Some(progress_trampoline), ptr::from_mut(self).cast()),
            None => (None, ptr::null_mut()),
        }
    }

    /// Re-raises the first panic the handler raised, once the call is final.
    pub(crate) fn raise(&mut self) {
        if let Some(panic) = self.panic.take() {
            std::panic::resume_unwind(panic)
        }
    }
}

impl fmt::Debug for Progress<'_> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("Progress")
            .field("reporting", &self.handler.is_some())
            .finish_non_exhaustive()
    }
}

/// Runs the handler of the [`Progress`] at `state`, cancelling the call on a `Break` or a panic.
///
/// # Safety
/// `state` addresses the `Progress` of the call the callback was passed to, which nothing else reaches until the
/// callback returns, since the library never runs one callback concurrently with itself.
unsafe extern "C" fn progress_trampoline(
    finished: ffi::ustore_size_t,
    total: ffi::ustore_size_t,
    state: *mut c_void,
) -> ffi::ustore_enum_t {
    let progress = &mut *state.cast::<Progress<'_>>();
    let Some(handler) = progress.handler.as_mut() else {
        return ffi::USTORE_CONTINUE_K;
    };
    match std::panic::catch_unwind(AssertUnwindSafe(|| handler(finished as usize, total as usize))) {
        Ok(ControlFlow::Continue(())) => ffi::USTORE_CONTINUE_K,
        Ok(ControlFlow::Break(())) => ffi::USTORE_CANCEL_K,
        Err(panic) => {
            progress.panic.get_or_insert(panic);
            ffi::USTORE_CANCEL_K
        }
    }
}

// endregion: Progress

// region: Calls

/// The traits only this crate implements.
pub(crate) mod sealed {
    use super::{ffi, Result, View};

    /// One verb over one `#[repr(C)]` argument struct: where the arguments point, which C entry point takes them,
    /// and what a successful call hands back.
    pub trait Calling: Sized {
        /// The argument struct of the verb's C entry point.
        type Arguments;
        /// What a successful call hands back.
        type Output;

        /// The arguments over the inputs and outputs this verb holds, or `None` for a batch that reaches no engine.
        ///
        /// The arguments point into `self`, so the verb stays in place until the call they feed is final.
        fn arguments<Parent, Completion>(
            &mut self,
            view: &View<'_, Parent, Completion>,
            outcome: *mut ffi::ustore_outcome_t,
            completion: ffi::ustore_completion_t,
        ) -> Result<Option<Self::Arguments>>;

        /// Passes `arguments` to the verb's C entry point.
        ///
        /// # Safety
        /// Everything `arguments` points to stays alive and in place until the outcome is final.
        unsafe fn call(arguments: *mut Self::Arguments);

        /// Re-raises the first panic a handler the verb lent raised, once the call is final.
        fn raise(&mut self) {}

        /// What the verb hands back once its outcome is a success.
        fn finish(self) -> Result<Self::Output>;
    }

    /// How the verbs of a view complete, which decides what each of them answers.
    pub trait Completion: Sized {
        /// What a verb answers: `Result<Output>` when blocking, `Result<Deferred<Verb>>` when deferring.
        type Answer<Verb: Calling>;

        /// Runs `verb` through `view`, or submits it there.
        fn answer<Parent, Verb: Calling>(view: &View<'_, Parent, Self>, verb: Verb) -> Self::Answer<Verb>;
    }

    /// The inputs a completion takes: a blocking view takes borrows and owned values alike, while a deferring one
    /// takes only what it owns outright, since the library reads its inputs after the verb returns. Both borrow the
    /// arena, which no call reads as an input, and whose free waits for the calls writing into it.
    #[diagnostic::on_unimplemented(
        message = "a deferred view needs owned inputs",
        label = "the library would still read this borrow after the verb returns",
        note = "pass keys or edges in an array or a `Vec`, keys beside a `BytesTape` or a `CharsTape`, paths in a \
                `CharsTape`, columns in a `Vec<Column<'static>>`, names as a `String`, and `'static` collections, \
                roles, fields, names and progress handlers"
    )]
    pub trait Accepts<Input> {}

    /// The parents whose views write: the head of a database and a transaction, while a snapshot only reads.
    pub trait Writable {}

    /// The offsets of a tape the library reads as it stands: 32-bit or 64-bit.
    pub trait TapeOffset: stringtape::OffsetType {
        /// The view of the tape whose `offsets` split `bytes`, one more offset than strings.
        fn tape(bytes: &[u8], offsets: &[Self]) -> ffi::ustore_strings_view_t;
    }
}

/// The completion of a [`View`], whose verbs return once their call is final and answer what it produced.
#[derive(Clone, Copy, Debug)]
pub struct Blocking;

/// The completion of a [`DeferredView`], whose verbs return once their call is submitted and answer a [`Deferred`].
#[derive(Clone, Copy, Debug)]
pub struct Deferring;

impl sealed::Completion for Blocking {
    type Answer<Verb: sealed::Calling> = Result<Verb::Output>;

    fn answer<Parent, Verb: sealed::Calling>(
        view: &View<'_, Parent, Blocking>,
        mut verb: Verb,
    ) -> Result<Verb::Output> {
        let mut outcome = ffi::ustore_outcome_t::default();
        if let Some(mut arguments) = verb.arguments(view, &mut outcome, ffi::ustore_completion_t::Blocking)? {
            // SAFETY: the arguments point into `verb` and `outcome`, which stay in place until the call returns, and
            // a blocking call is final on return.
            unsafe { Verb::call(&mut arguments) };
            verb.raise();
            check(outcome)?;
        }
        verb.finish()
    }
}

impl sealed::Completion for Deferring {
    type Answer<Verb: sealed::Calling> = Result<Deferred<Verb>>;

    fn answer<Parent, Verb: sealed::Calling>(view: &View<'_, Parent, Deferring>, verb: Verb) -> Result<Deferred<Verb>> {
        let lent = Box::into_raw(Box::new(Lent {
            outcome: ffi::ustore_outcome_t::default(),
            verb,
        }));
        // SAFETY: the box is fresh and reached through `lent` alone, and the arguments point into its fields, where
        // they stay until the box is freed, while submission copies the arguments themselves. A verb that reaches no
        // engine leaves the outcome a final success.
        unsafe {
            let outcome = ptr::addr_of_mut!((*lent).outcome);
            match (*lent)
                .verb
                .arguments(view, outcome, ffi::ustore_completion_t::Deferred)
            {
                Ok(Some(mut arguments)) => Verb::call(&mut arguments),
                Ok(None) => {}
                Err(refusal) => {
                    drop(Box::from_raw(lent));
                    return Err(refusal);
                }
            }
            Ok(Deferred {
                lent: NonNull::new_unchecked(lent),
            })
        }
    }
}

impl<Input> sealed::Accepts<Input> for Blocking {}

// Owned inputs alone, so a forgotten `Deferred` leaks them rather than lending the library memory freed under it
impl sealed::Accepts<Vec<Key>> for Deferring {}
impl<const COUNT: usize> sealed::Accepts<[Key; COUNT]> for Deferring {}
impl<Keys, Offset: stringtape::OffsetType> sealed::Accepts<(Keys, BytesTape<Offset>)> for Deferring where
    Deferring: sealed::Accepts<Keys>
{
}
impl<Keys, Offset: stringtape::OffsetType> sealed::Accepts<(Keys, CharsTape<Offset>)> for Deferring where
    Deferring: sealed::Accepts<Keys>
{
}
impl<Keys, Paths, Values> sealed::Accepts<(Keys, Paths, Values)> for Deferring where
    Deferring: sealed::Accepts<Keys> + sealed::Accepts<Paths> + sealed::Accepts<Values>
{
}
impl<Field, Texts> sealed::Accepts<KeyedBy<Field, Texts>> for Deferring where
    Deferring: sealed::Accepts<Field> + sealed::Accepts<Texts>
{
}
impl<Offset: stringtape::OffsetType> sealed::Accepts<CharsTape<Offset>> for Deferring {}
impl sealed::Accepts<Vec<Column<'static>>> for Deferring {}
impl<const COUNT: usize> sealed::Accepts<[Column<'static>; COUNT]> for Deferring {}
impl sealed::Accepts<String> for Deferring {}
impl sealed::Accepts<&'static str> for Deferring {}
impl sealed::Accepts<Vec<Edge>> for Deferring {}
impl<const COUNT: usize> sealed::Accepts<[Edge; COUNT]> for Deferring {}
impl sealed::Accepts<Collections<'static>> for Deferring {}
impl sealed::Accepts<Roles<'static>> for Deferring {}
impl sealed::Accepts<Option<&'static mut ProgressCallback<'static>>> for Deferring {}
impl sealed::Accepts<Matching<'static>> for Deferring {}

impl sealed::Writable for Database {}
impl sealed::Writable for Transaction<'_> {}

/// A submitted call, which owns its outcome, inputs and output slots in one box the library reads until the call is
/// final, and borrows its arena as a blocking call does.
///
/// [`Deferred::status`] polls without blocking, and [`Deferred::wait`] blocks until the call is final and hands back
/// what it produced, while `.await` polls it from async code. Dropping an unfinished one waits for it, then frees the
/// box. A panic its progress handler raised resurfaces from `wait`, `.await`, or a drop outside unwinding.
///
/// Rust-only; polling calls `ustore_outcome_status`, and waiting and dropping call `ustore_outcome_wait`.
#[must_use = "a deferred call's outcome is seen only through `status`, `wait` or `.await`"]
pub struct Deferred<Verb: sealed::Calling> {
    lent: NonNull<Lent<Verb>>,
}

impl<Verb: sealed::Calling> Deferred<Verb> {
    /// `Err(Status::Deferred)` until the call is final, then its final status, read without blocking.
    ///
    /// Calls `ustore_outcome_status`.
    pub fn status(&self) -> std::result::Result<(), Status> {
        // SAFETY: the outcome lives in the box this `Deferred` owns until it is final.
        Status::from_raw(unsafe { ffi::ustore_outcome_status(ptr::addr_of_mut!((*self.lent.as_ptr()).outcome)) })
    }

    /// Blocks until the call is final, then hands back what it produced.
    ///
    /// Calls `ustore_outcome_wait`.
    pub fn wait(self) -> Result<Verb::Output> {
        let lent = ManuallyDrop::new(self).lent.as_ptr();
        // SAFETY: once the outcome is final the library touches the box no more, so it is freed here.
        let Lent { outcome, mut verb } = unsafe {
            ffi::ustore_outcome_wait(ptr::addr_of_mut!((*lent).outcome));
            *Box::from_raw(lent)
        };
        verb.raise();
        check(outcome)?;
        verb.finish()
    }
}

impl<Verb: sealed::Calling> Drop for Deferred<Verb> {
    fn drop(&mut self) {
        let lent = self.lent.as_ptr();
        // SAFETY: once the outcome is final the library touches the box no more, and this `Deferred` owned it.
        let Lent { mut verb, .. } = unsafe {
            ffi::ustore_outcome_wait(ptr::addr_of_mut!((*lent).outcome));
            *Box::from_raw(lent)
        };
        // A second panic while unwinding would abort, so only a drop outside one re-raises.
        if !std::thread::panicking() {
            verb.raise();
        }
    }
}

impl<Verb: sealed::Calling> fmt::Debug for Deferred<Verb> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "Deferred({:?})", self.status())
    }
}

// SAFETY: the box moves with the `Deferred`, and until its call is final only the library reaches it.
unsafe impl<Verb: sealed::Calling + Send> Send for Deferred<Verb> {}

impl<Verb: sealed::Calling> IntoFuture for Deferred<Verb> {
    type Output = Result<Verb::Output>;
    type IntoFuture = Wait<Verb>;

    fn into_future(self) -> Wait<Verb> {
        Wait(Some(self))
    }
}

/// A [`Deferred`] being awaited: every poll reads its status, and one not yet final asks to be polled again.
///
/// The C library has no wake-up, so an executor re-polls it while other tasks run, spinning only when nothing else is
/// ready; [`Deferred::wait`] blocks without spinning.
///
/// Rust-only; every poll calls `ustore_outcome_status`, and the final one `ustore_outcome_wait`, which returns at once.
#[must_use = "an awaited call does nothing unless polled"]
pub struct Wait<Verb: sealed::Calling>(Option<Deferred<Verb>>);

impl<Verb: sealed::Calling> Future for Wait<Verb> {
    type Output = Result<Verb::Output>;

    fn poll(self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
        let awaited = &mut self.get_mut().0;
        match awaited.take() {
            None => Poll::Ready(Err(fail(Status::InvalidArgument, "the call was already awaited"))),
            Some(call) if call.status() == Err(Status::Deferred) => {
                *awaited = Some(call);
                context.waker().wake_by_ref();
                Poll::Pending
            }
            Some(call) => Poll::Ready(call.wait()),
        }
    }
}

/// One deferred call and everything it lends the library, at one heap address until its status is final.
///
/// The argument struct is not among them, since submission copies it.
struct Lent<Verb: sealed::Calling> {
    /// The outcome the library finishes.
    outcome: ffi::ustore_outcome_t,
    /// The inputs, the output slots and the arena borrow.
    verb: Verb,
}

// endregion: Calls

// region: Executors

/// The threads that run calls, shared by any number of databases and views.
///
/// Calls on one executor start in submission order, and a blocking call runs alone: after every call submitted before
/// it finishes, and before any submitted after it starts. An executor holds at most `deferred_limit` deferred calls at
/// once, so a submission finding its deferred table full finishes at once with [`Status::Contended`]. Dropping the
/// executor drains it.
#[derive(Debug)]
pub struct Executor {
    handle: ffi::ustore_executor_t,
}

// SAFETY: `types.h` shares an executor between any number of databases and threads.
unsafe impl Send for Executor {}
unsafe impl Sync for Executor {}

impl Executor {
    /// Spawns host threads for the RAM, RocksDB and LevelDB engines across the compute domains local to
    /// `memory_domains`, NUMA nodes as the operating system numbers them, holding up to `deferred_limit` deferred calls
    /// at once, or 256 for zero.
    ///
    /// An empty list derives every domain the process may allocate on, and an identifier no domain carries is refused
    /// with [`Status::InvalidArgument`]. The scratch of each thread, and every arena the executor makes, lives on the
    /// domains named. Zero `threads` takes 7/8 of the cores those domains carry, rounded down and at least one, which
    /// leaves the rest to the driver thread, to blocking calls and to the background work of RocksDB and LevelDB.
    ///
    /// Calls `ustore_executor_for_ram`.
    pub fn for_ram(memory_domains: &[i32], threads: usize, deferred_limit: usize) -> Result<Self> {
        let mut handle: ffi::ustore_executor_t = ptr::null_mut();
        let mut outcome = ffi::ustore_outcome_t::default();
        let mut arguments = ffi::ustore_executor_for_ram_t {
            executor: &mut handle,
            outcome: &mut outcome,
            ram: ram_options(memory_domains),
            threads: threads as ffi::ustore_size_t,
            deferred_limit: deferred_limit as ffi::ustore_size_t,
            cpu_class: CPUClass::to_raw(None),
        };
        // SAFETY: every pointer in `arguments` outlives the call, which is final on return and copies the domains.
        unsafe { ffi::ustore_executor_for_ram(&mut arguments) };
        Executor::created(handle, outcome)
    }

    /// Creates an executor for the RAM, RocksDB and LevelDB engines over a ForkUnion pool it borrows, so several
    /// libraries in one process share one set of threads, while it keeps their scratch on `memory_domains`.
    ///
    /// A batch finding the pool running its lender's dispatch runs on the executor's own driver thread instead of
    /// forking over the lender.
    ///
    /// Calls `ustore_executor_on_pool`.
    ///
    /// # Safety
    /// `pool` must address a live ForkUnion `fu_pool_t` that outlives the executor.
    pub unsafe fn on_pool(memory_domains: &[i32], pool: *mut c_void, deferred_limit: usize) -> Result<Self> {
        let mut handle: ffi::ustore_executor_t = ptr::null_mut();
        let mut outcome = ffi::ustore_outcome_t::default();
        let mut arguments = ffi::ustore_executor_on_pool_t {
            executor: &mut handle,
            outcome: &mut outcome,
            ram: ram_options(memory_domains),
            pool,
            deferred_limit: deferred_limit as ffi::ustore_size_t,
        };
        // SAFETY: every pointer in `arguments` outlives the call, which is final on return, and the caller vouches
        // for the pool.
        ffi::ustore_executor_on_pool(&mut arguments);
        Executor::created(handle, outcome)
    }

    /// Spawns the threads of the NVMe engine, each submitting and polling its own I/O ring, where zero `threads`
    /// derives them from the cores, and tells every database [`Database::open_nvme`] opens on it to move bytes as
    /// `io` says.
    ///
    /// Calls `ustore_executor_for_nvme`.
    pub fn for_nvme(threads: usize, io: &IOOptions, deferred_limit: usize) -> Result<Self> {
        let mut handle: ffi::ustore_executor_t = ptr::null_mut();
        let mut outcome = ffi::ustore_outcome_t::default();
        let mut arguments = ffi::ustore_executor_for_nvme_t {
            executor: &mut handle,
            outcome: &mut outcome,
            nvme: nvme_options(io),
            threads: threads as ffi::ustore_size_t,
            deferred_limit: deferred_limit as ffi::ustore_size_t,
            cpu_class: CPUClass::to_raw(None),
        };
        // SAFETY: every pointer in `arguments` outlives the call, which is final on return.
        unsafe { ffi::ustore_executor_for_nvme(&mut arguments) };
        Executor::created(handle, outcome)
    }

    /// Opens one submission stream on `device`, a [`GPUDevice::ordinal`] of either vendor, or borrows the caller's
    /// `stream`, all of it driven by one host thread - which is why this takes no thread count - moving bytes as `gds`
    /// says. Zero `scratch_limit` lets scratch grow as calls ask. It fails with [`Status::Device`] for an ordinal no
    /// device carries. [`NVMEOptions::compactions`] places the merges of an NVMe database on one of an NVIDIA device.
    ///
    /// Calls `ustore_executor_for_vram`.
    ///
    /// # Safety
    /// `stream` must be null or a live stream on `device` that outlives the executor: a `cudaStream_t` on NVIDIA and a
    /// `hipStream_t` on AMD.
    pub unsafe fn for_vram(
        device: i32,
        stream: *mut c_void,
        scratch_limit: usize,
        gds: Gds,
        deferred_limit: usize,
    ) -> Result<Self> {
        let mut handle: ffi::ustore_executor_t = ptr::null_mut();
        let mut outcome = ffi::ustore_outcome_t::default();
        let mut arguments = ffi::ustore_executor_for_vram_t {
            executor: &mut handle,
            outcome: &mut outcome,
            device,
            stream,
            scratch_limit: scratch_limit as ffi::ustore_size_t,
            gds,
            deferred_limit: deferred_limit as ffi::ustore_size_t,
        };
        // SAFETY: every pointer in `arguments` outlives the call, which is final on return, and the caller vouches
        // for the stream.
        ffi::ustore_executor_for_vram(&mut arguments);
        Executor::created(handle, outcome)
    }

    /// The executor a constructor wrote, refusing a handle the library left null.
    fn created(handle: ffi::ustore_executor_t, outcome: ffi::ustore_outcome_t) -> Result<Self> {
        check(outcome)?;
        match handle.is_null() {
            true => Err(fail(Status::Internal, "the library returned no executor and no error")),
            false => Ok(Executor { handle }),
        }
    }

    /// Waits for every call submitted before the drain began, then leaves the threads in `mood` whenever they next go
    /// idle.
    pub fn drain(&self, mood: Mood) -> Result<()> {
        let mut outcome = ffi::ustore_outcome_t::default();
        let mut arguments = ffi::ustore_executor_drain_t {
            executor: self.handle,
            outcome: &mut outcome,
            mood,
        };
        // SAFETY: every pointer in `arguments` outlives the call, which is final on return.
        unsafe { ffi::ustore_executor_drain(&mut arguments) };
        check(outcome)
    }
}

impl Drop for Executor {
    fn drop(&mut self) {
        // SAFETY: the handle came from a successful creation and is freed once, which drains every call first.
        unsafe { ffi::ustore_executor_free(self.handle) };
    }
}

// endregion: Executors

// region: Transactions

/// An optimistic transaction grouping reads and writes across collections and modalities.
///
/// It serves one call at a time, so a call on a transaction that serves an unfinished deferred one finishes with
/// [`Status::Contended`]. It is freed before its database closes, which the borrow of the database carries, and
/// dropping it waits for every deferred call naming it to finish.
#[derive(Debug)]
pub struct Transaction<'db> {
    database: &'db Database,
    handle: ffi::ustore_transaction_t,
}

impl<'db> Transaction<'db> {
    /// The view every call inside this transaction addresses, watching the keys it touches or not.
    pub fn view(&self, watch: Watch) -> View<'_, Transaction<'db>> {
        View {
            database: self.database.handle(),
            // SAFETY: the transaction is open, and its view tags the handle without allocating.
            view: unsafe { ffi::ustore_transaction_view(self.handle, watch) },
            transaction: self.handle,
            snapshot: ptr::null_mut(),
            executor: ptr::null_mut(),
            watch,
            marker: PhantomData,
        }
    }

    /// The view of this transaction watching the keys it touches, with every call through it running on `executor`.
    pub fn on<'with>(&'with self, executor: &'with Executor) -> View<'with, Transaction<'db>> {
        self.view(Watch::Keys).on(executor)
    }

    /// The view of this transaction watching the keys it touches, with every verb deferred on `executor`.
    pub fn deferred<'with>(&'with self, executor: &'with Executor) -> DeferredView<'with, Transaction<'db>> {
        self.view(Watch::Keys).deferred(executor)
    }

    /// Stages the writes as the first phase of a two-phase commit, reporting the stamp the transaction reads at.
    pub fn stage(&mut self) -> Result<ReadStamp> {
        self.view(Watch::Keys).stage()
    }

    /// Commits the writes, reporting the commit's position in commit order.
    ///
    /// A conflict is reported rather than retried, and the transaction keeps its contents either way, so a caller
    /// may stage them again after [`Transaction::rollback`] or start over after [`Transaction::reset`].
    pub fn commit(&mut self, durability: Durability) -> Result<CommitStamp> {
        self.view(Watch::Keys).commit(durability)
    }

    /// Withdraws a stage, releasing whatever it claimed, while keeping the reads and writes to stage again.
    pub fn rollback(&mut self) -> Result<()> {
        self.view(Watch::Keys).rollback()
    }

    /// Discards the reads and writes and reopens the transaction on the newest state, ready for another try.
    pub fn reset(&mut self) -> Result<()> {
        self.view(Watch::Keys).reset()
    }
}

impl Drop for Transaction<'_> {
    fn drop(&mut self) {
        // SAFETY: the handle came from a successful begin and is freed once, after every view, which borrows this
        // transaction, while the library waits for the deferred calls naming it, forgotten ones included.
        unsafe { ffi::ustore_transaction_free(self.handle) };
    }
}

// SAFETY: a transaction may move to another thread with its database borrow; it stays `!Sync`, and the library
// refuses a second call while one is unfinished, which is all the C interface asks of it.
unsafe impl Send for Transaction<'_> {}

impl<Completion: sealed::Completion> View<'_, Transaction<'_>, Completion> {
    /// Stages the writes of this view's transaction as the first phase of a two-phase commit, reporting the stamp the
    /// transaction reads at.
    pub fn stage(&self) -> Completion::Answer<Stage> {
        let verb = Stage {
            stamp: ReadStamp::default(),
        };
        Completion::answer(self, verb)
    }

    /// Commits the writes of this view's transaction, reporting the commit's position in commit order.
    pub fn commit(&self, durability: Durability) -> Completion::Answer<Commit> {
        let verb = Commit {
            durability,
            stamp: CommitStamp::default(),
        };
        Completion::answer(self, verb)
    }

    /// Withdraws a stage of this view's transaction, releasing whatever it claimed, while keeping the reads and
    /// writes to stage again.
    pub fn rollback(&self) -> Completion::Answer<Rollback> {
        Completion::answer(self, Rollback)
    }

    /// Discards the reads and writes of this view's transaction and reopens it on the newest state, keeping its
    /// handle.
    pub fn reset(&self) -> Completion::Answer<Reset> {
        let verb = Reset {
            transaction: Address::default(),
        };
        Completion::answer(self, verb)
    }
}

/// The first phase of a two-phase commit; see [`View::stage`].
#[derive(Debug)]
pub struct Stage {
    stamp: ReadStamp,
}

impl sealed::Calling for Stage {
    type Arguments = ffi::ustore_transaction_stage_t;
    type Output = ReadStamp;

    fn arguments<Parent, Completion>(
        &mut self,
        view: &View<'_, Parent, Completion>,
        outcome: *mut ffi::ustore_outcome_t,
        completion: ffi::ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        Ok(Some(ffi::ustore_transaction_stage_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            transaction: view.transaction(),
            read_stamp: &mut self.stamp.0,
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_transaction_stage(arguments)
    }

    fn finish(self) -> Result<ReadStamp> {
        Ok(self.stamp)
    }
}

/// A commit of a transaction; see [`View::commit`].
#[derive(Debug)]
pub struct Commit {
    durability: Durability,
    stamp: CommitStamp,
}

impl sealed::Calling for Commit {
    type Arguments = ffi::ustore_transaction_commit_t;
    type Output = CommitStamp;

    fn arguments<Parent, Completion>(
        &mut self,
        view: &View<'_, Parent, Completion>,
        outcome: *mut ffi::ustore_outcome_t,
        completion: ffi::ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        Ok(Some(ffi::ustore_transaction_commit_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            transaction: view.transaction(),
            commit_stamp: &mut self.stamp.0,
            durability: self.durability,
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_transaction_commit(arguments)
    }

    fn finish(self) -> Result<CommitStamp> {
        Ok(self.stamp)
    }
}

/// A withdrawal of a stage; see [`View::rollback`].
#[derive(Debug)]
pub struct Rollback;

impl sealed::Calling for Rollback {
    type Arguments = ffi::ustore_transaction_rollback_t;
    type Output = ();

    fn arguments<Parent, Completion>(
        &mut self,
        view: &View<'_, Parent, Completion>,
        outcome: *mut ffi::ustore_outcome_t,
        completion: ffi::ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        Ok(Some(ffi::ustore_transaction_rollback_t {
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
            transaction: view.transaction(),
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_transaction_rollback(arguments)
    }

    fn finish(self) -> Result<()> {
        Ok(())
    }
}

/// A reset of a transaction for another try; see [`View::reset`].
#[derive(Debug)]
pub struct Reset {
    /// The handle the library resets in place, which the arguments point at.
    transaction: Address<ffi::ustore_transaction_handle_t>,
}

impl sealed::Calling for Reset {
    type Arguments = ffi::ustore_transaction_init_t;
    type Output = ();

    fn arguments<Parent, Completion>(
        &mut self,
        view: &View<'_, Parent, Completion>,
        outcome: *mut ffi::ustore_outcome_t,
        completion: ffi::ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        self.transaction.0 = view.transaction();
        Ok(Some(ffi::ustore_transaction_init_t {
            transaction: ptr::from_mut(&mut self.transaction.0).cast(),
            database: view.database(),
            executor: view.executor(),
            outcome,
            completion,
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_transaction_init(arguments)
    }

    fn finish(self) -> Result<()> {
        Ok(())
    }
}

// endregion: Transactions

// region: Snapshots

/// A point-in-time view of the whole database, which pins the versions its reads need until it is
/// dropped.
///
/// Every view at a snapshot borrows it, so the snapshot outlives them, and the snapshot borrows its
/// database, which it is freed before. Dropping it waits for every deferred call naming it to finish.
#[derive(Debug)]
pub struct Snapshot<'db> {
    database: &'db Database,
    handle: ffi::ustore_snapshot_t,
    read_stamp: ReadStamp,
}

impl<'db> Snapshot<'db> {
    /// The view every call at this snapshot addresses, which no write may go through.
    pub fn view(&self) -> View<'_, Snapshot<'db>> {
        View {
            database: self.database.handle(),
            // SAFETY: the snapshot is pinned, and its view tags the handle without allocating.
            view: unsafe { ffi::ustore_snapshot_view(self.handle) },
            transaction: ptr::null_mut(),
            snapshot: self.handle,
            executor: ptr::null_mut(),
            watch: Watch::None,
            marker: PhantomData,
        }
    }

    /// The view of this snapshot with every call through it running on `executor`.
    pub fn on<'with>(&'with self, executor: &'with Executor) -> View<'with, Snapshot<'db>> {
        self.view().on(executor)
    }

    /// The view of this snapshot with every verb deferred on `executor`.
    pub fn deferred<'with>(&'with self, executor: &'with Executor) -> DeferredView<'with, Snapshot<'db>> {
        self.view().deferred(executor)
    }

    /// The stamp this snapshot reads at, at or above the commit stamp of every commit it sees.
    pub fn read_stamp(&self) -> ReadStamp {
        self.read_stamp
    }

    /// Writes this snapshot out as a new database of the same engine; see [`View::save`].
    pub fn save<'progress>(
        &self,
        path: &str,
        progress: Option<&'progress mut ProgressCallback<'progress>>,
    ) -> Result<()> {
        self.view().save(path, progress)
    }
}

impl Drop for Snapshot<'_> {
    fn drop(&mut self) {
        // SAFETY: the handle came from a successful init and is freed once, after every view, which borrows this
        // snapshot, while the library waits for the deferred calls naming it, forgotten ones included.
        unsafe { ffi::ustore_snapshot_free(self.handle) };
    }
}

// SAFETY: a snapshot may move to another thread with its database borrow; it stays `!Sync`, since the C
// interface promises concurrent calls only through a database.
unsafe impl Send for Snapshot<'_> {}

impl<Completion: sealed::Completion> View<'_, Snapshot<'_>, Completion> {
    /// Writes the snapshot this view reads out as a new database of the same engine, which that engine's open then
    /// serves, holding exactly the collections and names the snapshot records, whatever changed since.
    ///
    /// RocksDB and LevelDB create `path`, while the RAM engine writes its image into the existing directory `path`
    /// names. `progress` counts the bytes of the keys and values saved, and a save it cancels removes what it wrote,
    /// as a failed one does.
    ///
    /// Calls `ustore_snapshot_save`.
    pub fn save<'progress, Path: AsRef<str>>(
        &self,
        path: Path,
        progress: Option<&'progress mut ProgressCallback<'progress>>,
    ) -> Completion::Answer<Save<'progress, Path>>
    where
        Completion: sealed::Accepts<Path> + sealed::Accepts<Option<&'progress mut ProgressCallback<'progress>>>,
    {
        let verb = Save {
            path,
            progress: Progress::new(progress),
        };
        Completion::answer(self, verb)
    }
}

/// A save of a snapshot as a database of its own; see [`View::save`].
#[derive(Debug)]
pub struct Save<'progress, Path> {
    path: Path,
    progress: Progress<'progress>,
}

impl<Path: AsRef<str>> sealed::Calling for Save<'_, Path> {
    type Arguments = ffi::ustore_snapshot_save_t;
    type Output = ();

    fn arguments<Parent, Completion>(
        &mut self,
        view: &View<'_, Parent, Completion>,
        outcome: *mut ffi::ustore_outcome_t,
        completion: ffi::ustore_completion_t,
    ) -> Result<Option<Self::Arguments>> {
        let (progress, progress_state) = self.progress.lowered();
        Ok(Some(ffi::ustore_snapshot_save_t {
            snapshot: view.snapshot(),
            executor: view.executor(),
            outcome,
            completion,
            path: self.path.as_ref().as_bytes().into(),
            progress,
            progress_state,
        }))
    }

    unsafe fn call(arguments: *mut Self::Arguments) {
        ffi::ustore_snapshot_save(arguments)
    }

    fn raise(&mut self) {
        self.progress.raise()
    }

    fn finish(self) -> Result<()> {
        Ok(())
    }
}

// endregion: Snapshots

// region: Change Feeds

/// What one committed change did to its key.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug, Hash)]
pub enum ChangeKind {
    /// Wrote the whole value.
    Write = 0,
    /// Removed the key.
    Removal = 1,
    /// Changed part of the value, as a document patch or a page edit does.
    Change = 2,
}

impl ChangeKind {
    fn from_raw(raw: ffi::ustore_enum_t) -> Option<Self> {
        match raw {
            0 => Some(ChangeKind::Write),
            1 => Some(ChangeKind::Removal),
            2 => Some(ChangeKind::Change),
            _ => None,
        }
    }
}

impl fmt::Display for ChangeKind {
    /// Spells the kind as "write", "removal" or "change".
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(spelling(ffi::ustore_change_kind_name, *self as ffi::ustore_enum_t))
    }
}

impl FromStr for ChangeKind {
    type Err = Error;

    /// Reads "write", "removal" or "change" back.
    fn from_str(text: &str) -> Result<Self> {
        read_spelling(
            text,
            ffi::ustore_change_kind_from_name,
            ffi::ustore_change_kind_name,
            ChangeKind::from_raw,
        )
    }
}

impl Database {
    /// Opens a feed of the changes committed to `collection` after `since`, oldest first, reading
    /// nothing until its first pull.
    ///
    /// A feed whose history is already gone is refused with [`Status::WriteConflict`], and an
    /// engine that keeps no stamps, as LevelDB does, refuses it with [`Status::Unsupported`].
    pub fn changes_cursor(&self, collection: CollectionID, since: CommitStamp) -> Result<ChangesCursor<'_>> {
        let mut cursor = ChangesCursor {
            handle: ptr::null_mut(),
            marker: PhantomData,
        };
        let mut outcome = ffi::ustore_outcome_t::default();
        let mut arguments = ffi::ustore_changes_cursor_init_t {
            database: self.handle,
            executor: ptr::null_mut(),
            outcome: &mut outcome,
            completion: ffi::ustore_completion_t::Blocking,
            collection,
            since_stamp: since.0,
            cursor: &mut cursor.handle,
        };
        // SAFETY: every pointer in `arguments` outlives the call, which is final on return.
        unsafe { ffi::ustore_changes_cursor_init(&mut arguments) };
        check(outcome).map(|()| cursor)
    }
}

/// A feed of the changes committed to one collection after a stamp, oldest first, which every pull
/// resumes past the last change the pull before it handed out; see [`Database::changes_cursor`].
///
/// It pins the stamp it reads from, as a snapshot does, and holds its database open until it is
/// dropped. A pull that fails leaves it where it was.
#[derive(Debug)]
pub struct ChangesCursor<'database> {
    handle: ffi::ustore_changes_cursor_t,
    marker: PhantomData<&'database Database>,
}

impl ChangesCursor<'_> {
    /// Pulls up to `wanted` next changes into `arena`, none once the feed is at its newest commit.
    pub fn next<'arena>(&mut self, wanted: usize, arena: &'arena mut Arena) -> Result<Changes<'arena>> {
        self.pull(wanted, arena, ptr::null_mut())
    }

    /// Pulls up to `wanted` next changes with the bytes every one carries into `arena` in one call.
    pub fn next_pairs<'arena>(&mut self, wanted: usize, arena: &'arena mut Arena) -> Result<Changes<'arena>> {
        let mut values = ffi::ustore_strings_view_t::default();
        let mut changes = self.pull(wanted, arena, &mut values)?;
        // SAFETY: the pull succeeded, writing each change's value as a tape into the `'arena` one.
        changes.values = unsafe { Strings::exported(values, changes.keys.len())? };
        Ok(changes)
    }

    /// Pulls a page of changes into `arena`, with their values into `values` unless it is null.
    fn pull<'arena>(
        &mut self,
        wanted: usize,
        arena: &'arena mut Arena,
        values: *mut ffi::ustore_strings_view_t,
    ) -> Result<Changes<'arena>> {
        let mut count: ffi::ustore_size_t = 0;
        let mut keys: *const Key = ptr::null();
        let mut stamps: *const ffi::ustore_stamp_t = ptr::null();
        let mut kinds: *const ffi::ustore_enum_t = ptr::null();
        let mut outcome = ffi::ustore_outcome_t::default();
        let mut arguments = ffi::ustore_changes_cursor_next_t {
            cursor: self.handle,
            executor: ptr::null_mut(),
            outcome: &mut outcome,
            completion: ffi::ustore_completion_t::Blocking,
            wanted: wanted as ffi::ustore_size_t,
            count: &mut count,
            keys: &mut keys,
            stamps: &mut stamps,
            kinds: &mut kinds,
            values,
            arena: arena.handle,
        };
        // SAFETY: every pointer in `arguments` outlives the call, which is final on return.
        unsafe { ffi::ustore_changes_cursor_next(&mut arguments) };
        check(outcome)?;
        let count = count as usize;
        // SAFETY: a successful pull wrote `count` of each output to the arena borrowed for `'arena`
        let raw_kinds = unsafe { exported_slice(kinds, count) };
        if raw_kinds.iter().any(|&kind| ChangeKind::from_raw(kind).is_none()) {
            return Err(fail(Status::Malformed, "the library wrote an unknown change kind"));
        }
        // SAFETY: every kind was just checked to be a `ChangeKind`, which has the layout of the C
        // enum, and a `CommitStamp` is a transparent stamp.
        unsafe {
            Ok(Changes {
                keys: exported_slice(keys, count),
                stamps: exported_slice(stamps.cast::<CommitStamp>(), count),
                kinds: exported_slice(kinds.cast::<ChangeKind>(), count),
                values: Strings::empty(),
            })
        }
    }
}

impl Drop for ChangesCursor<'_> {
    fn drop(&mut self) {
        // SAFETY: a successful init made the handle, or a failed one left null, which free ignores.
        unsafe { ffi::ustore_changes_cursor_free(self.handle) };
    }
}

/// The changes one pull of a [`ChangesCursor`] handed out, oldest first, in the arena it filled.
#[derive(Clone, Copy, Debug)]
pub struct Changes<'arena> {
    keys: &'arena [Key],
    stamps: &'arena [CommitStamp],
    kinds: &'arena [ChangeKind],
    values: Strings<'arena>,
}

impl<'arena> Changes<'arena> {
    /// How many changes the pull handed out.
    pub fn len(&self) -> usize {
        self.keys.len()
    }

    /// Whether the feed reached the newest commit.
    pub fn is_empty(&self) -> bool {
        self.keys.is_empty()
    }

    /// The key of every change.
    pub fn keys(&self) -> &'arena [Key] {
        self.keys
    }

    /// The stamp every change was committed at.
    pub fn stamps(&self) -> &'arena [CommitStamp] {
        self.stamps
    }

    /// What every change did.
    pub fn kinds(&self) -> &'arena [ChangeKind] {
        self.kinds
    }

    /// The bytes the change at `index` carries, empty for a removal, or `None` past the end or when
    /// the pull read no values.
    pub fn value(&self, index: usize) -> Option<&'arena [u8]> {
        self.values.get(index)
    }
}

// endregion: Change Feeds

// region: Tests

#[cfg(test)]
mod tests {
    use super::*;
    use crate::testing::temporary_directory;
    use crate::testing::{block_on, created, key, keys};

    #[test]
    fn change_feeds_hand_out_writes_and_removals_oldest_first() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let blobs = created(&database, Modality::Blobs);
        let mut feed = database.changes_cursor(blobs, CommitStamp::default()).unwrap();
        let entries = [crate::blobs::Entry::new(key(1), b"a")];
        database.head().write(blobs, &entries, Durability::Buffered).unwrap();
        database.head().remove(blobs, [key(1)], Durability::Buffered).unwrap();
        let mut arena = Arena::new().unwrap();
        let changes = feed.next_pairs(8, &mut arena).unwrap();
        assert_eq!(changes.keys(), &keys([1, 1]));
        assert_eq!(changes.kinds(), &[ChangeKind::Write, ChangeKind::Removal]);
        assert_eq!(changes.value(0), Some(b"a".as_slice()));
        assert!(changes.stamps()[0] < changes.stamps()[1]);
        assert!(feed.next(8, &mut arena).unwrap().is_empty());
    }

    #[test]
    fn databases_describe_their_engine_and_what_they_hold() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let description = database.describe().unwrap();
        assert!(description.supports_transactions());
        assert!(description.supports_snapshots());
        assert_eq!((description.engine, description.keys), (Engine::RAM, 0));

        let people = database.collection_create("people", Modality::Blobs).unwrap();
        let places = database.collection_create("places", Modality::Blobs).unwrap();
        let entries = [crate::blobs::Entry::new(key(1), b"Ada")];
        database.head().write(people, &entries, Durability::Buffered).unwrap();
        database.head().write(places, &entries, Durability::Buffered).unwrap();
        assert_eq!(database.describe().unwrap().keys, 2, "every collection counts");
    }

    #[test]
    fn databases_open_at_a_directory_and_keep_what_they_held() {
        let directory = temporary_directory("database_reopens");
        let mut arena = Arena::new().unwrap();

        let database = Database::open_ram(directory.path(), &RAMOptions::default()).unwrap();
        let blobs = created(&database, Modality::Blobs);
        let entries = [crate::blobs::Entry::new(key(7), b"alpha")];
        database.head().write(blobs, &entries, Durability::Flushed).unwrap();
        drop(database);

        let reopened = Database::open_ram(directory.path(), &RAMOptions::default()).unwrap();
        let values = reopened.head().read(blobs, keys([7]), &mut arena).unwrap();
        assert_eq!(values.get(0), Some(b"alpha".as_slice()));
    }

    #[test]
    fn databases_refuse_a_directory_that_is_not_there() {
        let missing = Database::open_ram("/nonexistent/ustore/directory", &RAMOptions::default()).unwrap_err();
        assert_eq!(missing.status, Status::NotFound);
        let empty = Database::open_ram("", &RAMOptions::default()).unwrap_err();
        assert_eq!(empty.status, Status::InvalidArgument);
    }

    #[test]
    fn databases_start_at_either_isolation() {
        for isolation in [Isolation::StrictSerializable, Isolation::Snapshot] {
            let options = RAMOptions {
                isolation,
                ..RAMOptions::default()
            };
            let description = Database::init_ram(&options).unwrap().describe().unwrap();
            assert_eq!(description.isolation, isolation);
        }
    }

    #[test]
    fn databases_open_on_every_engine_or_report_it_missing() {
        let executor = Executor::for_ram(&[], 2, 0).unwrap();
        let ram = temporary_directory("database_ram");
        let rocksdb = temporary_directory("database_rocksdb");
        let leveldb = temporary_directory("database_leveldb");
        let nvme = temporary_directory("database_nvme");
        let nvme_manifest = format!("{}/ustore.ustore.json", nvme.path());
        let opened = [
            (Database::open_ram(ram.path(), &RAMOptions::default()), true),
            (
                Database::open_rocksdb(&RocksDBOptions::at(rocksdb.path())),
                cfg!(feature = "rocksdb"),
            ),
            (
                Database::open_leveldb(&LevelDBOptions::at(leveldb.path())),
                cfg!(feature = "leveldb"),
            ),
            (
                Database::open_nvme(&NVMEOptions::at(&nvme_manifest)),
                cfg!(feature = "nvme"),
            ),
        ];
        let mut arena = Arena::new().unwrap();
        for (database, compiled) in opened {
            if !compiled {
                assert_eq!(database.unwrap_err().status, Status::Unsupported);
                continue;
            }
            let database = database.unwrap();
            let blobs = created(&database, Modality::Blobs);
            let entries = [crate::blobs::Entry::new(key(7), b"alpha")];
            database.head().write(blobs, &entries, Durability::Flushed).unwrap();
            let values = database.head().read(blobs, keys([7]), &mut arena).unwrap();
            assert_eq!(values.get(0), Some(b"alpha".as_slice()));
        }

        // Every engine but NVMe reopens on an executor, keeping what it held.
        let on = Some(&executor);
        let reopened = [
            (
                Database::open_ram(
                    ram.path(),
                    &RAMOptions {
                        executor: on,
                        ..RAMOptions::default()
                    },
                ),
                true,
            ),
            (
                Database::open_rocksdb(&RocksDBOptions {
                    executor: on,
                    ..RocksDBOptions::at(rocksdb.path())
                }),
                cfg!(feature = "rocksdb"),
            ),
            (
                Database::open_leveldb(&LevelDBOptions {
                    executor: on,
                    ..LevelDBOptions::at(leveldb.path())
                }),
                cfg!(feature = "leveldb"),
            ),
        ];
        for (database, compiled) in reopened {
            if !compiled {
                assert_eq!(database.unwrap_err().status, Status::Unsupported);
                continue;
            }
            let database = database.unwrap();
            let (blobs, modality) = database.head().collection_find("blobs", &mut arena).unwrap();
            assert_eq!(modality, Modality::Blobs, "a reopened name keeps its modality");
            let values = database.head().read(blobs, keys([7]), &mut arena).unwrap();
            assert_eq!(values.get(0), Some(b"alpha".as_slice()));
        }
    }

    #[test]
    #[cfg(feature = "rocksdb")]
    fn a_viewed_rocksdb_database_reads_and_refuses_changes() {
        let directory = temporary_directory("database_viewed");
        let mut arena = Arena::new().unwrap();
        let entries = [crate::blobs::Entry::new(key(7), b"alpha")];
        let written = Database::open_rocksdb(&RocksDBOptions::at(directory.path())).unwrap();
        let blobs = created(&written, Modality::Blobs);
        written.head().write(blobs, &entries, Durability::Flushed).unwrap();
        drop(written);

        let viewed = Database::view_rocksdb(&RocksDBOptions::at(directory.path())).unwrap();
        assert_eq!(viewed.describe().unwrap().engine, Engine::RocksDB);
        let values = viewed.head().read(blobs, keys([7]), &mut arena).unwrap();
        assert_eq!(values.get(0), Some(b"alpha".as_slice()));
        let write = viewed.head().write(blobs, &entries, Durability::Buffered).unwrap_err();
        let clear = viewed.clear(DropMode::Pairs).unwrap_err();
        let compact = viewed.compact(None).unwrap_err();
        for refusal in [write, clear, compact] {
            assert_eq!(refusal.status, Status::ReadOnly);
        }
    }

    #[test]
    fn databases_clear_values_pairs_or_collections() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new().unwrap();
        let people = database.collection_create("people", Modality::Blobs).unwrap();
        let entries = [crate::blobs::Entry::new(key(1), b"Ada")];
        database.head().write(people, &entries, Durability::Buffered).unwrap();

        database.clear(DropMode::Values).unwrap();
        let emptied = database.head().read(people, keys([1]), &mut arena).unwrap();
        assert_eq!(emptied.get(0), Some(b"".as_slice()), "the key stays without its value");

        database.clear(DropMode::Pairs).unwrap();
        assert_eq!(
            database.head().read(people, keys([1]), &mut arena).unwrap().get(0),
            None
        );
        assert_eq!(database.head().collection_list(&mut arena).unwrap(), &[people]);

        // A snapshot belongs to its holder, so the collections stay while one is open.
        let snapshot = database.snapshot().unwrap();
        let refused = database.clear(DropMode::Collection).unwrap_err();
        assert_eq!(refused.status, Status::InvalidArgument);
        drop(snapshot);
        let executor = Executor::for_ram(&[], 1, 0).unwrap();
        database
            .head()
            .deferred(&executor)
            .clear(DropMode::Collection)
            .unwrap()
            .wait()
            .unwrap();
        assert!(database.head().collection_list(&mut arena).unwrap().is_empty());
    }

    #[test]
    fn progress_reports_cancels_and_resurfaces_a_panic() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let entries = [crate::blobs::Entry::new(key(1), b"Ada")];
        for name in ["people", "places"] {
            let collection = database.collection_create(name, Modality::Blobs).unwrap();
            database
                .head()
                .write(collection, &entries, Durability::Buffered)
                .unwrap();
        }
        let mut reports = Vec::new();
        let mut counting = |finished: usize, total: usize| {
            reports.push((finished, total));
            ControlFlow::Continue(())
        };
        database.compact(Some(&mut counting)).unwrap();
        assert!(!reports.is_empty() && reports.windows(2).all(|pair| pair[0].0 <= pair[1].0));

        let mut stopping = |_: usize, _: usize| -> ControlFlow<()> { ControlFlow::Break(()) };
        let cancelled = database.compact(Some(&mut stopping)).unwrap_err();
        assert_eq!(cancelled.status, Status::Cancelled);

        let snapshot = database.snapshot().unwrap();
        let directory = temporary_directory("progress_panics");
        let mut panicking = |_: usize, _: usize| -> ControlFlow<()> { panic!("a progress fault") };
        let resurfaced = std::panic::catch_unwind(AssertUnwindSafe(|| {
            snapshot.save(directory.path(), Some(&mut panicking))
        }));
        assert!(resurfaced.is_err(), "the panic resurfaces once the call is final");

        // A deferred call owns its handler, which outlives the `Deferred`, and resurfaces the panic from `wait`.
        let executor = Executor::for_ram(&[], 1, 0).unwrap();
        let leaked: &'static mut ProgressCallback<'static> =
            Box::leak(Box::new(|_: usize, _: usize| -> ControlFlow<()> {
                panic!("a deferred progress fault")
            }));
        let compaction = database.head().deferred(&executor).compact(Some(leaked)).unwrap();
        assert!(std::panic::catch_unwind(AssertUnwindSafe(|| compaction.wait())).is_err());
    }

    #[test]
    fn executors_take_memory_domains_or_refuse_them() {
        let unknown = [1 << 20];
        assert_eq!(
            Executor::for_ram(&unknown, 1, 0).unwrap_err().status,
            Status::InvalidArgument
        );
        // SAFETY: a null pool is refused before anything reads it.
        let lent = unsafe { Executor::on_pool(&[], ptr::null_mut(), 0) };
        assert_eq!(lent.unwrap_err().status, Status::InvalidArgument);
        let blocking = IOOptions {
            transport: Transport::Blocking,
            ..IOOptions::default()
        };
        drop(Executor::for_nvme(1, &blocking, 0).unwrap());
        // A build without CUDA refuses every device, and one with CUDA refuses an ordinal no device carries.
        // SAFETY: a null stream lends nothing, so the executor creates its own.
        let devices = unsafe { Executor::for_vram(1 << 20, ptr::null_mut(), 0, Gds::Automatic, 0) }
            .unwrap_err()
            .status;
        assert!(devices == Status::Unsupported || devices == Status::Device);

        // Zero threads derive their count from the cores of every domain.
        let derived = Executor::for_ram(&[], 0, 0).unwrap();
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let blobs = created(&database, Modality::Blobs);
        let entries = [crate::blobs::Entry::new(key(1), b"Ada")];
        database
            .head()
            .on(&derived)
            .write(blobs, &entries, Durability::Buffered)
            .unwrap();
    }

    #[test]
    fn enums_spell_and_parse_as_the_library_names_them() {
        use crate::docs::Modification;
        use crate::graph::Role;

        assert_eq!(Isolation::StrictSerializable.to_string(), "strict-serializable");
        assert_eq!(Transport::LinuxUring.to_string(), "linux-uring");
        assert_eq!("rocksdb".parse::<Engine>(), Ok(Engine::RocksDB));
        assert_eq!("pairs".parse::<DropMode>(), Ok(DropMode::Pairs));
        assert_eq!("graph".parse::<Modality>(), Ok(Modality::Graph));
        assert_eq!("spinning".parse::<Mood>(), Ok(Mood::Spinning));
        assert_eq!("flushed".parse::<Durability>(), Ok(Durability::Flushed));
        assert_eq!("none".parse::<Watch>(), Ok(Watch::None));
        assert_eq!("target".parse::<Role>(), Ok(Role::Target));
        assert_eq!("merge".parse::<Modification>(), Ok(Modification::Merge));
        assert_eq!(Engine::NVME.to_string().parse::<Engine>(), Ok(Engine::NVME));

        // Every inverse answers its zero for a name it does not know, which the spelling check refuses.
        for unknown in ["levitating", "unrecognized", "Pairs", ""] {
            assert_eq!(
                unknown.parse::<DropMode>().unwrap_err().status,
                Status::InvalidArgument,
                "{unknown}"
            );
        }
        assert!(
            "unknown".parse::<Role>().is_err(),
            "a role the binding never passes is refused"
        );
    }

    #[test]
    fn collections_are_created_found_listed_and_dropped() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new().unwrap();
        assert!(
            database.head().collection_list(&mut arena).unwrap().is_empty(),
            "a database holds no collection until one is created"
        );

        let people = database.collection_create("people", Modality::Blobs).unwrap();
        let places = database.collection_create("places", Modality::Graph).unwrap();
        assert_ne!(people, places);
        let head = database.head();
        assert_eq!(
            head.collection_find("people", &mut arena).unwrap(),
            (people, Modality::Blobs)
        );
        assert_eq!(
            head.collection_find("places", &mut arena).unwrap(),
            (places, Modality::Graph),
            "a name resolves to the modality it was created with"
        );

        let listed = head.collection_list(&mut arena).unwrap();
        assert_eq!(listed.len(), 2, "the system collections are never listed");
        assert!(listed.contains(&people) && listed.contains(&places));

        // A name is bound once, and a name nobody bound addresses nothing.
        assert_eq!(
            database
                .collection_create("people", Modality::Blobs)
                .unwrap_err()
                .status,
            Status::AlreadyExists
        );
        assert_eq!(
            head.collection_find("nowhere", &mut arena).unwrap_err().status,
            Status::NotFound
        );

        database.collection_drop(people, DropMode::Collection).unwrap();
        assert_eq!(
            head.collection_find("people", &mut arena).unwrap_err().status,
            Status::NotFound
        );
        assert_eq!(head.collection_list(&mut arena).unwrap().len(), 1);
    }

    #[test]
    fn collections_clear_their_pairs_or_drop_whole() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let mut arena = Arena::new().unwrap();
        let people = database.collection_create("people", Modality::Blobs).unwrap();
        let entries = [crate::blobs::Entry::new(key(1), b"alpha")];
        database.head().write(people, &entries, Durability::Buffered).unwrap();

        database.collection_drop(people, DropMode::Pairs).unwrap();
        let values = database.head().read(people, &keys([1]), &mut arena).unwrap();
        assert_eq!(values.get(0), None);
        assert_eq!(
            database.head().collection_list(&mut arena).unwrap(),
            &[people],
            "clearing the pairs keeps the collection"
        );

        // A deferred view of the head drops the collection whole.
        let executor = Executor::for_ram(&[], 1, 0).unwrap();
        database.head().write(people, &entries, Durability::Buffered).unwrap();
        database
            .head()
            .deferred(&executor)
            .collection_drop(people, DropMode::Collection)
            .unwrap()
            .wait()
            .unwrap();
        assert_eq!(database.head().collection_list(&mut arena).unwrap().len(), 0);
    }

    #[test]
    fn transactions_commit_what_they_wrote() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let blobs = created(&database, Modality::Blobs);
        let mut arena = Arena::new().unwrap();
        let mut transaction = database.transaction().unwrap();
        let entries = [crate::blobs::Entry::new(key(1), b"alpha")];
        transaction
            .view(Watch::Keys)
            .write(blobs, &entries, Durability::Buffered)
            .unwrap();

        // Nothing is visible at the head until the commit lands.
        let before = database.head().read(blobs, keys([1]), &mut arena).unwrap();
        assert_eq!(before.get(0), None);

        let staged = transaction.stage().unwrap();
        let committed = transaction.commit(Durability::Buffered).unwrap();
        assert!(committed.0 >= staged.0);

        let after = database.head().read(blobs, keys([1]), &mut arena).unwrap();
        assert_eq!(after.get(0), Some(b"alpha".as_slice()));
    }

    #[test]
    fn transactions_reset_and_run_again() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let blobs = created(&database, Modality::Blobs);
        let mut arena = Arena::new().unwrap();
        let mut transaction = database.transaction().unwrap();
        let abandoned = [crate::blobs::Entry::new(key(2), b"abandoned")];
        let other = [crate::blobs::Entry::new(key(3), b"abandoned")];
        transaction
            .view(Watch::Keys)
            .write(blobs, &abandoned, Durability::Buffered)
            .unwrap();
        transaction
            .view(Watch::Keys)
            .write(blobs, &other, Durability::Buffered)
            .unwrap();
        transaction.reset().unwrap();

        let kept = [crate::blobs::Entry::new(key(2), b"kept")];
        transaction
            .view(Watch::Keys)
            .write(blobs, &kept, Durability::Buffered)
            .unwrap();
        transaction.commit(Durability::Buffered).unwrap();

        let values = database.head().read(blobs, keys([2, 3]), &mut arena).unwrap();
        assert_eq!(values.get(0), Some(b"kept".as_slice()));
        assert_eq!(values.get(1), None, "a reset forgets every write before it");
    }

    #[test]
    fn transactions_roll_back_a_stage_and_keep_their_writes() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let blobs = created(&database, Modality::Blobs);
        let mut arena = Arena::new().unwrap();
        let mut transaction = database.transaction().unwrap();
        let kept = [crate::blobs::Entry::new(key(2), b"kept")];
        transaction
            .view(Watch::Keys)
            .write(blobs, &kept, Durability::Buffered)
            .unwrap();
        transaction.stage().unwrap();
        transaction.rollback().unwrap();
        transaction.commit(Durability::Buffered).unwrap();

        let values = database.head().read(blobs, keys([2]), &mut arena).unwrap();
        assert_eq!(values.get(0), Some(b"kept".as_slice()));
    }

    #[test]
    fn transactions_refuse_a_flush_of_their_own() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let blobs = created(&database, Modality::Blobs);
        let transaction = database.transaction().unwrap();
        let entries = [crate::blobs::Entry::new(key(3), b"alpha")];
        let failure = transaction
            .view(Watch::Keys)
            .write(blobs, &entries, Durability::Flushed)
            .unwrap_err();
        assert_eq!(failure.status, Status::InvalidArgument);
    }

    #[test]
    fn transactions_conflict_rather_than_block() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let blobs = created(&database, Modality::Blobs);
        let mut first = database.transaction().unwrap();
        let mut second = database.transaction().unwrap();

        let mine = [crate::blobs::Entry::new(key(4), b"mine")];
        let yours = [crate::blobs::Entry::new(key(4), b"yours")];
        first
            .view(Watch::Keys)
            .write(blobs, &mine, Durability::Buffered)
            .unwrap();
        second
            .view(Watch::Keys)
            .write(blobs, &yours, Durability::Buffered)
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
        let database = Database::init_ram(&RAMOptions {
            isolation: Isolation::Snapshot,
            ..RAMOptions::default()
        })
        .unwrap();
        let blobs = created(&database, Modality::Blobs);
        let mut arena = Arena::new().unwrap();
        let seed = [crate::blobs::Entry::new(key(8), b"seed")];
        database.head().write(blobs, &seed, Durability::Buffered).unwrap();

        // A watched read is part of the serial order, so a writer that lands under it conflicts.
        let mut watched = database.transaction().unwrap();
        let read = watched.view(Watch::Keys).read(blobs, keys([8]), &mut arena).unwrap();
        assert_eq!(read.get(0), Some(b"seed".as_slice()));
        let overwritten = [crate::blobs::Entry::new(key(8), b"overwritten")];
        database
            .head()
            .write(blobs, &overwritten, Durability::Buffered)
            .unwrap();
        let failure = watched.commit(Durability::Buffered).unwrap_err();
        assert!(
            matches!(failure.status, Status::ReadConflict | Status::WriteConflict),
            "{failure:?}"
        );

        // The same run through an unwatched view records nothing, so the commit stands.
        let mut unwatched = database.transaction().unwrap();
        let read = unwatched.view(Watch::None).read(blobs, keys([8]), &mut arena).unwrap();
        assert_eq!(read.get(0), Some(b"overwritten".as_slice()));
        let again = [crate::blobs::Entry::new(key(8), b"again")];
        database.head().write(blobs, &again, Durability::Buffered).unwrap();
        unwatched.commit(Durability::Buffered).unwrap();
    }

    #[test]
    fn snapshots_hold_the_state_they_were_taken_at() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let blobs = created(&database, Modality::Blobs);
        let executor = Executor::for_ram(&[], 1, 0).unwrap();
        let mut arena = Arena::new().unwrap();
        let before = [crate::blobs::Entry::new(key(5), b"before")];
        database.head().write(blobs, &before, Durability::Buffered).unwrap();

        let snapshot = database.snapshot().unwrap();
        let after = [crate::blobs::Entry::new(key(5), b"after")];
        database.head().write(blobs, &after, Durability::Buffered).unwrap();

        let pinned = snapshot.view().read(blobs, keys([5]), &mut arena).unwrap();
        assert_eq!(pinned.get(0), Some(b"before".as_slice()));
        let pinned = snapshot.on(&executor).read(blobs, keys([5]), &mut arena).unwrap();
        assert_eq!(
            pinned.get(0),
            Some(b"before".as_slice()),
            "a snapshot reads the same on an executor"
        );
        let latest = database.head().read(blobs, keys([5]), &mut arena).unwrap();
        assert_eq!(latest.get(0), Some(b"after".as_slice()));
    }

    #[test]
    fn snapshots_save_a_database_of_their_own() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let blobs = created(&database, Modality::Blobs);
        let mut arena = Arena::new().unwrap();
        let entries = [crate::blobs::Entry::new(key(6), b"saved")];
        database.head().write(blobs, &entries, Durability::Buffered).unwrap();

        let directory = temporary_directory("snapshot_saves");
        let deferred_directory = temporary_directory("snapshot_saves_deferred");
        let executor = Executor::for_ram(&[], 1, 0).unwrap();
        let snapshot = database.snapshot().unwrap();
        snapshot.save(directory.path(), None).unwrap();
        snapshot
            .deferred(&executor)
            .save(deferred_directory.path().to_owned(), None)
            .unwrap()
            .wait()
            .unwrap();
        assert_eq!(
            snapshot.save("/nonexistent/ustore/directory", None).unwrap_err().status,
            Status::NotFound
        );

        for path in [directory.path(), deferred_directory.path()] {
            let saved = Database::open_ram(path, &RAMOptions::default()).unwrap();
            let values = saved.head().read(blobs, &keys([6]), &mut arena).unwrap();
            assert_eq!(values.get(0), Some(b"saved".as_slice()));
        }
    }

    /// A database whose blobs collection holds `count` keys from zero, so a call over all of them is still deferred
    /// when the next call is submitted.
    fn crowded(count: u64) -> (Database, CollectionID) {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let blobs = created(&database, Modality::Blobs);
        let keys: Vec<Key> = (0..count).map(Key::from).collect();
        let values: stringtape::BytesTape<u64> = keys.iter().map(|_| b"value".as_slice()).collect();
        database
            .head()
            .write(blobs, (keys, values), Durability::Buffered)
            .unwrap();
        (database, blobs)
    }

    #[test]
    fn transaction_verbs_block_or_defer_on_an_executor() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let blobs = created(&database, Modality::Blobs);
        let executor = Executor::for_ram(&[], 2, 0).unwrap();
        let mut arena = Arena::new().unwrap();

        // A blocking view on the executor answers the stamps directly.
        let transaction = database.transaction().unwrap();
        let blocked = [crate::blobs::Entry::new(key(1), b"blocked")];
        let on_executor = transaction.on(&executor);
        on_executor.write(blobs, &blocked, Durability::Buffered).unwrap();
        let staged = on_executor.stage().unwrap();
        let first = on_executor.commit(Durability::Buffered).unwrap();
        assert!(staged.0 <= first.0);

        // A deferred view answers a `Deferred` per verb, which hands the same back.
        let transaction = database.transaction().unwrap();
        let kept = [crate::blobs::Entry::new(key(2), b"kept")];
        transaction
            .view(Watch::Keys)
            .write(blobs, &kept, Durability::Buffered)
            .unwrap();
        let deferred = transaction.deferred(&executor);
        let staged = deferred.stage().unwrap().wait().unwrap();
        deferred.rollback().unwrap().wait().unwrap();
        let second = deferred.commit(Durability::Buffered).unwrap().wait().unwrap();
        assert!(first < second && staged.0 <= second.0);

        let transaction = database.transaction().unwrap();
        let abandoned = [crate::blobs::Entry::new(key(3), b"abandoned")];
        transaction
            .view(Watch::Keys)
            .write(blobs, &abandoned, Durability::Buffered)
            .unwrap();
        transaction.deferred(&executor).reset().unwrap().wait().unwrap();
        transaction
            .deferred(&executor)
            .commit(Durability::Buffered)
            .unwrap()
            .wait()
            .unwrap();

        let values = database.head().read(blobs, &keys([1, 2, 3]), &mut arena).unwrap();
        let read: Vec<Option<&[u8]>> = values.iter().collect();
        assert_eq!(read, [Some(b"blocked".as_slice()), Some(b"kept"), None]);
    }

    #[test]
    fn collections_list_through_a_deferred_view() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let executor = Executor::for_ram(&[], 1, 0).unwrap();
        let mut arena = Arena::new_in(&executor).unwrap();
        let people = database.collection_create("people", Modality::Blobs).unwrap();
        let snapshot = database.snapshot().unwrap();
        let places = database.collection_create("places", Modality::Graph).unwrap();

        let listing = database.head().deferred(&executor).collection_list(&mut arena).unwrap();
        let mut found = listing.wait().unwrap().to_vec();
        found.sort_unstable();
        let mut expected = [people, places];
        expected.sort_unstable();
        assert_eq!(found, expected);

        let pinned = snapshot
            .deferred(&executor)
            .collection_list(&mut arena)
            .unwrap()
            .wait()
            .unwrap();
        assert_eq!(pinned, &[people], "a snapshot lists what it was taken at");
    }

    /// One arena serves every verb in turn, whether an executor made it or it serves the calling thread.
    #[test]
    fn one_arena_serves_every_verb_in_turn() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let executor = Executor::for_ram(&[], 2, 0).unwrap();
        let people = database.collection_create("people", Modality::Docs).unwrap();
        let names = created(&database, Modality::Blobs);
        let entries = [crate::blobs::Entry::new(key(1), b"Ada")];
        database.head().write(names, &entries, Durability::Buffered).unwrap();
        let documents = [crate::docs::DocumentEntry::<crate::docs::Json>::new(
            key(1),
            r#"{"name":"Ada"}"#,
        )];
        let upsert = crate::docs::Modification::Upsert;
        database
            .head()
            .docs_write(people, &documents, upsert, Durability::Buffered)
            .unwrap();

        for mut arena in [Arena::new().unwrap(), Arena::new_in(&executor).unwrap()] {
            let head = database.head().on(&executor);
            assert_eq!(head.read(names, keys([1, 2]), &mut arena).unwrap().get(1), None);
            assert_eq!(
                head.scan(names, key(0), 10, Matching::default(), &mut arena)
                    .unwrap()
                    .keys,
                &keys([1])
            );
            assert_eq!(head.docs_gist(people, keys([1]), &mut arena).unwrap().len(), 1);
            let listed = head.collection_list(&mut arena).unwrap();
            assert!(listed.len() == 2 && listed.contains(&people) && listed.contains(&names));
            assert_eq!(
                head.collection_find("people", &mut arena).unwrap(),
                (people, Modality::Docs)
            );
        }
    }

    #[test]
    fn a_call_without_an_arena_is_refused() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let mut outcome = ffi::ustore_outcome_t::default();
        let mut collections: *const CollectionID = ptr::null();
        let mut written: ffi::ustore_size_t = 42;
        let mut arguments = ffi::ustore_collection_list_t {
            database: database.handle,
            executor: ptr::null_mut(),
            outcome: &mut outcome,
            completion: ffi::ustore_completion_t::Blocking,
            snapshot: ptr::null_mut(),
            collections: &mut collections,
            modalities: ptr::null_mut(),
            names: ptr::null_mut(),
            written: &mut written,
            arena: ptr::null_mut(),
        };
        // SAFETY: every pointer in `arguments` outlives the call, which is final on return.
        unsafe { ffi::ustore_collection_list(&mut arguments) };
        assert_eq!(check(outcome).unwrap_err().status, Status::InvalidArgument);
        assert!(
            collections.is_null() && written == 0,
            "a refused call leaves its outputs null"
        );
    }

    #[test]
    fn deferred_calls_resolve_through_await() {
        let (database, blobs) = crowded(1_000);
        let executor = Executor::for_ram(&[], 1, 0).unwrap();
        let mut arena = Arena::new_in(&executor).unwrap();
        let deferred = database.head().deferred(&executor);

        let values = block_on(async {
            let fresh: stringtape::BytesTape<u64> = [b"fresh".as_slice()].into_iter().collect();
            deferred.write(blobs, (keys([7]), fresh), Durability::Buffered)?.await?;
            deferred.read(blobs, keys([7, 8, 9_999]), &mut arena)?.await
        })
        .unwrap();
        let read: Vec<Option<&[u8]>> = values.iter().collect();
        assert_eq!(read, [Some(b"fresh".as_slice()), Some(b"value"), None]);

        // A failure reaches the awaiting task as it reaches `wait`.
        let unknown = CollectionID::from(4242u64);
        let missing = block_on(async { deferred.read(unknown, keys([1]), &mut arena)?.await });
        assert_eq!(missing.unwrap_err().status, Status::NotFound);
    }

    #[test]
    fn stamps_order_reads_before_commits_before_later_snapshots() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let blobs = created(&database, Modality::Blobs);
        let mut arena = Arena::new().unwrap();
        let earlier = database.snapshot().unwrap();

        let mut transaction = database.transaction().unwrap();
        let entries = [crate::blobs::Entry::new(key(1), b"alpha")];
        transaction
            .view(Watch::Keys)
            .write(blobs, &entries, Durability::Buffered)
            .unwrap();
        let read = transaction.stage().unwrap();
        let commit = transaction.commit(Durability::Buffered).unwrap();
        let later = database.snapshot().unwrap();

        assert!(earlier.read_stamp() <= read);
        assert!(read.0 <= commit.0, "a stage reads below the commit it precedes");
        assert!(
            commit.0 <= later.read_stamp().0,
            "a later snapshot reads at or above the commit"
        );
        let seen = later.view().read(blobs, &keys([1]), &mut arena).unwrap();
        assert_eq!(seen.get(0), Some(b"alpha".as_slice()));
        let unseen = earlier.view().read(blobs, &keys([1]), &mut arena).unwrap();
        assert_eq!(unseen.get(0), None);
    }

    #[test]
    fn a_forgotten_deferred_call_holds_its_database_open() {
        let executor = Executor::for_ram(&[], 1, 0).unwrap();
        let mut arena = Arena::new_in(&executor).unwrap();
        let (database, blobs) = crowded(100_000);
        let scan = database
            .head()
            .deferred(&executor)
            .scan(blobs, key(0), 100_000, Matching::default(), &mut arena)
            .unwrap();
        let lent = scan.lent;
        std::mem::forget(scan);

        // The free waits in the library for the call naming the database, which the forgotten box still holds.
        drop(database);
        let scan = Deferred { lent };
        assert_eq!(scan.status(), Ok(()));
        assert_eq!(scan.wait().unwrap().keys.len(), 100_000);
    }

    #[test]
    fn a_dropped_deferred_call_waits_until_it_is_final() {
        let executor = Executor::for_ram(&[], 1, 0).unwrap();
        let mut arena = Arena::new_in(&executor).unwrap();
        let (database, blobs) = crowded(100_000);
        let scan = database
            .head()
            .deferred(&executor)
            .scan(blobs, key(0), 100_000, Matching::default(), &mut arena)
            .unwrap();
        drop(scan);

        // The drop waited for the scan to release the arena, so the next call through it is not contended.
        assert_eq!(
            database
                .head()
                .scan(blobs, key(0), 1, Matching::default(), &mut arena)
                .unwrap()
                .keys,
            &keys([0])
        );
    }

    #[test]
    fn a_full_deferred_table_answers_contended() {
        let (database, blobs) = crowded(100_000);
        let executor = Executor::for_ram(&[], 1, 1).unwrap();
        let mut scanned = Arena::new_in(&executor).unwrap();
        let mut measured = Arena::new_in(&executor).unwrap();
        let head = database.head().deferred(&executor);

        let scan = head
            .scan(blobs, key(0), 100_000, Matching::default(), &mut scanned)
            .unwrap();
        let refused = head.measure(blobs, key(0)..key(9), &mut measured).unwrap();
        assert_eq!(
            refused.status(),
            Err(Status::Contended),
            "a refused call is final at once"
        );
        assert_eq!(refused.wait().unwrap_err().status, Status::Contended);
        assert_eq!(scan.wait().unwrap().keys.len(), 100_000);
        head.measure(blobs, key(0)..key(9), &mut measured)
            .unwrap()
            .wait()
            .unwrap();
    }

    #[test]
    fn a_transaction_serving_a_deferred_call_answers_contended() {
        let (database, blobs) = crowded(100_000);
        let executor = Executor::for_ram(&[], 1, 0).unwrap();
        let transaction = database.transaction().unwrap();
        let mut arena = Arena::new().unwrap();
        let mut deferred_arena = Arena::new_in(&executor).unwrap();

        let every: Vec<Key> = (0..100_000u64).map(Key::from).collect();
        let read = transaction
            .deferred(&executor)
            .read(blobs, every, &mut deferred_arena)
            .unwrap();
        let refused = transaction
            .view(Watch::Keys)
            .read(blobs, &keys([1]), &mut arena)
            .unwrap_err();
        assert_eq!(refused.status, Status::Contended);

        assert_eq!(read.wait().unwrap().len(), 100_000);
        let after = transaction
            .view(Watch::Keys)
            .read(blobs, &keys([1]), &mut arena)
            .unwrap();
        assert_eq!(after.get(0), Some(b"value".as_slice()));
    }

    #[test]
    fn a_drain_returns_once_every_earlier_call_is_final() {
        let database = Database::init_ram(&RAMOptions::default()).unwrap();
        let blobs = created(&database, Modality::Blobs);
        let executor = Executor::for_ram(&[], 2, 0).unwrap();
        let head = database.head().deferred(&executor);
        let writes: Vec<_> = (0..4)
            .map(|index| {
                let value: stringtape::BytesTape<u64> = [b"value".as_slice()].into_iter().collect();
                head.write(blobs, ([key(index)], value), Durability::Buffered).unwrap()
            })
            .collect();

        executor.drain(Mood::Spinning).unwrap();
        for write in &writes {
            assert_eq!(write.status(), Ok(()));
        }
        executor.drain(Mood::Parked).unwrap();
        for write in writes {
            write.wait().unwrap();
        }
    }

    #[test]
    fn statuses_map_spell_and_tolerate_newer_libraries() {
        let written = ffi::ustore_outcome_t {
            status: ffi::ustore_status_t(Status::NotFound as ffi::ustore_enum_t),
            message: c"No such collection".as_ptr(),
        };
        let failure = check(written).unwrap_err();
        assert_eq!(failure.status, Status::NotFound);
        assert_eq!(failure.message, "No such collection");
        assert_eq!(failure.to_string(), "No such collection");
        assert_eq!(failure.status.to_string(), "not-found");

        let contended = ffi::ustore_outcome_t {
            status: ffi::ustore_status_t(Status::Contended as ffi::ustore_enum_t),
            message: c"The arena serves another call".as_ptr(),
        };
        assert_eq!(check(contended).unwrap_err().status, Status::Contended);
        assert_eq!("cancelled".parse::<Status>(), Ok(Status::Cancelled));
        assert_eq!("read-only".parse::<Status>(), Ok(Status::ReadOnly));
        assert_eq!("deferred".parse::<Status>(), Ok(Status::Deferred));

        // Success is left to `Result`, so it parses as no failure, and a status a newer library added reads as an
        // internal one.
        for unspelled in ["success", "levitating", "Not-Found"] {
            let refusal = unspelled.parse::<Status>().unwrap_err();
            assert_eq!(refusal.status, Status::InvalidArgument, "{unspelled}");
        }
        let newer = ffi::ustore_outcome_t {
            status: ffi::ustore_status_t(4242),
            message: ptr::null(),
        };
        assert_eq!(check(newer).unwrap_err().status, Status::Internal);
        assert!(check(ffi::ustore_outcome_t::default()).is_ok());
        assert_eq!(Status::from_raw(ffi::ustore_status_t::DEFERRED), Err(Status::Deferred));
    }
}

// endregion: Tests
