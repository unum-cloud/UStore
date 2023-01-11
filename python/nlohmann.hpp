/**
 * @brief Unlike @see "pybind11/nlohmann.hpp" provides a more fine-grained
 * control over how the conversion is implemented.
 *
 * TODO: Revert to this: https://github.com/ashvardanian/ukv/commit/6d6df13188efb0ad12d67c96ce2904fda6c838d0
 */
#pragma once
#include <fmt/core.h>
#include <pybind11/pybind11.h>
#include <nlohmann/json.hpp>

namespace unum::ukv::pyb {

namespace py = pybind11;
using json_t = nlohmann::json;

inline static constexpr char int_to_hex_k[16] = {
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

inline void char_to_hex(uint8_t const c, uint8_t* hex) noexcept {
    hex[0] = int_to_hex_k[c >> 4];
    hex[1] = int_to_hex_k[c & 0x0F];
}

PyObject* from_json(nlohmann::json const& js) {
    if (js.is_null())
        return Py_None;
    else if (js.is_boolean())
        return js.get<bool>() ? Py_True : Py_False;
    else if (js.is_number_integer())
        return PyLong_FromLong(js.get<nlohmann::json::number_integer_t>());
    else if (js.is_number_float())
        return PyFloat_FromDouble(js.get<double>());
    else if (js.is_string()) {
        auto str = js.get<nlohmann::json::string_t>();
        return PyUnicode_FromStringAndSize(str.c_str(), str.size());
    }
    else if (js.is_array()) {
        PyObject* obj = PyList_New(js.size());
        for (size_t i = 0; i < js.size(); i++)
            PyList_SetItem(obj, i, from_json(js[i]));
        return obj;
    }
    else // Object
    {
        PyObject* obj = PyDict_New();
        for (nlohmann::json::const_iterator it = js.cbegin(); it != js.cend(); ++it)
            PyDict_SetItem(obj, PyUnicode_FromStringAndSize(it.key().c_str(), it.key().size()), from_json(it.value()));
        return obj;
    }
}

inline void to_string(PyObject* obj, std::string& output) {
    if (obj == Py_None)
        output += "null";
    else if (PyBool_Check(obj))
        output += obj == Py_False ? "false" : "true";
    else if (PyLong_Check(obj))
        fmt::format_to(std::back_inserter(output), "{}", PyLong_AsLong(obj));
    else if (PyFloat_Check(obj))
        fmt::format_to(std::back_inserter(output), "{}", PyFloat_AsDouble(obj));
    else if (PyBytes_Check(obj)) {
        output.reserve(output.size() + PyBytes_Size(obj) + 2);
        output += "\"";
        output += PyBytes_AsString(obj);
        output += "\"";
    }
    else if (PyUnicode_Check(obj)) {
        Py_ssize_t size;
        const char* char_ptr = PyUnicode_AsUTF8AndSize(obj, &size);
        output.reserve(output.size() + size + 2);
        output += "\"";
        for (std::size_t i = 0; i != size; ++i) {
            uint8_t c = char_ptr[i];
            switch (c) {
            case 34: output += "\\\""; break;
            case 92: output += "\\\\"; break;
            case 8: output += "\\b"; break;
            case 9: output += "\\t"; break;
            case 10: output += "\\n"; break;
            case 12: output += "\\f"; break;
            case 13: output += "\\r"; break;
            case 0:
            case 1:
            case 2:
            case 3:
            case 4:
            case 5:
            case 6:
            case 7:
            case 11:
            case 14:
            case 15:
            case 16:
            case 17:
            case 18:
            case 19:
            case 20:
            case 21:
            case 22:
            case 23:
            case 24:
            case 25:
            case 26:
            case 27:
            case 28:
            case 29:
            case 30:
            case 31: {
                output += "\\u0000";
                auto target_ptr = reinterpret_cast<uint8_t*>(output.data() + output.size() - 2);
                char_to_hex(c, target_ptr);
                break;
            }
            default: output += char_ptr[i];
            }
        }
        output += "\"";
    }
    else if (PySequence_Check(obj)) {
        output += "[";
        if (!PySequence_Length(obj))
            output += ']';
        else {
            for (Py_ssize_t i = 0; i < PySequence_Length(obj); i++) {
                to_string(PySequence_GetItem(obj, i), output);
                output += ",";
            }
            output[output.size() - 1] = ']';
        }
    }
    else if (PyDict_Check(obj)) {
        output += "{";
        if (!PyDict_Size(obj))
            output += "}";
        else {
            PyObject *key, *value;
            Py_ssize_t pos = 0;
            while (PyDict_Next(obj, &pos, &key, &value)) {
                to_string(key, output);
                output += ":";
                to_string(value, output);
                output += ",";
            }
            output[output.size() - 1] = '}';
        }
    }
    else
        throw std::runtime_error("invalid type for conversion.");
}

} // namespace unum::ukv::pyb