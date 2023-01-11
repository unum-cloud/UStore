#include "pybind.hpp"
#include "crud.hpp"
#include "cast_args.hpp"

using namespace unum::ukv::pyb;
using namespace unum::ukv;
using namespace unum;

struct degree_view_t {
    std::weak_ptr<py_graph_t> net_ptr;
    ukv_vertex_role_t roles = ukv_vertex_role_any_k;
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

void ukv::wrap_networkx(py::module& m) {

    auto degs = py::class_<degree_view_t>(m, "DegreeView", py::module_local());
    degs.def("__getitem__", [](degree_view_t& degs, ukv_key_t v) {
        py_graph_t& g = *degs.net_ptr.lock().get();
        auto result = g.ref().degree(v, degs.roles).throw_or_release();
        return result;
    });
    degs.def("__getitem__", [](degree_view_t& degs, py::object vs) {
        py_graph_t& g = *degs.net_ptr.lock().get();
        auto ids_handle = py_buffer(vs.ptr());
        auto ids = py_strided_range<ukv_key_t const>(ids_handle);
        auto result = g.ref().degrees(ids, {{&degs.roles, 0u}, ids.size()}).throw_or_release();
        return wrap_into_buffer<ukv_vertex_degree_t const>(
            g,
            strided_range<ukv_vertex_degree_t const>(result.begin(), result.end()));
    });

    auto edges_range = py::class_<range_gt<graph_stream_t>>(m, "EdgesRange", py::module_local());
    edges_range.def("__iter__", [](range_gt<graph_stream_t>& range) { return std::move(range).begin(); });

    auto edges_stream = py::class_<graph_stream_t>(m, "EdgesStream", py::module_local());
    edges_stream.def("__next__", [](graph_stream_t& stream) {
        if (stream.is_end())
            throw py::stop_iteration();
        auto edge = stream.edge();
        ++stream;
        return py::make_tuple(edge.source_id, edge.target_id);
    });

    auto g = py::class_<py_graph_t, std::shared_ptr<py_graph_t>>(m, "Network", py::module_local());
    g.def( //
        py::init([](std::shared_ptr<py_db_t> py_db,
                    std::optional<std::string> index,
                    std::optional<std::string> sources_attrs,
                    std::optional<std::string> targets_attrs,
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
            if (sources_attrs)
                net_ptr->sources_attrs = db.find_or_create(sources_attrs->c_str()).throw_or_release();
            if (targets_attrs)
                net_ptr->sources_attrs = db.find_or_create(targets_attrs->c_str()).throw_or_release();
            if (relations_attrs)
                net_ptr->sources_attrs = db.find_or_create(relations_attrs->c_str()).throw_or_release();

            return net_ptr;
        }),
        py::arg("db"),
        py::arg("index"),
        py::arg("sources") = std::nullopt,
        py::arg("targets") = std::nullopt,
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
            degs_ptr->roles = ukv_vertex_role_any_k;
            return degs_ptr;
        },
        "A DegreeView for the graph.");
    g.def_property_readonly(
        "in_degree",
        [](py_graph_t& g) {
            auto degs_ptr = std::make_unique<degree_view_t>();
            degs_ptr->net_ptr = g.shared_from_this();
            degs_ptr->roles = ukv_vertex_target_k;
            return degs_ptr;
        },
        "A DegreeView with the number incoming edges for each Vertex.");
    g.def_property_readonly(
        "out_degree",
        [](py_graph_t& g) {
            auto degs_ptr = std::make_unique<degree_view_t>();
            degs_ptr->net_ptr = g.shared_from_this();
            degs_ptr->roles = ukv_vertex_source_k;
            return degs_ptr;
        },
        "A DegreeView with the number outgoing edges for each Vertex.");
    g.def(
        "size",
        [](py_graph_t& g) { return g.relations_attrs.size(); },
        "Returns the number of attributed edges.");
    g.def(
        "number_of_edges",
        [](py_graph_t& g, ukv_key_t v1, ukv_key_t v2) { return g.ref().edges(v1, v2).throw_or_release().size(); },
        "Returns the number of edges between two nodes.");

    g.def(
        "number_of_edges",
        [](py_graph_t& g) {
            auto present_edges = g.ref().edges(ukv_vertex_source_k).throw_or_release();
            auto present_it = std::move(present_edges).begin();
            auto count_results = 0;
            for (; !present_it.is_end(); ++present_it)
                ++count_results;
            return count_results;
        },
        "Returns edges count");

    // Reporting nodes edges and neighbors
    // https://networkx.org/documentation/stable/reference/classes/multidigraph.html#reporting-nodes-edges-and-neighbors
    g.def(
        "nodes",
        [](py_graph_t& g) {
            blobs_range_t members(g.index.db(), g.index.txn(), g.index);
            keys_range_t range {members};
            return py::cast(std::make_unique<keys_range_t>(range));
        },
        "A NodeView of the graph.");
    g.def(
        "__iter__",
        [](py_graph_t& g) { throw_not_implemented(); },
        "Iterate over the nodes.");
    g.def(
        "has_node",
        [](py_graph_t& g, ukv_key_t v) {
            auto maybe = g.ref().contains(v);
            maybe.throw_unhandled();
            return *maybe;
        },
        py::arg("n"),
        "Returns True if the graph contains the node n.");
    g.def(
        "__contains__",
        [](py_graph_t& g, ukv_key_t v) { return g.ref().contains(v).throw_or_release(); },
        py::arg("n"),
        "Returns True if the graph contains the node n.");

    g.def("edges", [](py_graph_t& g) { return g.ref().edges(ukv_vertex_source_k).throw_or_release(); });

    g.def(
        "has_edge",
        [](py_graph_t& g, ukv_key_t v1, ukv_key_t v2) { return g.ref().edges(v1, v2).throw_or_release().size() != 0; },
        py::arg("u"),
        py::arg("v"));
    g.def(
        "has_edge",
        [](py_graph_t& g, ukv_key_t v1, ukv_key_t v2, ukv_key_t e) {
            auto ids = g.ref().edges(v1, v2).throw_or_release().edge_ids;
            return std::find(ids.begin(), ids.end(), e) != ids.end();
        },
        py::arg("u"),
        py::arg("v"),
        py::arg("key"));
    g.def(
        "get_edge_data",
        [](py_graph_t& g, ukv_key_t v1, ukv_key_t v2) { throw_not_implemented(); },
        py::arg("u"),
        py::arg("v"));

    g.def(
        "neighbors",
        [](py_graph_t& g, ukv_key_t n) { return wrap_into_buffer(g, g.ref().neighbors(n).throw_or_release()); },
        py::arg("n"),
        "Returns an iterable of incoming and outgoing nodes of n. Potentially with duplicates.");
    g.def(
        "successors",
        [](py_graph_t& g, ukv_key_t n) { return wrap_into_buffer(g, g.ref().successors(n).throw_or_release()); },
        py::arg("n"),
        "Returns an iterable of successor nodes of n.");
    g.def(
        "predecessors",
        [](py_graph_t& g, ukv_key_t n) { return wrap_into_buffer(g, g.ref().predecessors(n).throw_or_release()); },
        py::arg("n"),
        "Returns an iterable of follower nodes of n.");
    g.def(
        "nbunch_iter",
        [](py_graph_t& g, PyObject* vs) {
            auto ids_handle = py_buffer(vs);
            auto ids = py_strided_range<ukv_key_t const>(ids_handle);
            auto result = g.ref().contains(ids).throw_or_release();

            py::array_t<ukv_key_t> res_array(ids.size());
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
        [](py_graph_t& g, ukv_key_t v) { g.ref().upsert_vertex(v).throw_unhandled(); },
        py::arg("v_to_upsert"));
    g.def(
        "add_edge",
        [](py_graph_t& g, ukv_key_t v1, ukv_key_t v2) {
            g.ref().upsert_edge(edge_t {v1, v2}).throw_unhandled();
        },
        py::arg("u_for_edge"),
        py::arg("v_for_edge"));
    g.def(
        "add_edge",
        [](py_graph_t& g, ukv_key_t v1, ukv_key_t v2, ukv_key_t e) {
            g.ref().upsert_edge(edge_t {v1, v2, e}).throw_unhandled();
        },
        py::arg("u_for_edge"),
        py::arg("v_for_edge"),
        py::arg("key"));
    g.def(
        "remove_node",
        [](py_graph_t& g, ukv_key_t v) { g.ref().remove_vertex(v).throw_unhandled(); },
        py::arg("v_to_remove"));
    g.def(
        "remove_edge",
        [](py_graph_t& g, ukv_key_t v1, ukv_key_t v2) {
            g.ref().remove_edge(edge_t {v1, v2}).throw_unhandled();
        },
        py::arg("u_for_edge"),
        py::arg("v_for_edge"));
    g.def(
        "remove_edge",
        [](py_graph_t& g, ukv_key_t v1, ukv_key_t v2, ukv_key_t e) {
            g.ref().remove_edge(edge_t {v1, v2, e}).throw_unhandled();
        },
        py::arg("u_for_edge"),
        py::arg("v_for_edge"),
        py::arg("key"));
    g.def("add_nodes_from", [](py_graph_t& g, py::object vs) {
        if (PyObject_CheckBuffer(vs.ptr())) {
            py_buffer_t buf = py_buffer(vs.ptr());
            if (!can_cast_internal_scalars<ukv_key_t>(buf))
                throw std::invalid_argument("Expecting @c ukv_key_t scalars in zero-copy interface");
            auto vertices = py_strided_range<ukv_key_t const>(buf);
            g.ref().upsert_vertices(vertices).throw_unhandled();
        }
        else {
            if (!PySequence_Check(vs.ptr()))
                throw std::invalid_argument("Nodes Must Be Sequence");
            std::vector<ukv_key_t> vertices(PySequence_Size(vs.ptr()));
            py_transform_n(vs.ptr(), &py_to_scalar<ukv_key_t>, vertices.begin());
            g.ref().upsert_vertices(vertices).throw_unhandled();
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
            if (!can_cast_internal_scalars<ukv_key_t>(buf))
                throw std::invalid_argument("Expecting @c ukv_key_t scalars in zero-copy interface");
            auto vertices = py_strided_range<ukv_key_t const>(buf);
            g.ref().remove_vertices(vertices).throw_unhandled();
        }
        else {
            if (!PySequence_Check(vs.ptr()))
                throw std::invalid_argument("Nodes Must Be Sequence");
            std::vector<ukv_key_t> vertices(PySequence_Size(vs.ptr()));
            py_transform_n(vs.ptr(), &py_to_scalar<ukv_key_t>, vertices.begin());
            g.ref().remove_vertices(vertices).throw_unhandled();
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
        [](py_graph_t& g, py::object v1s, py::object v2s, py::object es) {
            g.ref().upsert_edges(parsed_adjacency_list_t(v1s.ptr(), v2s.ptr(), es.ptr())).throw_unhandled();
        },
        py::arg("us"),
        py::arg("vs"),
        py::arg("keys") = nullptr,
        "Adds edges from members of the first array to members of the second array.");
    g.def(
        "remove_edges_from",
        [](py_graph_t& g, py::object v1s, py::object v2s, py::object es) {
            g.ref().remove_edges(parsed_adjacency_list_t(v1s.ptr(), v2s.ptr(), es.ptr())).throw_unhandled();
        },
        py::arg("us"),
        py::arg("vs"),
        py::arg("keys") = nullptr,
        "Removes edges from members of the first array to members of the second array.");

    g.def("clear_edges", [](py_graph_t& g) { throw_not_implemented(); });
    g.def(
        "clear",
        [](py_graph_t& g) {
            // database_t& db = g.db_ptr->native;
            // db.clear(g.index);
            // db.clear(g.sources_attrs);
            // db.clear(g.targets_attrs);
            // db.clear(g.relations_attrs);
            // throw_not_implemented();
        },
        "Removes both vertices and edges from the graph.");

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
        [](py_graph_t& g, ukv_key_t n, std::size_t hops) { throw_not_implemented(); },
        "Returns a subgraph in a form of an adjacency list with 3 columns, where every edge (row) "
        "contains at least one vertex from the supplied list at a distance withing a given number "
        "`hops` from the supplied `n`.");

    // Free-standing Functions and Properties
    // https://networkx.org/documentation/stable/reference/functions.html#graph
    // https://networkx.org/documentation/stable/reference/generated/networkx.classes.function.density.html
    // https://networkx.org/documentation/stable/reference/generated/networkx.classes.function.is_directed.html?highlight=is_directed
    g.def_property_readonly("is_directed", [](py_graph_t& g) { return g.is_directed; });
    g.def_property_readonly("is_multi", [](py_graph_t& g) { return g.is_multi; });
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
