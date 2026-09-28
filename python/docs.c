/**
 *  @file python/docs.c
 *  @author Ash Vardanian
 *  @date September 17, 2026
 *  @brief JSON documents: writes, reads, gists and typed gathers.
 *
 *  A document read answers the same tape a binary read does, one run of bytes per task, while a
 *  gather answers typed columns whose scalars export in place and whose strings decode on access.
 *  Every verb claims the database's arena, drops the interpreter lock around the call, and either
 *  returns the arena or hands it to the result it answers with.
 */
#include "lib.h"

#pragma region Field Types

/** Every type a written or read value is spelled as, as a caller spells it. */
static ustore_py_choice_naming_t const value_type_namings[] = {
    {"json", ustore_doc_field_json_k}, {"str", ustore_doc_field_str_k},
    {"bytes", ustore_doc_field_bin_k}, {"null", ustore_doc_field_null_k},
    {"bool", ustore_doc_field_bool_k}, {"f64", ustore_doc_field_f64_k},
    {"f32", ustore_doc_field_f32_k},   {"i64", ustore_doc_field_i64_k},
    {"i32", ustore_doc_field_i32_k},   {"i16", ustore_doc_field_i16_k},
    {"i8", ustore_doc_field_i8_k},     {"u64", ustore_doc_field_u64_k},
    {"u32", ustore_doc_field_u32_k},   {"u16", ustore_doc_field_u16_k},
    {"u8", ustore_doc_field_u8_k},     {NULL, 0},
};

/** Every type a gather converts a field into, as a caller spells it. */
static ustore_py_choice_naming_t const column_type_namings[] = {
    {"bool", ustore_doc_field_bool_k}, {"uuid", ustore_doc_field_uuid_k},
    {"f64", ustore_doc_field_f64_k},   {"f32", ustore_doc_field_f32_k},
    {"f16", ustore_doc_field_f16_k},   {"i64", ustore_doc_field_i64_k},
    {"i32", ustore_doc_field_i32_k},   {"i16", ustore_doc_field_i16_k},
    {"i8", ustore_doc_field_i8_k},     {"u64", ustore_doc_field_u64_k},
    {"u32", ustore_doc_field_u32_k},   {"u16", ustore_doc_field_u16_k},
    {"u8", ustore_doc_field_u8_k},     {"str", ustore_doc_field_str_k},
    {"bytes", ustore_doc_field_bin_k}, {NULL, 0},
};

/** Bytes one UUID takes, as the ABI lays it out. */
enum { uuid_bytes_k = 16 };

/** Every modification a write applies, as a caller spells it. */
static ustore_py_choice_naming_t const modification_namings[] = {
    {"upsert", ustore_doc_modify_upsert_k}, {"update", ustore_doc_modify_update_k},
    {"insert", ustore_doc_modify_insert_k}, {"patch", ustore_doc_modify_patch_k},
    {"merge", ustore_doc_modify_merge_k},   {NULL, 0},
};

/**
 *  @brief Answers the struct-module format one scalar of a field type exports as.
 *
 *  A UUID is sixteen bytes rather than a scalar @c struct spells, so it carries no format and
 *  travels as a two-dimensional run of bytes instead.
 *
 *  @param[in] type The field type.
 *  @return The format, or NULL for UUIDs, strings and every type a gather does not fill.
 */
static char const* field_type_format(ustore_doc_field_type_t type) {
    switch (type) {
    case ustore_doc_field_bool_k: return "?";
    case ustore_doc_field_f64_k: return "d";
    case ustore_doc_field_f32_k: return "f";
    case ustore_doc_field_f16_k: return "e";
    case ustore_doc_field_i64_k: return "q";
    case ustore_doc_field_i32_k: return "i";
    case ustore_doc_field_i16_k: return "h";
    case ustore_doc_field_i8_k: return "b";
    case ustore_doc_field_u64_k: return "Q";
    case ustore_doc_field_u32_k: return "I";
    case ustore_doc_field_u16_k: return "H";
    case ustore_doc_field_u8_k: return "B";
    default: return NULL;
    }
}

/**
 *  @brief Answers the bytes one value of a fixed-width field type takes.
 *  @param[in] type The field type.
 *  @return The width, or zero for JSON, strings and every type without a fixed one.
 */
static size_t field_type_width(ustore_doc_field_type_t type) {
    switch (type) {
    case ustore_doc_field_bool_k: return 1;
    case ustore_doc_field_uuid_k: return uuid_bytes_k;
    case ustore_doc_field_f64_k: return 8;
    case ustore_doc_field_f32_k: return 4;
    case ustore_doc_field_f16_k: return 2;
    case ustore_doc_field_i64_k: return 8;
    case ustore_doc_field_i32_k: return 4;
    case ustore_doc_field_i16_k: return 2;
    case ustore_doc_field_i8_k: return 1;
    case ustore_doc_field_u64_k: return 8;
    case ustore_doc_field_u32_k: return 4;
    case ustore_doc_field_u16_k: return 2;
    case ustore_doc_field_u8_k: return 1;
    default: return 0;
    }
}

/** The spelling of @p type, which every value of the two closed sets has. */
static char const* field_type_name(ustore_doc_field_type_t type) {
    for (ustore_py_choice_naming_t const* naming = value_type_namings; naming->name; ++naming)
        if (naming->value == (int)type) return naming->name;
    for (ustore_py_choice_naming_t const* naming = column_type_namings; naming->name; ++naming)
        if (naming->value == (int)type) return naming->name;
    return "unknown";
}

/**
 *  @brief Reads an optional string keyword as null-terminated UTF-8.
 *  @param[in] object The keyword's value, a string or @c None.
 *  @param[in] field The keyword to name in a failure.
 *  @param[out] text The UTF-8 bytes the string caches, or NULL for @c None.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
static int optional_text_from_object(PyObject* object, char const* field, char const** text) {
    *text = NULL;
    if (object == Py_None) return 0;
    if (!PyUnicode_Check(object)) {
        PyErr_Format(PyExc_TypeError, "The '%s' option must be a string or None, got %S", field, Py_TYPE(object));
        return -1;
    }
    *text = PyUnicode_AsUTF8(object);
    return *text ? 0 : -1;
}

#pragma endregion Field Types

#pragma region Writes

/**
 *  @brief The documents one write batch hands the ABI.
 *
 *  A string travels as the UTF-8 bytes it caches, so the batch holds a tuple of every document for
 *  the length of the call, which no concurrent change to the caller's list can release.
 */
typedef struct documents_t {

    /** The borrowed runs the ABI reads, empty when every key is removed. */
    ustore_py_contents_t contents;

    /** A tuple of the documents, keeping every string alive, or NULL. */
    PyObject* held;
} documents_t;

/** Borrows one document, reading a string as its UTF-8 bytes and anything else through its buffer. */
static int document_borrow(PyObject* object, ustore_py_contents_t* batch) {
    if (!PyUnicode_Check(object)) return ustore_py_contents_borrow(object, batch);
    Py_ssize_t length = 0;
    char const* const text = PyUnicode_AsUTF8AndSize(object, &length);
    if (!text) return -1;
    batch->values[batch->count] = (ustore_bytes_cptr_t)text;
    batch->lengths[batch->count] = (ustore_length_t)length;
    ++batch->count;
    return 0;
}

/** Releases whatever @c documents_from_object acquired. */
static void documents_release(documents_t* documents) {
    ustore_py_contents_release(&documents->contents);
    Py_CLEAR(documents->held);
}

/**
 *  @brief Lays a contiguous buffer of fixed-width scalars out as one borrowed value per task.
 *
 *  A buffer holding one scalar stands for every task, and a buffer holding one per task is split in
 *  place, so a column of floats or integers reaches the ABI without ever being printed as JSON.
 *
 *  @param[in] object The column, anything exporting a C-contiguous buffer.
 *  @param[in] type The field type every scalar carries.
 *  @param[in] tasks_count Tasks the write addresses.
 *  @param[out] documents The borrowed runs and the layout they travel with.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
static int scalars_from_object(PyObject* object, ustore_doc_field_type_t type, size_t tasks_count,
                               documents_t* documents) {
    memset(documents, 0, sizeof(*documents));
    size_t const width = field_type_width(type);
    Py_buffer buffer;
    if (PyObject_GetBuffer(object, &buffer, PyBUF_C_CONTIGUOUS | PyBUF_FORMAT) != 0)
        return ustore_py_raise_from_cause(PyExc_TypeError, "A column of '%s' must export a contiguous buffer, not %S",
                                          field_type_name(type), Py_TYPE(object));

    // A buffer of bytes carries no width of its own, so only a typed one is held to the field's.
    if (buffer.itemsize != 1 && (size_t)buffer.itemsize != width) {
        PyErr_Format(PyExc_ValueError, "A column of '%s' holds %zu-byte items, got %zd-byte ones",
                     field_type_name(type), width, buffer.itemsize);
        PyBuffer_Release(&buffer);
        return -1;
    }

    size_t const values_count = (size_t)buffer.len == width ? 1 : tasks_count;
    if ((size_t)buffer.len != values_count * width) {
        PyErr_Format(PyExc_ValueError, "A column of '%s' spans %zd bytes, not the %zu that %zu tasks take",
                     field_type_name(type), buffer.len, tasks_count * width, tasks_count);
        PyBuffer_Release(&buffer);
        return -1;
    }

    if (ustore_py_contents_reserve(&documents->contents, values_count) != 0) {
        PyBuffer_Release(&buffer);
        return -1;
    }
    // One buffer backs every task, so the batch borrows once and releases once.
    documents->contents.buffers[0] = buffer;
    documents->contents.borrowed = 1;
    documents->contents.count = values_count;
    documents->contents.layout = values_count == 1 ? ustore_py_contents_broadcast_k : ustore_py_contents_per_task_k;
    for (size_t task_index = 0; task_index != values_count; ++task_index) {
        documents->contents.values[task_index] = (ustore_bytes_cptr_t)buffer.buf + task_index * width;
        documents->contents.lengths[task_index] = (ustore_length_t)width;
    }
    return 0;
}

/**
 *  @brief Gathers the documents of a write: a sequence, one document for every key, or None.
 *  @param[in] object The documents, each a string, a bytes-like object, or None to remove it.
 *  @param[out] documents The borrowed runs and the layout they travel with.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
static int documents_from_object(PyObject* object, documents_t* documents) {
    memset(documents, 0, sizeof(*documents));
    if (object == Py_None) return 0;

    // A string or a bytes-like object is one document for every key, not a sequence of characters.
    if (PyUnicode_Check(object) || PyObject_CheckBuffer(object)) {
        if (ustore_py_contents_reserve(&documents->contents, 1) != 0) return -1;
        if (document_borrow(object, &documents->contents) != 0) {
            documents_release(documents);
            return -1;
        }
        documents->contents.layout = ustore_py_contents_broadcast_k;
        return 0;
    }

    documents->held = PySequence_Tuple(object);
    if (!documents->held)
        return ustore_py_raise_from_cause(
            PyExc_TypeError, "Expected a sequence of documents, one document, or None, not %S", Py_TYPE(object));
    size_t const count = (size_t)PyTuple_GET_SIZE(documents->held);
    if (ustore_py_contents_reserve(&documents->contents, count) != 0) {
        Py_CLEAR(documents->held);
        return -1;
    }
    for (size_t task_index = 0; task_index != count; ++task_index)
        if (document_borrow(PyTuple_GET_ITEM(documents->held, (Py_ssize_t)task_index), &documents->contents) != 0) {
            documents_release(documents);
            return -1;
        }
    return 0;
}

char const doc_docs_write[] =                                                                    //
    "docs_write(keys, documents, *, collection=None, field=None, modification='upsert',\n"       //
    "           type='json', id_field=None, flush=False) answers None.\n"                        //
    "\n"                                                                                         //
    "Writes a batch of JSON documents, or one field inside each of them. Every document is\n"    //
    "borrowed for the length of the call rather than copied.\n"                                  //
    "\n"                                                                                         //
    "Args:\n"                                                                                    //
    "  keys (Sequence[KeyLike] | buffer | None): The keys to write; None reads every key from\n" //
    "    `id_field` of its document.\n"                                                          //
    "  documents (Sequence[str | bytes-like | None] | str | bytes-like | None): One document\n"  //
    "    per key, one document broadcast to every key, or None to remove them all. A None\n"     //
    "    inside a sequence removes that one document, or the field inside it. Under a\n"         //
    "    fixed-width `type`, a contiguous buffer of one scalar per key is a column, which\n"     //
    "    reaches the store without being printed as JSON and parsed back.\n"                     //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n"           //
    "  field (str, optional): A top-level key or a JSON Pointer such as '/user/name' to\n"       //
    "    write inside every document; None writes whole documents.\n"                            //
    "  modification (str, optional): 'upsert', 'update', 'insert', an RFC 6902 'patch' or\n"     //
    "    an RFC 7386 'merge'.\n"                                                                 //
    "  type (str, optional): How every document is spelled: 'json', 'str' for a plain\n"         //
    "    string, 'null', or the raw bytes of 'bool', 'f64', 'f32', 'i64' through 'u8'.\n"        //
    "  id_field (str, optional): The field holding every key when `keys` is None.\n"             //
    "  flush (bool, optional): Whether to reach the drive before returning.\n"                   //
    "Raises:\n"                                                                                  //
    "  MalformedError: A document does not parse, or its `id_field` holds no integer.\n"         //
    "  MissingError: A field is written inside a document that does not exist.\n"                //
    "  ExistsError: An insert names a document that already exists.";

PyObject* ustore_py_api_docs_write(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                   PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    Py_ssize_t const args_names_count = ustore_py_args_names_count_checked("docs_write", positional_args_count,
                                                                           args_names_tuple, 2);
    if (args_names_count < 0) return NULL;

    ustore_py_verb_options_t options;
    ustore_py_verb_options_init(&context, &options);
    int modification = ustore_doc_modify_upsert_k;
    int type = ustore_doc_field_json_k;
    char const* field = NULL;
    char const* id_field = NULL;
    for (Py_ssize_t names_progress = 0, args_progress = positional_args_count; names_progress != args_names_count;
         ++args_progress, ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(args_names_tuple, names_progress);
        PyObject* const value = args[args_progress];
        int const consumed = ustore_py_verb_option_from_keyword(&context, name, value, &options);
        if (consumed < 0) return NULL;
        if (consumed > 0) continue;
        int parsed = 0;
        if (PyUnicode_CompareWithASCIIString(name, "field") == 0)
            parsed = optional_text_from_object(value, "field", &field);
        else if (PyUnicode_CompareWithASCIIString(name, "id_field") == 0)
            parsed = optional_text_from_object(value, "id_field", &id_field);
        else if (PyUnicode_CompareWithASCIIString(name, "modification") == 0)
            parsed = ustore_py_choice_from_object(value, "modification", modification_namings, &modification);
        else if (PyUnicode_CompareWithASCIIString(name, "type") == 0)
            parsed = ustore_py_choice_from_object(value, "type", value_type_namings, &type);
        else {
            PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
            return NULL;
        }
        if (parsed != 0) return NULL;
    }

    // A fixed-width type given a buffer is a column of scalars, which splits in place per key.
    ustore_py_keys_t keys;
    memset(&keys, 0, sizeof(keys));
    int const columnar = field_type_width((ustore_doc_field_type_t)type) != 0 && args[0] != Py_None &&
                         !PyUnicode_Check(args[1]) && PyObject_CheckBuffer(args[1]);
    if (args[0] != Py_None && ustore_py_keys_from_object(args[0], &keys) != 0) return NULL;

    documents_t documents;
    int const gathered = columnar ? scalars_from_object(args[1], (ustore_doc_field_type_t)type, keys.count, &documents)
                                  : documents_from_object(args[1], &documents);
    if (gathered != 0) {
        ustore_py_keys_release(&keys);
        return NULL;
    }

    size_t tasks_count = 0;
    if (args[0] == Py_None) {
        // Every key is read out of its own document, so each task needs a document of its own.
        if (!id_field || !documents.held) {
            documents_release(&documents);
            PyErr_SetString(PyExc_TypeError, "Documents without keys need a sequence of them and an 'id_field'");
            return NULL;
        }
        tasks_count = documents.contents.count;
    }
    else {
        tasks_count = keys.count;
        if (documents.held && documents.contents.count != keys.count) {
            PyErr_Format(PyExc_ValueError, "Got %zu documents for %zu keys", documents.contents.count, keys.count);
            documents_release(&documents);
            ustore_py_keys_release(&keys);
            return NULL;
        }
    }

    ustore_error_t error = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context.database);

    ustore_docs_write_t write = {0};
    write.db = context.database->handle;
    write.error = &error;
    write.transaction = context.transaction;
    write.arena = &arena;
    write.options = options.options;
    write.tasks_count = tasks_count;
    write.type = (ustore_doc_field_type_t)type;
    write.modification = (ustore_doc_modification_t)modification;
    write.collections = &options.collection;
    write.keys = keys.keys;
    write.keys_stride = sizeof(ustore_key_t);
    write.fields = field ? &field : NULL;
    write.values = documents.contents.count ? documents.contents.values : NULL;
    write.values_stride = documents.contents.layout == ustore_py_contents_broadcast_k ? 0 : sizeof(ustore_bytes_cptr_t);
    write.lengths = documents.contents.count ? documents.contents.lengths : NULL;
    write.lengths_stride = documents.contents.layout == ustore_py_contents_broadcast_k ? 0 : sizeof(ustore_length_t);
    write.id_field = id_field;

    Py_BEGIN_ALLOW_THREADS;
    ustore_docs_write(&write);
    Py_END_ALLOW_THREADS;

    documents_release(&documents);
    ustore_py_keys_release(&keys);
    ustore_py_arena_return(context.database, arena);
    if (ustore_py_raise_on_error(&error) != 0) return NULL;
    Py_RETURN_NONE;
}

#pragma endregion Writes

#pragma region Reads

char const doc_docs_read[] =                                                                   //
    "docs_read(keys, *, collection=None, snapshot=0, field=None, type='json') answers a\n"     //
    "Values tape.\n"                                                                           //
    "\n"                                                                                       //
    "Reads whole documents, or one field of each, as JSON text or as printed strings. Whole\n" //
    "documents read as JSON pass straight through from the engine, so nothing is parsed.\n"    //
    "\n"                                                                                       //
    "Args:\n"                                                                                  //
    "  keys (Sequence[KeyLike] | buffer): The keys to read.\n"                                 //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n"         //
    "  snapshot (Snapshot, optional): The snapshot to read; None reads the head.\n"            //
    "  field (str, optional): A top-level key or a JSON Pointer to read inside every\n"        //
    "    document; None reads whole documents.\n"                                              //
    "  type (str, optional): 'json' for JSON text, or 'str' to print scalars as text.\n"       //
    "Returns:\n"                                                                               //
    "  Values: One entry per key, None where the document or the field is absent.";

PyObject* ustore_py_api_docs_read(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                  PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    Py_ssize_t const args_names_count = ustore_py_args_names_count_checked("docs_read", positional_args_count,
                                                                           args_names_tuple, 1);
    if (args_names_count < 0) return NULL;

    ustore_py_verb_options_t options;
    ustore_py_verb_options_init(&context, &options);
    int type = ustore_doc_field_json_k;
    char const* field = NULL;
    for (Py_ssize_t names_progress = 0, args_progress = positional_args_count; names_progress != args_names_count;
         ++args_progress, ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(args_names_tuple, names_progress);
        PyObject* const value = args[args_progress];
        int const consumed = ustore_py_verb_option_from_keyword(&context, name, value, &options);
        if (consumed < 0) return NULL;
        if (consumed > 0) continue;
        int parsed = 0;
        if (PyUnicode_CompareWithASCIIString(name, "field") == 0)
            parsed = optional_text_from_object(value, "field", &field);
        else if (PyUnicode_CompareWithASCIIString(name, "type") == 0)
            parsed = ustore_py_choice_from_object(value, "type", value_type_namings, &type);
        else {
            PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
            return NULL;
        }
        if (parsed != 0) return NULL;
    }

    ustore_py_keys_t keys;
    if (ustore_py_keys_from_object(args[0], &keys) != 0) return NULL;

    ustore_length_t* offsets = NULL;
    ustore_length_t* lengths = NULL;
    ustore_byte_t* values = NULL;
    ustore_error_t error = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context.database);

    ustore_docs_read_t read = {0};
    read.db = context.database->handle;
    read.error = &error;
    read.transaction = context.transaction;
    read.snapshot = options.snapshot;
    read.arena = &arena;
    read.options = options.options;
    read.type = (ustore_doc_field_type_t)type;
    read.tasks_count = keys.count;
    read.collections = &options.collection;
    read.keys = keys.keys;
    read.keys_stride = sizeof(ustore_key_t);
    read.fields = field ? &field : NULL;
    read.offsets = &offsets;
    read.lengths = &lengths;
    read.values = &values;

    Py_BEGIN_ALLOW_THREADS;
    ustore_docs_read(&read);
    Py_END_ALLOW_THREADS;

    size_t const tasks_count = keys.count;
    ustore_py_keys_release(&keys);
    if (error.status != ustore_success_k) {
        ustore_py_arena_return(context.database, arena);
        ustore_py_raise_on_error(&error);
        return NULL;
    }
    return ustore_py_values_adopt(arena, values, offsets, lengths, tasks_count);
}

char const doc_docs_gist[] =                                                                  //
    "docs_gist(keys, *, collection=None, snapshot=None) answers a tuple of JSON Pointers.\n"  //
    "\n"                                                                                      //
    "Lists every distinct path to a leaf across the requested documents, in the order they\n" //
    "were first met, which is how a table learns the columns its documents hold.\n"           //
    "\n"                                                                                      //
    "Args:\n"                                                                                 //
    "  keys (Sequence[KeyLike] | buffer): The documents to inspect.\n"                        //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n"        //
    "  snapshot (Snapshot, optional): The snapshot to read; None reads the head.\n"           //
    "Returns:\n"                                                                              //
    "  tuple[str, ...]: One JSON Pointer per leaf, such as '/user/name'.";

PyObject* ustore_py_api_docs_gist(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                  PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_args_names_count_checked("docs_gist", positional_args_count, args_names_tuple, 1) < 0) return NULL;

    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, args, positional_args_count, args_names_tuple, &options) != 0)
        return NULL;

    ustore_py_keys_t keys;
    if (ustore_py_keys_from_object(args[0], &keys) != 0) return NULL;

    ustore_size_t fields_count = 0;
    ustore_length_t* offsets = NULL;
    ustore_char_t* fields = NULL;
    ustore_error_t error = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context.database);

    ustore_docs_gist_t gist = {0};
    gist.db = context.database->handle;
    gist.error = &error;
    gist.transaction = context.transaction;
    gist.snapshot = options.snapshot;
    gist.arena = &arena;
    gist.options = options.options;
    gist.docs_count = keys.count;
    gist.collections = &options.collection;
    gist.keys = keys.keys;
    gist.keys_stride = sizeof(ustore_key_t);
    gist.fields_count = &fields_count;
    gist.offsets = &offsets;
    gist.fields = &fields;

    Py_BEGIN_ALLOW_THREADS;
    ustore_docs_gist(&gist);
    Py_END_ALLOW_THREADS;

    ustore_py_keys_release(&keys);
    PyObject* listed = NULL;
    if (error.status != ustore_success_k) { ustore_py_raise_on_error(&error); }
    // Every path is a `str`, which owns its bytes, so the arena goes back to the database at once.
    else if ((listed = PyTuple_New((Py_ssize_t)fields_count)) != NULL) {
        for (ustore_size_t field_index = 0; field_index != fields_count; ++field_index) {
            PyObject* const path = PyUnicode_FromString(fields + offsets[field_index]);
            if (!path) {
                Py_CLEAR(listed);
                break;
            }
            PyTuple_SET_ITEM(listed, (Py_ssize_t)field_index, path);
        }
    }
    ustore_py_arena_return(context.database, arena);
    return listed;
}

#pragma endregion Reads

#pragma region Gathers

/**
 *  @brief Wraps the columns of one gather and the arena holding them into a @c Columns.
 *  @param[inout] arena The arena the result adopts; it is freed on failure.
 *  @param[inout] types The heap block of field types the result adopts; it is freed on failure.
 *  @param[in] gather The finished gather, whose outputs point at the columns.
 *  @return A new reference, or NULL with an exception set.
 */
static PyObject* columns_adopt(ustore_arena_t arena, ustore_doc_field_type_t* types,
                               ustore_docs_gather_t const* gather) {
    Columns* const instance = PyObject_New(Columns, &ColumnsType);
    if (!instance) {
        ustore_arena_free(arena);
        PyMem_Free(types);
        return NULL;
    }
    instance->arena = arena;
    instance->validities = *gather->columns_validities;
    instance->conversions = *gather->columns_conversions;
    instance->collisions = *gather->columns_collisions;
    instance->scalars = *gather->columns_scalars;
    instance->offsets = *gather->columns_offsets;
    instance->lengths = *gather->columns_lengths;
    instance->joined = *gather->joined_strings;
    instance->types = types;
    instance->fields_count = gather->fields_count;
    instance->docs_count = gather->docs_count;
    return (PyObject*)instance;
}

char const doc_docs_gather[] =                                                                //
    "docs_gather(keys, fields, types, *, collection=None, snapshot=None) answers Columns.\n"  //
    "\n"                                                                                      //
    "Reads every field of every document into typed columns, converting each value on a\n"    //
    "best-effort basis and recording what was found, converted, or could not be.\n"           //
    "\n"                                                                                      //
    "Args:\n"                                                                                 //
    "  keys (Sequence[KeyLike] | buffer): The documents, the rows of every column.\n"         //
    "  fields (Sequence[str]): Top-level keys or JSON Pointers, the columns of the result.\n" //
    "  types (str | Sequence[str]): One type per field, or one for every field: 'bool',\n"    //
    "    'uuid', 'f64', 'f32', 'f16', 'i64', 'i32', 'i16', 'i8', 'u64', 'u32', 'u16',\n"      //
    "    'u8', 'str' or 'bytes'. An 'f16' column exports as a half-precision view NumPy\n"    //
    "    reads as float16, and a 'uuid' column as sixteen bytes per document.\n"              //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n"        //
    "  snapshot (Snapshot, optional): The snapshot to read; None reads the head.\n"           //
    "Returns:\n"                                                                              //
    "  Columns: One column per field, each holding one entry per document.";

PyObject* ustore_py_api_docs_gather(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                    PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_args_names_count_checked("docs_gather", positional_args_count, args_names_tuple, 3) < 0) return NULL;

    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, args, positional_args_count, args_names_tuple, &options) != 0)
        return NULL;

    if (PyUnicode_Check(args[1])) {
        PyErr_SetString(PyExc_TypeError, "The fields are a sequence of strings, not one string");
        return NULL;
    }
    // The tuple keeps every field name alive while the call reads the UTF-8 bytes they cache.
    PyObject* const fields_held = PySequence_Tuple(args[1]);
    if (!fields_held) return NULL;
    size_t const fields_count = (size_t)PyTuple_GET_SIZE(fields_held);

    PyObject* const types_held = PyUnicode_Check(args[2]) ? NULL : PySequence_Tuple(args[2]);
    if (!PyUnicode_Check(args[2]) && !types_held) {
        Py_DECREF(fields_held);
        return NULL;
    }
    if (types_held && (size_t)PyTuple_GET_SIZE(types_held) != fields_count) {
        PyErr_Format(PyExc_ValueError, "Got %zd types for %zu fields", PyTuple_GET_SIZE(types_held), fields_count);
        Py_DECREF(types_held);
        Py_DECREF(fields_held);
        return NULL;
    }

    // The types outlive the call inside the result, so they take a block of their own.
    ustore_str_view_t* const field_texts = (ustore_str_view_t*)PyMem_Malloc((fields_count + 1) *
                                                                            sizeof(ustore_str_view_t));
    ustore_doc_field_type_t* const types = (ustore_doc_field_type_t*)PyMem_Malloc((fields_count + 1) *
                                                                                  sizeof(ustore_doc_field_type_t));
    int failed = !field_texts || !types;
    if (failed) PyErr_NoMemory();

    for (size_t field_index = 0; !failed && field_index != fields_count; ++field_index) {
        PyObject* const field = PyTuple_GET_ITEM(fields_held, (Py_ssize_t)field_index);
        PyObject* const type_object = types_held ? PyTuple_GET_ITEM(types_held, (Py_ssize_t)field_index) : args[2];
        if (!PyUnicode_Check(field)) {
            PyErr_Format(PyExc_TypeError, "A field is a string, got %S", Py_TYPE(field));
            failed = 1;
            break;
        }
        field_texts[field_index] = PyUnicode_AsUTF8(field);
        int type = ustore_doc_field_json_k;
        failed = !field_texts[field_index] ||
                 ustore_py_choice_from_object(type_object, "types", column_type_namings, &type) != 0;
        if (failed) break;
        types[field_index] = (ustore_doc_field_type_t)type;
    }
    Py_XDECREF(types_held);

    ustore_py_keys_t keys;
    if (!failed && ustore_py_keys_from_object(args[0], &keys) != 0) failed = 1;
    if (failed) {
        PyMem_Free(field_texts);
        PyMem_Free(types);
        Py_DECREF(fields_held);
        return NULL;
    }

    ustore_octet_t** validities = NULL;
    ustore_octet_t** conversions = NULL;
    ustore_octet_t** collisions = NULL;
    ustore_byte_t** scalars = NULL;
    ustore_length_t** offsets = NULL;
    ustore_length_t** lengths = NULL;
    ustore_byte_t* joined = NULL;
    ustore_error_t error = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context.database);

    ustore_docs_gather_t gather = {0};
    gather.db = context.database->handle;
    gather.error = &error;
    gather.transaction = context.transaction;
    gather.snapshot = options.snapshot;
    gather.arena = &arena;
    gather.options = options.options;
    gather.docs_count = keys.count;
    gather.fields_count = fields_count;
    gather.collections = &options.collection;
    gather.keys = keys.keys;
    gather.keys_stride = sizeof(ustore_key_t);
    gather.fields = field_texts;
    gather.fields_stride = sizeof(ustore_str_view_t);
    gather.types = types;
    gather.types_stride = sizeof(ustore_doc_field_type_t);
    gather.columns_validities = &validities;
    gather.columns_conversions = &conversions;
    gather.columns_collisions = &collisions;
    gather.columns_scalars = &scalars;
    gather.columns_offsets = &offsets;
    gather.columns_lengths = &lengths;
    gather.joined_strings = &joined;

    Py_BEGIN_ALLOW_THREADS;
    ustore_docs_gather(&gather);
    Py_END_ALLOW_THREADS;

    ustore_py_keys_release(&keys);
    PyMem_Free(field_texts);
    Py_DECREF(fields_held);
    if (error.status != ustore_success_k) {
        PyMem_Free(types);
        ustore_py_arena_return(context.database, arena);
        ustore_py_raise_on_error(&error);
        return NULL;
    }
    return columns_adopt(arena, types, &gather);
}

#pragma endregion Gathers

#pragma region Columns

static char const doc_Columns[] =                                                               //
    "The typed columns one gather exported, read straight out of the arena holding them.\n"     //
    "\n"                                                                                        //
    "Indexing a scalar column answers a `memoryview` of its type over the stored scalars, a\n"  //
    "'uuid' column a two-dimensional view of sixteen bytes per document, and a string column\n" //
    "a tuple of `str` or `bytes`, with None where the value is not valid. An 'f16' column\n"    //
    "carries the 'e' format, which NumPy reads as float16 without a copy.\n"                    //
    "\n"                                                                                        //
    "The bitmaps address documents least significant bit first, as Apache\n"                    //
    "Arrow does, and every view keeps this object, and therefore the arena, alive.";

static void Columns_dealloc(Columns* self) {
    ustore_arena_free(self->arena);
    PyMem_Free(self->types);
    PyObject_Free(self);
}

static Py_ssize_t Columns_length(Columns* self) { return (Py_ssize_t)self->fields_count; }

/** Normalizes @p index_object into a field index, raising @c IndexError past the columns. */
static int columns_field_index(Columns const* self, PyObject* index_object, size_t* field_index) {
    Py_ssize_t index = PyNumber_AsSsize_t(index_object, PyExc_IndexError);
    if (index == -1 && PyErr_Occurred()) return -1;
    if (index < 0) index += (Py_ssize_t)self->fields_count;
    if (index < 0 || (size_t)index >= self->fields_count) {
        PyErr_SetString(PyExc_IndexError, "Field index out of range");
        return -1;
    }
    *field_index = (size_t)index;
    return 0;
}

/** Answers whether the value of document @p row in field @p field_index is valid. */
static int columns_valid(Columns const* self, size_t field_index, size_t row) {
    return (self->validities[field_index][row / 8] >> (row % 8)) & 1;
}

static PyObject* Columns_item(Columns* self, Py_ssize_t field_index) {
    if (field_index < 0 || (size_t)field_index >= self->fields_count) {
        PyErr_SetString(PyExc_IndexError, "Field index out of range");
        return NULL;
    }
    ustore_doc_field_type_t const type = self->types[field_index];
    char const* const format = field_type_format(type);
    Py_ssize_t const rows = (Py_ssize_t)self->docs_count;
    if (format) {
        void* const data = rows ? (void*)self->scalars[field_index] : NULL;
        return ustore_py_span_view((PyObject*)self, data, format, (Py_ssize_t)field_type_width(type), 1, &rows);
    }
    if (type == ustore_doc_field_uuid_k) {
        void* const data = rows ? (void*)self->scalars[field_index] : NULL;
        Py_ssize_t const shape[2] = {rows, uuid_bytes_k};
        return ustore_py_span_view((PyObject*)self, data, "B", 1, 2, shape);
    }

    PyObject* const strings = PyTuple_New(rows);
    if (!strings) return NULL;
    for (Py_ssize_t row = 0; row != rows; ++row) {
        PyObject* entry = NULL;
        if (!columns_valid(self, (size_t)field_index, (size_t)row)) { entry = Py_NewRef(Py_None); }
        else {
            Py_ssize_t const length = (Py_ssize_t)self->lengths[field_index][row];
            // Nothing but empty strings leaves the joined contents without an address.
            char const* const text = length ? (char const*)self->joined + self->offsets[field_index][row] : "";
            entry = type == ustore_doc_field_str_k ? PyUnicode_DecodeUTF8(text, length, "strict")
                                                   : PyBytes_FromStringAndSize(text, length);
        }
        if (!entry) {
            Py_DECREF(strings);
            return NULL;
        }
        PyTuple_SET_ITEM(strings, row, entry);
    }
    return strings;
}

/** Answers one bitmap of @p bitmaps as a byte view, for the field @p index_object names. */
static PyObject* columns_bitmap(Columns* self, ustore_octet_t** bitmaps, PyObject* index_object) {
    size_t field_index = 0;
    if (columns_field_index(self, index_object, &field_index) != 0) return NULL;
    Py_ssize_t const bytes = (Py_ssize_t)((self->docs_count + 7) / 8);
    void* const data = bytes ? (void*)bitmaps[field_index] : NULL;
    return ustore_py_span_view((PyObject*)self, data, "B", 1, 1, &bytes);
}

static char const doc_Columns_validity[] =
    "validity(field) answers the bitmap of documents whose value was found and converted, as bytes.";

static PyObject* Columns_validity(Columns* self, PyObject* index_object) {
    return columns_bitmap(self, self->validities, index_object);
}

static char const doc_Columns_conversion[] =
    "conversion(field) answers the bitmap of values whose conversion changed their meaning, as bytes.";

static PyObject* Columns_conversion(Columns* self, PyObject* index_object) {
    return columns_bitmap(self, self->conversions, index_object);
}

static char const doc_Columns_collision[] =
    "collision(field) answers the bitmap of values of a type no conversion reaches, as bytes.";

static PyObject* Columns_collision(Columns* self, PyObject* index_object) {
    return columns_bitmap(self, self->collisions, index_object);
}

static char const doc_Columns_types[] = "The type every column was converted into, as a tuple of names.";

static PyObject* Columns_get_types(Columns* self, void* Py_UNUSED(closure)) {
    PyObject* const types = PyTuple_New((Py_ssize_t)self->fields_count);
    if (!types) return NULL;
    for (size_t field_index = 0; field_index != self->fields_count; ++field_index) {
        PyObject* const name = PyUnicode_FromString(field_type_name(self->types[field_index]));
        if (!name) {
            Py_DECREF(types);
            return NULL;
        }
        PyTuple_SET_ITEM(types, (Py_ssize_t)field_index, name);
    }
    return types;
}

static char const doc_Columns_rows[] = "The documents the gather carried, the length of every column.";

static PyObject* Columns_get_rows(Columns* self, void* Py_UNUSED(closure)) {
    return PyLong_FromSize_t(self->docs_count);
}

static PyObject* Columns_repr(Columns* self) {
    return PyUnicode_FromFormat("Columns(fields=%zu, rows=%zu)", self->fields_count, self->docs_count);
}

static PyMethodDef Columns_methods[] = {
    {"validity", (PyCFunction)Columns_validity, METH_O, doc_Columns_validity},
    {"conversion", (PyCFunction)Columns_conversion, METH_O, doc_Columns_conversion},
    {"collision", (PyCFunction)Columns_collision, METH_O, doc_Columns_collision},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef Columns_getset[] = {
    {"types", (getter)Columns_get_types, NULL, doc_Columns_types, NULL},
    {"rows", (getter)Columns_get_rows, NULL, doc_Columns_rows, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PySequenceMethods Columns_as_sequence = {
    .sq_length = (lenfunc)Columns_length,
    .sq_item = (ssizeargfunc)Columns_item,
};

PyTypeObject ColumnsType = {
    PyVarObject_HEAD_INIT(NULL, 0) //
        .tp_name = "ustore.Columns",
    .tp_doc = doc_Columns,
    .tp_basicsize = sizeof(Columns),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = NULL,
    .tp_dealloc = (destructor)Columns_dealloc,
    .tp_repr = (reprfunc)Columns_repr,
    .tp_as_sequence = &Columns_as_sequence,
    .tp_methods = Columns_methods,
    .tp_getset = Columns_getset,
};

#pragma endregion Columns
