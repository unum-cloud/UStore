# UStore C Interface

UStore's primary interface is implemented in C99.
It makes UStore compatible with almost every higher-level language or runtime.
But if you want to use the C99 layer directly to maximize the performance, we recommend CMake for installation.

```cmake
FetchContent_Declare(
    ustore
    GIT_REPOSITORY https://github.com/unum-cloud/ustore.git
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(ustore)
include_directories(${ustore_SOURCE_DIR}/include)
```

## Opening the database

Every function on our API receives just one argument - the task description.
Some tasks have a lot of attributes, but most have default values, so you can avoid the boilerplate.

```c
ustore_database_t db { NULL };
ustore_error_t error { NULL };
ustore_database_init_t init {
    .db = &db,
    .config = "{}",
    .error = &error,
};
ustore_database_init(&init);
```

The passed `config` must be a valid JSON string.
Every function (except for deallocators) can fail, so it must be given an `ustore_error_t` object pointer, in which the error message will be written.

## Writes

Assuming `task_idx` is smaller than `tasks_count`, following approximates how the content chunks are sliced.

```cpp
std::size_t task_idx = ...;
auto chunk_begin = values + values_stride * task_idx;
auto offset_in_chunk = offsets + offsets_stride * task_idx;
auto length = lengths + lengths_stride * task_idx;
std::string_view content = { chunk_begin + offset_in_chunk, length };
```

It is not the simplest interface, but like Vulkan or BLAS, it gives you extreme flexibility.
Submitting a single write request can be as easy as:

```c
ustore_key_t key { 42 };
ustore_bytes_cptr_t value { "meaning of life" };
ustore_write_t write {
    .db = db,
    .keys = &key,
    .values = &value,
    .error = &error,
};
ustore_write(&write);
```

Similarly, submitting a batch may look like:

```c
ustore_key_t keys[2] = { 42, 43 };
ustore_bytes_cptr_t values[2] { "meaning of life", "is unknown" };
ustore_write_t write {
    .db = db,
    .tasks_count = 2,
    .keys = keys,
    .keys_stride = sizeof(ustore_key_t),
    .values = values,
    .values_stride = sizeof(ustore_bytes_cptr_t),
    .error = &error,
};
ustore_write(&write);
```

The beauty of strides is that your data may have an Array-of-Structures layout, rather than Structure-of-Arrays.
Like this:

```c
struct pair_t {
    ustore_key_t key;
    ustore_bytes_cptr_t value;
};
pair_t pairs[2] = {
    { 42, "meaning of life" },
    { 43, "is unknown" },
};
ustore_write_t write {
    .db = db,
    .tasks_count = 2,
    .keys = &pairs[0].key,
    .keys_stride = sizeof(pair_t),
    .values = &pairs[0].value,
    .values_stride = sizeof(pair_t),
    .error = &error,
};
ustore_write(&write);
```

To delete an entry, one can perform a similar trick:

```c
ustore_key_t key { 42 };
ustore_bytes_cptr_t value { NULL };
ustore_write_t write {
    .db = db,
    .keys = &key,
    .values = &value,
    .error = &error,
};
ustore_write(&write);
```

Or just pass `.values = NULL` all together.

## Reads

The read interface is similarly flexible.
To retrieve the value stored under key 42, we can:

```c
ustore_key_t key { 42 };
ustore_bytes_ptr_t values { NULL };
ustore_length_t *lengths { NULL };
ustore_read_t read {
    .db = db,
    .keys = &key,
    .values = &values,
    .lengths = &lengths,
    .error = &error,
};
ustore_read(&read);
assert(strncmp(values, "meaning of life", lengths[0]) == 0);
```

We may not want to export the value.
In that case we can only request the lengths:

```c
ustore_read_t read {
    .db = db,
    .keys = &key,
    .values = NULL,
    .lengths = &lengths,
    .error = &error,
};
ustore_read(&read);
```

Similarly, we can request `presences` boolean presence indicators or `offsets` within the exported `values` tape.
The `lengths` and `offsets` provide similar amount of information and can be used interchangeably.

### Offsets, Lengths and Tapes

In most object-oriented languages strings are represented by combination of a pointer and size.
Those directly match to `values` and `lengths`, especially if you operate with arrays/batches of such strings.

Columnar stores, however, pack variable length strings into tapes.
All strings are concatenated after each other, and are addressed by their `offsets` from the beginning.
In Apache Arrow, as in similar systems, for `N` items `N+1` offsets will be exported, to allow inferring the length of the last string.

## Snapshots

Snapshots are related to the topic of [Transactions](#acid-transactions).
They are a versioning mechanism for read operations, giving you ability to reference a certain state within the history of the database.

TODO:

## Transactions

Writing a batch of values is always atomic in UStore.
If one operation in the batch fails - all do.
But if you need more complex logic like Read-Modify-Write or Compare-And-Swap, you need "Transactions".
To create one:

```c
ustore_transaction_t transaction { NULL };
ustore_transaction_init_t transaction_init {
    .db = db,
    .transaction = &transaction,
    .error = &error,
};
ustore_transaction_init(&transaction_init);
```

## Documents

Document collections are meant to replace DBs like MongoDB.
The standard doesn't force the implementation to stick to any internal implementation, but it has to be able to import and export JSONs, BSONs and MessagePacks, as the most commonly used document serialization forms.

Following functions are provided:

- `ustore_docs_write()`: Adding data.
- `ustore_docs_read()`: Retrieving data.
- `ustore_docs_gist()`: Introspecting docs structure.
- `ustore_docs_gather()`: Exporting tables.

## Graphs

Graph collections are meant to replace DBs like Neo4J.
Graph interfaces is extremely short:

- `ustore_graph_find_edges()`: The only lookup function you need.
- `ustore_graph_upsert_edges()`: Adding edges, upserting nodes.
- `ustore_graph_remove_edges()`: Removing edges, but keeping nodes.
- `ustore_graph_remove_vertices()`: Removing vertices and related edges.

If you understand the BLOB interface, this requires no additional explanation.

## Paths

Paths are the same BLOB collections, except keys can strings.

- `ustore_paths_write()`: Adding data.
- `ustore_paths_read()`: Retrieving data.
- `ustore_paths_match()`: Prefix or RegEx matching across keys.

TODO: Current FOSS implementation of last function has linear complexity.
TODO: Rename `ustore_paths_match` to `ustore_paths_search`

## Vectors

- `ustore_vectors_write()`: Updating vectors.
- `ustore_vectors_read()`: Retrieving vectors.
- `ustore_vectors_search()`: Approximate Nearest Neighbors Search.

TODO: Current FOSS implementation of last function has linear complexity.
