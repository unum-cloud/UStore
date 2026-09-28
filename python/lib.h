/**
 *  @file python/lib.h
 *  @author Ash Vardanian
 *  @date September 16, 2026
 *  @brief Declarations shared by every UStore CPython translation unit.
 *
 *  One extension, one file per header of the C ABI: @c types.c owns the vocabulary every modality
 *  shares, @c database.c owns databases, collections, snapshots and transactions, @c blobs.c owns
 *  the binary verbs, @c docs.c the document verbs, @c graph.c the graph verbs, @c strings.c resolves
 *  names onto identifiers, and @c lib.c owns the module glue alone. A verb is written once and
 *  reached from both method tables, since a database and a transaction differ only in the view
 *  they address.
 *
 *  @section python_arena_ownership Arena Ownership
 *
 *  Results land in an arena the caller owns, and a later call on the same arena discards what an
 *  earlier one left there, so an arena is owned by exactly one live object at a time. A database
 *  keeps one idle arena; a verb claims it, runs, and either gives it back before returning or
 *  hands it to the result object it built. A claim that finds the slot empty makes an arena of its
 *  own, so two threads never share one. A result object frees its arena in @c tp_dealloc, and a
 *  @c memoryview over that object holds a reference to it, so no view outlives the bytes it reads
 *  and nothing can write into an arena a result owns.
 *
 *  @section python_calling_convention Latency and Arguments Parsing
 *
 *  No PyBind11, NanoBind or SWIG, only the CPython API, and never @c PyArg_ParseTupleAndKeywords,
 *  whose format specifiers are parsed at runtime and which cannot use the fast calling convention.
 *  Every verb is @c METH_FASTCALL | @c METH_KEYWORDS and walks its keywords by hand.
 *
 *  @section python_buffers Buffer Protocol and Batches
 *
 *  Keys and values arrive through PEP 3118 where the layout allows it, so a NumPy array of keys
 *  reaches the ABI without a copy and NumPy is never a build-time dependency. A batch small enough
 *  lays its strided arrays out on the stack, so the hot path allocates nothing beyond the objects
 *  CPython itself demands.
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

#include <ustore/ustore.h>

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

extern PyTypeObject DatabaseType;
extern PyTypeObject TransactionType;
extern PyTypeObject ValuesType;
extern PyTypeObject KeysType;
extern PyTypeObject ColumnsType;
extern PyTypeObject EdgesType;
extern PyTypeObject SpanType;

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

    /** What the engine supports, read once at open. */
    ustore_capabilities_t capabilities;

    /** Transactions still holding this database, which must reach zero before it closes. */
    Py_ssize_t transactions;
} Database;

/** An optimistic transaction grouping operations across collections and modalities. */
typedef struct Transaction {

    /** The object header every instance carries. */
    PyObject ob_base;

    /** The open transaction, or NULL once it has been closed. */
    ustore_transaction_t handle;

    /** The database this transaction runs against, kept alive for its whole life. */
    PyObject* database;

    /** Whether the transaction watches the keys it touches. */
    ustore_options_t options;
} Transaction;

/**
 *  @brief The database, the view and the arena one verb runs against.
 *
 *  A call addresses exactly one view, so a transaction and a snapshot are never both set. The
 *  arena belongs to @c database for the length of the verb and is returned or adopted before it
 *  ends.
 */
typedef struct ustore_py_context_t {

    /** The database the verb calls into. */
    Database* database;

    /** The transaction the verb joins, or NULL for the head. */
    ustore_transaction_t transaction;

    /** The snapshot the verb reads, or zero for the head. */
    ustore_snapshot_t snapshot;

    /** Options the view adds to the verb's own, such as the watch of a transaction. */
    ustore_options_t options;
} ustore_py_context_t;

/**
 *  @brief A batch of keys, and whatever storage gathering them needed.
 *
 *  A caller passing a contiguous buffer of 64-bit keys hands us the array outright and @c owned
 *  stays NULL; a caller passing a sequence of integers gets the inline run, or one allocation when
 *  the batch outgrows it.
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
 *  are released together once it returns. A task whose value is @c None removes its key, which the
 *  ABI spells as a null pointer.
 */
typedef struct ustore_py_contents_t {

    /** Pointer to the bytes of every task, NULL where the task removes its key. */
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
 *  @brief One tape of values, read back off the arena that holds it.
 *
 *  The object owns the arena, exports the tape through the buffer protocol, and slices one task's
 *  bytes out of it without copying. A @c memoryview taken here keeps this object alive, so the
 *  arena outlives every view of it.
 */
typedef struct Values {

    /** The object header every instance carries. */
    PyObject ob_base;

    /** The arena holding the tape, freed when this object dies. */
    ustore_arena_t arena;

    /** The bytes of every value, laid end to end. */
    ustore_byte_t* tape;

    /** One offset into @c tape per task, plus one closing the last value. */
    ustore_length_t* offsets;

    /** The length of every value, or @c ustore_length_missing_k where the key was absent. */
    ustore_length_t* lengths;

    /** Tasks the read carried. */
    size_t count;
} Values;

/**
 *  @brief One tape of keys, read back off the arena that holds it.
 *
 *  A scan and a sample answer the same shape: a run of keys per task, laid end to end, with the
 *  boundaries beside them. The object owns the arena on the same terms a tape of values does.
 */
typedef struct Keys {

    /** The object header every instance carries. */
    PyObject ob_base;

    /** The arena holding the tape, freed when this object dies. */
    ustore_arena_t arena;

    /** The keys every task found, laid end to end. */
    ustore_key_t* tape;

    /** One offset into @c tape per task, plus one closing the last run. */
    ustore_length_t* offsets;

    /** The number of keys every task found. */
    ustore_length_t* counts;

    /** Tasks the scan or sample carried. */
    size_t count;
} Keys;

/**
 *  @brief The typed columns one gather exported, read back off the arena that holds them.
 *
 *  Every column holds one entry per document with three bitmaps beside it; a string column holds
 *  offsets and lengths into the strings every column shares. The object owns the arena on the same
 *  terms a tape of values does.
 */
typedef struct Columns {

    /** The object header every instance carries. */
    PyObject ob_base;

    /** The arena holding every column, freed when this object dies. */
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

    /** The type every field was converted into, on the heap this object frees. */
    ustore_doc_field_type_t* types;

    /** Fields the gather carried, the columns of the result. */
    size_t fields_count;

    /** Documents the gather carried, the rows of every column. */
    size_t docs_count;
} Columns;

/**
 *  @brief The edges one batch of vertices connects to, read back off the arena that holds them.
 *
 *  Every edge is three keys - source, target and edge identifier - laid end to end vertex after
 *  vertex, so the whole tape exports as an array of shape (edges, 3). The object owns the arena on
 *  the same terms a tape of values does.
 */
typedef struct Edges {

    /** The object header every instance carries. */
    PyObject ob_base;

    /** The arena holding the tape, freed when this object dies. */
    ustore_arena_t arena;

    /** Three keys per edge, laid end to end vertex after vertex. */
    ustore_key_t* tape;

    /** The degree of every vertex, or @c ustore_vertex_degree_missing_k where it is absent. */
    ustore_vertex_degree_t* degrees;

    /** One edge offset per vertex plus one closing the tape, on the heap this object frees. */
    size_t* offsets;

    /** Vertices the lookup carried. */
    size_t count;

    /** The shape the whole tape exports: edges, then the three keys of each. */
    Py_ssize_t shape[2];

    /** The strides matching @c shape. */
    Py_ssize_t strides[2];
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
 *  @brief Raises the exception @p error 's status maps onto.
 *  @param[in] error The outcome of one call, whose message is static and never freed.
 *  @return -1 with an exception set when the call failed, 0 otherwise.
 */
int ustore_py_raise_on_error(ustore_error_t const* error);

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
    ustore_collection_t collection;

    /** The snapshot a read follows, or zero for the head. */
    ustore_snapshot_t snapshot;

    /** Flags the verb adds to the view's own. */
    ustore_options_t options;
} ustore_py_verb_options_t;

/** Starts the shared options from @p context, so a verb only overrides what a keyword names. */
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
 *  @param[out] context The database, view and options the verb runs against.
 *  @return 0 on success, -1 with an exception set when the handle is closed.
 */
int ustore_py_context_from_object(PyObject* self, ustore_py_context_t* context);

/**
 *  @brief Reads one key from an object CPython can index.
 *  @param[in] object The key, as an integer or anything that becomes one.
 *  @param[out] key The key the ABI addresses.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
int ustore_py_key_from_object(PyObject* object, ustore_key_t* key);

/**
 *  @brief Gathers a batch of keys from a buffer of 64-bit integers or a sequence.
 *  @param[in] object The keys, as a contiguous buffer or a sequence of integers.
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
 *  @brief Borrows the bytes of one value, or notes that the task removes its key.
 *  @param[in] object The value, as anything exporting a buffer, or @c None to remove.
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
 *  @brief Wraps a tape of values and the arena holding it into a @c Values.
 *  @param[inout] arena The arena the result adopts; it is freed on failure.
 *  @param[in] tape The bytes of every value, laid end to end.
 *  @param[in] offsets One offset per task, plus one closing the last value.
 *  @param[in] lengths The length of every value.
 *  @param[in] count Tasks the read carried.
 *  @return A new reference, or NULL with an exception set.
 */
PyObject* ustore_py_values_adopt(ustore_arena_t arena, ustore_byte_t* tape, ustore_length_t* offsets,
                                 ustore_length_t* lengths, size_t count);

/**
 *  @brief Wraps a tape of keys and the arena holding it into a @c Keys.
 *  @param[inout] arena The arena the result adopts; it is freed on failure.
 *  @param[in] tape The keys every task found, laid end to end.
 *  @param[in] offsets One offset per task, plus one closing the last run.
 *  @param[in] counts The number of keys every task found.
 *  @param[in] count Tasks the scan or sample carried.
 *  @return A new reference, or NULL with an exception set.
 */
PyObject* ustore_py_keys_adopt(ustore_arena_t arena, ustore_key_t* tape, ustore_length_t* offsets,
                               ustore_length_t* counts, size_t count);

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
 *  @brief Reads the collection a name was given, at the head or at the snapshot of @p context.
 *  @param[in] context The database and view to resolve against.
 *  @param[in] name The name's bytes, which need no null terminator.
 *  @param[in] length The name's length in bytes.
 *  @param[out] collection The identifier the name addresses, untouched unless the name was found.
 *  @return Whether the name was found, missing, or could not be looked up.
 */
ustore_py_naming_t ustore_py_collection_named(ustore_py_context_t const* context, char const* name, size_t length,
                                              ustore_collection_t* collection);

/**
 *  @brief Reads the collection an argument names, accepting an identifier, a name, or @c None.
 *  @param[in] context The database and view to resolve a name against.
 *  @param[in] object The collection, as an integer identifier, a string name, or @c None for the main one.
 *  @param[out] collection The identifier the argument addresses.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
int ustore_py_collection_from_object(ustore_py_context_t const* context, PyObject* object,
                                     ustore_collection_t* collection);

/**
 *  @brief Lists the names of collections in the order of their text, up to @p limit of them.
 *  @param[in] context The database and view to list.
 *  @param[in] after The smallest name listed, or NULL to start at the first one.
 *  @param[in] after_length The length of @p after in bytes.
 *  @param[in] limit Most names the walk returns.
 *  @return A new tuple of names, or NULL with an exception set.
 */
PyObject* ustore_py_collection_names(ustore_py_context_t const* context, char const* after, size_t after_length,
                                     size_t limit);

#pragma endregion Names

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
extern char const doc_pop[];
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
PyObject* ustore_py_api_pop(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
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
