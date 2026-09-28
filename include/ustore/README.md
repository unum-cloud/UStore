# UStore for C and C++

`include/ustore/` is the public interface of UStore, and the only one: every engine and every binding meets here.
The `*.h` headers declare a C99 binary interface that every engine implements — the in-memory SmashTable reference engine, the RocksDB and LevelDB key-value stores, and NVMe on NVMe as a v1 target — so any language with a C FFI can drive the database.
The `*.hpp` headers beside them wrap the same calls in C++ with RAII handles, batched references and typed ranges, and add no ABI of their own.

| Header     | Declares                                                                                          |
| :--------- | :------------------------------------------------------------------------------------------------ |
| `ustore.h` | Every C header below                                                                              |
| `db.h`     | Databases, collections, snapshots, transactions, arenas and errors                                |
| `blobs.h`  | Binary values: `ustore_write`, `ustore_read`, `ustore_scan`, `ustore_sample`, `ustore_measure`    |
| `docs.h`   | JSON documents: `ustore_docs_write`, `ustore_docs_read`, `ustore_docs_gist`, `ustore_docs_gather` |
| `graph.h`  | Graph edges and vertices                                                                          |

| Header                   | Declares                                                     |
| :----------------------- | :----------------------------------------------------------- |
| `ustore.hpp`             | Every C++ header below                                       |
| `db.hpp`                 | `database_t`, and `context_t` with its `transaction_t` alias |
| `blobs_collection.hpp`   | Binary collections                                           |
| `blobs_ref.hpp`          | Batched references to binary values                          |
| `blobs_range.hpp`        | Key ranges                                                   |
| `docs_collection.hpp`    | Document collections                                         |
| `docs_ref.hpp`           | Batched references to documents                              |
| `docs_table.hpp`         | Gathered tables                                              |
| `graph_collection.hpp`   | Graph collections                                            |
| `graph_stream.hpp`       | Adjacency streams                                            |
| `strings_collection.hpp` | String-keyed collections and their prefix walks              |
| `status.hpp`             | `status_code_t`, `status_t` and `expected`                   |
| `types.hpp`              | Value views and arenas                                       |
| `ranges.hpp`             | Strided ranges                                               |
| `ranges_args.hpp`        | Argument adapters                                            |

## Installation

UStore builds with CMake into one library, `ustore`, holding the in-memory engine and every engine the build enables through `USTORE_WITH_ROCKSDB`, `USTORE_WITH_LEVELDB` and `USTORE_BUILD_NVME`.
Each engine is its own object of the archive, so a program links only the engines it opens.

```cmake
FetchContent_Declare(
    ustore
    GIT_REPOSITORY https://github.com/unum-cloud/UStore.git
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(ustore)
target_link_libraries(your_target PRIVATE ustore::ustore)
```

## C Interface

### Conventions

Every call takes a single pointer to an argument struct named after it with a `_t` suffix, so arguments are named and every omitted one is zero.
Every fallible call reports through `error`, a `ustore_error_t` whose `status` names the failure and whose static `message` is never freed.
Reads export into an arena that UStore allocates and grows, which the caller frees once with `ustore_arena_free`.
The next call through the same arena discards what the previous one exported, unless it passes `ustore_option_dont_discard_memory_k`.
Every call is a batch of `tasks_count` tasks, with each input array walked by its own stride in bytes, and a zero stride repeats one element for every task.
Keys are `ustore_key_t`, a signed 64-bit integer.

The examples below share these three handles:

```c
ustore_database_t db = NULL;
ustore_status_t status = {0};
ustore_arena_t arena = NULL;
```

### Opening the Database

```c
#include <ustore/ustore.h>

ustore_database_init_rocksdb_t init = {
    .db = &db,
    .error = &error,
    .directory = "/var/lib/ustore",
};
ustore_database_init_rocksdb(&init);
if (error.status != ustore_success_k)
    fprintf(stderr, "%s\n", error.message);

ustore_arena_free(arena);
ustore_database_free(db);
```

Each engine opens through its own call, `ustore_database_init_ram`, `_rocksdb`, `_leveldb` or `_nvme`, whose arguments hold what that engine can be asked, and a zero field keeps the engine's default.
Every freeing function accepts `NULL`.
`ustore_database_capabilities` reports whether the engine supports transactions, collections and snapshots.

### Collections

Every database has a main collection, `ustore_collection_main_k`, which a `NULL` collections array addresses.
A collection is created with an optional name and the `ustore_modality_t` it holds, and addressed afterwards by the identifier the call answers:

```c
ustore_collection_t movies = 0;
ustore_collection_create_t create = {
    .db = db,
    .error = &error,
    .name = "movies",
    .modality = ustore_modality_docs_k,
    .id = &movies,
};
ustore_collection_create(&create);

ustore_collection_drop_t drop = {
    .db = db,
    .error = &error,
    .id = movies,
    .mode = ustore_drop_keys_vals_handle_k,
};
ustore_collection_drop(&drop);
```

Each engine records names apart from every collection, so no identifier reaches them, and a name resolves in two phases: once through `ustore_collection_find`, which answers the identifier and the modality of every name of a batch at the head or at a snapshot, and by identifier from then on, as no other call carries one.
A create records the name in the same step as the collection, refusing a name another collection carries even under a concurrent creator.
The main collection is never removed, so only `ustore_drop_keys_vals_k` and `ustore_drop_vals_k` apply to it, and dropping any other collection whole forgets its name before the collection goes.
`ustore_collection_list` exports the identifier of every other collection and, where asked, its modality and its name, which is empty for a collection created without one.

### Writes

For task `index`, `ustore_write` slices the written value out of its strided arrays like this:

```c
ustore_bytes_cptr_t chunk = *(ustore_bytes_cptr_t const*)((char const*)values + values_stride * index);
ustore_length_t offset = offsets ? *(ustore_length_t const*)((char const*)offsets + offsets_stride * index) : 0;
ustore_length_t length = *(ustore_length_t const*)((char const*)lengths + lengths_stride * index);
```

The value is `length` bytes starting at `chunk + offset`.
Without `lengths`, each length comes from consecutive `offsets`, and without either it comes from a null terminator.
A single write looks like this:

```c
ustore_key_t key = 42;
ustore_bytes_cptr_t value = (ustore_bytes_cptr_t)"meaning of life";
ustore_write_t write = {
    .db = db,
    .error = &error,
    .arena = &arena,
    .tasks_count = 1,
    .keys = &key,
    .values = &value,
};
ustore_write(&write);
```

A batch passes arrays with their strides:

```c
ustore_key_t keys[2] = {42, 43};
ustore_bytes_cptr_t values[2] = {(ustore_bytes_cptr_t)"meaning of life", (ustore_bytes_cptr_t)"is unknown"};
ustore_write_t write = {
    .db = db,
    .error = &error,
    .arena = &arena,
    .tasks_count = 2,
    .keys = keys,
    .keys_stride = sizeof(ustore_key_t),
    .values = values,
    .values_stride = sizeof(ustore_bytes_cptr_t),
};
ustore_write(&write);
```

Strides let the same batch come from an array of structures without copying it into separate arrays:

```c
typedef struct pair_t {
    ustore_key_t key;
    ustore_bytes_cptr_t value;
} pair_t;

pair_t pairs[2] = {
    {42, (ustore_bytes_cptr_t)"meaning of life"},
    {43, (ustore_bytes_cptr_t)"is unknown"},
};
ustore_write_t write = {
    .db = db,
    .error = &error,
    .arena = &arena,
    .tasks_count = 2,
    .keys = &pairs[0].key,
    .keys_stride = sizeof(pair_t),
    .values = &pairs[0].value,
    .values_stride = sizeof(pair_t),
};
ustore_write(&write);
```

A `NULL` chunk removes its key, and `.values = NULL` removes every key of the batch.
A batch is atomic: if one task fails, none is applied.

### Reads

```c
ustore_key_t key = 42;
ustore_bytes_ptr_t values = NULL;
ustore_length_t* lengths = NULL;
ustore_read_t read = {
    .db = db,
    .error = &error,
    .arena = &arena,
    .tasks_count = 1,
    .keys = &key,
    .lengths = &lengths,
    .values = &values,
};
ustore_read(&read);
assert(lengths[0] == 15 && memcmp(values, "meaning of life", lengths[0]) == 0);
```

Every output is optional, so a read that leaves `.values` out exports only lengths, and `.presences` exports a bitmap of the keys that are present.
A missing key reads with the length `ustore_length_missing_k`.

#### Offsets, Lengths and Tapes

Exported values are joined into one tape instead of being allocated one by one.

- With only `offsets`, entries follow in order without gaps, and `tasks_count + 1` offsets are exported so the last length can be inferred; empty and missing entries look alike unless `presences` are requested too.
- With only `lengths`, entries follow in order without gaps.
- With both, entries may sit in the tape in any order.

### Scans

`ustore_scan` exports up to `count_limits[index]` keys starting from `start_keys[index]`, or from the smallest key after it, and exports no values:

```c
ustore_key_t start = 0;
ustore_length_t limit = 100;
ustore_length_t* counts = NULL;
ustore_key_t* found = NULL;
ustore_scan_t scan = {
    .db = db,
    .error = &error,
    .arena = &arena,
    .tasks_count = 1,
    .start_keys = &start,
    .count_limits = &limit,
    .counts = &counts,
    .keys = &found,
};
ustore_scan(&scan);
```

The next page starts from the last exported key plus one, and a `ustore_read` of the found keys fetches their values.
`ustore_sample` draws keys uniformly at random, and `ustore_measure` bounds the cardinality, value bytes and disk usage between two keys from below and above.

### Transactions

A transaction groups reads and writes across collections and modalities, and commits all of them or none.
Reads inside it register their keys, and the commit fails if another writer changed any of them since; `ustore_option_transaction_dont_watch_k` skips that registration.

```c
ustore_transaction_t transaction = NULL;
ustore_transaction_init_t transaction_init = {
    .db = db,
    .error = &error,
    .transaction = &transaction,
};
ustore_transaction_init(&transaction_init);

write.transaction = transaction;
ustore_write(&write);

ustore_transaction_commit_t commit = {
    .db = db,
    .error = &error,
    .transaction = transaction,
};
ustore_transaction_commit(&commit);
ustore_transaction_free(transaction);
```

A failed commit keeps the contents of the transaction for a retry, and `ustore_transaction_init` on an existing handle resets it while reusing its memory.
`ustore_transaction_stage` stages the transaction for a two-phase commit, and both calls can export a `sequence_number`.

### Snapshots

A snapshot pins a point-in-time view of the whole database, which reads select through their `snapshot` argument:

```c
ustore_snapshot_t snapshot = 0;
ustore_snapshot_create_t snapshot_create = {
    .db = db,
    .error = &error,
    .id = &snapshot,
};
ustore_snapshot_create(&snapshot_create);

read.snapshot = snapshot;
ustore_read(&read);

ustore_snapshot_drop_t snapshot_drop = {
    .db = db,
    .error = &error,
    .id = snapshot,
};
ustore_snapshot_drop(&snapshot_drop);
```

`ustore_snapshot_list` lists the live snapshots, and `ustore_snapshot_export` writes one out as a new database under a directory.

### Documents

A document collection stores JSON text, so reading a document back through `ustore_read` may not return the exact bytes written.
A field addresses a value inside a document, either as a top-level key such as `_id` or as an RFC 6901 JSON Pointer such as `/posts/0/text`.

- `ustore_docs_write` upserts, updates or inserts whole documents or fields, or applies RFC 6902 patches or RFC 7386 merge patches, as its `modification` selects.
- `ustore_docs_read` reads whole documents or fields, as JSON or as values printed into text.
- `ustore_docs_gist` lists the unique JSON Pointers to the leaves of the requested documents.
- `ustore_docs_gather` reads many fields of many documents into typed columns, with validity, conversion and collision bitmaps per column.

```c
ustore_key_t key = 1;
ustore_str_view_t document = "{\"person\": \"Alice\", \"age\": 27}";
ustore_docs_write_t docs_write = {
    .db = db,
    .error = &error,
    .arena = &arena,
    .tasks_count = 1,
    .type = ustore_doc_field_json_k,
    .modification = ustore_doc_modify_upsert_k,
    .keys = &key,
    .values = (ustore_bytes_cptr_t const*)&document,
};
ustore_docs_write(&docs_write);

ustore_str_view_t field = "/person";
ustore_bytes_ptr_t person = NULL;
ustore_length_t* lengths = NULL;
ustore_docs_read_t docs_read = {
    .db = db,
    .error = &error,
    .arena = &arena,
    .type = ustore_doc_field_str_k,
    .tasks_count = 1,
    .keys = &key,
    .fields = &field,
    .lengths = &lengths,
    .values = &person,
};
ustore_docs_read(&docs_read);
```

Without `keys`, `ustore_docs_write` reads the key of every document from its `id_field`, which must hold an integer.

### Graphs

A graph collection stores edges as triplets of a source, a target and an edge identifier, indexed from both ends.
Several edges between the same two vertices need distinct edge identifiers, and an edge without one gets `ustore_default_edge_id_k`.

- `ustore_graph_upsert_edges` adds edges together with their vertices.
- `ustore_graph_upsert_vertices` adds vertices without edges.
- `ustore_graph_find_edges` exports the degree and the edges of every requested vertex in the requested role.
- `ustore_graph_remove_edges` removes edges and keeps their vertices.
- `ustore_graph_remove_vertices` removes vertices together with all their edges.

```c
typedef struct triplet_t {
    ustore_key_t source;
    ustore_key_t target;
    ustore_key_t id;
} triplet_t;

triplet_t triplets[2] = {{1, 2, 9}, {2, 3, 10}};
ustore_graph_upsert_edges_t upsert = {
    .db = db,
    .error = &error,
    .arena = &arena,
    .tasks_count = 2,
    .edges_ids = &triplets[0].id,
    .edges_stride = sizeof(triplet_t),
    .sources_ids = &triplets[0].source,
    .sources_stride = sizeof(triplet_t),
    .targets_ids = &triplets[0].target,
    .targets_stride = sizeof(triplet_t),
};
ustore_graph_upsert_edges(&upsert);

ustore_key_t vertex = 2;
ustore_vertex_role_t role = ustore_vertex_role_any_k;
ustore_vertex_degree_t* degrees = NULL;
ustore_key_t* edges = NULL;
ustore_graph_find_edges_t find = {
    .db = db,
    .error = &error,
    .arena = &arena,
    .tasks_count = 1,
    .vertices = &vertex,
    .roles = &role,
    .degrees_per_vertex = &degrees,
    .edges_per_vertex = &edges,
};
ustore_graph_find_edges(&find);
```

Every found edge takes three keys of `edges`: its source, its target and its identifier.
Outgoing edges come first, sorted by target, then incoming edges, sorted by source, and a missing vertex has the degree `ustore_vertex_degree_missing_k`.
Roles matter only in directed graphs, where `ustore_vertex_source_k` and `ustore_vertex_target_k` select one direction.

## C++ Interface

### Conventions

Everything lives in `unum::ustore`, and `#include <ustore/ustore.hpp>` brings in all of it.
Fallible calls return a `status_t`, a `status_code_t` beside a static message, or an `expected` that holds either a value or the `status_t` explaining its absence; both are `[[nodiscard]]`.
Dereferencing an `expected<value_type>` yields the `value_type`, and `auto [value, status]` decomposes one whose value is default constructible.
Nothing copies values out of the arena, and the few calls that throw are marked `noexcept(false)`.

```cpp
using namespace unum::ustore;

database_t db;
if (status_t const status = db.open(); status.failed())
    std::fprintf(stderr, "%s\n", status.message);
```

### Blobs

A collection handle is cheap to construct, and its `[]` operator returns a `blobs_ref` over one key or a batch of keys that reads and writes through the C interface:

```cpp
blobs_collection_t main = db.main();

status_t status = main[42].assign("purpose of life");
expected<value_view_t> value = main[42].value();
bool const matches = value && *value == "purpose of life";

status = main[{43, 44}].assign("same value");
auto present = main[{43, 44}].present();
auto lengths = main[{43, 44}].length();
status = main[43].erase();

std::array<ustore_key_t, 3> keys {65, 66, 67};
auto values = main[keys].value();
for (value_view_t found : *values)
    std::printf("%zu bytes\n", found.size());
```

A single key yields a `value_view_t`, a `bool` presence and a length, while a batch yields `embedded_blobs_t`, `bits_span_t` and a range of lengths.
`erase` removes the keys, while `clear` keeps them and empties their values.
`keys` and `items` range over a collection in key order, and `size_range` estimates how many keys it holds.

### Named Collections and Transactions

```cpp
expected<docs_collection_t> people = db.find_or_create<docs_collection_t>("people");

expected<transaction_t> transaction = db.transact();
blobs_collection_t inside = transaction->main();
status = inside[42].assign("written inside a transaction");
status = transaction->commit();

expected<context_t> snapshot = db.snapshot();
auto pinned = snapshot->main()[42].value();
```

A handle taken from a transaction or a snapshot reads and writes through it, and a transaction commits only when `commit` is called.

### Arenas

Every collection and transaction handle owns one arena, which all references produced from it share.
Threads sharing one handle, or reads interleaved with new requests, pass their own `arena_t` through `on`:

```cpp
arena_t arena(db);
auto lengths = main[{43, 44}].on(arena).length();
```

The exported ranges stay valid until the next call through the same arena.

### Documents

A `docs_collection_t` hands out `docs_ref` proxies, which read and write JSON through `ustore_docs_read` and `ustore_docs_write`:

```cpp
docs_collection_t people = db.main<docs_collection_t>();
status_t status = people[1].assign(R"({"person": "Alice", "age": 27, "height": 1})");
status = people[2].assign(R"({"person": "Bob", "age": "27", "weight": 2})");

expected<value_view_t> document = people[1].value();
expected<value_view_t> person = people[collection_key_field_t {1, "/person"}].value(ustore_doc_field_str_k);

status = people[1].patch(R"([{"op": "replace", "path": "/age", "value": 28}])");
status = people[2].merge(R"({"weight": null})");
expected<joined_strs_t> paths = people[{1, 2}].gist();
```

Besides `assign`, a reference offers `insert`, `update` and `upsert`, and `as` switches the format every later call exchanges.

### Tables

`gather` reads fields of many documents into a `docs_table`, converting each field into its column type on a best-effort basis.
A header whose column types are known at compile time is built column by column:

```cpp
auto header = make_table_header() //
                  .with<std::int32_t>("age")
                  .with<std::string_view>("age")
                  .with<std::string_view>("person");

std::array<ustore_key_t, 3> keys {1, 2, 3};
auto table = people[keys].gather(header);
auto ages = table->column<0>();
for (std::size_t row = 0; row != ages.size(); ++row)
    if (ages[row].valid)
        std::printf("%d\n", ages[row].value);
```

A header whose column types are known only at runtime views an array of `field_type_t`, and its columns are typed at the point of use:

```cpp
field_type_t const columns[] = {
    {"age", ustore_doc_field_i32_k},
    {"age", ustore_doc_field_str_k},
    {"person", ustore_doc_field_str_k},
};
auto dynamic = people[keys].gather(make_table_header(columns));
auto names = dynamic->column(2).as<std::string_view>();
```

Every cell carries `valid`, `converted` and `collides` flags beside its `value`, which is meaningful only when `valid`.

### Graphs

A `graph_collection_t` batches edges and vertices, and can be used from inside a transaction like any other handle:

```cpp
graph_collection_t graph = db.main<graph_collection_t>();
status_t status = graph.upsert_edge(edge_t {1, 2, 9});

std::array<edge_t, 2> triplets {edge_t {2, 3, 10}, edge_t {3, 4, 11}};
status = graph.upsert_edges(edges(triplets));

expected<ustore_vertex_degree_t> degree = graph.degree(2);
auto successors = graph.successors(2);
auto between = graph.edges_between(1, 2);
auto touching = graph.edges_containing(3, ustore_vertex_target_k);
```

`edges` streams every edge of the collection, and `vertex_stream` every vertex, both reading ahead in batches.
