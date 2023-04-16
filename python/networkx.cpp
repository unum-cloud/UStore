#include <charconv>

#include "pybind.hpp"
#include "crud.hpp"
#include "nlohmann.hpp"
#include "cast_args.hpp"
#include "algorithms/louvain.cpp"

using namespace unum::ustore::pyb;
using namespace unum::ustore;
using namespace unum;

embedded_blobs_t read_attributes( //
    docs_collection_t& collection,
    strided_range_gt<ustore_key_t const> keys,
    ustore_str_view_t field) {

    status_t status;
    ustore_length_t* found_offsets = nullptr;
    ustore_length_t* found_lengths = nullptr;
    ustore_bytes_ptr_t found_values = nullptr;
    auto count = static_cast<ustore_size_t>(keys.size());

    ustore_docs_read_t docs_read {};
    docs_read.db = collection.db();
    docs_read.error = status.member_ptr();
    docs_read.options = ustore_option_dont_discard_memory_k;
    docs_read.transaction = collection.txn();
    docs_read.snapshot = collection.snap();
    docs_read.arena = collection.member_arena();
    docs_read.type = ustore_doc_field_json_k;
    docs_read.tasks_count = count;
    docs_read.collections = collection.member_ptr();
    docs_read.keys = keys.begin().get();
    docs_read.keys_stride = keys.stride();
    docs_read.fields = &field;
    docs_read.offsets = &found_offsets;
    docs_read.lengths = &found_lengths;
    docs_read.values = &found_values;

    ustore_docs_read(&docs_read);
    status.throw_unhandled();
    return embedded_blobs_t {count, found_offsets, found_lengths, found_values};
}

void compute_degrees(py_graph_t& graph,
                     strided_range_gt<ustore_key_t const> vertices,
                     ustore_vertex_role_t role,
                     ustore_str_view_t weight,
                     ustore_vertex_degree_t** degrees) {

    status_t status;
    ustore_key_t* edges_per_vertex = nullptr;
    auto count = static_cast<ustore_size_t>(vertices.size());

    ustore_graph_find_edges_t graph_find_edges {};
    graph_find_edges.db = graph.index.db();
    graph_find_edges.error = status.member_ptr();
    graph_find_edges.transaction = graph.index.txn();
    graph_find_edges.arena = graph.index.member_arena();
    graph_find_edges.tasks_count = count;
    graph_find_edges.collections = graph.index.member_ptr();
    graph_find_edges.vertices = vertices.begin().get();
    graph_find_edges.vertices_stride = vertices.stride();
    graph_find_edges.roles = &role;
    graph_find_edges.degrees_per_vertex = degrees;
    graph_find_edges.edges_per_vertex = &edges_per_vertex;

    ustore_graph_find_edges(&graph_find_edges);
    status.throw_unhandled();
    if (!weight)
        return;

    auto edges_begin = reinterpret_cast<edge_t*>(edges_per_vertex);
    auto all_edges_count = transform_reduce_n(*degrees, count, 0ul, [](ustore_vertex_degree_t deg) {
        return deg == ustore_vertex_degree_missing_k ? 0 : deg;
    });
    auto edges = edges_span_t {edges_begin, edges_begin + all_edges_count};

    ustore_length_t* found_offsets = nullptr;
    ustore_length_t* found_lengths = nullptr;
    ustore_bytes_ptr_t found_values = nullptr;

    auto values = read_attributes(graph.relations_attrs, edges.edge_ids.immutable(), weight);
    auto value_iterator = values.begin();
    for (std::size_t i = 0; i != count; ++i) {
        auto edges_count = (*degrees)[i];
        (*degrees)[i] = transform_reduce_n(value_iterator, edges_count, 0ul, [](value_view_t edge_weight) {
            ustore_vertex_degree_t weight;
            std::from_chars(edge_weight.c_str(), edge_weight.c_str() + edge_weight.size(), weight);
            return weight;
        });
    }
}

struct nodes_stream_t {
    keys_stream_t native;
    docs_collection_t& collection;
    bool read_data;
    std::string field;
    std::string default_value;

    embedded_blobs_t attrs;
    ptr_range_gt<ustore_key_t const> nodes;
    std::size_t index = 0;

    nodes_stream_t(
        keys_stream_t&& stream, docs_collection_t& col, bool data, std::string field, std::string default_value)
        : native(std::move(stream)), collection(col), read_data(data), field(field), default_value(default_value) {
        nodes = native.keys_batch();
        if (read_data)
            attrs = read_attributes(collection, nodes.strided(), field.size() ? field.c_str() : nullptr);
    }

    py::object next() {
        if (index >= nodes.size()) {
            if (native.is_end())
                throw py::stop_iteration();
            native.seek_to_next_batch();
            nodes = native.keys_batch();
            if (read_data)
                attrs = read_attributes(collection, nodes.strided(), field.size() ? field.c_str() : nullptr);
            index = 0;
        }
        py::object ret;
        if (read_data) {
            auto data = attrs[index] && !attrs[index].empty() ? attrs[index] : value_view_t(default_value);
            ret = py::make_tuple(nodes[index], py::reinterpret_steal<py::object>(from_json(json_t::parse(data))));
        }
        else
            ret = py::cast(nodes[index]);
        ++index;
        return ret;
    }
};

struct edges_stream_t {
    graph_stream_t native;
    docs_collection_t& collection;
    bool read_data;
    std::string field;
    std::string default_value;

    embedded_blobs_t attrs;
    edges_span_t edges;
    std::size_t index = 0;

    edges_stream_t(
        graph_stream_t&& stream, docs_collection_t& col, bool data, std::string field, std::string default_value)
        : native(std::move(stream)), collection(col), read_data(data), field(field), default_value(default_value) {
        edges = native.edges_batch();
        if (read_data)
            attrs = read_attributes(collection, edges.edge_ids.immutable(), field.size() ? field.c_str() : nullptr);
    }

    py::object next() {
        if (index >= edges.size()) {
            if (native.is_end())
                throw py::stop_iteration();
            native.seek_to_next_batch();
            edges = native.edges_batch();
            if (read_data)
                attrs = read_attributes(collection, edges.edge_ids.immutable(), field.size() ? field.c_str() : nullptr);
            index = 0;
        }
        auto edge = edges[index];
        py::object ret;
        if (read_data) {
            auto data = attrs[index] && !attrs[index].empty() ? attrs[index] : value_view_t(default_value);
            ret = py::make_tuple(edge.source_id,
                                 edge.target_id,
                                 py::reinterpret_steal<py::object>(from_json(json_t::parse(data))));
        }
        else
            ret = py::make_tuple(edge.source_id, edge.target_id);
        ++index;
        return ret;
    }
};

struct edges_nbunch_iter_t {
    edges_span_t edges;
    embedded_blobs_t attrs;
    bool read_data;
    std::string default_value;
    std::size_t index = 0;

    edges_nbunch_iter_t(edges_span_t edges_span, embedded_blobs_t attributes, bool data, std::string default_value)
        : edges(edges_span), attrs(attributes), read_data(data), default_value(default_value) {}

    py::object next() {
        if (index == edges.size())
            throw py::stop_iteration();
        edge_t edge = edges[index];
        py::object ret;
        if (read_data) {
            value_view_t data = attrs[index] && !attrs[index].empty() ? attrs[index] : value_view_t(default_value);
            ret = py::make_tuple(edge.source_id,
                                 edge.target_id,
                                 py::reinterpret_steal<py::object>(from_json(json_t::parse(data))));
        }
        else
            ret = py::make_tuple(edge.source_id, edge.target_id);
        ++index;
        return ret;
    }
};

struct degrees_stream_t {
    keys_stream_t keys_stream;
    py_graph_t& graph;
    std::string weight_field;
    ustore_vertex_role_t vertex_role;

    ptr_range_gt<ustore_key_t const> fetched_nodes;
    ustore_vertex_degree_t* degrees = nullptr;
    std::size_t index = 0;

    degrees_stream_t(keys_stream_t&& stream, py_graph_t& net, std::string field, ustore_vertex_role_t role)
        : keys_stream(std::move(stream)), graph(net), weight_field(field), vertex_role(role) {
        fetched_nodes = keys_stream.keys_batch();
        compute_degrees(graph,
                        fetched_nodes,
                        vertex_role,
                        weight_field.size() ? weight_field.c_str() : nullptr,
                        &degrees);
    }

    py::object next() {
        if (index >= fetched_nodes.size()) {
            if (keys_stream.is_end())
                throw py::stop_iteration();
            keys_stream.seek_to_next_batch();
            fetched_nodes = keys_stream.keys_batch();
            compute_degrees(graph,
                            fetched_nodes,
                            vertex_role,
                            weight_field.size() ? weight_field.c_str() : nullptr,
                            &degrees);
            index = 0;
        }

        auto ret = py::make_tuple(fetched_nodes[index], degrees[index]);
        ++index;
        return ret;
    }
};

struct nodes_range_t : public std::enable_shared_from_this<nodes_range_t> {
    keys_range_t native;
    docs_collection_t& collection;
    bool read_data = false;
    std::string field;
    std::string default_value;

    nodes_range_t(keys_range_t n, docs_collection_t& c) : native(n), collection(c) {}
};

struct edges_range_t : public std::enable_shared_from_this<edges_range_t> {
    std::weak_ptr<py_graph_t> net_ptr;
    std::vector<ustore_key_t> vertices;
    bool read_data = false;
    std::string field;
    std::string default_value;
};

struct degree_view_t {
    std::weak_ptr<py_graph_t> net_ptr;
    ustore_vertex_role_t roles = ustore_vertex_role_any_k;
    std::string weight = "";
};

template <typename element_at>
py::object wrap_into_buffer(py_graph_t& g, strided_range_gt<element_at> range) {

    py_buffer_memory_t& buf = g.last_buffer;

    buf.strides[0] = range.stride();
    buf.strides[1] = buf.strides[2] = 1;
    buf.shape[0] = range.size();
    buf.shape[1] = buf.shape[2] = 1;

    // https://docs.python.org/3/c-api/buffer.html
    buf.raw.buf = (void*)range.begin().get();
    buf.raw.obj = NULL;
    buf.raw.len = range.size() * sizeof(element_at);
    buf.raw.itemsize = sizeof(element_at);
    // https://docs.python.org/3/library/struct.html#format-characters
    buf.raw.format = (char*)&format_code_gt<std::remove_const_t<element_at>>::value[0];
    buf.raw.ndim = 1;
    buf.raw.shape = &buf.shape[0];
    buf.raw.strides = &buf.strides[0];
    buf.raw.suboffsets = nullptr;
    buf.raw.readonly = true;
    buf.raw.internal = nullptr;
    PyObject* obj = PyMemoryView_FromBuffer(&buf.raw);
    return py::reinterpret_steal<py::object>(obj);
}

void ustore::wrap_networkx(py::module& m) {

    auto degs = py::class_<degree_view_t>(m, "DegreeView", py::module_local());
    auto degs_stream = py::class_<degrees_stream_t>(m, "DegreesStream", py::module_local());
    degs.def("__getitem__", [](degree_view_t& degs, ustore_key_t v) {
        py_graph_t& g = *degs.net_ptr.lock().get();
        auto result = g.ref().degree(v, degs.roles).throw_or_release();
        return result;
    });

    degs.def(
        "__call__",
        [](degree_view_t& degs, py::object vs, std::string weight = "") {
            py_graph_t& g = *degs.net_ptr.lock().get();
            ustore_vertex_degree_t* degrees;

            if (PyObject_CheckBuffer(vs.ptr())) {
                auto vs_handle = py_buffer(vs.ptr());
                auto vertices = py_strided_range<ustore_key_t const>(vs_handle);
                compute_degrees(g, vertices, degs.roles, weight.size() ? weight.c_str() : nullptr, &degrees);
                return wrap_into_buffer<ustore_vertex_degree_t const>(
                    g,
                    strided_range<ustore_vertex_degree_t const>(degrees, degrees + vertices.size()));
            }
            if (!PySequence_Check(vs.ptr()))
                throw std::invalid_argument("Nodes Must Be Sequence");

            std::vector<ustore_key_t> vertices(PySequence_Size(vs.ptr()));
            py_transform_n(vs.ptr(), &py_to_scalar<ustore_key_t>, vertices.begin());
            compute_degrees(g,
                            strided_range(vertices).immutable(),
                            degs.roles,
                            weight.size() ? weight.c_str() : nullptr,
                            &degrees);
            py::list res(vertices.size());
            for (std::size_t i = 0; i != vertices.size(); ++i)
                res[i] = py::make_tuple(vertices[i], degrees[i]);
            return py::object(res);
        },
        py::arg("vs"),
        py::arg("weight") = "");

    degs.def(
        "__call__",
        [](degree_view_t& degs, std::string weight = "") {
            degs.weight = weight;
            return degs;
        },
        py::arg("weight") = "");

    degs.def("__iter__", [](degree_view_t& degs) {
        py_graph_t& g = *degs.net_ptr.lock().get();
        blobs_range_t members(g.index.db(), g.index.txn(), 0, g.index);
        keys_stream_t stream = keys_range_t({members}).begin();
        return degrees_stream_t(std::move(stream), g, degs.weight, degs.roles);
    });

    degs_stream.def("__next__", [](degrees_stream_t& stream) { return stream.next(); });

    auto nodes_range = py::class_<nodes_range_t, std::shared_ptr<nodes_range_t>>(m, "NodesRange", py::module_local());
    nodes_range.def("__iter__", [](nodes_range_t& range) {
        return nodes_stream_t {std::move(range.native).begin(),
                               range.collection,
                               range.read_data,
                               range.field,
                               range.default_value};
    });

    nodes_range.def(
        "__call__",
        [](nodes_range_t& range, bool data = false) {
            range.read_data = data;
            range.default_value = "{}";
            return range.shared_from_this();
        },
        py::arg("data") = false);

    nodes_range.def(
        "__call__",
        [](nodes_range_t& range, std::string& data, py::object def_value) {
            range.read_data = true;
            range.field = data;
            std::string str;
            to_string(def_value.ptr(), str);
            range.default_value = std::move(str);
            return range.shared_from_this();
        },
        py::arg("data"),
        py::arg("default") = py::none());

    auto nodes_stream = py::class_<nodes_stream_t>(m, "NodesStream", py::module_local());
    nodes_stream.def("__next__", [](nodes_stream_t& stream) { return stream.next(); });

    auto edges_range = py::class_<edges_range_t, std::shared_ptr<edges_range_t>>(m, "EdgesRange", py::module_local());
    edges_range.def("__iter__", [](edges_range_t& range) {
        py_graph_t& g = *range.net_ptr.lock().get();
        if (range.vertices.size()) {
            auto vertices = strided_range(range.vertices).immutable();
            auto role = ustore_vertex_source_k;
            auto edges = g.ref().edges_containing(vertices, {{&role}, 1}).throw_or_release();
            auto attrs = read_attributes(g.relations_attrs,
                                         edges.edge_ids.immutable(),
                                         range.field.size() ? range.field.c_str() : nullptr);
            return py::cast(edges_nbunch_iter_t(edges, attrs, range.read_data, range.default_value));
        }
        auto edges = g.ref().edges(ustore_vertex_source_k).throw_or_release();
        return py::cast(edges_stream_t(std::move(edges).begin(),
                                       g.relations_attrs,
                                       range.read_data,
                                       range.field,
                                       range.default_value));
    });

    edges_range.def(
        "__call__",
        [](edges_range_t& range, bool data = false) {
            range.read_data = data;
            range.default_value = "{}";
            return range.shared_from_this();
        },
        py::arg("data") = false);

    edges_range.def(
        "__call__",
        [](edges_range_t& range, std::string& data, py::object def_value) {
            range.read_data = true;
            range.field = data;
            std::string str;
            to_string(def_value.ptr(), str);
            range.default_value = std::move(str);
            return range.shared_from_this();
        },
        py::arg("data"),
        py::arg("default") = py::none());

    edges_range.def(
        "__call__",
        [](edges_range_t& range, py::object vs, bool data = false) {
            range.read_data = data;
            range.default_value = "{}";

            if (PyNumber_Check(vs.ptr()))
                range.vertices.push_back(py_to_scalar<ustore_key_t>(vs.ptr()));
            else {
                range.vertices.resize(PySequence_Size(vs.ptr()));
                py_transform_n(vs.ptr(), &py_to_scalar<ustore_key_t>, range.vertices.begin());
            }
            return range.shared_from_this();
        },
        py::arg("vs"),
        py::arg("data"));

    edges_range.def(
        "__call__",
        [](edges_range_t& range, py::object vs, std::string data, py::object def_value) {
            range.read_data = true;
            range.field = data;
            std::string str;
            to_string(def_value.ptr(), str);
            range.default_value = std::move(str);

            if (PyNumber_Check(vs.ptr()))
                range.vertices.push_back(py_to_scalar<ustore_key_t>(vs.ptr()));
            else {
                range.vertices.resize(PySequence_Size(vs.ptr()));
                py_transform_n(vs.ptr(), &py_to_scalar<ustore_key_t>, range.vertices.begin());
            }
            return range.shared_from_this();
        },
        py::arg("vs"),
        py::arg("data"),
        py::arg("default") = py::none());

    auto edges_iter = py::class_<edges_nbunch_iter_t>(m, "EdgesIter", py::module_local());
    edges_iter.def("__next__", [](edges_nbunch_iter_t& iter) { return iter.next(); });

    auto edges_stream = py::class_<edges_stream_t>(m, "EdgesStream", py::module_local());
    edges_stream.def("__next__", [](edges_stream_t& stream) { return stream.next(); });

    auto g = py::class_<py_graph_t, std::shared_ptr<py_graph_t>>(m, "Network", py::module_local());
    g.def( //
        py::init([](std::shared_ptr<py_db_t> py_db,
                    std::optional<std::string> index,
                    std::optional<std::string> vertices_attrs,
                    std::optional<std::string> relations_attrs,
                    bool directed = false,
                    bool multi = false,
                    bool loops = false) {
            //
            if (!py_db)
                return std::shared_ptr<py_graph_t> {};

            auto net_ptr = std::make_shared<py_graph_t>();
            net_ptr->py_db_ptr = py_db;
            net_ptr->is_directed = directed;
            net_ptr->is_multi = multi;
            net_ptr->allow_self_loops = loops;

            // Attach the primary collection
            database_t& db = py_db->native;
            net_ptr->index = db.find_or_create(index ? index->c_str() : "").throw_or_release();

            // Attach the additional collections
            if (vertices_attrs)
                net_ptr->vertices_attrs =
                    db.find_or_create<docs_collection_t>(vertices_attrs->c_str()).throw_or_release();
            if (relations_attrs)
                net_ptr->relations_attrs =
                    db.find_or_create<docs_collection_t>(relations_attrs->c_str()).throw_or_release();

            return net_ptr;
        }),
        py::arg("db"),
        py::arg("index") = std::nullopt,
        py::arg("vertices") = std::nullopt,
        py::arg("relations") = std::nullopt,
        py::arg("directed") = false,
        py::arg("multi") = false,
        py::arg("loops") = false);

    // Counting nodes edges and neighbors
    // https://networkx.org/documentation/stable/reference/classes/graph.html#counting-nodes-edges-and-neighbors
    // https://networkx.org/documentation/stable/reference/classes/multidigraph.html#counting-nodes-edges-and-neighbors
    g.def(
        "order",
        [](py_graph_t& g) { return g.index.size(); },
        "Returns the number of nodes in the graph.");
    g.def(
        "number_of_nodes",
        [](py_graph_t& g) { return g.index.size(); },
        "Returns the number of nodes in the graph.");
    g.def(
        "__len__",
        [](py_graph_t& g) { return g.index.size(); },
        "Returns the number of nodes in the graph.");
    g.def_property_readonly(
        "degree",
        [](py_graph_t& g) {
            auto degs_ptr = std::make_unique<degree_view_t>();
            degs_ptr->net_ptr = g.shared_from_this();
            degs_ptr->roles = ustore_vertex_role_any_k;
            return degs_ptr;
        },
        "A DegreeView for the graph.");
    g.def_property_readonly(
        "in_degree",
        [](py_graph_t& g) {
            auto degs_ptr = std::make_unique<degree_view_t>();
            degs_ptr->net_ptr = g.shared_from_this();
            degs_ptr->roles = ustore_vertex_target_k;
            return degs_ptr;
        },
        "A DegreeView with the number incoming edges for each Vertex.");
    g.def_property_readonly(
        "out_degree",
        [](py_graph_t& g) {
            auto degs_ptr = std::make_unique<degree_view_t>();
            degs_ptr->net_ptr = g.shared_from_this();
            degs_ptr->roles = ustore_vertex_source_k;
            return degs_ptr;
        },
        "A DegreeView with the number outgoing edges for each Vertex.");
    g.def(
        "size",
        [](py_graph_t& g, std::string weight) {
            if (!weight.size())
                return g.ref().number_of_edges();

            std::size_t size = 0;
            auto stream = g.ref().edges(ustore_vertex_source_k).throw_or_release().begin();
            while (!stream.is_end()) {
                auto edge_ids = stream.edges_batch().edge_ids.immutable();
                auto attrs = read_attributes(g.relations_attrs, edge_ids, weight.c_str());
                for (std::size_t i = 0; i != edge_ids.size(); ++i) {
                    if (attrs[i] && attrs[i].size()) {
                        std::size_t number = 0;
                        auto result = std::from_chars((char*)attrs[i].begin(), (char*)attrs[i].end(), number);
                        if (result.ec == std::errc())
                            size += number;
                        else if (result.ec == std::errc::invalid_argument)
                            throw std::runtime_error("Unsupported Type");
                        else
                            throw std::runtime_error("Failed To Read Attribute");
                    }
                    else
                        size += 1;
                }
                stream.seek_to_next_batch();
            }
            return size;
        },
        py::arg("weight") = "",
        "Returns the number of attributed edges.");
    g.def(
        "number_of_edges",
        [](py_graph_t& g, ustore_key_t v1, ustore_key_t v2) {
            return g.ref().edges_between(v1, v2).throw_or_release().size();
        },
        "Returns the number of edges between two nodes.");

    g.def(
        "number_of_edges",
        [](py_graph_t& g) { return g.ref().number_of_edges(); },
        "Returns edges count");

    // Reporting nodes edges and neighbors
    // https://networkx.org/documentation/stable/reference/classes/multidigraph.html#reporting-nodes-edges-and-neighbors
    g.def_property_readonly(
        "nodes",
        [](py_graph_t& g) {
            blobs_range_t members(g.index.db(), g.index.txn(), 0, g.index);
            keys_range_t keys {members};
            auto range = std::make_shared<nodes_range_t>(keys, g.vertices_attrs);
            return range;
        },
        "A NodeView of the graph.");
    g.def(
        "__iter__",
        [](py_graph_t& g) { throw_not_implemented(); },
        "Iterate over the nodes.");
    g.def(
        "has_node",
        [](py_graph_t& g, ustore_key_t v) {
            auto maybe = g.ref().contains(v);
            maybe.throw_unhandled();
            return *maybe;
        },
        py::arg("n"),
        "Returns True if the graph contains the node n.");
    g.def(
        "__contains__",
        [](py_graph_t& g, ustore_key_t v) { return g.ref().contains(v).throw_or_release(); },
        py::arg("n"),
        "Returns True if the graph contains the node n.");

    g.def(
        "set_node_attributes",
        [](py_graph_t& g, py::object obj, std::optional<std::string> name) {
            std::string json_to_merge;

            if (PyDict_Check(obj.ptr())) {
                PyObject *key, *value;
                Py_ssize_t pos = 0;
                while (PyDict_Next(obj.ptr(), &pos, &key, &value)) {
                    json_to_merge.clear();
                    auto vertex = py_to_scalar<ustore_key_t>(key);
                    if (!PyDict_Check(value)) {
                        fmt::format_to(std::back_inserter(json_to_merge), "{{\"{}\":", name.value());
                        to_string(value, json_to_merge);
                        json_to_merge += "}";
                    }
                    else
                        to_string(value, json_to_merge);

                    g.vertices_attrs[vertex].merge(json_to_merge.c_str());
                }
            }
            else {
                if (!name)
                    throw std::invalid_argument("Invalid Argument");

                fmt::format_to(std::back_inserter(json_to_merge), "{{\"{}\":", name.value());
                to_string(obj.ptr(), json_to_merge);
                json_to_merge += "}";

                auto stream = g.ref().vertex_stream().throw_or_release();
                while (!stream.is_end()) {
                    g.vertices_attrs[stream.keys_batch().strided()].merge(json_to_merge.c_str());
                    stream.seek_to_next_batch();
                }
            }
        },
        py::arg("values"),
        py::arg("name") = std::nullopt);

    g.def(
        "get_node_attributes",
        [](py_graph_t& g, std::string& name) {
            std::unordered_map<ustore_key_t, py::object> map;
            auto stream = g.ref().vertex_stream().throw_or_release();
            while (!stream.is_end()) {
                auto keys = stream.keys_batch().strided();
                auto attrs = read_attributes(g.vertices_attrs, keys, name.c_str());
                for (std::size_t i = 0; i != keys.size(); ++i)
                    map[keys[i]] = py::reinterpret_steal<py::object>(from_json(json_t::parse(attrs[i])));

                stream.seek_to_next_batch();
            }
            return py::cast(map);
        },
        py::arg("name"));

    g.def_property_readonly("edges", [](py_graph_t& g) {
        auto edges_ptr = std::make_shared<edges_range_t>();
        edges_ptr->net_ptr = g.shared_from_this();
        return edges_ptr;
    });

    g.def(
        "has_edge",
        [](py_graph_t& g, ustore_key_t v1, ustore_key_t v2) {
            return g.ref().edges_between(v1, v2).throw_or_release().size() != 0;
        },
        py::arg("u"),
        py::arg("v"));
    g.def(
        "has_edge",
        [](py_graph_t& g, ustore_key_t v1, ustore_key_t v2, ustore_key_t e) {
            auto ids = g.ref().edges_between(v1, v2).throw_or_release().edge_ids;
            return std::find(ids.begin(), ids.end(), e) != ids.end();
        },
        py::arg("u"),
        py::arg("v"),
        py::arg("key"));
    g.def(
        "get_edge_data",
        [](py_graph_t& g, ustore_key_t v1, ustore_key_t v2, py::object default_value) -> py::object {
            std::string default_value_str;
            to_string(default_value.ptr(), default_value_str);
            auto edges = g.ref().edges_between(v1, v2).throw_or_release();
            if (!edges.size())
                return py::none();

            auto edge_ids = edges.edge_ids.immutable();
            auto attrs = read_attributes(g.relations_attrs, edge_ids, nullptr);
            if (attrs[0] && attrs[0].size())
                return py::reinterpret_steal<py::object>(from_json(json_t::parse(attrs[0])));
            if (default_value_str.size())
                return py::reinterpret_steal<py::object>(from_json(json_t::parse(default_value_str)));
            return py::dict();
        },
        py::arg("u"),
        py::arg("v"),
        py::arg("default") = py::none());

    g.def(
        "get_edge_attributes",
        [](py_graph_t& g, std::string& name) {
            auto hash = [](py::tuple const& tuple) {
                return py_to_scalar<ustore_key_t>(PyTuple_GetItem(tuple.ptr(), 2));
            };
            std::unordered_map<py::tuple, py::object, decltype(hash)> map(0, hash);

            auto stream = g.ref().edges().throw_or_release().begin();
            while (!stream.is_end()) {
                auto edges = stream.edges_batch();
                map.reserve(edges.size());
                auto sources = edges.source_ids;
                auto targets = edges.target_ids;
                auto edge_ids = edges.edge_ids.immutable();
                auto attrs = read_attributes(g.relations_attrs, edge_ids, name.c_str());
                for (std::size_t i = 0; i != edge_ids.size(); ++i) {
                    auto key = py::make_tuple(sources[i], targets[i], edge_ids[i]);
                    if (attrs[i] && attrs[i].size())
                        map[key] = py::reinterpret_steal<py::object>(from_json(json_t::parse(attrs[i])));
                    else
                        map[key] = py::dict();
                }

                stream.seek_to_next_batch();
            }
            return py::cast(map);
        },
        py::arg("name"));

    g.def(
        "set_edge_attributes",
        [](py_graph_t& g, py::object obj, std::optional<std::string> name) {
            std::string json_to_merge;

            if (PyDict_Check(obj.ptr())) {
                PyObject *key, *value;
                Py_ssize_t pos = 0;
                while (PyDict_Next(obj.ptr(), &pos, &key, &value)) {
                    json_to_merge.clear();
                    if (!PyTuple_Check(key) || PyTuple_Size(key) != 3)
                        throw std::invalid_argument("Invalid Argument");
                    auto attr_key = py_to_scalar<ustore_key_t>(PyTuple_GetItem(key, 2));
                    if (!PyDict_Check(value)) {
                        fmt::format_to(std::back_inserter(json_to_merge), "{{\"{}\":", name.value());
                        to_string(value, json_to_merge);
                        json_to_merge += "}";
                    }
                    else
                        to_string(value, json_to_merge);

                    g.relations_attrs[attr_key].merge(json_to_merge.c_str());
                }
            }
            else {
                if (!name)
                    throw std::invalid_argument("Invalid Argument");

                fmt::format_to(std::back_inserter(json_to_merge), "{{\"{}\":", name.value());
                to_string(obj.ptr(), json_to_merge);
                json_to_merge += "}";

                auto stream = g.ref().edges().throw_or_release().begin();
                while (!stream.is_end()) {
                    g.relations_attrs[stream.edges_batch().edge_ids].merge(json_to_merge.c_str());
                    stream.seek_to_next_batch();
                }
            }
        },
        py::arg("values"),
        py::arg("name") = std::nullopt);

    g.def(
        "__getitem__",
        [](py_graph_t& g, ustore_key_t n) { return wrap_into_buffer(g, g.ref().neighbors(n).throw_or_release()); },
        py::arg("n"),
        "Returns an iterable of incoming and outgoing nodes of n. Potentially with duplicates.");
    g.def(
        "neighbors",
        [](py_graph_t& g, ustore_key_t n) { return wrap_into_buffer(g, g.ref().neighbors(n).throw_or_release()); },
        py::arg("n"),
        "Returns an iterable of incoming and outgoing nodes of n. Potentially with duplicates.");
    g.def(
        "successors",
        [](py_graph_t& g, ustore_key_t n) { return wrap_into_buffer(g, g.ref().successors(n).throw_or_release()); },
        py::arg("n"),
        "Returns an iterable of successor nodes of n.");
    g.def(
        "predecessors",
        [](py_graph_t& g, ustore_key_t n) { return wrap_into_buffer(g, g.ref().predecessors(n).throw_or_release()); },
        py::arg("n"),
        "Returns an iterable of follower nodes of n.");
    g.def(
        "nbunch_iter",
        [](py_graph_t& g, PyObject* vs) {
            auto ids_handle = py_buffer(vs);
            auto ids = py_strided_range<ustore_key_t const>(ids_handle);
            auto result = g.ref().contains(ids).throw_or_release();

            py::array_t<ustore_key_t> res_array(ids.size());
            size_t j = 0;
            for (size_t i = 0; i < ids.size(); i++)
                if (result[i])
                    res_array.mutable_at(j++) = ids[i];
            res_array.resize(py::array::ShapeContainer({std::max<size_t>(j, 1) - 1}));

            return res_array;
        },
        "Filters given nodes which are also in the graph and returns an iterator over them.");

    // Adding and Removing Nodes and Edges
    // https://networkx.org/documentation/stable/reference/classes/multidigraph.html#adding-and-removing-nodes-and-edges
    g.def(
        "add_node",
        [](py_graph_t& g, ustore_key_t v, py::kwargs const& attrs) {
            g.ref().upsert_vertex(v).throw_unhandled();
            if (!attrs.size())
                return;
            std::string json_str;
            to_string(attrs.ptr(), json_str);
            g.vertices_attrs[v].assign(value_view_t(json_str)).throw_unhandled();
        },
        py::arg("v_to_upsert"));
    g.def(
        "add_edge",
        [](py_graph_t& g, ustore_key_t v1, ustore_key_t v2) {
            g.ref().upsert_edge(edge_t {v1, v2}).throw_unhandled();
        },
        py::arg("u_for_edge"),
        py::arg("v_for_edge"));
    g.def(
        "add_edge",
        [](py_graph_t& g, ustore_key_t v1, ustore_key_t v2, ustore_key_t e, py::kwargs const& attrs) {
            g.ref().upsert_edge(edge_t {v1, v2, e}).throw_unhandled();
            if (!attrs.size())
                return;
            std::string json_str;
            to_string(attrs.ptr(), json_str);
            g.relations_attrs[e].assign(value_view_t(json_str)).throw_unhandled();
        },
        py::arg("u_for_edge"),
        py::arg("v_for_edge"),
        py::arg("id"));
    g.def(
        "remove_node",
        [](py_graph_t& g, ustore_key_t v) {
            g.ref().remove_vertex(v).throw_unhandled();
            if (g.vertices_attrs.db())
                g.vertices_attrs[v].clear().throw_unhandled();
        },
        py::arg("v_to_remove"));
    g.def(
        "remove_edge",
        [](py_graph_t& g, ustore_key_t v1, ustore_key_t v2) {
            g.ref().remove_edge(edge_t {v1, v2}).throw_unhandled();
        },
        py::arg("u_for_edge"),
        py::arg("v_for_edge"));
    g.def(
        "remove_edge",
        [](py_graph_t& g, ustore_key_t v1, ustore_key_t v2, ustore_key_t e) {
            g.ref().remove_edge(edge_t {v1, v2, e}).throw_unhandled();
        },
        py::arg("u_for_edge"),
        py::arg("v_for_edge"),
        py::arg("key"));
    g.def("add_nodes_from", [](py_graph_t& g, py::object vs, py::kwargs const& attrs) {
        if (PyObject_CheckBuffer(vs.ptr())) {
            py_buffer_t buf = py_buffer(vs.ptr());
            if (!can_cast_internal_scalars<ustore_key_t>(buf))
                throw std::invalid_argument("Expecting @c ustore_key_t scalars in zero-copy interface");
            auto vertices = py_strided_range<ustore_key_t const>(buf);
            g.ref().upsert_vertices(vertices).throw_unhandled();
            if (!attrs.size())
                return;
            std::string json_str;
            to_string(attrs.ptr(), json_str);
            g.vertices_attrs[vertices].assign(value_view_t(json_str)).throw_unhandled();
        }
        else {
            if (!PySequence_Check(vs.ptr()))
                throw std::invalid_argument("Nodes Must Be Sequence");
            std::vector<ustore_key_t> vertices(PySequence_Size(vs.ptr()));
            py_transform_n(vs.ptr(), &py_to_scalar<ustore_key_t>, vertices.begin());
            g.ref().upsert_vertices(vertices).throw_unhandled();
            if (!attrs.size())
                return;
            std::string json_str;
            to_string(attrs.ptr(), json_str);
            g.vertices_attrs[vertices].assign(value_view_t(json_str)).throw_unhandled();
        }
    });
    g.def(
        "add_edges_from",
        [](py_graph_t& g, py::object adjacency_list) {
            g.ref().upsert_edges(parsed_adjacency_list_t(adjacency_list.ptr())).throw_unhandled();
        },
        py::arg("ebunch_to_add"),
        "Adds an adjacency list (in a form of 2 or 3 columnar matrix) to the graph.");
    g.def("remove_nodes_from", [](py_graph_t& g, py::object vs) {
        if (PyObject_CheckBuffer(vs.ptr())) {
            py_buffer_t buf = py_buffer(vs.ptr());
            if (!can_cast_internal_scalars<ustore_key_t>(buf))
                throw std::invalid_argument("Expecting @c ustore_key_t scalars in zero-copy interface");
            auto vertices = py_strided_range<ustore_key_t const>(buf);
            g.ref().remove_vertices(vertices).throw_unhandled();
            if (g.vertices_attrs.db())
                g.vertices_attrs[vertices].clear().throw_unhandled();
        }
        else {
            if (!PySequence_Check(vs.ptr()))
                throw std::invalid_argument("Nodes Must Be Sequence");
            std::vector<ustore_key_t> vertices(PySequence_Size(vs.ptr()));
            py_transform_n(vs.ptr(), &py_to_scalar<ustore_key_t>, vertices.begin());
            g.ref().remove_vertices(vertices).throw_unhandled();
            if (g.vertices_attrs.db())
                g.vertices_attrs[vertices].clear().throw_unhandled();
        }
    });
    g.def(
        "remove_edges_from",
        [](py_graph_t& g, py::object adjacency_list) {
            g.ref().remove_edges(parsed_adjacency_list_t(adjacency_list.ptr())).throw_unhandled();
        },
        py::arg("ebunch"),
        "Removes all edges in supplied adjacency list (in a form of 2 or 3 columnar matrix) from the graph.");

    g.def(
        "add_edges_from",
        [](py_graph_t& g, py::object v1s, py::object v2s, py::object es, py::kwargs const& attrs) {
            g.ref().upsert_edges(parsed_adjacency_list_t(v1s.ptr(), v2s.ptr(), es.ptr())).throw_unhandled();

            if (!attrs.size())
                return;

            std::string json_str;
            to_string(attrs.ptr(), json_str);
            if (PyObject_CheckBuffer(es.ptr())) {
                py_buffer_t buf = py_buffer(es.ptr());
                if (!can_cast_internal_scalars<ustore_key_t>(buf))
                    throw std::invalid_argument("Expecting @c ustore_key_t scalars in zero-copy interface");
                auto edge_ids = py_strided_range<ustore_key_t const>(buf);
                g.relations_attrs[edge_ids].assign(value_view_t(json_str)).throw_unhandled();
            }
            else {
                if (!PySequence_Check(es.ptr()))
                    throw std::invalid_argument("Edge Ids Must Be Sequence");
                std::vector<ustore_key_t> edge_ids(PySequence_Size(es.ptr()));
                py_transform_n(es.ptr(), &py_to_scalar<ustore_key_t>, edge_ids.begin());
                g.relations_attrs[edge_ids].assign(value_view_t(json_str)).throw_unhandled();
            }
        },
        py::arg("us"),
        py::arg("vs"),
        py::arg("keys") = nullptr,
        "Adds edges from members of the first array to members of the second array.");
    g.def(
        "remove_edges_from",
        [](py_graph_t& g, py::object v1s, py::object v2s, py::object es) {
            g.ref().remove_edges(parsed_adjacency_list_t(v1s.ptr(), v2s.ptr(), es.ptr())).throw_unhandled();

            if (!g.relations_attrs.db())
                return;

            if (PyObject_CheckBuffer(es.ptr())) {
                py_buffer_t buf = py_buffer(es.ptr());
                if (!can_cast_internal_scalars<ustore_key_t>(buf))
                    throw std::invalid_argument("Expecting @c ustore_key_t scalars in zero-copy interface");
                auto edge_ids = py_strided_range<ustore_key_t const>(buf);
                g.relations_attrs[edge_ids].clear().throw_unhandled();
            }
            else {
                if (!PySequence_Check(es.ptr()))
                    throw std::invalid_argument("Edge Ids Must Be Sequence");
                std::vector<ustore_key_t> edge_ids(PySequence_Size(es.ptr()));
                py_transform_n(es.ptr(), &py_to_scalar<ustore_key_t>, edge_ids.begin());
                g.relations_attrs[edge_ids].clear().throw_unhandled();
            }
        },
        py::arg("us"),
        py::arg("vs"),
        py::arg("keys") = nullptr,
        "Removes edges from members of the first array to members of the second array.");

    g.def(
        "clear_edges",
        [](py_graph_t& g) {
            g.index.clear_values().throw_unhandled();
            if (g.relations_attrs.db())
                g.relations_attrs.clear_values().throw_unhandled();
        },
        "Removes edges from the graph.");
    g.def(
        "clear",
        [](py_graph_t& g) {
            g.index.clear();
            if (g.vertices_attrs.db())
                g.vertices_attrs.clear().throw_unhandled();
            if (g.relations_attrs.db())
                g.relations_attrs.clear().throw_unhandled();
        },
        "Removes both vertices and edges from the graph.");

    g.def(
        "community_louvain",
        [](py_graph_t& g) {
            graph_collection_t graph = g.ref();
            auto partition = best_partition(graph);
            return py::cast(partition);
        },
        "Community Louvain.");

    // Making copies and subgraphs
    // https://networkx.org/documentation/stable/reference/classes/multidigraph.html#making-copies-and-subgraphs
    g.def("copy", [](py_graph_t& g) { throw_not_implemented(); });
    g.def("to_directed", [](py_graph_t& g) { throw_not_implemented(); });
    g.def("to_undirected", [](py_graph_t& g) { throw_not_implemented(); });
    g.def("reverse", [](py_graph_t& g) { throw_not_implemented(); });
    g.def("subgraph", [](py_graph_t& g) { throw_not_implemented(); });
    g.def("edge_subgraph", [](py_graph_t& g) { throw_not_implemented(); });
    g.def(
        "subgraph",
        [](py_graph_t& g, py::object ns) { throw_not_implemented(); },
        "Returns a subgraph in a form of an adjacency list with 3 columns, where every edge (row) "
        "contains at least one vertex from the supplied list. Some edges may be duplicated.");
    g.def(
        "subgraph",
        [](py_graph_t& g, ustore_key_t n, std::size_t hops) { throw_not_implemented(); },
        "Returns a subgraph in a form of an adjacency list with 3 columns, where every edge (row) "
        "contains at least one vertex from the supplied list at a distance withing a given number "
        "`hops` from the supplied `n`.");

    // Free-standing Functions and Properties
    // https://networkx.org/documentation/stable/reference/functions.html#graph
    // https://networkx.org/documentation/stable/reference/generated/networkx.classes.function.density.html
    // https://networkx.org/documentation/stable/reference/generated/networkx.classes.function.is_directed.html?highlight=is_directed
    g.def("is_directed", [](py_graph_t& g) { return g.is_directed; });
    g.def("is_multigraph", [](py_graph_t& g) { return g.is_multi; });
    g.def_property_readonly("allows_loops", [](py_graph_t& g) { return g.allow_self_loops; });
    m.def("is_directed", [](py_graph_t& g) { return g.is_directed; });
    m.def("is_multi", [](py_graph_t& g) { return g.is_multi; });
    m.def("allows_loops", [](py_graph_t& g) { return g.allow_self_loops; });
    m.def("density", [](py_graph_t& g) {
        throw_not_implemented();
        return 0.0;
    });

    // Reading and Writing Graphs
    // https://networkx.org/documentation/stable/reference/readwrite/
    // https://networkx.org/documentation/stable/reference/readwrite/adjlist.html
    // https://networkx.org/documentation/stable/reference/readwrite/json_graph.html
    m.def(
        "write_adjlist",
        [](py_graph_t& g,
           std::string const& path,
           std::string const& comments,
           std::string const& delimiter,
           std::string const& encoding) { return; },
        py::arg("G"),
        py::arg("path"),
        py::arg("comments") = "#",
        py::arg("delimiter") = " ",
        py::arg("encoding") = "utf-8");
}
