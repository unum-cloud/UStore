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
     "The call needs a handle that is closed or was never opened."},
    {ustore_status_config_mismatch_k, "ustore.ConfigMismatchError", &ErrorType, &PyExc_ValueError,
     "The handles or arguments are each well-formed but cannot be joined, like devices of two vendors."},
    {ustore_status_contended_k, "ustore.ContendedError", &ErrorType, NULL,
     "An executor, arena or transaction serving another call refused the call instead of waiting."},
    {ustore_status_cancelled_k, "ustore.CancelledError", &ErrorType, NULL,
     "A progress callback stopped the call, which unwound and published nothing."},
    {ustore_status_overflow_k, "ustore.CapacityOverflowError", &ErrorType, &PyExc_OverflowError,
     "A count or size outgrew the integer holding it, like the open exports of one arena."},
    {ustore_status_unsupported_k, "ustore.UnsupportedError", &ErrorType, &PyExc_NotImplementedError,
     "The engine or this build does not implement what the call asks for."},
    {ustore_status_invalid_argument_k, "ustore.InvalidArgumentError", &ErrorType, &PyExc_ValueError,
     "An argument is null, contradicts another one, or holds a choice the call does not accept."},
    {ustore_status_not_found_k, "ustore.NotFoundError", &ErrorType, &PyExc_KeyError,
     "A collection, a directory, or a document a modification needs does not exist."},
    {ustore_status_already_exists_k, "ustore.AlreadyExistsError", &ErrorType, &PyExc_ValueError,
     "A collection name or a document the call creates is already taken, or a raw medium's first slot is not free."},
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
    // A status this specific supersedes whatever the caller was holding, and building the
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
    "Every failure the engine reports, holding the status shared with the C ABI.\n"             //
    "\n"                                                                                        //
    "Each failing status raises a subclass of its own, such as `NotFoundError`, which also\n"   //
    "derives the built-in naming the same fault where one exists, so `except KeyError` still\n" //
    "catches a `NotFoundError`. The three conflicts share `ConflictError`, the one base a\n"    //
    "retry loop catches. A failure caught before the boundary raises the class the engine\n"    //
    "would have raised for the same fault.\n"                                                   //
    "\n"                                                                                        //
    "Attributes:\n"                                                                             //
    "  status (Status): The status, such as `Status.WRITE_CONFLICT`.\n"                         //
    "  args (tuple): The detail the status cannot hold, as on any exception.";

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

static char const doc_Arena[] =                                                                //
    "Arena(executor=None, /)\n"                                                                //
    "\n"                                                                                       //
    "The memory every output whose length scales with a call lands in, from values and keys\n" //
    "to columns, edges and names. Every such verb takes one as its last positional argument\n" //
    "and rewinds it, so a steady loop allocates once.\n"                                       //
    "\n"                                                                                       //
    "The executor makes it where its calls compute, and none makes host pages for calls on\n"  //
    "the calling thread. Results are zero-copy views over it, valid until the next call\n"     //
    "naming it rewinds it; a result read after that reads whatever it then holds, which\n"     //
    "nothing detects. A call naming it while another still fills it raises ContendedError,\n"  //
    "so each deferred call in flight needs one of its own. Calls `ustore_arena_init`.\n"       //
    "\n"                                                                                       //
    "Args:\n"                                                                                  //
    "  executor (Executor, optional): The executor whose calls it serves, closed only after\n" //
    "    the arena is freed; None serves the calling thread.";

/** A new arena of @p type made where @p executor computes, or NULL with an exception set. */
static PyObject* arena_made(PyTypeObject* type, Executor* executor) {
    if (executor && !executor->handle)
        return ustore_py_raise(ustore_status_uninitialized_k, "The executor is closed"), NULL;
    ustore_arena_t handle = NULL;
    ustore_outcome_t outcome = {0};
    ustore_arena_init_t init = {.arena = &handle, .executor = executor ? executor->handle : NULL, .outcome = &outcome};
    ustore_arena_init(&init);
    if (ustore_py_raise_on_outcome(&outcome) != 0) return NULL;
    Arena* const arena = (Arena*)type->tp_alloc(type, 0);
    if (!arena) {
        ustore_arena_free(handle);
        return NULL;
    }
    arena->handle = handle;
    arena->executor = (Executor*)Py_XNewRef((PyObject*)executor);
    return (PyObject*)arena;
}

PyObject* ustore_py_arena_new(Executor* executor) { return arena_made(&ArenaType, executor); }

static PyObject* Arena_tp_new(PyTypeObject* type, PyObject* arguments, PyObject* keywords) {
    Py_ssize_t const positional_arguments_count = PyTuple_GET_SIZE(arguments);
    if (positional_arguments_count > 1 || (keywords && PyDict_GET_SIZE(keywords) != 0)) {
        PyErr_SetString(PyExc_TypeError, "Arena() takes one optional positional argument, the executor");
        return NULL;
    }
    PyObject* const executor_object = positional_arguments_count ? PyTuple_GET_ITEM(arguments, 0) : Py_None;
    if (executor_object == Py_None) return arena_made(type, NULL);
    if (!PyObject_TypeCheck(executor_object, &ExecutorType)) {
        PyErr_Format(PyExc_TypeError, "Expected an Executor or None, got %S", Py_TYPE(executor_object));
        return NULL;
    }
    return arena_made(type, (Executor*)executor_object);
}

static void Arena_dealloc(Arena* self);
static ustore_status_t arena_follow_unfollowed(Arena* arena);

PyTypeObject ArenaType = {
    PyVarObject_HEAD_INIT(NULL, 0) //
        .tp_name = "ustore.Arena",
    .tp_doc = doc_Arena,
    .tp_basicsize = sizeof(Arena),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = Arena_tp_new,
    .tp_dealloc = (destructor)Arena_dealloc,
};

/** Where the outputs of @p arena live, as the library reports it, or -1 with an exception set. */
static int arena_described(PyObject* arena, ustore_arena_description_t* description) {
    ustore_outcome_t outcome = {0};
    ustore_arena_describe_t describe = {
        .arena = ((Arena*)arena)->handle,
        .outcome = &outcome,
        .description = description,
    };
    ustore_arena_describe(&describe);
    return ustore_py_raise_on_outcome(&outcome);
}

/** 0 when the library reports @p arena as memory the host may read, or -1 raising a DeviceError. */
static int arena_readable(PyObject* arena) {
    ustore_arena_description_t description = {0};
    if (arena_described(arena, &description) != 0) return -1;
    if (description.host_access == ustore_host_access_readable_k) return 0;
    return ustore_py_raise(ustore_status_device_k,
                           "The arena lives in the memory of device %d, which Python cannot read",
                           (int)description.device);
}

int ustore_py_arena_from_object(PyObject* object, PyObject** arena) {
    if (!PyObject_TypeCheck(object, &ArenaType)) {
        PyErr_Format(PyExc_TypeError, "The arena must be an Arena, got %S", Py_TYPE(object));
        return -1;
    }
    // Every answer reads its outputs on the host, so a device arena is refused before the call.
    if (arena_readable(object) != 0) return -1;
    // Dropped exports still open wait for their consumers' kernels, which may still read the memory
    ustore_status_t const followed = arena_follow_unfollowed((Arena*)object);
    if (followed != ustore_status_success_k)
        return ustore_py_raise(followed, "the arena's stream could not wait for a dropped export's consumer");
    *arena = Py_NewRef(object);
    return 0;
}

ustore_arena_t ustore_py_arena_handle(PyObject* arena) { return ((Arena*)arena)->handle; }

#pragma endregion Arenas

#pragma region DLPack

DLDevice ustore_py_dlpack_device(ustore_arena_description_t const* description) {
    int32_t const ordinal = description->vendor_ordinal;
    if (description->residency != ustore_residency_vram_k) {
        if (description->device >= 0 && description->vendor == ustore_gpu_vendor_nvidia_k)
            return (DLDevice) {kDLCUDAHost, ordinal};
        if (description->device >= 0 && description->vendor == ustore_gpu_vendor_amd_k)
            return (DLDevice) {kDLROCMHost, ordinal};
        return (DLDevice) {kDLCPU, 0};
    }
    // No Metal exchange is spoken yet, so Apple's shared memory exports as the host memory it is
    if (description->vendor == ustore_gpu_vendor_apple_k) return (DLDevice) {kDLCPU, 0};
    // DLPack names no managed memory on ROCm, so a managed AMD arena reports its device
    if (description->vendor == ustore_gpu_vendor_amd_k) return (DLDevice) {kDLROCM, ordinal};
    return (DLDevice) {description->host_access == ustore_host_access_readable_k ? kDLCUDAManaged : kDLCUDA, ordinal};
}

/** The @c Arena an output view reads, which every view keeps alive. */
static PyObject* view_arena(PyObject* view) {
    PyTypeObject const* const type = Py_TYPE(view);
    if (type == &ValuesType) return ((Values*)view)->arena;
    if (type == &KeysType) return ((Keys*)view)->arena;
    if (type == &PageType) return ((Page*)view)->arena;
    if (type == &NeighborhoodsType) return ((Neighborhoods*)view)->arena;
    if (type == &ColumnsType) return ((Columns*)view)->arena;
    return ((Span*)view)->arena;
}

/** The DLPack device of the outputs @p view reads, or -1 with an exception set. */
static int view_device(PyObject* view, DLDevice* device) {
    ustore_arena_description_t description = {0};
    if (arena_described(view_arena(view), &description) != 0) return -1;
    *device = ustore_py_dlpack_device(&description);
    return 0;
}

/** Opens one export of the arena @p view reads, refusing every call naming it until the export
 *  closes, or -1 raising a ContendedError while a call holds it. */
static int view_export_open(PyObject* view) {
    ustore_status_t const status = ustore_arena_export_open(ustore_py_arena_handle(view_arena(view)));
    if (status == ustore_status_success_k) return 0;
    return ustore_py_raise(status, "The arena exports nothing while a call holds it");
}

/** Closes the export @c view_export_open opened on the arena @p view reads. */
static void view_export_close(PyObject* view) { ustore_arena_export_close(ustore_py_arena_handle(view_arena(view))); }

/** What one exported capsule keeps until its consumer deletes it: the buffer pinning the arena and
 *  the geometry. */
typedef struct dlpack_export_t {

    /** The tensor as the consumer asked for it, legacy or versioned. */
    union {
        DLManagedTensor legacy;
        DLManagedTensorVersioned versioned;
    } managed;

    /** The buffer the exporter answered, held until released. */
    Py_buffer view;

    /** The stream the consumer's kernels read the tensor on, which the arena's stream follows
     *  before the export closes, or NULL when the consumer named none. */
    void* consumer_stream;

    /** The next export on the arena's @c unfollowed stack, once this one waits there. */
    struct dlpack_export_t* next;

    /** Items along every dimension. */
    int64_t shape[3];

    /** Items between neighbours along every dimension. */
    int64_t strides[3];
} dlpack_export_t;

/** Orders the first stream of @p arena 's executor after the consumer's @p stream, if any. */
static ustore_status_t arena_follow(Arena* arena, void* stream) {
    if (!stream) return ustore_status_success_k;
    ustore_outcome_t outcome = {0};
    ustore_executor_follow_t follow = {
        .executor = arena->executor ? arena->executor->handle : NULL,
        .position = 0,
        .stream = stream,
        .outcome = &outcome,
    };
    ustore_executor_follow(&follow);
    return outcome.status;
}

/** Pushes the exports @p first through @p last onto the @c unfollowed stack of @p arena. */
static void arena_push_unfollowed(Arena* arena, dlpack_export_t* first, dlpack_export_t* last) {
    last->next = atomic_load(&arena->unfollowed);
    while (!atomic_compare_exchange_weak(&arena->unfollowed, &last->next, first)) {}
}

/** Follows and closes every export on the @c unfollowed stack of @p arena, answering the first
 *  failed follow's status with that export and the rest pushed back. */
static ustore_status_t arena_follow_unfollowed(Arena* arena) {
    dlpack_export_t* exported = atomic_exchange(&arena->unfollowed, NULL);
    while (exported) {
        ustore_status_t const followed = arena_follow(arena, exported->consumer_stream);
        if (followed != ustore_status_success_k) {
            dlpack_export_t* last = exported;
            while (last->next) last = last->next;
            arena_push_unfollowed(arena, exported, last);
            return followed;
        }
        dlpack_export_t* const next = exported->next;
        ustore_arena_export_close(arena->handle);
        PyMem_RawFree(exported);
        exported = next;
    }
    return ustore_status_success_k;
}

static void Arena_dealloc(Arena* self) {
    ustore_status_t const followed = arena_follow_unfollowed(self);
    if (followed != ustore_status_success_k) {
        // The consumers' streams can't be followed, so the arena waits for its own work instead
        ustore_outcome_t outcome = {0};
        ustore_executor_drain_t drain = {.executor = self->executor ? self->executor->handle : NULL,
                                         .outcome = &outcome,
                                         .mood = ustore_mood_parked_k};
        Py_BEGIN_ALLOW_THREADS;
        if (drain.executor) ustore_executor_drain(&drain);
        Py_END_ALLOW_THREADS;
        for (dlpack_export_t* exported = atomic_exchange(&self->unfollowed, NULL); exported;) {
            dlpack_export_t* const next = exported->next;
            ustore_arena_export_close(self->handle);
            PyMem_RawFree(exported);
            exported = next;
        }
        PyObject* const pending = PyErr_GetRaisedException();
        ustore_py_raise(followed, "the arena was freed before its stream could wait for a dropped export's consumer");
        PyErr_WriteUnraisable(NULL);
        PyErr_SetRaisedException(pending);
    }
    Py_BEGIN_ALLOW_THREADS;
    ustore_arena_free(self->handle);
    Py_END_ALLOW_THREADS;
    Py_CLEAR(self->executor);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

/** Closes the export, or leaves it for the arena's next user to follow and close, taking the
 *  interpreter, as a consumer may delete from a thread of its own. */
static void dlpack_export_free(dlpack_export_t* exported) {
    PyGILState_STATE const state = PyGILState_Ensure();
    // The consumer's kernels may still read the memory, so the arena's stream waits for them first
    PyObject* const owner = exported->view.obj;
    Arena* const arena = (Arena*)view_arena(owner);
    if (arena_follow(arena, exported->consumer_stream) == ustore_status_contended_k) {
        // A call on another arena holds the executor: the export stays open until the arena's next
        // user follows the stream, and the buffer goes, as the stack must not keep the arena alive
        Py_buffer view = exported->view;
        arena_push_unfollowed(arena, exported, exported);
        PyBuffer_Release(&view);
    }
    else {
        view_export_close(owner);
        PyBuffer_Release(&exported->view);
        PyMem_RawFree(exported);
    }
    PyGILState_Release(state);
}

static void dlpack_legacy_deleter(DLManagedTensor* self) { dlpack_export_free((dlpack_export_t*)self->manager_ctx); }

static void dlpack_versioned_deleter(DLManagedTensorVersioned* self) {
    dlpack_export_free((dlpack_export_t*)self->manager_ctx);
}

/** Deletes a capsule no consumer took, which a consumer renames once it does. */
static void dlpack_capsule_free(PyObject* capsule) {
    if (PyCapsule_IsValid(capsule, "dltensor")) {
        DLManagedTensor* const managed = (DLManagedTensor*)PyCapsule_GetPointer(capsule, "dltensor");
        managed->deleter(managed);
    }
    else if (PyCapsule_IsValid(capsule, "dltensor_versioned")) {
        DLManagedTensorVersioned* const managed = (DLManagedTensorVersioned*)PyCapsule_GetPointer(capsule,
                                                                                                  "dltensor_versioned");
        managed->deleter(managed);
    }
}

char const ustore_py_dlpack_doc[] =                                                         //
    "__dlpack__(*, stream=None, max_version=None, dl_device=None, copy=None) answers a\n"   //
    "DLPack capsule over the same memory the buffer protocol exports.\n"                    //
    "\n"                                                                                    //
    "The capsule holds this object and its arena until the consumer deletes it, so\n"       //
    "`numpy.from_dlpack`, `torch.from_dlpack` and their peers read the outputs in place.\n" //
    "A `max_version` of 1 or above answers a versioned capsule flagged read-only. The\n"    //
    "call that wrote the outputs is final, so no `stream` waits on anything; it is still\n" //
    "checked as the standard spells it for the device: None or -1 anywhere, and a stream\n" //
    "only on a CUDA or ROCm device, where CUDA refuses 0 and a handle must sit on the\n"    //
    "outputs' device. Once the consumer deletes the capsule, the arena's next call waits\n" //
    "for the work queued on that stream before it rewinds the memory.\n"                    //
    "\n"                                                                                    //
    "Raises:\n"                                                                             //
    "  BufferError: A copy was asked for, or a device other than the outputs' own.\n"       //
    "  TypeError: The stream is not an integer.\n"                                          //
    "  ValueError: The stream names nothing on the outputs' device.";

char const ustore_py_dlpack_device_doc[] =                                                     //
    "__dlpack_device__() answers the DLPack device type and ordinal of the outputs: (1, 0)\n"  //
    "for host pages, (3, ordinal) or (11, ordinal) for host pages page-locked for an NVIDIA\n" //
    "or AMD device, and (13, ordinal) for NVIDIA managed memory, the ordinal being the\n"      //
    "vendor's own. Apple's shared memory answers (1, 0), as no Metal exchange is spoken yet.";

/** A tuple of two integers, @p first and @p second, or NULL raising. */
static PyObject* integers_pair(long first, long second) {
    PyObject* const pair = PyTuple_New(2);
    if (!pair) return NULL;
    PyTuple_SET_ITEM(pair, 0, PyLong_FromLong(first));
    PyTuple_SET_ITEM(pair, 1, PyLong_FromLong(second));
    if (PyTuple_GET_ITEM(pair, 0) && PyTuple_GET_ITEM(pair, 1)) return pair;
    Py_DECREF(pair);
    return NULL;
}

PyObject* ustore_py_dlpack_device_answer(PyObject* self, PyObject* Py_UNUSED(unused)) {
    DLDevice device;
    if (view_device(self, &device) != 0) return NULL;
    return integers_pair((long)device.device_type, (long)device.device_id);
}

/** Reads one keyword of @c __dlpack__ into @p values, in the order stream, max_version, dl_device,
 *  copy. */
static int dlpack_arguments(PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                            PyObject* keyword_names_tuple, PyObject* values[4]) {
    static char const* const names[4] = {"stream", "max_version", "dl_device", "copy"};
    if (positional_arguments_count) {
        PyErr_SetString(PyExc_TypeError, "__dlpack__() takes keyword arguments only");
        return -1;
    }
    Py_ssize_t const keyword_names_count = keyword_names_tuple ? PyTuple_GET_SIZE(keyword_names_tuple) : 0;
    for (Py_ssize_t index = 0; index != keyword_names_count; ++index) {
        PyObject* const name = PyTuple_GET_ITEM(keyword_names_tuple, index);
        size_t slot = 0;
        while (slot != 4 && PyUnicode_CompareWithASCIIString(name, names[slot]) != 0) ++slot;
        if (slot == 4) {
            PyErr_Format(PyExc_TypeError, "__dlpack__() got an unexpected keyword argument '%S'", name);
            return -1;
        }
        values[slot] = arguments[index];
    }
    return 0;
}

/**
 *  @brief Checks the @p stream a consumer passes for outputs of @p arena on @p device, as the
 *      DLPack standard spells it, and that a stream handle sits on the arena's device.
 *  @param[out] consumer_stream The stream the consumer's kernels read on, or NULL for None, -1 and
 *      the default stream of ROCm.
 *  @return 0, or -1 raising.
 */
static int dlpack_stream_check(PyObject* stream, PyObject* arena, DLDevice device, void** consumer_stream) {
    *consumer_stream = NULL;
    if (stream == Py_None) return 0;
    if (!PyLong_Check(stream) || PyBool_Check(stream)) {
        PyErr_Format(PyExc_TypeError, "stream must be an integer or None, got %S", Py_TYPE(stream));
        return -1;
    }
    int overflowed = 0;
    long long const handle = PyLong_AsLongLongAndOverflow(stream, &overflowed);
    if (handle == -1 && PyErr_Occurred()) return -1;
    if (handle == -1 && !overflowed) return 0;
    int const cuda = device.device_type == kDLCUDA || device.device_type == kDLCUDAManaged;
    int const rocm = device.device_type == kDLROCM;
    if (!overflowed && !(cuda && handle > 0) && !(rocm && handle >= 0)) {
        PyErr_Format(PyExc_ValueError, "stream %S names no stream on DLPack device (%d, %d)", stream,
                     (int)device.device_type, (int)device.device_id);
        return -1;
    }
    void* const named = PyLong_AsVoidPtr(stream);
    if (!named && PyErr_Occurred()) return -1;
    Executor* const executor = ((Arena*)arena)->executor;
    if (!executor || !executor->handle) return ustore_py_raise(ustore_status_uninitialized_k, "the executor is closed");
    if (overflowed || (cuda && handle > 2) || (rocm && handle > 0)) { // a handle rather than a default stream
        ustore_outcome_t outcome = {0};
        ustore_executor_accepts_t accepts = {.executor = executor->handle,
                                             .position = 0,
                                             .stream = named,
                                             .outcome = &outcome};
        ustore_executor_accepts(&accepts);
        if (outcome.status == ustore_status_config_mismatch_k) {
            PyErr_Format(PyExc_ValueError, "stream %S sits on another device than the outputs", stream);
            return -1;
        }
        if (ustore_py_raise_on_outcome(&outcome) != 0) return -1;
    }
    *consumer_stream = named;
    return 0;
}

/** The DLPack type of items spelled by the struct-module @p format, @p itemsize bytes each. */
static DLDataType dlpack_type_of(char const* format, Py_ssize_t itemsize) {
    char const kind = format ? format[0] : 'B';
    uint8_t const code = kind == '?'              ? kDLBool
                         : strchr("efd", kind)    ? kDLFloat
                         : strchr("bhilqn", kind) ? kDLInt
                                                  : kDLUInt;
    return (DLDataType) {code, (uint8_t)(itemsize * 8), 1};
}

PyObject* ustore_py_dlpack(PyObject* self, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                           PyObject* keyword_names_tuple) {
    PyObject* values[4] = {Py_None, Py_None, Py_None, Py_None};
    if (dlpack_arguments(arguments, positional_arguments_count, keyword_names_tuple, values) != 0) return NULL;
    PyObject* const max_version = values[1];
    PyObject* const dl_device = values[2];
    DLDevice device;
    void* consumer_stream = NULL;
    if (view_device(self, &device) != 0 ||
        dlpack_stream_check(values[0], view_arena(self), device, &consumer_stream) != 0)
        return NULL;
    int const copied = values[3] == Py_None ? 0 : PyObject_IsTrue(values[3]);
    if (copied < 0) return NULL;
    if (copied) {
        PyErr_SetString(PyExc_BufferError, "The outputs export in place, never as a copy");
        return NULL;
    }
    if (dl_device != Py_None) {
        int const paired = PyTuple_Check(dl_device) && PyTuple_GET_SIZE(dl_device) == 2;
        long const type = paired ? PyLong_AsLong(PyTuple_GET_ITEM(dl_device, 0)) : -1;
        long const ordinal = paired ? PyLong_AsLong(PyTuple_GET_ITEM(dl_device, 1)) : -1;
        if (!paired || PyErr_Occurred()) {
            PyErr_Clear();
            PyErr_SetString(PyExc_TypeError, "dl_device must be a (device type, ordinal) tuple");
            return NULL;
        }
        if (type != (long)device.device_type || ordinal != (long)device.device_id) {
            PyErr_Format(PyExc_BufferError, "The outputs export only on the device they live on, (%d, %d)",
                         (int)device.device_type, (int)device.device_id);
            return NULL;
        }
    }
    long major = 0;
    if (max_version != Py_None) {
        major = PyTuple_Check(max_version) && PyTuple_GET_SIZE(max_version) >= 1
                    ? PyLong_AsLong(PyTuple_GET_ITEM(max_version, 0))
                    : -1;
        if (major == -1 && !PyErr_Occurred())
            PyErr_SetString(PyExc_TypeError, "max_version must be a (major, minor) tuple");
        if (PyErr_Occurred()) return NULL;
    }
    int const versioned = major >= DLPACK_MAJOR_VERSION;

    dlpack_export_t* const exported = (dlpack_export_t*)PyMem_RawCalloc(1, sizeof(dlpack_export_t));
    if (!exported) return PyErr_NoMemory();
    if (view_export_open(self) != 0) {
        PyMem_RawFree(exported);
        return NULL;
    }
    if (PyObject_GetBuffer(self, &exported->view, PyBUF_RECORDS_RO) != 0) {
        view_export_close(self);
        PyMem_RawFree(exported);
        return NULL;
    }
    exported->consumer_stream = consumer_stream;
    Py_buffer const* const view = &exported->view;
    for (int dimension = 0; dimension != view->ndim; ++dimension) {
        exported->shape[dimension] = (int64_t)view->shape[dimension];
        exported->strides[dimension] = (int64_t)(view->strides[dimension] / view->itemsize);
    }
    DLTensor* const tensor = versioned ? &exported->managed.versioned.dl_tensor : &exported->managed.legacy.dl_tensor;
    *tensor = (DLTensor) {
        .data = view->buf,
        .device = device,
        .ndim = view->ndim,
        .dtype = dlpack_type_of(view->format, view->itemsize),
        .shape = exported->shape,
        .strides = exported->strides,
        .byte_offset = 0,
    };
    if (versioned) {
        exported->managed.versioned.version = (DLPackVersion) {DLPACK_MAJOR_VERSION, DLPACK_MINOR_VERSION};
        exported->managed.versioned.manager_ctx = exported;
        exported->managed.versioned.deleter = dlpack_versioned_deleter;
        exported->managed.versioned.flags = DLPACK_FLAG_BITMASK_READ_ONLY;
    }
    else {
        exported->managed.legacy.manager_ctx = exported;
        exported->managed.legacy.deleter = dlpack_legacy_deleter;
    }
    PyObject* const capsule = PyCapsule_New(&exported->managed, versioned ? "dltensor_versioned" : "dltensor",
                                            dlpack_capsule_free);
    if (!capsule) dlpack_export_free(exported);
    return capsule;
}

/** Deletes the tensor of a capsule @c dlpack_import consumed, once its holder drops it. */
static void dlpack_import_free(PyObject* capsule) {
    if (PyCapsule_IsValid(capsule, "used_dltensor_versioned")) {
        DLManagedTensorVersioned* const managed = (DLManagedTensorVersioned*)PyCapsule_GetPointer(
            capsule, "used_dltensor_versioned");
        if (managed->deleter) managed->deleter(managed);
    }
    else if (PyCapsule_IsValid(capsule, "used_dltensor")) {
        DLManagedTensor* const managed = (DLManagedTensor*)PyCapsule_GetPointer(capsule, "used_dltensor");
        if (managed->deleter) managed->deleter(managed);
    }
}

/** The DLPack @c stream a consumer running on @p executor passes: the native handle of its first
 *  stream, so a producer still writing is ordered before the call reads, or None without one. */
static PyObject* executor_stream_object(ustore_executor_t executor) {
    if (!executor) return Py_NewRef(Py_None);
    void* stream = NULL;
    ustore_outcome_t outcome = {0};
    ustore_executor_stream_t named = {.executor = executor, .position = 0, .outcome = &outcome, .stream = &stream};
    ustore_executor_stream(&named);
    if (outcome.status == ustore_status_unsupported_k || (outcome.status == ustore_status_success_k && !stream))
        return Py_NewRef(Py_None);
    if (ustore_py_raise_on_outcome(&outcome) != 0) return NULL;
    return PyLong_FromVoidPtr(stream);
}

int ustore_py_executor_follow_and_drain(ustore_executor_t executor, void* event, void* stream) {
    if (!executor)
        return ustore_py_raise(ustore_status_unsupported_k, "Data still being produced needs a GPU executor to wait");
    ustore_outcome_t outcome = {0};
    ustore_executor_follow_t follow = {.executor = executor,
                                       .position = 0,
                                       .event = event,
                                       .stream = stream,
                                       .outcome = &outcome};
    ustore_executor_drain_t drain = {.executor = executor, .outcome = &outcome, .mood = ustore_mood_parked_k};
    Py_BEGIN_ALLOW_THREADS;
    ustore_executor_follow(&follow);
    if (outcome.status == ustore_status_success_k) ustore_executor_drain(&drain);
    Py_END_ALLOW_THREADS;
    return ustore_py_raise_on_outcome(&outcome);
}

/** Calls @p exporter with @p stream for a versioned capsule when @p versioned, or NULL raising. */
static PyObject* dlpack_call(PyObject* exporter, PyObject* stream, int versioned) {
    PyObject* const max_version = versioned ? integers_pair(DLPACK_MAJOR_VERSION, DLPACK_MINOR_VERSION) : NULL;
    PyObject* const stream_name = PyUnicode_InternFromString("stream");
    PyObject* const version_name = versioned ? PyUnicode_InternFromString("max_version") : NULL;
    PyObject* const names = !stream_name ? NULL
                            : versioned  ? (version_name ? PyTuple_Pack(2, stream_name, version_name) : NULL)
                                         : PyTuple_Pack(1, stream_name);
    PyObject* const values[2] = {stream, max_version};
    PyObject* const capsule = names && (max_version || !versioned) ? PyObject_Vectorcall(exporter, values, 0, names)
                                                                   : NULL;
    Py_XDECREF(names);
    Py_XDECREF(version_name);
    Py_XDECREF(stream_name);
    Py_XDECREF(max_version);
    return capsule;
}

/**
 *  @brief Takes the DLPack tensor @p object exports, asking for a versioned capsule first and a
 *      legacy one from an exporter older than DLPack 1.0, from any device of the table
 *      @c ustore_py_dlpack_device derives.
 *  @param[out] tensor The tensor, alive until the answer is dropped.
 *  @return The renamed capsule owning the tensor; NULL and no exception for an object without a
 *      DLPack export or one refusing this layout, which leaves the batch to the next route; NULL
 *      raising when the export fails otherwise.
 */
static PyObject* dlpack_import(PyObject* object, ustore_executor_t executor, DLTensor const** tensor) {
    PyObject* const exporter = PyObject_GetAttrString(object, "__dlpack__");
    if (!exporter) {
        if (!PyErr_ExceptionMatches(PyExc_AttributeError)) return NULL;
        PyErr_Clear();
        return NULL;
    }
    PyObject* const stream = executor_stream_object(executor);
    if (!stream) {
        Py_DECREF(exporter);
        return NULL;
    }
    PyObject* capsule = dlpack_call(exporter, stream, 1);
    // An exporter older than DLPack 1.0 knows no `max_version`, and says so with a plain TypeError.
    PyObject* const refusal = capsule ? NULL : PyErr_GetRaisedException();
    if (refusal && Py_IS_TYPE(refusal, (PyTypeObject*)PyExc_TypeError)) {
        Py_DECREF(refusal);
        capsule = dlpack_call(exporter, stream, 0);
    }
    else if (refusal) PyErr_SetRaisedException(refusal);
    Py_DECREF(stream);
    Py_DECREF(exporter);
    // NumPy refuses 16-byte items and PyArrow fixed-size binary, which leaves the next route.
    if (!capsule) {
        if (PyErr_ExceptionMatches(PyExc_BufferError) || PyErr_ExceptionMatches(PyExc_TypeError)) PyErr_Clear();
        return NULL;
    }
    int const versioned = PyCapsule_IsValid(capsule, "dltensor_versioned");
    if (!versioned && !PyCapsule_IsValid(capsule, "dltensor")) {
        Py_DECREF(capsule);
        PyErr_SetString(PyExc_TypeError, "__dlpack__ must answer a capsule named 'dltensor' or 'dltensor_versioned'");
        return NULL;
    }
    void* const managed = PyCapsule_GetPointer(capsule, versioned ? "dltensor_versioned" : "dltensor");
    // Renamed, the capsule is ours, and dropping it deletes the tensor.
    if (PyCapsule_SetName(capsule, versioned ? "used_dltensor_versioned" : "used_dltensor") != 0 ||
        PyCapsule_SetDestructor(capsule, dlpack_import_free) != 0) {
        Py_DECREF(capsule);
        return NULL;
    }
    DLPackVersion const version = versioned ? ((DLManagedTensorVersioned*)managed)->version
                                            : (DLPackVersion) {DLPACK_MAJOR_VERSION, 0};
    if (version.major != DLPACK_MAJOR_VERSION) {
        Py_DECREF(capsule);
        PyErr_Format(PyExc_BufferError, "A DLPack %u.%u tensor has a major version this build cannot read",
                     (unsigned)version.major, (unsigned)version.minor);
        return NULL;
    }
    // Every import only reads the tensor, so a read-only one passes as it is.
    DLTensor const* const found = versioned ? &((DLManagedTensorVersioned*)managed)->dl_tensor
                                            : &((DLManagedTensor*)managed)->dl_tensor;
    switch (found->device.device_type) {
    case kDLCPU:
    case kDLCUDA:
    case kDLCUDAHost:
    case kDLCUDAManaged:
    case kDLROCM:
    case kDLROCMHost: *tensor = found; return capsule;
    case kDLMetal:
        ustore_py_raise(ustore_status_unsupported_k, "Metal exchange through DLPack is not supported yet");
        break;
    default:
        ustore_py_raise(ustore_status_device_k, "A DLPack tensor on device type %d is none UStore reads",
                        (int)found->device.device_type);
        break;
    }
    Py_DECREF(capsule);
    return NULL;
}

/** Whether the host may read memory on @p device in place. */
static int dlpack_host_readable(DLDevice device) {
    return device.device_type == kDLCPU || device.device_type == kDLCUDAHost || device.device_type == kDLROCMHost ||
           device.device_type == kDLCUDAManaged;
}

#pragma endregion DLPack

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
    // The slot before the arguments is scratch the callee may view, as the offset flag permits.
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

/** Converts @p count packed 64-bit integers into keys, refusing a negative signed one. */
static int keys_from_integers(void const* data, size_t count, int is_signed, ustore_py_keys_t* batch) {
    if (keys_reserve(batch, count) != 0) return -1;
    char const* const items = (char const*)data;
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

/** Whether a batch of keys travels as a viewed buffer, converts from one, or reads
 *  element by element. */
typedef enum keys_route_t {

    /** A contiguous run of 16-byte keys the batch views outright. */
    keys_route_viewed_k,

    /** A contiguous run of 64-bit integers, converted into keys and released. */
    keys_route_converted_k,

    /** Not a buffer of either layout, so each element converts on its own terms. */
    keys_route_sequence_k,

    /** The conversion failed, with an exception set. */
    keys_route_failed_k,
} keys_route_t;

/** Tries the DLPack routes: packed 16-byte keys are viewed wherever they live, as the library
 *  classifies the address, and packed 64-bit integers convert where the host reads them. */
static keys_route_t keys_from_dlpack(PyObject* object, ustore_executor_t executor, ustore_py_keys_t* batch) {
    DLTensor const* tensor = NULL;
    PyObject* const capsule = dlpack_import(object, executor, &tensor);
    if (!capsule) return PyErr_Occurred() ? keys_route_failed_k : keys_route_sequence_k;
    int64_t const rows = tensor->ndim >= 1 ? tensor->shape[0] : 0;
    int64_t const* const strides = tensor->strides;
    int const integral = (tensor->dtype.code == kDLInt || tensor->dtype.code == kDLUInt) && tensor->dtype.lanes == 1;
    char const* const data = (char const*)tensor->data + tensor->byte_offset;
    if (integral && tensor->dtype.bits == 8 && tensor->ndim == 2 && tensor->shape[1] == (int64_t)sizeof(ustore_key_t) &&
        (!strides || (strides[1] == 1 && (strides[0] == (int64_t)sizeof(ustore_key_t) || rows <= 1)))) {
        batch->held = capsule;
        batch->count = (size_t)rows;
        batch->keys = rows ? (ustore_key_t*)data : NULL;
        return keys_route_viewed_k;
    }
    if (!dlpack_host_readable(tensor->device) || !integral || tensor->dtype.bits != 64 || tensor->ndim != 1 ||
        (strides && strides[0] != 1 && rows > 1)) {
        Py_DECREF(capsule);
        return keys_route_sequence_k;
    }
    int const converted = keys_from_integers(data, (size_t)rows, tensor->dtype.code == kDLInt, batch);
    Py_DECREF(capsule);
    return converted == 0 ? keys_route_converted_k : keys_route_failed_k;
}

/** The one Arrow array an object exports, with its device and the capsules owning it. */
typedef struct arrow_import_t {
    struct ArrowSchema const* schema;
    struct ArrowArray const* array;
    DLDevice device;
    PyObject* capsules;
} arrow_import_t;

/**
 *  @brief Takes the one Arrow array @p object exports, through @c __arrow_c_device_array__ first
 *      and @c __arrow_c_array__ otherwise, from any device @c ustore_py_dlpack_device derives.
 *      A device array still producing on its sync event is waited for through @p executor.
 *  @return 1 with @p imported filled, its capsules owned by the caller; 0 when @p object exports
 *      neither; -1 raising when the export fails or its producer cannot be waited for.
 */
static int arrow_import(PyObject* object, ustore_executor_t executor, arrow_import_t* imported) {
    static char const* const methods[2] = {"__arrow_c_device_array__", "__arrow_c_array__"};
    for (int host = 0; host != 2; ++host) {
        PyObject* const exporter = PyObject_GetAttrString(object, methods[host]);
        if (!exporter) {
            if (!PyErr_ExceptionMatches(PyExc_AttributeError)) return -1;
            PyErr_Clear();
            continue;
        }
        PyObject* const capsules = PyObject_CallNoArgs(exporter);
        Py_DECREF(exporter);
        if (!capsules) return -1;
        char const* const name = host ? "arrow_array" : "arrow_device_array";
        int const paired = PyTuple_Check(capsules) && PyTuple_GET_SIZE(capsules) == 2 &&
                           PyCapsule_IsValid(PyTuple_GET_ITEM(capsules, 0), "arrow_schema") &&
                           PyCapsule_IsValid(PyTuple_GET_ITEM(capsules, 1), name);
        if (!paired) {
            Py_DECREF(capsules);
            PyErr_Format(PyExc_TypeError, "%s must answer an 'arrow_schema' capsule and an '%s' capsule", methods[host],
                         name);
            return -1;
        }
        imported->schema = PyCapsule_GetPointer(PyTuple_GET_ITEM(capsules, 0), "arrow_schema");
        void* const pointer = PyCapsule_GetPointer(PyTuple_GET_ITEM(capsules, 1), name);
        imported->capsules = capsules;
        if (host) {
            imported->array = (struct ArrowArray const*)pointer;
            imported->device = (DLDevice) {kDLCPU, 0};
            return 1;
        }
        struct ArrowDeviceArray const* const device_array = (struct ArrowDeviceArray const*)pointer;
        imported->array = &device_array->array;
        imported->device = (DLDevice) {(DLDeviceType)device_array->device_type, (int32_t)device_array->device_id};
        int failed = 0;
        switch (imported->device.device_type) {
        case kDLCPU: imported->device.device_id = 0; break;
        case kDLCUDA:
        case kDLCUDAHost:
        case kDLCUDAManaged:
        case kDLROCM:
        case kDLROCMHost: break;
        case kDLMetal:
            failed = ustore_py_raise(ustore_status_unsupported_k, "Metal exchange through Arrow is not supported yet");
            break;
        default:
            failed = ustore_py_raise(ustore_status_device_k, "An Arrow array on device type %d is none UStore reads",
                                     (int)imported->device.device_type);
            break;
        }
        if (!failed && device_array->sync_event)
            failed = ustore_py_executor_follow_and_drain(executor, device_array->sync_event, NULL);
        if (!failed) return 1;
        Py_DECREF(capsules);
        return -1;
    }
    return 0;
}

/** Whether the Arrow @p array holds a null, which only a validity bitmap with a zero bit spells. */
static int arrow_holds_null(struct ArrowArray const* array) {
    uint8_t const* const validities = array->n_buffers ? (uint8_t const*)array->buffers[0] : NULL;
    if (!validities || array->null_count == 0) return 0;
    for (int64_t index = array->offset; index != array->offset + array->length; ++index)
        if (!((validities[index / 8] >> (index % 8)) & 1)) return 1;
    return 0;
}

/** Tries the Arrow routes: <tt>fixed_size_binary(16)</tt> keys are viewed where they live, as the
 *  library classifies their address, and 64-bit integers convert where the host reads them. */
static keys_route_t keys_from_arrow(PyObject* object, ustore_executor_t executor, ustore_py_keys_t* batch) {
    arrow_import_t imported = {0};
    int const found = arrow_import(object, executor, &imported);
    if (found <= 0) return found ? keys_route_failed_k : keys_route_sequence_k;
    struct ArrowArray const* const array = imported.array;
    char const* const format = imported.schema->format;
    int const records = strcmp(format, "w:16") == 0;
    int const integers = strcmp(format, "l") == 0 || strcmp(format, "L") == 0;
    keys_route_t route = keys_route_sequence_k;
    if ((!records && !integers) || array->n_buffers != 2) route = keys_route_sequence_k;
    else if (dlpack_host_readable(imported.device) && arrow_holds_null(array)) {
        ustore_py_raise(ustore_status_invalid_argument_k, "An Arrow array of keys holds a null");
        route = keys_route_failed_k;
    }
    else if (records) {
        batch->count = (size_t)array->length;
        batch->keys = array->length ? (ustore_key_t*)array->buffers[1] + array->offset : NULL;
        batch->held = Py_NewRef(imported.capsules);
        route = keys_route_viewed_k;
    }
    else if (dlpack_host_readable(imported.device))
        route = keys_from_integers((int64_t const*)array->buffers[1] + array->offset, (size_t)array->length,
                                   format[0] == 'l', batch) == 0
                    ? keys_route_converted_k
                    : keys_route_failed_k;
    Py_DECREF(imported.capsules);
    return route;
}

/** Tries the CUDA Array Interface, viewing packed 16-byte keys wherever they live, as the library
 *  classifies the address: <tt>(keys, 16)</tt> bytes or <tt>(keys,)</tt> 16-byte records. */
static keys_route_t keys_from_cuda_array_interface(PyObject* object, ustore_executor_t executor,
                                                   ustore_py_keys_t* batch) {
    PyObject* const described = PyObject_GetAttrString(object, "__cuda_array_interface__");
    if (!described) {
        if (!PyErr_ExceptionMatches(PyExc_AttributeError)) return keys_route_failed_k;
        PyErr_Clear();
        return keys_route_sequence_k;
    }
    PyObject* const data = PyDict_Check(described) ? PyDict_GetItemString(described, "data") : NULL;
    PyObject* const shape = PyDict_Check(described) ? PyDict_GetItemString(described, "shape") : NULL;
    PyObject* const typestr = PyDict_Check(described) ? PyDict_GetItemString(described, "typestr") : NULL;
    PyObject* const strides = PyDict_Check(described) ? PyDict_GetItemString(described, "strides") : NULL;
    PyObject* const stream = PyDict_Check(described) ? PyDict_GetItemString(described, "stream") : NULL;
    keys_route_t route = keys_route_sequence_k;
    if (!data || !shape || !typestr || !PyTuple_Check(data) || PyTuple_GET_SIZE(data) != 2 || !PyTuple_Check(shape) ||
        !PyUnicode_Check(typestr)) {
        PyErr_SetString(PyExc_TypeError, "__cuda_array_interface__ must hold data, shape and typestr");
        route = keys_route_failed_k;
    }
    else if (stream && stream != Py_None &&
             (!PyLong_Check(stream) ||
              ustore_py_executor_follow_and_drain(executor, NULL, PyLong_AsVoidPtr(stream)) != 0)) {
        if (!PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, "A CUDA Array Interface stream is an integer");
        route = keys_route_failed_k;
    }
    else {
        Py_ssize_t const dimensions = PyTuple_GET_SIZE(shape);
        long long const rows = dimensions ? PyLong_AsLongLong(PyTuple_GET_ITEM(shape, 0)) : 0;
        long long const width = dimensions == 2 ? PyLong_AsLongLong(PyTuple_GET_ITEM(shape, 1)) : 0;
        int const bytes = dimensions == 2 && width == (long long)sizeof(ustore_key_t) &&
                          PyUnicode_CompareWithASCIIString(typestr, "|u1") == 0;
        int const records = dimensions == 1 && PyUnicode_CompareWithASCIIString(typestr, "|V16") == 0;
        void* const address = PyLong_AsVoidPtr(PyTuple_GET_ITEM(data, 0));
        if (PyErr_Occurred()) route = keys_route_failed_k;
        else if ((bytes || records) && (!strides || strides == Py_None)) {
            batch->count = (size_t)rows;
            batch->keys = rows ? (ustore_key_t*)address : NULL;
            batch->held = Py_NewRef(object);
            route = keys_route_viewed_k;
        }
    }
    Py_DECREF(described);
    return route;
}

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
    batch->viewed = 1;
    Py_buffer const* const buffer = &batch->buffer;
    if (buffer_holds_keys(buffer)) {
        batch->count = (size_t)buffer->shape[0];
        // The ABI spells an empty batch with no keys at all, and refuses an address for no tasks.
        batch->keys = batch->count ? (ustore_key_t*)buffer->buf : NULL;
        return keys_route_viewed_k;
    }
    if (buffer_holds_integers(buffer)) {
        char const kind = buffer->format[0] == '@' || buffer->format[0] == '=' ? buffer->format[1] : buffer->format[0];
        int const is_signed = kind == 'q' || kind == 'l' || kind == 'n';
        int const converted = keys_from_integers(buffer->buf, (size_t)buffer->shape[0], is_signed, batch);
        PyBuffer_Release(&batch->buffer);
        batch->viewed = 0;
        return converted == 0 ? keys_route_converted_k : keys_route_failed_k;
    }
    PyBuffer_Release(&batch->buffer);
    batch->viewed = 0;
    return keys_route_sequence_k;
}

int ustore_py_keys_from_object(PyObject* object, ustore_executor_t executor, ustore_py_keys_t* batch) {
    memset(batch, 0, sizeof(*batch));
    // Text and bytes are sequences too, and iterating one would answer a key per character
    if (ustore_py_is_one_key(object)) {
        PyErr_Format(PyExc_TypeError, "A batch of keys takes a sequence or a buffer, not one %S", Py_TYPE(object));
        return -1;
    }

    // DLPack, Arrow, the CUDA Array Interface, then the buffer protocol, as USearch and UForm.
    keys_route_t route = keys_from_dlpack(object, executor, batch);
    if (route == keys_route_sequence_k) route = keys_from_arrow(object, executor, batch);
    if (route == keys_route_sequence_k) route = keys_from_cuda_array_interface(object, executor, batch);
    if (route == keys_route_sequence_k) route = keys_from_buffer(object, batch);
    switch (route) {
    case keys_route_viewed_k:
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

int ustore_py_keys_from_subscript(PyObject* object, ustore_executor_t executor, ustore_py_keys_t* batch) {
    if (!ustore_py_is_one_key(object)) return ustore_py_keys_from_object(object, executor, batch) == 0 ? 0 : -1;
    memset(batch, 0, sizeof(*batch));
    if (ustore_py_key_from_object(object, &batch->inline_keys[0]) != 0) return -1;
    batch->keys = batch->inline_keys;
    batch->count = 1;
    return 1;
}

void ustore_py_keys_release(ustore_py_keys_t* batch) {
    if (batch->viewed) PyBuffer_Release(&batch->buffer);
    Py_CLEAR(batch->held);
    if (batch->owned) PyMem_Free(batch->owned);
    batch->viewed = 0;
    batch->owned = NULL;
    batch->keys = NULL;
    batch->count = 0;
}

#pragma endregion Keys

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
    // A tape of empty strings may hold no bytes at all, and no address may be offset from NULL.
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

int ustore_py_contents_view(PyObject* object, ustore_py_contents_t* batch) {
    Py_buffer* const buffer = &batch->buffers[batch->viewed];
    if (PyObject_GetBuffer(object, buffer, PyBUF_SIMPLE) != 0)
        return ustore_py_raise_from_cause(PyExc_TypeError, "A value must be bytes-like, not %S; remove() removes keys",
                                          Py_TYPE(object));
    ++batch->viewed;
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
    batch->viewed = 2;
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

/** Reads an Arrow array of binary or text as a tape: 1 for one, 0 when @p object exports none, -1 failing. */
static int contents_from_arrow(PyObject* object, ustore_executor_t executor, ustore_py_contents_t* batch) {
    arrow_import_t imported = {0};
    int const found = arrow_import(object, executor, &imported);
    if (found <= 0) return found;
    struct ArrowArray const* const array = imported.array;
    char const* const format = imported.schema->format;
    int const narrow = strcmp(format, "z") == 0 || strcmp(format, "u") == 0;
    int const wide = strcmp(format, "Z") == 0 || strcmp(format, "U") == 0;
    int read = -1;
    if (!dlpack_host_readable(imported.device))
        ustore_py_raise(ustore_status_device_k, "An Arrow array of values on device type %d is none the host reads",
                        (int)imported.device.device_type);
    else if (!narrow && !wide)
        PyErr_Format(PyExc_TypeError,
                     "An Arrow array of values is binary, large_binary, string or large_string, not of format '%s'",
                     format);
    else if (array->n_buffers != 3)
        PyErr_Format(PyExc_TypeError, "An Arrow array of values holds three buffers, not %lld",
                     (long long)array->n_buffers);
    else if (arrow_holds_null(array))
        ustore_py_raise(ustore_status_invalid_argument_k,
                        "An Arrow array of values holds a null; remove() removes keys");
    else {
        // The capsules free the array's buffers once they die, so holding them keeps it readable.
        batch->held = Py_NewRef(imported.capsules);
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
    Py_DECREF(imported.capsules);
    return read;
}

int ustore_py_contents_from_tape(PyObject* object, ustore_executor_t executor, ustore_py_contents_t* batch) {
    // The common batches are builtins no Arrow exporter derives from, so they skip the lookup of its method.
    if (PyList_CheckExact(object) || PyBytes_CheckExact(object) || PyByteArray_CheckExact(object) ||
        PyUnicode_CheckExact(object) || PyDict_CheckExact(object))
        return 0;
    memset(batch, 0, sizeof(*batch));
    batch->views = batch->inline_views;
    batch->buffers = batch->inline_buffers;
    int const read = PyTuple_CheckExact(object) ? contents_from_pair(object, batch)
                                                : contents_from_arrow(object, executor, batch);
    if (read < 0) ustore_py_contents_release(batch);
    return read;
}

void ustore_py_contents_release(ustore_py_contents_t* batch) {
    for (size_t buffer_index = 0; buffer_index != batch->viewed; ++buffer_index)
        PyBuffer_Release(&batch->buffers[buffer_index]);
    batch->viewed = 0;
    if (batch->block) PyMem_Free(batch->block);
    batch->block = NULL;
    batch->count = 0;
    Py_CLEAR(batch->held);
}

/** Views one text into the next view of @p texts: a string as the UTF-8 bytes it caches, a
 *  bytes-like object as it is, None refused, and any other item through @p serializer where
 *  there is one. */
static int text_view(PyObject* object, ustore_py_text_serializer_t serializer, void* state, ustore_py_texts_t* texts) {
    ustore_py_contents_t* const batch = &texts->contents;
    if (!PyUnicode_Check(object)) {
        if (!serializer || object == Py_None || PyObject_CheckBuffer(object))
            return ustore_py_contents_view(object, batch);
        return serializer(object, state);
    }
    Py_ssize_t length = 0;
    char const* const text = PyUnicode_AsUTF8AndSize(object, &length);
    if (!text) return -1;
    batch->views[batch->count].data = (uint8_t const*)text;
    batch->views[batch->count].length = (ustore_size_t)length;
    ++batch->count;
    return 0;
}

int ustore_py_texts_from_object(PyObject* object, char const* argument, ustore_py_text_serializer_t serializer,
                                void* state, ustore_py_texts_t* texts) {
    memset(texts, 0, sizeof(*texts));
    int const tape = ustore_py_contents_from_tape(object, NULL, &texts->contents);
    if (tape != 0) return tape < 0 ? -1 : 0;

    int const one = PyUnicode_Check(object) || PyObject_CheckBuffer(object) || PyDict_Check(object);
    texts->held = one ? Py_NewRef(object) : PySequence_Tuple(object);
    if (!texts->held)
        return ustore_py_raise_from_cause(PyExc_TypeError, "The %s are a tape, a sequence of them or one, not %S",
                                          argument, Py_TYPE(object));
    size_t const count = one ? 1 : (size_t)PyTuple_GET_SIZE(texts->held);
    if (ustore_py_contents_reserve(&texts->contents, count,
                                   one ? ustore_py_contents_broadcast_k : ustore_py_contents_per_task_k) != 0) {
        Py_CLEAR(texts->held);
        return -1;
    }
    for (size_t task_index = 0; task_index != count; ++task_index)
        if (text_view(one ? object : PyTuple_GET_ITEM(texts->held, (Py_ssize_t)task_index), serializer, state, texts) !=
            0) {
            ustore_py_texts_release(texts);
            return -1;
        }
    return 0;
}

void ustore_py_texts_release(ustore_py_texts_t* texts) {
    ustore_py_contents_release(&texts->contents);
    Py_CLEAR(texts->held);
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

    // An empty run may hold no address at all, and a buffer must still point somewhere.
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

PyObject* ustore_py_span_view(PyObject* arena, void const* data, char const* format, Py_ssize_t itemsize, int ndim,
                              Py_ssize_t const* shape) {
    Span* const span = PyObject_New(Span, &SpanType);
    if (!span) return NULL;
    span->arena = Py_NewRef(arena);
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
    return ustore_py_buffer_fill(view, (PyObject*)self, self->data, self->format, self->itemsize, self->ndim,
                                 self->shape, self->strides, flags);
}

static PyMethodDef Span_methods[] = {
    {"__dlpack__", (PyCFunction)(void (*)(void))ustore_py_dlpack, ustore_py_method_flags_k, ustore_py_dlpack_doc},
    {"__dlpack_device__", ustore_py_dlpack_device_answer, METH_NOARGS, ustore_py_dlpack_device_doc},
    {NULL, NULL, 0, NULL},
};

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
    .tp_methods = Span_methods,
};

#pragma endregion Spans

#pragma region Value Tapes

static char const doc_Values[] =                                                                //
    "The values one batch read wrote into an `Arena`, viewing it until it rewinds.\n"           //
    "\n"                                                                                        //
    "Indexing answers a `memoryview` over the stored bytes, or None where the key was\n"        //
    "absent; nothing is copied, and `bytes(values[index])` is where a copy happens. The\n"      //
    "whole batch exports through the Arrow C data interface as `binary`, or as\n"               //
    "`large_binary` once its bytes outgrow 32-bit offsets, so `pyarrow.array(values)` reads\n"  //
    "the arena in place too, and a write takes it back as the tape it is. Read after a later\n" //
    "call rewound the arena, it reads whatever the arena then holds.";

PyObject* ustore_py_values_new(PyObject* arena, ustore_octet_t const* presences, ustore_strings_view_t const* tape,
                               size_t count) {
    Values* const instance = PyObject_New(Values, &ValuesType);
    if (!instance) return NULL;
    instance->arena = Py_NewRef(arena);
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
    PyObject* item = NULL;
    if (!values_present(self, (size_t)task_index)) item = Py_NewRef(Py_None);
    else {
        ustore_bytes_view_t const value = ustore_py_tape_string(&self->tape, (size_t)task_index);
        Py_ssize_t const shape = (Py_ssize_t)value.length;
        item = ustore_py_span_view(self->arena, value.data, "B", 1, 1, &shape);
    }
    return item;
}

static int Values_getbuffer(Values* self, Py_buffer* view, int flags) {
    view->obj = NULL;
    // A tape of no values still answers a buffer, so a caller need not special-case an empty read.
    void* const data = values_data(self) ? (void*)values_data(self) : (void*)self;
    Py_ssize_t const bytes = (Py_ssize_t)ustore_py_tape_bytes(&self->tape, self->count);
    return PyBuffer_FillInfo(view, (PyObject*)self, data, bytes, 1, flags);
}

static PyObject* Values_repr(Values* self) { return PyUnicode_FromFormat("Values(count=%zu)", self->count); }

static char const doc_Values_lengths[] = "The length of every value, with None where the key was absent, as a tuple.";

static PyObject* Values_get_lengths(Values* self, void* Py_UNUSED(closure)) {
    PyObject* lengths = PyTuple_New((Py_ssize_t)self->count);
    for (size_t task_index = 0; lengths && task_index != self->count; ++task_index) {
        ustore_size_t const length = ustore_py_tape_string(&self->tape, task_index).length;
        PyObject* const entry = !values_present(self, task_index) ? Py_NewRef(Py_None)
                                                                  : PyLong_FromUnsignedLongLong(length);
        if (!entry) Py_CLEAR(lengths);
        else PyTuple_SET_ITEM(lengths, (Py_ssize_t)task_index, entry);
    }
    return lengths;
}

/** What one exported array holds until its consumer releases it: the @c Values and its buffers. */
typedef struct values_export_t {

    /** The @c Values viewing the tape and its spans, held by a strong reference, its arena pinned. */
    Values* owner;

    /** The presence bits, the offsets and the bytes, in the order `binary` and `large_binary` list them. */
    void const* buffers[3];
} values_export_t;

/** Any readable address, for offsets or a tape of no bytes that a consumer still dereferences. */
static int64_t const values_export_empty_k[1] = {0};

static void values_schema_release(struct ArrowSchema* schema) { schema->release = NULL; }

/** Lets go of the exported @c Values, taking the interpreter, as a consumer may release from a
 *  thread of its own. */
static void values_array_release(struct ArrowArray* array) {
    values_export_t* const exported = (values_export_t*)array->private_data;
    PyGILState_STATE const state = PyGILState_Ensure();
    view_export_close((PyObject*)exported->owner);
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

static char const doc_Values_arrow_c_array[] =                                             //
    "__arrow_c_array__(requested_schema=None) answers the schema and array PyCapsules.\n"  //
    "\n"                                                                                   //
    "Exports the batch as Arrow `binary` with the 32-bit offsets the read wrote, or as\n"  //
    "`large_binary` with 64-bit ones once its bytes outgrow 4 GiB, with a value missing\n" //
    "where the key was absent. The presence bits, offsets and bytes the read wrote are\n"  //
    "exported in place, and the array keeps this object and its arena alive until Arrow\n" //
    "releases it. A requested schema is ignored, and Arrow casts from the exported type\n" //
    "where it asked for another.";

static PyObject* Values_arrow_c_array(Values* self, PyObject* const* Py_UNUSED(arguments),
                                      Py_ssize_t positional_arguments_count, PyObject* keyword_names_tuple) {
    Py_ssize_t const keyword_names_count = keyword_names_tuple ? PyTuple_GET_SIZE(keyword_names_tuple) : 0;
    int const named = keyword_names_count == 1 &&
                      PyUnicode_CompareWithASCIIString(PyTuple_GET_ITEM(keyword_names_tuple, 0), "requested_schema") ==
                          0;
    if (positional_arguments_count + keyword_names_count > 1 || (keyword_names_count && !named)) {
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
    if (view_export_open((PyObject*)self) != 0) {
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
    {"__arrow_c_device_array__", (PyCFunction)(void (*)(void))ustore_py_arrow_c_device_array, ustore_py_method_flags_k,
     ustore_py_arrow_c_device_array_doc},
    {"__dlpack__", (PyCFunction)(void (*)(void))ustore_py_dlpack, ustore_py_method_flags_k, ustore_py_dlpack_doc},
    {"__dlpack_device__", ustore_py_dlpack_device_answer, METH_NOARGS, ustore_py_dlpack_device_doc},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef Values_getset[] = {
    {"lengths", (getter)Values_get_lengths, NULL, doc_Values_lengths, NULL},
    {"__cuda_array_interface__", (getter)ustore_py_cuda_array_interface, NULL, ustore_py_cuda_array_interface_doc,
     NULL},
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
    .tp_methods = Values_methods,
    .tp_getset = Values_getset,
};

#pragma endregion Value Tapes

#pragma region Key Tapes

static char const doc_Keys[] =                                                                 //
    "The keys one batch scan or sample wrote into an `Arena`, viewing it until it rewinds.\n"  //
    "\n"                                                                                       //
    "Indexing answers a tuple of `uuid.UUID` keys for one task, which is where a copy\n"       //
    "happens. The whole tape exports through the buffer protocol as an array of shape\n"       //
    "`(keys, 16)` bytes, so `np.asarray(keys)` reads it without copying. Read after a later\n" //
    "call rewound the arena, it reads whatever the arena then holds.";

PyObject* ustore_py_keys_new(PyObject* arena, ustore_length_t const* offsets, ustore_key_t const* tape, size_t count,
                             float const* scores) {
    Keys* const instance = PyObject_New(Keys, &KeysType);
    if (!instance) return NULL;
    instance->arena = Py_NewRef(arena);
    instance->offsets = offsets;
    instance->tape = tape;
    instance->scores = scores;
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
    ustore_key_t const* const run = self->tape + self->offsets[task_index];
    Py_ssize_t const found = (Py_ssize_t)(self->offsets[task_index + 1] - self->offsets[task_index]);
    PyObject* keys = PyTuple_New(found);
    for (Py_ssize_t key_index = 0; keys && key_index != found; ++key_index) {
        PyObject* const key = ustore_py_uuid_to_object(&run[key_index].uuid);
        if (!key) Py_CLEAR(keys);
        else PyTuple_SET_ITEM(keys, key_index, key);
    }
    return keys;
}

static int Keys_getbuffer(Keys* self, Py_buffer* view, int flags) {
    view->obj = NULL;
    return ustore_py_buffer_fill(view, (PyObject*)self, self->tape, "B", 1, 2, self->shape, self->strides, flags);
}

static PyObject* Keys_repr(Keys* self) {
    return PyUnicode_FromFormat("Keys(tasks=%zu, keys=%zd)", self->count, self->shape[0]);
}

static char const doc_Keys_counts[] = "The number of keys every task found, as a tuple.";

static PyObject* Keys_get_counts(Keys* self, void* Py_UNUSED(closure)) {
    PyObject* counts = PyTuple_New((Py_ssize_t)self->count);
    for (size_t task_index = 0; counts && task_index != self->count; ++task_index) {
        PyObject* const entry = PyLong_FromUnsignedLongLong(self->offsets[task_index + 1] - self->offsets[task_index]);
        if (!entry) Py_CLEAR(counts);
        else PyTuple_SET_ITEM(counts, (Py_ssize_t)task_index, entry);
    }
    return counts;
}

static char const doc_Keys_scores[] =
    "One BM25 score per key, in the order of the tape, when the scan's patterns held weights, or None.";

static PyObject* Keys_get_scores(Keys* self, void* Py_UNUSED(closure)) {
    if (!self->scores) Py_RETURN_NONE;
    PyObject* scores = PyTuple_New(self->shape[0]);
    for (Py_ssize_t key_index = 0; scores && key_index != self->shape[0]; ++key_index) {
        PyObject* const score = PyFloat_FromDouble((double)self->scores[key_index]);
        if (!score) Py_CLEAR(scores);
        else PyTuple_SET_ITEM(scores, key_index, score);
    }
    return scores;
}

static PyMethodDef Keys_methods[] = {
    {"__arrow_c_array__", (PyCFunction)(void (*)(void))ustore_py_arrow_c_array, ustore_py_method_flags_k,
     ustore_py_arrow_c_array_doc},
    {"__arrow_c_device_array__", (PyCFunction)(void (*)(void))ustore_py_arrow_c_device_array, ustore_py_method_flags_k,
     ustore_py_arrow_c_device_array_doc},
    {"__dlpack__", (PyCFunction)(void (*)(void))ustore_py_dlpack, ustore_py_method_flags_k, ustore_py_dlpack_doc},
    {"__dlpack_device__", ustore_py_dlpack_device_answer, METH_NOARGS, ustore_py_dlpack_device_doc},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef Keys_getset[] = {
    {"counts", (getter)Keys_get_counts, NULL, doc_Keys_counts, NULL},
    {"scores", (getter)Keys_get_scores, NULL, doc_Keys_scores, NULL},
    {"__cuda_array_interface__", (getter)ustore_py_cuda_array_interface, NULL, ustore_py_cuda_array_interface_doc,
     NULL},
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
    .tp_methods = Keys_methods,
    .tp_getset = Keys_getset,
};

#pragma endregion Key Tapes

#pragma region Interchange Exports

/** What one Arrow array over the buffer of an output view keeps until Arrow releases it. */
typedef struct buffer_export_t {

    /** The buffer the view answered, holding the view and its arena. */
    Py_buffer view;

    /** No validity, then the bytes of every row. */
    void const* buffers[2];
} buffer_export_t;

/** Frees the <tt>fixed_size_binary</tt> format the schema spells. */
static void buffer_schema_release(struct ArrowSchema* schema) {
    PyMem_RawFree(schema->private_data);
    schema->release = NULL;
}

/** Releases the buffer, taking the interpreter, as a consumer may release from any thread. */
static void buffer_array_release(struct ArrowArray* array) {
    buffer_export_t* const exported = (buffer_export_t*)array->private_data;
    PyGILState_STATE const state = PyGILState_Ensure();
    view_export_close(exported->view.obj);
    PyBuffer_Release(&exported->view);
    PyGILState_Release(state);
    PyMem_RawFree(exported);
    array->release = NULL;
}

/** The capsules @c __arrow_c_array__ answers over @p schema and @p array, or NULL raising. */
static PyObject* arrow_capsules(struct ArrowSchema* schema, struct ArrowArray* array) {
    PyObject* const schema_capsule = PyCapsule_New(schema, "arrow_schema", values_schema_capsule_free);
    PyObject* const array_capsule = schema_capsule ? PyCapsule_New(array, "arrow_array", values_array_capsule_free)
                                                   : NULL;
    if (!schema_capsule) {
        schema->release(schema);
        PyMem_RawFree(schema);
    }
    if (!array_capsule) {
        array->release(array);
        PyMem_RawFree(array);
    }
    PyObject* const capsules = array_capsule ? PyTuple_Pack(2, schema_capsule, array_capsule) : NULL;
    Py_XDECREF(schema_capsule);
    Py_XDECREF(array_capsule);
    return capsules;
}

/** 0 when an Arrow export got at most the ignored @c requested_schema, or -1 raising. */
static int arrow_export_arguments(char const* method, Py_ssize_t positional_arguments_count,
                                  PyObject* keyword_names_tuple) {
    Py_ssize_t const keyword_names_count = keyword_names_tuple ? PyTuple_GET_SIZE(keyword_names_tuple) : 0;
    int const named = keyword_names_count == 1 &&
                      PyUnicode_CompareWithASCIIString(PyTuple_GET_ITEM(keyword_names_tuple, 0), "requested_schema") ==
                          0;
    if (positional_arguments_count + keyword_names_count <= 1 && (!keyword_names_count || named)) return 0;
    PyErr_Format(PyExc_TypeError, "%s() takes at most the 'requested_schema' argument", method);
    return -1;
}

char const ustore_py_arrow_c_array_doc[] =                                                   //
    "__arrow_c_array__(requested_schema=None) answers the schema and array PyCapsules.\n"    //
    "\n"                                                                                     //
    "Exports every row of the buffer as one Arrow `fixed_size_binary` item: 16 bytes per\n"  //
    "key, or 48 per edge of three keys. The array reads the arena in place and keeps this\n" //
    "object and its arena alive until Arrow releases it. A requested schema is ignored.";

PyObject* ustore_py_arrow_c_array(PyObject* self, PyObject* const* Py_UNUSED(arguments),
                                  Py_ssize_t positional_arguments_count, PyObject* keyword_names_tuple) {
    if (arrow_export_arguments("__arrow_c_array__", positional_arguments_count, keyword_names_tuple) != 0) return NULL;
    struct ArrowSchema* const schema = (struct ArrowSchema*)PyMem_RawCalloc(1, sizeof(struct ArrowSchema));
    struct ArrowArray* const array = (struct ArrowArray*)PyMem_RawCalloc(1, sizeof(struct ArrowArray));
    buffer_export_t* const exported = (buffer_export_t*)PyMem_RawCalloc(1, sizeof(buffer_export_t));
    char* const format = (char*)PyMem_RawCalloc(1, 32);
    if (!schema || !array || !exported || !format) {
        PyMem_RawFree(format);
        PyMem_RawFree(exported);
        PyMem_RawFree(array);
        PyMem_RawFree(schema);
        return PyErr_NoMemory();
    }
    int const opened = view_export_open(self) == 0;
    if (!opened || PyObject_GetBuffer(self, &exported->view, PyBUF_C_CONTIGUOUS) != 0) {
        if (opened) view_export_close(self);
        PyMem_RawFree(format);
        PyMem_RawFree(exported);
        PyMem_RawFree(array);
        PyMem_RawFree(schema);
        return NULL;
    }
    Py_buffer const* const view = &exported->view;
    // Every view exporting no Arrow type of its own is rows of bytes, laid out in C order.
    Py_ssize_t const rows = view->ndim ? view->shape[0] : 0;
    snprintf(format, 32, "w:%zd", view->ndim > 1 ? view->strides[0] : view->itemsize);
    *schema =
        (struct ArrowSchema) {.format = format, .name = "", .release = buffer_schema_release, .private_data = format};
    exported->buffers[1] = view->buf;
    *array = (struct ArrowArray) {.length = (int64_t)rows,
                                  .null_count = 0,
                                  .n_buffers = 2,
                                  .buffers = exported->buffers,
                                  .release = buffer_array_release,
                                  .private_data = exported};
    return arrow_capsules(schema, array);
}

/** Releases the array a device array holds, unless a consumer moved it out, and frees it. */
static void device_array_capsule_free(PyObject* capsule) {
    struct ArrowDeviceArray* const device_array = (struct ArrowDeviceArray*)PyCapsule_GetPointer(capsule,
                                                                                                 "arrow_device_array");
    if (device_array->array.release) device_array->array.release(&device_array->array);
    PyMem_RawFree(device_array);
}

char const ustore_py_arrow_c_device_array_doc[] =                                               //
    "__arrow_c_device_array__(requested_schema=None, **kwargs) answers the schema and device\n" //
    "array PyCapsules.\n"                                                                       //
    "\n"                                                                                        //
    "Exports what `__arrow_c_array__` exports, tagged with the device `__dlpack_device__`\n"    //
    "names: the CPU with ordinal -1 for plain host pages, and the vendor's device type and\n"   //
    "ordinal otherwise. The call that wrote the outputs is final, so no sync event is set.";

PyObject* ustore_py_arrow_c_device_array(PyObject* self, PyObject* const* Py_UNUSED(arguments),
                                         Py_ssize_t Py_UNUSED(positional_arguments_count),
                                         PyObject* Py_UNUSED(keyword_names_tuple)) {
    DLDevice device;
    if (view_device(self, &device) != 0) return NULL;
    PyObject* const capsules = PyObject_CallMethod(self, "__arrow_c_array__", NULL);
    if (!capsules) return NULL;
    struct ArrowDeviceArray* const device_array = (struct ArrowDeviceArray*)PyMem_RawCalloc(
        1, sizeof(struct ArrowDeviceArray));
    if (!device_array) {
        Py_DECREF(capsules);
        return PyErr_NoMemory();
    }
    struct ArrowArray* const array = (struct ArrowArray*)PyCapsule_GetPointer(PyTuple_GET_ITEM(capsules, 1),
                                                                              "arrow_array");
    device_array->array = *array;
    array->release = NULL;
    device_array->device_type = (ArrowDeviceType)device.device_type;
    device_array->device_id = device.device_type == kDLCPU ? -1 : (int64_t)device.device_id;
    PyObject* const device_capsule = PyCapsule_New(device_array, "arrow_device_array", device_array_capsule_free);
    if (!device_capsule) {
        device_array->array.release(&device_array->array);
        PyMem_RawFree(device_array);
        Py_DECREF(capsules);
        return NULL;
    }
    PyObject* const answer = PyTuple_Pack(2, PyTuple_GET_ITEM(capsules, 0), device_capsule);
    Py_DECREF(device_capsule);
    Py_DECREF(capsules);
    return answer;
}

char const ustore_py_cuda_array_interface_doc[] =                                                //
    "The CUDA Array Interface, version 3, of outputs in NVIDIA device or managed memory, with\n" //
    "no stream, as the call that wrote them is final; any other memory has none, and reading\n"  //
    "the attribute raises AttributeError.";

PyObject* ustore_py_cuda_array_interface(PyObject* self, void* Py_UNUSED(closure)) {
    DLDevice device;
    if (view_device(self, &device) != 0) return NULL;
    if (device.device_type != kDLCUDA && device.device_type != kDLCUDAManaged) {
        PyErr_SetString(PyExc_AttributeError, "Only NVIDIA device or managed memory has __cuda_array_interface__");
        return NULL;
    }
    Py_buffer view;
    if (PyObject_GetBuffer(self, &view, PyBUF_RECORDS_RO) != 0) return NULL;
    char const kind = view.format ? view.format[0] : 'B';
    char const code = kind == '?' ? 'b' : strchr("efd", kind) ? 'f' : strchr("bhilqn", kind) ? 'i' : 'u';
    char typestr[8];
    snprintf(typestr, sizeof(typestr), "%c%c%zd", view.itemsize == 1 ? '|' : '<', code, view.itemsize);
    PyObject* shape = PyTuple_New(view.ndim);
    for (int dimension = 0; shape && dimension != view.ndim; ++dimension) {
        PyObject* const items = PyLong_FromSsize_t(view.shape[dimension]);
        if (!items) Py_CLEAR(shape);
        else PyTuple_SET_ITEM(shape, dimension, items);
    }
    PyObject* const address = PyLong_FromVoidPtr(view.buf);
    PyBuffer_Release(&view);
    PyObject* const data = address ? PyTuple_Pack(2, address, Py_True) : NULL;
    PyObject* const typestr_object = PyUnicode_FromString(typestr);
    PyObject* const version = PyLong_FromLong(3);
    PyObject* described = shape && data && typestr_object && version ? PyDict_New() : NULL;
    if (described && (PyDict_SetItemString(described, "shape", shape) != 0 ||
                      PyDict_SetItemString(described, "typestr", typestr_object) != 0 ||
                      PyDict_SetItemString(described, "data", data) != 0 ||
                      PyDict_SetItemString(described, "strides", Py_None) != 0 ||
                      PyDict_SetItemString(described, "version", version) != 0 ||
                      PyDict_SetItemString(described, "stream", Py_None) != 0))
        Py_CLEAR(described);
    Py_XDECREF(version);
    Py_XDECREF(typestr_object);
    Py_XDECREF(data);
    Py_XDECREF(address);
    Py_XDECREF(shape);
    return described;
}

#pragma endregion Interchange Exports

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

int ustore_py_optional_argument(char const* verb, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                                PyObject* keyword_names_tuple, Py_ssize_t required_positional, char const* keyword,
                                PyObject** argument) {
    Py_ssize_t const keyword_names_count = keyword_names_tuple ? PyTuple_GET_SIZE(keyword_names_tuple) : 0;
    int const named = keyword_names_count == 1 &&
                      PyUnicode_CompareWithASCIIString(PyTuple_GET_ITEM(keyword_names_tuple, 0), keyword) == 0;
    Py_ssize_t const given = positional_arguments_count + keyword_names_count;
    if (positional_arguments_count < required_positional || given > required_positional + 1 ||
        (keyword_names_count && !named)) {
        PyErr_Format(PyExc_TypeError, "%s() takes %zd positional arguments and then '%s', received %zd arguments", verb,
                     required_positional, keyword, given);
        return -1;
    }
    if (given > required_positional) *argument = arguments[required_positional];
    return 0;
}

Py_ssize_t ustore_py_keyword_names_count_checked(char const* verb, Py_ssize_t positional_arguments_count,
                                                 PyObject* keyword_names_tuple, Py_ssize_t required_positional) {
    if (positional_arguments_count != required_positional) {
        PyErr_Format(PyExc_TypeError, "%s() takes %zd positional arguments, received %zd", verb, required_positional,
                     positional_arguments_count);
        return -1;
    }
    return keyword_names_tuple ? PyTuple_GET_SIZE(keyword_names_tuple) : 0;
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
        PyErr_Format(PyExc_TypeError, "Expected a Database, Transaction, Snapshot or On, got %S", Py_TYPE(origin));
        return -1;
    }
    context->origin = origin;
    return 0;
}

int ustore_py_context_from_object(PyObject* self, ustore_py_context_t* context) {
    memset(context, 0, sizeof(*context));
    if (!PyObject_TypeCheck(self, &OnType) && !PyObject_TypeCheck(self, &DeferredOnType)) {
        ustore_watch_t const watch = PyObject_TypeCheck(self, &TransactionType) ? ((Transaction*)self)->watch
                                                                                : ustore_watch_keys_k;
        return context_from_origin(self, watch, context);
    }
    On const* const view = (On const*)self;
    if (view->executor && !view->executor->handle)
        return ustore_py_raise(ustore_status_uninitialized_k, "The executor is closed");
    context->executor = view->executor;
    context->completion = PyObject_TypeCheck(self, &DeferredOnType) ? ustore_completion_deferred_k
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
    [ustore_py_enum_cpu_class_k] = "CPUClass",
    [ustore_py_enum_watch_k] = "Watch",
    [ustore_py_enum_durability_k] = "Durability",
    [ustore_py_enum_drop_mode_k] = "DropMode",
    [ustore_py_enum_scan_mode_k] = "ScanMode",
    [ustore_py_enum_scan_direction_k] = "ScanDirection",
    [ustore_py_enum_modality_k] = "Modality",
    [ustore_py_enum_isolation_k] = "Isolation",
    [ustore_py_enum_transport_k] = "Transport",
    [ustore_py_enum_gds_k] = "Gds",
    [ustore_py_enum_gpu_vendor_k] = "GPUVendor",
    [ustore_py_enum_gpu_class_k] = "GPUClass",
    [ustore_py_enum_pattern_kind_k] = "PatternKind",
    [ustore_py_enum_case_sensitivity_k] = "CaseSensitivity",
    [ustore_py_enum_predicate_cost_k] = "PredicateCost",
    [ustore_py_enum_engine_k] = "Engine",
    [ustore_py_enum_role_k] = "Role",
    [ustore_py_enum_field_type_k] = "FieldType",
    [ustore_py_enum_modification_k] = "Modification",
    [ustore_py_enum_page_encoding_k] = "PageEncoding",
    [ustore_py_enum_change_kind_k] = "ChangeKind",
    [ustore_py_enum_phase_k] = "Phase",
};

/** Every enumerator of every closed set fits a byte, as the C suite checks, so walks find all. */
enum { enum_values_limit_k = 256 };

/** What the C <tt>_name</tt> functions answer for a value no enumerator holds. */
static char const enum_unrecognized_k[] = "unrecognized";

/** The canonical spelling of @p value as an enumerator of @p kind, or "unrecognized" where it is none. */
static char const* enum_spelling(ustore_py_enum_t kind, int value) {
    switch (kind) {
    case ustore_py_enum_status_k: return ustore_status_name((ustore_status_t)value);
    case ustore_py_enum_mood_k: return ustore_mood_name((ustore_mood_t)value);
    case ustore_py_enum_cpu_class_k: return ustore_cpu_class_name((ustore_cpu_class_t)value);
    case ustore_py_enum_watch_k: return ustore_watch_name((ustore_watch_t)value);
    case ustore_py_enum_durability_k: return ustore_durability_name((ustore_durability_t)value);
    case ustore_py_enum_drop_mode_k: return ustore_drop_mode_name((ustore_drop_mode_t)value);
    case ustore_py_enum_scan_mode_k: return ustore_scan_mode_name((ustore_scan_mode_t)value);
    case ustore_py_enum_scan_direction_k: return ustore_scan_direction_name((ustore_scan_direction_t)value);
    case ustore_py_enum_modality_k: return ustore_modality_name((ustore_modality_t)value);
    case ustore_py_enum_isolation_k: return ustore_isolation_name((ustore_isolation_t)value);
    case ustore_py_enum_transport_k: return ustore_transport_name((ustore_transport_t)value);
    case ustore_py_enum_gds_k: return ustore_gds_name((ustore_gds_t)value);
    case ustore_py_enum_gpu_vendor_k: return ustore_gpu_vendor_name((ustore_gpu_vendor_t)value);
    case ustore_py_enum_gpu_class_k: return ustore_gpu_class_name((ustore_gpu_class_t)value);
    case ustore_py_enum_pattern_kind_k: return ustore_pattern_kind_name((ustore_pattern_kind_t)value);
    case ustore_py_enum_case_sensitivity_k: return ustore_case_sensitivity_name((ustore_case_sensitivity_t)value);
    case ustore_py_enum_predicate_cost_k: return ustore_predicate_cost_name((ustore_predicate_cost_t)value);
    case ustore_py_enum_engine_k: return ustore_engine_name((ustore_engine_t)value);
    case ustore_py_enum_role_k: return ustore_vertex_role_name((ustore_vertex_role_t)value);
    case ustore_py_enum_field_type_k: return ustore_docs_field_type_name((ustore_docs_field_type_t)value);
    case ustore_py_enum_modification_k: return ustore_docs_modification_name((ustore_docs_modification_t)value);
    case ustore_py_enum_page_encoding_k: return ustore_page_encoding_name((ustore_page_encoding_t)value);
    case ustore_py_enum_change_kind_k: return ustore_change_kind_name((ustore_change_kind_t)value);
    case ustore_py_enum_phase_k: return ustore_phase_name((ustore_phase_t)value);
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

int ustore_py_verb_options_from_keywords(ustore_py_context_t const* context, PyObject* const* arguments,
                                         Py_ssize_t positional_arguments_count, PyObject* keyword_names_tuple,
                                         ustore_py_verb_options_t* options) {
    ustore_py_verb_options_init(options);
    Py_ssize_t const keyword_names_count = keyword_names_tuple ? PyTuple_GET_SIZE(keyword_names_tuple) : 0;
    for (Py_ssize_t names_progress = 0, arguments_progress = positional_arguments_count;
         names_progress != keyword_names_count; ++arguments_progress, ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(keyword_names_tuple, names_progress);
        int const consumed = ustore_py_verb_option_from_keyword(context, name, arguments[arguments_progress], options);
        if (consumed < 0) return -1;
        if (consumed == 0) {
            PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
            return -1;
        }
    }
    return 0;
}

int ustore_py_verb_keywords_from_arguments(ustore_py_context_t const* context, PyObject* const* arguments,
                                           Py_ssize_t positional_arguments_count, PyObject* keyword_names_tuple,
                                           char const* const* names, ustore_py_verb_options_t* options,
                                           PyObject** values) {
    ustore_py_verb_options_init(options);
    for (size_t index = 0; names[index]; ++index) values[index] = NULL;
    Py_ssize_t const keyword_names_count = keyword_names_tuple ? PyTuple_GET_SIZE(keyword_names_tuple) : 0;
    for (Py_ssize_t names_progress = 0, arguments_progress = positional_arguments_count;
         names_progress != keyword_names_count; ++arguments_progress, ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(keyword_names_tuple, names_progress);
        PyObject* const value = arguments[arguments_progress];
        int const consumed = ustore_py_verb_option_from_keyword(context, name, value, options);
        if (consumed < 0) return -1;
        if (consumed > 0) continue;
        size_t index = 0;
        while (names[index] && PyUnicode_CompareWithASCIIString(name, names[index]) != 0) ++index;
        if (!names[index]) {
            PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
            return -1;
        }
        values[index] = value == Py_None ? NULL : value;
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
    atomic_init(&deferred->answering, 0);
    atomic_init(&deferred->answer, NULL);
    atomic_init(&deferred->raised, NULL);
    memset(deferred->call, 0, verb->call_bytes);
    PyObject_GC_Track(deferred);
    return deferred->call;
}

/** Answers @p call once it is final: its outcome's failure, else its answer. */
static PyObject* call_answer(ustore_py_verb_t const* verb, void* call) {
    ustore_py_call_t* const header = (ustore_py_call_t*)call;
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
    atomic_store(&deferred->answering, 1);
    PyObject* const raised = PyErr_GetRaisedException();
    Py_DECREF(deferred);
    PyErr_SetRaisedException(raised);
    return NULL;
}

void ustore_py_call_release(ustore_py_call_t* call) {
    if (call->arena) Py_DECREF(call->arena);
    call->arena = NULL;
    Py_CLEAR(call->progress);
    Py_CLEAR(call->result);
    Py_CLEAR(call->predicate.handler);
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
 *  Reports never overlap and never follow the final status, but nothing places them on the
 *  calling thread, so the interpreter is attached here rather than assumed. An answer of None
 *  continues, as does a truthy one, while a falsy one cancels, and a raise reports through the
 *  unraisable hook and cancels.
 */
static ustore_continuation_t progress_trampoline(ustore_size_t finished, ustore_size_t total, void* state) {
    ustore_py_call_t* const call = (ustore_py_call_t*)state;
    PyGILState_STATE const interpreter = PyGILState_Ensure();
    PyObject* const finished_object = PyLong_FromUnsignedLongLong(finished);
    PyObject* const total_object = finished_object ? PyLong_FromUnsignedLongLong(total) : NULL;
    PyObject* const arguments[2] = {finished_object, total_object};
    PyObject* const verdict = total_object ? PyObject_Vectorcall(call->progress, arguments, 2, NULL) : NULL;
    int const truth = !verdict ? -1 : verdict == Py_None ? 1 : PyObject_IsTrue(verdict);
    Py_XDECREF(verdict);
    Py_XDECREF(total_object);
    Py_XDECREF(finished_object);
    if (truth < 0) PyErr_WriteUnraisable(call->progress);
    PyGILState_Release(interpreter);
    return truth > 0 ? ustore_continue_k : ustore_cancel_k;
}

ustore_progress_callback_t ustore_py_progress_callback(ustore_py_call_t const* call) {
    return call->progress ? progress_trampoline : NULL;
}

int ustore_py_predicate_from_object(PyObject* object, PyObject** handler) {
    *handler = NULL;
    if (object == Py_None) return 0;
    if (!PyCallable_Check(object) && !Py_IS_TYPE(object, &BatchType)) {
        PyErr_Format(PyExc_TypeError, "The 'predicate' option takes a callable, a ustore.Batch or None, got %S",
                     Py_TYPE(object));
        return -1;
    }
    *handler = object;
    return 0;
}

/**
 *  @brief Re-enters Python for one page of the scan whose predicate is @p state, asking its
 *      callable about each key in turn as a <tt>uuid.UUID</tt>, from whichever thread runs it.
 *
 *  A callable that raises reports through the unraisable hook, and its key and the rest of the
 *  page are rejected.
 */
static void predicate_each_trampoline(ustore_key_t const* keys, ustore_admission_t* admissions, ustore_size_t count,
                                      void* state) {
    PyObject* const callable = ((ustore_py_predicate_t const*)state)->handler;
    PyGILState_STATE const interpreter = PyGILState_Ensure();
    ustore_size_t index = 0;
    for (; index != count; ++index) {
        PyObject* const key = ustore_py_uuid_to_object(&keys[index].uuid);
        PyObject* const verdict = key ? PyObject_CallOneArg(callable, key) : NULL;
        Py_XDECREF(key);
        int const truth = verdict ? PyObject_IsTrue(verdict) : -1;
        Py_XDECREF(verdict);
        if (truth < 0) break;
        admissions[index] = truth ? ustore_admit_k : ustore_reject_k;
    }
    if (index != count) PyErr_WriteUnraisable(callable);
    for (; index != count; ++index) admissions[index] = ustore_reject_k;
    PyGILState_Release(interpreter);
}

/** A batch predicate: the handler answering a page of keys per call, and where a scan asks it. */
typedef struct Batch {
    PyObject ob_base;

    /** The callable taking the keys and the admissions to write, held. */
    PyObject* handler;

    /** Where in a scan @c handler is asked, fixed at construction. */
    ustore_predicate_cost_t cost;
} Batch;

_Static_assert(sizeof(ustore_admission_t) == sizeof(unsigned int), "admissions cross as an 'I' memoryview");

/** Closes @p view, stolen, so a handler keeping it past its call reads nothing; one still
 *  exporting it reports through the unraisable hook. @return 0 on success, -1 otherwise. */
static int predicate_view_close(PyObject* view, PyObject* handler) {
    if (!view) return 0;
    PyObject* const closed = PyObject_CallMethod(view, "release", NULL);
    Py_DECREF(view);
    if (closed) {
        Py_DECREF(closed);
        return 0;
    }
    PyErr_WriteUnraisable(handler);
    return -1;
}

/**
 *  @brief Re-enters Python for one page of the scan whose predicate is @p state, handing its
 *      @c Batch handler the keys and the admissions as memoryviews over the library's buffers.
 *
 *  The keys are a read-only "B" view of @p count rows of 16 bytes, and the admissions a writable
 *  "I" view of @p count entries the library pre-filled with rejections. Both close when the handler
 *  returns. A handler that raises, or keeps a view exported past its call, reports through the
 *  unraisable hook, and the whole page is rejected.
 */
static void predicate_batch_trampoline(ustore_key_t const* keys, ustore_admission_t* admissions, ustore_size_t count,
                                       void* state) {
    PyObject* const handler = ((Batch const*)((ustore_py_predicate_t const*)state)->handler)->handler;
    PyGILState_STATE const interpreter = PyGILState_Ensure();
    Py_ssize_t keys_shape[2] = {(Py_ssize_t)count, (Py_ssize_t)sizeof(ustore_key_t)};
    Py_ssize_t admissions_shape[1] = {(Py_ssize_t)count};
    Py_buffer keys_buffer = {.buf = (void*)keys,
                             .len = (Py_ssize_t)(count * sizeof(ustore_key_t)),
                             .itemsize = 1,
                             .readonly = 1,
                             .ndim = 2,
                             .format = "B",
                             .shape = keys_shape};
    Py_buffer admissions_buffer = {.buf = admissions,
                                   .len = (Py_ssize_t)(count * sizeof(ustore_admission_t)),
                                   .itemsize = sizeof(ustore_admission_t),
                                   .ndim = 1,
                                   .format = "I",
                                   .shape = admissions_shape};
    PyObject* const keys_view = PyMemoryView_FromBuffer(&keys_buffer);
    PyObject* const admissions_view = keys_view ? PyMemoryView_FromBuffer(&admissions_buffer) : NULL;
    PyObject* const arguments[2] = {keys_view, admissions_view};
    PyObject* const answered = admissions_view ? PyObject_Vectorcall(handler, arguments, 2, NULL) : NULL;
    if (!answered) PyErr_WriteUnraisable(handler);
    Py_XDECREF(answered);
    int const kept = predicate_view_close(keys_view, handler) | predicate_view_close(admissions_view, handler);
    if (!answered || kept)
        for (ustore_size_t index = 0; index != count; ++index) admissions[index] = ustore_reject_k;
    PyGILState_Release(interpreter);
}

ustore_predicate_t ustore_py_predicate_of(ustore_py_predicate_t* predicate) {
    PyObject* const handler = predicate->handler;
    if (!handler) return (ustore_predicate_t) {0};
    if (Py_IS_TYPE(handler, &BatchType))
        return (ustore_predicate_t) {predicate_batch_trampoline, predicate, ((Batch const*)handler)->cost};
    return (ustore_predicate_t) {predicate_each_trampoline, predicate, ustore_predicate_cost_cheap_k};
}

static char const doc_Batch[] =                                                                      //
    "Batch(handler, /, *, cost=PredicateCost.EXPENSIVE)\n"                                           //
    "\n"                                                                                             //
    "A `predicate=` that answers a page of keys in one call, for handlers that pay per call, such\n" //
    "as a lookup in an external database. `handler(keys, admissions)` reads a `(count, 16)`\n"       //
    "memoryview of key bytes and writes 1 into a `(count,)` memoryview of admissions for every\n"    //
    "key it keeps, the rest staying rejected. Both views are valid only for the call. An\n"          //
    "expensive batch is asked about the keys patterns and filters kept, and a cheap one before\n"    //
    "they read any value. Wraps `ustore_predicate_t`.\n"                                             //
    "\n"                                                                                             //
    "Example:\n"                                                                                     //
    "  >>> def even(keys, admissions):\n"                                                            //
    "  ...     for row in range(len(admissions)):\n"                                                 //
    "  ...         admissions[row] = keys[row, 15] % 2 == 0\n"                                       //
    "  >>> database.scan(None, 100, arena, collection='posters', predicate=ustore.Batch(even))";

static PyObject* Batch_tp_new(PyTypeObject* type, PyObject* args, PyObject* keywords) {
    PyObject* const cost_object = keywords ? PyDict_GetItemString(keywords, "cost") : NULL;
    if (PyTuple_GET_SIZE(args) != 1 || (keywords && PyDict_GET_SIZE(keywords) != (cost_object != NULL))) {
        PyErr_SetString(PyExc_TypeError, "Batch() takes one positional handler and an optional cost=");
        return NULL;
    }
    PyObject* const handler = PyTuple_GET_ITEM(args, 0);
    if (!PyCallable_Check(handler)) {
        PyErr_Format(PyExc_TypeError, "Batch() takes a callable handler, got %S", Py_TYPE(handler));
        return NULL;
    }
    int cost = ustore_predicate_cost_expensive_k;
    if (cost_object && ustore_py_enum_from_object(cost_object, "cost", ustore_py_enum_predicate_cost_k, &cost) != 0)
        return NULL;
    Batch* const batch = (Batch*)type->tp_alloc(type, 0);
    if (!batch) return NULL;
    batch->handler = Py_NewRef(handler);
    batch->cost = (ustore_predicate_cost_t)cost;
    return (PyObject*)batch;
}

static int Batch_traverse(Batch* self, visitproc visit, void* arg) {
    Py_VISIT(self->handler);
    return 0;
}

static int Batch_clear(Batch* self) {
    Py_CLEAR(self->handler);
    return 0;
}

static void Batch_dealloc(Batch* self) {
    PyObject_GC_UnTrack(self);
    Batch_clear(self);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject* Batch_get_handler(Batch* self, void* Py_UNUSED(closure)) { return Py_NewRef(self->handler); }

static PyObject* Batch_get_cost(Batch* self, void* Py_UNUSED(closure)) {
    return ustore_py_enum_to_object(ustore_py_enum_predicate_cost_k, (int)self->cost);
}

static PyGetSetDef Batch_getset[] = {
    {"handler", (getter)Batch_get_handler, NULL, "The callable taking the keys and the admissions.", NULL},
    {"cost", (getter)Batch_get_cost, NULL, "The `PredicateCost` placing the handler in a scan.", NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

PyTypeObject BatchType = {
    PyVarObject_HEAD_INIT(NULL, 0) //
        .tp_name = "ustore.Batch",
    .tp_doc = doc_Batch,
    .tp_basicsize = sizeof(Batch),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_new = Batch_tp_new,
    .tp_dealloc = (destructor)Batch_dealloc,
    .tp_traverse = (traverseproc)Batch_traverse,
    .tp_clear = (inquiry)Batch_clear,
    .tp_getset = Batch_getset,
};

#pragma endregion Calls

#pragma region Deferred Calls

static char const doc_Deferred[] =                                                                         //
    "A call a deferred view submitted, which answers once it finishes.\n"                                  //
    "\n"                                                                                                   //
    "Built by the verbs of a DeferredOn, never by name. `await` suspends the coroutine until the call\n"   //
    "is final, re-polling `status` once per turn of the running loop and parking no thread, and\n"         //
    "`wait()` blocks the thread instead. Either answers what the blocking verb would have answered, or\n"  //
    "raises what it would have raised, as many times as it is asked. The object holds the outcome, the\n"  //
    "keys and buffers the call views, its output slots and its arena until the call finishes, so\n"        //
    "dropping it waits for that; meanwhile C refuses another call naming the arena with ContendedError.\n" //
    "\n"                                                                                                   //
    "Example:\n"                                                                                           //
    "  >>> async def read(database, executor, keys):\n"                                                    //
    "  ...     reading = database.deferred(executor).read(keys, ustore.Arena(executor))\n"                 //
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

/** Finishes a final @p self exactly once: the one thread taking the call answers it, lets go
 *  of its inputs and its arena, then publishes. */
static void deferred_finish(Deferred* self) {
    if (atomic_exchange(&self->answering, 1)) return;
    PyObject* const answer = call_answer(self->verb, self->call);
    PyObject* const raised = answer ? NULL : PyErr_GetRaisedException();
    call_release(self->verb, self->call);
    if (answer) atomic_store_explicit(&self->answer, answer, memory_order_release);
    else atomic_store_explicit(&self->raised, raised, memory_order_release);
}

/** Answers what a final @p self finished with, finishing it first when nobody has; a thread
 *  asking while another still answers is refused as contended rather than made to wait. */
static PyObject* deferred_answer(Deferred* self) {
    deferred_finish(self);
    PyObject* const raised = atomic_load_explicit(&self->raised, memory_order_acquire);
    if (raised) {
        PyErr_SetRaisedException(Py_NewRef(raised));
        return NULL;
    }
    PyObject* const answer = atomic_load_explicit(&self->answer, memory_order_acquire);
    if (answer) return Py_NewRef(answer);
    ustore_py_raise(ustore_status_contended_k, "Another thread is answering this call");
    return NULL;
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

/** Answers the iterator of the <tt>ustore._settled</tt> coroutine, which re-polls the status once per
 *  turn of the running loop and never parks a thread, then answers the wait. */
static PyObject* Deferred_await(Deferred* self) {
    PyObject* const package = PyImport_ImportModule("ustore");
    PyObject* const settled = package ? PyObject_GetAttrString(package, "_settled") : NULL;
    Py_XDECREF(package);
    PyObject* const coroutine = settled ? PyObject_CallOneArg(settled, (PyObject*)self) : NULL;
    Py_XDECREF(settled);
    PyObject* const awaiting = coroutine ? PyObject_CallMethod(coroutine, "__await__", NULL) : NULL;
    Py_XDECREF(coroutine);
    return awaiting;
}

static void Deferred_dealloc(Deferred* self) {
    PyObject_GC_UnTrack(self);
    // The library writes into the call this object holds, and reads the inputs it views, until
    // the call finishes
    if (!atomic_load(&self->answering)) {
        deferred_block(self);
        call_release(self->verb, self->call);
    }
    Py_XDECREF(atomic_load(&self->answer));
    Py_XDECREF(atomic_load(&self->raised));
    Py_XDECREF(self->origin);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

/** Visits what @p self holds; an unfinished call's arena and callables stay unvisited, so every
 *  cycle through them waits for a later collection rather than being cleared while the library
 *  still uses them. */
static int Deferred_traverse(Deferred* self, visitproc visit, void* arg) {
    PyObject* const answer = atomic_load_explicit(&self->answer, memory_order_relaxed);
    PyObject* const raised = atomic_load_explicit(&self->raised, memory_order_relaxed);
    Py_VISIT(self->origin);
    Py_VISIT(answer);
    Py_VISIT(raised);
    if (atomic_load(&self->answering) || deferred_status(self) == ustore_status_deferred_k) return 0;
    ustore_py_call_t const* const header = (ustore_py_call_t const*)(void const*)self->call;
    Py_VISIT(header->arena);
    Py_VISIT(header->progress);
    Py_VISIT(header->result);
    Py_VISIT(header->predicate.handler);
    return 0;
}

/** Breaks a garbage cycle through a final @p self; an unfinished one is left whole, as its traversal hid the cycle. */
static int Deferred_clear(Deferred* self) {
    if (deferred_status(self) == ustore_status_deferred_k) return 0;
    if (!atomic_exchange(&self->answering, 1)) call_release(self->verb, self->call);
    Py_XDECREF(atomic_exchange(&self->answer, NULL));
    Py_XDECREF(atomic_exchange(&self->raised, NULL));
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

static char const doc_Executor[] =                                                              //
    "The threads that run calls, shared by any number of databases.\n"                          //
    "\n"                                                                                        //
    "Built through `Executor.for_ram()`, `for_nvme()` or `for_vram()`, never by name.\n"        //
    "A receiver names it through `on(executor)`, whose calls block on it, or\n"                 //
    "`deferred(executor)`, whose calls its own first thread runs while the caller continues,\n" //
    "so polling alone finishes them. It serves one call at a time: a second call while one\n"   //
    "runs raises ContendedError once waited for. Progress callbacks run on its threads.\n"      //
    "\n"                                                                                        //
    "Used as a context manager, it waits for its calls and frees its threads on exit.";

static PyObject* Executor_tp_new(PyTypeObject* Py_UNUSED(type), PyObject* Py_UNUSED(arguments),
                                 PyObject* Py_UNUSED(keywords)) {
    PyErr_SetString(PyExc_TypeError, "An executor is built through Executor.for_ram(), .for_nvme() or .for_vram()");
    return NULL;
}

/**
 *  @brief Reads the arguments of an executor factory into @p values, each left NULL when the
 *      call held none.
 *  @param[in] names The names of the arguments, in the order they may come by position.
 *  @param[in] positional How many of @p names may come by position.
 *  @param[in] keyword The first of @p names that may come by name, so those before it come by position alone.
 *  @return 0 on success, -1 with a @c TypeError set for one too many, unknown or repeated.
 */
static int factory_arguments(char const* factory, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                             PyObject* keyword_names_tuple, char const* const* names, PyObject** values,
                             Py_ssize_t names_count, Py_ssize_t positional, Py_ssize_t keyword) {
    if (positional_arguments_count > positional) {
        PyErr_Format(PyExc_TypeError, "%s() takes at most %zd positional arguments, received %zd", factory, positional,
                     positional_arguments_count);
        return -1;
    }
    for (Py_ssize_t index = 0; index != names_count; ++index)
        values[index] = index < positional_arguments_count ? arguments[index] : NULL;
    Py_ssize_t const keyword_names_count = keyword_names_tuple ? PyTuple_GET_SIZE(keyword_names_tuple) : 0;
    for (Py_ssize_t names_progress = 0; names_progress != keyword_names_count; ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(keyword_names_tuple, names_progress);
        Py_ssize_t index = keyword;
        while (index != names_count && PyUnicode_CompareWithASCIIString(name, names[index]) != 0) ++index;
        if (index == names_count || values[index]) {
            PyErr_Format(PyExc_TypeError, "%s() got an unexpected or repeated argument %R", factory, name);
            return -1;
        }
        values[index] = arguments[positional_arguments_count + names_progress];
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

static char const doc_Executor_for_ram[] =                                                        //
    "for_ram(threads=0, *, memory_domains=None, scratch_limit=0) answers an executor of host\n"   //
    "threads.\n"                                                                                  //
    "\n"                                                                                          //
    "Runs the calls of the RAM, RocksDB and LevelDB engines, keeping the scratch of every\n"      //
    "thread on its local memory domain and every arena it makes on the first one named. Calls\n"  //
    "`ustore_executor_for_ram`.\n"                                                                //
    "\n"                                                                                          //
    "Args:\n"                                                                                     //
    "  threads (int, optional): Threads to spawn; zero takes all but one in eight of the cores\n" //
    "    the memory domains hold, less the caller's, leaving the rest to blocking callers and\n"  //
    "    the engines' own background work.\n"                                                     //
    "  memory_domains (Sequence[int], optional): NUMA nodes the threads stay near and keep\n"     //
    "    their memory on; None derives every node the process may allocate on.\n"                 //
    "  scratch_limit (int, optional): Bytes of scratch each thread's calls carve from, past\n"    //
    "    which a call raises OutOfMemoryError; zero takes 16 GiB.\n"                              //
    "Returns:\n"                                                                                  //
    "  Executor: The new executor.\n"                                                             //
    "Raises:\n"                                                                                   //
    "  InvalidArgumentError: A memory domain names no NUMA node.";

static PyObject* Executor_for_ram(PyObject* type, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                                  PyObject* keyword_names_tuple) {
    static char const* const names[] = {"threads", "memory_domains", "scratch_limit"};
    PyObject* values[3];
    size_t threads = 0, scratch_limit = 0, memory_domains_count = 0;
    int32_t* memory_domains = NULL;
    if (factory_arguments("for_ram", arguments, positional_arguments_count, keyword_names_tuple, names, values, 3, 1,
                          0) != 0 ||
        (values[0] && ustore_py_size_from_object(values[0], "threads", &threads) != 0) ||
        (values[2] && ustore_py_size_from_object(values[2], "scratch_limit", &scratch_limit) != 0) ||
        identifiers_from_object(values[1], "memory_domains", &memory_domains, &memory_domains_count) != 0)
        return NULL;

    ustore_executor_t handle = NULL;
    ustore_outcome_t outcome = {0};
    ustore_executor_for_ram_t made = {
        .executor = &handle,
        .outcome = &outcome,
        .ram = {memory_domains, (ustore_size_t)memory_domains_count},
        .threads = (ustore_size_t)threads,
        .scratch_limit = (ustore_size_t)scratch_limit,
    };
    Py_BEGIN_ALLOW_THREADS;
    ustore_executor_for_ram(&made);
    Py_END_ALLOW_THREADS;
    PyMem_Free(memory_domains);
    return executor_made(type, handle, &outcome);
}

static char const doc_Executor_for_nvme[] =                                                     //
    "for_nvme(threads=0, *, transport=Transport.AUTOMATIC, paths=None, memory_domains=None,\n"  //
    "scratch_limit=0, **sizes) answers an executor of I/O rings.\n"                             //
    "\n"                                                                                        //
    "Runs the calls of the NVMe engine, every thread submitting and polling a ring of its\n"    //
    "own, and tells every database Database.open_nvme() opens on it how to move bytes. Every\n" //
    "size keyword names a field of ustore_nvme_options_t: queue_depth, request_buffer_bytes,\n" //
    "request_buffers_count, stream_buffer_bytes or stream_buffers_count, where zero keeps\n"    //
    "the engine's default. Calls `ustore_executor_for_nvme`.\n"                                 //
    "\n"                                                                                        //
    "Args:\n"                                                                                   //
    "  threads (int, optional): Threads to spawn; zero takes the fewest hardware queues of\n"   //
    "    the disks under `paths`, capped by the cores.\n"                                       //
    "  transport (Transport, optional): How bytes move between the files and memory.\n"         //
    "  paths (Sequence, optional): The data directories of the databases it will open, whose\n" //
    "    shared NUMA node places the threads unless `memory_domains` names one.\n"              //
    "  memory_domains (Sequence[int], optional): NUMA nodes the threads and their staging\n"    //
    "    live on, the first placing the threads.\n"                                             //
    "  scratch_limit (int, optional): Bytes of scratch each thread's calls carve from, past\n"  //
    "    which a call raises OutOfMemoryError; zero takes 16 GiB.\n"                            //
    "Returns:\n"                                                                                //
    "  Executor: The new executor.\n"                                                           //
    "Raises:\n"                                                                                 //
    "  InvalidArgumentError: A memory domain names no NUMA node.";

/**
 *  @brief Encodes @p object, None or a sequence of paths, into views of their bytes.
 *  @param[out] encoded The list the caller releases, which keeps the encoded bytes alive, or
 *      NULL when @p object is None.
 *  @param[out] views The views into @c encoded, or NULL for None, which the caller frees.
 *  @param[out] count Entries of @c views.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
static int paths_from_object(PyObject* object, PyObject** encoded, ustore_bytes_view_t** views, size_t* count) {
    *encoded = NULL;
    *views = NULL;
    *count = 0;
    if (!object || object == Py_None) return 0;
    PyObject* const items = PySequence_Fast(object, "The 'paths' option takes a sequence of paths");
    if (!items) return -1;
    Py_ssize_t const length = PySequence_Fast_GET_SIZE(items);
    *encoded = PyList_New(length);
    *views = PyMem_New(ustore_bytes_view_t, (size_t)length + 1);
    int failed = !*encoded || !*views;
    if (failed && !PyErr_Occurred()) PyErr_NoMemory();
    for (Py_ssize_t index = 0; !failed && index != length; ++index) {
        PyObject* bytes = NULL;
        failed = !PyUnicode_FSConverter(PySequence_Fast_GET_ITEM(items, index), &bytes);
        if (failed) break;
        PyList_SET_ITEM(*encoded, index, bytes);
        (*views)[index].data = (uint8_t const*)PyBytes_AS_STRING(bytes);
        (*views)[index].length = (ustore_size_t)PyBytes_GET_SIZE(bytes);
    }
    Py_DECREF(items);
    if (failed) {
        Py_CLEAR(*encoded);
        PyMem_Free(*views);
        *views = NULL;
        return -1;
    }
    *count = (size_t)length;
    return 0;
}

/** The size keywords of @c Executor.for_nvme, in the order they follow its other arguments. */
static char const* const nvme_size_names[] = {"queue_depth", "request_buffer_bytes", "request_buffers_count",
                                              "stream_buffer_bytes", "stream_buffers_count"};

static PyObject* Executor_for_nvme(PyObject* type, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                                   PyObject* keyword_names_tuple) {
    static char const* const names[] = {"threads",
                                        "transport",
                                        "queue_depth",
                                        "request_buffer_bytes",
                                        "request_buffers_count",
                                        "stream_buffer_bytes",
                                        "stream_buffers_count",
                                        "paths",
                                        "memory_domains",
                                        "scratch_limit"};
    PyObject* values[10];
    size_t threads = 0, scratch_limit = 0, sizes[5] = {0};
    int transport = ustore_transport_automatic_k;
    if (factory_arguments("for_nvme", arguments, positional_arguments_count, keyword_names_tuple, names, values, 10, 1,
                          0) != 0 ||
        (values[0] && ustore_py_size_from_object(values[0], "threads", &threads) != 0) ||
        (values[9] && ustore_py_size_from_object(values[9], "scratch_limit", &scratch_limit) != 0) ||
        (values[1] && ustore_py_enum_from_object(values[1], "transport", ustore_py_enum_transport_k, &transport) != 0))
        return NULL;
    for (size_t index = 0; index != 5; ++index)
        if (values[2 + index] &&
            ustore_py_size_from_object(values[2 + index], nvme_size_names[index], &sizes[index]) != 0)
            return NULL;
    PyObject* encoded_paths = NULL;
    ustore_bytes_view_t* paths = NULL;
    size_t paths_count = 0, memory_domains_count = 0;
    int32_t* memory_domains = NULL;
    if (paths_from_object(values[7], &encoded_paths, &paths, &paths_count) != 0) return NULL;
    if (identifiers_from_object(values[8], "memory_domains", &memory_domains, &memory_domains_count) != 0) {
        Py_XDECREF(encoded_paths);
        PyMem_Free(paths);
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
                .queue_depth = (ustore_size_t)sizes[0],
                .request_buffer_bytes = (ustore_size_t)sizes[1],
                .request_buffers_count = (ustore_size_t)sizes[2],
                .stream_buffer_bytes = (ustore_size_t)sizes[3],
                .stream_buffers_count = (ustore_size_t)sizes[4],
                .paths = paths,
                .paths_count = (ustore_size_t)paths_count,
            },
        .ram = {memory_domains, (ustore_size_t)memory_domains_count},
        .threads = (ustore_size_t)threads,
        .scratch_limit = (ustore_size_t)scratch_limit,
    };
    Py_BEGIN_ALLOW_THREADS;
    ustore_executor_for_nvme(&made);
    Py_END_ALLOW_THREADS;
    Py_XDECREF(encoded_paths);
    PyMem_Free(paths);
    PyMem_Free(memory_domains);
    return executor_made(type, handle, &outcome);
}

static char const doc_Executor_for_vram[] =                                                       //
    "for_vram(*, devices=None, scratch_limit=0, gds=Gds.AUTOMATIC) answers an executor of GPUs\n" //
    "of one vendor.\n"                                                                            //
    "\n"                                                                                          //
    "Every named device gets one stream the executor creates and destroys with it, all fed by\n"  //
    "the one host thread calling the verbs. Arenas it makes live on the first device, where\n"    //
    "every call computes but a filtered scan, which deals its pages across every device.\n"       //
    "Database.open_nvme() takes one as its `compactions`. Calls `ustore_executor_for_vram`.\n"    //
    "\n"                                                                                          //
    "Args:\n"                                                                                     //
    "  devices (Sequence[int], optional): Ordinals of the devices, their positions in\n"          //
    "    `gpu_devices()`; None takes every listed device of the first vendor with a known\n"      //
    "    class.\n"                                                                                //
    "  scratch_limit (int, optional): Bytes of device scratch a call or a merge takes on each\n"  //
    "    device; zero lets scratch grow as calls ask.\n"                                          //
    "  gds (Gds, optional): How bytes move between the drives and the devices.\n"                 //
    "Returns:\n"                                                                                  //
    "  Executor: The new executor.\n"                                                             //
    "Raises:\n"                                                                                   //
    "  DeviceError: No device holds an ordinal, or its driver refuses a stream.\n"                //
    "  ConfigMismatchError: The devices mix vendors.";

static PyObject* Executor_for_vram(PyObject* type, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                                   PyObject* keyword_names_tuple) {
    static char const* const names[] = {"devices", "scratch_limit", "gds"};
    PyObject* values[3];
    size_t scratch_limit = 0, devices_count = 0;
    int32_t* devices = NULL;
    int gds = ustore_gds_automatic_k;
    if (factory_arguments("for_vram", arguments, positional_arguments_count, keyword_names_tuple, names, values, 3, 0,
                          0) != 0 ||
        (values[1] && ustore_py_size_from_object(values[1], "scratch_limit", &scratch_limit) != 0) ||
        (values[2] && ustore_py_enum_from_object(values[2], "gds", ustore_py_enum_gds_k, &gds) != 0) ||
        identifiers_from_object(values[0], "devices", &devices, &devices_count) != 0)
        return NULL;

    ustore_executor_t handle = NULL;
    ustore_outcome_t outcome = {0};
    ustore_executor_for_vram_t made = {
        .executor = &handle,
        .outcome = &outcome,
        .vram = {devices, (ustore_size_t)devices_count},
        .scratch_limit = (ustore_size_t)scratch_limit,
        .gds = (ustore_gds_t)gds,
    };
    Py_BEGIN_ALLOW_THREADS;
    ustore_executor_for_vram(&made);
    Py_END_ALLOW_THREADS;
    PyMem_Free(devices);
    return executor_made(type, handle, &outcome);
}

/** Frees the threads of @p executor once its calls finish, leaving the object to refuse further calls. */
static void executor_close(Executor* executor) {
    ustore_executor_t const handle = executor->handle;
    executor->handle = NULL;
    if (handle) {
        Py_BEGIN_ALLOW_THREADS;
        ustore_executor_free(handle);
        Py_END_ALLOW_THREADS;
    }
}

static void Executor_dealloc(Executor* self) {
    executor_close(self);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static char const doc_Executor_drain[] =                                                    //
    "drain(mood=Mood.PARKED) answers None.\n"                                               //
    "\n"                                                                                    //
    "Waits until no call holds the executor, then parks its threads or keeps them\n"        //
    "spinning, as `mood` asks.\n"                                                           //
    "\n"                                                                                    //
    "Args:\n"                                                                               //
    "  mood (Mood, optional): What the threads do once the executor next goes idle:\n"      //
    "    `Mood.PARKED` pays a wake-up on the next call, `Mood.SPINNING` trades power for\n" //
    "    its latency.";

static PyObject* Executor_drain(PyObject* self, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                                PyObject* keyword_names_tuple) {
    PyObject* mood_object = NULL;
    if (ustore_py_optional_argument("drain", arguments, positional_arguments_count, keyword_names_tuple, 0, "mood",
                                    &mood_object) != 0)
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

static char const doc_Executor_close[] =                                                     //
    "close() answers None.\n"                                                                //
    "\n"                                                                                     //
    "Waits until no call holds the executor, then frees the threads. A receiver naming it\n" //
    "refuses calls afterwards, and closing twice is safe.";

static PyObject* Executor_close(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    executor_close((Executor*)self);
    Py_RETURN_NONE;
}

static PyObject* Executor_enter(PyObject* self, PyObject* Py_UNUSED(ignored)) { return Py_NewRef(self); }

static PyObject* Executor_exit(PyObject* self, PyObject* const* Py_UNUSED(arguments),
                               Py_ssize_t Py_UNUSED(positional_arguments_count),
                               PyObject* Py_UNUSED(keyword_names_tuple)) {
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

/** Answers <tt>Type(field=value, ...)</tt> over every getter, then every member, of @p self. */
static PyObject* report_repr(PyObject* self) {
    PyTypeObject* const type = Py_TYPE(self);
    PyObject* const parts = PyList_New(0);
    int failed = !parts;
    for (PyGetSetDef const* getter = type->tp_getset; !failed && getter && getter->name; ++getter) {
        PyObject* const value = PyObject_GetAttrString(self, getter->name);
        PyObject* const part = value ? PyUnicode_FromFormat("%s=%R", getter->name, value) : NULL;
        failed = !part || PyList_Append(parts, part) != 0;
        Py_XDECREF(part);
        Py_XDECREF(value);
    }
    for (PyMemberDef const* member = type->tp_members; !failed && member && member->name; ++member) {
        PyObject* const value = PyObject_GetAttrString(self, member->name);
        PyObject* const part = value ? PyUnicode_FromFormat("%s=%R", member->name, value) : NULL;
        failed = !part || PyList_Append(parts, part) != 0;
        Py_XDECREF(part);
        Py_XDECREF(value);
    }
    PyObject* const separator = failed ? NULL : PyUnicode_FromString(", ");
    PyObject* const joined = separator ? PyUnicode_Join(separator, parts) : NULL;
    char const* const dot = strrchr(type->tp_name, '.');
    PyObject* const repr = joined ? PyUnicode_FromFormat("%s(%U)", dot ? dot + 1 : type->tp_name, joined) : NULL;
    Py_XDECREF(joined);
    Py_XDECREF(separator);
    Py_XDECREF(parts);
    return repr;
}

PyObject* ustore_py_report_new(PyTypeObject* type) { return type->tp_alloc(type, 0); }

static char const doc_Capabilities[] = "What an engine supports, as `Database.capabilities` reports it.";

PyObject* ustore_py_capabilities_to_object(ustore_capabilities_t flags) {
    Capabilities* const capabilities = (Capabilities*)ustore_py_report_new(&CapabilitiesType);
    if (capabilities) capabilities->flags = flags;
    return (PyObject*)capabilities;
}

static PyObject* Capabilities_get_flag(Capabilities* self, void* closure) {
    return PyBool_FromLong((self->flags & (ustore_capabilities_t)(uintptr_t)closure) != 0);
}

static PyObject* Capabilities_richcompare(PyObject* self, PyObject* other, int operation) {
    if (!PyObject_TypeCheck(other, &CapabilitiesType) || (operation != Py_EQ && operation != Py_NE))
        Py_RETURN_NOTIMPLEMENTED;
    int const same = ((Capabilities*)self)->flags == ((Capabilities*)other)->flags;
    return PyBool_FromLong(operation == Py_EQ ? same : !same);
}

static PyGetSetDef Capabilities_getset[] = {
    {"transactions", (getter)Capabilities_get_flag, NULL, "Whether the engine groups operations into transactions.",
     (void*)(uintptr_t)ustore_capabilities_transactions_k},
    {"snapshots", (getter)Capabilities_get_flag, NULL, "Whether the engine pins point-in-time states.",
     (void*)(uintptr_t)ustore_capabilities_snapshots_k},
    {"saves_snapshots", (getter)Capabilities_get_flag, NULL,
     "Whether the engine writes a snapshot out as a database of its own.",
     (void*)(uintptr_t)ustore_capabilities_saves_snapshots_k},
    {NULL, NULL, NULL, NULL, NULL},
};

PyTypeObject CapabilitiesType = {
    PyVarObject_HEAD_INIT(NULL, 0) //
        .tp_name = "ustore.Capabilities",
    .tp_doc = doc_Capabilities,
    .tp_basicsize = sizeof(Capabilities),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_repr = report_repr,
    .tp_richcompare = Capabilities_richcompare,
    .tp_getset = Capabilities_getset,
};

static char const doc_Measurement[] =
    "The bounds on what a key range holds, as `measure()` estimates them without walking it.";

void ustore_py_measurement_settle(Measurement* measurement) {
    for (size_t index = 0; index != ustore_py_table_size_(measurement->slots); ++index)
        measurement->bounds[index] = measurement->slots[index] ? measurement->slots[index][0] : 0;
}

static PyMemberDef Measurement_members[] = {
    {"min_cardinality", Py_T_ULONGLONG, offsetof(Measurement, bounds[0]), Py_READONLY,
     "The fewest entries the range may hold."},
    {"max_cardinality", Py_T_ULONGLONG, offsetof(Measurement, bounds[1]), Py_READONLY,
     "The most entries the range may hold."},
    {"min_value_bytes", Py_T_ULONGLONG, offsetof(Measurement, bounds[2]), Py_READONLY,
     "The fewest bytes the values in the range may take."},
    {"max_value_bytes", Py_T_ULONGLONG, offsetof(Measurement, bounds[3]), Py_READONLY,
     "The most bytes the values in the range may take."},
    {"min_space_usage", Py_T_ULONGLONG, offsetof(Measurement, bounds[4]), Py_READONLY,
     "The fewest bytes the range may take on disk."},
    {"max_space_usage", Py_T_ULONGLONG, offsetof(Measurement, bounds[5]), Py_READONLY,
     "The most bytes the range may take on disk."},
    {NULL, 0, 0, 0, NULL},
};

PyTypeObject MeasurementType = {
    PyVarObject_HEAD_INIT(NULL, 0) //
        .tp_name = "ustore.Measurement",
    .tp_doc = doc_Measurement,
    .tp_basicsize = sizeof(Measurement),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_repr = report_repr,
    .tp_members = Measurement_members,
};

static char const doc_Changes[] = "The changes one pull of a ChangesCursor answers, oldest first.";

static void Changes_dealloc(Changes* self) {
    Py_XDECREF(self->keys_object);
    Py_XDECREF(self->values_object);
    Py_XDECREF(self->stamps_object);
    Py_XDECREF(self->kinds_object);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyMemberDef Changes_members[] = {
    {"keys", Py_T_OBJECT_EX, offsetof(Changes, keys_object), Py_READONLY,
     "The Page of the key of every change, oldest change first."},
    {"values", Py_T_OBJECT_EX, offsetof(Changes, values_object), Py_READONLY,
     "The Values of the bytes every change holds when the pull asked for them, or None."},
    {"stamps", Py_T_OBJECT_EX, offsetof(Changes, stamps_object), Py_READONLY,
     "The stamp every change was committed at."},
    {"kinds", Py_T_OBJECT_EX, offsetof(Changes, kinds_object), Py_READONLY, "What every change did, as a ChangeKind."},
    {NULL, 0, 0, 0, NULL},
};

PyTypeObject ChangesType = {
    PyVarObject_HEAD_INIT(NULL, 0) //
        .tp_name = "ustore.Changes",
    .tp_doc = doc_Changes,
    .tp_basicsize = sizeof(Changes),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_dealloc = (destructor)Changes_dealloc,
    .tp_repr = report_repr,
    .tp_members = Changes_members,
};

static char const doc_Description[] =                                                      //
    "What an open database reports about itself, as `describe()` answers it, every size\n" //
    "the engine's estimate.";

static PyObject* Description_get_capabilities(Description* self, void* Py_UNUSED(closure)) {
    return ustore_py_capabilities_to_object(self->description.capabilities);
}

static PyObject* Description_get_engine(Description* self, void* Py_UNUSED(closure)) {
    return ustore_py_enum_to_object(ustore_py_enum_engine_k, (int)self->description.engine);
}

static PyObject* Description_get_isolation(Description* self, void* Py_UNUSED(closure)) {
    return ustore_py_enum_to_object(ustore_py_enum_isolation_k, (int)self->description.isolation);
}

static PyObject* Description_get_cpu_class(Description* self, void* Py_UNUSED(closure)) {
    return ustore_py_enum_to_object(ustore_py_enum_cpu_class_k, (int)self->description.cpu_class);
}

static PyGetSetDef Description_getset[] = {
    {"capabilities", (getter)Description_get_capabilities, NULL, "What the engine supports, as a Capabilities.", NULL},
    {"engine", (getter)Description_get_engine, NULL, "The engine serving the database, an Engine.", NULL},
    {"isolation", (getter)Description_get_isolation, NULL,
     "How transactions see each other: what the RAM engine was opened with, and SNAPSHOT elsewhere.", NULL},
    {"cpu_class", (getter)Description_get_cpu_class, NULL,
     "The CPUClass the engine's calls run as, picked at open; UNKNOWN for RocksDB and LevelDB.", NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyMemberDef Description_members[] = {
    {"keys", Py_T_ULONGLONG, offsetof(Description, description.keys), Py_READONLY,
     "Keys across every collection, as the engine estimates them."},
    {"memory_bytes", Py_T_ULONGLONG, offsetof(Description, description.memory_bytes), Py_READONLY,
     "Bytes of memory the engine holds for the data, or zero where it keeps no estimate."},
    {"disk_bytes", Py_T_ULONGLONG, offsetof(Description, description.disk_bytes), Py_READONLY,
     "Bytes the files of the database take on disk."},
    {"unpinned_staging_bytes", Py_T_ULONGLONG, offsetof(Description, description.unpinned_staging_bytes), Py_READONLY,
     "Bytes of the NVMe engine's io_uring arena the locked-memory cap refused to pin, so each request pins its own."},
    {"requests_across_registered_slots", Py_T_ULONGLONG,
     offsetof(Description, description.requests_across_registered_slots), Py_READONLY,
     "Requests the NVMe engine sent unpinned while its arena was pinned, as each crossed a gibibyte of it."},
    {"commit_conflicts_limit", Py_T_ULONGLONG, offsetof(Description, description.commit_conflicts_limit), Py_READONLY,
     "Conflicts a commit of the database's own absorbs before it fails, as its open named or derived them; zero off " //
     "the RAM and NVMe engines."},
    {NULL, 0, 0, 0, NULL},
};

PyTypeObject DescriptionType = {
    PyVarObject_HEAD_INIT(NULL, 0) //
        .tp_name = "ustore.Description",
    .tp_doc = doc_Description,
    .tp_basicsize = sizeof(Description),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_repr = report_repr,
    .tp_members = Description_members,
    .tp_getset = Description_getset,
};

/** One GPU this process can drive, as @c ustore_gpu_devices wrote it, and its place there. */
typedef struct GPUDevice {

    /** The object header every instance holds. */
    PyObject ob_base;

    /** The ordinal <tt>Executor.for_vram()</tt> names the device by, its position in the listing. */
    unsigned long long ordinal;

    /** The device as the library reports it. */
    ustore_gpu_device_t device;
} GPUDevice;

static char const doc_GPUDevice[] = "One GPU this process can drive, as `gpu_devices()` lists it.";

static PyObject* GPUDevice_get_vendor(GPUDevice* self, void* Py_UNUSED(closure)) {
    return ustore_py_enum_to_object(ustore_py_enum_gpu_vendor_k, (int)self->device.vendor);
}

static PyObject* GPUDevice_get_gpu_class(GPUDevice* self, void* Py_UNUSED(closure)) {
    return ustore_py_enum_to_object(ustore_py_enum_gpu_class_k, (int)self->device.gpu_class);
}

static PyObject* GPUDevice_get_name(GPUDevice* self, void* Py_UNUSED(closure)) {
    return PyUnicode_DecodeUTF8(self->device.name, (Py_ssize_t)strnlen(self->device.name, sizeof(self->device.name)),
                                "replace");
}

static PyMemberDef GPUDevice_members[] = {
    {"ordinal", Py_T_ULONGLONG, offsetof(GPUDevice, ordinal), Py_READONLY,
     "The ordinal `Executor.for_vram()` names the device by, its position in `gpu_devices()`."},
    {"vendor_ordinal", Py_T_UINT, offsetof(GPUDevice, device.vendor_ordinal), Py_READONLY,
     "The device's ordinal in its vendor's runtime, as `nvidia-smi` or `rocm-smi` list it."},
    {"compute_units", Py_T_UINT, offsetof(GPUDevice, device.compute_units), Py_READONLY,
     "Streaming multiprocessors or compute units."},
    {"memory_bytes", Py_T_ULONGLONG, offsetof(GPUDevice, device.memory_bytes), Py_READONLY, "Device memory in bytes."},
    {NULL, 0, 0, 0, NULL},
};

static PyGetSetDef GPUDevice_getset[] = {
    {"vendor", (getter)GPUDevice_get_vendor, NULL, "The vendor whose runtime drives the device, a GPUVendor.", NULL},
    {"gpu_class", (getter)GPUDevice_get_gpu_class, NULL, "The class of the device, a GPUClass.", NULL},
    {"name", (getter)GPUDevice_get_name, NULL, "The vendor's own name for the device.", NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

PyTypeObject GPUDeviceType = {
    PyVarObject_HEAD_INIT(NULL, 0) //
        .tp_name = "ustore.GPUDevice",
    .tp_doc = doc_GPUDevice,
    .tp_basicsize = sizeof(GPUDevice),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_repr = report_repr,
    .tp_members = GPUDevice_members,
    .tp_getset = GPUDevice_getset,
};

char const ustore_py_gpu_devices_doc[] =                                                      //
    "gpu_devices() answers a tuple of GPUDevice.\n"                                           //
    "\n"                                                                                      //
    "Every GPU this process can drive, NVIDIA devices first, then AMD, then Apple, each at\n" //
    "the ordinal `Executor.for_vram()` names it by. A vendor whose runtime is absent lists\n" //
    "none, so a machine without GPUs answers an empty tuple. Calls `ustore_gpu_devices`.";

PyObject* ustore_py_gpu_devices(PyObject* Py_UNUSED(module), PyObject* Py_UNUSED(arguments)) {
    ustore_gpu_device_t devices[128];
    ustore_size_t count = 0;
    ustore_outcome_t outcome = {0};
    ustore_gpu_devices_t listing = {
        .outcome = &outcome,
        .devices = devices,
        .devices_capacity = sizeof(devices) / sizeof(devices[0]),
        .devices_count = &count,
    };
    Py_BEGIN_ALLOW_THREADS;
    ustore_gpu_devices(&listing);
    Py_END_ALLOW_THREADS;
    if (ustore_py_raise_on_outcome(&outcome) != 0) return NULL;
    if (count > listing.devices_capacity) count = listing.devices_capacity;
    PyObject* const listed = PyTuple_New((Py_ssize_t)count);
    for (ustore_size_t index = 0; listed && index != count; ++index) {
        GPUDevice* const entry = (GPUDevice*)ustore_py_report_new(&GPUDeviceType);
        if (!entry) {
            Py_DECREF(listed);
            return NULL;
        }
        entry->ordinal = index;
        entry->device = devices[index];
        PyTuple_SET_ITEM(listed, (Py_ssize_t)index, (PyObject*)entry);
    }
    return listed;
}

#pragma endregion Module Types
