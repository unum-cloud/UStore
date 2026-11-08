/**
 *  @file python/blobs.c
 *  @author Ash Vardanian
 *  @date September 16, 2026
 *  @brief Binary values: writes, reads, scans, samples, measurements and cursors.
 *
 *  Every verb here is written once and reached from every receiver's method table, since receivers
 *  differ only in what their calls address and how they run, which the shared context carries.
 *  Each one detaches the thread state around the call. A verb whose outputs scale with the call
 *  takes an @c Arena as its last positional argument and answers results viewing it, while `get`
 *  and `contains`, which read one key, make a one-off arena and copy out of it. Every verb fills a
 *  call the driver keeps on the stack or inside a @c Deferred, so one body serves both completions.
 */
#include "lib.h"

#pragma region Reads

char const doc_read[] =                                                                     //
    "read(keys, arena, *, collection, offset=0, limit=None) answers a Values tape.\n"       //
    "\n"                                                                                    //
    "Reads a batch of binary values in one call, which no sequence of single reads\n"       //
    "matches for throughput. Nothing is copied: the tape sits in the arena, and indexing\n" //
    "the answer gives a memoryview over the stored bytes, or None for an absent key.\n"     //
    "\n"                                                                                    //
    "Args:\n"                                                                               //
    "  keys (Sequence[KeyLike] | buffer): The keys to read, as a sequence, a contiguous\n"  //
    "    buffer of 16-byte keys, which is read in place, or one of 64-bit integers.\n"      //
    "  arena (Arena): Where the tape lands, rewound by this call.\n"                        //
    "  collection (UUID | str): The collection, by identifier or by name.\n"                //
    "  offset (int, optional): Byte of every value the read starts at, where an offset\n"   //
    "    past the end reads empty.\n"                                                       //
    "  limit (int, optional): Most bytes of every value to read from the offset, or None\n" //
    "    for the rest of it.\n"                                                             //
    "Returns:\n"                                                                            //
    "  Values: One entry per key, in the order they were asked for.\n"                      //
    "Raises:\n"                                                                             //
    "  ContendedError: The arena serves another call or exports a buffer.\n"                //
    "  Error: Whatever the engine reported.";

static void read_submit(void* arguments) { ustore_blobs_read((ustore_blobs_read_t*)arguments); }

PyObject* ustore_py_read_answer_values(void* erased) {
    ustore_py_read_call_t const* const call = (ustore_py_read_call_t const*)erased;
    return ustore_py_values_new(call->call.arena, call->presences, &call->values, call->keys.count);
}

/** Copies the value of task @p task_index of @p call into bytes, or answers None where its key is absent. */
static PyObject* read_copy(ustore_py_read_call_t const* call, size_t task_index) {
    if (!ustore_py_present(call->presences, task_index)) Py_RETURN_NONE;
    ustore_bytes_view_t const value = ustore_py_tape_string(&call->values, task_index);
    return PyBytes_FromStringAndSize((char const*)value.data, (Py_ssize_t)value.length);
}

PyObject* ustore_py_read_answer_copies(void* erased) {
    ustore_py_read_call_t const* const call = (ustore_py_read_call_t const*)erased;
    if (call->subscript && !ustore_py_present(call->presences, 0)) {
        PyErr_SetObject(PyExc_KeyError, call->subscript);
        return NULL;
    }
    if (call->subscript) return read_copy(call, 0);
    PyObject* copies = PyList_New((Py_ssize_t)call->keys.count);
    for (size_t task_index = 0; copies && task_index != call->keys.count; ++task_index) {
        PyObject* const copy = read_copy(call, task_index);
        if (!copy) Py_CLEAR(copies);
        else PyList_SET_ITEM(copies, (Py_ssize_t)task_index, copy);
    }
    return copies;
}

/** Answers a dict of every key the read was handed onto a copy of its value, or None where it is absent. */
PyObject* ustore_py_read_answer_drawn(void* erased) {
    ustore_py_read_call_t const* const call = (ustore_py_read_call_t const*)erased;
    PyObject* drawn = PyDict_New();
    for (size_t task_index = 0; drawn && task_index != call->keys.count; ++task_index) {
        PyObject* const key = ustore_py_uuid_to_object(&call->keys.keys[task_index].uuid);
        PyObject* const copy = key ? read_copy(call, task_index) : NULL;
        if (!copy || PyDict_SetItem(drawn, key, copy) != 0) Py_CLEAR(drawn);
        Py_XDECREF(key);
        Py_XDECREF(copy);
    }
    return drawn;
}

void ustore_py_read_release(void* erased) {
    ustore_py_read_call_t* const call = (ustore_py_read_call_t*)erased;
    ustore_py_keys_release(&call->keys);
    Py_CLEAR(call->field);
    Py_CLEAR(call->subscript);
    ustore_py_call_release(&call->call);
}

ustore_py_verb_t const ustore_py_read_verb = {sizeof(ustore_py_read_call_t), read_submit, ustore_py_read_answer_values,
                                              ustore_py_read_release};

ustore_py_verb_t const ustore_py_read_copies_verb = {sizeof(ustore_py_read_call_t), read_submit,
                                                     ustore_py_read_answer_copies, ustore_py_read_release};

ustore_py_verb_t const ustore_py_read_drawn_verb = {sizeof(ustore_py_read_call_t), read_submit,
                                                    ustore_py_read_answer_drawn, ustore_py_read_release};

PyObject* ustore_py_values_read(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                                ustore_py_verb_t const* verb, ustore_py_read_call_t* call, PyObject* arena) {
    if (ustore_py_arena_claim(arena, &call->call.arena) != 0) return ustore_py_call_discard(context, verb, call);
    call->call.collection = options->collection;
    ustore_blobs_read_t read = {
        .database = context->database_handle,
        .executor = ustore_py_context_executor(context),
        .outcome = &call->call.outcome,
        .completion = context->completion,
        .view = context->view,
        .count = call->keys.count,
        .collections = {&call->call.collection, 0},
        .keys = {call->keys.keys, sizeof(ustore_key_t)},
        .offsets = {call->offset ? &call->offset : NULL, 0},
        .limits = {call->extent == ustore_py_read_limited_k ? &call->limit : NULL, 0},
        .presences = &call->presences,
        .values = &call->values,
        .arena = ustore_py_arena_handle(call->call.arena),
    };
    return ustore_py_call_run(context, verb, call, &read);
}

PyObject* ustore_py_api_read(PyObject* self, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                             PyObject* keyword_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_keyword_names_count_checked("read", positional_arguments_count, keyword_names_tuple, 2) < 0)
        return NULL;

    static char const* const names[] = {"offset", "limit", NULL};
    PyObject* given[2];
    ustore_py_verb_options_t options;
    if (ustore_py_verb_keywords_from_arguments(&context, arguments, positional_arguments_count, keyword_names_tuple,
                                               names, &options, given) != 0)
        return NULL;
    size_t offset = 0;
    size_t limit = 0;
    if ((given[0] && ustore_py_size_from_object(given[0], "offset", &offset) != 0) ||
        (given[1] && ustore_py_size_from_object(given[1], "limit", &limit) != 0))
        return NULL;

    ustore_py_read_call_t stack;
    ustore_py_read_call_t* const call = ustore_py_call_open(&context, &ustore_py_read_verb, &stack);
    if (!call) return NULL;
    if (ustore_py_keys_from_object(arguments[0], &call->keys) != 0)
        return ustore_py_call_discard(&context, &ustore_py_read_verb, call);
    call->offset = offset;
    call->limit = limit;
    call->extent = given[1] ? ustore_py_read_limited_k : ustore_py_read_to_end_k;
    return ustore_py_values_read(&context, &options, &ustore_py_read_verb, call, arguments[1]);
}

/**
 *  @brief Reads the one key gathered into @p call through @p verb, into a one-off arena made where @p context runs.
 *  @return What @p verb answers, or its @c Deferred, which owns the arena; NULL with an exception set otherwise.
 */
static PyObject* values_read_once(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                                  ustore_py_verb_t const* verb, ustore_py_read_call_t* call) {
    PyObject* const arena = ustore_py_arena_new(ustore_py_context_executor(context));
    if (!arena) return ustore_py_call_discard(context, verb, call);
    PyObject* const answer = ustore_py_values_read(context, options, verb, call, arena);
    Py_DECREF(arena);
    return answer;
}

char const doc_get[] =                                                                         //
    "get(key, *, collection) answers bytes or None.\n"                                         //
    "\n"                                                                                       //
    "Reads one value into a one-off arena and copies it into a bytes object, so nothing of\n"  //
    "the arena outlives the call. That costs an arena per call, so reach for `read` with an\n" //
    "arena of your own when several keys are wanted, or in a loop.\n"                          //
    "\n"                                                                                       //
    "Args:\n"                                                                                  //
    "  key (KeyLike): The key to read.\n"                                                      //
    "  collection (UUID | str): The collection, by identifier or by name.\n"                   //
    "Returns:\n"                                                                               //
    "  bytes | None: The stored bytes, or None when the key is absent.";

static PyObject* get_answer(void* erased) { return read_copy((ustore_py_read_call_t const*)erased, 0); }

static ustore_py_verb_t const get_verb = {sizeof(ustore_py_read_call_t), read_submit, get_answer,
                                          ustore_py_read_release};

/** Opens a call of @p verb through @p context holding the one key @p key inline. */
static ustore_py_read_call_t* one_key_open(ustore_py_context_t const* context, ustore_py_verb_t const* verb,
                                           ustore_key_t const* key, ustore_py_read_call_t* stack) {
    ustore_py_read_call_t* const call = ustore_py_call_open(context, verb, stack);
    if (!call) return NULL;
    call->keys.inline_keys[0] = *key;
    call->keys.keys = call->keys.inline_keys;
    call->keys.count = 1;
    return call;
}

PyObject* ustore_py_api_get(PyObject* self, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                            PyObject* keyword_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_keyword_names_count_checked("get", positional_arguments_count, keyword_names_tuple, 1) < 0)
        return NULL;

    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, arguments, positional_arguments_count, keyword_names_tuple,
                                             &options) != 0)
        return NULL;

    ustore_key_t key = {0};
    if (ustore_py_key_from_object(arguments[0], &key) != 0) return NULL;
    ustore_py_read_call_t stack;
    ustore_py_read_call_t* const call = one_key_open(&context, &get_verb, &key, &stack);
    return call ? values_read_once(&context, &options, &get_verb, call) : NULL;
}

char const doc_contains[] =                                                              //
    "contains(key, *, collection) answers whether the key is present.\n"                 //
    "\n"                                                                                 //
    "Reads the presence alone into a one-off arena, so no value crosses the boundary.\n" //
    "\n"                                                                                 //
    "Args:\n"                                                                            //
    "  key (KeyLike): The key to look for.\n"                                            //
    "  collection (UUID | str): The collection, by identifier or by name.\n"             //
    "Returns:\n"                                                                         //
    "  bool: Whether the key carries a value.";

static PyObject* contains_answer(void* erased) {
    return PyBool_FromLong(ustore_py_present(((ustore_py_read_call_t const*)erased)->presences, 0));
}

static ustore_py_verb_t const contains_verb = {sizeof(ustore_py_read_call_t), read_submit, contains_answer,
                                               ustore_py_read_release};

PyObject* ustore_py_value_contains(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                                   ustore_key_t const* key) {
    ustore_py_read_call_t stack;
    ustore_py_read_call_t* const call = one_key_open(context, &contains_verb, key, &stack);
    if (!call) return NULL;
    PyObject* const arena = ustore_py_arena_new(ustore_py_context_executor(context));
    int const claimed = arena ? ustore_py_arena_claim(arena, &call->call.arena) : -1;
    Py_XDECREF(arena);
    if (claimed != 0) return ustore_py_call_discard(context, &contains_verb, call);
    call->call.collection = options->collection;
    // The presence alone is asked for, so no value crosses into the arena.
    ustore_blobs_read_t read = {
        .database = context->database_handle,
        .executor = ustore_py_context_executor(context),
        .outcome = &call->call.outcome,
        .completion = context->completion,
        .view = context->view,
        .count = 1,
        .collections = {&call->call.collection, 0},
        .keys = {call->keys.keys, sizeof(ustore_key_t)},
        .presences = &call->presences,
        .arena = ustore_py_arena_handle(call->call.arena),
    };
    return ustore_py_call_run(context, &contains_verb, call, &read);
}

PyObject* ustore_py_api_contains(PyObject* self, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                                 PyObject* keyword_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_keyword_names_count_checked("contains", positional_arguments_count, keyword_names_tuple, 1) < 0)
        return NULL;

    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, arguments, positional_arguments_count, keyword_names_tuple,
                                             &options) != 0)
        return NULL;

    ustore_key_t key = {0};
    if (ustore_py_key_from_object(arguments[0], &key) != 0) return NULL;
    return ustore_py_value_contains(&context, &options, &key);
}

#pragma endregion Reads

#pragma region Writes

int ustore_py_contents_from_object(PyObject* object, size_t count, ustore_py_contents_t* batch) {
    int const tape = ustore_py_contents_from_tape(object, batch);
    if (tape < 0) return -1;
    if (tape && batch->count != count) {
        ustore_py_raise(ustore_status_invalid_argument_k, "Got a tape of %zu values for %zu keys", batch->count, count);
        ustore_py_contents_release(batch);
        return -1;
    }
    if (tape) return 0;

    // A bytes-like object is one value for every key rather than a sequence of them, which is what
    // makes `write(keys, b"same")` mean what it reads like.
    if (PyObject_CheckBuffer(object)) {
        if (ustore_py_contents_reserve(batch, 1, ustore_py_contents_broadcast_k) != 0) return -1;
        if (ustore_py_contents_borrow(object, batch) != 0) {
            ustore_py_contents_release(batch);
            return -1;
        }
        return 0;
    }

    PyObject* const sequence = PySequence_Fast(object, "Expected a sequence of values, a tape or one buffer");
    if (!sequence) return -1;
    if ((size_t)PySequence_Fast_GET_SIZE(sequence) != count) {
        ustore_py_raise(ustore_status_invalid_argument_k, "Got %zd values for %zu keys",
                        PySequence_Fast_GET_SIZE(sequence), count);
        Py_DECREF(sequence);
        return -1;
    }
    if (ustore_py_contents_reserve(batch, count, ustore_py_contents_per_task_k) != 0) {
        Py_DECREF(sequence);
        return -1;
    }
    for (size_t task_index = 0; task_index != count; ++task_index)
        if (ustore_py_contents_borrow(PySequence_Fast_GET_ITEM(sequence, (Py_ssize_t)task_index), batch) != 0) {
            Py_DECREF(sequence);
            ustore_py_contents_release(batch);
            return -1;
        }
    Py_DECREF(sequence);
    return 0;
}

static void write_submit(void* arguments) { ustore_blobs_write((ustore_blobs_write_t*)arguments); }

static void write_release(void* erased) {
    ustore_py_write_call_t* const call = (ustore_py_write_call_t*)erased;
    ustore_py_contents_release(&call->contents);
    ustore_py_keys_release(&call->keys);
    ustore_py_call_release(&call->call);
}

ustore_py_verb_t const ustore_py_write_verb = {sizeof(ustore_py_write_call_t), write_submit, ustore_py_call_answer_none,
                                               write_release};

PyObject* ustore_py_values_write(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                                 ustore_py_write_call_t* call) {
    call->call.collection = options->collection;
    ustore_blobs_write_t write = {
        .database = context->database_handle,
        .executor = ustore_py_context_executor(context),
        .outcome = &call->call.outcome,
        .completion = context->completion,
        .transaction = context->transaction,
        .count = call->keys.count,
        .collections = {&call->call.collection, 0},
        .keys = {call->keys.keys, sizeof(ustore_key_t)},
        .values = call->contents.view,
        .durability = options->durability,
        .watch = context->watch,
    };
    return ustore_py_call_run(context, &ustore_py_write_verb, call, &write);
}

static void remove_submit(void* arguments) { ustore_blobs_remove((ustore_blobs_remove_t*)arguments); }

static void remove_release(void* erased) {
    ustore_py_remove_call_t* const call = (ustore_py_remove_call_t*)erased;
    ustore_py_keys_release(&call->keys);
    ustore_py_keys_release(&call->ends);
    ustore_py_call_release(&call->call);
}

int ustore_py_ends_from_object(PyObject* object, ustore_py_keys_t const* starts, ustore_py_keys_t* ends) {
    if (!object) return 0;
    if (ustore_py_keys_from_subscript(object, ends) < 0) return -1;
    if (ends->count == starts->count) return 0;
    return ustore_py_raise(ustore_status_invalid_argument_k, "Got %zu ends for %zu starts", ends->count, starts->count);
}

ustore_keys_view_t ustore_py_ends_view(ustore_py_keys_t const* ends) {
    ustore_keys_view_t const view = {ends->count ? ends->keys : NULL, ends->count ? sizeof(ustore_key_t) : 0};
    return view;
}

ustore_py_verb_t const ustore_py_remove_verb = {sizeof(ustore_py_remove_call_t), remove_submit,
                                                ustore_py_call_answer_none, remove_release};

static void docs_remove_submit(void* arguments) { ustore_docs_remove((ustore_docs_remove_t*)arguments); }

// It answers and releases as a removal of values does, so a call opened for either runs through either
static ustore_py_verb_t const docs_remove_verb = {sizeof(ustore_py_remove_call_t), docs_remove_submit,
                                                  ustore_py_call_answer_none, remove_release};

PyObject* ustore_py_values_remove(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                                  ustore_modality_t modality, ustore_py_remove_call_t* call) {
    call->call.collection = options->collection;
    if (modality == ustore_modality_docs_k) {
        ustore_docs_remove_t remove = {
            .database = context->database_handle,
            .executor = ustore_py_context_executor(context),
            .outcome = &call->call.outcome,
            .completion = context->completion,
            .transaction = context->transaction,
            .count = call->keys.count,
            .collections = {&call->call.collection, 0},
            .keys = {call->keys.keys, sizeof(ustore_key_t)},
            .end_keys = ustore_py_ends_view(&call->ends),
            .durability = options->durability,
            .watch = context->watch,
        };
        return ustore_py_call_run(context, &docs_remove_verb, call, &remove);
    }
    ustore_blobs_remove_t remove = {
        .database = context->database_handle,
        .executor = ustore_py_context_executor(context),
        .outcome = &call->call.outcome,
        .completion = context->completion,
        .transaction = context->transaction,
        .count = call->keys.count,
        .collections = {&call->call.collection, 0},
        .keys = {call->keys.keys, sizeof(ustore_key_t)},
        .end_keys = ustore_py_ends_view(&call->ends),
        .durability = options->durability,
        .watch = context->watch,
    };
    return ustore_py_call_run(context, &ustore_py_remove_verb, call, &remove);
}

char const doc_write[] =                                                                     //
    "write(keys, values, *, collection, durability=Durability.BUFFERED) answers None.\n"     //
    "\n"                                                                                     //
    "Upserts a batch of binary values in one call. Every value is borrowed for the\n"        //
    "length of the call rather than copied, so a memory-mapped buffer reaches the engine\n"  //
    "untouched.\n"                                                                           //
    "\n"                                                                                     //
    "Args:\n"                                                                                //
    "  keys (Sequence[KeyLike] | buffer): The keys to write.\n"                              //
    "  values (Sequence[bytes-like] | bytes-like | tape): One value per key, a tape of\n"    //
    "    them, or a single bytes-like object broadcast to every key. A tape is an Arrow\n"   //
    "    `binary`, `large_binary`, `string` or `large_string` array, such as `Values`, or\n" //
    "    a `(data, offsets)` tuple of buffers with unsigned 32-bit or 64-bit offsets, and\n" //
    "    passes without a copy. None is refused, since `remove` removes keys.\n"             //
    "  collection (UUID | str): The collection, by identifier or by name.\n"                 //
    "  durability (Durability, optional): `Durability.FLUSHED` reaches the drive before\n"   //
    "    returning.\n"                                                                       //
    "Raises:\n"                                                                              //
    "  TypeError: A value is None or exports no buffer.\n"                                   //
    "  Error: Whatever the engine reported.";

PyObject* ustore_py_api_write(PyObject* self, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                              PyObject* keyword_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0 || ustore_py_context_writable(&context) != 0) return NULL;
    if (ustore_py_keyword_names_count_checked("write", positional_arguments_count, keyword_names_tuple, 2) < 0)
        return NULL;

    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, arguments, positional_arguments_count, keyword_names_tuple,
                                             &options) != 0)
        return NULL;

    ustore_py_write_call_t stack;
    ustore_py_write_call_t* const call = ustore_py_call_open(&context, &ustore_py_write_verb, &stack);
    if (!call) return NULL;
    if (ustore_py_keys_from_object(arguments[0], &call->keys) != 0 ||
        ustore_py_contents_from_object(arguments[1], call->keys.count, &call->contents) != 0)
        return ustore_py_call_discard(&context, &ustore_py_write_verb, call);
    return ustore_py_values_write(&context, &options, call);
}

char const doc_set[] =                                                                     //
    "set(key, value, *, collection, durability=Durability.BUFFERED) answers None.\n"       //
    "\n"                                                                                   //
    "Upserts one binary value.\n"                                                          //
    "\n"                                                                                   //
    "Args:\n"                                                                              //
    "  key (KeyLike): The key to write.\n"                                                 //
    "  value (bytes-like): The bytes to store, borrowed rather than copied.\n"             //
    "  collection (UUID | str): The collection, by identifier or by name.\n"               //
    "  durability (Durability, optional): `Durability.FLUSHED` reaches the drive before\n" //
    "    returning.\n"                                                                     //
    "Raises:\n"                                                                            //
    "  TypeError: The value is None or exports no buffer.";

PyObject* ustore_py_api_set(PyObject* self, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                            PyObject* keyword_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0 || ustore_py_context_writable(&context) != 0) return NULL;
    if (ustore_py_keyword_names_count_checked("set", positional_arguments_count, keyword_names_tuple, 2) < 0)
        return NULL;

    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, arguments, positional_arguments_count, keyword_names_tuple,
                                             &options) != 0)
        return NULL;

    ustore_py_write_call_t stack;
    ustore_py_write_call_t* const call = ustore_py_call_open(&context, &ustore_py_write_verb, &stack);
    if (!call) return NULL;
    call->keys.keys = call->keys.inline_keys;
    call->keys.count = 1;
    if (ustore_py_key_from_object(arguments[0], &call->keys.inline_keys[0]) != 0 ||
        ustore_py_contents_reserve(&call->contents, 1, ustore_py_contents_per_task_k) != 0 ||
        ustore_py_contents_borrow(arguments[1], &call->contents) != 0)
        return ustore_py_call_discard(&context, &ustore_py_write_verb, call);
    return ustore_py_values_write(&context, &options, call);
}

char const doc_remove[] =                                                                     //
    "remove(keys, *, collection, ends=None, durability=Durability.BUFFERED) answers None.\n"  //
    "\n"                                                                                      //
    "Removes a batch of keys with their values, or one key given on its own, while\n"         //
    "`docs_remove` removes documents. With `ends`, every key starts a range reaching up to\n" //
    "its end, and every key of the range goes, all in one atomic call.\n"                     //
    "\n"                                                                                      //
    "Args:\n"                                                                                 //
    "  keys (KeyLike | Sequence[KeyLike] | buffer): The keys to remove, or the first key\n"   //
    "    of every range.\n"                                                                   //
    "  collection (UUID | str): The collection, by identifier or by name.\n"                  //
    "  ends (KeyLike | Sequence[KeyLike] | buffer, optional): The key every range stops\n"    //
    "    before, one per key.\n"                                                              //
    "  durability (Durability, optional): `Durability.FLUSHED` reaches the drive before\n"    //
    "    returning.";

PyObject* ustore_py_api_remove(PyObject* self, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                               PyObject* keyword_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0 || ustore_py_context_writable(&context) != 0) return NULL;
    if (ustore_py_keyword_names_count_checked("remove", positional_arguments_count, keyword_names_tuple, 1) < 0)
        return NULL;

    static char const* const names[] = {"ends", NULL};
    PyObject* ends = NULL;
    ustore_py_verb_options_t options;
    if (ustore_py_verb_keywords_from_arguments(&context, arguments, positional_arguments_count, keyword_names_tuple,
                                               names, &options, &ends) != 0)
        return NULL;

    ustore_py_remove_call_t stack;
    ustore_py_remove_call_t* const call = ustore_py_call_open(&context, &ustore_py_remove_verb, &stack);
    if (!call) return NULL;
    if (ustore_py_keys_from_subscript(arguments[0], &call->keys) < 0 ||
        ustore_py_ends_from_object(ends, &call->keys, &call->ends) != 0)
        return ustore_py_call_discard(&context, &ustore_py_remove_verb, call);
    return ustore_py_values_remove(&context, &options, ustore_modality_blobs_k, call);
}

#pragma endregion Writes

#pragma region Scans

/** What a scan or a cursor matches, as its keywords named it, every object borrowed from the arguments. */
typedef struct scan_match_t {

    /** The sequence of `str` or bytes-like patterns, or NULL. */
    PyObject* patterns;

    /** The `PatternKind` the patterns are matched as. */
    int kind;

    /** The `CaseSensitivity` of the patterns. */
    int sensitivity;

    /** The sequence of one weight per substring pattern, or NULL. */
    PyObject* weights;

    /** The JSONPath or XPath filter as `str` or bytes, or NULL. */
    PyObject* filter;

    /** The predicate callable, or NULL. */
    PyObject* predicate;

    /** The `PredicateCost` placing the predicate. */
    int cost;
} scan_match_t;

/** Reads one match keyword into @p match, answering 1 when @p name is one, 0 when it is not, -1 failing. */
static int scan_match_from_keyword(PyObject* name, PyObject* value, scan_match_t* match) {
    if (PyUnicode_CompareWithASCIIString(name, "patterns") == 0) {
        match->patterns = value == Py_None ? NULL : value;
        return 1;
    }
    if (PyUnicode_CompareWithASCIIString(name, "weights") == 0) {
        match->weights = value == Py_None ? NULL : value;
        return 1;
    }
    if (PyUnicode_CompareWithASCIIString(name, "filter") == 0) {
        match->filter = value == Py_None ? NULL : value;
        return 1;
    }
    if (PyUnicode_CompareWithASCIIString(name, "pattern_kind") == 0)
        return ustore_py_enum_from_object(value, "pattern_kind", ustore_py_enum_pattern_kind_k, &match->kind) == 0 ? 1
                                                                                                                   : -1;
    if (PyUnicode_CompareWithASCIIString(name, "sensitivity") == 0)
        return ustore_py_enum_from_object(value, "sensitivity", ustore_py_enum_case_sensitivity_k,
                                          &match->sensitivity) == 0
                   ? 1
                   : -1;
    if (PyUnicode_CompareWithASCIIString(name, "predicate") == 0)
        return ustore_py_predicate_from_object(value, &match->predicate) == 0 ? 1 : -1;
    if (PyUnicode_CompareWithASCIIString(name, "predicate_cost") == 0)
        return ustore_py_enum_from_object(value, "predicate_cost", ustore_py_enum_predicate_cost_k, &match->cost) == 0
                   ? 1
                   : -1;
    return 0;
}

/** Refuses the match keywords @p modality takes none of, as only blobs scans take patterns, and documents and pages a
 *  filter. */
static int scan_match_admitted(scan_match_t const* match, ustore_modality_t modality) {
    if (match->patterns && modality != ustore_modality_blobs_k) {
        PyErr_SetString(PyExc_TypeError, "Only scans of blobs take patterns");
        return -1;
    }
    if (match->weights && !match->patterns) {
        PyErr_SetString(PyExc_TypeError, "Weights score patterns, which the call names none of");
        return -1;
    }
    if (match->filter && modality != ustore_modality_docs_k && modality != ustore_modality_pages_k) {
        PyErr_SetString(PyExc_TypeError, "Only scans of documents and pages take a filter");
        return -1;
    }
    return 0;
}

/** Walks the keywords of a scan, which takes the order its keys come in, its direction, the end
 *  keyword @p end_name spells, a progress callable and what it matches beside the shared ones. */
static int scan_options_from_keywords(ustore_py_context_t const* context, PyObject* const* arguments,
                                      Py_ssize_t positional_arguments_count, PyObject* keyword_names_tuple,
                                      char const* end_name, ustore_py_verb_options_t* options, ustore_scan_mode_t* mode,
                                      ustore_scan_direction_t* direction, PyObject** end, PyObject** progress,
                                      scan_match_t* match) {
    ustore_py_verb_options_init(options);
    *progress = NULL;
    *end = NULL;
    memset(match, 0, sizeof(*match));
    int chosen = ustore_scan_ordered_k;
    int walked = ustore_scan_ascending_k;
    Py_ssize_t const keyword_names_count = keyword_names_tuple ? PyTuple_GET_SIZE(keyword_names_tuple) : 0;
    for (Py_ssize_t names_progress = 0, arguments_progress = positional_arguments_count;
         names_progress != keyword_names_count; ++arguments_progress, ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(keyword_names_tuple, names_progress);
        int const consumed = ustore_py_verb_option_from_keyword(context, name, arguments[arguments_progress], options);
        if (consumed < 0) return -1;
        if (consumed > 0) continue;
        int const matched = scan_match_from_keyword(name, arguments[arguments_progress], match);
        if (matched < 0) return -1;
        if (matched > 0) continue;
        int read = -1;
        if (PyUnicode_CompareWithASCIIString(name, "mode") == 0)
            read = ustore_py_enum_from_object(arguments[arguments_progress], "mode", ustore_py_enum_scan_mode_k,
                                              &chosen);
        else if (PyUnicode_CompareWithASCIIString(name, "direction") == 0)
            read = ustore_py_enum_from_object(arguments[arguments_progress], "direction",
                                              ustore_py_enum_scan_direction_k, &walked);
        else if (PyUnicode_CompareWithASCIIString(name, end_name) == 0) {
            *end = arguments[arguments_progress] == Py_None ? NULL : arguments[arguments_progress];
            read = 0;
        }
        else if (PyUnicode_CompareWithASCIIString(name, "progress") == 0)
            read = ustore_py_progress_from_object(arguments[arguments_progress], progress);
        else PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
        if (read != 0) return -1;
    }
    *mode = (ustore_scan_mode_t)chosen;
    *direction = (ustore_scan_direction_t)walked;
    return 0;
}

/** Reads the key a walk in @p direction starts from, None standing for the first key that way. */
static int start_from_object(PyObject* object, ustore_scan_direction_t direction, ustore_key_t* start) {
    if (object != Py_None) return ustore_py_key_from_object(object, start);
    ustore_key_t const smallest = {0};
    *start = direction == ustore_scan_descending_k ? ustore_key_unknown_k : smallest;
    return 0;
}

/** A batch of scans or samples, whose keys land in the arena its header holds. */
typedef struct keys_call_t {

    /** What the call keeps until it finishes. */
    ustore_py_call_t call;

    /** The key every scan starts from, empty for a sample. */
    ustore_py_keys_t starts;

    /** The key every scan stops before, one per start, or empty for no bound. */
    ustore_py_keys_t ends;

    /** Which way every scan walks from its start. */
    ustore_scan_direction_t direction;

    /** Scans or samples the batch carries. */
    size_t tasks;

    /** Output slot for the offsets into @c keys, inside the arena. */
    ustore_length_t const* offsets;

    /** Output slot for the keys every task found, inside the arena. */
    ustore_key_t const* keys;

    /** Output slot for the BM25 scores of the keys, inside the arena, when the patterns carry weights. */
    float const* scores;

    /** The bytes of the patterns, borrowed until the call finishes. */
    ustore_py_contents_t patterns;

    /** The patterns as bytes, `str` ones encoded to UTF-8, held until the call finishes, or NULL. */
    PyObject* encoded;

    /** One weight per pattern, copied, or NULL. */
    float* weights;

    /** The filter as bytes, held until the call finishes, or NULL. */
    PyObject* filter;
} keys_call_t;

static PyObject* keys_answer(void* erased) {
    keys_call_t const* const call = (keys_call_t const*)erased;
    return ustore_py_keys_new(call->call.arena, call->offsets, call->keys, call->tasks, call->scores);
}

static void keys_release(void* erased) {
    keys_call_t* const call = (keys_call_t*)erased;
    ustore_py_keys_release(&call->starts);
    ustore_py_keys_release(&call->ends);
    ustore_py_contents_release(&call->patterns);
    Py_CLEAR(call->encoded);
    Py_CLEAR(call->filter);
    PyMem_Free(call->weights);
    call->weights = NULL;
    ustore_py_call_release(&call->call);
}

/** The bytes of @p object, a `str` encoded to UTF-8 or a bytes-like object as it is, as a new reference. */
static PyObject* match_bytes(PyObject* object, char const* field) {
    if (PyUnicode_Check(object)) return PyUnicode_AsUTF8String(object);
    if (PyObject_CheckBuffer(object)) return Py_NewRef(object);
    PyErr_Format(PyExc_TypeError, "The '%s' option takes str or bytes-like items, got %S", field, Py_TYPE(object));
    return NULL;
}

/** Gathers into @p call what @p match names, borrowing its patterns, copying its weights and holding its filter until
 *  the call finishes, then writes the ABI's spelling of them into @p patterns and @p filter. */
static int match_fill(keys_call_t* call, scan_match_t const* match, ustore_patterns_t* patterns,
                      ustore_bytes_view_t* filter) {
    call->call.predicate.callable = Py_XNewRef(match->predicate);
    if (match->patterns) {
        PyObject* const listed = PySequence_Tuple(match->patterns);
        if (!listed) return -1;
        Py_ssize_t const count = PyTuple_GET_SIZE(listed);
        call->encoded = PyTuple_New(count);
        for (Py_ssize_t index = 0; call->encoded && index != count; ++index) {
            PyObject* const encoded = match_bytes(PyTuple_GET_ITEM(listed, index), "patterns");
            if (!encoded) Py_CLEAR(call->encoded);
            else PyTuple_SET_ITEM(call->encoded, index, encoded);
        }
        Py_DECREF(listed);
        if (!call->encoded ||
            ustore_py_contents_reserve(&call->patterns, (size_t)count, ustore_py_contents_per_task_k) != 0)
            return -1;
        for (Py_ssize_t index = 0; index != count; ++index)
            if (ustore_py_contents_borrow(PyTuple_GET_ITEM(call->encoded, index), &call->patterns) != 0) return -1;
        patterns->patterns = call->patterns.view;
        patterns->count = (ustore_size_t)count;
        patterns->kind = (ustore_pattern_kind_t)match->kind;
        patterns->sensitivity = (ustore_case_sensitivity_t)match->sensitivity;
        if (match->weights) {
            PyObject* const weights = PySequence_Tuple(match->weights);
            if (!weights) return -1;
            if (PyTuple_GET_SIZE(weights) != count) {
                Py_DECREF(weights);
                PyErr_SetString(PyExc_ValueError, "Weights count one per pattern");
                return -1;
            }
            call->weights = PyMem_New(float, (size_t)count + 1);
            for (Py_ssize_t index = 0; call->weights && index != count; ++index)
                call->weights[index] = (float)PyFloat_AsDouble(PyTuple_GET_ITEM(weights, index));
            Py_DECREF(weights);
            if (!call->weights) {
                PyErr_NoMemory();
                return -1;
            }
            if (PyErr_Occurred()) return -1;
            patterns->weights = call->weights;
        }
    }
    if (match->filter) {
        call->filter = match_bytes(match->filter, "filter");
        if (!call->filter) return -1;
        Py_buffer view;
        if (PyObject_GetBuffer(call->filter, &view, PyBUF_SIMPLE) != 0) return -1;
        // The held object keeps the bytes in place, so the view outlives the released export
        filter->data = (uint8_t const*)view.buf;
        filter->length = (ustore_size_t)view.len;
        PyBuffer_Release(&view);
    }
    return 0;
}

// A batch of no tasks answers an empty tape, and the ABI takes no starts for no tasks, so it is final at once.
static void scan_submit(void* arguments) {
    ustore_blobs_scan_t* const scan = (ustore_blobs_scan_t*)arguments;
    if (scan->count) ustore_blobs_scan(scan);
}

static void sample_submit(void* arguments) {
    ustore_blobs_sample_t* const sample = (ustore_blobs_sample_t*)arguments;
    if (sample->count) ustore_blobs_sample(sample);
}

static void docs_sample_submit(void* arguments) {
    ustore_docs_sample_t* const sample = (ustore_docs_sample_t*)arguments;
    if (sample->count) ustore_docs_sample(sample);
}

static void pages_sample_submit(void* arguments) {
    ustore_pages_sample_t* const sample = (ustore_pages_sample_t*)arguments;
    if (sample->count) ustore_pages_sample(sample);
}

static void vertices_sample_submit(void* arguments) {
    ustore_graph_sample_vertices_t* const sample = (ustore_graph_sample_vertices_t*)arguments;
    if (sample->count) ustore_graph_sample_vertices(sample);
}

static void docs_scan_submit(void* arguments) {
    ustore_docs_scan_t* const scan = (ustore_docs_scan_t*)arguments;
    if (scan->count) ustore_docs_scan(scan);
}

static void pages_scan_submit(void* arguments) {
    ustore_pages_scan_t* const scan = (ustore_pages_scan_t*)arguments;
    if (scan->count) ustore_pages_scan(scan);
}

static void vertices_scan_submit(void* arguments) {
    ustore_graph_scan_vertices_t* const scan = (ustore_graph_scan_vertices_t*)arguments;
    if (scan->count) ustore_graph_scan_vertices(scan);
}

static ustore_py_verb_t const scan_verb = {sizeof(keys_call_t), scan_submit, keys_answer, keys_release};

// They answer and release as a scan of keys does, so a call opened for one runs through any of them
static ustore_py_verb_t const docs_scan_verb = {sizeof(keys_call_t), docs_scan_submit, keys_answer, keys_release};
static ustore_py_verb_t const pages_scan_verb = {sizeof(keys_call_t), pages_scan_submit, keys_answer, keys_release};
static ustore_py_verb_t const vertices_scan_verb = {sizeof(keys_call_t), vertices_scan_submit, keys_answer,
                                                    keys_release};

static ustore_py_verb_t const sample_verb = {sizeof(keys_call_t), sample_submit, keys_answer, keys_release};
static ustore_py_verb_t const docs_sample_verb = {sizeof(keys_call_t), docs_sample_submit, keys_answer, keys_release};
static ustore_py_verb_t const pages_sample_verb = {sizeof(keys_call_t), pages_sample_submit, keys_answer, keys_release};
static ustore_py_verb_t const vertices_sample_verb = {sizeof(keys_call_t), vertices_sample_submit, keys_answer,
                                                      keys_release};

/** Runs the scans gathered into @p call through the scan of @p modality, reporting to @p progress and claiming
 *  @p arena for their keys. */
static PyObject* keys_scan(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                           ustore_modality_t modality, size_t wanted, ustore_scan_mode_t mode, PyObject* progress,
                           scan_match_t const* match, keys_call_t* call, PyObject* arena) {
    if (ustore_py_arena_claim(arena, &call->call.arena) != 0) return ustore_py_call_discard(context, &scan_verb, call);
    call->call.collection = options->collection;
    call->call.progress = Py_XNewRef(progress);
    call->tasks = call->starts.count;
    scan_match_t const nothing = {0};
    if (!match) match = &nothing;
    ustore_patterns_t patterns = {0};
    ustore_bytes_view_t filter = {0};
    if (scan_match_admitted(match, modality) != 0 || match_fill(call, match, &patterns, &filter) != 0)
        return ustore_py_call_discard(context, &scan_verb, call);
    ustore_predicate_t const predicate = ustore_py_predicate_of(&call->call.predicate, match->cost);
    if (modality == ustore_modality_docs_k) {
        ustore_docs_scan_t scan = {
            .database = context->database_handle,
            .executor = ustore_py_context_executor(context),
            .outcome = &call->call.outcome,
            .completion = context->completion,
            .view = context->view,
            .count = call->tasks,
            .collections = {&call->call.collection, 0},
            .start_keys = {call->starts.keys, sizeof(ustore_key_t)},
            .end_keys = ustore_py_ends_view(&call->ends),
            .direction = call->direction,
            .filter = filter,
            .predicate = predicate,
            .offsets = &call->offsets,
            .keys = &call->keys,
            .arena = ustore_py_arena_handle(call->call.arena),
            .wanted = wanted,
            .mode = mode,
            .progress = ustore_py_progress_callback(&call->call),
            .progress_state = &call->call,
        };
        return ustore_py_call_run(context, &docs_scan_verb, call, &scan);
    }
    if (modality == ustore_modality_pages_k) {
        ustore_pages_scan_t scan = {
            .database = context->database_handle,
            .executor = ustore_py_context_executor(context),
            .outcome = &call->call.outcome,
            .completion = context->completion,
            .view = context->view,
            .count = call->tasks,
            .collections = {&call->call.collection, 0},
            .start_keys = {call->starts.keys, sizeof(ustore_key_t)},
            .end_keys = ustore_py_ends_view(&call->ends),
            .direction = call->direction,
            .filter = filter,
            .predicate = predicate,
            .offsets = &call->offsets,
            .keys = &call->keys,
            .arena = ustore_py_arena_handle(call->call.arena),
            .wanted = wanted,
            .mode = mode,
            .progress = ustore_py_progress_callback(&call->call),
            .progress_state = &call->call,
        };
        return ustore_py_call_run(context, &pages_scan_verb, call, &scan);
    }
    if (modality == ustore_modality_graph_k) {
        ustore_graph_scan_vertices_t scan = {
            .database = context->database_handle,
            .executor = ustore_py_context_executor(context),
            .outcome = &call->call.outcome,
            .completion = context->completion,
            .view = context->view,
            .count = call->tasks,
            .collections = {&call->call.collection, 0},
            .start_keys = {call->starts.keys, sizeof(ustore_key_t)},
            .end_keys = ustore_py_ends_view(&call->ends),
            .direction = call->direction,
            .predicate = predicate,
            .offsets = &call->offsets,
            .keys = &call->keys,
            .arena = ustore_py_arena_handle(call->call.arena),
            .wanted = wanted,
            .mode = mode,
            .progress = ustore_py_progress_callback(&call->call),
            .progress_state = &call->call,
        };
        return ustore_py_call_run(context, &vertices_scan_verb, call, &scan);
    }
    ustore_blobs_scan_t scan = {
        .database = context->database_handle,
        .executor = ustore_py_context_executor(context),
        .outcome = &call->call.outcome,
        .completion = context->completion,
        .view = context->view,
        .count = call->tasks,
        .collections = {&call->call.collection, 0},
        .start_keys = {call->starts.keys, sizeof(ustore_key_t)},
        .end_keys = ustore_py_ends_view(&call->ends),
        .direction = call->direction,
        .patterns = patterns,
        .predicate = predicate,
        .offsets = &call->offsets,
        .keys = &call->keys,
        .scores = patterns.weights ? &call->scores : NULL,
        .arena = ustore_py_arena_handle(call->call.arena),
        .wanted = wanted,
        .mode = mode,
        .progress = ustore_py_progress_callback(&call->call),
        .progress_state = &call->call,
    };
    return ustore_py_call_run(context, &scan_verb, call, &scan);
}

char const doc_scan[] =                                                                        //
    "scan(start, wanted, arena, *, collection, end=None, direction=ScanDirection.ASCENDING,\n" //
    "mode=ScanMode.ORDERED, progress=None) answers a Keys tape.\n"                             //
    "\n"                                                                                       //
    "Lists up to `wanted` keys from `start` towards `end`. Values are not exported, so a\n"    //
    "read follows the scan, or a `cursor()` walks a collection with its values instead.\n"     //
    "\n"                                                                                       //
    "Args:\n"                                                                                  //
    "  start (KeyLike | None): The first key the scan may return, or None for the first\n"     //
    "    key in its direction.\n"                                                              //
    "  wanted (int): Most keys the scan returns.\n"                                            //
    "  arena (Arena): Where the keys land, rewound by this call.\n"                            //
    "  collection (UUID | str): The collection, by identifier or by name.\n"                   //
    "  end (KeyLike, optional): The key the scan stops before, or None for no bound.\n"        //
    "  direction (ScanDirection, optional): `ScanDirection.DESCENDING` walks down from\n"      //
    "    `start`, largest key first.\n"                                                        //
    "  mode (ScanMode, optional): `ScanMode.UNORDERED` lets keys come out of order,\n"         //
    "    removed or repeated, to speed up the read after the scan; no engine uses it yet.\n"   //
    "  progress (Callable[[int, int], bool | None], optional): Called with the scans\n"        //
    "    finished and the scans in all; a falsy answer other than None cancels the call.\n"    //
    "Returns:\n"                                                                               //
    "  Keys: One run of keys, in the order of the direction.\n"                                //
    "Raises:\n"                                                                                //
    "  CancelledError: The progress callable stopped the scan.";

/** Runs one scan of @p modality from @p start in @p direction, stopping before @p end unless it is
 *  NULL, narrowed by what @p match names when it is not NULL. */
static PyObject* keys_scan_matched(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                                   ustore_modality_t modality, ustore_key_t const* start, ustore_key_t const* end,
                                   ustore_scan_direction_t direction, size_t wanted, ustore_scan_mode_t mode,
                                   PyObject* progress, scan_match_t const* match, PyObject* arena) {
    keys_call_t stack;
    keys_call_t* const call = ustore_py_call_open(context, &scan_verb, &stack);
    if (!call) return NULL;
    call->starts.inline_keys[0] = *start;
    call->starts.keys = call->starts.inline_keys;
    call->starts.count = 1;
    if (end) {
        call->ends.inline_keys[0] = *end;
        call->ends.keys = call->ends.inline_keys;
        call->ends.count = 1;
    }
    call->direction = direction;
    return keys_scan(context, options, modality, wanted, mode, progress, match, call, arena);
}

PyObject* ustore_py_keys_scan(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                              ustore_modality_t modality, ustore_key_t const* start, size_t wanted,
                              ustore_scan_mode_t mode, PyObject* progress, PyObject* arena) {
    return keys_scan_matched(context, options, modality, start, NULL, ustore_scan_ascending_k, wanted, mode, progress,
                             NULL, arena);
}

PyObject* ustore_py_api_scan_of(PyObject* self, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                                PyObject* keyword_names_tuple, char const* name, ustore_modality_t modality) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_keyword_names_count_checked(name, positional_arguments_count, keyword_names_tuple, 3) < 0)
        return NULL;

    ustore_py_verb_options_t options;
    ustore_scan_mode_t mode = ustore_scan_ordered_k;
    ustore_scan_direction_t direction = ustore_scan_ascending_k;
    PyObject* end_object = NULL;
    PyObject* progress = NULL;
    scan_match_t match;
    if (scan_options_from_keywords(&context, arguments, positional_arguments_count, keyword_names_tuple, "end",
                                   &options, &mode, &direction, &end_object, &progress, &match) != 0)
        return NULL;

    ustore_key_t start = {0};
    ustore_key_t end = {0};
    size_t wanted = 0;
    if (start_from_object(arguments[0], direction, &start) != 0) return NULL;
    if (end_object && ustore_py_key_from_object(end_object, &end) != 0) return NULL;
    if (ustore_py_size_from_object(arguments[1], "wanted", &wanted) != 0) return NULL;
    return keys_scan_matched(&context, &options, modality, &start, end_object ? &end : NULL, direction, wanted, mode,
                             progress, &match, arguments[2]);
}

PyObject* ustore_py_api_scan(PyObject* self, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                             PyObject* keyword_names_tuple) {
    return ustore_py_api_scan_of(self, arguments, positional_arguments_count, keyword_names_tuple, "scan",
                                 ustore_modality_blobs_k);
}

char const doc_scan_batch[] =                                                                  //
    "scan_batch(starts, wanted, arena, *, collection, ends=None,\n"                            //
    "direction=ScanDirection.ASCENDING, mode=ScanMode.ORDERED, progress=None)\n"               //
    "answers a Keys tape.\n"                                                                   //
    "\n"                                                                                       //
    "Runs many scans in one call, which is what the ABI takes and what an engine can\n"        //
    "spread across its threads; indexing the answer by start gives the run that scan\n"        //
    "found.\n"                                                                                 //
    "\n"                                                                                       //
    "Args:\n"                                                                                  //
    "  starts (Sequence[KeyLike] | buffer): The first key every scan may return.\n"            //
    "  wanted (int): Most keys every scan returns.\n"                                          //
    "  arena (Arena): Where the keys land, rewound by this call.\n"                            //
    "  collection (UUID | str): The collection, by identifier or by name.\n"                   //
    "  ends (Sequence[KeyLike] | buffer, optional): The key every scan stops before, one\n"    //
    "    per start, or None for no bound.\n"                                                   //
    "  direction (ScanDirection, optional): Which way every scan walks, as `scan` takes it.\n" //
    "  mode (ScanMode, optional): In what order every scan hands its keys over, as\n"          //
    "    `scan` takes it.\n"                                                                   //
    "  progress (Callable[[int, int], bool | None], optional): Called with the scans\n"        //
    "    finished and the scans in all, from whichever thread finished the last one, never\n"  //
    "    two at once; a falsy answer other than None cancels the call.\n"                      //
    "Returns:\n"                                                                               //
    "  Keys: One run of keys per start, in the order they were asked for.\n"                   //
    "Raises:\n"                                                                                //
    "  CancelledError: The progress callable stopped the scans.";

PyObject* ustore_py_api_scan_batch(PyObject* self, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                                   PyObject* keyword_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_keyword_names_count_checked("scan_batch", positional_arguments_count, keyword_names_tuple, 3) < 0)
        return NULL;

    ustore_py_verb_options_t options;
    ustore_scan_mode_t mode = ustore_scan_ordered_k;
    ustore_scan_direction_t direction = ustore_scan_ascending_k;
    PyObject* ends = NULL;
    PyObject* progress = NULL;
    scan_match_t match;
    if (scan_options_from_keywords(&context, arguments, positional_arguments_count, keyword_names_tuple, "ends",
                                   &options, &mode, &direction, &ends, &progress, &match) != 0)
        return NULL;

    size_t wanted = 0;
    if (ustore_py_size_from_object(arguments[1], "wanted", &wanted) != 0) return NULL;
    keys_call_t stack;
    keys_call_t* const call = ustore_py_call_open(&context, &scan_verb, &stack);
    if (!call) return NULL;
    if (ustore_py_keys_from_object(arguments[0], &call->starts) != 0 ||
        ustore_py_ends_from_object(ends, &call->starts, &call->ends) != 0)
        return ustore_py_call_discard(&context, &scan_verb, call);
    call->direction = direction;
    return keys_scan(&context, &options, ustore_modality_blobs_k, wanted, mode, progress, &match, call, arguments[2]);
}

/** Runs @p tasks samples of up to @p wanted keys each through the sample of @p modality, claiming @p arena for their
 *  keys. */
static PyObject* keys_sample(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                             ustore_modality_t modality, size_t tasks, size_t wanted, PyObject* arena) {
    keys_call_t stack;
    keys_call_t* const call = ustore_py_call_open(context, &sample_verb, &stack);
    if (!call) return NULL;
    if (ustore_py_arena_claim(arena, &call->call.arena) != 0)
        return ustore_py_call_discard(context, &sample_verb, call);
    call->call.collection = options->collection;
    call->tasks = tasks;
    if (modality == ustore_modality_docs_k) {
        ustore_docs_sample_t sample = {
            .database = context->database_handle,
            .executor = ustore_py_context_executor(context),
            .outcome = &call->call.outcome,
            .completion = context->completion,
            .view = context->view,
            .count = tasks,
            .collections = {&call->call.collection, 0},
            .offsets = &call->offsets,
            .keys = &call->keys,
            .arena = ustore_py_arena_handle(call->call.arena),
            .wanted = wanted,
        };
        return ustore_py_call_run(context, &docs_sample_verb, call, &sample);
    }
    if (modality == ustore_modality_pages_k) {
        ustore_pages_sample_t sample = {
            .database = context->database_handle,
            .executor = ustore_py_context_executor(context),
            .outcome = &call->call.outcome,
            .completion = context->completion,
            .view = context->view,
            .count = tasks,
            .collections = {&call->call.collection, 0},
            .offsets = &call->offsets,
            .keys = &call->keys,
            .arena = ustore_py_arena_handle(call->call.arena),
            .wanted = wanted,
        };
        return ustore_py_call_run(context, &pages_sample_verb, call, &sample);
    }
    if (modality == ustore_modality_graph_k) {
        ustore_graph_sample_vertices_t sample = {
            .database = context->database_handle,
            .executor = ustore_py_context_executor(context),
            .outcome = &call->call.outcome,
            .completion = context->completion,
            .view = context->view,
            .count = tasks,
            .collections = {&call->call.collection, 0},
            .offsets = &call->offsets,
            .keys = &call->keys,
            .arena = ustore_py_arena_handle(call->call.arena),
            .wanted = wanted,
        };
        return ustore_py_call_run(context, &vertices_sample_verb, call, &sample);
    }
    ustore_blobs_sample_t sample = {
        .database = context->database_handle,
        .executor = ustore_py_context_executor(context),
        .outcome = &call->call.outcome,
        .completion = context->completion,
        .view = context->view,
        .count = tasks,
        .collections = {&call->call.collection, 0},
        .offsets = &call->offsets,
        .keys = &call->keys,
        .arena = ustore_py_arena_handle(call->call.arena),
        .wanted = wanted,
    };
    return ustore_py_call_run(context, &sample_verb, call, &sample);
}

char const doc_sample[] =                                                                 //
    "sample(wanted, arena, *, collection) answers a Keys tape.\n"                         //
    "\n"                                                                                  //
    "Draws keys uniformly at random from one collection, which is how a workload picks\n" //
    "entries it has no list of.\n"                                                        //
    "\n"                                                                                  //
    "Args:\n"                                                                             //
    "  wanted (int): Most keys to draw.\n"                                                //
    "  arena (Arena): Where the keys land, rewound by this call.\n"                       //
    "  collection (UUID | str): The collection, by identifier or by name.\n"              //
    "Returns:\n"                                                                          //
    "  Keys: One run of keys, in no particular order.";

PyObject* ustore_py_keys_sample(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                                ustore_modality_t modality, size_t wanted, PyObject* arena) {
    return keys_sample(context, options, modality, 1, wanted, arena);
}

PyObject* ustore_py_api_sample_of(PyObject* self, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                                  PyObject* keyword_names_tuple, char const* name, ustore_modality_t modality) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_keyword_names_count_checked(name, positional_arguments_count, keyword_names_tuple, 2) < 0)
        return NULL;

    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, arguments, positional_arguments_count, keyword_names_tuple,
                                             &options) != 0)
        return NULL;

    size_t wanted = 0;
    if (ustore_py_size_from_object(arguments[0], "wanted", &wanted) != 0) return NULL;
    return keys_sample(&context, &options, modality, 1, wanted, arguments[1]);
}

PyObject* ustore_py_api_sample(PyObject* self, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                               PyObject* keyword_names_tuple) {
    return ustore_py_api_sample_of(self, arguments, positional_arguments_count, keyword_names_tuple, "sample",
                                   ustore_modality_blobs_k);
}

char const doc_sample_batch[] =                                                               //
    "sample_batch(count, wanted, arena, *, collection) answers a Keys tape.\n"                //
    "\n"                                                                                      //
    "Draws `count` independent samples in one call, all from the one collection named,\n"     //
    "which is how a workload builds several random batches without paying for a call each.\n" //
    "\n"                                                                                      //
    "Args:\n"                                                                                 //
    "  count (int): Samples to draw.\n"                                                       //
    "  wanted (int): Most keys every sample draws.\n"                                         //
    "  arena (Arena): Where the keys land, rewound by this call.\n"                           //
    "  collection (UUID | str): The one collection every sample draws from, by\n"             //
    "    identifier or by name.\n"                                                            //
    "Returns:\n"                                                                              //
    "  Keys: One run of keys per sample, in no particular order.";

PyObject* ustore_py_api_sample_batch(PyObject* self, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                                     PyObject* keyword_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_keyword_names_count_checked("sample_batch", positional_arguments_count, keyword_names_tuple, 3) < 0)
        return NULL;

    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, arguments, positional_arguments_count, keyword_names_tuple,
                                             &options) != 0)
        return NULL;

    size_t tasks = 0;
    size_t wanted = 0;
    if (ustore_py_size_from_object(arguments[0], "count", &tasks) != 0) return NULL;
    if (ustore_py_size_from_object(arguments[1], "wanted", &wanted) != 0) return NULL;
    return keys_sample(&context, &options, ustore_modality_blobs_k, tasks, wanted, arguments[2]);
}

char const doc_measure[] =                                                                   //
    "measure(start, end, arena, *, collection, direction=ScanDirection.ASCENDING)\n"         //
    "answers a Measurement.\n"                                                               //
    "\n"                                                                                     //
    "Estimates what a key range holds without walking it, so the answers are bounds\n"       //
    "rather than counts.\n"                                                                  //
    "\n"                                                                                     //
    "Args:\n"                                                                                //
    "  start (KeyLike | None): First key of the range, or None for the first key in the\n"   //
    "    direction.\n"                                                                       //
    "  end (KeyLike | None): The key the range stops before, or None for no bound.\n"        //
    "  arena (Arena): Where the bounds land before they are copied out, rewound by this\n"   //
    "    call.\n"                                                                            //
    "  collection (UUID | str): The collection, by identifier or by name.\n"                 //
    "  direction (ScanDirection, optional): `ScanDirection.DESCENDING` ranges down from\n"   //
    "    `start`.\n"                                                                         //
    "Returns:\n"                                                                             //
    "  Measurement: The lower and upper bounds on the entries, the bytes of their values,\n" //
    "  and the space they take on disk.";

/** A measurement of one range, whose bounds land in the arena its header holds. */
typedef struct measure_call_t {

    /** What the call keeps until it finishes. */
    ustore_py_call_t call;

    /** First key of the range. */
    ustore_key_t start;

    /** The key the range stops before, unless the call named none. */
    ustore_key_t end;

    /** Which way the range runs from @c start. */
    ustore_scan_direction_t direction;

    /** Output slots for the six bounds, in the order of the fields of @c MeasurementType, inside the arena. */
    ustore_size_t const* bounds[6];
} measure_call_t;

static void measure_submit(void* arguments) { ustore_blobs_measure((ustore_blobs_measure_t*)arguments); }
static void docs_measure_submit(void* arguments) { ustore_docs_measure((ustore_docs_measure_t*)arguments); }
static void pages_measure_submit(void* arguments) { ustore_pages_measure((ustore_pages_measure_t*)arguments); }
static void vertices_measure_submit(void* arguments) {
    ustore_graph_measure_vertices((ustore_graph_measure_vertices_t*)arguments);
}

static PyObject* measure_answer(void* erased) {
    measure_call_t const* const call = (measure_call_t const*)erased;
    PyObject* bounds = PyStructSequence_New(MeasurementType);
    for (Py_ssize_t index = 0; bounds && index != 6; ++index) {
        PyObject* const value = PyLong_FromUnsignedLongLong(call->bounds[index] ? call->bounds[index][0] : 0);
        if (!value) Py_CLEAR(bounds);
        else PyStructSequence_SetItem(bounds, index, value);
    }
    return bounds;
}

static void measure_release(void* call) { ustore_py_call_release(&((measure_call_t*)call)->call); }

static ustore_py_verb_t const measure_verb = {sizeof(measure_call_t), measure_submit, measure_answer, measure_release};

/** Measures documents through the call type, answer and release of @c measure_verb. */
static ustore_py_verb_t const docs_measure_verb = {sizeof(measure_call_t), docs_measure_submit, measure_answer,
                                                   measure_release};

/** Measures pages through the call type, answer and release of @c measure_verb. */
static ustore_py_verb_t const pages_measure_verb = {sizeof(measure_call_t), pages_measure_submit, measure_answer,
                                                    measure_release};

/** Measures vertices through the call type, answer and release of @c measure_verb. */
static ustore_py_verb_t const vertices_measure_verb = {sizeof(measure_call_t), vertices_measure_submit, measure_answer,
                                                       measure_release};

PyObject* ustore_py_api_measure_of(PyObject* self, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                                   PyObject* keyword_names_tuple, char const* name, ustore_modality_t modality) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_keyword_names_count_checked(name, positional_arguments_count, keyword_names_tuple, 3) < 0)
        return NULL;

    static char const* const names[] = {"direction", NULL};
    PyObject* direction_object = NULL;
    ustore_py_verb_options_t options;
    if (ustore_py_verb_keywords_from_arguments(&context, arguments, positional_arguments_count, keyword_names_tuple,
                                               names, &options, &direction_object) != 0)
        return NULL;
    int direction = ustore_scan_ascending_k;
    if (direction_object &&
        ustore_py_enum_from_object(direction_object, "direction", ustore_py_enum_scan_direction_k, &direction) != 0)
        return NULL;

    ustore_key_t start = {0};
    ustore_key_t end = {0};
    PyObject* const end_object = arguments[1] == Py_None ? NULL : arguments[1];
    if (start_from_object(arguments[0], (ustore_scan_direction_t)direction, &start) != 0) return NULL;
    if (end_object && ustore_py_key_from_object(end_object, &end) != 0) return NULL;

    measure_call_t stack;
    measure_call_t* const call = ustore_py_call_open(&context, &measure_verb, &stack);
    if (!call) return NULL;
    if (ustore_py_arena_claim(arguments[2], &call->call.arena) != 0)
        return ustore_py_call_discard(&context, &measure_verb, call);
    call->call.collection = options.collection;
    call->start = start;
    call->end = end;
    call->direction = (ustore_scan_direction_t)direction;
    ustore_keys_view_t const end_keys = {end_object ? &call->end : NULL, 0};
    if (modality == ustore_modality_docs_k) {
        ustore_docs_measure_t measure = {
            .database = context.database_handle,
            .executor = ustore_py_context_executor(&context),
            .outcome = &call->call.outcome,
            .completion = context.completion,
            .view = context.view,
            .count = 1,
            .collections = {&call->call.collection, 0},
            .start_keys = {&call->start, 0},
            .end_keys = end_keys,
            .direction = call->direction,
            .min_cardinalities = &call->bounds[0],
            .max_cardinalities = &call->bounds[1],
            .min_value_bytes = &call->bounds[2],
            .max_value_bytes = &call->bounds[3],
            .min_space_usages = &call->bounds[4],
            .max_space_usages = &call->bounds[5],
            .arena = ustore_py_arena_handle(call->call.arena),
        };
        return ustore_py_call_run(&context, &docs_measure_verb, call, &measure);
    }
    if (modality == ustore_modality_pages_k) {
        ustore_pages_measure_t measure = {
            .database = context.database_handle,
            .executor = ustore_py_context_executor(&context),
            .outcome = &call->call.outcome,
            .completion = context.completion,
            .view = context.view,
            .count = 1,
            .collections = {&call->call.collection, 0},
            .start_keys = {&call->start, 0},
            .end_keys = end_keys,
            .direction = call->direction,
            .min_cardinalities = &call->bounds[0],
            .max_cardinalities = &call->bounds[1],
            .min_value_bytes = &call->bounds[2],
            .max_value_bytes = &call->bounds[3],
            .min_space_usages = &call->bounds[4],
            .max_space_usages = &call->bounds[5],
            .arena = ustore_py_arena_handle(call->call.arena),
        };
        return ustore_py_call_run(&context, &pages_measure_verb, call, &measure);
    }
    if (modality == ustore_modality_graph_k) {
        ustore_graph_measure_vertices_t measure = {
            .database = context.database_handle,
            .executor = ustore_py_context_executor(&context),
            .outcome = &call->call.outcome,
            .completion = context.completion,
            .view = context.view,
            .count = 1,
            .collections = {&call->call.collection, 0},
            .start_keys = {&call->start, 0},
            .end_keys = end_keys,
            .direction = call->direction,
            .min_cardinalities = &call->bounds[0],
            .max_cardinalities = &call->bounds[1],
            .min_value_bytes = &call->bounds[2],
            .max_value_bytes = &call->bounds[3],
            .min_space_usages = &call->bounds[4],
            .max_space_usages = &call->bounds[5],
            .arena = ustore_py_arena_handle(call->call.arena),
        };
        return ustore_py_call_run(&context, &vertices_measure_verb, call, &measure);
    }
    ustore_blobs_measure_t measure = {
        .database = context.database_handle,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->call.outcome,
        .completion = context.completion,
        .view = context.view,
        .count = 1,
        .collections = {&call->call.collection, 0},
        .start_keys = {&call->start, 0},
        .end_keys = end_keys,
        .direction = call->direction,
        .min_cardinalities = &call->bounds[0],
        .max_cardinalities = &call->bounds[1],
        .min_value_bytes = &call->bounds[2],
        .max_value_bytes = &call->bounds[3],
        .min_space_usages = &call->bounds[4],
        .max_space_usages = &call->bounds[5],
        .arena = ustore_py_arena_handle(call->call.arena),
    };
    return ustore_py_call_run(&context, &measure_verb, call, &measure);
}

PyObject* ustore_py_api_measure(PyObject* self, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                                PyObject* keyword_names_tuple) {
    return ustore_py_api_measure_of(self, arguments, positional_arguments_count, keyword_names_tuple, "measure",
                                    ustore_modality_blobs_k);
}

#pragma endregion Scans

#pragma region Cursors

/** A walk over the keys of one collection of blobs, documents, pages or graph vertices, pulled page by page into the
 *  arena each pull names. */
typedef struct Cursor {

    /** The object header every instance carries. */
    PyObject ob_base;

    /** The cursor of the modality @c modality names, NULL once closed, or the address of @c cursor_lent while a pull
     *  holds it. */
    void* handle;

    /** Which cursor of the ABI @c handle is: blobs, documents, pages or graph vertices. */
    ustore_modality_t modality;

    /** The encoding every pull of a walk over pages renders them into. */
    ustore_page_encoding_t outputs_encoding;

    /** The predicate the walk compiled at init, which every pull asks, and what it raised. */
    ustore_py_predicate_t predicate;

    /** The database the cursor was opened on, which the cursor holds open in C. */
    ustore_database_t database;

    /** The executor every pull blocks on, held, or NULL for the calling thread. */
    Executor* executor;
} Cursor;

/** The keys one pull wrote into an arena, and their values when it asked, viewed until the arena rewinds. */
typedef struct Page {

    /** The object header every instance carries. */
    PyObject ob_base;

    /** The @c Arena the keys live in, kept alive so they outlive every other name for them. */
    PyObject* arena;

    /** The arena's generation when the pull wrote the keys. */
    size_t generation;

    /** The keys, in ascending order. */
    ustore_key_t const* keys;

    /** Keys the page holds. */
    size_t count;

    /** The @c Values of the keys, or None when the pull asked for none. */
    PyObject* values;

    /** The shape the keys export: keys, then the sixteen bytes of each. */
    Py_ssize_t shape[2];

    /** The strides matching @c shape. */
    Py_ssize_t strides[2];
} Page;

static char const doc_Page[] =                                                                   //
    "The keys one pull of a Cursor wrote into an `Arena`, viewing it until it rewinds.\n"        //
    "\n"                                                                                         //
    "Indexing answers a `uuid.UUID`, and the whole page exports through the buffer protocol\n"   //
    "as an array of shape (keys, 16) bytes, pinning the arena until released. Its length\n"      //
    "counts the keys, so the empty page ending a walk is falsy. `values` holds the Values of\n"  //
    "the keys when the pull asked for them, every one present, and None otherwise. Read after\n" //
    "a later call rewound the arena, it raises `UninitializedError`.";

/** A new @c Page over the @p count keys a pull wrote into @p arena, taking @p values, their @c Values or None. */
static PyObject* page_new(PyObject* arena, ustore_key_t const* keys, size_t count, PyObject* values) {
    Page* const page = PyObject_New(Page, &PageType);
    if (!page) {
        Py_DECREF(values);
        return NULL;
    }
    page->arena = Py_NewRef(arena);
    page->generation = ustore_py_arena_generation(arena);
    page->keys = keys;
    page->count = count;
    page->values = values;
    page->shape[0] = (Py_ssize_t)count;
    page->shape[1] = (Py_ssize_t)sizeof(ustore_key_t);
    page->strides[0] = (Py_ssize_t)sizeof(ustore_key_t);
    page->strides[1] = 1;
    return (PyObject*)page;
}

static void Page_dealloc(Page* self) {
    Py_XDECREF(self->values);
    Py_XDECREF(self->arena);
    PyObject_Free(self);
}

static Py_ssize_t Page_length(Page* self) { return (Py_ssize_t)self->count; }

static PyObject* Page_item(Page* self, Py_ssize_t key_index) {
    if (key_index < 0 || (size_t)key_index >= self->count) {
        PyErr_SetString(PyExc_IndexError, "Key index out of range");
        return NULL;
    }
    if (ustore_py_arena_pin(self->arena, self->generation) != 0) return NULL;
    PyObject* const key = ustore_py_uuid_to_object(&self->keys[key_index].uuid);
    ustore_py_arena_unpin(self->arena);
    return key;
}

static int Page_getbuffer(Page* self, Py_buffer* view, int flags) {
    view->obj = NULL;
    if (ustore_py_arena_pin(self->arena, self->generation) != 0) return -1;
    if (ustore_py_buffer_fill(view, (PyObject*)self, self->keys, "B", 1, 2, self->shape, self->strides, flags) == 0)
        return 0;
    ustore_py_arena_unpin(self->arena);
    return -1;
}

static void Page_releasebuffer(Page* self, Py_buffer* Py_UNUSED(view)) { ustore_py_arena_unpin(self->arena); }

static PyObject* Page_repr(Page* self) { return PyUnicode_FromFormat("Page(keys=%zu)", self->count); }

static char const doc_Page_values[] = "The Values of the keys when the pull asked for them, or None.";

static PyObject* Page_get_values(Page* self, void* Py_UNUSED(closure)) { return Py_NewRef(self->values); }

static PyObject* Page_dlpack_device(Page* self, PyObject* Py_UNUSED(unused)) {
    return ustore_py_dlpack_device(self->arena);
}

static PyMethodDef Page_methods[] = {
    {"__dlpack__", (PyCFunction)(void (*)(void))ustore_py_dlpack, ustore_py_method_flags_k, ustore_py_dlpack_doc},
    {"__dlpack_device__", (PyCFunction)Page_dlpack_device, METH_NOARGS, ustore_py_dlpack_device_doc},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef Page_getset[] = {
    {"values", (getter)Page_get_values, NULL, doc_Page_values, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PySequenceMethods Page_as_sequence = {
    .sq_length = (lenfunc)Page_length,
    .sq_item = (ssizeargfunc)Page_item,
};

static PyBufferProcs Page_as_buffer = {
    .bf_getbuffer = (getbufferproc)Page_getbuffer,
    .bf_releasebuffer = (releasebufferproc)Page_releasebuffer,
};

PyTypeObject PageType = {
    PyVarObject_HEAD_INIT(NULL, 0) //
        .tp_name = "ustore.Page",
    .tp_doc = doc_Page,
    .tp_basicsize = sizeof(Page),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = NULL,
    .tp_dealloc = (destructor)Page_dealloc,
    .tp_repr = (reprfunc)Page_repr,
    .tp_as_sequence = &Page_as_sequence,
    .tp_as_buffer = &Page_as_buffer,
    .tp_methods = Page_methods,
    .tp_getset = Page_getset,
};

/** One pull of a cursor, whose keys and values land in the arena its header holds. */
typedef struct pull_call_t {

    /** What the call keeps until it finishes. */
    ustore_py_call_t call;

    /** Output slot for the keys of the page, inside the arena. */
    ustore_key_t const* keys;

    /** Output slot for the tape of their values, inside the arena, when the pull asks for them. */
    ustore_strings_view_t values;

    /** Output slot for the keys the page holds. */
    ustore_size_t written;
} pull_call_t;

static void pull_submit(void* arguments) { ustore_blobs_cursor_next((ustore_blobs_cursor_next_t*)arguments); }
static void docs_pull_submit(void* arguments) { ustore_docs_cursor_next((ustore_docs_cursor_next_t*)arguments); }
static void pages_pull_submit(void* arguments) { ustore_pages_cursor_next((ustore_pages_cursor_next_t*)arguments); }
static void vertices_pull_submit(void* arguments) {
    ustore_graph_vertices_cursor_next((ustore_graph_vertices_cursor_next_t*)arguments);
}

static PyObject* pull_answer_keys(void* erased) {
    pull_call_t const* const call = (pull_call_t const*)erased;
    return page_new(call->call.arena, call->keys, (size_t)call->written, Py_NewRef(Py_None));
}

static PyObject* pull_answer_pairs(void* erased) {
    pull_call_t const* const call = (pull_call_t const*)erased;
    PyObject* const values = ustore_py_values_new(call->call.arena, NULL, &call->values, (size_t)call->written);
    return values ? page_new(call->call.arena, call->keys, (size_t)call->written, values) : NULL;
}

static void pull_release(void* call) { ustore_py_call_release(&((pull_call_t*)call)->call); }

/** A pull answering the keys alone. */
static ustore_py_verb_t const pull_keys_verb = {sizeof(pull_call_t), pull_submit, pull_answer_keys, pull_release};

/** A pull answering the keys with their values. */
static ustore_py_verb_t const pull_pairs_verb = {sizeof(pull_call_t), pull_submit, pull_answer_pairs, pull_release};

// A pull of documents, pages or vertices answers as a pull of blobs does
static ustore_py_verb_t const docs_pull_keys_verb = {sizeof(pull_call_t), docs_pull_submit, pull_answer_keys,
                                                     pull_release};
static ustore_py_verb_t const docs_pull_pairs_verb = {sizeof(pull_call_t), docs_pull_submit, pull_answer_pairs,
                                                      pull_release};
static ustore_py_verb_t const pages_pull_keys_verb = {sizeof(pull_call_t), pages_pull_submit, pull_answer_keys,
                                                      pull_release};
static ustore_py_verb_t const pages_pull_pairs_verb = {sizeof(pull_call_t), pages_pull_submit, pull_answer_pairs,
                                                       pull_release};
static ustore_py_verb_t const vertices_pull_keys_verb = {sizeof(pull_call_t), vertices_pull_submit, pull_answer_keys,
                                                         pull_release};

/** Stands in the handle slot of a cursor while one pull holds its handle, refusing another pull or a close. */
static char cursor_lent;

/**
 *  @brief Frees the cursor of @p self once, fenced against a concurrent close and refused during a pull.
 *  @return 0 on success, -1 with a @c ContendedError set while a pull holds the handle.
 */
static int cursor_close(Cursor* self) {
    void* const lent = (void*)&cursor_lent;
    void* handle = NULL;
    ustore_py_critical_section_begin_((PyObject*)self);
    handle = self->handle;
    if (handle != lent) self->handle = NULL;
    ustore_py_critical_section_end_();
    if (handle == lent) return ustore_py_raise(ustore_status_contended_k, "A pull of this cursor is still running");
    if (!handle) return 0;
    ustore_modality_t const modality = self->modality;
    Py_BEGIN_ALLOW_THREADS;
    if (modality == ustore_modality_docs_k) ustore_docs_cursor_free((ustore_docs_cursor_t)handle);
    else if (modality == ustore_modality_pages_k) ustore_pages_cursor_free((ustore_pages_cursor_t)handle);
    else if (modality == ustore_modality_graph_k)
        ustore_graph_vertices_cursor_free((ustore_graph_vertices_cursor_t)handle);
    else ustore_blobs_cursor_free((ustore_blobs_cursor_t)handle);
    Py_END_ALLOW_THREADS;
    return 0;
}

static void Cursor_dealloc(Cursor* self) {
    // No pull can hold the handle, since every pull holds the cursor.
    cursor_close(self);
    Py_XDECREF((PyObject*)self->executor);
    Py_XDECREF(self->predicate.callable);
    Py_XDECREF(self->predicate.raised);
    PyObject_Free(self);
}

/** Pulls the next page of the cursor @p handle, which @p self lent its pull, as @p verb answers it. */
static PyObject* cursor_pull(Cursor* self, void* handle, size_t wanted, PyObject* arena, int values,
                             PyObject* progress) {
    if (self->executor && !self->executor->handle) {
        ustore_py_raise(ustore_status_uninitialized_k, "The executor is closed");
        return NULL;
    }
    if (values && self->modality == ustore_modality_graph_k) {
        PyErr_SetString(PyExc_TypeError, "A walk over vertices pulls keys alone");
        return NULL;
    }
    ustore_py_verb_t const* const verb = self->modality == ustore_modality_docs_k
                                             ? (values ? &docs_pull_pairs_verb : &docs_pull_keys_verb)
                                         : self->modality == ustore_modality_pages_k
                                             ? (values ? &pages_pull_pairs_verb : &pages_pull_keys_verb)
                                         : self->modality == ustore_modality_graph_k ? &vertices_pull_keys_verb
                                         : values                                    ? &pull_pairs_verb
                                                                                     : &pull_keys_verb;
    ustore_py_context_t const context = {.executor = self->executor, .completion = ustore_completion_blocking_k};
    pull_call_t stack;
    pull_call_t* const call = ustore_py_call_open(&context, verb, &stack);
    if (ustore_py_arena_claim(arena, &call->call.arena) != 0) return ustore_py_call_discard(&context, verb, call);
    call->call.progress = Py_XNewRef(progress);
    PyObject* page = NULL;
    if (self->modality == ustore_modality_docs_k) {
        ustore_docs_cursor_next_t next = {
            .database = self->database,
            .executor = ustore_py_context_executor(&context),
            .outcome = &call->call.outcome,
            .cursor = (ustore_docs_cursor_t)handle,
            .keys = &call->keys,
            .values = values ? &call->values : NULL,
            .written = &call->written,
            .arena = ustore_py_arena_handle(call->call.arena),
            .wanted = wanted,
            .progress = ustore_py_progress_callback(&call->call),
            .progress_state = &call->call,
        };
        page = ustore_py_call_run(&context, verb, call, &next);
    }
    else if (self->modality == ustore_modality_pages_k) {
        ustore_pages_cursor_next_t next = {
            .database = self->database,
            .executor = ustore_py_context_executor(&context),
            .outcome = &call->call.outcome,
            .cursor = (ustore_pages_cursor_t)handle,
            .keys = &call->keys,
            .values = values ? &call->values : NULL,
            .written = &call->written,
            .arena = ustore_py_arena_handle(call->call.arena),
            .wanted = wanted,
            .outputs_encoding = self->outputs_encoding,
            .progress = ustore_py_progress_callback(&call->call),
            .progress_state = &call->call,
        };
        page = ustore_py_call_run(&context, verb, call, &next);
    }
    else if (self->modality == ustore_modality_graph_k) {
        ustore_graph_vertices_cursor_next_t next = {
            .database = self->database,
            .executor = ustore_py_context_executor(&context),
            .outcome = &call->call.outcome,
            .cursor = (ustore_graph_vertices_cursor_t)handle,
            .keys = &call->keys,
            .written = &call->written,
            .arena = ustore_py_arena_handle(call->call.arena),
            .wanted = wanted,
            .progress = ustore_py_progress_callback(&call->call),
            .progress_state = &call->call,
        };
        page = ustore_py_call_run(&context, verb, call, &next);
    }
    else {
        ustore_blobs_cursor_next_t next = {
            .database = self->database,
            .executor = ustore_py_context_executor(&context),
            .outcome = &call->call.outcome,
            .cursor = (ustore_blobs_cursor_t)handle,
            .keys = &call->keys,
            .values = values ? &call->values : NULL,
            .written = &call->written,
            .arena = ustore_py_arena_handle(call->call.arena),
            .wanted = wanted,
            .progress = ustore_py_progress_callback(&call->call),
            .progress_state = &call->call,
        };
        page = ustore_py_call_run(&context, verb, call, &next);
    }
    // What the walk's predicate raised outranks the page the pull answered
    if (self->predicate.raised) {
        Py_CLEAR(page);
        ustore_py_predicate_raise(&self->predicate);
    }
    return page;
}

static char const doc_Cursor[] =                                                              //
    "A walk over the keys of one collection, pulled page by page by `next`.\n"                //
    "\n"                                                                                      //
    "Built through `cursor()` on a database, transaction, snapshot or blocking view, never\n" //
    "by name. It keeps only the key its next pull starts at, so a key written behind the\n"   //
    "walk never comes and one written ahead of it does. A page shorter than `wanted`\n"       //
    "exhausts the walk for good: every later pull answers an empty page without reading,\n"   //
    "whatever was written since. One pull runs at a time, and an overlapping pull or\n"       //
    "`close()` raises ContendedError. Used as a context manager, it closes on exit.";

static char const doc_Cursor_next[] =                                                           //
    "next(wanted, arena, /, *, values=False, progress=None) answers a Page.\n"                  //
    "\n"                                                                                        //
    "Pulls up to `wanted` keys past the last page into `arena`, with their values in the\n"     //
    "same call when asked, blocking on the executor the cursor was opened through. A key\n"     //
    "removed at the head between the listing and the read of one pull reads as an empty\n"      //
    "value. A page shorter than `wanted` exhausts the walk, and an empty one, which is\n"       //
    "falsy, ends it. A pull that fails or is cancelled leaves the cursor where it was. Calls\n" //
    "`ustore_blobs_cursor_next`.\n"                                                             //
    "\n"                                                                                        //
    "Args:\n"                                                                                   //
    "  wanted (int): Most keys the page holds.\n"                                               //
    "  arena (Arena): Where the keys and values land, rewound by this pull.\n"                  //
    "  values (bool, optional): Whether the page carries the values of its keys.\n"             //
    "  progress (Callable[[int, int], bool | None], optional): Called once for the pull,\n"     //
    "    which a falsy answer other than None cancels before the cursor advances.\n"            //
    "Returns:\n"                                                                                //
    "  Page: The keys in ascending order, and their Values when asked.\n"                       //
    "Raises:\n"                                                                                 //
    "  CancelledError: The progress callable stopped the pull.\n"                               //
    "  UninitializedError: The cursor or its executor is closed.";

static PyObject* Cursor_next(Cursor* self, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                             PyObject* keyword_names_tuple) {
    Py_ssize_t const keyword_names_count = ustore_py_keyword_names_count_checked("next", positional_arguments_count,
                                                                                 keyword_names_tuple, 2);
    if (keyword_names_count < 0) return NULL;
    int values = 0;
    PyObject* progress = NULL;
    for (Py_ssize_t names_progress = 0; names_progress != keyword_names_count; ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(keyword_names_tuple, names_progress);
        PyObject* const value = arguments[positional_arguments_count + names_progress];
        int read = -1;
        if (PyUnicode_CompareWithASCIIString(name, "values") == 0) {
            values = PyObject_IsTrue(value);
            read = values < 0 ? -1 : 0;
        }
        else if (PyUnicode_CompareWithASCIIString(name, "progress") == 0)
            read = ustore_py_progress_from_object(value, &progress);
        else PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
        if (read != 0) return NULL;
    }
    size_t wanted = 0;
    if (ustore_py_size_from_object(arguments[0], "wanted", &wanted) != 0) return NULL;

    void* const lent = (void*)&cursor_lent;
    void* handle = NULL;
    ustore_py_critical_section_begin_((PyObject*)self);
    handle = self->handle;
    if (handle) self->handle = lent;
    ustore_py_critical_section_end_();
    if (!handle || handle == lent) {
        ustore_py_raise(handle ? ustore_status_contended_k : ustore_status_uninitialized_k,
                        handle ? "Another pull of this cursor is running" : "The cursor is closed");
        return NULL;
    }
    PyObject* const page = cursor_pull(self, handle, wanted, arguments[1], values, progress);
    ustore_py_critical_section_begin_((PyObject*)self);
    self->handle = handle;
    ustore_py_critical_section_end_();
    return page;
}

static char const doc_Cursor_close[] =                                                       //
    "close() answers None.\n"                                                                //
    "\n"                                                                                     //
    "Frees the walk, giving back the holds it kept on its database and on the transaction\n" //
    "or snapshot it reads through. Closing twice is safe. Calls `ustore_blobs_cursor_free`.";

static PyObject* Cursor_close(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    if (cursor_close((Cursor*)self) != 0) return NULL;
    Py_RETURN_NONE;
}

static PyObject* Cursor_enter(PyObject* self, PyObject* Py_UNUSED(ignored)) { return Py_NewRef(self); }

static PyObject* Cursor_exit(PyObject* self, PyObject* const* Py_UNUSED(arguments),
                             Py_ssize_t Py_UNUSED(positional_arguments_count),
                             PyObject* Py_UNUSED(keyword_names_tuple)) {
    return Cursor_close(self, NULL);
}

static PyMethodDef Cursor_methods[] = {
    {"next", (PyCFunction)(void (*)(void))Cursor_next, ustore_py_method_flags_k, doc_Cursor_next},
    {"close", (PyCFunction)(void (*)(void))Cursor_close, METH_NOARGS, doc_Cursor_close},
    {"__enter__", (PyCFunction)(void (*)(void))Cursor_enter, METH_NOARGS, NULL},
    {"__exit__", (PyCFunction)(void (*)(void))Cursor_exit, ustore_py_method_flags_k, NULL},
    {NULL, NULL, 0, NULL},
};

PyTypeObject CursorType = {
    PyVarObject_HEAD_INIT(NULL, 0) //
        .tp_name = "ustore.Cursor",
    .tp_doc = doc_Cursor,
    .tp_basicsize = sizeof(Cursor),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = NULL,
    .tp_dealloc = (destructor)Cursor_dealloc,
    .tp_methods = Cursor_methods,
};

char const doc_cursor[] =                                                                        //
    "cursor(start=None, /, *, collection, end=None, direction=ScanDirection.ASCENDING,\n"        //
    "patterns=None, pattern_kind=PatternKind.REGEX, sensitivity=CaseSensitivity.CASED,\n"        //
    "predicate=None, predicate_cost=PredicateCost.CHEAP) answers a Cursor.\n"                    //
    "\n"                                                                                         //
    "Opens a walk over the keys of one collection from `start` towards `end`, reading nothing\n" //
    "until its first pull, which `Cursor.next` makes into an arena of its own. Every pull\n"     //
    "blocks on the receiver's executor, or on the calling thread. The cursor holds its\n"        //
    "database open, and the transaction or snapshot it reads through, until it closes.\n"        //
    "Calls `ustore_blobs_cursor_init`.\n"                                                        //
    "\n"                                                                                         //
    "Args:\n"                                                                                    //
    "  start (KeyLike, optional): The first key the walk may return, or None for the first\n"    //
    "    key in its direction.\n"                                                                //
    "  collection (UUID | str): The collection, by identifier or by name.\n"                     //
    "  end (KeyLike, optional): The key the walk stops before, or None for no bound.\n"          //
    "  direction (ScanDirection, optional): `ScanDirection.DESCENDING` walks down from\n"        //
    "    `start`, largest key first.\n"                                                          //
    "  patterns (Sequence[str | bytes], optional): Patterns every value the walk hands out\n"    //
    "    matches, compiled once here.\n"                                                         //
    "  predicate (Callable[[tuple[UUID, ...]], Sequence], optional): Asked once per page\n"      //
    "    about its keys, answering a truthy item for every key the walk keeps.\n"                //
    "Returns:\n"                                                                                 //
    "  Cursor: The walk, pulled page by page.";

char const doc_docs_cursor[] =                                                                    //
    "docs_cursor(start=None, /, *, collection, end=None, direction=ScanDirection.ASCENDING,\n"    //
    "filter=None, predicate=None, predicate_cost=PredicateCost.CHEAP) answers a Cursor over\n"    //
    "documents.\n"                                                                                //
    "\n"                                                                                          //
    "Walks as `cursor()` does, every page keeping the documents `filter`, a JSONPath or a bare\n" //
    "root filter such as `?@.age > 30`, matches, and its pulls reading whole documents when\n"    //
    "asked. Calls `ustore_docs_cursor_init`.";

char const doc_pages_cursor[] =                                                                  //
    "pages_cursor(start=None, /, *, collection, end=None, direction=ScanDirection.ASCENDING,\n"  //
    "filter=None, predicate=None, predicate_cost=PredicateCost.CHEAP,\n"                         //
    "outputs_encoding=PageEncoding.XML) answers a Cursor over pages.\n"                          //
    "\n"                                                                                         //
    "Walks as `cursor()` does, keeping the pages that hold a node of `filter`, a path in\n"      //
    "StringZilla's XPath subset such as '/doc/b[2]', and its pulls rendering whole pages into\n" //
    "`outputs_encoding` when asked. Calls `ustore_pages_cursor_init`.";

char const doc_graph_vertices_cursor[] =                                                         //
    "graph_vertices_cursor(start=None, /, *, collection, end=None,\n"                            //
    "direction=ScanDirection.ASCENDING, predicate=None, predicate_cost=PredicateCost.CHEAP)\n"   //
    "answers a Cursor over vertices.\n"                                                          //
    "\n"                                                                                         //
    "Walks the vertices of one graph collection as `cursor()` walks keys, its pulls answering\n" //
    "keys alone. Calls `ustore_graph_vertices_cursor_init`.";

/** Opens a walk of @p modality as the receiver @p self names, from the start and with the match its arguments give. */
static PyObject* cursor_open(PyObject* self, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                             PyObject* keyword_names_tuple, char const* name, ustore_modality_t modality) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (positional_arguments_count > 1) {
        PyErr_Format(PyExc_TypeError, "%s() takes the start alone by position, received %zd arguments", name,
                     positional_arguments_count);
        return NULL;
    }
    ustore_py_verb_options_t options;
    ustore_py_verb_options_init(&options);
    scan_match_t match;
    memset(&match, 0, sizeof(match));
    int outputs_encoding = ustore_page_encoding_xml_k;
    int direction = ustore_scan_ascending_k;
    PyObject* end_object = NULL;
    Py_ssize_t const keyword_names_count = keyword_names_tuple ? PyTuple_GET_SIZE(keyword_names_tuple) : 0;
    for (Py_ssize_t names_progress = 0; names_progress != keyword_names_count; ++names_progress) {
        PyObject* const keyword = PyTuple_GET_ITEM(keyword_names_tuple, names_progress);
        PyObject* const value = arguments[positional_arguments_count + names_progress];
        int const consumed = ustore_py_verb_option_from_keyword(&context, keyword, value, &options);
        if (consumed < 0) return NULL;
        if (consumed > 0) continue;
        if (modality == ustore_modality_pages_k && PyUnicode_CompareWithASCIIString(keyword, "outputs_encoding") == 0) {
            if (ustore_py_enum_from_object(value, "outputs_encoding", ustore_py_enum_page_encoding_k,
                                           &outputs_encoding) != 0)
                return NULL;
            continue;
        }
        if (PyUnicode_CompareWithASCIIString(keyword, "direction") == 0) {
            if (ustore_py_enum_from_object(value, "direction", ustore_py_enum_scan_direction_k, &direction) != 0)
                return NULL;
            continue;
        }
        if (PyUnicode_CompareWithASCIIString(keyword, "end") == 0) {
            end_object = value == Py_None ? NULL : value;
            continue;
        }
        int const matched = scan_match_from_keyword(keyword, value, &match);
        if (matched < 0) return NULL;
        if (matched == 0) {
            PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", keyword);
            return NULL;
        }
    }
    if (scan_match_admitted(&match, modality) != 0) return NULL;
    if (match.weights) {
        PyErr_SetString(PyExc_TypeError, "A cursor writes no scores, so it takes no weights");
        return NULL;
    }
    ustore_key_t start = {0};
    ustore_key_t end = {0};
    if (start_from_object(positional_arguments_count == 1 ? arguments[0] : Py_None, (ustore_scan_direction_t)direction,
                          &start) != 0)
        return NULL;
    if (end_object && ustore_py_key_from_object(end_object, &end) != 0) return NULL;
    ustore_key_t const* const end_key = end_object ? &end : NULL;

    Cursor* const cursor = PyObject_New(Cursor, &CursorType);
    if (!cursor) return NULL;
    cursor->handle = NULL;
    cursor->modality = modality;
    cursor->outputs_encoding = (ustore_page_encoding_t)outputs_encoding;
    cursor->predicate.callable = Py_XNewRef(match.predicate);
    cursor->predicate.raised = NULL;
    cursor->database = context.database_handle;
    cursor->executor = (Executor*)Py_XNewRef((PyObject*)context.executor);
    // The patterns and the filter are compiled at init, so they are borrowed only while it runs
    keys_call_t gathered;
    memset(&gathered, 0, sizeof(gathered));
    ustore_patterns_t patterns = {0};
    ustore_bytes_view_t filter = {0};
    ustore_predicate_t const predicate = ustore_py_predicate_of(&cursor->predicate, match.cost);
    scan_match_t borrowed = match;
    borrowed.predicate = NULL;
    if (match_fill(&gathered, &borrowed, &patterns, &filter) != 0) {
        keys_release(&gathered);
        Py_DECREF(cursor);
        return NULL;
    }
    ustore_outcome_t outcome = {0};
    if (modality == ustore_modality_docs_k) {
        ustore_docs_cursor_init_t init = {
            .cursor = (ustore_docs_cursor_t*)&cursor->handle,
            .database = context.database_handle,
            .executor = ustore_py_context_executor(&context),
            .outcome = &outcome,
            .view = context.view,
            .collection = options.collection,
            .start_key = start,
            .end_key = end_key,
            .direction = (ustore_scan_direction_t)direction,
            .filter = filter,
            .predicate = predicate,
        };
        Py_BEGIN_ALLOW_THREADS;
        ustore_docs_cursor_init(&init);
        Py_END_ALLOW_THREADS;
    }
    else if (modality == ustore_modality_pages_k) {
        ustore_pages_cursor_init_t init = {
            .cursor = (ustore_pages_cursor_t*)&cursor->handle,
            .database = context.database_handle,
            .executor = ustore_py_context_executor(&context),
            .outcome = &outcome,
            .view = context.view,
            .collection = options.collection,
            .start_key = start,
            .end_key = end_key,
            .direction = (ustore_scan_direction_t)direction,
            .filter = filter,
            .predicate = predicate,
        };
        Py_BEGIN_ALLOW_THREADS;
        ustore_pages_cursor_init(&init);
        Py_END_ALLOW_THREADS;
    }
    else if (modality == ustore_modality_graph_k) {
        ustore_graph_vertices_cursor_init_t init = {
            .cursor = (ustore_graph_vertices_cursor_t*)&cursor->handle,
            .database = context.database_handle,
            .executor = ustore_py_context_executor(&context),
            .outcome = &outcome,
            .view = context.view,
            .collection = options.collection,
            .start_key = start,
            .end_key = end_key,
            .direction = (ustore_scan_direction_t)direction,
            .predicate = predicate,
        };
        Py_BEGIN_ALLOW_THREADS;
        ustore_graph_vertices_cursor_init(&init);
        Py_END_ALLOW_THREADS;
    }
    else {
        ustore_blobs_cursor_init_t init = {
            .cursor = (ustore_blobs_cursor_t*)&cursor->handle,
            .database = context.database_handle,
            .executor = ustore_py_context_executor(&context),
            .outcome = &outcome,
            .view = context.view,
            .collection = options.collection,
            .start_key = start,
            .end_key = end_key,
            .direction = (ustore_scan_direction_t)direction,
            .patterns = patterns,
            .predicate = predicate,
        };
        Py_BEGIN_ALLOW_THREADS;
        ustore_blobs_cursor_init(&init);
        Py_END_ALLOW_THREADS;
    }
    keys_release(&gathered);
    if (ustore_py_raise_on_outcome(&outcome) == 0) return (PyObject*)cursor;
    Py_DECREF(cursor);
    return NULL;
}

PyObject* ustore_py_api_cursor(PyObject* self, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                               PyObject* keyword_names_tuple) {
    return cursor_open(self, arguments, positional_arguments_count, keyword_names_tuple, "cursor",
                       ustore_modality_blobs_k);
}

PyObject* ustore_py_api_docs_cursor(PyObject* self, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                                    PyObject* keyword_names_tuple) {
    return cursor_open(self, arguments, positional_arguments_count, keyword_names_tuple, "docs_cursor",
                       ustore_modality_docs_k);
}

PyObject* ustore_py_api_pages_cursor(PyObject* self, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                                     PyObject* keyword_names_tuple) {
    return cursor_open(self, arguments, positional_arguments_count, keyword_names_tuple, "pages_cursor",
                       ustore_modality_pages_k);
}

PyObject* ustore_py_api_graph_vertices_cursor(PyObject* self, PyObject* const* arguments,
                                              Py_ssize_t positional_arguments_count, PyObject* keyword_names_tuple) {
    return cursor_open(self, arguments, positional_arguments_count, keyword_names_tuple, "graph_vertices_cursor",
                       ustore_modality_graph_k);
}

/** A feed of the changes committed to one collection after a stamp, pulled page by page into the
 *  arena each pull names. */
typedef struct ChangesCursor {

    /** The object header every instance carries. */
    PyObject ob_base;

    /** The feed, NULL once closed, or the address of @c cursor_lent while a pull holds it. */
    void* handle;
} ChangesCursor;

/** One pull of a feed, whose outputs land in the arena its header holds. */
typedef struct changes_pull_call_t {

    /** What the call keeps until it finishes. */
    ustore_py_call_t call;

    /** Output slot for the changes the pull yielded. */
    ustore_size_t count;

    /** Output slot for the key of every change, inside the arena. */
    ustore_key_t const* keys;

    /** Output slot for the stamp of every change, inside the arena. */
    ustore_stamp_t const* stamps;

    /** Output slot for the kind of every change, inside the arena. */
    ustore_change_kind_t const* kinds;

    /** Output slot for the tape of each change's bytes in the arena, when the pull asks for it. */
    ustore_strings_view_t values;
} changes_pull_call_t;

static void changes_pull_submit(void* arguments) {
    ustore_changes_cursor_next((ustore_changes_cursor_next_t*)arguments);
}

/** The @c Changes of the pull @p call, carrying @p values, a @c Values or None, which it takes. */
static PyObject* changes_answer(changes_pull_call_t const* call, PyObject* values) {
    size_t const count = (size_t)call->count;
    PyObject* const changes = PyStructSequence_New(ChangesType);
    PyObject* const keys = page_new(call->call.arena, call->keys, count, Py_NewRef(Py_None));
    PyObject* const stamps = PyTuple_New((Py_ssize_t)count);
    PyObject* const kinds = PyTuple_New((Py_ssize_t)count);
    int failed = !changes || !keys || !stamps || !kinds;
    for (size_t index = 0; !failed && index != count; ++index) {
        PyObject* const stamp = PyLong_FromUnsignedLongLong(call->stamps[index]);
        PyObject* const kind = ustore_py_enum_to_object(ustore_py_enum_change_kind_k, (int)call->kinds[index]);
        if (stamp) PyTuple_SET_ITEM(stamps, (Py_ssize_t)index, stamp);
        if (kind) PyTuple_SET_ITEM(kinds, (Py_ssize_t)index, kind);
        failed = !stamp || !kind;
    }
    if (failed) {
        Py_XDECREF(changes);
        Py_XDECREF(keys);
        Py_XDECREF(values);
        Py_XDECREF(stamps);
        Py_XDECREF(kinds);
        return NULL;
    }
    PyStructSequence_SetItem(changes, 0, keys);
    PyStructSequence_SetItem(changes, 1, values);
    PyStructSequence_SetItem(changes, 2, stamps);
    PyStructSequence_SetItem(changes, 3, kinds);
    return changes;
}

static PyObject* changes_answer_keys(void* erased) {
    return changes_answer((changes_pull_call_t const*)erased, Py_NewRef(Py_None));
}

static PyObject* changes_answer_pairs(void* erased) {
    changes_pull_call_t const* const call = (changes_pull_call_t const*)erased;
    PyObject* const values = ustore_py_values_new(call->call.arena, NULL, &call->values, (size_t)call->count);
    return values ? changes_answer(call, values) : NULL;
}

static void changes_pull_release(void* call) { ustore_py_call_release(&((changes_pull_call_t*)call)->call); }

static ustore_py_verb_t const changes_pull_keys_verb = {sizeof(changes_pull_call_t), changes_pull_submit,
                                                        changes_answer_keys, changes_pull_release};
static ustore_py_verb_t const changes_pull_pairs_verb = {sizeof(changes_pull_call_t), changes_pull_submit,
                                                         changes_answer_pairs, changes_pull_release};

/** Frees the feed of @p self once, fenced against a concurrent close and refused during a pull. */
static int changes_cursor_close(ChangesCursor* self) {
    void* const lent = (void*)&cursor_lent;
    void* handle = NULL;
    ustore_py_critical_section_begin_((PyObject*)self);
    handle = self->handle;
    if (handle != lent) self->handle = NULL;
    ustore_py_critical_section_end_();
    if (handle == lent) return ustore_py_raise(ustore_status_contended_k, "A pull of this feed is still running");
    if (!handle) return 0;
    Py_BEGIN_ALLOW_THREADS;
    ustore_changes_cursor_free((ustore_changes_cursor_t)handle);
    Py_END_ALLOW_THREADS;
    return 0;
}

static void ChangesCursor_dealloc(ChangesCursor* self) {
    changes_cursor_close(self);
    PyObject_Free(self);
}

static char const doc_ChangesCursor[] =                                                           //
    "A feed of the changes committed to one collection after a stamp, oldest first.\n"            //
    "\n"                                                                                          //
    "Built through `changes_cursor()` on a database, never by name. It pins the stamp it reads\n" //
    "from, as a snapshot does, and every pull resumes past the last change the pull before it\n"  //
    "handed out. One pull runs at a time, and an overlapping pull or `close()` raises\n"          //
    "ContendedError. Used as a context manager, it closes on exit.";

static char const doc_ChangesCursor_next[] =                                                   //
    "next(wanted, arena, /, *, values=False) answers a Changes tuple.\n"                       //
    "\n"                                                                                       //
    "Pulls up to `wanted` changes past the last pull into `arena`, with the bytes every one\n" //
    "carries in the same call when asked, on the calling thread. An empty pull, whose keys\n"  //
    "are falsy, reached the newest commit, and later pulls see later commits. A pull that\n"   //
    "fails leaves the feed where it was.\n"                                                    //
    "\n"                                                                                       //
    "Args:\n"                                                                                  //
    "  wanted (int): Most changes the pull yields.\n"                                          //
    "  arena (Arena): Where the keys and values land, rewound by this pull.\n"                 //
    "  values (bool, optional): Whether the pull carries the bytes of every change, empty\n"   //
    "    for a removal.\n"                                                                     //
    "Returns:\n"                                                                               //
    "  Changes: The keys as a Page, their Values or None, and a tuple each of commit stamps\n" //
    "  and ChangeKind members.\n"                                                              //
    "Raises:\n"                                                                                //
    "  UninitializedError: The feed is closed.";

static PyObject* ChangesCursor_next(ChangesCursor* self, PyObject* const* arguments,
                                    Py_ssize_t positional_arguments_count, PyObject* keyword_names_tuple) {
    Py_ssize_t const keyword_names_count = ustore_py_keyword_names_count_checked("next", positional_arguments_count,
                                                                                 keyword_names_tuple, 2);
    if (keyword_names_count < 0) return NULL;
    int values = 0;
    for (Py_ssize_t names_progress = 0; names_progress != keyword_names_count; ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(keyword_names_tuple, names_progress);
        if (PyUnicode_CompareWithASCIIString(name, "values") != 0) {
            PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
            return NULL;
        }
        values = PyObject_IsTrue(arguments[positional_arguments_count + names_progress]);
        if (values < 0) return NULL;
    }
    size_t wanted = 0;
    if (ustore_py_size_from_object(arguments[0], "wanted", &wanted) != 0) return NULL;

    void* const lent = (void*)&cursor_lent;
    void* handle = NULL;
    ustore_py_critical_section_begin_((PyObject*)self);
    handle = self->handle;
    if (handle) self->handle = lent;
    ustore_py_critical_section_end_();
    if (!handle || handle == lent) {
        ustore_py_raise(handle ? ustore_status_contended_k : ustore_status_uninitialized_k,
                        handle ? "Another pull of this feed is running" : "The feed is closed");
        return NULL;
    }
    ustore_py_verb_t const* const verb = values ? &changes_pull_pairs_verb : &changes_pull_keys_verb;
    ustore_py_context_t const context = {.completion = ustore_completion_blocking_k};
    changes_pull_call_t stack;
    changes_pull_call_t* const call = ustore_py_call_open(&context, verb, &stack);
    PyObject* changes = NULL;
    if (ustore_py_arena_claim(arguments[1], &call->call.arena) != 0)
        changes = ustore_py_call_discard(&context, verb, call);
    else {
        ustore_changes_cursor_next_t next = {
            .cursor = (ustore_changes_cursor_t)handle,
            .outcome = &call->call.outcome,
            .wanted = wanted,
            .count = &call->count,
            .keys = &call->keys,
            .stamps = &call->stamps,
            .kinds = &call->kinds,
            .values = values ? &call->values : NULL,
            .arena = ustore_py_arena_handle(call->call.arena),
        };
        changes = ustore_py_call_run(&context, verb, call, &next);
    }
    ustore_py_critical_section_begin_((PyObject*)self);
    self->handle = handle;
    ustore_py_critical_section_end_();
    return changes;
}

static char const doc_ChangesCursor_close[] =                                               //
    "close() answers None.\n"                                                               //
    "\n"                                                                                    //
    "Frees the feed, releasing the stamp it pinned and the hold it kept on its database.\n" //
    "Closing twice is safe.";

static PyObject* ChangesCursor_close(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    if (changes_cursor_close((ChangesCursor*)self) != 0) return NULL;
    Py_RETURN_NONE;
}

static PyObject* ChangesCursor_exit(PyObject* self, PyObject* const* Py_UNUSED(arguments),
                                    Py_ssize_t Py_UNUSED(positional_arguments_count),
                                    PyObject* Py_UNUSED(keyword_names_tuple)) {
    return ChangesCursor_close(self, NULL);
}

static PyMethodDef ChangesCursor_methods[] = {
    {"next", (PyCFunction)(void (*)(void))ChangesCursor_next, ustore_py_method_flags_k, doc_ChangesCursor_next},
    {"close", (PyCFunction)(void (*)(void))ChangesCursor_close, METH_NOARGS, doc_ChangesCursor_close},
    {"__enter__", (PyCFunction)(void (*)(void))Cursor_enter, METH_NOARGS, NULL},
    {"__exit__", (PyCFunction)(void (*)(void))ChangesCursor_exit, ustore_py_method_flags_k, NULL},
    {NULL, NULL, 0, NULL},
};

PyTypeObject ChangesCursorType = {
    PyVarObject_HEAD_INIT(NULL, 0) //
        .tp_name = "ustore.ChangesCursor",
    .tp_doc = doc_ChangesCursor,
    .tp_basicsize = sizeof(ChangesCursor),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = NULL,
    .tp_dealloc = (destructor)ChangesCursor_dealloc,
    .tp_methods = ChangesCursor_methods,
};

char const doc_changes_cursor[] =                                                               //
    "changes_cursor(since=0, /, *, collection) answers a ChangesCursor.\n"                      //
    "\n"                                                                                        //
    "Opens a feed of the changes committed to one collection after the commit stamp `since`,\n" //
    "oldest first, reading nothing until its first pull.\n"                                     //
    "\n"                                                                                        //
    "Args:\n"                                                                                   //
    "  since (int, optional): The commit stamp after which changes are yielded, as `commit`\n"  //
    "    answers it.\n"                                                                         //
    "  collection (UUID | str): The collection, by identifier or by name.\n"                    //
    "Returns:\n"                                                                                //
    "  ChangesCursor: The feed, pulled page by page.\n"                                         //
    "Raises:\n"                                                                                 //
    "  WriteConflictError: The history after `since` is already gone.\n"                        //
    "  UnsupportedError: The engine keeps no stamps, as LevelDB does.";

PyObject* ustore_py_api_changes_cursor(PyObject* self, PyObject* const* arguments,
                                       Py_ssize_t positional_arguments_count, PyObject* keyword_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (positional_arguments_count > 1) {
        PyErr_Format(PyExc_TypeError, "changes_cursor() takes the stamp alone by position, received %zd arguments",
                     positional_arguments_count);
        return NULL;
    }
    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, arguments, positional_arguments_count, keyword_names_tuple,
                                             &options) != 0)
        return NULL;
    size_t since = 0;
    if (positional_arguments_count == 1 && ustore_py_size_from_object(arguments[0], "since", &since) != 0) return NULL;

    ChangesCursor* const cursor = PyObject_New(ChangesCursor, &ChangesCursorType);
    if (!cursor) return NULL;
    cursor->handle = NULL;
    ustore_outcome_t outcome = {0};
    ustore_changes_cursor_init_t init = {
        .database = context.database_handle,
        .outcome = &outcome,
        .collection = options.collection,
        .since_stamp = (ustore_stamp_t)since,
        .cursor = (ustore_changes_cursor_t*)&cursor->handle,
    };
    Py_BEGIN_ALLOW_THREADS;
    ustore_changes_cursor_init(&init);
    Py_END_ALLOW_THREADS;
    if (ustore_py_raise_on_outcome(&outcome) == 0) return (PyObject*)cursor;
    Py_DECREF(cursor);
    return NULL;
}

#pragma endregion Cursors
