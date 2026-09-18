/**
 *  @file python/database.c
 *  @author Ash Vardanian
 *  @date September 16, 2026
 *  @brief Databases, collections, snapshots and transactions.
 *
 *  A database owns its directory and the arena it lends, and answers what its engine supports. A
 *  transaction holds a reference to the database it runs against, so the handle it stages into can
 *  never outlive the one it came from, and a database refuses to close while one is still open.
 */
#include "lib.h"

#pragma region Configuration

/**
 *  @brief Builds the JSON configuration that points an engine at @p directory.
 *  @param[in] directory The path, as a string, bytes, or anything os.PathLike.
 *  @return A new @c bytes whose buffer is the configuration, or NULL with an exception set.
 */
static PyObject* config_from_directory(PyObject* directory) {
    PyObject* encoded = NULL;
    if (!PyUnicode_FSConverter(directory, &encoded)) return NULL;
    char const* const path = PyBytes_AS_STRING(encoded);
    Py_ssize_t const length = PyBytes_GET_SIZE(encoded);

    static char const opening[] = "{\"version\": \"1.0\", \"directory\": \"";
    static char const closing[] = "\"}";
    // Two characters of a path would close the JSON string early, and each of them grows to two.
    char* const text = (char*)PyMem_Malloc(sizeof(opening) + sizeof(closing) + (size_t)length * 2);
    if (!text) {
        Py_DECREF(encoded);
        PyErr_NoMemory();
        return NULL;
    }

    char* cursor = text;
    memcpy(cursor, opening, sizeof(opening) - 1);
    cursor += sizeof(opening) - 1;
    for (Py_ssize_t position = 0; position != length; ++position) {
        char const character = path[position];
        if (character == '"' || character == '\\') *cursor++ = '\\';
        *cursor++ = character;
    }
    memcpy(cursor, closing, sizeof(closing));
    Py_DECREF(encoded);

    PyObject* const built = PyBytes_FromString(text);
    PyMem_Free(text);
    return built;
}

#pragma endregion Configuration

#pragma region Databases

static char const doc_Database[] =                                                            //
    "Database(directory=None, *, config=None)\n"                                              //
    "\n"                                                                                      //
    "An embedded database, persistent when it names a directory and in memory otherwise.\n"   //
    "\n"                                                                                      //
    "The directory belongs to the database, so no two processes may open the same one at\n"   //
    "once. Every database holds a main collection, which can be cleared but never dropped,\n" //
    "beside the collections it creates.\n"                                                    //
    "\n"                                                                                      //
    "Args:\n"                                                                                 //
    "  directory (str | bytes | PathLike, optional): Where the database persists; None\n"     //
    "    keeps it in memory. Turned into the engine's own JSON configuration.\n"              //
    "  config (str, optional): The engine's JSON configuration, given whole. Names the\n"     //
    "    directory itself, so it cannot be combined with the argument above.";

static PyObject* Database_tp_new(PyTypeObject* type, PyObject* args, PyObject* keywords) {
    PyObject* directory_object = Py_None;
    PyObject* config_object = Py_None;

    Py_ssize_t const positional_args_count = PyTuple_GET_SIZE(args);
    if (positional_args_count > 1) {
        PyErr_SetString(PyExc_TypeError, "Database() takes at most one positional argument");
        return NULL;
    }
    if (positional_args_count == 1) directory_object = PyTuple_GET_ITEM(args, 0);
    if (keywords) {
        PyObject* key = NULL;
        PyObject* value = NULL;
        Py_ssize_t position = 0;
        while (PyDict_Next(keywords, &position, &key, &value)) {
            if (PyUnicode_CompareWithASCIIString(key, "directory") == 0) {
                if (positional_args_count == 1) {
                    PyErr_SetString(PyExc_TypeError, "Database() got 'directory' twice");
                    return NULL;
                }
                directory_object = value;
            }
            else if (PyUnicode_CompareWithASCIIString(key, "config") == 0) { config_object = value; }
            else {
                PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", key);
                return NULL;
            }
        }
    }
    if (directory_object != Py_None && config_object != Py_None) {
        PyErr_SetString(PyExc_TypeError, "A configuration already names the directory, so only one of them is taken");
        return NULL;
    }

    // The configuration outlives the call through this reference alone, since the ABI borrows it.
    PyObject* configuration = NULL;
    if (directory_object != Py_None) {
        configuration = config_from_directory(directory_object);
        if (!configuration) return NULL;
    }
    else if (config_object != Py_None) {
        if (!PyUnicode_Check(config_object)) {
            PyErr_Format(PyExc_TypeError, "The 'config' option must be a string, got %S", Py_TYPE(config_object));
            return NULL;
        }
        configuration = PyUnicode_AsUTF8String(config_object);
        if (!configuration) return NULL;
    }

    Database* const instance = (Database*)type->tp_alloc(type, 0);
    if (!instance) {
        Py_XDECREF(configuration);
        return NULL;
    }
    instance->handle = NULL;
    instance->arenas.idle = NULL;
    instance->metadata = 0;
    instance->transactions = 0;

    ustore_str_view_t const config = configuration ? PyBytes_AS_STRING(configuration) : NULL;
    ustore_database_t handle = NULL;
    ustore_error_t error = {0};
    ustore_database_init_t open = {0};
    open.config = config;
    open.db = &handle;
    open.error = &error;

    Py_BEGIN_ALLOW_THREADS;
    ustore_database_init(&open);
    Py_END_ALLOW_THREADS;
    Py_XDECREF(configuration);

    if (ustore_py_raise_on_error(&error) != 0) {
        Py_DECREF(instance);
        return NULL;
    }
    instance->handle = handle;

    ustore_metadata_t metadata = 0;
    ustore_get_metadata_t capabilities = {0};
    capabilities.db = handle;
    capabilities.error = &error;
    capabilities.metadata = &metadata;
    ustore_get_metadata(&capabilities);
    if (ustore_py_raise_on_error(&error) != 0) {
        Py_DECREF(instance);
        return NULL;
    }
    instance->metadata = metadata;
    return (PyObject*)instance;
}

/** Closes @p database and releases the arena it was lending. */
static void database_close(Database* database) {
    ustore_database_t const handle = database->handle;
    ustore_arena_t const arena = database->arenas.idle;
    database->handle = NULL;
    database->arenas.idle = NULL;
    if (!handle && !arena) return;

    Py_BEGIN_ALLOW_THREADS;
    ustore_arena_free(arena);
    ustore_database_free(handle);
    Py_END_ALLOW_THREADS;
}

static void Database_dealloc(Database* self) {
    database_close(self);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static char const doc_Database_close[] =                                                  //
    "close() answers None.\n"                                                             //
    "\n"                                                                                  //
    "Closes the database, while the data persists on disk. Every transaction it opened\n" //
    "must be closed first, since a transaction stages into the handle this releases.\n"   //
    "\n"                                                                                  //
    "Raises:\n"                                                                           //
    "  ArgumentError: A transaction of this database is still open.";

static PyObject* Database_close(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    Database* const database = (Database*)self;
    if (database->transactions != 0) {
        ustore_py_raise(ustore_error_invalid_argument_k, "%zd transactions of this database are still open",
                        database->transactions);
        return NULL;
    }
    database_close(database);
    Py_RETURN_NONE;
}

static PyObject* Database_enter(PyObject* self, PyObject* Py_UNUSED(ignored)) { return Py_NewRef(self); }

static PyObject* Database_exit(PyObject* self, PyObject* const* Py_UNUSED(args),
                               Py_ssize_t Py_UNUSED(positional_args_count), PyObject* Py_UNUSED(args_names_tuple)) {
    return Database_close(self, NULL);
}

static PyObject* Database_repr(Database* self) {
    if (!self->handle) return PyUnicode_FromString("Database(<closed>)");
    return PyUnicode_FromFormat("Database(at=%p)", (void*)self->handle);
}

static char const doc_Database_supports_transactions[] = "Whether the engine groups operations into transactions.";

static PyObject* Database_get_supports_transactions(Database* self, void* Py_UNUSED(closure)) {
    return PyBool_FromLong((self->metadata & ustore_supports_transactions_k) != 0);
}

static char const doc_Database_supports_collections[] = "Whether the engine holds collections beyond the main one.";

static PyObject* Database_get_supports_collections(Database* self, void* Py_UNUSED(closure)) {
    return PyBool_FromLong((self->metadata & ustore_supports_collections_k) != 0);
}

static char const doc_Database_supports_snapshots[] = "Whether the engine captures point-in-time views.";

static PyObject* Database_get_supports_snapshots(Database* self, void* Py_UNUSED(closure)) {
    return PyBool_FromLong((self->metadata & ustore_supports_snapshots_k) != 0);
}

static char const doc_Database_control[] =                                                   //
    "control(request) answers the engine's response as a string.\n"                          //
    "\n"                                                                                     //
    "Sends a free-form request to the engine. The requests have no stable format and are\n"  //
    "expensive: 'clear' removes all data and keeps the collections, 'reset' removes both,\n" //
    "'compact' flushes and compacts, 'info' describes the build, and 'usage' estimates\n"    //
    "the sizes of collections and the use of memory and disk.\n"                             //
    "\n"                                                                                     //
    "Args:\n"                                                                                //
    "  request (str): The request, as the engine spells it.\n"                               //
    "Returns:\n"                                                                             //
    "  str: Whatever the engine answered.";

static PyObject* Database_control(PyObject* self, PyObject* request_object) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (!PyUnicode_Check(request_object)) {
        PyErr_Format(PyExc_TypeError, "A request is a string, got %S", Py_TYPE(request_object));
        return NULL;
    }
    char const* const request = PyUnicode_AsUTF8(request_object);
    if (!request) return NULL;

    ustore_str_view_t response = NULL;
    ustore_error_t error = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context.database);

    ustore_database_control_t control = {0};
    control.db = context.database->handle;
    control.arena = &arena;
    control.error = &error;
    control.request = request;
    control.response = &response;

    Py_BEGIN_ALLOW_THREADS;
    ustore_database_control(&control);
    Py_END_ALLOW_THREADS;

    PyObject* answered = NULL;
    if (error.status != ustore_success_k) { ustore_py_raise_on_error(&error); }
    else { answered = PyUnicode_FromString(response ? response : ""); }
    ustore_py_arena_return(context.database, arena);
    return answered;
}

#pragma endregion Databases

#pragma region Collections

/** What a drop removes, as a caller spells it. */
static ustore_py_choice_naming_t const drop_namings[] = {
    {"collection", ustore_drop_keys_vals_handle_k},
    {"pairs", ustore_drop_keys_vals_k},
    {"values", ustore_drop_vals_k},
    {NULL, 0},
};

static char const doc_Database_collection_create[] =                                         //
    "collection_create(name, *, config=None) answers the new collection's identifier.\n"     //
    "\n"                                                                                     //
    "Creates an empty collection and records the name for it, in the two phases a name\n"    //
    "always resolves through. Every other verb addresses the collection by the identifier\n" //
    "this answers.\n"                                                                        //
    "\n"                                                                                     //
    "Args:\n"                                                                                //
    "  name (str): The name to record, which must not already be taken.\n"                   //
    "  config (str, optional): The engine's JSON configuration for this collection.\n"       //
    "Returns:\n"                                                                             //
    "  int: The identifier addressing the new collection.\n"                                 //
    "Raises:\n"                                                                              //
    "  ExistsError: A collection already carries the name.\n"                                //
    "  UnsupportedError: The engine holds only the main collection.";

static PyObject* Database_collection_create(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                            PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;

    Py_ssize_t const args_names_count = ustore_py_args_names_count_checked("collection_create", positional_args_count,
                                                                           args_names_tuple, 1);
    if (args_names_count < 0) return NULL;

    PyObject* config_object = Py_None;
    for (Py_ssize_t names_progress = 0, args_progress = positional_args_count; names_progress != args_names_count;
         ++args_progress, ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(args_names_tuple, names_progress);
        if (PyUnicode_CompareWithASCIIString(name, "config") == 0) { config_object = args[args_progress]; }
        else {
            PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
            return NULL;
        }
    }

    if (!PyUnicode_Check(args[0])) {
        PyErr_Format(PyExc_TypeError, "A collection name is a string, got %S", Py_TYPE(args[0]));
        return NULL;
    }
    Py_ssize_t name_length = 0;
    char const* const name = PyUnicode_AsUTF8AndSize(args[0], &name_length);
    if (!name) return NULL;
    if (name_length == 0) {
        ustore_py_raise(ustore_error_invalid_argument_k, "The main collection has no name and always exists");
        return NULL;
    }

    ustore_collection_t taken = ustore_collection_main_k;
    switch (ustore_py_collection_named(&context, name, (size_t)name_length, &taken)) {
    case ustore_py_naming_failed_k: return NULL;
    case ustore_py_naming_found_k:
        ustore_py_raise(ustore_error_already_exists_k, "A collection is already named %R", args[0]);
        return NULL;
    case ustore_py_naming_missing_k: break;
    }

    ustore_str_view_t config = NULL;
    if (config_object != Py_None) {
        if (!PyUnicode_Check(config_object)) {
            PyErr_Format(PyExc_TypeError, "The 'config' option must be a string, got %S", Py_TYPE(config_object));
            return NULL;
        }
        config = PyUnicode_AsUTF8(config_object);
        if (!config) return NULL;
    }

    ustore_collection_t created = ustore_collection_main_k;
    ustore_error_t error = {0};
    ustore_collection_create_t create = {0};
    create.db = context.database->handle;
    create.error = &error;
    create.config = config;
    create.id = &created;

    Py_BEGIN_ALLOW_THREADS;
    ustore_collection_create(&create);
    Py_END_ALLOW_THREADS;
    if (ustore_py_raise_on_error(&error) != 0) return NULL;

    if (ustore_py_collection_name(&context, name, (size_t)name_length, &created) != 0) return NULL;
    return PyLong_FromUnsignedLongLong(created);
}

static char const doc_Database_collection_find[] =                               //
    "collection_find(name) answers the identifier the name addresses.\n"         //
    "\n"                                                                         //
    "Args:\n"                                                                    //
    "  name (str): The name to resolve; the empty one is the main collection.\n" //
    "Returns:\n"                                                                 //
    "  int: The identifier addressing the collection.\n"                         //
    "Raises:\n"                                                                  //
    "  MissingError: No collection carries the name.";

static PyObject* Database_collection_find(PyObject* self, PyObject* name_object) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    ustore_collection_t found = ustore_collection_main_k;
    if (ustore_py_collection_from_object(&context, name_object, &found) != 0) return NULL;
    return PyLong_FromUnsignedLongLong(found);
}

static char const doc_Database_collection_contains[] =                            //
    "collection_contains(name) answers whether a collection carries the name.\n"  //
    "\n"                                                                          //
    "Args:\n"                                                                     //
    "  name (str): The name to look for; the empty one is the main collection.\n" //
    "Returns:\n"                                                                  //
    "  bool: Whether the name is taken.";

static PyObject* Database_collection_contains(PyObject* self, PyObject* name_object) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (!PyUnicode_Check(name_object)) {
        PyErr_Format(PyExc_TypeError, "A collection name is a string, got %S", Py_TYPE(name_object));
        return NULL;
    }
    Py_ssize_t length = 0;
    char const* const name = PyUnicode_AsUTF8AndSize(name_object, &length);
    if (!name) return NULL;
    if (length == 0) Py_RETURN_TRUE;

    ustore_collection_t found = ustore_collection_main_k;
    switch (ustore_py_collection_named(&context, name, (size_t)length, &found)) {
    case ustore_py_naming_found_k: Py_RETURN_TRUE;
    case ustore_py_naming_missing_k: Py_RETURN_FALSE;
    case ustore_py_naming_failed_k: return NULL;
    }
    Py_RETURN_FALSE;
}

static char const doc_Database_collection_list[] =                                           //
    "collection_list() answers the identifiers of every collection.\n"                       //
    "\n"                                                                                     //
    "The main collection is never listed, and a name lives in the strings modality rather\n" //
    "than in the catalog of an engine, so this answers identifiers alone.\n"                 //
    "\n"                                                                                     //
    "Returns:\n"                                                                             //
    "  tuple[int, ...]: One identifier per collection.";

static PyObject* Database_collection_list(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;

    ustore_size_t count = 0;
    ustore_collection_t* identifiers = NULL;
    ustore_error_t error = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context.database);

    ustore_collection_list_t list = {0};
    list.db = context.database->handle;
    list.error = &error;
    list.transaction = context.transaction;
    list.snapshot = context.snapshot;
    list.arena = &arena;
    list.options = context.options;
    list.count = &count;
    list.ids = &identifiers;

    Py_BEGIN_ALLOW_THREADS;
    ustore_collection_list(&list);
    Py_END_ALLOW_THREADS;

    PyObject* listed = NULL;
    if (error.status != ustore_success_k) { ustore_py_raise_on_error(&error); }
    else if ((listed = PyTuple_New((Py_ssize_t)count)) != NULL) {
        for (ustore_size_t entry_index = 0; entry_index != count; ++entry_index) {
            PyObject* const identifier = PyLong_FromUnsignedLongLong(identifiers[entry_index]);
            if (!identifier) {
                Py_CLEAR(listed);
                break;
            }
            PyTuple_SET_ITEM(listed, (Py_ssize_t)entry_index, identifier);
        }
    }
    ustore_py_arena_return(context.database, arena);
    return listed;
}

static char const doc_Database_collection_names[] =                                          //
    "collection_names(*, after=None, limit=1024) answers one page of recorded names.\n"      //
    "\n"                                                                                     //
    "Names come in the order of their text, and the walk paginates rather than holding an\n" //
    "iterator over concurrent state, so a listing longer than one page resumes from the\n"   //
    "last name it saw.\n"                                                                    //
    "\n"                                                                                     //
    "Args:\n"                                                                                //
    "  after (str, optional): The smallest name the page may carry; None starts at the\n"    //
    "    first one. The name given is itself included, so a resuming caller drops it.\n"     //
    "  limit (int, optional): Most names the page carries.\n"                                //
    "Returns:\n"                                                                             //
    "  tuple[str, ...]: The names recorded for collections, in order.";

static PyObject* Database_collection_names(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                           PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;

    Py_ssize_t const args_names_count = ustore_py_args_names_count_checked("collection_names", positional_args_count,
                                                                           args_names_tuple, 0);
    if (args_names_count < 0) return NULL;

    PyObject* after_object = Py_None;
    size_t limit = 1024;
    for (Py_ssize_t names_progress = 0, args_progress = positional_args_count; names_progress != args_names_count;
         ++args_progress, ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(args_names_tuple, names_progress);
        PyObject* const value = args[args_progress];
        if (PyUnicode_CompareWithASCIIString(name, "after") == 0) { after_object = value; }
        else if (PyUnicode_CompareWithASCIIString(name, "limit") == 0) {
            if (ustore_py_size_from_object(value, "limit", &limit) != 0) return NULL;
        }
        else {
            PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
            return NULL;
        }
    }
    if (limit > UINT32_MAX) {
        PyErr_SetString(PyExc_ValueError, "A page carries at most 4294967295 names");
        return NULL;
    }

    char const* after = NULL;
    Py_ssize_t after_length = 0;
    if (after_object != Py_None) {
        if (!PyUnicode_Check(after_object)) {
            PyErr_Format(PyExc_TypeError, "The 'after' option must be a string, got %S", Py_TYPE(after_object));
            return NULL;
        }
        after = PyUnicode_AsUTF8AndSize(after_object, &after_length);
        if (!after) return NULL;
    }
    return ustore_py_collection_names(&context, after, (size_t)after_length, limit);
}

static char const doc_Database_collection_drop[] =                                           //
    "collection_drop(collection, *, mode='collection') answers None.\n"                      //
    "\n"                                                                                     //
    "Removes a collection, or empties it. Dropping one addressed by name forgets the name\n" //
    "with it, so the name can be given to another collection afterwards.\n"                  //
    "\n"                                                                                     //
    "Args:\n"                                                                                //
    "  collection (int | str): The collection, by identifier or by name.\n"                  //
    "  mode (str, optional): 'collection' removes the handle and everything in it,\n"        //
    "    'pairs' removes every key and value, and 'values' removes every value. The main\n"  //
    "    collection accepts the latter two alone.\n"                                         //
    "Raises:\n"                                                                              //
    "  MissingError: No collection carries the name.";

static PyObject* Database_collection_drop(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                          PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;

    Py_ssize_t const args_names_count = ustore_py_args_names_count_checked("collection_drop", positional_args_count,
                                                                           args_names_tuple, 1);
    if (args_names_count < 0) return NULL;

    int mode = ustore_drop_keys_vals_handle_k;
    for (Py_ssize_t names_progress = 0, args_progress = positional_args_count; names_progress != args_names_count;
         ++args_progress, ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(args_names_tuple, names_progress);
        if (PyUnicode_CompareWithASCIIString(name, "mode") == 0) {
            if (ustore_py_choice_from_object(args[args_progress], "mode", drop_namings, &mode) != 0) return NULL;
        }
        else {
            PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
            return NULL;
        }
    }

    ustore_collection_t collection = ustore_collection_main_k;
    if (ustore_py_collection_from_object(&context, args[0], &collection) != 0) return NULL;

    ustore_error_t error = {0};
    ustore_collection_drop_t drop = {0};
    drop.db = context.database->handle;
    drop.error = &error;
    drop.id = collection;
    drop.mode = (ustore_drop_mode_t)mode;

    Py_BEGIN_ALLOW_THREADS;
    ustore_collection_drop(&drop);
    Py_END_ALLOW_THREADS;
    if (ustore_py_raise_on_error(&error) != 0) return NULL;

    // A handle that is gone leaves its name addressing nothing, so the name goes with it.
    if (mode == ustore_drop_keys_vals_handle_k && PyUnicode_Check(args[0])) {
        Py_ssize_t length = 0;
        char const* const name = PyUnicode_AsUTF8AndSize(args[0], &length);
        if (!name) return NULL;
        if (length != 0 && ustore_py_collection_name(&context, name, (size_t)length, NULL) != 0) return NULL;
    }
    Py_RETURN_NONE;
}

#pragma endregion Collections

#pragma region Snapshots

static char const doc_Database_snapshot_create[] =                                          //
    "snapshot_create() answers the identifier of a new snapshot.\n"                         //
    "\n"                                                                                    //
    "Captures a point-in-time view of the whole database, which reads may then follow by\n" //
    "naming its identifier.\n"                                                              //
    "\n"                                                                                    //
    "Returns:\n"                                                                            //
    "  int: The identifier addressing the snapshot.";

static PyObject* Database_snapshot_create(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;

    ustore_snapshot_t identifier = 0;
    ustore_error_t error = {0};
    ustore_snapshot_create_t create = {0};
    create.db = context.database->handle;
    create.error = &error;
    create.id = &identifier;

    Py_BEGIN_ALLOW_THREADS;
    ustore_snapshot_create(&create);
    Py_END_ALLOW_THREADS;
    if (ustore_py_raise_on_error(&error) != 0) return NULL;
    return PyLong_FromUnsignedLongLong(identifier);
}

static char const doc_Database_snapshot_list[] =                   //
    "snapshot_list() answers the identifiers of every snapshot.\n" //
    "\n"                                                           //
    "Returns:\n"                                                   //
    "  tuple[int, ...]: One identifier per snapshot.";

static PyObject* Database_snapshot_list(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;

    ustore_size_t count = 0;
    ustore_snapshot_t* identifiers = NULL;
    ustore_error_t error = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context.database);

    ustore_snapshot_list_t list = {0};
    list.db = context.database->handle;
    list.error = &error;
    list.arena = &arena;
    list.options = context.options;
    list.count = &count;
    list.ids = &identifiers;

    Py_BEGIN_ALLOW_THREADS;
    ustore_snapshot_list(&list);
    Py_END_ALLOW_THREADS;

    PyObject* listed = NULL;
    if (error.status != ustore_success_k) { ustore_py_raise_on_error(&error); }
    else if ((listed = PyTuple_New((Py_ssize_t)count)) != NULL) {
        for (ustore_size_t entry_index = 0; entry_index != count; ++entry_index) {
            PyObject* const identifier = PyLong_FromUnsignedLongLong(identifiers[entry_index]);
            if (!identifier) {
                Py_CLEAR(listed);
                break;
            }
            PyTuple_SET_ITEM(listed, (Py_ssize_t)entry_index, identifier);
        }
    }
    ustore_py_arena_return(context.database, arena);
    return listed;
}

static char const doc_Database_snapshot_drop[] =                        //
    "snapshot_drop(snapshot) answers None.\n"                           //
    "\n"                                                                //
    "Releases a snapshot, after which reads can no longer follow it.\n" //
    "\n"                                                                //
    "Args:\n"                                                           //
    "  snapshot (int): The identifier of an existing snapshot.";

static PyObject* Database_snapshot_drop(PyObject* self, PyObject* snapshot_object) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    size_t identifier = 0;
    if (ustore_py_size_from_object(snapshot_object, "snapshot", &identifier) != 0) return NULL;

    ustore_error_t error = {0};
    ustore_snapshot_drop_t drop = {0};
    drop.db = context.database->handle;
    drop.error = &error;
    drop.id = (ustore_snapshot_t)identifier;

    Py_BEGIN_ALLOW_THREADS;
    ustore_snapshot_drop(&drop);
    Py_END_ALLOW_THREADS;
    if (ustore_py_raise_on_error(&error) != 0) return NULL;
    Py_RETURN_NONE;
}

static char const doc_Database_snapshot_export[] =                                    //
    "snapshot_export(snapshot, directory) answers None.\n"                            //
    "\n"                                                                              //
    "Writes a snapshot out as a new database, which another process may then open.\n" //
    "\n"                                                                              //
    "Args:\n"                                                                         //
    "  snapshot (int): The identifier of an existing snapshot.\n"                     //
    "  directory (str | bytes | PathLike): Where the exported database lands.";

static PyObject* Database_snapshot_export(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                          PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    Py_ssize_t const args_names_count = ustore_py_args_names_count_checked("snapshot_export", positional_args_count,
                                                                           args_names_tuple, 2);
    if (args_names_count < 0) return NULL;
    if (args_names_count != 0) {
        PyErr_SetString(PyExc_TypeError, "snapshot_export() takes no keyword arguments");
        return NULL;
    }

    size_t identifier = 0;
    if (ustore_py_size_from_object(args[0], "snapshot", &identifier) != 0) return NULL;
    PyObject* encoded = NULL;
    if (!PyUnicode_FSConverter(args[1], &encoded)) return NULL;

    ustore_error_t error = {0};
    ustore_snapshot_export_t exported = {0};
    exported.db = context.database->handle;
    exported.error = &error;
    exported.id = (ustore_snapshot_t)identifier;
    exported.path = PyBytes_AS_STRING(encoded);

    Py_BEGIN_ALLOW_THREADS;
    ustore_snapshot_export(&exported);
    Py_END_ALLOW_THREADS;
    Py_DECREF(encoded);
    if (ustore_py_raise_on_error(&error) != 0) return NULL;
    Py_RETURN_NONE;
}

#pragma endregion Snapshots

#pragma region Transactions

static char const doc_Transaction[] =                                                         //
    "An optimistic transaction grouping operations across collections and modalities.\n"      //
    "\n"                                                                                      //
    "Built through `Database.transaction()`, never by name. A watching transaction records\n" //
    "what it reads and writes and refuses to commit when a concurrent writer touched any\n"   //
    "of it, so a caller retries rather than locks.\n"                                         //
    "\n"                                                                                      //
    "Every verb here takes a `watch` keyword overriding that for one call, so a read a\n"     //
    "retry must not widen its conflict set with passes `watch=False`; only a transaction\n"   //
    "has a conflict set, so a database refuses the keyword.\n"                                //
    "\n"                                                                                      //
    "Used as a context manager, it commits on a clean exit, drops the staged work on an\n"    //
    "exception, and closes either way.";

static PyObject* Transaction_tp_new(PyTypeObject* Py_UNUSED(type), PyObject* Py_UNUSED(args),
                                    PyObject* Py_UNUSED(keywords)) {
    PyErr_SetString(PyExc_TypeError, "A transaction is built through Database.transaction()");
    return NULL;
}

/** Releases the transaction's handle and lets go of the database that was holding it open. */
static void transaction_close(Transaction* transaction) {
    if (!transaction->handle) return;
    ustore_transaction_t const handle = transaction->handle;
    transaction->handle = NULL;

    Py_BEGIN_ALLOW_THREADS;
    ustore_transaction_free(handle);
    Py_END_ALLOW_THREADS;
    --((Database*)transaction->database)->transactions;
}

static void Transaction_dealloc(Transaction* self) {
    transaction_close(self);
    Py_XDECREF(self->database);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static char const doc_Database_transaction[] =                                              //
    "transaction(*, watch=True) answers a new Transaction.\n"                               //
    "\n"                                                                                    //
    "Args:\n"                                                                               //
    "  watch (bool, optional): Whether the transaction records what it reads and writes,\n" //
    "    and so whether a concurrent writer can make it fail to commit. A transaction\n"    //
    "    that watches nothing always commits, and sees whatever the head holds.\n"          //
    "Returns:\n"                                                                            //
    "  Transaction: A transaction over this database.\n"                                    //
    "Raises:\n"                                                                             //
    "  UnsupportedError: The engine has no transactions.";

static PyObject* Database_transaction(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                      PyObject* args_names_tuple) {
    Database* const database = (Database*)self;
    if (!database->handle) {
        PyErr_SetString(PyExc_ValueError, "The database is closed");
        return NULL;
    }

    Py_ssize_t const args_names_count = ustore_py_args_names_count_checked("transaction", positional_args_count,
                                                                           args_names_tuple, 0);
    if (args_names_count < 0) return NULL;

    ustore_options_t options = ustore_options_default_k;
    for (Py_ssize_t names_progress = 0, args_progress = positional_args_count; names_progress != args_names_count;
         ++args_progress, ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(args_names_tuple, names_progress);
        if (PyUnicode_CompareWithASCIIString(name, "watch") == 0) {
            int const watching = PyObject_IsTrue(args[args_progress]);
            if (watching < 0) return NULL;
            if (!watching) options |= ustore_option_transaction_dont_watch_k;
        }
        else {
            PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
            return NULL;
        }
    }

    ustore_transaction_t handle = NULL;
    ustore_error_t error = {0};
    ustore_transaction_init_t begin = {0};
    begin.db = database->handle;
    begin.error = &error;
    begin.transaction = &handle;

    Py_BEGIN_ALLOW_THREADS;
    ustore_transaction_init(&begin);
    Py_END_ALLOW_THREADS;
    if (ustore_py_raise_on_error(&error) != 0) return NULL;

    Transaction* const instance = (Transaction*)TransactionType.tp_alloc(&TransactionType, 0);
    if (!instance) {
        ustore_transaction_free(handle);
        return NULL;
    }
    instance->handle = handle;
    instance->database = Py_NewRef(self);
    instance->options = options;
    ++database->transactions;
    return (PyObject*)instance;
}

static char const doc_Transaction_commit[] =                                             //
    "commit(*, flush=False) answers None.\n"                                             //
    "\n"                                                                                 //
    "Applies every staged change, or none of them. The contents are kept whatever the\n" //
    "outcome, so a rejected transaction can be inspected or replayed.\n"                 //
    "\n"                                                                                 //
    "Args:\n"                                                                            //
    "  flush (bool, optional): Whether to reach the drive before returning.\n"           //
    "Raises:\n"                                                                          //
    "  ConflictError: A concurrent writer touched what this transaction watched.";

static PyObject* Transaction_commit(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                    PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;

    Py_ssize_t const args_names_count = ustore_py_args_names_count_checked("commit", positional_args_count,
                                                                           args_names_tuple, 0);
    if (args_names_count < 0) return NULL;

    ustore_options_t options = ustore_options_default_k;
    for (Py_ssize_t names_progress = 0, args_progress = positional_args_count; names_progress != args_names_count;
         ++args_progress, ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(args_names_tuple, names_progress);
        if (PyUnicode_CompareWithASCIIString(name, "flush") == 0) {
            int const flushing = PyObject_IsTrue(args[args_progress]);
            if (flushing < 0) return NULL;
            if (flushing) options |= ustore_option_write_flush_k;
        }
        else {
            PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
            return NULL;
        }
    }

    ustore_error_t error = {0};
    ustore_transaction_commit_t commit = {0};
    commit.db = context.database->handle;
    commit.error = &error;
    commit.transaction = context.transaction;
    commit.options = options;

    Py_BEGIN_ALLOW_THREADS;
    ustore_transaction_commit(&commit);
    Py_END_ALLOW_THREADS;
    if (ustore_py_raise_on_error(&error) != 0) return NULL;
    Py_RETURN_NONE;
}

static char const doc_Transaction_stage[] =                                            //
    "stage() answers the sequence number the stage took.\n"                            //
    "\n"                                                                               //
    "Runs the first phase of a two-phase commit, after which a commit publishes the\n" //
    "work.\n"                                                                          //
    "\n"                                                                               //
    "Returns:\n"                                                                       //
    "  int: The order this stage took among the transactions that applied.";

static PyObject* Transaction_stage(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;

    ustore_sequence_number_t sequence_number = 0;
    ustore_error_t error = {0};
    ustore_transaction_stage_t stage = {0};
    stage.db = context.database->handle;
    stage.error = &error;
    stage.transaction = context.transaction;
    stage.sequence_number = &sequence_number;

    Py_BEGIN_ALLOW_THREADS;
    ustore_transaction_stage(&stage);
    Py_END_ALLOW_THREADS;
    if (ustore_py_raise_on_error(&error) != 0) return NULL;
    return PyLong_FromUnsignedLongLong(sequence_number);
}

static char const doc_Transaction_rollback[] =                                           //
    "rollback() answers None.\n"                                                         //
    "\n"                                                                                 //
    "Drops every staged change and starts a new generation on the same handle, so the\n" //
    "transaction is ready to be used again without allocating another.";

static PyObject* Transaction_rollback(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;

    Transaction* const transaction = (Transaction*)self;
    ustore_error_t error = {0};
    ustore_transaction_init_t reset = {0};
    reset.db = context.database->handle;
    reset.error = &error;
    reset.transaction = &transaction->handle;

    Py_BEGIN_ALLOW_THREADS;
    ustore_transaction_init(&reset);
    Py_END_ALLOW_THREADS;
    if (ustore_py_raise_on_error(&error) != 0) return NULL;
    Py_RETURN_NONE;
}

static char const doc_Transaction_close[] =                                               //
    "close() answers None.\n"                                                             //
    "\n"                                                                                  //
    "Drops whatever is still staged and releases the handle, which a database waits on\n" //
    "before it closes. Closing twice is safe.";

static PyObject* Transaction_close(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    transaction_close((Transaction*)self);
    Py_RETURN_NONE;
}

static PyObject* Transaction_enter(PyObject* self, PyObject* Py_UNUSED(ignored)) { return Py_NewRef(self); }

static PyObject* Transaction_exit(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                  PyObject* Py_UNUSED(args_names_tuple)) {
    // A body that raised leaves the staged work unpublished, since the exception is the caller's
    // answer about whether the work was wanted at all.
    int const raised = positional_args_count >= 1 && args[0] != Py_None;
    if (!raised && ((Transaction*)self)->handle) {
        PyObject* const committed = Transaction_commit(self, NULL, 0, NULL);
        if (!committed) {
            transaction_close((Transaction*)self);
            return NULL;
        }
        Py_DECREF(committed);
    }
    transaction_close((Transaction*)self);
    Py_RETURN_NONE;
}

static PyObject* Transaction_repr(Transaction* self) {
    if (!self->handle) return PyUnicode_FromString("Transaction(<closed>)");
    return PyUnicode_FromFormat("Transaction(at=%p)", (void*)self->handle);
}

static char const doc_Transaction_database[] = "The database this transaction runs against.";

static PyObject* Transaction_get_database(Transaction* self, void* Py_UNUSED(closure)) {
    return Py_NewRef(self->database);
}

#pragma endregion Transactions

#pragma region Tables

static PyGetSetDef Database_getset[] = {
    {"supports_transactions", (getter)Database_get_supports_transactions, NULL, doc_Database_supports_transactions,
     NULL},
    {"supports_collections", (getter)Database_get_supports_collections, NULL, doc_Database_supports_collections, NULL},
    {"supports_snapshots", (getter)Database_get_supports_snapshots, NULL, doc_Database_supports_snapshots, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyMethodDef Database_methods[] = {
    {"close", (PyCFunction)Database_close, METH_NOARGS, doc_Database_close},
    {"__enter__", (PyCFunction)Database_enter, METH_NOARGS, NULL},
    {"__exit__", (PyCFunction)Database_exit, ustore_py_method_flags_k, NULL},
    {"control", (PyCFunction)Database_control, METH_O, doc_Database_control},

    {"collection_create", (PyCFunction)Database_collection_create, ustore_py_method_flags_k,
     doc_Database_collection_create},
    {"collection_find", (PyCFunction)Database_collection_find, METH_O, doc_Database_collection_find},
    {"collection_contains", (PyCFunction)Database_collection_contains, METH_O, doc_Database_collection_contains},
    {"collection_list", (PyCFunction)Database_collection_list, METH_NOARGS, doc_Database_collection_list},
    {"collection_names", (PyCFunction)Database_collection_names, ustore_py_method_flags_k,
     doc_Database_collection_names},
    {"collection_drop", (PyCFunction)Database_collection_drop, ustore_py_method_flags_k, doc_Database_collection_drop},

    {"snapshot_create", (PyCFunction)Database_snapshot_create, METH_NOARGS, doc_Database_snapshot_create},
    {"snapshot_list", (PyCFunction)Database_snapshot_list, METH_NOARGS, doc_Database_snapshot_list},
    {"snapshot_drop", (PyCFunction)Database_snapshot_drop, METH_O, doc_Database_snapshot_drop},
    {"snapshot_export", (PyCFunction)Database_snapshot_export, ustore_py_method_flags_k, doc_Database_snapshot_export},
    {"transaction", (PyCFunction)Database_transaction, ustore_py_method_flags_k, doc_Database_transaction},

    {"read", (PyCFunction)ustore_py_api_read, ustore_py_method_flags_k, doc_read},
    {"write", (PyCFunction)ustore_py_api_write, ustore_py_method_flags_k, doc_write},
    {"scan", (PyCFunction)ustore_py_api_scan, ustore_py_method_flags_k, doc_scan},
    {"scan_batch", (PyCFunction)ustore_py_api_scan_batch, ustore_py_method_flags_k, doc_scan_batch},
    {"sample", (PyCFunction)ustore_py_api_sample, ustore_py_method_flags_k, doc_sample},
    {"sample_batch", (PyCFunction)ustore_py_api_sample_batch, ustore_py_method_flags_k, doc_sample_batch},
    {"measure", (PyCFunction)ustore_py_api_measure, ustore_py_method_flags_k, doc_measure},
    {"get", (PyCFunction)ustore_py_api_get, ustore_py_method_flags_k, doc_get},
    {"set", (PyCFunction)ustore_py_api_set, ustore_py_method_flags_k, doc_set},
    {"pop", (PyCFunction)ustore_py_api_pop, ustore_py_method_flags_k, doc_pop},
    {"contains", (PyCFunction)ustore_py_api_contains, ustore_py_method_flags_k, doc_contains},

    {"docs_write", (PyCFunction)ustore_py_api_docs_write, ustore_py_method_flags_k, doc_docs_write},
    {"docs_read", (PyCFunction)ustore_py_api_docs_read, ustore_py_method_flags_k, doc_docs_read},
    {"docs_gist", (PyCFunction)ustore_py_api_docs_gist, ustore_py_method_flags_k, doc_docs_gist},
    {"docs_gather", (PyCFunction)ustore_py_api_docs_gather, ustore_py_method_flags_k, doc_docs_gather},
    {"graph_upsert_edges", (PyCFunction)ustore_py_api_graph_upsert_edges, ustore_py_method_flags_k,
     doc_graph_upsert_edges},
    {"graph_remove_edges", (PyCFunction)ustore_py_api_graph_remove_edges, ustore_py_method_flags_k,
     doc_graph_remove_edges},
    {"graph_find_edges", (PyCFunction)ustore_py_api_graph_find_edges, ustore_py_method_flags_k, doc_graph_find_edges},
    {"graph_degrees", (PyCFunction)ustore_py_api_graph_degrees, ustore_py_method_flags_k, doc_graph_degrees},
    {"graph_upsert_vertices", (PyCFunction)ustore_py_api_graph_upsert_vertices, ustore_py_method_flags_k,
     doc_graph_upsert_vertices},
    {"graph_remove_vertices", (PyCFunction)ustore_py_api_graph_remove_vertices, ustore_py_method_flags_k,
     doc_graph_remove_vertices},
    {NULL, NULL, 0, NULL},
};

PyTypeObject DatabaseType = {
    PyVarObject_HEAD_INIT(NULL, 0) //
        .tp_name = "ustore.Database",
    .tp_doc = doc_Database,
    .tp_basicsize = sizeof(Database),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = Database_tp_new,
    .tp_dealloc = (destructor)Database_dealloc,
    .tp_repr = (reprfunc)Database_repr,
    .tp_methods = Database_methods,
    .tp_getset = Database_getset,
};

static PyGetSetDef Transaction_getset[] = {
    {"database", (getter)Transaction_get_database, NULL, doc_Transaction_database, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyMethodDef Transaction_methods[] = {
    {"commit", (PyCFunction)Transaction_commit, ustore_py_method_flags_k, doc_Transaction_commit},
    {"stage", (PyCFunction)Transaction_stage, METH_NOARGS, doc_Transaction_stage},
    {"rollback", (PyCFunction)Transaction_rollback, METH_NOARGS, doc_Transaction_rollback},
    {"close", (PyCFunction)Transaction_close, METH_NOARGS, doc_Transaction_close},
    {"__enter__", (PyCFunction)Transaction_enter, METH_NOARGS, NULL},
    {"__exit__", (PyCFunction)Transaction_exit, ustore_py_method_flags_k, NULL},

    {"read", (PyCFunction)ustore_py_api_read, ustore_py_method_flags_k, doc_read},
    {"write", (PyCFunction)ustore_py_api_write, ustore_py_method_flags_k, doc_write},
    {"scan", (PyCFunction)ustore_py_api_scan, ustore_py_method_flags_k, doc_scan},
    {"scan_batch", (PyCFunction)ustore_py_api_scan_batch, ustore_py_method_flags_k, doc_scan_batch},
    {"sample", (PyCFunction)ustore_py_api_sample, ustore_py_method_flags_k, doc_sample},
    {"sample_batch", (PyCFunction)ustore_py_api_sample_batch, ustore_py_method_flags_k, doc_sample_batch},
    {"measure", (PyCFunction)ustore_py_api_measure, ustore_py_method_flags_k, doc_measure},
    {"get", (PyCFunction)ustore_py_api_get, ustore_py_method_flags_k, doc_get},
    {"set", (PyCFunction)ustore_py_api_set, ustore_py_method_flags_k, doc_set},
    {"pop", (PyCFunction)ustore_py_api_pop, ustore_py_method_flags_k, doc_pop},
    {"contains", (PyCFunction)ustore_py_api_contains, ustore_py_method_flags_k, doc_contains},

    {"docs_write", (PyCFunction)ustore_py_api_docs_write, ustore_py_method_flags_k, doc_docs_write},
    {"docs_read", (PyCFunction)ustore_py_api_docs_read, ustore_py_method_flags_k, doc_docs_read},
    {"docs_gist", (PyCFunction)ustore_py_api_docs_gist, ustore_py_method_flags_k, doc_docs_gist},
    {"docs_gather", (PyCFunction)ustore_py_api_docs_gather, ustore_py_method_flags_k, doc_docs_gather},
    {"graph_upsert_edges", (PyCFunction)ustore_py_api_graph_upsert_edges, ustore_py_method_flags_k,
     doc_graph_upsert_edges},
    {"graph_remove_edges", (PyCFunction)ustore_py_api_graph_remove_edges, ustore_py_method_flags_k,
     doc_graph_remove_edges},
    {"graph_find_edges", (PyCFunction)ustore_py_api_graph_find_edges, ustore_py_method_flags_k, doc_graph_find_edges},
    {"graph_degrees", (PyCFunction)ustore_py_api_graph_degrees, ustore_py_method_flags_k, doc_graph_degrees},
    {"graph_upsert_vertices", (PyCFunction)ustore_py_api_graph_upsert_vertices, ustore_py_method_flags_k,
     doc_graph_upsert_vertices},
    {"graph_remove_vertices", (PyCFunction)ustore_py_api_graph_remove_vertices, ustore_py_method_flags_k,
     doc_graph_remove_vertices},
    {NULL, NULL, 0, NULL},
};

PyTypeObject TransactionType = {
    PyVarObject_HEAD_INIT(NULL, 0) //
        .tp_name = "ustore.Transaction",
    .tp_doc = doc_Transaction,
    .tp_basicsize = sizeof(Transaction),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = Transaction_tp_new,
    .tp_dealloc = (destructor)Transaction_dealloc,
    .tp_repr = (reprfunc)Transaction_repr,
    .tp_methods = Transaction_methods,
    .tp_getset = Transaction_getset,
};

#pragma endregion Tables
