#include <arrow/c/bridge.h>
#include <arrow/python/pyarrow.h>

#include "ukv/arrow.h"
#include "pybind.hpp"
#include "crud.hpp"
#include "cast.hpp"

using namespace unum::ukv::pyb;
using namespace unum::ukv;
using namespace unum;

enum class read_format_t {
    pythonic_k,
    arrow_k,
    tensor_k,
};

static void commit_txn(py_transaction_t& py_txn) {

    [[maybe_unused]] py::gil_scoped_release release;
    py_txn.native.commit().throw_unhandled();
}

template <typename collection_at>
static std::unique_ptr<py_collection_gt<collection_at>> punned_collection( //
    std::shared_ptr<py_db_t> py_db_ptr,
    std::shared_ptr<py_transaction_t> py_txn_ptr,
    std::string const& name) {

    ukv_collection_t collection = py_db_ptr->native.find_or_create<collection_at>(name.c_str()).throw_or_release();

    auto py_collection = std::make_unique<py_collection_gt<collection_at>>();
    py_collection->name = name;
    py_collection->py_db_ptr = py_db_ptr;
    py_collection->py_txn_ptr = py_txn_ptr;
    py_collection->in_txn = py_txn_ptr != nullptr;
    py_collection->native = collection_at {
        py_db_ptr->native,
        collection,
        py_txn_ptr //
            ? ukv_transaction_t(py_txn_ptr->native)
            : ukv_transaction_t(nullptr),
    };
    return py_collection;
}

static std::unique_ptr<py_blobs_collection_t> punned_db_collection(py_db_t& db, std::string const& collection) {
    return punned_collection<blobs_collection_t>(db.shared_from_this(), nullptr, collection);
}

static std::unique_ptr<py_blobs_collection_t> punned_txn_collection(py_transaction_t& txn,
                                                                    std::string const& collection) {
    return punned_collection<blobs_collection_t>(txn.py_db_ptr.lock(), txn.shared_from_this(), collection);
}

template <typename range_at>
range_at& since(range_at& range, ukv_key_t key) {
    range.members.since(key);
    return range;
}

template <typename range_at>
range_at& until(range_at& range, ukv_key_t key) {
    range.members.until(key);
    return range;
}

py::object sample(py_blobs_collection_t& py_collection, std::size_t count) {
    blobs_range_t members(py_collection.db(), py_collection.txn(), *py_collection.member_collection());
    keys_range_t range {members};
    ptr_range_gt<ukv_key_t> samples = range.sample(count, py_collection.member_arena()).throw_or_release();

    status_t status;
    ArrowSchema c_arrow_schema;
    ArrowArray c_arrow_array;
    ukv_to_arrow_schema(count, 0, &c_arrow_schema, &c_arrow_array, status.member_ptr());
    ukv_to_arrow_column(count,
                        "samples",
                        ukv_doc_field_i64_k,
                        nullptr,
                        nullptr,
                        samples.begin(),
                        &c_arrow_schema,
                        &c_arrow_array,
                        status.member_ptr());

    arrow::Result<std::shared_ptr<arrow::Array>> array = arrow::ImportArray(&c_arrow_array, &c_arrow_schema);
    PyObject* array_python = arrow::py::wrap_array(array.ValueOrDie());
    return py::reinterpret_steal<py::object>(array_python);
}

template <typename range_at>
auto iterate(range_at& range) {
    using native_t = typename range_at::iterator_type;
    using wrap_t = py_stream_with_ending_gt<native_t>;
    native_t stream = range.begin();
    wrap_t wrap {std::move(stream), range.members.max_key()};
    return std::make_unique<wrap_t>(std::move(wrap));
}

void ukv::wrap_database(py::module& m) {
    // Define our primary classes: `DataBase`, `Collection`, `Transaction`
    auto py_db = py::class_<py_db_t, std::shared_ptr<py_db_t>>(m, "DataBase", py::module_local());
    auto py_txn = py::class_<py_transaction_t, std::shared_ptr<py_transaction_t>>(m, "Transaction", py::module_local());
    auto py_collection = py::class_<py_blobs_collection_t>(m, "Collection", py::module_local());

    using py_kstream_t = py_stream_with_ending_gt<keys_stream_t>;
    using py_kvstream_t = py_stream_with_ending_gt<pairs_stream_t>;
    auto py_krange = py::class_<keys_range_t>(m, "KeysRange", py::module_local());
    auto py_kvrange = py::class_<pairs_range_t>(m, "ItemsRange", py::module_local());
    auto py_kstream = py::class_<py_kstream_t>(m, "KeysStream", py::module_local());
    auto py_kvstream = py::class_<py_kvstream_t>(m, "ItemsStream", py::module_local());

    // Define `DataBase`
    py_db.def( //
        py::init([](std::string const& config, bool open, bool prefer_arrow) {
            database_t db;
            if (open)
                db.open(config.c_str()).throw_unhandled();
            auto py_db_ptr = std::make_shared<py_db_t>(std::move(db), config);
            py_db_ptr->export_into_arrow = prefer_arrow;
            return py_db_ptr;
        }),
        py::arg("config") = "",
        py::arg("open") = true,
        py::arg("prefer_arrow") = true);

#pragma region CRUD Operations

    // Python tasks are generally called for a single collection.
    // That greatly simplifies the implementation.
    py_collection.def("set", &write_binary<blobs_collection_t>);
    py_collection.def("pop", &remove_binary<blobs_collection_t>);  // Unlike Python, won't return the result
    py_collection.def("has_key", &has_binary<blobs_collection_t>); // Similar to Python 2
    py_collection.def("get", &read_binary);
    py_collection.def("sample_keys", &sample);
    py_collection.def("update", &update_binary);
    py_collection.def("broadcast", &broadcast_binary);
    py_collection.def("scan", &scan_binary<blobs_collection_t>);
    py_collection.def("__setitem__", &write_binary<blobs_collection_t>);
    py_collection.def("__delitem__", &remove_binary<blobs_collection_t>);
    py_collection.def("__contains__", &has_binary<blobs_collection_t>);
    py_collection.def("__getitem__", &read_binary);
    py_collection.def("__len__", &get_length<blobs_collection_t>);

    py_collection.def("clear",
                      [](py_blobs_collection_t& py_collection) { py_collection.native.clear().throw_unhandled(); });
    py_collection.def("remove",
                      [](py_blobs_collection_t& py_collection) { py_collection.native.drop().throw_unhandled(); });

    // ML-oriented procedures for zero-copy variants exporting
    // Apache Arrow shared memory handles:
    py_collection.def(
        "get_matrix",
        [](py_blobs_collection_t& py_collection, py::object keys, std::size_t truncation, char padding) { return 0; });
    py_collection.def("set_matrix",
                      [](py_blobs_collection_t& py_collection, py::object keys, py::object vals) { return 0; });

#pragma region Transactions and Lifetime

    py_txn.def( //
        py::init([](py_db_t& py_db, bool begin, bool watch, bool flush_writes, bool snapshot) {
            auto db_ptr = py_db.shared_from_this();
            auto maybe_txn = py_db.native.transact(snapshot);
            maybe_txn.throw_unhandled();
            auto py_txn_ptr = std::make_shared<py_transaction_t>(*std::move(maybe_txn), db_ptr);
            py_txn_ptr->dont_watch = !watch;
            py_txn_ptr->flush_writes = flush_writes;
            return py_txn_ptr;
        }),
        py::arg("db"),
        py::arg("begin") = true,
        py::arg("watch") = true,
        py::arg("flush_writes") = false,
        py::arg("snapshot") = false);

    py_txn.def("__enter__", [](py_transaction_t& py_txn) {
        if (py_txn.native)
            return py_txn.shared_from_this();

        [[maybe_unused]] py::gil_scoped_release release;
        py_txn.native.reset().throw_unhandled();
        return py_txn.shared_from_this();
    });
    py_txn.def("commit", &commit_txn);

    py_db.def("__enter__", [](py_db_t& py_db) {
        if (!py_db.native)
            py_db.native.open(py_db.config.c_str()).throw_unhandled();
        return py_db.shared_from_this();
    });
    py_db.def("close", [](py_db_t& py_db) { py_db.native.close(); });

    py_db.def( //
        "__exit__",
        [](py_db_t& py_db,
           py::object const& exception_type,
           py::object const& exception_value,
           py::object const& trace) {
            py_db.native.close();
            return false;
        });
    py_txn.def( //
        "__exit__",
        [](py_transaction_t& py_txn,
           py::object const& exception_type,
           py::object const& exception_value,
           py::object const& trace) {
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

#pragma region Managing Collections

    py_db.def_property_readonly("main", [](py_db_t& py_db) { return punned_db_collection(py_db, ""); });
    py_txn.def_property_readonly("main", [](py_transaction_t& py_txn) { return punned_txn_collection(py_txn, ""); });
    py_db.def("__getitem__", &punned_db_collection, py::arg("collection"));
    py_txn.def("__getitem__", &punned_txn_collection, py::arg("collection"));
    py_db.def("clear", [](py_db_t& py_db) { py_db.native.clear().throw_unhandled(); });

    py_db.def("collection_names", [](py_db_t& py_db) {
        status_t status;
        ukv_size_t count {0};
        ukv_collection_t* ids {nullptr};
        arena_t arena(py_db.native);
        ukv_str_span_t names {nullptr};
        ukv_collection_list_t collection_list {
            .db = py_db.native,
            .error = status.member_ptr(),
            .arena = arena.member_ptr(),
            .count = &count,
            .ids = &ids,
            .names = &names,
        };
        ukv_collection_list(&collection_list);
        status.throw_unhandled();
        std::vector<std::string> names_copy {count};
        strings_tape_iterator_t names_it {count, names};
        while (!names_it.is_end())
            names_copy.push_back(*names_it);
        return names_copy;
    });

    py_db.def(
        "__contains__",
        [](py_db_t& py_db, std::string const& name) { return py_db.native.contains(name.c_str()).throw_or_release(); },
        py::arg("collection"));
    py_db.def(
        "__delitem__",
        [](py_db_t& py_db, std::string const& name) { py_db.native.drop(name.c_str()).throw_unhandled(); },
        py::arg("collection"));

    py_collection.def_property_readonly("graph", [](py_blobs_collection_t& py_collection) {
        auto py_graph = std::make_shared<py_graph_t>();
        py_graph->py_db_ptr = py_collection.py_db_ptr;
        py_graph->py_txn_ptr = py_collection.py_txn_ptr;
        py_graph->in_txn = py_collection.in_txn;
        py_graph->index = py_collection.native;
        return py::cast(py_graph);
    });
    py_collection.def_property_readonly("table", [](py_blobs_collection_t& py_collection) {
        auto py_table = std::make_shared<py_table_collection_t>();
        py_table->binary = py_collection;
        return py::cast(py_table);
    });
    py_collection.def_property_readonly("docs", [](py_blobs_collection_t& py_collection) {
        return punned_collection<docs_collection_t>(py_collection.py_db_ptr.lock(),
                                                    py_collection.py_txn_ptr.lock(),
                                                    py_collection.name);
    });
    py_collection.def_property_readonly("media", [](py_blobs_collection_t& py_collection) { return 0; });

#pragma region Streams and Ranges

    py_krange.def("__iter__", &iterate<keys_range_t>);
    py_krange.def("since", &since<keys_range_t>);
    py_krange.def("until", &until<keys_range_t>);
    py_kvrange.def("__iter__", &iterate<pairs_range_t>);
    py_kvrange.def("since", &since<pairs_range_t>);
    py_kvrange.def("until", &until<pairs_range_t>);

    // Using slices on the keys view is too cumbersome!
    // It's never clear if we want a range of IDs or offsets.
    // Offsets seems to be the Python-ic way, yet Pandas matches against labels.
    // Furthermore, skipping with offsets will be very inefficient in the underlying
    // DBMS implementations, unlike seeking to key.
    // py_krange.def("__getitem__", [](keys_range_t& keys_range, py::slice slice) {
    //     Py_ssize_t start = 0, stop = 0, step = 0;
    //     if (PySlice_Unpack(slice.ptr(), &start, &stop, &step) || step != 1 || start >= stop)
    //         throw std::invalid_argument("Invalid Slice");
    //     keys_stream_t stream = keys_range.members.keys_begin(stop).throw_or_release();
    //     auto keys = stream.keys_batch();
    //     auto remaining = std::min<Py_ssize_t>(stop - start, keys.size() - start);
    //     return py::array(remaining, keys.begin() + start);
    // });

    py_kstream.def("__next__", [](py_kstream_t& kstream) {
        ukv_key_t key = kstream.native.key();
        if (kstream.native.is_end() || kstream.stop)
            throw py::stop_iteration();
        kstream.stop = kstream.terminal == key;
        ++kstream.native;
        return key;
    });
    py_kvstream.def("__next__", [](py_kvstream_t& kvstream) {
        ukv_key_t key = kvstream.native.key();
        if (kvstream.native.is_end() || kvstream.stop)
            throw py::stop_iteration();
        kvstream.stop = kvstream.terminal == key;
        value_view_t value_view = kvstream.native.value();
        PyObject* value_ptr = PyBytes_FromStringAndSize(value_view.c_str(), value_view.size());
        ++kvstream.native;
        return py::make_tuple(key, py::reinterpret_borrow<py::object>(value_ptr));
    });

    py_collection.def_property_readonly("keys", [](py_blobs_collection_t& py_collection) {
        blobs_range_t members(py_collection.db(), py_collection.txn(), *py_collection.member_collection());
        keys_range_t range {members};
        return py::cast(std::make_unique<keys_range_t>(range));
    });
    py_collection.def_property_readonly("items", [](py_blobs_collection_t& py_collection) {
        blobs_range_t members(py_collection.db(), py_collection.txn(), *py_collection.member_collection());
        pairs_range_t range {members};
        return py::cast(std::make_unique<pairs_range_t>(range));
    });
}
