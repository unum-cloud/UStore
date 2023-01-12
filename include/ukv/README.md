# UKV C Standard

The world of C can be tricky.
It is the lingua-france of computing, so we wanted to make the bindings as flexible as possible, for all other runtimes to be built on top of it with minimal type-casting costs.

## Opening the database

Every function on our API receives just one argument - the task description.
Some tasks have a lot of attributes, but most have default values, so you can avoid the boilerplate.

```c
ukv_database_t db { NULL };
ukv_error_t error { NULL };
ukv_database_init_t init {
    .db = &db,
    .config = "{}",
    .error = &error,
};
ukv_database_init(&init);
```

The passed `config` must be a valid JSON string.
Every function (except for deallocators) can fail, so it must be given an `ukv_error_t` object pointer, in which the error message will be written.

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
ukv_key_t key { 42 };
ukv_bytes_cptr_t value { "meaning of life" };
ukv_write_t write {
    .db = db,
    .keys = &key,
    .values = &value,
    .error = &error,
};
ukv_write(&write);
```

Similarly, submitting a batch may look like:

```c
ukv_key_t keys[2] = { 42, 43 };
ukv_bytes_cptr_t values[2] { "meaning of life", "is unknown" };
ukv_write_t write {
    .db = db,
    .tasks_count = 2,
    .keys = keys,
    .keys_stride = sizeof(ukv_key_t),
    .values = values,
    .values_stride = sizeof(ukv_bytes_cptr_t),
    .error = &error,
};
ukv_write(&write);
```

The beauty of strides is that your data may have an Array-of-Structures layout, rather than Structure-of-Arrays.
Like this:

```c
struct pair_t {
    ukv_key_t key;
    ukv_bytes_cptr_t value;
};
pair_t pairs[2] = {
    { 42, "meaning of life" },
    { 43, "is unknown" },
};
ukv_write_t write {
    .db = db,
    .tasks_count = 2,
    .keys = &pairs[0].key,
    .keys_stride = sizeof(pair_t),
    .values = &pairs[0].value,
    .values_stride = sizeof(pair_t),
    .error = &error,
};
ukv_write(&write);
```

To delete an entry, one can perform a similar trick:

```c
ukv_key_t key { 42 };
ukv_bytes_cptr_t value { NULL };
ukv_write_t write {
    .db = db,
    .keys = &key,
    .values = &value,
    .error = &error,
};
ukv_write(&write);
```

Or just pass `.values = NULL` all together.

## Reads

The read interface is similarly flexible.
To retrieve the value stored under key 42, we can:

```c
ukv_key_t key { 42 };
ukv_bytes_ptr_t values { NULL };
ukv_length_t *lengths { NULL };
ukv_read_t read {
    .db = db,
    .keys = &key,
    .values = &values,
    .lengths = &lengths,
    .error = &error,
};
ukv_read(&read);
assert(strncmp(values, "meaning of life", lengths[0]) == 0);
```

We may not want to export the value.
In that case we can only request the lengths:

```c
ukv_read_t read {
    .db = db,
    .keys = &key,
    .values = NULL,
    .lengths = &lengths,
    .error = &error,
};
ukv_read(&read);
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

## ACID Transactions

Writing a batch of values is always atomic in UKV.
If one operation in the batch fails - all do.
But if you need more complex logic like Read-Modify-Write or Compare-And-Swap, you need "Transactions".
To create one:

```c
ukv_transaction_t transaction { NULL };
ukv_transaction_init_t transaction_init {
    .db = db,
    .transaction = &transaction,
    .error = &error,
};
ukv_transaction_init(&transaction_init);
```

Now let's understand what ACI(D)-ity means and what "Transactions" can do fo us.

### A: Atomicity

Atomicity is always guaranteed.
Even on non-transactional writes - either all updates pass or all fail.

### C: Consistency

Consistency is implemented in the strictest possible form - ["Strict Serializability"][ss] meaning that:

* reads are ["Serializable"][s],
* writes are ["Linearizable"][l].

The default behavior, however, can be tweaked at the level of specific operations.
For that the `::ukv_option_transaction_dont_watch_k` can be passed to `ukv_transaction_init()` or any transactional read/write operation, to control the consistency checks during staging.

|                                      |     Reads     |    Writes     |
| :----------------------------------- | :-----------: | :-----------: |
| Head                                 | Strict Serial | Strict Serial |
| Transactions over [Snapshots][snap]  |    Serial     | Strict Serial |
| Transactions w/out [Snapshots][snap] | Strict Serial | Strict Serial |
| Transactions w/out Watches           | Strict Serial |  Sequential   |

If this topic is new to you, please check out the [Jepsen.io][jepsen] blog on consistency.

[ss]: https://jepsen.io/consistency/models/strict-serializable
[s]: https://jepsen.io/consistency/models/serializable
[l]: https://jepsen.io/consistency/models/linearizable
[jepsen]: https://jepsen.io/consistency
[snap]: #snapshots

### I: Isolation

|                                      | Reads | Writes |
| :----------------------------------- | :---: | :----: |
| Transactions over [Snapshots][snap]  |   ✓   |   ✓    |
| Transactions w/out [Snapshots][snap] |   ✗   |   ✓    |

### D: Durability

Durability doesn't apply to in-memory systems by definition.
In hybrid or persistent systems we prefer to disable it by default.
Almost every DBMS that builds on top of KVS prefers to implement its own durability mechanism.
Even more so in distributed databases, where three separate Write Ahead Logs may exist:

* in KVS,
* in DBMS,
* in Distributed Consensus implementation.

If you still need durability, flush writes on `ukv_transaction_commit()` with `::ukv_option_write_flush_k`.

## Documents

Document collections are meant to replace DBs like MongoDB.
The standard doesn't force the implementation to stick to any internal implementation, but it has to be able to import and export JSONs, BSONs and MessagePacks, as the most commonly used document serialization forms.

Following functions are provided:

* `ukv_docs_write()`: Adding data.
* `ukv_docs_read()`: Retrieving data.
* `ukv_docs_gist()`: Introspecting docs structure.
* `ukv_docs_gather()`: Exporting tables.

## Graphs

Graph collections are meant to replace DBs like Neo4J.
Graph interfaces is extremely short:

* `ukv_graph_find_edges()`: The only lookup function you need.
* `ukv_graph_upsert_edges()`: Adding edges, upserting nodes.
* `ukv_graph_remove_edges()`: Removing edges, but keeping nodes.
* `ukv_graph_remove_vertices()`: Removing vertices and related edges.

If you understand the BLOB interface, this requires no additional explanation.

## Paths

Paths are the same BLOB collections, except keys can strings.

* `ukv_paths_write()`: Adding data.
* `ukv_paths_read()`: Retrieving data.
* `ukv_paths_match()`: Prefix or RegEx matching across keys.

Current FOSS implementation of last function has linear complexity.

## Vectors

* add
* query.

Current FOSS implementation of last function has linear complexity.

## FAQ

### Integer Keys

As of the current version, 64-bit signed integers are used.
It allows unique keys in the range from `[0, 2^63)`.
128-bit builds with UUIDs can be considered, but variable-length keys are highly discouraged.
Why so?

Using variable length keys forces numerous limitations on the design of a Key-Value store.
Firstly, it implies slow character-wise comparisons — a performance killer on modern hyperscalar CPUs.
Secondly, it forces keys and values to be joined on a disk to minimize the needed metadata for navigation.
Lastly, it violates our simple logical view of KVS as a "persistent memory allocator", putting a lot more responsibility on it.

---

The recommended approach to dealing with string keys is:

1. Choose a mechanism to generate unique integer keys (UID). Ex: monotonically increasing values.
2. Use ["paths"](#paths) modality build up a persistent hash map of strings to UIDs.
3. Use those UIDs to address the rest of the data in binary, document and graph modalities.

This will result in a single conversion point from string to integer representations and will keep most of the system snappy and the C-level interfaces simpler than they could have been.

### Smallish Values

We can only address 4 GB values or smaller as of the current version.
Why?
Key-Value Stores are generally intended for high-frequency operations.
Frequently (thousands of times each second), accessing and modifying 4 GB and larger files is impossible on modern hardware.
So we stick to smaller length types, making using Apache Arrow representation slightly easier and allowing the KVS to compress indexes better.
