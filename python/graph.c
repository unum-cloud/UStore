/**
 *  @file python/graph.c
 *  @author Ash Vardanian
 *  @date September 17, 2026
 *  @brief Graphs: edge and vertex upserts and removals, degrees and edge lookups.
 *
 *  A graph collection maps every vertex onto its sorted neighborships, so an edge is three keys -
 *  source, target and edge identifier - and a lookup answers a tape of such triples. Every verb
 *  claims the database's arena, drops the interpreter lock around the call, and either returns the
 *  arena or hands it to the tape it answers with.
 */
#include "lib.h"

#pragma region Shared Arguments

/** The role a vertex plays in the wanted edges, as a caller spells it. */
static ustore_py_choice_naming_t const role_namings[] = {
    {"source", ustore_vertex_source_k},
    {"target", ustore_vertex_target_k},
    {"any", ustore_vertex_role_any_k},
    {NULL, 0},
};

/**
 *  @brief Walks the keywords of a verb whose only keyword of its own is the vertex role.
 *  @param[in] context The database and view the verb runs against.
 *  @param[in] verb The method's name, as a failure should read it.
 *  @param[in] args The fast-call arguments, positional ones first.
 *  @param[in] positional_args_count Positional arguments the call carried.
 *  @param[in] args_names_tuple The keyword names, or NULL when none followed.
 *  @param[out] options Where every shared keyword lands.
 *  @param[out] role The role the call names, which starts as either end of an edge.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
static int role_options_from_keywords(ustore_py_context_t const* context, char const* verb, PyObject* const* args,
                                      Py_ssize_t positional_args_count, PyObject* args_names_tuple,
                                      ustore_py_verb_options_t* options, ustore_vertex_role_t* role) {
    Py_ssize_t const args_names_count = ustore_py_args_names_count_checked(verb, positional_args_count,
                                                                           args_names_tuple, 1);
    if (args_names_count < 0) return -1;
    ustore_py_verb_options_init(context, options);
    int chosen = ustore_vertex_role_any_k;
    for (Py_ssize_t names_progress = 0, args_progress = positional_args_count; names_progress != args_names_count;
         ++args_progress, ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(args_names_tuple, names_progress);
        int const consumed = ustore_py_verb_option_from_keyword(context, name, args[args_progress], options);
        if (consumed < 0) return -1;
        if (consumed > 0) continue;
        if (PyUnicode_CompareWithASCIIString(name, "role") != 0) {
            PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
            return -1;
        }
        if (ustore_py_choice_from_object(args[args_progress], "role", role_namings, &chosen) != 0) return -1;
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

    /** The collection, view and flags the call names. */
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
 *  @param[in] args The fast-call arguments, positional ones first.
 *  @param[in] positional_args_count Positional arguments the call carried.
 *  @param[in] args_names_tuple The keyword names, or NULL when none followed.
 *  @param[out] batches The keys and options the verb runs with.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
static int edge_batches_from_args(ustore_py_context_t const* context, char const* verb, PyObject* const* args,
                                  Py_ssize_t positional_args_count, PyObject* args_names_tuple,
                                  edge_batches_t* batches) {
    memset(batches, 0, sizeof(*batches));
    Py_ssize_t const args_names_count = ustore_py_args_names_count_checked(verb, positional_args_count,
                                                                           args_names_tuple, 2);
    if (args_names_count < 0) return -1;
    ustore_py_verb_options_init(context, &batches->options);
    PyObject* edges_object = Py_None;
    for (Py_ssize_t names_progress = 0, args_progress = positional_args_count; names_progress != args_names_count;
         ++args_progress, ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(args_names_tuple, names_progress);
        int const consumed = ustore_py_verb_option_from_keyword(context, name, args[args_progress], &batches->options);
        if (consumed < 0) return -1;
        if (consumed > 0) continue;
        if (PyUnicode_CompareWithASCIIString(name, "edges") != 0) {
            PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
            return -1;
        }
        edges_object = args[args_progress];
    }

    if (ustore_py_keys_from_object(args[0], &batches->sources) != 0) return -1;
    if (ustore_py_keys_from_object(args[1], &batches->targets) != 0 ||
        (edges_object != Py_None && ustore_py_keys_from_object(edges_object, &batches->edges) != 0)) {
        edge_batches_release(batches);
        return -1;
    }
    size_t const count = batches->sources.count;
    if (batches->targets.count != count || (edges_object != Py_None && batches->edges.count != count)) {
        PyErr_Format(PyExc_ValueError, "Got %zu sources, %zu targets and %zu edge identifiers", count,
                     batches->targets.count, batches->edges.count);
        edge_batches_release(batches);
        return -1;
    }
    return 0;
}

#pragma endregion Shared Arguments

#pragma region Edges

char const doc_graph_upsert_edges[] =                                                         //
    "graph_upsert_edges(sources, targets, *, edges=None, collection=None, flush=False)\n"     //
    "answers None.\n"                                                                         //
    "\n"                                                                                      //
    "Inserts a batch of edges, creating the vertices they connect. An edge already present\n" //
    "is kept, so upserting the same batch twice changes nothing.\n"                           //
    "\n"                                                                                      //
    "Args:\n"                                                                                 //
    "  sources (Sequence[int] | buffer): The vertex every edge leaves.\n"                     //
    "  targets (Sequence[int] | buffer): The vertex every edge enters.\n"                     //
    "  edges (Sequence[int] | buffer, optional): The identifier of every edge, which tells\n" //
    "    apart several edges between one pair; None gives every edge `default_edge_id`.\n"    //
    "  collection (int | str, optional): The graph collection, by identifier or by name.\n"   //
    "  flush (bool, optional): Whether to reach the drive before returning.";

PyObject* ustore_py_api_graph_upsert_edges(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                           PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    edge_batches_t batches;
    if (edge_batches_from_args(&context, "graph_upsert_edges", args, positional_args_count, args_names_tuple,
                               &batches) != 0)
        return NULL;

    ustore_error_t error = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context.database);

    ustore_graph_upsert_edges_t upsert = {0};
    upsert.db = context.database->handle;
    upsert.error = &error;
    upsert.transaction = context.transaction;
    upsert.arena = &arena;
    upsert.options = batches.options.options;
    upsert.tasks_count = batches.sources.count;
    upsert.collections = &batches.options.collection;
    upsert.edges_ids = batches.edges.keys;
    upsert.edges_stride = sizeof(ustore_key_t);
    upsert.sources_ids = batches.sources.keys;
    upsert.sources_stride = sizeof(ustore_key_t);
    upsert.targets_ids = batches.targets.keys;
    upsert.targets_stride = sizeof(ustore_key_t);

    Py_BEGIN_ALLOW_THREADS;
    ustore_graph_upsert_edges(&upsert);
    Py_END_ALLOW_THREADS;

    edge_batches_release(&batches);
    ustore_py_arena_return(context.database, arena);
    if (ustore_py_raise_on_error(&error) != 0) return NULL;
    Py_RETURN_NONE;
}

char const doc_graph_remove_edges[] =                                                          //
    "graph_remove_edges(sources, targets, *, edges=None, collection=None, flush=False)\n"      //
    "answers None.\n"                                                                          //
    "\n"                                                                                       //
    "Removes a batch of edges and keeps the vertices they connected, even once isolated.\n"    //
    "\n"                                                                                       //
    "Args:\n"                                                                                  //
    "  sources (Sequence[int] | buffer): The vertex every edge leaves.\n"                      //
    "  targets (Sequence[int] | buffer): The vertex every edge enters.\n"                      //
    "  edges (Sequence[int] | buffer, optional): The identifier of every edge; None removes\n" //
    "    the edges that were given `default_edge_id`.\n"                                       //
    "  collection (int | str, optional): The graph collection, by identifier or by name.\n"    //
    "  flush (bool, optional): Whether to reach the drive before returning.";

PyObject* ustore_py_api_graph_remove_edges(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                           PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    edge_batches_t batches;
    if (edge_batches_from_args(&context, "graph_remove_edges", args, positional_args_count, args_names_tuple,
                               &batches) != 0)
        return NULL;

    ustore_error_t error = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context.database);

    ustore_graph_remove_edges_t remove = {0};
    remove.db = context.database->handle;
    remove.error = &error;
    remove.transaction = context.transaction;
    remove.arena = &arena;
    remove.options = batches.options.options;
    remove.tasks_count = batches.sources.count;
    remove.collections = &batches.options.collection;
    remove.edges_ids = batches.edges.keys;
    remove.edges_stride = sizeof(ustore_key_t);
    remove.sources_ids = batches.sources.keys;
    remove.sources_stride = sizeof(ustore_key_t);
    remove.targets_ids = batches.targets.keys;
    remove.targets_stride = sizeof(ustore_key_t);

    Py_BEGIN_ALLOW_THREADS;
    ustore_graph_remove_edges(&remove);
    Py_END_ALLOW_THREADS;

    edge_batches_release(&batches);
    ustore_py_arena_return(context.database, arena);
    if (ustore_py_raise_on_error(&error) != 0) return NULL;
    Py_RETURN_NONE;
}

/**
 *  @brief Wraps a tape of edges and the arena holding it into an @c Edges.
 *  @param[inout] arena The arena the result adopts; it is freed on failure.
 *  @param[in] tape Three keys per edge, laid end to end vertex after vertex.
 *  @param[in] degrees The degree of every vertex.
 *  @param[in] count Vertices the lookup carried.
 *  @return A new reference, or NULL with an exception set.
 */
static PyObject* edges_adopt(ustore_arena_t arena, ustore_key_t* tape, ustore_vertex_degree_t* degrees, size_t count) {
    size_t* const offsets = (size_t*)PyMem_Malloc((count + 1) * sizeof(size_t));
    Edges* const instance = offsets ? PyObject_New(Edges, &EdgesType) : NULL;
    if (!instance) {
        if (!offsets) PyErr_NoMemory();
        PyMem_Free(offsets);
        ustore_arena_free(arena);
        return NULL;
    }
    // A missing vertex reports a sentinel degree yet holds no edges on the tape.
    offsets[0] = 0;
    for (size_t vertex_index = 0; vertex_index != count; ++vertex_index) {
        ustore_vertex_degree_t const degree = degrees[vertex_index];
        offsets[vertex_index + 1] = offsets[vertex_index] + (degree == ustore_vertex_degree_missing_k ? 0 : degree);
    }
    instance->arena = arena;
    instance->tape = tape;
    instance->degrees = degrees;
    instance->offsets = offsets;
    instance->count = count;
    instance->shape[0] = (Py_ssize_t)offsets[count];
    instance->shape[1] = 3;
    instance->strides[0] = 3 * (Py_ssize_t)sizeof(ustore_key_t);
    instance->strides[1] = (Py_ssize_t)sizeof(ustore_key_t);
    return (PyObject*)instance;
}

char const doc_graph_find_edges[] =                                                             //
    "graph_find_edges(vertices, *, role='any', collection=None, snapshot=0) answers Edges.\n"   //
    "\n"                                                                                        //
    "Finds every edge a batch of vertices takes part in. The edges of one vertex come sorted\n" //
    "by neighbour; with both roles the outgoing ones precede the incoming ones.\n"              //
    "\n"                                                                                        //
    "Args:\n"                                                                                   //
    "  vertices (Sequence[int] | buffer): The vertices whose edges are found.\n"                //
    "  role (str, optional): 'source' for the edges a vertex leaves by, 'target' for those\n"   //
    "    it is entered by, or 'any' for both, which is how an undirected graph asks.\n"         //
    "  collection (int | str, optional): The graph collection, by identifier or by name.\n"     //
    "  snapshot (int, optional): The snapshot to read; 0 reads the head.\n"                     //
    "Returns:\n"                                                                                //
    "  Edges: One run of (source, target, edge) triples per vertex, None where it is absent.";

PyObject* ustore_py_api_graph_find_edges(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                         PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    ustore_py_verb_options_t options;
    ustore_vertex_role_t role = ustore_vertex_role_any_k;
    if (role_options_from_keywords(&context, "graph_find_edges", args, positional_args_count, args_names_tuple,
                                   &options, &role) != 0)
        return NULL;

    ustore_py_keys_t vertices;
    if (ustore_py_keys_from_object(args[0], &vertices) != 0) return NULL;

    ustore_vertex_degree_t* degrees = NULL;
    ustore_key_t* tape = NULL;
    ustore_error_t error = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context.database);

    ustore_graph_find_edges_t find = {0};
    find.db = context.database->handle;
    find.error = &error;
    find.transaction = context.transaction;
    find.snapshot = options.snapshot;
    find.arena = &arena;
    find.options = options.options;
    find.tasks_count = vertices.count;
    find.collections = &options.collection;
    find.vertices = vertices.keys;
    find.vertices_stride = sizeof(ustore_key_t);
    find.roles = &role;
    find.degrees_per_vertex = &degrees;
    find.edges_per_vertex = &tape;

    Py_BEGIN_ALLOW_THREADS;
    ustore_graph_find_edges(&find);
    Py_END_ALLOW_THREADS;

    size_t const vertices_count = vertices.count;
    ustore_py_keys_release(&vertices);
    if (error.status != ustore_success_k) {
        ustore_py_arena_return(context.database, arena);
        ustore_py_raise_on_error(&error);
        return NULL;
    }
    return edges_adopt(arena, tape, degrees, vertices_count);
}

char const doc_graph_degrees[] =                                                             //
    "graph_degrees(vertices, *, role='any', collection=None, snapshot=0) answers a tuple.\n" //
    "\n"                                                                                     //
    "Counts the edges of a batch of vertices without exporting them.\n"                      //
    "\n"                                                                                     //
    "Args:\n"                                                                                //
    "  vertices (Sequence[int] | buffer): The vertices whose edges are counted.\n"           //
    "  role (str, optional): 'source', 'target' or 'any', as `graph_find_edges` takes it.\n" //
    "  collection (int | str, optional): The graph collection, by identifier or by name.\n"  //
    "  snapshot (int, optional): The snapshot to read; 0 reads the head.\n"                  //
    "Returns:\n"                                                                             //
    "  tuple[int | None, ...]: The degree of every vertex, None where it is absent.";

PyObject* ustore_py_api_graph_degrees(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                      PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    ustore_py_verb_options_t options;
    ustore_vertex_role_t role = ustore_vertex_role_any_k;
    if (role_options_from_keywords(&context, "graph_degrees", args, positional_args_count, args_names_tuple, &options,
                                   &role) != 0)
        return NULL;

    ustore_py_keys_t vertices;
    if (ustore_py_keys_from_object(args[0], &vertices) != 0) return NULL;

    ustore_vertex_degree_t* degrees = NULL;
    ustore_error_t error = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context.database);

    ustore_graph_find_edges_t find = {0};
    find.db = context.database->handle;
    find.error = &error;
    find.transaction = context.transaction;
    find.snapshot = options.snapshot;
    find.arena = &arena;
    find.options = options.options;
    find.tasks_count = vertices.count;
    find.collections = &options.collection;
    find.vertices = vertices.keys;
    find.vertices_stride = sizeof(ustore_key_t);
    find.roles = &role;
    find.degrees_per_vertex = &degrees;

    Py_BEGIN_ALLOW_THREADS;
    ustore_graph_find_edges(&find);
    Py_END_ALLOW_THREADS;

    size_t const vertices_count = vertices.count;
    ustore_py_keys_release(&vertices);
    PyObject* counted = NULL;
    if (error.status != ustore_success_k) { ustore_py_raise_on_error(&error); }
    else if ((counted = PyTuple_New((Py_ssize_t)vertices_count)) != NULL) {
        for (size_t vertex_index = 0; vertex_index != vertices_count; ++vertex_index) {
            ustore_vertex_degree_t const degree = degrees[vertex_index];
            PyObject* const entry = degree == ustore_vertex_degree_missing_k ? Py_NewRef(Py_None)
                                                                             : PyLong_FromUnsignedLong(degree);
            if (!entry) {
                Py_CLEAR(counted);
                break;
            }
            PyTuple_SET_ITEM(counted, (Py_ssize_t)vertex_index, entry);
        }
    }
    ustore_py_arena_return(context.database, arena);
    return counted;
}

#pragma endregion Edges

#pragma region Vertices

char const doc_graph_upsert_vertices[] =                                                 //
    "graph_upsert_vertices(vertices, *, collection=None, flush=False) answers None.\n"   //
    "\n"                                                                                 //
    "Inserts a batch of vertices without edges, keeping those already present.\n"        //
    "\n"                                                                                 //
    "Args:\n"                                                                            //
    "  vertices (Sequence[int] | buffer): The vertices to insert.\n"                     //
    "  collection (int | str, optional): The graph collection, by identifier or name.\n" //
    "  flush (bool, optional): Whether to reach the drive before returning.";

PyObject* ustore_py_api_graph_upsert_vertices(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                              PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_args_names_count_checked("graph_upsert_vertices", positional_args_count, args_names_tuple, 1) < 0)
        return NULL;
    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, args, positional_args_count, args_names_tuple, &options) != 0)
        return NULL;

    ustore_py_keys_t vertices;
    if (ustore_py_keys_from_object(args[0], &vertices) != 0) return NULL;

    ustore_error_t error = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context.database);

    ustore_graph_upsert_vertices_t upsert = {0};
    upsert.db = context.database->handle;
    upsert.error = &error;
    upsert.transaction = context.transaction;
    upsert.arena = &arena;
    upsert.options = options.options;
    upsert.tasks_count = vertices.count;
    upsert.collections = &options.collection;
    upsert.vertices = vertices.keys;
    upsert.vertices_stride = sizeof(ustore_key_t);

    Py_BEGIN_ALLOW_THREADS;
    ustore_graph_upsert_vertices(&upsert);
    Py_END_ALLOW_THREADS;

    ustore_py_keys_release(&vertices);
    ustore_py_arena_return(context.database, arena);
    if (ustore_py_raise_on_error(&error) != 0) return NULL;
    Py_RETURN_NONE;
}

char const doc_graph_remove_vertices[] =                                                           //
    "graph_remove_vertices(vertices, *, role='any', collection=None, flush=False) answers None.\n" //
    "\n"                                                                                           //
    "Removes a batch of vertices together with every edge they take part in.\n"                    //
    "\n"                                                                                           //
    "Args:\n"                                                                                      //
    "  vertices (Sequence[int] | buffer): The vertices to remove.\n"                               //
    "  role (str, optional): The role every vertex plays, which only a joining graph between\n"    //
    "    two collections needs; 'any' otherwise.\n"                                                //
    "  collection (int | str, optional): The graph collection, by identifier or by name.\n"        //
    "  flush (bool, optional): Whether to reach the drive before returning.";

PyObject* ustore_py_api_graph_remove_vertices(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                              PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    ustore_py_verb_options_t options;
    ustore_vertex_role_t role = ustore_vertex_role_any_k;
    if (role_options_from_keywords(&context, "graph_remove_vertices", args, positional_args_count, args_names_tuple,
                                   &options, &role) != 0)
        return NULL;

    ustore_py_keys_t vertices;
    if (ustore_py_keys_from_object(args[0], &vertices) != 0) return NULL;

    ustore_error_t error = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context.database);

    ustore_graph_remove_vertices_t remove = {0};
    remove.db = context.database->handle;
    remove.error = &error;
    remove.transaction = context.transaction;
    remove.arena = &arena;
    remove.options = options.options;
    remove.tasks_count = vertices.count;
    remove.collections = &options.collection;
    remove.vertices = vertices.keys;
    remove.vertices_stride = sizeof(ustore_key_t);
    remove.roles = &role;

    Py_BEGIN_ALLOW_THREADS;
    ustore_graph_remove_vertices(&remove);
    Py_END_ALLOW_THREADS;

    ustore_py_keys_release(&vertices);
    ustore_py_arena_return(context.database, arena);
    if (ustore_py_raise_on_error(&error) != 0) return NULL;
    Py_RETURN_NONE;
}

#pragma endregion Vertices

#pragma region Edge Tapes

static char const doc_Edges[] =                                                                  //
    "The edges one batch lookup found, read straight out of the arena holding them.\n"           //
    "\n"                                                                                         //
    "Indexing answers a `memoryview` of shape (degree, 3) holding the (source, target, edge)\n"  //
    "triples of one vertex, or None where the vertex is absent; the whole tape exports as one\n" //
    "array of shape (edges, 3). Nothing is copied, and every view keeps this object, and\n"      //
    "therefore the arena, alive.";

static void Edges_dealloc(Edges* self) {
    ustore_arena_free(self->arena);
    PyMem_Free(self->offsets);
    PyObject_Free(self);
}

static Py_ssize_t Edges_length(Edges* self) { return (Py_ssize_t)self->count; }

static PyObject* Edges_item(Edges* self, Py_ssize_t vertex_index) {
    if (vertex_index < 0 || (size_t)vertex_index >= self->count) {
        PyErr_SetString(PyExc_IndexError, "Vertex index out of range");
        return NULL;
    }
    if (self->degrees[vertex_index] == ustore_vertex_degree_missing_k) Py_RETURN_NONE;
    size_t const offset = self->offsets[vertex_index];
    Py_ssize_t const shape[2] = {(Py_ssize_t)(self->offsets[vertex_index + 1] - offset), 3};
    void* const data = self->tape ? (void*)(self->tape + offset * 3) : NULL;
    return ustore_py_span_view((PyObject*)self, data, "q", sizeof(ustore_key_t), 2, shape);
}

static int Edges_getbuffer(Edges* self, Py_buffer* view, int flags) {
    return ustore_py_buffer_fill(view, (PyObject*)self, self->tape, "q", sizeof(ustore_key_t), 2, self->shape,
                                 self->strides, flags);
}

static PyObject* Edges_repr(Edges* self) {
    return PyUnicode_FromFormat("Edges(vertices=%zu, edges=%zd)", self->count, self->shape[0]);
}

static char const doc_Edges_degrees[] = "The degree of every vertex, with None where it is absent, as a tuple.";

static PyObject* Edges_get_degrees(Edges* self, void* Py_UNUSED(closure)) {
    PyObject* const degrees = PyTuple_New((Py_ssize_t)self->count);
    if (!degrees) return NULL;
    for (size_t vertex_index = 0; vertex_index != self->count; ++vertex_index) {
        ustore_vertex_degree_t const degree = self->degrees[vertex_index];
        PyObject* const entry = degree == ustore_vertex_degree_missing_k ? Py_NewRef(Py_None)
                                                                         : PyLong_FromUnsignedLong(degree);
        if (!entry) {
            Py_DECREF(degrees);
            return NULL;
        }
        PyTuple_SET_ITEM(degrees, (Py_ssize_t)vertex_index, entry);
    }
    return degrees;
}

static PyGetSetDef Edges_getset[] = {
    {"degrees", (getter)Edges_get_degrees, NULL, doc_Edges_degrees, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PySequenceMethods Edges_as_sequence = {
    .sq_length = (lenfunc)Edges_length,
    .sq_item = (ssizeargfunc)Edges_item,
};

static PyBufferProcs Edges_as_buffer = {
    .bf_getbuffer = (getbufferproc)Edges_getbuffer,
};

PyTypeObject EdgesType = {
    PyVarObject_HEAD_INIT(NULL, 0) //
        .tp_name = "ustore.Edges",
    .tp_doc = doc_Edges,
    .tp_basicsize = sizeof(Edges),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = NULL,
    .tp_dealloc = (destructor)Edges_dealloc,
    .tp_repr = (reprfunc)Edges_repr,
    .tp_as_sequence = &Edges_as_sequence,
    .tp_as_buffer = &Edges_as_buffer,
    .tp_getset = Edges_getset,
};

#pragma endregion Edge Tapes
