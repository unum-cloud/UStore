/**
 *  @file python/docs.c
 *  @author Ash Vardanian
 *  @date September 17, 2026
 *  @brief JSON documents: writes, reads, gists and typed gathers.
 *
 *  A document read answers the same tape a binary read does, one run of bytes per task, while a
 *  gather answers typed columns whose scalars export in place and whose strings decode on access.
 *  Every verb drops the interpreter lock around the call, and every read claims the database's
 *  arena and either returns it or hands it to the result it answers with.
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

#pragma region JSON Conversions

/** Reads a number too wide for 64 bits, which a document keeps as its digits, as the int or float they spell. */
static PyObject* object_from_digits(yyjson_val* value) {
    char const* const digits = yyjson_get_raw(value);
    size_t const length = yyjson_get_len(value);
    int integral = length != 0;
    for (size_t index = 0; index != length; ++index)
        integral &= (digits[index] >= '0' && digits[index] <= '9') || (index == 0 && digits[index] == '-');
    PyObject* const text = PyUnicode_DecodeASCII(digits, (Py_ssize_t)length, "strict");
    if (!text) return NULL;
    PyObject* const number = integral ? PyLong_FromUnicodeObject(text, 10) : PyFloat_FromString(text);
    Py_DECREF(text);
    return number;
}

/** Builds the list of @p value, converting every element. */
static PyObject* list_from_json(yyjson_val* value) {
    PyObject* const list = PyList_New((Py_ssize_t)yyjson_arr_size(value));
    if (!list) return NULL;
    size_t index = 0, count = 0;
    yyjson_val* element = NULL;
    yyjson_arr_foreach(value, index, count, element) {
        PyObject* const item = ustore_py_object_from_json(element);
        if (!item) {
            Py_DECREF(list);
            return NULL;
        }
        PyList_SET_ITEM(list, (Py_ssize_t)index, item);
    }
    return list;
}

/** Builds the dict of @p value, converting every member, where a repeated key keeps its last value. */
static PyObject* dict_from_json(yyjson_val* value) {
    PyObject* const dict = PyDict_New();
    if (!dict) return NULL;
    size_t index = 0, count = 0;
    yyjson_val* key = NULL;
    yyjson_val* member = NULL;
    yyjson_obj_foreach(value, index, count, key, member) {
        PyObject* const name = PyUnicode_DecodeUTF8(yyjson_get_str(key), (Py_ssize_t)yyjson_get_len(key), "strict");
        PyObject* const item = name ? ustore_py_object_from_json(member) : NULL;
        int const added = item ? PyDict_SetItem(dict, name, item) : -1;
        Py_XDECREF(name);
        Py_XDECREF(item);
        if (added != 0) {
            Py_DECREF(dict);
            return NULL;
        }
    }
    return dict;
}

PyObject* ustore_py_object_from_json(yyjson_val* value) {
    switch (yyjson_get_type(value)) {
    case YYJSON_TYPE_NULL: Py_RETURN_NONE;
    case YYJSON_TYPE_BOOL: return PyBool_FromLong(yyjson_get_bool(value));
    case YYJSON_TYPE_NUM:
        switch (yyjson_get_subtype(value)) {
        case YYJSON_SUBTYPE_UINT: return PyLong_FromUnsignedLongLong(yyjson_get_uint(value));
        case YYJSON_SUBTYPE_SINT: return PyLong_FromLongLong(yyjson_get_sint(value));
        default: return PyFloat_FromDouble(yyjson_get_real(value));
        }
    case YYJSON_TYPE_RAW: return object_from_digits(value);
    case YYJSON_TYPE_STR:
        return PyUnicode_DecodeUTF8(yyjson_get_str(value), (Py_ssize_t)yyjson_get_len(value), "strict");
    case YYJSON_TYPE_ARR:
    case YYJSON_TYPE_OBJ: {
        // A document nests as deep as its text does, so the C stack is guarded like any recursion in Python.
        if (Py_EnterRecursiveCall(" while reading a document") != 0) return NULL;
        PyObject* const container = yyjson_is_arr(value) ? list_from_json(value) : dict_from_json(value);
        Py_LeaveRecursiveCall();
        return container;
    }
    default: ustore_py_raise(ustore_status_malformed_k, "The document holds a value JSON has no type for"); return NULL;
    }
}

/** Serializes an integer, keeping one past 64 bits as its digits, which JSON holds at any width. */
static int json_from_integer(PyObject* object, yyjson_mut_doc* document, yyjson_mut_val** value) {
    int overflow = 0;
    long long const narrow = PyLong_AsLongLongAndOverflow(object, &overflow);
    if (narrow == -1 && PyErr_Occurred()) return -1;
    unsigned long long wide = 0;
    if (!overflow) *value = yyjson_mut_sint(document, narrow);
    else if (overflow > 0 && (wide = PyLong_AsUnsignedLongLong(object)) != (unsigned long long)-1)
        *value = yyjson_mut_uint(document, wide);
    else {
        if (PyErr_Occurred() && !PyErr_ExceptionMatches(PyExc_OverflowError)) return -1;
        PyErr_Clear();
        PyObject* const digits = PyNumber_ToBase(object, 10);
        if (!digits) return -1;
        Py_ssize_t length = 0;
        char const* const text = PyUnicode_AsUTF8AndSize(digits, &length);
        *value = text ? yyjson_mut_rawncpy(document, text, (size_t)length) : NULL;
        Py_DECREF(digits);
        if (!text) return -1;
    }
    if (*value) return 0;
    PyErr_NoMemory();
    return -1;
}

/** Serializes a dict, whose keys must be strings, as JSON objects' keys are. */
static int json_from_dict(PyObject* object, yyjson_mut_doc* document, yyjson_mut_val** value) {
    yyjson_mut_val* const members = yyjson_mut_obj(document);
    if (!members) {
        PyErr_NoMemory();
        return -1;
    }
    int failed = 0;
    Py_ssize_t position = 0;
    PyObject* name = NULL;
    PyObject* member = NULL;
    ustore_py_critical_section_begin_(object);
    while (!failed && PyDict_Next(object, &position, &name, &member)) {
        if (!PyUnicode_Check(name)) {
            PyErr_Format(PyExc_TypeError, "A document's keys are strings, not %S", Py_TYPE(name));
            failed = 1;
            break;
        }
        Py_ssize_t length = 0;
        char const* const text = PyUnicode_AsUTF8AndSize(name, &length);
        yyjson_mut_val* const key = text ? yyjson_mut_strncpy(document, text, (size_t)length) : NULL;
        // The member is held, since a nested critical section may let another thread change this dict.
        PyObject* const held = Py_NewRef(member);
        yyjson_mut_val* item = NULL;
        failed = !key || ustore_py_json_from_object(held, document, &item) != 0 ||
                 !yyjson_mut_obj_add(members, key, item);
        Py_DECREF(held);
        if (failed && !PyErr_Occurred()) PyErr_NoMemory();
    }
    ustore_py_critical_section_end_();
    *value = members;
    return failed ? -1 : 0;
}

/** Serializes a list or a tuple as a JSON array. */
static int json_from_sequence(PyObject* object, yyjson_mut_doc* document, yyjson_mut_val** value) {
    yyjson_mut_val* const items = yyjson_mut_arr(document);
    if (!items) {
        PyErr_NoMemory();
        return -1;
    }
    int failed = 0;
    ustore_py_critical_section_begin_(object);
    for (Py_ssize_t index = 0; !failed && index < PySequence_Fast_GET_SIZE(object); ++index) {
        PyObject* const held = Py_NewRef(PySequence_Fast_GET_ITEM(object, index));
        yyjson_mut_val* item = NULL;
        failed = ustore_py_json_from_object(held, document, &item) != 0 || !yyjson_mut_arr_append(items, item);
        Py_DECREF(held);
        if (failed && !PyErr_Occurred()) PyErr_NoMemory();
    }
    ustore_py_critical_section_end_();
    *value = items;
    return failed ? -1 : 0;
}

int ustore_py_json_from_object(PyObject* object, yyjson_mut_doc* document, yyjson_mut_val** value) {
    // A bool is an int too, so it is told apart first.
    if (object == Py_None) *value = yyjson_mut_null(document);
    else if (PyBool_Check(object)) *value = yyjson_mut_bool(document, object == Py_True);
    else if (PyLong_Check(object)) return json_from_integer(object, document, value);
    else if (PyFloat_Check(object)) *value = yyjson_mut_real(document, PyFloat_AS_DOUBLE(object));
    else if (PyUnicode_Check(object)) {
        Py_ssize_t length = 0;
        char const* const text = PyUnicode_AsUTF8AndSize(object, &length);
        if (!text) return -1;
        *value = yyjson_mut_strncpy(document, text, (size_t)length);
    }
    else if (PyDict_Check(object) || PyList_Check(object) || PyTuple_Check(object)) {
        if (Py_EnterRecursiveCall(" while serializing a document") != 0) return -1;
        int const serialized = PyDict_Check(object) ? json_from_dict(object, document, value)
                                                    : json_from_sequence(object, document, value);
        Py_LeaveRecursiveCall();
        return serialized;
    }
    else {
        PyErr_Format(PyExc_TypeError, "A document holds dicts, lists, strings, numbers, booleans and None, not %S",
                     Py_TYPE(object));
        return -1;
    }
    if (*value) return 0;
    PyErr_NoMemory();
    return -1;
}

#pragma endregion JSON Conversions

#pragma region Writes

/**
 *  @brief The documents one write batch hands the ABI.
 *
 *  A string travels as the UTF-8 bytes it caches, so the batch holds a tuple of every document for
 *  the length of the call, which no concurrent change to the caller's list can release. A document
 *  given as a Python object is serialized into memory the batch owns until it is released.
 */
typedef struct documents_t {

    /** The borrowed runs the ABI reads. */
    ustore_py_contents_t contents;

    /** A tuple of the documents, keeping every string alive, or NULL. */
    PyObject* held;

    /** The text of every serialized document, or NULL before the first. */
    yyjson_alc* serialized;
} documents_t;

/** Appends @p object serialized as JSON to @p documents, keeping the text until the batch is released. */
static int document_serialize(PyObject* object, documents_t* documents) {
    if (!documents->serialized && !(documents->serialized = yyjson_alc_dyn_new())) {
        PyErr_NoMemory();
        return -1;
    }
    yyjson_mut_doc* const document = yyjson_mut_doc_new(NULL);
    if (!document) {
        PyErr_NoMemory();
        return -1;
    }
    yyjson_mut_val* root = NULL;
    yyjson_write_err error = {0};
    size_t length = 0;
    // The engine reads NaN and infinities back, as Python's own `json` writes them.
    char const* const text = ustore_py_json_from_object(object, document, &root) != 0
                                 ? NULL
                                 : yyjson_mut_val_write_opts(root, YYJSON_WRITE_ALLOW_INF_AND_NAN,
                                                             documents->serialized, &length, &error);
    yyjson_mut_doc_free(document);
    if (!text) {
        if (!PyErr_Occurred())
            ustore_py_raise(error.code == YYJSON_WRITE_ERROR_MEMORY_ALLOCATION ? ustore_status_out_of_memory_k
                                                                               : ustore_status_malformed_k,
                            "Cannot serialize the document: %s", error.msg ? error.msg : "no detail");
        return -1;
    }
    ustore_py_contents_t* const batch = &documents->contents;
    batch->values[batch->count] = (ustore_bytes_cptr_t)text;
    batch->lengths[batch->count] = (ustore_length_t)length;
    ++batch->count;
    return 0;
}

/** Borrows one document: a string as its UTF-8 bytes, a buffer as it is, and any other object but None
 *  serialized when the documents are JSON, since a removal takes a verb of its own. */
static int document_borrow(PyObject* object, ustore_doc_field_type_t type, documents_t* documents) {
    ustore_py_contents_t* const batch = &documents->contents;
    if (!PyUnicode_Check(object)) {
        if (object == Py_None || type != ustore_doc_field_json_k || PyObject_CheckBuffer(object))
            return ustore_py_contents_borrow(object, batch);
        return document_serialize(object, documents);
    }
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
    if (documents->serialized) yyjson_alc_dyn_free(documents->serialized);
    documents->serialized = NULL;
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
 *  @brief Gathers the documents of a write: a sequence, or one document for every key.
 *  @param[in] object The documents, each a string, a bytes-like object, or a dict or list to serialize.
 *  @param[in] type How every document is spelled, where only JSON serializes Python objects.
 *  @param[out] documents The borrowed runs and the layout they travel with.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
static int documents_from_object(PyObject* object, ustore_doc_field_type_t type, documents_t* documents) {
    memset(documents, 0, sizeof(*documents));
    // A string, a bytes-like object or a dict is one document for every key, not a sequence of characters or keys.
    if (PyUnicode_Check(object) || PyObject_CheckBuffer(object) || PyDict_Check(object)) {
        if (ustore_py_contents_reserve(&documents->contents, 1) != 0) return -1;
        if (document_borrow(object, type, documents) != 0) {
            documents_release(documents);
            return -1;
        }
        documents->contents.layout = ustore_py_contents_broadcast_k;
        return 0;
    }

    documents->held = PySequence_Tuple(object);
    if (!documents->held)
        return ustore_py_raise_from_cause(PyExc_TypeError, "Expected a sequence of documents or one document, not %S",
                                          Py_TYPE(object));
    size_t const count = (size_t)PyTuple_GET_SIZE(documents->held);
    if (ustore_py_contents_reserve(&documents->contents, count) != 0) {
        Py_CLEAR(documents->held);
        return -1;
    }
    for (size_t task_index = 0; task_index != count; ++task_index)
        if (document_borrow(PyTuple_GET_ITEM(documents->held, (Py_ssize_t)task_index), type, documents) != 0) {
            documents_release(documents);
            return -1;
        }
    return 0;
}

/**
 *  @brief Gathers one Python object per key, each serialized as a whole document, so text is a JSON string.
 *  @param[in] objects A sequence of documents, as `Collection.docs` is assigned them.
 *  @param[out] documents The serialized runs, one per key.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
static int documents_from_objects(PyObject* objects, documents_t* documents) {
    memset(documents, 0, sizeof(*documents));
    if (PyUnicode_Check(objects) || PyDict_Check(objects) || PyObject_CheckBuffer(objects)) {
        PyErr_Format(PyExc_TypeError, "A batch of documents is a sequence of them, not one %S", Py_TYPE(objects));
        return -1;
    }
    documents->held = PySequence_Tuple(objects);
    if (!documents->held)
        return ustore_py_raise_from_cause(PyExc_TypeError, "A batch of documents is a sequence of them, not %S",
                                          Py_TYPE(objects));
    size_t const count = (size_t)PyTuple_GET_SIZE(documents->held);
    if (ustore_py_contents_reserve(&documents->contents, count) != 0) {
        Py_CLEAR(documents->held);
        return -1;
    }
    for (size_t task_index = 0; task_index != count; ++task_index)
        if (document_serialize(PyTuple_GET_ITEM(documents->held, (Py_ssize_t)task_index), documents) != 0) {
            documents_release(documents);
            return -1;
        }
    return 0;
}

/**
 *  @brief Runs one @c ustore_docs_write with the transaction of @p context, the options of a call and the runs of
 *      @p documents.
 *  @param[inout] write The request, whose tasks, keys and modification the caller filled.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
static int documents_write(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                           ustore_docs_write_t* write, documents_t const* documents) {
    ustore_outcome_t outcome = {0};
    int const broadcast = documents->contents.layout == ustore_py_contents_broadcast_k;
    write->database = context->database->handle;
    write->outcome = &outcome;
    write->transaction = context->transaction;
    write->collections = &options->collection;
    write->values = documents->contents.count ? documents->contents.values : NULL;
    write->values_stride = broadcast ? 0 : sizeof(ustore_bytes_cptr_t);
    write->lengths = documents->contents.count ? documents->contents.lengths : NULL;
    write->lengths_stride = broadcast ? 0 : sizeof(ustore_length_t);
    write->durability = options->durability;
    write->watch = options->watch;

    Py_BEGIN_ALLOW_THREADS;
    ustore_docs_write(write);
    Py_END_ALLOW_THREADS;
    return ustore_py_raise_on_outcome(&outcome);
}

int ustore_py_docs_upsert(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                          ustore_key_t const* keys, size_t count, PyObject* objects,
                          ustore_py_documents_spelling_t spelling) {
    documents_t documents;
    int const gathered = spelling == ustore_py_documents_as_objects_k
                             ? documents_from_objects(objects, &documents)
                             : documents_from_object(objects, ustore_doc_field_json_k, &documents);
    if (gathered != 0) return -1;
    if (documents.held && documents.contents.count != count) {
        PyErr_Format(PyExc_ValueError, "Got %zu documents for %zu keys", documents.contents.count, count);
        documents_release(&documents);
        return -1;
    }

    ustore_docs_write_t write = {0};
    write.count = count;
    write.type = ustore_doc_field_json_k;
    write.modification = ustore_doc_modify_upsert_k;
    write.keys = keys;
    write.keys_stride = sizeof(ustore_key_t);
    int const written = documents_write(context, options, &write, &documents);
    documents_release(&documents);
    return written;
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
    "  documents (Sequence[str | bytes-like | object] | str | bytes-like | dict): One\n"         //
    "    document per key, or one document broadcast to every key. Text and bytes are JSON\n"    //
    "    already, while a dict, a list or a number inside a sequence is serialized. None is\n"   //
    "    refused: `remove` removes whole documents, and a 'merge' with a JSON null removes\n"    //
    "    a field. Under a fixed-width `type`, a contiguous buffer of one scalar per key is a\n"  //
    "    column, which reaches the store without being printed as JSON and parsed back.\n"       //
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
    "  TypeError: A document is None.\n"                                                         //
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
                                  : documents_from_object(args[1], (ustore_doc_field_type_t)type, &documents);
    if (gathered != 0) {
        ustore_py_keys_release(&keys);
        return NULL;
    }

    size_t count = 0;
    if (args[0] == Py_None) {
        // Every key is read out of its own document, so each task needs a document of its own.
        if (!id_field || !documents.held) {
            documents_release(&documents);
            PyErr_SetString(PyExc_TypeError, "Documents without keys need a sequence of them and an 'id_field'");
            return NULL;
        }
        count = documents.contents.count;
    }
    else {
        count = keys.count;
        if (documents.held && documents.contents.count != keys.count) {
            PyErr_Format(PyExc_ValueError, "Got %zu documents for %zu keys", documents.contents.count, keys.count);
            documents_release(&documents);
            ustore_py_keys_release(&keys);
            return NULL;
        }
    }

    ustore_docs_write_t write = {0};
    write.count = count;
    write.type = (ustore_doc_field_type_t)type;
    write.modification = (ustore_doc_modification_t)modification;
    write.keys = keys.keys;
    write.keys_stride = sizeof(ustore_key_t);
    write.fields = field ? &field : NULL;
    write.id_field = id_field;
    int const written = documents_write(&context, &options, &write, &documents);

    documents_release(&documents);
    ustore_py_keys_release(&keys);
    if (written != 0) return NULL;
    Py_RETURN_NONE;
}

#pragma endregion Writes

#pragma region Reads

char const doc_docs_read[] =                                                                   //
    "docs_read(keys, *, collection=None, snapshot=None, field=None, type='json') answers\n"    //
    "a Values tape.\n"                                                                         //
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

PyObject* ustore_py_docs_read(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                              ustore_key_t const* keys, size_t count, char const* field, ustore_doc_field_type_t type) {
    Values* const result = ustore_py_values_adopt(count);
    if (!result) return NULL;
    ustore_outcome_t outcome = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context->database);

    ustore_docs_read_t read = {0};
    read.database = context->database->handle;
    read.outcome = &outcome;
    read.view = options->view;
    read.count = count;
    read.collections = &options->collection;
    read.keys = keys;
    read.keys_stride = sizeof(ustore_key_t);
    read.fields = field ? &field : NULL;
    read.presences = result->presences;
    read.offsets = result->offsets;
    read.values = &result->tape;
    read.arena = &arena;
    read.type = type;

    Py_BEGIN_ALLOW_THREADS;
    ustore_docs_read(&read);
    Py_END_ALLOW_THREADS;

    if (ustore_py_raise_on_outcome(&outcome) != 0) {
        ustore_py_arena_return(context->database, arena);
        Py_DECREF(result);
        return NULL;
    }
    result->arena = arena;
    return (PyObject*)result;
}

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
    PyObject* const values = ustore_py_docs_read(&context, &options, keys.keys, keys.count, field,
                                                 (ustore_doc_field_type_t)type);
    ustore_py_keys_release(&keys);
    return values;
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
    ustore_outcome_t outcome = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context.database);

    ustore_docs_gist_t gist = {0};
    gist.database = context.database->handle;
    gist.outcome = &outcome;
    gist.view = options.view;
    gist.count = keys.count;
    gist.collections = &options.collection;
    gist.keys = keys.keys;
    gist.keys_stride = sizeof(ustore_key_t);
    gist.fields_count = &fields_count;
    gist.offsets = &offsets;
    gist.fields = &fields;
    gist.arena = &arena;

    Py_BEGIN_ALLOW_THREADS;
    ustore_docs_gist(&gist);
    Py_END_ALLOW_THREADS;

    ustore_py_keys_release(&keys);
    PyObject* listed = NULL;
    // Every path is a `str`, which owns its bytes, so the arena goes back to the database at once.
    if (ustore_py_raise_on_outcome(&outcome) == 0 && (listed = PyTuple_New((Py_ssize_t)fields_count)) != NULL) {
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

/** Rounds @p bytes up to the eight bytes every run of the trailing storage of @c Columns starts at. */
static size_t columns_aligned(size_t bytes) { return (bytes + 7) & ~(size_t)7; }

/** The bytes one column of @p type takes for @p docs_count documents: offsets and lengths for strings, else scalars. */
static size_t column_bytes(ustore_doc_field_type_t type, size_t docs_count) {
    if (type == ustore_doc_field_str_k || type == ustore_doc_field_bin_k)
        return (2 * docs_count + 1) * sizeof(ustore_length_t);
    return columns_aligned(field_type_width(type) * docs_count);
}

/**
 *  @brief Allocates a @c Columns whose trailing storage a gather of @p docs_count documents writes every column into.
 *
 *  The storage holds the six runs of column pointers the ABI takes, then the columns, then the types, then the three
 *  bitmaps of every field, which start cleared so the bits past the last document read as unset.
 *
 *  @param[in] types The type of every field, copied into the result.
 *  @return A new reference with no arena, or NULL with an exception set.
 */
static Columns* columns_adopt(ustore_doc_field_type_t const* types, size_t fields_count, size_t docs_count) {
    size_t const pointers_bytes = columns_aligned(6 * fields_count * sizeof(void*));
    size_t columns_bytes = 0;
    for (size_t field_index = 0; field_index != fields_count; ++field_index)
        columns_bytes += column_bytes(types[field_index], docs_count);
    size_t const types_bytes = columns_aligned(fields_count * sizeof(ustore_doc_field_type_t));
    size_t const bitmap_bytes = (docs_count + 7) / 8;
    size_t const bitmaps_bytes = 3 * fields_count * bitmap_bytes;
    size_t const storage_bytes = pointers_bytes + columns_bytes + types_bytes + bitmaps_bytes;

    Columns* const instance = PyObject_NewVar(Columns, &ColumnsType, (Py_ssize_t)storage_bytes);
    if (!instance) return NULL;
    unsigned char* const storage = (unsigned char*)instance->storage;
    void** const pointers = (void**)storage;
    instance->validities = (ustore_octet_t**)pointers;
    instance->conversions = (ustore_octet_t**)(pointers + fields_count);
    instance->collisions = (ustore_octet_t**)(pointers + 2 * fields_count);
    instance->scalars = (ustore_byte_t**)(pointers + 3 * fields_count);
    instance->offsets = (ustore_length_t**)(pointers + 4 * fields_count);
    instance->lengths = (ustore_length_t**)(pointers + 5 * fields_count);

    unsigned char* column = storage + pointers_bytes;
    for (size_t field_index = 0; field_index != fields_count; ++field_index) {
        ustore_doc_field_type_t const type = types[field_index];
        int const textual = type == ustore_doc_field_str_k || type == ustore_doc_field_bin_k;
        instance->scalars[field_index] = textual ? NULL : column;
        instance->offsets[field_index] = textual ? (ustore_length_t*)(void*)column : NULL;
        instance->lengths[field_index] = textual ? (ustore_length_t*)(void*)column + docs_count + 1 : NULL;
        column += column_bytes(type, docs_count);
    }
    instance->types = (ustore_doc_field_type_t*)(void*)column;
    if (fields_count) memcpy(instance->types, types, fields_count * sizeof(ustore_doc_field_type_t));

    unsigned char* const bitmaps = column + types_bytes;
    memset(bitmaps, 0, bitmaps_bytes);
    for (size_t field_index = 0; field_index != fields_count; ++field_index) {
        instance->validities[field_index] = bitmaps + 3 * field_index * bitmap_bytes;
        instance->conversions[field_index] = instance->validities[field_index] + bitmap_bytes;
        instance->collisions[field_index] = instance->conversions[field_index] + bitmap_bytes;
    }
    instance->arena = NULL;
    instance->joined = NULL;
    instance->fields_count = fields_count;
    instance->docs_count = docs_count;
    return instance;
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

    // The types are copied into the result once its storage is sized by them.
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
    Columns* const columns = failed ? NULL : columns_adopt(types, fields_count, keys.count);
    PyMem_Free(types);
    if (!columns) {
        if (!failed) ustore_py_keys_release(&keys);
        PyMem_Free(field_texts);
        Py_DECREF(fields_held);
        return NULL;
    }

    ustore_outcome_t outcome = {0};
    ustore_arena_t arena = ustore_py_arena_claim(context.database);

    ustore_docs_gather_t gather = {0};
    gather.database = context.database->handle;
    gather.outcome = &outcome;
    gather.view = options.view;
    gather.count = keys.count;
    gather.fields_count = fields_count;
    gather.collections = &options.collection;
    gather.keys = keys.keys;
    gather.keys_stride = sizeof(ustore_key_t);
    gather.fields = field_texts;
    gather.fields_stride = sizeof(ustore_str_view_t);
    gather.types = columns->types;
    gather.types_stride = sizeof(ustore_doc_field_type_t);
    gather.columns_validities = columns->validities;
    gather.columns_conversions = columns->conversions;
    gather.columns_collisions = columns->collisions;
    gather.columns_scalars = columns->scalars;
    gather.columns_offsets = columns->offsets;
    gather.columns_lengths = columns->lengths;
    gather.joined_strings = &columns->joined;
    gather.arena = &arena;

    Py_BEGIN_ALLOW_THREADS;
    ustore_docs_gather(&gather);
    Py_END_ALLOW_THREADS;

    ustore_py_keys_release(&keys);
    PyMem_Free(field_texts);
    Py_DECREF(fields_held);
    if (ustore_py_raise_on_outcome(&outcome) != 0) {
        ustore_py_arena_return(context.database, arena);
        Py_DECREF(columns);
        return NULL;
    }
    columns->arena = arena;
    return (PyObject*)columns;
}

#pragma endregion Gathers

#pragma region Columns

static char const doc_Columns[] =                                                               //
    "The typed columns one gather exported, read straight out of the memory holding them.\n"    //
    "\n"                                                                                        //
    "Indexing a scalar column answers a `memoryview` of its type over the stored scalars, a\n"  //
    "'uuid' column a two-dimensional view of sixteen bytes per document, and a string column\n" //
    "a tuple of `str` or `bytes`, with None where the value is not valid. An 'f16' column\n"    //
    "carries the 'e' format, which NumPy reads as float16 without a copy.\n"                    //
    "\n"                                                                                        //
    "The bitmaps address documents least significant bit first, as Apache\n"                    //
    "Arrow does, and every view keeps this object, and therefore its memory, alive.";

static void Columns_dealloc(Columns* self) {
    ustore_arena_free(self->arena);
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
    .tp_itemsize = 1,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = NULL,
    .tp_dealloc = (destructor)Columns_dealloc,
    .tp_repr = (reprfunc)Columns_repr,
    .tp_as_sequence = &Columns_as_sequence,
    .tp_methods = Columns_methods,
    .tp_getset = Columns_getset,
};

#pragma endregion Columns
