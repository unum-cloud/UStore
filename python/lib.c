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
    "groups changes across collections, and a snapshot fixes a point in time. Every verb\n"   //
    "is a batch, and a single operation is a batch of one.\n"                                 //
    "\n"                                                                                      //
    "Results land in an arena the returned tape owns, so a `memoryview` over one reads the\n" //
    "engine's own bytes and copies nothing; the arena lives exactly as long as the tape.";

static struct {
    char const* name;
    PyTypeObject* type;
} const module_types[] = {
    {"Database", &DatabaseType},
    {"Transaction", &TransactionType},
    {"Values", &ValuesType}, //! Exported by a read, and named so a caller can test against it.
    {"Keys", &KeysType},     //! Exported by a scan or a sample, likewise.
};

static char const doc_engines_compiled[] =                                               //
    "engines_compiled() answers the engines this build carries as a tuple of strings.\n" //
    "\n"                                                                                 //
    "Every engine library defines the same symbols, so one of them links into the\n"     //
    "extension and the tuple names it.";

static PyObject* api_engines_compiled(PyObject* Py_UNUSED(module), PyObject* Py_UNUSED(arguments)) {
    PyObject* const joined = PyUnicode_FromString(USTORE_ENGINES);
    PyObject* const separator = PyUnicode_FromString(",");
    PyObject* const names = joined && separator ? PyUnicode_Split(joined, separator, -1) : NULL;
    PyObject* const engines = names ? PyList_AsTuple(names) : NULL;
    Py_XDECREF(names);
    Py_XDECREF(separator);
    Py_XDECREF(joined);
    return engines;
}

static PyMethodDef module_methods[] = {
    {"engines_compiled", api_engines_compiled, METH_NOARGS, doc_engines_compiled},
    {NULL, NULL, 0, NULL},
};

// The dotted name the extension is built as, so `lib.__name__` agrees with where the import
// machinery found it. The types keep the undotted `ustore.` prefix in their `tp_name`, which is
// where the package re-exports them.
static PyModuleDef module_definition = {
    PyModuleDef_HEAD_INIT, .m_name = "ustore.lib", .m_doc = doc_module, .m_size = -1, .m_methods = module_methods,
};

PyMODINIT_FUNC PyInit_lib(void) {
    size_t const types_count = ustore_py_table_size_(module_types);
    for (size_t type_index = 0; type_index != types_count; ++type_index)
        if (PyType_Ready(module_types[type_index].type) < 0) return NULL;

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
