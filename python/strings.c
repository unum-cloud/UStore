/**
 *  @file python/strings.c
 *  @author Ash Vardanian
 *  @date September 16, 2026
 *  @brief Collection names, resolved onto identifiers through the strings modality.
 *
 *  Names resolve in two phases: a name maps onto a collection identifier once here, and every
 *  other verb addresses that collection by the identifier, so no other call carries a name. The
 *  mapping lives in the collection the ABI reserves for it, which no create ever hands out.
 */
#include "lib.h"

#pragma region Lookups

ustore_py_naming_t ustore_py_collection_named(ustore_py_context_t const* context, char const* name, size_t length,
                                              ustore_collection_t* collection) {
    ustore_length_t const name_length = (ustore_length_t)length;
    ustore_octet_t* presences = NULL;
    ustore_collection_t* ids = NULL;
    ustore_error_t error = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context->database);

    // Names resolve at the head or at the context's snapshot, so a transaction never watches the lookup.
    ustore_named_collections_find_t find = {0};
    find.db = context->database->handle;
    find.error = &error;
    find.snapshot = context->snapshot;
    find.arena = &arena;
    find.tasks_count = 1;
    find.names = &name;
    find.names_lengths = &name_length;
    find.presences = &presences;
    find.ids = &ids;

    Py_BEGIN_ALLOW_THREADS;
    ustore_named_collections_find(&find);
    Py_END_ALLOW_THREADS;

    // An engine without collections of its own refuses the reserved collection outright, which is
    // the same answer as an unknown name to everyone above this call.
    ustore_py_naming_t verdict = ustore_py_naming_missing_k;
    if (error.status != ustore_success_k && error.status != ustore_error_unsupported_k) {
        ustore_py_raise_on_error(&error);
        verdict = ustore_py_naming_failed_k;
    }
    else if (error.status == ustore_success_k && (presences[0] & 1)) {
        *collection = ids[0];
        verdict = ustore_py_naming_found_k;
    }

    ustore_py_arena_return(context->database, arena);
    return verdict;
}

int ustore_py_collection_from_object(ustore_py_context_t const* context, PyObject* object,
                                     ustore_collection_t* collection) {
    if (!object || object == Py_None) {
        *collection = ustore_collection_main_k;
        return 0;
    }
    if (PyUnicode_Check(object)) {
        Py_ssize_t length = 0;
        char const* const name = PyUnicode_AsUTF8AndSize(object, &length);
        if (!name) return -1;
        if (length == 0) {
            *collection = ustore_collection_main_k;
            return 0;
        }
        switch (ustore_py_collection_named(context, name, (size_t)length, collection)) {
        case ustore_py_naming_found_k: return 0;
        case ustore_py_naming_failed_k: return -1;
        case ustore_py_naming_missing_k:
            return ustore_py_raise(ustore_error_not_found_k, "No collection is named %R", object);
        }
    }
    if (!PyIndex_Check(object)) {
        PyErr_Format(PyExc_TypeError, "A collection is an identifier, a name, or None, not %S", Py_TYPE(object));
        return -1;
    }
    PyObject* const integer = PyNumber_Index(object);
    if (!integer) return -1;
    unsigned long long const identifier = PyLong_AsUnsignedLongLong(integer);
    Py_DECREF(integer);
    if (identifier == (unsigned long long)-1 && PyErr_Occurred())
        return ustore_py_raise_from_cause(PyExc_OverflowError, "A collection identifier must fit 64 unsigned bits");
    *collection = (ustore_collection_t)identifier;
    return 0;
}

#pragma endregion Lookups

#pragma region Walks

PyObject* ustore_py_collection_names(ustore_py_context_t const* context, char const* after, size_t after_length,
                                     size_t limit) {
    ustore_collection_t const names = ustore_collection_names_k;
    char const* const prefix = "";
    ustore_length_t const prefix_length = 0;
    ustore_length_t const resume_length = (ustore_length_t)after_length;
    ustore_length_t const count_limit = (ustore_length_t)limit;
    ustore_length_t* counts = NULL;
    ustore_length_t* strings_offsets = NULL;
    ustore_char_t* strings = NULL;
    ustore_error_t error = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context->database);

    ustore_strings_scan_t scan = {0};
    scan.db = context->database->handle;
    scan.error = &error;
    scan.transaction = context->transaction;
    scan.snapshot = context->snapshot;
    scan.arena = &arena;
    scan.options = context->options;
    scan.tasks_count = 1;
    scan.collections = &names;
    scan.prefixes = &prefix;
    scan.prefixes_lengths = &prefix_length;
    scan.start_strings = after ? &after : NULL;
    scan.start_strings_lengths = after ? &resume_length : NULL;
    scan.count_limits = &count_limit;
    scan.counts = &counts;
    scan.strings_offsets = &strings_offsets;
    scan.strings = &strings;

    Py_BEGIN_ALLOW_THREADS;
    ustore_strings_scan(&scan);
    Py_END_ALLOW_THREADS;

    if (error.status != ustore_success_k) {
        ustore_py_arena_return(context->database, arena);
        // An engine without collections holds no names, which is an empty listing rather than a fault.
        if (error.status == ustore_error_unsupported_k) return PyTuple_New(0);
        ustore_py_raise_on_error(&error);
        return NULL;
    }

    // The names are copied out here rather than borrowed: every one is a `str`, which owns its own
    // bytes whatever the arena does, so the arena goes back to the database immediately.
    size_t const found = counts[0];
    PyObject* const listed = PyTuple_New((Py_ssize_t)found);
    if (listed)
        for (size_t name_index = 0; name_index != found; ++name_index) {
            ustore_length_t const begins = strings_offsets[name_index];
            ustore_length_t const ends = strings_offsets[name_index + 1];
            PyObject* const name = PyUnicode_DecodeUTF8(strings + begins, (Py_ssize_t)(ends - begins), "strict");
            if (!name) {
                Py_DECREF(listed);
                ustore_py_arena_return(context->database, arena);
                return NULL;
            }
            PyTuple_SET_ITEM(listed, (Py_ssize_t)name_index, name);
        }

    ustore_py_arena_return(context->database, arena);
    return listed;
}

#pragma endregion Walks
