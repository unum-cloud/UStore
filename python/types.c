/**
 *  @file python/types.c
 *  @author Ash Vardanian
 *  @date September 16, 2026
 *  @brief The vocabulary every modality shares: failures, arenas, batches, result tapes, executors and deferred
 *      calls.
 *
 *  Keys, values and the arenas holding them live here rather than inside any one modality, so a
 *  tape a binary read exported is the very object a later verb can slice, because there is one
 *  definition and one address. Every call runs through one driver here too, which keeps a blocking
 *  call on the stack and a deferred one inside its @c Deferred.
 */
#include "lib.h"

#pragma region Failures

/** The failure every other one specializes, subclassing @c RuntimeError. */
static PyObject* ErrorType = NULL;

/** The one base of the three conflicts, which a retry loop catches. */
static PyObject* ConflictErrorType = NULL;

/** The class each failing status raises, indexed by status; success and deferred never raise. */
static PyObject* status_error_types[ustore_status_capacity_exhausted_k + 1];

/** One failing status, the dotted name of the class it raises, the bases it derives, and its doc. */
static struct {
    ustore_status_t status;
    char const* name;

    /** @c ErrorType, or @c ConflictErrorType for the three conflicts. */
    PyObject** base;

    /** The built-in naming the same fault, or NULL when none does. */
    PyObject** builtin;
    char const* doc;
} const status_errors[] = {
    {ustore_status_out_of_memory_k, "ustore.OutOfMemoryError", &ErrorType, &PyExc_MemoryError,
     "An allocation could not be satisfied."},
    {ustore_status_read_only_k, "ustore.ReadOnlyError", &ErrorType, NULL,
     "The call would change a database opened read-only, as Database.view_rocksdb opens one."},
    {ustore_status_io_k, "ustore.InputOutputError", &ErrorType, &PyExc_OSError,
     "A file or directory could not be opened, read, written, or closed."},
    {ustore_status_device_k, "ustore.DeviceError", &ErrorType, NULL,
     "A device could not be selected or claimed, or the call's executor, database and arena live apart."},
    {ustore_status_malformed_k, "ustore.MalformedError", &ErrorType, &PyExc_ValueError,
     "Stored or supplied data could not be decoded, like a corrupted file or an invalid document."},
    {ustore_status_internal_k, "ustore.InternalError", &ErrorType, NULL,
     "The engine failed in a way no other status names, which points at a defect."},
    {ustore_status_uninitialized_k, "ustore.UninitializedError", &ErrorType, NULL,
     "The call needs a handle that is closed or was never opened, or reads a result a later call rewound."},
    {ustore_status_contended_k, "ustore.ContendedError", &ErrorType, NULL,
     "A full deferred table, or an arena or transaction serving another call, refused the call instead of waiting."},
    {ustore_status_cancelled_k, "ustore.CancelledError", &ErrorType, NULL,
     "A progress callback stopped the call, which unwound and published nothing."},
    {ustore_status_unsupported_k, "ustore.UnsupportedError", &ErrorType, &PyExc_NotImplementedError,
     "The engine or this build does not implement what the call asks for."},
    {ustore_status_invalid_argument_k, "ustore.InvalidArgumentError", &ErrorType, &PyExc_ValueError,
     "An argument is null, contradicts another one, or carries a choice the call does not accept."},
    {ustore_status_not_found_k, "ustore.NotFoundError", &ErrorType, &PyExc_KeyError,
     "A collection, a directory, or a document a modification needs does not exist."},
    {ustore_status_already_exists_k, "ustore.AlreadyExistsError", &ErrorType, &PyExc_ValueError,
     "A collection name or a document the call creates is already taken."},
    {ustore_status_write_conflict_k, "ustore.WriteConflictError", &ConflictErrorType, NULL,
     "A concurrent writer changed a key this transaction wrote; retry the transaction."},
    {ustore_status_read_conflict_k, "ustore.ReadConflictError", &ConflictErrorType, NULL,
     "A concurrent writer changed a key this transaction read; retry the transaction."},
    {ustore_status_phantom_conflict_k, "ustore.PhantomConflictError", &ConflictErrorType, NULL,
     "A concurrent writer inserted into a range this transaction scanned; retry the transaction."},
    {ustore_status_capacity_exhausted_k, "ustore.CapacityExhaustedError", &ErrorType, &PyExc_OverflowError,
     "A hard design ceiling was reached."},
};

int ustore_py_raise(ustore_status_t status, char const* format, ...) {
    // A status this specific supersedes whatever the caller was carrying, and building the
    // instance below must not start with an exception already set.
    PyErr_Clear();
    PyObject* type = (size_t)status < ustore_py_table_size_(status_error_types) ? status_error_types[status] : NULL;
    if (!type) {
        status = ustore_status_internal_k;
        type = status_error_types[status];
    }

    va_list arguments;
    va_start(arguments, format);
    PyObject* const message = PyUnicode_FromFormatV(format ? format : "", arguments);
    va_end(arguments);
    if (!message) return -1;

    PyObject* const instance = PyObject_CallOneArg(type, message);
    Py_DECREF(message);
    if (!instance) return -1;

    // A status-less failure would misreport the fault, so a failure here is reported as itself
    // rather than papered over with a half-built instance.
    PyObject* const member = ustore_py_enum_to_object(ustore_py_enum_status_k, (int)status);
    int const attached = member ? PyObject_SetAttrString(instance, "status", member) : -1;
    Py_XDECREF(member);
    if (attached != 0) {
        Py_DECREF(instance);
        return -1;
    }
    PyErr_SetRaisedException(instance);
    return -1;
}

int ustore_py_raise_on_outcome(ustore_outcome_t* outcome) {
    ustore_status_t const status = ustore_outcome_status(outcome);
    if (status == ustore_status_success_k) return 0;
    // The message is static and never freed, so it is handed to the formatter as a `%s` argument.
    return ustore_py_raise(status, "%s", outcome->message ? outcome->message : "the engine named no detail");
}

int ustore_py_raise_from_cause(PyObject* kind, char const* format, ...) {
    PyObject* const cause = PyErr_GetRaisedException();
    va_list arguments;
    va_start(arguments, format);
    PyObject* const message = PyUnicode_FromFormatV(format, arguments);
    va_end(arguments);
    if (!message) {
        Py_XDECREF(cause);
        return -1;
    }

    PyErr_SetObject(kind, message);
    Py_DECREF(message);
    if (cause) {
        // Both setters steal, so the cause needs a second reference before it is handed over twice.
        PyObject* const raised = PyErr_GetRaisedException();
        PyException_SetCause(raised, Py_NewRef(cause));
        PyException_SetContext(raised, cause);
        PyErr_SetRaisedException(raised);
    }
    return -1;
}

static char const doc_Error[] =                                                                 //
    "Every failure the engine reports, carrying the status shared with the C ABI.\n"            //
    "\n"                                                                                        //
    "Each failing status raises a subclass of its own, such as `NotFoundError`, which also\n"   //
    "derives the built-in naming the same fault where one exists, so `except KeyError` still\n" //
    "catches a `NotFoundError`. The three conflicts share `ConflictError`, the one base a\n"    //
    "retry loop catches. A failure caught before the boundary raises the class the engine\n"    //
    "would have raised for the same fault.\n"                                                   //
    "\n"                                                                                        //
    "Attributes:\n"                                                                             //
    "  status (Status): The status, such as `Status.WRITE_CONFLICT`.\n"                         //
    "  args (tuple): The detail the status cannot carry, as on any exception.";

static char const doc_ConflictError[] =
    "A concurrent writer touched what this transaction wrote, read or scanned; retry the transaction.";

int ustore_py_register_errors(PyObject* module) {
    ErrorType = PyErr_NewExceptionWithDoc("ustore.Error", (char*)doc_Error, PyExc_RuntimeError, NULL);
    if (!ErrorType || PyModule_AddObjectRef(module, "Error", ErrorType) < 0) return -1;
    ConflictErrorType = PyErr_NewExceptionWithDoc("ustore.ConflictError", (char*)doc_ConflictError, ErrorType, NULL);
    if (!ConflictErrorType || PyModule_AddObjectRef(module, "ConflictError", ConflictErrorType) < 0) return -1;

    for (size_t row = 0; row != ustore_py_table_size_(status_errors); ++row) {
        PyObject* const bases = status_errors[row].builtin
                                    ? PyTuple_Pack(2, *status_errors[row].base, *status_errors[row].builtin)
                                    : Py_NewRef(*status_errors[row].base);
        if (!bases) return -1;
        PyObject* const type = PyErr_NewExceptionWithDoc(status_errors[row].name, (char*)status_errors[row].doc, bases,
                                                         NULL);
        Py_DECREF(bases);
        if (!type) return -1;
        status_error_types[status_errors[row].status] = type;
        if (PyModule_AddObjectRef(module, status_errors[row].name + sizeof("ustore.") - 1, type) < 0) return -1;
    }
    return 0;
}

#pragma endregion Failures

#pragma region Arenas

static char const doc_Arena[] =                                                                  //
    "Arena(executor=None, /)\n"                                                                  //
    "\n"                                                                                         //
    "The memory every output whose length scales with a call lands in, from values and keys\n"   //
    "to columns, edges and names. Every such verb takes one as its last positional argument\n"   //
    "and rewinds it, so a steady loop allocates once.\n"                                         //
    "\n"                                                                                         //
    "The executor makes it where its calls compute, and none makes host pages for calls on\n"    //
    "the calling thread. Results are zero-copy views over it. Like a `bytearray`, it refuses\n"  //
    "to rewind with `ContendedError` while a buffer over its outputs is exported, and a\n"       //
    "result read after a later call rewound it raises `UninitializedError`. A deferred call\n"   //
    "holds it as a blocking one does, until it is waited on or dropped, so each deferred call\n" //
    "needs one of its own. Calls `ustore_arena_init`.\n"                                         //
    "\n"                                                                                         //
    "Args:\n"                                                                                    //
    "  executor (Executor, optional): The executor whose calls it serves; None serves the\n"     //
    "    calling thread.";

/** A new arena of @p type made where @p executor computes, or NULL with an exception set. */
static PyObject* arena_made(PyTypeObject* type, ustore_executor_t executor) {
    ustore_arena_t handle = NULL;
    ustore_outcome_t outcome = {0};
    ustore_arena_init_t init = {.arena = &handle, .executor = executor, .outcome = &outcome};
    ustore_arena_init(&init);
    if (ustore_py_raise_on_outcome(&outcome) != 0) return NULL;
    Arena* const arena = (Arena*)type->tp_alloc(type, 0);
    if (!arena) {
        ustore_arena_free(handle);
        return NULL;
    }
    arena->handle = handle;
    return (PyObject*)arena;
}

PyObject* ustore_py_arena_new(ustore_executor_t executor) { return arena_made(&ArenaType, executor); }

static PyObject* Arena_tp_new(PyTypeObject* type, PyObject* args, PyObject* keywords) {
    Py_ssize_t const positional_args_count = PyTuple_GET_SIZE(args);
    if (positional_args_count > 1 || (keywords && PyDict_GET_SIZE(keywords) != 0)) {
        PyErr_SetString(PyExc_TypeError, "Arena() takes one optional positional argument, the executor");
        return NULL;
    }
    PyObject* const executor_object = positional_args_count ? PyTuple_GET_ITEM(args, 0) : Py_None;
    if (executor_object == Py_None) return arena_made(type, NULL);
    if (!PyObject_TypeCheck(executor_object, &ExecutorType)) {
        PyErr_Format(PyExc_TypeError, "Expected an Executor or None, got %S", Py_TYPE(executor_object));
        return NULL;
    }
    ustore_executor_t const executor = ((Executor*)executor_object)->handle;
    if (executor) return arena_made(type, executor);
    ustore_py_raise(ustore_status_uninitialized_k, "The executor is closed");
    return NULL;
}

static void Arena_dealloc(Arena* self) {
    Py_BEGIN_ALLOW_THREADS;
    ustore_arena_free(self->handle);
    Py_END_ALLOW_THREADS;
    Py_TYPE(self)->tp_free((PyObject*)self);
}

PyTypeObject ArenaType = {
    PyVarObject_HEAD_INIT(NULL, 0) //
        .tp_name = "ustore.Arena",
    .tp_doc = doc_Arena,
    .tp_basicsize = sizeof(Arena),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = Arena_tp_new,
    .tp_dealloc = (destructor)Arena_dealloc,
};

int ustore_py_arena_claim(PyObject* object, PyObject** arena) {
    if (!PyObject_TypeCheck(object, &ArenaType)) {
        PyErr_Format(PyExc_TypeError, "The arena must be an Arena, got %S", Py_TYPE(object));
        return -1;
    }
    Arena* const claimed = (Arena*)object;
    Py_ssize_t held = 0;
    ustore_py_critical_section_begin_(object);
    held = claimed->exports;
    if (!held) {
        claimed->exports = 1;
        ++claimed->generation;
    }
    ustore_py_critical_section_end_();
    if (held)
        return ustore_py_raise(ustore_status_contended_k,
                               "The arena serves a call or exports a buffer over its outputs; release it first");
    *arena = Py_NewRef(object);
    return 0;
}

void ustore_py_arena_unclaim(PyObject* arena) {
    ustore_py_critical_section_begin_(arena);
    --((Arena*)arena)->exports;
    ustore_py_critical_section_end_();
    Py_DECREF(arena);
}

ustore_arena_t ustore_py_arena_handle(PyObject* arena) { return ((Arena*)arena)->handle; }

size_t ustore_py_arena_generation(PyObject* arena) {
    size_t generation = 0;
    ustore_py_critical_section_begin_(arena);
    generation = ((Arena*)arena)->generation;
    ustore_py_critical_section_end_();
    return generation;
}

int ustore_py_arena_pin(PyObject* arena, size_t generation) {
    Arena* const pinned = (Arena*)arena;
    int fresh = 0;
    ustore_py_critical_section_begin_(arena);
    fresh = pinned->generation == generation;
    if (fresh) ++pinned->exports;
    ustore_py_critical_section_end_();
    if (fresh) return 0;
    return ustore_py_raise(ustore_status_uninitialized_k, "A later call rewound the arena this output lived in");
}

void ustore_py_arena_unpin(PyObject* arena) {
    ustore_py_critical_section_begin_(arena);
    --((Arena*)arena)->exports;
    ustore_py_critical_section_end_();
}

#pragma endregion Arenas

#pragma region Keys

PyObject* UUIDType = NULL;

/** Writes @p high then @p low big-endian into @p uuid, which is how an integer spells as a UUID. */
static void uuid_from_halves(unsigned long long high, unsigned long long low, ustore_uuid_t* uuid) {
    for (int byte_index = 7; byte_index >= 0; --byte_index, high >>= 8, low >>= 8) {
        uuid->bytes[byte_index] = (uint8_t)(high & 0xFF);
        uuid->bytes[byte_index + 8] = (uint8_t)(low & 0xFF);
    }
}

/** Reads a non-negative integer below 2**128 into @p uuid, as `uuid.UUID(int=n)` spells it. */
static int uuid_from_integer(PyObject* object, ustore_uuid_t* uuid) {
    PyObject* const integer = PyNumber_Index(object);
    if (!integer) return -1;
    unsigned long long low = PyLong_AsUnsignedLongLong(integer);
    unsigned long long high = 0;
    if (low == (unsigned long long)-1 && PyErr_Occurred()) {
        if (!PyErr_ExceptionMatches(PyExc_OverflowError)) {
            Py_DECREF(integer);
            return -1;
        }
        // Past 64 bits or negative, and a negative integer keeps a negative upper half, which refuses too
        PyErr_Clear();
        PyObject* const shift = PyLong_FromLong(64);
        PyObject* const upper = shift ? PyNumber_Rshift(integer, shift) : NULL;
        Py_XDECREF(shift);
        high = upper ? PyLong_AsUnsignedLongLong(upper) : (unsigned long long)-1;
        Py_XDECREF(upper);
        if (high == (unsigned long long)-1 && PyErr_Occurred()) {
            Py_DECREF(integer);
            return ustore_py_raise_from_cause(PyExc_OverflowError, "A key must be an integer in [0, 2**128)");
        }
        low = PyLong_AsUnsignedLongLongMask(integer);
    }
    Py_DECREF(integer);
    if (low == (unsigned long long)-1 && PyErr_Occurred()) return -1;
    uuid_from_halves(high, low, uuid);
    return 0;
}

/** Copies the sixteen bytes a `uuid.UUID` holds, which its `bytes` attribute spells big-endian. */
static int uuid_from_uuid(PyObject* object, ustore_uuid_t* uuid) {
    PyObject* const bytes = PyObject_GetAttrString(object, "bytes");
    if (!bytes) return -1;
    int const sized = PyBytes_Check(bytes) && PyBytes_GET_SIZE(bytes) == (Py_ssize_t)sizeof(uuid->bytes);
    if (sized) memcpy(uuid->bytes, PyBytes_AS_STRING(bytes), sizeof(uuid->bytes));
    Py_DECREF(bytes);
    if (!sized) PyErr_SetString(PyExc_TypeError, "A UUID must hold sixteen bytes");
    return sized ? 0 : -1;
}

int ustore_py_uuid_from_object(PyObject* object, ustore_uuid_t* uuid) {
    if (PyObject_TypeCheck(object, (PyTypeObject*)UUIDType)) return uuid_from_uuid(object, uuid);
    if (PyUnicode_Check(object)) {
        // `uuid.UUID` parses every spelling Python itself accepts, so the text is handed to it
        PyObject* const parsed = PyObject_CallOneArg(UUIDType, object);
        if (!parsed) return ustore_py_raise_from_cause(PyExc_ValueError, "Cannot read a UUID from %R", object);
        int const read = uuid_from_uuid(parsed, uuid);
        Py_DECREF(parsed);
        return read;
    }
    // `PyIndex_Check` rather than `PyLong_Check`, so a NumPy scalar - which is not a Python
    // integer but knows how to become one - is read the same way a literal is, while an array,
    // which exports an index too, is a sequence and falls through to the bytes it holds.
    if (PyIndex_Check(object) && !PySequence_Check(object)) return uuid_from_integer(object, uuid);
    if (PyObject_CheckBuffer(object)) {
        Py_buffer buffer;
        if (PyObject_GetBuffer(object, &buffer, PyBUF_SIMPLE) != 0) return -1;
        int const sized = buffer.len == (Py_ssize_t)sizeof(uuid->bytes);
        if (sized) memcpy(uuid->bytes, buffer.buf, sizeof(uuid->bytes));
        PyBuffer_Release(&buffer);
        if (sized) return 0;
        PyErr_Format(PyExc_ValueError, "A UUID spelled as bytes takes sixteen of them, not %zd", buffer.len);
        return -1;
    }
    PyErr_Format(PyExc_TypeError, "Cannot read a UUID from %S", Py_TYPE(object));
    return -1;
}

/** The keyword `uuid.UUID` reads raw bytes through, built once beside @c UUIDType. */
static PyObject* uuid_bytes_keyword = NULL;

int ustore_py_uuid_import(void) {
    if (UUIDType) return 0;
    PyObject* const uuid_module = PyImport_ImportModule("uuid");
    if (!uuid_module) return -1;
    UUIDType = PyObject_GetAttrString(uuid_module, "UUID");
    Py_DECREF(uuid_module);
    if (!UUIDType) return -1;
    PyObject* const keyword = PyUnicode_InternFromString("bytes");
    uuid_bytes_keyword = keyword ? PyTuple_Pack(1, keyword) : NULL;
    Py_XDECREF(keyword);
    if (uuid_bytes_keyword) return 0;
    Py_CLEAR(UUIDType);
    return -1;
}

PyObject* ustore_py_uuid_to_object(ustore_uuid_t const* uuid) {
    PyObject* const bytes = PyBytes_FromStringAndSize((char const*)uuid->bytes, (Py_ssize_t)sizeof(uuid->bytes));
    if (!bytes) return NULL;
    // The slot before the arguments is scratch the callee may borrow, as the offset flag permits.
    PyObject* arguments[2] = {NULL, bytes};
    PyObject* const object = PyObject_Vectorcall(UUIDType, arguments + 1, PY_VECTORCALL_ARGUMENTS_OFFSET,
                                                 uuid_bytes_keyword);
    Py_DECREF(bytes);
    return object;
}

int ustore_py_key_from_object(PyObject* object, ustore_key_t* key) {
    return ustore_py_uuid_from_object(object, &key->uuid);
}

int ustore_py_is_one_key(PyObject* object) {
    if (PyObject_TypeCheck(object, (PyTypeObject*)UUIDType)) return 1;
    if (PyUnicode_Check(object) || PyBytes_Check(object) || PyByteArray_Check(object)) return 1;
    // An array exports an index too, so only something that is not a sequence reads as one key.
    return PyIndex_Check(object) && !PySequence_Check(object);
}

/** Whether a buffer lays out 16-byte keys, one per item or one per row of sixteen bytes. */
static int buffer_holds_keys(Py_buffer const* buffer) {
    if (buffer->ndim == 1) return buffer->itemsize == (Py_ssize_t)sizeof(ustore_key_t);
    return buffer->ndim == 2 && buffer->itemsize == 1 && buffer->shape[1] == (Py_ssize_t)sizeof(ustore_key_t);
}

/** Whether a buffer lays out 64-bit integers, which convert into keys one by one. */
static int buffer_holds_integers(Py_buffer const* buffer) {
    if (buffer->ndim != 1 || buffer->itemsize != 8 || !buffer->format) return 0;
    char const* const format = buffer->format[0] == '@' || buffer->format[0] == '=' ? buffer->format + 1
                                                                                    : buffer->format;
    static char const readable[] = "qQlLnN";
    return format[0] != '\0' && format[1] == '\0' && strchr(readable, format[0]) != NULL;
}

/** Points @p batch at storage for @p count keys: the inline run, or one allocation past it. */
static int keys_reserve(ustore_py_keys_t* batch, size_t count) {
    if (count == 0) batch->keys = NULL;
    else if (count <= ustore_py_keys_inline_k) batch->keys = batch->inline_keys;
    else {
        batch->owned = (ustore_key_t*)PyMem_Malloc(count * sizeof(ustore_key_t));
        if (!batch->owned) {
            PyErr_NoMemory();
            return -1;
        }
        batch->keys = batch->owned;
    }
    batch->count = count;
    return 0;
}

/** Converts a buffer of 64-bit integers into keys, refusing a negative signed one. */
static int keys_from_integers(Py_buffer const* buffer, ustore_py_keys_t* batch) {
    char const kind = buffer->format[0] == '@' || buffer->format[0] == '=' ? buffer->format[1] : buffer->format[0];
    int const is_signed = kind == 'q' || kind == 'l' || kind == 'n';
    size_t const count = (size_t)buffer->shape[0];
    if (keys_reserve(batch, count) != 0) return -1;
    char const* const items = (char const*)buffer->buf;
    for (size_t index = 0; index != count; ++index) {
        uint64_t value = 0;
        memcpy(&value, items + index * 8, sizeof(value));
        if (is_signed && (int64_t)value < 0) {
            PyErr_SetString(PyExc_OverflowError, "A key must be an integer in [0, 2**128)");
            return -1;
        }
        uuid_from_halves(0, value, &batch->keys[index].uuid);
    }
    return 0;
}

/** Whether a batch of keys travels as a borrowed buffer, converts from one, or reads element by element. */
typedef enum keys_route_t {

    /** A contiguous run of 16-byte keys the batch borrows outright. */
    keys_route_borrowed_k,

    /** A contiguous run of 64-bit integers, converted into keys and released. */
    keys_route_converted_k,

    /** Not a buffer of either layout, so each element converts on its own terms. */
    keys_route_sequence_k,

    /** The conversion failed, with an exception set. */
    keys_route_failed_k,
} keys_route_t;

/**
 *  @brief Tries the buffer routes, falling back to a per-element conversion otherwise.
 *
 *  Only the failure the buffer request itself provoked is cleared, so nothing an exporter raised on
 *  its own behalf is swallowed.
 */
static keys_route_t keys_from_buffer(PyObject* object, ustore_py_keys_t* batch) {
    if (!PyObject_CheckBuffer(object)) return keys_route_sequence_k;
    if (PyObject_GetBuffer(object, &batch->buffer, PyBUF_C_CONTIGUOUS | PyBUF_FORMAT) != 0) {
        PyErr_Clear();
        return keys_route_sequence_k;
    }
    batch->borrowed = 1;
    Py_buffer const* const buffer = &batch->buffer;
    if (buffer_holds_keys(buffer)) {
        batch->count = (size_t)buffer->shape[0];
        // The ABI spells an empty batch with no keys at all, and refuses an address for no tasks.
        batch->keys = batch->count ? (ustore_key_t*)buffer->buf : NULL;
        return keys_route_borrowed_k;
    }
    if (buffer_holds_integers(buffer)) {
        int const converted = keys_from_integers(buffer, batch);
        PyBuffer_Release(&batch->buffer);
        batch->borrowed = 0;
        return converted == 0 ? keys_route_converted_k : keys_route_failed_k;
    }
    PyBuffer_Release(&batch->buffer);
    batch->borrowed = 0;
    return keys_route_sequence_k;
}

int ustore_py_keys_from_object(PyObject* object, ustore_py_keys_t* batch) {
    memset(batch, 0, sizeof(*batch));
    // Text and bytes are sequences too, and iterating one would answer a key per character
    if (ustore_py_is_one_key(object)) {
        PyErr_Format(PyExc_TypeError, "A batch of keys takes a sequence or a buffer, not one %S", Py_TYPE(object));
        return -1;
    }

    switch (keys_from_buffer(object, batch)) {
    case keys_route_borrowed_k:
    case keys_route_converted_k: return 0;
    case keys_route_failed_k: ustore_py_keys_release(batch); return -1;
    case keys_route_sequence_k: break;
    }

    PyObject* const sequence = PySequence_Fast(object, "Expected a sequence of keys or a buffer of keys");
    if (!sequence) return -1;
    Py_ssize_t const count = PySequence_Fast_GET_SIZE(sequence);
    if (keys_reserve(batch, (size_t)count) != 0) {
        Py_DECREF(sequence);
        return -1;
    }
    for (Py_ssize_t entry_index = 0; entry_index != count; ++entry_index)
        if (ustore_py_key_from_object(PySequence_Fast_GET_ITEM(sequence, entry_index), &batch->keys[entry_index]) !=
            0) {
            Py_DECREF(sequence);
            ustore_py_keys_release(batch);
            return -1;
        }
    Py_DECREF(sequence);
    return 0;
}

int ustore_py_keys_from_subscript(PyObject* object, ustore_py_keys_t* batch) {
    if (!ustore_py_is_one_key(object)) return ustore_py_keys_from_object(object, batch) == 0 ? 0 : -1;
    memset(batch, 0, sizeof(*batch));
    if (ustore_py_key_from_object(object, &batch->inline_keys[0]) != 0) return -1;
    batch->keys = batch->inline_keys;
    batch->count = 1;
    return 1;
}

void ustore_py_keys_release(ustore_py_keys_t* batch) {
    if (batch->borrowed) PyBuffer_Release(&batch->buffer);
    if (batch->owned) PyMem_Free(batch->owned);
    batch->borrowed = 0;
    batch->owned = NULL;
    batch->keys = NULL;
    batch->count = 0;
}

#pragma endregion Keys

#pragma region Arrow Arrays

#ifndef ARROW_C_DATA_INTERFACE
#define ARROW_C_DATA_INTERFACE

#define ARROW_FLAG_DICTIONARY_ORDERED 1
#define ARROW_FLAG_NULLABLE           2
#define ARROW_FLAG_MAP_KEYS_SORTED    4

/** The type of an exported array, laid out as the Arrow C data interface fixes it. */
struct ArrowSchema {
    char const* format;
    char const* name;
    char const* metadata;
    int64_t flags;
    int64_t n_children;
    struct ArrowSchema** children;
    struct ArrowSchema* dictionary;
    void (*release)(struct ArrowSchema*);
    void* private_data;
};

/** The buffers of an exported array, laid out as the Arrow C data interface fixes it. */
struct ArrowArray {
    int64_t length;
    int64_t null_count;
    int64_t offset;
    int64_t n_buffers;
    int64_t n_children;
    void const** buffers;
    struct ArrowArray** children;
    struct ArrowArray* dictionary;
    void (*release)(struct ArrowArray*);
    void* private_data;
};

#endif // ARROW_C_DATA_INTERFACE

#pragma endregion Arrow Arrays

#pragma region Strings Views

/** Writes the view of every requested string out of the views the call pinned when it was gathered. */
static void strings_get_each(void const* handle, ustore_size_t const* indices, ustore_size_t count,
                             ustore_bytes_view_t* views) {
    ustore_bytes_view_t const* const pinned = (ustore_bytes_view_t const*)handle;
    for (ustore_size_t index = 0; index != count; ++index) views[index] = pinned[indices[index]];
}

/** Writes the one view the call pinned for every requested string. */
static void strings_get_repeated(void const* handle, ustore_size_t const* Py_UNUSED(indices), ustore_size_t count,
                                 ustore_bytes_view_t* views) {
    ustore_bytes_view_t const repeated = *(ustore_bytes_view_t const*)handle;
    for (ustore_size_t index = 0; index != count; ++index) views[index] = repeated;
}

ustore_strings_view_t ustore_py_strings_each(ustore_bytes_view_t const* views) {
    ustore_strings_view_t strings = {.layout = ustore_strings_accessor_k};
    strings.data.accessor.get = strings_get_each;
    strings.data.accessor.handle = views;
    return strings;
}

ustore_strings_view_t ustore_py_strings_repeated(ustore_bytes_view_t const* view) {
    ustore_strings_view_t strings = {.layout = ustore_strings_accessor_k};
    strings.data.accessor.get = strings_get_repeated;
    strings.data.accessor.handle = view;
    return strings;
}

ustore_bytes_view_t ustore_py_tape_string(ustore_strings_view_t const* tape, size_t index) {
    uint8_t const* data = tape->data.u64_tape.data;
    uint64_t begin = 0, end = 0;
    if (tape->layout == ustore_strings_u32_tape_k) {
        data = tape->data.u32_tape.data;
        begin = tape->data.u32_tape.offsets[index], end = tape->data.u32_tape.offsets[index + 1];
    }
    else begin = tape->data.u64_tape.offsets[index], end = tape->data.u64_tape.offsets[index + 1];
    // A tape of empty strings may carry no bytes at all, and no address may be offset from NULL.
    ustore_bytes_view_t const string = {data ? data + begin : data, end - begin};
    return string;
}

ustore_size_t ustore_py_tape_bytes(ustore_strings_view_t const* tape, size_t count) {
    if (tape->layout == ustore_strings_u32_tape_k)
        return tape->data.u32_tape.offsets ? tape->data.u32_tape.offsets[count] : 0;
    return tape->data.u64_tape.offsets ? tape->data.u64_tape.offsets[count] : 0;
}

#pragma endregion Strings Views

#pragma region Contents

int ustore_py_contents_reserve(ustore_py_contents_t* batch, size_t count, ustore_py_contents_spread_t spread) {
    memset(batch, 0, sizeof(*batch));
    batch->spread = spread;
    if (count <= ustore_py_contents_inline_k) {
        batch->views = batch->inline_views;
        batch->buffers = batch->inline_buffers;
    }
    else {
        // One block carves into both runs, the views leading, so a batch of any width is one allocation.
        size_t const widest = sizeof(ustore_bytes_view_t) + sizeof(Py_buffer);
        if (count > (size_t)PY_SSIZE_T_MAX / widest) {
            PyErr_NoMemory();
            return -1;
        }
        batch->block = PyMem_Malloc(count * widest);
        if (!batch->block) {
            PyErr_NoMemory();
            return -1;
        }
        batch->views = (ustore_bytes_view_t*)batch->block;
        batch->buffers = (Py_buffer*)(void*)(batch->views + count);
    }
    batch->view = spread == ustore_py_contents_broadcast_k ? ustore_py_strings_repeated(batch->views)
                                                           : ustore_py_strings_each(batch->views);
    return 0;
}

int ustore_py_contents_borrow(PyObject* object, ustore_py_contents_t* batch) {
    Py_buffer* const buffer = &batch->buffers[batch->borrowed];
    if (PyObject_GetBuffer(object, buffer, PyBUF_SIMPLE) != 0)
        return ustore_py_raise_from_cause(PyExc_TypeError, "A value must be bytes-like, not %S; remove() removes keys",
                                          Py_TYPE(object));
    ++batch->borrowed;
    batch->views[batch->count].data = (uint8_t const*)buffer->buf;
    batch->views[batch->count].length = (ustore_size_t)buffer->len;
    ++batch->count;
    return 0;
}

/** The width of the unsigned offsets @p buffer lays out, four or eight bytes, or zero for anything else. */
static size_t buffer_offsets_width(Py_buffer const* buffer) {
    if (buffer->ndim != 1 || (buffer->itemsize != 4 && buffer->itemsize != 8) || !buffer->format) return 0;
    char const* const format = buffer->format[0] == '@' || buffer->format[0] == '=' ? buffer->format + 1
                                                                                    : buffer->format;
    static char const readable[] = "ILQN";
    int const offsets = format[0] != '\0' && format[1] == '\0' && strchr(readable, format[0]) != NULL;
    return offsets ? (size_t)buffer->itemsize : 0;
}

/** Reads a `(data, offsets)` tuple as a tape: 1 for one, 0 when its second item lays out no offsets, -1 failing. */
static int contents_from_pair(PyObject* object, ustore_py_contents_t* batch) {
    if (PyTuple_GET_SIZE(object) != 2) return 0;
    PyObject* const offsets_object = PyTuple_GET_ITEM(object, 1);
    if (!PyObject_CheckBuffer(offsets_object)) return 0;
    Py_buffer* const data = &batch->buffers[0];
    Py_buffer* const offsets = &batch->buffers[1];
    // Only a buffer of unsigned offsets makes the pair a tape, so one that exports none leaves it two values.
    if (PyObject_GetBuffer(offsets_object, offsets, PyBUF_C_CONTIGUOUS | PyBUF_FORMAT) != 0) {
        PyErr_Clear();
        return 0;
    }
    size_t const width = buffer_offsets_width(offsets);
    if (!width || offsets->shape[0] == 0) {
        PyBuffer_Release(offsets);
        return 0;
    }
    if (PyObject_GetBuffer(PyTuple_GET_ITEM(object, 0), data, PyBUF_SIMPLE) != 0) {
        PyBuffer_Release(offsets);
        return ustore_py_raise_from_cause(PyExc_TypeError, "The data of a tape must be bytes-like, not %S",
                                          Py_TYPE(PyTuple_GET_ITEM(object, 0)));
    }
    batch->borrowed = 2;
    batch->count = (size_t)offsets->shape[0] - 1;

    // The engine reads every string as the difference of two neighbouring offsets, so they must climb within the data.
    uint64_t previous = 0;
    for (size_t index = 0; index <= batch->count; ++index) {
        uint64_t const offset = width == 4 ? ((uint32_t const*)offsets->buf)[index]
                                           : ((uint64_t const*)offsets->buf)[index];
        if (offset < previous || offset > (uint64_t)data->len)
            return ustore_py_raise(ustore_status_invalid_argument_k,
                                   "The offsets of a tape must climb within its %zd bytes", data->len);
        previous = offset;
    }
    batch->view.layout = width == 4 ? ustore_strings_u32_tape_k : ustore_strings_u64_tape_k;
    if (width == 4) batch->view.data.u32_tape.data = data->buf, batch->view.data.u32_tape.offsets = offsets->buf;
    else batch->view.data.u64_tape.data = data->buf, batch->view.data.u64_tape.offsets = offsets->buf;
    return 1;
}

/** Whether an Arrow array holds a null, which its validity bitmap marks with an unset bit. */
static int arrow_holds_null(struct ArrowArray const* array) {
    uint8_t const* const validities = (uint8_t const*)array->buffers[0];
    if (!validities || array->null_count == 0) return 0;
    for (int64_t index = array->offset; index != array->offset + array->length; ++index)
        if (!((validities[index / 8] >> (index % 8)) & 1)) return 1;
    return 0;
}

/** Reads an Arrow array of binary or text as a tape: 1 for one, 0 when @p object exports none, -1 failing. */
static int contents_from_arrow(PyObject* object, ustore_py_contents_t* batch) {
    PyObject* const exporter = PyObject_GetAttrString(object, "__arrow_c_array__");
    if (!exporter) {
        if (!PyErr_ExceptionMatches(PyExc_AttributeError)) return -1;
        PyErr_Clear();
        return 0;
    }
    PyObject* const capsules = PyObject_CallNoArgs(exporter);
    Py_DECREF(exporter);
    if (!capsules) return -1;
    struct ArrowSchema const* schema = NULL;
    struct ArrowArray const* array = NULL;
    if (PyTuple_Check(capsules) && PyTuple_GET_SIZE(capsules) == 2) {
        schema = PyCapsule_GetPointer(PyTuple_GET_ITEM(capsules, 0), "arrow_schema");
        array = schema ? PyCapsule_GetPointer(PyTuple_GET_ITEM(capsules, 1), "arrow_array") : NULL;
    }
    char const* const format = array ? schema->format : "";
    int const narrow = strcmp(format, "z") == 0 || strcmp(format, "u") == 0;
    int const wide = strcmp(format, "Z") == 0 || strcmp(format, "U") == 0;
    int read = -1;
    if (!array) {
        if (!PyErr_Occurred())
            PyErr_SetString(PyExc_TypeError, "__arrow_c_array__ must answer a schema capsule and an array capsule");
    }
    else if (!narrow && !wide)
        PyErr_Format(PyExc_TypeError,
                     "An Arrow array of values is binary, large_binary, string or large_string, not of format '%s'",
                     format);
    else if (array->n_buffers != 3)
        PyErr_Format(PyExc_TypeError, "An Arrow array of values carries three buffers, not %lld",
                     (long long)array->n_buffers);
    else if (arrow_holds_null(array))
        ustore_py_raise(ustore_status_invalid_argument_k,
                        "An Arrow array of values holds a null; remove() removes keys");
    else {
        // The capsule releases the array's buffers once it dies, so holding it keeps the tape readable.
        batch->held = Py_NewRef(PyTuple_GET_ITEM(capsules, 1));
        batch->count = (size_t)array->length;
        uint8_t const* const data = (uint8_t const*)array->buffers[2];
        if (narrow) {
            batch->view.layout = ustore_strings_u32_tape_k;
            batch->view.data.u32_tape.data = data;
            batch->view.data.u32_tape.offsets = (uint32_t const*)array->buffers[1] + array->offset;
        }
        else {
            batch->view.layout = ustore_strings_u64_tape_k;
            batch->view.data.u64_tape.data = data;
            batch->view.data.u64_tape.offsets = (uint64_t const*)array->buffers[1] + array->offset;
        }
        read = 1;
    }
    Py_DECREF(capsules);
    return read;
}

int ustore_py_contents_from_tape(PyObject* object, ustore_py_contents_t* batch) {
    // The common batches are builtins no Arrow exporter derives from, so they skip the lookup of its method.
    if (PyList_CheckExact(object) || PyBytes_CheckExact(object) || PyByteArray_CheckExact(object) ||
        PyUnicode_CheckExact(object) || PyDict_CheckExact(object))
        return 0;
    memset(batch, 0, sizeof(*batch));
    batch->views = batch->inline_views;
    batch->buffers = batch->inline_buffers;
    int const read = PyTuple_CheckExact(object) ? contents_from_pair(object, batch)
                                                : contents_from_arrow(object, batch);
    if (read < 0) ustore_py_contents_release(batch);
    return read;
}

void ustore_py_contents_release(ustore_py_contents_t* batch) {
    for (size_t buffer_index = 0; buffer_index != batch->borrowed; ++buffer_index)
        PyBuffer_Release(&batch->buffers[buffer_index]);
    batch->borrowed = 0;
    if (batch->block) PyMem_Free(batch->block);
    batch->block = NULL;
    batch->count = 0;
    Py_CLEAR(batch->held);
}

#pragma endregion Contents

#pragma region Spans

static char const doc_Span[] = "A read-only window into an arena, which a memoryview is built over.";

int ustore_py_buffer_fill(Py_buffer* view, PyObject* owner, void const* data, char const* format, Py_ssize_t itemsize,
                          int ndim, Py_ssize_t* shape, Py_ssize_t* strides, int flags) {
    if ((flags & PyBUF_WRITABLE) == PyBUF_WRITABLE) {
        view->obj = NULL;
        PyErr_SetString(PyExc_BufferError, "The outputs a call wrote into an arena are read-only");
        return -1;
    }
    Py_ssize_t items = 1;
    for (int dimension = 0; dimension != ndim; ++dimension) items *= shape[dimension];

    // An empty run may carry no address at all, and a buffer must still point somewhere.
    view->buf = data ? (void*)data : (void*)owner;
    view->obj = Py_NewRef(owner);
    view->len = items * itemsize;
    view->readonly = 1;
    view->itemsize = itemsize;
    view->format = (flags & PyBUF_FORMAT) == PyBUF_FORMAT ? (char*)format : NULL;
    // A consumer that asks for no shape reads the buffer as one run of bytes.
    view->ndim = (flags & PyBUF_ND) == PyBUF_ND ? ndim : 1;
    view->shape = (flags & PyBUF_ND) == PyBUF_ND ? shape : NULL;
    view->strides = (flags & PyBUF_STRIDES) == PyBUF_STRIDES ? strides : NULL;
    view->suboffsets = NULL;
    view->internal = NULL;
    return 0;
}

PyObject* ustore_py_span_view(PyObject* arena, size_t generation, void const* data, char const* format,
                              Py_ssize_t itemsize, int ndim, Py_ssize_t const* shape) {
    Span* const span = PyObject_New(Span, &SpanType);
    if (!span) return NULL;
    span->arena = Py_NewRef(arena);
    span->generation = generation;
    span->data = data;
    span->format = format;
    span->itemsize = itemsize;
    span->ndim = ndim;
    span->shape[0] = shape[0];
    span->shape[1] = ndim == 2 ? shape[1] : 1;
    span->strides[0] = ndim == 2 ? itemsize * shape[1] : itemsize;
    span->strides[1] = itemsize;
    // The view holds the span through its buffer, and the span holds the owner.
    PyObject* const view = PyMemoryView_FromObject((PyObject*)span);
    Py_DECREF(span);
    return view;
}

static void Span_dealloc(Span* self) {
    Py_XDECREF(self->arena);
    PyObject_Free(self);
}

static int Span_getbuffer(Span* self, Py_buffer* view, int flags) {
    view->obj = NULL;
    if (ustore_py_arena_pin(self->arena, self->generation) != 0) return -1;
    if (ustore_py_buffer_fill(view, (PyObject*)self, self->data, self->format, self->itemsize, self->ndim, self->shape,
                              self->strides, flags) == 0)
        return 0;
    ustore_py_arena_unpin(self->arena);
    return -1;
}

static void Span_releasebuffer(Span* self, Py_buffer* Py_UNUSED(view)) { ustore_py_arena_unpin(self->arena); }

static PyBufferProcs Span_as_buffer = {
    .bf_getbuffer = (getbufferproc)Span_getbuffer,
    .bf_releasebuffer = (releasebufferproc)Span_releasebuffer,
};

PyTypeObject SpanType = {
    PyVarObject_HEAD_INIT(NULL, 0) //
        .tp_name = "ustore.Span",
    .tp_doc = doc_Span,
    .tp_basicsize = sizeof(Span),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = NULL,
    .tp_dealloc = (destructor)Span_dealloc,
    .tp_as_buffer = &Span_as_buffer,
};

#pragma endregion Spans

#pragma region Value Tapes

static char const doc_Values[] =                                                                //
    "The values one batch read wrote into an `Arena`, viewing it until it rewinds.\n"           //
    "\n"                                                                                        //
    "Indexing answers a `memoryview` over the stored bytes, or None where the key was\n"        //
    "absent; nothing is copied, and the view pins the arena against the next call until it\n"   //
    "is released. `bytes(values[index])` is where a copy happens, and nowhere before it. The\n" //
    "whole batch exports through the Arrow C data interface as `binary`, or as\n"               //
    "`large_binary` once its bytes outgrow 32-bit offsets, so `pyarrow.array(values)` reads\n"  //
    "the arena in place too, and a write takes it back as the tape it is. Read after a later\n" //
    "call rewound the arena, it raises `UninitializedError`.";

PyObject* ustore_py_values_new(PyObject* arena, ustore_octet_t const* presences, ustore_strings_view_t const* tape,
                               size_t count) {
    Values* const instance = PyObject_New(Values, &ValuesType);
    if (!instance) return NULL;
    instance->arena = Py_NewRef(arena);
    instance->generation = ustore_py_arena_generation(arena);
    instance->presences = presences;
    instance->tape = *tape;
    instance->count = count;
    return (PyObject*)instance;
}

static void Values_dealloc(Values* self) {
    Py_XDECREF(self->arena);
    PyObject_Free(self);
}

int ustore_py_present(ustore_octet_t const* presences, size_t task_index) {
    return presences && ((presences[task_index / 8] >> (task_index % 8)) & 1);
}

/** Whether task @p task_index of @p values found its key, where a tape without presence bits holds every value. */
static int values_present(Values const* values, size_t task_index) {
    return !values->presences || ustore_py_present(values->presences, task_index);
}

/** The bytes of every value of @p values, back to back, which a read of no values may leave without an address. */
static uint8_t const* values_data(Values const* values) {
    return values->tape.layout == ustore_strings_u32_tape_k ? values->tape.data.u32_tape.data
                                                            : values->tape.data.u64_tape.data;
}

static Py_ssize_t Values_length(Values* self) { return (Py_ssize_t)self->count; }

static PyObject* Values_item(Values* self, Py_ssize_t task_index) {
    // `PySequence_GetItem` normalizes a negative index before reaching here.
    if (task_index < 0 || (size_t)task_index >= self->count) {
        PyErr_SetString(PyExc_IndexError, "Task index out of range");
        return NULL;
    }
    if (ustore_py_arena_pin(self->arena, self->generation) != 0) return NULL;
    PyObject* item = NULL;
    if (!values_present(self, (size_t)task_index)) item = Py_NewRef(Py_None);
    else {
        ustore_bytes_view_t const value = ustore_py_tape_string(&self->tape, (size_t)task_index);
        Py_ssize_t const shape = (Py_ssize_t)value.length;
        item = ustore_py_span_view(self->arena, self->generation, value.data, "B", 1, 1, &shape);
    }
    ustore_py_arena_unpin(self->arena);
    return item;
}

static int Values_getbuffer(Values* self, Py_buffer* view, int flags) {
    view->obj = NULL;
    if (ustore_py_arena_pin(self->arena, self->generation) != 0) return -1;
    // A tape of no values still answers a buffer, so a caller need not special-case an empty read.
    void* const data = values_data(self) ? (void*)values_data(self) : (void*)self;
    Py_ssize_t const bytes = (Py_ssize_t)ustore_py_tape_bytes(&self->tape, self->count);
    if (PyBuffer_FillInfo(view, (PyObject*)self, data, bytes, 1, flags) == 0) return 0;
    ustore_py_arena_unpin(self->arena);
    return -1;
}

static void Values_releasebuffer(Values* self, Py_buffer* Py_UNUSED(view)) { ustore_py_arena_unpin(self->arena); }

static PyObject* Values_repr(Values* self) { return PyUnicode_FromFormat("Values(count=%zu)", self->count); }

static char const doc_Values_lengths[] = "The length of every value, with None where the key was absent, as a tuple.";

static PyObject* Values_get_lengths(Values* self, void* Py_UNUSED(closure)) {
    if (ustore_py_arena_pin(self->arena, self->generation) != 0) return NULL;
    PyObject* lengths = PyTuple_New((Py_ssize_t)self->count);
    for (size_t task_index = 0; lengths && task_index != self->count; ++task_index) {
        ustore_size_t const length = ustore_py_tape_string(&self->tape, task_index).length;
        PyObject* const entry = !values_present(self, task_index) ? Py_NewRef(Py_None)
                                                                  : PyLong_FromUnsignedLongLong(length);
        if (!entry) Py_CLEAR(lengths);
        else PyTuple_SET_ITEM(lengths, (Py_ssize_t)task_index, entry);
    }
    ustore_py_arena_unpin(self->arena);
    return lengths;
}

/** What one exported array keeps alive until its consumer releases it: the @c Values, its pin and its buffers. */
typedef struct values_export_t {

    /** The @c Values viewing the tape and its spans, held by a strong reference, its arena pinned. */
    Values* owner;

    /** The presence bits, the offsets and the bytes, in the order `binary` and `large_binary` list them. */
    void const* buffers[3];
} values_export_t;

/** Any readable address, for offsets or a tape of no bytes that a consumer still dereferences. */
static int64_t const values_export_empty_k[1] = {0};

static void values_schema_release(struct ArrowSchema* schema) { schema->release = NULL; }

/** Lets go of the exported @c Values and its pin, taking the interpreter, as a consumer may release from a thread of
 *  its own. */
static void values_array_release(struct ArrowArray* array) {
    values_export_t* const exported = (values_export_t*)array->private_data;
    PyGILState_STATE const state = PyGILState_Ensure();
    ustore_py_arena_unpin(exported->owner->arena);
    Py_DECREF(exported->owner);
    PyGILState_Release(state);
    PyMem_RawFree(exported);
    array->release = NULL;
}

static void values_schema_capsule_free(PyObject* capsule) {
    struct ArrowSchema* const schema = (struct ArrowSchema*)PyCapsule_GetPointer(capsule, "arrow_schema");
    if (schema->release) schema->release(schema);
    PyMem_RawFree(schema);
}

static void values_array_capsule_free(PyObject* capsule) {
    struct ArrowArray* const array = (struct ArrowArray*)PyCapsule_GetPointer(capsule, "arrow_array");
    if (array->release) array->release(array);
    PyMem_RawFree(array);
}

static char const doc_Values_arrow_c_array[] =                                              //
    "__arrow_c_array__(requested_schema=None) answers the schema and array PyCapsules.\n"   //
    "\n"                                                                                    //
    "Exports the batch as Arrow `binary` with the 32-bit offsets the read wrote, or as\n"   //
    "`large_binary` with 64-bit ones once its bytes outgrow 4 GiB, with a value missing\n"  //
    "where the key was absent. The presence bits, offsets and bytes the read wrote are\n"   //
    "exported in place, and the array keeps this object alive and pins its arena until\n"   //
    "Arrow releases it. A requested schema is ignored, and Arrow casts from the exported\n" //
    "type where it asked for another.";

static PyObject* Values_arrow_c_array(Values* self, PyObject* const* Py_UNUSED(args), Py_ssize_t positional_args_count,
                                      PyObject* args_names_tuple) {
    Py_ssize_t const args_names_count = args_names_tuple ? PyTuple_GET_SIZE(args_names_tuple) : 0;
    int const named = args_names_count == 1 &&
                      PyUnicode_CompareWithASCIIString(PyTuple_GET_ITEM(args_names_tuple, 0), "requested_schema") == 0;
    if (positional_args_count + args_names_count > 1 || (args_names_count && !named)) {
        PyErr_SetString(PyExc_TypeError, "__arrow_c_array__() takes at most the 'requested_schema' argument");
        return NULL;
    }

    struct ArrowSchema* const schema = (struct ArrowSchema*)PyMem_RawCalloc(1, sizeof(struct ArrowSchema));
    struct ArrowArray* const array = (struct ArrowArray*)PyMem_RawCalloc(1, sizeof(struct ArrowArray));
    values_export_t* const exported = (values_export_t*)PyMem_RawCalloc(1, sizeof(values_export_t));
    if (!schema || !array || !exported) {
        PyMem_RawFree(exported);
        PyMem_RawFree(array);
        PyMem_RawFree(schema);
        PyErr_NoMemory();
        return NULL;
    }
    if (ustore_py_arena_pin(self->arena, self->generation) != 0) {
        PyMem_RawFree(exported);
        PyMem_RawFree(array);
        PyMem_RawFree(schema);
        return NULL;
    }

    int const narrow = self->tape.layout == ustore_strings_u32_tape_k;
    void const* const offsets = narrow ? (void const*)self->tape.data.u32_tape.offsets
                                       : (void const*)self->tape.data.u64_tape.offsets;
    schema->format = narrow ? "z" : "Z";
    schema->name = "";
    schema->flags = ARROW_FLAG_NULLABLE;
    schema->release = values_schema_release;

    exported->owner = (Values*)Py_NewRef((PyObject*)self);
    exported->buffers[0] = self->presences;
    exported->buffers[1] = offsets ? offsets : (void const*)values_export_empty_k;
    exported->buffers[2] = values_data(self) ? (void const*)values_data(self) : (void const*)values_export_empty_k;
    array->length = (int64_t)self->count;
    // The count of absent keys is left for the consumer to take from the presence bits, where there are any.
    array->null_count = self->presences ? -1 : 0;
    array->n_buffers = 3;
    array->buffers = exported->buffers;
    array->release = values_array_release;
    array->private_data = exported;

    PyObject* const schema_capsule = PyCapsule_New(schema, "arrow_schema", values_schema_capsule_free);
    if (!schema_capsule) {
        PyMem_RawFree(schema);
        values_array_release(array);
        PyMem_RawFree(array);
        return NULL;
    }
    PyObject* const array_capsule = PyCapsule_New(array, "arrow_array", values_array_capsule_free);
    if (!array_capsule) {
        Py_DECREF(schema_capsule);
        values_array_release(array);
        PyMem_RawFree(array);
        return NULL;
    }
    PyObject* const capsules = PyTuple_Pack(2, schema_capsule, array_capsule);
    Py_DECREF(schema_capsule);
    Py_DECREF(array_capsule);
    return capsules;
}

static PyMethodDef Values_methods[] = {
    {"__arrow_c_array__", (PyCFunction)(void (*)(void))Values_arrow_c_array, ustore_py_method_flags_k,
     doc_Values_arrow_c_array},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef Values_getset[] = {
    {"lengths", (getter)Values_get_lengths, NULL, doc_Values_lengths, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PySequenceMethods Values_as_sequence = {
    .sq_length = (lenfunc)Values_length,
    .sq_item = (ssizeargfunc)Values_item,
};

static PyBufferProcs Values_as_buffer = {
    .bf_getbuffer = (getbufferproc)Values_getbuffer,
    .bf_releasebuffer = (releasebufferproc)Values_releasebuffer,
};

PyTypeObject ValuesType = {
    PyVarObject_HEAD_INIT(NULL, 0) //
        .tp_name = "ustore.Values",
    .tp_doc = doc_Values,
    .tp_basicsize = sizeof(Values),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = NULL,
    .tp_dealloc = (destructor)Values_dealloc,
    .tp_repr = (reprfunc)Values_repr,
    .tp_as_sequence = &Values_as_sequence,
    .tp_as_buffer = &Values_as_buffer,
    .tp_methods = Values_methods,
    .tp_getset = Values_getset,
};

#pragma endregion Value Tapes

#pragma region Key Tapes

static char const doc_Keys[] =                                                                //
    "The keys one batch scan or sample wrote into an `Arena`, viewing it until it rewinds.\n" //
    "\n"                                                                                      //
    "Indexing answers a tuple of `uuid.UUID` keys for one task, which is where a copy\n"      //
    "happens. The whole tape exports through the buffer protocol as an array of shape\n"      //
    "(keys, 16) bytes, so `np.asarray(keys)` reads it without copying, and pins the arena\n"  //
    "against the next call until released. Read after a later call rewound the arena, it\n"   //
    "raises `UninitializedError`.";

PyObject* ustore_py_keys_new(PyObject* arena, ustore_length_t const* offsets, ustore_key_t const* tape, size_t count) {
    Keys* const instance = PyObject_New(Keys, &KeysType);
    if (!instance) return NULL;
    instance->arena = Py_NewRef(arena);
    instance->generation = ustore_py_arena_generation(arena);
    instance->offsets = offsets;
    instance->tape = tape;
    instance->count = count;
    // The keys found are known once the call wrote the last offset, while the call still holds the arena.
    instance->shape[0] = offsets ? (Py_ssize_t)offsets[count] : 0;
    instance->shape[1] = (Py_ssize_t)sizeof(ustore_key_t);
    instance->strides[0] = (Py_ssize_t)sizeof(ustore_key_t);
    instance->strides[1] = 1;
    return (PyObject*)instance;
}

static void Keys_dealloc(Keys* self) {
    Py_XDECREF(self->arena);
    PyObject_Free(self);
}

static Py_ssize_t Keys_length(Keys* self) { return (Py_ssize_t)self->count; }

static PyObject* Keys_item(Keys* self, Py_ssize_t task_index) {
    if (task_index < 0 || (size_t)task_index >= self->count) {
        PyErr_SetString(PyExc_IndexError, "Task index out of range");
        return NULL;
    }
    if (ustore_py_arena_pin(self->arena, self->generation) != 0) return NULL;
    ustore_key_t const* const run = self->tape + self->offsets[task_index];
    Py_ssize_t const found = (Py_ssize_t)(self->offsets[task_index + 1] - self->offsets[task_index]);
    PyObject* keys = PyTuple_New(found);
    for (Py_ssize_t key_index = 0; keys && key_index != found; ++key_index) {
        PyObject* const key = ustore_py_uuid_to_object(&run[key_index].uuid);
        if (!key) Py_CLEAR(keys);
        else PyTuple_SET_ITEM(keys, key_index, key);
    }
    ustore_py_arena_unpin(self->arena);
    return keys;
}

static int Keys_getbuffer(Keys* self, Py_buffer* view, int flags) {
    view->obj = NULL;
    if (ustore_py_arena_pin(self->arena, self->generation) != 0) return -1;
    if (ustore_py_buffer_fill(view, (PyObject*)self, self->tape, "B", 1, 2, self->shape, self->strides, flags) == 0)
        return 0;
    ustore_py_arena_unpin(self->arena);
    return -1;
}

static void Keys_releasebuffer(Keys* self, Py_buffer* Py_UNUSED(view)) { ustore_py_arena_unpin(self->arena); }

static PyObject* Keys_repr(Keys* self) {
    return PyUnicode_FromFormat("Keys(tasks=%zu, keys=%zd)", self->count, self->shape[0]);
}

static char const doc_Keys_counts[] = "The number of keys every task found, as a tuple.";

static PyObject* Keys_get_counts(Keys* self, void* Py_UNUSED(closure)) {
    if (ustore_py_arena_pin(self->arena, self->generation) != 0) return NULL;
    PyObject* counts = PyTuple_New((Py_ssize_t)self->count);
    for (size_t task_index = 0; counts && task_index != self->count; ++task_index) {
        PyObject* const entry = PyLong_FromUnsignedLongLong(self->offsets[task_index + 1] - self->offsets[task_index]);
        if (!entry) Py_CLEAR(counts);
        else PyTuple_SET_ITEM(counts, (Py_ssize_t)task_index, entry);
    }
    ustore_py_arena_unpin(self->arena);
    return counts;
}

static PyGetSetDef Keys_getset[] = {
    {"counts", (getter)Keys_get_counts, NULL, doc_Keys_counts, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PySequenceMethods Keys_as_sequence = {
    .sq_length = (lenfunc)Keys_length,
    .sq_item = (ssizeargfunc)Keys_item,
};

static PyBufferProcs Keys_as_buffer = {
    .bf_getbuffer = (getbufferproc)Keys_getbuffer,
    .bf_releasebuffer = (releasebufferproc)Keys_releasebuffer,
};

PyTypeObject KeysType = {
    PyVarObject_HEAD_INIT(NULL, 0) //
        .tp_name = "ustore.Keys",
    .tp_doc = doc_Keys,
    .tp_basicsize = sizeof(Keys),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = NULL,
    .tp_dealloc = (destructor)Keys_dealloc,
    .tp_repr = (reprfunc)Keys_repr,
    .tp_as_sequence = &Keys_as_sequence,
    .tp_as_buffer = &Keys_as_buffer,
    .tp_getset = Keys_getset,
};

#pragma endregion Key Tapes

#pragma region Closed Sets

int ustore_py_size_from_object(PyObject* object, char const* field, size_t* value) {
    Py_ssize_t const signed_value = PyNumber_AsSsize_t(object, PyExc_OverflowError);
    if (signed_value == -1 && PyErr_Occurred()) {
        // A count past the index range is a bad value like a negative one, not an arithmetic failure
        if (PyErr_ExceptionMatches(PyExc_OverflowError)) {
            PyErr_Clear();
            PyErr_Format(PyExc_ValueError, "The '%s' option does not fit an index", field);
        }
        return -1;
    }
    if (signed_value < 0) {
        PyErr_Format(PyExc_ValueError, "The '%s' option must not be negative", field);
        return -1;
    }
    *value = (size_t)signed_value;
    return 0;
}

int ustore_py_optional_argument(char const* verb, PyObject* const* args, Py_ssize_t positional_args_count,
                                PyObject* args_names_tuple, Py_ssize_t required_positional, char const* keyword,
                                PyObject** argument) {
    Py_ssize_t const args_names_count = args_names_tuple ? PyTuple_GET_SIZE(args_names_tuple) : 0;
    int const named = args_names_count == 1 &&
                      PyUnicode_CompareWithASCIIString(PyTuple_GET_ITEM(args_names_tuple, 0), keyword) == 0;
    Py_ssize_t const given = positional_args_count + args_names_count;
    if (positional_args_count < required_positional || given > required_positional + 1 ||
        (args_names_count && !named)) {
        PyErr_Format(PyExc_TypeError, "%s() takes %zd positional arguments and then '%s', received %zd arguments", verb,
                     required_positional, keyword, given);
        return -1;
    }
    if (given > required_positional) *argument = args[required_positional];
    return 0;
}

Py_ssize_t ustore_py_args_names_count_checked(char const* verb, Py_ssize_t positional_args_count,
                                              PyObject* args_names_tuple, Py_ssize_t required_positional) {
    if (positional_args_count != required_positional) {
        PyErr_Format(PyExc_TypeError, "%s() takes %zd positional arguments, received %zd", verb, required_positional,
                     positional_args_count);
        return -1;
    }
    return args_names_tuple ? PyTuple_GET_SIZE(args_names_tuple) : 0;
}

/** Fills @p context with what @p origin, a @c Database, @c Transaction or @c Snapshot, addresses under @p watch. */
static int context_from_origin(PyObject* origin, ustore_watch_t watch, ustore_py_context_t* context) {
    if (PyObject_TypeCheck(origin, &DatabaseType)) {
        Database* const database = (Database*)origin;
        if (!database->handle) return ustore_py_raise(ustore_status_uninitialized_k, "The database is closed");
        context->database_handle = database->handle;
    }
    else if (PyObject_TypeCheck(origin, &TransactionType)) {
        Transaction* const transaction = (Transaction*)origin;
        if (!transaction->handle) return ustore_py_raise(ustore_status_uninitialized_k, "The transaction is closed");
        context->database_handle = transaction->database_handle;
        context->transaction = transaction->handle;
        context->watch = watch;
        context->view = ustore_transaction_view(transaction->handle, watch);
    }
    else if (PyObject_TypeCheck(origin, &SnapshotType)) {
        Snapshot* const snapshot = (Snapshot*)origin;
        if (!snapshot->handle) return ustore_py_raise(ustore_status_uninitialized_k, "The snapshot is closed");
        context->database_handle = snapshot->database_handle;
        context->snapshot = snapshot->handle;
        context->view = ustore_snapshot_view(snapshot->handle);
    }
    else {
        PyErr_Format(PyExc_TypeError, "Expected a Database, Transaction, Snapshot or View, got %S", Py_TYPE(origin));
        return -1;
    }
    context->origin = origin;
    return 0;
}

int ustore_py_context_from_object(PyObject* self, ustore_py_context_t* context) {
    memset(context, 0, sizeof(*context));
    if (!PyObject_TypeCheck(self, &ViewType) && !PyObject_TypeCheck(self, &DeferredViewType)) {
        ustore_watch_t const watch = PyObject_TypeCheck(self, &TransactionType) ? ((Transaction*)self)->watch
                                                                                : ustore_watch_keys_k;
        return context_from_origin(self, watch, context);
    }
    View const* const view = (View const*)self;
    if (view->executor && !view->executor->handle)
        return ustore_py_raise(ustore_status_uninitialized_k, "The executor is closed");
    context->executor = view->executor;
    context->completion = PyObject_TypeCheck(self, &DeferredViewType) ? ustore_completion_deferred_k
                                                                      : ustore_completion_blocking_k;
    return context_from_origin(view->origin, view->watch, context);
}

ustore_executor_t ustore_py_context_executor(ustore_py_context_t const* context) {
    return context->executor ? context->executor->handle : NULL;
}

int ustore_py_context_writable(ustore_py_context_t const* context) {
    if (!context->snapshot) return 0;
    PyErr_SetString(PyExc_TypeError, "A snapshot only reads, so it takes no writes");
    return -1;
}

int ustore_py_context_blocking(ustore_py_context_t const* context, char const* operation, char const* instead) {
    if (context->completion == ustore_completion_blocking_k) return 0;
    PyErr_Format(PyExc_TypeError, "%s, so a deferred view refuses it; call %s instead", operation, instead);
    return -1;
}

#pragma endregion Closed Sets

#pragma region Enums

/** The name of the `enum.IntEnum` mirroring each C enum, as `ustore` exports it. */
static char const* const enum_names[ustore_py_enums_count_k] = {
    [ustore_py_enum_status_k] = "Status",
    [ustore_py_enum_mood_k] = "Mood",
    [ustore_py_enum_watch_k] = "Watch",
    [ustore_py_enum_durability_k] = "Durability",
    [ustore_py_enum_drop_mode_k] = "DropMode",
    [ustore_py_enum_scan_mode_k] = "ScanMode",
    [ustore_py_enum_modality_k] = "Modality",
    [ustore_py_enum_isolation_k] = "Isolation",
    [ustore_py_enum_transport_k] = "Transport",
    [ustore_py_enum_engine_k] = "Engine",
    [ustore_py_enum_role_k] = "Role",
    [ustore_py_enum_field_type_k] = "FieldType",
    [ustore_py_enum_modification_k] = "Modification",
};

/** Every enumerator of every closed set lies below this, as the C suite checks, so a walk up to it finds them all. */
enum { enum_values_limit_k = 64 };

/** What the C `_name` functions answer for a value no enumerator carries. */
static char const enum_unrecognized_k[] = "unrecognized";

/** The canonical spelling of @p value as an enumerator of @p kind, or "unrecognized" where it is none. */
static char const* enum_spelling(ustore_py_enum_t kind, int value) {
    switch (kind) {
    case ustore_py_enum_status_k: return ustore_status_name((ustore_status_t)value);
    case ustore_py_enum_mood_k: return ustore_mood_name((ustore_mood_t)value);
    case ustore_py_enum_watch_k: return ustore_watch_name((ustore_watch_t)value);
    case ustore_py_enum_durability_k: return ustore_durability_name((ustore_durability_t)value);
    case ustore_py_enum_drop_mode_k: return ustore_drop_mode_name((ustore_drop_mode_t)value);
    case ustore_py_enum_scan_mode_k: return ustore_scan_mode_name((ustore_scan_mode_t)value);
    case ustore_py_enum_modality_k: return ustore_modality_name((ustore_modality_t)value);
    case ustore_py_enum_isolation_k: return ustore_isolation_name((ustore_isolation_t)value);
    case ustore_py_enum_transport_k: return ustore_transport_name((ustore_transport_t)value);
    case ustore_py_enum_engine_k: return ustore_engine_name((ustore_engine_t)value);
    case ustore_py_enum_role_k: return ustore_vertex_role_name((ustore_vertex_role_t)value);
    case ustore_py_enum_field_type_k: return ustore_docs_field_type_name((ustore_docs_field_type_t)value);
    case ustore_py_enum_modification_k: return ustore_docs_modification_name((ustore_docs_modification_t)value);
    case ustore_py_enums_count_k: break;
    }
    return enum_unrecognized_k;
}

/** Whether @p value is an enumerator of @p kind, which the library spells. */
static int enum_spelled(ustore_py_enum_t kind, long value) {
    return value >= 0 && value < enum_values_limit_k &&
           strcmp(enum_spelling(kind, (int)value), enum_unrecognized_k) != 0;
}

/** The module whose attributes hold the enum classes, once `ustore/__init__.py` built them. */
static PyObject* enums_module = NULL;

/** The members of @p kind, each named by its spelling in upper case with dashes as underscores, onto its value. */
static PyObject* enum_members(ustore_py_enum_t kind) {
    PyObject* members = PyDict_New();
    for (int value = 0; members && value != enum_values_limit_k; ++value) {
        if (!enum_spelled(kind, value)) continue;
        char const* const spelling = enum_spelling(kind, value);
        char name[32] = {0};
        for (size_t index = 0; spelling[index] && index + 1 != sizeof(name); ++index)
            name[index] = spelling[index] == '-' ? '_' : (char)Py_TOUPPER(spelling[index]);
        PyObject* const number = PyLong_FromLong(value);
        if (!number || PyDict_SetItemString(members, name, number) != 0) Py_CLEAR(members);
        Py_XDECREF(number);
    }
    return members;
}

int ustore_py_register_enums(PyObject* module) {
    PyObject* enums = PyDict_New();
    for (int kind = 0; enums && kind != ustore_py_enums_count_k; ++kind) {
        PyObject* const members = enum_members((ustore_py_enum_t)kind);
        if (!members || PyDict_SetItemString(enums, enum_names[kind], members) != 0) Py_CLEAR(enums);
        Py_XDECREF(members);
    }
    if (!enums) return -1;
    int const added = PyModule_AddObjectRef(module, "_enumerators", enums);
    Py_DECREF(enums);
    if (added != 0) return -1;
    enums_module = Py_NewRef(module);
    return 0;
}

int ustore_py_enum_from_object(PyObject* object, char const* field, ustore_py_enum_t kind, int* value) {
    if (!PyLong_Check(object) || PyBool_Check(object)) {
        PyErr_Format(PyExc_TypeError, "The '%s' option takes a ustore.%s, got %S", field, enum_names[kind],
                     Py_TYPE(object));
        return -1;
    }
    int overflow = 0;
    long const read = PyLong_AsLongAndOverflow(object, &overflow);
    if (read == -1 && PyErr_Occurred()) return -1;
    if (!overflow && enum_spelled(kind, read)) {
        *value = (int)read;
        return 0;
    }
    PyErr_Format(PyExc_ValueError, "The '%s' option takes a ustore.%s, and %R is none", field, enum_names[kind],
                 object);
    return -1;
}

PyObject* ustore_py_enum_to_object(ustore_py_enum_t kind, int value) {
    PyObject* const type = PyObject_GetAttrString(enums_module, enum_names[kind]);
    if (!type) return NULL;
    PyObject* const member = PyObject_CallFunction(type, "i", value);
    Py_DECREF(type);
    return member;
}

#pragma endregion Enums

#pragma region Verb Options

void ustore_py_verb_options_init(ustore_py_verb_options_t* options) {
    options->collection = (ustore_collection_id_t) {{{0}}};
    options->durability = ustore_durability_buffered_k;
}

int ustore_py_verb_option_from_keyword(ustore_py_context_t const* context, PyObject* name, PyObject* value,
                                       ustore_py_verb_options_t* options) {
    if (PyUnicode_CompareWithASCIIString(name, "collection") == 0)
        return ustore_py_collection_from_object(context, value, &options->collection) == 0 ? 1 : -1;
    if (PyUnicode_CompareWithASCIIString(name, "durability") == 0) {
        int durability = ustore_durability_buffered_k;
        if (ustore_py_enum_from_object(value, "durability", ustore_py_enum_durability_k, &durability) != 0) return -1;
        options->durability = (ustore_durability_t)durability;
        return 1;
    }
    return 0;
}

int ustore_py_verb_options_from_keywords(ustore_py_context_t const* context, PyObject* const* args,
                                         Py_ssize_t positional_args_count, PyObject* args_names_tuple,
                                         ustore_py_verb_options_t* options) {
    ustore_py_verb_options_init(options);
    Py_ssize_t const args_names_count = args_names_tuple ? PyTuple_GET_SIZE(args_names_tuple) : 0;
    for (Py_ssize_t names_progress = 0, args_progress = positional_args_count; names_progress != args_names_count;
         ++args_progress, ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(args_names_tuple, names_progress);
        int const consumed = ustore_py_verb_option_from_keyword(context, name, args[args_progress], options);
        if (consumed < 0) return -1;
        if (consumed == 0) {
            PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
            return -1;
        }
    }
    return 0;
}

#pragma endregion Verb Options

#pragma region Calls

/** The @c Deferred whose trailing storage holds @p call. */
static Deferred* deferred_of(void* call) { return (Deferred*)(void*)((char*)call - offsetof(Deferred, call)); }

void* ustore_py_call_open(ustore_py_context_t const* context, ustore_py_verb_t const* verb, void* stack) {
    if (context->completion == ustore_completion_blocking_k) {
        memset(stack, 0, verb->call_bytes);
        return stack;
    }
    Deferred* const deferred = PyObject_GC_NewVar(Deferred, &DeferredType, (Py_ssize_t)verb->call_bytes);
    if (!deferred) return NULL;
    deferred->verb = verb;
    deferred->origin = Py_NewRef(context->origin);
    deferred->answer = NULL;
    deferred->raised = NULL;
    memset(deferred->call, 0, verb->call_bytes);
    PyObject_GC_Track(deferred);
    return deferred->call;
}

/** Answers @p call once it is final: what its progress callable raised, else its outcome's failure, else its answer. */
static PyObject* call_answer(ustore_py_verb_t const* verb, void* call) {
    ustore_py_call_t* const header = (ustore_py_call_t*)call;
    if (header->raised) {
        PyErr_SetRaisedException(header->raised);
        header->raised = NULL;
        return NULL;
    }
    return ustore_py_raise_on_outcome(&header->outcome) == 0 ? verb->answer(call) : NULL;
}

/** Lets go of what @p call gathered, keeping whatever exception is set. */
static void call_release(ustore_py_verb_t const* verb, void* call) {
    PyObject* const raised = PyErr_GetRaisedException();
    verb->release(call);
    PyErr_SetRaisedException(raised);
}

PyObject* ustore_py_call_run(ustore_py_context_t const* context, ustore_py_verb_t const* verb, void* call,
                             void* arguments) {
    Py_BEGIN_ALLOW_THREADS;
    verb->submit(arguments);
    Py_END_ALLOW_THREADS;
    if (context->completion == ustore_completion_deferred_k) return (PyObject*)deferred_of(call);
    PyObject* const answer = call_answer(verb, call);
    call_release(verb, call);
    return answer;
}

PyObject* ustore_py_call_discard(ustore_py_context_t const* context, ustore_py_verb_t const* verb, void* call) {
    call_release(verb, call);
    if (context->completion == ustore_completion_blocking_k) return NULL;
    Deferred* const deferred = deferred_of(call);
    deferred->verb = NULL;
    PyObject* const raised = PyErr_GetRaisedException();
    Py_DECREF(deferred);
    PyErr_SetRaisedException(raised);
    return NULL;
}

void ustore_py_call_release(ustore_py_call_t* call) {
    if (call->arena) ustore_py_arena_unclaim(call->arena);
    call->arena = NULL;
    Py_CLEAR(call->progress);
    Py_CLEAR(call->raised);
}

PyObject* ustore_py_call_answer_none(void* Py_UNUSED(call)) { Py_RETURN_NONE; }

int ustore_py_progress_from_object(PyObject* object, PyObject** progress) {
    *progress = NULL;
    if (object == Py_None) return 0;
    if (!PyCallable_Check(object)) {
        PyErr_Format(PyExc_TypeError, "The 'progress' option takes a callable or None, got %S", Py_TYPE(object));
        return -1;
    }
    *progress = object;
    return 0;
}

/**
 *  @brief Re-enters Python for one report of the call whose header is @p state, from whichever thread runs it.
 *
 *  Reports never overlap and never follow the final status, but nothing places them on the calling thread, so the
 *  interpreter is attached here rather than assumed. An answer of None carries on, as does a truthy one, while a
 *  falsy one cancels, and so does a raise, whose exception the header keeps for the answer.
 */
static ustore_continuation_t progress_trampoline(ustore_size_t finished, ustore_size_t total, void* state) {
    ustore_py_call_t* const call = (ustore_py_call_t*)state;
    if (call->raised) return ustore_cancel_k;
    PyGILState_STATE const interpreter = PyGILState_Ensure();
    ustore_continuation_t continuation = ustore_cancel_k;
    PyObject* const finished_object = PyLong_FromUnsignedLongLong(finished);
    PyObject* const total_object = finished_object ? PyLong_FromUnsignedLongLong(total) : NULL;
    PyObject* const arguments[2] = {finished_object, total_object};
    PyObject* const verdict = total_object ? PyObject_Vectorcall(call->progress, arguments, 2, NULL) : NULL;
    if (verdict) {
        int const truth = verdict == Py_None ? 1 : PyObject_IsTrue(verdict);
        if (truth > 0) continuation = ustore_continue_k;
        Py_DECREF(verdict);
    }
    Py_XDECREF(total_object);
    Py_XDECREF(finished_object);
    if (PyErr_Occurred()) call->raised = PyErr_GetRaisedException();
    PyGILState_Release(interpreter);
    return continuation;
}

ustore_progress_callback_t ustore_py_progress_callback(ustore_py_call_t const* call) {
    return call->progress ? progress_trampoline : NULL;
}

#pragma endregion Calls

#pragma region Deferred Calls

static char const doc_Deferred[] =                                                                //
    "A call a deferred view submitted, which answers once it finishes.\n"                         //
    "\n"                                                                                          //
    "Built by the verbs of a DeferredView, never by name. `await` suspends the coroutine until\n" //
    "the call is final, running `wait()` on the event loop's own executor, and `wait()` blocks\n" //
    "the thread instead. Either answers what the blocking verb would have answered, or raises\n"  //
    "what it would have raised, as many times as it is asked; `status` polls without waiting.\n"  //
    "The object holds the outcome, the keys and buffers the call borrows, its output slots and\n" //
    "its arena until the call finishes, so dropping it waits for that, and another call naming\n" //
    "the arena raises ContendedError until it is waited on or dropped.\n"                         //
    "\n"                                                                                          //
    "Example:\n"                                                                                  //
    "  >>> async def read(database, executor, keys):\n"                                           //
    "  ...     reading = database.deferred(executor).read(keys, ustore.Arena(executor))\n"        //
    "  ...     return await reading  # or: reading.wait()";

/** The outcome of the call @p deferred holds. */
static ustore_outcome_t* deferred_outcome(Deferred* deferred) {
    return &((ustore_py_call_t*)(void*)deferred->call)->outcome;
}

/** The status of the call @p deferred holds, as the library finishes it elsewhere. */
static ustore_status_t deferred_status(Deferred* deferred) { return ustore_outcome_status(deferred_outcome(deferred)); }

/** Blocks with the thread state detached until the call @p deferred holds is final. */
static void deferred_block(Deferred* deferred) {
    if (deferred_status(deferred) != ustore_status_deferred_k) return;
    ustore_outcome_t* const outcome = deferred_outcome(deferred);
    Py_BEGIN_ALLOW_THREADS;
    ustore_outcome_wait(outcome);
    Py_END_ALLOW_THREADS;
}

/**
 *  @brief Finishes a final @p self exactly once: the thread taking its verb answers, publishes, then lets go of its
 *      inputs and its arena.
 *
 *  Answering may run Python, which lets another thread in even under the GIL, so the verb is taken before answering
 *  and the answer published after it, never one check spanning both.
 */
static void deferred_finish(Deferred* self) {
    ustore_py_verb_t const* finishing = NULL;
    ustore_py_critical_section_begin_((PyObject*)self);
    finishing = self->verb;
    self->verb = NULL;
    ustore_py_critical_section_end_();
    if (!finishing) return;
    PyObject* const answer = call_answer(finishing, self->call);
    PyObject* const raised = answer ? NULL : PyErr_GetRaisedException();
    ustore_py_critical_section_begin_((PyObject*)self);
    self->answer = answer;
    self->raised = raised;
    ustore_py_critical_section_end_();
    call_release(finishing, self->call);
}

/** Answers what a final @p self finished with, finishing it first when nobody has. */
static PyObject* deferred_answer(Deferred* self) {
    deferred_finish(self);
    for (;;) {
        PyObject* answer = NULL;
        int published = 0;
        ustore_py_critical_section_begin_((PyObject*)self);
        published = self->answer || self->raised;
        if (self->raised) PyErr_SetRaisedException(Py_NewRef(self->raised));
        else if (self->answer) answer = Py_NewRef(self->answer);
        ustore_py_critical_section_end_();
        if (published) return answer;
        // Another thread took the verb and is still answering, so this one yields until it publishes.
        Py_BEGIN_ALLOW_THREADS;
        Py_END_ALLOW_THREADS;
    }
}

static char const doc_Deferred_status[] =                                                        //
    "The status of the call: `Status.DEFERRED` until it finishes, then the final one, such as\n" //
    "`Status.SUCCESS`. Calls `ustore_outcome_status`.";

static PyObject* Deferred_get_status(Deferred* self, void* Py_UNUSED(closure)) {
    ustore_status_t const status = deferred_status(self);
    // A poller that saw the call final never needs to wait, so the inputs are let go of here too
    if (status != ustore_status_deferred_k) deferred_finish(self);
    return ustore_py_enum_to_object(ustore_py_enum_status_k, (int)status);
}

static char const doc_Deferred_wait[] =                                                      //
    "wait() answers what the call answers, once it finishes.\n"                              //
    "\n"                                                                                     //
    "Blocks the calling thread, letting other threads run, until the call finishes, and\n"   //
    "raises what the blocking verb would have raised, ContendedError included; `await` is\n" //
    "the native form. Calls `ustore_outcome_wait`.";

static PyObject* Deferred_wait(Deferred* self, PyObject* Py_UNUSED(ignored)) {
    deferred_block(self);
    return deferred_answer(self);
}

/** Answers the iterator of an `asyncio.Future` that `wait()` resolves on the running loop's default executor. */
static PyObject* Deferred_await(Deferred* self) {
    PyObject* const asyncio = PyImport_ImportModule("asyncio");
    PyObject* const loop = asyncio ? PyObject_CallMethod(asyncio, "get_running_loop", NULL) : NULL;
    Py_XDECREF(asyncio);
    PyObject* const wait = loop ? PyObject_GetAttrString((PyObject*)self, "wait") : NULL;
    PyObject* const future = wait ? PyObject_CallMethod(loop, "run_in_executor", "OO", Py_None, wait) : NULL;
    Py_XDECREF(wait);
    Py_XDECREF(loop);
    PyObject* const awaiting = future ? PyObject_CallMethod(future, "__await__", NULL) : NULL;
    Py_XDECREF(future);
    return awaiting;
}

static void Deferred_dealloc(Deferred* self) {
    PyObject_GC_UnTrack(self);
    // The library writes into the call this object holds, and reads the inputs it borrows, until the call finishes
    if (self->verb) {
        deferred_block(self);
        call_release(self->verb, self->call);
    }
    Py_XDECREF(self->answer);
    Py_XDECREF(self->raised);
    Py_XDECREF(self->origin);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

/** Visits what @p self holds; an unfinished call's arena and progress callable stay unvisited, so every cycle through
 *  them waits for a later collection rather than being cleared while the library still uses them. */
static int Deferred_traverse(Deferred* self, visitproc visit, void* arg) {
    Py_VISIT(self->origin);
    Py_VISIT(self->answer);
    Py_VISIT(self->raised);
    if (!self->verb || deferred_status(self) == ustore_status_deferred_k) return 0;
    ustore_py_call_t const* const header = (ustore_py_call_t const*)(void const*)self->call;
    Py_VISIT(header->arena);
    Py_VISIT(header->progress);
    Py_VISIT(header->raised);
    return 0;
}

/** Breaks a garbage cycle through a final @p self; an unfinished one is left whole, as its traversal hid the cycle. */
static int Deferred_clear(Deferred* self) {
    if (deferred_status(self) == ustore_status_deferred_k) return 0;
    ustore_py_verb_t const* const releasing = self->verb;
    self->verb = NULL;
    if (releasing) call_release(releasing, self->call);
    Py_CLEAR(self->answer);
    Py_CLEAR(self->raised);
    Py_CLEAR(self->origin);
    return 0;
}

static PyObject* Deferred_repr(Deferred* self) {
    return PyUnicode_FromFormat("Deferred(status=%s)", ustore_status_name(deferred_status(self)));
}

static PyMethodDef Deferred_methods[] = {
    {"wait", (PyCFunction)(void (*)(void))Deferred_wait, METH_NOARGS, doc_Deferred_wait},
    {"__class_getitem__", Py_GenericAlias, METH_O | METH_CLASS, "Names what `wait()` answers, as `Deferred[Values]`."},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef Deferred_getset[] = {
    {"status", (getter)Deferred_get_status, NULL, doc_Deferred_status, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyAsyncMethods Deferred_as_async = {
    .am_await = (unaryfunc)Deferred_await,
};

PyTypeObject DeferredType = {
    PyVarObject_HEAD_INIT(NULL, 0) //
        .tp_name = "ustore.Deferred",
    .tp_doc = doc_Deferred,
    .tp_basicsize = sizeof(Deferred),
    .tp_itemsize = 1,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_new = NULL,
    .tp_dealloc = (destructor)Deferred_dealloc,
    .tp_traverse = (traverseproc)Deferred_traverse,
    .tp_clear = (inquiry)Deferred_clear,
    .tp_repr = (reprfunc)Deferred_repr,
    .tp_as_async = &Deferred_as_async,
    .tp_methods = Deferred_methods,
    .tp_getset = Deferred_getset,
};

#pragma endregion Deferred Calls

#pragma region Executors

static char const doc_Executor[] =                                                             //
    "The threads that run calls, shared by any number of databases.\n"                         //
    "\n"                                                                                       //
    "Built through `Executor.for_ram()`, `on_pool()`, `for_nvme()` or `for_vram()`, never\n"   //
    "by name. A receiver names it through `on(executor)`, whose calls block on it, or\n"       //
    "`deferred(executor)`, whose calls it runs while the caller carries on. Calls start in\n"  //
    "submission order, and a blocking call runs alone: after every call submitted before it\n" //
    "finishes. Its deferred table holds `deferred_limit` calls, and a submission that finds\n" //
    "it full raises ContendedError once waited for.\n"                                         //
    "\n"                                                                                       //
    "Used as a context manager, it waits for its calls and frees its threads on exit.";

static PyObject* Executor_tp_new(PyTypeObject* Py_UNUSED(type), PyObject* Py_UNUSED(args),
                                 PyObject* Py_UNUSED(keywords)) {
    PyErr_SetString(PyExc_TypeError,
                    "An executor is built through Executor.for_ram(), .on_pool(), .for_nvme() or .for_vram()");
    return NULL;
}

/**
 *  @brief Reads the arguments of an executor factory into @p values, each left NULL when the call carried none.
 *  @param[in] names The names of the arguments, in the order they may come by position.
 *  @param[in] positional How many of @p names may come by position.
 *  @param[in] keyword The first of @p names that may come by name, so those before it come by position alone.
 *  @return 0 on success, -1 with a @c TypeError set for one too many, unknown or repeated.
 */
static int factory_arguments(char const* factory, PyObject* const* args, Py_ssize_t positional_args_count,
                             PyObject* args_names_tuple, char const* const* names, PyObject** values,
                             Py_ssize_t names_count, Py_ssize_t positional, Py_ssize_t keyword) {
    if (positional_args_count > positional) {
        PyErr_Format(PyExc_TypeError, "%s() takes at most %zd positional arguments, received %zd", factory, positional,
                     positional_args_count);
        return -1;
    }
    for (Py_ssize_t index = 0; index != names_count; ++index)
        values[index] = index < positional_args_count ? args[index] : NULL;
    Py_ssize_t const args_names_count = args_names_tuple ? PyTuple_GET_SIZE(args_names_tuple) : 0;
    for (Py_ssize_t names_progress = 0; names_progress != args_names_count; ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(args_names_tuple, names_progress);
        Py_ssize_t index = keyword;
        while (index != names_count && PyUnicode_CompareWithASCIIString(name, names[index]) != 0) ++index;
        if (index == names_count || values[index]) {
            PyErr_Format(PyExc_TypeError, "%s() got an unexpected or repeated argument %R", factory, name);
            return -1;
        }
        values[index] = args[positional_args_count + names_progress];
    }
    return 0;
}

/**
 *  @brief Copies @p object, None or a sequence of identifiers, such as NUMA nodes or CUDA ordinals, into a new block
 *      the caller frees.
 *  @param[in] option The keyword the sequence came in, as a failure names it.
 *  @param[out] identifiers The identifiers, or NULL for None, which derives every one.
 *  @param[out] count Identifiers the block holds.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
static int identifiers_from_object(PyObject* object, char const* option, int32_t** identifiers, size_t* count) {
    *identifiers = NULL;
    *count = 0;
    if (!object || object == Py_None) return 0;
    PyObject* const items = PySequence_Fast(object, "The option takes a sequence of integers");
    if (!items) return -1;
    Py_ssize_t const length = PySequence_Fast_GET_SIZE(items);
    *identifiers = PyMem_New(int32_t, (size_t)length + 1);
    if (!*identifiers) {
        Py_DECREF(items);
        PyErr_NoMemory();
        return -1;
    }
    for (Py_ssize_t index = 0; index != length; ++index) {
        long const identifier = PyLong_AsLong(PySequence_Fast_GET_ITEM(items, index));
        if (identifier == -1 && PyErr_Occurred()) break;
        if (identifier < INT32_MIN || identifier > INT32_MAX) {
            PyErr_Format(PyExc_OverflowError, "Every entry of '%s' fits 32 bits, unlike %ld", option, identifier);
            break;
        }
        (*identifiers)[index] = (int32_t)identifier;
    }
    Py_DECREF(items);
    if (PyErr_Occurred()) {
        PyMem_Free(*identifiers);
        *identifiers = NULL;
        return -1;
    }
    *count = (size_t)length;
    return 0;
}

/** Wraps @p handle, which a factory made unless @p outcome failed, in a new @c Executor of @p type. */
static PyObject* executor_made(PyObject* type, ustore_executor_t handle, ustore_outcome_t* outcome) {
    Executor* const instance = ustore_py_raise_on_outcome(outcome) == 0
                                   ? (Executor*)((PyTypeObject*)type)->tp_alloc((PyTypeObject*)type, 0)
                                   : NULL;
    if (!instance) {
        ustore_executor_free(handle);
        return NULL;
    }
    instance->handle = handle;
    return (PyObject*)instance;
}

static char const doc_Executor_for_ram[] =                                                       //
    "for_ram(threads=0, *, memory_domains=None, deferred_limit=0) answers an executor of host\n" //
    "threads.\n"                                                                                 //
    "\n"                                                                                         //
    "Runs the calls of the RAM, RocksDB and LevelDB engines, keeping the scratch of every\n"     //
    "thread on its local memory domain and every arena it makes on the first one named. Calls\n" //
    "`ustore_executor_for_ram`.\n"                                                               //
    "\n"                                                                                         //
    "Args:\n"                                                                                    //
    "  threads (int, optional): Threads to spawn; zero takes 7/8 of the cores the memory\n"      //
    "    domains carry, leaving the rest to blocking callers and the engines' own background\n"  //
    "    work.\n"                                                                                //
    "  memory_domains (Sequence[int], optional): NUMA nodes the threads stay near and keep\n"    //
    "    their memory on; None derives every node the process may allocate on.\n"                //
    "  deferred_limit (int, optional): Deferred calls the executor holds at once; zero takes\n"  //
    "    the library's default.\n"                                                               //
    "Returns:\n"                                                                                 //
    "  Executor: The new executor.\n"                                                            //
    "Raises:\n"                                                                                  //
    "  InvalidArgumentError: A memory domain names no NUMA node.";

static PyObject* Executor_for_ram(PyObject* type, PyObject* const* args, Py_ssize_t positional_args_count,
                                  PyObject* args_names_tuple) {
    static char const* const names[] = {"threads", "memory_domains", "deferred_limit"};
    PyObject* values[3];
    size_t threads = 0, deferred_limit = 0, memory_domains_count = 0;
    int32_t* memory_domains = NULL;
    if (factory_arguments("for_ram", args, positional_args_count, args_names_tuple, names, values, 3, 1, 0) != 0 ||
        (values[0] && ustore_py_size_from_object(values[0], "threads", &threads) != 0) ||
        (values[2] && ustore_py_size_from_object(values[2], "deferred_limit", &deferred_limit) != 0) ||
        identifiers_from_object(values[1], "memory_domains", &memory_domains, &memory_domains_count) != 0)
        return NULL;

    ustore_executor_t handle = NULL;
    ustore_outcome_t outcome = {0};
    ustore_executor_for_ram_t made = {
        .executor = &handle,
        .outcome = &outcome,
        .ram = {memory_domains, (ustore_size_t)memory_domains_count},
        .threads = (ustore_size_t)threads,
        .deferred_limit = (ustore_size_t)deferred_limit,
    };
    Py_BEGIN_ALLOW_THREADS;
    ustore_executor_for_ram(&made);
    Py_END_ALLOW_THREADS;
    PyMem_Free(memory_domains);
    return executor_made(type, handle, &outcome);
}

static char const doc_Executor_on_pool[] =                                                      //
    "on_pool(pool, /, *, memory_domains=None, deferred_limit=0) answers an executor over a\n"   //
    "ForkUnion pool another library already runs.\n"                                            //
    "\n"                                                                                        //
    "Runs the calls of the RAM, RocksDB and LevelDB engines on the borrowed threads, so one\n"  //
    "process keeps one set of them, while the executor keeps its own scratch and deferred\n"    //
    "table. The pool keeps its lender's mood. Calls `ustore_executor_on_pool`.\n"               //
    "\n"                                                                                        //
    "Args:\n"                                                                                   //
    "  pool (int): The address of the `fu_pool_t` to borrow, as `ctypes` and `cffi` hand\n"     //
    "    pointers over; the pool must outlive the executor.\n"                                  //
    "  memory_domains (Sequence[int], optional): NUMA nodes the scratch and the arenas\n"       //
    "    live on; None derives every node the process may allocate on.\n"                       //
    "  deferred_limit (int, optional): Deferred calls the executor holds at once; zero takes\n" //
    "    the library's default.\n"                                                              //
    "Returns:\n"                                                                                //
    "  Executor: The new executor.\n"                                                           //
    "Raises:\n"                                                                                 //
    "  InvalidArgumentError: The address is null, or a memory domain names no NUMA node.";

static PyObject* Executor_on_pool(PyObject* type, PyObject* const* args, Py_ssize_t positional_args_count,
                                  PyObject* args_names_tuple) {
    static char const* const names[] = {"pool", "memory_domains", "deferred_limit"};
    PyObject* values[3];
    size_t deferred_limit = 0, memory_domains_count = 0;
    if (factory_arguments("on_pool", args, positional_args_count, args_names_tuple, names, values, 3, 1, 1) != 0 ||
        (values[2] && ustore_py_size_from_object(values[2], "deferred_limit", &deferred_limit) != 0))
        return NULL;
    if (!values[0]) {
        PyErr_SetString(PyExc_TypeError, "on_pool() takes the address of the pool by position");
        return NULL;
    }
    PyObject* const address = PyNumber_Index(values[0]);
    void* const pool = address ? PyLong_AsVoidPtr(address) : NULL;
    Py_XDECREF(address);
    int32_t* memory_domains = NULL;
    if (PyErr_Occurred() ||
        identifiers_from_object(values[1], "memory_domains", &memory_domains, &memory_domains_count) != 0)
        return NULL;

    ustore_executor_t handle = NULL;
    ustore_outcome_t outcome = {0};
    ustore_executor_on_pool_t made = {
        .executor = &handle,
        .outcome = &outcome,
        .ram = {memory_domains, (ustore_size_t)memory_domains_count},
        .pool = pool,
        .deferred_limit = (ustore_size_t)deferred_limit,
    };
    Py_BEGIN_ALLOW_THREADS;
    ustore_executor_on_pool(&made);
    Py_END_ALLOW_THREADS;
    PyMem_Free(memory_domains);
    return executor_made(type, handle, &outcome);
}

static char const doc_Executor_for_nvme[] =                                                     //
    "for_nvme(threads=0, *, transport=Transport.AUTOMATIC, vram=None, deferred_limit=0,\n"      //
    "**sizes) answers an executor of I/O rings.\n"                                              //
    "\n"                                                                                        //
    "Runs the calls of the NVMe engine, every thread submitting and polling a ring of its\n"    //
    "own, and tells every database Database.open_nvme() opens on it how to move bytes. Every\n" //
    "size keyword names a field of ustore_nvme_options_t: shared_io_channels, queue_depth,\n"   //
    "request_buffer_bytes, request_buffers_count, stream_buffer_bytes or\n"                     //
    "stream_buffers_count, where zero keeps the engine's default. Calls\n"                      //
    "`ustore_executor_for_nvme`.\n"                                                             //
    "\n"                                                                                        //
    "Args:\n"                                                                                   //
    "  threads (int, optional): Threads to spawn; zero derives them from the cores.\n"          //
    "  transport (Transport, optional): How bytes move between the files and memory.\n"         //
    "  vram (Executor, optional): An executor of Executor.for_vram() whose devices the\n"       //
    "    compactions run on, kept until this one closes; None compacts on host threads.\n"      //
    "  deferred_limit (int, optional): Deferred calls the executor holds at once; zero\n"       //
    "    takes the library's default.\n"                                                        //
    "Returns:\n"                                                                                //
    "  Executor: The new executor.\n"                                                           //
    "Raises:\n"                                                                                 //
    "  DeviceError: The `vram` executor was not made by Executor.for_vram().";

/** The size keywords of @c Executor.for_nvme, in the order they follow its other arguments. */
static char const* const nvme_size_names[] = {"shared_io_channels",    "queue_depth",         "request_buffer_bytes",
                                              "request_buffers_count", "stream_buffer_bytes", "stream_buffers_count"};

static PyObject* Executor_for_nvme(PyObject* type, PyObject* const* args, Py_ssize_t positional_args_count,
                                   PyObject* args_names_tuple) {
    static char const* const names[] = {"threads",
                                        "transport",
                                        "vram",
                                        "deferred_limit",
                                        "shared_io_channels",
                                        "queue_depth",
                                        "request_buffer_bytes",
                                        "request_buffers_count",
                                        "stream_buffer_bytes",
                                        "stream_buffers_count"};
    PyObject* values[10];
    size_t threads = 0, deferred_limit = 0, sizes[6] = {0};
    int transport = ustore_transport_automatic_k;
    if (factory_arguments("for_nvme", args, positional_args_count, args_names_tuple, names, values, 10, 1, 0) != 0 ||
        (values[0] && ustore_py_size_from_object(values[0], "threads", &threads) != 0) ||
        (values[1] &&
         ustore_py_enum_from_object(values[1], "transport", ustore_py_enum_transport_k, &transport) != 0) ||
        (values[3] && ustore_py_size_from_object(values[3], "deferred_limit", &deferred_limit) != 0))
        return NULL;
    for (size_t index = 0; index != 6; ++index)
        if (values[4 + index] &&
            ustore_py_size_from_object(values[4 + index], nvme_size_names[index], &sizes[index]) != 0)
            return NULL;
    PyObject* const vram = values[2] && values[2] != Py_None ? values[2] : NULL;
    if (vram && !PyObject_TypeCheck(vram, &ExecutorType)) {
        PyErr_Format(PyExc_TypeError, "The 'vram' option takes an Executor or None, got %S", Py_TYPE(vram));
        return NULL;
    }
    if (vram && !((Executor*)vram)->handle) {
        ustore_py_raise(ustore_status_uninitialized_k, "The executor is closed");
        return NULL;
    }

    ustore_executor_t handle = NULL;
    ustore_outcome_t outcome = {0};
    ustore_executor_for_nvme_t made = {
        .executor = &handle,
        .outcome = &outcome,
        .nvme =
            {
                .transport = (ustore_transport_t)transport,
                .shared_io_channels = (ustore_size_t)sizes[0],
                .queue_depth = (ustore_size_t)sizes[1],
                .request_buffer_bytes = (ustore_size_t)sizes[2],
                .request_buffers_count = (ustore_size_t)sizes[3],
                .stream_buffer_bytes = (ustore_size_t)sizes[4],
                .stream_buffers_count = (ustore_size_t)sizes[5],
            },
        .vram = vram ? ((Executor*)vram)->handle : NULL,
        .threads = (ustore_size_t)threads,
        .deferred_limit = (ustore_size_t)deferred_limit,
    };
    Py_BEGIN_ALLOW_THREADS;
    ustore_executor_for_nvme(&made);
    Py_END_ALLOW_THREADS;
    PyObject* const made_object = executor_made(type, handle, &outcome);
    if (made_object && vram) ((Executor*)made_object)->borrowed = Py_NewRef(vram);
    return made_object;
}

static char const doc_Executor_for_vram[] =                                                  //
    "for_vram(devices=None, *, scratch_limit=0, deferred_limit=0) answers an executor of\n"  //
    "CUDA devices.\n"                                                                        //
    "\n"                                                                                     //
    "One host thread drives each device. No engine places a database in VRAM yet, so it\n"   //
    "serves as the `vram` of Executor.for_nvme(), whose compactions run on its devices.\n"   //
    "Calls `ustore_executor_for_vram`.\n"                                                    //
    "\n"                                                                                     //
    "Args:\n"                                                                                //
    "  devices (Sequence[int], optional): CUDA ordinals; None takes every visible device.\n" //
    "  scratch_limit (int, optional): Bytes of device memory the scratch may take on each\n" //
    "    device; zero takes 256 MiB.\n"                                                      //
    "  deferred_limit (int, optional): Deferred calls the executor holds at once; zero\n"    //
    "    takes the library's default.\n"                                                     //
    "Returns:\n"                                                                             //
    "  Executor: The new executor.\n"                                                        //
    "Raises:\n"                                                                              //
    "  UnsupportedError: This build carries no CUDA devices.\n"                              //
    "  DeviceError: No device is visible, or an ordinal names none.";

static PyObject* Executor_for_vram(PyObject* type, PyObject* const* args, Py_ssize_t positional_args_count,
                                   PyObject* args_names_tuple) {
    static char const* const names[] = {"devices", "scratch_limit", "deferred_limit"};
    PyObject* values[3];
    size_t scratch_limit = 0, deferred_limit = 0, devices_count = 0;
    int32_t* devices = NULL;
    if (factory_arguments("for_vram", args, positional_args_count, args_names_tuple, names, values, 3, 1, 0) != 0 ||
        (values[1] && ustore_py_size_from_object(values[1], "scratch_limit", &scratch_limit) != 0) ||
        (values[2] && ustore_py_size_from_object(values[2], "deferred_limit", &deferred_limit) != 0) ||
        identifiers_from_object(values[0], "devices", &devices, &devices_count) != 0)
        return NULL;

    ustore_executor_t handle = NULL;
    ustore_outcome_t outcome = {0};
    ustore_executor_for_vram_t made = {
        .executor = &handle,
        .outcome = &outcome,
        .devices = devices,
        .devices_count = (ustore_size_t)devices_count,
        .scratch_limit = (ustore_size_t)scratch_limit,
        .deferred_limit = (ustore_size_t)deferred_limit,
    };
    Py_BEGIN_ALLOW_THREADS;
    ustore_executor_for_vram(&made);
    Py_END_ALLOW_THREADS;
    PyMem_Free(devices);
    return executor_made(type, handle, &outcome);
}

/** Frees the threads of @p executor once its calls finish, leaving the object to refuse further calls. */
static void executor_close(Executor* executor) {
    ustore_executor_t handle = NULL;
    PyObject* borrowed = NULL;
    ustore_py_critical_section_begin_((PyObject*)executor);
    handle = executor->handle;
    executor->handle = NULL;
    borrowed = executor->borrowed;
    executor->borrowed = NULL;
    ustore_py_critical_section_end_();
    if (handle) {
        Py_BEGIN_ALLOW_THREADS;
        ustore_executor_free(handle);
        Py_END_ALLOW_THREADS;
    }
    // The borrowed executor goes only once the one borrowing it has freed its threads
    Py_XDECREF(borrowed);
}

static void Executor_dealloc(Executor* self) {
    executor_close(self);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static char const doc_Executor_drain[] =                                                    //
    "drain(mood=Mood.PARKED) answers None.\n"                                               //
    "\n"                                                                                    //
    "Waits for every call submitted before the drain began, so later submissions never\n"   //
    "delay it.\n"                                                                           //
    "\n"                                                                                    //
    "Args:\n"                                                                               //
    "  mood (Mood, optional): What the threads do once the executor next goes idle:\n"      //
    "    `Mood.PARKED` pays a wake-up on the next call, `Mood.SPINNING` trades power for\n" //
    "    its latency.";

static PyObject* Executor_drain(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                PyObject* args_names_tuple) {
    PyObject* mood_object = NULL;
    if (ustore_py_optional_argument("drain", args, positional_args_count, args_names_tuple, 0, "mood", &mood_object) !=
        0)
        return NULL;
    int mood = ustore_mood_parked_k;
    if (mood_object && ustore_py_enum_from_object(mood_object, "mood", ustore_py_enum_mood_k, &mood) != 0) return NULL;
    Executor* const executor = (Executor*)self;
    if (!executor->handle) {
        ustore_py_raise(ustore_status_uninitialized_k, "The executor is closed");
        return NULL;
    }

    ustore_outcome_t outcome = {0};
    ustore_executor_drain_t drain = {0};
    drain.executor = executor->handle;
    drain.outcome = &outcome;
    drain.mood = (ustore_mood_t)mood;
    Py_BEGIN_ALLOW_THREADS;
    ustore_executor_drain(&drain);
    Py_END_ALLOW_THREADS;
    if (ustore_py_raise_on_outcome(&outcome) != 0) return NULL;
    Py_RETURN_NONE;
}

static char const doc_Executor_close[] =                                                       //
    "close() answers None.\n"                                                                  //
    "\n"                                                                                       //
    "Waits for every submitted call, then frees the threads. A receiver naming the executor\n" //
    "refuses calls afterwards, and closing twice is safe.";

static PyObject* Executor_close(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    executor_close((Executor*)self);
    Py_RETURN_NONE;
}

static PyObject* Executor_enter(PyObject* self, PyObject* Py_UNUSED(ignored)) { return Py_NewRef(self); }

static PyObject* Executor_exit(PyObject* self, PyObject* const* Py_UNUSED(args),
                               Py_ssize_t Py_UNUSED(positional_args_count), PyObject* Py_UNUSED(args_names_tuple)) {
    executor_close((Executor*)self);
    Py_RETURN_NONE;
}

static PyObject* Executor_repr(Executor* self) {
    if (!self->handle) return PyUnicode_FromString("Executor(<closed>)");
    return PyUnicode_FromFormat("Executor(at=%p)", (void*)self->handle);
}

static PyMethodDef Executor_methods[] = {
    {"for_ram", (PyCFunction)(void (*)(void))Executor_for_ram, ustore_py_method_flags_k | METH_CLASS,
     doc_Executor_for_ram},
    {"on_pool", (PyCFunction)(void (*)(void))Executor_on_pool, ustore_py_method_flags_k | METH_CLASS,
     doc_Executor_on_pool},
    {"for_nvme", (PyCFunction)(void (*)(void))Executor_for_nvme, ustore_py_method_flags_k | METH_CLASS,
     doc_Executor_for_nvme},
    {"for_vram", (PyCFunction)(void (*)(void))Executor_for_vram, ustore_py_method_flags_k | METH_CLASS,
     doc_Executor_for_vram},
    {"drain", (PyCFunction)(void (*)(void))Executor_drain, ustore_py_method_flags_k, doc_Executor_drain},
    {"close", (PyCFunction)(void (*)(void))Executor_close, METH_NOARGS, doc_Executor_close},
    {"__enter__", (PyCFunction)(void (*)(void))Executor_enter, METH_NOARGS, NULL},
    {"__exit__", (PyCFunction)(void (*)(void))Executor_exit, ustore_py_method_flags_k, NULL},
    {NULL, NULL, 0, NULL},
};

PyTypeObject ExecutorType = {
    PyVarObject_HEAD_INIT(NULL, 0) //
        .tp_name = "ustore.Executor",
    .tp_doc = doc_Executor,
    .tp_basicsize = sizeof(Executor),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = Executor_tp_new,
    .tp_dealloc = (destructor)Executor_dealloc,
    .tp_repr = (reprfunc)Executor_repr,
    .tp_methods = Executor_methods,
};

#pragma endregion Executors

#pragma region Module Types

PyTypeObject* CapabilitiesType = NULL;

static PyStructSequence_Field capabilities_fields[] = {
    {"transactions", "Whether the engine groups operations into transactions."},
    {"snapshots", "Whether the engine pins point-in-time states."},
    {NULL, NULL},
};

static PyStructSequence_Desc capabilities_description = {
    "ustore.Capabilities",
    "What an engine supports, as `Database.capabilities` reports it.",
    capabilities_fields,
    2,
};

PyTypeObject* MeasurementType = NULL;

static PyStructSequence_Field measurement_fields[] = {
    {"min_cardinality", "The fewest entries the range may hold."},
    {"max_cardinality", "The most entries the range may hold."},
    {"min_value_bytes", "The fewest bytes the values in the range may take."},
    {"max_value_bytes", "The most bytes the values in the range may take."},
    {"min_space_usage", "The fewest bytes the range may take on disk."},
    {"max_space_usage", "The most bytes the range may take on disk."},
    {NULL, NULL},
};

static PyStructSequence_Desc measurement_description = {
    "ustore.Measurement",
    "The bounds on what a key range holds, as `measure()` estimates them without walking it.",
    measurement_fields,
    6,
};

PyTypeObject* DescriptionType = NULL;

static PyStructSequence_Field description_fields[] = {
    {"capabilities", "What the engine supports, as a Capabilities."},
    {"engine", "The engine serving the database, an Engine."},
    {"isolation", "How transactions see each other: what the RAM engine was opened with, and SNAPSHOT elsewhere."},
    {"keys", "Keys across every collection, as the engine estimates them."},
    {"memory_bytes", "Bytes of memory the engine holds for the data, or zero where it keeps no estimate."},
    {"disk_bytes", "Bytes the files of the database take on disk."},
    {NULL, NULL},
};

static PyStructSequence_Desc description_description = {
    "ustore.Description",
    "What an open database reports about itself, as `describe()` answers it, every size the engine's estimate.",
    description_fields,
    6,
};

int ustore_py_types_import(void) {
    if (!CapabilitiesType) CapabilitiesType = PyStructSequence_NewType(&capabilities_description);
    if (!MeasurementType) MeasurementType = PyStructSequence_NewType(&measurement_description);
    if (!DescriptionType) DescriptionType = PyStructSequence_NewType(&description_description);
    return CapabilitiesType && MeasurementType && DescriptionType ? 0 : -1;
}

#pragma endregion Module Types
