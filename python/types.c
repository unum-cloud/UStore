/**
 *  @file python/types.c
 *  @author Ash Vardanian
 *  @date September 16, 2026
 *  @brief The vocabulary every modality shares: failures, arenas, batches and result tapes.
 *
 *  Keys, values and the arenas holding them live here rather than inside any one modality, so a
 *  tape a binary read exported is the very object a later verb can slice, because there is one
 *  definition and one address.
 */
#include "lib.h"

#pragma region Failures

PyObject* ErrorType = NULL;
PyObject* ArgumentErrorType = NULL;
PyObject* UnsupportedErrorType = NULL;
PyObject* MissingErrorType = NULL;
PyObject* ExistsErrorType = NULL;
PyObject* ConflictErrorType = NULL;
PyObject* MalformedErrorType = NULL;
PyObject* CapacityErrorType = NULL;
PyObject* StorageErrorType = NULL;

/** One status, the exception it raises, and the name it carries into Python. */
typedef struct status_naming_t {

    /** The status this row describes. */
    ustore_status_t status;

    /** The exception the status raises, read at raise time rather than at load time. */
    PyObject** kind;

    /** The lowercase, dash-separated spelling the exception carries. */
    char const* name;
} status_naming_t;

static status_naming_t const status_namings[] = {
    {ustore_error_out_of_memory_k, &CapacityErrorType, "out-of-memory"},
    {ustore_error_invalid_argument_k, &ArgumentErrorType, "invalid-argument"},
    {ustore_error_uninitialized_k, &ArgumentErrorType, "uninitialized"},
    {ustore_error_unsupported_k, &UnsupportedErrorType, "unsupported"},
    {ustore_error_io_k, &StorageErrorType, "io"},
    {ustore_error_malformed_k, &MalformedErrorType, "malformed"},
    {ustore_error_not_found_k, &MissingErrorType, "not-found"},
    {ustore_error_already_exists_k, &ExistsErrorType, "already-exists"},
    {ustore_error_write_conflict_k, &ConflictErrorType, "write-conflict"},
    {ustore_error_read_conflict_k, &ConflictErrorType, "read-conflict"},
    {ustore_error_phantom_conflict_k, &ConflictErrorType, "phantom-conflict"},
    {ustore_error_capacity_exhausted_k, &CapacityErrorType, "capacity-exhausted"},
    {ustore_error_device_k, &StorageErrorType, "device"},
    {ustore_error_internal_k, &StorageErrorType, "internal"},
};

static status_naming_t const* status_naming(ustore_status_t status) {
    for (size_t naming_index = 0; naming_index != ustore_py_table_size_(status_namings); ++naming_index)
        if (status_namings[naming_index].status == status) return &status_namings[naming_index];
    return NULL;
}

int ustore_py_raise(ustore_status_t status, char const* format, ...) {
    // A status this specific supersedes whatever the caller was carrying, and building the
    // instance below must not start with an exception already pending.
    PyErr_Clear();
    status_naming_t const* const naming = status_naming(status);
    PyObject* const kind = naming ? *naming->kind : ErrorType;

    va_list arguments;
    va_start(arguments, format);
    PyObject* const message = PyUnicode_FromFormatV(format ? format : "", arguments);
    va_end(arguments);
    if (!message) return -1;

    PyObject* const instance = PyObject_CallFunction(kind, "O", message);
    Py_DECREF(message);
    if (!instance) return -1;

    PyObject* const status_name = PyUnicode_FromString(naming ? naming->name : "unknown");
    if (!status_name) {
        Py_DECREF(instance);
        return -1;
    }
    // A status-less failure would misreport the fault, so a failure here is reported as itself
    // rather than papered over with a half-built instance.
    int const attached = PyObject_SetAttrString(instance, "status", status_name);
    Py_DECREF(status_name);
    if (attached != 0) {
        Py_DECREF(instance);
        return -1;
    }
    PyErr_SetObject(kind, instance);
    Py_DECREF(instance);
    return -1;
}

int ustore_py_raise_on_error(ustore_error_t const* error) {
    if (error->status == ustore_success_k) return 0;
    // The message is static and never freed, so it is handed straight to the formatter.
    return ustore_py_raise(error->status, error->message ? error->message : "the engine named no detail");
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

static char const doc_Error[] =                                                         //
    "Every failure the engine reports, carrying the status shared with the C ABI.\n"    //
    "\n"                                                                                //
    "A failure caught before the boundary reports the same identity the engine would\n" //
    "have reported for the same fault. Catch this to catch every one of them.\n"        //
    "\n"                                                                                //
    "Attributes:\n"                                                                     //
    "  status (str): Lowercase, dash-separated status, such as 'write-conflict'.\n"     //
    "  args (tuple): The detail the status cannot carry, as on any exception.";

static char const doc_ArgumentError[] =
    "An argument is null, contradicts another one, or names a handle that was never opened.";

static char const doc_UnsupportedError[] = "The engine or this build does not implement what the call asks for.";

static char const doc_MissingError[] = "A snapshot, collection or name the call addresses does not exist.";

static char const doc_ExistsError[] = "A snapshot or collection the call creates already exists.";

static char const doc_ConflictError[] =
    "A concurrent writer touched what this transaction read, wrote or scanned; retry it.";

static char const doc_MalformedError[] = "Stored or supplied data could not be decoded.";

static char const doc_CapacityError[] = "An allocation could not be satisfied, or a design ceiling was reached.";

static char const doc_StorageError[] =
    "A file, directory or device could not be reached, or the engine failed in a way no other status names.";

/** One exception type, where it is remembered, and the documentation it carries. */
typedef struct error_naming_t {

    /** The dotted name the type answers to. */
    char const* name;

    /** Where the readied type is remembered for the raising helpers. */
    PyObject** slot;

    /** The documentation the type carries. */
    char const* doc;
} error_naming_t;

static error_naming_t const error_namings[] = {
    {"ustore.ArgumentError", &ArgumentErrorType, doc_ArgumentError},
    {"ustore.UnsupportedError", &UnsupportedErrorType, doc_UnsupportedError},
    {"ustore.MissingError", &MissingErrorType, doc_MissingError},
    {"ustore.ExistsError", &ExistsErrorType, doc_ExistsError},
    {"ustore.ConflictError", &ConflictErrorType, doc_ConflictError},
    {"ustore.MalformedError", &MalformedErrorType, doc_MalformedError},
    {"ustore.CapacityError", &CapacityErrorType, doc_CapacityError},
    {"ustore.StorageError", &StorageErrorType, doc_StorageError},
};

/** Adds @p type to @p module under the last component of its dotted name. */
static int register_error(PyObject* module, char const* dotted_name, PyObject* type) {
    char const* const bare_name = strrchr(dotted_name, '.') + 1;
    Py_INCREF(type);
    if (PyModule_AddObject(module, bare_name, type) < 0) {
        Py_DECREF(type);
        return -1;
    }
    return 0;
}

int ustore_py_register_errors(PyObject* module) {
    ErrorType = PyErr_NewExceptionWithDoc("ustore.Error", (char*)doc_Error, PyExc_RuntimeError, NULL);
    if (!ErrorType) return -1;
    if (register_error(module, "ustore.Error", ErrorType) != 0) return -1;

    for (size_t naming_index = 0; naming_index != ustore_py_table_size_(error_namings); ++naming_index) {
        error_naming_t const* const naming = &error_namings[naming_index];
        PyObject* const type = PyErr_NewExceptionWithDoc((char*)naming->name, (char*)naming->doc, ErrorType, NULL);
        if (!type) return -1;
        *naming->slot = type;
        if (register_error(module, naming->name, type) != 0) return -1;
    }
    return 0;
}

#pragma endregion Failures

#pragma region Arenas

ustore_arena_t ustore_py_arena_claim(Database* database) {
    ustore_arena_t claimed = NULL;
    ustore_py_critical_section_begin_((PyObject*)database);
    claimed = database->arenas.idle;
    database->arenas.idle = NULL;
    ustore_py_critical_section_end_();
    return claimed;
}

void ustore_py_arena_return(Database* database, ustore_arena_t arena) {
    if (!arena) return;
    ustore_arena_t displaced = arena;
    ustore_py_critical_section_begin_((PyObject*)database);
    if (!database->arenas.idle) {
        database->arenas.idle = arena;
        displaced = NULL;
    }
    ustore_py_critical_section_end_();
    // A second arena is one more than a database lends, so whichever lost the slot is released
    // rather than kept for a call that would have made its own anyway.
    if (displaced) ustore_arena_free(displaced);
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

void ustore_py_keys_release(ustore_py_keys_t* batch) {
    if (batch->borrowed) PyBuffer_Release(&batch->buffer);
    if (batch->owned) PyMem_Free(batch->owned);
    batch->borrowed = 0;
    batch->owned = NULL;
    batch->keys = NULL;
    batch->count = 0;
}

#pragma endregion Keys

#pragma region Contents

int ustore_py_contents_reserve(ustore_py_contents_t* batch, size_t count) {
    memset(batch, 0, sizeof(*batch));
    if (count <= ustore_py_contents_inline_k) {
        batch->values = batch->inline_values;
        batch->lengths = batch->inline_lengths;
        batch->buffers = batch->inline_buffers;
        return 0;
    }

    // One block carves into the three runs, so a batch of any width is one allocation rather than
    // three, and the pointer run leads so the block is aligned for every one of them.
    size_t const widest = sizeof(ustore_bytes_cptr_t) + sizeof(Py_buffer) + sizeof(ustore_length_t);
    if (count > (size_t)PY_SSIZE_T_MAX / widest) {
        PyErr_NoMemory();
        return -1;
    }
    size_t const values_bytes = count * sizeof(ustore_bytes_cptr_t);
    size_t const buffers_bytes = count * sizeof(Py_buffer);
    size_t const lengths_bytes = count * sizeof(ustore_length_t);
    batch->block = PyMem_Malloc(values_bytes + buffers_bytes + lengths_bytes);
    if (!batch->block) {
        PyErr_NoMemory();
        return -1;
    }
    batch->values = (ustore_bytes_cptr_t*)batch->block;
    batch->buffers = (Py_buffer*)(void*)((char*)batch->block + values_bytes);
    batch->lengths = (ustore_length_t*)(void*)((char*)batch->block + values_bytes + buffers_bytes);
    return 0;
}

int ustore_py_contents_borrow(PyObject* object, ustore_py_contents_t* batch) {
    size_t const task = batch->count;
    if (object == Py_None) {
        // A null chunk is how the ABI spells a removal, so a task with no value needs no buffer.
        batch->values[task] = NULL;
        batch->lengths[task] = 0;
        ++batch->count;
        return 0;
    }

    Py_buffer* const buffer = &batch->buffers[batch->borrowed];
    if (PyObject_GetBuffer(object, buffer, PyBUF_SIMPLE) != 0)
        return ustore_py_raise_from_cause(PyExc_TypeError, "A value must be bytes-like or None, not %S",
                                          Py_TYPE(object));
    ++batch->borrowed;
    batch->values[task] = (ustore_bytes_cptr_t)buffer->buf;
    batch->lengths[task] = (ustore_length_t)buffer->len;
    ++batch->count;
    return 0;
}

void ustore_py_contents_release(ustore_py_contents_t* batch) {
    for (size_t buffer_index = 0; buffer_index != batch->borrowed; ++buffer_index)
        PyBuffer_Release(&batch->buffers[buffer_index]);
    batch->borrowed = 0;
    if (batch->block) PyMem_Free(batch->block);
    batch->block = NULL;
    batch->count = 0;
}

#pragma endregion Contents

#pragma region Spans

static char const doc_Span[] = "A read-only window into a result's arena, which a memoryview is built over.";

int ustore_py_buffer_fill(Py_buffer* view, PyObject* owner, void* data, char const* format, Py_ssize_t itemsize,
                          int ndim, Py_ssize_t* shape, Py_ssize_t* strides, int flags) {
    if ((flags & PyBUF_WRITABLE) == PyBUF_WRITABLE) {
        view->obj = NULL;
        PyErr_SetString(PyExc_BufferError, "The arena a result owns is read-only");
        return -1;
    }
    Py_ssize_t items = 1;
    for (int dimension = 0; dimension != ndim; ++dimension) items *= shape[dimension];

    // An empty run may carry no address at all, and a buffer must still point somewhere.
    view->buf = data ? data : (void*)owner;
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

PyObject* ustore_py_span_view(PyObject* owner, void* data, char const* format, Py_ssize_t itemsize, int ndim,
                              Py_ssize_t const* shape) {
    Span* const span = PyObject_New(Span, &SpanType);
    if (!span) return NULL;
    span->owner = Py_NewRef(owner);
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
    Py_XDECREF(self->owner);
    PyObject_Free(self);
}

static int Span_getbuffer(Span* self, Py_buffer* view, int flags) {
    return ustore_py_buffer_fill(view, (PyObject*)self, self->data, self->format, self->itemsize, self->ndim,
                                 self->shape, self->strides, flags);
}

static PyBufferProcs Span_as_buffer = {
    .bf_getbuffer = (getbufferproc)Span_getbuffer,
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

static char const doc_Values[] =                                                             //
    "The values one batch read exported, read straight out of the arena holding them.\n"     //
    "\n"                                                                                     //
    "Indexing answers a `memoryview` over the stored bytes, or None where the key was\n"     //
    "absent; nothing is copied, and the view keeps this object, and therefore the arena,\n"  //
    "alive for as long as it is held. `bytes(values[index])` is where a copy happens, and\n" //
    "nowhere before it.";

PyObject* ustore_py_values_adopt(ustore_arena_t arena, ustore_byte_t* tape, ustore_length_t* offsets,
                                 ustore_length_t* lengths, size_t count) {
    Values* const instance = PyObject_New(Values, &ValuesType);
    if (!instance) {
        ustore_arena_free(arena);
        return NULL;
    }
    instance->arena = arena;
    instance->tape = tape;
    instance->offsets = offsets;
    instance->lengths = lengths;
    instance->count = count;
    return (PyObject*)instance;
}

static void Values_dealloc(Values* self) {
    // Every `memoryview` over this tape holds a reference to the object exporting it, so the last
    // view has died by the time this runs and the arena is safe to release.
    ustore_arena_free(self->arena);
    PyObject_Free(self);
}

static Py_ssize_t Values_length(Values* self) { return (Py_ssize_t)self->count; }

static PyObject* Values_item(Values* self, Py_ssize_t task_index) {
    // `PySequence_GetItem` normalizes a negative index before reaching here.
    if (task_index < 0 || (size_t)task_index >= self->count) {
        PyErr_SetString(PyExc_IndexError, "Task index out of range");
        return NULL;
    }
    if (self->lengths[task_index] == ustore_length_missing_k) Py_RETURN_NONE;
    Py_ssize_t const shape = (Py_ssize_t)self->lengths[task_index];
    return ustore_py_span_view((PyObject*)self, self->tape + self->offsets[task_index], "B", 1, 1, &shape);
}

static int Values_getbuffer(Values* self, Py_buffer* view, int flags) {
    // A tape of no values still answers a buffer, so a caller need not special-case an empty read.
    Py_ssize_t const spanned = self->count ? (Py_ssize_t)self->offsets[self->count] : 0;
    void* const data = self->tape ? (void*)self->tape : (void*)self;
    return PyBuffer_FillInfo(view, (PyObject*)self, data, spanned, 1, flags);
}

static PyObject* Values_repr(Values* self) {
    Py_ssize_t const spanned = self->count ? (Py_ssize_t)self->offsets[self->count] : 0;
    return PyUnicode_FromFormat("Values(count=%zu, bytes=%zd)", self->count, spanned);
}

static char const doc_Values_lengths[] = "The length of every value, with None where the key was absent, as a tuple.";

static PyObject* Values_get_lengths(Values* self, void* Py_UNUSED(closure)) {
    PyObject* const lengths = PyTuple_New((Py_ssize_t)self->count);
    if (!lengths) return NULL;
    for (size_t task_index = 0; task_index != self->count; ++task_index) {
        ustore_length_t const length = self->lengths[task_index];
        PyObject* const entry = length == ustore_length_missing_k ? Py_NewRef(Py_None)
                                                                  : PyLong_FromUnsignedLongLong(length);
        if (!entry) {
            Py_DECREF(lengths);
            return NULL;
        }
        PyTuple_SET_ITEM(lengths, (Py_ssize_t)task_index, entry);
    }
    return lengths;
}

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
    .tp_getset = Values_getset,
};

#pragma endregion Value Tapes

#pragma region Key Tapes

static char const doc_Keys[] =                                                                //
    "The keys one batch scan or sample found, read straight out of the arena holding them.\n" //
    "\n"                                                                                      //
    "Indexing answers a tuple of `uuid.UUID` keys for one task, which is where a copy\n"      //
    "happens. The whole tape exports through the buffer protocol as an array of shape\n"      //
    "(keys, 16) bytes, so `numpy.asarray(keys)` reads the arena without copying, and keeps\n" //
    "this object, and therefore the arena, alive for as long as it is held.";

PyObject* ustore_py_keys_adopt(ustore_arena_t arena, ustore_key_t* tape, ustore_length_t* offsets,
                               ustore_length_t* counts, size_t count) {
    Keys* const instance = PyObject_New(Keys, &KeysType);
    if (!instance) {
        ustore_arena_free(arena);
        return NULL;
    }
    instance->arena = arena;
    instance->tape = tape;
    instance->offsets = offsets;
    instance->counts = counts;
    instance->count = count;
    instance->shape[0] = count ? (Py_ssize_t)offsets[count] : 0;
    instance->shape[1] = (Py_ssize_t)sizeof(ustore_key_t);
    instance->strides[0] = (Py_ssize_t)sizeof(ustore_key_t);
    instance->strides[1] = 1;
    return (PyObject*)instance;
}

static void Keys_dealloc(Keys* self) {
    ustore_arena_free(self->arena);
    PyObject_Free(self);
}

static Py_ssize_t Keys_length(Keys* self) { return (Py_ssize_t)self->count; }

static PyObject* Keys_item(Keys* self, Py_ssize_t task_index) {
    if (task_index < 0 || (size_t)task_index >= self->count) {
        PyErr_SetString(PyExc_IndexError, "Task index out of range");
        return NULL;
    }
    ustore_key_t const* const run = self->tape + self->offsets[task_index];
    Py_ssize_t const found = (Py_ssize_t)self->counts[task_index];
    PyObject* const keys = PyTuple_New(found);
    if (!keys) return NULL;
    for (Py_ssize_t key_index = 0; key_index != found; ++key_index) {
        PyObject* const key = ustore_py_uuid_to_object(&run[key_index].uuid);
        if (!key) {
            Py_DECREF(keys);
            return NULL;
        }
        PyTuple_SET_ITEM(keys, key_index, key);
    }
    return keys;
}

static int Keys_getbuffer(Keys* self, Py_buffer* view, int flags) {
    return ustore_py_buffer_fill(view, (PyObject*)self, self->tape, "B", 1, 2, self->shape, self->strides, flags);
}

static PyObject* Keys_repr(Keys* self) {
    size_t found = 0;
    for (size_t task_index = 0; task_index != self->count; ++task_index) found += self->counts[task_index];
    return PyUnicode_FromFormat("Keys(tasks=%zu, keys=%zu)", self->count, found);
}

static char const doc_Keys_counts[] = "The number of keys every task found, as a tuple.";

static PyObject* Keys_get_counts(Keys* self, void* Py_UNUSED(closure)) {
    PyObject* const counts = PyTuple_New((Py_ssize_t)self->count);
    if (!counts) return NULL;
    for (size_t task_index = 0; task_index != self->count; ++task_index) {
        PyObject* const entry = PyLong_FromUnsignedLongLong(self->counts[task_index]);
        if (!entry) {
            Py_DECREF(counts);
            return NULL;
        }
        PyTuple_SET_ITEM(counts, (Py_ssize_t)task_index, entry);
    }
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

int ustore_py_choice_from_object(PyObject* object, char const* field, ustore_py_choice_naming_t const* namings,
                                 int* value) {
    if (!PyUnicode_Check(object)) {
        PyErr_Format(PyExc_TypeError, "The '%s' option must be a string, got %S", field, Py_TYPE(object));
        return -1;
    }
    for (ustore_py_choice_naming_t const* naming = namings; naming->name; ++naming)
        if (PyUnicode_CompareWithASCIIString(object, naming->name) == 0) {
            *value = naming->value;
            return 0;
        }
    PyErr_Format(PyExc_ValueError, "Unsupported '%s' option %R", field, object);
    return -1;
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

int ustore_py_context_from_object(PyObject* self, ustore_py_context_t* context) {
    memset(context, 0, sizeof(*context));
    if (PyObject_TypeCheck(self, &DatabaseType)) {
        Database* const database = (Database*)self;
        if (!database->handle) {
            PyErr_SetString(PyExc_ValueError, "The database is closed");
            return -1;
        }
        context->database = database;
        return 0;
    }
    if (PyObject_TypeCheck(self, &TransactionType)) {
        Transaction* const transaction = (Transaction*)self;
        if (!transaction->handle) {
            PyErr_SetString(PyExc_ValueError, "The transaction is closed");
            return -1;
        }
        Database* const database = (Database*)transaction->database;
        if (!database->handle) {
            PyErr_SetString(PyExc_ValueError, "The database is closed");
            return -1;
        }
        context->database = database;
        context->transaction = transaction->handle;
        context->options = transaction->options;
        return 0;
    }
    PyErr_Format(PyExc_TypeError, "Expected a Database or a Transaction, got %S", Py_TYPE(self));
    return -1;
}

#pragma endregion Closed Sets

#pragma region Verb Options

void ustore_py_verb_options_init(ustore_py_context_t const* context, ustore_py_verb_options_t* options) {
    options->collection = ustore_collection_main_k;
    options->snapshot = NULL;
    options->options = context->options;
}

int ustore_py_verb_option_from_keyword(ustore_py_context_t const* context, PyObject* name, PyObject* value,
                                       ustore_py_verb_options_t* options) {
    if (PyUnicode_CompareWithASCIIString(name, "collection") == 0)
        return ustore_py_collection_from_object(context, value, &options->collection) == 0 ? 1 : -1;
    if (PyUnicode_CompareWithASCIIString(name, "snapshot") == 0) {
        if (value == Py_None) {
            options->snapshot = NULL;
            return 1;
        }
        // A call addresses exactly one view, so a transaction and a snapshot are never both named.
        if (context->transaction) {
            ustore_py_raise(ustore_error_invalid_argument_k,
                            "A transaction already fixes the view, so it takes no snapshot");
            return -1;
        }
        if (!PyObject_TypeCheck(value, &SnapshotType)) {
            PyErr_Format(PyExc_TypeError, "The 'snapshot' option must be a Snapshot or None, got %S", Py_TYPE(value));
            return -1;
        }
        Snapshot* const snapshot = (Snapshot*)value;
        if (!snapshot->handle) {
            PyErr_SetString(PyExc_ValueError, "The snapshot is closed");
            return -1;
        }
        if (snapshot->database != (PyObject*)context->database) {
            ustore_py_raise(ustore_error_invalid_argument_k, "The snapshot belongs to another database");
            return -1;
        }
        options->snapshot = snapshot->handle;
        return 1;
    }
    if (PyUnicode_CompareWithASCIIString(name, "flush") == 0) {
        int const flushing = PyObject_IsTrue(value);
        if (flushing < 0) return -1;
        if (flushing) options->options |= ustore_option_write_flush_k;
        return 1;
    }
    if (PyUnicode_CompareWithASCIIString(name, "watch") == 0) {
        int const watching = PyObject_IsTrue(value);
        if (watching < 0) return -1;
        // Only a transaction has a conflict set, so outside one the keyword would promise nothing.
        if (!context->transaction) {
            ustore_py_raise(ustore_error_invalid_argument_k, "Only a transaction watches what it touches");
            return -1;
        }
        if (watching) options->options &= (ustore_options_t) ~(unsigned)ustore_option_transaction_dont_watch_k;
        else options->options |= ustore_option_transaction_dont_watch_k;
        return 1;
    }
    return 0;
}

int ustore_py_verb_options_from_keywords(ustore_py_context_t const* context, PyObject* const* args,
                                         Py_ssize_t positional_args_count, PyObject* args_names_tuple,
                                         ustore_py_verb_options_t* options) {
    ustore_py_verb_options_init(context, options);
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
