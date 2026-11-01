/**
 *  @file python/collection.c
 *  @author Ash Vardanian
 *  @date September 28, 2026
 *  @brief Collections addressed by name, and the mapping views each one answers for its modality.
 *
 *  Indexing a receiver by name answers a @c Collection, which resolves the name once, on first use,
 *  and remembers what it found. Its @c blobs, @c docs and @c docs_json views read and write through
 *  the verb cores the flat methods share, so no keyword is parsed on the way, and they run as the
 *  receiver runs its calls. They have no place for an arena, so every read makes a one-off arena,
 *  copies what it read into Python objects and frees the arena with the call. A read of a missing
 *  collection is refused, the first write through a view creates it to hold that view's modality,
 *  and a view of another modality is refused. Collections are
 *  created at the head even from a transaction, as the ABI creates them, and never from a snapshot,
 *  which only reads.
 */
#include "lib.h"

#pragma region Collections

/** A collection a receiver addresses by name, resolved on first use and created by its first write. */
typedef struct Collection {

    /** The object header every instance carries. */
    PyObject ob_base;

    /** The receiver indexed, which every verb runs through and which this keeps alive. */
    PyObject* view;

    /** The name, a str. */
    PyObject* name;

    /** The collection the name addresses, once @c resolved. */
    ustore_collection_id_t id;

    /** What the collection holds, once @c resolved. */
    ustore_modality_t modality;

    /** Whether the name was resolved, or the collection created under it. */
    int resolved;
} Collection;

/** One of the mapping views a collection answers, which all share this shape. */
typedef struct CollectionView {

    /** The object header every instance carries. */
    PyObject ob_base;

    /** The collection the view reads and writes. */
    Collection* collection;
} CollectionView;

/** The noun a refusal names @p modality with, as in "'movies' holds docs, not a graph". */
static char const* modality_noun(ustore_modality_t modality) {
    switch (modality) {
    case ustore_modality_blobs_k: return "blobs";
    case ustore_modality_docs_k: return "docs";
    case ustore_modality_graph_k: return "a graph";
    case ustore_modality_strings_k: return "strings";
    case ustore_modality_pages_k: return "pages";
    default: return "an unknown modality";
    }
}

/** What resolving a name does when no collection carries it. */
typedef enum collection_missing_t {

    /** Raise @c NotFoundError, as every read does. */
    collection_missing_refused_k = 0,

    /** Create the collection at the head to hold the modality of the view, as the first write does. */
    collection_missing_created_k = 1,
} collection_missing_t;

/**
 *  @brief Finds the collection @p self names at the head or at the snapshot of @p context, or creates it at the head
 *      to hold @p modality.
 *
 *  @param[in] missing Whether a missing name is refused or created, where a snapshot always refuses.
 *  @param[out] id The collection the name addresses.
 *  @param[out] held What the collection holds.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
static int collection_lookup(Collection const* self, ustore_py_context_t const* context, ustore_modality_t modality,
                             collection_missing_t missing, ustore_collection_id_t* id, ustore_modality_t* held) {
    Py_ssize_t length = 0;
    char const* const name = PyUnicode_AsUTF8AndSize(self->name, &length);
    if (!name) return -1;
    ustore_py_naming_t naming = ustore_py_collection_named(context, name, (size_t)length, id, held);
    if (naming != ustore_py_naming_missing_k) return naming == ustore_py_naming_found_k ? 0 : -1;
    if (missing == collection_missing_refused_k || context->snapshot)
        return ustore_py_raise(ustore_status_not_found_k, "No collection is named %R", self->name);

    ustore_outcome_t outcome = {0};
    ustore_collection_create_t create = {0};
    create.database = context->database_handle;
    create.outcome = &outcome;
    create.name.data = (uint8_t const*)name;
    create.name.length = (ustore_size_t)length;
    create.modality = modality;
    create.collection = id;

    Py_BEGIN_ALLOW_THREADS;
    ustore_collection_create(&create);
    Py_END_ALLOW_THREADS;

    *held = modality;
    // Another writer may have created it since the lookup, and then the name addresses theirs.
    if (ustore_outcome_status(&outcome) == ustore_status_already_exists_k) {
        naming = ustore_py_collection_named(context, name, (size_t)length, id, held);
        if (naming != ustore_py_naming_missing_k) return naming == ustore_py_naming_found_k ? 0 : -1;
    }
    return ustore_py_raise_on_outcome(&outcome);
}

/**
 *  @brief Resolves @p self once, filling what a verb core runs with, whatever the collection holds.
 *  @param[in] modality What a missing collection is created to hold, when @p missing creates it.
 *  @param[in] missing Whether a missing collection is refused or created to hold @p modality.
 *  @param[out] context What the verb addresses and how it runs, as the receiver indexed names it.
 *  @param[out] options The view's own options, addressing the collection.
 *  @param[out] held What the collection holds.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
static int collection_resolve_any(Collection* self, ustore_modality_t modality, collection_missing_t missing,
                                  ustore_py_context_t* context, ustore_py_verb_options_t* options,
                                  ustore_modality_t* held) {
    if (ustore_py_context_from_object(self->view, context) != 0) return -1;
    ustore_py_verb_options_init(options);
    ustore_collection_id_t id = {{{0}}};
    int resolved = 0;
    ustore_py_critical_section_begin_((PyObject*)self);
    id = self->id;
    *held = self->modality;
    resolved = self->resolved;
    ustore_py_critical_section_end_();

    if (!resolved) {
        if (collection_lookup(self, context, modality, missing, &id, held) != 0) return -1;
        ustore_py_critical_section_begin_((PyObject*)self);
        self->id = id;
        self->modality = *held;
        self->resolved = 1;
        ustore_py_critical_section_end_();
    }
    options->collection = id;
    return 0;
}

/** Resolves @p self once, refusing a missing collection, whatever it holds. */
static int collection_found(Collection* self, ustore_py_context_t* context, ustore_py_verb_options_t* options,
                            ustore_modality_t* held) {
    return collection_resolve_any(self, ustore_modality_blobs_k, collection_missing_refused_k, context, options, held);
}

/**
 *  @brief Resolves @p self once and checks it holds what a view reads, filling what a verb core runs with.
 *  @param[in] modality What the view reads and writes.
 *  @param[in] missing Whether a missing collection is refused or created to hold @p modality.
 *  @param[out] context What the verb addresses and how it runs, as the receiver indexed names it.
 *  @param[out] options The view's own options, addressing the collection.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
static int collection_resolve(Collection* self, ustore_modality_t modality, collection_missing_t missing,
                              ustore_py_context_t* context, ustore_py_verb_options_t* options) {
    ustore_modality_t held = modality;
    if (collection_resolve_any(self, modality, missing, context, options, &held) != 0) return -1;
    if (held == modality) return 0;
    return ustore_py_raise(ustore_status_invalid_argument_k, "%R holds %s, not %s", self->name, modality_noun(held),
                           modality_noun(modality));
}

PyObject* ustore_py_collection_subscript(PyObject* view, PyObject* name) {
    if (!PyUnicode_Check(name)) {
        PyErr_Format(PyExc_TypeError, "A collection is named by a string, not %S", Py_TYPE(name));
        return NULL;
    }
    Collection* const collection = PyObject_New(Collection, &CollectionType);
    if (!collection) return NULL;
    collection->view = Py_NewRef(view);
    collection->name = Py_NewRef(name);
    collection->id = (ustore_collection_id_t) {{{0}}};
    collection->modality = ustore_modality_blobs_k;
    collection->resolved = 0;
    return (PyObject*)collection;
}

static void Collection_dealloc(Collection* self) {
    Py_XDECREF(self->view);
    Py_XDECREF(self->name);
    PyObject_Free(self);
}

static PyObject* Collection_repr(Collection* self) { return PyUnicode_FromFormat("Collection(%R)", self->name); }

/** Answers a new mapping view of @p type over @p self. */
static PyObject* collection_view(Collection* self, PyTypeObject* type) {
    CollectionView* const view = PyObject_New(CollectionView, type);
    if (!view) return NULL;
    view->collection = (Collection*)Py_NewRef((PyObject*)self);
    return (PyObject*)view;
}

/**
 *  @brief Answers `module.class_name(view, id)` over @p self, resolved or created to hold @p modality.
 *
 *  The module is imported first, so a view whose dependencies are missing creates no collection.
 */
static PyObject* collection_python_view(Collection* self, ustore_modality_t modality, char const* module_name,
                                        char const* class_name) {
    PyObject* const module = PyImport_ImportModule(module_name);
    PyObject* const type = module ? PyObject_GetAttrString(module, class_name) : NULL;
    Py_XDECREF(module);
    if (!type) return NULL;

    ustore_py_context_t context;
    ustore_py_verb_options_t options;
    PyObject* const id = collection_resolve(self, modality, collection_missing_created_k, &context, &options) == 0
                             ? ustore_py_uuid_to_object(&options.collection.uuid)
                             : NULL;
    PyObject* const built = id ? PyObject_CallFunctionObjArgs(type, self->view, id, NULL) : NULL;
    Py_XDECREF(id);
    Py_DECREF(type);
    return built;
}

static char const doc_Collection_blobs[] =
    "The binary values, as a mapping: `blobs[key]` reads a copy as bytes, a batch of keys a list of them.";

static PyObject* Collection_get_blobs(Collection* self, void* Py_UNUSED(closure)) {
    return collection_view(self, &BlobsViewType);
}

static char const doc_Collection_docs[] =
    "The JSON documents as Python objects, as a mapping: `docs[key]` parses one, `docs[key] = {...}` serializes one.";

static PyObject* Collection_get_docs(Collection* self, void* Py_UNUSED(closure)) {
    return collection_view(self, &DocsViewType);
}

static char const doc_Collection_docs_json[] =
    "The JSON documents as their text, as a mapping: `docs_json[key]` reads a copy of the JSON as bytes.";

static PyObject* Collection_get_docs_json(Collection* self, void* Py_UNUSED(closure)) {
    return collection_view(self, &DocsJSONViewType);
}

static char const doc_Collection_graph[] =
    "The graph, as a `ustore.graph_view.DiGraph`, creating a graph collection when the name is missing.";

static PyObject* Collection_get_graph(Collection* self, void* Py_UNUSED(closure)) {
    return collection_python_view(self, ustore_modality_graph_k, "ustore.graph_view", "DiGraph");
}

static char const doc_Collection_table[] =
    "The documents as rows, as a `ustore.table_view.Table`, creating a docs collection when the name is missing.";

static PyObject* Collection_get_table(Collection* self, void* Py_UNUSED(closure)) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self->view, &context) != 0 ||
        ustore_py_context_blocking(&context, "A table view pages through several calls",
                                   "docs_gather() or docs_write()") != 0)
        return NULL;
    return collection_python_view(self, ustore_modality_docs_k, "ustore.table_view", "Table");
}

static char const doc_Collection_name[] = "The name the collection was addressed by.";

static PyObject* Collection_get_name(Collection* self, void* Py_UNUSED(closure)) { return Py_NewRef(self->name); }

static char const doc_Collection_id[] = "The ID the name addresses, raising NotFoundError when it addresses none.";

static PyObject* Collection_get_id(Collection* self, void* Py_UNUSED(closure)) {
    ustore_py_context_t context;
    ustore_py_verb_options_t options;
    ustore_modality_t held = ustore_modality_blobs_k;
    if (collection_found(self, &context, &options, &held) != 0) return NULL;
    return ustore_py_uuid_to_object(&options.collection.uuid);
}

static char const doc_Collection_modality[] =
    "What the collection holds, such as `Modality.DOCS`, raising NotFoundError when the name addresses none.";

static PyObject* Collection_get_modality(Collection* self, void* Py_UNUSED(closure)) {
    ustore_py_context_t context;
    ustore_py_verb_options_t options;
    ustore_modality_t held = ustore_modality_blobs_k;
    if (collection_found(self, &context, &options, &held) != 0) return NULL;
    return ustore_py_enum_to_object(ustore_py_enum_modality_k, (int)held);
}

static char const doc_Collection_arrow_c_stream[] =                                          //
    "__arrow_c_stream__(requested_schema=None) answers an `arrow_array_stream` capsule.\n"   //
    "\n"                                                                                     //
    "Exports the whole collection as one Arrow stream through the receiver's view, so\n"     //
    "`pyarrow.table(db['posters'])` reads it in place: keys as `arrow.uuid`, values as\n"    //
    "binary, and a graph as `source`, `target` and `edge`. At the head the stream reads a\n" //
    "snapshot it holds until released. Every batch keeps its own memory, outliving the\n"    //
    "stream, and the requested schema is ignored, as the protocol allows.";

static PyObject* Collection_arrow_c_stream(Collection* self, PyObject* const* arguments,
                                           Py_ssize_t positional_arguments_count, PyObject* keyword_names_tuple) {
    PyObject* requested = Py_None;
    if (ustore_py_optional_argument("__arrow_c_stream__", arguments, positional_arguments_count, keyword_names_tuple, 0,
                                    "requested_schema", &requested) != 0)
        return NULL;
    ustore_py_context_t context;
    ustore_py_verb_options_t options;
    ustore_modality_t held = ustore_modality_blobs_k;
    if (collection_found(self, &context, &options, &held) != 0) return NULL;
    return ustore_py_stream_export(&context, options.collection);
}

static char const doc_Collection_sample[] =                                                     //
    "sample(wanted) answers a dict of up to `wanted` keys drawn at random onto their values.\n" //
    "\n"                                                                                        //
    "Draws the keys and then reads their values through the same view, each into a one-off\n"   //
    "arena, and copies every value into bytes, so neither arena outlives the call. A key\n"     //
    "removed between the draw and the read maps onto None. In a loop, `sample` and `read`\n"    //
    "with arenas of your own allocate nothing per call.\n"                                      //
    "\n"                                                                                        //
    "Args:\n"                                                                                   //
    "  wanted (int): Most keys to draw.\n"                                                      //
    "Returns:\n"                                                                                //
    "  dict[UUID, bytes | None]: Every key drawn, in the order drawn, onto its value.\n"        //
    "Raises:\n"                                                                                 //
    "  NotFoundError: No collection carries the name.";

static PyObject* Collection_sample(Collection* self, PyObject* wanted_object) {
    size_t wanted = 0;
    if (ustore_py_size_from_object(wanted_object, "wanted", &wanted) != 0) return NULL;
    ustore_py_context_t context;
    ustore_py_verb_options_t options;
    ustore_modality_t held = ustore_modality_blobs_k;
    if (collection_found(self, &context, &options, &held) != 0 ||
        ustore_py_context_blocking(&context, "Collection.sample() reads what it draws in a second call",
                                   "sample() and then read() on the deferred view") != 0)
        return NULL;

    ustore_executor_t const executor = ustore_py_context_executor(&context);
    PyObject* const drawing = ustore_py_arena_new(executor);
    PyObject* const drawn = drawing ? ustore_py_keys_sample(&context, &options, held, wanted, drawing) : NULL;
    Py_XDECREF(drawing);
    if (!drawn) return NULL;
    // The tape of the draw exports as 16-byte keys, which the read borrows in place from the other arena
    int const documents = held == ustore_modality_docs_k;
    ustore_py_verb_t const* const verb = documents ? &ustore_py_docs_read_drawn_verb : &ustore_py_read_drawn_verb;
    ustore_py_read_call_t call;
    ustore_py_call_open(&context, verb, &call);
    PyObject* const reading = ustore_py_arena_new(executor);
    PyObject* values = NULL;
    if (!reading || ustore_py_keys_from_object(drawn, &call.keys) != 0)
        values = ustore_py_call_discard(&context, verb, &call);
    else if (documents)
        values = ustore_py_docs_read(&context, &options, ustore_docs_field_type_json_k, verb, &call, reading);
    else values = ustore_py_values_read(&context, &options, verb, &call, reading);
    Py_XDECREF(reading);
    Py_DECREF(drawn);
    return values;
}

static char const doc_Collection_sample_keys[] =                                               //
    "sample_keys(wanted, arena) answers a Keys tape of up to `wanted` keys drawn at random.\n" //
    "\n"                                                                                       //
    "Args:\n"                                                                                  //
    "  wanted (int): Most keys to draw.\n"                                                     //
    "  arena (Arena): Where the keys land, rewound by this call.\n"                            //
    "Returns:\n"                                                                               //
    "  Keys: One run of keys, in no particular order.";

static PyObject* Collection_sample_keys(Collection* self, PyObject* const* arguments,
                                        Py_ssize_t positional_arguments_count) {
    if (ustore_py_keyword_names_count_checked("sample_keys", positional_arguments_count, NULL, 2) < 0) return NULL;
    size_t wanted = 0;
    if (ustore_py_size_from_object(arguments[0], "wanted", &wanted) != 0) return NULL;
    ustore_py_context_t context;
    ustore_py_verb_options_t options;
    ustore_modality_t held = ustore_modality_blobs_k;
    if (collection_found(self, &context, &options, &held) != 0) return NULL;
    return ustore_py_keys_sample(&context, &options, held, wanted, arguments[1]);
}

static char const doc_Collection_scan[] =                                                   //
    "scan(start, wanted, arena) answers a Keys tape of up to `wanted` keys from `start`.\n" //
    "\n"                                                                                    //
    "Scans the keys of blobs, the documents or the vertices, as the collection holds.\n"    //
    "\n"                                                                                    //
    "Args:\n"                                                                               //
    "  start (KeyLike): The smallest key the scan may return.\n"                            //
    "  wanted (int): Most keys the scan returns.\n"                                         //
    "  arena (Arena): Where the keys land, rewound by this call.\n"                         //
    "Returns:\n"                                                                            //
    "  Keys: One run of keys, in ascending order.";

static PyObject* Collection_scan(Collection* self, PyObject* const* arguments, Py_ssize_t positional_arguments_count) {
    if (ustore_py_keyword_names_count_checked("scan", positional_arguments_count, NULL, 3) < 0) return NULL;
    ustore_key_t start = {0};
    size_t wanted = 0;
    if (ustore_py_key_from_object(arguments[0], &start) != 0) return NULL;
    if (ustore_py_size_from_object(arguments[1], "wanted", &wanted) != 0) return NULL;
    ustore_py_context_t context;
    ustore_py_verb_options_t options;
    ustore_modality_t held = ustore_modality_blobs_k;
    if (collection_found(self, &context, &options, &held) != 0) return NULL;
    return ustore_py_keys_scan(&context, &options, held, &start, wanted, ustore_scan_ordered_k, NULL, arguments[2]);
}

static char const doc_Collection[] =                                                           //
    "A collection a receiver addresses by name, as `db['movies']` or `snapshot['movies']`.\n"  //
    "\n"                                                                                       //
    "The name resolves once, on first use, at the snapshot of a snapshot and at the head\n"    //
    "otherwise, raising NotFoundError on a read when no collection carries it. A write\n"      //
    "through `blobs` or `docs`, or an access of `graph` or `table`, creates a missing\n"       //
    "collection at the head to hold that modality, so a transaction never conflicts with a\n"  //
    "collection made for it. A view of another modality raises InvalidArgumentError.\n"        //
    "\n"                                                                                       //
    "Its calls run as the receiver's do. The mapping views and `sample` copy what they read\n" //
    "out of a one-off arena each, while `scan` and `sample_keys` take an Arena of yours.\n"    //
    "Through a deferred view, `blobs[key]`, `docs[key]`, `docs_json[key]`, `scan`,\n"          //
    "`sample_keys` and the reads of `graph` that take one call answer a Deferred, while an\n"  //
    "assignment, `del`, `in`, `sample` and `table` raise TypeError naming the flat verb to\n"  //
    "call instead.";

static PyMethodDef Collection_methods[] = {
    {"__arrow_c_stream__", (PyCFunction)(void (*)(void))Collection_arrow_c_stream, ustore_py_method_flags_k,
     doc_Collection_arrow_c_stream},
    {"sample", (PyCFunction)(void (*)(void))Collection_sample, METH_O, doc_Collection_sample},
    {"sample_keys", (PyCFunction)(void (*)(void))Collection_sample_keys, METH_FASTCALL, doc_Collection_sample_keys},
    {"scan", (PyCFunction)(void (*)(void))Collection_scan, METH_FASTCALL, doc_Collection_scan},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef Collection_getset[] = {
    {"blobs", (getter)Collection_get_blobs, NULL, doc_Collection_blobs, NULL},
    {"docs", (getter)Collection_get_docs, NULL, doc_Collection_docs, NULL},
    {"docs_json", (getter)Collection_get_docs_json, NULL, doc_Collection_docs_json, NULL},
    {"graph", (getter)Collection_get_graph, NULL, doc_Collection_graph, NULL},
    {"table", (getter)Collection_get_table, NULL, doc_Collection_table, NULL},
    {"name", (getter)Collection_get_name, NULL, doc_Collection_name, NULL},
    {"id", (getter)Collection_get_id, NULL, doc_Collection_id, NULL},
    {"modality", (getter)Collection_get_modality, NULL, doc_Collection_modality, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

PyTypeObject CollectionType = {
    PyVarObject_HEAD_INIT(NULL, 0) //
        .tp_name = "ustore.Collection",
    .tp_doc = doc_Collection,
    .tp_basicsize = sizeof(Collection),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = NULL,
    .tp_dealloc = (destructor)Collection_dealloc,
    .tp_repr = (reprfunc)Collection_repr,
    .tp_methods = Collection_methods,
    .tp_getset = Collection_getset,
};

#pragma endregion Collections

#pragma region Views

static PyObject* CollectionView_repr(CollectionView* self) {
    char const* const member = Py_TYPE(self) == &BlobsViewType  ? "blobs"
                               : Py_TYPE(self) == &DocsViewType ? "docs"
                                                                : "docs_json";
    return PyUnicode_FromFormat("Collection(%R).%s", self->collection->name, member);
}

static void CollectionView_dealloc(CollectionView* self) {
    Py_XDECREF(self->collection);
    PyObject_Free(self);
}

/** Answers 0 for @p answered, which a mapping protocol discards, or -1 when it is NULL with an exception set. */
static int answered_nothing(PyObject* answered) {
    if (!answered) return -1;
    Py_DECREF(answered);
    return 0;
}

/** Answers whether @p key is present in the collection of @p self, which must hold @p modality. */
static int collection_view_contains(CollectionView* self, PyObject* key, ustore_modality_t modality) {
    ustore_key_t parsed = {0};
    if (ustore_py_key_from_object(key, &parsed) != 0) return -1;
    ustore_py_context_t context;
    ustore_py_verb_options_t options;
    if (collection_resolve(self->collection, modality, collection_missing_refused_k, &context, &options) != 0 ||
        ustore_py_context_blocking(&context, "`in` answers a bool at once", "contains(key, collection=...)") != 0)
        return -1;
    PyObject* const found = modality == ustore_modality_docs_k
                                ? ustore_py_document_contains(&context, &options, &parsed)
                                : ustore_py_value_contains(&context, &options, &parsed);
    if (!found) return -1;
    Py_DECREF(found);
    return found == Py_True;
}

/** Fills @p context for a write through @p self, refusing a receiver that defers its calls or only reads. */
static int collection_view_writing(CollectionView* self, char const* operation, char const* instead,
                                   ustore_py_context_t* context) {
    if (ustore_py_context_from_object(self->collection->view, context) != 0) return -1;
    if (ustore_py_context_blocking(context, operation, instead) != 0) return -1;
    return ustore_py_context_writable(context);
}

/** Removes the keys @p key addresses from the collection of @p self, which must hold @p modality and which a
 *  removal never creates. */
static int collection_view_remove(CollectionView* self, PyObject* key, ustore_modality_t modality) {
    ustore_py_context_t context;
    if (collection_view_writing(self, "`del` discards what it answers", "remove(keys, collection=...)", &context) != 0)
        return -1;
    ustore_py_remove_call_t call;
    ustore_py_call_open(&context, &ustore_py_remove_verb, &call);
    ustore_py_verb_options_t options;
    if (ustore_py_keys_from_subscript(key, &call.keys) < 0 ||
        collection_resolve(self->collection, modality, collection_missing_refused_k, &context, &options) != 0) {
        ustore_py_call_discard(&context, &ustore_py_remove_verb, &call);
        return -1;
    }
    return answered_nothing(ustore_py_values_remove(&context, &options, modality, &call));
}

/**
 *  @brief Opens a read of @p verb through the receiver of @p self, gathering the keys @p key spells into it and
 *      resolving the collection, which must hold @p modality, into @p options.
 *  @return The call with its subscript set for one key, or NULL with an exception set.
 */
static ustore_py_read_call_t* collection_view_read_open(CollectionView* self, PyObject* key, ustore_modality_t modality,
                                                        ustore_py_verb_t const* verb, ustore_py_context_t* context,
                                                        ustore_py_verb_options_t* options,
                                                        ustore_py_read_call_t* stack) {
    if (ustore_py_context_from_object(self->collection->view, context) != 0) return NULL;
    ustore_py_read_call_t* const call = ustore_py_call_open(context, verb, stack);
    if (!call) return NULL;
    int const one = ustore_py_keys_from_subscript(key, &call->keys);
    if (one < 0 ||
        collection_resolve(self->collection, modality, collection_missing_refused_k, context, options) != 0) {
        ustore_py_call_discard(context, verb, call);
        return NULL;
    }
    if (one) call->subscript = Py_NewRef(key);
    return call;
}

static PyObject* BlobsView_subscript(CollectionView* self, PyObject* key) {
    ustore_py_context_t context;
    ustore_py_verb_options_t options;
    ustore_py_read_call_t stack;
    ustore_py_verb_t const* const verb = &ustore_py_read_copies_verb;
    ustore_py_read_call_t* const call = collection_view_read_open(self, key, ustore_modality_blobs_k, verb, &context,
                                                                  &options, &stack);
    if (!call) return NULL;
    PyObject* const arena = ustore_py_arena_new(ustore_py_context_executor(&context));
    if (!arena) return ustore_py_call_discard(&context, verb, call);
    PyObject* const answer = ustore_py_values_read(&context, &options, verb, call, arena);
    Py_DECREF(arena);
    return answer;
}

static int BlobsView_ass_subscript(CollectionView* self, PyObject* key, PyObject* value) {
    if (!value) return collection_view_remove(self, key, ustore_modality_blobs_k);
    ustore_py_context_t context;
    if (collection_view_writing(self, "Assignment discards what it answers", "write(keys, values, collection=...)",
                                &context) != 0)
        return -1;
    ustore_py_write_call_t call;
    ustore_py_call_open(&context, &ustore_py_write_verb, &call);
    int const one = ustore_py_keys_from_subscript(key, &call.keys);
    // One key takes one bytes-like value, and a batch one per key or one for every key.
    int gathered = one < 0 ? -1 : 0;
    if (one > 0) {
        gathered = ustore_py_contents_reserve(&call.contents, 1, ustore_py_contents_per_task_k);
        if (gathered == 0) gathered = ustore_py_contents_borrow(value, &call.contents);
    }
    else if (one == 0) { gathered = ustore_py_contents_from_object(value, call.keys.count, &call.contents); }

    // The values are gathered before the collection is resolved, so a refused batch creates nothing.
    ustore_py_verb_options_t options;
    if (gathered != 0 || collection_resolve(self->collection, ustore_modality_blobs_k, collection_missing_created_k,
                                            &context, &options) != 0) {
        ustore_py_call_discard(&context, &ustore_py_write_verb, &call);
        return -1;
    }
    return answered_nothing(ustore_py_values_write(&context, &options, &call));
}

static int BlobsView_contains(CollectionView* self, PyObject* key) {
    return collection_view_contains(self, key, ustore_modality_blobs_k);
}

/** Reads the documents @p key addresses from the collection of @p self into a one-off arena, answering them as
 *  @p verb copies them out. */
static PyObject* docs_view_read(CollectionView* self, PyObject* key, ustore_py_verb_t const* verb) {
    ustore_py_context_t context;
    ustore_py_verb_options_t options;
    ustore_py_read_call_t stack;
    ustore_py_read_call_t* const call = collection_view_read_open(self, key, ustore_modality_docs_k, verb, &context,
                                                                  &options, &stack);
    if (!call) return NULL;
    PyObject* const arena = ustore_py_arena_new(ustore_py_context_executor(&context));
    if (!arena) return ustore_py_call_discard(&context, verb, call);
    PyObject* const answer = ustore_py_docs_read(&context, &options, ustore_docs_field_type_json_k, verb, call, arena);
    Py_DECREF(arena);
    return answer;
}

/** Writes documents under @p key, spelled as @p spelling says, or removes them when @p documents is NULL. */
static int docs_view_write(CollectionView* self, PyObject* key, PyObject* documents,
                           ustore_py_documents_spelling_t spelling) {
    if (!documents) return collection_view_remove(self, key, ustore_modality_docs_k);
    ustore_py_context_t context;
    if (collection_view_writing(self, "Assignment discards what it answers",
                                "docs_write(keys, documents, collection=...)", &context) != 0)
        return -1;
    ustore_py_keys_t keys;
    int const one = ustore_py_keys_from_subscript(key, &keys);
    if (one < 0) return -1;
    // One key takes its document whole, even a list, so it travels as a batch of one.
    PyObject* const batch = one && spelling == ustore_py_documents_as_objects_k ? PyTuple_Pack(1, documents)
                                                                                : Py_NewRef(documents);
    ustore_py_verb_options_t options;
    int const written = batch && collection_resolve(self->collection, ustore_modality_docs_k,
                                                    collection_missing_created_k, &context, &options) == 0
                            ? ustore_py_docs_upsert(&context, &options, keys.keys, keys.count, batch, spelling)
                            : -1;
    Py_XDECREF(batch);
    ustore_py_keys_release(&keys);
    return written;
}

static PyObject* DocsView_subscript(CollectionView* self, PyObject* key) {
    return docs_view_read(self, key, &ustore_py_docs_read_objects_verb);
}

static int DocsView_ass_subscript(CollectionView* self, PyObject* key, PyObject* document) {
    return docs_view_write(self, key, document, ustore_py_documents_as_objects_k);
}

static int DocsView_contains(CollectionView* self, PyObject* key) {
    return collection_view_contains(self, key, ustore_modality_docs_k);
}

static PyObject* DocsJSONView_subscript(CollectionView* self, PyObject* key) {
    return docs_view_read(self, key, &ustore_py_docs_read_copies_verb);
}

static int DocsJSONView_ass_subscript(CollectionView* self, PyObject* key, PyObject* text) {
    return docs_view_write(self, key, text, ustore_py_documents_as_json_k);
}

static char const doc_BlobsView[] =                                                            //
    "The binary values of a collection, as a mapping.\n"                                       //
    "\n"                                                                                       //
    "`blobs[key]` reads into a one-off arena and answers a copy as bytes, raising KeyError\n"  //
    "when the key is absent, and a batch of keys answers a list with None in place of every\n" //
    "absent key; `read` with an arena of yours answers zero-copy Values instead.\n"            //
    "`blobs[key] = value` takes anything bytes-like, a batch takes one value per key or one\n" //
    "for every key, and `del blobs[key]` removes.";

static PyMappingMethods BlobsView_as_mapping = {
    .mp_subscript = (binaryfunc)BlobsView_subscript,
    .mp_ass_subscript = (objobjargproc)BlobsView_ass_subscript,
};

static PySequenceMethods BlobsView_as_sequence = {
    .sq_contains = (objobjproc)BlobsView_contains,
};

PyTypeObject BlobsViewType = {
    PyVarObject_HEAD_INIT(NULL, 0) //
        .tp_name = "ustore.BlobsView",
    .tp_doc = doc_BlobsView,
    .tp_basicsize = sizeof(CollectionView),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = NULL,
    .tp_dealloc = (destructor)CollectionView_dealloc,
    .tp_repr = (reprfunc)CollectionView_repr,
    .tp_as_sequence = &BlobsView_as_sequence,
    .tp_as_mapping = &BlobsView_as_mapping,
};

static char const doc_DocsView[] =                                                             //
    "The JSON documents of a collection as Python objects, as a mapping.\n"                    //
    "\n"                                                                                       //
    "`docs[key]` parses the document into dicts, lists, strings, numbers, booleans and\n"      //
    "None, raising KeyError when the key is absent, and a batch of keys answers a list with\n" //
    "None where a document is absent. `docs[key] = document` serializes any such object,\n"    //
    "so a string is stored as a JSON string, and `del docs[key]` removes.";

static PyMappingMethods DocsView_as_mapping = {
    .mp_subscript = (binaryfunc)DocsView_subscript,
    .mp_ass_subscript = (objobjargproc)DocsView_ass_subscript,
};

static PySequenceMethods DocsView_as_sequence = {
    .sq_contains = (objobjproc)DocsView_contains,
};

PyTypeObject DocsViewType = {
    PyVarObject_HEAD_INIT(NULL, 0) //
        .tp_name = "ustore.DocsView",
    .tp_doc = doc_DocsView,
    .tp_basicsize = sizeof(CollectionView),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = NULL,
    .tp_dealloc = (destructor)CollectionView_dealloc,
    .tp_repr = (reprfunc)CollectionView_repr,
    .tp_as_sequence = &DocsView_as_sequence,
    .tp_as_mapping = &DocsView_as_mapping,
};

static char const doc_DocsJSONView[] =                                                         //
    "The JSON documents of a collection as their text, as a mapping.\n"                        //
    "\n"                                                                                       //
    "`docs_json[key]` answers a copy of the stored JSON as bytes, raising KeyError when the\n" //
    "key is absent, and a batch of keys answers a list with None in place of every absent\n"   //
    "key. `docs_json[key] = text` stores JSON text or bytes as they are, and\n"                //
    "`del docs_json[key]` removes.";

static PyMappingMethods DocsJSONView_as_mapping = {
    .mp_subscript = (binaryfunc)DocsJSONView_subscript,
    .mp_ass_subscript = (objobjargproc)DocsJSONView_ass_subscript,
};

PyTypeObject DocsJSONViewType = {
    PyVarObject_HEAD_INIT(NULL, 0) //
        .tp_name = "ustore.DocsJSONView",
    .tp_doc = doc_DocsJSONView,
    .tp_basicsize = sizeof(CollectionView),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = NULL,
    .tp_dealloc = (destructor)CollectionView_dealloc,
    .tp_repr = (reprfunc)CollectionView_repr,
    .tp_as_sequence = &DocsView_as_sequence,
    .tp_as_mapping = &DocsJSONView_as_mapping,
};

#pragma endregion Views
