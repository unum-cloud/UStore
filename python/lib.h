/**
 *  @file python/lib.h
 *  @author Ash Vardanian
 *  @date September 16, 2026
 *  @brief Declarations shared by every UStore CPython translation unit.
 *
 *  One extension, one file per header of the C ABI: @c types.c owns the vocabulary every modality
 *  shares, with executors and deferred calls, @c db.c owns databases, collections, snapshots,
 *  transactions and views, @c blobs.c owns the binary verbs and cursors, @c docs.c the document verbs, @c graph.c
 *  the graph verbs, @c strings.c resolves names onto identifiers, @c collection.c answers the
 *  collections a database is indexed by, and @c lib.c owns the module glue alone. A verb is written
 *  once and reached from every method table, since receivers differ only in what their calls
 *  address and how they run, and its core is reached from the collection views too, which skip the
 *  keywords.
 *
 *  @section python_receivers Receivers and Deferred Calls
 *
 *  A receiver carries what every argument struct puts before its inputs: the view, the executor and
 *  the completion. A @c Database, @c Transaction and @c Snapshot run their calls on the calling
 *  thread, a @c View runs them blocking on an executor, and a @c DeferredView submits them and
 *  answers a @c Deferred at once. Constructors address no database yet, so they take an `executor`
 *  keyword and block on it, as USearch's do. Receivers never change after they are made, so one
 *  is shared across threads freely. A verb gathers its call, fills the C argument struct on its own stack and
 *  submits it, which copies the struct, so the call keeps only the outcome, the inputs it owns, the
 *  output slots and its arena: on the stack when it blocks, and inside its @c Deferred otherwise,
 *  which gathers the keys and the borrowed buffers in place, because a batch's inline storage points
 *  into itself.
 *
 *  The binding starts no threads, keeps no queues and counts no deferred calls: the C executor owns
 *  every thread, freeing a database, transaction, snapshot or arena waits in C for the deferred calls
 *  naming it, `status` polls through @c ustore_outcome_status, and `await` runs `wait()` on the event
 *  loop's own executor.
 *
 *  @section python_enums Statuses and Modes
 *
 *  Every C enum a caller meets is an `enum.IntEnum` of `ustore`, valued as its enumerators and named
 *  by their canonical spellings in upper case, such as `ustore.Status.WRITE_CONFLICT` for
 *  "write-conflict". The module reads the enumerators through the C `_name` functions and exports
 *  them as `_enumerators`, `ustore/__init__.py` builds the classes from them once and keeps them as
 *  attributes of this module, and the verbs read any `int` the library spells and answer the members
 *  they look up there.
 *
 *  @section python_progress Progress
 *
 *  A verb taking `progress=` registers one trampoline, whose state is the header of its call. The
 *  library calls it from whichever thread runs the call, never two at once, so it attaches the
 *  interpreter itself, and a callable that raises cancels the call and keeps its exception in the
 *  header, which the answer raises in place of the cancellation.
 *
 *  @section python_arenas Arenas
 *
 *  Every output whose length scales with the call lands in the @c Arena the caller passes, and the
 *  verb answers results that view it without copying. An arena tracks its @c exports, since a buffer
 *  it exported cannot be revoked before C rewinds the memory under it: a call adds one until it is
 *  waited on or dropped, and an arena exporting a buffer refuses the next call with
 *  @c ContendedError, as a @c bytearray refuses to resize. Each claim advances the arena's
 *  @c generation, and a result reads only while the generation it was written at is current,
 *  raising @c UninitializedError once a later call rewound it. Shorthand with no place for an arena,
 *  such as `get`, `contains` and the mapping views of a collection, makes a one-off arena through
 *  @c ustore_arena_init, copies what it read into Python objects and frees the arena with the call.
 *
 *  @section python_strings Ragged Inputs and Outputs
 *
 *  Every ragged input reaches the ABI as one @c ustore_strings_view_t. An Arrow array of binary or
 *  text, or a `(data, offsets)` pair of buffers, passes as the u32 or u64 tape it already is. Every
 *  other batch is borrowed value by value into an array of @c ustore_bytes_view_t inside the call,
 *  which an accessor reads on the executor's thread: it reads only those views, pinned when the
 *  call was gathered, never a Python container the caller may change meanwhile. Ragged outputs are
 *  tapes of either offset width, which results read through @c ustore_py_tape_string.
 *
 *  @section python_calling_convention Latency and Arguments Parsing
 *
 *  No PyBind11, NanoBind or SWIG, only the CPython API, and never @c PyArg_ParseTupleAndKeywords,
 *  whose format specifiers are parsed at runtime and which cannot use the fast calling convention.
 *  Every verb is @c METH_FASTCALL | @c METH_KEYWORDS and walks its keywords by hand.
 *
 *  @section python_buffers Buffer Protocol and Batches
 *
 *  Keys and values arrive through PEP 3118 where the layout allows it, so a NumPy array of 16-byte
 *  keys reaches the ABI without a copy and NumPy is never a build-time dependency. A batch small
 *  enough lays its strided arrays out on the stack, so the hot path allocates nothing beyond the
 *  objects CPython itself demands.
 *
 *  @section python_identifiers Keys and Collection IDs
 *
 *  A key and a collection ID are 16-byte UUIDs, which Python meets as `uuid.UUID`. A key is read
 *  from a `uuid.UUID`, 16 raw bytes, canonical UUID text, or a non-negative integer below 2**128,
 *  which spells as its 128-bit big-endian bytes the way `uuid.UUID(int=n)` does, so integer keys
 *  order as the integers. Every key and collection ID a verb answers is a `uuid.UUID`.
 */
#ifndef UNUM_USTORE_PYTHON_LIB_H
#define UNUM_USTORE_PYTHON_LIB_H

/*  CPython defines preprocessor macros that reach into the standard headers, so it leads rather
 *  than joining the group below, and it carries `<string.h>` for the byte moves every unit makes. */
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <stdarg.h> // `va_list`
#include <stddef.h> // `size_t`
#include <stdint.h> // `int64_t`

#include <yyjson.h> // `yyjson_val`, `yyjson_mut_doc`

#include "ustore/ustore.h"

#ifdef __cplusplus
extern "C" {
#endif

#pragma region Calling Convention

/** Compile-time constants of the binding layer. */
enum {

    /** Fast-call plus keywords, the calling convention every verb uses. */
    ustore_py_method_flags_k = METH_FASTCALL | METH_KEYWORDS,

    /** Keys one batch lays out on the stack before it reaches for the heap. */
    ustore_py_keys_inline_k = 64,

    /** Values one batch borrows on the stack before it reaches for the heap. */
    ustore_py_contents_inline_k = 16,
};

/** Element count of a statically-sized table, which every naming table asks for. */
#define ustore_py_table_size_(table) (sizeof(table) / sizeof((table)[0]))

/**
 *  @brief Guards a mutable field of an object against a concurrent verb.
 *
 *  Free-threading, and the critical sections that fence it, arrived in 3.13; on 3.12 the
 *  interpreter lock already serializes what these protect, so they compile to the bare block.
 */
#if PY_VERSION_HEX >= 0x030D0000
#define ustore_py_critical_section_begin_(object) Py_BEGIN_CRITICAL_SECTION(object)
#define ustore_py_critical_section_end_()         Py_END_CRITICAL_SECTION()
#else
#define ustore_py_critical_section_begin_(object) do {
#define ustore_py_critical_section_end_() \
    }                                     \
    while (0)
#endif

#pragma endregion Calling Convention

#pragma region Shared Types

/** The `uuid.UUID` class every key and collection ID is answered as, imported once at module init. */
extern PyObject* UUIDType;

/**
 *  @brief Imports `uuid.UUID` into @c UUIDType, which every conversion between keys and objects needs.
 *  @return 0 on success, or -1 with an exception set.
 */
int ustore_py_uuid_import(void);

extern PyTypeObject DatabaseType;
extern PyTypeObject TransactionType;
extern PyTypeObject SnapshotType;
extern PyTypeObject ExecutorType;
extern PyTypeObject ArenaType;
extern PyTypeObject ViewType;
extern PyTypeObject DeferredViewType;
extern PyTypeObject DeferredType;
extern PyTypeObject ValuesType;
extern PyTypeObject KeysType;
extern PyTypeObject ColumnsType;
extern PyTypeObject NeighborhoodsType;
extern PyTypeObject SpanType;
extern PyTypeObject CollectionType;
extern PyTypeObject BlobsViewType;
extern PyTypeObject DocsViewType;
extern PyTypeObject DocsJSONViewType;
extern PyTypeObject CursorType;
extern PyTypeObject PageType;

/** The named tuple of what an engine supports, built at module init as `ustore.Capabilities`. */
extern PyTypeObject* CapabilitiesType;

/** The named tuple of the bounds a measurement answers, built at module init as `ustore.Measurement`. */
extern PyTypeObject* MeasurementType;

/** The named tuple of what an open database reports about itself, built at module init as `ustore.Description`. */
extern PyTypeObject* DescriptionType;

/**
 *  @brief Builds @c CapabilitiesType, @c MeasurementType and @c DescriptionType.
 *  @return 0 on success, or -1 with an exception set.
 */
int ustore_py_types_import(void);

/** The C enums a caller meets, each mirrored as the `enum.IntEnum` of `ustore` named in @c types.c. */
typedef enum ustore_py_enum_t {
    ustore_py_enum_status_k = 0,
    ustore_py_enum_mood_k,
    ustore_py_enum_watch_k,
    ustore_py_enum_durability_k,
    ustore_py_enum_drop_mode_k,
    ustore_py_enum_scan_mode_k,
    ustore_py_enum_modality_k,
    ustore_py_enum_isolation_k,
    ustore_py_enum_transport_k,
    ustore_py_enum_engine_k,
    ustore_py_enum_role_k,
    ustore_py_enum_field_type_k,
    ustore_py_enum_modification_k,
    ustore_py_enums_count_k,
} ustore_py_enum_t;

/**
 *  @brief Adds the enumerators of every enum to @p module as `_enumerators`, and keeps @p module, whose attributes
 *      hold the classes once `ustore/__init__.py` built them from it.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
int ustore_py_register_enums(PyObject* module);

/**
 *  @brief Reads any `int` that is an enumerator of @p kind, such as a member of the class mirroring it.
 *  @param[in] object The value, refused when it is a bool, which reads as zero or one.
 *  @param[in] field The keyword to name in a failure.
 *  @param[out] value The enumerator.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
int ustore_py_enum_from_object(PyObject* object, char const* field, ustore_py_enum_t kind, int* value);

/** The member of @p kind valued @p value as a new reference, or NULL with an exception set. */
PyObject* ustore_py_enum_to_object(ustore_py_enum_t kind, int value);

/** An open database and the capabilities its engine reports. */
typedef struct Database {

    /** The object header every instance carries. */
    PyObject ob_base;

    /** The open database, or NULL once it has been closed. */
    ustore_database_t handle;

    /** What the engine supports, described once at open. */
    ustore_capabilities_t capabilities;
} Database;

/** An optimistic transaction grouping operations across collections and modalities. */
typedef struct Transaction {

    /** The object header every instance carries. */
    PyObject ob_base;

    /** The open transaction, or NULL once it has been closed. */
    ustore_transaction_t handle;

    /** The database this transaction runs against, kept alive for its whole life. */
    PyObject* database;

    /** The database handle this transaction holds open in C, valid after the database object closes. */
    ustore_database_t database_handle;

    /** Whether the verbs record the keys they touch, unless a view of it says otherwise. */
    ustore_watch_t watch;
} Transaction;

/** A point-in-time state of the whole database, whose read verbs see what it pinned. */
typedef struct Snapshot {

    /** The object header every instance carries. */
    PyObject ob_base;

    /** The pinned view, or NULL once it has been closed. */
    ustore_snapshot_t handle;

    /** The database this snapshot reads, kept alive for its whole life. */
    PyObject* database;

    /** The database handle this snapshot holds open in C, valid after the database object closes. */
    ustore_database_t database_handle;

    /** The stamp the snapshot reads at, as its init reported it. */
    ustore_stamp_t read_stamp;
} Snapshot;

/** The threads that run calls, which the C executor owns. */
typedef struct Executor {

    /** The object header every instance carries. */
    PyObject ob_base;

    /** The executor, or NULL once it has been closed. */
    ustore_executor_t handle;
} Executor;

/**
 *  @brief The memory every output whose length scales with a call lands in, rewound by the next call handed it.
 *
 *  A call is refused while a buffer over the previous outputs is still exported, and every result over them refuses
 *  to be read once a later call rewound it.
 */
typedef struct Arena {

    /** The object header every instance carries. */
    PyObject ob_base;

    /** The arena, freed once the object dies, which waits in C for the deferred calls naming it. */
    ustore_arena_t handle;

    /** Buffers exported over its outputs, plus one while a call holds it. */
    Py_ssize_t exports;

    /** Calls that claimed it so far, which a result compares against the one it was written by. */
    size_t generation;
} Arena;

/** What a call addresses and how it runs, fixed per receiver: the header of every argument struct. */
typedef struct View {

    /** The object header every instance carries. */
    PyObject ob_base;

    /** The @c Database, @c Transaction or @c Snapshot the calls address, held. */
    PyObject* origin;

    /** The executor running the calls, held, or NULL for the calling thread. */
    Executor* executor;

    /** Whether a transaction records the keys the calls touch. */
    ustore_watch_t watch;
} View;

/** What one verb addresses and how it runs, as the receiver it was reached through names them. */
typedef struct ustore_py_context_t {

    /** The database handle the verb calls into, held open by the database object or by the origin. */
    ustore_database_t database_handle;

    /** The @c Database, @c Transaction or @c Snapshot the verb addresses, or the @c Database class an open builds,
     *  borrowed. */
    PyObject* origin;

    /** The transaction the verb joins, or NULL. */
    ustore_transaction_t transaction;

    /** The snapshot the verb reads, at which collection names resolve too, or NULL. */
    ustore_snapshot_t snapshot;

    /** Whether the transaction records the keys its verbs touch. */
    ustore_watch_t watch;

    /** What a read sees: the head, the transaction under @c watch, or the snapshot. */
    ustore_view_t view;

    /** The executor running the call, borrowed, or NULL for the calling thread. */
    Executor* executor;

    /** Whether the call answers once final or once submitted. */
    ustore_completion_t completion;
} ustore_py_context_t;

/** What every call keeps until it finishes, leading its verb's call: on the stack while it blocks, inside its
 *  @c Deferred once submitted. */
typedef struct ustore_py_call_t {

    /** The outcome the library finishes. */
    ustore_outcome_t outcome;

    /** The collection every task addresses, which the argument struct points into. */
    ustore_collection_id_t collection;

    /** The claimed @c Arena the call's outputs land in, held, or NULL for a verb with none. */
    PyObject* arena;

    /** The callable the progress trampoline re-enters, held, or NULL when the caller passed none. */
    PyObject* progress;

    /** The first exception @c progress raised, which outranks the cancellation it turned into; NULL while none has. */
    PyObject* raised;
} ustore_py_call_t;

/** How a verb runs a call it filled: the bytes its call takes, how it reaches the library, answers and lets go. */
typedef struct ustore_py_verb_t {

    /** Bytes of the verb's call, which leads with a @c ustore_py_call_t. */
    size_t call_bytes;

    /** Hands the C argument struct to the library, which copies it, with the thread state detached around it. */
    void (*submit)(void* arguments);

    /** Builds the answer of a call that succeeded out of its output slots, or raises. */
    PyObject* (*answer)(void* call);

    /** Lets go of whatever the fill gathered, the arena claim included. */
    void (*release)(void* call);
} ustore_py_verb_t;

/** A deferred call owning everything the library touches until it finishes, the object being the allocation, so
 *  nothing it lends ever moves. */
typedef struct Deferred {

    /** The object header every instance carries, sized in bytes of the verb's call. */
    PyVarObject ob_base;

    /** How the call answers and lets go; NULL once it let go of its inputs. */
    ustore_py_verb_t const* verb;

    /** The @c Database, @c Transaction or @c Snapshot the call addresses, or the @c Database class an open builds,
     *  held so the handles it names outlive the call. */
    PyObject* origin;

    /** The answer, once the call finished without raising. */
    PyObject* answer;

    /** The exception, once the call finished raising it. */
    PyObject* raised;

    /** The verb's call, led by a @c ustore_py_call_t, which the outcome lives in. */
    uint64_t call[];
} Deferred;

/**
 *  @brief A batch of keys, and whatever storage gathering them needed.
 *
 *  A caller passing a contiguous buffer of 16-byte keys hands us the array outright and @c owned
 *  stays NULL; a caller passing a buffer of 64-bit integers or a sequence of keys gets them
 *  converted into the inline run, or into one allocation when the batch outgrows it.
 */
typedef struct ustore_py_keys_t {

    /** The keys, aliasing whichever storage the fields below name. */
    ustore_key_t* keys;

    /** Keys the batch holds. */
    size_t count;

    /** The heap block backing @c keys, or NULL when they sit inline or in a buffer. */
    ustore_key_t* owned;

    /** The buffer backing @c keys when the caller exported one. */
    Py_buffer buffer;

    /** Whether a buffer was borrowed and must be released. */
    int borrowed;

    /** Storage for a batch that fits without the heap. */
    ustore_key_t inline_keys[ustore_py_keys_inline_k];
} ustore_py_keys_t;

/** Whether one value repeats for every task, or every task carries its own. */
typedef enum ustore_py_contents_spread_t {

    /** One value per task. */
    ustore_py_contents_per_task_k = 0,

    /** One value for every task, which the accessor repeats. */
    ustore_py_contents_broadcast_k = 1,
} ustore_py_contents_spread_t;

/**
 *  @brief The values one write batch hands the ABI as one strings view, every byte borrowed until the call finishes.
 *
 *  A tape passes as it is. Any other batch is borrowed value by value into @c views, which an accessor reads, and its
 *  buffers stay exported for the whole call. A value is never @c None, since removals take a verb of their own.
 */
typedef struct ustore_py_contents_t {

    /** The values as the call passes them: the caller's tape, or an accessor over @c views. */
    ustore_strings_view_t view;

    /** The view of every borrowed value, or of the one value every task repeats. */
    ustore_bytes_view_t* views;

    /** One exported buffer per borrowed value, or the bytes and the offsets of a pair of buffers. */
    Py_buffer* buffers;

    /** Values the batch holds, one when it broadcasts. */
    size_t count;

    /** Buffers already exported, so a failure releases exactly those. */
    size_t borrowed;

    /** Whether the values repeat for every task or stand one per task. */
    ustore_py_contents_spread_t spread;

    /** The capsule of the Arrow array whose tape the call reads, held until it finishes, or NULL. */
    PyObject* held;

    /** The one heap block the views and the buffers share, or NULL when they sit inline. */
    void* block;

    /** Storage for the views of a batch that fits without the heap. */
    ustore_bytes_view_t inline_views[ustore_py_contents_inline_k];

    /** Storage for the buffers of a batch that fits without the heap. */
    Py_buffer inline_buffers[ustore_py_contents_inline_k];
} ustore_py_contents_t;

/**
 *  @brief One tape of values a read wrote into an arena, with its presences, viewed until it rewinds.
 *
 *  Exports the tape through the buffer protocol and slices one task's bytes out of it without copying, and every
 *  exported buffer pins the arena against the next call until it is released.
 */
typedef struct Values {

    /** The object header every instance carries. */
    PyObject ob_base;

    /** The @c Arena the outputs live in, kept alive so they outlive every other name for them. */
    PyObject* arena;

    /** The arena's generation when the read wrote the outputs; any later one means a call rewound it. */
    size_t generation;

    /** One bit per task marking the present keys, or NULL for a read of no tasks. */
    ustore_octet_t const* presences;

    /** The values as the read wrote them, a tape of 32-bit or 64-bit offsets, zeroed for a read of no tasks. */
    ustore_strings_view_t tape;

    /** Tasks the read carried. */
    size_t count;
} Values;

/**
 *  @brief The keys one batch scan or sample wrote into an arena, viewed until it rewinds.
 *
 *  A scan and a sample answer the same shape: a run of keys per task, laid end to end, with the
 *  @c count + 1 boundaries in front of them.
 */
typedef struct Keys {

    /** The object header every instance carries. */
    PyObject ob_base;

    /** The @c Arena the outputs live in, kept alive so they outlive every other name for them. */
    PyObject* arena;

    /** The arena's generation when the call wrote the outputs. */
    size_t generation;

    /** One offset into @c tape per task, plus one closing the last run, or NULL for no tasks. */
    ustore_length_t const* offsets;

    /** The keys every task found, laid end to end. */
    ustore_key_t const* tape;

    /** Tasks the scan or sample carried. */
    size_t count;

    /** The shape the whole tape exports: keys, then the sixteen bytes of each. */
    Py_ssize_t shape[2];

    /** The strides matching @c shape. */
    Py_ssize_t strides[2];
} Keys;

/**
 *  @brief The typed columns one gather wrote into an arena, viewed until it rewinds.
 *
 *  Every column holds one entry per document with three bitmaps beside it, and a string column
 *  holds offsets into its own strings, as one @c ustore_docs_column_view_t per field describes.
 */
typedef struct Columns {

    /** The object header every instance carries, sized in fields of trailing storage. */
    PyVarObject ob_base;

    /** The @c Arena the columns live in, kept alive so they outlive every other name for them. */
    PyObject* arena;

    /** The arena's generation when the gather wrote the columns. */
    size_t generation;

    /** One view per field, inside the arena, or NULL for a gather of no fields. */
    ustore_docs_column_view_t const* columns;

    /** Fields the gather carried, the columns of the result. */
    size_t fields_count;

    /** Documents the gather carried, the rows of every column. */
    size_t docs_count;

    /** The type every field was converted into, owned by the object. */
    ustore_docs_field_type_t types[];
} Columns;

/**
 *  @brief The edges one batch of vertices connects to, written into an arena and viewed until it rewinds.
 *
 *  Every edge is three keys, the source, the target and the edge ID, laid end to end vertex after
 *  vertex, so the whole tape exports as an array of shape (edges, 3, 16) bytes. The trailing
 *  storage holds the edge offsets derived from the degrees once the read finished.
 */
typedef struct Neighborhoods {

    /** The object header every instance carries. */
    PyObject ob_base;

    /** The @c Arena the tape, the offsets and the degrees live in, kept alive so they outlive every other name for
     *  them. */
    PyObject* arena;

    /** The arena's generation when the read wrote the outputs. */
    size_t generation;

    /** Three keys per edge, laid end to end vertex after vertex. */
    ustore_key_t const* tape;

    /** One offset per vertex into @c tape, counted in edges, plus one closing it, or NULL for a read of no vertices. */
    ustore_length_t const* offsets;

    /** The degree of every vertex, or @c ustore_vertex_degree_missing_k where it is absent. */
    ustore_vertex_degree_t const* degrees;

    /** Vertices the lookup carried. */
    size_t count;

    /** The shape the whole tape exports: edges, the three keys of each, and the sixteen bytes of a key. */
    Py_ssize_t shape[3];

    /** The strides matching @c shape. */
    Py_ssize_t strides[3];
} Neighborhoods;

/**
 *  @brief A read-only window into an arena, which a @c memoryview is built over.
 *
 *  A view of one task carries its own shape and format while keeping the arena alive, so no slice
 *  or cast is needed and an empty run still exports its shape. Each buffer it exports pins the arena
 *  until released, and it refuses to export once a later call rewound the arena.
 */
typedef struct Span {

    /** The object header every instance carries. */
    PyObject ob_base;

    /** The @c Arena the window lies in, kept alive for as long as this window is. */
    PyObject* arena;

    /** The arena's generation when the call wrote the memory. */
    size_t generation;

    /** The first byte of the window. */
    void const* data;

    /** The struct-module format of one item. */
    char const* format;

    /** Bytes one item takes. */
    Py_ssize_t itemsize;

    /** Dimensions the window has, one or two. */
    int ndim;

    /** Items along every dimension. */
    Py_ssize_t shape[2];

    /** Bytes between neighbours along every dimension. */
    Py_ssize_t strides[2];
} Span;

#pragma endregion Shared Types

#pragma region Failures

/**
 *  @brief Raises the exception the status of @p outcome maps onto.
 *  @param[in] outcome The outcome of one call, whose message is static and never freed.
 *  @return -1 with an exception set when the call failed, 0 otherwise.
 */
int ustore_py_raise_on_outcome(ustore_outcome_t* outcome);

/**
 *  @brief Raises the exception @p status maps onto, with a formatted message.
 *  @param[in] status What went wrong, which picks the exception.
 *  @param[in] format A @c PyUnicode_FromFormat template for the detail.
 *  @return -1 with an exception set, always.
 */
int ustore_py_raise(ustore_status_t status, char const* format, ...);

/**
 *  @brief Raises @p kind with a formatted message, keeping whatever is already raised as its
 *      cause, since the exporter's own diagnosis is more precise than any guess we could make.
 *  @param[in] kind The exception to raise.
 *  @param[in] format A @c PyUnicode_FromFormat template for the detail.
 *  @return -1 with an exception set, always.
 */
int ustore_py_raise_from_cause(PyObject* kind, char const* format, ...);

/**
 *  @brief Readies every exception type and adds it to @p module.
 *  @param[inout] module The extension module under construction.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
int ustore_py_register_errors(PyObject* module);

#pragma endregion Failures

#pragma region Arenas

/**
 *  @brief A new @c Arena made through @c ustore_arena_init where @p executor computes, unclaimed.
 *
 *  Shorthand with no place for an arena passes one of these to its verb and drops its own reference
 *  at once, so the arena dies with the call that claimed it.
 *
 *  @param[in] executor The executor whose calls it serves, or NULL for calls on the calling thread.
 *  @return A new reference, or NULL with an exception set.
 */
PyObject* ustore_py_arena_new(ustore_executor_t executor);

/**
 *  @brief Claims the @c Arena @p object for one call, rewinding it: every result over its previous
 *      outputs goes stale.
 *  @param[out] arena A new reference to the claimed @c Arena; hand it to @c ustore_py_arena_unclaim.
 *  @return 0 on success, -1 with an exception set when @p object is no @c Arena, serves a call, or
 *      still exports.
 */
int ustore_py_arena_claim(PyObject* object, PyObject** arena);

/** Ends the claim @c ustore_py_arena_claim took, and drops its reference. */
void ustore_py_arena_unclaim(PyObject* arena);

/** The C handle of the @c Arena @p arena. */
ustore_arena_t ustore_py_arena_handle(PyObject* arena);

/** The generation of the @c Arena @p arena, which a result built while the arena serves its call records. */
size_t ustore_py_arena_generation(PyObject* arena);

/**
 *  @brief Holds @p arena against a rewind while memory a call of @p generation wrote is read, as one more export.
 *  @return 0 on success, -1 with an @c UninitializedError set when a later call rewound it.
 */
int ustore_py_arena_pin(PyObject* arena, size_t generation);

/** Ends the hold @c ustore_py_arena_pin took. */
void ustore_py_arena_unpin(PyObject* arena);

#pragma endregion Arenas

#pragma region Verb Options

/** The knobs every verb accepts as keywords after its own arguments. */
typedef struct ustore_py_verb_options_t {

    /** The collection the tasks address. */
    ustore_collection_id_t collection;

    /** How durable the writes are once a call at the head finishes. */
    ustore_durability_t durability;
} ustore_py_verb_options_t;

/** Starts the shared options at the main collection and buffered writes, so a verb only overrides what a keyword
 *  names. */
void ustore_py_verb_options_init(ustore_py_verb_options_t* options);

/**
 *  @brief Reads one of the keywords every verb shares, leaving the rest to the caller.
 *  @param[in] context The receiver the verb runs through, at whose snapshot a collection name resolves.
 *  @param[in] name The keyword's name.
 *  @param[in] value The keyword's value.
 *  @param[out] options Where a recognized keyword lands.
 *  @return 1 when the keyword was consumed, 0 when it belongs to the verb, -1 on a failure.
 */
int ustore_py_verb_option_from_keyword(ustore_py_context_t const* context, PyObject* name, PyObject* value,
                                       ustore_py_verb_options_t* options);

/**
 *  @brief Walks the keywords of a verb that takes none of its own.
 *  @param[in] context The database and view the verb runs against.
 *  @param[in] args The fast-call arguments, positional ones first.
 *  @param[in] positional_args_count Positional arguments the call carried.
 *  @param[in] args_names_tuple The keyword names, or NULL when none followed.
 *  @param[out] options Where every recognized keyword lands.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
int ustore_py_verb_options_from_keywords(ustore_py_context_t const* context, PyObject* const* args,
                                         Py_ssize_t positional_args_count, PyObject* args_names_tuple,
                                         ustore_py_verb_options_t* options);

#pragma endregion Verb Options

#pragma region Shared Helpers

/**
 *  @brief Fills @p context from a @c Database, @c Transaction, @c Snapshot, @c View or @c DeferredView.
 *  @param[in] self The receiver the verb was reached through.
 *  @param[out] context What the verb addresses and how it runs.
 *  @return 0 on success, -1 with an exception set when a handle or the executor is closed.
 */
int ustore_py_context_from_object(PyObject* self, ustore_py_context_t* context);

/** The executor handle @p context runs its calls on, or NULL for the calling thread. */
ustore_executor_t ustore_py_context_executor(ustore_py_context_t const* context);

/**
 *  @brief Refuses a write through a snapshot, which only reads.
 *  @return 0 when @p context may write, or -1 with a @c TypeError set.
 */
int ustore_py_context_writable(ustore_py_context_t const* context);

/**
 *  @brief Refuses @p operation on a deferred receiver, naming the flat verb that does the same.
 *  @return 0 when @p context blocks, or -1 with a @c TypeError set.
 */
int ustore_py_context_blocking(ustore_py_context_t const* context, char const* operation, char const* instead);

/**
 *  @brief Opens the storage of one call: @p stack when @p context blocks, or the trailing storage of a new
 *      @c Deferred otherwise.
 *  @param[in] stack The caller's storage of @c verb->call_bytes, used while the call blocks.
 *  @return The zeroed call, or NULL with an exception set.
 */
void* ustore_py_call_open(ustore_py_context_t const* context, ustore_py_verb_t const* verb, void* stack);

/**
 *  @brief Runs a filled call: blocking, answering once it is final, or submitted, answering its @c Deferred at once.
 *  @param[in] arguments The C argument struct on the caller's stack, pointing into @p call, which submission copies.
 *  @return A new reference, or NULL with an exception set.
 */
PyObject* ustore_py_call_run(ustore_py_context_t const* context, ustore_py_verb_t const* verb, void* call,
                             void* arguments);

/**
 *  @brief Lets go of a call whose fill failed before it was submitted, keeping the exception set.
 *  @return NULL, always.
 */
PyObject* ustore_py_call_discard(ustore_py_context_t const* context, ustore_py_verb_t const* verb, void* call);

/** Ends the arena claim the header of a call holds, and lets go of its progress callable and what that raised. */
void ustore_py_call_release(ustore_py_call_t* call);

/** Answers None, for a call whose only answer is its success. */
PyObject* ustore_py_call_answer_none(void* call);

/**
 *  @brief Reads the `progress` keyword of a verb, borrowing the callable for the call to hold once it opens.
 *  @param[in] object A callable taking the units finished and the units in all, or None.
 *  @param[out] progress The callable, borrowed, or NULL for None.
 *  @return 0 on success, -1 with a @c TypeError set when @p object is neither callable nor None.
 */
int ustore_py_progress_from_object(PyObject* object, PyObject** progress);

/** The trampoline a verb registers with @p call as its state, or NULL when the call holds no progress callable. */
ustore_progress_callback_t ustore_py_progress_callback(ustore_py_call_t const* call);

/**
 *  @brief Reads one UUID from a `uuid.UUID`, 16 raw bytes, canonical UUID text or a non-negative integer.
 *  @param[in] object The identifier in any of those spellings.
 *  @param[out] uuid The sixteen bytes it spells, an integer big-endian as `uuid.UUID(int=n)` spells it.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
int ustore_py_uuid_from_object(PyObject* object, ustore_uuid_t* uuid);

/**
 *  @brief Builds the `uuid.UUID` whose bytes are @p uuid.
 *  @param[in] uuid The sixteen bytes, in the order they are stored.
 *  @return A new reference, or NULL with an exception set.
 */
PyObject* ustore_py_uuid_to_object(ustore_uuid_t const* uuid);

/**
 *  @brief Reads one key from any spelling @c ustore_py_uuid_from_object accepts.
 *  @param[in] object The key.
 *  @param[out] key The key the ABI addresses.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
int ustore_py_key_from_object(PyObject* object, ustore_key_t* key);

/**
 *  @brief Whether @p object spells one key rather than a batch of them.
 *
 *  A `uuid.UUID`, a string and a run of raw bytes are one key although the latter two are
 *  sequences, while an integer is one key unless it is also a sequence, as an array is.
 */
int ustore_py_is_one_key(PyObject* object);

/**
 *  @brief Gathers a batch of keys from a buffer of 16-byte keys, a buffer of 64-bit integers, or a sequence.
 *  @param[in] object The keys, as a contiguous buffer of either layout or a sequence of keys.
 *  @param[out] batch The keys and whatever storage gathering them needed.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
int ustore_py_keys_from_object(PyObject* object, ustore_py_keys_t* batch);

/**
 *  @brief Gathers one key or a batch of them, the way `view[key]` and `view[[keys]]` differ.
 *  @param[in] object One key in any spelling, or a batch as @c ustore_py_keys_from_object reads it.
 *  @param[out] batch The keys, one inline for a single key.
 *  @return 1 for one key, 0 for a batch, or -1 with an exception set.
 */
int ustore_py_keys_from_subscript(PyObject* object, ustore_py_keys_t* batch);

/**
 *  @brief Releases whatever @c ustore_py_keys_from_object acquired.
 *  @param[inout] batch The batch to release.
 */
void ustore_py_keys_release(ustore_py_keys_t* batch);

/** A strings view whose accessor writes @p views[i] for string @c i, reading only memory the call keeps in place. */
ustore_strings_view_t ustore_py_strings_each(ustore_bytes_view_t const* views);

/** A strings view whose accessor writes @p view for every string, which is how one string reaches every task. */
ustore_strings_view_t ustore_py_strings_repeated(ustore_bytes_view_t const* view);

/** The bytes of string @p index of a tape a call wrote, whichever width its offsets take. */
ustore_bytes_view_t ustore_py_tape_string(ustore_strings_view_t const* tape, size_t index);

/** Bytes the first @p count strings of a tape span, zero when the call wrote no offsets. */
ustore_size_t ustore_py_tape_bytes(ustore_strings_view_t const* tape, size_t count);

/**
 *  @brief Borrows the bytes of one value into the next view of @p batch.
 *  @param[in] object The value, as anything exporting a buffer.
 *  @param[out] batch The batch the borrowed value joins.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
int ustore_py_contents_borrow(PyObject* object, ustore_py_contents_t* batch);

/**
 *  @brief Sizes @p batch for @p count values, laying their views inline or in one heap block, and points its strings
 *      view at them.
 *  @param[out] batch The batch to prepare, which must stay in place until it is released.
 *  @param[in] count Values the batch will hold.
 *  @param[in] spread Whether they stand one per task or one value repeats for every task.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
int ustore_py_contents_reserve(ustore_py_contents_t* batch, size_t count, ustore_py_contents_spread_t spread);

/**
 *  @brief Reads @p object as a tape the call passes as it is: an object exporting an Arrow `binary`, `large_binary`,
 *      `string` or `large_string` array, or a `(data, offsets)` tuple of buffers whose offsets are unsigned 32-bit or
 *      64-bit integers.
 *  @param[out] batch The batch holding the tape and whatever keeps it alive.
 *  @return 1 for a tape, 0 for anything else, or -1 with an exception set.
 */
int ustore_py_contents_from_tape(PyObject* object, ustore_py_contents_t* batch);

/**
 *  @brief Releases every buffer @p batch borrowed, the block they shared and the tape it held.
 *  @param[inout] batch The batch to release.
 */
void ustore_py_contents_release(ustore_py_contents_t* batch);

/**
 *  @brief A new @c Values over the tape a read of @p count tasks wrote into @p arena, which its call still claims.
 *  @param[in] presences The presence bits, or NULL for a tape whose every value is present, as a cursor writes one.
 *  @return A new reference, or NULL with an exception set.
 */
PyObject* ustore_py_values_new(PyObject* arena, ustore_octet_t const* presences, ustore_strings_view_t const* tape,
                               size_t count);

/** Whether task @p task_index found its key, as Arrow presence bits address it, least significant first. */
int ustore_py_present(ustore_octet_t const* presences, size_t task_index);

/**
 *  @brief A new @c Keys over the runs a scan or sample of @p count tasks wrote into @p arena, which its call still
 *      claims.
 *  @return A new reference, or NULL with an exception set.
 */
PyObject* ustore_py_keys_new(PyObject* arena, ustore_length_t const* offsets, ustore_key_t const* tape, size_t count);

/**
 *  @brief Builds a read-only @c memoryview over memory a call of @p generation wrote into @p arena, which pins the
 *      arena until the view is released.
 *  @param[in] arena The @c Arena holding the memory, which the view holds a reference to.
 *  @param[in] data The first byte of the window, which may be NULL for an empty one.
 *  @param[in] format The struct-module format of one item.
 *  @param[in] itemsize Bytes one item takes.
 *  @param[in] ndim Dimensions the window has, one or two.
 *  @param[in] shape Items along every dimension.
 *  @return A new reference, or NULL with an exception set.
 */
PyObject* ustore_py_span_view(PyObject* arena, size_t generation, void const* data, char const* format,
                              Py_ssize_t itemsize, int ndim, Py_ssize_t const* shape);

/**
 *  @brief Fills a read-only buffer over memory @p owner keeps alive, honouring what @p flags ask for.
 *  @param[out] view The buffer to fill.
 *  @param[in] owner The object exporting it, which the buffer holds a reference to.
 *  @param[in] data The first byte, which may be NULL for an empty buffer.
 *  @param[in] format The struct-module format of one item.
 *  @param[in] itemsize Bytes one item takes.
 *  @param[in] ndim Dimensions the buffer has.
 *  @param[in] shape Items along every dimension, which must outlive the buffer.
 *  @param[in] strides Bytes between neighbours along every dimension, which must outlive the buffer.
 *  @param[in] flags What the consumer asked for.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
int ustore_py_buffer_fill(Py_buffer* view, PyObject* owner, void const* data, char const* format, Py_ssize_t itemsize,
                          int ndim, Py_ssize_t* shape, Py_ssize_t* strides, int flags);

/**
 *  @brief Reads a non-negative count from an object CPython can index.
 *  @param[in] object The count.
 *  @param[in] field The keyword to name in a failure.
 *  @param[out] value The count read.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
int ustore_py_size_from_object(PyObject* object, char const* field, size_t* value);

/**
 *  @brief Reads the one optional argument of a verb, given after its required ones by position or as @p keyword.
 *  @param[in] verb The method's name, as a failure should read it.
 *  @param[in] positional_args_count Positional arguments the call carried.
 *  @param[in] args_names_tuple The keyword names, or NULL when none followed.
 *  @param[in] required_positional Positional arguments the verb fixes before the optional one.
 *  @param[in] keyword The name the optional argument goes by.
 *  @param[out] argument The argument, left untouched when the call carried none.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
int ustore_py_optional_argument(char const* verb, PyObject* const* args, Py_ssize_t positional_args_count,
                                PyObject* args_names_tuple, Py_ssize_t required_positional, char const* keyword,
                                PyObject** argument);

/**
 *  @brief Checks a fast-call's shape and reports how many keywords followed.
 *  @param[in] verb The method's name, as a failure should read it.
 *  @param[in] positional_args_count Positional arguments the call carried.
 *  @param[in] args_names_tuple The keyword names, or NULL when none followed.
 *  @param[in] required_positional Positional arguments the verb fixes.
 *  @return The keyword count, or -1 with an exception set when the shape is wrong.
 */
Py_ssize_t ustore_py_args_names_count_checked(char const* verb, Py_ssize_t positional_args_count,
                                              PyObject* args_names_tuple, Py_ssize_t required_positional);

#pragma endregion Shared Helpers

#pragma region Names

/** What a lookup of a collection name found. */
typedef enum ustore_py_naming_t {

    /** The name addresses the collection written to @c collection. */
    ustore_py_naming_found_k = 0,

    /** No collection carries the name. */
    ustore_py_naming_missing_k = 1,

    /** The lookup failed and an exception is set. */
    ustore_py_naming_failed_k = 2,
} ustore_py_naming_t;

/**
 *  @brief Reads the collection a name was given at the snapshot of @p context, or at the head without one.
 *
 *  The lookup runs on the calling thread whatever executor the receiver names, so a deferred call
 *  never waits behind the calls submitted before it to learn its collection.
 *
 *  @param[in] context The receiver to resolve through.
 *  @param[in] name The name's bytes, which need no null terminator.
 *  @param[in] length The name's length in bytes.
 *  @param[out] collection The identifier the name addresses, untouched unless the name was found.
 *  @param[out] modality What the collection holds, or NULL when the caller does not ask.
 *  @return Whether the name was found, missing, or could not be looked up.
 */
ustore_py_naming_t ustore_py_collection_named(ustore_py_context_t const* context, char const* name, size_t length,
                                              ustore_collection_id_t* collection, ustore_modality_t* modality);

/**
 *  @brief Reads the collection an argument names, accepting an ID, a name, or @c None.
 *  @param[in] context The receiver to resolve a name through.
 *  @param[in] object The collection, as a `uuid.UUID` ID, a string name, or @c None for the main one.
 *  @param[out] collection The identifier the argument addresses.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
int ustore_py_collection_from_object(ustore_py_context_t const* context, PyObject* object,
                                     ustore_collection_id_t* collection);

/**
 *  @brief Lists the names of collections at the head in the order of their text, up to @p limit of them.
 *  @param[in] context The database to list.
 *  @param[in] after The smallest name listed, or NULL to start at the first one.
 *  @param[in] after_length The length of @p after in bytes.
 *  @param[in] limit Most names the walk returns.
 *  @param[in] arena The @c Arena the listing lands in, claimed for the call.
 *  @return A new tuple of names, or NULL with an exception set.
 */
PyObject* ustore_py_collection_names(ustore_py_context_t const* context, char const* after, size_t after_length,
                                     size_t limit, PyObject* arena);

/**
 *  @brief Answers the @c Collection a receiver addresses by name, as `db['movies']` does.
 *  @param[in] view The @c Database, @c Transaction, @c Snapshot, @c View or @c DeferredView indexed.
 *  @param[in] name The collection's name, where the empty one is the main collection.
 *  @return A new reference, or NULL with an exception set.
 */
PyObject* ustore_py_collection_subscript(PyObject* view, PyObject* name);

#pragma endregion Names

#pragma region Verb Cores

/** A read of the values or the documents of a batch of keys, which its caller gathers in place. */
typedef struct ustore_py_read_call_t {

    /** What the call keeps until it finishes: the outcome and the arena holding the tape. */
    ustore_py_call_t call;

    /** The keys to read. */
    ustore_py_keys_t keys;

    /** The top-level key or JSON Pointer a read of documents reads inside each, held, or NULL. */
    PyObject* field;

    /** The UTF-8 bytes @c field caches, which the accessor repeats for every key. */
    ustore_bytes_view_t field_view;

    /** The one key a subscript named, held, whose absence raises @c KeyError, or NULL for a batch. */
    PyObject* subscript;

    /** Output slot for the presence bits, inside the arena. */
    ustore_octet_t const* presences;

    /** Output slot for the tape of values, inside the arena. */
    ustore_strings_view_t values;
} ustore_py_read_call_t;

/** A read of values answering the @c Values over its arena, as `read` does. */
extern ustore_py_verb_t const ustore_py_read_verb;

/** A read of values answering copies, as `Collection.blobs` does: bytes for a subscript, raising @c KeyError where
 *  the key is absent, or a list with None in place of every absent key. */
extern ustore_py_verb_t const ustore_py_read_copies_verb;

/** A read of values answering a dict of every key it read onto a copy of its value or None, as `Collection.sample`
 *  does. */
extern ustore_py_verb_t const ustore_py_read_drawn_verb;

/** Answers the @c Values a read of values or documents wrote into its arena. */
PyObject* ustore_py_read_answer_values(void* call);

/** Answers copies of what a read of values or documents wrote, as @c ustore_py_read_copies_verb spells them. */
PyObject* ustore_py_read_answer_copies(void* call);

/** Lets go of the keys, the field, the subscript and the arena claim of a read. */
void ustore_py_read_release(void* call);

/**
 *  @brief Reads the keys gathered into @p call: the core of `read`, of `Collection.blobs[key]` and of
 *      `Collection.sample`.
 *  @param[in] context The receiver the read runs through.
 *  @param[in] options The collection and knobs the read carries.
 *  @param[in] verb How the read answers, the verb @p call was opened with.
 *  @param[inout] call What @c ustore_py_call_open answered, its keys gathered and its subscript set for one key.
 *  @param[in] arena The @c Arena the outputs land in, claimed here.
 *  @return The answer of @p verb, or its @c Deferred; NULL with an exception set otherwise.
 */
PyObject* ustore_py_values_read(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                                ustore_py_verb_t const* verb, ustore_py_read_call_t* call, PyObject* arena);

/**
 *  @brief Reads whether one key is present into a one-off arena: the core of `contains` and of
 *      `key in Collection.blobs`.
 *  @return True or False, or the @c Deferred of either, or NULL with an exception set.
 */
PyObject* ustore_py_value_contains(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                                   ustore_key_t const* key);

/**
 *  @brief Gathers the values of a batch write, broadcasting one value to every key.
 *  @param[in] object The values: a sequence, or one buffer to broadcast.
 *  @param[in] count Keys the batch carries.
 *  @param[out] batch The borrowed runs the ABI reads, and the layout they travel with.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
int ustore_py_contents_from_object(PyObject* object, size_t count, ustore_py_contents_t* batch);

/** An upsert of a batch of values, which its caller gathers in place. */
typedef struct ustore_py_write_call_t {

    /** What the call keeps until it finishes. */
    ustore_py_call_t call;

    /** The keys to write. */
    ustore_py_keys_t keys;

    /** The borrowed values, one per key or one for every key. */
    ustore_py_contents_t contents;
} ustore_py_write_call_t;

/** How a write runs. */
extern ustore_py_verb_t const ustore_py_write_verb;

/**
 *  @brief Upserts the values gathered into @p call: the core of `write`, `set` and `Collection.blobs[key] = value`.
 *  @return None, or the @c Deferred of it, or NULL with an exception set.
 */
PyObject* ustore_py_values_write(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                                 ustore_py_write_call_t* call);

/** A removal of a batch of keys, which its caller gathers in place. */
typedef struct ustore_py_remove_call_t {

    /** What the call keeps until it finishes. */
    ustore_py_call_t call;

    /** The keys to remove. */
    ustore_py_keys_t keys;
} ustore_py_remove_call_t;

/** How a removal runs. */
extern ustore_py_verb_t const ustore_py_remove_verb;

/**
 *  @brief Removes the keys gathered into @p call with their values or documents: the core of `remove` and of `del`
 *      on every view.
 *  @return None, or the @c Deferred of it, or NULL with an exception set.
 */
PyObject* ustore_py_values_remove(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                                  ustore_py_remove_call_t* call);

/**
 *  @brief Lists up to @p wanted keys from @p start upwards: the core of `scan` and of `Collection.scan`.
 *  @param[in] mode In what order the keys come.
 *  @param[in] progress The callable the scan reports to, borrowed, or NULL for none.
 *  @param[in] arena The @c Arena the keys land in, claimed here.
 *  @return A new @c Keys of one task, or its @c Deferred, or NULL with an exception set.
 */
PyObject* ustore_py_keys_scan(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                              ustore_key_t const* start, size_t wanted, ustore_scan_mode_t mode, PyObject* progress,
                              PyObject* arena);

/**
 *  @brief Draws up to @p wanted keys at random: the core of `sample`, `Collection.sample_keys` and
 *      `Collection.sample`.
 *  @param[in] arena The @c Arena the keys land in, claimed here.
 *  @return A new @c Keys of one task, or its @c Deferred, or NULL with an exception set.
 */
PyObject* ustore_py_keys_sample(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                                size_t wanted, PyObject* arena);

/** How a write takes its documents. */
typedef enum ustore_py_documents_spelling_t {

    /** As `docs_write` and `Collection.docs_json` take them: JSON text or bytes, or a dict or list to serialize. */
    ustore_py_documents_as_json_k = 0,

    /** As `Collection.docs` takes them: one Python object per key, serialized whole. */
    ustore_py_documents_as_objects_k = 1,
} ustore_py_documents_spelling_t;

/** A read of documents answering the @c Values over its arena, as `docs_read` does. */
extern ustore_py_verb_t const ustore_py_docs_read_verb;

/** A read of documents answering copies of their JSON text, as `Collection.docs_json` does. */
extern ustore_py_verb_t const ustore_py_docs_read_copies_verb;

/** A read of documents answering them parsed into Python objects, as `Collection.docs` does. */
extern ustore_py_verb_t const ustore_py_docs_read_objects_verb;

/**
 *  @brief Reads the documents gathered into @p call: the core of `docs_read` and of `Collection.docs[key]`.
 *  @param[in] type How every value is spelled, JSON text or a printed string.
 *  @param[in] verb How the read answers, the verb @p call was opened with.
 *  @param[inout] call What @c ustore_py_call_open answered, its keys gathered, and its field and subscript set as the
 *      caller wants them.
 *  @param[in] arena The @c Arena the outputs land in, claimed here.
 *  @return The answer of @p verb, or its @c Deferred; NULL with an exception set otherwise.
 */
PyObject* ustore_py_docs_read(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                              ustore_docs_field_type_t type, ustore_py_verb_t const* verb, ustore_py_read_call_t* call,
                              PyObject* arena);

/**
 *  @brief Upserts whole documents, blocking: the core of `Collection.docs[key] = document`.
 *  @param[in] documents The documents, spelled as @p spelling says.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
int ustore_py_docs_upsert(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                          ustore_key_t const* keys, size_t count, PyObject* documents,
                          ustore_py_documents_spelling_t spelling);

/**
 *  @brief The Python value of @p value: dict, list, str, int, float, bool or None.
 *  @return A new reference, or NULL with an exception set.
 */
PyObject* ustore_py_object_from_json(yyjson_val* value);

/**
 *  @brief Serializes @p object into @p document: dict, list, tuple, str, int, float, bool or None.
 *  @param[out] value The value built inside @p document.
 *  @return 0 on success, or -1 with a Python error for a type JSON cannot hold.
 */
int ustore_py_json_from_object(PyObject* object, yyjson_mut_doc* document, yyjson_mut_val** value);

#pragma endregion Verb Cores

#pragma region Verbs

extern char const doc_read[];
extern char const doc_write[];
extern char const doc_scan[];
extern char const doc_scan_batch[];
extern char const doc_sample[];
extern char const doc_sample_batch[];
extern char const doc_measure[];
extern char const doc_cursor[];
extern char const doc_get[];
extern char const doc_set[];
extern char const doc_remove[];
extern char const doc_contains[];
extern char const doc_docs_write[];
extern char const doc_docs_read[];
extern char const doc_docs_gist[];
extern char const doc_docs_gather[];
extern char const doc_graph_read_edges[];
extern char const doc_graph_degrees[];
extern char const doc_graph_write_edges[];
extern char const doc_graph_remove_edges[];
extern char const doc_graph_write_vertices[];
extern char const doc_graph_remove_vertices[];

PyObject* ustore_py_api_read(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                             PyObject* args_names_tuple);
PyObject* ustore_py_api_write(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                              PyObject* args_names_tuple);
PyObject* ustore_py_api_scan(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                             PyObject* args_names_tuple);
PyObject* ustore_py_api_scan_batch(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                   PyObject* args_names_tuple);
PyObject* ustore_py_api_sample(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                               PyObject* args_names_tuple);
PyObject* ustore_py_api_sample_batch(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                     PyObject* args_names_tuple);
PyObject* ustore_py_api_measure(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                PyObject* args_names_tuple);
PyObject* ustore_py_api_cursor(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                               PyObject* args_names_tuple);
PyObject* ustore_py_api_get(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                            PyObject* args_names_tuple);
PyObject* ustore_py_api_set(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                            PyObject* args_names_tuple);
PyObject* ustore_py_api_remove(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                               PyObject* args_names_tuple);
PyObject* ustore_py_api_contains(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                 PyObject* args_names_tuple);
PyObject* ustore_py_api_docs_write(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                   PyObject* args_names_tuple);
PyObject* ustore_py_api_docs_read(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                  PyObject* args_names_tuple);
PyObject* ustore_py_api_docs_gist(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                  PyObject* args_names_tuple);
PyObject* ustore_py_api_docs_gather(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                    PyObject* args_names_tuple);
PyObject* ustore_py_api_graph_read_edges(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                         PyObject* args_names_tuple);
PyObject* ustore_py_api_graph_degrees(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                      PyObject* args_names_tuple);
PyObject* ustore_py_api_graph_write_edges(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                          PyObject* args_names_tuple);
PyObject* ustore_py_api_graph_remove_edges(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                           PyObject* args_names_tuple);
PyObject* ustore_py_api_graph_write_vertices(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                             PyObject* args_names_tuple);
PyObject* ustore_py_api_graph_remove_vertices(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                              PyObject* args_names_tuple);

#pragma endregion Verbs

#ifdef __cplusplus
}
#endif

#endif // UNUM_USTORE_PYTHON_LIB_H
