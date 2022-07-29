/**
 * @brief Python bindings for UKV.
 *
 * @section Interface
 *
 * Primary DataBase Methods:
 *      * get(collection?, key, default?) ~ Single Read
 *      * set(collection?, key, value, default?) ~ Single Insert
 *      * __in__(key) ~ Single & Batch Contains
 *      * __getitem__(key: int) ~ Value Lookup
 *      * __setitem__(key: int, value) ~ Value Upserts
 *      * __getitem__(collection: str) ~ Sub-Collection Lookup
 *      * clear() ~ Removes all items
 *      * TODO: pop(key, default?) ~ Removes the key in and returns its value.
 *      * TODO: update(mapping) ~ Batch Insert/Put
 *
 * Additional Batch Methods:
 *      * fill_tensor(collection?, keys, max_length: int, padding: byte)
 *
 * Intentionally not implemented:
 *      * __len__() ~ It's hard to consistently estimate the collection.
 *      * popitem() ~ We can't guarantee Last-In First-Out semantics.
 *      * setdefault(key[, default]) ~ As default values are useless in DBs.
 *
 * Similarly, those operations are supported at "collection-level", not just
 * DataBase level.
 *
 * Full @c `dict` API:
 * https://docs.python.org/3/library/stdtypes.html#mapping-types-dict
 * https://python-reference.readthedocs.io/en/latest/docs/dict/
 * https://docs.python.org/3/tutorial/datastructures.html#dictionaries
 * https://docs.python.org/3/c-api/dict.html
 *
 * @section Understanding Python Strings
 *
 * Most dynamic allocations in CPython are done via `PyObject_Malloc`,
 * `PyMem_Malloc`, `PyMem_Calloc`, so understanding the memory layout
 * is as easy as searching the git repo for those function calls.
 * What you will see, is that metadata is generally stored in the same allocated
 * region, as a prefix, as in most dynamically typed or List-oriented systems.
 * It's identical for `PyBytes_FromStringAndSize`, `PyUnicode_New`, `PyList_New`.
 *
 * Same way for lists of lists. The `PyListObject` stores a vector of pointers
 * to it's internal entries in a member @p `ob_item`. So we forward that list directly
 * to our C bindings, checking beforehand, that the internal objects are either strings,
 * byte-strings, or NumPy arrays.
 *
 * > PEP 393 – Flexible String Representation
 *   Describes the 3 possible memory layouts, including the @c `PyASCIIObject`,
 *   the @c `PyCompactUnicodeObject` and the @c `PyUnicodeObject`.
 *   https://peps.python.org/pep-0393/
 *   https://docs.python.org/3/c-api/unicode.html
 *
 */

#include <vector>
#include <utility>
#include <algorithm>

#include <Python.h>

#include "pybind.hpp"

using namespace unum::ukv;
using namespace unum;

std::shared_ptr<py_txn_t> begin_if_needed(py_txn_t& py_txn) {
    if (py_txn.native)
        return py_txn.shared_from_this();

    [[maybe_unused]] py::gil_scoped_release release;
    py_txn.native.reset().throw_unhandled();
    return py_txn.shared_from_this();
}

void commit_txn(py_txn_t& py_txn) {

    [[maybe_unused]] py::gil_scoped_release release;
    py_txn.native.commit().throw_unhandled();
}

bool contains_item( //
    ukv_t db_ptr,
    ukv_txn_t txn_ptr,
    ukv_collection_t collection_ptr,
    arena_t& arena,
    ukv_key_t key) {

    status_t status;
    ukv_options_t options = ukv_option_read_lengths_k;
    ukv_val_len_t* found_lengths = nullptr;
    ukv_val_ptr_t found_values = nullptr;

    [[maybe_unused]] py::gil_scoped_release release;
    ukv_read( //
        db_ptr,
        txn_ptr,
        1,
        &collection_ptr,
        0,
        &key,
        0,
        options,
        &found_lengths,
        &found_values,
        arena.member_ptr(),
        status.member_ptr());
    status.throw_unhandled();

    return found_lengths[0] != ukv_val_len_missing_k;
}

std::optional<py::bytes> get_item( //
    ukv_t db_ptr,
    ukv_txn_t txn_ptr,
    ukv_collection_t collection_ptr,
    arena_t& arena,
    ukv_key_t key) {

    status_t status;
    ukv_options_t options = ukv_options_default_k;
    ukv_val_len_t* found_lengths = nullptr;
    ukv_val_ptr_t found_values = nullptr;

    [[maybe_unused]] py::gil_scoped_release release;
    ukv_read( //
        db_ptr,
        txn_ptr,
        1,
        &collection_ptr,
        0,
        &key,
        0,
        options,
        &found_lengths,
        &found_values,
        arena.member_ptr(),
        status.member_ptr());
    status.throw_unhandled();

    if (found_lengths[0] == ukv_val_len_missing_k)
        return std::nullopt;

    // To fetch data without copies, there is a hacky way:
    // https://github.com/pybind/pybind11/issues/1236#issuecomment-527730864
    // But in that case we can't guarantee memory alignment, so doing a copy
    // is hard to avoid in Python.
    // https://github.com/pybind/pybind11/blob/a05bc3d2359d12840ef2329d68f613f1a7df9c5d/include/pybind11/pytypes.h#L1474
    // https://docs.python.org/3/c-api/bytes.html
    // https://github.com/python/cpython/blob/main/Objects/bytesobject.c
    auto data = reinterpret_cast<char const*>(found_values);
    return py::bytes {data, found_lengths[0]};
}

/**
 * @brief Exports values into preallocated multi-dimensional NumPy-like buffers.
 *        The most performant batch-reading method, ideal for ML.
 *
 * Contrary to most data types exposed by the Python interpreter,
 * buffers are not @c `PyObject` pointers but rather simple C structures.
 * This allows them to be created and copied very simply.
 * When a generic wrapper around a buffer is needed, a @c `memoryview`
 * object can be created.
 * https://docs.python.org/3/c-api/buffer.html#buffer-structure
 *
 * @param keys       A NumPy array of keys.
 * @param values_arr A buffer-protocol object, whose `shape` has 2 dims
 *                   the `itemsize` is just one byte. The first dimensions
 *                   must match with `len(keys)` and the second one must be
 *                   at least 8 for us be able to efficiently reuse that memory.
 *
 * @param values_lengths May be nullptr.
 *
 * https://pybind11.readthedocs.io/en/stable/advanced/pycpp/numpy.html#buffer-protocol
 * https://pybind11.readthedocs.io/en/stable/advanced/cast/overview.html#list-of-all-builtin-conversions
 * https://docs.python.org/3/c-api/buffer.html
 */
void fill_tensor( //
    ukv_t db_ptr,
    ukv_txn_t txn_ptr,
    ukv_collection_t collection_ptr,
    arena_t& arena,
    py::handle keys_arr,
    py::handle values_arr,
    py::handle values_lengths_arr,
    std::uint8_t padding_char = 0) {

    // TODO: Reuse strided_range and strided_matrix converters
    // Check if we are receiving protocol buffers
    PyObject* keys_obj = keys_arr.ptr();
    PyObject* values_obj = values_arr.ptr();
    PyObject* values_lengths_obj = values_lengths_arr.ptr();
    if (!PyObject_CheckBuffer(keys_obj) | !PyObject_CheckBuffer(values_obj) | !PyObject_CheckBuffer(values_lengths_obj))
        throw std::invalid_argument("All arguments must implement the buffer protocol");

    // Take buffer protocol handles
    // Flags can be: https://docs.python.org/3/c-api/buffer.html#readonly-format
    auto output_flags = PyBUF_WRITABLE | PyBUF_ANY_CONTIGUOUS | PyBUF_STRIDED;
    py_received_buffer_t keys, values, values_lengths;
    keys.initialized = PyObject_GetBuffer(keys_obj, &keys.py, PyBUF_ANY_CONTIGUOUS) == 0;
    values.initialized = PyObject_GetBuffer(values_obj, &values.py, output_flags) == 0;
    values_lengths.initialized = PyObject_GetBuffer(values_lengths_obj, &values_lengths.py, output_flags) == 0;
    if (!keys.initialized | !values.initialized | !values_lengths.initialized)
        throw std::invalid_argument("Couldn't obtain buffer overviews");
    if (!values.py.shape | !values_lengths.py.shape | !values.py.strides | !values_lengths.py.strides)
        throw std::invalid_argument("Outputs shape wasn't inferred");

    // Validate the format of `keys`
    if (keys.py.itemsize != sizeof(ukv_key_t))
        throw std::invalid_argument("Keys type mismatch");
    if (keys.py.ndim != 1 || !PyBuffer_IsContiguous(&keys.py, 'A'))
        throw std::invalid_argument("Keys must be placed in a continuous 1 dimensional array");
    if (keys.py.strides[0] != sizeof(ukv_key_t))
        throw std::invalid_argument("Keys can't be strided");
    ukv_size_t const tasks_count = static_cast<ukv_size_t>(keys.py.len / keys.py.itemsize);
    ukv_key_t const* keys_ptr = reinterpret_cast<ukv_key_t const*>(keys.py.buf);

    // Validate the format of `values`
    if (values.py.ndim != 2)
        throw std::invalid_argument("Output tensor must have rank 2");
    if (values.py.itemsize != sizeof(std::uint8_t))
        throw std::invalid_argument("Output tensor must have single-byte entries");
    if ((values.py.shape[0] <= 0) || values.py.shape[1] <= 0)
        throw std::invalid_argument("Output tensor sides can't be zero");
    if ((values.py.strides[0] <= 0) || values.py.strides[1] <= 0)
        throw std::invalid_argument("Output tensor strides can't be negative");
    if (tasks_count != static_cast<ukv_size_t>(values.py.shape[0]))
        throw std::invalid_argument("Number of input keys and output slots doesn't match");
    auto outputs_bytes = reinterpret_cast<std::uint8_t*>(values.py.buf);
    auto outputs_bytes_stride = static_cast<std::size_t>(values.py.strides[0]);
    auto output_bytes_cap = static_cast<ukv_val_len_t>(values.py.shape[1]);

    // Validate the format of `values_lengths`
    if (values_lengths.py.ndim != 1)
        throw std::invalid_argument("Lengths tensor must have rank 1");
    if (values_lengths.py.itemsize != sizeof(ukv_val_len_t))
        throw std::invalid_argument("Lengths tensor must have 4-byte entries");
    if (values_lengths.py.shape[0] <= 0)
        throw std::invalid_argument("Lengths tensor sides can't be zero");
    if (values_lengths.py.strides[0] <= 0)
        throw std::invalid_argument("Lengths tensor strides can't be negative");
    if (tasks_count != static_cast<ukv_size_t>(values_lengths.py.shape[0]))
        throw std::invalid_argument("Number of input keys and output slots doesn't match");
    auto outputs_lengths_bytes = reinterpret_cast<std::uint8_t*>(values_lengths.py.buf);
    auto outputs_lengths_bytes_stride = static_cast<std::size_t>(values_lengths.py.strides[0]);

    // Perform the read
    [[maybe_unused]] py::gil_scoped_release release;
    status_t status;
    ukv_val_len_t* found_lengths = nullptr;
    ukv_val_ptr_t found_values = nullptr;
    ukv_options_t options = ukv_options_default_k;

    ukv_read( //
        db_ptr,
        txn_ptr,
        tasks_count,
        &collection_ptr,
        0,
        keys_ptr,
        sizeof(ukv_key_t),
        options,
        &found_lengths,
        &found_values,
        arena.member_ptr(),
        status.member_ptr());

    status.throw_unhandled();

    // Export the data into the matrix
    taped_values_view_t inputs {found_lengths, found_values, tasks_count};
    tape_iterator_t input_it = inputs.begin();
    for (ukv_size_t i = 0; i != tasks_count; ++i, ++input_it) {
        value_view_t input = *input_it;
        auto input_bytes = reinterpret_cast<std::uint8_t const*>(input.begin());
        auto input_length = static_cast<ukv_val_len_t const>(input.size());
        std::uint8_t* output_bytes = outputs_bytes + outputs_bytes_stride * i;
        ukv_val_len_t& output_length =
            *reinterpret_cast<ukv_val_len_t*>(outputs_lengths_bytes + outputs_lengths_bytes_stride * i);

        std::size_t count_copy = std::min(output_bytes_cap, input_length);
        std::size_t count_pads = output_bytes_cap - count_copy;
        std::memcpy(output_bytes, input_bytes, count_copy);
        std::memset(output_bytes + count_copy, padding_char, count_pads);

        output_length = count_copy;
    }
}

void set_item( //
    ukv_t db_ptr,
    ukv_txn_t txn_ptr,
    ukv_collection_t collection_ptr,
    arena_t& arena,
    ukv_key_t key,
    py::bytes const* value = nullptr) {

    ukv_options_t options = ukv_options_default_k;
    ukv_val_ptr_t ptr = value ? ukv_val_ptr_t(std::string_view {*value}.data()) : nullptr;
    ukv_val_len_t len = value ? static_cast<ukv_val_len_t>(std::string_view {*value}.size()) : 0;
    status_t status;
    ukv_val_len_t offset_in_val = 0;

    [[maybe_unused]] py::gil_scoped_release release;
    ukv_write( //
        db_ptr,
        txn_ptr,
        1,
        &collection_ptr,
        0,
        &key,
        0,
        &ptr,
        0,
        &offset_in_val,
        0,
        &len,
        0,
        options,
        arena.member_ptr(),
        status.member_ptr());

    status.throw_unhandled();
}

void set_item( //
    ukv_t db_ptr,
    ukv_txn_t txn_ptr,
    ukv_collection_t collection_ptr,
    arena_t& arena,
    py::handle const& keys_arr,
    py::handle const& values_arr) {

    PyObject* keys_obj = keys_arr.ptr();
    PyObject* values_obj = values_arr.ptr();

    if (!PyObject_CheckBuffer(keys_obj) | !PyObject_CheckBuffer(values_obj))
        throw std::invalid_argument("All arguments must implement the buffer protocol");

    py_received_buffer_t keys, values;
    keys.initialized = PyObject_GetBuffer(keys_obj, &keys.py, PyBUF_ANY_CONTIGUOUS) == 0;
    values.initialized = PyObject_GetBuffer(values_obj, &values.py, PyBUF_ANY_CONTIGUOUS) == 0;

    if (!keys.initialized | !values.initialized)
        throw std::invalid_argument("Couldn't obtain buffer overviews");

    // Validate the format of `keys`
    if (keys.py.itemsize != sizeof(ukv_key_t))
        throw std::invalid_argument("Keys type mismatch");
    if (keys.py.ndim != 1 || !PyBuffer_IsContiguous(&keys.py, 'A'))
        throw std::invalid_argument("Keys must be placed in a continuous 1 dimensional array");
    if (keys.py.strides[0] != sizeof(ukv_key_t))
        throw std::invalid_argument("Keys can't be strided");
    ukv_size_t const tasks_count = static_cast<ukv_size_t>(keys.py.len / keys.py.itemsize);
    ukv_key_t const* keys_ptr = reinterpret_cast<ukv_key_t const*>(keys.py.buf);
    ukv_val_ptr_t const* values_ptr = reinterpret_cast<ukv_val_ptr_t const*>(&values.py.buf);

    // TODO: if matrix contains bytes (not characters), use the full length
    // TODO: support non-continuous buffers and lists
    // Pairs should become: <col_key_field_t, val_arg_t>
    // If we can't cast to lists or buffers, we can use iterators:
    //   Lists: PyList_Check, PyList_Size, PyList_GetItem
    //   Tuples: https://docs.python.org/3/c-api/tuple.html
    //   Iterators: PyIter_Check, PyIter_Next: https://docs.python.org/3/c-api/iter.html
    // On every value (in non-tensor case) we should check, what kind of sequence it is:
    //   Bytes: PyByteArray_Size, PyByteArray_AsString:  https://docs.python.org/3/c-api/bytearray.html
    //   Unicode Strings: https://docs.python.org/3/c-api/unicode.html#unicode-type
    // Mappings could be sliced into keys and values, but it shouldn't be faster,
    // than doing it natively in Python. Raw `dict`s, however, provide a nice
    // iterator through CPython API: https://docs.python.org/3/c-api/dict.html#c.PyDict_Next
    // https://docs.python.org/3/c-api/mapping.html#c.PyMapping_Keys
    std::vector<std::pair<ukv_val_len_t, ukv_val_len_t>> offsets(tasks_count);
    ukv_val_len_t max_size = values.py.itemsize;

    for (size_t i = 0; i < tasks_count; ++i) {
        ukv_val_len_t off = max_size * i;
        ukv_val_len_t slen = std::strlen(reinterpret_cast<char const*>(*values_ptr + off));
        offsets[i] = std::make_pair(off, std::min<ukv_val_len_t>(slen, max_size));
    }

    ukv_options_t options = ukv_options_default_k;
    status_t status;

    [[maybe_unused]] py::gil_scoped_release release;
    ukv_write( //
        db_ptr,
        txn_ptr,
        tasks_count,
        &collection_ptr,
        0,
        keys_ptr,
        sizeof(ukv_key_t),
        values_ptr,
        0,
        &offsets[0].first,
        sizeof(ukv_val_len_t) * 2,
        &offsets[0].second,
        sizeof(ukv_val_len_t) * 2,
        options,
        arena.member_ptr(),
        status.member_ptr());

    status.throw_unhandled();
}

std::optional<py::tuple> scan( //
    ukv_t db_ptr,
    ukv_txn_t txn_ptr,
    ukv_collection_t collection_ptr,
    arena_t& arena,
    ukv_key_t min_key,
    ukv_size_t scan_length) {

    ukv_key_t* found_keys = nullptr;
    ukv_val_len_t* found_lengths = nullptr;
    ukv_options_t options = ukv_option_read_lengths_k;
    status_t status;

    ukv_scan( //
        db_ptr,
        txn_ptr,
        1,
        &collection_ptr,
        0,
        &min_key,
        0,
        &scan_length,
        0,
        options,
        &found_keys,
        &found_lengths,
        arena.member_ptr(),
        status.member_ptr());

    status.throw_unhandled();
    return py::make_tuple(py::array_t<ukv_key_t>(scan_length, found_keys),
                          py::array_t<ukv_val_len_t>(scan_length, found_lengths));
}

py::object punned_collection( //
    py_db_t* py_db_ptr,
    py_txn_t* py_txn_ptr,
    std::string const& collection,
    ukv_format_t format) {

    db_t& db = py_db_ptr->native;
    txn_t& txn = py_txn_ptr->native;

    auto maybe_col = !py_txn_ptr ? db.collection(collection.c_str()) : txn.collection(collection.c_str());
    maybe_col.throw_unhandled();
    maybe_col->as(format);

    if (format == ukv_format_graph_k) {
        auto py_graph = std::make_shared<py_graph_t>();
        py_graph->db_ptr = py_db_ptr->shared_from_this();
        py_graph->index = *std::move(maybe_col);
        return py::cast(py_graph);
    }
    else {
        auto py_col = std::make_shared<py_col_t>();
        py_col->name = collection;
        py_col->db_ptr = py_db_ptr->shared_from_this();
        py_col->native = *std::move(maybe_col);
        return py::cast(py_col);
    }
}

void ukv::wrap_database(py::module& m) {

    // Define our primary classes: `DataBase`, `Collection`, `Transaction`
    auto py_db = py::class_<py_db_t, std::shared_ptr<py_db_t>>(m, "DataBase", py::module_local());
    auto py_col = py::class_<py_col_t, std::shared_ptr<py_col_t>>(m, "Collection", py::module_local());
    auto py_txn = py::class_<py_txn_t, std::shared_ptr<py_txn_t>>(m, "Transaction", py::module_local());

    py::enum_<ukv_format_t>(m, "Format", py::module_local())
        .value("Binary", ukv_format_binary_k)
        .value("Graph", ukv_format_graph_k)
        .value("MsgPack", ukv_format_msgpack_k)
        .value("JSON", ukv_format_json_k)
        .value("BSON", ukv_format_bson_k)
        .value("CBOR", ukv_format_cbor_k)
        .value("UBJSON", ukv_format_ubjson_k);

    // Define `DataBase`
    py_db.def( //
        py::init([](std::string const& config, bool open) {
            db_t db;
            if (open)
                db.open(config).throw_unhandled();
            return std::make_shared<py_db_t>(std::move(db), config);
        }),
        py::arg("config") = "",
        py::arg("open") = true);

    py_db.def(
        "get",
        [](py_db_t& py_db, ukv_key_t key) {
            return get_item(py_db.native, nullptr, ukv_default_collection_k, py_db.arena, key);
        },
        py::arg("key"));

    py_db.def(
        "get",
        [](py_db_t& py_db, std::string const& collection, ukv_key_t key) {
            auto maybe_col = py_db.native.collection(collection.c_str());
            maybe_col.throw_unhandled();
            return get_item(py_db.native, nullptr, *maybe_col, py_db.arena, key);
        },
        py::arg("collection"),
        py::arg("key"));

    py_db.def(
        "set",
        [](py_db_t& py_db, ukv_key_t key, py::bytes const& value) {
            return set_item(py_db.native, nullptr, ukv_default_collection_k, py_db.arena, key, &value);
        },
        py::arg("key"),
        py::arg("value"));

    py_db.def(
        "set",
        [](py_db_t& py_db, std::string const& collection, ukv_key_t key, py::bytes const& value) {
            auto maybe_col = py_db.native.collection(collection.c_str());
            maybe_col.throw_unhandled();
            return set_item(py_db.native, nullptr, *maybe_col, py_db.arena, key, &value);
        },
        py::arg("collection"),
        py::arg("key"),
        py::arg("value"));

    py_db.def(
        "set",
        [](py_db_t& py_db, py::handle keys, py::handle values) {
            return set_item(py_db.native, nullptr, ukv_default_collection_k, py_db.arena, keys, values);
        },
        py::arg("keys"),
        py::arg("values"));

    py_db.def(
        "set",
        [](py_db_t& py_db, std::string const& collection, py::handle keys, py::handle values) {
            auto maybe_col = py_db.native.collection(collection.c_str());
            maybe_col.throw_unhandled();
            return set_item(py_db.native, nullptr, *maybe_col, py_db.arena, keys, values);
        },
        py::arg("collection"),
        py::arg("keys"),
        py::arg("values"));
    py_col.def("clear", [](py_db_t& py_db) {
        // TODO:
    });

    py_db.def("scan", [](py_db_t& py_db, ukv_key_t min_key, ukv_size_t length) {
        return scan(py_db.native, nullptr, ukv_default_collection_k, py_db.arena, min_key, length);
    });

    // Define `Collection`s member method, without defining any external constructors
    py_col.def(
        "get",
        [](py_col_t& py_col, ukv_key_t key) {
            return get_item(py_col.db_ptr->native,
                            py_col.txn_ptr ? py_col.txn_ptr->native : ukv_txn_t(nullptr),
                            py_col.native,
                            py_col.txn_ptr ? py_col.txn_ptr->arena : py_col.db_ptr->arena,
                            key);
        },
        py::arg("key"));

    py_col.def(
        "set",
        [](py_col_t& py_col, ukv_key_t key, py::bytes const& value) {
            return set_item(py_col.db_ptr->native,
                            py_col.txn_ptr ? py_col.txn_ptr->native : ukv_txn_t(nullptr),
                            py_col.native,
                            py_col.txn_ptr ? py_col.txn_ptr->arena : py_col.db_ptr->arena,
                            key,
                            &value);
        },
        py::arg("key"),
        py::arg("value"));

    py_col.def("clear", [](py_col_t& py_col) {
        db_t& db = py_col.db_ptr->native;
        db.remove(py_col.name.c_str()).throw_unhandled();
        auto maybe_col = db.collection(py_col.name.c_str());
        maybe_col.throw_unhandled();
        py_col.native = *std::move(maybe_col);
    });

    py_col.def(
        "set",
        [](py_col_t& py_col, py::handle keys, py::handle values) {
            return set_item(py_col.db_ptr->native,
                            py_col.txn_ptr ? py_col.txn_ptr->native : ukv_txn_t(nullptr),
                            py_col.native,
                            py_col.txn_ptr ? py_col.txn_ptr->arena : py_col.db_ptr->arena,
                            keys,
                            values);
        },
        py::arg("keys"),
        py::arg("values"));

    py_col.def("scan", [](py_col_t& py_col, ukv_key_t min_key, ukv_size_t length) {
        return scan(py_col.db_ptr->native,
                    py_col.txn_ptr ? py_col.txn_ptr->native : ukv_txn_t(nullptr),
                    py_col.native,
                    py_col.txn_ptr ? py_col.txn_ptr->arena : py_col.db_ptr->arena,
                    min_key,
                    length);
    });

    // `Transaction`:
    py_txn.def( //
        py::init([](py_db_t& py_db, bool begin) {
            auto db_ptr = py_db.shared_from_this();
            auto maybe_txn = py_db.native.transact();
            maybe_txn.throw_unhandled();
            return std::make_shared<py_txn_t>(std::move(db_ptr), *std::move(maybe_txn));
        }),
        py::arg("db"),
        py::arg("begin") = true);
    py_txn.def(
        "get",
        [](py_txn_t& py_txn, ukv_key_t key) {
            return get_item(py_txn.db_ptr->native, py_txn.native, ukv_default_collection_k, py_txn.arena, key);
        },
        py::arg("key"));

    py_txn.def(
        "get",
        [](py_txn_t& py_txn, std::string const& collection, ukv_key_t key) {
            auto maybe_col = py_txn.db_ptr->native.collection(collection.c_str());
            maybe_col.throw_unhandled();
            return get_item(py_txn.db_ptr->native, py_txn.native, *maybe_col, py_txn.arena, key);
        },
        py::arg("collection"),
        py::arg("key"));

    py_txn.def(
        "set",
        [](py_txn_t& py_txn, ukv_key_t key, py::bytes const& value) {
            return set_item(py_txn.db_ptr->native, py_txn.native, ukv_default_collection_k, py_txn.arena, key, &value);
        },
        py::arg("key"),
        py::arg("value"));

    py_txn.def(
        "set",
        [](py_txn_t& py_txn, std::string const& collection, ukv_key_t key, py::bytes const& value) {
            auto maybe_col = py_txn.db_ptr->native.collection(collection.c_str());
            maybe_col.throw_unhandled();
            return set_item(py_txn.db_ptr->native, py_txn.native, *maybe_col, py_txn.arena, key, &value);
        },
        py::arg("collection"),
        py::arg("key"),
        py::arg("value"));

    py_txn.def(
        "set",
        [](py_txn_t& py_txn, py::handle keys, py::handle values) {
            return set_item(py_txn.db_ptr->native, py_txn.native, ukv_default_collection_k, py_txn.arena, keys, values);
        },
        py::arg("keys"),
        py::arg("values"));

    py_txn.def(
        "set",
        [](py_txn_t& py_txn, std::string const& collection, py::handle keys, py::handle values) {
            auto maybe_col = py_txn.db_ptr->native.collection(collection.c_str());
            maybe_col.throw_unhandled();
            return set_item(py_txn.db_ptr->native, py_txn.native, *maybe_col, py_txn.arena, keys, values);
        },
        py::arg("collection"),
        py::arg("keys"),
        py::arg("values"));

    // Resource management
    py_txn.def("__enter__", &begin_if_needed);
    py_txn.def("commit", &commit_txn);

    py_db.def("__enter__", [](py_db_t& py_db) {
        if (!py_db.native)
            py_db.native.open(py_db.config).throw_unhandled();
        return py_db.shared_from_this();
    });
    py_db.def("close", [](py_db_t& py_db) { py_db.native.close(); });

    py_db.def( //
        "__exit__",
        [](py_db_t& py_db, py::object const& exc_type, py::object const& exc_value, py::object const& traceback) {
            py_db.native.close();
            return false;
        });
    py_txn.def(
        "__exit__",
        [](py_txn_t& py_txn, py::object const& exc_type, py::object const& exc_value, py::object const& traceback) {
            try {
                commit_txn(py_txn);
            }
            catch (...) {
                // We must now propagate this exception upwards:
                // https://stackoverflow.com/a/35483461
                // https://gist.github.com/YannickJadoul/f1fc8db711ed980cf02610277af058e4
                // https://github.com/pybind/pybind11/commit/5a7d17ff16a01436f7228a688c62511ab8c3efde
            }
            return false;
        });

    // Operator overloads used to edit entries
    py_db.def("__contains__", [](py_db_t& py_db, ukv_key_t key) {
        return contains_item(py_db.native, nullptr, ukv_default_collection_k, py_db.arena, key);
    });
    py_db.def("__getitem__", [](py_db_t& py_db, ukv_key_t key) {
        return get_item(py_db.native, nullptr, ukv_default_collection_k, py_db.arena, key);
    });
    py_db.def("__setitem__", [](py_db_t& py_db, ukv_key_t key, py::bytes const& value) {
        return set_item(py_db.native, nullptr, ukv_default_collection_k, py_db.arena, key, &value);
    });
    py_db.def("__delitem__", [](py_db_t& py_db, ukv_key_t key) {
        return set_item(py_db.native, nullptr, ukv_default_collection_k, py_db.arena, key);
    });
    py_db.def("__setitem__", [](py_db_t& py_db, py::handle keys, py::handle values) {
        return set_item(py_db.native, nullptr, ukv_default_collection_k, py_db.arena, keys, values);
    });

    py_txn.def("__contains__", [](py_txn_t& py_txn, ukv_key_t key) {
        return contains_item(py_txn.db_ptr->native, py_txn.native, ukv_default_collection_k, py_txn.arena, key);
    });
    py_txn.def("__getitem__", [](py_txn_t& py_txn, ukv_key_t key) {
        return get_item(py_txn.db_ptr->native, py_txn.native, ukv_default_collection_k, py_txn.arena, key);
    });
    py_txn.def("__setitem__", [](py_txn_t& py_txn, ukv_key_t key, py::bytes const& value) {
        return set_item(py_txn.db_ptr->native, py_txn.native, ukv_default_collection_k, py_txn.arena, key, &value);
    });
    py_txn.def("__delitem__", [](py_txn_t& py_txn, ukv_key_t key) {
        return set_item(py_txn.db_ptr->native, py_txn.native, ukv_default_collection_k, py_txn.arena, key);
    });
    py_txn.def("__setitem__", [](py_txn_t& py_txn, py::handle keys, py::handle values) {
        return set_item(py_txn.db_ptr->native, py_txn.native, ukv_default_collection_k, py_txn.arena, keys, values);
    });

    py_col.def("__contains__", [](py_col_t& py_col, ukv_key_t key) {
        return contains_item(py_col.db_ptr->native,
                             py_col.txn_ptr ? py_col.txn_ptr->native : ukv_txn_t(nullptr),
                             py_col.native,
                             py_col.txn_ptr ? py_col.txn_ptr->arena : py_col.db_ptr->arena,
                             key);
    });
    py_col.def("__getitem__", [](py_col_t& py_col, ukv_key_t key) {
        return get_item(py_col.db_ptr->native,
                        py_col.txn_ptr ? py_col.txn_ptr->native : ukv_txn_t(nullptr),
                        py_col.native,
                        py_col.txn_ptr ? py_col.txn_ptr->arena : py_col.db_ptr->arena,
                        key);
    });
    py_col.def("__setitem__", [](py_col_t& py_col, ukv_key_t key, py::bytes const& value) {
        return set_item(py_col.db_ptr->native,
                        py_col.txn_ptr ? py_col.txn_ptr->native : ukv_txn_t(nullptr),
                        py_col.native,
                        py_col.txn_ptr ? py_col.txn_ptr->arena : py_col.db_ptr->arena,
                        key,
                        &value);
    });
    py_col.def("__delitem__", [](py_col_t& py_col, ukv_key_t key) {
        return set_item(py_col.db_ptr->native,
                        py_col.txn_ptr ? py_col.txn_ptr->native : ukv_txn_t(nullptr),
                        py_col.native,
                        py_col.txn_ptr ? py_col.txn_ptr->arena : py_col.db_ptr->arena,
                        key);
    });
    py_col.def("__setitem__", [](py_col_t& py_col, py::handle keys, py::handle values) {
        return set_item(py_col.db_ptr->native,
                        py_col.txn_ptr ? py_col.txn_ptr->native : ukv_txn_t(nullptr),
                        py_col.native,
                        py_col.txn_ptr ? py_col.txn_ptr->arena : py_col.db_ptr->arena,
                        keys,
                        values);
    });
    // Operator overloads used to access collections
    py_db.def(
        "__contains__",
        [](py_db_t& py_db, std::string const& collection) {
            auto maybe = py_db.native.contains(collection.c_str());
            maybe.throw_unhandled();
            return *maybe;
        },
        py::arg());
    py_db.def(
        "__getitem__",
        [](py_db_t& py_db, std::string const& collection, ukv_format_t format) -> py::object {
            return punned_collection(&py_db, nullptr, collection, format);
        },
        py::arg(),
        py::arg("format") = ukv_format_binary_k);
    py_txn.def(
        "__getitem__",
        [](py_txn_t& py_txn, std::string const& collection, ukv_format_t format) {
            return punned_collection(py_txn.db_ptr.get(), &py_txn, collection, format);
        },
        py::arg(),
        py::arg("format") = ukv_format_binary_k);
    py_db.def("__delitem__", [](py_db_t& py_db, std::string const& collection) { //
        py_db.native.remove(collection.c_str()).throw_unhandled();
    });

    // Batch Matrix Operations
    py_db.def(
        "fill_tensor",
        [](py_db_t& py_db,
           py::handle keys,
           py::handle values,
           py::handle values_lengths,
           std::uint8_t padding_char = 0) {
            return fill_tensor(py_db.native,
                               nullptr,
                               ukv_default_collection_k,
                               py_db.arena,
                               keys,
                               values,
                               values_lengths,
                               padding_char);
        },
        py::arg("keys"),
        py::arg("values"),
        py::arg("values_lengths"),
        py::arg("padding") = 0);
    py_col.def(
        "fill_tensor",
        [](py_col_t& py_col,
           py::handle keys,
           py::handle values,
           py::handle values_lengths,
           std::uint8_t padding_char = 0) {
            return fill_tensor(py_col.db_ptr->native,
                               py_col.txn_ptr ? py_col.txn_ptr->native : ukv_txn_t(nullptr),
                               py_col.native,
                               py_col.txn_ptr ? py_col.txn_ptr->arena : py_col.db_ptr->arena,
                               keys,
                               values,
                               values_lengths,
                               padding_char);
        },
        py::arg("keys"),
        py::arg("values"),
        py::arg("values_lengths"),
        py::arg("padding") = 0);
    py_txn.def(
        "fill_tensor",
        [](py_txn_t& py_txn,
           py::handle keys,
           py::handle values,
           py::handle values_lengths,
           std::uint8_t padding_char = 0) {
            return fill_tensor(py_txn.db_ptr->native,
                               py_txn.native,
                               ukv_default_collection_k,
                               py_txn.arena,
                               keys,
                               values,
                               values_lengths,
                               padding_char);
        },
        py::arg("keys"),
        py::arg("values"),
        py::arg("values_lengths"),
        py::arg("padding") = 0);
}
