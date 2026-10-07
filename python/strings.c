/**
 *  @file python/strings.c
 *  @author Ash Vardanian
 *  @date September 16, 2026
 *  @brief Collection names, resolved onto identifiers and listed in the order of their text.
 *
 *  Names resolve in two phases: a name maps onto a collection identifier once here, and every
 *  other verb addresses that collection by the identifier, so no other call carries a name. Each
 *  engine keeps the names apart from every collection, so no identifier reaches them.
 */
#include <stdlib.h> // `qsort`
#include <string.h> // `memcmp`

#include "lib.h"

#pragma region Lookups

ustore_py_naming_t ustore_py_collection_named(ustore_py_context_t const* context, ustore_snapshot_t snapshot,
                                              char const* name, size_t length, ustore_collection_id_t* collection,
                                              ustore_modality_t* modality) {
    ustore_length_t const name_length = (ustore_length_t)length;
    ustore_octet_t presence = 0;
    ustore_collection_id_t found = ustore_collection_main_k;
    ustore_modality_t held = ustore_modality_any_k;
    ustore_outcome_t outcome = {0};

    ustore_collection_find_t find = {0};
    find.database = context->database->handle;
    find.outcome = &outcome;
    find.snapshot = snapshot;
    find.count = 1;
    find.names = &name;
    find.names_lengths = &name_length;
    find.presences = &presence;
    find.collections = &found;
    find.collections_stride = sizeof(ustore_collection_id_t);
    find.modalities = &held;
    find.modalities_stride = sizeof(ustore_modality_t);

    Py_BEGIN_ALLOW_THREADS;
    ustore_collection_find(&find);
    Py_END_ALLOW_THREADS;

    if (ustore_py_raise_on_outcome(&outcome) != 0) return ustore_py_naming_failed_k;
    if (!(presence & 1)) return ustore_py_naming_missing_k;
    *collection = found;
    if (modality) *modality = held;
    return ustore_py_naming_found_k;
}

int ustore_py_collection_from_object(ustore_py_context_t const* context, ustore_snapshot_t snapshot, PyObject* object,
                                     ustore_collection_id_t* collection) {
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
        switch (ustore_py_collection_named(context, snapshot, name, (size_t)length, collection, NULL)) {
        case ustore_py_naming_found_k: return 0;
        case ustore_py_naming_failed_k: return -1;
        case ustore_py_naming_missing_k:
            return ustore_py_raise(ustore_status_not_found_k, "No collection is named %R", object);
        }
    }
    // Only a `uuid.UUID` spells an ID here, since text already means a name
    if (!PyObject_TypeCheck(object, (PyTypeObject*)UUIDType)) {
        PyErr_Format(PyExc_TypeError, "A collection is a UUID, a name, or None, not %S", Py_TYPE(object));
        return -1;
    }
    return ustore_py_uuid_from_object(object, &collection->uuid);
}

#pragma endregion Lookups

#pragma region Walks

/** One recorded name, viewed inside the arena the listing exported it into. */
typedef struct ustore_py_name_view_t {
    char const* text;
    size_t length;
} ustore_py_name_view_t;

/** Orders two names by their bytes, where of two names sharing a leading run the shorter goes first. */
static int ustore_py_name_view_order(void const* first_pointer, void const* second_pointer) {
    ustore_py_name_view_t const* const first = (ustore_py_name_view_t const*)first_pointer;
    ustore_py_name_view_t const* const second = (ustore_py_name_view_t const*)second_pointer;
    size_t const shared = first->length < second->length ? first->length : second->length;
    int const ordered = shared ? memcmp(first->text, second->text, shared) : 0;
    if (ordered != 0) return ordered;
    return (first->length > second->length) - (first->length < second->length);
}

PyObject* ustore_py_collection_names(ustore_py_context_t const* context, char const* after, size_t after_length,
                                     size_t limit) {
    ustore_size_t count = 0;
    ustore_length_t* names_offsets = NULL;
    ustore_char_t* names = NULL;
    ustore_outcome_t outcome = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context->database);

    ustore_collection_list_t list = {0};
    list.database = context->database->handle;
    list.outcome = &outcome;
    list.count = &count;
    list.names_offsets = &names_offsets;
    list.names = &names;
    list.arena = &arena;

    Py_BEGIN_ALLOW_THREADS;
    ustore_collection_list(&list);
    Py_END_ALLOW_THREADS;

    if (ustore_py_raise_on_outcome(&outcome) != 0) {
        ustore_py_arena_return(context->database, arena);
        return NULL;
    }

    // An engine lists in an order of its own, so the named collections are gathered and sorted here
    ustore_py_name_view_t* const views = PyMem_New(ustore_py_name_view_t, count ? count : 1);
    if (!views) {
        ustore_py_arena_return(context->database, arena);
        return PyErr_NoMemory();
    }
    size_t named = 0;
    for (size_t collection_index = 0; collection_index != count; ++collection_index) {
        ustore_length_t const begins = names_offsets[collection_index];
        ustore_length_t const ends = names_offsets[collection_index + 1];
        if (begins == ends) continue;
        views[named].text = names + begins;
        views[named].length = ends - begins;
        ++named;
    }
    qsort(views, named, sizeof(ustore_py_name_view_t), &ustore_py_name_view_order);

    ustore_py_name_view_t const start = {after ? after : "", after ? after_length : 0};
    size_t first = 0;
    while (first != named && ustore_py_name_view_order(&views[first], &start) < 0) ++first;
    size_t const found = named - first < limit ? named - first : limit;

    // The names are copied out here rather than borrowed: every one is a `str`, which owns its own
    // bytes whatever the arena does, so the arena goes back to the database immediately.
    PyObject* listed = PyTuple_New((Py_ssize_t)found);
    for (size_t name_index = 0; listed && name_index != found; ++name_index) {
        ustore_py_name_view_t const view = views[first + name_index];
        PyObject* const name = PyUnicode_DecodeUTF8(view.text, (Py_ssize_t)view.length, "strict");
        if (!name) {
            Py_CLEAR(listed);
            break;
        }
        PyTuple_SET_ITEM(listed, (Py_ssize_t)name_index, name);
    }

    PyMem_Free(views);
    ustore_py_arena_return(context->database, arena);
    return listed;
}

#pragma endregion Walks
