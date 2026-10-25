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
// Python.h must precede every system header, and brings `qsort` and `memcmp` along
#include "lib.h"

#pragma region Lookups

ustore_py_naming_t ustore_py_collection_named(ustore_py_context_t const* context, char const* name, size_t length,
                                              ustore_collection_id_t* collection, ustore_modality_t* modality) {
    ustore_bytes_view_t const text = {(uint8_t const*)name, (ustore_size_t)length};
    ustore_octet_t const* presences = NULL;
    ustore_collection_id_t const* found = NULL;
    ustore_modality_t const* held = NULL;
    ustore_outcome_t outcome = {0};

    // Resolving a name is shorthand inside every verb that takes one, so it reads into a one-off arena.
    ustore_arena_t arena = NULL;
    ustore_arena_init_t init = {.arena = &arena, .outcome = &outcome};
    ustore_arena_init(&init);
    if (ustore_py_raise_on_outcome(&outcome) != 0) return ustore_py_naming_failed_k;
    ustore_collection_find_t find = {
        .database = context->database_handle,
        .outcome = &outcome,
        .snapshot = context->snapshot,
        .count = 1,
        .names = ustore_py_strings_repeated(&text),
        .presences = &presences,
        .collections = &found,
        .modalities = &held,
        .arena = arena,
    };
    Py_BEGIN_ALLOW_THREADS;
    ustore_collection_find(&find);
    Py_END_ALLOW_THREADS;

    ustore_py_naming_t naming = ustore_py_naming_failed_k;
    if (ustore_py_raise_on_outcome(&outcome) == 0) naming = ustore_py_naming_missing_k;
    if (naming == ustore_py_naming_missing_k && ustore_py_present(presences, 0)) {
        naming = ustore_py_naming_found_k;
        *collection = found[0];
        if (modality) *modality = held[0];
    }
    ustore_arena_free(arena);
    return naming;
}

int ustore_py_collection_from_object(ustore_py_context_t const* context, PyObject* object,
                                     ustore_collection_id_t* collection) {
    if (PyUnicode_Check(object)) {
        Py_ssize_t length = 0;
        char const* const name = PyUnicode_AsUTF8AndSize(object, &length);
        if (!name) return -1;
        switch (ustore_py_collection_named(context, name, (size_t)length, collection, NULL)) {
        case ustore_py_naming_found_k: return 0;
        case ustore_py_naming_failed_k: return -1;
        case ustore_py_naming_missing_k:
            return ustore_py_raise(ustore_status_not_found_k, "No collection is named %R", object);
        }
    }
    // Only a `uuid.UUID` spells an ID here, since text already means a name
    if (!PyObject_TypeCheck(object, (PyTypeObject*)UUIDType)) {
        PyErr_Format(PyExc_TypeError, "A collection is a UUID or a name, not %S", Py_TYPE(object));
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
                                     size_t limit, PyObject* arena) {
    PyObject* claimed = NULL;
    if (ustore_py_arena_claim(arena, &claimed) != 0) return NULL;
    ustore_strings_view_t names = {0};
    ustore_size_t count = 0;
    ustore_outcome_t outcome = {0};
    ustore_collection_list_t list = {
        .database = context->database_handle,
        .outcome = &outcome,
        .names = &names,
        .written = &count,
        .arena = ustore_py_arena_handle(claimed),
    };
    Py_BEGIN_ALLOW_THREADS;
    ustore_collection_list(&list);
    Py_END_ALLOW_THREADS;

    if (ustore_py_raise_on_outcome(&outcome) != 0) {
        ustore_py_arena_unclaim(claimed);
        return NULL;
    }

    // An engine lists in an order of its own, so the named collections are gathered and sorted here
    ustore_py_name_view_t* const views = PyMem_New(ustore_py_name_view_t, count ? count : 1);
    if (!views) {
        ustore_py_arena_unclaim(claimed);
        return PyErr_NoMemory();
    }
    size_t named = 0;
    for (size_t collection_index = 0; collection_index != count; ++collection_index) {
        ustore_bytes_view_t const name = ustore_py_tape_string(&names, collection_index);
        if (!name.length) continue;
        views[named].text = (char const*)name.data;
        views[named].length = (size_t)name.length;
        ++named;
    }
    qsort(views, named, sizeof(ustore_py_name_view_t), &ustore_py_name_view_order);

    ustore_py_name_view_t const start = {after ? after : "", after ? after_length : 0};
    size_t first = 0;
    while (first != named && ustore_py_name_view_order(&views[first], &start) < 0) ++first;
    size_t const found = named - first < limit ? named - first : limit;

    // The names are copied out here rather than borrowed: every one is a `str`, which owns its own
    // bytes whatever the arena does, so the arena serves the next call at once.
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
    ustore_py_arena_unclaim(claimed);
    return listed;
}

#pragma endregion Walks
