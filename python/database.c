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

#pragma region Opening

/** The spellings @c isolation= accepts, closed by a nameless row. */
static ustore_py_choice_naming_t const isolation_namings[] = {
    {"strict-serializable", ustore_isolation_strict_serializable_k},
    {"snapshot", ustore_isolation_snapshot_k},
    {NULL, 0},
};

/** The spellings @c transport= accepts, closed by a nameless row. */
static ustore_py_choice_naming_t const transport_namings[] = {
    {"automatic", ustore_transport_automatic_k},     {"blocking", ustore_transport_blocking_k},
    {"linux-uring", ustore_transport_linux_uring_k}, {"linux-aio", ustore_transport_linux_aio_k},
    {"linux-spdk", ustore_transport_linux_spdk_k},   {NULL, 0},
};

/** A size keyword of @c Database.open_nvme and the field of the init call it fills. */
typedef struct nvme_size_keyword_t {

    /** The keyword, spelled as the field, or NULL closing the table. */
    char const* name;

    /** Where the field sits in @c ustore_database_init_nvme_t. */
    size_t offset;
} nvme_size_keyword_t;

/** Every size keyword of @c Database.open_nvme, closed by a nameless row. */
static nvme_size_keyword_t const nvme_size_keywords[] = {
    {"memory_limit", offsetof(ustore_database_init_nvme_t, memory_limit)},
    {"gpu_memory_limit", offsetof(ustore_database_init_nvme_t, gpu_memory_limit)},
    {"shared_io_channels", offsetof(ustore_database_init_nvme_t, shared_io_channels)},
    {"queue_depth", offsetof(ustore_database_init_nvme_t, queue_depth)},
    {"request_buffer_bytes", offsetof(ustore_database_init_nvme_t, request_buffer_bytes)},
    {"request_buffers_count", offsetof(ustore_database_init_nvme_t, request_buffers_count)},
    {"stream_buffer_bytes", offsetof(ustore_database_init_nvme_t, stream_buffer_bytes)},
    {"stream_buffers_count", offsetof(ustore_database_init_nvme_t, stream_buffers_count)},
    {"transaction_max_bytes", offsetof(ustore_database_init_nvme_t, transaction_max_bytes)},
    {"transaction_max_elements", offsetof(ustore_database_init_nvme_t, transaction_max_elements)},
    {"collection_concurrency_limit", offsetof(ustore_database_init_nvme_t, collection_concurrency_limit)},
    {"collection_cache_limit", offsetof(ustore_database_init_nvme_t, collection_cache_limit)},
    {"collection_write_buffer_max_bytes", offsetof(ustore_database_init_nvme_t, collection_write_buffer_max_bytes)},
    {"collection_write_buffer_max_elements",
     offsetof(ustore_database_init_nvme_t, collection_write_buffer_max_elements)},
    {"collection_first_level_max_bytes", offsetof(ustore_database_init_nvme_t, collection_first_level_max_bytes)},
    {"collection_level_enlarge_factor", offsetof(ustore_database_init_nvme_t, collection_level_enlarge_factor)},
    {"collection_value_size", offsetof(ustore_database_init_nvme_t, collection_value_size)},
    {"collection_value_max_size", offsetof(ustore_database_init_nvme_t, collection_value_max_size)},
    {NULL, 0},
};

/** What an init call borrows from the arguments of a constructor, kept alive until the call returns. */
typedef struct opening_t {

    /** The encoded directory, or NULL for none. */
    PyObject* directory;

    /** The encoded paths @c data_directories point into, or NULL for none. */
    PyObject* paths;

    /** The data directories, or NULL for none. */
    ustore_data_directory_t* data_directories;

    /** Entries of @c data_directories. */
    size_t data_directories_count;

    /** The CUDA devices, or NULL for none. */
    int* gpu_devices;

    /** Entries of @c gpu_devices. */
    size_t gpu_devices_count;
} opening_t;

/** Releases whatever @p opening holds. */
static void opening_release(opening_t* opening) {
    Py_XDECREF(opening->directory);
    Py_XDECREF(opening->paths);
    PyMem_Free(opening->data_directories);
    PyMem_Free(opening->gpu_devices);
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
        opening->data_directories[index].path = PyBytes_AS_STRING(encoded);
        opening->data_directories[index].max_bytes = (ustore_size_t)max_bytes;
    }
    Py_DECREF(items);
    return 0;
}

/** Reads @p object, a sequence of CUDA device ordinals, as the GPU devices. */
static int opening_gpu_devices(opening_t* opening, PyObject* object) {
    PyObject* const items = PySequence_Fast(object, "The 'gpu_devices' option must be a sequence");
    if (!items) return -1;
    Py_ssize_t const count = PySequence_Fast_GET_SIZE(items);
    PyMem_Free(opening->gpu_devices);
    opening->gpu_devices = PyMem_Calloc((size_t)count + 1, sizeof(int));
    opening->gpu_devices_count = (size_t)count;
    if (!opening->gpu_devices) {
        Py_DECREF(items);
        PyErr_NoMemory();
        return -1;
    }
    for (Py_ssize_t index = 0; index != count; ++index) {
        long const device = PyLong_AsLong(PySequence_Fast_GET_ITEM(items, index));
        if (device == -1 && PyErr_Occurred()) {
            Py_DECREF(items);
            return -1;
        }
        if (device < 0 || device > INT_MAX) {
            Py_DECREF(items);
            PyErr_Format(PyExc_ValueError, "A CUDA device ordinal must be within [0, %d], got %ld", INT_MAX, device);
            return -1;
        }
        opening->gpu_devices[index] = (int)device;
    }
    Py_DECREF(items);
    return 0;
}

/** Wraps @p handle, which an init call opened unless @p outcome failed, in a new database of @p type. */
static PyObject* database_opened(PyTypeObject* type, ustore_database_t handle, ustore_outcome_t* outcome) {
    if (ustore_py_raise_on_outcome(outcome) != 0) return NULL;

    ustore_database_description_t description = {0};
    ustore_database_describe_t describe = {0};
    describe.database = handle;
    describe.outcome = outcome;
    describe.description = &description;
    ustore_database_describe(&describe);
    Database* const instance = outcome->status ? NULL : (Database*)type->tp_alloc(type, 0);
    if (!instance) {
        ustore_database_free(handle);
        if (!PyErr_Occurred()) ustore_py_raise_on_outcome(outcome);
        return NULL;
    }
    instance->handle = handle;
    instance->arenas.idle = NULL;
    instance->capabilities = description.capabilities;
    instance->transactions = 0;
    instance->snapshots = 0;
    return (PyObject*)instance;
}

#pragma endregion Opening

#pragma region Databases

static char const doc_Database[] =                                                             //
    "An embedded database, opened through one class method per engine.\n"                      //
    "\n"                                                                                       //
    "Database.open_ram(), open_rocksdb(), open_leveldb() and open_nvme() each take what\n"     //
    "their engine can be asked; engines_compiled() names the ones this build carries, and\n"   //
    "the others refuse to open. The directory belongs to the database, so no two\n"            //
    "processes may open the same one at once. Every database holds a main collection, which\n" //
    "can be cleared but never dropped, beside the collections it creates.\n"                   //
    "\n"                                                                                       //
    "Indexing it by name, as `db['movies']`, answers a Collection, whose `blobs`, `docs`,\n"   //
    "`graph` and `table` views create it on their first write.";

static PyObject* Database_tp_new(PyTypeObject* Py_UNUSED(type), PyObject* Py_UNUSED(args),
                                 PyObject* Py_UNUSED(keywords)) {
    PyErr_SetString(PyExc_TypeError,
                    "Database() names no engine: use open_ram, open_rocksdb, open_leveldb or open_nvme");
    return NULL;
}

static char const doc_Database_open_ram[] =                                                //
    "open_ram(directory=None, *, isolation='strict-serializable') answers a database in\n" //
    "memory.\n"                                                                            //
    "\n"                                                                                   //
    "Args:\n"                                                                              //
    "  directory (str | bytes | PathLike, optional): Where the database persists as one\n" //
    "    image, rewritten whole on a flush and on close; None keeps it in memory alone.\n" //
    "  isolation (str): How transactions see each other: 'strict-serializable' or\n"       //
    "    'snapshot'.\n"                                                                    //
    "Returns:\n"                                                                           //
    "  Database: The opened database.";

static PyObject* Database_open_ram(PyObject* type, PyObject* const* args, Py_ssize_t positional_args_count,
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
            read = ustore_py_choice_from_object(value, "isolation", isolation_namings, &isolation);
        else PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
        if (read != 0) goto failed;
    }

    ustore_database_t handle = NULL;
    ustore_outcome_t outcome = {0};
    ustore_database_init_ram_t init = {0};
    init.database = &handle;
    init.outcome = &outcome;
    init.directory = opening.directory ? PyBytes_AS_STRING(opening.directory) : NULL;
    init.isolation = (ustore_isolation_t)isolation;
    Py_BEGIN_ALLOW_THREADS;
    ustore_database_init_ram(&init);
    Py_END_ALLOW_THREADS;
    opening_release(&opening);
    return database_opened((PyTypeObject*)type, handle, &outcome);

failed:
    opening_release(&opening);
    return NULL;
}

static char const doc_Database_open_rocksdb[] =                                           //
    "open_rocksdb(directory, *, data_directories=()) answers a database on RocksDB.\n"    //
    "\n"                                                                                  //
    "Args:\n"                                                                             //
    "  directory (str | bytes | PathLike): The database's directory, which must exist.\n" //
    "  data_directories (Sequence): Directories the table files fill in order, each a\n"  //
    "    path or a (path, max_bytes) pair, where zero bytes means no limit.\n"            //
    "Returns:\n"                                                                          //
    "  Database: The opened database.\n"                                                  //
    "Raises:\n"                                                                           //
    "  UnsupportedError: This build carries no RocksDB engine.";

static PyObject* Database_open_rocksdb(PyObject* type, PyObject* const* args, Py_ssize_t positional_args_count,
                                       PyObject* args_names_tuple) {
    opening_t opening = {0};
    Py_ssize_t const args_names_count = opening_positional(&opening, "open_rocksdb", args, positional_args_count,
                                                           args_names_tuple);
    if (args_names_count < 0) goto failed;
    for (Py_ssize_t names_progress = 0; names_progress != args_names_count; ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(args_names_tuple, names_progress);
        PyObject* const value = args[positional_args_count + names_progress];
        int read = -1;
        if (PyUnicode_CompareWithASCIIString(name, "directory") == 0) read = opening_directory(&opening, value);
        else if (PyUnicode_CompareWithASCIIString(name, "data_directories") == 0)
            read = opening_data_directories(&opening, value);
        else PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
        if (read != 0) goto failed;
    }

    ustore_database_t handle = NULL;
    ustore_outcome_t outcome = {0};
    ustore_database_init_rocksdb_t init = {0};
    init.database = &handle;
    init.outcome = &outcome;
    init.directory = opening.directory ? PyBytes_AS_STRING(opening.directory) : NULL;
    init.data_directories = opening.data_directories;
    init.data_directories_count = (ustore_size_t)opening.data_directories_count;
    Py_BEGIN_ALLOW_THREADS;
    ustore_database_init_rocksdb(&init);
    Py_END_ALLOW_THREADS;
    opening_release(&opening);
    return database_opened((PyTypeObject*)type, handle, &outcome);

failed:
    opening_release(&opening);
    return NULL;
}

static char const doc_Database_open_leveldb[] =                                           //
    "open_leveldb(directory) answers a database on LevelDB.\n"                            //
    "\n"                                                                                  //
    "Args:\n"                                                                             //
    "  directory (str | bytes | PathLike): The database's directory, which must exist.\n" //
    "Returns:\n"                                                                          //
    "  Database: The opened database.\n"                                                  //
    "Raises:\n"                                                                           //
    "  UnsupportedError: This build carries no LevelDB engine.";

static PyObject* Database_open_leveldb(PyObject* type, PyObject* const* args, Py_ssize_t positional_args_count,
                                       PyObject* args_names_tuple) {
    opening_t opening = {0};
    Py_ssize_t const args_names_count = opening_positional(&opening, "open_leveldb", args, positional_args_count,
                                                           args_names_tuple);
    if (args_names_count < 0) goto failed;
    for (Py_ssize_t names_progress = 0; names_progress != args_names_count; ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(args_names_tuple, names_progress);
        PyObject* const value = args[positional_args_count + names_progress];
        int read = -1;
        if (PyUnicode_CompareWithASCIIString(name, "directory") == 0) read = opening_directory(&opening, value);
        else PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
        if (read != 0) goto failed;
    }

    ustore_database_t handle = NULL;
    ustore_outcome_t outcome = {0};
    ustore_database_init_leveldb_t init = {0};
    init.database = &handle;
    init.outcome = &outcome;
    init.directory = opening.directory ? PyBytes_AS_STRING(opening.directory) : NULL;
    Py_BEGIN_ALLOW_THREADS;
    ustore_database_init_leveldb(&init);
    Py_END_ALLOW_THREADS;
    opening_release(&opening);
    return database_opened((PyTypeObject*)type, handle, &outcome);

failed:
    opening_release(&opening);
    return NULL;
}

static char const doc_Database_open_nvme[] =                                                //
    "open_nvme(directory, *, transport='automatic', **sizes) answers a database on NVMe.\n" //
    "\n"                                                                                    //
    "Every size keyword names a field of ustore_database_init_nvme_t, like memory_limit,\n" //
    "queue_depth or collection_cache_limit, where zero keeps the engine's default.\n"       //
    "\n"                                                                                    //
    "Args:\n"                                                                               //
    "  directory (str | bytes | PathLike): The directory of the database's own files.\n"    //
    "  data_directories (Sequence): Directories the volumes fill, each a path or a\n"       //
    "    (path, max_bytes) pair; none keeps one volume inside the directory.\n"             //
    "  transport (str): How bytes move: 'automatic', 'blocking', 'linux-uring',\n"          //
    "    'linux-aio' or 'linux-spdk'.\n"                                                    //
    "  gpu_devices (Sequence[int]): CUDA devices compaction may run on.\n"                  //
    "Returns:\n"                                                                            //
    "  Database: The opened database.\n"                                                    //
    "Raises:\n"                                                                             //
    "  UnsupportedError: This build carries no NVMe engine.";

static PyObject* Database_open_nvme(PyObject* type, PyObject* const* args, Py_ssize_t positional_args_count,
                                    PyObject* args_names_tuple) {
    opening_t opening = {0};
    ustore_database_init_nvme_t init = {0};
    int transport = ustore_transport_automatic_k;
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
            *(ustore_size_t*)((char*)&init + keyword->offset) = (ustore_size_t)size;
        }
        else if (PyUnicode_CompareWithASCIIString(name, "directory") == 0) read = opening_directory(&opening, value);
        else if (PyUnicode_CompareWithASCIIString(name, "data_directories") == 0)
            read = opening_data_directories(&opening, value);
        else if (PyUnicode_CompareWithASCIIString(name, "gpu_devices") == 0)
            read = opening_gpu_devices(&opening, value);
        else if (PyUnicode_CompareWithASCIIString(name, "transport") == 0)
            read = ustore_py_choice_from_object(value, "transport", transport_namings, &transport);
        else PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
        if (read != 0) goto failed;
    }

    ustore_database_t handle = NULL;
    ustore_outcome_t outcome = {0};
    init.database = &handle;
    init.outcome = &outcome;
    init.directory = opening.directory ? PyBytes_AS_STRING(opening.directory) : NULL;
    init.data_directories = opening.data_directories;
    init.data_directories_count = (ustore_size_t)opening.data_directories_count;
    init.transport = (ustore_transport_t)transport;
    init.gpu_devices = opening.gpu_devices;
    init.gpu_devices_count = (ustore_size_t)opening.gpu_devices_count;
    Py_BEGIN_ALLOW_THREADS;
    ustore_database_init_nvme(&init);
    Py_END_ALLOW_THREADS;
    opening_release(&opening);
    return database_opened((PyTypeObject*)type, handle, &outcome);

failed:
    opening_release(&opening);
    return NULL;
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

static char const doc_Database_close[] =                                                        //
    "close() answers None.\n"                                                                   //
    "\n"                                                                                        //
    "Closes the database, while the data persists on disk. Every transaction and snapshot it\n" //
    "opened must be closed first, since both read through the handle this releases.\n"          //
    "\n"                                                                                        //
    "Raises:\n"                                                                                 //
    "  ArgumentError: A transaction or a snapshot of this database is still open.";

static PyObject* Database_close(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    Database* const database = (Database*)self;
    if (database->transactions != 0) {
        ustore_py_raise(ustore_status_invalid_argument_k, "%zd transactions of this database are still open",
                        database->transactions);
        return NULL;
    }
    if (database->snapshots != 0) {
        ustore_py_raise(ustore_status_invalid_argument_k, "%zd snapshots of this database are still open",
                        database->snapshots);
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
    return PyBool_FromLong((self->capabilities & ustore_supports_transactions_k) != 0);
}

static char const doc_Database_supports_collections[] = "Whether the engine holds collections beyond the main one.";

static PyObject* Database_get_supports_collections(Database* self, void* Py_UNUSED(closure)) {
    return PyBool_FromLong((self->capabilities & ustore_supports_collections_k) != 0);
}

static char const doc_Database_supports_snapshots[] = "Whether the engine captures point-in-time views.";

static PyObject* Database_get_supports_snapshots(Database* self, void* Py_UNUSED(closure)) {
    return PyBool_FromLong((self->capabilities & ustore_supports_snapshots_k) != 0);
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
    ustore_outcome_t outcome = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context.database);

    ustore_database_control_t control = {0};
    control.database = context.database->handle;
    control.outcome = &outcome;
    control.request = request;
    control.response = &response;
    control.arena = &arena;

    Py_BEGIN_ALLOW_THREADS;
    ustore_database_control(&control);
    Py_END_ALLOW_THREADS;

    PyObject* const answered = ustore_py_raise_on_outcome(&outcome) == 0
                                   ? PyUnicode_FromString(response ? response : "")
                                   : NULL;
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

ustore_py_choice_naming_t const ustore_py_modality_namings[] = {
    {"any", ustore_modality_any_k},
    {"blobs", ustore_modality_blobs_k},
    {"docs", ustore_modality_docs_k},
    {"graph", ustore_modality_graph_k},
    {"strings", ustore_modality_strings_k},
    {"pages", ustore_modality_pages_k},
    {NULL, 0},
};

static char const doc_Database_collection_create[] =                                         //
    "collection_create(name, *, modality='any') answers the new collection's identifier.\n"  //
    "\n"                                                                                     //
    "Creates an empty collection and records the name for it, in the two phases a name\n"    //
    "always resolves through. Every other verb addresses the collection by the identifier\n" //
    "this answers.\n"                                                                        //
    "\n"                                                                                     //
    "Args:\n"                                                                                //
    "  name (str): The name to record, which must not already be taken.\n"                   //
    "  modality (str): What the collection holds: 'any', 'blobs', 'docs', 'graph',\n"        //
    "    'strings' or 'pages'.\n"                                                            //
    "Returns:\n"                                                                             //
    "  UUID: The identifier addressing the new collection.\n"                                //
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

    int modality = ustore_modality_any_k;
    for (Py_ssize_t names_progress = 0, args_progress = positional_args_count; names_progress != args_names_count;
         ++args_progress, ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(args_names_tuple, names_progress);
        if (PyUnicode_CompareWithASCIIString(name, "modality") == 0) {
            if (ustore_py_choice_from_object(args[args_progress], "modality", ustore_py_modality_namings, &modality) !=
                0)
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
    create.database = context.database->handle;
    create.outcome = &outcome;
    create.name = name;
    create.name_length = (ustore_length_t)name_length;
    create.modality = (ustore_modality_t)modality;
    create.collection = &created;

    Py_BEGIN_ALLOW_THREADS;
    ustore_collection_create(&create);
    Py_END_ALLOW_THREADS;
    if (ustore_py_raise_on_outcome(&outcome) != 0) return NULL;
    return ustore_py_uuid_to_object(&created.uuid);
}

static char const doc_Database_collection_find[] =                               //
    "collection_find(name) answers the identifier the name addresses.\n"         //
    "\n"                                                                         //
    "Args:\n"                                                                    //
    "  name (str): The name to resolve; the empty one is the main collection.\n" //
    "Returns:\n"                                                                 //
    "  UUID: The identifier addressing the collection.\n"                        //
    "Raises:\n"                                                                  //
    "  MissingError: No collection carries the name.";

static PyObject* Database_collection_find(PyObject* self, PyObject* name_object) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    ustore_collection_id_t found = ustore_collection_main_k;
    if (ustore_py_collection_from_object(&context, NULL, name_object, &found) != 0) return NULL;
    return ustore_py_uuid_to_object(&found.uuid);
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
    switch (ustore_py_collection_named(&context, NULL, name, (size_t)length, &found, NULL)) {
    case ustore_py_naming_found_k: Py_RETURN_TRUE;
    case ustore_py_naming_missing_k: Py_RETURN_FALSE;
    case ustore_py_naming_failed_k: return NULL;
    }
    Py_RETURN_FALSE;
}

static char const doc_Database_collection_list[] =                                   //
    "collection_list() answers the identifiers of every collection.\n"               //
    "\n"                                                                             //
    "The main collection is never listed, and collection_names answers the names.\n" //
    "\n"                                                                             //
    "Returns:\n"                                                                     //
    "  tuple[UUID, ...]: One identifier per collection.";

static PyObject* Database_collection_list(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;

    ustore_size_t count = 0;
    ustore_collection_id_t* identifiers = NULL;
    ustore_outcome_t outcome = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context.database);

    ustore_collection_list_t list = {0};
    list.database = context.database->handle;
    list.outcome = &outcome;
    list.count = &count;
    list.collections = &identifiers;
    list.arena = &arena;

    Py_BEGIN_ALLOW_THREADS;
    ustore_collection_list(&list);
    Py_END_ALLOW_THREADS;

    PyObject* listed = NULL;
    if (ustore_py_raise_on_outcome(&outcome) == 0 && (listed = PyTuple_New((Py_ssize_t)count)) != NULL) {
        for (ustore_size_t entry_index = 0; entry_index != count; ++entry_index) {
            PyObject* const identifier = ustore_py_uuid_to_object(&identifiers[entry_index].uuid);
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

static char const doc_Database_collection_drop[] =                                          //
    "collection_drop(collection, *, mode='collection') answers None.\n"                     //
    "\n"                                                                                    //
    "Removes a collection, or empties it. Removing it forgets its name with it, so the\n"   //
    "name can be given to another collection afterwards.\n"                                 //
    "\n"                                                                                    //
    "Args:\n"                                                                               //
    "  collection (UUID | str): The collection, by identifier or by name.\n"                //
    "  mode (str, optional): 'collection' removes the handle and everything in it,\n"       //
    "    'pairs' removes every key and value, and 'values' removes every value. The main\n" //
    "    collection accepts the latter two alone.\n"                                        //
    "Raises:\n"                                                                             //
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

    // A name resolves first, and the collection it addresses takes the name with it when it goes
    ustore_collection_id_t collection = ustore_collection_main_k;
    if (ustore_py_collection_from_object(&context, NULL, args[0], &collection) != 0) return NULL;
    ustore_outcome_t outcome = {0};
    ustore_collection_drop_t drop = {0};
    drop.database = context.database->handle;
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

static char const doc_Snapshot[] =                                                           //
    "A point-in-time view of the whole database, which reads follow by naming it.\n"         //
    "\n"                                                                                     //
    "Built through `Database.snapshot()`, never by name. Every read verb of the database\n"  //
    "takes it through the `snapshot` keyword, and it pins the versions those reads need\n"   //
    "until it closes. It lives no longer than the process, and a database waits for every\n" //
    "snapshot it opened before it closes.\n"                                                 //
    "\n"                                                                                     //
    "Used as a context manager, it closes on exit.";

static PyObject* Snapshot_tp_new(PyTypeObject* Py_UNUSED(type), PyObject* Py_UNUSED(args),
                                 PyObject* Py_UNUSED(keywords)) {
    PyErr_SetString(PyExc_TypeError, "A snapshot is built through Database.snapshot()");
    return NULL;
}

/** Releases the snapshot's handle and lets go of the database that was holding it open. */
static void snapshot_close(Snapshot* snapshot) {
    if (!snapshot->handle) return;
    ustore_snapshot_t const handle = snapshot->handle;
    snapshot->handle = NULL;

    Py_BEGIN_ALLOW_THREADS;
    ustore_snapshot_free(handle);
    Py_END_ALLOW_THREADS;
    --((Database*)snapshot->database)->snapshots;
}

static void Snapshot_dealloc(Snapshot* self) {
    snapshot_close(self);
    Py_XDECREF(self->database);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static char const doc_Database_snapshot[] =                                                //
    "snapshot() answers a new Snapshot.\n"                                                 //
    "\n"                                                                                   //
    "Pins a point-in-time view of the whole database, which reads then follow by naming\n" //
    "it through their `snapshot` keyword.\n"                                               //
    "\n"                                                                                   //
    "Returns:\n"                                                                           //
    "  Snapshot: A view of this database as it stands now.\n"                              //
    "Raises:\n"                                                                            //
    "  UnsupportedError: The engine keeps no snapshots.";

static PyObject* Database_snapshot(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    Database* const database = (Database*)self;
    if (!database->handle) {
        PyErr_SetString(PyExc_ValueError, "The database is closed");
        return NULL;
    }

    ustore_snapshot_t handle = NULL;
    ustore_outcome_t outcome = {0};
    ustore_snapshot_init_t init = {0};
    init.snapshot = &handle;
    init.database = database->handle;
    init.outcome = &outcome;

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
    ++database->snapshots;
    return (PyObject*)instance;
}

static char const doc_Snapshot_export[] =                                               //
    "export(directory) answers None.\n"                                                 //
    "\n"                                                                                //
    "Writes the snapshot out as a new database, which another process may then open.\n" //
    "\n"                                                                                //
    "Args:\n"                                                                           //
    "  directory (str | bytes | PathLike): Where the exported database lands.\n"        //
    "Raises:\n"                                                                         //
    "  UnsupportedError: The engine exports no snapshots.";

static PyObject* Snapshot_export(PyObject* self, PyObject* directory) {
    Snapshot* const snapshot = (Snapshot*)self;
    if (!snapshot->handle) {
        PyErr_SetString(PyExc_ValueError, "The snapshot is closed");
        return NULL;
    }
    Database* const database = (Database*)snapshot->database;
    if (!database->handle) {
        PyErr_SetString(PyExc_ValueError, "The database is closed");
        return NULL;
    }
    PyObject* encoded = NULL;
    if (!PyUnicode_FSConverter(directory, &encoded)) return NULL;

    ustore_outcome_t outcome = {0};
    ustore_snapshot_export_t exported = {0};
    exported.database = database->handle;
    exported.outcome = &outcome;
    exported.snapshot = snapshot->handle;
    exported.path = PyBytes_AS_STRING(encoded);

    Py_BEGIN_ALLOW_THREADS;
    ustore_snapshot_export(&exported);
    Py_END_ALLOW_THREADS;
    Py_DECREF(encoded);
    if (ustore_py_raise_on_outcome(&outcome) != 0) return NULL;
    Py_RETURN_NONE;
}

static char const doc_Snapshot_close[] =                                                  //
    "close() answers None.\n"                                                             //
    "\n"                                                                                  //
    "Releases the snapshot, unpinning the versions only it kept, and lets its database\n" //
    "close. Closing twice is safe.";

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

    ustore_watch_t watch = ustore_watch_keys_k;
    for (Py_ssize_t names_progress = 0, args_progress = positional_args_count; names_progress != args_names_count;
         ++args_progress, ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(args_names_tuple, names_progress);
        if (PyUnicode_CompareWithASCIIString(name, "watch") == 0) {
            int const watching = PyObject_IsTrue(args[args_progress]);
            if (watching < 0) return NULL;
            watch = watching ? ustore_watch_keys_k : ustore_watch_none_k;
        }
        else {
            PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
            return NULL;
        }
    }

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
    instance->watch = watch;
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

    ustore_durability_t durability = ustore_durability_buffered_k;
    for (Py_ssize_t names_progress = 0, args_progress = positional_args_count; names_progress != args_names_count;
         ++args_progress, ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(args_names_tuple, names_progress);
        if (PyUnicode_CompareWithASCIIString(name, "flush") == 0) {
            int const flushing = PyObject_IsTrue(args[args_progress]);
            if (flushing < 0) return NULL;
            durability = flushing ? ustore_durability_flushed_k : ustore_durability_buffered_k;
        }
        else {
            PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
            return NULL;
        }
    }

    ustore_outcome_t outcome = {0};
    ustore_transaction_commit_t commit = {0};
    commit.database = context.database->handle;
    commit.outcome = &outcome;
    commit.transaction = context.transaction;
    commit.durability = durability;

    Py_BEGIN_ALLOW_THREADS;
    ustore_transaction_commit(&commit);
    Py_END_ALLOW_THREADS;
    if (ustore_py_raise_on_outcome(&outcome) != 0) return NULL;
    Py_RETURN_NONE;
}

static char const doc_Transaction_stage[] =                                            //
    "stage() answers the stamp the stage took.\n"                                      //
    "\n"                                                                               //
    "Runs the first phase of a two-phase commit, after which a commit publishes the\n" //
    "work.\n"                                                                          //
    "\n"                                                                               //
    "Returns:\n"                                                                       //
    "  int: The order this stage took among the transactions that applied.";

static PyObject* Transaction_stage(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;

    ustore_stamp_t stamp = 0;
    ustore_outcome_t outcome = {0};
    ustore_transaction_stage_t stage = {0};
    stage.database = context.database->handle;
    stage.outcome = &outcome;
    stage.transaction = context.transaction;
    stage.stamp = &stamp;

    Py_BEGIN_ALLOW_THREADS;
    ustore_transaction_stage(&stage);
    Py_END_ALLOW_THREADS;
    if (ustore_py_raise_on_outcome(&outcome) != 0) return NULL;
    return PyLong_FromUnsignedLongLong(stamp);
}

static char const doc_Transaction_rollback[] =                                            //
    "rollback() answers None.\n"                                                          //
    "\n"                                                                                  //
    "Withdraws a stage, releasing its stamp and whatever it claimed, while keeping the\n" //
    "reads and writes, so the transaction can stage or commit again. `reset` drops the\n" //
    "work itself.";

static PyObject* Transaction_rollback(PyObject* self, PyObject* Py_UNUSED(ignored)) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;

    ustore_outcome_t outcome = {0};
    ustore_transaction_rollback_t rollback = {0};
    rollback.database = context.database->handle;
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
    reset.database = context.database->handle;
    reset.outcome = &outcome;

    Py_BEGIN_ALLOW_THREADS;
    ustore_transaction_init(&reset);
    Py_END_ALLOW_THREADS;
    if (ustore_py_raise_on_outcome(&outcome) != 0) return NULL;
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
    {"open_ram", (PyCFunction)Database_open_ram, ustore_py_method_flags_k | METH_CLASS, doc_Database_open_ram},
    {"open_rocksdb", (PyCFunction)Database_open_rocksdb, ustore_py_method_flags_k | METH_CLASS,
     doc_Database_open_rocksdb},
    {"open_leveldb", (PyCFunction)Database_open_leveldb, ustore_py_method_flags_k | METH_CLASS,
     doc_Database_open_leveldb},
    {"open_nvme", (PyCFunction)Database_open_nvme, ustore_py_method_flags_k | METH_CLASS, doc_Database_open_nvme},
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

    {"snapshot", (PyCFunction)Database_snapshot, METH_NOARGS, doc_Database_snapshot},
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
    {"remove", (PyCFunction)ustore_py_api_remove, ustore_py_method_flags_k, doc_remove},
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

/** Indexing a database by name answers the collection, as `db['movies']` does. */
static PyMappingMethods Database_as_mapping = {
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
    .tp_as_mapping = &Database_as_mapping,
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
    {"reset", (PyCFunction)Transaction_reset, METH_NOARGS, doc_Transaction_reset},
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
    {"remove", (PyCFunction)ustore_py_api_remove, ustore_py_method_flags_k, doc_remove},
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

/** Indexing a transaction by name answers the collection its verbs stage into, as `txn['payments']` does. */
static PyMappingMethods Transaction_as_mapping = {
    .mp_subscript = (binaryfunc)ustore_py_collection_subscript,
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
    .tp_as_mapping = &Transaction_as_mapping,
    .tp_methods = Transaction_methods,
    .tp_getset = Transaction_getset,
};

static PyGetSetDef Snapshot_getset[] = {
    {"database", (getter)Snapshot_get_database, NULL, doc_Snapshot_database, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyMethodDef Snapshot_methods[] = {
    {"export", (PyCFunction)Snapshot_export, METH_O, doc_Snapshot_export},
    {"close", (PyCFunction)Snapshot_close, METH_NOARGS, doc_Snapshot_close},
    {"__enter__", (PyCFunction)Snapshot_enter, METH_NOARGS, NULL},
    {"__exit__", (PyCFunction)Snapshot_exit, ustore_py_method_flags_k, NULL},
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
    .tp_methods = Snapshot_methods,
    .tp_getset = Snapshot_getset,
};

#pragma endregion Tables
