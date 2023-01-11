/**
 * @file cast.hpp
 * @brief Generic range-casting functions to avoid copies when exchange data.
 */
#pragma once
#include <Python.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>

#include "ukv/cpp/ranges.hpp" // `value_view_t`

namespace unum::ukv::pyb {

/**
 * @brief Defines Pythons type marker for primitive types.
 * @b All of them are only a single character long.
 */
template <typename element_at>
struct format_code_gt {};

#pragma region Zero Copy Buffer Protocol

/**
 * @brief RAII object for `Py_buffer` parsing purposes,
 * which releases the buffer in the destructor.
 */
struct py_buffer_t {
    py_buffer_t() = default;
    py_buffer_t(py_buffer_t const&) = delete;
    py_buffer_t(py_buffer_t&& other) noexcept : raw(std::exchange(other.raw, Py_buffer {})) {}
    ~py_buffer_t() { PyBuffer_Release(&raw); }

    Py_buffer raw;
};

inline py_buffer_t py_buffer(PyObject* obj, bool const_ = true) {
    auto flags = PyBUF_ANY_CONTIGUOUS | PyBUF_STRIDED | PyBUF_FORMAT;
    if (!const_)
        flags |= PyBUF_WRITABLE;

    py_buffer_t buf;
    bool initialized = PyObject_GetBuffer(obj, &buf.raw, flags) == 0;
    if (!initialized)
        throw std::invalid_argument("Couldn't obtain buffer overviews");
    if (!buf.raw.shape)
        throw std::invalid_argument("Shape wasn't inferred");
    return buf;
}

/**
 * @brief Provides a typed view of 1D potentially-strided tensor.
 * @param obj Must implement the "Buffer protocol".
 */
template <typename scalar_at>
strided_range_gt<scalar_at> py_strided_range(py_buffer_t const& buf) {

    if (buf.raw.ndim != 1)
        throw std::invalid_argument("Expecting tensor rank 1");
    if (buf.raw.itemsize != sizeof(scalar_at))
        throw std::invalid_argument("Scalar type mismatch");

    strided_range_gt<scalar_at> result {
        {reinterpret_cast<scalar_at*>(buf.raw.buf), static_cast<ukv_size_t>(buf.raw.strides[0])},
        static_cast<ukv_size_t>(buf.raw.shape[0]),
    };
    return result;
}

/**
 * @brief Provides a typed view of 2D potentially-strided tensor.
 * @param obj Must implement the "Buffer protocol".
 */
template <typename scalar_at>
strided_matrix_gt<scalar_at> py_strided_matrix(py_buffer_t const& buf) {

    if (buf.raw.ndim != 2)
        throw std::invalid_argument("Expecting tensor rank 2");
    if (buf.raw.itemsize != sizeof(scalar_at))
        throw std::invalid_argument("Scalar type mismatch");

    strided_matrix_gt<scalar_at> result {
        reinterpret_cast<scalar_at*>(buf.raw.buf),
        static_cast<ukv_size_t>(buf.raw.shape[0]),
        static_cast<ukv_size_t>(buf.raw.shape[1]),
        static_cast<ukv_size_t>(buf.raw.strides[0]),
        static_cast<ukv_size_t>(buf.raw.strides[1]),
    };
    return result;
}

#pragma region Casting Python Objects

template <typename scalar_at>
scalar_at py_to_scalar(PyObject* obj) {
    if constexpr (std::is_integral_v<scalar_at>) {
        if (PyLong_Check(obj))
            return static_cast<scalar_at>(PyLong_AsUnsignedLong(obj));
        else if (PyNumber_Check(obj))
            return static_cast<scalar_at>(PyNumber_AsSsize_t(obj, NULL));
        else if (arrow::py::is_scalar(obj)) {
            auto scalar = arrow::py::unwrap_scalar(obj).ValueOrDie()->CastTo(arrow::uint64());
            auto uint64_scalar = std::static_pointer_cast<arrow::UInt64Scalar>(scalar.ValueOrDie());
            return static_cast<scalar_at>(uint64_scalar->value);
        }

        throw std::invalid_argument("Expects integer");
        return 0;
    }
    else {
        if (PyFloat_Check(obj))
            return static_cast<scalar_at>(PyFloat_AsDouble(obj));

        throw std::invalid_argument("Expects float");
        return 0;
    }
}

inline value_view_t py_to_bytes(PyObject* obj) {

    if (PyUnicode_Check(obj)) {
        PyObject* utf_obj = PyUnicode_AsUTF8String(obj);
        return value_view_t {reinterpret_cast<byte_t*>(PyBytes_AsString(utf_obj)),
                             static_cast<size_t>(PyBytes_Size(utf_obj))};
    }
    if (PyBytes_Check(obj)) {
        char* buffer = nullptr;
        Py_ssize_t length = 0;
        PyBytes_AsStringAndSize(obj, &buffer, &length);
        return {reinterpret_cast<ukv_bytes_ptr_t>(buffer), static_cast<ukv_length_t>(length)};
    }

    if (obj == Py_None) // Means the object must be deleted
        return value_view_t {};

    throw std::invalid_argument("Value must be representable as a byte array");
    return {};
}

inline ukv_str_view_t py_to_str(PyObject* obj) {
    return py_to_bytes(obj).c_str();
}

inline bool py_is_sequence(PyObject* obj) {
    return PyTuple_Check(obj) || PyList_Check(obj) || PyIter_Check(obj);
}

inline std::optional<std::size_t> py_sequence_length(PyObject* obj) {
    if (PySequence_Check(obj))
        return PySequence_Length(obj);

    return std::nullopt;
}

/**
 * @brief Iterates over Python `tuple`, `list`, or any `iter`.
 * @param call Callback for member `PyObject`s.
 */
template <typename transform_at, typename output_iterator_at>
void py_transform_n(PyObject* obj,
                    transform_at&& transform,
                    output_iterator_at output_iterator,
                    std::size_t max_count = std::numeric_limits<std::size_t>::max()) {

    // TODO: Bring back the support of NumPy arrays
    // TODO: Add support for Arrow inputs

    if (PyTuple_Check(obj)) {
        max_count = std::min<std::size_t>(PyTuple_Size(obj), max_count);
        for (std::size_t i = 0; i != max_count; ++i, ++output_iterator)
            *output_iterator = transform(PyTuple_GetItem(obj, i));
    }
    else if (PyList_Check(obj)) {
        max_count = std::min<std::size_t>(PyList_Size(obj), max_count);
        for (std::size_t i = 0; i != max_count; ++i, ++output_iterator)
            *output_iterator = transform(PyList_GetItem(obj, i));
    }
    else if (PyIter_Check(obj)) {
        PyObject* item = nullptr;
        while ((item = PyIter_Next(obj))) {
            *output_iterator = transform(item);
            Py_DECREF(item);
            ++output_iterator;
        }
    }
    else if (PySequence_Check(obj)) {
        max_count = std::min<std::size_t>(PySequence_Size(obj), max_count);
        for (std::size_t i = 0; i != max_count; ++i, ++output_iterator)
            *output_iterator = transform(PySequence_GetItem(obj, i));
    }
    else if (obj != Py_None)
        throw std::invalid_argument("Invalid Argument!");
}

/**
 * @brief Iterates over Python `dict`-like object.
 * @param call Callback for the key and value `PyObject`s.
 * @return true If a supported iterable type was detected.
 */
template <typename member_callback_at>
void py_scan_dict(PyObject* obj, member_callback_at&& call) {

    PyObject* key = nullptr;
    PyObject* value = nullptr;
    Py_ssize_t pos = 0;

    while (PyDict_Next(obj, &pos, &key, &value))
        call(key, value);
}

inline void throw_not_implemented() {
    // https://github.com/pybind/pybind11/issues/1125#issuecomment-691552571
    throw std::runtime_error("Not Implemented!");
}

#pragma region Type Conversions Guides

// clang-format off
template <> struct format_code_gt<bool> { inline static constexpr char value[2] = "?"; };
template <> struct format_code_gt<char> { inline static constexpr char value[2] = "c"; };
template <> struct format_code_gt<int8_t> { inline static constexpr char value[2] = "b"; };
template <> struct format_code_gt<uint8_t> { inline static constexpr char value[2] = "B"; };

template <> struct format_code_gt<int16_t> { inline static constexpr char value[2] = "h"; };
template <> struct format_code_gt<uint16_t> { inline static constexpr char value[2] = "H"; };
template <> struct format_code_gt<int32_t> { inline static constexpr char value[2] = "i"; };
template <> struct format_code_gt<uint32_t> { inline static constexpr char value[2] = "I"; };
template <> struct format_code_gt<int64_t> { inline static constexpr char value[2] = "l"; };
template <> struct format_code_gt<uint64_t> { inline static constexpr char value[2] = "L"; };
template <> struct format_code_gt<long long> { inline static constexpr char value[2] = "q"; };
template <> struct format_code_gt<unsigned long long> { inline static constexpr char value[2] = "Q"; };

template <> struct format_code_gt<float> { inline static constexpr char value[2] = "f"; };
template <> struct format_code_gt<double> { inline static constexpr char value[2] = "d"; };
// clang-format on

template <typename scalar_at>
scalar_at py_cast_scalar(byte_t* data, char format) {
    switch (format) {
    case format_code_gt<int8_t>::value[0]: return *reinterpret_cast<int8_t*>(data);
    case format_code_gt<uint8_t>::value[0]: return *reinterpret_cast<uint8_t*>(data);
    case format_code_gt<int16_t>::value[0]: return *reinterpret_cast<int16_t*>(data);
    case format_code_gt<uint16_t>::value[0]: return *reinterpret_cast<uint16_t*>(data);
    case format_code_gt<int32_t>::value[0]: return *reinterpret_cast<int32_t*>(data);
    case format_code_gt<uint32_t>::value[0]: return *reinterpret_cast<uint32_t*>(data);
    case format_code_gt<int64_t>::value[0]: return *reinterpret_cast<int64_t*>(data);
    case format_code_gt<uint64_t>::value[0]: return *reinterpret_cast<uint64_t*>(data);
    case format_code_gt<long long>::value[0]: return *reinterpret_cast<long long*>(data);
    case format_code_gt<unsigned long long>::value[0]: return *reinterpret_cast<unsigned long long*>(data);

    default: throw std::invalid_argument("Cant cast this type to `ukv_key_t`");
    }
}

template <typename scalar_at>
bool can_cast_internal_scalars(py_buffer_t const& buf) {

    auto fmt_len = std::strlen(buf.raw.format);
    if (fmt_len != 1)
        throw std::invalid_argument("Unknown Python format string");

    if (buf.raw.itemsize != sizeof(scalar_at))
        return false;

    constexpr bool ii = std::is_integral_v<scalar_at>;
    constexpr bool is = std::is_signed_v<scalar_at>;

    switch (buf.raw.format[0]) {

        // Signed integral types
    case format_code_gt<short>::value[0]: return is && ii;
    case format_code_gt<int>::value[0]: return is && ii;
    case format_code_gt<long>::value[0]: return is && ii;
    case format_code_gt<long long>::value[0]:
        return is && ii;

        // Unsigned integral types
    case format_code_gt<unsigned short>::value[0]: return !is && ii;
    case format_code_gt<unsigned int>::value[0]: return !is && ii;
    case format_code_gt<unsigned long>::value[0]: return !is && ii;
    case format_code_gt<unsigned long long>::value[0]: return !is && ii;

    // Non-integral types
    case format_code_gt<float>::value[0]: return !ii;
    case format_code_gt<double>::value[0]: return !ii;

    // All following types are considered `byte_t`
    case format_code_gt<char>::value[0]: return true;
    case format_code_gt<signed char>::value[0]: return true;
    case format_code_gt<unsigned char>::value[0]: return true;

    // Special cases
    case format_code_gt<bool>::value[0]: return std::is_same_v<bool, scalar_at>;
    default: return false;
    }
}

template <typename scalar_at>
bool can_view_as_strided_range(py_buffer_t const& buf) {
    return buf.raw.ndim == 1 && can_cast_internal_scalars<scalar_at>(buf);
}

template <typename scalar_at>
bool can_view_as_strided_matrix(py_buffer_t const& buf) {
    return buf.raw.ndim == 2 && can_cast_internal_scalars<scalar_at>(buf);
}

} // namespace unum::ukv::pyb