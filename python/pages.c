/**
 *  @file python/pages.c
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief Pages: imports, positional edits, renders and scans of ordered element trees.
 *
 *  Every argument of a pages verb is one item for every task or one item per task, so a batch of
 *  edits on one page names the page once, and the tasks of a call are agreed across its arguments
 *  as they are gathered. Every verb fills a call the driver keeps on the stack or inside a
 *  @c Deferred, holding the keys, markup and names it borrows until the call finishes, and a read
 *  claims the arena it is given and answers a tape viewing it. An insert answers the key every
 *  element ended up with, copied out of a one-off arena.
 */
#include "lib.h"

#pragma region Spread Arguments

/** The tasks one call carries, agreed across its arguments as they are gathered. */
typedef struct tasks_t {

    /** Tasks the call carries, one until an argument of one item per task decides it. */
    size_t count;

    /** Whether an argument of one item per task decided @c count. */
    int decided;
} tasks_t;

/**
 *  @brief Folds an argument of @p count items into @p tasks: one item for every task fits any count, and every
 *      argument of one item per task must agree with the first.
 *  @param[in] argument The argument to name in a failure.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
static int tasks_fold(tasks_t* tasks, size_t count, ustore_py_contents_spread_t spread, char const* argument) {
    if (spread == ustore_py_contents_broadcast_k) return 0;
    if (!tasks->decided) {
        tasks->count = count;
        tasks->decided = 1;
        return 0;
    }
    if (tasks->count == count) return 0;
    return ustore_py_raise(ustore_status_invalid_argument_k, "Got %zu %s for %zu tasks", count, argument, tasks->count);
}

/** A batch of keys, one for every task or one per task. */
typedef struct spread_keys_t {

    /** The keys, one inline when it stands for every task. */
    ustore_py_keys_t batch;

    /** Whether the one key repeats for every task. */
    ustore_py_contents_spread_t spread;
} spread_keys_t;

/**
 *  @brief Gathers one key for every task or a batch of one per task, folding its count into @p tasks.
 *  @param[in] object The keys, or NULL for an argument left out, which stands for Nil in every task.
 *  @param[in] argument The argument to name in a failure.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
static int spread_keys_from_object(PyObject* object, char const* argument, tasks_t* tasks, spread_keys_t* keys) {
    memset(keys, 0, sizeof(*keys));
    keys->spread = ustore_py_contents_broadcast_k;
    if (!object) {
        keys->batch.keys = keys->batch.inline_keys;
        keys->batch.count = 1;
        return 0;
    }
    int const one = ustore_py_keys_from_subscript(object, &keys->batch);
    if (one < 0) return -1;
    if (!one) keys->spread = ustore_py_contents_per_task_k;
    return tasks_fold(tasks, keys->batch.count, keys->spread, argument);
}

/** The strided view of @p keys, whose stride of zero repeats the one key for every task. */
static ustore_keys_view_t spread_keys_view(spread_keys_t const* keys) {
    ustore_keys_view_t const view = {
        keys->batch.keys,
        keys->spread == ustore_py_contents_broadcast_k ? 0 : sizeof(ustore_key_t),
    };
    return view;
}

/** Gathers a tape or a sequence of texts, or one text for every task, folding its count into @p tasks. */
static int spread_texts_from_object(PyObject* object, char const* argument, tasks_t* tasks, ustore_py_texts_t* texts) {
    if (ustore_py_texts_from_object(object, argument, NULL, NULL, texts) != 0) return -1;
    return tasks_fold(tasks, texts->contents.count, texts->contents.spread, argument);
}

/** A batch of byte positions or lengths, one for every task or one per task. */
typedef struct spread_lengths_t {

    /** The lengths, copied, or NULL before they were gathered. */
    ustore_length_t* lengths;

    /** Whether the one length repeats for every task. */
    ustore_py_contents_spread_t spread;
} spread_lengths_t;

/**
 *  @brief Gathers one length for every task or a sequence of one per task, folding its count into @p tasks.
 *  @param[in] object A non-negative integer, or a sequence of them.
 *  @param[in] argument The argument to name in a failure.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
static int spread_lengths_from_object(PyObject* object, char const* argument, tasks_t* tasks,
                                      spread_lengths_t* lengths) {
    memset(lengths, 0, sizeof(*lengths));
    if (PyIndex_Check(object)) {
        size_t length = 0;
        if (ustore_py_size_from_object(object, argument, &length) != 0) return -1;
        lengths->lengths = PyMem_New(ustore_length_t, 1);
        if (!lengths->lengths) {
            PyErr_NoMemory();
            return -1;
        }
        lengths->lengths[0] = (ustore_length_t)length;
        lengths->spread = ustore_py_contents_broadcast_k;
        return 0;
    }
    PyObject* const sequence = PySequence_Fast(object, "Expected a non-negative integer or a sequence of them");
    if (!sequence) return -1;
    size_t const count = (size_t)PySequence_Fast_GET_SIZE(sequence);
    lengths->lengths = PyMem_New(ustore_length_t, count + 1);
    int failed = !lengths->lengths;
    if (failed) PyErr_NoMemory();
    for (size_t task_index = 0; !failed && task_index != count; ++task_index) {
        size_t length = 0;
        failed = ustore_py_size_from_object(PySequence_Fast_GET_ITEM(sequence, (Py_ssize_t)task_index), argument,
                                            &length) != 0;
        if (!failed) lengths->lengths[task_index] = (ustore_length_t)length;
    }
    Py_DECREF(sequence);
    if (failed) return -1;
    lengths->spread = ustore_py_contents_per_task_k;
    return tasks_fold(tasks, count, lengths->spread, argument);
}

/** The strided view of @p lengths, whose stride of zero repeats the one length for every task. */
static ustore_lengths_view_t spread_lengths_view(spread_lengths_t const* lengths) {
    ustore_lengths_view_t const view = {
        lengths->lengths,
        lengths->spread == ustore_py_contents_broadcast_k ? 0 : sizeof(ustore_length_t),
    };
    return view;
}

/** The keywords a pages verb may take beside the shared ones, every object borrowed, NULL where absent or None. */
typedef struct page_keywords_t {

    /** The elements every task addresses, for verbs whose elements are optional. */
    PyObject* elements;

    /** The parents the elements land under. */
    PyObject* parents;

    /** The siblings after which the elements land. */
    PyObject* anchors;

    /** The callable reporting progress. */
    PyObject* progress;

    /** The @c PageEncoding the markup is parsed from. */
    int encoding;

    /** The @c PageEncoding a read renders its markup into. */
    int outputs_encoding;
} page_keywords_t;

/**
 *  @brief Walks the keywords of a pages verb, admitting the shared ones and those @p admitted names.
 *  @param[in] verb The method's name, as a failure should read it.
 *  @param[in] required_positional Positional arguments the verb fixes.
 *  @param[in] admitted The keywords of the verb's own, ended by NULL.
 *  @param[out] options Where every shared keyword lands.
 *  @param[out] keywords Where every keyword of the verb's own lands, both encodings starting at XML.
 *  @return 0 on success, -1 with an exception set otherwise.
 */
static int page_keywords_from_args(ustore_py_context_t const* context, char const* verb, PyObject* const* args,
                                   Py_ssize_t positional_args_count, PyObject* args_names_tuple,
                                   Py_ssize_t required_positional, char const* const* admitted,
                                   ustore_py_verb_options_t* options, page_keywords_t* keywords) {
    Py_ssize_t const args_names_count = ustore_py_args_names_count_checked(verb, positional_args_count,
                                                                           args_names_tuple, required_positional);
    if (args_names_count < 0) return -1;
    ustore_py_verb_options_init(options);
    memset(keywords, 0, sizeof(*keywords));
    // HTML leads the enum, yet no engine parses it, so XML is what a caller naming none means.
    keywords->encoding = ustore_page_encoding_xml_k;
    keywords->outputs_encoding = ustore_page_encoding_xml_k;
    for (Py_ssize_t names_progress = 0, args_progress = positional_args_count; names_progress != args_names_count;
         ++args_progress, ++names_progress) {
        PyObject* const name = PyTuple_GET_ITEM(args_names_tuple, names_progress);
        PyObject* const value = args[args_progress];
        int const consumed = ustore_py_verb_option_from_keyword(context, name, value, options);
        if (consumed < 0) return -1;
        if (consumed > 0) continue;
        char const* const* own = admitted;
        while (*own && PyUnicode_CompareWithASCIIString(name, *own) != 0) ++own;
        if (!*own) {
            PyErr_Format(PyExc_TypeError, "Got unexpected keyword argument: %S", name);
            return -1;
        }
        PyObject* const given = value == Py_None ? NULL : value;
        int parsed = 0;
        if (strcmp(*own, "elements") == 0) keywords->elements = given;
        else if (strcmp(*own, "parents") == 0) keywords->parents = given;
        else if (strcmp(*own, "anchors") == 0) keywords->anchors = given;
        else if (strcmp(*own, "progress") == 0) parsed = ustore_py_progress_from_object(value, &keywords->progress);
        else if (strcmp(*own, "encoding") == 0)
            parsed = ustore_py_enum_from_object(value, *own, ustore_py_enum_page_encoding_k, &keywords->encoding);
        else if (strcmp(*own, "outputs_encoding") == 0)
            parsed = ustore_py_enum_from_object(value, *own, ustore_py_enum_page_encoding_k,
                                                &keywords->outputs_encoding);
        if (parsed != 0) return -1;
    }
    return 0;
}

/** The keywords of their own each pages verb admits, every list ended by NULL. */
static char const* const import_keywords[] = {"encoding", "progress", NULL};
static char const* const insert_keywords[] = {"elements", "parents", "anchors", "encoding", NULL};
static char const* const move_keywords[] = {"parents", "anchors", NULL};
static char const* const read_keywords[] = {"elements", "outputs_encoding", NULL};
static char const* const no_keywords[] = {NULL};

#pragma endregion Spread Arguments

#pragma region Edits

/** One edit of pages, whichever verb it runs through, every argument it does not take left zeroed. */
typedef struct edit_call_t {

    /** What the call keeps until it finishes. */
    ustore_py_call_t call;

    /** The tasks the call carries. */
    tasks_t tasks;

    /** The page every task edits. */
    spread_keys_t pages;

    /** The element every task addresses. */
    spread_keys_t elements;

    /** The parent every element lands under. */
    spread_keys_t parents;

    /** The sibling after which every element lands. */
    spread_keys_t anchors;

    /** The attribute names every task sets or removes. */
    ustore_py_texts_t fields;

    /** The markup, fragments, text runs or attribute values every task writes. */
    ustore_py_texts_t values;

    /** Bytes into the text of every element where its splice starts. */
    spread_lengths_t text_starts;

    /** Bytes of text every splice replaces. */
    spread_lengths_t text_lengths;

    /** Output slot for the key every inserted element ended up with, inside the arena. */
    ustore_key_t const* effective_keys;
} edit_call_t;

static void import_submit(void* arguments) { ustore_pages_import((ustore_pages_import_t*)arguments); }
static void insert_submit(void* arguments) { ustore_pages_insert((ustore_pages_insert_t*)arguments); }
static void remove_submit(void* arguments) { ustore_pages_remove((ustore_pages_remove_t*)arguments); }
static void move_submit(void* arguments) { ustore_pages_move((ustore_pages_move_t*)arguments); }
static void splice_submit(void* arguments) { ustore_pages_splice((ustore_pages_splice_t*)arguments); }
static void set_attributes_submit(void* arguments) {
    ustore_pages_set_attributes((ustore_pages_set_attributes_t*)arguments);
}
static void remove_attributes_submit(void* arguments) {
    ustore_pages_remove_attributes((ustore_pages_remove_attributes_t*)arguments);
}

// Every key is a `uuid.UUID`, which owns its bytes, so nothing of the arena outlives the answer.
static PyObject* insert_answer(void* erased) {
    edit_call_t const* const call = (edit_call_t const*)erased;
    PyObject* const keys = PyTuple_New((Py_ssize_t)call->tasks.count);
    for (size_t task_index = 0; keys && task_index != call->tasks.count; ++task_index) {
        PyObject* const key = ustore_py_uuid_to_object(&call->effective_keys[task_index].uuid);
        if (!key) {
            Py_DECREF(keys);
            return NULL;
        }
        PyTuple_SET_ITEM(keys, (Py_ssize_t)task_index, key);
    }
    return keys;
}

static void edit_release(void* erased) {
    edit_call_t* const call = (edit_call_t*)erased;
    ustore_py_keys_release(&call->pages.batch);
    ustore_py_keys_release(&call->elements.batch);
    ustore_py_keys_release(&call->parents.batch);
    ustore_py_keys_release(&call->anchors.batch);
    ustore_py_texts_release(&call->fields);
    ustore_py_texts_release(&call->values);
    PyMem_Free(call->text_starts.lengths);
    PyMem_Free(call->text_lengths.lengths);
    call->text_starts.lengths = NULL;
    call->text_lengths.lengths = NULL;
    ustore_py_call_release(&call->call);
}

static ustore_py_verb_t const import_verb = {sizeof(edit_call_t), import_submit, ustore_py_call_answer_none,
                                             edit_release};
static ustore_py_verb_t const insert_verb = {sizeof(edit_call_t), insert_submit, insert_answer, edit_release};
static ustore_py_verb_t const remove_verb = {sizeof(edit_call_t), remove_submit, ustore_py_call_answer_none,
                                             edit_release};
static ustore_py_verb_t const move_verb = {sizeof(edit_call_t), move_submit, ustore_py_call_answer_none, edit_release};
static ustore_py_verb_t const splice_verb = {sizeof(edit_call_t), splice_submit, ustore_py_call_answer_none,
                                             edit_release};
static ustore_py_verb_t const set_attributes_verb = {sizeof(edit_call_t), set_attributes_submit,
                                                     ustore_py_call_answer_none, edit_release};
static ustore_py_verb_t const remove_attributes_verb = {sizeof(edit_call_t), remove_attributes_submit,
                                                        ustore_py_call_answer_none, edit_release};

/**
 *  @brief Opens an edit of @p verb through @p self, refusing a snapshot, and gathers the pages its first argument
 *      names and the keywords @p admitted names.
 *  @param[out] context What the call addresses and how it runs.
 *  @param[out] options The collection and durability the call names.
 *  @param[out] keywords The keywords of the verb's own.
 *  @param[in] stack The caller's storage, used while the call blocks.
 *  @return The call, or NULL with an exception set.
 */
static edit_call_t* edit_open(PyObject* self, char const* name, ustore_py_verb_t const* verb, PyObject* const* args,
                              Py_ssize_t positional_args_count, PyObject* args_names_tuple,
                              Py_ssize_t required_positional, char const* const* admitted, ustore_py_context_t* context,
                              ustore_py_verb_options_t* options, page_keywords_t* keywords, edit_call_t* stack) {
    if (ustore_py_context_from_object(self, context) != 0 || ustore_py_context_writable(context) != 0) return NULL;
    if (page_keywords_from_args(context, name, args, positional_args_count, args_names_tuple, required_positional,
                                admitted, options, keywords) != 0)
        return NULL;
    edit_call_t* const call = ustore_py_call_open(context, verb, stack);
    if (!call) return NULL;
    call->tasks.count = 1;
    call->call.collection = options->collection;
    if (spread_keys_from_object(args[0], "pages", &call->tasks, &call->pages) == 0) return call;
    ustore_py_call_discard(context, verb, call);
    return NULL;
}

char const doc_pages_import[] =                                                                   //
    "pages_import(keys, markup, *, collection, encoding=PageEncoding.XML,\n"                      //
    "             durability=Durability.BUFFERED, progress=None) answers None.\n"                 //
    "\n"                                                                                          //
    "Replaces whole pages with parsed markup, minting a key for every element. Every markup is\n" //
    "borrowed for the length of the call rather than copied.\n"                                   //
    "\n"                                                                                          //
    "Args:\n"                                                                                     //
    "  keys (KeyLike | Sequence[KeyLike] | buffer): The pages, one per markup.\n"                 //
    "  markup (Sequence[str | bytes-like] | str | bytes-like | tape): One markup per page, a\n"   //
    "    tape of them, or one for every page.\n"                                                  //
    "  collection (UUID | str): The pages collection, by identifier or by name.\n"                //
    "  encoding (PageEncoding, optional): How the markup is spelled: `PageEncoding.XML`, or\n"    //
    "    `PageEncoding.TEXT` for one run of plain text.\n"                                        //
    "  durability (Durability, optional): `Durability.FLUSHED` reaches the drive before\n"        //
    "    returning.\n"                                                                            //
    "  progress (Callable[[int, int], bool | None], optional): Called with the bytes parsed\n"    //
    "    and the bytes in all; a falsy answer other than None cancels the call.\n"                //
    "Raises:\n"                                                                                   //
    "  MalformedError: A markup does not parse.\n"                                                //
    "  UnsupportedError: The encoding is HTML, Markdown or JSON.";

PyObject* ustore_py_api_pages_import(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                     PyObject* args_names_tuple) {
    ustore_py_context_t context;
    ustore_py_verb_options_t options;
    page_keywords_t keywords;
    edit_call_t stack;
    edit_call_t* const call = edit_open(self, "pages_import", &import_verb, args, positional_args_count,
                                        args_names_tuple, 2, import_keywords, &context, &options, &keywords, &stack);
    if (!call) return NULL;
    if (spread_texts_from_object(args[1], "markups", &call->tasks, &call->values) != 0)
        return ustore_py_call_discard(&context, &import_verb, call);
    call->call.progress = Py_XNewRef(keywords.progress);
    ustore_pages_import_t import = {
        .database = context.database_handle,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->call.outcome,
        .completion = context.completion,
        .transaction = context.transaction,
        .count = call->tasks.count,
        .collections = {&call->call.collection, 0},
        .keys = spread_keys_view(&call->pages),
        .values = call->values.contents.view,
        .encoding = (ustore_page_encoding_t)keywords.encoding,
        .durability = options.durability,
        .watch = context.watch,
        .progress = ustore_py_progress_callback(&call->call),
        .progress_state = &call->call,
    };
    return ustore_py_call_run(&context, &import_verb, call, &import);
}

char const doc_pages_insert[] =                                                                   //
    "pages_insert(keys, fragments, *, collection, elements=None, parents=None, anchors=None,\n"   //
    "             encoding=PageEncoding.XML, durability=Durability.BUFFERED)\n"                   //
    "answers a tuple of element keys.\n"                                                          //
    "\n"                                                                                          //
    "Parses every fragment into elements under its parent after its anchor. Insertions after\n"   //
    "one sibling order newest first, and a later task of the call may anchor on an element an\n"  //
    "earlier one inserted.\n"                                                                     //
    "\n"                                                                                          //
    "Args:\n"                                                                                     //
    "  keys (KeyLike | Sequence[KeyLike] | buffer): The page of every task, or one for all.\n"    //
    "  fragments (Sequence[str | bytes-like] | str | bytes-like | tape): One fragment per\n"      //
    "    task, a tape of them, or one for every task.\n"                                          //
    "  collection (UUID | str): The pages collection, by identifier or by name.\n"                //
    "  elements (KeyLike | Sequence[KeyLike] | buffer, optional): The key to give every\n"        //
    "    inserted element, where Nil or None mints one.\n"                                        //
    "  parents (KeyLike | Sequence[KeyLike] | buffer, optional): The parent every element\n"      //
    "    lands under, where Nil or None stands for the root.\n"                                   //
    "  anchors (KeyLike | Sequence[KeyLike] | buffer, optional): The sibling after which every\n" //
    "    element lands, where Nil or None stands for the front.\n"                                //
    "  encoding (PageEncoding, optional): How the fragments are spelled.\n"                       //
    "  durability (Durability, optional): `Durability.FLUSHED` reaches the drive before\n"        //
    "    returning.\n"                                                                            //
    "Returns:\n"                                                                                  //
    "  tuple[UUID, ...]: The key every inserted element ended up with, minted or given.\n"        //
    "Raises:\n"                                                                                   //
    "  NotFoundError: A page does not exist.\n"                                                   //
    "  MalformedError: A fragment does not parse.";

PyObject* ustore_py_api_pages_insert(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                     PyObject* args_names_tuple) {
    ustore_py_context_t context;
    ustore_py_verb_options_t options;
    page_keywords_t keywords;
    edit_call_t stack;
    edit_call_t* const call = edit_open(self, "pages_insert", &insert_verb, args, positional_args_count,
                                        args_names_tuple, 2, insert_keywords, &context, &options, &keywords, &stack);
    if (!call) return NULL;
    if (spread_texts_from_object(args[1], "fragments", &call->tasks, &call->values) != 0 ||
        spread_keys_from_object(keywords.elements, "elements", &call->tasks, &call->elements) != 0 ||
        spread_keys_from_object(keywords.parents, "parents", &call->tasks, &call->parents) != 0 ||
        spread_keys_from_object(keywords.anchors, "anchors", &call->tasks, &call->anchors) != 0)
        return ustore_py_call_discard(&context, &insert_verb, call);
    // The arena only carries the effective keys out, so it dies with the call that claimed it.
    PyObject* const arena = ustore_py_arena_new(ustore_py_context_executor(&context));
    int const claimed = arena ? ustore_py_arena_claim(arena, &call->call.arena) : -1;
    Py_XDECREF(arena);
    if (claimed != 0) return ustore_py_call_discard(&context, &insert_verb, call);
    ustore_pages_insert_t insert = {
        .database = context.database_handle,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->call.outcome,
        .completion = context.completion,
        .transaction = context.transaction,
        .count = call->tasks.count,
        .collections = {&call->call.collection, 0},
        .keys = spread_keys_view(&call->pages),
        .elements = spread_keys_view(&call->elements),
        .parents = spread_keys_view(&call->parents),
        .anchors = spread_keys_view(&call->anchors),
        .values = call->values.contents.view,
        .encoding = (ustore_page_encoding_t)keywords.encoding,
        .effective_keys = &call->effective_keys,
        .arena = ustore_py_arena_handle(call->call.arena),
        .durability = options.durability,
        .watch = context.watch,
    };
    return ustore_py_call_run(&context, &insert_verb, call, &insert);
}

char const doc_pages_remove[] =                                                                //
    "pages_remove(keys, elements, *, collection, durability=Durability.BUFFERED)\n"            //
    "answers None.\n"                                                                          //
    "\n"                                                                                       //
    "Removes elements with their subtrees, where the page key removes the whole page.\n"       //
    "\n"                                                                                       //
    "Args:\n"                                                                                  //
    "  keys (KeyLike | Sequence[KeyLike] | buffer): The page of every task, or one for all.\n" //
    "  elements (KeyLike | Sequence[KeyLike] | buffer): The elements to remove.\n"             //
    "  collection (UUID | str): The pages collection, by identifier or by name.\n"             //
    "  durability (Durability, optional): `Durability.FLUSHED` reaches the drive before\n"     //
    "    returning.\n"                                                                         //
    "Raises:\n"                                                                                //
    "  NotFoundError: A page does not exist.";

PyObject* ustore_py_api_pages_remove(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                     PyObject* args_names_tuple) {
    ustore_py_context_t context;
    ustore_py_verb_options_t options;
    page_keywords_t keywords;
    edit_call_t stack;
    edit_call_t* const call = edit_open(self, "pages_remove", &remove_verb, args, positional_args_count,
                                        args_names_tuple, 2, no_keywords, &context, &options, &keywords, &stack);
    if (!call) return NULL;
    if (spread_keys_from_object(args[1], "elements", &call->tasks, &call->elements) != 0)
        return ustore_py_call_discard(&context, &remove_verb, call);
    ustore_pages_remove_t remove = {
        .database = context.database_handle,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->call.outcome,
        .completion = context.completion,
        .transaction = context.transaction,
        .count = call->tasks.count,
        .collections = {&call->call.collection, 0},
        .keys = spread_keys_view(&call->pages),
        .elements = spread_keys_view(&call->elements),
        .durability = options.durability,
        .watch = context.watch,
    };
    return ustore_py_call_run(&context, &remove_verb, call, &remove);
}

char const doc_pages_move[] =                                                                     //
    "pages_move(keys, elements, *, collection, parents=None, anchors=None,\n"                     //
    "           durability=Durability.BUFFERED) answers None.\n"                                  //
    "\n"                                                                                          //
    "Moves elements with their subtrees under parents after anchors, keeping their keys.\n"       //
    "\n"                                                                                          //
    "Args:\n"                                                                                     //
    "  keys (KeyLike | Sequence[KeyLike] | buffer): The page of every task, or one for all.\n"    //
    "  elements (KeyLike | Sequence[KeyLike] | buffer): The elements to move.\n"                  //
    "  collection (UUID | str): The pages collection, by identifier or by name.\n"                //
    "  parents (KeyLike | Sequence[KeyLike] | buffer, optional): The parent every element\n"      //
    "    lands under, where Nil or None stands for the root.\n"                                   //
    "  anchors (KeyLike | Sequence[KeyLike] | buffer, optional): The sibling after which every\n" //
    "    element lands, where Nil or None stands for the front.\n"                                //
    "  durability (Durability, optional): `Durability.FLUSHED` reaches the drive before\n"        //
    "    returning.\n"                                                                            //
    "Raises:\n"                                                                                   //
    "  NotFoundError: A page does not exist.";

PyObject* ustore_py_api_pages_move(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                   PyObject* args_names_tuple) {
    ustore_py_context_t context;
    ustore_py_verb_options_t options;
    page_keywords_t keywords;
    edit_call_t stack;
    edit_call_t* const call = edit_open(self, "pages_move", &move_verb, args, positional_args_count, args_names_tuple,
                                        2, move_keywords, &context, &options, &keywords, &stack);
    if (!call) return NULL;
    if (spread_keys_from_object(args[1], "elements", &call->tasks, &call->elements) != 0 ||
        spread_keys_from_object(keywords.parents, "parents", &call->tasks, &call->parents) != 0 ||
        spread_keys_from_object(keywords.anchors, "anchors", &call->tasks, &call->anchors) != 0)
        return ustore_py_call_discard(&context, &move_verb, call);
    ustore_pages_move_t move = {
        .database = context.database_handle,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->call.outcome,
        .completion = context.completion,
        .transaction = context.transaction,
        .count = call->tasks.count,
        .collections = {&call->call.collection, 0},
        .keys = spread_keys_view(&call->pages),
        .elements = spread_keys_view(&call->elements),
        .parents = spread_keys_view(&call->parents),
        .anchors = spread_keys_view(&call->anchors),
        .durability = options.durability,
        .watch = context.watch,
    };
    return ustore_py_call_run(&context, &move_verb, call, &move);
}

char const doc_pages_splice[] =                                                                 //
    "pages_splice(keys, elements, starts, lengths, texts, *, collection,\n"                     //
    "             durability=Durability.BUFFERED) answers None.\n"                              //
    "\n"                                                                                        //
    "Replaces `lengths` bytes of the text of every element from `starts` on with `texts`, so\n" //
    "a zero length only inserts and an empty text only removes.\n"                              //
    "\n"                                                                                        //
    "Args:\n"                                                                                   //
    "  keys (KeyLike | Sequence[KeyLike] | buffer): The page of every task, or one for all.\n"  //
    "  elements (KeyLike | Sequence[KeyLike] | buffer): The elements whose text is spliced.\n"  //
    "  starts (int | Sequence[int]): Bytes into every text where its splice starts.\n"          //
    "  lengths (int | Sequence[int]): Bytes of text every splice replaces.\n"                   //
    "  texts (Sequence[str | bytes-like] | str | bytes-like | tape): The new bytes.\n"          //
    "  collection (UUID | str): The pages collection, by identifier or by name.\n"              //
    "  durability (Durability, optional): `Durability.FLUSHED` reaches the drive before\n"      //
    "    returning.\n"                                                                          //
    "Raises:\n"                                                                                 //
    "  NotFoundError: A page does not exist.";

PyObject* ustore_py_api_pages_splice(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                     PyObject* args_names_tuple) {
    ustore_py_context_t context;
    ustore_py_verb_options_t options;
    page_keywords_t keywords;
    edit_call_t stack;
    edit_call_t* const call = edit_open(self, "pages_splice", &splice_verb, args, positional_args_count,
                                        args_names_tuple, 5, no_keywords, &context, &options, &keywords, &stack);
    if (!call) return NULL;
    if (spread_keys_from_object(args[1], "elements", &call->tasks, &call->elements) != 0 ||
        spread_lengths_from_object(args[2], "starts", &call->tasks, &call->text_starts) != 0 ||
        spread_lengths_from_object(args[3], "lengths", &call->tasks, &call->text_lengths) != 0 ||
        spread_texts_from_object(args[4], "texts", &call->tasks, &call->values) != 0)
        return ustore_py_call_discard(&context, &splice_verb, call);
    ustore_pages_splice_t splice = {
        .database = context.database_handle,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->call.outcome,
        .completion = context.completion,
        .transaction = context.transaction,
        .count = call->tasks.count,
        .collections = {&call->call.collection, 0},
        .keys = spread_keys_view(&call->pages),
        .elements = spread_keys_view(&call->elements),
        .text_starts = spread_lengths_view(&call->text_starts),
        .text_lengths = spread_lengths_view(&call->text_lengths),
        .values = call->values.contents.view,
        .durability = options.durability,
        .watch = context.watch,
    };
    return ustore_py_call_run(&context, &splice_verb, call, &splice);
}

char const doc_pages_set_attributes[] =                                                        //
    "pages_set_attributes(keys, elements, fields, values, *, collection,\n"                    //
    "                     durability=Durability.BUFFERED) answers None.\n"                     //
    "\n"                                                                                       //
    "Sets one attribute of an element per task, adding the ones it lacks.\n"                   //
    "\n"                                                                                       //
    "Args:\n"                                                                                  //
    "  keys (KeyLike | Sequence[KeyLike] | buffer): The page of every task, or one for all.\n" //
    "  elements (KeyLike | Sequence[KeyLike] | buffer): The elements whose attributes are\n"   //
    "    set.\n"                                                                               //
    "  fields (Sequence[str | bytes-like] | str | bytes-like | tape): The attribute names.\n"  //
    "  values (Sequence[str | bytes-like] | str | bytes-like | tape): The attribute values.\n" //
    "  collection (UUID | str): The pages collection, by identifier or by name.\n"             //
    "  durability (Durability, optional): `Durability.FLUSHED` reaches the drive before\n"     //
    "    returning.\n"                                                                         //
    "Raises:\n"                                                                                //
    "  NotFoundError: A page does not exist.";

PyObject* ustore_py_api_pages_set_attributes(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                             PyObject* args_names_tuple) {
    ustore_py_context_t context;
    ustore_py_verb_options_t options;
    page_keywords_t keywords;
    edit_call_t stack;
    edit_call_t* const call = edit_open(self, "pages_set_attributes", &set_attributes_verb, args, positional_args_count,
                                        args_names_tuple, 4, no_keywords, &context, &options, &keywords, &stack);
    if (!call) return NULL;
    if (spread_keys_from_object(args[1], "elements", &call->tasks, &call->elements) != 0 ||
        spread_texts_from_object(args[2], "fields", &call->tasks, &call->fields) != 0 ||
        spread_texts_from_object(args[3], "values", &call->tasks, &call->values) != 0)
        return ustore_py_call_discard(&context, &set_attributes_verb, call);
    ustore_pages_set_attributes_t set = {
        .database = context.database_handle,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->call.outcome,
        .completion = context.completion,
        .transaction = context.transaction,
        .count = call->tasks.count,
        .collections = {&call->call.collection, 0},
        .keys = spread_keys_view(&call->pages),
        .elements = spread_keys_view(&call->elements),
        .fields = call->fields.contents.view,
        .values = call->values.contents.view,
        .durability = options.durability,
        .watch = context.watch,
    };
    return ustore_py_call_run(&context, &set_attributes_verb, call, &set);
}

char const doc_pages_remove_attributes[] =                                                     //
    "pages_remove_attributes(keys, elements, fields, *, collection,\n"                         //
    "                        durability=Durability.BUFFERED) answers None.\n"                  //
    "\n"                                                                                       //
    "Removes one attribute of an element per task.\n"                                          //
    "\n"                                                                                       //
    "Args:\n"                                                                                  //
    "  keys (KeyLike | Sequence[KeyLike] | buffer): The page of every task, or one for all.\n" //
    "  elements (KeyLike | Sequence[KeyLike] | buffer): The elements whose attributes are\n"   //
    "    removed.\n"                                                                           //
    "  fields (Sequence[str | bytes-like] | str | bytes-like | tape): The attribute names.\n"  //
    "  collection (UUID | str): The pages collection, by identifier or by name.\n"             //
    "  durability (Durability, optional): `Durability.FLUSHED` reaches the drive before\n"     //
    "    returning.\n"                                                                         //
    "Raises:\n"                                                                                //
    "  NotFoundError: A page does not exist.";

PyObject* ustore_py_api_pages_remove_attributes(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                                PyObject* args_names_tuple) {
    ustore_py_context_t context;
    ustore_py_verb_options_t options;
    page_keywords_t keywords;
    edit_call_t stack;
    edit_call_t* const call = edit_open(self, "pages_remove_attributes", &remove_attributes_verb, args,
                                        positional_args_count, args_names_tuple, 3, no_keywords, &context, &options,
                                        &keywords, &stack);
    if (!call) return NULL;
    if (spread_keys_from_object(args[1], "elements", &call->tasks, &call->elements) != 0 ||
        spread_texts_from_object(args[2], "fields", &call->tasks, &call->fields) != 0)
        return ustore_py_call_discard(&context, &remove_attributes_verb, call);
    ustore_pages_remove_attributes_t remove = {
        .database = context.database_handle,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->call.outcome,
        .completion = context.completion,
        .transaction = context.transaction,
        .count = call->tasks.count,
        .collections = {&call->call.collection, 0},
        .keys = spread_keys_view(&call->pages),
        .elements = spread_keys_view(&call->elements),
        .fields = call->fields.contents.view,
        .durability = options.durability,
        .watch = context.watch,
    };
    return ustore_py_call_run(&context, &remove_attributes_verb, call, &remove);
}

#pragma endregion Edits

#pragma region Reads

/** A render of pages or subtrees, whose tape lands in the arena its header holds. */
typedef struct read_call_t {

    /** What the call keeps until it finishes. */
    ustore_py_call_t call;

    /** The tasks the call carries. */
    tasks_t tasks;

    /** The page every task renders. */
    spread_keys_t pages;

    /** The element every task renders with its subtree, where Nil renders the whole page. */
    spread_keys_t elements;

    /** Output slot for the presence bits, inside the arena. */
    ustore_octet_t const* presences;

    /** Output slot for the tape of renderings, inside the arena. */
    ustore_strings_view_t values;
} read_call_t;

static void read_submit(void* arguments) { ustore_pages_read((ustore_pages_read_t*)arguments); }

static PyObject* read_answer(void* erased) {
    read_call_t const* const call = (read_call_t const*)erased;
    return ustore_py_values_new(call->call.arena, call->presences, &call->values, call->tasks.count);
}

static void read_release(void* erased) {
    read_call_t* const call = (read_call_t*)erased;
    ustore_py_keys_release(&call->pages.batch);
    ustore_py_keys_release(&call->elements.batch);
    ustore_py_call_release(&call->call);
}

static ustore_py_verb_t const read_verb = {sizeof(read_call_t), read_submit, read_answer, read_release};

char const doc_pages_read[] =                                                                    //
    "pages_read(keys, arena, *, collection, elements=None, outputs_encoding=PageEncoding.XML)\n" //
    "answers a Values tape.\n"                                                                   //
    "\n"                                                                                         //
    "Renders whole pages, or the subtrees of elements, as the receiver sees them.\n"             //
    "\n"                                                                                         //
    "Args:\n"                                                                                    //
    "  keys (KeyLike | Sequence[KeyLike] | buffer): The page of every task, or one for all.\n"   //
    "  arena (Arena): Where the tape lands, rewound by this call.\n"                             //
    "  collection (UUID | str): The pages collection, by identifier or by name.\n"               //
    "  elements (KeyLike | Sequence[KeyLike] | buffer, optional): The element every task\n"      //
    "    renders with its subtree, where Nil or None renders the whole page.\n"                  //
    "  outputs_encoding (PageEncoding, optional): `PageEncoding.XML` for markup, or\n"           //
    "    `PageEncoding.TEXT` for the text of every run in document order.\n"                     //
    "Returns:\n"                                                                                 //
    "  Values: One rendering per task, None where the page or the element is absent.\n"          //
    "Raises:\n"                                                                                  //
    "  UnsupportedError: The encoding is HTML, Markdown or JSON.";

PyObject* ustore_py_api_pages_read(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                   PyObject* args_names_tuple) {
    ustore_py_context_t context;
    if (ustore_py_context_from_object(self, &context) != 0) return NULL;
    ustore_py_verb_options_t options;
    page_keywords_t keywords;
    if (page_keywords_from_args(&context, "pages_read", args, positional_args_count, args_names_tuple, 2, read_keywords,
                                &options, &keywords) != 0)
        return NULL;

    read_call_t stack;
    read_call_t* const call = ustore_py_call_open(&context, &read_verb, &stack);
    if (!call) return NULL;
    call->tasks.count = 1;
    if (spread_keys_from_object(args[0], "pages", &call->tasks, &call->pages) != 0 ||
        spread_keys_from_object(keywords.elements, "elements", &call->tasks, &call->elements) != 0 ||
        ustore_py_arena_claim(args[1], &call->call.arena) != 0)
        return ustore_py_call_discard(&context, &read_verb, call);
    call->call.collection = options.collection;
    ustore_pages_read_t read = {
        .database = context.database_handle,
        .executor = ustore_py_context_executor(&context),
        .outcome = &call->call.outcome,
        .completion = context.completion,
        .view = context.view,
        .count = call->tasks.count,
        .collections = {&call->call.collection, 0},
        .keys = spread_keys_view(&call->pages),
        .elements = spread_keys_view(&call->elements),
        .presences = &call->presences,
        .values = &call->values,
        .arena = ustore_py_arena_handle(call->call.arena),
        .outputs_encoding = (ustore_page_encoding_t)keywords.outputs_encoding,
    };
    return ustore_py_call_run(&context, &read_verb, call, &read);
}

char const doc_pages_scan[] =                                                                   //
    "pages_scan(start, wanted, arena, *, collection, mode=ScanMode.ORDERED, progress=None,\n"   //
    "           filter=None, predicate=None, predicate_cost=PredicateCost.CHEAP)\n"             //
    "answers a Keys tape.\n"                                                                    //
    "\n"                                                                                        //
    "Lists up to `wanted` page keys from `start` upwards, as `scan` lists the keys of blobs.\n" //
    "Pages are not rendered, so a `pages_read` follows the scan.\n"                             //
    "\n"                                                                                        //
    "Args:\n"                                                                                   //
    "  start (KeyLike): The smallest key the scan may return.\n"                                //
    "  wanted (int): Most keys the scan returns.\n"                                             //
    "  arena (Arena): Where the keys land, rewound by this call.\n"                             //
    "  collection (UUID | str): The pages collection, by identifier or by name.\n"              //
    "  mode (ScanMode, optional): In what order the keys come, as `scan` takes it.\n"           //
    "  progress (Callable[[int, int], bool | None], optional): Called with the scans\n"         //
    "    finished and the scans in all; a falsy answer other than None cancels the call.\n"     //
    "  filter (str | bytes, optional): A path in StringZilla's XPath subset, such as\n"         //
    "    '/doc/b[2]', that every listed page holds a node of.\n"                                //
    "Returns:\n"                                                                                //
    "  Keys: One run of keys, in ascending order.\n"                                            //
    "Raises:\n"                                                                                 //
    "  CancelledError: The progress callable stopped the scan.";

PyObject* ustore_py_api_pages_scan(PyObject* self, PyObject* const* args, Py_ssize_t positional_args_count,
                                   PyObject* args_names_tuple) {
    return ustore_py_api_scan_of(self, args, positional_args_count, args_names_tuple, "pages_scan",
                                 ustore_modality_pages_k);
}

#pragma endregion Reads
