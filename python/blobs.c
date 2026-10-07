/**
 *  @file python/blobs.c
 *  @author Ash Vardanian
 *  @date September 16, 2026
 *  @brief Binary values: writes, reads, scans, samples and measurements.
 *
 *  Every verb here is written once and reached from both method tables, since a database and a
 *  transaction differ only in the view they address, which the shared context carries. Each one
 *  drops the interpreter lock around the call. A read claims the database's arena for the tape and
 *  either returns it or hands it to the result, while every other verb writes into spans the result
 *  or the stack holds and needs no arena.
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

PyObject* ustore_py_values_read(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                                ustore_key_t const* keys, size_t count) {
    Values* const result = ustore_py_values_adopt(count);
    if (!result) return NULL;
    ustore_outcome_t outcome = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context->database);

    ustore_read_t read = {0};
    read.database = context->database->handle;
    read.outcome = &outcome;
    read.view = options->view;
    read.count = count;
    read.collections = &options->collection;
    read.keys = keys;
    read.keys_stride = sizeof(ustore_key_t);
    read.presences = result->presences;
    read.offsets = result->offsets;
    read.values = &result->tape;
    read.arena = &arena;

    Py_BEGIN_ALLOW_THREADS;
    ustore_read(&read);
    Py_END_ALLOW_THREADS;

    if (ustore_py_raise_on_outcome(&outcome) != 0) {
        ustore_py_arena_return(context->database, arena);
        Py_DECREF(result);
        return NULL;
    }
    // The tape leaves with the arena that holds it, so the database lends a fresh one next time
    // rather than overwriting bytes the caller is still reading.
    result->arena = arena;
    return (PyObject*)result;
}

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
    PyObject* const values = ustore_py_values_read(&context, &options, keys.keys, keys.count);
    ustore_py_keys_release(&keys);
    return values;
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

    ustore_octet_t presence = 0;
    ustore_length_t offsets[2] = {0, 0};
    ustore_byte_t* values = NULL;
    ustore_outcome_t outcome = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context.database);

    ustore_read_t read = {0};
    read.database = context.database->handle;
    read.outcome = &outcome;
    read.view = options.view;
    read.count = 1;
    read.collections = &options.collection;
    read.keys = &key;
    read.presences = &presence;
    read.offsets = offsets;
    read.values = &values;
    read.arena = &arena;

    Py_BEGIN_ALLOW_THREADS;
    ustore_read(&read);
    Py_END_ALLOW_THREADS;

    PyObject* found = NULL;
    if (ustore_py_raise_on_outcome(&outcome) == 0)
        found = (presence & 1)
                    ? PyBytes_FromStringAndSize((char const*)values + offsets[0], (Py_ssize_t)(offsets[1] - offsets[0]))
                    : Py_NewRef(Py_None);
    ustore_py_arena_return(context.database, arena);
    return found;
}

char const doc_contains[] =                                                                  //
    "contains(key, *, collection=None, snapshot=None) answers whether the key is present.\n" //
    "\n"                                                                                     //
    "Reads the presence alone, so no value crosses the boundary and no arena is touched.\n"  //
    "\n"                                                                                     //
    "Args:\n"                                                                                //
    "  key (KeyLike): The key to look for.\n"                                                //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n"       //
    "  snapshot (Snapshot, optional): The snapshot to read; None reads the head.\n"          //
    "Returns:\n"                                                                             //
    "  bool: Whether the key carries a value.";

int ustore_py_value_contains(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                             ustore_key_t const* key) {
    ustore_octet_t presence = 0;
    ustore_outcome_t outcome = {0};

    ustore_read_t read = {0};
    read.database = context->database->handle;
    read.outcome = &outcome;
    read.view = options->view;
    read.count = 1;
    read.collections = &options->collection;
    read.keys = key;
    read.presences = &presence;

    Py_BEGIN_ALLOW_THREADS;
    ustore_read(&read);
    Py_END_ALLOW_THREADS;

    // The bitset is addressed as Apache Arrow does, so the first task is the lowest bit.
    return ustore_py_raise_on_outcome(&outcome) != 0 ? -1 : (presence & 1) != 0;
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
    int const found = ustore_py_value_contains(&context, &options, &key);
    return found < 0 ? NULL : PyBool_FromLong(found);
}

#pragma endregion Reads

#pragma region Writes

int ustore_py_contents_from_object(PyObject* object, size_t count, ustore_py_contents_t* batch) {
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

    PyObject* const sequence = PySequence_Fast(object, "Expected a sequence of values or one buffer");
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

char const doc_write[] =                                                                    //
    "write(keys, values, *, collection=None, flush=False) answers None.\n"                  //
    "\n"                                                                                    //
    "Upserts a batch of binary values in one call. Every value is borrowed for the\n"       //
    "length of the call rather than copied, so a memory-mapped buffer reaches the engine\n" //
    "untouched.\n"                                                                          //
    "\n"                                                                                    //
    "Args:\n"                                                                               //
    "  keys (Sequence[KeyLike] | buffer): The keys to write.\n"                             //
    "  values (Sequence[bytes-like] | bytes-like): One value per key, or a single\n"        //
    "    bytes-like object broadcast to every key. None is refused, since `remove`\n"       //
    "    removes keys.\n"                                                                   //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n"      //
    "  flush (bool, optional): Whether to reach the drive before returning.\n"              //
    "Raises:\n"                                                                             //
    "  TypeError: A value is None or exports no buffer.\n"                                  //
    "  Error: Whatever the engine reported.";

int ustore_py_values_write(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                           ustore_key_t const* keys, size_t count, ustore_py_contents_t const* contents) {
    int const broadcast = contents->layout == ustore_py_contents_broadcast_k;
    ustore_outcome_t outcome = {0};

    ustore_write_t write = {0};
    write.database = context->database->handle;
    write.outcome = &outcome;
    write.transaction = context->transaction;
    write.count = count;
    write.collections = &options->collection;
    write.keys = keys;
    write.keys_stride = sizeof(ustore_key_t);
    write.values = contents->values;
    write.lengths = contents->lengths;
    // A stride of zero repeats the first entry, which is how one value reaches every key.
    write.values_stride = broadcast ? 0 : sizeof(ustore_bytes_cptr_t);
    write.lengths_stride = broadcast ? 0 : sizeof(ustore_length_t);
    write.durability = options->durability;
    write.watch = options->watch;

    Py_BEGIN_ALLOW_THREADS;
    ustore_write(&write);
    Py_END_ALLOW_THREADS;
    return ustore_py_raise_on_outcome(&outcome);
}

int ustore_py_values_remove(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                            ustore_key_t const* keys, size_t count) {
    ustore_outcome_t outcome = {0};

    ustore_remove_t remove = {0};
    remove.database = context->database->handle;
    remove.outcome = &outcome;
    remove.transaction = context->transaction;
    remove.count = count;
    remove.collections = &options->collection;
    remove.keys = keys;
    remove.keys_stride = sizeof(ustore_key_t);
    remove.durability = options->durability;
    remove.watch = options->watch;

    Py_BEGIN_ALLOW_THREADS;
    ustore_remove(&remove);
    Py_END_ALLOW_THREADS;
    return ustore_py_raise_on_outcome(&outcome);
}

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
    if (ustore_py_contents_from_object(args[1], keys.count, &contents) != 0) {
        ustore_py_keys_release(&keys);
        return NULL;
    }

    int const written = ustore_py_values_write(&context, &options, keys.keys, keys.count, &contents);
    ustore_py_contents_release(&contents);
    ustore_py_keys_release(&keys);
    if (written != 0) return NULL;
    Py_RETURN_NONE;
}

char const doc_set[] =                                                                 //
    "set(key, value, *, collection=None, flush=False) answers None.\n"                 //
    "\n"                                                                               //
    "Upserts one binary value.\n"                                                      //
    "\n"                                                                               //
    "Args:\n"                                                                          //
    "  key (KeyLike): The key to write.\n"                                             //
    "  value (bytes-like): The bytes to store, borrowed rather than copied.\n"         //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n" //
    "  flush (bool, optional): Whether to reach the drive before returning.\n"         //
    "Raises:\n"                                                                        //
    "  TypeError: The value is None or exports no buffer.";

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

    int const written = ustore_py_values_write(&context, &options, &key, 1, &contents);
    ustore_py_contents_release(&contents);
    if (written != 0) return NULL;
    Py_RETURN_NONE;
}

char const doc_remove[] =                                                                    //
    "remove(keys, *, collection=None, flush=False) answers None.\n"                          //
    "\n"                                                                                     //
    "Removes a batch of keys with their values or documents, or one key given on its own.\n" //
    "\n"                                                                                     //
    "Args:\n"                                                                                //
    "  keys (KeyLike | Sequence[KeyLike] | buffer): The keys to remove.\n"                   //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n"       //
    "  flush (bool, optional): Whether to reach the drive before returning.";

PyObject* ustore_py_api_remove(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                               PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_args_names_count_checked("remove", positional_args_count, args_names_tuple, 1) < 0) return NULL;

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

    int const removed = ustore_py_values_remove(&context, &options, keys.keys, keys.count);
    if (keys.keys != &single) ustore_py_keys_release(&keys);
    if (removed != 0) return NULL;
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

    /** The sum of every limit, which sizes the keys the batch may find, saturating rather than wrapping. */
    size_t total;

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
        batch->total = limit > SIZE_MAX - batch->total ? SIZE_MAX : batch->total + limit;
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

PyObject* ustore_py_keys_scan(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                              ustore_key_t const* start, size_t count) {
    Keys* const found = ustore_py_keys_adopt(1, count);
    if (!found) return NULL;
    ustore_length_t const count_limit = (ustore_length_t)count;
    ustore_outcome_t outcome = {0};

    ustore_scan_t scan = {0};
    scan.database = context->database->handle;
    scan.outcome = &outcome;
    scan.view = options->view;
    scan.count = 1;
    scan.collections = &options->collection;
    scan.start_keys = start;
    scan.count_limits = &count_limit;
    scan.offsets = found->offsets;
    scan.keys = found->tape;

    Py_BEGIN_ALLOW_THREADS;
    ustore_scan(&scan);
    Py_END_ALLOW_THREADS;

    if (ustore_py_raise_on_outcome(&outcome) == 0) return (PyObject*)found;
    Py_DECREF(found);
    return NULL;
}

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
    return ustore_py_keys_scan(&context, &options, &start, limit);
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

    Keys* const found = ustore_py_keys_adopt(starts.count, limits.total);
    ustore_outcome_t outcome = {0};

    // A batch of no requests answers an empty tape, and the ABI takes no starts for no tasks.
    if (found && starts.count) {
        ustore_scan_t scan = {0};
        scan.database = context.database->handle;
        scan.outcome = &outcome;
        scan.view = options.view;
        scan.count = starts.count;
        scan.collections = &options.collection;
        scan.start_keys = starts.keys;
        scan.start_keys_stride = sizeof(ustore_key_t);
        scan.count_limits = limits.limits;
        scan.count_limits_stride = sizeof(ustore_length_t);
        scan.offsets = found->offsets;
        scan.keys = found->tape;

        Py_BEGIN_ALLOW_THREADS;
        ustore_scan(&scan);
        Py_END_ALLOW_THREADS;
    }

    limits_release(&limits);
    ustore_py_keys_release(&starts);
    if (!found || ustore_py_raise_on_outcome(&outcome) == 0) return (PyObject*)found;
    Py_DECREF(found);
    return NULL;
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

PyObject* ustore_py_keys_sample(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                                size_t count) {
    Keys* const found = ustore_py_keys_adopt(1, count);
    if (!found) return NULL;
    ustore_length_t const count_limit = (ustore_length_t)count;
    ustore_outcome_t outcome = {0};

    ustore_sample_t sample = {0};
    sample.database = context->database->handle;
    sample.outcome = &outcome;
    sample.view = options->view;
    sample.count = 1;
    sample.collections = &options->collection;
    sample.count_limits = &count_limit;
    sample.offsets = found->offsets;
    sample.keys = found->tape;

    Py_BEGIN_ALLOW_THREADS;
    ustore_sample(&sample);
    Py_END_ALLOW_THREADS;

    if (ustore_py_raise_on_outcome(&outcome) == 0) return (PyObject*)found;
    Py_DECREF(found);
    return NULL;
}

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
    return ustore_py_keys_sample(&context, &options, wanted);
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

    Keys* const found = ustore_py_keys_adopt(limits.count, limits.total);
    ustore_outcome_t outcome = {0};

    // A batch of no requests answers an empty tape, and the ABI takes no limits for no tasks.
    if (found && limits.count) {
        ustore_sample_t sample = {0};
        sample.database = context.database->handle;
        sample.outcome = &outcome;
        sample.view = options.view;
        sample.count = limits.count;
        sample.collections = &options.collection;
        sample.count_limits = limits.limits;
        sample.count_limits_stride = sizeof(ustore_length_t);
        sample.offsets = found->offsets;
        sample.keys = found->tape;

        Py_BEGIN_ALLOW_THREADS;
        ustore_sample(&sample);
        Py_END_ALLOW_THREADS;
    }

    limits_release(&limits);
    if (!found || ustore_py_raise_on_outcome(&outcome) == 0) return (PyObject*)found;
    Py_DECREF(found);
    return NULL;
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

    char const* const names[6] = {"min_cardinality", "max_cardinality", "min_value_bytes",
                                  "max_value_bytes", "min_space_usage", "max_space_usage"};
    ustore_size_t values[6] = {0};
    ustore_outcome_t outcome = {0};

    ustore_measure_t measure = {0};
    measure.database = context.database->handle;
    measure.outcome = &outcome;
    measure.view = options.view;
    measure.count = 1;
    measure.collections = &options.collection;
    measure.start_keys = &start;
    measure.end_keys = &end;
    measure.min_cardinalities = &values[0];
    measure.min_cardinalities_stride = sizeof(ustore_size_t);
    measure.max_cardinalities = &values[1];
    measure.max_cardinalities_stride = sizeof(ustore_size_t);
    measure.min_value_bytes = &values[2];
    measure.min_value_bytes_stride = sizeof(ustore_size_t);
    measure.max_value_bytes = &values[3];
    measure.max_value_bytes_stride = sizeof(ustore_size_t);
    measure.min_space_usages = &values[4];
    measure.min_space_usages_stride = sizeof(ustore_size_t);
    measure.max_space_usages = &values[5];
    measure.max_space_usages_stride = sizeof(ustore_size_t);

    Py_BEGIN_ALLOW_THREADS;
    ustore_measure(&measure);
    Py_END_ALLOW_THREADS;
    if (ustore_py_raise_on_outcome(&outcome) != 0) return NULL;

    PyObject* bounds = PyDict_New();
    for (size_t index = 0; bounds && index != 6; ++index) {
        PyObject* const value = PyLong_FromUnsignedLongLong(values[index]);
        if (!value || PyDict_SetItemString(bounds, names[index], value) != 0) Py_CLEAR(bounds);
        Py_XDECREF(value);
    }
    return bounds;
}

#pragma endregion Scans
