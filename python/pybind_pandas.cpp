/**
 * @brief Python bindings for a Document Store, that mimics Pandas.
 * Mostly intended for usage with NumPy and Arrow buffers.
 */

#include "pybind.hpp"
#include "ukv/docs.h"

using namespace unum::ukv;
using namespace unum;

struct col_name_t {
    std::string owned;
    ukv_str_view_t view;
};

struct col_keys_range_t {
    ukv_collection_t col = ukv_collection_default_k;
    ukv_key_t min = std::numeric_limits<ukv_key_t>::min();
    ukv_key_t max = std::numeric_limits<ukv_key_t>::max();
    std::size_t limit = std::numeric_limits<std::size_t>::max();
};

/**
 * @brief Materialized view over a specific subset of documents
 * UIDs (potentially, in different collections) and column (field) names.
 *
 */
struct py_frame_t : public std::enable_shared_from_this<py_frame_t> {

    ukv_t db = NULL;

    std::variant<std::monostate, col_name_t, std::vector<col_name_t>> fields;
    std::variant<std::monostate, col_keys_range_t, std::vector<col_key_t>> docs;

    py_frame_t() = default;
    py_frame_t(py_frame_t&&) = delete;
    py_frame_t(py_frame_t const&) = delete;
};

void ukv::wrap_pandas(py::module& m) {

    // Once packed, our DataFrames output Apache Arrow Tables / RecordBatches:
    // https://stackoverflow.com/a/57907044/2766161
    // https://arrow.apache.org/docs/python/integration/extending.html
    auto df = py::class_<py_frame_t, std::shared_ptr<py_frame_t>>(m, "DataFrame", py::module_local());
    df.def(py::init([](std::vector<std::string> fields) { return std::make_shared<py_frame_t>(); }));

    df.def("columns", [](py_frame_t& df) {});

    // Batch Access
    // https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.iloc.html#pandas.DataFrame.loc
    // https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.iloc.html#pandas.DataFrame.iloc
    df.def("loc", [](py_frame_t& df, py::handle const ids, ukv_format_t) {});
    df.def("iloc", [](py_frame_t& df, py::handle const ids, ukv_format_t) {});

    // https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.sample.html
    df.def("sample", [](py_frame_t& df, std::size_t count, bool replace) {});

    // https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.concat.html
    // https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.assign.html
    df.def("concat", [](py_frame_t const& df, py_frame_t const& df_other) {});
    df.def("assign", [](py_frame_t& df, py_frame_t const& df_other) {});

    // https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.to_json.html
    // https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.to_parquet.html
    // https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.to_csv.html
    // https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.to_numpy.html
    df.def("to_json", [](py_frame_t& df, py::object const& path_or_buf) {});
    df.def("to_parquet", [](py_frame_t& df, py::object const& path_or_buf) {});
    df.def("to_csv", [](py_frame_t& df, py::object const& path_or_buf) {});
    df.def("to_numpy", [](py_frame_t& df, py::handle const& mat) {});
    df.def("to_arrow", [](py_frame_t& df, py::handle const& mat) {});

    // https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.replace.html
    // https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.merge.html
    // https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.join.html
    df.def("replace", [](py_frame_t& df) {});
    df.def("merge", [](py_frame_t& df) {});
    df.def("join", [](py_frame_t& df) {});
}
