/**
 *  @file python/graph.c
 *  @author Ash Vardanian
 *  @date September 17, 2026
 *  @brief Graphs: edge and vertex writes and removals, degrees and edge reads.
 *
 *  A graph collection maps every vertex onto its sorted neighborships, so an edge is three keys,
 *  the source, the target and the edge identifier, and a read answers a tape of such triples. Every
 *  verb fills a call the driver keeps on the stack or inside a @c Deferred, and a read of edges or
 *  degrees claims the arena it is given, answering a tape viewing it or a tuple copied out of it.
 */
#include "lib.h"

#pragma region Shared Arguments

/**
 *  @brief Walks the keywords of a verb whose only keyword of its own is the vertex role.
 *  @param[in] context The database and view the verb runs against.
 *  @param[in] verb The method's name, as a failure should read it.
 *  @param[in] arguments The fast-call arguments, positional ones first.
 *  @param[in] positional_arguments_count Positional arguments the call carried.
 *  @param[in] keyword_names_tuple The keyword names, or NULL when none followed.
 *  @param[in] required_positional Positional arguments the verb fixes.
 *  @param[out] options Where every shared keyword lands.
 *  @param[out] role The role the call names, which starts as either end of an edge.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
static int role_options_from_keywords(ustore_py_context_t const* context, char const* verb, PyObject* const* arguments,
                                      Py_ssize_t positional_arguments_count, PyObject* keyword_names_tuple,
                                      Py_ssize_t required_positional, ustore_py_verb_options_t* options,
                                      ustore_vertex_role_t* role) {
    Py_ssize_t const keyword_names_count = ustore_py_keyword_names_count_checked(
        verb, positional_arguments_count, keyword_names_tuple, required_positional);
    if (keyword_names_count < 0) return -1;
    ustore_py_verb_options_init(options);
    int chosen = ustore_vertex_role_any_k;
    for (Py_ssize_t names_progress = 0, arguments_progress = positional_arguments_count;
         names_progress != keyword_names_count; ++arguments_progress, ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(keyword_names_tuple, names_progress);
        int const consumed = ustore_py_verb_option_from_keyword(context, name, arguments[arguments_progress], options);
        if (consumed < 0) return -1;
        if (consumed > 0) continue;
        if (PyUnicode_CompareWithASCIIString(name, "role") != 0) {
            PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
            return -1;
        }
        if (ustore_py_enum_from_object(arguments[arguments_progress], "role", ustore_py_enum_role_k, &chosen) != 0)
            return -1;
    }
    *role = (ustore_vertex_role_t)chosen;
    return 0;
}

/** The three batches of keys an edge verb addresses, and the options beside them. */
typedef struct edge_batches_t {

    /** The vertices every edge leaves. */
    ustore_py_keys_t sources;

    /** The vertices every edge enters. */
    ustore_py_keys_t targets;

    /** The identifier of every edge, empty when the edges were given none. */
    ustore_py_keys_t edges;

    /** The collection and knobs the call names. */
    ustore_py_verb_options_t options;
} edge_batches_t;

/** Releases whatever @c edge_batches_from_args acquired. */
static void edge_batches_release(edge_batches_t* batches) {
    ustore_py_keys_release(&batches->sources);
    ustore_py_keys_release(&batches->targets);
    ustore_py_keys_release(&batches->edges);
}

/**
 *  @brief Reads the sources, targets, optional edge identifiers and shared keywords of an edge verb.
 *  @param[in] context The database and view the verb runs against.
 *  @param[in] verb The method's name, as a failure should read it.
 *  @param[in] arguments The fast-call arguments, positional ones first.
 *  @param[in] positional_arguments_count Positional arguments the call carried.
 *  @param[in] keyword_names_tuple The keyword names, or NULL when none followed.
 *  @param[out] batches The keys and options the verb runs with.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
static int edge_batches_from_args(ustore_py_context_t const* context, char const* verb, PyObject* const* arguments,
                                  Py_ssize_t positional_arguments_count, PyObject* keyword_names_tuple,
                                  edge_batches_t* batches) {
    memset(batches, 0, sizeof(*batches));
    Py_ssize_t const keyword_names_count = ustore_py_keyword_names_count_checked(verb, positional_arguments_count,
                                                                                 keyword_names_tuple, 2);
    if (keyword_names_count < 0) return -1;
    ustore_py_verb_options_init(&batches->options);
    PyObject* edges_object = Py_None;
    for (Py_ssize_t names_progress = 0, arguments_progress = positional_arguments_count;
         names_progress != keyword_names_count; ++arguments_progress, ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(keyword_names_tuple, names_progress);
        int const consumed = ustore_py_verb_option_from_keyword(context, name, arguments[arguments_progress],
                                                                &batches->options);
        if (consumed < 0) return -1;
        if (consumed > 0) continue;
        if (PyUnicode_CompareWithASCIIString(name, "edges") != 0) {
            PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
            return -1;
        }
        edges_object = arguments[arguments_progress];
    }

    if (ustore_py_keys_from_object(arguments[0], &batches->sources) != 0) return -1;
    if (ustore_py_keys_from_object(arguments[1], &batches->targets) != 0 ||
        (edges_object != Py_None && ustore_py_keys_from_object(edges_object, &batches->edges) != 0)) {
        edge_batches_release(batches);
        return -1;
    }
    size_t const count = batches->sources.count;
    if (batches->targets.count != count || (edges_object != Py_None && batches->edges.count != count)) {
        ustore_py_raise(ustore_status_invalid_argument_k, "Got %zu sources, %zu targets and %zu edge identifiers",
                        count, batches->targets.count, batches->edges.count);
        edge_batches_release(batches);
        return -1;
    }
    return 0;
}

#pragma endregion Shared Arguments

#pragma region Edges

char const doc_graph_write_edges[] =                                                              //
    "graph_write_edges(sources, targets, *, edges=None, collection,\n"                            //
    "                  durability=Durability.BUFFERED) answers None.\n"                           //
    "\n"                                                                                          //
    "Inserts a batch of edges, creating the vertices they connect. An edge already present\n"     //
    "is kept, so writing the same batch twice changes nothing.\n"                                 //
    "\n"                                                                                          //
    "Args:\n"                                                                                     //
    "  sources (Sequence[KeyLike] | buffer): The vertex every edge leaves.\n"                     //
    "  targets (Sequence[KeyLike] | buffer): The vertex every edge enters.\n"                     //
    "  edges (Sequence[KeyLike] | buffer, optional): The identifier of every edge, which tells\n" //
    "    apart several edges between one pair; None gives every edge `edge_id_default`.\n"        //
    "  collection (UUID | str): The graph collection, by identifier or by name.\n"                //
    "  durability (Durability, optional): `Durability.FLUSHED` reaches the drive before\n"        //
    "    returning.";

/** A write or a removal of a batch of edges, which its caller gathers in place. */
typedef struct edges_write_call_t {

    /** What the call keeps until it finishes. */
    ustore_py_call_t call;

    /** The edges and the options the call names. */
    edge_batches_t batches;
} edges_write_call_t;

static void edges_write_submit(void* arguments) { ustore_graph_write_edges((ustore_graph_write_edges_t*)arguments); }

static void edges_remove_submit(void* arguments) { ustore_graph_remove_edges((ustore_graph_remove_edges_t*)arguments); }

static void edges_write_release(void* erased) {
    edges_write_call_t* const call = (edges_write_call_t*)erased;
    edge_batches_release(&call->batches);
    ustore_py_call_release(&call->call);
}

static ustore_py_verb_t const edges_write_verb = {sizeof(edges_write_call_t), edges_write_submit,
                                                  ustore_py_call_answer_none, edges_write_release};

static ustore_py_verb_t const edges_remove_verb = {sizeof(edges_write_call_t), edges_remove_submit,
                                                   ustore_py_call_answer_none, edges_write_release};

/**
 *  @brief Opens a call of @p verb through @p self and gathers into it the edges and options its arguments name.
 *  @param[out] context What the call addresses and how it runs.
 *  @param[in] stack The caller's storage, used while the call blocks.
 *  @return The call, or NULL with an exception set.
 */
static edges_write_call_t* edges_write_open(PyObject* self, char const* name, ustore_py_verb_t const* verb,
                                            PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                                            PyObject* keyword_names_tuple, ustore_py_context_t* context,
                                            edges_write_call_t* stack) {
    if (ustore_py_context_from_object(self, context) != 0 || ustore_py_context_writable(context) != 0) return NULL;
    edges_write_call_t* const call = ustore_py_call_open(context, verb, stack);
    if (!call) return NULL;
    if (edge_batches_from_args(context, name, arguments, positional_arguments_count, keyword_names_tuple,
                               &call->batches) == 0) {
        call->call.collection = call->batches.options.collection;
        return call;
    }
    ustore_py_call_discard(context, verb, call);
    return NULL;
}

PyObject* ustore_py_api_graph_write_edges(PyObject* self, PyObject* const* arguments,
                                          Py_ssize_t positional_arguments_count, PyObject* keyword_names_tuple) {
    ustore_py_context_t context;
    edges_write_call_t stack;
    edges_write_call_t* const call = edges_write_open(self, "graph_write_edges", &edges_write_verb, arguments,
                                                      positional_arguments_count, keyword_names_tuple, &context,
                                                      &stack);
    if (!call) return NULL;
    edge_batches_t const* const batches = &call->batches;
    ustore_graph_write_edges_t write = {
        .database = context.database_handle,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->call.outcome,
        .completion = context.completion,
        .transaction = context.transaction,
        .count = batches->sources.count,
        .collections = {&call->call.collection, 0},
        .edges = {batches->edges.keys, sizeof(ustore_key_t)},
        .sources = {batches->sources.keys, sizeof(ustore_key_t)},
        .targets = {batches->targets.keys, sizeof(ustore_key_t)},
        .durability = batches->options.durability,
        .watch = context.watch,
    };
    return ustore_py_call_run(&context, &edges_write_verb, call, &write);
}

char const doc_graph_remove_edges[] =                                                              //
    "graph_remove_edges(sources, targets, *, edges=None, collection,\n"                            //
    "                   durability=Durability.BUFFERED) answers None.\n"                           //
    "\n"                                                                                           //
    "Removes a batch of edges and keeps the vertices they connected, even once isolated.\n"        //
    "\n"                                                                                           //
    "Args:\n"                                                                                      //
    "  sources (Sequence[KeyLike] | buffer): The vertex every edge leaves.\n"                      //
    "  targets (Sequence[KeyLike] | buffer): The vertex every edge enters.\n"                      //
    "  edges (Sequence[KeyLike] | buffer, optional): The identifier of every edge; None removes\n" //
    "    the edges that were given `edge_id_default`.\n"                                           //
    "  collection (UUID | str): The graph collection, by identifier or by name.\n"                 //
    "  durability (Durability, optional): `Durability.FLUSHED` reaches the drive before\n"         //
    "    returning.";

PyObject* ustore_py_api_graph_remove_edges(PyObject* self, PyObject* const* arguments,
                                           Py_ssize_t positional_arguments_count, PyObject* keyword_names_tuple) {
    ustore_py_context_t context;
    edges_write_call_t stack;
    edges_write_call_t* const call = edges_write_open(self, "graph_remove_edges", &edges_remove_verb, arguments,
                                                      positional_arguments_count, keyword_names_tuple, &context,
                                                      &stack);
    if (!call) return NULL;
    edge_batches_t const* const batches = &call->batches;
    ustore_graph_remove_edges_t remove = {
        .database = context.database_handle,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->call.outcome,
        .completion = context.completion,
        .transaction = context.transaction,
        .count = batches->sources.count,
        .collections = {&call->call.collection, 0},
        .edges = {batches->edges.keys, sizeof(ustore_key_t)},
        .sources = {batches->sources.keys, sizeof(ustore_key_t)},
        .targets = {batches->targets.keys, sizeof(ustore_key_t)},
        .durability = batches->options.durability,
        .watch = context.watch,
    };
    return ustore_py_call_run(&context, &edges_remove_verb, call, &remove);
}

/** The degree of every one of @p count vertices as a tuple, with None where a vertex is absent. */
static PyObject* degrees_tuple(ustore_vertex_degree_t const* degrees, size_t count) {
    PyObject* const counted = PyTuple_New((Py_ssize_t)count);
    for (size_t vertex_index = 0; counted && vertex_index != count; ++vertex_index) {
        ustore_vertex_degree_t const degree = degrees[vertex_index];
        PyObject* const entry = degree == ustore_vertex_degree_missing_k ? Py_NewRef(Py_None)
                                                                         : PyLong_FromUnsignedLong(degree);
        if (!entry) {
            Py_DECREF(counted);
            return NULL;
        }
        PyTuple_SET_ITEM(counted, (Py_ssize_t)vertex_index, entry);
    }
    return counted;
}

/** A new @c Neighborhoods over what a read of @p count vertices wrote into @p arena, which its call still claims. */
static PyObject* neighborhoods_new(PyObject* arena, ustore_key_t const* tape, ustore_length_t const* offsets,
                                   ustore_vertex_degree_t const* degrees, size_t count) {
    Neighborhoods* const instance = PyObject_New(Neighborhoods, &NeighborhoodsType);
    if (!instance) return NULL;
    instance->arena = Py_NewRef(arena);
    instance->generation = ustore_py_arena_generation(arena);
    instance->tape = tape;
    instance->offsets = offsets;
    instance->degrees = degrees;
    instance->count = count;
    // The edges found are known once the call wrote the last offset, while the call still holds the arena.
    instance->shape[0] = offsets ? (Py_ssize_t)offsets[count] : 0;
    instance->shape[1] = 3;
    instance->shape[2] = (Py_ssize_t)sizeof(ustore_key_t);
    instance->strides[0] = 3 * (Py_ssize_t)sizeof(ustore_key_t);
    instance->strides[1] = (Py_ssize_t)sizeof(ustore_key_t);
    instance->strides[2] = 1;
    return (PyObject*)instance;
}

char const doc_graph_read_edges[] =                                                             //
    "graph_read_edges(vertices, arena, *, role=Role.ANY, collection) answers Neighborhoods.\n"  //
    "\n"                                                                                        //
    "Reads every edge a batch of vertices takes part in. The edges of one vertex come sorted\n" //
    "by neighbour; with both roles the outgoing ones precede the incoming ones.\n"              //
    "\n"                                                                                        //
    "Args:\n"                                                                                   //
    "  vertices (Sequence[KeyLike] | buffer): The vertices whose edges are read.\n"             //
    "  arena (Arena): Where the degrees and the edges land, rewound by this call.\n"            //
    "  role (Role, optional): `Role.SOURCE` for the edges a vertex leaves by, `Role.TARGET`\n"  //
    "    for those it is entered by, or `Role.ANY` for both, which is how an undirected\n"      //
    "    graph asks.\n"                                                                         //
    "  collection (UUID | str): The graph collection, by identifier or by name.\n"              //
    "Returns:\n"                                                                                //
    "  Neighborhoods: One run of (source, target, edge) triples per vertex, None where it is\n" //
    "  absent.";

/** A read of the edges or the degrees of a batch of vertices, which its caller gathers in place. */
typedef struct edges_read_call_t {

    /** What the call keeps until it finishes: the outcome and the arena holding the degrees and the edges. */
    ustore_py_call_t call;

    /** The vertices whose edges are read. */
    ustore_py_keys_t vertices;

    /** The role every vertex plays. */
    ustore_vertex_role_t role;

    /** Output slot for the degree of every vertex, inside the arena. */
    ustore_vertex_degree_t const* degrees;

    /** Output slot for the edge offsets of every vertex, inside the arena, left NULL by a count of degrees. */
    ustore_length_t const* edges_offsets;

    /** Output slot for the edges of every vertex, inside the arena, left NULL by a count of degrees. */
    ustore_key_t const* edges;
} edges_read_call_t;

static void edges_read_submit(void* arguments) { ustore_graph_read_edges((ustore_graph_read_edges_t*)arguments); }

static PyObject* neighborhoods_answer(void* erased) {
    edges_read_call_t const* const call = (edges_read_call_t const*)erased;
    return neighborhoods_new(call->call.arena, call->edges, call->edges_offsets, call->degrees, call->vertices.count);
}

static PyObject* degrees_answer(void* erased) {
    edges_read_call_t const* const call = (edges_read_call_t const*)erased;
    return degrees_tuple(call->degrees, call->vertices.count);
}

static void edges_read_release(void* erased) {
    edges_read_call_t* const call = (edges_read_call_t*)erased;
    ustore_py_keys_release(&call->vertices);
    ustore_py_call_release(&call->call);
}

static ustore_py_verb_t const neighborhoods_verb = {sizeof(edges_read_call_t), edges_read_submit, neighborhoods_answer,
                                                    edges_read_release};

static ustore_py_verb_t const degrees_verb = {sizeof(edges_read_call_t), edges_read_submit, degrees_answer,
                                              edges_read_release};

static PyObject* contains_answer(void* erased) {
    edges_read_call_t const* const call = (edges_read_call_t const*)erased;
    return PyBool_FromLong(call->degrees[0] != ustore_vertex_degree_missing_k);
}

static ustore_py_verb_t const contains_verb = {sizeof(edges_read_call_t), edges_read_submit, contains_answer,
                                               edges_read_release};

/** Submits the read @p call gathered, its vertices, role, collection and arena set, as @p verb answers it. */
static PyObject* edges_read_run(ustore_py_context_t const* context, ustore_py_verb_t const* verb,
                                edges_read_call_t* call) {
    // Only the edges themselves export edges, every other answer counts degrees.
    int const exports_edges = verb == &neighborhoods_verb;
    ustore_graph_read_edges_t read = {
        .database = context->database_handle,
        .executor = ustore_py_context_executor(context),
        .outcome = &call->call.outcome,
        .completion = context->completion,
        .view = context->view,
        .count = call->vertices.count,
        .collections = {&call->call.collection, 0},
        .vertices = {call->vertices.keys, sizeof(ustore_key_t)},
        .roles = {&call->role, 0},
        .degrees_per_vertex = &call->degrees,
        .edges_offsets = exports_edges ? &call->edges_offsets : NULL,
        .edges_per_vertex = exports_edges ? &call->edges : NULL,
        .arena = ustore_py_arena_handle(call->call.arena),
    };
    return ustore_py_call_run(context, verb, call, &read);
}

/** Reads the edges or the degrees of the vertices `vertices, arena` name, as @p verb answers them. */
static PyObject* edges_read(PyObject* self, char const* name, ustore_py_verb_t const* verb, PyObject* const* arguments,
                            Py_ssize_t positional_arguments_count, PyObject* keyword_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    ustore_py_verb_options_t options;
    ustore_vertex_role_t role = ustore_vertex_role_any_k;
    if (role_options_from_keywords(&context, name, arguments, positional_arguments_count, keyword_names_tuple, 2,
                                   &options, &role) != 0)
        return NULL;
    edges_read_call_t stack;
    edges_read_call_t* const call = ustore_py_call_open(&context, verb, &stack);
    if (!call) return NULL;
    if (ustore_py_keys_from_object(arguments[0], &call->vertices) != 0 ||
        ustore_py_arena_claim(arguments[1], &call->call.arena) != 0)
        return ustore_py_call_discard(&context, verb, call);
    call->role = role;
    call->call.collection = options.collection;
    return edges_read_run(&context, verb, call);
}

PyObject* ustore_py_api_graph_read_edges(PyObject* self, PyObject* const* arguments,
                                         Py_ssize_t positional_arguments_count, PyObject* keyword_names_tuple) {
    return edges_read(self, "graph_read_edges", &neighborhoods_verb, arguments, positional_arguments_count,
                      keyword_names_tuple);
}

char const doc_graph_degrees[] =                                                            //
    "graph_degrees(vertices, arena, *, role=Role.ANY, collection) answers a tuple.\n"       //
    "\n"                                                                                    //
    "Counts the edges of a batch of vertices without exporting them.\n"                     //
    "\n"                                                                                    //
    "Args:\n"                                                                               //
    "  vertices (Sequence[KeyLike] | buffer): The vertices whose edges are counted.\n"      //
    "  arena (Arena): Where the degrees land before they are copied out, rewound by this\n" //
    "    call.\n"                                                                           //
    "  role (Role, optional): The role of every vertex, as `graph_read_edges` takes it.\n"  //
    "  collection (UUID | str): The graph collection, by identifier or by name.\n"          //
    "Returns:\n"                                                                            //
    "  tuple[int | None, ...]: The degree of every vertex, None where it is absent.";

PyObject* ustore_py_api_graph_degrees(PyObject* self, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                                      PyObject* keyword_names_tuple) {
    return edges_read(self, "graph_degrees", &degrees_verb, arguments, positional_arguments_count, keyword_names_tuple);
}

char const doc_graph_contains[] =                                                        //
    "graph_contains(vertex, *, collection) answers a bool.\n"                            //
    "\n"                                                                                 //
    "Whether a vertex is present, read as its degree through a one-off arena, which a\n" //
    "loop avoids with `graph_degrees` and an arena of its own.\n"                        //
    "\n"                                                                                 //
    "Args:\n"                                                                            //
    "  vertex (KeyLike): The vertex to look for.\n"                                      //
    "  collection (UUID | str): The graph collection, by identifier or by name.\n"       //
    "Returns:\n"                                                                         //
    "  bool: Whether the vertex is present.";

PyObject* ustore_py_api_graph_contains(PyObject* self, PyObject* const* arguments,
                                       Py_ssize_t positional_arguments_count, PyObject* keyword_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_keyword_names_count_checked("graph_contains", positional_arguments_count, keyword_names_tuple, 1) < 0)
        return NULL;
    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, arguments, positional_arguments_count, keyword_names_tuple,
                                             &options) != 0)
        return NULL;
    edges_read_call_t stack;
    edges_read_call_t* const call = ustore_py_call_open(&context, &contains_verb, &stack);
    if (!call) return NULL;
    call->vertices.keys = call->vertices.inline_keys;
    call->vertices.count = 1;
    PyObject* const arena = ustore_py_arena_new(ustore_py_context_executor(&context));
    if (!arena || ustore_py_key_from_object(arguments[0], call->vertices.inline_keys) != 0 ||
        ustore_py_arena_claim(arena, &call->call.arena) != 0) {
        Py_XDECREF(arena);
        return ustore_py_call_discard(&context, &contains_verb, call);
    }
    Py_DECREF(arena);
    call->role = ustore_vertex_role_any_k;
    call->call.collection = options.collection;
    return edges_read_run(&context, &contains_verb, call);
}

#pragma endregion Edges

#pragma region Vertices

char const doc_graph_write_vertices[] =                                                    //
    "graph_write_vertices(vertices, *, collection, durability=Durability.BUFFERED)\n"      //
    "answers None.\n"                                                                      //
    "\n"                                                                                   //
    "Inserts a batch of vertices without edges, keeping those already present.\n"          //
    "\n"                                                                                   //
    "Args:\n"                                                                              //
    "  vertices (Sequence[KeyLike] | buffer): The vertices to insert.\n"                   //
    "  collection (UUID | str): The graph collection, by identifier or name.\n"            //
    "  durability (Durability, optional): `Durability.FLUSHED` reaches the drive before\n" //
    "    returning.";

/** A write or a removal of a batch of vertices, which its caller gathers in place. */
typedef struct vertices_write_call_t {

    /** What the call keeps until it finishes. */
    ustore_py_call_t call;

    /** The vertices to write or remove, or inclusive starts of ranges when @c ends holds any. */
    ustore_py_keys_t vertices;

    /** Exclusive ends of vertex ranges to remove, one per vertex, or empty for vertices alone. */
    ustore_py_keys_t ends;

    /** The role every removed vertex plays. */
    ustore_vertex_role_t role;
} vertices_write_call_t;

static void vertices_write_submit(void* arguments) {
    ustore_graph_write_vertices((ustore_graph_write_vertices_t*)arguments);
}

static void vertices_remove_submit(void* arguments) {
    ustore_graph_remove_vertices((ustore_graph_remove_vertices_t*)arguments);
}

static void vertices_write_release(void* erased) {
    vertices_write_call_t* const call = (vertices_write_call_t*)erased;
    ustore_py_keys_release(&call->vertices);
    ustore_py_keys_release(&call->ends);
    ustore_py_call_release(&call->call);
}

static ustore_py_verb_t const vertices_write_verb = {sizeof(vertices_write_call_t), vertices_write_submit,
                                                     ustore_py_call_answer_none, vertices_write_release};

static ustore_py_verb_t const vertices_remove_verb = {sizeof(vertices_write_call_t), vertices_remove_submit,
                                                      ustore_py_call_answer_none, vertices_write_release};

PyObject* ustore_py_api_graph_write_vertices(PyObject* self, PyObject* const* arguments,
                                             Py_ssize_t positional_arguments_count, PyObject* keyword_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0 || ustore_py_context_writable(&context) != 0) return NULL;
    if (ustore_py_keyword_names_count_checked("graph_write_vertices", positional_arguments_count, keyword_names_tuple,
                                              1) < 0)
        return NULL;
    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, arguments, positional_arguments_count, keyword_names_tuple,
                                             &options) != 0)
        return NULL;

    vertices_write_call_t stack;
    vertices_write_call_t* const call = ustore_py_call_open(&context, &vertices_write_verb, &stack);
    if (!call) return NULL;
    if (ustore_py_keys_from_object(arguments[0], &call->vertices) != 0)
        return ustore_py_call_discard(&context, &vertices_write_verb, call);
    call->call.collection = options.collection;
    ustore_graph_write_vertices_t write = {
        .database = context.database_handle,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->call.outcome,
        .completion = context.completion,
        .transaction = context.transaction,
        .count = call->vertices.count,
        .collections = {&call->call.collection, 0},
        .vertices = {call->vertices.keys, sizeof(ustore_key_t)},
        .durability = options.durability,
        .watch = context.watch,
    };
    return ustore_py_call_run(&context, &vertices_write_verb, call, &write);
}

char const doc_graph_remove_vertices[] =                                                         //
    "graph_remove_vertices(vertices, *, role=Role.ANY, collection, ends=None,\n"                 //
    "                      durability=Durability.BUFFERED) answers None.\n"                      //
    "\n"                                                                                         //
    "Removes a batch of vertices together with every edge they take part in. With `ends`,\n"     //
    "every vertex starts a range removed up to its end, as `remove` takes them.\n"               //
    "\n"                                                                                         //
    "Args:\n"                                                                                    //
    "  vertices (Sequence[KeyLike] | buffer): The vertices to remove, or the first vertex of\n"  //
    "    every range.\n"                                                                         //
    "  ends (Sequence[KeyLike] | buffer, optional): The vertex every range stops before, one\n"  //
    "    per vertex.\n"                                                                          //
    "  role (Role, optional): The role every vertex plays, which only a joining graph between\n" //
    "    two collections needs; `Role.ANY` otherwise.\n"                                         //
    "  collection (UUID | str): The graph collection, by identifier or by name.\n"               //
    "  durability (Durability, optional): `Durability.FLUSHED` reaches the drive before\n"       //
    "    returning.";

PyObject* ustore_py_api_graph_remove_vertices(PyObject* self, PyObject* const* arguments,
                                              Py_ssize_t positional_arguments_count, PyObject* keyword_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0 || ustore_py_context_writable(&context) != 0) return NULL;
    if (ustore_py_keyword_names_count_checked("graph_remove_vertices", positional_arguments_count, keyword_names_tuple,
                                              1) < 0)
        return NULL;
    static char const* const names[] = {"role", "ends", NULL};
    PyObject* given[2];
    ustore_py_verb_options_t options;
    if (ustore_py_verb_keywords_from_arguments(&context, arguments, positional_arguments_count, keyword_names_tuple,
                                               names, &options, given) != 0)
        return NULL;
    int role = ustore_vertex_role_any_k;
    if (given[0] && ustore_py_enum_from_object(given[0], "role", ustore_py_enum_role_k, &role) != 0) return NULL;

    vertices_write_call_t stack;
    vertices_write_call_t* const call = ustore_py_call_open(&context, &vertices_remove_verb, &stack);
    if (!call) return NULL;
    if (ustore_py_keys_from_object(arguments[0], &call->vertices) != 0 ||
        ustore_py_ends_from_object(given[1], &call->vertices, &call->ends) != 0)
        return ustore_py_call_discard(&context, &vertices_remove_verb, call);
    call->role = (ustore_vertex_role_t)role;
    call->call.collection = options.collection;
    ustore_graph_remove_vertices_t remove = {
        .database = context.database_handle,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->call.outcome,
        .completion = context.completion,
        .transaction = context.transaction,
        .count = call->vertices.count,
        .collections = {&call->call.collection, 0},
        .vertices = {call->vertices.keys, sizeof(ustore_key_t)},
        .end_vertices = ustore_py_ends_view(&call->ends),
        .roles = {&call->role, 0},
        .durability = options.durability,
        .watch = context.watch,
    };
    return ustore_py_call_run(&context, &vertices_remove_verb, call, &remove);
}

char const doc_graph_scan_vertices[] =                                                          //
    "graph_scan_vertices(start, wanted, arena, *, collection, end=None,\n"                      //
    "direction=ScanDirection.ASCENDING, mode=ScanMode.ORDERED, progress=None)\n"                //
    "answers a Keys tape.\n"                                                                    //
    "\n"                                                                                        //
    "Lists up to `wanted` vertices from `start` towards `end`, those without edges included.\n" //
    "Edges are not exported, so a `graph_read_edges` follows the scan.\n"                       //
    "\n"                                                                                        //
    "Args:\n"                                                                                   //
    "  start (KeyLike | None): The first vertex the scan may return, or None for the first\n"   //
    "    vertex in its direction.\n"                                                            //
    "  wanted (int): Most vertices the scan returns.\n"                                         //
    "  arena (Arena): Where the vertices land, rewound by this call.\n"                         //
    "  collection (UUID | str): The graph collection, by identifier or by name.\n"              //
    "  end (KeyLike, optional): The vertex the scan stops before, or None for no bound.\n"      //
    "  direction (ScanDirection, optional): Which way the scan walks, as `scan` takes it.\n"    //
    "  mode (ScanMode, optional): In what order the vertices come, as `scan` takes it.\n"       //
    "  progress (Callable[[int, int], bool | None], optional): Called with the scans\n"         //
    "    finished and the scans in all; a falsy answer other than None cancels the call.\n"     //
    "Returns:\n"                                                                                //
    "  Keys: One run of vertices, in the order of the direction.\n"                             //
    "Raises:\n"                                                                                 //
    "  CancelledError: The progress callable stopped the scan.";

PyObject* ustore_py_api_graph_scan_vertices(PyObject* self, PyObject* const* arguments,
                                            Py_ssize_t positional_arguments_count, PyObject* keyword_names_tuple) {
    return ustore_py_api_scan_of(self, arguments, positional_arguments_count, keyword_names_tuple,
                                 "graph_scan_vertices", ustore_modality_graph_k);
}

char const doc_graph_sample_vertices[] =                                                  //
    "graph_sample_vertices(wanted, arena, *, collection) answers a Keys tape.\n"          //
    "\n"                                                                                  //
    "Draws vertices uniformly at random from one graph collection, those without edges\n" //
    "included, as `sample` draws the keys of blobs.";

PyObject* ustore_py_api_graph_sample_vertices(PyObject* self, PyObject* const* arguments,
                                              Py_ssize_t positional_arguments_count, PyObject* keyword_names_tuple) {
    return ustore_py_api_sample_of(self, arguments, positional_arguments_count, keyword_names_tuple,
                                   "graph_sample_vertices", ustore_modality_graph_k);
}

char const doc_graph_measure_vertices[] =                                                           //
    "graph_measure_vertices(start, end, arena, *, collection, direction=ScanDirection.ASCENDING)\n" //
    "answers a Measurement.\n"                                                                      //
    "\n"                                                                                            //
    "Estimates what a range of vertices holds without walking it, as `measure` does for blobs.";

PyObject* ustore_py_api_graph_measure_vertices(PyObject* self, PyObject* const* arguments,
                                               Py_ssize_t positional_arguments_count, PyObject* keyword_names_tuple) {
    return ustore_py_api_measure_of(self, arguments, positional_arguments_count, keyword_names_tuple,
                                    "graph_measure_vertices", ustore_modality_graph_k);
}

#pragma endregion Vertices

#pragma region Neighborhood Tapes

static char const doc_Neighborhoods[] =                                                           //
    "The edges one batch read found around its vertices, viewing its `Arena` until it rewinds.\n" //
    "\n"                                                                                          //
    "Indexing answers a tuple of (source, target, edge) triples of `uuid.UUID` for one\n"         //
    "vertex, or None where the vertex is absent, which is where a copy happens. The whole\n"      //
    "tape exports through the buffer protocol as an array of shape (edges, 3, 16) bytes,\n"       //
    "copying nothing and pinning the arena against the next call until released. Read after\n"    //
    "a later call rewound the arena, it raises `UninitializedError`.";

static void Neighborhoods_dealloc(Neighborhoods* self) {
    Py_XDECREF(self->arena);
    PyObject_Free(self);
}

static Py_ssize_t Neighborhoods_length(Neighborhoods* self) { return (Py_ssize_t)self->count; }

/** The triples of vertex @p vertex_index of @p self as a tuple, or None where the vertex is absent. */
static PyObject* neighborhood_triples(Neighborhoods const* self, size_t vertex_index) {
    if (self->degrees[vertex_index] == ustore_vertex_degree_missing_k) Py_RETURN_NONE;
    size_t const offset = self->offsets[vertex_index];
    Py_ssize_t const found = (Py_ssize_t)(self->offsets[vertex_index + 1] - offset);
    PyObject* edges = PyTuple_New(found);
    for (Py_ssize_t edge_index = 0; edges && edge_index != found; ++edge_index) {
        ustore_key_t const* const members = self->tape + (offset + (size_t)edge_index) * 3;
        PyObject* const source = ustore_py_uuid_to_object(&members[0].uuid);
        PyObject* const target = source ? ustore_py_uuid_to_object(&members[1].uuid) : NULL;
        PyObject* const edge = target ? ustore_py_uuid_to_object(&members[2].uuid) : NULL;
        PyObject* const triple = edge ? PyTuple_Pack(3, source, target, edge) : NULL;
        Py_XDECREF(source);
        Py_XDECREF(target);
        Py_XDECREF(edge);
        if (!triple) Py_CLEAR(edges);
        else PyTuple_SET_ITEM(edges, edge_index, triple);
    }
    return edges;
}

static PyObject* Neighborhoods_item(Neighborhoods* self, Py_ssize_t vertex_index) {
    if (vertex_index < 0 || (size_t)vertex_index >= self->count) {
        PyErr_SetString(PyExc_IndexError, "Vertex index out of range");
        return NULL;
    }
    if (ustore_py_arena_pin(self->arena, self->generation) != 0) return NULL;
    PyObject* const triples = neighborhood_triples(self, (size_t)vertex_index);
    ustore_py_arena_unpin(self->arena);
    return triples;
}

static int Neighborhoods_getbuffer(Neighborhoods* self, Py_buffer* view, int flags) {
    view->obj = NULL;
    if (ustore_py_arena_pin(self->arena, self->generation) != 0) return -1;
    if (ustore_py_buffer_fill(view, (PyObject*)self, self->tape, "B", 1, 3, self->shape, self->strides, flags) == 0)
        return 0;
    ustore_py_arena_unpin(self->arena);
    return -1;
}

static void Neighborhoods_releasebuffer(Neighborhoods* self, Py_buffer* Py_UNUSED(view)) {
    ustore_py_arena_unpin(self->arena);
}

static PyObject* Neighborhoods_repr(Neighborhoods* self) {
    return PyUnicode_FromFormat("Neighborhoods(vertices=%zu, edges=%zd)", self->count, self->shape[0]);
}

static char const doc_Neighborhoods_degrees[] = "The degree of every vertex, with None where it is absent, as a tuple.";

static PyObject* Neighborhoods_get_degrees(Neighborhoods* self, void* Py_UNUSED(closure)) {
    if (ustore_py_arena_pin(self->arena, self->generation) != 0) return NULL;
    PyObject* const degrees = degrees_tuple(self->degrees, self->count);
    ustore_py_arena_unpin(self->arena);
    return degrees;
}

static PyGetSetDef Neighborhoods_getset[] = {
    {"degrees", (getter)Neighborhoods_get_degrees, NULL, doc_Neighborhoods_degrees, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PySequenceMethods Neighborhoods_as_sequence = {
    .sq_length = (lenfunc)Neighborhoods_length,
    .sq_item = (ssizeargfunc)Neighborhoods_item,
};

static PyBufferProcs Neighborhoods_as_buffer = {
    .bf_getbuffer = (getbufferproc)Neighborhoods_getbuffer,
    .bf_releasebuffer = (releasebufferproc)Neighborhoods_releasebuffer,
};

PyTypeObject NeighborhoodsType = {
    PyVarObject_HEAD_INIT(NULL, 0) //
        .tp_name = "ustore.Neighborhoods",
    .tp_doc = doc_Neighborhoods,
    .tp_basicsize = sizeof(Neighborhoods),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = NULL,
    .tp_dealloc = (destructor)Neighborhoods_dealloc,
    .tp_repr = (reprfunc)Neighborhoods_repr,
    .tp_as_sequence = &Neighborhoods_as_sequence,
    .tp_as_buffer = &Neighborhoods_as_buffer,
    .tp_getset = Neighborhoods_getset,
};

#pragma endregion Neighborhood Tapes
