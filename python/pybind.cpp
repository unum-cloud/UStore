/**
 * @brief Python bindings for Universal Key-Values.
 *
 * ## Features
 *
 * - Zero-Copy data forwarding into Python runtime
 *   https://stackoverflow.com/questions/58113973/returning-multiple-pyarray-without-copying-in-pybind11
 * - Calls the C functions outside of the Global Interpret Lock
 *   https://stackoverflow.com/a/55205951
 *
 * ## Low-level CPython bindings
 *
 * The complexity of implementing the low-level interface boils
 * down to frequent manual calls to `PyArg_ParseTuple()`.
 * It also gives us a more fine-grained control over `PyGILState_Release()`.
 * https://docs.python.org/3/extending/extending.html
 * https://realpython.com/build-python-c-extension-module/
 * https://docs.python.org/3/c-api/arg.html
 * https://docs.python.org/3/c-api/mapping.html
 * https://docs.python.org/3/c-api/init.html#thread-state-and-the-global-interpreter-lock
 *
 * ## High-level Python bindings generators
 *
 * https://realpython.com/python-bindings-overview/
 * http://blog.behnel.de/posts/cython-pybind11-cffi-which-tool-to-choose.html
 * https://pythonspeed.com/articles/python-extension-performance/
 * https://github.com/wjakob/nanobind
 * https://wiki.python.org/moin/IntegratingPythonWithOtherLanguages
 */

#include "pybind.hpp"

using namespace unum::ukv;
using namespace unum;

#define stringify_value_m(a) stringify_m(a)
#define stringify_m(a) #a

PYBIND11_MODULE(UKV_PYTHON_MODULE_NAME, m) {
    m.attr("__name__") = "ukv." stringify_value_m(UKV_PYTHON_MODULE_NAME);

    m.doc() = R"doc(
    ======================================================
    Python bindings for Universal Key Value store library.
    ======================================================
    
    Supports:
    **********
    * Collection-level CRUD operations, like `dict`.
    * Batch operations & ACID transactions.
    * Graph collections, mimicking `networkx`.
    * Tabular views, mimicking `pandas`.
    * Apache Arrow exports for inter-process communication.
    
    )doc";

    if (arrow::py::import_pyarrow())
        throw std::runtime_error("Failed to initialize PyArrow");

    wrap_database(m);
    wrap_pandas(m);
    wrap_networkx(m);
    wrap_document(m);
}
