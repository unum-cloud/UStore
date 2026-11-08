/**
 *  @file python/strings.c
 *  @author Ash Vardanian
 *  @date September 16, 2026
 *  @brief Collection names, resolved onto identifiers and listed in text order, plus strings verbs.
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

#pragma region Verbs

/** One strings call: the batch or bounds it borrows until it finishes, and its verb's outputs. */
typedef struct strings_call_t {

    /** What the call keeps until it finishes. */
    ustore_py_call_t call;

    /** The strings of a write, a read or a removal. */
    ustore_py_texts_t texts;

    /** The keys a write maps the strings onto. */
    ustore_py_keys_t keys;

    /** The prefix, the start and the end of a walk, held, or NULL where it names none. */
    PyObject* held[3];

    /** The bytes of @c held, which an accessor repeats for every task. */
    ustore_bytes_view_t bounds[3];

    /** Direction of a walk or a measurement. */
    ustore_scan_direction_t direction;

    /** Output slot for the presence bits of a read, inside the arena. */
    ustore_octet_t const* presences;

    /** Output slot for the keys of a read, inside the arena. */
    ustore_key_t const* values;

    /** Output slot for the offsets around the strings of a scan or a sample, inside the arena. */
    ustore_length_t const* offsets;

    /** Output slot for the strings of a scan or a sample, inside the arena. */
    ustore_strings_view_t strings;

    /** Output slots for the six bounds of a measurement, in the order of @c MeasurementType. */
    ustore_size_t const* limits[6];
} strings_call_t;

static void strings_release(void* erased) {
    strings_call_t* const call = (strings_call_t*)erased;
    ustore_py_texts_release(&call->texts);
    ustore_py_keys_release(&call->keys);
    for (size_t index = 0; index != 3; ++index) Py_CLEAR(call->held[index]);
    ustore_py_call_release(&call->call);
}

/** Holds the text or bytes @p object as the view it lends the library, empty for None. */
static int strings_bound_from_object(PyObject* object, char const* field, PyObject** held, ustore_bytes_view_t* view) {
    if (!object) return 0;
    Py_ssize_t length = 0;
    char const* data = NULL;
    if (PyUnicode_Check(object)) data = PyUnicode_AsUTF8AndSize(object, &length);
    else if (PyBytes_Check(object)) PyBytes_AsStringAndSize(object, (char**)&data, &length);
    else {
        PyErr_Format(PyExc_TypeError, "'%s' takes a str, bytes or None, got %S", field, Py_TYPE(object));
        return -1;
    }
    if (!data) return -1;
    *held = Py_NewRef(object);
    view->data = (ustore_byte_t const*)data;
    view->length = (ustore_size_t)length;
    return 0;
}

/** The view of the bound @p index, which a zeroed one stands in for when the call named none. */
static ustore_strings_view_t strings_bound(strings_call_t const* call, size_t index) {
    ustore_strings_view_t const none = {0};
    return call->held[index] ? ustore_py_strings_repeated(&call->bounds[index]) : none;
}

/** Decodes @p count strings of the tape @p strings, one @c str each, into a tuple. */
static PyObject* strings_tuple(ustore_strings_view_t const* strings, size_t count) {
    PyObject* const listed = PyTuple_New((Py_ssize_t)count);
    for (size_t index = 0; listed && index != count; ++index) {
        ustore_bytes_view_t const view = ustore_py_tape_string(strings, index);
        PyObject* const text = PyUnicode_DecodeUTF8((char const*)view.data, (Py_ssize_t)view.length, "strict");
        if (!text) {
            Py_DECREF(listed);
            return NULL;
        }
        PyTuple_SET_ITEM(listed, (Py_ssize_t)index, text);
    }
    return listed;
}

static void strings_write_submit(void* arguments) { ustore_strings_write((ustore_strings_write_t*)arguments); }
static void strings_remove_submit(void* arguments) { ustore_strings_remove((ustore_strings_remove_t*)arguments); }
static void strings_read_submit(void* arguments) { ustore_strings_read((ustore_strings_read_t*)arguments); }
static void strings_scan_submit(void* arguments) { ustore_strings_scan((ustore_strings_scan_t*)arguments); }
static void strings_sample_submit(void* arguments) { ustore_strings_sample((ustore_strings_sample_t*)arguments); }
static void strings_measure_submit(void* arguments) { ustore_strings_measure((ustore_strings_measure_t*)arguments); }

static PyObject* strings_read_answer(void* erased) {
    strings_call_t const* const call = (strings_call_t const*)erased;
    size_t const count = call->texts.contents.count;
    PyObject* const keys = PyTuple_New((Py_ssize_t)count);
    for (size_t index = 0; keys && index != count; ++index) {
        PyObject* const key = ustore_py_present(call->presences, index)
                                  ? ustore_py_uuid_to_object(&call->values[index].uuid)
                                  : Py_NewRef(Py_None);
        if (!key) {
            Py_DECREF(keys);
            return NULL;
        }
        PyTuple_SET_ITEM(keys, (Py_ssize_t)index, key);
    }
    return keys;
}

static PyObject* strings_listing_answer(void* erased) {
    strings_call_t const* const call = (strings_call_t const*)erased;
    return strings_tuple(&call->strings, call->offsets ? (size_t)call->offsets[1] : 0);
}

static PyObject* strings_measure_answer(void* erased) {
    strings_call_t const* const call = (strings_call_t const*)erased;
    PyObject* bounds = PyStructSequence_New(MeasurementType);
    for (Py_ssize_t index = 0; bounds && index != 6; ++index) {
        PyObject* const value = PyLong_FromUnsignedLongLong(call->limits[index] ? call->limits[index][0] : 0);
        if (!value) Py_CLEAR(bounds);
        else PyStructSequence_SetItem(bounds, index, value);
    }
    return bounds;
}

static ustore_py_verb_t const strings_write_verb = {sizeof(strings_call_t), strings_write_submit,
                                                    ustore_py_call_answer_none, strings_release};
static ustore_py_verb_t const strings_remove_verb = {sizeof(strings_call_t), strings_remove_submit,
                                                     ustore_py_call_answer_none, strings_release};
static ustore_py_verb_t const strings_read_verb = {sizeof(strings_call_t), strings_read_submit, strings_read_answer,
                                                   strings_release};
static ustore_py_verb_t const strings_scan_verb = {sizeof(strings_call_t), strings_scan_submit, strings_listing_answer,
                                                   strings_release};
static ustore_py_verb_t const strings_sample_verb = {sizeof(strings_call_t), strings_sample_submit,
                                                     strings_listing_answer, strings_release};
static ustore_py_verb_t const strings_measure_verb = {sizeof(strings_call_t), strings_measure_submit,
                                                      strings_measure_answer, strings_release};

char const doc_strings_write[] =                                                                  //
    "strings_write(strings, keys, *, collection, durability=Durability.BUFFERED) answers None.\n" //
    "\n"                                                                                          //
    "Upserts a batch of strings and the keys they map onto, one key per string. Every string\n"   //
    "is borrowed for the length of the call rather than copied.\n"                                //
    "\n"                                                                                          //
    "Args:\n"                                                                                     //
    "  strings (Sequence[str | bytes-like] | str | bytes-like | tape): One string per key, a\n"   //
    "    tape of them, or one string broadcast to every key.\n"                                   //
    "  keys (Sequence[KeyLike] | buffer): The key every string maps onto.\n"                      //
    "  collection (UUID | str): The strings collection, by identifier or by name.\n"              //
    "  durability (Durability, optional): `Durability.FLUSHED` reaches the drive before\n"        //
    "    returning.\n"                                                                            //
    "Raises:\n"                                                                                   //
    "  InvalidArgumentError: The batch holds another number of keys than strings, or the\n"       //
    "    collection holds another modality.";

PyObject* ustore_py_api_strings_write(PyObject* self, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                                      PyObject* keyword_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0 || ustore_py_context_writable(&context) != 0) return NULL;
    if (ustore_py_keyword_names_count_checked("strings_write", positional_arguments_count, keyword_names_tuple, 2) < 0)
        return NULL;
    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, arguments, positional_arguments_count, keyword_names_tuple,
                                             &options) != 0)
        return NULL;

    strings_call_t stack;
    strings_call_t* const call = ustore_py_call_open(&context, &strings_write_verb, &stack);
    if (!call) return NULL;
    if (ustore_py_keys_from_object(arguments[1], &call->keys) != 0 ||
        ustore_py_texts_from_object(arguments[0], "strings", NULL, NULL, &call->texts) != 0)
        return ustore_py_call_discard(&context, &strings_write_verb, call);
    ustore_py_contents_t const* const contents = &call->texts.contents;
    if (contents->spread != ustore_py_contents_broadcast_k && contents->count != call->keys.count) {
        ustore_py_raise(ustore_status_invalid_argument_k, "Got %zu strings for %zu keys", contents->count,
                        call->keys.count);
        return ustore_py_call_discard(&context, &strings_write_verb, call);
    }
    call->call.collection = options.collection;
    ustore_strings_write_t write = {
        .database = context.database_handle,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->call.outcome,
        .completion = context.completion,
        .transaction = context.transaction,
        .count = call->keys.count,
        .collections = {&call->call.collection, 0},
        .strings = contents->view,
        .values = {call->keys.keys, sizeof(ustore_key_t)},
        .durability = options.durability,
        .watch = context.watch,
    };
    return ustore_py_call_run(&context, &strings_write_verb, call, &write);
}

char const doc_strings_remove[] =                                                                //
    "strings_remove(strings, *, collection, durability=Durability.BUFFERED) answers None.\n"     //
    "\n"                                                                                         //
    "Removes a batch of strings with their keys, whether or not the collection holds them.\n"    //
    "\n"                                                                                         //
    "Args:\n"                                                                                    //
    "  strings (Sequence[str | bytes-like] | str | bytes-like | tape): The strings to remove.\n" //
    "  collection (UUID | str): The strings collection, by identifier or by name.\n"             //
    "  durability (Durability, optional): `Durability.FLUSHED` reaches the drive before\n"       //
    "    returning.";

PyObject* ustore_py_api_strings_remove(PyObject* self, PyObject* const* arguments,
                                       Py_ssize_t positional_arguments_count, PyObject* keyword_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0 || ustore_py_context_writable(&context) != 0) return NULL;
    if (ustore_py_keyword_names_count_checked("strings_remove", positional_arguments_count, keyword_names_tuple, 1) < 0)
        return NULL;
    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, arguments, positional_arguments_count, keyword_names_tuple,
                                             &options) != 0)
        return NULL;

    strings_call_t stack;
    strings_call_t* const call = ustore_py_call_open(&context, &strings_remove_verb, &stack);
    if (!call) return NULL;
    if (ustore_py_texts_from_object(arguments[0], "strings", NULL, NULL, &call->texts) != 0)
        return ustore_py_call_discard(&context, &strings_remove_verb, call);
    call->call.collection = options.collection;
    ustore_strings_remove_t remove = {
        .database = context.database_handle,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->call.outcome,
        .completion = context.completion,
        .transaction = context.transaction,
        .count = call->texts.contents.count,
        .collections = {&call->call.collection, 0},
        .strings = call->texts.contents.view,
        .durability = options.durability,
        .watch = context.watch,
    };
    return ustore_py_call_run(&context, &strings_remove_verb, call, &remove);
}

char const doc_strings_read[] =                                                                //
    "strings_read(strings, arena, *, collection) answers a tuple of keys.\n"                   //
    "\n"                                                                                       //
    "Reads the keys a batch of strings map onto.\n"                                            //
    "\n"                                                                                       //
    "Args:\n"                                                                                  //
    "  strings (Sequence[str | bytes-like] | str | bytes-like | tape): The strings to read.\n" //
    "  arena (Arena): Where the keys land before they are copied out, rewound by this\n"       //
    "    call.\n"                                                                              //
    "  collection (UUID | str): The strings collection, by identifier or by name.\n"           //
    "Returns:\n"                                                                               //
    "  tuple[UUID | None, ...]: The key of every string, in order, or None for a string\n"     //
    "  the collection does not hold.";

PyObject* ustore_py_api_strings_read(PyObject* self, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                                     PyObject* keyword_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_keyword_names_count_checked("strings_read", positional_arguments_count, keyword_names_tuple, 2) < 0)
        return NULL;
    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, arguments, positional_arguments_count, keyword_names_tuple,
                                             &options) != 0)
        return NULL;

    strings_call_t stack;
    strings_call_t* const call = ustore_py_call_open(&context, &strings_read_verb, &stack);
    if (!call) return NULL;
    if (ustore_py_texts_from_object(arguments[0], "strings", NULL, NULL, &call->texts) != 0 ||
        ustore_py_arena_claim(arguments[1], &call->call.arena) != 0)
        return ustore_py_call_discard(&context, &strings_read_verb, call);
    call->call.collection = options.collection;
    ustore_strings_read_t read = {
        .database = context.database_handle,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->call.outcome,
        .completion = context.completion,
        .view = context.view,
        .count = call->texts.contents.count,
        .collections = {&call->call.collection, 0},
        .strings = call->texts.contents.view,
        .presences = &call->presences,
        .values = &call->values,
        .arena = ustore_py_arena_handle(call->call.arena),
    };
    return ustore_py_call_run(&context, &strings_read_verb, call, &read);
}

char const doc_strings_scan[] =                                                                  //
    "strings_scan(prefix, wanted, arena, *, collection, start=None, end=None,\n"                 //
    "             direction=ScanDirection.ASCENDING) answers a tuple of strings.\n"              //
    "\n"                                                                                         //
    "Lists up to `wanted` strings starting with `prefix`, in the order of their bytes. Keys\n"   //
    "are not exported, so a `strings_read` follows the scan, and a scan resumes from `start`.\n" //
    "\n"                                                                                         //
    "Args:\n"                                                                                    //
    "  prefix (str | bytes | None): What every string starts with, or None for every string.\n"  //
    "  wanted (int): Most strings the scan returns.\n"                                           //
    "  arena (Arena): Where the strings land before they are copied out, rewound by this\n"      //
    "    call.\n"                                                                                //
    "  collection (UUID | str): The strings collection, by identifier or by name.\n"             //
    "  start (str | bytes, optional): The first string the scan may return, or None for the\n"   //
    "    first one in its direction.\n"                                                          //
    "  end (str | bytes, optional): The string the scan stops before, or None for no bound.\n"   //
    "  direction (ScanDirection, optional): `ScanDirection.DESCENDING` walks down from\n"        //
    "    `start`, largest string first.\n"                                                       //
    "Returns:\n"                                                                                 //
    "  tuple[str, ...]: The strings, in the order of the direction.\n"                           //
    "Raises:\n"                                                                                  //
    "  UnicodeDecodeError: A listed string is not UTF-8.";

PyObject* ustore_py_api_strings_scan(PyObject* self, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                                     PyObject* keyword_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_keyword_names_count_checked("strings_scan", positional_arguments_count, keyword_names_tuple, 3) < 0)
        return NULL;
    static char const* const names[] = {"start", "end", "direction", NULL};
    PyObject* keywords[3] = {NULL, NULL, NULL};
    ustore_py_verb_options_t options;
    if (ustore_py_verb_keywords_from_arguments(&context, arguments, positional_arguments_count, keyword_names_tuple,
                                               names, &options, keywords) != 0)
        return NULL;
    int direction = ustore_scan_ascending_k;
    if (keywords[2] &&
        ustore_py_enum_from_object(keywords[2], "direction", ustore_py_enum_scan_direction_k, &direction) != 0)
        return NULL;
    size_t wanted = 0;
    if (ustore_py_size_from_object(arguments[1], "wanted", &wanted) != 0) return NULL;

    strings_call_t stack;
    strings_call_t* const call = ustore_py_call_open(&context, &strings_scan_verb, &stack);
    if (!call) return NULL;
    PyObject* const prefix = arguments[0] == Py_None ? NULL : arguments[0];
    if (strings_bound_from_object(prefix, "prefix", &call->held[0], &call->bounds[0]) != 0 ||
        strings_bound_from_object(keywords[0], "start", &call->held[1], &call->bounds[1]) != 0 ||
        strings_bound_from_object(keywords[1], "end", &call->held[2], &call->bounds[2]) != 0 ||
        ustore_py_arena_claim(arguments[2], &call->call.arena) != 0)
        return ustore_py_call_discard(&context, &strings_scan_verb, call);
    call->call.collection = options.collection;
    ustore_strings_scan_t scan = {
        .database = context.database_handle,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->call.outcome,
        .completion = context.completion,
        .view = context.view,
        .count = 1,
        .collections = {&call->call.collection, 0},
        .prefixes = strings_bound(call, 0),
        .start_strings = strings_bound(call, 1),
        .end_strings = strings_bound(call, 2),
        .direction = (ustore_scan_direction_t)direction,
        .offsets = &call->offsets,
        .strings = &call->strings,
        .arena = ustore_py_arena_handle(call->call.arena),
        .wanted = wanted,
    };
    return ustore_py_call_run(&context, &strings_scan_verb, call, &scan);
}

char const doc_strings_sample[] =                                                           //
    "strings_sample(wanted, arena, *, collection) answers a tuple of strings.\n"            //
    "\n"                                                                                    //
    "Draws strings uniformly at random from one collection.\n"                              //
    "\n"                                                                                    //
    "Args:\n"                                                                               //
    "  wanted (int): Most strings to draw.\n"                                               //
    "  arena (Arena): Where the strings land before they are copied out, rewound by this\n" //
    "    call.\n"                                                                           //
    "  collection (UUID | str): The strings collection, by identifier or by name.\n"        //
    "Returns:\n"                                                                            //
    "  tuple[str, ...]: The strings, in no particular order.";

PyObject* ustore_py_api_strings_sample(PyObject* self, PyObject* const* arguments,
                                       Py_ssize_t positional_arguments_count, PyObject* keyword_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_keyword_names_count_checked("strings_sample", positional_arguments_count, keyword_names_tuple, 2) < 0)
        return NULL;
    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, arguments, positional_arguments_count, keyword_names_tuple,
                                             &options) != 0)
        return NULL;
    size_t wanted = 0;
    if (ustore_py_size_from_object(arguments[0], "wanted", &wanted) != 0) return NULL;

    strings_call_t stack;
    strings_call_t* const call = ustore_py_call_open(&context, &strings_sample_verb, &stack);
    if (!call) return NULL;
    if (ustore_py_arena_claim(arguments[1], &call->call.arena) != 0)
        return ustore_py_call_discard(&context, &strings_sample_verb, call);
    call->call.collection = options.collection;
    ustore_strings_sample_t sample = {
        .database = context.database_handle,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->call.outcome,
        .completion = context.completion,
        .view = context.view,
        .count = 1,
        .collections = {&call->call.collection, 0},
        .offsets = &call->offsets,
        .strings = &call->strings,
        .arena = ustore_py_arena_handle(call->call.arena),
        .wanted = wanted,
    };
    return ustore_py_call_run(&context, &strings_sample_verb, call, &sample);
}

char const doc_strings_measure[] =                                                            //
    "strings_measure(start, end, arena, *, collection,\n"                                     //
    "                direction=ScanDirection.ASCENDING) answers a Measurement.\n"             //
    "\n"                                                                                      //
    "Estimates what a range of strings holds without walking it, so the answers are\n"        //
    "bounds rather than counts.\n"                                                            //
    "\n"                                                                                      //
    "Args:\n"                                                                                 //
    "  start (str | bytes | None): First string of the range, or None for the first one in\n" //
    "    the direction.\n"                                                                    //
    "  end (str | bytes | None): The string the range stops before, or None for no bound.\n"  //
    "  arena (Arena): Where the bounds land before they are copied out, rewound by this\n"    //
    "    call.\n"                                                                             //
    "  collection (UUID | str): The strings collection, by identifier or by name.\n"          //
    "  direction (ScanDirection, optional): The way the range runs from `start`.\n"           //
    "Returns:\n"                                                                              //
    "  Measurement: The lower and upper bounds on the strings, the bytes of their values,\n"  //
    "  and the space they take on disk.";

PyObject* ustore_py_api_strings_measure(PyObject* self, PyObject* const* arguments,
                                        Py_ssize_t positional_arguments_count, PyObject* keyword_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_keyword_names_count_checked("strings_measure", positional_arguments_count, keyword_names_tuple, 3) <
        0)
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

    strings_call_t stack;
    strings_call_t* const call = ustore_py_call_open(&context, &strings_measure_verb, &stack);
    if (!call) return NULL;
    PyObject* const start = arguments[0] == Py_None ? NULL : arguments[0];
    PyObject* const end = arguments[1] == Py_None ? NULL : arguments[1];
    if (strings_bound_from_object(start, "start", &call->held[1], &call->bounds[1]) != 0 ||
        strings_bound_from_object(end, "end", &call->held[2], &call->bounds[2]) != 0 ||
        ustore_py_arena_claim(arguments[2], &call->call.arena) != 0)
        return ustore_py_call_discard(&context, &strings_measure_verb, call);
    call->call.collection = options.collection;
    ustore_strings_measure_t measure = {
        .database = context.database_handle,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->call.outcome,
        .completion = context.completion,
        .view = context.view,
        .count = 1,
        .collections = {&call->call.collection, 0},
        .start_strings = strings_bound(call, 1),
        .end_strings = strings_bound(call, 2),
        .direction = (ustore_scan_direction_t)direction,
        .min_cardinalities = &call->limits[0],
        .max_cardinalities = &call->limits[1],
        .min_value_bytes = &call->limits[2],
        .max_value_bytes = &call->limits[3],
        .min_space_usages = &call->limits[4],
        .max_space_usages = &call->limits[5],
        .arena = ustore_py_arena_handle(call->call.arena),
    };
    return ustore_py_call_run(&context, &strings_measure_verb, call, &measure);
}

#pragma endregion Verbs
