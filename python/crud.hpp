
#pragma once
#include <vector> // `std::vector`

#include <pybind11/pybind11.h> // `gil_scoped_release`
#include <Python.h>            // `PyObject`
#include <arrow/array.h>
#include <arrow/python/pyarrow.h>

#include "ukv/ukv.h"
#include "cast_args.hpp"

namespace unum::ukv::pyb {

struct py_bin_req_t {
    ukv_key_t key {ukv_key_unknown_k};
    ukv_str_view_t field {nullptr};
    ukv_bytes_ptr_t ptr {nullptr};
    ukv_length_t off {0};
    ukv_length_t len {0};
};

#pragma region Writes

/**
 * @param key_py Must be a `PyLong`.
 * @param val_py Can be anything.
 */
template <typename collection_at>
static void write_one_binary(py_collection_gt<collection_at>& collection, PyObject* key_py, PyObject* val_py) {

    status_t status;
    ukv_key_t key = py_to_scalar<ukv_key_t>(key_py);
    value_view_t val = py_to_bytes(val_py);

    [[maybe_unused]] py::gil_scoped_release release;

    ukv_write_t write {
        .db = collection.db(),
        .error = status.member_ptr(),
        .transaction = collection.txn(),
        .arena = collection.member_arena(),
        .options = collection.options(),
        .tasks_count = 1,
        .collections = collection.member_collection(),
        .keys = &key,
        .lengths = val.member_length(),
        .values = val.member_ptr(),
    };

    ukv_write(&write);
    status.throw_unhandled();
}

template <typename collection_at>
static void write_many_binaries(py_collection_gt<collection_at>& collection, PyObject* keys_py, PyObject* vals_py) {

    status_t status;
    parsed_places_t parsed_places {keys_py, collection.native};
    places_arg_t places = parsed_places;
    parsed_contents_t parsed_contents {vals_py};
    contents_arg_t contents = parsed_contents;

    [[maybe_unused]] py::gil_scoped_release release;

    ukv_write_t write {
        .db = collection.db(),
        .error = status.member_ptr(),
        .transaction = collection.txn(),
        .arena = collection.member_arena(),
        .options = collection.options(),
        .tasks_count = places.count,
        .collections = collection.member_collection(),
        .keys = places.keys_begin.get(),
        .keys_stride = places.keys_begin.stride(),
        .presences = contents.presences_begin.get(),
        .offsets = contents.offsets_begin.get(),
        .offsets_stride = contents.offsets_begin.stride(),
        .lengths = contents.lengths_begin.get(),
        .lengths_stride = contents.lengths_begin.stride(),
        .values = contents.contents_begin.get(),
        .values_stride = contents.contents_begin.stride(),
    };

    ukv_write(&write);

    status.throw_unhandled();
}

static void broadcast_binary(py_blobs_collection_t& collection, py::object keys_py, py::object val_py) {

    status_t status;
    parsed_places_t parsed_places {keys_py.ptr(), collection.native};
    places_arg_t places = parsed_places;
    value_view_t val = py_to_bytes(val_py.ptr());

    [[maybe_unused]] py::gil_scoped_release release;

    ukv_write_t write {
        .db = collection.db(),
        .error = status.member_ptr(),
        .transaction = collection.txn(),
        .arena = collection.member_arena(),
        .options = collection.options(),
        .tasks_count = places.count,
        .collections = collection.member_collection(),
        .keys = places.keys_begin.get(),
        .keys_stride = places.keys_begin.stride(),
        .lengths = val.member_length(),
        .values = val.member_ptr(),
    };

    ukv_write(&write);

    status.throw_unhandled();
}

#pragma region Reads

template <typename collection_at>
static py::object has_one_binary(py_collection_gt<collection_at>& collection, PyObject* key_py) {

    status_t status;
    ukv_key_t key = py_to_scalar<ukv_key_t>(key_py);
    ukv_octet_t* found_presences = nullptr;

    {
        [[maybe_unused]] py::gil_scoped_release release;
        ukv_read_t read {
            .db = collection.db(),
            .error = status.member_ptr(),
            .transaction = collection.txn(),
            .arena = collection.member_arena(),
            .options = collection.options(),
            .tasks_count = 1,
            .collections = collection.member_collection(),
            .keys = &key,
            .presences = &found_presences,
        };

        ukv_read(&read);
        status.throw_unhandled();
    }

    // Exporting Arrow objects here would be over-engineering:
    // if (collection.export_into_arrow()) {
    //     auto shared = std::make_shared<arrow::BooleanScalar>(presences[0]);
    //     PyObject* obj_ptr = arrow::py::wrap_scalar(std::static_pointer_cast<arrow::Scalar>(shared));
    //     return py::reinterpret_steal<py::object>(obj_ptr);
    // }
    bits_span_t presences {found_presences};
    PyObject* obj_ptr = presences[0] ? Py_True : Py_False;
    return py::reinterpret_borrow<py::object>(obj_ptr);
}

static py::object read_one_binary(py_blobs_collection_t& collection, PyObject* key_py) {

    status_t status;
    ukv_key_t key = py_to_scalar<ukv_key_t>(key_py);
    ukv_length_t* found_lengths = nullptr;
    ukv_bytes_ptr_t found_values = nullptr;

    {
        [[maybe_unused]] py::gil_scoped_release release;
        ukv_read_t read {
            .db = collection.db(),
            .error = status.member_ptr(),
            .transaction = collection.txn(),
            .arena = collection.member_arena(),
            .options = collection.options(),
            .tasks_count = 1,
            .collections = collection.member_collection(),
            .keys = &key,
            .lengths = &found_lengths,
            .values = &found_values,
        };
        ukv_read(&read);
        status.throw_unhandled();
    }

    // To fetch data without copies, there is a hacky way:
    // https://github.com/pybind/pybind11/issues/1236#issuecomment-527730864
    // But in that case we can't guarantee memory alignment, so doing a copy
    // is hard to avoid in Python.
    // https://github.com/pybind/pybind11/blob/a05bc3d2359d12840ef2329d68f613f1a7df9c5d/include/pybind11/pytypes.h#L1474
    // https://docs.python.org/3/c-api/bytes.html
    // https://github.com/python/cpython/blob/main/Objects/bytesobject.c
    value_view_t val {found_values, found_lengths[0]};
    PyObject* obj_ptr = val ? PyBytes_FromStringAndSize(val.c_str(), val.size()) : Py_None;
    return py::reinterpret_borrow<py::object>(obj_ptr);
}

template <typename collection_at>
static py::object has_many_binaries(py_collection_gt<collection_at>& collection, PyObject* keys_py) {

    status_t status;
    ukv_octet_t* found_presences = nullptr;

    parsed_places_t parsed_places {keys_py, collection.native};
    places_arg_t places = parsed_places;

    {
        [[maybe_unused]] py::gil_scoped_release release;
        ukv_read_t read {
            .db = collection.db(),
            .error = status.member_ptr(),
            .transaction = collection.txn(),
            .arena = collection.member_arena(),
            .options = collection.options(),
            .tasks_count = places.count,
            .collections = collection.member_collection(),
            .keys = places.keys_begin.get(),
            .keys_stride = places.keys_begin.stride(),
            .presences = &found_presences,
        };

        ukv_read(&read);
        status.throw_unhandled();
    }

    bits_span_t presences {found_presences};
    PyObject* tuple_ptr = PyTuple_New(places.size());
    for (std::size_t i = 0; i != places.size(); ++i) {
        PyObject* obj_ptr = presences[i] ? Py_True : Py_False;
        PyTuple_SetItem(tuple_ptr, i, obj_ptr);
    }
    return py::reinterpret_steal<py::object>(tuple_ptr);
}

static py::object read_many_binaries(py_blobs_collection_t& collection, PyObject* keys_py) {

    status_t status;
    ukv_octet_t* found_presences = nullptr;
    ukv_length_t* found_offsets = nullptr;
    ukv_length_t* found_lengths = nullptr;
    ukv_bytes_ptr_t found_values = nullptr;
    bool const export_arrow = collection.export_into_arrow();

    parsed_places_t parsed_places {keys_py, collection.native};
    places_arg_t places = parsed_places;

    {
        [[maybe_unused]] py::gil_scoped_release release;
        ukv_read_t read {
            .db = collection.db(),
            .error = status.member_ptr(),
            .transaction = collection.txn(),
            .arena = collection.member_arena(),
            .options = collection.options(),
            .tasks_count = places.count,
            .collections = collection.member_collection(),
            .collections_stride = 0,
            .keys = places.keys_begin.get(),
            .keys_stride = places.keys_begin.stride(),
            .presences = export_arrow ? &found_presences : nullptr,
            .offsets = &found_offsets,
            .lengths = !export_arrow ? &found_lengths : nullptr,
            .values = &found_values,
        };

        ukv_read(&read);
        status.throw_unhandled();
    }

    if (export_arrow) {
        auto shared_length = static_cast<int64_t>(places.count);
        auto shared_offsets = std::make_shared<arrow::Buffer>( //
            reinterpret_cast<uint8_t*>(found_offsets),
            (shared_length + 1) * sizeof(ukv_length_t));
        auto shared_data = std::make_shared<arrow::Buffer>( //
            reinterpret_cast<uint8_t*>(found_values),
            static_cast<int64_t>(found_offsets[places.count]));
        auto shared_bitmap = std::make_shared<arrow::Buffer>( //
            reinterpret_cast<uint8_t*>(found_presences),
            divide_round_up<int64_t>(shared_length, CHAR_BIT));
        auto shared = std::make_shared<arrow::BinaryArray>(shared_length, shared_offsets, shared_data, shared_bitmap);
        PyObject* obj_ptr = arrow::py::wrap_array(std::static_pointer_cast<arrow::Array>(shared));
        return py::reinterpret_steal<py::object>(obj_ptr);
    }
    else {
        embedded_blobs_t bins {places.size(), found_offsets, found_lengths, found_values};
        PyObject* tuple_ptr = PyTuple_New(places.size());
        for (std::size_t i = 0; i != places.size(); ++i) {
            value_view_t val = bins[i];
            PyObject* obj_ptr = val ? PyBytes_FromStringAndSize(val.c_str(), val.size()) : Py_None;
            PyTuple_SetItem(tuple_ptr, i, obj_ptr);
        }
        return py::reinterpret_steal<py::object>(tuple_ptr);
    }
}

template <typename collection_at>
static py::object has_binary(py_collection_gt<collection_at>& collection, py::object key_py) {
    auto is_single = !PySequence_Check(key_py.ptr());
    auto func = is_single ? &has_one_binary<collection_at> : &has_many_binaries<collection_at>;
    return func(collection, key_py.ptr());
}

static py::object read_binary(py_blobs_collection_t& collection, py::object key_py) {
    auto is_single = !PySequence_Check(key_py.ptr());
    auto func = is_single ? &read_one_binary : &read_many_binaries;
    return func(collection, key_py.ptr());
}

template <typename collection_at>
static void write_binary(py_collection_gt<collection_at>& collection, py::object key_py, py::object val_py) {
    auto is_single = !PySequence_Check(key_py.ptr());
    auto func = is_single ? &write_one_binary<collection_at> : &write_many_binaries<collection_at>;
    return func(collection, key_py.ptr(), val_py.ptr());
}

template <typename collection_at>
static void remove_binary(py_collection_gt<collection_at>& collection, py::object key_py) {
    auto is_single = !PySequence_Check(key_py.ptr());
    auto func = is_single ? &write_one_binary<collection_at> : &write_many_binaries<collection_at>;
    return func(collection, key_py.ptr(), Py_None);
}

static void update_binary(py_blobs_collection_t& collection, py::object dict_py) {
    status_t status;
    ukv_size_t step = sizeof(py_bin_req_t);

    std::vector<py_bin_req_t> keys;
    keys.reserve(PyDict_Size(dict_py.ptr()));

    std::size_t key_idx = 0;
    py_scan_dict(dict_py.ptr(), [&](PyObject* key_obj, PyObject* val_obj) {
        auto val = py_to_bytes(val_obj);
        py_bin_req_t& req = keys[key_idx];
        req.key = py_to_scalar<ukv_key_t>(key_obj);
        req.ptr = ukv_bytes_ptr_t(val.begin());
        req.len = static_cast<ukv_length_t>(val.size());
        ++key_idx;
    });

    [[maybe_unused]] py::gil_scoped_release release;

    ukv_write_t write {
        .db = collection.db(),
        .error = status.member_ptr(),
        .transaction = collection.txn(),
        .arena = collection.member_arena(),
        .options = collection.options(),
        .tasks_count = static_cast<ukv_size_t>(keys.size()),
        .collections = collection.member_collection(),
        .keys = &keys[0].key,
        .keys_stride = step,
        .offsets = &keys[0].off,
        .offsets_stride = step,
        .lengths = &keys[0].len,
        .lengths_stride = step,
        .values = &keys[0].ptr,
        .values_stride = step,
    };

    ukv_write(&write);
    status.throw_unhandled();
}

template <typename collection_at>
static py::array_t<ukv_key_t> scan_binary( //
    py_collection_gt<collection_at>& collection,
    ukv_key_t min_key,
    ukv_length_t count_limit) {

    status_t status;
    ukv_length_t* found_lengths = nullptr;
    ukv_key_t* found_keys = nullptr;
    bool const export_arrow = collection.export_into_arrow();
    ukv_scan_t scan {
        .db = collection.db(),
        .error = status.member_ptr(),
        .transaction = collection.txn(),
        .arena = collection.member_arena(),
        .options = collection.options(),
        .tasks_count = 1,
        .collections = collection.member_collection(),
        .start_keys = &min_key,
        .count_limits = &count_limit,
        .counts = &found_lengths,
        .keys = &found_keys,
    };

    ukv_scan(&scan);

    status.throw_unhandled();

    if (export_arrow) {
        auto shared_length = static_cast<int64_t>(found_lengths[0]);
        auto shared_data = std::make_shared<arrow::Buffer>( //
            reinterpret_cast<uint8_t*>(found_keys),
            shared_length * sizeof(ukv_key_t));
        static_assert(std::is_same_v<ukv_key_t, int64_t>, "Change the following line!");
        auto shared = std::make_shared<arrow::NumericArray<arrow::Int64Type>>(shared_length, shared_data);
        PyObject* obj_ptr = arrow::py::wrap_array(std::static_pointer_cast<arrow::Array>(shared));
        return py::reinterpret_steal<py::object>(obj_ptr);
    }
    else
        return py::array_t<ukv_key_t>(found_lengths[0], found_keys);
}

template <typename collection_at>
static std::size_t get_length(py_collection_gt<collection_at>& collection) {
    return collection.native.size();
}

#if 0
/**
 * @brief Exports values into preallocated multi-dimensional NumPy-like buffers.
 *        The most performant batch-reading method, ideal for ML.
 *
 * Contrary to most data types exposed by the Python interpreter,
 * buffers are not @c PyObject pointers but rather simple C structures.
 * This allows them to be created and copied very simply.
 * When a generic wrapper around a buffer is needed, a @c memoryview
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
    ukv_database_t db_ptr,
    ukv_transaction_t txn_ptr,
    ukv_collection_t collection_ptr,
    managed_arena_t& arena,
    py::handle keys_arr,
    py::handle values_arr,
    py::handle values_lengths_arr,
    std::uint8_t padding_char = 0) {

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
    auto output_bytes_cap = static_cast<ukv_length_t>(values.py.shape[1]);

    // Validate the format of `values_lengths`
    if (values_lengths.py.ndim != 1)
        throw std::invalid_argument("Lengths tensor must have rank 1");
    if (values_lengths.py.itemsize != sizeof(ukv_length_t))
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
    ukv_length_t* found_lengths = nullptr;
    ukv_bytes_ptr_t found_values = nullptr;
    ukv_options_t options = ukv_options_default_k;
    ukv_read_t read {
        .db = db_ptr,
        .error = status.internal_cptr(),
        .transaction = txn_ptr,
        .arena = arena.internal_cptr(),
        .options = options,
        .tasks_count = tasks_count,
        .collections = &collection_ptr,
        .keys = keys_ptr,
        .keys_stride = sizeof(ukv_key_t),
        .lengths = &found_lengths,
        .values = &found_values,
    };

    ukv_read(&read);

    status.throw_unhandled();

    // Export the data into the matrix
    taped_values_view_t inputs {found_lengths, found_values, tasks_count};
    tape_iterator_t input_it = inputs.begin();
    for (ukv_size_t i = 0; i != tasks_count; ++i, ++input_it) {
        value_view_t input = *input_it;
        auto input_bytes = reinterpret_cast<std::uint8_t const*>(input.begin());
        auto input_length = static_cast<ukv_length_t const>(input.size());
        std::uint8_t* output_bytes = outputs_bytes + outputs_bytes_stride * i;
        ukv_length_t& output_length =
            *reinterpret_cast<ukv_length_t*>(outputs_lengths_bytes + outputs_lengths_bytes_stride * i);

        std::size_t count_copy = std::min(output_bytes_cap, input_length);
        std::size_t count_pads = output_bytes_cap - count_copy;
        std::memcpy(output_bytes, input_bytes, count_copy);
        std::memset(output_bytes + count_copy, padding_char, count_pads);

        output_length = count_copy;
    }
}
#endif

} // namespace unum::ukv::pyb