/**
 *  @file python/blobs.c
 *  @author Ash Vardanian
 *  @date September 16, 2026
 *  @brief Binary values: writes, reads, scans, samples and measurements.
 *
 *  Every verb here is written once and reached from both method tables, since a database and a
 *  transaction differ only in the view they address, which the shared context carries. Each one
 *  claims the database's arena, drops the interpreter lock around the call, and either returns the
 *  arena or hands it to the tape it answers with.
 */
#include "lib.h"

#pragma region Reads

char const doc_read[] =                                                                    //
    "read(keys, *, collection=None, snapshot=None) answers a Values tape.\n"               //
    "\n"                                                                                   //
    "Reads a batch of binary values in one call, which no sequence of single reads\n"      //
    "matches for throughput. Nothing is copied: the tape sits in an arena the result\n"    //
    "owns, and indexing it answers a memoryview over the stored bytes, or None for an\n"   //
    "absent key.\n"                                                                        //
    "\n"                                                                                   //
    "Args:\n"                                                                              //
    "  keys (Sequence[KeyLike] | buffer): The keys to read, as a sequence, a contiguous\n" //
    "    buffer of 16-byte keys, which is read in place, or one of 64-bit integers.\n"     //
    "  collection (UUID | str, optional): The collection, by identifier or by name;\n"     //
    "    None addresses the main one.\n"                                                   //
    "  snapshot (Snapshot, optional): The snapshot to read; None reads the head. A\n"      //
    "    transaction already fixes the view and therefore refuses this.\n"                 //
    "Returns:\n"                                                                           //
    "  Values: One entry per key, in the order they were asked for.\n"                     //
    "Raises:\n"                                                                            //
    "  Error: Whatever the engine reported.";

PyObject* ustore_py_api_read(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                             PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_args_names_count_checked("read", positional_args_count, args_names_tuple, 1) < 0) return NULL;

    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, args, positional_args_count, args_names_tuple, &options) != 0)
        return NULL;

    ustore_py_keys_t keys;
    if (ustore_py_keys_from_object(args[0], &keys) != 0) return NULL;

    ustore_length_t* offsets = NULL;
    ustore_length_t* lengths = NULL;
    ustore_byte_t* values = NULL;
    ustore_error_t error = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context.database);

    ustore_read_t read = {0};
    read.db = context.database->handle;
    read.error = &error;
    read.transaction = context.transaction;
    read.snapshot = options.snapshot;
    read.arena = &arena;
    read.options = options.options;
    read.tasks_count = keys.count;
    read.collections = &options.collection;
    read.keys = keys.keys;
    read.keys_stride = sizeof(ustore_key_t);
    read.offsets = &offsets;
    read.lengths = &lengths;
    read.values = &values;

    Py_BEGIN_ALLOW_THREADS;
    ustore_read(&read);
    Py_END_ALLOW_THREADS;

    size_t const tasks_count = keys.count;
    ustore_py_keys_release(&keys);
    if (error.status != ustore_success_k) {
        ustore_py_arena_return(context.database, arena);
        ustore_py_raise_on_error(&error);
        return NULL;
    }
    // The tape leaves with the arena that holds it, so the database lends a fresh one next time
    // rather than overwriting bytes the caller is still reading.
    return ustore_py_values_adopt(arena, values, offsets, lengths, tasks_count);
}

char const doc_get[] =                                                                     //
    "get(key, *, collection=None, snapshot=None) answers bytes or None.\n"                 //
    "\n"                                                                                   //
    "Reads one value and copies it into a bytes object, so nothing of the arena\n"         //
    "outlives the call. Reach for `read` when several keys are wanted, or when the copy\n" //
    "is not.\n"                                                                            //
    "\n"                                                                                   //
    "Args:\n"                                                                              //
    "  key (KeyLike): The key to read.\n"                                                  //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n"     //
    "  snapshot (Snapshot, optional): The snapshot to read; None reads the head.\n"        //
    "Returns:\n"                                                                           //
    "  bytes | None: The stored bytes, or None when the key is absent.";

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

    ustore_length_t* offsets = NULL;
    ustore_length_t* lengths = NULL;
    ustore_byte_t* values = NULL;
    ustore_error_t error = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context.database);

    ustore_read_t read = {0};
    read.db = context.database->handle;
    read.error = &error;
    read.transaction = context.transaction;
    read.snapshot = options.snapshot;
    read.arena = &arena;
    read.options = options.options;
    read.tasks_count = 1;
    read.collections = &options.collection;
    read.keys = &key;
    read.offsets = &offsets;
    read.lengths = &lengths;
    read.values = &values;

    Py_BEGIN_ALLOW_THREADS;
    ustore_read(&read);
    Py_END_ALLOW_THREADS;

    PyObject* found = NULL;
    if (error.status != ustore_success_k) { ustore_py_raise_on_error(&error); }
    else if (lengths[0] == ustore_length_missing_k) { found = Py_NewRef(Py_None); }
    else { found = PyBytes_FromStringAndSize((char const*)(values + offsets[0]), (Py_ssize_t)lengths[0]); }
    ustore_py_arena_return(context.database, arena);
    return found;
}

char const doc_contains[] =                                                                  //
    "contains(key, *, collection=None, snapshot=None) answers whether the key is present.\n" //
    "\n"                                                                                     //
    "Reads the presence alone, so no value crosses the boundary and no tape is built.\n"     //
    "\n"                                                                                     //
    "Args:\n"                                                                                //
    "  key (KeyLike): The key to look for.\n"                                                //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n"       //
    "  snapshot (Snapshot, optional): The snapshot to read; None reads the head.\n"          //
    "Returns:\n"                                                                             //
    "  bool: Whether the key carries a value.";

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

    ustore_octet_t* presences = NULL;
    ustore_error_t error = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context.database);

    ustore_read_t read = {0};
    read.db = context.database->handle;
    read.error = &error;
    read.transaction = context.transaction;
    read.snapshot = options.snapshot;
    read.arena = &arena;
    read.options = options.options;
    read.tasks_count = 1;
    read.collections = &options.collection;
    read.keys = &key;
    read.presences = &presences;

    Py_BEGIN_ALLOW_THREADS;
    ustore_read(&read);
    Py_END_ALLOW_THREADS;

    PyObject* found = NULL;
    if (error.status != ustore_success_k) { ustore_py_raise_on_error(&error); }
    // The bitset is addressed as Apache Arrow does, so the first task is the lowest bit.
    else { found = PyBool_FromLong((presences[0] & 1) != 0); }
    ustore_py_arena_return(context.database, arena);
    return found;
}

#pragma endregion Reads

#pragma region Writes

/**
 *  @brief Gathers the values of a batch, broadcasting one value or removing every key.
 *  @param[in] object The values: a sequence, one buffer to broadcast, or None to remove.
 *  @param[in] count Keys the batch carries.
 *  @param[out] batch The borrowed runs the ABI reads, and the layout they travel with.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
static int contents_from_object(PyObject* object, size_t count, ustore_py_contents_t* batch) {
    if (object == Py_None) {
        // A batch with no values at all removes every key, which the ABI spells as a null run.
        memset(batch, 0, sizeof(*batch));
        return 0;
    }
    // A bytes-like object is one value for every key rather than a sequence of them, which is what
    // makes `write(keys, b"same")` mean what it reads like.
    if (PyObject_CheckBuffer(object)) {
        if (ustore_py_contents_reserve(batch, 1) != 0) return -1;
        if (ustore_py_contents_borrow(object, batch) != 0) {
            ustore_py_contents_release(batch);
            return -1;
        }
        batch->layout = ustore_py_contents_broadcast_k;
        return 0;
    }

    PyObject* const sequence = PySequence_Fast(object, "Expected a sequence of values, one buffer, or None");
    if (!sequence) return -1;
    if ((size_t)PySequence_Fast_GET_SIZE(sequence) != count) {
        PyErr_Format(PyExc_ValueError, "Got %zd values for %zu keys", PySequence_Fast_GET_SIZE(sequence), count);
        Py_DECREF(sequence);
        return -1;
    }
    if (ustore_py_contents_reserve(batch, count) != 0) {
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

char const doc_write[] =                                                                     //
    "write(keys, values, *, collection=None, flush=False) answers None.\n"                   //
    "\n"                                                                                     //
    "Upserts or removes a batch of binary values in one call. Every value is borrowed for\n" //
    "the length of the call rather than copied, so a memory-mapped buffer reaches the\n"     //
    "engine untouched.\n"                                                                    //
    "\n"                                                                                     //
    "Args:\n"                                                                                //
    "  keys (Sequence[KeyLike] | buffer): The keys to write.\n"                              //
    "  values (Sequence[bytes-like | None] | bytes-like | None): One value per key, or a\n"  //
    "    single bytes-like object broadcast to every key, or None to remove them all. A\n"   //
    "    None inside a sequence removes that one key.\n"                                     //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n"       //
    "  flush (bool, optional): Whether to reach the drive before returning.\n"               //
    "Raises:\n"                                                                              //
    "  Error: Whatever the engine reported.";

PyObject* ustore_py_api_write(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                              PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_args_names_count_checked("write", positional_args_count, args_names_tuple, 2) < 0) return NULL;

    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, args, positional_args_count, args_names_tuple, &options) != 0)
        return NULL;

    ustore_py_keys_t keys;
    if (ustore_py_keys_from_object(args[0], &keys) != 0) return NULL;

    ustore_py_contents_t contents;
    if (contents_from_object(args[1], keys.count, &contents) != 0) {
        ustore_py_keys_release(&keys);
        return NULL;
    }

    ustore_error_t error = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context.database);

    ustore_write_t write = {0};
    write.db = context.database->handle;
    write.error = &error;
    write.transaction = context.transaction;
    write.arena = &arena;
    write.options = options.options;
    write.tasks_count = keys.count;
    write.collections = &options.collection;
    write.keys = keys.keys;
    write.keys_stride = sizeof(ustore_key_t);
    write.values = contents.values;
    write.lengths = contents.lengths;
    // A stride of zero repeats the first entry, which is how one value reaches every key.
    write.values_stride = contents.layout == ustore_py_contents_broadcast_k ? 0 : sizeof(ustore_bytes_cptr_t);
    write.lengths_stride = contents.layout == ustore_py_contents_broadcast_k ? 0 : sizeof(ustore_length_t);

    Py_BEGIN_ALLOW_THREADS;
    ustore_write(&write);
    Py_END_ALLOW_THREADS;

    ustore_py_contents_release(&contents);
    ustore_py_keys_release(&keys);
    ustore_py_arena_return(context.database, arena);
    if (ustore_py_raise_on_error(&error) != 0) return NULL;
    Py_RETURN_NONE;
}

char const doc_set[] =                                                                 //
    "set(key, value, *, collection=None, flush=False) answers None.\n"                 //
    "\n"                                                                               //
    "Upserts one binary value, or removes the key when the value is None.\n"           //
    "\n"                                                                               //
    "Args:\n"                                                                          //
    "  key (KeyLike): The key to write.\n"                                             //
    "  value (bytes-like | None): The bytes to store, borrowed rather than copied.\n"  //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n" //
    "  flush (bool, optional): Whether to reach the drive before returning.";

PyObject* ustore_py_api_set(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                            PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_args_names_count_checked("set", positional_args_count, args_names_tuple, 2) < 0) return NULL;

    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, args, positional_args_count, args_names_tuple, &options) != 0)
        return NULL;

    ustore_key_t key = {0};
    if (ustore_py_key_from_object(args[0], &key) != 0) return NULL;

    ustore_py_contents_t contents;
    if (ustore_py_contents_reserve(&contents, 1) != 0) return NULL;
    if (ustore_py_contents_borrow(args[1], &contents) != 0) {
        ustore_py_contents_release(&contents);
        return NULL;
    }

    ustore_error_t error = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context.database);

    ustore_write_t write = {0};
    write.db = context.database->handle;
    write.error = &error;
    write.transaction = context.transaction;
    write.arena = &arena;
    write.options = options.options;
    write.tasks_count = 1;
    write.collections = &options.collection;
    write.keys = &key;
    write.values = contents.values;
    write.lengths = contents.lengths;

    Py_BEGIN_ALLOW_THREADS;
    ustore_write(&write);
    Py_END_ALLOW_THREADS;

    ustore_py_contents_release(&contents);
    ustore_py_arena_return(context.database, arena);
    if (ustore_py_raise_on_error(&error) != 0) return NULL;
    Py_RETURN_NONE;
}

char const doc_pop[] =                                                                 //
    "pop(keys, *, collection=None, flush=False) answers None.\n"                       //
    "\n"                                                                               //
    "Removes a batch of keys, or one key given on its own.\n"                          //
    "\n"                                                                               //
    "Args:\n"                                                                          //
    "  keys (KeyLike | Sequence[KeyLike] | buffer): The keys to remove.\n"             //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n" //
    "  flush (bool, optional): Whether to reach the drive before returning.";

PyObject* ustore_py_api_pop(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                            PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_args_names_count_checked("pop", positional_args_count, args_names_tuple, 1) < 0) return NULL;

    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, args, positional_args_count, args_names_tuple, &options) != 0)
        return NULL;

    ustore_key_t single = {0};
    ustore_py_keys_t keys;
    memset(&keys, 0, sizeof(keys));
    if (ustore_py_is_one_key(args[0])) {
        if (ustore_py_key_from_object(args[0], &single) != 0) return NULL;
        keys.keys = &single;
        keys.count = 1;
    }
    else if (ustore_py_keys_from_object(args[0], &keys) != 0) { return NULL; }

    ustore_error_t error = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context.database);

    ustore_write_t write = {0};
    write.db = context.database->handle;
    write.error = &error;
    write.transaction = context.transaction;
    write.arena = &arena;
    write.options = options.options;
    write.tasks_count = keys.count;
    write.collections = &options.collection;
    write.keys = keys.keys;
    write.keys_stride = sizeof(ustore_key_t);

    Py_BEGIN_ALLOW_THREADS;
    ustore_write(&write);
    Py_END_ALLOW_THREADS;

    if (keys.keys != &single) ustore_py_keys_release(&keys);
    ustore_py_arena_return(context.database, arena);
    if (ustore_py_raise_on_error(&error) != 0) return NULL;
    Py_RETURN_NONE;
}

#pragma endregion Writes

#pragma region Scans

/**
 *  @brief The per-request limits one batch scan or sample carries.
 *
 *  A batch small enough lays its limits on the stack, and one allocation backs the rest, matching
 *  what a batch of keys does beside it.
 */
typedef struct limits_t {

    /** The limits, aliasing whichever storage the fields below name. */
    ustore_length_t* limits;

    /** Requests the batch holds. */
    size_t count;

    /** The heap block backing @c limits, or NULL when they sit inline. */
    ustore_length_t* owned;

    /** Storage for a batch that fits without the heap. */
    ustore_length_t inline_limits[ustore_py_keys_inline_k];
} limits_t;

/** Releases whatever @c limits_from_object acquired. */
static void limits_release(limits_t* batch) {
    if (batch->owned) PyMem_Free(batch->owned);
    batch->owned = NULL;
    batch->limits = NULL;
    batch->count = 0;
}

/**
 *  @brief Gathers one limit per request from a sequence of non-negative integers.
 *  @param[in] object The limits, as a sequence or anything indexing into one.
 *  @param[out] batch The limits and whatever storage gathering them needed.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
static int limits_from_object(PyObject* object, limits_t* batch) {
    memset(batch, 0, sizeof(*batch));
    PyObject* const sequence = PySequence_Fast(object, "Expected a sequence of per-request limits");
    if (!sequence) return -1;
    Py_ssize_t const count = PySequence_Fast_GET_SIZE(sequence);

    if (count == 0) { batch->limits = NULL; }
    else if ((size_t)count <= ustore_py_keys_inline_k) { batch->limits = batch->inline_limits; }
    else {
        batch->owned = (ustore_length_t*)PyMem_Malloc((size_t)count * sizeof(ustore_length_t));
        if (!batch->owned) {
            Py_DECREF(sequence);
            PyErr_NoMemory();
            return -1;
        }
        batch->limits = batch->owned;
    }

    for (Py_ssize_t entry_index = 0; entry_index != count; ++entry_index) {
        size_t limit = 0;
        if (ustore_py_size_from_object(PySequence_Fast_GET_ITEM(sequence, entry_index), "count", &limit) != 0) {
            Py_DECREF(sequence);
            limits_release(batch);
            return -1;
        }
        batch->limits[entry_index] = (ustore_length_t)limit;
    }
    Py_DECREF(sequence);
    batch->count = (size_t)count;
    return 0;
}

char const doc_scan[] =                                                                   //
    "scan(start, count, *, collection=None, snapshot=None) answers a Keys tape.\n"        //
    "\n"                                                                                  //
    "Lists up to `count` keys from `start` upwards. Values are not exported, so a read\n" //
    "follows the scan; a walk longer than one page resumes from the last key it saw,\n"   //
    "since holding an iterator over concurrent state costs more than paginating.\n"       //
    "\n"                                                                                  //
    "Args:\n"                                                                             //
    "  start (KeyLike): The smallest key the scan may return.\n"                          //
    "  count (int): Most keys the scan returns.\n"                                        //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n"    //
    "  snapshot (Snapshot, optional): The snapshot to scan; None scans the head.\n"       //
    "Returns:\n"                                                                          //
    "  Keys: One run of keys, in ascending order.";

PyObject* ustore_py_api_scan(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                             PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_args_names_count_checked("scan", positional_args_count, args_names_tuple, 2) < 0) return NULL;

    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, args, positional_args_count, args_names_tuple, &options) != 0)
        return NULL;

    ustore_key_t start = {0};
    size_t limit = 0;
    if (ustore_py_key_from_object(args[0], &start) != 0) return NULL;
    if (ustore_py_size_from_object(args[1], "count", &limit) != 0) return NULL;

    ustore_length_t const count_limit = (ustore_length_t)limit;
    ustore_length_t* offsets = NULL;
    ustore_length_t* counts = NULL;
    ustore_key_t* found = NULL;
    ustore_error_t error = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context.database);

    ustore_scan_t scan = {0};
    scan.db = context.database->handle;
    scan.error = &error;
    scan.transaction = context.transaction;
    scan.snapshot = options.snapshot;
    scan.arena = &arena;
    scan.options = options.options;
    scan.tasks_count = 1;
    scan.collections = &options.collection;
    scan.start_keys = &start;
    scan.count_limits = &count_limit;
    scan.offsets = &offsets;
    scan.counts = &counts;
    scan.keys = &found;

    Py_BEGIN_ALLOW_THREADS;
    ustore_scan(&scan);
    Py_END_ALLOW_THREADS;

    if (error.status != ustore_success_k) {
        ustore_py_arena_return(context.database, arena);
        ustore_py_raise_on_error(&error);
        return NULL;
    }
    return ustore_py_keys_adopt(arena, found, offsets, counts, 1);
}

char const doc_scan_batch[] =                                                              //
    "scan_batch(starts, counts, *, collection=None, snapshot=None) answers a Keys tape.\n" //
    "\n"                                                                                   //
    "Runs many scans in one call, which is what the ABI takes and what an engine can\n"    //
    "spread across its threads; indexing the answer by request gives the run that\n"       //
    "request found. Limits differ per request, so a walk that resumes several ranges at\n" //
    "once asks for exactly what each one still needs.\n"                                   //
    "\n"                                                                                   //
    "Args:\n"                                                                              //
    "  starts (Sequence[KeyLike] | buffer): The smallest key every scan may return.\n"     //
    "  counts (Sequence[int]): Most keys every scan returns, one limit per start.\n"       //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n"     //
    "  snapshot (Snapshot, optional): The snapshot to scan; None scans the head.\n"        //
    "Returns:\n"                                                                           //
    "  Keys: One run of ascending keys per request, in the order they were asked for.";

PyObject* ustore_py_api_scan_batch(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                   PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_args_names_count_checked("scan_batch", positional_args_count, args_names_tuple, 2) < 0) return NULL;

    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, args, positional_args_count, args_names_tuple, &options) != 0)
        return NULL;

    ustore_py_keys_t starts;
    if (ustore_py_keys_from_object(args[0], &starts) != 0) return NULL;
    limits_t limits;
    if (limits_from_object(args[1], &limits) != 0) {
        ustore_py_keys_release(&starts);
        return NULL;
    }
    if (limits.count != starts.count) {
        PyErr_Format(PyExc_ValueError, "Got %zu limits for %zu starts", limits.count, starts.count);
        limits_release(&limits);
        ustore_py_keys_release(&starts);
        return NULL;
    }

    ustore_length_t* offsets = NULL;
    ustore_length_t* counts = NULL;
    ustore_key_t* found = NULL;
    ustore_error_t error = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context.database);

    // A batch of no requests answers an empty tape, which the ABI has no address to write into.
    if (starts.count) {
        ustore_scan_t scan = {0};
        scan.db = context.database->handle;
        scan.error = &error;
        scan.transaction = context.transaction;
        scan.snapshot = options.snapshot;
        scan.arena = &arena;
        scan.options = options.options;
        scan.tasks_count = starts.count;
        scan.collections = &options.collection;
        scan.start_keys = starts.keys;
        scan.start_keys_stride = sizeof(ustore_key_t);
        scan.count_limits = limits.limits;
        scan.count_limits_stride = sizeof(ustore_length_t);
        scan.offsets = &offsets;
        scan.counts = &counts;
        scan.keys = &found;

        Py_BEGIN_ALLOW_THREADS;
        ustore_scan(&scan);
        Py_END_ALLOW_THREADS;
    }

    size_t const tasks_count = starts.count;
    limits_release(&limits);
    ustore_py_keys_release(&starts);
    if (error.status != ustore_success_k) {
        ustore_py_arena_return(context.database, arena);
        ustore_py_raise_on_error(&error);
        return NULL;
    }
    return ustore_py_keys_adopt(arena, found, offsets, counts, tasks_count);
}

char const doc_sample[] =                                                                 //
    "sample(count, *, collection=None, snapshot=None) answers a Keys tape.\n"             //
    "\n"                                                                                  //
    "Draws keys uniformly at random from one collection, which is how a workload picks\n" //
    "entries it has no list of.\n"                                                        //
    "\n"                                                                                  //
    "Args:\n"                                                                             //
    "  count (int): Keys to draw.\n"                                                      //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n"    //
    "  snapshot (Snapshot, optional): The snapshot to sample; None samples the head.\n"   //
    "Returns:\n"                                                                          //
    "  Keys: One run of keys, in no particular order.";

PyObject* ustore_py_api_sample(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                               PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_args_names_count_checked("sample", positional_args_count, args_names_tuple, 1) < 0) return NULL;

    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, args, positional_args_count, args_names_tuple, &options) != 0)
        return NULL;

    size_t wanted = 0;
    if (ustore_py_size_from_object(args[0], "count", &wanted) != 0) return NULL;

    ustore_length_t const count_limit = (ustore_length_t)wanted;
    ustore_length_t* offsets = NULL;
    ustore_length_t* counts = NULL;
    ustore_key_t* found = NULL;
    ustore_error_t error = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context.database);

    ustore_sample_t sample = {0};
    sample.db = context.database->handle;
    sample.error = &error;
    sample.transaction = context.transaction;
    sample.snapshot = options.snapshot;
    sample.arena = &arena;
    sample.options = options.options;
    sample.tasks_count = 1;
    sample.collections = &options.collection;
    sample.count_limits = &count_limit;
    sample.offsets = &offsets;
    sample.counts = &counts;
    sample.keys = &found;

    Py_BEGIN_ALLOW_THREADS;
    ustore_sample(&sample);
    Py_END_ALLOW_THREADS;

    if (error.status != ustore_success_k) {
        ustore_py_arena_return(context.database, arena);
        ustore_py_raise_on_error(&error);
        return NULL;
    }
    return ustore_py_keys_adopt(arena, found, offsets, counts, 1);
}

char const doc_sample_batch[] =                                                            //
    "sample_batch(counts, *, collection=None, snapshot=None) answers a Keys tape.\n"       //
    "\n"                                                                                   //
    "Draws many independent samples in one call, one per limit, which is how a workload\n" //
    "builds several disjoint batches without paying for a call each.\n"                    //
    "\n"                                                                                   //
    "Args:\n"                                                                              //
    "  counts (Sequence[int]): Keys every sample draws, one limit per request.\n"          //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n"     //
    "  snapshot (Snapshot, optional): The snapshot to sample; None samples the head.\n"    //
    "Returns:\n"                                                                           //
    "  Keys: One run of keys per request, in no particular order.";

PyObject* ustore_py_api_sample_batch(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                     PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_args_names_count_checked("sample_batch", positional_args_count, args_names_tuple, 1) < 0) return NULL;

    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, args, positional_args_count, args_names_tuple, &options) != 0)
        return NULL;

    limits_t limits;
    if (limits_from_object(args[0], &limits) != 0) return NULL;

    ustore_length_t* offsets = NULL;
    ustore_length_t* counts = NULL;
    ustore_key_t* found = NULL;
    ustore_error_t error = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context.database);

    // A batch of no requests answers an empty tape, which the ABI has no address to write into.
    if (limits.count) {
        ustore_sample_t sample = {0};
        sample.db = context.database->handle;
        sample.error = &error;
        sample.transaction = context.transaction;
        sample.snapshot = options.snapshot;
        sample.arena = &arena;
        sample.options = options.options;
        sample.tasks_count = limits.count;
        sample.collections = &options.collection;
        sample.count_limits = limits.limits;
        sample.count_limits_stride = sizeof(ustore_length_t);
        sample.offsets = &offsets;
        sample.counts = &counts;
        sample.keys = &found;

        Py_BEGIN_ALLOW_THREADS;
        ustore_sample(&sample);
        Py_END_ALLOW_THREADS;
    }

    size_t const tasks_count = limits.count;
    limits_release(&limits);
    if (error.status != ustore_success_k) {
        ustore_py_arena_return(context.database, arena);
        ustore_py_raise_on_error(&error);
        return NULL;
    }
    return ustore_py_keys_adopt(arena, found, offsets, counts, tasks_count);
}

char const doc_measure[] =                                                               //
    "measure(start, end, *, collection=None, snapshot=None) answers a dict of bounds.\n" //
    "\n"                                                                                 //
    "Estimates what a key range holds without walking it, so the answers are bounds\n"   //
    "rather than counts.\n"                                                              //
    "\n"                                                                                 //
    "Args:\n"                                                                            //
    "  start (KeyLike): First key of the range.\n"                                       //
    "  end (KeyLike): Last key of the range.\n"                                          //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n"   //
    "  snapshot (Snapshot, optional): The snapshot to measure; None measures the\n"      //
    "    head.\n"                                                                        //
    "Returns:\n"                                                                         //
    "  dict[str, int]: The lower and upper bounds on the entries, the bytes of their\n"  //
    "  values, and the space they take on disk.";

PyObject* ustore_py_api_measure(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_args_names_count_checked("measure", positional_args_count, args_names_tuple, 2) < 0) return NULL;

    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, args, positional_args_count, args_names_tuple, &options) != 0)
        return NULL;

    ustore_key_t start = {0};
    ustore_key_t end = {0};
    if (ustore_py_key_from_object(args[0], &start) != 0) return NULL;
    if (ustore_py_key_from_object(args[1], &end) != 0) return NULL;

    ustore_size_t* min_cardinalities = NULL;
    ustore_size_t* max_cardinalities = NULL;
    ustore_size_t* min_value_bytes = NULL;
    ustore_size_t* max_value_bytes = NULL;
    ustore_size_t* min_space_usages = NULL;
    ustore_size_t* max_space_usages = NULL;
    ustore_error_t error = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context.database);

    ustore_measure_t measure = {0};
    measure.db = context.database->handle;
    measure.error = &error;
    measure.transaction = context.transaction;
    measure.snapshot = options.snapshot;
    measure.arena = &arena;
    measure.options = options.options;
    measure.tasks_count = 1;
    measure.collections = &options.collection;
    measure.start_keys = &start;
    measure.end_keys = &end;
    measure.min_cardinalities = &min_cardinalities;
    measure.max_cardinalities = &max_cardinalities;
    measure.min_value_bytes = &min_value_bytes;
    measure.max_value_bytes = &max_value_bytes;
    measure.min_space_usages = &min_space_usages;
    measure.max_space_usages = &max_space_usages;

    Py_BEGIN_ALLOW_THREADS;
    ustore_measure(&measure);
    Py_END_ALLOW_THREADS;

    PyObject* bounds = NULL;
    if (error.status != ustore_success_k) { ustore_py_raise_on_error(&error); }
    else {
        char const* const names[6] = {"min_cardinality", "max_cardinality", "min_value_bytes",
                                      "max_value_bytes", "min_space_usage", "max_space_usage"};
        ustore_size_t const values[6] = {min_cardinalities[0], max_cardinalities[0], min_value_bytes[0],
                                         max_value_bytes[0],   min_space_usages[0],  max_space_usages[0]};
        bounds = PyDict_New();
        for (size_t index = 0; bounds && index != 6; ++index) {
            PyObject* const value = PyLong_FromUnsignedLongLong(values[index]);
            if (!value || PyDict_SetItemString(bounds, names[index], value) != 0) Py_CLEAR(bounds);
            Py_XDECREF(value);
        }
    }
    ustore_py_arena_return(context.database, arena);
    return bounds;
}

#pragma endregion Scans
