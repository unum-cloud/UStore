/**
 *  @file python/lib.h
 *  @author Ash Vardanian
 *  @date September 16, 2026
 *  @brief Declarations shared by every UStore CPython translation unit.
 *
 *  One extension, one file per header of the C ABI: @c types.c owns the vocabulary every modality
 *  shares, @c database.c owns databases, collections, snapshots and transactions, @c blobs.c owns
 *  the binary verbs, @c docs.c the document verbs, @c graph.c the graph verbs, @c strings.c resolves
 *  names onto identifiers, @c collection.c answers the collections a database is indexed by, and
 *  @c lib.c owns the module glue alone. A verb is written once and reached from both method tables,
 *  since a database and a transaction differ only in the view they address, and its core is reached
 *  from the collection views too, which skip the keywords.
 *
 *  @section python_arena_ownership Arena Ownership
 *
 *  Outputs whose size the inputs bound, such as offsets, presences, degrees and the keys of a scan,
 *  land in the trailing storage of the result object, which is allocated before the call, so they
 *  live exactly as long as the result. Outputs only a call can size, such as tapes of values, land
 *  in an arena, and a later call on the same arena discards what an earlier one left there, so an
 *  arena is owned by exactly one live object at a time. A database keeps one idle arena; a verb
 *  that needs one claims it, runs, and either gives it back before returning or hands it to the
 *  result object it built. A claim that finds the slot empty makes an arena of its own, so two
 *  threads never share one. A result object frees its arena in @c tp_dealloc, and a @c memoryview
 *  over that object holds a reference to it, so no view outlives the bytes it reads and nothing
 *  can write into an arena a result owns.
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
extern PyTypeObject ValuesType;
extern PyTypeObject KeysType;
extern PyTypeObject ColumnsType;
extern PyTypeObject EdgesType;
extern PyTypeObject SpanType;
extern PyTypeObject CollectionType;
extern PyTypeObject BlobsViewType;
extern PyTypeObject DocsViewType;
extern PyTypeObject DocsJSONViewType;

/** The failure every other one specializes, subclassing @c RuntimeError. */
extern PyObject* ErrorType;

/** An argument is null, contradicts another one, or names a handle that was never opened. */
extern PyObject* ArgumentErrorType;

/** The engine or the build does not implement what the call asks for. */
extern PyObject* UnsupportedErrorType;

/** A snapshot, collection or key the call addresses does not exist. */
extern PyObject* MissingErrorType;

/** A snapshot or collection the call creates already exists. */
extern PyObject* ExistsErrorType;

/** A concurrent writer touched what this transaction read, wrote or scanned. */
extern PyObject* ConflictErrorType;

/** Stored or supplied data could not be decoded. */
extern PyObject* MalformedErrorType;

/** An allocation could not be satisfied, or a design ceiling was reached. */
extern PyObject* CapacityErrorType;

/** A file, directory or device could not be reached, or the engine failed outright. */
extern PyObject* StorageErrorType;

/**
 *  @brief The one arena a database lends to the verb in flight.
 *
 *  A verb claims the arena, empties the slot, and refills it on the way out unless the result it
 *  returns took the arena with it. A claim that finds the slot empty makes an arena of its own, so
 *  a second thread is never handed the arena a first one is writing into.
 */
typedef struct ustore_py_arena_pool_t {

    /** The arena no verb is using, or NULL while one holds it. */
    ustore_arena_t idle;
} ustore_py_arena_pool_t;

/** An open database, the arena it lends and the capabilities its engine reports. */
typedef struct Database {

    /** The object header every instance carries. */
    PyObject ob_base;

    /** The open database, or NULL once it has been closed. */
    ustore_database_t handle;

    /** The arena this database lends to one verb at a time. */
    ustore_py_arena_pool_t arenas;

    /** What the engine supports, described once at open. */
    ustore_capabilities_t capabilities;

    /** Transactions still holding this database, which must reach zero before it closes. */
    Py_ssize_t transactions;

    /** Snapshots still pinning this database, which must reach zero before it closes. */
    Py_ssize_t snapshots;
} Database;

/** An optimistic transaction grouping operations across collections and modalities. */
typedef struct Transaction {

    /** The object header every instance carries. */
    PyObject ob_base;

    /** The open transaction, or NULL once it has been closed. */
    ustore_transaction_t handle;

    /** The database this transaction runs against, kept alive for its whole life. */
    PyObject* database;

    /** Whether the verbs record the keys they touch, unless one call says otherwise. */
    ustore_watch_t watch;
} Transaction;

/** A point-in-time view of the whole database, which a read names through its `snapshot` keyword. */
typedef struct Snapshot {

    /** The object header every instance carries. */
    PyObject ob_base;

    /** The pinned view, or NULL once it has been closed. */
    ustore_snapshot_t handle;

    /** The database this snapshot reads, kept alive for its whole life. */
    PyObject* database;
} Snapshot;

/** The database and the transaction one verb runs against, as the object it was reached through names them. */
typedef struct ustore_py_context_t {

    /** The database the verb calls into. */
    Database* database;

    /** The transaction the verb joins, or NULL for the head. */
    ustore_transaction_t transaction;

    /** Whether the transaction records the keys its verbs touch. */
    ustore_watch_t watch;
} ustore_py_context_t;

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

/** Whether one borrowed run repeats for every task, or every task carries its own. */
typedef enum ustore_py_contents_layout_t {

    /** One run per task, which the ABI walks with a stride of one entry. */
    ustore_py_contents_per_task_k = 0,

    /** One run for every task, which the ABI repeats with a stride of zero. */
    ustore_py_contents_broadcast_k = 1,
} ustore_py_contents_layout_t;

/**
 *  @brief The borrowed byte runs one write batch hands the ABI.
 *
 *  Every value is borrowed rather than copied, so the buffers stay exported for the whole call and
 *  are released together once it returns. A value is never @c None, since removals take a verb of
 *  their own.
 */
typedef struct ustore_py_contents_t {

    /** Pointer to the bytes of every task. */
    ustore_bytes_cptr_t* values;

    /** Length of the bytes of every task. */
    ustore_length_t* lengths;

    /** One exported buffer per task, in the order they were borrowed. */
    Py_buffer* buffers;

    /** Tasks the batch holds. */
    size_t count;

    /** Buffers already exported, so a failure releases exactly those. */
    size_t borrowed;

    /** Whether the runs repeat for every task or stand one per task. */
    ustore_py_contents_layout_t layout;

    /** The one heap block the three runs share, or NULL when they sit inline. */
    void* block;

    /** Storage for the pointers of a batch that fits without the heap. */
    ustore_bytes_cptr_t inline_values[ustore_py_contents_inline_k];

    /** Storage for the lengths of a batch that fits without the heap. */
    ustore_length_t inline_lengths[ustore_py_contents_inline_k];

    /** Storage for the buffers of a batch that fits without the heap. */
    Py_buffer inline_buffers[ustore_py_contents_inline_k];
} ustore_py_contents_t;

/**
 *  @brief One tape of values, read back off the arena that holds it, with its offsets and presences in tow.
 *
 *  The object owns the arena, exports the tape through the buffer protocol, and slices one task's
 *  bytes out of it without copying. Its trailing storage holds the @c count + 1 offsets and then the
 *  presence bits the read wrote, so they live exactly as long as the tape. A @c memoryview taken
 *  here keeps this object alive, so the arena outlives every view of it.
 */
typedef struct Values {

    /** The object header every instance carries, sized in bytes of trailing storage. */
    PyVarObject ob_base;

    /** The arena holding the tape, freed when this object dies. */
    ustore_arena_t arena;

    /** The bytes of every value, laid end to end. */
    ustore_byte_t* tape;

    /** One bit per task marking the present keys, in the trailing storage past @c offsets. */
    ustore_octet_t* presences;

    /** Tasks the read carried. */
    size_t count;

    /** The @c Keys a sample drew, one per value, or NULL for a read that was handed its keys. */
    PyObject* keys;

    /** One offset into @c tape per task, plus one closing the last value. */
    ustore_length_t offsets[];
} Values;

/**
 *  @brief The keys one batch scan or sample found, held in the object's own trailing storage.
 *
 *  A scan and a sample answer the same shape: a run of keys per task, laid end to end, with the
 *  @c count + 1 boundaries in front of them, sized for every key the limits allow.
 */
typedef struct Keys {

    /** The object header every instance carries, sized in bytes of trailing storage. */
    PyVarObject ob_base;

    /** The keys every task found, laid end to end in the trailing storage past @c offsets. */
    ustore_key_t* tape;

    /** Tasks the scan or sample carried. */
    size_t count;

    /** The shape the whole tape exports: keys, then the sixteen bytes of each. */
    Py_ssize_t shape[2];

    /** The strides matching @c shape. */
    Py_ssize_t strides[2];

    /** One offset into @c tape per task, plus one closing the last run. */
    ustore_length_t offsets[];
} Keys;

/**
 *  @brief The typed columns one gather exported into the object's own trailing storage.
 *
 *  Every column holds one entry per document with three bitmaps beside it; a string column holds
 *  offsets and lengths into the strings every column shares, which alone land in the arena this
 *  object owns on the same terms a tape of values does.
 */
typedef struct Columns {

    /** The object header every instance carries, sized in bytes of trailing storage. */
    PyVarObject ob_base;

    /** The arena holding the joined strings, freed when this object dies. */
    ustore_arena_t arena;

    /** One bitmap per field, marking the documents whose value was found and converted. */
    ustore_octet_t** validities;

    /** One bitmap per field, marking the values whose conversion changed their meaning. */
    ustore_octet_t** conversions;

    /** One bitmap per field, marking the values of a type no conversion reaches. */
    ustore_octet_t** collisions;

    /** One run of fixed-width scalars per field, NULL for a string field. */
    ustore_byte_t** scalars;

    /** One run of offsets into @c joined per field, NULL for a scalar field. */
    ustore_length_t** offsets;

    /** One run of string lengths per field, NULL for a scalar field. */
    ustore_length_t** lengths;

    /** The contents of every string, joined document by document. */
    ustore_byte_t* joined;

    /** The type every field was converted into. */
    ustore_doc_field_type_t* types;

    /** Fields the gather carried, the columns of the result. */
    size_t fields_count;

    /** Documents the gather carried, the rows of every column. */
    size_t docs_count;

    /** The pointer runs above, then every column, the types and the bitmaps, carved at eight-byte boundaries. */
    uint64_t storage[];
} Columns;

/**
 *  @brief The edges one batch of vertices connects to, read back off the arena that holds them.
 *
 *  Every edge is three keys - source, target and edge ID - laid end to end vertex after vertex,
 *  so the whole tape exports as an array of shape (edges, 3, 16) bytes. The object owns the arena on
 *  the same terms a tape of values does, and its trailing storage holds the edge offsets and then
 *  the degrees.
 */
typedef struct Edges {

    /** The object header every instance carries, sized in bytes of trailing storage. */
    PyVarObject ob_base;

    /** The arena holding the tape, freed when this object dies. */
    ustore_arena_t arena;

    /** Three keys per edge, laid end to end vertex after vertex. */
    ustore_key_t* tape;

    /** The degree of every vertex, or @c ustore_vertex_degree_missing_k where it is absent, past @c offsets. */
    ustore_vertex_degree_t* degrees;

    /** Vertices the lookup carried. */
    size_t count;

    /** The shape the whole tape exports: edges, the three keys of each, and the sixteen bytes of a key. */
    Py_ssize_t shape[3];

    /** The strides matching @c shape. */
    Py_ssize_t strides[3];

    /** One edge offset per vertex plus one closing the tape. */
    size_t offsets[];
} Edges;

/**
 *  @brief A read-only window into memory another object owns, which a @c memoryview is built over.
 *
 *  A view of one task carries its own shape and format while keeping the result that owns the
 *  arena alive, so no slice or cast is needed and an empty run still exports its shape.
 */
typedef struct Span {

    /** The object header every instance carries. */
    PyObject ob_base;

    /** The result owning the memory, kept alive for as long as this window is. */
    PyObject* owner;

    /** The first byte of the window. */
    void* data;

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
int ustore_py_raise_on_outcome(ustore_outcome_t const* outcome);

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
 *  @brief Takes the idle arena of @p database, or NULL to have the engine make one.
 *
 *  A NULL arena is what the ABI expects of a first call, so an empty slot needs no allocation of
 *  our own; the engine grows one and the verb returns it to the slot afterwards.
 *
 *  @param[inout] database The database lending the arena.
 *  @return The arena to pass along, which may be NULL.
 */
ustore_arena_t ustore_py_arena_claim(Database* database);

/**
 *  @brief Gives @p arena back to @p database, or frees it when the slot is already full.
 *  @param[inout] database The database the arena came from.
 *  @param[inout] arena The arena the verb is done with.
 */
void ustore_py_arena_return(Database* database, ustore_arena_t arena);

#pragma endregion Arenas

#pragma region Verb Options

/** The view and options every verb accepts beside its own arguments. */
typedef struct ustore_py_verb_options_t {

    /** The collection the tasks address. */
    ustore_collection_id_t collection;

    /** What a read sees: the head, the transaction of the context, or a snapshot. */
    ustore_view_t view;

    /** The snapshot the `snapshot` keyword names, at which collection names resolve, or NULL for the head. */
    ustore_snapshot_t snapshot;

    /** Whether the transaction records the keys this call touches. */
    ustore_watch_t watch;

    /** How durable the writes are once a call at the head completes. */
    ustore_durability_t durability;
} ustore_py_verb_options_t;

/** Starts the shared options from @p context, computing its view, so a verb only overrides what a keyword names. */
void ustore_py_verb_options_init(ustore_py_context_t const* context, ustore_py_verb_options_t* options);

/**
 *  @brief Reads one of the keywords every verb shares, leaving the rest to the caller.
 *  @param[in] context The database and view the verb runs against.
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
 *  @brief Fills @p context from a @c Database or a @c Transaction.
 *  @param[in] self The object the verb was reached through.
 *  @param[out] context The database, transaction and watch the verb runs against.
 *  @return 0 on success, -1 with an exception set when the handle is closed.
 */
int ustore_py_context_from_object(PyObject* self, ustore_py_context_t* context);

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
 *  @brief Releases whatever @c ustore_py_keys_from_object acquired.
 *  @param[inout] batch The batch to release.
 */
void ustore_py_keys_release(ustore_py_keys_t* batch);

/**
 *  @brief Borrows the bytes of one value.
 *  @param[in] object The value, as anything exporting a buffer.
 *  @param[out] batch The batch the borrowed run joins.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
int ustore_py_contents_borrow(PyObject* object, ustore_py_contents_t* batch);

/**
 *  @brief Sizes @p batch for @p count tasks, laying them inline or in one heap block.
 *  @param[out] batch The batch to prepare.
 *  @param[in] count Tasks the batch will hold.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
int ustore_py_contents_reserve(ustore_py_contents_t* batch, size_t count);

/**
 *  @brief Releases every buffer @p batch borrowed and the block they shared.
 *  @param[inout] batch The batch to release.
 */
void ustore_py_contents_release(ustore_py_contents_t* batch);

/**
 *  @brief Allocates a @c Values whose trailing storage a read of @p count tasks writes its offsets and presences
 *      into, before the read runs; the read then hands the result its tape and the arena holding it.
 *  @param[in] count Tasks the read carries.
 *  @return A new reference with no arena and an empty first offset, or NULL with an exception set.
 */
Values* ustore_py_values_adopt(size_t count);

/** Whether task @p task_index of @p values found its key, as the presence bits address it, least significant first. */
int ustore_py_values_present(Values const* values, size_t task_index);

/**
 *  @brief Allocates a @c Keys whose trailing storage a scan or sample of @p count tasks writes into.
 *  @param[in] count Tasks the scan or sample carries.
 *  @param[in] capacity Keys every task may find together, the sum of their limits.
 *  @return A new reference with an empty first offset, or NULL with an exception set.
 */
Keys* ustore_py_keys_adopt(size_t count, size_t capacity);

/**
 *  @brief Builds a read-only @c memoryview over memory @p owner keeps alive.
 *  @param[in] owner The result owning the memory, which the view holds a reference to.
 *  @param[in] data The first byte of the window.
 *  @param[in] format The struct-module format of one item.
 *  @param[in] itemsize Bytes one item takes.
 *  @param[in] ndim Dimensions the window has, one or two.
 *  @param[in] shape Items along every dimension.
 *  @return A new reference, or NULL with an exception set.
 */
PyObject* ustore_py_span_view(PyObject* owner, void* data, char const* format, Py_ssize_t itemsize, int ndim,
                              Py_ssize_t const* shape);

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
int ustore_py_buffer_fill(Py_buffer* view, PyObject* owner, void* data, char const* format, Py_ssize_t itemsize,
                          int ndim, Py_ssize_t* shape, Py_ssize_t* strides, int flags);

/**
 *  @brief Reads a non-negative count from an object CPython can index.
 *  @param[in] object The count.
 *  @param[in] field The keyword to name in a failure.
 *  @param[out] value The count read.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
int ustore_py_size_from_object(PyObject* object, char const* field, size_t* value);

/** One spelling of a closed-set option and the value it selects, closed by a row with no name. */
typedef struct ustore_py_choice_naming_t {

    /** The keyword's spelling, as a caller writes it, or NULL closing the table. */
    char const* name;

    /** The enumerator the spelling selects. */
    int value;
} ustore_py_choice_naming_t;

/**
 *  @brief Reads one of a closed set of names into the value it selects.
 *  @param[in] object The name, as a string.
 *  @param[in] field The keyword to name in a failure.
 *  @param[in] namings The spellings and the enumerators they select, closed by a nameless row.
 *  @param[out] value The enumerator the name selected.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
int ustore_py_choice_from_object(PyObject* object, char const* field, ustore_py_choice_naming_t const* namings,
                                 int* value);

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
 *  @brief Reads the collection a name was given at the head, or at @p snapshot when it is set.
 *  @param[in] context The database to resolve against.
 *  @param[in] snapshot The snapshot whose names are read, or NULL for the head.
 *  @param[in] name The name's bytes, which need no null terminator.
 *  @param[in] length The name's length in bytes.
 *  @param[out] collection The identifier the name addresses, untouched unless the name was found.
 *  @param[out] modality What the collection holds, or NULL when the caller does not ask.
 *  @return Whether the name was found, missing, or could not be looked up.
 */
ustore_py_naming_t ustore_py_collection_named(ustore_py_context_t const* context, ustore_snapshot_t snapshot,
                                              char const* name, size_t length, ustore_collection_id_t* collection,
                                              ustore_modality_t* modality);

/**
 *  @brief Reads the collection an argument names, accepting an ID, a name, or @c None.
 *  @param[in] context The database to resolve a name against.
 *  @param[in] snapshot The snapshot whose names are read, or NULL for the head.
 *  @param[in] object The collection, as a `uuid.UUID` ID, a string name, or @c None for the main one.
 *  @param[out] collection The identifier the argument addresses.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
int ustore_py_collection_from_object(ustore_py_context_t const* context, ustore_snapshot_t snapshot, PyObject* object,
                                     ustore_collection_id_t* collection);

/**
 *  @brief Lists the names of collections at the head in the order of their text, up to @p limit of them.
 *  @param[in] context The database to list.
 *  @param[in] after The smallest name listed, or NULL to start at the first one.
 *  @param[in] after_length The length of @p after in bytes.
 *  @param[in] limit Most names the walk returns.
 *  @return A new tuple of names, or NULL with an exception set.
 */
PyObject* ustore_py_collection_names(ustore_py_context_t const* context, char const* after, size_t after_length,
                                     size_t limit);

/** Every modality a collection holds, as a caller spells it, closed by a nameless row. */
extern ustore_py_choice_naming_t const ustore_py_modality_namings[];

/**
 *  @brief Answers the @c Collection a database or a transaction addresses by name, as `db['movies']` does.
 *  @param[in] view The @c Database or @c Transaction indexed.
 *  @param[in] name The collection's name, where the empty one is the main collection.
 *  @return A new reference, or NULL with an exception set.
 */
PyObject* ustore_py_collection_subscript(PyObject* view, PyObject* name);

#pragma endregion Names

#pragma region Verb Cores

/**
 *  @brief Reads a batch of values: the core of `read`, of `Collection.blobs[keys]` and of `Collection.sample`.
 *  @param[in] context The database and view the read runs against.
 *  @param[in] options The collection and flags the read carries.
 *  @param[in] keys The keys to read, NULL only when @p count is zero.
 *  @param[in] count Keys to read.
 *  @return A new @c Values, or NULL with an exception set.
 */
PyObject* ustore_py_values_read(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                                ustore_key_t const* keys, size_t count);

/**
 *  @brief Reads whether one key is present: the core of `contains` and of `key in Collection.blobs`.
 *  @return 1 when present, 0 when absent, or -1 with an exception set.
 */
int ustore_py_value_contains(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                             ustore_key_t const* key);

/**
 *  @brief Gathers the values of a batch write, broadcasting one value to every key.
 *  @param[in] object The values: a sequence, or one buffer to broadcast.
 *  @param[in] count Keys the batch carries.
 *  @param[out] batch The borrowed runs the ABI reads, and the layout they travel with.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
int ustore_py_contents_from_object(PyObject* object, size_t count, ustore_py_contents_t* batch);

/**
 *  @brief Upserts a batch of values: the core of `write`, `set` and `Collection.blobs[key] = value`.
 *  @param[in] contents The borrowed values.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
int ustore_py_values_write(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                           ustore_key_t const* keys, size_t count, ustore_py_contents_t const* contents);

/**
 *  @brief Removes a batch of keys with their values or documents: the core of `remove` and of `del` on every view.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
int ustore_py_values_remove(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                            ustore_key_t const* keys, size_t count);

/**
 *  @brief Lists up to @p count keys from @p start upwards: the core of `scan` and of `Collection.scan`.
 *  @return A new @c Keys of one task, or NULL with an exception set.
 */
PyObject* ustore_py_keys_scan(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                              ustore_key_t const* start, size_t count);

/**
 *  @brief Draws up to @p count keys at random: the core of `sample` and of `Collection.sample`.
 *  @return A new @c Keys of one task, or NULL with an exception set.
 */
PyObject* ustore_py_keys_sample(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                                size_t count);

/**
 *  @brief Reads documents, or one field of each: the core of `docs_read` and of `Collection.docs[key]`.
 *  @param[in] field A top-level key or a JSON Pointer, or NULL for whole documents.
 *  @param[in] type How every value is spelled, JSON text or a printed string.
 *  @return A new @c Values, or NULL with an exception set.
 */
PyObject* ustore_py_docs_read(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                              ustore_key_t const* keys, size_t count, char const* field, ustore_doc_field_type_t type);

/** How a write spells its documents. */
typedef enum ustore_py_documents_spelling_t {

    /** As `docs_write` reads them: JSON text or bytes, a dict or list to serialize, or a sequence of those. */
    ustore_py_documents_as_json_k = 0,

    /** As `Collection.docs` reads them: a sequence of one Python object per key, each serialized. */
    ustore_py_documents_as_objects_k = 1,
} ustore_py_documents_spelling_t;

/**
 *  @brief Upserts whole documents: the core of `Collection.docs[key] = document`.
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
extern char const doc_get[];
extern char const doc_set[];
extern char const doc_remove[];
extern char const doc_contains[];
extern char const doc_docs_write[];
extern char const doc_docs_read[];
extern char const doc_docs_gist[];
extern char const doc_docs_gather[];
extern char const doc_graph_find_edges[];
extern char const doc_graph_degrees[];
extern char const doc_graph_upsert_edges[];
extern char const doc_graph_remove_edges[];
extern char const doc_graph_upsert_vertices[];
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
PyObject* ustore_py_api_graph_find_edges(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                         PyObject* args_names_tuple);
PyObject* ustore_py_api_graph_degrees(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                      PyObject* args_names_tuple);
PyObject* ustore_py_api_graph_upsert_edges(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                           PyObject* args_names_tuple);
PyObject* ustore_py_api_graph_remove_edges(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                           PyObject* args_names_tuple);
PyObject* ustore_py_api_graph_upsert_vertices(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                              PyObject* args_names_tuple);
PyObject* ustore_py_api_graph_remove_vertices(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                              PyObject* args_names_tuple);

#pragma endregion Verbs

#ifdef __cplusplus
}
#endif

#endif // UNUM_USTORE_PYTHON_LIB_H
