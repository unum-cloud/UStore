/**
 *  @file python/db.c
 *  @author Ash Vardanian
 *  @date September 16, 2026
 *  @brief Databases, collections, snapshots, transactions and the views that run their calls.
 *
 *  A database owns its directory and answers what its engine supports. A transaction or a snapshot
 *  holds a reference to the database object it answers as `database`, while C holds the database
 *  itself open until the transaction or snapshot is freed, so either may close first. A @c View or
 *  a @c DeferredView holds its origin and its executor the same way, and every @c Deferred holds
 *  its origin and its arena, while the C frees wait for the deferred calls naming a handle, so
 *  closing one never needs to count them.
 */
#include "lib.h"

#pragma region Opening

/** A size keyword of @c Database.open_nvme and the field of the init call it fills. */
typedef struct nvme_size_keyword_t {

    /** The keyword, spelled as the field, or NULL closing the table. */
    char const* name;

    /** Where the field sits in @c ustore_database_open_nvme_t. */
    size_t offset;
} nvme_size_keyword_t;

/** Every size keyword of @c Database.open_nvme, closed by a nameless row. */
static nvme_size_keyword_t const nvme_size_keywords[] = {
    {"memory_limit", offsetof(ustore_database_open_nvme_t, memory_limit)},
    {"transaction_max_bytes", offsetof(ustore_database_open_nvme_t, transaction_max_bytes)},
    {"transaction_max_deltas", offsetof(ustore_database_open_nvme_t, transaction_max_deltas)},
    {"collection_concurrency_limit", offsetof(ustore_database_open_nvme_t, collection_concurrency_limit)},
    {"collection_cache_limit", offsetof(ustore_database_open_nvme_t, collection_cache_limit)},
    {"collection_memory_runs_max_bytes", offsetof(ustore_database_open_nvme_t, collection_memory_runs_max_bytes)},
    {"collection_memory_runs_max_deltas", offsetof(ustore_database_open_nvme_t, collection_memory_runs_max_deltas)},
    {"collection_level_enlarge_factor", offsetof(ustore_database_open_nvme_t, collection_level_enlarge_factor)},
    {"collection_value_size", offsetof(ustore_database_open_nvme_t, collection_value_size)},
    {"collection_value_max_size", offsetof(ustore_database_open_nvme_t, collection_value_max_size)},
    {NULL, 0},
};

/** What an init call borrows from the arguments of a constructor, kept alive until the call returns. */
typedef struct opening_t {

    /** The executor the init runs on, borrowed from the arguments, or NULL for the calling thread. */
    ustore_executor_t executor;

    /** The encoded directory, or NULL for none. */
    PyObject* directory;

    /** The encoded paths @c data_directories point into, or NULL for none. */
    PyObject* paths;

    /** The data directories, or NULL for none. */
    ustore_data_directory_t* data_directories;

    /** Entries of @c data_directories. */
    size_t data_directories_count;
} opening_t;

/** Releases whatever @p opening holds. */
static void opening_release(opening_t* opening) {
    Py_XDECREF(opening->directory);
    Py_XDECREF(opening->paths);
    PyMem_Free(opening->data_directories);
}

/** Encodes @p object, a str, bytes or PathLike, as the directory, leaving None as no directory. */
static int opening_directory(opening_t* opening, PyObject* object) {
    if (opening->directory) {
        PyErr_SetString(PyExc_TypeError, "The directory was given twice");
        return -1;
    }
    if (object == Py_None) return 0;
    return PyUnicode_FSConverter(object, &opening->directory) ? 0 : -1;
}

/** The bytes of @p path, a bytes object encoded by @c PyUnicode_FSConverter, or a zeroed view for NULL. */
static ustore_bytes_view_t path_bytes(PyObject* path) {
    ustore_bytes_view_t bytes = {0};
    if (!path) return bytes;
    bytes.data = (uint8_t const*)PyBytes_AS_STRING(path);
    bytes.length = (ustore_size_t)PyBytes_GET_SIZE(path);
    return bytes;
}

/** Reads @p object, an @c Executor or None, as the executor the init runs on. */
static int opening_executor(opening_t* opening, PyObject* object) {
    if (object == Py_None) return 0;
    if (!PyObject_TypeCheck(object, &ExecutorType)) {
        PyErr_Format(PyExc_TypeError, "The 'executor' option takes an Executor or None, got %S", Py_TYPE(object));
        return -1;
    }
    opening->executor = ((Executor*)object)->handle;
    if (opening->executor) return 0;
    return ustore_py_raise(ustore_status_uninitialized_k, "The executor is closed");
}

/**
 *  @brief Reads the positional arguments of a constructor, which name the directory at most.
 *  @param[in] verb The constructor's name, as a failure should read it.
 *  @return The keyword count, or -1 with an exception set.
 */
static Py_ssize_t opening_positional(opening_t* opening, char const* verb, PyObject* const* args,
                                     Py_ssize_t positional_args_count, PyObject* args_names_tuple) {
    if (positional_args_count > 1) {
        PyErr_Format(PyExc_TypeError, "Database.%s() takes at most one positional argument", verb);
        return -1;
    }
    if (positional_args_count == 1 && opening_directory(opening, args[0]) != 0) return -1;
    return args_names_tuple ? PyTuple_GET_SIZE(args_names_tuple) : 0;
}

/** Reads @p object, a sequence of paths or of @b (path,max_bytes) pairs, as the data directories. */
static int opening_data_directories(opening_t* opening, PyObject* object) {
    PyObject* const items = PySequence_Fast(object, "The 'data_directories' option must be a sequence");
    if (!items) return -1;
    Py_ssize_t const count = PySequence_Fast_GET_SIZE(items);
    Py_XDECREF(opening->paths);
    PyMem_Free(opening->data_directories);
    opening->paths = PyList_New(count);
    opening->data_directories = PyMem_Calloc((size_t)count + 1, sizeof(ustore_data_directory_t));
    opening->data_directories_count = (size_t)count;
    if (!opening->paths || !opening->data_directories) {
        Py_DECREF(items);
        if (!PyErr_Occurred()) PyErr_NoMemory();
        return -1;
    }
    for (Py_ssize_t index = 0; index != count; ++index) {
        PyObject* path = PySequence_Fast_GET_ITEM(items, index);
        size_t max_bytes = 0;
        if (PyTuple_Check(path)) {
            if (PyTuple_GET_SIZE(path) != 2) {
                Py_DECREF(items);
                PyErr_SetString(PyExc_TypeError, "A data directory is a path or a (path, max_bytes) pair");
                return -1;
            }
            if (ustore_py_size_from_object(PyTuple_GET_ITEM(path, 1), "data_directories", &max_bytes) != 0) {
                Py_DECREF(items);
                return -1;
            }
            path = PyTuple_GET_ITEM(path, 0);
        }
        PyObject* encoded = NULL;
        if (!PyUnicode_FSConverter(path, &encoded)) {
            Py_DECREF(items);
            return -1;
        }
        PyList_SET_ITEM(opening->paths, index, encoded);
        opening->data_directories[index].path = path_bytes(encoded);
        opening->data_directories[index].max_bytes = (ustore_size_t)max_bytes;
    }
    Py_DECREF(items);
    return 0;
}

/** Wraps @p handle, which an init call opened unless @p outcome failed, in a new @c Database. */
static PyObject* database_opened(ustore_database_t handle, ustore_outcome_t* outcome) {
    if (ustore_py_raise_on_outcome(outcome) != 0) return NULL;

    ustore_database_description_t description = {0};
    ustore_database_describe_t describe = {0};
    describe.database = handle;
    describe.outcome = outcome;
    describe.description = &description;
    ustore_database_describe(&describe);
    Database* const instance = ustore_py_raise_on_outcome(outcome) == 0
                                   ? (Database*)DatabaseType.tp_alloc(&DatabaseType, 0)
                                   : NULL;
    if (!instance) {
        ustore_database_free(handle);
        return NULL;
    }
    instance->handle = handle;
    instance->capabilities = description.capabilities;
    return (PyObject*)instance;
}

#pragma endregion Opening

#pragma region Databases

static char const doc_Database[] =                                                             //
    "An embedded database, opened through one class method per engine.\n"                      //
    "\n"                                                                                       //
    "Database.open_ram(), open_rocksdb(), view_rocksdb(), open_leveldb() and open_nvme()\n"    //
    "each take what their engine can be asked; engines_compiled() names the ones this build\n" //
    "carries, and the others refuse to open. Each takes an `executor` keyword and blocks\n"    //
    "on it, as it blocks on the calling thread without one. The directory belongs to\n"        //
    "the database, so no two processes may open the same one at once. Every database holds\n"  //
    "a main collection, which can be cleared but never dropped, beside the collections it\n"   //
    "creates.\n"                                                                               //
    "\n"                                                                                       //
    "Indexing it by name, as `db['movies']`, answers a Collection, whose `blobs`, `docs`,\n"   //
    "`graph` and `table` views create it on their first write. Its verbs run on the calling\n" //
    "thread; `on(executor)` and `deferred(executor)` answer views running them on threads.";

static PyObject* Database_tp_new(PyTypeObject* Py_UNUSED(type), PyObject* Py_UNUSED(args),
                                 PyObject* Py_UNUSED(keywords)) {
    PyErr_SetString(PyExc_TypeError,
                    "Database() names no engine: use open_ram, open_rocksdb, view_rocksdb, open_leveldb or open_nvme");
    return NULL;
}

static char const doc_Database_open_ram[] =                                                  //
    "open_ram(directory=None, *, isolation=Isolation.STRICT_SERIALIZABLE, executor=None)\n"  //
    "answers a database in memory.\n"                                                        //
    "\n"                                                                                     //
    "Calls `ustore_database_open_ram` with a directory and `ustore_database_init_ram`\n"     //
    "without one.\n"                                                                         //
    "\n"                                                                                     //
    "Args:\n"                                                                                //
    "  directory (str | bytes | PathLike, optional): Where the database persists as one\n"   //
    "    image, rewritten whole on a flush and on close; None keeps it in memory alone.\n"   //
    "  isolation (Isolation): How transactions see each other.\n"                            //
    "  executor (Executor, optional): The executor the open blocks on, or None to open on\n" //
    "    the calling thread.\n"                                                              //
    "Returns:\n"                                                                             //
    "  Database: The opened database.";

static PyObject* Database_open_ram(PyObject* Py_UNUSED(type), PyObject* const* args, Py_ssize_t positional_args_count,
                                   PyObject* args_names_tuple) {
    opening_t opening = {0};
    int isolation = ustore_isolation_strict_serializable_k;
    Py_ssize_t const args_names_count = opening_positional(&opening, "open_ram", args, positional_args_count,
                                                           args_names_tuple);
    if (args_names_count < 0) goto failed;
    for (Py_ssize_t names_progress = 0; names_progress != args_names_count; ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(args_names_tuple, names_progress);
        PyObject* const value = args[positional_args_count + names_progress];
        int read = -1;
        if (PyUnicode_CompareWithASCIIString(name, "directory") == 0) read = opening_directory(&opening, value);
        else if (PyUnicode_CompareWithASCIIString(name, "isolation") == 0)
            read = ustore_py_enum_from_object(value, "isolation", ustore_py_enum_isolation_k, &isolation);
        else if (PyUnicode_CompareWithASCIIString(name, "executor") == 0) read = opening_executor(&opening, value);
        else PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
        if (read != 0) goto failed;
    }

    ustore_database_t handle = NULL;
    ustore_outcome_t outcome = {0};
    ustore_database_init_ram_t init_ram = {
        .database = &handle,
        .executor = opening.executor,
        .outcome = &outcome,
        .isolation = (ustore_isolation_t)isolation,
    };
    ustore_database_open_ram_t open_ram = {
        .database = &handle,
        .executor = opening.executor,
        .outcome = &outcome,
        .directory = path_bytes(opening.directory),
        .isolation = (ustore_isolation_t)isolation,
    };
    Py_BEGIN_ALLOW_THREADS;
    if (opening.directory) ustore_database_open_ram(&open_ram);
    else ustore_database_init_ram(&init_ram);
    Py_END_ALLOW_THREADS;
    opening_release(&opening);
    return database_opened(handle, &outcome);

failed:
    opening_release(&opening);
    return NULL;
}

static char const doc_Database_open_rocksdb[] =                                              //
    "open_rocksdb(directory, *, data_directories=(), executor=None) answers a database on\n" //
    "RocksDB.\n"                                                                             //
    "\n"                                                                                     //
    "Args:\n"                                                                                //
    "  directory (str | bytes | PathLike): The database's directory, which must exist.\n"    //
    "  data_directories (Sequence): Directories the table files fill in order, each a\n"     //
    "    path or a (path, max_bytes) pair, where zero bytes means no limit.\n"               //
    "  executor (Executor, optional): The executor the open blocks on, or None to open on\n" //
    "    the calling thread.\n"                                                              //
    "Returns:\n"                                                                             //
    "  Database: The opened database.\n"                                                     //
    "Raises:\n"                                                                              //
    "  UnsupportedError: This build carries no RocksDB engine.";

/** Reads the arguments both RocksDB constructors take: the directory, the data directories and the executor. */
static int rocksdb_opening(opening_t* opening, char const* verb, PyObject* const* args,
                           Py_ssize_t positional_args_count, PyObject* args_names_tuple) {
    Py_ssize_t const args_names_count = opening_positional(opening, verb, args, positional_args_count,
                                                           args_names_tuple);
    if (args_names_count < 0) return -1;
    for (Py_ssize_t names_progress = 0; names_progress != args_names_count; ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(args_names_tuple, names_progress);
        PyObject* const value = args[positional_args_count + names_progress];
        int read = -1;
        if (PyUnicode_CompareWithASCIIString(name, "directory") == 0) read = opening_directory(opening, value);
        else if (PyUnicode_CompareWithASCIIString(name, "data_directories") == 0)
            read = opening_data_directories(opening, value);
        else if (PyUnicode_CompareWithASCIIString(name, "executor") == 0) read = opening_executor(opening, value);
        else PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
        if (read != 0) return -1;
    }
    return 0;
}

static PyObject* Database_open_rocksdb(PyObject* Py_UNUSED(type), PyObject* const* args,
                                       Py_ssize_t positional_args_count, PyObject* args_names_tuple) {
    opening_t opening = {0};
    if (rocksdb_opening(&opening, "open_rocksdb", args, positional_args_count, args_names_tuple) != 0) {
        opening_release(&opening);
        return NULL;
    }
    ustore_database_t handle = NULL;
    ustore_outcome_t outcome = {0};
    ustore_database_open_rocksdb_t open_rocksdb = {
        .database = &handle,
        .executor = opening.executor,
        .outcome = &outcome,
        .directory = path_bytes(opening.directory),
        .data_directories = opening.data_directories,
        .data_directories_count = (ustore_size_t)opening.data_directories_count,
    };
    Py_BEGIN_ALLOW_THREADS;
    ustore_database_open_rocksdb(&open_rocksdb);
    Py_END_ALLOW_THREADS;
    opening_release(&opening);
    return database_opened(handle, &outcome);
}

static char const doc_Database_view_rocksdb[] =                                              //
    "view_rocksdb(directory, *, data_directories=(), executor=None) answers a read-only\n"   //
    "database on RocksDB.\n"                                                                 //
    "\n"                                                                                     //
    "Reads the state the directory held when it opened, and refuses every call that would\n" //
    "change it with ReadOnlyError: writes, removes, transactions, collection creates and\n"  //
    "drops, clears and compactions. Snapshots and their saves still serve.\n"                //
    "\n"                                                                                     //
    "Args:\n"                                                                                //
    "  directory (str | bytes | PathLike): The directory of an existing database.\n"         //
    "  data_directories (Sequence): The directories the table files fill, as the database\n" //
    "    was opened with, each a path or a (path, max_bytes) pair.\n"                        //
    "  executor (Executor, optional): The executor the open blocks on, or None to open on\n" //
    "    the calling thread.\n"                                                              //
    "Returns:\n"                                                                             //
    "  Database: The opened database.\n"                                                     //
    "Raises:\n"                                                                              //
    "  UnsupportedError: This build carries no RocksDB engine.";

static PyObject* Database_view_rocksdb(PyObject* Py_UNUSED(type), PyObject* const* args,
                                       Py_ssize_t positional_args_count, PyObject* args_names_tuple) {
    opening_t opening = {0};
    if (rocksdb_opening(&opening, "view_rocksdb", args, positional_args_count, args_names_tuple) != 0) {
        opening_release(&opening);
        return NULL;
    }
    ustore_database_t handle = NULL;
    ustore_outcome_t outcome = {0};
    ustore_database_view_rocksdb_t view_rocksdb = {
        .database = &handle,
        .executor = opening.executor,
        .outcome = &outcome,
        .directory = path_bytes(opening.directory),
        .data_directories = opening.data_directories,
        .data_directories_count = (ustore_size_t)opening.data_directories_count,
    };
    Py_BEGIN_ALLOW_THREADS;
    ustore_database_view_rocksdb(&view_rocksdb);
    Py_END_ALLOW_THREADS;
    opening_release(&opening);
    return database_opened(handle, &outcome);
}

static char const doc_Database_open_leveldb[] =                                              //
    "open_leveldb(directory, *, executor=None) answers a database on LevelDB.\n"             //
    "\n"                                                                                     //
    "Args:\n"                                                                                //
    "  directory (str | bytes | PathLike): The database's directory, which must exist.\n"    //
    "  executor (Executor, optional): The executor the open blocks on, or None to open on\n" //
    "    the calling thread.\n"                                                              //
    "Returns:\n"                                                                             //
    "  Database: The opened database.\n"                                                     //
    "Raises:\n"                                                                              //
    "  UnsupportedError: This build carries no LevelDB engine.";

static PyObject* Database_open_leveldb(PyObject* Py_UNUSED(type), PyObject* const* args,
                                       Py_ssize_t positional_args_count, PyObject* args_names_tuple) {
    opening_t opening = {0};
    Py_ssize_t const args_names_count = opening_positional(&opening, "open_leveldb", args, positional_args_count,
                                                           args_names_tuple);
    if (args_names_count < 0) goto failed;
    for (Py_ssize_t names_progress = 0; names_progress != args_names_count; ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(args_names_tuple, names_progress);
        PyObject* const value = args[positional_args_count + names_progress];
        int read = -1;
        if (PyUnicode_CompareWithASCIIString(name, "directory") == 0) read = opening_directory(&opening, value);
        else if (PyUnicode_CompareWithASCIIString(name, "executor") == 0) read = opening_executor(&opening, value);
        else PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
        if (read != 0) goto failed;
    }

    ustore_database_t handle = NULL;
    ustore_outcome_t outcome = {0};
    ustore_database_open_leveldb_t open_leveldb = {
        .database = &handle,
        .executor = opening.executor,
        .outcome = &outcome,
        .directory = path_bytes(opening.directory),
    };
    Py_BEGIN_ALLOW_THREADS;
    ustore_database_open_leveldb(&open_leveldb);
    Py_END_ALLOW_THREADS;
    opening_release(&opening);
    return database_opened(handle, &outcome);

failed:
    opening_release(&opening);
    return NULL;
}

static char const doc_Database_open_nvme[] =                                                    //
    "open_nvme(directory, *, executor=None, **sizes) answers a database on NVMe.\n"             //
    "\n"                                                                                        //
    "Every size keyword names a field of ustore_database_open_nvme_t, like memory_limit,\n"     //
    "transaction_max_bytes or collection_cache_limit, where zero keeps the engine's default.\n" //
    "The database moves bytes as an executor of Executor.for_nvme() says, read once here,\n"    //
    "and with that call's defaults for any other executor and for none.\n"                      //
    "\n"                                                                                        //
    "Args:\n"                                                                                   //
    "  directory (str | bytes | PathLike): The directory of the database's own files.\n"        //
    "  executor (Executor, optional): The executor the open blocks on, or None to open on\n"    //
    "    the calling thread.\n"                                                                 //
    "  data_directories (Sequence): Directories the volumes fill, each a path or a\n"           //
    "    (path, max_bytes) pair; none keeps one volume inside the directory.\n"                 //
    "Returns:\n"                                                                                //
    "  Database: The opened database.\n"                                                        //
    "Raises:\n"                                                                                 //
    "  UnsupportedError: This build carries no NVMe engine.";

static PyObject* Database_open_nvme(PyObject* Py_UNUSED(type), PyObject* const* args, Py_ssize_t positional_args_count,
                                    PyObject* args_names_tuple) {
    opening_t opening = {0};
    ustore_database_open_nvme_t open_nvme = {0};
    Py_ssize_t const args_names_count = opening_positional(&opening, "open_nvme", args, positional_args_count,
                                                           args_names_tuple);
    if (args_names_count < 0) goto failed;
    for (Py_ssize_t names_progress = 0; names_progress != args_names_count; ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(args_names_tuple, names_progress);
        PyObject* const value = args[positional_args_count + names_progress];
        nvme_size_keyword_t const* keyword = nvme_size_keywords;
        while (keyword->name && PyUnicode_CompareWithASCIIString(name, keyword->name) != 0) ++keyword;
        size_t size = 0;
        int read = -1;
        if (keyword->name) {
            read = ustore_py_size_from_object(value, keyword->name, &size);
            *(ustore_size_t*)((char*)&open_nvme + keyword->offset) = (ustore_size_t)size;
        }
        else if (PyUnicode_CompareWithASCIIString(name, "directory") == 0) read = opening_directory(&opening, value);
        else if (PyUnicode_CompareWithASCIIString(name, "executor") == 0) read = opening_executor(&opening, value);
        else if (PyUnicode_CompareWithASCIIString(name, "data_directories") == 0)
            read = opening_data_directories(&opening, value);
        else PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
        if (read != 0) goto failed;
    }

    ustore_database_t handle = NULL;
    ustore_outcome_t outcome = {0};
    open_nvme.database = &handle;
    open_nvme.executor = opening.executor;
    open_nvme.outcome = &outcome;
    open_nvme.directory = path_bytes(opening.directory);
    open_nvme.data_directories = opening.data_directories;
    open_nvme.data_directories_count = (ustore_size_t)opening.data_directories_count;
    Py_BEGIN_ALLOW_THREADS;
    ustore_database_open_nvme(&open_nvme);
    Py_END_ALLOW_THREADS;
    opening_release(&opening);
    return database_opened(handle, &outcome);

failed:
    opening_release(&opening);
    return NULL;
}

/** Closes @p database once the deferred calls naming it finish. */
static void database_close(Database* database) {
    ustore_database_t const handle = database->handle;
    if (!handle) return;
    database->handle = NULL;

    Py_BEGIN_ALLOW_THREADS;
    ustore_database_free(handle);
    Py_END_ALLOW_THREADS;
}

static void Database_dealloc(Database* self) {
    database_close(self);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static char const doc_Database_close[] =                                                        //
    "close() answers None.\n"                                                                   //
    "\n"                                                                                        //
    "Closes the database once every deferred call on it finishes, while the data persists on\n" //
    "disk. Transactions and snapshots it opened keep working, as each holds the database\n"     //
    "open until it closes too.";

static PyObject* Database_close(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    database_close((Database*)self);
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

/** A new @c Capabilities of what @p flags marks, or NULL with an exception set. */
static PyObject* capabilities_object(ustore_capabilities_t flags) {
    PyObject* const capabilities = PyStructSequence_New(CapabilitiesType);
    if (!capabilities) return NULL;
    ustore_capabilities_t const named[3] = {ustore_capabilities_transactions_k, ustore_capabilities_collections_k,
                                            ustore_capabilities_snapshots_k};
    for (Py_ssize_t index = 0; index != 3; ++index)
        PyStructSequence_SetItem(capabilities, index, PyBool_FromLong((flags & named[index]) != 0));
    return capabilities;
}

static char const doc_Database_capabilities[] =
    "What the engine supports, as a Capabilities of `transactions`, `collections` and `snapshots`.";

static PyObject* Database_get_capabilities(Database* self, void* Py_UNUSED(closure)) {
    return capabilities_object(self->capabilities);
}

/** Fills @p context for a verb of the database itself, refusing a view of a transaction or of a snapshot. */
static int database_context(PyObject* self, char const* verb, ustore_py_context_t* context) {
    if (ustore_py_context_from_object(self, context) != 0) return -1;
    if (!context->transaction && !context->snapshot) return 0;
    PyErr_Format(PyExc_TypeError, "%s() needs a view of a database", verb);
    return -1;
}

/**
 *  @brief Reads the `progress` keyword a verb takes after its @p required positional arguments, its only keyword.
 *  @param[out] progress The callable, borrowed, or NULL when the call carried none.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
static int progress_keyword(char const* verb, PyObject* const* args, Py_ssize_t positional_args_count,
                            PyObject* args_names_tuple, Py_ssize_t required, PyObject** progress) {
    *progress = NULL;
    Py_ssize_t const args_names_count = ustore_py_args_names_count_checked(verb, positional_args_count,
                                                                           args_names_tuple, required);
    if (args_names_count < 0) return -1;
    if (args_names_count == 0) return 0;
    if (args_names_count == 1 &&
        PyUnicode_CompareWithASCIIString(PyTuple_GET_ITEM(args_names_tuple, 0), "progress") == 0)
        return ustore_py_progress_from_object(args[required], progress);
    PyErr_Format(PyExc_TypeError, "%s() takes 'progress' as its only keyword", verb);
    return -1;
}

/** Lets go of a call that holds nothing beyond its header. */
static void header_release(void* call) { ustore_py_call_release((ustore_py_call_t*)call); }

static char const doc_Database_clear[] =                                                      //
    "clear(mode) answers None.\n"                                                             //
    "\n"                                                                                      //
    "Empties every collection at once at the head, as `collection_drop` empties one. Calls\n" //
    "`ustore_database_clear`.\n"                                                              //
    "\n"                                                                                      //
    "Args:\n"                                                                                 //
    "  mode (DropMode): `DropMode.PAIRS` removes every key and value and keeps the\n"         //
    "    collections, `DropMode.COLLECTION` drops every collection but the main one with\n"   //
    "    its name and empties the main one, and `DropMode.VALUES` empties every value and\n"  //
    "    keeps the keys.\n"                                                                   //
    "Raises:\n"                                                                               //
    "  InvalidArgumentError: The RAM and RocksDB engines drop no collection while a\n"        //
    "    snapshot is open, since the snapshot belongs to its holder.\n"                       //
    "  ReadOnlyError: The database was opened read-only.";

static void clear_submit(void* arguments) { ustore_database_clear((ustore_database_clear_t*)arguments); }

static ustore_py_verb_t const clear_verb = {sizeof(ustore_py_call_t), clear_submit, ustore_py_call_answer_none,
                                            header_release};

static PyObject* Database_clear(PyObject* self, PyObject* mode_object) {
    ustore_py_context_t context;
    int mode = ustore_drop_pairs_k;
    if (database_context(self, "clear", &context) != 0 ||
        ustore_py_enum_from_object(mode_object, "mode", ustore_py_enum_drop_mode_k, &mode) != 0)
        return NULL;
    ustore_py_call_t stack;
    ustore_py_call_t* const call = ustore_py_call_open(&context, &clear_verb, &stack);
    if (!call) return NULL;
    ustore_database_clear_t clear = {
        .database = context.database_handle,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->outcome,
        .completion = context.completion,
        .mode = (ustore_drop_mode_t)mode,
    };
    return ustore_py_call_run(&context, &clear_verb, call, &clear);
}

static char const doc_Database_compact[] =                                                     //
    "compact(*, progress=None) answers None.\n"                                                //
    "\n"                                                                                       //
    "Reclaims the space overwritten and removed data still takes, one collection at a time:\n" //
    "RocksDB and LevelDB flush and compact their trees, and the RAM engine frees the\n"        //
    "versions no reader needs and rewrites its image. Calls `ustore_database_compact`.\n"      //
    "\n"                                                                                       //
    "Args:\n"                                                                                  //
    "  progress (Callable[[int, int], bool | None], optional): Called with the units\n"        //
    "    compacted and the units in all, bytes where the engine measures them, as RocksDB\n"   //
    "    does, and collections elsewhere. A falsy answer other than None stops between\n"      //
    "    collections, keeping what was compacted.\n"                                           //
    "Raises:\n"                                                                                //
    "  CancelledError: The progress callable stopped the compaction.\n"                        //
    "  ReadOnlyError: The database was opened read-only.";

static void compact_submit(void* arguments) { ustore_database_compact((ustore_database_compact_t*)arguments); }

static ustore_py_verb_t const compact_verb = {sizeof(ustore_py_call_t), compact_submit, ustore_py_call_answer_none,
                                              header_release};

static PyObject* Database_compact(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                  PyObject* args_names_tuple) {
    ustore_py_context_t context;
    PyObject* progress = NULL;
    if (database_context(self, "compact", &context) != 0 ||
        progress_keyword("compact", args, positional_args_count, args_names_tuple, 0, &progress) != 0)
        return NULL;
    ustore_py_call_t stack;
    ustore_py_call_t* const call = ustore_py_call_open(&context, &compact_verb, &stack);
    if (!call) return NULL;
    call->progress = Py_XNewRef(progress);
    ustore_database_compact_t compact = {
        .database = context.database_handle,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->outcome,
        .completion = context.completion,
        .progress = ustore_py_progress_callback(call),
        .progress_state = call,
    };
    return ustore_py_call_run(&context, &compact_verb, call, &compact);
}

static char const doc_Database_describe[] =                                                  //
    "describe() answers a Description.\n"                                                    //
    "\n"                                                                                     //
    "Reports the engine serving the database, what it supports, its isolation, and the\n"    //
    "keys, memory and disk it holds, every size the engine's estimate and zero where it\n"   //
    "keeps none. Calls `ustore_database_describe`.\n"                                        //
    "\n"                                                                                     //
    "Returns:\n"                                                                             //
    "  Description: The `capabilities`, `engine`, `isolation`, `keys`, `memory_bytes` and\n" //
    "  `disk_bytes` of the database.";

/** A description of the database, which the library fills whole. */
typedef struct describe_call_t {

    /** What the call keeps until it finishes. */
    ustore_py_call_t call;

    /** Output slot for the description. */
    ustore_database_description_t description;
} describe_call_t;

static void describe_submit(void* arguments) { ustore_database_describe((ustore_database_describe_t*)arguments); }

static PyObject* describe_answer(void* erased) {
    ustore_database_description_t const* const description = &((describe_call_t const*)erased)->description;
    PyObject* fields[6] = {capabilities_object(description->capabilities)};
    fields[1] = fields[0] ? ustore_py_enum_to_object(ustore_py_enum_engine_k, (int)description->engine) : NULL;
    fields[2] = fields[1] ? ustore_py_enum_to_object(ustore_py_enum_isolation_k, (int)description->isolation) : NULL;
    fields[3] = fields[2] ? PyLong_FromUnsignedLongLong(description->keys) : NULL;
    fields[4] = fields[3] ? PyLong_FromUnsignedLongLong(description->memory_bytes) : NULL;
    fields[5] = fields[4] ? PyLong_FromUnsignedLongLong(description->disk_bytes) : NULL;
    PyObject* const described = fields[5] ? PyStructSequence_New(DescriptionType) : NULL;
    for (Py_ssize_t index = 0; index != 6; ++index)
        if (described) PyStructSequence_SetItem(described, index, fields[index]);
        else Py_XDECREF(fields[index]);
    return described;
}

static ustore_py_verb_t const describe_verb = {sizeof(describe_call_t), describe_submit, describe_answer,
                                               header_release};

static PyObject* Database_describe(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    ustore_py_context_t context;
    if (database_context(self, "describe", &context) != 0) return NULL;
    describe_call_t stack;
    describe_call_t* const call = ustore_py_call_open(&context, &describe_verb, &stack);
    if (!call) return NULL;
    ustore_database_describe_t describe = {
        .database = context.database_handle,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->call.outcome,
        .completion = context.completion,
        .description = &call->description,
    };
    return ustore_py_call_run(&context, &describe_verb, call, &describe);
}

#pragma endregion Databases

#pragma region Collections

static char const doc_Database_collection_create[] =                                               //
    "collection_create(name, *, modality=Modality.ANY) answers the new collection's identifier.\n" //
    "\n"                                                                                           //
    "Creates an empty collection and records the name for it, in the two phases a name\n"          //
    "always resolves through. Every other verb addresses the collection by the identifier\n"       //
    "this answers.\n"                                                                              //
    "\n"                                                                                           //
    "Args:\n"                                                                                      //
    "  name (str): The name to record, which must not already be taken.\n"                         //
    "  modality (Modality): What the collection holds.\n"                                          //
    "Returns:\n"                                                                                   //
    "  UUID: The identifier addressing the new collection.\n"                                      //
    "Raises:\n"                                                                                    //
    "  AlreadyExistsError: A collection already carries the name.\n"                               //
    "  UnsupportedError: The engine holds only the main collection.";

static PyObject* Database_collection_create(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                            PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;

    Py_ssize_t const args_names_count = ustore_py_args_names_count_checked("collection_create", positional_args_count,
                                                                           args_names_tuple, 1);
    if (args_names_count < 0) return NULL;

    int modality = ustore_modality_any_k;
    for (Py_ssize_t names_progress = 0, args_progress = positional_args_count; names_progress != args_names_count;
         ++args_progress, ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(args_names_tuple, names_progress);
        if (PyUnicode_CompareWithASCIIString(name, "modality") == 0) {
            if (ustore_py_enum_from_object(args[args_progress], "modality", ustore_py_enum_modality_k, &modality) != 0)
                return NULL;
        }
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
        ustore_py_raise(ustore_status_invalid_argument_k, "The main collection has no name and always exists");
        return NULL;
    }

    ustore_collection_id_t created = ustore_collection_main_k;
    ustore_outcome_t outcome = {0};
    ustore_collection_create_t create = {0};
    create.database = context.database_handle;
    create.outcome = &outcome;
    create.name.data = (uint8_t const*)name;
    create.name.length = (ustore_size_t)name_length;
    create.modality = (ustore_modality_t)modality;
    create.collection = &created;

    Py_BEGIN_ALLOW_THREADS;
    ustore_collection_create(&create);
    Py_END_ALLOW_THREADS;
    if (ustore_py_raise_on_outcome(&outcome) != 0) return NULL;
    return ustore_py_uuid_to_object(&created.uuid);
}

static char const doc_Database_collection_find[] =                                       //
    "collection_find(name, arena) answers the identifier the name addresses.\n"          //
    "\n"                                                                                 //
    "Reads the name at the snapshot of a snapshot, and at the head otherwise, since a\n" //
    "transaction never creates or drops collections.\n"                                  //
    "\n"                                                                                 //
    "Args:\n"                                                                            //
    "  name (str): The name to resolve; the empty one is the main collection.\n"         //
    "  arena (Arena): Where the lookup lands before it is copied out, rewound by this\n" //
    "    call.\n"                                                                        //
    "Returns:\n"                                                                         //
    "  UUID: The identifier addressing the collection.\n"                                //
    "Raises:\n"                                                                          //
    "  NotFoundError: No collection carries the name.";

/** A lookup of the collection one name addresses, whose identifier lands in the arena its header holds. */
typedef struct find_call_t {

    /** What the call keeps until it finishes, its collection the one given outright. */
    ustore_py_call_t call;

    /** The name looked up, held. */
    PyObject* name;

    /** The UTF-8 bytes @c name caches, which the accessor reads, with no data for a collection given outright. */
    ustore_bytes_view_t text;

    /** Output slot for the presence bit of the name, inside the arena. */
    ustore_octet_t const* presences;

    /** Output slot for the identifier the name addresses, inside the arena. */
    ustore_collection_id_t const* collections;
} find_call_t;

// A collection given by ID, or the main one, needs no lookup, so its call is final at once.
static void find_submit(void* arguments) {
    ustore_collection_find_t* const find = (ustore_collection_find_t*)arguments;
    if (find->count) ustore_collection_find(find);
}

static PyObject* find_answer(void* erased) {
    find_call_t const* const call = (find_call_t const*)erased;
    if (!call->text.data) return ustore_py_uuid_to_object(&call->call.collection.uuid);
    if (ustore_py_present(call->presences, 0)) return ustore_py_uuid_to_object(&call->collections[0].uuid);
    ustore_py_raise(ustore_status_not_found_k, "No collection is named %R", call->name);
    return NULL;
}

static void find_release(void* erased) {
    find_call_t* const call = (find_call_t*)erased;
    Py_CLEAR(call->name);
    ustore_py_call_release(&call->call);
}

static ustore_py_verb_t const find_verb = {sizeof(find_call_t), find_submit, find_answer, find_release};

static PyObject* Database_collection_find(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_args_names_count_checked("collection_find", positional_args_count, NULL, 2) < 0) return NULL;
    PyObject* const name_object = args[0];
    find_call_t stack;
    find_call_t* const call = ustore_py_call_open(&context, &find_verb, &stack);
    if (!call) return NULL;
    Py_ssize_t length = 0;
    int const looked_up = PyUnicode_Check(name_object) && PyUnicode_GET_LENGTH(name_object) != 0;
    if (!looked_up && ustore_py_collection_from_object(&context, name_object, &call->call.collection) != 0)
        return ustore_py_call_discard(&context, &find_verb, call);
    if (looked_up && !(call->text.data = (uint8_t const*)PyUnicode_AsUTF8AndSize(name_object, &length)))
        return ustore_py_call_discard(&context, &find_verb, call);
    call->name = Py_NewRef(name_object);
    call->text.length = (ustore_size_t)length;
    if (ustore_py_arena_claim(args[1], &call->call.arena) != 0)
        return ustore_py_call_discard(&context, &find_verb, call);
    ustore_collection_find_t find = {
        .database = context.database_handle,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->call.outcome,
        .completion = context.completion,
        .snapshot = context.snapshot,
        .count = looked_up,
        .names = ustore_py_strings_repeated(&call->text),
        .presences = &call->presences,
        .collections = &call->collections,
        .arena = ustore_py_arena_handle(call->call.arena),
    };
    return ustore_py_call_run(&context, &find_verb, call, &find);
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

    ustore_collection_id_t found = ustore_collection_main_k;
    switch (ustore_py_collection_named(&context, name, (size_t)length, &found, NULL)) {
    case ustore_py_naming_found_k: Py_RETURN_TRUE;
    case ustore_py_naming_missing_k: Py_RETURN_FALSE;
    case ustore_py_naming_failed_k: return NULL;
    }
    Py_RETURN_FALSE;
}

static char const doc_Database_collection_list[] =                                        //
    "collection_list(arena) answers the identifiers of every collection.\n"               //
    "\n"                                                                                  //
    "Lists the collections at the snapshot of a snapshot, and at the head otherwise.\n"   //
    "The main collection is never listed, and collection_names answers the names.\n"      //
    "\n"                                                                                  //
    "Args:\n"                                                                             //
    "  arena (Arena): Where the listing lands before it is copied out, rewound by this\n" //
    "    call.\n"                                                                         //
    "Returns:\n"                                                                          //
    "  tuple[UUID, ...]: One identifier per collection.";

/** A listing of every collection, whose identifiers land in the arena its header holds. */
typedef struct list_call_t {

    /** What the call keeps until it finishes. */
    ustore_py_call_t call;

    /** Output slot for their identifiers, inside the arena. */
    ustore_collection_id_t const* collections;

    /** Collections the listing found. */
    ustore_size_t written;
} list_call_t;

static void list_submit(void* arguments) { ustore_collection_list((ustore_collection_list_t*)arguments); }

// Every identifier is a `uuid.UUID` of its own, so nothing of the arena outlives the answer.
static PyObject* list_answer(void* erased) {
    list_call_t const* const call = (list_call_t const*)erased;
    PyObject* const listed = PyTuple_New((Py_ssize_t)call->written);
    for (ustore_size_t entry_index = 0; listed && entry_index != call->written; ++entry_index) {
        PyObject* const identifier = ustore_py_uuid_to_object(&call->collections[entry_index].uuid);
        if (!identifier) {
            Py_DECREF(listed);
            return NULL;
        }
        PyTuple_SET_ITEM(listed, (Py_ssize_t)entry_index, identifier);
    }
    return listed;
}

static void list_release(void* call) { ustore_py_call_release(&((list_call_t*)call)->call); }

static ustore_py_verb_t const list_verb = {sizeof(list_call_t), list_submit, list_answer, list_release};

static PyObject* Database_collection_list(PyObject* self, PyObject* arena) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    list_call_t stack;
    list_call_t* const call = ustore_py_call_open(&context, &list_verb, &stack);
    if (!call) return NULL;
    if (ustore_py_arena_claim(arena, &call->call.arena) != 0) return ustore_py_call_discard(&context, &list_verb, call);
    ustore_collection_list_t list = {
        .database = context.database_handle,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->call.outcome,
        .completion = context.completion,
        .snapshot = context.snapshot,
        .collections = &call->collections,
        .written = &call->written,
        .arena = ustore_py_arena_handle(call->call.arena),
    };
    return ustore_py_call_run(&context, &list_verb, call, &list);
}

static char const doc_Database_collection_names[] =                                            //
    "collection_names(arena, *, after=None, limit=1024) answers one page of recorded names.\n" //
    "\n"                                                                                       //
    "Names come in the order of their text, and the walk paginates rather than holding an\n"   //
    "iterator over concurrent state, so a listing longer than one page resumes from the\n"     //
    "last name it saw.\n"                                                                      //
    "\n"                                                                                       //
    "Args:\n"                                                                                  //
    "  arena (Arena): Where the listing lands before it is copied out, rewound by this\n"      //
    "    call.\n"                                                                              //
    "  after (str, optional): The smallest name the page may carry; None starts at the\n"      //
    "    first one. The name given is itself included, so a resuming caller drops it.\n"       //
    "  limit (int, optional): Most names the page carries.\n"                                  //
    "Returns:\n"                                                                               //
    "  tuple[str, ...]: The names recorded for collections, in order.";

static PyObject* Database_collection_names(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                           PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;

    Py_ssize_t const args_names_count = ustore_py_args_names_count_checked("collection_names", positional_args_count,
                                                                           args_names_tuple, 1);
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
    return ustore_py_collection_names(&context, after, (size_t)after_length, limit, args[0]);
}

static char const doc_Database_collection_drop[] =                                           //
    "collection_drop(collection, mode=DropMode.COLLECTION) answers None.\n"                  //
    "\n"                                                                                     //
    "Removes a collection, or empties it. Removing it forgets its name with it, so the\n"    //
    "name can be given to another collection afterwards.\n"                                  //
    "\n"                                                                                     //
    "Args:\n"                                                                                //
    "  collection (UUID | str): The collection, by identifier or by name.\n"                 //
    "  mode (DropMode, optional): `DropMode.COLLECTION` removes the handle and everything\n" //
    "    in it, `DropMode.PAIRS` removes every key and value, and `DropMode.VALUES` every\n" //
    "    value. The main collection accepts the latter two alone.\n"                         //
    "Raises:\n"                                                                              //
    "  NotFoundError: No collection carries the name.";

static PyObject* Database_collection_drop(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                          PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;

    PyObject* mode_object = NULL;
    if (ustore_py_optional_argument("collection_drop", args, positional_args_count, args_names_tuple, 1, "mode",
                                    &mode_object) != 0)
        return NULL;
    int mode = ustore_drop_collection_k;
    if (mode_object && ustore_py_enum_from_object(mode_object, "mode", ustore_py_enum_drop_mode_k, &mode) != 0)
        return NULL;

    // A name resolves first, and the collection it addresses takes the name with it when it goes
    ustore_collection_id_t collection = ustore_collection_main_k;
    if (ustore_py_collection_from_object(&context, args[0], &collection) != 0) return NULL;
    ustore_outcome_t outcome = {0};
    ustore_collection_drop_t drop = {0};
    drop.database = context.database_handle;
    drop.outcome = &outcome;
    drop.collection = collection;
    drop.mode = (ustore_drop_mode_t)mode;

    Py_BEGIN_ALLOW_THREADS;
    ustore_collection_drop(&drop);
    Py_END_ALLOW_THREADS;
    if (ustore_py_raise_on_outcome(&outcome) != 0) return NULL;
    Py_RETURN_NONE;
}

#pragma endregion Collections

#pragma region Snapshots

static char const doc_Snapshot[] =                                                            //
    "A point-in-time state of the whole database, whose read verbs see what it pinned.\n"     //
    "\n"                                                                                      //
    "Built through `Database.snapshot()`, never by name. It reads as a database does, with\n" //
    "`read`, `get`, `scan`, `sample`, `measure` and the other reads, and `snapshot[name]`\n"  //
    "answers a Collection resolved at the snapshot. It pins the versions those reads need\n"  //
    "until it closes, lives no longer than the process, and a database waits for every\n"     //
    "snapshot it opened before it closes.\n"                                                  //
    "\n"                                                                                      //
    "Used as a context manager, it closes on exit.";

static PyObject* Snapshot_tp_new(PyTypeObject* Py_UNUSED(type), PyObject* Py_UNUSED(args),
                                 PyObject* Py_UNUSED(keywords)) {
    PyErr_SetString(PyExc_TypeError, "A snapshot is built through Database.snapshot()");
    return NULL;
}

/** Releases the snapshot's handle once the deferred calls naming it finish, with the hold it kept on its database. */
static void snapshot_close(Snapshot* snapshot) {
    ustore_snapshot_t const handle = snapshot->handle;
    if (!handle) return;
    snapshot->handle = NULL;

    Py_BEGIN_ALLOW_THREADS;
    ustore_snapshot_free(handle);
    Py_END_ALLOW_THREADS;
}

static void Snapshot_dealloc(Snapshot* self) {
    snapshot_close(self);
    Py_XDECREF(self->database);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static char const doc_Database_snapshot[] =                                           //
    "snapshot() answers a new Snapshot.\n"                                            //
    "\n"                                                                              //
    "Pins a point-in-time state of the whole database, which the read verbs of the\n" //
    "snapshot then see.\n"                                                            //
    "\n"                                                                              //
    "Returns:\n"                                                                      //
    "  Snapshot: A view of this database as it stands now.\n"                         //
    "Raises:\n"                                                                       //
    "  UnsupportedError: The engine keeps no snapshots.";

static PyObject* Database_snapshot(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    Database* const database = (Database*)self;
    if (!database->handle) {
        ustore_py_raise(ustore_status_uninitialized_k, "The database is closed");
        return NULL;
    }

    ustore_snapshot_t handle = NULL;
    ustore_stamp_t read_stamp = 0;
    ustore_outcome_t outcome = {0};
    ustore_snapshot_init_t init = {0};
    init.snapshot = &handle;
    init.database = database->handle;
    init.outcome = &outcome;
    init.read_stamp = &read_stamp;

    Py_BEGIN_ALLOW_THREADS;
    ustore_snapshot_init(&init);
    Py_END_ALLOW_THREADS;
    if (ustore_py_raise_on_outcome(&outcome) != 0) return NULL;

    Snapshot* const instance = (Snapshot*)SnapshotType.tp_alloc(&SnapshotType, 0);
    if (!instance) {
        ustore_snapshot_free(handle);
        return NULL;
    }
    instance->handle = handle;
    instance->database = Py_NewRef(self);
    instance->database_handle = database->handle;
    instance->read_stamp = read_stamp;
    return (PyObject*)instance;
}

static char const doc_Snapshot_save[] =                                                        //
    "save(path, *, progress=None) answers None.\n"                                             //
    "\n"                                                                                       //
    "Writes the snapshot out as a new database of the same engine, holding exactly the\n"      //
    "collections and names the snapshot records, which that engine's open then serves.\n"      //
    "RocksDB and LevelDB create the directory, while the RAM engine writes its image into\n"   //
    "the directory the path names. Calls `ustore_snapshot_save`.\n"                            //
    "\n"                                                                                       //
    "Args:\n"                                                                                  //
    "  path (str | bytes | PathLike): The directory the saved database lands in.\n"            //
    "  progress (Callable[[int, int], bool | None], optional): Called with the bytes of the\n" //
    "    keys and values saved and the bytes in all. A falsy answer other than None stops\n"   //
    "    the save, which removes what it wrote.\n"                                             //
    "Raises:\n"                                                                                //
    "  CancelledError: The progress callable stopped the save.\n"                              //
    "  UnsupportedError: The engine saves no snapshots.";

/** A save of a snapshot as a new database. */
typedef struct save_call_t {

    /** What the call keeps until it finishes. */
    ustore_py_call_t call;

    /** The directory the database lands in, encoded as bytes and held. */
    PyObject* path;
} save_call_t;

static void save_submit(void* arguments) { ustore_snapshot_save((ustore_snapshot_save_t*)arguments); }

static void save_release(void* erased) {
    save_call_t* const call = (save_call_t*)erased;
    Py_CLEAR(call->path);
    ustore_py_call_release(&call->call);
}

static ustore_py_verb_t const save_verb = {sizeof(save_call_t), save_submit, ustore_py_call_answer_none, save_release};

static PyObject* Snapshot_save(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                               PyObject* args_names_tuple) {
    PyObject* progress = NULL;
    if (progress_keyword("save", args, positional_args_count, args_names_tuple, 1, &progress) != 0) return NULL;
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (!context.snapshot) {
        PyErr_SetString(PyExc_TypeError, "save() needs a view of a snapshot");
        return NULL;
    }
    save_call_t stack;
    save_call_t* const call = ustore_py_call_open(&context, &save_verb, &stack);
    if (!call) return NULL;
    if (!PyUnicode_FSConverter(args[0], &call->path)) return ustore_py_call_discard(&context, &save_verb, call);
    call->call.progress = Py_XNewRef(progress);
    ustore_snapshot_save_t save = {
        .snapshot = context.snapshot,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->call.outcome,
        .completion = context.completion,
        .path = path_bytes(call->path),
        .progress = ustore_py_progress_callback(&call->call),
        .progress_state = &call->call,
    };
    return ustore_py_call_run(&context, &save_verb, call, &save);
}

static char const doc_Snapshot_close[] =                                                      //
    "close() answers None.\n"                                                                 //
    "\n"                                                                                      //
    "Releases the snapshot once every deferred call on it finishes, unpinning the versions\n" //
    "only it kept, and lets its database close. Closing twice is safe.";

static PyObject* Snapshot_close(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    snapshot_close((Snapshot*)self);
    Py_RETURN_NONE;
}

static PyObject* Snapshot_enter(PyObject* self, PyObject* Py_UNUSED(ignored)) { return Py_NewRef(self); }

static PyObject* Snapshot_exit(PyObject* self, PyObject* const* Py_UNUSED(args),
                               Py_ssize_t Py_UNUSED(positional_args_count), PyObject* Py_UNUSED(args_names_tuple)) {
    snapshot_close((Snapshot*)self);
    Py_RETURN_NONE;
}

static PyObject* Snapshot_repr(Snapshot* self) {
    if (!self->handle) return PyUnicode_FromString("Snapshot(<closed>)");
    return PyUnicode_FromFormat("Snapshot(at=%p)", (void*)self->handle);
}

static char const doc_Snapshot_database[] = "The database this snapshot reads.";

static PyObject* Snapshot_get_database(Snapshot* self, void* Py_UNUSED(closure)) { return Py_NewRef(self->database); }

static char const doc_Snapshot_read_stamp[] =                                                  //
    "The stamp the snapshot reads at, as an int: it sees every commit whose commit stamp is\n" //
    "at or below it, and none above.";

static PyObject* Snapshot_get_read_stamp(Snapshot* self, void* Py_UNUSED(closure)) {
    return PyLong_FromUnsignedLongLong(self->read_stamp);
}

#pragma endregion Snapshots

#pragma region Transactions

static char const doc_Transaction[] =                                                         //
    "An optimistic transaction grouping operations across collections and modalities.\n"      //
    "\n"                                                                                      //
    "Built through `Database.transaction()`, never by name. A watching transaction records\n" //
    "what it reads and writes and refuses to commit when a concurrent writer touched any\n"   //
    "of it, so a caller retries rather than locks. It serves one call at a time, and a\n"     //
    "second call while one is unfinished raises ContendedError.\n"                            //
    "\n"                                                                                      //
    "`unwatched()` answers a view of it whose calls record nothing, so a read a retry must\n" //
    "not widen its conflict set with goes through `txn.unwatched()`, and `watched()` does\n"  //
    "the opposite for a transaction begun with `Watch.NONE`.\n"                               //
    "\n"                                                                                      //
    "Used as a context manager, it commits on a clean exit, drops the staged work on an\n"    //
    "exception, and closes either way.\n"                                                     //
    "\n"                                                                                      //
    "Indexing it by name, as `txn['payments']`, answers a Collection whose writes stage\n"    //
    "here: its reads resolve the name through this transaction, while its writes resolve\n"   //
    "it at the head, where a missing collection is created.";

static PyObject* Transaction_tp_new(PyTypeObject* Py_UNUSED(type), PyObject* Py_UNUSED(args),
                                    PyObject* Py_UNUSED(keywords)) {
    PyErr_SetString(PyExc_TypeError, "A transaction is built through Database.transaction()");
    return NULL;
}

/** Releases the transaction's handle once the deferred calls naming it finish, with the hold it kept on its
 *  database. */
static void transaction_close(Transaction* transaction) {
    ustore_transaction_t const handle = transaction->handle;
    if (!handle) return;
    transaction->handle = NULL;

    Py_BEGIN_ALLOW_THREADS;
    ustore_transaction_free(handle);
    Py_END_ALLOW_THREADS;
}

static void Transaction_dealloc(Transaction* self) {
    transaction_close(self);
    Py_XDECREF(self->database);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static char const doc_Database_transaction[] =                                              //
    "transaction(watch=Watch.KEYS) answers a new Transaction.\n"                            //
    "\n"                                                                                    //
    "Args:\n"                                                                               //
    "  watch (Watch, optional): Whether the transaction records the keys it reads and\n"    //
    "    writes, and so whether a concurrent writer can make it fail to commit. One that\n" //
    "    watches `Watch.NONE` always commits, and sees whatever the head holds.\n"          //
    "Returns:\n"                                                                            //
    "  Transaction: A transaction over this database.\n"                                    //
    "Raises:\n"                                                                             //
    "  UnsupportedError: The engine has no transactions.";

static PyObject* Database_transaction(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                      PyObject* args_names_tuple) {
    Database* const database = (Database*)self;
    if (!database->handle) {
        ustore_py_raise(ustore_status_uninitialized_k, "The database is closed");
        return NULL;
    }

    PyObject* watch_object = NULL;
    if (ustore_py_optional_argument("transaction", args, positional_args_count, args_names_tuple, 0, "watch",
                                    &watch_object) != 0)
        return NULL;
    int watch = ustore_watch_keys_k;
    if (watch_object && ustore_py_enum_from_object(watch_object, "watch", ustore_py_enum_watch_k, &watch) != 0)
        return NULL;

    ustore_transaction_t handle = NULL;
    ustore_outcome_t outcome = {0};
    ustore_transaction_init_t begin = {0};
    begin.transaction = &handle;
    begin.database = database->handle;
    begin.outcome = &outcome;

    Py_BEGIN_ALLOW_THREADS;
    ustore_transaction_init(&begin);
    Py_END_ALLOW_THREADS;
    if (ustore_py_raise_on_outcome(&outcome) != 0) return NULL;

    Transaction* const instance = (Transaction*)TransactionType.tp_alloc(&TransactionType, 0);
    if (!instance) {
        ustore_transaction_free(handle);
        return NULL;
    }
    instance->handle = handle;
    instance->database = Py_NewRef(self);
    instance->database_handle = database->handle;
    instance->watch = (ustore_watch_t)watch;
    return (PyObject*)instance;
}

/** Fills @p context for a verb of a transaction, refusing a receiver that addresses none. */
static int transaction_context(PyObject* self, char const* verb, ustore_py_context_t* context) {
    if (ustore_py_context_from_object(self, context) != 0) return -1;
    if (context->transaction) return 0;
    PyErr_Format(PyExc_TypeError, "%s() needs a view of a transaction", verb);
    return -1;
}

/** A commit or a stage, which answers its commit stamp or its read stamp. */
typedef struct stamped_call_t {

    /** What the call keeps until it finishes. */
    ustore_py_call_t call;

    /** The commit stamp of a commit, or the read stamp of a stage. */
    ustore_stamp_t stamp;
} stamped_call_t;

static void commit_submit(void* arguments) { ustore_transaction_commit((ustore_transaction_commit_t*)arguments); }

static void stage_submit(void* arguments) { ustore_transaction_stage((ustore_transaction_stage_t*)arguments); }

static PyObject* stamped_answer(void* call) { return PyLong_FromUnsignedLongLong(((stamped_call_t*)call)->stamp); }

static void stamped_release(void* call) { ustore_py_call_release(&((stamped_call_t*)call)->call); }

static ustore_py_verb_t const commit_verb = {sizeof(stamped_call_t), commit_submit, stamped_answer, stamped_release};

static ustore_py_verb_t const stage_verb = {sizeof(stamped_call_t), stage_submit, stamped_answer, stamped_release};

static char const doc_Transaction_commit[] =                                                  //
    "commit(durability=Durability.BUFFERED) answers the commit stamp.\n"                      //
    "\n"                                                                                      //
    "Applies every staged change, or none of them. The contents are kept whatever the\n"      //
    "outcome, so a rejected transaction can be inspected or replayed.\n"                      //
    "\n"                                                                                      //
    "Args:\n"                                                                                 //
    "  durability (Durability, optional): `Durability.FLUSHED` reaches the drive before\n"    //
    "    returning.\n"                                                                        //
    "Returns:\n"                                                                              //
    "  int: The commit stamp, this commit's position in commit order, which every snapshot\n" //
    "  whose read stamp is at or above it sees.\n"                                            //
    "Raises:\n"                                                                               //
    "  ConflictError: A concurrent writer touched what this transaction watched.";

static PyObject* Transaction_commit(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                    PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (transaction_context(self, "commit", &context) != 0) return NULL;

    PyObject* durability_object = NULL;
    if (ustore_py_optional_argument("commit", args, positional_args_count, args_names_tuple, 0, "durability",
                                    &durability_object) != 0)
        return NULL;
    int durability = ustore_durability_buffered_k;
    if (durability_object &&
        ustore_py_enum_from_object(durability_object, "durability", ustore_py_enum_durability_k, &durability) != 0)
        return NULL;

    stamped_call_t stack;
    stamped_call_t* const call = ustore_py_call_open(&context, &commit_verb, &stack);
    if (!call) return NULL;
    ustore_transaction_commit_t commit = {
        .database = context.database_handle,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->call.outcome,
        .completion = context.completion,
        .transaction = context.transaction,
        .commit_stamp = &call->stamp,
        .durability = (ustore_durability_t)durability,
    };
    return ustore_py_call_run(&context, &commit_verb, call, &commit);
}

static char const doc_Transaction_stage[] =                                                //
    "stage() answers the read stamp.\n"                                                    //
    "\n"                                                                                   //
    "Runs the first phase of a two-phase commit, after which a commit publishes the\n"     //
    "work. Staging reserves no position in commit order.\n"                                //
    "\n"                                                                                   //
    "Returns:\n"                                                                           //
    "  int: The read stamp, the stamp the transaction reads at and its commit validates\n" //
    "  against, which is no greater than the commit stamp the commit then answers.";

static PyObject* Transaction_stage(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    ustore_py_context_t context;
    if (transaction_context(self, "stage", &context) != 0) return NULL;

    stamped_call_t stack;
    stamped_call_t* const call = ustore_py_call_open(&context, &stage_verb, &stack);
    if (!call) return NULL;
    ustore_transaction_stage_t stage = {
        .database = context.database_handle,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->call.outcome,
        .completion = context.completion,
        .transaction = context.transaction,
        .read_stamp = &call->stamp,
    };
    return ustore_py_call_run(&context, &stage_verb, call, &stage);
}

static char const doc_Transaction_rollback[] =                                        //
    "rollback() answers None.\n"                                                      //
    "\n"                                                                              //
    "Withdraws a stage, releasing whatever it claimed, while keeping the reads and\n" //
    "writes, so the transaction can stage or commit again. `reset` drops the work itself.";

static PyObject* Transaction_rollback(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;

    ustore_outcome_t outcome = {0};
    ustore_transaction_rollback_t rollback = {0};
    rollback.database = context.database_handle;
    rollback.outcome = &outcome;
    rollback.transaction = context.transaction;

    Py_BEGIN_ALLOW_THREADS;
    ustore_transaction_rollback(&rollback);
    Py_END_ALLOW_THREADS;
    if (ustore_py_raise_on_outcome(&outcome) != 0) return NULL;
    Py_RETURN_NONE;
}

static char const doc_Transaction_reset[] =                                               //
    "reset() answers None.\n"                                                             //
    "\n"                                                                                  //
    "Drops every read and write and starts a new generation on the same handle, so the\n" //
    "transaction is ready to be used again without allocating another.";

static PyObject* Transaction_reset(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;

    ustore_outcome_t outcome = {0};
    ustore_transaction_init_t reset = {0};
    reset.transaction = &((Transaction*)self)->handle;
    reset.database = context.database_handle;
    reset.outcome = &outcome;

    Py_BEGIN_ALLOW_THREADS;
    ustore_transaction_init(&reset);
    Py_END_ALLOW_THREADS;
    if (ustore_py_raise_on_outcome(&outcome) != 0) return NULL;
    Py_RETURN_NONE;
}

static char const doc_Transaction_close[] =                                                   //
    "close() answers None.\n"                                                                 //
    "\n"                                                                                      //
    "Drops whatever is still staged and releases the handle once every deferred call on it\n" //
    "finishes, which a database waits on before it closes. Closing twice is safe.";

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
    PyObject* const committed = raised || !((Transaction*)self)->handle ? Py_NewRef(Py_None)
                                                                        : Transaction_commit(self, NULL, 0, NULL);
    transaction_close((Transaction*)self);
    if (!committed) return NULL;
    Py_DECREF(committed);
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

#pragma region Views

static char const doc_View[] =                                                                     //
    "What a Database, Transaction or Snapshot addresses, with the executor that runs its calls.\n" //
    "\n"                                                                                           //
    "Built through `on(executor)`, `watched()` or `unwatched()`, never by name. Its verbs and\n"   //
    "`view[name]` answer what the origin's do, each blocking until it finishes: on the\n"          //
    "executor, after every call submitted to it before, or on the calling thread without one.\n"   //
    "A view never changes after it is made, so threads share it freely.";

static char const doc_DeferredView[] =                                                          //
    "What a Database, Transaction or Snapshot addresses, with the executor its calls go to.\n"  //
    "\n"                                                                                        //
    "Built through `deferred(executor)`, never by name. Its verbs submit the call and answer\n" //
    "a Deferred at once, which answers what the blocking verb would have answered once the\n"   //
    "call finishes; arguments it refuses raise at the call, and the engine's refusals raise\n"  //
    "at `wait()` or `await`. `view[name].blobs[key]` and `view[name].docs[key]` answer a\n"     //
    "Deferred too, as do the reads of `view[name].graph` that take one call, while an\n"        //
    "assignment, `del`, `in` and `.table` raise TypeError naming the flat verb to call\n"       //
    "instead. Every pull of a cursor blocks, so it opens none. A view never changes after it\n" //
    "is made, so threads share it freely.";

static PyObject* View_tp_new(PyTypeObject* Py_UNUSED(type), PyObject* Py_UNUSED(args), PyObject* Py_UNUSED(keywords)) {
    PyErr_SetString(PyExc_TypeError, "A view is built through on(), deferred(), watched() or unwatched()");
    return NULL;
}

static void View_dealloc(View* self) {
    Py_XDECREF(self->origin);
    Py_XDECREF((PyObject*)self->executor);
    PyObject_Free(self);
}

static PyObject* View_repr(View* self) {
    return PyUnicode_FromFormat("%s(%R)", Py_TYPE(self)->tp_name + sizeof("ustore.") - 1, self->origin);
}

static char const doc_View_database[] = "The database the view's calls run against.";

static PyObject* View_get_database(View* self, void* Py_UNUSED(closure)) {
    if (PyObject_TypeCheck(self->origin, &TransactionType)) return Py_NewRef(((Transaction*)self->origin)->database);
    if (PyObject_TypeCheck(self->origin, &SnapshotType)) return Py_NewRef(((Snapshot*)self->origin)->database);
    return Py_NewRef(self->origin);
}

static char const doc_View_executor[] =
    "The executor the view's calls run on, where an arena for them belongs, or None for the calling thread.";

static PyObject* View_get_executor(View* self, void* Py_UNUSED(closure)) {
    return self->executor ? Py_NewRef((PyObject*)self->executor) : Py_NewRef(Py_None);
}

/** Answers a new receiver of @p type over @p origin, run by @p executor and recording under @p watch. */
static PyObject* view_new(PyTypeObject* type, PyObject* origin, Executor* executor, ustore_watch_t watch) {
    View* const view = PyObject_New(View, type);
    if (!view) return NULL;
    view->origin = Py_NewRef(origin);
    view->executor = (Executor*)Py_XNewRef((PyObject*)executor);
    view->watch = watch;
    return (PyObject*)view;
}

/** What @p self derives receivers from: a view's own origin, executor and watch, or an origin with neither. */
static void view_parts(PyObject* self, PyObject** origin, Executor** executor, ustore_watch_t* watch) {
    if (PyObject_TypeCheck(self, &ViewType) || PyObject_TypeCheck(self, &DeferredViewType)) {
        View const* const view = (View const*)self;
        *origin = view->origin;
        *executor = view->executor;
        *watch = view->watch;
        return;
    }
    *origin = self;
    *executor = NULL;
    *watch = PyObject_TypeCheck(self, &TransactionType) ? ((Transaction*)self)->watch : ustore_watch_keys_k;
}

/** Answers a view of @p type over what @p self addresses, run by @p executor_object, which must be an executor. */
static PyObject* view_on(PyObject* self, PyObject* executor_object, PyTypeObject* type) {
    if (!PyObject_TypeCheck(executor_object, &ExecutorType)) {
        PyErr_Format(PyExc_TypeError, "Expected an Executor, got %S", Py_TYPE(executor_object));
        return NULL;
    }
    PyObject* origin = NULL;
    Executor* executor = NULL;
    ustore_watch_t watch = ustore_watch_keys_k;
    view_parts(self, &origin, &executor, &watch);
    return view_new(type, origin, (Executor*)executor_object, watch);
}

static char const doc_on[] =                                                                   //
    "on(executor) answers a View whose calls block on the executor.\n"                         //
    "\n"                                                                                       //
    "Every call runs after the ones submitted to the executor before it finishes, so a read\n" //
    "through it sees what earlier deferred writes wrote.\n"                                    //
    "\n"                                                                                       //
    "Args:\n"                                                                                  //
    "  executor (Executor): The threads that run the calls.";

static PyObject* receiver_on(PyObject* self, PyObject* executor) { return view_on(self, executor, &ViewType); }

static char const doc_deferred[] =                                                         //
    "deferred(executor) answers a DeferredView whose calls the executor runs meanwhile.\n" //
    "\n"                                                                                   //
    "Every verb of it submits its call and answers a Deferred at once.\n"                  //
    "\n"                                                                                   //
    "Args:\n"                                                                              //
    "  executor (Executor): The threads that run the calls.";

static PyObject* receiver_deferred(PyObject* self, PyObject* executor) {
    return view_on(self, executor, &DeferredViewType);
}

/** Answers a view of the transaction @p self addresses, of the same completion, recording under @p watch. */
static PyObject* view_watching(PyObject* self, ustore_watch_t watch) {
    PyObject* origin = NULL;
    Executor* executor = NULL;
    ustore_watch_t current = ustore_watch_keys_k;
    view_parts(self, &origin, &executor, &current);
    if (!PyObject_TypeCheck(origin, &TransactionType)) {
        PyErr_SetString(PyExc_TypeError, "Only a transaction watches what it touches");
        return NULL;
    }
    PyTypeObject* const type = PyObject_TypeCheck(self, &DeferredViewType) ? &DeferredViewType : &ViewType;
    return view_new(type, origin, executor, watch);
}

static char const doc_watched[] =
    "watched() answers a view of the transaction whose calls record the keys they touch for conflicts.";

static PyObject* receiver_watched(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    return view_watching(self, ustore_watch_keys_k);
}

static char const doc_unwatched[] =
    "unwatched() answers a view of the transaction whose calls record nothing, so they never fail its commit.";

static PyObject* receiver_unwatched(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    return view_watching(self, ustore_watch_none_k);
}

#pragma endregion Views

#pragma region Tables

static PyGetSetDef Database_getset[] = {
    {"capabilities", (getter)Database_get_capabilities, NULL, doc_Database_capabilities, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyMethodDef Database_methods[] = {
    {"open_ram", (PyCFunction)(void (*)(void))Database_open_ram, ustore_py_method_flags_k | METH_CLASS,
     doc_Database_open_ram},
    {"open_rocksdb", (PyCFunction)(void (*)(void))Database_open_rocksdb, ustore_py_method_flags_k | METH_CLASS,
     doc_Database_open_rocksdb},
    {"view_rocksdb", (PyCFunction)(void (*)(void))Database_view_rocksdb, ustore_py_method_flags_k | METH_CLASS,
     doc_Database_view_rocksdb},
    {"open_leveldb", (PyCFunction)(void (*)(void))Database_open_leveldb, ustore_py_method_flags_k | METH_CLASS,
     doc_Database_open_leveldb},
    {"open_nvme", (PyCFunction)(void (*)(void))Database_open_nvme, ustore_py_method_flags_k | METH_CLASS,
     doc_Database_open_nvme},
    {"close", (PyCFunction)(void (*)(void))Database_close, METH_NOARGS, doc_Database_close},
    {"__enter__", (PyCFunction)(void (*)(void))Database_enter, METH_NOARGS, NULL},
    {"__exit__", (PyCFunction)(void (*)(void))Database_exit, ustore_py_method_flags_k, NULL},
    {"on", (PyCFunction)(void (*)(void))receiver_on, METH_O, doc_on},
    {"deferred", (PyCFunction)(void (*)(void))receiver_deferred, METH_O, doc_deferred},
    {"clear", (PyCFunction)(void (*)(void))Database_clear, METH_O, doc_Database_clear},
    {"compact", (PyCFunction)(void (*)(void))Database_compact, ustore_py_method_flags_k, doc_Database_compact},
    {"describe", (PyCFunction)(void (*)(void))Database_describe, METH_NOARGS, doc_Database_describe},
    {"cursor", (PyCFunction)(void (*)(void))ustore_py_api_cursor, ustore_py_method_flags_k, doc_cursor},

    {"collection_create", (PyCFunction)(void (*)(void))Database_collection_create, ustore_py_method_flags_k,
     doc_Database_collection_create},
    {"collection_find", (PyCFunction)(void (*)(void))Database_collection_find, METH_FASTCALL,
     doc_Database_collection_find},
    {"collection_contains", (PyCFunction)(void (*)(void))Database_collection_contains, METH_O,
     doc_Database_collection_contains},
    {"collection_list", (PyCFunction)(void (*)(void))Database_collection_list, METH_O, doc_Database_collection_list},
    {"collection_names", (PyCFunction)(void (*)(void))Database_collection_names, ustore_py_method_flags_k,
     doc_Database_collection_names},
    {"collection_drop", (PyCFunction)(void (*)(void))Database_collection_drop, ustore_py_method_flags_k,
     doc_Database_collection_drop},

    {"snapshot", (PyCFunction)(void (*)(void))Database_snapshot, METH_NOARGS, doc_Database_snapshot},
    {"transaction", (PyCFunction)(void (*)(void))Database_transaction, ustore_py_method_flags_k,
     doc_Database_transaction},

    {"read", (PyCFunction)(void (*)(void))ustore_py_api_read, ustore_py_method_flags_k, doc_read},
    {"write", (PyCFunction)(void (*)(void))ustore_py_api_write, ustore_py_method_flags_k, doc_write},
    {"scan", (PyCFunction)(void (*)(void))ustore_py_api_scan, ustore_py_method_flags_k, doc_scan},
    {"scan_batch", (PyCFunction)(void (*)(void))ustore_py_api_scan_batch, ustore_py_method_flags_k, doc_scan_batch},
    {"sample", (PyCFunction)(void (*)(void))ustore_py_api_sample, ustore_py_method_flags_k, doc_sample},
    {"sample_batch", (PyCFunction)(void (*)(void))ustore_py_api_sample_batch, ustore_py_method_flags_k,
     doc_sample_batch},
    {"measure", (PyCFunction)(void (*)(void))ustore_py_api_measure, ustore_py_method_flags_k, doc_measure},
    {"get", (PyCFunction)(void (*)(void))ustore_py_api_get, ustore_py_method_flags_k, doc_get},
    {"set", (PyCFunction)(void (*)(void))ustore_py_api_set, ustore_py_method_flags_k, doc_set},
    {"remove", (PyCFunction)(void (*)(void))ustore_py_api_remove, ustore_py_method_flags_k, doc_remove},
    {"contains", (PyCFunction)(void (*)(void))ustore_py_api_contains, ustore_py_method_flags_k, doc_contains},

    {"docs_write", (PyCFunction)(void (*)(void))ustore_py_api_docs_write, ustore_py_method_flags_k, doc_docs_write},
    {"docs_read", (PyCFunction)(void (*)(void))ustore_py_api_docs_read, ustore_py_method_flags_k, doc_docs_read},
    {"docs_gist", (PyCFunction)(void (*)(void))ustore_py_api_docs_gist, ustore_py_method_flags_k, doc_docs_gist},
    {"docs_gather", (PyCFunction)(void (*)(void))ustore_py_api_docs_gather, ustore_py_method_flags_k, doc_docs_gather},
    {"graph_write_edges", (PyCFunction)(void (*)(void))ustore_py_api_graph_write_edges, ustore_py_method_flags_k,
     doc_graph_write_edges},
    {"graph_remove_edges", (PyCFunction)(void (*)(void))ustore_py_api_graph_remove_edges, ustore_py_method_flags_k,
     doc_graph_remove_edges},
    {"graph_read_edges", (PyCFunction)(void (*)(void))ustore_py_api_graph_read_edges, ustore_py_method_flags_k,
     doc_graph_read_edges},
    {"graph_degrees", (PyCFunction)(void (*)(void))ustore_py_api_graph_degrees, ustore_py_method_flags_k,
     doc_graph_degrees},
    {"graph_write_vertices", (PyCFunction)(void (*)(void))ustore_py_api_graph_write_vertices, ustore_py_method_flags_k,
     doc_graph_write_vertices},
    {"graph_remove_vertices", (PyCFunction)(void (*)(void))ustore_py_api_graph_remove_vertices,
     ustore_py_method_flags_k, doc_graph_remove_vertices},
    {NULL, NULL, 0, NULL},
};

/** Indexing any receiver by name answers the collection, as `db['movies']` does. */
static PyMappingMethods receiver_as_mapping = {
    .mp_subscript = (binaryfunc)ustore_py_collection_subscript,
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
    .tp_as_mapping = &receiver_as_mapping,
    .tp_methods = Database_methods,
    .tp_getset = Database_getset,
};

static PyGetSetDef Transaction_getset[] = {
    {"database", (getter)Transaction_get_database, NULL, doc_Transaction_database, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyMethodDef Transaction_methods[] = {
    {"commit", (PyCFunction)(void (*)(void))Transaction_commit, ustore_py_method_flags_k, doc_Transaction_commit},
    {"stage", (PyCFunction)(void (*)(void))Transaction_stage, METH_NOARGS, doc_Transaction_stage},
    {"rollback", (PyCFunction)(void (*)(void))Transaction_rollback, METH_NOARGS, doc_Transaction_rollback},
    {"reset", (PyCFunction)(void (*)(void))Transaction_reset, METH_NOARGS, doc_Transaction_reset},
    {"close", (PyCFunction)(void (*)(void))Transaction_close, METH_NOARGS, doc_Transaction_close},
    {"__enter__", (PyCFunction)(void (*)(void))Transaction_enter, METH_NOARGS, NULL},
    {"__exit__", (PyCFunction)(void (*)(void))Transaction_exit, ustore_py_method_flags_k, NULL},
    {"on", (PyCFunction)(void (*)(void))receiver_on, METH_O, doc_on},
    {"deferred", (PyCFunction)(void (*)(void))receiver_deferred, METH_O, doc_deferred},
    {"watched", (PyCFunction)(void (*)(void))receiver_watched, METH_NOARGS, doc_watched},
    {"unwatched", (PyCFunction)(void (*)(void))receiver_unwatched, METH_NOARGS, doc_unwatched},
    {"cursor", (PyCFunction)(void (*)(void))ustore_py_api_cursor, ustore_py_method_flags_k, doc_cursor},

    {"read", (PyCFunction)(void (*)(void))ustore_py_api_read, ustore_py_method_flags_k, doc_read},
    {"write", (PyCFunction)(void (*)(void))ustore_py_api_write, ustore_py_method_flags_k, doc_write},
    {"scan", (PyCFunction)(void (*)(void))ustore_py_api_scan, ustore_py_method_flags_k, doc_scan},
    {"scan_batch", (PyCFunction)(void (*)(void))ustore_py_api_scan_batch, ustore_py_method_flags_k, doc_scan_batch},
    {"sample", (PyCFunction)(void (*)(void))ustore_py_api_sample, ustore_py_method_flags_k, doc_sample},
    {"sample_batch", (PyCFunction)(void (*)(void))ustore_py_api_sample_batch, ustore_py_method_flags_k,
     doc_sample_batch},
    {"measure", (PyCFunction)(void (*)(void))ustore_py_api_measure, ustore_py_method_flags_k, doc_measure},
    {"get", (PyCFunction)(void (*)(void))ustore_py_api_get, ustore_py_method_flags_k, doc_get},
    {"set", (PyCFunction)(void (*)(void))ustore_py_api_set, ustore_py_method_flags_k, doc_set},
    {"remove", (PyCFunction)(void (*)(void))ustore_py_api_remove, ustore_py_method_flags_k, doc_remove},
    {"contains", (PyCFunction)(void (*)(void))ustore_py_api_contains, ustore_py_method_flags_k, doc_contains},

    {"docs_write", (PyCFunction)(void (*)(void))ustore_py_api_docs_write, ustore_py_method_flags_k, doc_docs_write},
    {"docs_read", (PyCFunction)(void (*)(void))ustore_py_api_docs_read, ustore_py_method_flags_k, doc_docs_read},
    {"docs_gist", (PyCFunction)(void (*)(void))ustore_py_api_docs_gist, ustore_py_method_flags_k, doc_docs_gist},
    {"docs_gather", (PyCFunction)(void (*)(void))ustore_py_api_docs_gather, ustore_py_method_flags_k, doc_docs_gather},
    {"graph_write_edges", (PyCFunction)(void (*)(void))ustore_py_api_graph_write_edges, ustore_py_method_flags_k,
     doc_graph_write_edges},
    {"graph_remove_edges", (PyCFunction)(void (*)(void))ustore_py_api_graph_remove_edges, ustore_py_method_flags_k,
     doc_graph_remove_edges},
    {"graph_read_edges", (PyCFunction)(void (*)(void))ustore_py_api_graph_read_edges, ustore_py_method_flags_k,
     doc_graph_read_edges},
    {"graph_degrees", (PyCFunction)(void (*)(void))ustore_py_api_graph_degrees, ustore_py_method_flags_k,
     doc_graph_degrees},
    {"graph_write_vertices", (PyCFunction)(void (*)(void))ustore_py_api_graph_write_vertices, ustore_py_method_flags_k,
     doc_graph_write_vertices},
    {"graph_remove_vertices", (PyCFunction)(void (*)(void))ustore_py_api_graph_remove_vertices,
     ustore_py_method_flags_k, doc_graph_remove_vertices},
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
    .tp_as_mapping = &receiver_as_mapping,
    .tp_methods = Transaction_methods,
    .tp_getset = Transaction_getset,
};

static PyGetSetDef Snapshot_getset[] = {
    {"database", (getter)Snapshot_get_database, NULL, doc_Snapshot_database, NULL},
    {"read_stamp", (getter)Snapshot_get_read_stamp, NULL, doc_Snapshot_read_stamp, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyMethodDef Snapshot_methods[] = {
    {"save", (PyCFunction)(void (*)(void))Snapshot_save, ustore_py_method_flags_k, doc_Snapshot_save},
    {"cursor", (PyCFunction)(void (*)(void))ustore_py_api_cursor, ustore_py_method_flags_k, doc_cursor},
    {"close", (PyCFunction)(void (*)(void))Snapshot_close, METH_NOARGS, doc_Snapshot_close},
    {"__enter__", (PyCFunction)(void (*)(void))Snapshot_enter, METH_NOARGS, NULL},
    {"__exit__", (PyCFunction)(void (*)(void))Snapshot_exit, ustore_py_method_flags_k, NULL},
    {"on", (PyCFunction)(void (*)(void))receiver_on, METH_O, doc_on},
    {"deferred", (PyCFunction)(void (*)(void))receiver_deferred, METH_O, doc_deferred},
    {"collection_find", (PyCFunction)(void (*)(void))Database_collection_find, METH_FASTCALL,
     doc_Database_collection_find},
    {"collection_list", (PyCFunction)(void (*)(void))Database_collection_list, METH_O, doc_Database_collection_list},

    {"read", (PyCFunction)(void (*)(void))ustore_py_api_read, ustore_py_method_flags_k, doc_read},
    {"scan", (PyCFunction)(void (*)(void))ustore_py_api_scan, ustore_py_method_flags_k, doc_scan},
    {"scan_batch", (PyCFunction)(void (*)(void))ustore_py_api_scan_batch, ustore_py_method_flags_k, doc_scan_batch},
    {"sample", (PyCFunction)(void (*)(void))ustore_py_api_sample, ustore_py_method_flags_k, doc_sample},
    {"sample_batch", (PyCFunction)(void (*)(void))ustore_py_api_sample_batch, ustore_py_method_flags_k,
     doc_sample_batch},
    {"measure", (PyCFunction)(void (*)(void))ustore_py_api_measure, ustore_py_method_flags_k, doc_measure},
    {"get", (PyCFunction)(void (*)(void))ustore_py_api_get, ustore_py_method_flags_k, doc_get},
    {"contains", (PyCFunction)(void (*)(void))ustore_py_api_contains, ustore_py_method_flags_k, doc_contains},

    {"docs_read", (PyCFunction)(void (*)(void))ustore_py_api_docs_read, ustore_py_method_flags_k, doc_docs_read},
    {"docs_gist", (PyCFunction)(void (*)(void))ustore_py_api_docs_gist, ustore_py_method_flags_k, doc_docs_gist},
    {"docs_gather", (PyCFunction)(void (*)(void))ustore_py_api_docs_gather, ustore_py_method_flags_k, doc_docs_gather},
    {"graph_read_edges", (PyCFunction)(void (*)(void))ustore_py_api_graph_read_edges, ustore_py_method_flags_k,
     doc_graph_read_edges},
    {"graph_degrees", (PyCFunction)(void (*)(void))ustore_py_api_graph_degrees, ustore_py_method_flags_k,
     doc_graph_degrees},
    {NULL, NULL, 0, NULL},
};

PyTypeObject SnapshotType = {
    PyVarObject_HEAD_INIT(NULL, 0) //
        .tp_name = "ustore.Snapshot",
    .tp_doc = doc_Snapshot,
    .tp_basicsize = sizeof(Snapshot),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = Snapshot_tp_new,
    .tp_dealloc = (destructor)Snapshot_dealloc,
    .tp_repr = (reprfunc)Snapshot_repr,
    .tp_as_mapping = &receiver_as_mapping,
    .tp_methods = Snapshot_methods,
    .tp_getset = Snapshot_getset,
};

static PyGetSetDef View_getset[] = {
    {"database", (getter)View_get_database, NULL, doc_View_database, NULL},
    {"executor", (getter)View_get_executor, NULL, doc_View_executor, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyMethodDef View_methods[] = {
    {"on", (PyCFunction)(void (*)(void))receiver_on, METH_O, doc_on},
    {"deferred", (PyCFunction)(void (*)(void))receiver_deferred, METH_O, doc_deferred},
    {"watched", (PyCFunction)(void (*)(void))receiver_watched, METH_NOARGS, doc_watched},
    {"unwatched", (PyCFunction)(void (*)(void))receiver_unwatched, METH_NOARGS, doc_unwatched},
    {"commit", (PyCFunction)(void (*)(void))Transaction_commit, ustore_py_method_flags_k, doc_Transaction_commit},
    {"stage", (PyCFunction)(void (*)(void))Transaction_stage, METH_NOARGS, doc_Transaction_stage},
    {"save", (PyCFunction)(void (*)(void))Snapshot_save, ustore_py_method_flags_k, doc_Snapshot_save},
    {"clear", (PyCFunction)(void (*)(void))Database_clear, METH_O, doc_Database_clear},
    {"compact", (PyCFunction)(void (*)(void))Database_compact, ustore_py_method_flags_k, doc_Database_compact},
    {"describe", (PyCFunction)(void (*)(void))Database_describe, METH_NOARGS, doc_Database_describe},
    {"cursor", (PyCFunction)(void (*)(void))ustore_py_api_cursor, ustore_py_method_flags_k, doc_cursor},
    {"collection_find", (PyCFunction)(void (*)(void))Database_collection_find, METH_FASTCALL,
     doc_Database_collection_find},
    {"collection_list", (PyCFunction)(void (*)(void))Database_collection_list, METH_O, doc_Database_collection_list},

    {"read", (PyCFunction)(void (*)(void))ustore_py_api_read, ustore_py_method_flags_k, doc_read},
    {"write", (PyCFunction)(void (*)(void))ustore_py_api_write, ustore_py_method_flags_k, doc_write},
    {"scan", (PyCFunction)(void (*)(void))ustore_py_api_scan, ustore_py_method_flags_k, doc_scan},
    {"scan_batch", (PyCFunction)(void (*)(void))ustore_py_api_scan_batch, ustore_py_method_flags_k, doc_scan_batch},
    {"sample", (PyCFunction)(void (*)(void))ustore_py_api_sample, ustore_py_method_flags_k, doc_sample},
    {"sample_batch", (PyCFunction)(void (*)(void))ustore_py_api_sample_batch, ustore_py_method_flags_k,
     doc_sample_batch},
    {"measure", (PyCFunction)(void (*)(void))ustore_py_api_measure, ustore_py_method_flags_k, doc_measure},
    {"get", (PyCFunction)(void (*)(void))ustore_py_api_get, ustore_py_method_flags_k, doc_get},
    {"set", (PyCFunction)(void (*)(void))ustore_py_api_set, ustore_py_method_flags_k, doc_set},
    {"remove", (PyCFunction)(void (*)(void))ustore_py_api_remove, ustore_py_method_flags_k, doc_remove},
    {"contains", (PyCFunction)(void (*)(void))ustore_py_api_contains, ustore_py_method_flags_k, doc_contains},

    {"docs_write", (PyCFunction)(void (*)(void))ustore_py_api_docs_write, ustore_py_method_flags_k, doc_docs_write},
    {"docs_read", (PyCFunction)(void (*)(void))ustore_py_api_docs_read, ustore_py_method_flags_k, doc_docs_read},
    {"docs_gist", (PyCFunction)(void (*)(void))ustore_py_api_docs_gist, ustore_py_method_flags_k, doc_docs_gist},
    {"docs_gather", (PyCFunction)(void (*)(void))ustore_py_api_docs_gather, ustore_py_method_flags_k, doc_docs_gather},
    {"graph_write_edges", (PyCFunction)(void (*)(void))ustore_py_api_graph_write_edges, ustore_py_method_flags_k,
     doc_graph_write_edges},
    {"graph_remove_edges", (PyCFunction)(void (*)(void))ustore_py_api_graph_remove_edges, ustore_py_method_flags_k,
     doc_graph_remove_edges},
    {"graph_read_edges", (PyCFunction)(void (*)(void))ustore_py_api_graph_read_edges, ustore_py_method_flags_k,
     doc_graph_read_edges},
    {"graph_degrees", (PyCFunction)(void (*)(void))ustore_py_api_graph_degrees, ustore_py_method_flags_k,
     doc_graph_degrees},
    {"graph_write_vertices", (PyCFunction)(void (*)(void))ustore_py_api_graph_write_vertices, ustore_py_method_flags_k,
     doc_graph_write_vertices},
    {"graph_remove_vertices", (PyCFunction)(void (*)(void))ustore_py_api_graph_remove_vertices,
     ustore_py_method_flags_k, doc_graph_remove_vertices},
    {NULL, NULL, 0, NULL},
};

PyTypeObject ViewType = {
    PyVarObject_HEAD_INIT(NULL, 0) //
        .tp_name = "ustore.View",
    .tp_doc = doc_View,
    .tp_basicsize = sizeof(View),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = View_tp_new,
    .tp_dealloc = (destructor)View_dealloc,
    .tp_repr = (reprfunc)View_repr,
    .tp_as_mapping = &receiver_as_mapping,
    .tp_methods = View_methods,
    .tp_getset = View_getset,
};

/** The verbs of a blocking view, each of which submits its call here and answers a Deferred. */
static PyMethodDef DeferredView_methods[] = {
    {"on", (PyCFunction)(void (*)(void))receiver_on, METH_O, doc_on},
    {"deferred", (PyCFunction)(void (*)(void))receiver_deferred, METH_O, doc_deferred},
    {"watched", (PyCFunction)(void (*)(void))receiver_watched, METH_NOARGS, doc_watched},
    {"unwatched", (PyCFunction)(void (*)(void))receiver_unwatched, METH_NOARGS, doc_unwatched},
    {"commit", (PyCFunction)(void (*)(void))Transaction_commit, ustore_py_method_flags_k, doc_Transaction_commit},
    {"stage", (PyCFunction)(void (*)(void))Transaction_stage, METH_NOARGS, doc_Transaction_stage},
    {"save", (PyCFunction)(void (*)(void))Snapshot_save, ustore_py_method_flags_k, doc_Snapshot_save},
    {"clear", (PyCFunction)(void (*)(void))Database_clear, METH_O, doc_Database_clear},
    {"compact", (PyCFunction)(void (*)(void))Database_compact, ustore_py_method_flags_k, doc_Database_compact},
    {"describe", (PyCFunction)(void (*)(void))Database_describe, METH_NOARGS, doc_Database_describe},
    {"collection_find", (PyCFunction)(void (*)(void))Database_collection_find, METH_FASTCALL,
     doc_Database_collection_find},
    {"collection_list", (PyCFunction)(void (*)(void))Database_collection_list, METH_O, doc_Database_collection_list},

    {"read", (PyCFunction)(void (*)(void))ustore_py_api_read, ustore_py_method_flags_k, doc_read},
    {"write", (PyCFunction)(void (*)(void))ustore_py_api_write, ustore_py_method_flags_k, doc_write},
    {"scan", (PyCFunction)(void (*)(void))ustore_py_api_scan, ustore_py_method_flags_k, doc_scan},
    {"scan_batch", (PyCFunction)(void (*)(void))ustore_py_api_scan_batch, ustore_py_method_flags_k, doc_scan_batch},
    {"sample", (PyCFunction)(void (*)(void))ustore_py_api_sample, ustore_py_method_flags_k, doc_sample},
    {"sample_batch", (PyCFunction)(void (*)(void))ustore_py_api_sample_batch, ustore_py_method_flags_k,
     doc_sample_batch},
    {"measure", (PyCFunction)(void (*)(void))ustore_py_api_measure, ustore_py_method_flags_k, doc_measure},
    {"get", (PyCFunction)(void (*)(void))ustore_py_api_get, ustore_py_method_flags_k, doc_get},
    {"set", (PyCFunction)(void (*)(void))ustore_py_api_set, ustore_py_method_flags_k, doc_set},
    {"remove", (PyCFunction)(void (*)(void))ustore_py_api_remove, ustore_py_method_flags_k, doc_remove},
    {"contains", (PyCFunction)(void (*)(void))ustore_py_api_contains, ustore_py_method_flags_k, doc_contains},

    {"docs_write", (PyCFunction)(void (*)(void))ustore_py_api_docs_write, ustore_py_method_flags_k, doc_docs_write},
    {"docs_read", (PyCFunction)(void (*)(void))ustore_py_api_docs_read, ustore_py_method_flags_k, doc_docs_read},
    {"docs_gist", (PyCFunction)(void (*)(void))ustore_py_api_docs_gist, ustore_py_method_flags_k, doc_docs_gist},
    {"docs_gather", (PyCFunction)(void (*)(void))ustore_py_api_docs_gather, ustore_py_method_flags_k, doc_docs_gather},
    {"graph_write_edges", (PyCFunction)(void (*)(void))ustore_py_api_graph_write_edges, ustore_py_method_flags_k,
     doc_graph_write_edges},
    {"graph_remove_edges", (PyCFunction)(void (*)(void))ustore_py_api_graph_remove_edges, ustore_py_method_flags_k,
     doc_graph_remove_edges},
    {"graph_read_edges", (PyCFunction)(void (*)(void))ustore_py_api_graph_read_edges, ustore_py_method_flags_k,
     doc_graph_read_edges},
    {"graph_degrees", (PyCFunction)(void (*)(void))ustore_py_api_graph_degrees, ustore_py_method_flags_k,
     doc_graph_degrees},
    {"graph_write_vertices", (PyCFunction)(void (*)(void))ustore_py_api_graph_write_vertices, ustore_py_method_flags_k,
     doc_graph_write_vertices},
    {"graph_remove_vertices", (PyCFunction)(void (*)(void))ustore_py_api_graph_remove_vertices,
     ustore_py_method_flags_k, doc_graph_remove_vertices},
    {NULL, NULL, 0, NULL},
};

PyTypeObject DeferredViewType = {
    PyVarObject_HEAD_INIT(NULL, 0) //
        .tp_name = "ustore.DeferredView",
    .tp_doc = doc_DeferredView,
    .tp_basicsize = sizeof(View),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = View_tp_new,
    .tp_dealloc = (destructor)View_dealloc,
    .tp_repr = (reprfunc)View_repr,
    .tp_as_mapping = &receiver_as_mapping,
    .tp_methods = DeferredView_methods,
    .tp_getset = View_getset,
};

#pragma endregion Tables
