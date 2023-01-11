#include <fmt/format.h>

#include <arrow/c/bridge.h>
#include <arrow/python/pyarrow.h>

#define ARROW_C_DATA_INTERFACE 1
#define ARROW_C_STREAM_INTERFACE 1
#include "ukv/arrow.h"

#include "pybind.hpp"
#include "crud.hpp"
#include "cast.hpp"

using namespace std::literals;

using namespace unum::ukv::pyb;
using namespace unum::ukv;
using namespace unum;

static ukv_doc_field_type_t ukv_doc_field_from_str(ukv_str_view_t type_name) {
    if (type_name == "bool"sv)
        return ukv_doc_field_bool_k;
    else if (type_name == "int8"sv)
        return ukv_doc_field_i8_k;
    else if (type_name == "int16"sv)
        return ukv_doc_field_i16_k;
    else if (type_name == "int32"sv)
        return ukv_doc_field_i32_k;
    else if (type_name == "int64"sv)
        return ukv_doc_field_i64_k;
    else if (type_name == "uint8"sv)
        return ukv_doc_field_u8_k;
    else if (type_name == "uint16"sv)
        return ukv_doc_field_u16_k;
    else if (type_name == "uint32"sv)
        return ukv_doc_field_u32_k;
    else if (type_name == "uint64"sv)
        return ukv_doc_field_u64_k;
    else if (type_name == "float16"sv)
        return ukv_doc_field_f16_k;
    else if (type_name == "float32"sv)
        return ukv_doc_field_f32_k;
    else if (type_name == "float64"sv)
        return ukv_doc_field_f64_k;
    else if (type_name == "bytes"sv)
        return ukv_doc_field_bin_k;
    else if (type_name == "str"sv)
        return ukv_doc_field_str_k;

    throw std::invalid_argument("Unknown type name");
    return ukv_doc_field_json_k;
}

void scan_rows(py_table_collection_t& df) {
    auto keys_range = df.binary.native.keys();
    auto keys_stream = keys_range.begin();
    std::vector<ukv_key_t> keys_found;
    while (!keys_stream.is_end()) {
        keys_found.insert(keys_found.end(), keys_stream.keys_batch().begin(), keys_stream.keys_batch().end());
        keys_stream.seek_to_next_batch();
    }
    df.rows_keys = std::move(keys_found);
}

void scan_rows_range(py_table_collection_t& df) {
    auto& range = std::get<py_table_keys_range_t>(df.rows_keys);
    auto keys_range = df.binary.native.keys(range.min);
    auto keys_stream = keys_range.begin();
    std::vector<ukv_key_t> keys_found;
    while (!keys_stream.is_end()) {
        auto max_key_pos =
            std::upper_bound(keys_stream.keys_batch().begin(), keys_stream.keys_batch().end(), range.max);
        keys_found.insert(keys_found.end(), keys_stream.keys_batch().begin(), max_key_pos);
        if (max_key_pos != keys_stream.keys_batch().end())
            break;
        keys_stream.seek_to_next_batch();
    }
    df.rows_keys = std::move(keys_found);
}

void correct_table(docs_table_t& table) {

    std::vector<std::size_t> binary_column_indexes;
    for (std::size_t collection_idx = 0; collection_idx != table.collections(); ++collection_idx) {
        column_view_t column = table.column(collection_idx);
        if (column.type() == ukv_doc_field_str_k || column.type() == ukv_doc_field_bin_k)
            binary_column_indexes.push_back(collection_idx);
    }

    if (binary_column_indexes.size() < 2)
        return;

    // Collecting old offsets and lengths
    std::size_t offset_index = 0;
    ukv_length_t contents_length = 0;
    std::size_t offsets_per_column = table.rows() + 1;
    std::vector<ukv_length_t> offs(binary_column_indexes.size() * offsets_per_column);
    std::vector<ukv_length_t> lens(binary_column_indexes.size() * offsets_per_column);

    for (std::size_t column_idx : binary_column_indexes) {
        column_view_t column = table.column(column_idx);
        for (ukv_length_t idx = 0; idx != column.size(); ++idx) {
            offs[offset_index] = column.offsets()[idx];
            lens[offset_index] = column.lengths()[idx];
            contents_length += column.lengths()[idx];
            ++offset_index;
        }
        offs[offset_index++] = column.offsets()[column.size()];
    }

    // Correctly write contents and offsets in temporary buffers
    ukv_length_t offset = 0;
    auto contents_begin = table.column(0).contents();
    char buffer[contents_length];
    for (std::size_t idx = 0; idx != lens.size(); ++idx) {
        std::memcpy(buffer + offset, contents_begin + offs[idx], lens[idx]);
        offs[idx] = offset;
        offset += lens[idx];
    }

    // Rewrite contents and offsets on arena
    offset_index = 0;
    std::memcpy(contents_begin, buffer, contents_length);
    for (std::size_t column_idx : binary_column_indexes) {
        column_view_t column = table.column(column_idx);
        std::memcpy(column.offsets(), offs.data() + offset_index, offsets_per_column * sizeof(ukv_length_t));
        offset_index += offsets_per_column;
    }
}

static py::object materialize(py_table_collection_t& df) {

    // Extract the keys, if not explicitly defined
    if (std::holds_alternative<std::monostate>(df.rows_keys))
        throw std::invalid_argument("Full collection table materialization is not allowed");

    if (std::holds_alternative<py_table_keys_range_t>(df.rows_keys))
        scan_rows_range(df);

    // Slice the keys using `head` and `tail`
    auto& keys_found = std::get<std::vector<ukv_key_t>>(df.rows_keys);
    auto keys_begin = keys_found.data();
    auto keys_end = keys_found.data() + keys_found.size();
    auto keys_count = keys_found.size();
    if (df.head_was_defined_last) {
        if (keys_count > df.tail)
            keys_begin += keys_count - df.tail;
        keys_count = keys_end - keys_begin;
        if (keys_count > df.head)
            keys_end -= keys_count - df.head;
    }
    else {
        if (keys_count > df.head)
            keys_end -= keys_count - df.head;
        keys_count = keys_end - keys_begin;
        if (keys_count > df.tail)
            keys_begin += keys_count - df.tail;
    }
    keys_count = keys_end - keys_begin;
    if (keys_count != keys_found.size()) {
        std::memmove(keys_found.data(), keys_begin, keys_count);
        keys_found.resize(keys_count);
    }

    auto collection = docs_collection_t(df.binary.native.db(),
                                        df.binary.native,
                                        df.binary.native.txn(),
                                        df.binary.native.member_arena());
    auto members = collection[keys_found];

    // Extract the present fields
    if (std::holds_alternative<std::monostate>(df.columns_names)) {
        auto fields = members.gist().throw_or_release();
        auto names = std::vector<ukv_str_view_t>(fields.size());
        transform_n(fields, names.size(), names.begin(), std::mem_fn(&std::string_view::data));
        df.columns_names = names;
    }

    // Request the fields
    if (std::holds_alternative<std::monostate>(df.columns_types))
        throw std::invalid_argument("Column types must be specified");

    // Now the primary part, performing the exports
    auto fields = strided_range(std::get<std::vector<ukv_str_view_t>>(df.columns_names)).immutable();
    table_header_view_t header;
    header.count = fields.size();
    header.fields_begin = fields.begin();
    header.types_begin =
        std::holds_alternative<ukv_doc_field_type_t>(df.columns_types)
            ? strided_iterator_gt<ukv_doc_field_type_t const>(&std::get<ukv_doc_field_type_t>(df.columns_types), 0)
            : strided_iterator_gt<ukv_doc_field_type_t const>(
                  std::get<std::vector<ukv_doc_field_type_t>>(df.columns_types).data(),
                  sizeof(ukv_doc_field_type_t));
    docs_table_t table = members.gather(header).throw_or_release();
    table_header_view_t table_header = table.header();

    // Exports results into Arrow
    status_t status;
    ArrowSchema c_arrow_schema;
    ArrowArray c_arrow_array;
    ukv_to_arrow_schema( //
        table.rows(),
        table.collections(),
        &c_arrow_schema,
        &c_arrow_array,
        status.member_ptr());
    status.throw_unhandled();

    correct_table(table);
    // Exports columns one-by-one
    for (std::size_t collection_idx = 0; collection_idx != table.collections(); ++collection_idx) {
        column_view_t column = table.column(collection_idx);
        ukv_to_arrow_column( //
            table.rows(),
            table_header.fields_begin[collection_idx],
            table_header.types_begin[collection_idx],
            column.validities(),
            column.offsets(),
            column.contents(),
            c_arrow_schema.children[collection_idx],
            c_arrow_array.children[collection_idx],
            status.member_ptr());
        status.throw_unhandled();
    }

    // Pass C to C++ and then to Python:
    // https://github.com/apache/arrow/blob/master/cpp/src/arrow/c/bridge.h#L138
    arrow::Result<std::shared_ptr<arrow::RecordBatch>> table_arrow =
        arrow::ImportRecordBatch(&c_arrow_array, &c_arrow_schema);
    // https://github.com/apache/arrow/blob/a270afc946398a0279b1971a315858d8b5f07e2d/cpp/src/arrow/python/pyarrow.h#L52
    PyObject* table_python = arrow::py::wrap_batch(table_arrow.ValueOrDie());
    return py::reinterpret_steal<py::object>(table_python);
}

template <typename array_type_at>
void add_key_value( //
    std::shared_ptr<arrow::Array> array,
    std::string& jsons,
    std::string_view column_name,
    std::size_t row_idx) {
    auto numeric_array = std::static_pointer_cast<array_type_at>(array);
    fmt::format_to(std::back_inserter(jsons), "\"{}\":{},", column_name.data(), numeric_array->Value(row_idx));
}

template <>
void add_key_value<arrow::BinaryArray>( //
    std::shared_ptr<arrow::Array> array,
    std::string& jsons,
    std::string_view column_name,
    std::size_t row_idx) {
    auto binary_array = std::static_pointer_cast<arrow::BinaryArray>(array);
    auto value = binary_array->Value(row_idx);
    fmt::format_to(std::back_inserter(jsons),
                   "\"{}\":\"{}\",",
                   column_name.data(),
                   std::string_view(value.data(), value.size()));
}

void update(py_table_collection_t& df, py::object obj) {
    if (!arrow::py::is_batch(obj.ptr()))
        throw std::invalid_argument("Expected Arrow Table!");

    if (std::holds_alternative<std::monostate>(df.rows_keys))
        scan_rows(df);
    else if (std::holds_alternative<py_table_keys_range_t>(df.rows_keys))
        scan_rows_range(df);

    auto& keys = std::get<std::vector<ukv_key_t>>(df.rows_keys);
    auto collection = docs_collection_t(df.binary.native.db(), df.binary.native, df.binary.native.txn());

    arrow::Result<std::shared_ptr<arrow::RecordBatch>> maybe_record_batch = arrow::py::unwrap_batch(obj.ptr());
    std::shared_ptr<arrow::RecordBatch> record_batch = maybe_record_batch.ValueOrDie();

    if (record_batch->num_rows() != keys.size())
        throw std::invalid_argument("record_batch Rows Count Must Match Keys Count");

    std::size_t column_names_length = 0;
    for (size_t column_idx = 0; column_idx != record_batch->num_columns(); ++column_idx)
        column_names_length += std::strlen(record_batch->column_name(column_idx).c_str());

    std::string jsons_to_merge;
    jsons_to_merge.reserve(record_batch->num_rows() * (column_names_length + (record_batch->num_columns() * 3) + 2));
    std::vector<ukv_length_t> offsets(keys.size() + 1);

    for (size_t row_idx = 0; row_idx != record_batch->num_rows(); ++row_idx) {

        offsets[row_idx] = jsons_to_merge.size();
        jsons_to_merge += "{";
        for (size_t column_idx = 0; column_idx != record_batch->num_columns(); ++column_idx) {

            std::string_view name = record_batch->column_name(column_idx);
            std::shared_ptr<arrow::Array> array = record_batch->column(column_idx);

            using type = arrow::Type;
            switch (array->type_id()) {
            case type::HALF_FLOAT: add_key_value<arrow::HalfFloatArray>(array, jsons_to_merge, name, row_idx); break;
            case type::FLOAT: add_key_value<arrow::FloatArray>(array, jsons_to_merge, name, row_idx); break;
            case type::DOUBLE: add_key_value<arrow::DoubleArray>(array, jsons_to_merge, name, row_idx); break;
            case type::BOOL: add_key_value<arrow::BooleanArray>(array, jsons_to_merge, name, row_idx); break;
            case type::UINT8: add_key_value<arrow::UInt8Array>(array, jsons_to_merge, name, row_idx); break;
            case type::INT8: add_key_value<arrow::Int8Array>(array, jsons_to_merge, name, row_idx); break;
            case type::UINT16: add_key_value<arrow::UInt16Array>(array, jsons_to_merge, name, row_idx); break;
            case type::INT16: add_key_value<arrow::Int16Array>(array, jsons_to_merge, name, row_idx); break;
            case type::UINT32: add_key_value<arrow::UInt32Array>(array, jsons_to_merge, name, row_idx); break;
            case type::INT32: add_key_value<arrow::Int32Array>(array, jsons_to_merge, name, row_idx); break;
            case type::UINT64: add_key_value<arrow::UInt64Array>(array, jsons_to_merge, name, row_idx); break;
            case type::INT64: add_key_value<arrow::Int64Array>(array, jsons_to_merge, name, row_idx); break;
            case type::STRING:
            case type::BINARY: add_key_value<arrow::BinaryArray>(array, jsons_to_merge, name, row_idx); break;
            }
        }
        jsons_to_merge.back() = '}';
    }

    offsets.back() = jsons_to_merge.size();
    auto vals_begin = reinterpret_cast<ukv_bytes_ptr_t>(jsons_to_merge.data());
    contents_arg_t values {
        .offsets_begin = {offsets.data(), sizeof(ukv_length_t)},
        .contents_begin = {&vals_begin, 0},
    };

    collection[keys].merge(values);
}

void ukv::wrap_pandas(py::module& m) {

    auto df =
        py::class_<py_table_collection_t, std::shared_ptr<py_table_collection_t>>(m, "DataFrame", py::module_local());
    df.def(py::init([](py::handle dtype) {
        // `dtype` can be a `dict` or a `list[tuple[str, str]]`, where every pair of
        // strings contains a column name and Python type descriptor
        return std::make_shared<py_table_collection_t>();
    }));

#pragma region Managing Columns

    // https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.astype.html
    df.def("astype", [](py_table_collection_t& df, py::handle dtype_py) {
        // `dtype` can be one string, one enum, a `dict` or a `list[tuple[str, str]]`,
        // where every pair of strings contains a column name and Python type descriptor.
        if (PyDict_Check(dtype_py.ptr())) {
            std::vector<ukv_str_view_t> columns_names;
            std::vector<ukv_doc_field_type_t> columns_types;
            py_scan_dict(dtype_py.ptr(), [&](PyObject* key, PyObject* val) {
                columns_names.push_back(py_to_str(key));
                columns_types.push_back(ukv_doc_field_from_str(py_to_str(val)));
            });

            df.columns_names = columns_names;
            df.columns_types = columns_types;
        }
        // One type definition for all the columns
        // https://stackoverflow.com/a/45063514/2766161
        else if (PyUnicode_Check(dtype_py.ptr())) {
            df.columns_types = ukv_doc_field_from_str(py_to_str(dtype_py.ptr()));
        }
        return df.shared_from_this();
    });

    df.def("__getitem__", [](py_table_collection_t& df, py::handle columns_py) {
        //
        auto columns_count = py_sequence_length(columns_py.ptr());
        if (columns_count == std::nullopt || !*columns_count)
            throw std::invalid_argument("Columns must be a non-empty tuple or list");

        auto columns_names = std::vector<ukv_str_view_t>(*columns_count);
        py_transform_n(columns_py.ptr(), &py_to_str, columns_names.begin(), *columns_count);
        df.columns_names = columns_names;
        return df.shared_from_this();
    });

#pragma region Managing Rows

    df.def("loc", [](py_table_collection_t& df, py::handle rows_py) {
        //
        if (PySlice_Check(rows_py.ptr())) {
            Py_ssize_t start = 0, stop = 0, step = 0;
            if (PySlice_Unpack(rows_py.ptr(), &start, &stop, &step) || step != 1 || start >= stop)
                throw std::invalid_argument("Invalid Slice");
            df.rows_keys = py_table_keys_range_t {static_cast<ukv_key_t>(start), static_cast<ukv_key_t>(stop)};
        }
        else {
            auto rows_count = py_sequence_length(rows_py.ptr());
            if (rows_count == std::nullopt || !*rows_count)
                throw std::invalid_argument("Rows keys must be a non-empty tuple or list");

            auto rows_keys = std::vector<ukv_key_t>(*rows_count);
            py_transform_n(rows_py.ptr(), &py_to_scalar<ukv_key_t>, rows_keys.begin(), *rows_count);
            df.rows_keys = rows_keys;
        }
        return df.shared_from_this();
    });
    df.def("head", [](py_table_collection_t& df, std::size_t count) {
        df.head = count;
        df.head_was_defined_last = true;
        return df.shared_from_this();
    });
    df.def("tail", [](py_table_collection_t& df, std::size_t count) {
        df.tail = count;
        df.head_was_defined_last = false;
        return df.shared_from_this();
    });

    // Assigns or inserts elements from another DataFrame, passed in the Arrow form.
    // https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.update.html
    df.def("update", &update);

    // Primary batch export functions, that output Arrow Tables.
    // Addresses may be: specific IDs or a slice.
    // https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.loc.html#pandas.DataFrame.loc
    // https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.iloc.html#pandas.DataFrame.iloc
    df.def("to_arrow", &materialize);

    // https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.to_json.html
    // df.def("to_json", [](py_table_collection_t& df, py::object const& path_or_buf) {});
    // https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.to_parquet.html
    // df.def("to_parquet", [](py_table_collection_t& df, py::object const& path_or_buf) {});
    // https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.to_csv.html
    // df.def("to_csv", [](py_table_collection_t& df, py::object const& path_or_buf) {});
    // https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.to_numpy.html
    // df.def("to_numpy", [](py_table_collection_t& df, py::handle mat) {});

    // https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.index.html#pandas.DataFrame.index
    // df.def("index", [](py_table_collection_t& df) {});
    // https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.columns.html#pandas.DataFrame.columns
    // df.def("columns", [](py_table_collection_t& df) {});
    // https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.dtypes.html#pandas.DataFrame.dtypes
    // df.def("dtypes", [](py_table_collection_t& df) {});

    // https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.sample.html
    // df.def("sample", [](py_table_collection_t& df, std::size_t count, bool replace) {});

    // https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.concat.html
    // df.def("concat", [](py_table_collection_t const& df, py_table_collection_t const& df_other) {});
    // https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.assign.html
    // df.def("assign", [](py_table_collection_t& df, py_table_collection_t const& df_other) {});

    // https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.replace.html
    // df.def("replace", [](py_table_collection_t& df) {});
    // https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.merge.html
    // df.def("merge", [](py_table_collection_t& df) {});
    // https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.join.html
    // df.def("join", [](py_table_collection_t& df) {});
}
