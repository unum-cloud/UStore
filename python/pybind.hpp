
#pragma once
#include <utility> // `std::pair`

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>

#include <arrow/python/pyarrow.h>

#include "ukv/ukv.hpp"

namespace unum::ukv {

namespace py = pybind11;

struct py_db_t;
struct py_transaction_t;
struct py_collection_t;

struct py_graph_t;
struct py_table_collection_t;

struct py_task_ctx_t;

/**
 * @brief Wrapper for `ukv::database_t`.
 * Assumes that the Python client won't use more than one
 * concurrent session, as multithreading in Python is
 * prohibitively expensive.
 * We need to preserve the `config`, to allow re-opening.
 */
struct py_db_t : public std::enable_shared_from_this<py_db_t> {
    database_t native;
    std::string config;
    /**
     * @brief Some clients may prefer to receive extracted values
     * as native Python types when possible. By default, we export
     * into Apache Arrow arrays.
     */
    bool export_into_arrow {true};

    py_db_t(database_t&& n, std::string const& c) : native(std::move(n)), config(c) {}
    py_db_t(py_db_t&& other) noexcept : native(std::move(other.native)), config(std::move(other.config)) {}
    py_db_t(py_db_t const&) = delete;
};

/**
 * @brief Wrapper for `ukv::transaction_t`.
 * Only adds reference counting to the native C++ interface.
 */
struct py_transaction_t : public std::enable_shared_from_this<py_transaction_t> {
    transaction_t native;

    std::weak_ptr<py_db_t> py_db_ptr;

    bool dont_watch {false};
    bool flush_writes {false};

    py_transaction_t(transaction_t&& t, std::shared_ptr<py_db_t> py_db_ptr) noexcept
        : native(std::move(t)), py_db_ptr(py_db_ptr) {}
    py_transaction_t(py_transaction_t&& other) noexcept
        : native(std::move(other.native)), py_db_ptr(other.py_db_ptr), dont_watch(other.dont_watch),
          flush_writes(other.flush_writes) {}
    py_transaction_t(py_transaction_t const&) = delete;
};

/**
 * @brief Wrapper for `ukv::blobs_collection_t`.
 * We need to preserve the `name`, to upsert again, after removing it in `clear`.
 * We also keep the transaction pointer, to persist the context of operation.
 */
template <typename collection_at>
struct py_collection_gt {
    collection_at native;

    std::weak_ptr<py_db_t> py_db_ptr;
    std::weak_ptr<py_transaction_t> py_txn_ptr;
    std::string name;
    bool in_txn {false};

    ukv_collection_t* member_collection() noexcept { return native.member_ptr(); }
    ukv_arena_t* member_arena() noexcept { return native.member_arena(); }
    ukv_options_t options() noexcept {
        auto txn_ptr = py_txn_ptr.lock();
        auto base = ukv_options_default_k;
        return txn_ptr ? static_cast<ukv_options_t>( //
                             base |                  //
                             (txn_ptr->dont_watch ? ukv_option_transaction_dont_watch_k : base) |
                             (txn_ptr->flush_writes ? ukv_option_write_flush_k : base))
                       : base;
    }
    ukv_database_t db() noexcept(false) {
        if (py_db_ptr.expired())
            throw std::domain_error("Collection references closed DB");
        return native.db();
    }
    ukv_transaction_t txn() noexcept(false) {
        if (in_txn && py_txn_ptr.expired())
            throw std::domain_error("Collection references closed transaction");
        return in_txn ? ukv_transaction_t(py_txn_ptr.lock()->native) : ukv_transaction_t(nullptr);
    }

    /**
     * @brief Some clients may prefer to receive extracted values
     * as native Python types when possible. By default, we export
     * into Apache Arrow arrays.
     */
    bool export_into_arrow() const noexcept {
        auto db_ptr = py_db_ptr.lock();
        return db_ptr->export_into_arrow;
    }
};

using py_blobs_collection_t = py_collection_gt<blobs_collection_t>;
using py_docs_collection_t = py_collection_gt<docs_collection_t>;

struct py_buffer_memory_t {
    Py_buffer raw;
    /// The memory that `raw.shape` points to.
    Py_ssize_t shape[4];
    /// The memory that `raw.strides` points to.
    Py_ssize_t strides[4];
};

struct py_graph_t : public std::enable_shared_from_this<py_graph_t> {

    std::weak_ptr<py_db_t> py_db_ptr;
    std::weak_ptr<py_transaction_t> py_txn_ptr;

    blobs_collection_t index;
    blobs_collection_t sources_attrs;
    blobs_collection_t targets_attrs;
    blobs_collection_t relations_attrs;

    bool in_txn {false};
    bool is_directed {false};
    bool is_multi {false};
    bool allow_self_loops {false};

    py_buffer_memory_t last_buffer;

    py_graph_t() {}
    py_graph_t(py_graph_t&&) = delete;
    py_graph_t(py_graph_t const&) = delete;
    ~py_graph_t() {}

    graph_collection_t ref() { return graph_collection_t(index.db(), index, index.txn(), index.member_arena()); }
};

struct py_table_keys_range_t {
    ukv_key_t min {std::numeric_limits<ukv_key_t>::min()};
    ukv_key_t max {std::numeric_limits<ukv_key_t>::max()};
};

/**
 * @brief DataFrame representation, capable of viewing joined contents
 * of multiple collections. When materialized, exports Apache Arrow objects.
 */
struct py_table_collection_t : public std::enable_shared_from_this<py_table_collection_t> {

    py_blobs_collection_t binary;
    std::variant<std::monostate, std::vector<ukv_str_view_t>> columns_names;
    std::variant<std::monostate, ukv_doc_field_type_t, std::vector<ukv_doc_field_type_t>> columns_types;
    std::variant<std::monostate, py_table_keys_range_t, std::vector<ukv_key_t>> rows_keys;
    std::size_t head {std::numeric_limits<std::size_t>::max()};
    std::size_t tail {std::numeric_limits<std::size_t>::max()};
    bool head_was_defined_last {true};

    py_table_collection_t() = default;
    py_table_collection_t(py_table_collection_t&&) = delete;
    py_table_collection_t(py_table_collection_t const&) = delete;

    // Compatibility with Arrow Tables.
    // std::shared_ptr<ar::ChunkedArray> column(int i) const override;
    // std::vector<std::shared_ptr<ar::ChunkedArray>> const& columns() const override;
    // std::shared_ptr<ar::Table> Slice(int64_t offset, int64_t length) const override;
    // ar::Result<std::shared_ptr<ar::Table>> RemoveColumn(int i) const override;
    // ar::Result<std::shared_ptr<ar::Table>> AddColumn( //
    //     int i,
    //     std::shared_ptr<ar::Field> field_arg,
    //     std::shared_ptr<ar::ChunkedArray> column) const override;
    // ar::Result<std::shared_ptr<ar::Table>> SetColumn( //
    //     int i,
    //     std::shared_ptr<ar::Field> field_arg,
    //     std::shared_ptr<ar::ChunkedArray> column) const override;
    // std::shared_ptr<ar::Table> ReplaceSchemaMetadata(std::shared_ptr<ar::KeyValueMetadata const> const&) const
    // override; ar::Result<std::shared_ptr<ar::Table>> Flatten(ar::MemoryPool* = ar::default_memory_pool()) const
    // override; ar::Status Validate() const override; ar::Status ValidateFull() const override;
};

/**
 * @brief Proxy-object for binary @c py_collection_t collections that adds:
 * - serialization & deserialization of Python objects.
 * - field-level lookups.
 * - patching & merging: `.patch(...)` & `.merge(...)`.
 * - DataFrame exports (out of this single collection).
 */
template <typename native_at>
struct py_stream_with_ending_gt {
    native_at native;
    ukv_key_t terminal {ukv_key_unknown_k};
    bool stop {false};
};

/**
 * @brief Binds DBMS to Python, as if it was `dict[str, dict[int, bytes]]`.
 *
 * ## Interface
 *
 * DataBase Methods:
 *      - main ~ Accesses the default collection
 *      - __getitem__(collection: str) ~ Accesses a named collection
 *      - clear() ~ Clears all the data from DB
 *      - transact() - Starts a new transaction (supports context managers)
 *
 * Collection Methods:
 *      - __in__(key), has_key(...) ~ Single & Batch Contains
 *      - __getitem__(key: int), get(...) ~ Value Lookup
 *      - __setitem__(key: int, value), set(...) ~ Value Upserts
 *      - __delitem__(key), pop(...) ~ Removes a key
 * All those CRUD operations can be submitted in batches in forms of
 * Python `tuple`s, `list`s, NumPy arrays, or anything that supports buffer
 * protocol. Remaining collection methods include:
 *      - update(mapping: dict) ~ Batch Insert/Put
 *      - clear() ~ Removes all items in collection
 *      - get_column(keys) ~ Will extract/receive binary values as Apache Arrow collections
 *      - get_matrix(keys, max_length: int, padding: byte)
 *
 * All in all, collections mimic Python @c dict API, but some funcs were skipped:
 *      - __len__() ~ It's hard to consistently estimate the collection.
 *      - popitem() ~ We can't guarantee Last-In First-Out semantics.
 *      - setdefault(key[, default]) ~ As default values are useless in DBs.
 * To access typed collections following computable properties are provided:
 *      - docs  ~ Unpack objects into `dict`/`list`s and supports field-level ops
 *      - table ~ Accesses Docs in a Pandas-like fashion
 *      - graph ~ Accesses relations/links in NetworkX fashion
 *      - media ~ Unpacks and converts to Tensors on lookups
 *
 * ## Python classes vs Arrow Arrays
 *
 * Both kinds of arguments/results are supported with these bindings.
 * By default, we export native Python objects in single-entry operations,
 * but for batches - we use Arrow. Namely, in Batch-Reads and Range-Selects.
 *
 * https://python-reference.readthedocs.io/en/latest/docs/dict/
 * https://docs.python.org/3/library/stdtypes.html#mapping-types-dict
 */
void wrap_database(py::module&);

/**
 * @brief Python bindings for a Graph index, that mimics NetworkX.
 * Unlike C++ @c graph_collection_t this may include as many as 4 collections
 * seen as one heavily attributed relational index.
 * Is similar in it's purpose to a pure-Python project - NetworkXum:
 * https://github.com/unum-cloud/NetworkXum
 *
 * ## Supported Graph Types
 *
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
 * Beyond that, source and target vertices can belong to different collections.
 * To sum up, we differentiate following graph types:
 * - U: Undirected
 * - D: Directed
 * - J: Joining
 *
 * Example for simple non-attributed undirected graphs:
 * - relations_name: ".graph"
 * - attrs_name: ""
 * - sources_name: ""
 * - targets_name: ""
 *
 * Example for recommender systems
 * - relations_name: "views.graph"
 * - attrs_name: "views.docs"
 * - sources_name: "people.docs"
 * - targets_name: "movies.docs"
 *
 * ## Interface
 *
 * Primary single element methods:
 *      - add_edge(first, second, key?, attrs?)
 *      - remove_edge(first, second, key?, attrs?)
 * Additional batch methods:
 *      - add_edges_from(firsts, seconds, keys?, attrs?)
 *      - remove_edges_from(firsts, seconds, keys?, attrs?)
 * Intentionally not implemented:
 *      - __len__() ~ It's hard to consistently estimate the collection size.
 *
 * TODO:
 * - Implement basic algorithms: PageRank, Louvain, WCC and Force-based Layout
 * - Implement subgraph selection
 * - Implement attributes
 */
void wrap_networkx(py::module&);

/**
 * @brief Python bindings for a Document Store, that mimics Pandas.
 * Is designed to export results in the form of Apache Arrow Tables.
 *
 * ## Usage
 *
 * - Take first 5 rows starting with ID #100:
 *   db.main.table.astype('int32').loc[100:].head(5).df
 *   Note that contrary to usual python slices, both the start and the stop are included
 * - Take rows with IDs #100, #101:
 *   db.main.table.loc[[100, 101]].astype('float').df
 * - Take specific columns from a rows range:
 *   db.main.table.loc[100:101].astype({'age':'float', 'name':'str'}).df
 *
 * ## Interface
 *
 * Choosing subsample of rows:
 *      - tbl.loc[100:] ~ Starting from a certain ID
 *      - tbl.loc[[...]] ~ Specific list of IDs
 *      - tbl.head(5) ~ First rows of the table
 *      - tbl.tail(5) ~ Last rows of the table
 * Defining columns:
 *      - tbl.astype('int32') ~ All columns
 *      - tbl[names].astype('int32') ~ Specific columns
 *      - tbl.astype({'age':'float', 'name':'str'})
 *
 * In worst-case scenario, the lookup will contain 3 steps:
 *      1. iteration, to collect the IDs of documents forming a range.
 *      2. gist, to detect the names of fields in present documents.
 *      3. gather, to export into a table.
 *
 * https://stackoverflow.com/a/57907044/2766161
 * https://arrow.apache.org/docs/python/integration/extending.html
 */
void wrap_pandas(py::module&);

void wrap_document(py::module&);

} // namespace unum::ukv