/**
 * @brief Python bindings for a Graph index, that mimics NetworkX.
 * Is similar in it's purpose to a pure-Python NetworkXum:
 * https://github.com/unum-cloud/NetworkXum
 *
 * @section Supported Graph Types
 * We support all the NetworkX graph kinds and more:
 * https://networkx.org/documentation/stable/reference/classes/index.html#which-graph-class-should-i-use
 *
 *      | Class          | Type         | Self-loops | Parallel edges |
 *      | Graph          | undirected   | Yes        | No             |
 *      | DiGraph        | directed     | Yes        | No             |
 *      | MultiGraph     | undirected   | Yes        | Yes            |
 *      | MultiDiGraph   | directed     | Yes        | Yes            |
 *
 * Aside from those, you can instantiate the most generic `ukv.Network`,
 * controlling whether graph should be directed, allow loops, or have
 * attrs in source/target vertices or edges.
 *
 * @section Interface
 * Primary single element methods:
 *      * add_edge(first, second, key?, attrs?)
 *      * remove_edge(first, second, key?, attrs?)
 * Additional batch methods:
 *      * add_edges_from(firsts, seconds, keys?, attrs?)
 *      * remove_edges_from(firsts, seconds, keys?, attrs?)
 * Intentionally not implemented:
 *      * __len__() ~ It's hard to consistently estimate the collection.
 */

#include "pybind.hpp"
#include "ukv/ukv.hpp"

using namespace unum::ukv;
using namespace unum;

struct degree_view_t : public std::enable_shared_from_this<degree_view_t> {
    std::shared_ptr<py_graph_t> net_ptr;
    ukv_vertex_role_t roles = ukv_vertex_role_any_k;
};

template <typename element_at>
py::handle wrap_into_buffer(py_graph_t& g, strided_range_gt<element_at> range) {
    printf("received %i strided objects\n", (int)range.size());

    g.last_buffer_strides[0] = range.stride();
    g.last_buffer_strides[1] = g.last_buffer_strides[2] = 1;
    g.last_buffer_shape[0] = range.size();
    g.last_buffer_shape[1] = g.last_buffer_shape[2] = 1;

    // https://docs.python.org/3/c-api/buffer.html
    g.last_buffer.buf = (void*)range.begin().get();
    g.last_buffer.obj = NULL;
    g.last_buffer.len = range.size() * sizeof(element_at);
    g.last_buffer.itemsize = sizeof(element_at);
    // https://docs.python.org/3/library/struct.html#format-characters
    g.last_buffer.format = (char*)format_code_gt<std::remove_const_t<element_at>>::format_k;
    g.last_buffer.ndim = 1;
    g.last_buffer.shape = &g.last_buffer_shape[0];
    g.last_buffer.strides = &g.last_buffer_strides[0];
    g.last_buffer.suboffsets = nullptr;
    g.last_buffer.readonly = true;
    g.last_buffer.internal = nullptr;
    return PyMemoryView_FromBuffer(&g.last_buffer);
}

void ukv::wrap_networkx(py::module& m) {

    auto degs = py::class_<degree_view_t, std::shared_ptr<degree_view_t>>(m, "DegreeView", py::module_local());
    degs.def("__getitem__", [](degree_view_t& degs, ukv_key_t v) {
        py_graph_t& g = *degs.net_ptr;
        auto maybe = g.ref().degree(v, degs.roles);
        maybe.throw_unhandled();
        ukv_vertex_degree_t result = *maybe;
        return result;
    });
    degs.def("__getitem__", [](degree_view_t& degs, py::handle vs) {
        py_graph_t& g = *degs.net_ptr;
        auto handle_and_ids = strided_array<ukv_key_t const>(vs);
        auto maybe = g.ref().degrees(handle_and_ids.second, degs.roles);
        maybe.throw_unhandled();
        return wrap_into_buffer<ukv_vertex_degree_t const>(g, {maybe->begin(), maybe->end()});
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
            net_ptr->db_ptr = py_db;
            net_ptr->is_directed_ = directed;
            net_ptr->is_multi_ = multi;
            net_ptr->allow_self_loops_ = loops;

            // Attach the primary collection
            db_t& db = py_db->native;
            {
                auto col = db.collection(index ? index->c_str() : "");
                col.throw_unhandled();
                net_ptr->index = *std::move(col);
            }
            // Attach the additional collections
            if (sources_attrs) {
                auto col = db.collection(sources_attrs->c_str());
                col.throw_unhandled();
                net_ptr->sources_attrs = *std::move(col);
            }
            if (targets_attrs) {
                auto col = db.collection(targets_attrs->c_str());
                col.throw_unhandled();
                net_ptr->targets_attrs = *std::move(col);
            }
            if (relations_attrs) {
                auto col = db.collection(relations_attrs->c_str());
                col.throw_unhandled();
                net_ptr->relations_attrs = *std::move(col);
            }
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
        [](py_graph_t& g, ukv_key_t v) { return g.index.size(); },
        "Returns the number of nodes in the graph.");
    g.def(
        "number_of_nodes",
        [](py_graph_t& g, ukv_key_t v) { return g.index.size(); },
        "Returns the number of nodes in the graph.");
    g.def(
        "__len__",
        [](py_graph_t& g, ukv_key_t v) { return g.index.size(); },
        "Returns the number of nodes in the graph.");
    g.def_property_readonly(
        "degree",
        [](py_graph_t& g) {
            auto degs_ptr = std::make_shared<degree_view_t>();
            degs_ptr->net_ptr = g.shared_from_this();
            degs_ptr->roles = ukv_vertex_role_any_k;
            return degs_ptr;
        },
        "A DegreeView for the graph.");
    g.def_property_readonly(
        "in_degree",
        [](py_graph_t& g) {
            auto degs_ptr = std::make_shared<degree_view_t>();
            degs_ptr->net_ptr = g.shared_from_this();
            degs_ptr->roles = ukv_vertex_target_k;
            return degs_ptr;
        },
        "A DegreeView with the number incoming edges for each Vertex.");
    g.def_property_readonly(
        "out_degree",
        [](py_graph_t& g) {
            auto degs_ptr = std::make_shared<degree_view_t>();
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
        [](py_graph_t& g, ukv_key_t v1, ukv_key_t v2) {
            auto maybe = g.ref().edges(v1, v2);
            maybe.throw_unhandled();
            return maybe->size();
        },
        "Returns the number of edges between two nodes.");

    // Reporting nodes edges and neighbors
    // https://networkx.org/documentation/stable/reference/classes/multidigraph.html#reporting-nodes-edges-and-neighbors
    g.def(
        "nodes",
        [](py_graph_t& g) { throw_not_implemented(); },
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
        [](py_graph_t& g, ukv_key_t v) {
            auto maybe = g.ref().contains(v);
            maybe.throw_unhandled();
            return *maybe;
        },
        py::arg("n"),
        "Returns True if the graph contains the node n.");

    g.def("edges", [](py_graph_t& g) { throw_not_implemented(); });
    g.def("out_edges", [](py_graph_t& g) { throw_not_implemented(); });
    g.def("in_edges", [](py_graph_t& g) { throw_not_implemented(); });

    g.def(
        "has_edge",
        [](py_graph_t& g, ukv_key_t v1, ukv_key_t v2) {
            auto maybe = g.ref().edges(v1, v2);
            maybe.throw_unhandled();
            return maybe->size() != 0;
        },
        py::arg("u"),
        py::arg("v"));
    g.def(
        "has_edge",
        [](py_graph_t& g, ukv_key_t v1, ukv_key_t v2, ukv_key_t eid) {
            auto maybe = g.ref().edges(v1, v2);
            maybe.throw_unhandled();
            return std::find(maybe->edge_ids.begin(), maybe->edge_ids.end(), eid) != maybe->edge_ids.end();
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
        [](py_graph_t& g, ukv_key_t n) {
            // Retrieving neighbors is trickier than just `successors` or `predecessors`.
            // We are receiving an adjacency list, where both incoming an edges exist.
            // So the stride/offset is not uniform across the entire list.
            auto maybe = g.ref().edges(n, ukv_vertex_role_any_k);
            maybe.throw_unhandled();

            // We can gobble the contents a little bit by swapping the members of some
            // edges to make it uniform.
            auto edges = *maybe;
            auto count = edges.size();
            for (std::size_t i = 0; i != count; ++i) {
                ukv_key_t& u = const_cast<ukv_key_t&>(edges.source_ids[i]);
                ukv_key_t& v = const_cast<ukv_key_t&>(edges.target_ids[i]);
                if (u == n)
                    std::swap(u, v);
            }

            return wrap_into_buffer(g, edges.target_ids);
        },
        py::arg("n"),
        "Returns an iterable of incoming and outgoing nodes of n. Potentially with duplicates.");
    g.def(
        "successors",
        [](py_graph_t& g, ukv_key_t n) {
            auto maybe = g.ref().edges(n, ukv_vertex_source_k);
            maybe.throw_unhandled();
            return wrap_into_buffer(g, maybe->target_ids);
        },
        py::arg("n"),
        "Returns an iterable of successor nodes of n.");
    g.def(
        "predecessors",
        [](py_graph_t& g, ukv_key_t n) {
            auto maybe = g.ref().edges(n, ukv_vertex_target_k);
            maybe.throw_unhandled();
            return wrap_into_buffer(g, maybe->source_ids);
        },
        py::arg("n"),
        "Returns an iterable of follower nodes of n.");
    g.def(
        "nbunch_iter",
        [](py_graph_t& g, py::handle const& vs) {
            auto handle_and_ids = strided_array<ukv_key_t const>(vs);
            auto maybe = g.ref().contains(handle_and_ids.second);
            maybe.throw_unhandled();
            return wrap_into_buffer(g, *maybe);
        },
        "Checks given nodes against graph members and returns a filtered iterable object");

    // Adding and Removing Nodes and Edges
    // https://networkx.org/documentation/stable/reference/classes/multidigraph.html#adding-and-removing-nodes-and-edges
    g.def(
        "add_edge",
        [](py_graph_t& g, ukv_key_t v1, ukv_key_t v2) {
            edges_view_t edges {
                strided_range_gt<ukv_key_t const>(v1),
                strided_range_gt<ukv_key_t const>(v2),
            };
            g.ref().upsert(edges).throw_unhandled();
        },
        py::arg("u_for_edge"),
        py::arg("v_for_edge"));
    g.def(
        "add_edge",
        [](py_graph_t& g, ukv_key_t v1, ukv_key_t v2, ukv_key_t eid) {
            edges_view_t edges {
                strided_range_gt<ukv_key_t const>(v1),
                strided_range_gt<ukv_key_t const>(v2),
                strided_range_gt<ukv_key_t const>(eid),
            };
            g.ref().upsert(edges).throw_unhandled();
        },
        py::arg("u_for_edge"),
        py::arg("v_for_edge"),
        py::arg("key"));
    g.def(
        "remove_edge",
        [](py_graph_t& g, ukv_key_t v1, ukv_key_t v2) {
            edges_view_t edges {
                strided_range_gt<ukv_key_t const>(v1),
                strided_range_gt<ukv_key_t const>(v2),
            };
            g.ref().remove(edges).throw_unhandled();
        },
        py::arg("u_for_edge"),
        py::arg("v_for_edge"));
    g.def(
        "remove_edge",
        [](py_graph_t& g, ukv_key_t v1, ukv_key_t v2, ukv_key_t eid) {
            edges_view_t edges {
                strided_range_gt<ukv_key_t const>(v1),
                strided_range_gt<ukv_key_t const>(v2),
                strided_range_gt<ukv_key_t const>(eid),
            };
            g.ref().remove(edges).throw_unhandled();
        },
        py::arg("u_for_edge"),
        py::arg("v_for_edge"),
        py::arg("key"));
    g.def(
        "add_edges_from",
        [](py_graph_t& g, py::handle const& adjacency_list) {
            auto handle_and_list = strided_matrix<ukv_key_t const>(adjacency_list);
            if (handle_and_list.second.cols() != 2 || handle_and_list.second.cols() != 3)
                throw std::invalid_argument("Expecting 2 or 3 columns: sources, targets, edge IDs");

            edges_view_t edges {
                handle_and_list.second.col(0),
                handle_and_list.second.col(1),
                handle_and_list.second.cols() == 3 ? handle_and_list.second.col(2)
                                                   : strided_range_gt<ukv_key_t const>(ukv_default_edge_id_k),
            };
            g.ref().upsert(edges).throw_unhandled();
        },
        py::arg("ebunch_to_add"),
        "Adds an adjacency list (in a form of 2 or 3 columnar matrix) to the graph.");
    g.def(
        "remove_edges_from",
        [](py_graph_t& g, py::handle const& adjacency_list) {
            auto handle_and_list = strided_matrix<ukv_key_t const>(adjacency_list);
            if (handle_and_list.second.cols() != 2 || handle_and_list.second.cols() != 3)
                throw std::invalid_argument("Expecting 2 or 3 columns: sources, targets, edge IDs");

            edges_view_t edges {
                handle_and_list.second.col(0),
                handle_and_list.second.col(1),
                handle_and_list.second.cols() == 3 ? handle_and_list.second.col(2)
                                                   : strided_range_gt<ukv_key_t const>(ukv_default_edge_id_k),
            };
            g.ref().remove(edges).throw_unhandled();
        },
        py::arg("ebunch"),
        "Removes all edges in supplied adjacency list (in a form of 2 or 3 columnar matrix) from the graph.");

    g.def(
        "add_edges_from",
        [](py_graph_t& g, py::handle const& v1s, py::handle const& v2s) {
            auto handle_and_sources = strided_array<ukv_key_t const>(v1s);
            auto handle_and_targets = strided_array<ukv_key_t const>(v2s);
            edges_view_t edges {
                handle_and_sources.second,
                handle_and_targets.second,
            };
            g.ref().upsert(edges).throw_unhandled();
        },
        py::arg("us"),
        py::arg("vs"),
        "Adds edges from members of the first array to members of the second array.");
    g.def(
        "remove_edges_from",
        [](py_graph_t& g, py::handle const& v1s, py::handle const& v2s) {
            auto handle_and_sources = strided_array<ukv_key_t const>(v1s);
            auto handle_and_targets = strided_array<ukv_key_t const>(v2s);
            edges_view_t edges {
                handle_and_sources.second,
                handle_and_targets.second,
            };
            g.ref().remove(edges).throw_unhandled();
        },
        py::arg("us"),
        py::arg("vs"),
        "Removes edges from members of the first array to members of the second array.");

    g.def(
        "add_edges_from",
        [](py_graph_t& g, py::handle const& v1s, py::handle const& v2s, py::handle const& eids) {
            auto handle_and_sources = strided_array<ukv_key_t const>(v1s);
            auto handle_and_targets = strided_array<ukv_key_t const>(v2s);
            auto handle_and_edge_ids = strided_array<ukv_key_t const>(eids);
            edges_view_t edges {
                handle_and_sources.second,
                handle_and_targets.second,
                handle_and_edge_ids.second,
            };
            g.ref().upsert(edges).throw_unhandled();
        },
        py::arg("us"),
        py::arg("vs"),
        py::arg("keys"),
        "Adds edges from members of the first array to members of the second array using keys from the third array.");
    g.def(
        "remove_edges_from",
        [](py_graph_t& g, py::handle const& v1s, py::handle const& v2s, py::handle const& eids) {
            auto handle_and_sources = strided_array<ukv_key_t const>(v1s);
            auto handle_and_targets = strided_array<ukv_key_t const>(v2s);
            auto handle_and_edge_ids = strided_array<ukv_key_t const>(eids);
            edges_view_t edges {
                handle_and_sources.second,
                handle_and_targets.second,
                handle_and_edge_ids.second,
            };
            g.ref().remove(edges).throw_unhandled();
        },
        py::arg("us"),
        py::arg("vs"),
        py::arg("keys"),
        "Removes edges from members of the first array to members of the second array using keys from the third "
        "array.");
    g.def("clear_edges", [](py_graph_t& g) { throw_not_implemented(); });
    g.def(
        "clear",
        [](py_graph_t& g) {
            // db_t& db = g.db_ptr->native;
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
        [](py_graph_t& g, py::handle ns) { throw_not_implemented(); },
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
    g.def_property_readonly("is_directed", [](py_graph_t& g) { return g.is_directed_; });
    g.def_property_readonly("is_multi", [](py_graph_t& g) { return g.is_multi_; });
    g.def_property_readonly("allows_loops", [](py_graph_t& g) { return g.allow_self_loops_; });
    m.def("is_directed", [](py_graph_t& g) { return g.is_directed_; });
    m.def("is_multi", [](py_graph_t& g) { return g.is_multi_; });
    m.def("allows_loops", [](py_graph_t& g) { return g.allow_self_loops_; });
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
