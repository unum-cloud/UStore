/**
 *  @file python/lib.c
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief The module-init glue of `ustore.lib`: the type table, the failures and `PyInit_lib`.
 *
 *  Every modality contributes rows to one table here and its verbs to one file beside this one, so
 *  a modality joining the library touches two lines of glue and nothing else.
 */
#include "lib.h"

static char const doc_module[] =                                                              //
    "GPU-Accelerated Embedded Transactional Database for High-Throughput NVMe Devices.\n"     //
    "\n"                                                                                      //
    "One handle per view of the data: a `Database` addresses the head, a `Transaction`\n"     //
    "groups changes across collections, and a `Snapshot` fixes a point in time. Every verb\n" //
    "is a batch, and a single operation is a batch of one.\n"                                 //
    "\n"                                                                                      //
    "Results land in an arena the returned tape owns, so a `memoryview` over one reads the\n" //
    "engine's own bytes and copies nothing; the arena lives exactly as long as the tape.\n"   //
    "\n"                                                                                      //
    "A key, `KeyLike` in the docs of every verb, is a `uuid.UUID`, its text, sixteen raw\n"   //
    "bytes, or an integer in [0, 2**128) read as `uuid.UUID(int=n)`, so integer keys order\n" //
    "as integers do. Keys and collection IDs always come back as `uuid.UUID`.";

/**
 *  @brief Every type the module readies, and the name it is reachable under.
 *
 *  A nameless row is readied without being added, since only a result hands that type out.
 */
static struct {

    /** The attribute the type answers to on the module, or NULL to leave it unnamed. */
    char const* name;

    /** The type to ready. */
    PyTypeObject* type;
} const module_types[] = {
    {"Database", &DatabaseType},
    {"Transaction", &TransactionType},
    {"Snapshot", &SnapshotType},
    {"Values", &ValuesType},
    {"Keys", &KeysType},
    {"Columns", &ColumnsType},
    {"Edges", &EdgesType},
    {"Collection", &CollectionType},
    {"BlobsView", &BlobsViewType},
    {"DocsView", &DocsViewType},
    {"DocsJSONView", &DocsJSONViewType},
    {NULL, &SpanType},
};

static char const doc_engines_compiled[] =                         //
    "engines_compiled() answers the engines this build carries.\n" //
    "\n"                                                           //
    "Returns:\n"                                                   //
    "  tuple[str, ...]: Some of 'ram', 'rocksdb', 'leveldb' and 'nvme', in that order.";

/** The engines this build carries, in the order @c engines_compiled answers them. */
static char const* const engines_compiled[] = {
    "ram",
#if USTORE_WITH_ROCKSDB
    "rocksdb",
#endif
#if USTORE_WITH_LEVELDB
    "leveldb",
#endif
#if USTORE_WITH_NVME
    "nvme",
#endif
};

static PyObject* api_engines_compiled(PyObject* Py_UNUSED(module), PyObject* Py_UNUSED(arguments)) {
    size_t const engines_count = ustore_py_table_size_(engines_compiled);
    PyObject* const engines = PyTuple_New((Py_ssize_t)engines_count);
    if (!engines) return NULL;
    for (size_t index = 0; index != engines_count; ++index) {
        PyObject* const name = PyUnicode_FromString(engines_compiled[index]);
        if (!name) {
            Py_DECREF(engines);
            return NULL;
        }
        PyTuple_SET_ITEM(engines, (Py_ssize_t)index, name);
    }
    return engines;
}

static PyMethodDef module_methods[] = {
    {"engines_compiled", api_engines_compiled, METH_NOARGS, doc_engines_compiled},
    {NULL, NULL, 0, NULL},
};

/** The module, named as the extension is built so @c lib.__name__ agrees with where it was found. */
static PyModuleDef module_definition = {
    PyModuleDef_HEAD_INIT, .m_name = "ustore.lib", .m_doc = doc_module, .m_size = -1, .m_methods = module_methods,
};

PyMODINIT_FUNC PyInit_lib(void) {
    size_t const types_count = ustore_py_table_size_(module_types);
    for (size_t type_index = 0; type_index != types_count; ++type_index)
        if (PyType_Ready(module_types[type_index].type) < 0) return NULL;

    if (ustore_py_uuid_import() != 0) return NULL;

    PyObject* const module = PyModule_Create(&module_definition);
    if (!module) return NULL;

#ifdef Py_GIL_DISABLED
    // Database handles are shareable across threads by contract, every blocking verb drops the
    // interpreter lock around its call, and the arena a verb borrows is claimed under a critical
    // section, so nothing here needs that lock to stay correct.
    PyUnstable_Module_SetGIL(module, Py_MOD_GIL_NOT_USED);
#endif

    if (ustore_py_register_errors(module) < 0) goto failed;
    if (PyModule_AddStringConstant(module, "__version__", USTORE_VERSION) < 0) goto failed;
    PyObject* const default_edge_id = ustore_py_uuid_to_object(&ustore_default_edge_id_k.uuid);
    int const edge_id_added = default_edge_id ? PyModule_AddObjectRef(module, "default_edge_id", default_edge_id) : -1;
    Py_XDECREF(default_edge_id);
    if (edge_id_added < 0) goto failed;

    for (size_t type_index = 0; type_index != types_count; ++type_index) {
        char const* const name = module_types[type_index].name;
        if (!name) continue;
        PyTypeObject* const type = module_types[type_index].type;
        Py_INCREF(type);
        // Only a successful `PyModule_AddObject` steals the reference, so undo just this one and
        // let the dying module release the types it already owns.
        if (PyModule_AddObject(module, name, (PyObject*)type) < 0) {
            Py_DECREF(type);
            goto failed;
        }
    }

    return module;

failed:
    Py_DECREF(module);
    return NULL;
}
