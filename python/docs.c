/**
 *  @file python/docs.c
 *  @author Ash Vardanian
 *  @date September 17, 2026
 *  @brief JSON documents: writes, reads, gists and typed gathers.
 *
 *  A document read answers the same tape a binary read does, one run of bytes per task, while a
 *  gather answers typed columns whose scalars export in place and whose strings decode on access.
 *  Every verb fills a call the driver keeps on the stack or inside a @c Deferred, holding the
 *  strings and documents it borrows until the call finishes, and every read claims the arena it is
 *  given and answers results viewing it, or copies out of a one-off arena for the mapping views.
 */
#include "lib.h"

#pragma region Field Types

/** Bytes one UUID takes, as the ABI lays it out. */
enum { uuid_bytes_k = 16 };

/**
 *  @brief Answers the struct-module format one scalar of a field type exports as.
 *
 *  A UUID is sixteen bytes rather than a scalar @c struct spells, so it carries no format and
 *  travels as a two-dimensional run of bytes instead.
 *
 *  @param[in] type The field type.
 *  @return The format, or NULL for UUIDs, strings and every type a gather does not fill.
 */
static char const* field_type_format(ustore_docs_field_type_t type) {
    switch (type) {
    case ustore_docs_field_type_bool_k: return "?";
    case ustore_docs_field_type_f64_k: return "d";
    case ustore_docs_field_type_f32_k: return "f";
    case ustore_docs_field_type_f16_k: return "e";
    case ustore_docs_field_type_i64_k: return "q";
    case ustore_docs_field_type_i32_k: return "i";
    case ustore_docs_field_type_i16_k: return "h";
    case ustore_docs_field_type_i8_k: return "b";
    case ustore_docs_field_type_u64_k: return "Q";
    case ustore_docs_field_type_u32_k: return "I";
    case ustore_docs_field_type_u16_k: return "H";
    case ustore_docs_field_type_u8_k: return "B";
    default: return NULL;
    }
}

/**
 *  @brief Answers the bytes one value of a fixed-width field type takes.
 *  @param[in] type The field type.
 *  @return The width, or zero for JSON, strings and every type without a fixed one.
 */
static size_t field_type_width(ustore_docs_field_type_t type) {
    switch (type) {
    case ustore_docs_field_type_bool_k: return 1;
    case ustore_docs_field_type_uuid_k: return uuid_bytes_k;
    case ustore_docs_field_type_f64_k: return 8;
    case ustore_docs_field_type_f32_k: return 4;
    case ustore_docs_field_type_f16_k: return 2;
    case ustore_docs_field_type_i64_k: return 8;
    case ustore_docs_field_type_i32_k: return 4;
    case ustore_docs_field_type_i16_k: return 2;
    case ustore_docs_field_type_i8_k: return 1;
    case ustore_docs_field_type_u64_k: return 8;
    case ustore_docs_field_type_u32_k: return 4;
    case ustore_docs_field_type_u16_k: return 2;
    case ustore_docs_field_type_u8_k: return 1;
    default: return 0;
    }
}

/** Whether a gather has a column layout for @p type: a fixed-width scalar, a UUID, a string or bytes. */
static int field_type_gathers(ustore_docs_field_type_t type) {
    return field_type_width(type) != 0 || type == ustore_docs_field_type_str_k || type == ustore_docs_field_type_bin_k;
}

/**
 *  @brief Reads an optional string keyword, caching the UTF-8 bytes a call addresses.
 *  @param[in] object The keyword's value, a string or @c None.
 *  @param[in] field The keyword to name in a failure.
 *  @param[out] text The string, borrowed, or NULL for @c None.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
static int optional_text_from_object(PyObject* object, char const* field, PyObject** text) {
    *text = NULL;
    if (object == Py_None) return 0;
    if (!PyUnicode_Check(object)) {
        PyErr_Format(PyExc_TypeError, "The '%s' option must be a string or None, got %S", field, Py_TYPE(object));
        return -1;
    }
    if (!PyUnicode_AsUTF8(object)) return -1;
    *text = object;
    return 0;
}

/** The UTF-8 bytes @p text cached when its keyword was read, or a zeroed view for no string. */
static ustore_bytes_view_t cached_bytes(PyObject* text) {
    ustore_bytes_view_t bytes = {0};
    Py_ssize_t length = 0;
    if (text) bytes.data = (uint8_t const*)PyUnicode_AsUTF8AndSize(text, &length);
    bytes.length = (ustore_size_t)length;
    return bytes;
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
 *  A tape of JSON text passes as it is. A string travels as the UTF-8 bytes it caches, so the batch
 *  holds the one document or a tuple of every document until the call finishes, which no concurrent
 *  change to the caller's list can release. A document given as a Python object is serialized into
 *  memory the batch owns until it is released.
 */
typedef struct documents_t {

    /** The views the ABI reads, one per task or one for every task, or the tape it reads. */
    ustore_py_contents_t contents;

    /** The one document or a tuple of every document, keeping every string alive, or NULL for a column. */
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
    batch->views[batch->count].data = (uint8_t const*)text;
    batch->views[batch->count].length = (ustore_size_t)length;
    ++batch->count;
    return 0;
}

/** Borrows one document: a string as its UTF-8 bytes, a buffer as it is, and any other object but None
 *  serialized when the documents are JSON, since a removal takes a verb of its own. */
static int document_borrow(PyObject* object, ustore_docs_field_type_t type, documents_t* documents) {
    ustore_py_contents_t* const batch = &documents->contents;
    if (!PyUnicode_Check(object)) {
        if (object == Py_None || type != ustore_docs_field_type_json_k || PyObject_CheckBuffer(object))
            return ustore_py_contents_borrow(object, batch);
        return document_serialize(object, documents);
    }
    Py_ssize_t length = 0;
    char const* const text = PyUnicode_AsUTF8AndSize(object, &length);
    if (!text) return -1;
    batch->views[batch->count].data = (uint8_t const*)text;
    batch->views[batch->count].length = (ustore_size_t)length;
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
static int scalars_from_object(PyObject* object, ustore_docs_field_type_t type, size_t tasks_count,
                               documents_t* documents) {
    memset(documents, 0, sizeof(*documents));
    size_t const width = field_type_width(type);
    Py_buffer buffer;
    if (PyObject_GetBuffer(object, &buffer, PyBUF_C_CONTIGUOUS | PyBUF_FORMAT) != 0)
        return ustore_py_raise_from_cause(PyExc_TypeError, "A column of %s must export a contiguous buffer, not %S",
                                          ustore_docs_field_type_name(type), Py_TYPE(object));

    // A buffer of bytes carries no width of its own, so only a typed one is held to the field's.
    if (buffer.itemsize != 1 && (size_t)buffer.itemsize != width) {
        ustore_py_raise(ustore_status_invalid_argument_k, "A column of %s holds %zu-byte items, got %zd-byte ones",
                        ustore_docs_field_type_name(type), width, buffer.itemsize);
        PyBuffer_Release(&buffer);
        return -1;
    }

    size_t const values_count = (size_t)buffer.len == width ? 1 : tasks_count;
    if ((size_t)buffer.len != values_count * width) {
        ustore_py_raise(ustore_status_invalid_argument_k,
                        "A column of %s spans %zd bytes, not the %zu that %zu tasks take",
                        ustore_docs_field_type_name(type), buffer.len, tasks_count * width, tasks_count);
        PyBuffer_Release(&buffer);
        return -1;
    }

    ustore_py_contents_spread_t const spread = values_count == 1 ? ustore_py_contents_broadcast_k
                                                                 : ustore_py_contents_per_task_k;
    if (ustore_py_contents_reserve(&documents->contents, values_count, spread) != 0) {
        PyBuffer_Release(&buffer);
        return -1;
    }
    // One buffer backs every task, so the batch borrows once and releases once.
    documents->contents.buffers[0] = buffer;
    documents->contents.borrowed = 1;
    documents->contents.count = values_count;
    for (size_t task_index = 0; task_index != values_count; ++task_index) {
        documents->contents.views[task_index].data = (uint8_t const*)buffer.buf + task_index * width;
        documents->contents.views[task_index].length = (ustore_size_t)width;
    }
    return 0;
}

/**
 *  @brief Gathers the documents of a write: a tape or a sequence of them, or one document for every key.
 *  @param[in] object The documents, each a string, a bytes-like object, or a dict or list to serialize.
 *  @param[in] type How every document is spelled, where only JSON serializes Python objects.
 *  @param[out] documents The documents and whether one of them repeats for every key.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
static int documents_from_object(PyObject* object, ustore_docs_field_type_t type, documents_t* documents) {
    memset(documents, 0, sizeof(*documents));
    int const tape = ustore_py_contents_from_tape(object, &documents->contents);
    if (tape != 0) return tape < 0 ? -1 : 0;

    // A string, a bytes-like object or a dict is one document for every key, not a sequence of characters or keys.
    if (PyUnicode_Check(object) || PyObject_CheckBuffer(object) || PyDict_Check(object)) {
        if (ustore_py_contents_reserve(&documents->contents, 1, ustore_py_contents_broadcast_k) != 0) return -1;
        documents->held = Py_NewRef(object);
        if (document_borrow(object, type, documents) != 0) {
            documents_release(documents);
            return -1;
        }
        return 0;
    }

    documents->held = PySequence_Tuple(object);
    if (!documents->held)
        return ustore_py_raise_from_cause(PyExc_TypeError, "Expected a sequence of documents or one document, not %S",
                                          Py_TYPE(object));
    size_t const count = (size_t)PyTuple_GET_SIZE(documents->held);
    if (ustore_py_contents_reserve(&documents->contents, count, ustore_py_contents_per_task_k) != 0) {
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
    if (ustore_py_contents_reserve(&documents->contents, count, ustore_py_contents_per_task_k) != 0) {
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

/** A write of documents or of one field inside each, which its caller gathers in place. */
typedef struct docs_write_call_t {

    /** What the call keeps until it finishes. */
    ustore_py_call_t call;

    /** The keys to write, empty when every key is read out of its own document. */
    ustore_py_keys_t keys;

    /** The borrowed documents, one per key or one for every key. */
    documents_t documents;

    /** The top-level key or JSON Pointer written inside every document, held, or NULL for whole documents. */
    PyObject* field;

    /** The UTF-8 bytes @c field caches, which the accessor repeats for every document. */
    ustore_bytes_view_t field_view;

    /** The field holding every key of a write given no keys, held, or NULL. */
    PyObject* id_field;
} docs_write_call_t;

static void docs_write_submit(void* arguments) { ustore_docs_write((ustore_docs_write_t*)arguments); }

static void docs_write_release(void* erased) {
    docs_write_call_t* const call = (docs_write_call_t*)erased;
    documents_release(&call->documents);
    ustore_py_keys_release(&call->keys);
    Py_CLEAR(call->field);
    Py_CLEAR(call->id_field);
    ustore_py_call_release(&call->call);
}

static ustore_py_verb_t const docs_write_verb = {sizeof(docs_write_call_t), docs_write_submit,
                                                 ustore_py_call_answer_none, docs_write_release};

/** Whether @p documents spell one per key of a batch of @p count, raising otherwise; one for every key always does. */
static int documents_match(documents_t const* documents, size_t count) {
    if (documents->contents.spread == ustore_py_contents_broadcast_k || documents->contents.count == count) return 0;
    return ustore_py_raise(ustore_status_invalid_argument_k, "Got %zu documents for %zu keys",
                           documents->contents.count, count);
}

/**
 *  @brief Writes the documents gathered into @p call under @p keys, with the transaction of @p context.
 *  @param[in] keys The keys of the @p count tasks, or NULL for keys read out of the documents.
 *  @return None, or the @c Deferred of it, or NULL with an exception set.
 */
static PyObject* documents_write(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                                 ustore_key_t const* keys, size_t count, ustore_docs_field_type_t type,
                                 ustore_docs_modification_t modification, docs_write_call_t* call) {
    call->call.collection = options->collection;
    call->field_view = cached_bytes(call->field);
    ustore_docs_write_t write = {
        .database = context->database_handle,
        .executor = ustore_py_context_executor(context),
        .outcome = &call->call.outcome,
        .completion = context->completion,
        .transaction = context->transaction,
        .count = count,
        .collections = {&call->call.collection, 0},
        .keys = {keys, sizeof(ustore_key_t)},
        .id_field = cached_bytes(call->id_field),
        .values = call->documents.contents.view,
        .type = type,
        .modification = modification,
        .durability = options->durability,
        .watch = context->watch,
    };
    if (call->field) write.fields = ustore_py_strings_repeated(&call->field_view);
    return ustore_py_call_run(context, &docs_write_verb, call, &write);
}

int ustore_py_docs_upsert(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                          ustore_key_t const* keys, size_t count, PyObject* objects,
                          ustore_py_documents_spelling_t spelling) {
    // An assignment blocks until the write finishes, so the keys stay the caller's
    docs_write_call_t call;
    ustore_py_call_open(context, &docs_write_verb, &call);
    int const gathered = spelling == ustore_py_documents_as_objects_k
                             ? documents_from_objects(objects, &call.documents)
                             : documents_from_object(objects, ustore_docs_field_type_json_k, &call.documents);
    if (gathered != 0 || documents_match(&call.documents, count) != 0) {
        ustore_py_call_discard(context, &docs_write_verb, &call);
        return -1;
    }
    PyObject* const written = documents_write(context, options, keys, count, ustore_docs_field_type_json_k,
                                              ustore_docs_modify_upsert_k, &call);
    Py_XDECREF(written);
    return written ? 0 : -1;
}

char const doc_docs_write[] =                                                                         //
    "docs_write(keys, documents, *, collection=None, field=None, modification=Modification.UPSERT,\n" //
    "           type=FieldType.JSON, id_field=None, durability=Durability.BUFFERED) answers None.\n"  //
    "\n"                                                                                              //
    "Writes a batch of JSON documents, or one field inside each of them. Every document is\n"         //
    "borrowed for the length of the call rather than copied.\n"                                       //
    "\n"                                                                                              //
    "Args:\n"                                                                                         //
    "  keys (Sequence[KeyLike] | buffer | None): The keys to write; None reads every key from\n"      //
    "    `id_field` of its document.\n"                                                               //
    "  documents (Sequence[str | bytes-like | object] | str | bytes-like | dict | tape): One\n"       //
    "    document per key, a tape of them, or one document broadcast to every key. Text,\n"           //
    "    bytes and a tape, as `write` takes one, are JSON already, while a dict, a list or a\n"       //
    "    number inside a sequence is serialized. None is refused: `remove` removes whole\n"           //
    "    documents, and a merge with a JSON null removes a field. Under a fixed-width `type`,\n"      //
    "    a contiguous buffer of one scalar per key is a column, which reaches the store\n"            //
    "    without being printed as JSON and parsed back.\n"                                            //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n"                //
    "  field (str, optional): A top-level key or a JSON Pointer such as '/user/name' to\n"            //
    "    write inside every document; None writes whole documents.\n"                                 //
    "  modification (Modification, optional): An upsert, update or insert, an RFC 6902\n"             //
    "    `Modification.PATCH` or an RFC 7386 `Modification.MERGE`.\n"                                 //
    "  type (FieldType, optional): How every document is spelled: `FieldType.JSON`,\n"                //
    "    `FieldType.STR` for a plain string, `FieldType.NULL`, or the raw bytes of\n"                 //
    "    `FieldType.BOOL`, `FieldType.F64`, `FieldType.F32`, `FieldType.I64` through\n"               //
    "    `FieldType.U8`.\n"                                                                           //
    "  id_field (str, optional): The field holding every key when `keys` is None.\n"                  //
    "  durability (Durability, optional): `Durability.FLUSHED` reaches the drive before\n"            //
    "    returning.\n"                                                                                //
    "Raises:\n"                                                                                       //
    "  TypeError: A document is None.\n"                                                              //
    "  MalformedError: A document does not parse, or its `id_field` holds no integer.\n"              //
    "  NotFoundError: A field is written inside a document that does not exist.\n"                    //
    "  AlreadyExistsError: An insert names a document that already exists.";

PyObject* ustore_py_api_docs_write(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                   PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0 || ustore_py_context_writable(&context) != 0) return NULL;
    Py_ssize_t const args_names_count = ustore_py_args_names_count_checked("docs_write", positional_args_count,
                                                                           args_names_tuple, 2);
    if (args_names_count < 0) return NULL;

    ustore_py_verb_options_t options;
    ustore_py_verb_options_init(&options);
    int modification = ustore_docs_modify_upsert_k;
    int type = ustore_docs_field_type_json_k;
    PyObject* field = NULL;
    PyObject* id_field = NULL;
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
            parsed = ustore_py_enum_from_object(value, "modification", ustore_py_enum_modification_k, &modification);
        else if (PyUnicode_CompareWithASCIIString(name, "type") == 0)
            parsed = ustore_py_enum_from_object(value, "type", ustore_py_enum_field_type_k, &type);
        else {
            PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
            return NULL;
        }
        if (parsed != 0) return NULL;
    }

    docs_write_call_t stack;
    docs_write_call_t* const call = ustore_py_call_open(&context, &docs_write_verb, &stack);
    if (!call) return NULL;
    call->field = Py_XNewRef(field);
    call->id_field = Py_XNewRef(id_field);

    // A fixed-width type given a buffer is a column of scalars, which splits in place per key.
    int const columnar = field_type_width((ustore_docs_field_type_t)type) != 0 && args[0] != Py_None &&
                         !PyUnicode_Check(args[1]) && PyObject_CheckBuffer(args[1]);
    if (args[0] != Py_None && ustore_py_keys_from_object(args[0], &call->keys) != 0)
        return ustore_py_call_discard(&context, &docs_write_verb, call);
    int const gathered = columnar ? scalars_from_object(args[1], (ustore_docs_field_type_t)type, call->keys.count,
                                                        &call->documents)
                                  : documents_from_object(args[1], (ustore_docs_field_type_t)type, &call->documents);
    if (gathered != 0) return ustore_py_call_discard(&context, &docs_write_verb, call);

    size_t count = call->keys.count;
    if (args[0] == Py_None) {
        // Every key is read out of its own document, so each task needs a document of its own.
        if (!id_field || call->documents.contents.spread == ustore_py_contents_broadcast_k) {
            PyErr_SetString(PyExc_TypeError, "Documents without keys need a sequence of them and an 'id_field'");
            return ustore_py_call_discard(&context, &docs_write_verb, call);
        }
        count = call->documents.contents.count;
    }
    else if (documents_match(&call->documents, count) != 0)
        return ustore_py_call_discard(&context, &docs_write_verb, call);
    return documents_write(&context, &options, call->keys.keys, count, (ustore_docs_field_type_t)type,
                           (ustore_docs_modification_t)modification, call);
}

char const doc_docs_remove[] =                                                                  //
    "docs_remove(keys, *, collection=None, durability=Durability.BUFFERED) answers None.\n"     //
    "\n"                                                                                        //
    "Removes a batch of whole documents, or one given on its own; a patch or a merge removes\n" //
    "fields inside them instead.\n"                                                             //
    "\n"                                                                                        //
    "Args:\n"                                                                                   //
    "  keys (KeyLike | Sequence[KeyLike] | buffer): The keys of the documents to remove.\n"     //
    "  collection (UUID | str, optional): The docs collection, by identifier or by name.\n"     //
    "  durability (Durability, optional): `Durability.FLUSHED` reaches the drive before\n"      //
    "    returning.";

PyObject* ustore_py_api_docs_remove(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                    PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0 || ustore_py_context_writable(&context) != 0) return NULL;
    if (ustore_py_args_names_count_checked("docs_remove", positional_args_count, args_names_tuple, 1) < 0) return NULL;

    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, args, positional_args_count, args_names_tuple, &options) != 0)
        return NULL;

    ustore_py_remove_call_t stack;
    ustore_py_remove_call_t* const call = ustore_py_call_open(&context, &ustore_py_remove_verb, &stack);
    if (!call) return NULL;
    if (ustore_py_keys_from_subscript(args[0], &call->keys) < 0)
        return ustore_py_call_discard(&context, &ustore_py_remove_verb, call);
    return ustore_py_values_remove(&context, &options, ustore_modality_docs_k, call);
}

#pragma endregion Writes

#pragma region Reads

char const doc_docs_read[] =                                                                   //
    "docs_read(keys, arena, *, collection=None, field=None, outputs_type=FieldType.JSON)\n"    //
    "answers a Values tape.\n"                                                                 //
    "\n"                                                                                       //
    "Reads whole documents, or one field of each, as JSON text or as printed strings. Whole\n" //
    "documents read as JSON pass straight through from the engine, so nothing is parsed.\n"    //
    "\n"                                                                                       //
    "Args:\n"                                                                                  //
    "  keys (Sequence[KeyLike] | buffer): The keys to read.\n"                                 //
    "  arena (Arena): Where the tape lands, rewound by this call.\n"                           //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n"         //
    "  field (str, optional): A top-level key or a JSON Pointer to read inside every\n"        //
    "    document; None reads whole documents.\n"                                              //
    "  outputs_type (FieldType, optional): `FieldType.JSON` for JSON text, or\n"               //
    "    `FieldType.STR` to print scalars as text.\n"                                          //
    "Returns:\n"                                                                               //
    "  Values: One entry per key, None where the document or the field is absent.";

/** Parses the JSON of task @p task_index of @p call into Python objects, or answers None where it is missing. */
static PyObject* document_parse(ustore_py_read_call_t const* call, size_t task_index) {
    if (!ustore_py_present(call->presences, task_index)) Py_RETURN_NONE;
    ustore_bytes_view_t const text = ustore_py_tape_string(&call->values, task_index);
    yyjson_read_flag const flags = YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_INF_AND_NAN |
                                   YYJSON_READ_BIGNUM_AS_RAW;
    yyjson_read_err error = {0};
    // yyjson writes into its input only with `YYJSON_READ_INSITU`, which is never set.
    yyjson_doc* const document = yyjson_read_opts((char*)text.data, (size_t)text.length, flags, NULL, &error);
    if (!document) {
        ustore_py_raise(ustore_status_malformed_k, "A stored document is not JSON: %s", error.msg);
        return NULL;
    }
    PyObject* const object = ustore_py_object_from_json(yyjson_doc_get_root(document));
    yyjson_doc_free(document);
    return object;
}

/** Parses every document of @p erased: the only one a subscript named, raising KeyError where it is absent, or a
 *  list of them with None where one is absent. */
static PyObject* docs_read_answer_objects(void* erased) {
    ustore_py_read_call_t const* const call = (ustore_py_read_call_t const*)erased;
    if (call->subscript && !ustore_py_present(call->presences, 0)) {
        PyErr_SetObject(PyExc_KeyError, call->subscript);
        return NULL;
    }
    if (call->subscript) return document_parse(call, 0);
    PyObject* documents = PyList_New((Py_ssize_t)call->keys.count);
    for (size_t task_index = 0; documents && task_index != call->keys.count; ++task_index) {
        PyObject* const document = document_parse(call, task_index);
        if (!document) Py_CLEAR(documents);
        else PyList_SET_ITEM(documents, (Py_ssize_t)task_index, document);
    }
    return documents;
}

static void docs_read_submit(void* arguments) { ustore_docs_read((ustore_docs_read_t*)arguments); }

ustore_py_verb_t const ustore_py_docs_read_verb = {sizeof(ustore_py_read_call_t), docs_read_submit,
                                                   ustore_py_read_answer_values, ustore_py_read_release};

ustore_py_verb_t const ustore_py_docs_read_copies_verb = {sizeof(ustore_py_read_call_t), docs_read_submit,
                                                          ustore_py_read_answer_copies, ustore_py_read_release};

ustore_py_verb_t const ustore_py_docs_read_objects_verb = {sizeof(ustore_py_read_call_t), docs_read_submit,
                                                           docs_read_answer_objects, ustore_py_read_release};

PyObject* ustore_py_docs_read(ustore_py_context_t const* context, ustore_py_verb_options_t const* options,
                              ustore_docs_field_type_t type, ustore_py_verb_t const* verb, ustore_py_read_call_t* call,
                              PyObject* arena) {
    if (ustore_py_arena_claim(arena, &call->call.arena) != 0) return ustore_py_call_discard(context, verb, call);
    call->call.collection = options->collection;
    call->field_view = cached_bytes(call->field);
    ustore_docs_read_t read = {
        .database = context->database_handle,
        .executor = ustore_py_context_executor(context),
        .outcome = &call->call.outcome,
        .completion = context->completion,
        .view = context->view,
        .count = call->keys.count,
        .collections = {&call->call.collection, 0},
        .keys = {call->keys.keys, sizeof(ustore_key_t)},
        .presences = &call->presences,
        .values = &call->values,
        .arena = ustore_py_arena_handle(call->call.arena),
        .outputs_type = type,
    };
    if (call->field) read.fields = ustore_py_strings_repeated(&call->field_view);
    return ustore_py_call_run(context, verb, call, &read);
}

PyObject* ustore_py_api_docs_read(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                  PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    Py_ssize_t const args_names_count = ustore_py_args_names_count_checked("docs_read", positional_args_count,
                                                                           args_names_tuple, 2);
    if (args_names_count < 0) return NULL;

    ustore_py_verb_options_t options;
    ustore_py_verb_options_init(&options);
    int type = ustore_docs_field_type_json_k;
    PyObject* field = NULL;
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
        else if (PyUnicode_CompareWithASCIIString(name, "outputs_type") == 0)
            parsed = ustore_py_enum_from_object(value, "outputs_type", ustore_py_enum_field_type_k, &type);
        else {
            PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
            return NULL;
        }
        if (parsed != 0) return NULL;
    }

    ustore_py_read_call_t stack;
    ustore_py_read_call_t* const call = ustore_py_call_open(&context, &ustore_py_docs_read_verb, &stack);
    if (!call) return NULL;
    call->field = Py_XNewRef(field);
    if (ustore_py_keys_from_object(args[0], &call->keys) != 0)
        return ustore_py_call_discard(&context, &ustore_py_docs_read_verb, call);
    return ustore_py_docs_read(&context, &options, (ustore_docs_field_type_t)type, &ustore_py_docs_read_verb, call,
                               args[1]);
}

char const doc_docs_gist[] =                                                                  //
    "docs_gist(keys, arena, *, collection=None) answers a tuple of JSON Pointers.\n"          //
    "\n"                                                                                      //
    "Lists every distinct path to a leaf across the requested documents, in the order they\n" //
    "were first met, which is how a table learns the columns its documents hold.\n"           //
    "\n"                                                                                      //
    "Args:\n"                                                                                 //
    "  keys (Sequence[KeyLike] | buffer): The documents to inspect.\n"                        //
    "  arena (Arena): Where the paths land before they are copied out, rewound by this\n"     //
    "    call.\n"                                                                             //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n"        //
    "Returns:\n"                                                                              //
    "  tuple[str, ...]: One JSON Pointer per leaf, such as '/user/name'.";

/** A gist of a batch of documents, whose paths land in the arena its header holds. */
typedef struct gist_call_t {

    /** What the call keeps until it finishes. */
    ustore_py_call_t call;

    /** The documents to inspect. */
    ustore_py_keys_t keys;

    /** Output slot for the tape of paths, inside the arena. */
    ustore_strings_view_t fields;

    /** Distinct paths the documents hold. */
    ustore_size_t written;
} gist_call_t;

static void gist_submit(void* arguments) { ustore_docs_gist((ustore_docs_gist_t*)arguments); }

// Every path is a `str`, which owns its bytes, so nothing of the arena outlives the answer.
static PyObject* gist_answer(void* erased) {
    gist_call_t const* const call = (gist_call_t const*)erased;
    PyObject* const listed = PyTuple_New((Py_ssize_t)call->written);
    for (ustore_size_t field_index = 0; listed && field_index != call->written; ++field_index) {
        ustore_bytes_view_t const field = ustore_py_tape_string(&call->fields, field_index);
        PyObject* const path = PyUnicode_DecodeUTF8((char const*)field.data, (Py_ssize_t)field.length, "strict");
        if (!path) {
            Py_DECREF(listed);
            return NULL;
        }
        PyTuple_SET_ITEM(listed, (Py_ssize_t)field_index, path);
    }
    return listed;
}

static void gist_release(void* erased) {
    gist_call_t* const call = (gist_call_t*)erased;
    ustore_py_keys_release(&call->keys);
    ustore_py_call_release(&call->call);
}

static ustore_py_verb_t const gist_verb = {sizeof(gist_call_t), gist_submit, gist_answer, gist_release};

PyObject* ustore_py_api_docs_gist(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                  PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_args_names_count_checked("docs_gist", positional_args_count, args_names_tuple, 2) < 0) return NULL;

    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, args, positional_args_count, args_names_tuple, &options) != 0)
        return NULL;

    gist_call_t stack;
    gist_call_t* const call = ustore_py_call_open(&context, &gist_verb, &stack);
    if (!call) return NULL;
    if (ustore_py_keys_from_object(args[0], &call->keys) != 0 || ustore_py_arena_claim(args[1], &call->call.arena) != 0)
        return ustore_py_call_discard(&context, &gist_verb, call);
    call->call.collection = options.collection;
    ustore_docs_gist_t gist = {
        .database = context.database_handle,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->call.outcome,
        .completion = context.completion,
        .view = context.view,
        .count = call->keys.count,
        .collections = {&call->call.collection, 0},
        .keys = {call->keys.keys, sizeof(ustore_key_t)},
        .fields = &call->fields,
        .written = &call->written,
        .arena = ustore_py_arena_handle(call->call.arena),
    };
    return ustore_py_call_run(&context, &gist_verb, call, &gist);
}

char const doc_docs_scan[] =                                                                      //
    "docs_scan(start, wanted, arena, *, collection=None, mode=ScanMode.ORDERED, progress=None)\n" //
    "answers a Keys tape.\n"                                                                      //
    "\n"                                                                                          //
    "Lists up to `wanted` document keys from `start` upwards, as `scan` lists the keys of\n"      //
    "blobs. Documents are not exported, so a `docs_read` follows the scan.\n"                     //
    "\n"                                                                                          //
    "Args:\n"                                                                                     //
    "  start (KeyLike): The smallest key the scan may return.\n"                                  //
    "  wanted (int): Most keys the scan returns.\n"                                               //
    "  arena (Arena): Where the keys land, rewound by this call.\n"                               //
    "  collection (UUID | str, optional): The docs collection, by identifier or by name.\n"       //
    "  mode (ScanMode, optional): In what order the keys come, as `scan` takes it.\n"             //
    "  progress (Callable[[int, int], bool | None], optional): Called with the scans\n"           //
    "    finished and the scans in all; a falsy answer other than None cancels the call.\n"       //
    "Returns:\n"                                                                                  //
    "  Keys: One run of keys, in ascending order.\n"                                              //
    "Raises:\n"                                                                                   //
    "  CancelledError: The progress callable stopped the scan.";

PyObject* ustore_py_api_docs_scan(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                  PyObject* args_names_tuple) {
    return ustore_py_api_scan_of(self, args, positional_args_count, args_names_tuple, "docs_scan",
                                 ustore_modality_docs_k);
}

#pragma endregion Reads

#pragma region Gathers

/**
 *  @brief A new @c Columns over the columns a gather wrote into @p arena, which its call still claims.
 *  @param[in] types The type of every field, copied into the result.
 *  @return A new reference, or NULL with an exception set.
 */
static PyObject* columns_new(PyObject* arena, ustore_docs_column_view_t const* columns,
                             ustore_docs_field_type_t const* types, size_t fields_count, size_t docs_count) {
    Columns* const instance = PyObject_NewVar(Columns, &ColumnsType, (Py_ssize_t)fields_count);
    if (!instance) return NULL;
    instance->arena = Py_NewRef(arena);
    instance->generation = ustore_py_arena_generation(arena);
    instance->columns = columns;
    instance->fields_count = fields_count;
    instance->docs_count = docs_count;
    if (fields_count) memcpy(instance->types, types, fields_count * sizeof(ustore_docs_field_type_t));
    return (PyObject*)instance;
}

char const doc_docs_gather[] =                                                                //
    "docs_gather(keys, fields, outputs_types, arena, *, collection=None) answers Columns.\n"  //
    "\n"                                                                                      //
    "Reads every field of every document into typed columns, converting each value on a\n"    //
    "best-effort basis and recording what was found, converted, or could not be.\n"           //
    "\n"                                                                                      //
    "Args:\n"                                                                                 //
    "  keys (Sequence[KeyLike] | buffer): The documents, the rows of every column.\n"         //
    "  fields (Sequence[str]): Top-level keys or JSON Pointers, the columns of the result.\n" //
    "  outputs_types (FieldType | Sequence[FieldType]): One type per field, or one for\n"     //
    "    every field: a fixed-width scalar from `FieldType.BOOL` to `FieldType.U8`,\n"        //
    "    `FieldType.UUID`, `FieldType.STR` or `FieldType.BIN`. A `FieldType.F16` column\n"    //
    "    exports as a half-precision view NumPy reads as float16, and a `FieldType.UUID`\n"   //
    "    column as sixteen bytes per document.\n"                                             //
    "  arena (Arena): Where the columns land, rewound by this call.\n"                        //
    "  collection (UUID | str, optional): The collection, by identifier or by name.\n"        //
    "Returns:\n"                                                                              //
    "  Columns: One column per field, each holding one entry per document.";

/** A gather of typed columns, written into the arena its header holds. */
typedef struct gather_call_t {

    /** What the call keeps until it finishes. */
    ustore_py_call_t call;

    /** The documents, the rows of every column. */
    ustore_py_keys_t keys;

    /** A tuple of the field names, keeping alive the UTF-8 bytes @c field_views point into. */
    PyObject* fields_held;

    /** One view per field of the bytes its name caches, which the accessor reads. */
    ustore_bytes_view_t* field_views;

    /** The type every field converts into, which the answer copies. */
    ustore_docs_field_type_t* types;

    /** Fields the gather carries. */
    size_t fields_count;

    /** Output slot for one column view per field, inside the arena. */
    ustore_docs_column_view_t const* columns;
} gather_call_t;

static void gather_submit(void* arguments) { ustore_docs_gather((ustore_docs_gather_t*)arguments); }

static PyObject* gather_answer(void* erased) {
    gather_call_t const* const call = (gather_call_t const*)erased;
    return columns_new(call->call.arena, call->columns, call->types, call->fields_count, call->keys.count);
}

static void gather_release(void* erased) {
    gather_call_t* const call = (gather_call_t*)erased;
    ustore_py_keys_release(&call->keys);
    PyMem_Free(call->field_views);
    PyMem_Free(call->types);
    call->field_views = NULL;
    call->types = NULL;
    Py_CLEAR(call->fields_held);
    ustore_py_call_release(&call->call);
}

static ustore_py_verb_t const gather_verb = {sizeof(gather_call_t), gather_submit, gather_answer, gather_release};

PyObject* ustore_py_api_docs_gather(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                    PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    if (ustore_py_args_names_count_checked("docs_gather", positional_args_count, args_names_tuple, 4) < 0) return NULL;

    ustore_py_verb_options_t options;
    if (ustore_py_verb_options_from_keywords(&context, args, positional_args_count, args_names_tuple, &options) != 0)
        return NULL;

    if (PyUnicode_Check(args[1])) {
        PyErr_SetString(PyExc_TypeError, "The fields are a sequence of strings, not one string");
        return NULL;
    }
    gather_call_t stack;
    gather_call_t* const call = ustore_py_call_open(&context, &gather_verb, &stack);
    if (!call) return NULL;
    // The tuple keeps every field name alive while the call reads the UTF-8 bytes they cache.
    call->fields_held = PySequence_Tuple(args[1]);
    if (!call->fields_held) return ustore_py_call_discard(&context, &gather_verb, call);
    size_t const fields_count = (size_t)PyTuple_GET_SIZE(call->fields_held);

    // One `FieldType` is an int, and stands for every field
    int const broadcast = PyLong_Check(args[2]);
    PyObject* const types_held = broadcast ? NULL : PySequence_Tuple(args[2]);
    if (!broadcast && !types_held) return ustore_py_call_discard(&context, &gather_verb, call);
    if (types_held && (size_t)PyTuple_GET_SIZE(types_held) != fields_count) {
        ustore_py_raise(ustore_status_invalid_argument_k, "Got %zd outputs types for %zu fields",
                        PyTuple_GET_SIZE(types_held), fields_count);
        Py_DECREF(types_held);
        return ustore_py_call_discard(&context, &gather_verb, call);
    }

    call->fields_count = fields_count;
    call->field_views = (ustore_bytes_view_t*)PyMem_Malloc((fields_count + 1) * sizeof(ustore_bytes_view_t));
    call->types = (ustore_docs_field_type_t*)PyMem_Malloc((fields_count + 1) * sizeof(ustore_docs_field_type_t));
    int failed = !call->field_views || !call->types;
    if (failed) PyErr_NoMemory();

    for (size_t field_index = 0; !failed && field_index != fields_count; ++field_index) {
        PyObject* const field = PyTuple_GET_ITEM(call->fields_held, (Py_ssize_t)field_index);
        PyObject* const type_object = types_held ? PyTuple_GET_ITEM(types_held, (Py_ssize_t)field_index) : args[2];
        if (!PyUnicode_Check(field)) {
            PyErr_Format(PyExc_TypeError, "A field is a string, got %S", Py_TYPE(field));
            failed = 1;
            break;
        }
        Py_ssize_t length = 0;
        char const* const text = PyUnicode_AsUTF8AndSize(field, &length);
        call->field_views[field_index].data = (uint8_t const*)text;
        call->field_views[field_index].length = (ustore_size_t)length;
        int type = ustore_docs_field_type_json_k;
        failed = !text ||
                 ustore_py_enum_from_object(type_object, "outputs_types", ustore_py_enum_field_type_k, &type) != 0;
        if (failed) break;
        if (!field_type_gathers((ustore_docs_field_type_t)type)) {
            ustore_py_raise(ustore_status_invalid_argument_k,
                            "A gather converts into scalars, UUIDs, strings or bytes, not %s",
                            ustore_docs_field_type_name((ustore_docs_field_type_t)type));
            failed = 1;
            break;
        }
        call->types[field_index] = (ustore_docs_field_type_t)type;
    }
    Py_XDECREF(types_held);

    if (failed || ustore_py_keys_from_object(args[0], &call->keys) != 0 ||
        ustore_py_arena_claim(args[3], &call->call.arena) != 0)
        return ustore_py_call_discard(&context, &gather_verb, call);
    call->call.collection = options.collection;
    ustore_docs_gather_t gather = {
        .database = context.database_handle,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->call.outcome,
        .completion = context.completion,
        .view = context.view,
        .count = call->keys.count,
        .collections = {&call->call.collection, 0},
        .keys = {call->keys.keys, sizeof(ustore_key_t)},
        .fields_count = fields_count,
        .fields = ustore_py_strings_each(call->field_views),
        .columns = &call->columns,
        .arena = ustore_py_arena_handle(call->call.arena),
        .outputs_types = call->types,
    };
    return ustore_py_call_run(&context, &gather_verb, call, &gather);
}

#pragma endregion Gathers

#pragma region Columns

static char const doc_Columns[] =                                                              //
    "The typed columns one gather wrote into an `Arena`, viewing it until it rewinds.\n"       //
    "\n"                                                                                       //
    "Indexing a scalar column answers a `memoryview` of its type over the stored scalars, a\n" //
    "`FieldType.UUID` column a two-dimensional view of sixteen bytes per document, and a\n"    //
    "string column a tuple of `str` or `bytes`, with None where the value is not valid. A\n"   //
    "`FieldType.F16` column carries the 'e' format, which NumPy reads as float16 without a\n"  //
    "copy.\n"                                                                                  //
    "\n"                                                                                       //
    "The bitmaps address documents least significant bit first, as Apache Arrow does. Every\n" //
    "view pins the arena against the next call until it is released, and a read after a\n"     //
    "later call rewound the arena raises `UninitializedError`.";

static void Columns_dealloc(Columns* self) {
    Py_XDECREF(self->arena);
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

/** The column a gather of no documents reports, which writes no columns at all. */
static ustore_docs_column_view_t const column_empty_k = {0};

/** The column @p field_index of @p self, which a gather with nothing to report left without a view. */
static ustore_docs_column_view_t const* columns_column(Columns const* self, size_t field_index) {
    return self->columns ? &self->columns[field_index] : &column_empty_k;
}

/** Copies the strings of the column @p column of @p type into a tuple, with None where a row is not valid. */
static PyObject* column_strings(ustore_docs_column_view_t const* column, ustore_docs_field_type_t type, size_t rows) {
    // A string column spans the null terminator of every row, which a binary one does not write.
    ustore_size_t const terminator = type == ustore_docs_field_type_str_k ? 1 : 0;
    PyObject* strings = PyTuple_New((Py_ssize_t)rows);
    for (size_t row = 0; strings && row != rows; ++row) {
        PyObject* entry = NULL;
        if (!ustore_py_present(column->validities, row)) entry = Py_NewRef(Py_None);
        else {
            ustore_bytes_view_t const string = ustore_py_tape_string(&column->strings, row);
            Py_ssize_t const length = (Py_ssize_t)(string.length - terminator);
            // Nothing but empty strings leaves the strings without an address.
            char const* const text = length ? (char const*)string.data : "";
            entry = type == ustore_docs_field_type_str_k ? PyUnicode_DecodeUTF8(text, length, "strict")
                                                         : PyBytes_FromStringAndSize(text, length);
        }
        if (!entry) Py_CLEAR(strings);
        else PyTuple_SET_ITEM(strings, (Py_ssize_t)row, entry);
    }
    return strings;
}

static PyObject* Columns_item(Columns* self, Py_ssize_t field_index) {
    if (field_index < 0 || (size_t)field_index >= self->fields_count) {
        PyErr_SetString(PyExc_IndexError, "Field index out of range");
        return NULL;
    }
    if (ustore_py_arena_pin(self->arena, self->generation) != 0) return NULL;
    ustore_docs_column_view_t const* const column = columns_column(self, (size_t)field_index);
    ustore_docs_field_type_t const type = self->types[field_index];
    char const* const format = field_type_format(type);
    Py_ssize_t const shape[2] = {(Py_ssize_t)self->docs_count, uuid_bytes_k};
    PyObject* item = NULL;
    if (format)
        item = ustore_py_span_view(self->arena, self->generation, column->scalars, format,
                                   (Py_ssize_t)field_type_width(type), 1, shape);
    else if (type == ustore_docs_field_type_uuid_k)
        item = ustore_py_span_view(self->arena, self->generation, column->scalars, "B", 1, 2, shape);
    else item = column_strings(column, type, self->docs_count);
    ustore_py_arena_unpin(self->arena);
    return item;
}

/** The three bitmaps of a column, which @c columns_bitmap picks between. */
typedef enum column_bitmap_t {
    column_validities_k = 0,
    column_conversions_k = 1,
    column_collisions_k = 2,
} column_bitmap_t;

/** Answers one bitmap of a column as a byte view, for the field @p index_object names. */
static PyObject* columns_bitmap(Columns* self, column_bitmap_t bitmap, PyObject* index_object) {
    size_t field_index = 0;
    if (columns_field_index(self, index_object, &field_index) != 0) return NULL;
    if (ustore_py_arena_pin(self->arena, self->generation) != 0) return NULL;
    ustore_docs_column_view_t const* const column = columns_column(self, field_index);
    ustore_octet_t const* const bitmaps[3] = {column->validities, column->conversions, column->collisions};
    Py_ssize_t const bytes = (Py_ssize_t)((self->docs_count + 7) / 8);
    PyObject* const view = ustore_py_span_view(self->arena, self->generation, bitmaps[bitmap], "B", 1, 1, &bytes);
    ustore_py_arena_unpin(self->arena);
    return view;
}

static char const doc_Columns_validities[] =
    "validities(field) answers the bitmap of documents whose value was found and converted, as bytes.";

static PyObject* Columns_validities(Columns* self, PyObject* index_object) {
    return columns_bitmap(self, column_validities_k, index_object);
}

static char const doc_Columns_conversions[] =
    "conversions(field) answers the bitmap of values whose conversion changed their meaning, as bytes.";

static PyObject* Columns_conversions(Columns* self, PyObject* index_object) {
    return columns_bitmap(self, column_conversions_k, index_object);
}

static char const doc_Columns_collisions[] =
    "collisions(field) answers the bitmap of values of a type no conversion reaches, as bytes.";

static PyObject* Columns_collisions(Columns* self, PyObject* index_object) {
    return columns_bitmap(self, column_collisions_k, index_object);
}

static char const doc_Columns_types[] = "The type every column was converted into, as a tuple of FieldType.";

static PyObject* Columns_get_types(Columns* self, void* Py_UNUSED(closure)) {
    PyObject* const types = PyTuple_New((Py_ssize_t)self->fields_count);
    if (!types) return NULL;
    for (size_t field_index = 0; field_index != self->fields_count; ++field_index) {
        PyObject* const type = ustore_py_enum_to_object(ustore_py_enum_field_type_k, (int)self->types[field_index]);
        if (!type) {
            Py_DECREF(types);
            return NULL;
        }
        PyTuple_SET_ITEM(types, (Py_ssize_t)field_index, type);
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
    {"validities", (PyCFunction)(void (*)(void))Columns_validities, METH_O, doc_Columns_validities},
    {"conversions", (PyCFunction)(void (*)(void))Columns_conversions, METH_O, doc_Columns_conversions},
    {"collisions", (PyCFunction)(void (*)(void))Columns_collisions, METH_O, doc_Columns_collisions},
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
    .tp_itemsize = sizeof(ustore_docs_field_type_t),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = NULL,
    .tp_dealloc = (destructor)Columns_dealloc,
    .tp_repr = (reprfunc)Columns_repr,
    .tp_as_sequence = &Columns_as_sequence,
    .tp_methods = Columns_methods,
    .tp_getset = Columns_getset,
};

#pragma endregion Columns
