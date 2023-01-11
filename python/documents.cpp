#include "pybind.hpp"
#include <cast.hpp>
#include <crud.hpp>
#include <nlohmann.hpp>

using namespace unum::ukv::pyb;
using namespace unum::ukv;
using namespace unum;

class docs_pairs_stream_t {

    ukv_database_t db_ = nullptr;
    ukv_collection_t collection_ = ukv_collection_main_k;
    ukv_transaction_t txn_ = nullptr;

    arena_t arena_scan_;
    arena_t arena_read_;
    ukv_length_t read_ahead_ = 0;

    ukv_key_t next_min_key_ = std::numeric_limits<ukv_key_t>::min();
    ptr_range_gt<ukv_key_t> fetched_keys_;
    embedded_blobs_t values_view;
    std::size_t fetched_offset_ = 0;

    status_t prefetch() noexcept {

        if (next_min_key_ == ukv_key_unknown_k)
            return {};

        ukv_key_t* found_keys = nullptr;
        ukv_length_t* found_offsets = nullptr;
        ukv_length_t* found_counts = nullptr;
        ukv_length_t* found_lengths = nullptr;
        ukv_bytes_ptr_t found_values = nullptr;
        ukv_str_view_t fields = nullptr;
        status_t status;
        ukv_scan_t scan {
            .db = db_,
            .error = status.member_ptr(),
            .transaction = txn_,
            .arena = arena_scan_.member_ptr(),
            .tasks_count = 1,
            .collections = &collection_,
            .start_keys = &next_min_key_,
            .count_limits = &read_ahead_,
            .offsets = &found_offsets,
            .counts = &found_counts,
            .keys = &found_keys,
        };

        ukv_scan(&scan);
        if (!status)
            return status;

        fetched_keys_ = ptr_range_gt<ukv_key_t> {found_keys, found_keys + *found_counts};
        fetched_offset_ = 0;
        auto count = static_cast<ukv_size_t>(fetched_keys_.size());
        ukv_docs_read_t docs_read {
            .db = db_,
            .error = status.member_ptr(),
            .transaction = txn_,
            .arena = arena_read_.member_ptr(),
            .type = ukv_doc_field_json_k,
            .tasks_count = count,
            .collections = &collection_,
            .keys = found_keys,
            .keys_stride = sizeof(ukv_key_t),
            .fields = &fields,
            .fields_stride = 0,
            .offsets = &found_offsets,
            .lengths = &found_lengths,
            .values = &found_values,
        };

        ukv_docs_read(&docs_read);
        if (!status)
            return status;

        values_view = embedded_blobs_t {count, found_offsets, found_lengths, found_values};
        next_min_key_ = count <= read_ahead_ ? ukv_key_unknown_k : fetched_keys_[count - 1] + 1;
        return {};
    }

  public:
    static constexpr std::size_t default_read_ahead_k = 256;

    docs_pairs_stream_t(ukv_database_t db,
                        ukv_collection_t collection = ukv_collection_main_k,
                        std::size_t read_ahead = docs_pairs_stream_t::default_read_ahead_k,
                        ukv_transaction_t txn = nullptr)
        : db_(db), collection_(collection), txn_(txn), arena_scan_(db_), arena_read_(db_),
          read_ahead_(static_cast<ukv_size_t>(read_ahead)) {}

    status_t seek(ukv_key_t key) noexcept {
        fetched_keys_ = {};
        fetched_offset_ = 0;
        next_min_key_ = key;
        return prefetch();
    }

    status_t advance() noexcept {
        if (fetched_offset_ >= fetched_keys_.size())
            return prefetch();
        ++fetched_offset_;
        return {};
    }

    docs_pairs_stream_t& operator++() noexcept {
        status_t status = advance();
        if (status)
            return *this;

        fetched_keys_ = {};
        fetched_offset_ = 0;
        next_min_key_ = ukv_key_unknown_k;
        return *this;
    }

    ukv_key_t key() const noexcept { return fetched_keys_[fetched_offset_]; }
    value_view_t value() const noexcept {
        auto it = values_view.begin();
        for (size_t i = 0; i != fetched_offset_; ++i)
            ++it;
        return *it;
    }

    bool is_end() const noexcept {
        return next_min_key_ == ukv_key_unknown_k && fetched_offset_ >= fetched_keys_.size();
    }
};

class py_docs_kvrange_t {

    ukv_database_t db_;
    ukv_transaction_t txn_;
    ukv_collection_t collection_;
    ukv_key_t min_key_;
    ukv_key_t max_key_;

  public:
    py_docs_kvrange_t(ukv_database_t db,
                      ukv_transaction_t txn = nullptr,
                      ukv_collection_t collection = ukv_collection_main_k,
                      ukv_key_t min_key = std::numeric_limits<ukv_key_t>::min(),
                      ukv_key_t max_key = ukv_key_unknown_k) noexcept
        : db_(db), txn_(txn), collection_(collection), min_key_(min_key), max_key_(max_key) {}

    py_docs_kvrange_t& since(ukv_key_t min_key) noexcept {
        min_key_ = min_key;
        return *this;
    }
    py_docs_kvrange_t& until(ukv_key_t max_key) noexcept {
        max_key_ = max_key;
        return *this;
    }

    ukv_key_t max_key() noexcept { return max_key_; }
    docs_pairs_stream_t begin() noexcept(false) {
        docs_pairs_stream_t stream {db_, collection_, docs_pairs_stream_t::default_read_ahead_k, txn_};
        status_t status = stream.seek(min_key_);
        return stream;
    }
};

static void write_one_doc(py_docs_collection_t& py_collection, PyObject* key_py, PyObject* val_py) {
    std::string json_str;
    to_string(val_py, json_str);
    ukv_key_t key = py_to_scalar<ukv_key_t>(key_py);
    py_collection.native[key].assign(value_view_t(json_str)).throw_unhandled();
}

struct dummy_iterator_t {
    dummy_iterator_t operator*() { return *this; };
    void operator++() {}
};

static void write_many_docs(py_docs_collection_t& py_collection, PyObject* keys_py, PyObject* vals_py) {
    if (!PySequence_Check(keys_py) || !PySequence_Check(vals_py))
        throw std::invalid_argument("Keys And Vals Must Be Sequence");
    std::size_t keys_count = PySequence_Size(keys_py);
    if (keys_count != PySequence_Size(vals_py))
        throw std::invalid_argument("Keys Count Must Match Values Count");

    std::vector<ukv_key_t> keys(keys_count);
    std::vector<ukv_length_t> offs(keys_count + 1);
    std::string jsons;
    offs[0] = 0;
    size_t idx = 1;

    auto generate_values = [&](py::handle const& obj) {
        to_string(obj.ptr(), jsons);
        offs[idx] = jsons.size();
        ++idx;
        return dummy_iterator_t {};
    };

    py_transform_n(keys_py, &py_to_scalar<ukv_key_t>, keys.begin());
    py_transform_n(vals_py, generate_values, dummy_iterator_t {});

    auto vals_begin = reinterpret_cast<ukv_bytes_ptr_t>(jsons.data());
    contents_arg_t values {
        .offsets_begin = {offs.data(), sizeof(ukv_length_t)},
        .contents_begin = {&vals_begin, 0},
        .count = keys_count,
    };

    auto ref = py_collection.native[keys];
    ref.assign(values).throw_unhandled();
}

static void write_same_doc(py_docs_collection_t& py_collection, PyObject* keys_py, PyObject* val_py) {
    if (!PySequence_Check(keys_py))
        throw std::invalid_argument("Keys Must Be Sequence");
    std::vector<ukv_key_t> keys(PySequence_Size(keys_py));
    py_transform_n(keys_py, &py_to_scalar<ukv_key_t>, keys.begin());
    std::string json_str;
    to_string(val_py, json_str);
    py_collection.native[keys].assign(value_view_t(json_str)).throw_unhandled();
}

static void write_doc(py_docs_collection_t& py_collection, py::object key_py, py::object val_py) {
    auto is_single_key = !PySequence_Check(key_py.ptr());
    auto func = !is_single_key ? &write_many_docs : &write_one_doc;
    return func(py_collection, key_py.ptr(), val_py.ptr());
}

static void broadcast_doc(py_docs_collection_t& py_collection, py::object key_py, py::object val_py) {
    return write_same_doc(py_collection, key_py.ptr(), val_py.ptr());
}

static py::object read_one_doc(py_docs_collection_t& py_collection, PyObject* key_py) {
    ukv_key_t key = py_to_scalar<ukv_key_t>(key_py);
    auto value = py_collection.native[key].value();
    return value->empty() ? py::none {} : py::reinterpret_steal<py::object>(from_json(json_t::parse(*value)));
}

static py::object read_many_docs(py_docs_collection_t& py_collection, PyObject* keys_py) {
    if (!PySequence_Check(keys_py))
        throw std::invalid_argument("Keys Must Be Sequence");
    std::vector<ukv_key_t> keys(PySequence_Size(keys_py));
    py_transform_n(keys_py, &py_to_scalar<ukv_key_t>, keys.begin());
    py::list values(keys.size());

    auto maybe_retrieved = py_collection.native[keys].value();
    auto const& retrieved = maybe_retrieved.throw_or_ref();
    auto it = retrieved.begin();
    for (std::size_t i = 0; i != retrieved.size(); ++i)
        values[i] = it[i].empty() ? py::none {} : py::reinterpret_steal<py::object>(from_json(json_t::parse(it[i])));
    return values;
}

static py::object read_doc(py_docs_collection_t& py_collection, py::object key_py) {
    auto is_single = !PySequence_Check(key_py.ptr());
    auto func = is_single ? &read_one_doc : &read_many_docs;
    return func(py_collection, key_py.ptr());
}

static void remove_doc(py_docs_collection_t& py_collection, py::object key_py) {
    auto is_single = !PySequence_Check(key_py.ptr());
    auto func = is_single ? &write_one_binary<docs_collection_t> : &write_many_binaries<docs_collection_t>;
    return func(py_collection, key_py.ptr(), Py_None);
}

static py::object has_doc(py_docs_collection_t& py_collection, py::object key_py) {
    return has_binary(py_collection, key_py);
}

static py::object scan_doc(py_docs_collection_t& py_collection, ukv_key_t min_key, ukv_size_t count_limit) {
    return scan_binary(py_collection, min_key, count_limit);
}

static void merge(py_docs_collection_t& py_collection, py::object key_py, py::object val_py) {
    ukv_key_t key = py_to_scalar<ukv_key_t>(key_py.ptr());
    std::string json_str;
    to_string(val_py.ptr(), json_str);
    py_collection.native[key].merge(value_view_t(json_str));
}

static void patch(py_docs_collection_t& py_collection, py::object key_py, py::object val_py) {
    ukv_key_t key = py_to_scalar<ukv_key_t>(key_py.ptr());
    std::string json_str;
    to_string(val_py.ptr(), json_str);
    py_collection.native[key].patch(value_view_t(json_str));
}

void ukv::wrap_document(py::module& m) {
    using py_docs_kvstream_t = py_stream_with_ending_gt<docs_pairs_stream_t>;

    auto py_docs_collection = py::class_<py_docs_collection_t>(m, "DocsCollection", py::module_local());
    auto py_docs_kvrange = py::class_<py_docs_kvrange_t>(m, "DocsKVRange", py::module_local());
    auto py_docs_kvstream = py::class_<py_docs_kvstream_t>(m, "DocsKVStream", py::module_local());

    py_docs_collection.def("set", &write_doc);
    py_docs_collection.def("get", &read_doc);
    py_docs_collection.def("remove", &remove_doc);
    py_docs_collection.def("has_key", &has_doc);
    py_docs_collection.def("scan", &scan_doc);
    py_docs_collection.def("broadcast", &broadcast_doc);

    py_docs_collection.def("__setitem__", &write_doc);
    py_docs_collection.def("__delitem__", &remove_doc);
    py_docs_collection.def("__getitem__", &read_doc);
    py_docs_collection.def("__contains__", &has_doc);
    py_docs_collection.def("__len__", &get_length<docs_collection_t>);

    py_docs_collection.def("clear", [](py_docs_collection_t& collection) { collection.native.clear(); });

    py_docs_collection.def("merge", &merge);
    py_docs_collection.def("patch", &patch);

    py_docs_collection.def_property_readonly("keys", [](py_docs_collection_t& py_collection) {
        blobs_range_t members(py_collection.db(), py_collection.txn(), *py_collection.member_collection());
        keys_range_t range {members};
        return py::cast(std::make_unique<keys_range_t>(range));
    });

    py_docs_collection.def_property_readonly("items", [](py_docs_collection_t& py_collection) {
        py_docs_kvrange_t range(py_collection.db(), py_collection.txn(), *py_collection.member_collection());
        return py::cast(std::make_unique<py_docs_kvrange_t>(range));
    });

    py_docs_kvrange.def("__iter__", [](py_docs_kvrange_t& range) {
        docs_pairs_stream_t stream = range.begin();
        py_stream_with_ending_gt<docs_pairs_stream_t> wrap {std::move(stream), range.max_key()};
        return std::make_unique<py_stream_with_ending_gt<docs_pairs_stream_t>>(std::move(wrap));
    });

    py_docs_kvrange.def("since", [](py_docs_kvrange_t& range, ukv_key_t key) { return range.since(key); });
    py_docs_kvrange.def("until", [](py_docs_kvrange_t& range, ukv_key_t key) { return range.until(key); });

    py_docs_kvstream.def("__next__", [](py_docs_kvstream_t& kvstream) {
        ukv_key_t key = kvstream.native.key();
        if (kvstream.native.is_end() || kvstream.stop)
            throw py::stop_iteration();
        kvstream.stop = kvstream.terminal == key;
        value_view_t value_view = kvstream.native.value();
        ++kvstream.native;
        return py::make_tuple(key, py::reinterpret_steal<py::object>(from_json(json_t::parse(value_view))));
    });
}