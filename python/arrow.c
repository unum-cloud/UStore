/**
 *  @file python/arrow.c
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief Apache Arrow streams through the PyCapsule interface: the stream a collection exports, and the
 *      @c import_arrow and @c ingest_arrow verbs over any object exporting a stream or one array.
 *
 *  Nothing here imports pyarrow. An export walks the collection's cursor and hands each page out through the exporters
 *  of @c arrow.h, every batch in an arena of its own that its release frees. An import reads each batch's columns
 *  through the converters into the writes of the collection's modality, and an ingest appends them to one ingest.
 */
#include "lib.h"

#pragma region Sources

/** A schema and one batch moved out of the capsules of @c __arrow_c_array__, answered as a stream of one batch. */
typedef struct single_batch_stream_t {

    /** The schema of the batch, owned until the stream is released. */
    struct ArrowSchema schema;

    /** The batch, owned until the stream hands it out or is released. */
    struct ArrowArray batch;
} single_batch_stream_t;

/** Lets go of nothing, as the schema a single-batch stream answers borrows the one it owns. */
static void single_schema_release(struct ArrowSchema* schema) { schema->release = NULL; }

static int single_get_schema(struct ArrowArrayStream* stream, struct ArrowSchema* out) {
    single_batch_stream_t const* const single = (single_batch_stream_t const*)stream->private_data;
    *out = single->schema;
    out->release = single_schema_release;
    return 0;
}

static int single_get_next(struct ArrowArrayStream* stream, struct ArrowArray* out) {
    single_batch_stream_t* const single = (single_batch_stream_t*)stream->private_data;
    *out = single->batch;
    single->batch.release = NULL;
    return 0;
}

static char const* single_get_last_error(struct ArrowArrayStream* Py_UNUSED(stream)) { return NULL; }

static void single_release(struct ArrowArrayStream* stream) {
    single_batch_stream_t* const single = (single_batch_stream_t*)stream->private_data;
    if (single->schema.release) single->schema.release(&single->schema);
    if (single->batch.release) single->batch.release(&single->batch);
    stream->release = NULL;
}

/** The pointer @p capsule carries under @p name, or NULL with a TypeError naming the protocol @p method broke. */
static void* capsule_pointer(PyObject* capsule, char const* name, char const* method) {
    void* const pointer = PyCapsule_IsValid(capsule, name) ? PyCapsule_GetPointer(capsule, name) : NULL;
    if (!pointer) PyErr_Format(PyExc_TypeError, "%s must answer a capsule named '%s'", method, name);
    return pointer;
}

/** Calls the method @p name of @p source without arguments: 1 with its answer in @p answer, 0 when @p source has no
 *  such method, -1 failing. */
static int call_optional_method(PyObject* source, char const* name, PyObject** answer) {
    PyObject* const method = PyObject_GetAttrString(source, name);
    if (!method) {
        if (!PyErr_ExceptionMatches(PyExc_AttributeError)) return -1;
        PyErr_Clear();
        return 0;
    }
    *answer = PyObject_CallNoArgs(method);
    Py_DECREF(method);
    return *answer ? 1 : -1;
}

/**
 *  @brief Moves the stream @p source exports into @p stream, or the one array it exports into @p single, which
 *      @p stream then answers, leaving every capsule released.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
static int stream_from_object(PyObject* source, struct ArrowArrayStream* stream, single_batch_stream_t* single) {
    PyObject* answer = NULL;
    int const streamed = call_optional_method(source, "__arrow_c_stream__", &answer);
    if (streamed < 0) return -1;
    if (streamed) {
        struct ArrowArrayStream* const exported = capsule_pointer(answer, "arrow_array_stream", "__arrow_c_stream__");
        if (exported) {
            *stream = *exported;
            exported->release = NULL;
        }
        Py_DECREF(answer);
        return exported ? 0 : -1;
    }

    int const arrayed = call_optional_method(source, "__arrow_c_array__", &answer);
    if (arrayed < 0) return -1;
    if (!arrayed) {
        PyErr_Format(PyExc_TypeError,
                     "An Arrow source exports __arrow_c_stream__ or __arrow_c_array__, and %S has none",
                     Py_TYPE(source));
        return -1;
    }
    struct ArrowSchema* schema = NULL;
    struct ArrowArray* array = NULL;
    if (PyTuple_Check(answer) && PyTuple_GET_SIZE(answer) == 2) {
        schema = capsule_pointer(PyTuple_GET_ITEM(answer, 0), "arrow_schema", "__arrow_c_array__");
        array = schema ? capsule_pointer(PyTuple_GET_ITEM(answer, 1), "arrow_array", "__arrow_c_array__") : NULL;
    }
    else PyErr_SetString(PyExc_TypeError, "__arrow_c_array__ must answer a schema capsule and an array capsule");
    if (array) {
        single->schema = *schema, schema->release = NULL;
        single->batch = *array, array->release = NULL;
        stream->get_schema = single_get_schema;
        stream->get_next = single_get_next;
        stream->get_last_error = single_get_last_error;
        stream->release = single_release;
        stream->private_data = single;
    }
    Py_DECREF(answer);
    return array ? 0 : -1;
}

#pragma endregion Sources

#pragma region Schemas

/** Where a column sits among the children of a schema, or @c absent_column_k. */
static size_t const absent_column_k = (size_t)-1;

/** Where the columns of a modality sit among the children of a stream's schema. */
typedef struct modality_columns_t {

    /** Keys of blobs, documents and pages, or the keys strings map onto. */
    size_t keys;

    /** Values of blobs, documents and pages, or the strings of a strings collection. */
    size_t strings;

    /** The sources of edges. */
    size_t sources;

    /** The targets of edges. */
    size_t targets;

    /** The identifiers of edges, which a stream may leave out for the default edge. */
    size_t edges;
} modality_columns_t;

/** The views of one batch's columns, read in place, those its modality lacks left empty. */
typedef struct batch_views_t {
    ustore_size_t count;
    ustore_keys_view_t keys;
    ustore_strings_view_t strings;
    ustore_keys_view_t sources;
    ustore_keys_view_t targets;
    ustore_keys_view_t edges;
} batch_views_t;

/** Encodes @p count pairs of @p texts, keys and values interleaved, as Arrow metadata, in native 32-bit counts, into
 *  @p bytes, answering the bytes written, or measuring them for a NULL @p bytes. */
static size_t metadata_encode(char const* const* texts, size_t count, char* bytes) {
    size_t length = 0;
    int32_t const pairs = (int32_t)count;
    if (bytes) memcpy(bytes, &pairs, sizeof(pairs));
    length += sizeof(pairs);
    for (size_t index = 0; index != 2 * count; ++index) {
        int32_t const text_length = (int32_t)strlen(texts[index]);
        if (bytes) memcpy(bytes + length, &text_length, sizeof(text_length));
        length += sizeof(text_length);
        if (bytes) memcpy(bytes + length, texts[index], (size_t)text_length);
        length += (size_t)text_length;
    }
    return length;
}

/** The modality of @p collection as @p snapshot lists it, failing into @p outcome when absent. */
static ustore_modality_t listed_modality(ustore_database_t database, ustore_snapshot_t snapshot,
                                         ustore_collection_id_t collection, ustore_outcome_t* outcome) {
    ustore_arena_t arena = NULL;
    ustore_arena_init_t init = {.arena = &arena, .outcome = outcome};
    ustore_arena_init(&init);
    if (outcome->status != ustore_status_success_k) return ustore_modality_blobs_k;
    ustore_collection_id_t const* collections = NULL;
    ustore_modality_t const* modalities = NULL;
    ustore_size_t written = 0;
    ustore_collection_list_t list = {.database = database,
                                     .outcome = outcome,
                                     .snapshot = snapshot,
                                     .collections = &collections,
                                     .modalities = &modalities,
                                     .written = &written,
                                     .arena = arena};
    ustore_collection_list(&list);
    ustore_modality_t modality = ustore_modality_blobs_k;
    int found = 0;
    for (ustore_size_t index = 0; outcome->status == ustore_status_success_k && index != written; ++index)
        if (memcmp(&collections[index], &collection, sizeof(collection)) == 0) modality = modalities[index], found = 1;
    if (outcome->status == ustore_status_success_k && !found)
        *outcome = (ustore_outcome_t) {ustore_status_not_found_k, "The collection does not exist"};
    ustore_arena_free(arena);
    return modality;
}

#pragma endregion Schemas

#pragma region Exports

/** Rows one batch of an export holds at most, a graph counting its source vertices. */
static ustore_size_t const export_batch_rows_k = 65536;

/** What an exported stream holds between pulls, which no batch points back to. */
typedef struct export_stream_t {
    ustore_database_t database;
    ustore_view_t view;

    /** The snapshot of the head the stream took, freed after the cursor, or NULL for a caller's view. */
    ustore_snapshot_t snapshot;
    ustore_collection_id_t collection;

    /** What the collection holds, which names the member of @c cursor the stream opened. */
    ustore_modality_t modality;
    union {
        ustore_blobs_cursor_t blobs;
        ustore_docs_cursor_t docs;
        ustore_pages_cursor_t pages;
        ustore_strings_cursor_t strings;
        ustore_graph_vertices_cursor_t vertices;
    } cursor;

    /** Where a graph's vertices land before their edges are read into a batch's arena. */
    ustore_arena_t vertices_arena;

    /** Why the last callback failed. */
    ustore_outcome_t failure;
} export_stream_t;

/** One exported schema: the root, its columns, and the metadata they point at, freed by the root's release. */
typedef struct export_schema_t {
    struct ArrowSchema columns[3];
    struct ArrowSchema* children[3];
    char* metadata;
} export_schema_t;

/** What an exported column's release runs beside the exporter's own: its batch's live count. */
typedef struct export_column_t {
    struct export_batch_t* batch;
    void (*release)(struct ArrowArray*);
    void* private_data;
} export_column_t;

/** One exported batch: the columns the exporters handed out and the arena they live in, freed
 *  with the last of the batch and its columns, which a consumer may release in any order. */
typedef struct export_batch_t {
    struct ArrowArray columns[3];
    struct ArrowArray* children[3];
    struct ArrowSchema schemas[3];
    export_column_t holds[3];
    void const* root_buffers[1];
    ustore_arena_t arena;
    atomic_size_t live;
} export_batch_t;

static void export_column_schema_release(struct ArrowSchema* schema) { schema->release = NULL; }

static void export_schema_release(struct ArrowSchema* schema) {
    export_schema_t* const owned = (export_schema_t*)schema->private_data;
    PyMem_RawFree(owned->metadata);
    PyMem_RawFree(owned);
    schema->release = NULL;
}

static void export_batch_unshare(export_batch_t* owned) {
    if (atomic_fetch_sub_explicit(&owned->live, 1, memory_order_acq_rel) != 1) return;
    ustore_arena_free(owned->arena);
    PyMem_RawFree(owned);
}

static void export_column_release(struct ArrowArray* column) {
    export_column_t* const hold = (export_column_t*)column->private_data;
    column->release = hold->release;
    column->private_data = hold->private_data;
    column->release(column);
    export_batch_unshare(hold->batch);
}

/** Moves @p column into slot @p index of @p owned, counting it live until its release. */
static void export_column_hold(export_batch_t* owned, size_t index, struct ArrowArray column) {
    owned->holds[index] =
        (export_column_t) {.batch = owned, .release = column.release, .private_data = column.private_data};
    column.release = export_column_release;
    column.private_data = &owned->holds[index];
    owned->columns[index] = column;
    atomic_fetch_add_explicit(&owned->live, 1, memory_order_relaxed);
}

static void export_batch_release(struct ArrowArray* batch) {
    export_batch_t* const owned = (export_batch_t*)batch->private_data;
    for (int64_t index = 0; index != batch->n_children; ++index) {
        if (owned->columns[index].release) owned->columns[index].release(&owned->columns[index]);
        if (owned->schemas[index].release) owned->schemas[index].release(&owned->schemas[index]);
    }
    batch->release = NULL;
    export_batch_unshare(owned);
}

/** The names of the columns of @p modality and whether each one is ragged. */
static size_t export_columns_of(ustore_modality_t modality, char const** names, int* ragged) {
    if (modality == ustore_modality_graph_k) {
        names[0] = "source", names[1] = "target", names[2] = "edge";
        ragged[0] = ragged[1] = ragged[2] = 0;
        return 3;
    }
    names[0] = "key", names[1] = "value";
    ragged[0] = modality == ustore_modality_strings_k, ragged[1] = !ragged[0];
    return 2;
}

/** The @c errno code a stream callback answers for @p outcome, recording it as the stream's failure. */
static int export_failure(export_stream_t* state, ustore_outcome_t outcome) {
    state->failure = outcome;
    switch (outcome.status) {
    case ustore_status_success_k: return 0;
    case ustore_status_out_of_memory_k: return ENOMEM;
    case ustore_status_invalid_argument_k: return EINVAL;
    case ustore_status_unsupported_k: return ENOTSUP;
    case ustore_status_not_found_k: return ENOENT;
    case ustore_status_contended_k: return EBUSY;
    default: return EIO;
    }
}

static int export_get_schema(struct ArrowArrayStream* stream, struct ArrowSchema* out) {
    export_stream_t* const state = (export_stream_t*)stream->private_data;
    char const* names[3];
    int ragged[3];
    size_t const count = export_columns_of(state->modality, names, ragged);
    char const* const modality_pair[2] = {"ustore.modality", ustore_modality_name(state->modality)};
    char const* const uuid_pairs[4] = {"ARROW:extension:name", "arrow.uuid", "ARROW:extension:metadata", ""};
    char const* const xml_pair[2] = {"ustore.page_encoding", "xml"};
    size_t const modality_bytes = metadata_encode(modality_pair, 1, NULL);
    size_t const uuid_bytes = metadata_encode(uuid_pairs, 2, NULL);
    export_schema_t* const owned = (export_schema_t*)PyMem_RawCalloc(1, sizeof(export_schema_t));
    char* const metadata = owned ? (char*)PyMem_RawMalloc(modality_bytes + uuid_bytes + 64) : NULL;
    if (!metadata) {
        PyMem_RawFree(owned);
        return export_failure(state, (ustore_outcome_t) {ustore_status_out_of_memory_k, "Failed to allocate a schema"});
    }
    owned->metadata = metadata;
    metadata_encode(modality_pair, 1, metadata);
    char* const uuid_metadata = metadata + modality_bytes;
    metadata_encode(uuid_pairs, 2, uuid_metadata);
    char* const xml_metadata = uuid_metadata + uuid_bytes;
    metadata_encode(xml_pair, 1, xml_metadata);
    for (size_t index = 0; index != count; ++index) {
        int const pages_value = ragged[index] && state->modality == ustore_modality_pages_k;
        owned->columns[index] = (struct ArrowSchema) {
            .format = ragged[index] ? "z" : "w:16",
            .name = names[index],
            .metadata = ragged[index] ? (pages_value ? xml_metadata : NULL) : uuid_metadata,
            .release = export_column_schema_release,
        };
        owned->children[index] = &owned->columns[index];
    }
    *out = (struct ArrowSchema) {.format = "+s",
                                 .name = "",
                                 .metadata = metadata,
                                 .n_children = (int64_t)count,
                                 .children = owned->children,
                                 .release = export_schema_release,
                                 .private_data = owned};
    return 0;
}

/** Exports @p keys as column @p index of @p owned, failing into @p outcome. */
static void export_keys(export_batch_t* owned, size_t index, ustore_keys_view_t keys, ustore_size_t count,
                        ustore_arena_t arena, ustore_outcome_t* outcome) {
    if (outcome->status != ustore_status_success_k) return;
    struct ArrowDeviceArray column = {0};
    ustore_status_t const status = ustore_keys_view_to_arrow(keys, count, arena, &owned->schemas[index], &column);
    if (status != ustore_status_success_k)
        *outcome = (ustore_outcome_t) {status, "Failed to export the keys of a batch"};
    else export_column_hold(owned, index, column.array);
}

/** Exports @p strings as column @p index of @p owned as @c binary, which the stream's schema names. */
static void export_strings(export_batch_t* owned, size_t index, ustore_strings_view_t strings, ustore_size_t count,
                           ustore_arena_t arena, ustore_outcome_t* outcome) {
    if (outcome->status != ustore_status_success_k) return;
    struct ArrowDeviceArray column = {0};
    ustore_status_t const status = ustore_strings_view_to_arrow(strings, count, ustore_strings_u32_tape_k, arena,
                                                                &owned->schemas[index], &column);
    if (status != ustore_status_success_k)
        *outcome = (ustore_outcome_t) {status, "Failed to export the values of a batch"};
    else export_column_hold(owned, index, column.array);
}

/** Exports the keys and values a cursor pulled into @p arena as the two columns of @p owned. */
static void export_keyed(export_batch_t* owned, ustore_key_t const* keys, ustore_strings_view_t values,
                         ustore_size_t rows, ustore_arena_t arena, ustore_outcome_t* outcome) {
    if (outcome->status != ustore_status_success_k || !rows) return;
    export_keys(owned, 0, (ustore_keys_view_t) {keys, sizeof(ustore_key_t)}, rows, arena, outcome);
    export_strings(owned, 1, values, rows, arena, outcome);
}

/** Pulls the edges leaving the next pages of vertices, skipping pages whose vertices leave none, into @p arena, and
 *  exports their sources, targets and identifiers, answering the edges. */
static ustore_size_t export_edges(export_stream_t* state, export_batch_t* owned, ustore_arena_t arena,
                                  ustore_outcome_t* outcome) {
    for (;;) {
        ustore_key_t const* vertices = NULL;
        ustore_size_t written = 0;
        ustore_graph_vertices_cursor_next_t next = {.database = state->database,
                                                    .outcome = outcome,
                                                    .cursor = state->cursor.vertices,
                                                    .keys = &vertices,
                                                    .written = &written,
                                                    .arena = state->vertices_arena,
                                                    .wanted = export_batch_rows_k};
        ustore_graph_vertices_cursor_next(&next);
        if (outcome->status != ustore_status_success_k || !written) return 0;
        ustore_vertex_role_t const role = ustore_vertex_role_source_k;
        ustore_vertex_degree_t const* degrees = NULL;
        ustore_length_t const* offsets = NULL;
        ustore_key_t const* triples = NULL;
        ustore_graph_read_edges_t read = {.database = state->database,
                                          .outcome = outcome,
                                          .view = state->view,
                                          .count = written,
                                          .collections = {&state->collection, 0},
                                          .vertices = {vertices, sizeof(ustore_key_t)},
                                          .roles = {&role, 0},
                                          .degrees_per_vertex = &degrees,
                                          .edges_offsets = &offsets,
                                          .edges_per_vertex = &triples,
                                          .arena = arena};
        ustore_graph_read_edges(&read);
        if (outcome->status != ustore_status_success_k) return 0;
        ustore_size_t const edges = offsets ? (ustore_size_t)offsets[written] : 0;
        if (!edges && written == export_batch_rows_k) continue;
        for (size_t column = 0; edges && column != 3; ++column)
            export_keys(owned, column, (ustore_keys_view_t) {triples + column, 3 * sizeof(ustore_key_t)}, edges, arena,
                        outcome);
        return edges;
    }
}

static int export_get_next(struct ArrowArrayStream* stream, struct ArrowArray* out) {
    export_stream_t* const state = (export_stream_t*)stream->private_data;
    *out = (struct ArrowArray) {0};
    ustore_outcome_t outcome = {0};
    export_batch_t* const owned = (export_batch_t*)PyMem_RawCalloc(1, sizeof(export_batch_t));
    if (!owned) return export_failure(state, (ustore_outcome_t) {ustore_status_out_of_memory_k, "Failed to allocate"});
    atomic_init(&owned->live, 1);
    ustore_arena_init_t init = {.arena = &owned->arena, .outcome = &outcome};
    ustore_arena_init(&init);
    ustore_arena_t const arena = owned->arena;
    ustore_size_t rows = 0;
    if (outcome.status == ustore_status_success_k) switch (state->modality) {
        case ustore_modality_graph_k: rows = export_edges(state, owned, arena, &outcome); break;
        case ustore_modality_strings_k: {
            ustore_strings_view_t strings = {0};
            ustore_key_t const* values = NULL;
            ustore_strings_cursor_next_t next = {.database = state->database,
                                                 .outcome = &outcome,
                                                 .cursor = state->cursor.strings,
                                                 .strings = &strings,
                                                 .values = &values,
                                                 .written = &rows,
                                                 .arena = arena,
                                                 .wanted = export_batch_rows_k};
            ustore_strings_cursor_next(&next);
            if (outcome.status != ustore_status_success_k || !rows) break;
            export_strings(owned, 0, strings, rows, arena, &outcome);
            export_keys(owned, 1, (ustore_keys_view_t) {values, sizeof(ustore_key_t)}, rows, arena, &outcome);
        } break;
        case ustore_modality_docs_k: {
            ustore_key_t const* keys = NULL;
            ustore_strings_view_t values = {0};
            ustore_docs_cursor_next_t next = {.database = state->database,
                                              .outcome = &outcome,
                                              .cursor = state->cursor.docs,
                                              .keys = &keys,
                                              .values = &values,
                                              .written = &rows,
                                              .arena = arena,
                                              .wanted = export_batch_rows_k};
            ustore_docs_cursor_next(&next);
            export_keyed(owned, keys, values, rows, arena, &outcome);
        } break;
        case ustore_modality_pages_k: {
            ustore_key_t const* keys = NULL;
            ustore_strings_view_t values = {0};
            ustore_pages_cursor_next_t next = {.database = state->database,
                                               .outcome = &outcome,
                                               .cursor = state->cursor.pages,
                                               .keys = &keys,
                                               .values = &values,
                                               .written = &rows,
                                               .arena = arena,
                                               .wanted = export_batch_rows_k,
                                               .outputs_encoding = ustore_page_encoding_xml_k};
            ustore_pages_cursor_next(&next);
            export_keyed(owned, keys, values, rows, arena, &outcome);
        } break;
        default: {
            ustore_key_t const* keys = NULL;
            ustore_strings_view_t values = {0};
            ustore_blobs_cursor_next_t next = {.database = state->database,
                                               .outcome = &outcome,
                                               .cursor = state->cursor.blobs,
                                               .keys = &keys,
                                               .values = &values,
                                               .written = &rows,
                                               .arena = arena,
                                               .wanted = export_batch_rows_k};
            ustore_blobs_cursor_next(&next);
            export_keyed(owned, keys, values, rows, arena, &outcome);
        } break;
        }
    char const* names[3];
    int ragged[3];
    size_t const columns = export_columns_of(state->modality, names, ragged);
    struct ArrowArray batch = {.length = (int64_t)rows,
                               .n_buffers = 1,
                               .n_children = (int64_t)columns,
                               .buffers = owned->root_buffers,
                               .children = owned->children,
                               .release = export_batch_release,
                               .private_data = owned};
    for (size_t index = 0; index != columns; ++index) owned->children[index] = &owned->columns[index];
    if (outcome.status != ustore_status_success_k || !rows) {
        export_batch_release(&batch);
        return export_failure(state, outcome);
    }
    *out = batch;
    return 0;
}

static char const* export_get_last_error(struct ArrowArrayStream* stream) {
    return ((export_stream_t const*)stream->private_data)->failure.message;
}

static void export_stream_free(export_stream_t* state) {
    switch (state->modality) {
    case ustore_modality_docs_k: ustore_docs_cursor_free(state->cursor.docs); break;
    case ustore_modality_pages_k: ustore_pages_cursor_free(state->cursor.pages); break;
    case ustore_modality_strings_k: ustore_strings_cursor_free(state->cursor.strings); break;
    case ustore_modality_graph_k: ustore_graph_vertices_cursor_free(state->cursor.vertices); break;
    default: ustore_blobs_cursor_free(state->cursor.blobs); break;
    }
    ustore_arena_free(state->vertices_arena);
    ustore_snapshot_free(state->snapshot);
    PyMem_RawFree(state);
}

static void export_release(struct ArrowArrayStream* stream) {
    export_stream_free((export_stream_t*)stream->private_data);
    stream->release = NULL;
}

/** Opens the cursor of @p state's modality over its collection. */
static void export_open(export_stream_t* state, ustore_outcome_t* outcome) {
    switch (state->modality) {
    case ustore_modality_docs_k: {
        ustore_docs_cursor_init_t init = {.cursor = &state->cursor.docs,
                                          .database = state->database,
                                          .outcome = outcome,
                                          .view = state->view,
                                          .collection = state->collection};
        ustore_docs_cursor_init(&init);
    } break;
    case ustore_modality_pages_k: {
        ustore_pages_cursor_init_t init = {.cursor = &state->cursor.pages,
                                           .database = state->database,
                                           .outcome = outcome,
                                           .view = state->view,
                                           .collection = state->collection};
        ustore_pages_cursor_init(&init);
    } break;
    case ustore_modality_strings_k: {
        ustore_strings_cursor_init_t init = {.cursor = &state->cursor.strings,
                                             .database = state->database,
                                             .outcome = outcome,
                                             .view = state->view,
                                             .collection = state->collection};
        ustore_strings_cursor_init(&init);
    } break;
    case ustore_modality_graph_k: {
        ustore_arena_init_t arena = {.arena = &state->vertices_arena, .outcome = outcome};
        ustore_arena_init(&arena);
        if (outcome->status != ustore_status_success_k) return;
        ustore_graph_vertices_cursor_init_t init = {.cursor = &state->cursor.vertices,
                                                    .database = state->database,
                                                    .outcome = outcome,
                                                    .view = state->view,
                                                    .collection = state->collection};
        ustore_graph_vertices_cursor_init(&init);
    } break;
    default: {
        ustore_blobs_cursor_init_t init = {.cursor = &state->cursor.blobs,
                                           .database = state->database,
                                           .outcome = outcome,
                                           .view = state->view,
                                           .collection = state->collection};
        ustore_blobs_cursor_init(&init);
    } break;
    }
}

/** Releases the stream @p capsule still holds, unless a consumer moved it out, and frees its storage. */
static void stream_capsule_free(PyObject* capsule) {
    struct ArrowArrayStream* const stream = PyCapsule_GetPointer(capsule, "arrow_array_stream");
    if (!stream) {
        PyErr_Clear();
        return;
    }
    if (stream->release) stream->release(stream);
    PyMem_Free(stream);
}

PyObject* ustore_py_stream_export(ustore_py_context_t const* context, ustore_collection_id_t collection) {
    struct ArrowArrayStream* const stream = PyMem_Calloc(1, sizeof(struct ArrowArrayStream));
    export_stream_t* const state = (export_stream_t*)PyMem_RawCalloc(1, sizeof(export_stream_t));
    if (!stream || !state) {
        PyMem_Free(stream);
        PyMem_RawFree(state);
        return PyErr_NoMemory();
    }
    state->database = context->database_handle;
    state->view = context->view;
    state->collection = collection;
    ustore_outcome_t outcome = {0};
    Py_BEGIN_ALLOW_THREADS;
    // A head cursor reads a removed key as an empty value, so a head export reads a snapshot of its own
    if (!state->view) {
        ustore_snapshot_init_t init = {.snapshot = &state->snapshot, .database = state->database, .outcome = &outcome};
        ustore_snapshot_init(&init);
        state->view = ustore_snapshot_view(state->snapshot);
    }
    if (outcome.status == ustore_status_success_k)
        state->modality = listed_modality(state->database, state->snapshot ? state->snapshot : context->snapshot,
                                          collection, &outcome);
    if (outcome.status == ustore_status_success_k) export_open(state, &outcome);
    Py_END_ALLOW_THREADS;
    if (ustore_py_raise_on_outcome(&outcome) != 0) {
        export_stream_free(state);
        PyMem_Free(stream);
        return NULL;
    }
    *stream = (struct ArrowArrayStream) {.get_schema = export_get_schema,
                                         .get_next = export_get_next,
                                         .get_last_error = export_get_last_error,
                                         .release = export_release,
                                         .private_data = state};
    PyObject* const capsule = PyCapsule_New(stream, "arrow_array_stream", stream_capsule_free);
    if (!capsule) {
        stream->release(stream);
        PyMem_Free(stream);
    }
    return capsule;
}

#pragma endregion Exports

#pragma region Imports

/** Matches the children of @p schema to the columns of @p modality by name, refusing a missing, repeated or unknown
 *  one into @p outcome. */
static void resolve_columns(struct ArrowSchema const* schema, ustore_modality_t modality, modality_columns_t* columns,
                            ustore_outcome_t* outcome) {
    *columns = (modality_columns_t) {absent_column_k, absent_column_k, absent_column_k, absent_column_k,
                                     absent_column_k};
    int const graph = modality == ustore_modality_graph_k;
    int const strings = modality == ustore_modality_strings_k;
    for (int64_t index = 0; index != schema->n_children; ++index) {
        char const* const name = schema->children[index]->name ? schema->children[index]->name : "";
        size_t* slot = NULL;
        if (graph) {
            if (strcmp(name, "source") == 0) slot = &columns->sources;
            else if (strcmp(name, "target") == 0) slot = &columns->targets;
            else if (strcmp(name, "edge") == 0) slot = &columns->edges;
        }
        else if (strcmp(name, "key") == 0) slot = strings ? &columns->strings : &columns->keys;
        else if (strcmp(name, "value") == 0) slot = strings ? &columns->keys : &columns->strings;
        if (!slot || *slot != absent_column_k) {
            *outcome = (ustore_outcome_t) {
                ustore_status_invalid_argument_k,
                "The stream carries a column its modality does not name, or names one twice"};
            return;
        }
        *slot = (size_t)index;
    }
    int const complete = graph ? columns->sources != absent_column_k && columns->targets != absent_column_k
                               : columns->keys != absent_column_k && columns->strings != absent_column_k;
    if (!complete)
        *outcome = (ustore_outcome_t) {ustore_status_invalid_argument_k,
                                       "The stream lacks a column its modality needs"};
}

/** The outcome a converter's @p status reports for a column. */
static ustore_outcome_t column_outcome(ustore_status_t status) {
    switch (status) {
    case ustore_status_unsupported_k:
        return (ustore_outcome_t) {status, "A column is of another type, a dictionary or a view, which no write reads"};
    case ustore_status_malformed_k:
        return (ustore_outcome_t) {status, "A column misses a buffer or is shorter than its batch"};
    default: return (ustore_outcome_t) {status, "A column holds a null"};
    }
}

/** Reads column @p column of @p batch as keys into @p keys, starting at the batch's offset. */
static ustore_status_t keys_column(struct ArrowSchema const* schema, struct ArrowArray const* batch, size_t column,
                                   ustore_keys_view_t* keys) {
    if (column == absent_column_k) return ustore_status_success_k;
    ustore_size_t count = 0;
    ustore_status_t const status = ustore_keys_view_from_arrow(schema->children[column], batch->children[column], keys,
                                                               &count);
    if (status != ustore_status_success_k) return status;
    if (count < (ustore_size_t)(batch->offset + batch->length)) return ustore_status_malformed_k;
    if (keys->data) keys->data += batch->offset;
    return ustore_status_success_k;
}

/** Reads the columns of @p batch into @p rows in place, refusing into @p outcome. */
static void rows_of(struct ArrowSchema const* schema, struct ArrowArray const* batch, modality_columns_t const* columns,
                    batch_views_t* rows, ustore_outcome_t* outcome) {
    *rows = (batch_views_t) {.count = (ustore_size_t)batch->length};
    ustore_status_t status = batch->n_children == schema->n_children && batch->null_count <= 0
                                 ? ustore_status_success_k
                                 : ustore_status_malformed_k;
    if (status == ustore_status_success_k) status = keys_column(schema, batch, columns->keys, &rows->keys);
    if (status == ustore_status_success_k) status = keys_column(schema, batch, columns->sources, &rows->sources);
    if (status == ustore_status_success_k) status = keys_column(schema, batch, columns->targets, &rows->targets);
    if (status == ustore_status_success_k) status = keys_column(schema, batch, columns->edges, &rows->edges);
    if (status == ustore_status_success_k && columns->strings != absent_column_k) {
        ustore_size_t count = 0;
        status = ustore_strings_view_from_arrow(schema->children[columns->strings], batch->children[columns->strings],
                                                &rows->strings, &count);
        if (status == ustore_status_success_k && count < (ustore_size_t)(batch->offset + batch->length))
            status = ustore_status_malformed_k;
        if (rows->strings.layout == ustore_strings_u32_tape_k) rows->strings.data.u32_tape.offsets += batch->offset;
        else rows->strings.data.u64_tape.offsets += batch->offset;
    }
    if (status != ustore_status_success_k) *outcome = column_outcome(status);
}

/** What an import and an ingest both need to run their loop, filled before the thread state detaches. */
typedef struct arrow_loop_t {
    ustore_database_t database;
    ustore_collection_id_t collection;
    ustore_durability_t durability;
    struct ArrowArrayStream* stream;
    ustore_outcome_t* outcome;
    ustore_size_t* written;
} arrow_loop_t;

/** The loop of an import, whose writes go through the receiver's transaction, recording keys as it watches. */
typedef struct import_loop_t {
    arrow_loop_t arrow;
    ustore_transaction_t transaction;
    ustore_watch_t watch;
} import_loop_t;

/** The loop of an ingest, which commits on its own in the order its rows promise. */
typedef struct ingest_loop_t {
    arrow_loop_t arrow;
    ustore_ingest_order_t order;
} ingest_loop_t;

/** One import or ingest, owning the stream it moved out of its source until its loop releases it. */
typedef struct arrow_call_t {

    /** What the call keeps until it finishes. */
    ustore_py_call_t call;

    /** The stream the loop reads, released once the loop ends. */
    struct ArrowArrayStream stream;

    /** The schema and batch @c stream answers for a source of one array. */
    single_batch_stream_t single;

    /** Output slot for the rows written. */
    ustore_size_t written;
} arrow_call_t;

/** Rows one write of an import holds at most, halved for a write the head refuses for its size. */
static ustore_size_t const import_chunk_rows_k = 65536;

/** Writes rows @b [first,first+count) of @p rows through the writes of @p modality. */
static void write_chunk(import_loop_t const* importing, ustore_modality_t modality, batch_views_t const* rows,
                        ustore_size_t first, ustore_size_t count, ustore_durability_t durability) {
    arrow_loop_t const* const loop = &importing->arrow;
    ustore_collections_view_t const collections = {&loop->collection, 0};
    ustore_keys_view_t const keys = {rows->keys.data ? rows->keys.data + first : NULL, rows->keys.stride};
    ustore_strings_view_t strings = rows->strings;
    if (strings.layout == ustore_strings_u32_tape_k && strings.data.u32_tape.offsets)
        strings.data.u32_tape.offsets += first;
    else if (strings.layout == ustore_strings_u64_tape_k && strings.data.u64_tape.offsets)
        strings.data.u64_tape.offsets += first;
    switch (modality) {
    case ustore_modality_docs_k: {
        ustore_docs_write_t write = {.database = loop->database,
                                     .outcome = loop->outcome,
                                     .transaction = importing->transaction,
                                     .count = count,
                                     .collections = collections,
                                     .keys = keys,
                                     .values = strings,
                                     .type = ustore_docs_field_type_json_k,
                                     .modification = ustore_docs_modify_upsert_k,
                                     .durability = durability,
                                     .watch = importing->watch};
        ustore_docs_write(&write);
    } break;
    case ustore_modality_pages_k: {
        ustore_pages_import_t import = {.database = loop->database,
                                        .outcome = loop->outcome,
                                        .transaction = importing->transaction,
                                        .count = count,
                                        .collections = collections,
                                        .keys = keys,
                                        .values = strings,
                                        .encoding = ustore_page_encoding_xml_k,
                                        .durability = durability,
                                        .watch = importing->watch};
        ustore_pages_import(&import);
    } break;
    case ustore_modality_strings_k: {
        ustore_strings_write_t write = {.database = loop->database,
                                        .outcome = loop->outcome,
                                        .transaction = importing->transaction,
                                        .count = count,
                                        .collections = collections,
                                        .strings = strings,
                                        .values = keys,
                                        .durability = durability,
                                        .watch = importing->watch};
        ustore_strings_write(&write);
    } break;
    case ustore_modality_graph_k: {
        ustore_graph_write_edges_t write = {
            .database = loop->database,
            .outcome = loop->outcome,
            .transaction = importing->transaction,
            .count = count,
            .collections = collections,
            .edges = {rows->edges.data ? rows->edges.data + first : NULL, rows->edges.stride},
            .sources = {rows->sources.data + first, rows->sources.stride},
            .targets = {rows->targets.data + first, rows->targets.stride},
            .durability = durability,
            .watch = importing->watch};
        ustore_graph_write_edges(&write);
    } break;
    default: {
        ustore_blobs_write_t write = {.database = loop->database,
                                      .outcome = loop->outcome,
                                      .transaction = importing->transaction,
                                      .count = count,
                                      .collections = collections,
                                      .keys = keys,
                                      .values = strings,
                                      .durability = durability,
                                      .watch = importing->watch};
        ustore_blobs_write(&write);
    } break;
    }
}

/** Writes every row of @p rows in chunks, halving one the head refused for its size, each buffered but the last. */
static void write_rows(import_loop_t const* importing, ustore_modality_t modality, batch_views_t const* rows,
                       ustore_durability_t durability) {
    arrow_loop_t const* const loop = &importing->arrow;
    ustore_size_t chunk = import_chunk_rows_k;
    for (ustore_size_t first = 0; first != rows->count;) {
        ustore_size_t const left = rows->count - first;
        ustore_size_t const count = chunk < left ? chunk : left;
        int const last = first + count == rows->count;
        write_chunk(importing, modality, rows, first, count, last ? durability : ustore_durability_buffered_k);
        if (loop->outcome->status == ustore_status_capacity_exhausted_k && !importing->transaction && count > 1) {
            *loop->outcome = (ustore_outcome_t) {0};
            chunk = count / 2;
            continue;
        }
        if (loop->outcome->status != ustore_status_success_k) return;
        first += count;
    }
}

/** Reads the schema of the loop's stream into @p schema and its columns into @p columns, for @p modality. */
static void read_schema(arrow_loop_t const* loop, ustore_modality_t modality, struct ArrowSchema* schema,
                        modality_columns_t* columns) {
    if (loop->stream->get_schema(loop->stream, schema) != 0)
        *loop->outcome = (ustore_outcome_t) {ustore_status_io_k, "The stream failed to answer its schema"};
    else resolve_columns(schema, modality, columns, loop->outcome);
}

/** Pulls the next batch of the loop's stream into @p batch, released once the stream ends. */
static void pull_batch(arrow_loop_t const* loop, struct ArrowArray* batch) {
    *batch = (struct ArrowArray) {0};
    if (loop->stream->get_next(loop->stream, batch) == 0) return;
    *batch = (struct ArrowArray) {0};
    *loop->outcome = (ustore_outcome_t) {ustore_status_io_k, "The stream failed to answer a batch"};
}

/** Writes every batch of the stream through the writes of the collection's modality, the last paying the
 *  durability, and releases the stream. */
static void import_submit(void* erased) {
    import_loop_t const* const importing = (import_loop_t const*)erased;
    arrow_loop_t const* const loop = &importing->arrow;
    ustore_outcome_t* const outcome = loop->outcome;
    ustore_modality_t const modality = listed_modality(loop->database, NULL, loop->collection, outcome);
    struct ArrowSchema schema = {0};
    modality_columns_t columns;
    if (outcome->status == ustore_status_success_k) read_schema(loop, modality, &schema, &columns);
    struct ArrowArray current = {0}, upcoming = {0};
    if (outcome->status == ustore_status_success_k) pull_batch(loop, &current);
    while (outcome->status == ustore_status_success_k && current.release) {
        pull_batch(loop, &upcoming);
        batch_views_t rows;
        if (outcome->status == ustore_status_success_k) rows_of(&schema, &current, &columns, &rows, outcome);
        if (outcome->status == ustore_status_success_k)
            write_rows(importing, modality, &rows, upcoming.release ? ustore_durability_buffered_k : loop->durability);
        if (outcome->status == ustore_status_success_k) *loop->written += rows.count;
        current.release(&current);
        current = upcoming, upcoming = (struct ArrowArray) {0};
    }
    if (current.release) current.release(&current);
    if (schema.release) schema.release(&schema);
    loop->stream->release(loop->stream);
    if (outcome->status != ustore_status_success_k) *loop->written = 0;
}

/** Appends every batch of the stream to one ingest, lands it as one commit, and releases the stream. */
static void ingest_submit(void* erased) {
    ingest_loop_t const* const ingesting = (ingest_loop_t const*)erased;
    arrow_loop_t const* const loop = &ingesting->arrow;
    ustore_outcome_t* const outcome = loop->outcome;
    ustore_modality_t const modality = listed_modality(loop->database, NULL, loop->collection, outcome);
    struct ArrowSchema schema = {0};
    modality_columns_t columns;
    if (outcome->status == ustore_status_success_k) read_schema(loop, modality, &schema, &columns);
    ustore_ingest_t ingest = NULL;
    if (outcome->status == ustore_status_success_k) {
        ustore_ingest_init_t init = {.ingest = &ingest,
                                     .database = loop->database,
                                     .outcome = outcome,
                                     .collection = loop->collection,
                                     .order = ingesting->order};
        ustore_ingest_init(&init);
    }
    while (outcome->status == ustore_status_success_k) {
        struct ArrowArray batch;
        pull_batch(loop, &batch);
        if (!batch.release) break;
        batch_views_t rows;
        rows_of(&schema, &batch, &columns, &rows, outcome);
        if (outcome->status == ustore_status_success_k) {
            ustore_ingest_append_t append = {.ingest = ingest,
                                             .outcome = outcome,
                                             .count = rows.count,
                                             .keys = rows.keys,
                                             .values = rows.strings,
                                             .sources = rows.sources,
                                             .targets = rows.targets,
                                             .edges = rows.edges};
            ustore_ingest_append(&append);
        }
        if (outcome->status == ustore_status_success_k) *loop->written += rows.count;
        batch.release(&batch);
    }
    if (outcome->status == ustore_status_success_k) {
        ustore_ingest_finish_t finish = {.ingest = ingest, .outcome = outcome, .durability = loop->durability};
        ustore_ingest_finish(&finish);
    }
    ustore_ingest_free(ingest);
    if (schema.release) schema.release(&schema);
    loop->stream->release(loop->stream);
    if (outcome->status != ustore_status_success_k) *loop->written = 0;
}

static PyObject* arrow_answer(void* erased) {
    return PyLong_FromUnsignedLongLong(((arrow_call_t const*)erased)->written);
}

// A call discarded before its loop ran still owns its stream.
static void arrow_release(void* erased) {
    arrow_call_t* const call = (arrow_call_t*)erased;
    if (call->stream.release) call->stream.release(&call->stream);
    ustore_py_call_release(&call->call);
}

static ustore_py_verb_t const import_verb = {sizeof(arrow_call_t), import_submit, arrow_answer, arrow_release};
static ustore_py_verb_t const ingest_verb = {sizeof(arrow_call_t), ingest_submit, arrow_answer, arrow_release};

/**
 *  @brief Reads the keywords of @p verb: @c collection, @c durability, where None takes the default, and @c order when
 *      @p order is not NULL.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
static int arrow_keywords(ustore_py_context_t const* context, char const* verb, PyObject* const* arguments,
                          Py_ssize_t positional_arguments_count, PyObject* keyword_names_tuple,
                          ustore_py_verb_options_t* options, ustore_ingest_order_t* order) {
    Py_ssize_t const keyword_names_count = ustore_py_keyword_names_count_checked(verb, positional_arguments_count,
                                                                                 keyword_names_tuple, 1);
    if (keyword_names_count < 0) return -1;
    ustore_py_verb_options_init(options);
    for (Py_ssize_t names_progress = 0; names_progress != keyword_names_count; ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(keyword_names_tuple, names_progress);
        PyObject* const value = arguments[positional_arguments_count + names_progress];
        if (order && PyUnicode_CompareWithASCIIString(name, "order") == 0) {
            Py_ssize_t length = 0;
            char const* const spelled = PyUnicode_Check(value) ? PyUnicode_AsUTF8AndSize(value, &length) : NULL;
            if (!spelled && PyErr_Occurred()) return -1;
            ustore_bytes_view_t const bytes = {(uint8_t const*)spelled, (ustore_size_t)length};
            *order = spelled ? ustore_ingest_order_from_name(bytes) : ustore_ingest_sorted_k;
            if (!spelled || strcmp(ustore_ingest_order_name(*order), spelled) != 0) {
                PyErr_Format(PyExc_ValueError, "The 'order' option is \"sorted\" or \"unsorted\", not %R", value);
                return -1;
            }
            continue;
        }
        if (value == Py_None && PyUnicode_CompareWithASCIIString(name, "durability") == 0) continue;
        int const consumed = ustore_py_verb_option_from_keyword(context, name, value, options);
        if (consumed < 0) return -1;
        if (consumed == 0) {
            PyErr_Format(PyExc_TypeError, "%s() got an unexpected keyword argument %R", verb, name);
            return -1;
        }
    }
    ustore_collection_id_t const nil = {{{0}}};
    if (memcmp(&options->collection, &nil, sizeof(nil)) != 0) return 0;
    PyErr_Format(PyExc_TypeError, "%s() needs the collection to write", verb);
    return -1;
}

char const doc_import_arrow[] =                                                                //
    "import_arrow(source, *, collection, durability=None) answers the rows written.\n"         //
    "\n"                                                                                       //
    "Writes every batch of an Arrow stream through the writes of the collection's modality,\n" //
    "reading its columns in place. Blobs and docs take `key` and `value` columns, strings a\n" //
    "binary `key` and a uuid `value`, and graphs `source`, `target` and an optional `edge`.\n" //
    "At the head every write commits on its own, so a failure keeps the writes before it.\n"   //
    "\n"                                                                                       //
    "Args:\n"                                                                                  //
    "  source: An object exporting `__arrow_c_stream__`, such as a `pyarrow.Table`, whose\n"   //
    "    stream the call takes over, or one exporting `__arrow_c_array__` alone.\n"            //
    "  collection (UUID | str): The collection, by identifier or by name.\n"                   //
    "  durability (Durability, optional): `Durability.FLUSHED` reaches the drive before\n"     //
    "    returning, paid by the last write alone.\n"                                           //
    "Raises:\n"                                                                                //
    "  InvalidArgumentError: A column is missing, extra, mistyped, or holds a null.\n"         //
    "  UnsupportedError: A column is a binary or string view.";

char const doc_ingest_arrow[] =                                                                 //
    "ingest_arrow(source, *, collection, order=\"sorted\", durability=None) answers the rows\n" //
    "landed.\n"                                                                                 //
    "\n"                                                                                        //
    "Lands every batch of an Arrow stream as one commit of its own at the head, taking the\n"   //
    "schemas `import_arrow` takes, so a reader sees all of it or none of it.\n"                 //
    "\n"                                                                                        //
    "Args:\n"                                                                                   //
    "  source: An object exporting `__arrow_c_stream__` or `__arrow_c_array__`.\n"              //
    "  collection (UUID | str): The collection, by identifier or by name.\n"                    //
    "  order (str, optional): \"sorted\" when keys strictly ascend across the stream, or\n"     //
    "    \"unsorted\", where the last row of a repeated key wins.\n"                            //
    "  durability (Durability, optional): `Durability.FLUSHED` reaches the drive before\n"      //
    "    returning.\n"                                                                          //
    "Raises:\n"                                                                                 //
    "  TypeError: The receiver is a transaction, which no ingest joins.\n"                      //
    "  UnsupportedError: The engine ingests no stream.";

PyObject* ustore_py_api_import_arrow(PyObject* self, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                                     PyObject* keyword_names_tuple) {
    ustore_py_context_t context;
    ustore_py_verb_options_t options;
    if (ustore_py_context_from_object(self, &context) != 0 || ustore_py_context_writable(&context) != 0 ||
        arrow_keywords(&context, "import_arrow", arguments, positional_arguments_count, keyword_names_tuple, &options,
                       NULL) != 0)
        return NULL;
    arrow_call_t stack;
    arrow_call_t* const call = ustore_py_call_open(&context, &import_verb, &stack);
    if (!call) return NULL;
    call->call.collection = options.collection;
    if (stream_from_object(arguments[0], &call->stream, &call->single) != 0)
        return ustore_py_call_discard(&context, &import_verb, call);
    import_loop_t loop = {
        .arrow = {.database = context.database_handle,
                  .collection = call->call.collection,
                  .durability = options.durability,
                  .stream = &call->stream,
                  .outcome = &call->call.outcome,
                  .written = &call->written},
        .transaction = context.transaction,
        .watch = context.watch,
    };
    return ustore_py_call_run(&context, &import_verb, call, &loop);
}

PyObject* ustore_py_api_ingest_arrow(PyObject* self, PyObject* const* arguments, Py_ssize_t positional_arguments_count,
                                     PyObject* keyword_names_tuple) {
    ustore_py_context_t context;
    ustore_py_verb_options_t options;
    ustore_ingest_order_t order = ustore_ingest_sorted_k;
    if (ustore_py_context_from_object(self, &context) != 0 || ustore_py_context_writable(&context) != 0 ||
        arrow_keywords(&context, "ingest_arrow", arguments, positional_arguments_count, keyword_names_tuple, &options,
                       &order) != 0)
        return NULL;
    if (context.transaction) {
        PyErr_SetString(PyExc_TypeError, "An ingest commits on its own and joins no transaction; call it on the head");
        return NULL;
    }
    arrow_call_t stack;
    arrow_call_t* const call = ustore_py_call_open(&context, &ingest_verb, &stack);
    if (!call) return NULL;
    call->call.collection = options.collection;
    if (stream_from_object(arguments[0], &call->stream, &call->single) != 0)
        return ustore_py_call_discard(&context, &ingest_verb, call);
    ingest_loop_t loop = {
        .arrow = {.database = context.database_handle,
                  .collection = call->call.collection,
                  .durability = options.durability,
                  .stream = &call->stream,
                  .outcome = &call->call.outcome,
                  .written = &call->written},
        .order = order,
    };
    return ustore_py_call_run(&context, &ingest_verb, call, &loop);
}

#pragma endregion Imports
