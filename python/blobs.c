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
    "read(keys, arena, *, collection=None) answers a Values tape.\n"                        //
    "\n"                                                                                    //
    "Reads a batch of binary values in one call, which no sequence of single reads\n"       //
    "matches for throughput. Nothing is copied: the tape sits in the arena, and indexing\n" //
    "the answer gives a memoryview over the stored bytes, or None for an absent key.\n"     //
    "\n"                                                                                    //
    "Args:\n"                                                                               //
    "  keys (Sequence[KeyLike] | buffer): The keys to read, as a sequence, a contiguous\n"  //
    "    buffer of 16-byte keys, which is read in place, or one of 64-bit integers.\n"      //
    "  arena (Arena): Where the tape lands, rewound by this call.\n"                        //
    "  collection (UUID | str, optional): The collection, by identifier or by name;\n"      //
    "    None addresses the main one.\n"                                                    //
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
static PyObject* read_answer_drawn(void* erased) {
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

ustore_py_verb_t const ustore_py_read_drawn_verb = {sizeof(ustore_py_read_call_t), read_submit, read_answer_drawn,
                                                    ustore_py_read_release};

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
        .presences = &call->presences,
        .values = &call->values,
        .arena = ustore_py_arena_handle(call->call.arena),
    };
    return ustore_py_call_run(context, verb, call, &read);
}

PyObject* ustore_py_api_read(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                             PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_args_names_count_checked("read", positional_args_count, args_names_tuple, 2) < 0) return NULL;

    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, args, positional_args_count, args_names_tuple, &options) != 0)
        return NULL;

    ustore_py_read_call_t stack;
    ustore_py_read_call_t* const call = ustore_py_call_open(&context, &ustore_py_read_verb, &stack);
    if (!call) return NULL;
    if (ustore_py_keys_from_object(args[0], &call->keys) != 0)
        return ustore_py_call_discard(&context, &ustore_py_read_verb, call);
    return ustore_py_values_read(&context, &options, &ustore_py_read_verb, call, args[1]);
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
    "get(key, *, collection=None) answers bytes or None.\n"                                    //
    "\n"                                                                                       //
    "Reads one value into a one-off arena and copies it into a bytes object, so nothing of\n"  //
    "the arena outlives the call. That costs an arena per call, so reach for `read` with an\n" //
    "arena of your own when several keys are wanted, or in a loop.\n"                          //
    "\n"                                                                                       //
    "Args:\n"                                                                                  //
    "  key (KeyLike): The key to read.\n"                                                      //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n"         //
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

PyObject* ustore_py_api_get(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                            PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_args_names_count_checked("get", positional_args_count, args_names_tuple, 1) < 0) return NULL;

    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, args, positional_args_count, args_names_tuple, &options) != 0)
        return NULL;

    ustore_key_t key = {0};
    if (ustore_py_key_from_object(args[0], &key) != 0) return NULL;
    ustore_py_read_call_t stack;
    ustore_py_read_call_t* const call = one_key_open(&context, &get_verb, &key, &stack);
    return call ? values_read_once(&context, &options, &get_verb, call) : NULL;
}

char const doc_contains[] =                                                              //
    "contains(key, *, collection=None) answers whether the key is present.\n"            //
    "\n"                                                                                 //
    "Reads the presence alone into a one-off arena, so no value crosses the boundary.\n" //
    "\n"                                                                                 //
    "Args:\n"                                                                            //
    "  key (KeyLike): The key to look for.\n"                                            //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n"   //
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

PyObject* ustore_py_api_contains(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                 PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_args_names_count_checked("contains", positional_args_count, args_names_tuple, 1) < 0) return NULL;

    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, args, positional_args_count, args_names_tuple, &options) != 0)
        return NULL;

    ustore_key_t key = {0};
    if (ustore_py_key_from_object(args[0], &key) != 0) return NULL;
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
    ustore_py_call_release(&call->call);
}

ustore_py_verb_t const ustore_py_remove_verb = {sizeof(ustore_py_remove_call_t), remove_submit,
                                                ustore_py_call_answer_none, remove_release};

PyObject* ustore_py_values_remove(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                                  ustore_py_remove_call_t* call) {
    call->call.collection = options->collection;
    ustore_blobs_remove_t remove = {
        .database = context->database_handle,
        .executor = ustore_py_context_executor(context),
        .outcome = &call->call.outcome,
        .completion = context->completion,
        .transaction = context->transaction,
        .count = call->keys.count,
        .collections = {&call->call.collection, 0},
        .keys = {call->keys.keys, sizeof(ustore_key_t)},
        .durability = options->durability,
        .watch = context->watch,
    };
    return ustore_py_call_run(context, &ustore_py_remove_verb, call, &remove);
}

char const doc_write[] =                                                                      //
    "write(keys, values, *, collection=None, durability=Durability.BUFFERED) answers None.\n" //
    "\n"                                                                                      //
    "Upserts a batch of binary values in one call. Every value is borrowed for the\n"         //
    "length of the call rather than copied, so a memory-mapped buffer reaches the engine\n"   //
    "untouched.\n"                                                                            //
    "\n"                                                                                      //
    "Args:\n"                                                                                 //
    "  keys (Sequence[KeyLike] | buffer): The keys to write.\n"                               //
    "  values (Sequence[bytes-like] | bytes-like | tape): One value per key, a tape of\n"     //
    "    them, or a single bytes-like object broadcast to every key. A tape is an Arrow\n"    //
    "    `binary`, `large_binary`, `string` or `large_string` array, such as `Values`, or\n"  //
    "    a `(data, offsets)` tuple of buffers with unsigned 32-bit or 64-bit offsets, and\n"  //
    "    passes without a copy. None is refused, since `remove` removes keys.\n"              //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n"        //
    "  durability (Durability, optional): `Durability.FLUSHED` reaches the drive before\n"    //
    "    returning.\n"                                                                        //
    "Raises:\n"                                                                               //
    "  TypeError: A value is None or exports no buffer.\n"                                    //
    "  Error: Whatever the engine reported.";

PyObject* ustore_py_api_write(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                              PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0 || ustore_py_context_writable(&context) != 0) return NULL;
    if (ustore_py_args_names_count_checked("write", positional_args_count, args_names_tuple, 2) < 0) return NULL;

    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, args, positional_args_count, args_names_tuple, &options) != 0)
        return NULL;

    ustore_py_write_call_t stack;
    ustore_py_write_call_t* const call = ustore_py_call_open(&context, &ustore_py_write_verb, &stack);
    if (!call) return NULL;
    if (ustore_py_keys_from_object(args[0], &call->keys) != 0 ||
        ustore_py_contents_from_object(args[1], call->keys.count, &call->contents) != 0)
        return ustore_py_call_discard(&context, &ustore_py_write_verb, call);
    return ustore_py_values_write(&context, &options, call);
}

char const doc_set[] =                                                                     //
    "set(key, value, *, collection=None, durability=Durability.BUFFERED) answers None.\n"  //
    "\n"                                                                                   //
    "Upserts one binary value.\n"                                                          //
    "\n"                                                                                   //
    "Args:\n"                                                                              //
    "  key (KeyLike): The key to write.\n"                                                 //
    "  value (bytes-like): The bytes to store, borrowed rather than copied.\n"             //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n"     //
    "  durability (Durability, optional): `Durability.FLUSHED` reaches the drive before\n" //
    "    returning.\n"                                                                     //
    "Raises:\n"                                                                            //
    "  TypeError: The value is None or exports no buffer.";

PyObject* ustore_py_api_set(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                            PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0 || ustore_py_context_writable(&context) != 0) return NULL;
    if (ustore_py_args_names_count_checked("set", positional_args_count, args_names_tuple, 2) < 0) return NULL;

    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, args, positional_args_count, args_names_tuple, &options) != 0)
        return NULL;

    ustore_py_write_call_t stack;
    ustore_py_write_call_t* const call = ustore_py_call_open(&context, &ustore_py_write_verb, &stack);
    if (!call) return NULL;
    call->keys.keys = call->keys.inline_keys;
    call->keys.count = 1;
    if (ustore_py_key_from_object(args[0], &call->keys.inline_keys[0]) != 0 ||
        ustore_py_contents_reserve(&call->contents, 1, ustore_py_contents_per_task_k) != 0 ||
        ustore_py_contents_borrow(args[1], &call->contents) != 0)
        return ustore_py_call_discard(&context, &ustore_py_write_verb, call);
    return ustore_py_values_write(&context, &options, call);
}

char const doc_remove[] =                                                                    //
    "remove(keys, *, collection=None, durability=Durability.BUFFERED) answers None.\n"       //
    "\n"                                                                                     //
    "Removes a batch of keys with their values or documents, or one key given on its own.\n" //
    "\n"                                                                                     //
    "Args:\n"                                                                                //
    "  keys (KeyLike | Sequence[KeyLike] | buffer): The keys to remove.\n"                   //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n"       //
    "  durability (Durability, optional): `Durability.FLUSHED` reaches the drive before\n"   //
    "    returning.";

PyObject* ustore_py_api_remove(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                               PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0 || ustore_py_context_writable(&context) != 0) return NULL;
    if (ustore_py_args_names_count_checked("remove", positional_args_count, args_names_tuple, 1) < 0) return NULL;

    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, args, positional_args_count, args_names_tuple, &options) != 0)
        return NULL;

    ustore_py_remove_call_t stack;
    ustore_py_remove_call_t* const call = ustore_py_call_open(&context, &ustore_py_remove_verb, &stack);
    if (!call) return NULL;
    if (ustore_py_keys_from_subscript(args[0], &call->keys) < 0)
        return ustore_py_call_discard(&context, &ustore_py_remove_verb, call);
    return ustore_py_values_remove(&context, &options, call);
}

#pragma endregion Writes

#pragma region Scans

/** Walks the keywords of a scan, which takes the order its keys come in and a progress callable beside the shared
 *  ones. */
static int scan_options_from_keywords(ustore_py_context_t const* context, PyObject* const* args,
                                      Py_ssize_t positional_args_count, PyObject* args_names_tuple,
                                      ustore_py_verb_options_t* options, ustore_scan_mode_t* mode,
                                      PyObject** progress) {
    ustore_py_verb_options_init(options);
    *progress = NULL;
    int chosen = ustore_scan_ordered_k;
    Py_ssize_t const args_names_count = args_names_tuple ? PyTuple_GET_SIZE(args_names_tuple) : 0;
    for (Py_ssize_t names_progress = 0, args_progress = positional_args_count; names_progress != args_names_count;
         ++args_progress, ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(args_names_tuple, names_progress);
        int const consumed = ustore_py_verb_option_from_keyword(context, name, args[args_progress], options);
        if (consumed < 0) return -1;
        if (consumed > 0) continue;
        int read = -1;
        if (PyUnicode_CompareWithASCIIString(name, "mode") == 0)
            read = ustore_py_enum_from_object(args[args_progress], "mode", ustore_py_enum_scan_mode_k, &chosen);
        else if (PyUnicode_CompareWithASCIIString(name, "progress") == 0)
            read = ustore_py_progress_from_object(args[args_progress], progress);
        else PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
        if (read != 0) return -1;
    }
    *mode = (ustore_scan_mode_t)chosen;
    return 0;
}

/** A batch of scans or samples, whose keys land in the arena its header holds. */
typedef struct keys_call_t {

    /** What the call keeps until it finishes. */
    ustore_py_call_t call;

    /** The smallest key every scan may return, empty for a sample. */
    ustore_py_keys_t starts;

    /** Scans or samples the batch carries. */
    size_t tasks;

    /** Output slot for the offsets into @c keys, inside the arena. */
    ustore_length_t const* offsets;

    /** Output slot for the keys every task found, inside the arena. */
    ustore_key_t const* keys;
} keys_call_t;

static PyObject* keys_answer(void* erased) {
    keys_call_t const* const call = (keys_call_t const*)erased;
    return ustore_py_keys_new(call->call.arena, call->offsets, call->keys, call->tasks);
}

static void keys_release(void* erased) {
    keys_call_t* const call = (keys_call_t*)erased;
    ustore_py_keys_release(&call->starts);
    ustore_py_call_release(&call->call);
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

static ustore_py_verb_t const scan_verb = {sizeof(keys_call_t), scan_submit, keys_answer, keys_release};

static ustore_py_verb_t const sample_verb = {sizeof(keys_call_t), sample_submit, keys_answer, keys_release};

/** Runs the scans gathered into @p call, reporting to @p progress and claiming @p arena for their keys. */
static PyObject* keys_scan(ustore_py_context_t const* context, ustore_py_verb_options_t const* options, size_t wanted,
                           ustore_scan_mode_t mode, PyObject* progress, keys_call_t* call, PyObject* arena) {
    if (ustore_py_arena_claim(arena, &call->call.arena) != 0) return ustore_py_call_discard(context, &scan_verb, call);
    call->call.collection = options->collection;
    call->call.progress = Py_XNewRef(progress);
    call->tasks = call->starts.count;
    ustore_blobs_scan_t scan = {
        .database = context->database_handle,
        .executor = ustore_py_context_executor(context),
        .outcome = &call->call.outcome,
        .completion = context->completion,
        .view = context->view,
        .count = call->tasks,
        .collections = {&call->call.collection, 0},
        .start_keys = {call->starts.keys, sizeof(ustore_key_t)},
        .offsets = &call->offsets,
        .keys = &call->keys,
        .arena = ustore_py_arena_handle(call->call.arena),
        .wanted = wanted,
        .mode = mode,
        .progress = ustore_py_progress_callback(&call->call),
        .progress_state = &call->call,
    };
    return ustore_py_call_run(context, &scan_verb, call, &scan);
}

char const doc_scan[] =                                                                      //
    "scan(start, wanted, arena, *, collection=None, mode=ScanMode.ORDERED, progress=None)\n" //
    "answers a Keys tape.\n"                                                                 //
    "\n"                                                                                     //
    "Lists up to `wanted` keys from `start` upwards. Values are not exported, so a read\n"   //
    "follows the scan, or a `cursor()` walks a collection with its values instead.\n"        //
    "\n"                                                                                     //
    "Args:\n"                                                                                //
    "  start (KeyLike): The smallest key the scan may return.\n"                             //
    "  wanted (int): Most keys the scan returns.\n"                                          //
    "  arena (Arena): Where the keys land, rewound by this call.\n"                          //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n"       //
    "  mode (ScanMode, optional): `ScanMode.UNORDERED` lets keys come out of order,\n"       //
    "    removed or repeated, to speed up the read after the scan; no engine uses it yet.\n" //
    "  progress (Callable[[int, int], bool | None], optional): Called with the scans\n"      //
    "    finished and the scans in all; a falsy answer other than None cancels the call.\n"  //
    "Returns:\n"                                                                             //
    "  Keys: One run of keys, in ascending order.\n"                                         //
    "Raises:\n"                                                                              //
    "  CancelledError: The progress callable stopped the scan.";

PyObject* ustore_py_keys_scan(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                              ustore_key_t const* start, size_t wanted, ustore_scan_mode_t mode, PyObject* progress,
                              PyObject* arena) {
    keys_call_t stack;
    keys_call_t* const call = ustore_py_call_open(context, &scan_verb, &stack);
    if (!call) return NULL;
    call->starts.inline_keys[0] = *start;
    call->starts.keys = call->starts.inline_keys;
    call->starts.count = 1;
    return keys_scan(context, options, wanted, mode, progress, call, arena);
}

PyObject* ustore_py_api_scan(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                             PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_args_names_count_checked("scan", positional_args_count, args_names_tuple, 3) < 0) return NULL;

    ustore_py_verb_options_t options;
    ustore_scan_mode_t mode = ustore_scan_ordered_k;
    PyObject* progress = NULL;
    if (scan_options_from_keywords(&context, args, positional_args_count, args_names_tuple, &options, &mode,
                                   &progress) != 0)
        return NULL;

    ustore_key_t start = {0};
    size_t wanted = 0;
    if (ustore_py_key_from_object(args[0], &start) != 0) return NULL;
    if (ustore_py_size_from_object(args[1], "wanted", &wanted) != 0) return NULL;
    return ustore_py_keys_scan(&context, &options, &start, wanted, mode, progress, args[2]);
}

char const doc_scan_batch[] =                                                                       //
    "scan_batch(starts, wanted, arena, *, collection=None, mode=ScanMode.ORDERED, progress=None)\n" //
    "answers a Keys tape.\n"                                                                        //
    "\n"                                                                                            //
    "Runs many scans in one call, which is what the ABI takes and what an engine can\n"             //
    "spread across its threads; indexing the answer by start gives the run that scan\n"             //
    "found.\n"                                                                                      //
    "\n"                                                                                            //
    "Args:\n"                                                                                       //
    "  starts (Sequence[KeyLike] | buffer): The smallest key every scan may return.\n"              //
    "  wanted (int): Most keys every scan returns.\n"                                               //
    "  arena (Arena): Where the keys land, rewound by this call.\n"                                 //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n"              //
    "  mode (ScanMode, optional): In what order every scan hands its keys over, as\n"               //
    "    `scan` takes it.\n"                                                                        //
    "  progress (Callable[[int, int], bool | None], optional): Called with the scans\n"             //
    "    finished and the scans in all, from whichever thread finished the last one, never\n"       //
    "    two at once; a falsy answer other than None cancels the call.\n"                           //
    "Returns:\n"                                                                                    //
    "  Keys: One run of ascending keys per start, in the order they were asked for.\n"              //
    "Raises:\n"                                                                                     //
    "  CancelledError: The progress callable stopped the scans.";

PyObject* ustore_py_api_scan_batch(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                   PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_args_names_count_checked("scan_batch", positional_args_count, args_names_tuple, 3) < 0) return NULL;

    ustore_py_verb_options_t options;
    ustore_scan_mode_t mode = ustore_scan_ordered_k;
    PyObject* progress = NULL;
    if (scan_options_from_keywords(&context, args, positional_args_count, args_names_tuple, &options, &mode,
                                   &progress) != 0)
        return NULL;

    size_t wanted = 0;
    if (ustore_py_size_from_object(args[1], "wanted", &wanted) != 0) return NULL;
    keys_call_t stack;
    keys_call_t* const call = ustore_py_call_open(&context, &scan_verb, &stack);
    if (!call) return NULL;
    if (ustore_py_keys_from_object(args[0], &call->starts) != 0)
        return ustore_py_call_discard(&context, &scan_verb, call);
    return keys_scan(&context, &options, wanted, mode, progress, call, args[2]);
}

/** Runs @p tasks samples of up to @p wanted keys each, claiming @p arena for their keys. */
static PyObject* keys_sample(ustore_py_context_t const* context, ustore_py_verb_options_t const* options, size_t tasks,
                             size_t wanted, PyObject* arena) {
    keys_call_t stack;
    keys_call_t* const call = ustore_py_call_open(context, &sample_verb, &stack);
    if (!call) return NULL;
    if (ustore_py_arena_claim(arena, &call->call.arena) != 0)
        return ustore_py_call_discard(context, &sample_verb, call);
    call->call.collection = options->collection;
    call->tasks = tasks;
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
    "sample(wanted, arena, *, collection=None) answers a Keys tape.\n"                    //
    "\n"                                                                                  //
    "Draws keys uniformly at random from one collection, which is how a workload picks\n" //
    "entries it has no list of.\n"                                                        //
    "\n"                                                                                  //
    "Args:\n"                                                                             //
    "  wanted (int): Most keys to draw.\n"                                                //
    "  arena (Arena): Where the keys land, rewound by this call.\n"                       //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n"    //
    "Returns:\n"                                                                          //
    "  Keys: One run of keys, in no particular order.";

PyObject* ustore_py_keys_sample(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                                size_t wanted, PyObject* arena) {
    return keys_sample(context, options, 1, wanted, arena);
}

PyObject* ustore_py_api_sample(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                               PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_args_names_count_checked("sample", positional_args_count, args_names_tuple, 2) < 0) return NULL;

    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, args, positional_args_count, args_names_tuple, &options) != 0)
        return NULL;

    size_t wanted = 0;
    if (ustore_py_size_from_object(args[0], "wanted", &wanted) != 0) return NULL;
    return keys_sample(&context, &options, 1, wanted, args[1]);
}

char const doc_sample_batch[] =                                                               //
    "sample_batch(count, wanted, arena, *, collection=None) answers a Keys tape.\n"           //
    "\n"                                                                                      //
    "Draws `count` independent samples in one call, all from the one collection named,\n"     //
    "which is how a workload builds several random batches without paying for a call each.\n" //
    "\n"                                                                                      //
    "Args:\n"                                                                                 //
    "  count (int): Samples to draw.\n"                                                       //
    "  wanted (int): Most keys every sample draws.\n"                                         //
    "  arena (Arena): Where the keys land, rewound by this call.\n"                           //
    "  collection (UUID | str, optional): The one collection every sample draws from, by\n"   //
    "    identifier or by name.\n"                                                            //
    "Returns:\n"                                                                              //
    "  Keys: One run of keys per sample, in no particular order.";

PyObject* ustore_py_api_sample_batch(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                     PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_args_names_count_checked("sample_batch", positional_args_count, args_names_tuple, 3) < 0) return NULL;

    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, args, positional_args_count, args_names_tuple, &options) != 0)
        return NULL;

    size_t tasks = 0;
    size_t wanted = 0;
    if (ustore_py_size_from_object(args[0], "count", &tasks) != 0) return NULL;
    if (ustore_py_size_from_object(args[1], "wanted", &wanted) != 0) return NULL;
    return keys_sample(&context, &options, tasks, wanted, args[2]);
}

char const doc_measure[] =                                                                   //
    "measure(start, end, arena, *, collection=None) answers a Measurement.\n"                //
    "\n"                                                                                     //
    "Estimates what a key range holds without walking it, so the answers are bounds\n"       //
    "rather than counts.\n"                                                                  //
    "\n"                                                                                     //
    "Args:\n"                                                                                //
    "  start (KeyLike): First key of the range.\n"                                           //
    "  end (KeyLike): Last key of the range.\n"                                              //
    "  arena (Arena): Where the bounds land before they are copied out, rewound by this\n"   //
    "    call.\n"                                                                            //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n"       //
    "Returns:\n"                                                                             //
    "  Measurement: The lower and upper bounds on the entries, the bytes of their values,\n" //
    "  and the space they take on disk.";

/** A measurement of one range, whose bounds land in the arena its header holds. */
typedef struct measure_call_t {

    /** What the call keeps until it finishes. */
    ustore_py_call_t call;

    /** First key of the range. */
    ustore_key_t start;

    /** Last key of the range. */
    ustore_key_t end;

    /** Output slots for the six bounds, in the order of the fields of @c MeasurementType, inside the arena. */
    ustore_size_t const* bounds[6];
} measure_call_t;

static void measure_submit(void* arguments) { ustore_blobs_measure((ustore_blobs_measure_t*)arguments); }

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

PyObject* ustore_py_api_measure(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_args_names_count_checked("measure", positional_args_count, args_names_tuple, 3) < 0) return NULL;

    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, args, positional_args_count, args_names_tuple, &options) != 0)
        return NULL;

    ustore_key_t start = {0};
    ustore_key_t end = {0};
    if (ustore_py_key_from_object(args[0], &start) != 0) return NULL;
    if (ustore_py_key_from_object(args[1], &end) != 0) return NULL;

    measure_call_t stack;
    measure_call_t* const call = ustore_py_call_open(&context, &measure_verb, &stack);
    if (!call) return NULL;
    if (ustore_py_arena_claim(args[2], &call->call.arena) != 0)
        return ustore_py_call_discard(&context, &measure_verb, call);
    call->call.collection = options.collection;
    call->start = start;
    call->end = end;
    ustore_blobs_measure_t measure = {
        .database = context.database_handle,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->call.outcome,
        .completion = context.completion,
        .view = context.view,
        .count = 1,
        .collections = {&call->call.collection, 0},
        .start_keys = {&call->start, 0},
        .end_keys = {&call->end, 0},
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

#pragma endregion Scans

#pragma region Cursors

/** A walk over the keys of one collection, pulled page by page into the arena each pull names. */
typedef struct Cursor {

    /** The object header every instance carries. */
    PyObject ob_base;

    /** The cursor, NULL once closed, or the address of @c cursor_lent while a pull holds it. */
    ustore_blobs_cursor_t handle;

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

/** Stands in the handle slot of a cursor while one pull holds its handle, refusing another pull or a close. */
static char cursor_lent;

/**
 *  @brief Frees the cursor of @p self once, fenced against a concurrent close and refused during a pull.
 *  @return 0 on success, -1 with a @c ContendedError set while a pull holds the handle.
 */
static int cursor_close(Cursor* self) {
    ustore_blobs_cursor_t const lent = (ustore_blobs_cursor_t)(void*)&cursor_lent;
    ustore_blobs_cursor_t handle = NULL;
    ustore_py_critical_section_begin_((PyObject*)self);
    handle = self->handle;
    if (handle != lent) self->handle = NULL;
    ustore_py_critical_section_end_();
    if (handle == lent) return ustore_py_raise(ustore_status_contended_k, "A pull of this cursor is still running");
    if (!handle) return 0;
    Py_BEGIN_ALLOW_THREADS;
    ustore_blobs_cursor_free(handle);
    Py_END_ALLOW_THREADS;
    return 0;
}

static void Cursor_dealloc(Cursor* self) {
    // No pull can hold the handle, since every pull holds the cursor.
    cursor_close(self);
    Py_XDECREF((PyObject*)self->executor);
    PyObject_Free(self);
}

/** Pulls the next page of the cursor @p handle, which @p self lent its pull, as @p verb answers it. */
static PyObject* cursor_pull(Cursor* self, ustore_blobs_cursor_t handle, size_t wanted, PyObject* arena,
                             ustore_py_verb_t const* verb, PyObject* progress) {
    if (self->executor && !self->executor->handle) {
        ustore_py_raise(ustore_status_uninitialized_k, "The executor is closed");
        return NULL;
    }
    ustore_py_context_t const context = {.executor = self->executor, .completion = ustore_completion_blocking_k};
    pull_call_t stack;
    pull_call_t* const call = ustore_py_call_open(&context, verb, &stack);
    if (ustore_py_arena_claim(arena, &call->call.arena) != 0) return ustore_py_call_discard(&context, verb, call);
    call->call.progress = Py_XNewRef(progress);
    ustore_blobs_cursor_next_t next = {
        .database = self->database,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->call.outcome,
        .cursor = handle,
        .keys = &call->keys,
        .values = verb == &pull_pairs_verb ? &call->values : NULL,
        .written = &call->written,
        .arena = ustore_py_arena_handle(call->call.arena),
        .wanted = wanted,
        .progress = ustore_py_progress_callback(&call->call),
        .progress_state = &call->call,
    };
    return ustore_py_call_run(&context, verb, call, &next);
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

static PyObject* Cursor_next(Cursor* self, PyObject* const* args, Py_ssize_t positional_args_count,
                             PyObject* args_names_tuple) {
    Py_ssize_t const args_names_count = ustore_py_args_names_count_checked("next", positional_args_count,
                                                                           args_names_tuple, 2);
    if (args_names_count < 0) return NULL;
    ustore_py_verb_t const* verb = &pull_keys_verb;
    PyObject* progress = NULL;
    for (Py_ssize_t names_progress = 0; names_progress != args_names_count; ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(args_names_tuple, names_progress);
        PyObject* const value = args[positional_args_count + names_progress];
        int read = -1;
        if (PyUnicode_CompareWithASCIIString(name, "values") == 0) {
            int const asked = PyObject_IsTrue(value);
            verb = asked > 0 ? &pull_pairs_verb : &pull_keys_verb;
            read = asked < 0 ? -1 : 0;
        }
        else if (PyUnicode_CompareWithASCIIString(name, "progress") == 0)
            read = ustore_py_progress_from_object(value, &progress);
        else PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
        if (read != 0) return NULL;
    }
    size_t wanted = 0;
    if (ustore_py_size_from_object(args[0], "wanted", &wanted) != 0) return NULL;

    ustore_blobs_cursor_t const lent = (ustore_blobs_cursor_t)(void*)&cursor_lent;
    ustore_blobs_cursor_t handle = NULL;
    ustore_py_critical_section_begin_((PyObject*)self);
    handle = self->handle;
    if (handle) self->handle = lent;
    ustore_py_critical_section_end_();
    if (!handle || handle == lent) {
        ustore_py_raise(handle ? ustore_status_contended_k : ustore_status_uninitialized_k,
                        handle ? "Another pull of this cursor is running" : "The cursor is closed");
        return NULL;
    }
    PyObject* const page = cursor_pull(self, handle, wanted, args[1], verb, progress);
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

static PyObject* Cursor_exit(PyObject* self, PyObject* const* Py_UNUSED(args),
                             Py_ssize_t Py_UNUSED(positional_args_count), PyObject* Py_UNUSED(args_names_tuple)) {
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

char const doc_cursor[] =                                                                    //
    "cursor(start=0, /, *, collection=None) answers a Cursor.\n"                             //
    "\n"                                                                                     //
    "Opens a walk over the keys of one collection from `start` upwards, reading nothing\n"   //
    "until its first pull, which `Cursor.next` makes into an arena of its own. Every pull\n" //
    "blocks on the receiver's executor, or on the calling thread. The cursor holds its\n"    //
    "database open, and the transaction or snapshot it reads through, until it closes.\n"    //
    "Calls `ustore_blobs_cursor_init`.\n"                                                    //
    "\n"                                                                                     //
    "Args:\n"                                                                                //
    "  start (KeyLike, optional): The smallest key the walk returns.\n"                      //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n"       //
    "Returns:\n"                                                                             //
    "  Cursor: The walk, pulled page by page.";

PyObject* ustore_py_api_cursor(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                               PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (positional_args_count > 1) {
        PyErr_Format(PyExc_TypeError, "cursor() takes the start alone by position, received %zd arguments",
                     positional_args_count);
        return NULL;
    }
    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, args, positional_args_count, args_names_tuple, &options) != 0)
        return NULL;
    ustore_key_t start = {0};
    if (positional_args_count == 1 && ustore_py_key_from_object(args[0], &start) != 0) return NULL;

    Cursor* const cursor = PyObject_New(Cursor, &CursorType);
    if (!cursor) return NULL;
    cursor->handle = NULL;
    cursor->database = context.database_handle;
    cursor->executor = (Executor*)Py_XNewRef((PyObject*)context.executor);
    ustore_outcome_t outcome = {0};
    ustore_blobs_cursor_init_t init = {
        .cursor = &cursor->handle,
        .database = context.database_handle,
        .outcome = &outcome,
        .view = context.view,
        .collection = options.collection,
        .start_key = start,
    };
    Py_BEGIN_ALLOW_THREADS;
    ustore_blobs_cursor_init(&init);
    Py_END_ALLOW_THREADS;
    if (ustore_py_raise_on_outcome(&outcome) == 0) return (PyObject*)cursor;
    Py_DECREF(cursor);
    return NULL;
}

#pragma endregion Cursors
