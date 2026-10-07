/**
 *  @file python/collection.c
 *  @author Ash Vardanian
 *  @date September 28, 2026
 *  @brief Collections addressed by name, and the mapping views each one answers for its modality.
 *
 *  Indexing a database or a transaction by name answers a @c Collection, which resolves the name at
 *  the head once, on first use, and remembers what it found. Its @c blobs, @c docs and @c docs_json
 *  views read and write through the verb cores the flat methods share, so no keyword is parsed on
 *  the way. A read of a missing collection is refused, the first write through a view creates it to
 *  hold that view's modality, and a view of another modality is refused unless the collection holds
 *  anything. Collections are created at the head even from a transaction, as the ABI creates them.
 */
#include "lib.h"

#pragma region Collections

/** A collection a database or a transaction addresses by name, resolved on first use and created by its first write. */
typedef struct Collection {

    /** The object header every instance carries. */
    PyObject ob_base;

    /** The @c Database or @c Transaction indexed, which every verb runs through and which this keeps alive. */
    PyObject* view;

    /** The name, a str, where the empty one is the main collection. */
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
    default: return "anything";
    }
}

/** What resolving a name does when no collection carries it. */
typedef enum collection_missing_t {

    /** Raise @c MissingError, as every read does. */
    collection_missing_refused_k = 0,

    /** Create the collection at the head to hold the modality of the view, as the first write does. */
    collection_missing_created_k = 1,
} collection_missing_t;

/**
 *  @brief Finds the collection @p self names at the head or at the snapshot of @p options, or creates it at the head to
 *      hold @p modality.
 *
 *  @param[in] missing Whether a missing name is refused or created.
 *  @param[out] id The collection the name addresses.
 *  @param[out] held What the collection holds.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
static int collection_lookup(Collection const* self, ustore_py_context_t const* context,
                             ustore_py_verb_options_t const* options, ustore_modality_t modality,
                             collection_missing_t missing, ustore_collection_id_t* id, ustore_modality_t* held) {
    Py_ssize_t length = 0;
    char const* const name = PyUnicode_AsUTF8AndSize(self->name, &length);
    if (!name) return -1;
    if (length == 0) {
        *id = ustore_collection_main_k;
        *held = ustore_modality_any_k;
        return 0;
    }
    ustore_py_naming_t naming = ustore_py_collection_named(context, options->snapshot, name, (size_t)length, id, held);
    if (naming != ustore_py_naming_missing_k) return naming == ustore_py_naming_found_k ? 0 : -1;
    if (missing == collection_missing_refused_k)
        return ustore_py_raise(ustore_status_not_found_k, "No collection is named %R", self->name);

    ustore_outcome_t outcome = {0};
    ustore_collection_create_t create = {0};
    create.database = context->database->handle;
    create.outcome = &outcome;
    create.name = name;
    create.name_length = (ustore_length_t)length;
    create.modality = modality;
    create.collection = id;

    Py_BEGIN_ALLOW_THREADS;
    ustore_collection_create(&create);
    Py_END_ALLOW_THREADS;

    *held = modality;
    // Another writer may have created it since the lookup, and then the name addresses theirs.
    if (outcome.status == ustore_status_already_exists_k) {
        naming = ustore_py_collection_named(context, options->snapshot, name, (size_t)length, id, held);
        if (naming != ustore_py_naming_missing_k) return naming == ustore_py_naming_found_k ? 0 : -1;
    }
    return ustore_py_raise_on_outcome(&outcome);
}

/**
 *  @brief Resolves @p self once and checks it holds what a view reads, filling what a verb core runs with.
 *  @param[in] modality What the view reads and writes, where @c ustore_modality_any_k checks nothing.
 *  @param[in] missing Whether a missing collection is refused or created to hold @p modality.
 *  @param[out] context The database and transaction the verb runs against.
 *  @param[out] options The view's own options, addressing the collection.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
static int collection_resolve(Collection* self, ustore_modality_t modality, collection_missing_t missing,
                              ustore_py_context_t* context, ustore_py_verb_options_t* options) {
    if (ustore_py_context_from_object(self->view, context) != 0) return -1;
    ustore_py_verb_options_init(context, options);
    ustore_collection_id_t id = ustore_collection_main_k;
    ustore_modality_t held = ustore_modality_any_k;
    int resolved = 0;
    ustore_py_critical_section_begin_((PyObject*)self);
    id = self->id;
    held = self->modality;
    resolved = self->resolved;
    ustore_py_critical_section_end_();

    if (!resolved) {
        if (collection_lookup(self, context, options, modality, missing, &id, &held) != 0) return -1;
        ustore_py_critical_section_begin_((PyObject*)self);
        self->id = id;
        self->modality = held;
        self->resolved = 1;
        ustore_py_critical_section_end_();
    }
    options->collection = id;
    if (modality == ustore_modality_any_k || held == ustore_modality_any_k || held == modality) return 0;
    PyErr_Format(PyExc_TypeError, "%R holds %s, not %s", self->name, modality_noun(held), modality_noun(modality));
    return -1;
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
    collection->id = ustore_collection_main_k;
    collection->modality = ustore_modality_any_k;
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
    "The binary values, as a mapping: `blobs[key]` reads a zero-copy memoryview, a batch of keys reads Values.";

static PyObject* Collection_get_blobs(Collection* self, void* Py_UNUSED(closure)) {
    return collection_view(self, &BlobsViewType);
}

static char const doc_Collection_docs[] =
    "The JSON documents as Python objects, as a mapping: `docs[key]` parses one, `docs[key] = {...}` serializes one.";

static PyObject* Collection_get_docs(Collection* self, void* Py_UNUSED(closure)) {
    return collection_view(self, &DocsViewType);
}

static char const doc_Collection_docs_json[] =
    "The JSON documents as their text, as a mapping: `docs_json[key]` reads a zero-copy memoryview of the JSON.";

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
    return collection_python_view(self, ustore_modality_docs_k, "ustore.table_view", "Table");
}

static char const doc_Collection_name[] = "The name the collection was addressed by.";

static PyObject* Collection_get_name(Collection* self, void* Py_UNUSED(closure)) { return Py_NewRef(self->name); }

static char const doc_Collection_id[] = "The ID the name addresses, raising MissingError when it addresses none.";

static PyObject* Collection_get_id(Collection* self, void* Py_UNUSED(closure)) {
    ustore_py_context_t context;
    ustore_py_verb_options_t options;
    if (collection_resolve(self, ustore_modality_any_k, collection_missing_refused_k, &context, &options) != 0)
        return NULL;
    return ustore_py_uuid_to_object(&options.collection.uuid);
}

static char const doc_Collection_modality[] =
    "What the collection holds, such as 'docs', raising MissingError when the name addresses none.";

static PyObject* Collection_get_modality(Collection* self, void* Py_UNUSED(closure)) {
    ustore_py_context_t context;
    ustore_py_verb_options_t options;
    if (collection_resolve(self, ustore_modality_any_k, collection_missing_refused_k, &context, &options) != 0)
        return NULL;
    for (ustore_py_choice_naming_t const* naming = ustore_py_modality_namings; naming->name; ++naming)
        if (naming->value == (int)self->modality) return PyUnicode_FromString(naming->name);
    return PyUnicode_FromString("unknown");
}

static char const doc_Collection_sample[] =                                                 //
    "sample(count) answers the Values of up to `count` keys drawn at random.\n"             //
    "\n"                                                                                    //
    "Draws the keys and reads their values through the same view in one call. The keys\n"   //
    "come back as the `keys` attribute of the answer, and a key removed between the draw\n" //
    "and the read reads as None.\n"                                                         //
    "\n"                                                                                    //
    "Args:\n"                                                                               //
    "  count (int): Keys to draw.\n"                                                        //
    "Returns:\n"                                                                            //
    "  Values: One value per key drawn, with the keys beside them.\n"                       //
    "Raises:\n"                                                                             //
    "  MissingError: No collection carries the name.";

static PyObject* Collection_sample(Collection* self, PyObject* count_object) {
    size_t count = 0;
    if (ustore_py_size_from_object(count_object, "count", &count) != 0) return NULL;
    ustore_py_context_t context;
    ustore_py_verb_options_t options;
    if (collection_resolve(self, ustore_modality_any_k, collection_missing_refused_k, &context, &options) != 0)
        return NULL;

    PyObject* const drawn = ustore_py_keys_sample(&context, &options, count);
    if (!drawn) return NULL;
    Keys const* const keys = (Keys const*)drawn;
    PyObject* const values = ustore_py_values_read(&context, &options, keys->tape, (size_t)keys->offsets[1]);
    if (!values) {
        Py_DECREF(drawn);
        return NULL;
    }
    ((Values*)values)->keys = drawn;
    return values;
}

static char const doc_Collection_sample_keys[] =                                      //
    "sample_keys(count) answers a Keys tape of up to `count` keys drawn at random.\n" //
    "\n"                                                                              //
    "Args:\n"                                                                         //
    "  count (int): Keys to draw.\n"                                                  //
    "Returns:\n"                                                                      //
    "  Keys: One run of keys, in no particular order.";

static PyObject* Collection_sample_keys(Collection* self, PyObject* count_object) {
    size_t count = 0;
    if (ustore_py_size_from_object(count_object, "count", &count) != 0) return NULL;
    ustore_py_context_t context;
    ustore_py_verb_options_t options;
    if (collection_resolve(self, ustore_modality_any_k, collection_missing_refused_k, &context, &options) != 0)
        return NULL;
    return ustore_py_keys_sample(&context, &options, count);
}

static char const doc_Collection_scan[] =                                          //
    "scan(start, count) answers a Keys tape of up to `count` keys from `start`.\n" //
    "\n"                                                                           //
    "Args:\n"                                                                      //
    "  start (KeyLike): The smallest key the scan may return.\n"                   //
    "  count (int): Most keys the scan returns.\n"                                 //
    "Returns:\n"                                                                   //
    "  Keys: One run of keys, in ascending order.";

static PyObject* Collection_scan(Collection* self, PyObject* const* args, Py_ssize_t positional_args_count) {
    if (positional_args_count != 2) {
        PyErr_Format(PyExc_TypeError, "scan() takes 2 positional arguments, received %zd", positional_args_count);
        return NULL;
    }
    ustore_key_t start = {0};
    size_t count = 0;
    if (ustore_py_key_from_object(args[0], &start) != 0) return NULL;
    if (ustore_py_size_from_object(args[1], "count", &count) != 0) return NULL;
    ustore_py_context_t context;
    ustore_py_verb_options_t options;
    if (collection_resolve(self, ustore_modality_any_k, collection_missing_refused_k, &context, &options) != 0)
        return NULL;
    return ustore_py_keys_scan(&context, &options, &start, count);
}

static char const doc_Collection[] =                                                         //
    "A collection a database or a transaction addresses by name, as `db['movies']`.\n"       //
    "\n"                                                                                     //
    "The name resolves once, on first use. A read resolves it through the view of the\n"     //
    "object indexed, so a watching transaction records it, and raises MissingError when\n"   //
    "no collection carries it there. A write through `blobs` or `docs`, or an access of\n"   //
    "`graph` or `table`, resolves it at the head instead, creating a missing collection\n"   //
    "there to hold that modality, so a transaction never conflicts with a collection made\n" //
    "for it. A view of another modality raises TypeError, and a collection created to\n"     //
    "hold 'any' accepts every view.";

static PyMethodDef Collection_methods[] = {
    {"sample", (PyCFunction)Collection_sample, METH_O, doc_Collection_sample},
    {"sample_keys", (PyCFunction)Collection_sample_keys, METH_O, doc_Collection_sample_keys},
    {"scan", (PyCFunction)Collection_scan, METH_FASTCALL, doc_Collection_scan},
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

/**
 *  @brief Reads a subscript as one key or a batch of them, the way `view[key]` and `view[[keys]]` differ.
 *  @return 1 for one key, 0 for a batch, or -1 with an exception set.
 */
static int keys_from_subscript(PyObject* object, ustore_py_keys_t* keys) {
    if (!ustore_py_is_one_key(object)) return ustore_py_keys_from_object(object, keys) == 0 ? 0 : -1;
    memset(keys, 0, sizeof(*keys));
    if (ustore_py_key_from_object(object, &keys->inline_keys[0]) != 0) return -1;
    keys->keys = keys->inline_keys;
    keys->count = 1;
    return 1;
}

/** Answers the only value of @p values, a read of @p key, as a zero-copy memoryview, or raises KeyError. */
static PyObject* values_single(PyObject* values, PyObject* key) {
    if (!values) return NULL;
    Values const* const read = (Values const*)values;
    PyObject* found = NULL;
    Py_ssize_t const length = (Py_ssize_t)(read->offsets[1] - read->offsets[0]);
    if (!ustore_py_values_present(read, 0)) PyErr_SetObject(PyExc_KeyError, key);
    else found = ustore_py_span_view(values, read->tape + read->offsets[0], "B", 1, 1, &length);
    Py_DECREF(values);
    return found;
}

/** Parses the JSON of task @p task_index of @p values into Python objects, or answers None where it is missing. */
static PyObject* document_parse(Values const* values, size_t task_index) {
    if (!ustore_py_values_present(values, task_index)) Py_RETURN_NONE;
    // yyjson writes into its input only with `YYJSON_READ_INSITU`, which is never set.
    char* const text = (char*)(values->tape + values->offsets[task_index]);
    size_t const length = (size_t)(values->offsets[task_index + 1] - values->offsets[task_index]);
    yyjson_read_flag const flags = YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_INF_AND_NAN |
                                   YYJSON_READ_BIGNUM_AS_RAW;
    yyjson_read_err error = {0};
    yyjson_doc* const document = yyjson_read_opts(text, length, flags, NULL, &error);
    if (!document) {
        ustore_py_raise(ustore_status_malformed_k, "A stored document is not JSON: %s", error.msg);
        return NULL;
    }
    PyObject* const object = ustore_py_object_from_json(yyjson_doc_get_root(document));
    yyjson_doc_free(document);
    return object;
}

/** Parses every document of @p values, answering the only one for a single @p key or a list for a batch. */
static PyObject* documents_parse(PyObject* values, PyObject* key, int one) {
    if (!values) return NULL;
    Values const* const read = (Values const*)values;
    if (one && !ustore_py_values_present(read, 0)) {
        PyErr_SetObject(PyExc_KeyError, key);
        return NULL;
    }
    if (one) return document_parse(read, 0);
    PyObject* const documents = PyList_New((Py_ssize_t)read->count);
    for (size_t task_index = 0; documents && task_index != read->count; ++task_index) {
        PyObject* const document = document_parse(read, task_index);
        if (!document) {
            Py_DECREF(documents);
            return NULL;
        }
        PyList_SET_ITEM(documents, (Py_ssize_t)task_index, document);
    }
    return documents;
}

/** Releases @p values, giving its arena back to @p database when nothing else holds it, as `get` does. */
static void values_reclaim(Database* database, PyObject* values) {
    if (Py_REFCNT(values) == 1) {
        ustore_py_arena_return(database, ((Values*)values)->arena);
        ((Values*)values)->arena = NULL;
    }
    Py_DECREF(values);
}

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

/** Answers whether @p key is present in the collection of @p self, which must hold @p modality. */
static int collection_view_contains(CollectionView* self, PyObject* key, ustore_modality_t modality) {
    ustore_key_t parsed = {0};
    if (ustore_py_key_from_object(key, &parsed) != 0) return -1;
    ustore_py_context_t context;
    ustore_py_verb_options_t options;
    if (collection_resolve(self->collection, modality, collection_missing_refused_k, &context, &options) != 0)
        return -1;
    return ustore_py_value_contains(&context, &options, &parsed);
}

/** Removes the keys @p key addresses from the collection of @p self, which must hold @p modality and which a
 *  removal never creates. */
static int collection_view_remove(CollectionView* self, PyObject* key, ustore_modality_t modality) {
    ustore_py_keys_t keys;
    if (keys_from_subscript(key, &keys) < 0) return -1;
    ustore_py_context_t context;
    ustore_py_verb_options_t options;
    int const removed = collection_resolve(self->collection, modality, collection_missing_refused_k, &context,
                                           &options) == 0
                            ? ustore_py_values_remove(&context, &options, keys.keys, keys.count)
                            : -1;
    ustore_py_keys_release(&keys);
    return removed;
}

static PyObject* BlobsView_subscript(CollectionView* self, PyObject* key) {
    ustore_py_keys_t keys;
    int const one = keys_from_subscript(key, &keys);
    if (one < 0) return NULL;
    ustore_py_context_t context;
    ustore_py_verb_options_t options;
    PyObject* const values = collection_resolve(self->collection, ustore_modality_blobs_k, collection_missing_refused_k,
                                                &context, &options) == 0
                                 ? ustore_py_values_read(&context, &options, keys.keys, keys.count)
                                 : NULL;
    ustore_py_keys_release(&keys);
    return one ? values_single(values, key) : values;
}

static int BlobsView_ass_subscript(CollectionView* self, PyObject* key, PyObject* value) {
    if (!value) return collection_view_remove(self, key, ustore_modality_blobs_k);
    ustore_py_keys_t keys;
    int const one = keys_from_subscript(key, &keys);
    if (one < 0) return -1;

    // One key takes one bytes-like value, and a batch one per key or one for every key.
    ustore_py_contents_t contents;
    memset(&contents, 0, sizeof(contents));
    int gathered = 0;
    if (one) {
        gathered = ustore_py_contents_reserve(&contents, 1);
        if (gathered == 0) gathered = ustore_py_contents_borrow(value, &contents);
    }
    else { gathered = ustore_py_contents_from_object(value, keys.count, &contents); }

    ustore_py_context_t context;
    ustore_py_verb_options_t options;
    int const written = gathered == 0 && collection_resolve(self->collection, ustore_modality_blobs_k,
                                                            collection_missing_created_k, &context, &options) == 0
                            ? ustore_py_values_write(&context, &options, keys.keys, keys.count, &contents)
                            : -1;
    ustore_py_contents_release(&contents);
    ustore_py_keys_release(&keys);
    return written;
}

static int BlobsView_contains(CollectionView* self, PyObject* key) {
    return collection_view_contains(self, key, ustore_modality_blobs_k);
}

/** Reads the documents @p key addresses from the collection of @p self, answering their JSON text. */
static PyObject* docs_view_read(CollectionView* self, PyObject* key, ustore_py_context_t* context, int* one) {
    ustore_py_keys_t keys;
    *one = keys_from_subscript(key, &keys);
    if (*one < 0) return NULL;
    ustore_py_verb_options_t options;
    PyObject* const values = collection_resolve(self->collection, ustore_modality_docs_k, collection_missing_refused_k,
                                                context, &options) == 0
                                 ? ustore_py_docs_read(context, &options, keys.keys, keys.count, NULL,
                                                       ustore_doc_field_json_k)
                                 : NULL;
    ustore_py_keys_release(&keys);
    return values;
}

/** Writes documents under @p key, spelled as @p spelling says, or removes them when @p documents is NULL. */
static int docs_view_write(CollectionView* self, PyObject* key, PyObject* documents,
                           ustore_py_documents_spelling_t spelling) {
    if (!documents) return collection_view_remove(self, key, ustore_modality_docs_k);
    ustore_py_keys_t keys;
    int const one = keys_from_subscript(key, &keys);
    if (one < 0) return -1;
    // One key takes its document whole, even a list, so it travels as a batch of one.
    PyObject* const batch = one && spelling == ustore_py_documents_as_objects_k ? PyTuple_Pack(1, documents)
                                                                                : Py_NewRef(documents);
    ustore_py_context_t context;
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
    ustore_py_context_t context;
    int one = 0;
    PyObject* const values = docs_view_read(self, key, &context, &one);
    PyObject* const documents = documents_parse(values, key, one);
    if (values) values_reclaim(context.database, values);
    return documents;
}

static int DocsView_ass_subscript(CollectionView* self, PyObject* key, PyObject* document) {
    return docs_view_write(self, key, document, ustore_py_documents_as_objects_k);
}

static int DocsView_contains(CollectionView* self, PyObject* key) {
    return collection_view_contains(self, key, ustore_modality_docs_k);
}

static PyObject* DocsJSONView_subscript(CollectionView* self, PyObject* key) {
    ustore_py_context_t context;
    int one = 0;
    PyObject* const values = docs_view_read(self, key, &context, &one);
    return one ? values_single(values, key) : values;
}

static int DocsJSONView_ass_subscript(CollectionView* self, PyObject* key, PyObject* text) {
    return docs_view_write(self, key, text, ustore_py_documents_as_json_k);
}

static char const doc_BlobsView[] =                                                         //
    "The binary values of a collection, as a mapping.\n"                                    //
    "\n"                                                                                    //
    "`blobs[key]` answers a read-only memoryview over the arena of the read, raising\n"     //
    "KeyError when the key is absent, and a batch of keys answers Values, which export\n"   //
    "through the Arrow C data interface. `blobs[key] = value` takes anything bytes-like,\n" //
    "a batch takes one value per key or one for every key, and `del blobs[key]` removes.";

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
    "`docs_json[key]` answers a read-only memoryview over the stored JSON, raising KeyError\n" //
    "when the key is absent, and a batch of keys answers Values. `docs_json[key] = text`\n"    //
    "stores JSON text or bytes as they are, and `del docs_json[key]` removes.";

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
