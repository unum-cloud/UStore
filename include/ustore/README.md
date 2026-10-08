# UStore for C and C++

`include/ustore/` is the public interface of UStore, and the only one: every engine and every binding meets here.
The `*.h` headers declare a C99 binary interface that every engine implements — the in-memory SmashTable reference engine, the RocksDB and LevelDB key-value stores, and NVMe on NVMe as a v1 target — so any language with a C FFI can drive the database.
The `*.hpp` headers beside them wrap the same calls in C++ with RAII handles, batched references and typed ranges, and add no ABI of their own.

| Header      | Declares                                                                                          |
| :---------- | :------------------------------------------------------------------------------------------------ |
| `ustore.h`  | Every C header below                                                                              |
| `types.h`   | Statuses, outcomes, completion modes, arenas, progress callbacks and executors                    |
| `db.h`      | Databases, collections, snapshots, transactions and views                                         |
| `blobs.h`   | Binary values: `ustore_write`, `ustore_remove`, `ustore_read`, `ustore_scan`, `ustore_sample`     |
| `strings.h` | String-keyed values: `ustore_strings_write`, `_remove`, `_read` and `_scan`                       |
| `docs.h`    | JSON documents: `ustore_docs_write`, `ustore_docs_read`, `ustore_docs_gist`, `ustore_docs_gather` |
| `graph.h`   | Graph edges and vertices                                                                          |
| `pages.h`   | Ordered documents: one verb per edit, `ustore_pages_read` and `ustore_pages_history`              |

| Header                   | Declares                                                  |
| :----------------------- | :-------------------------------------------------------- |
| `ustore.hpp`             | Every C++ header below                                    |
| `db.hpp`                 | `database_t`, `transaction_t`, `snapshot_t` and receivers |
| `blobs_collection.hpp`   | Binary collections                                        |
| `blobs_ref.hpp`          | Batched references to binary values                       |
| `blobs_range.hpp`        | Key ranges                                                |
| `docs_collection.hpp`    | Document collections                                      |
| `docs_ref.hpp`           | Batched references to documents                           |
| `docs_table.hpp`         | Gathered tables                                           |
| `graph_collection.hpp`   | Graph collections                                         |
| `graph_stream.hpp`       | Adjacency streams                                         |
| `strings_collection.hpp` | String-keyed collections and their prefix walks           |
| `status.hpp`             | `status_code_t`, `status_t` and `expected`                |
| `types.hpp`              | Value views and arenas                                    |
| `ranges.hpp`             | Strided ranges and argument adapters                      |

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
Every struct opens with its handle, then `executor`, `outcome` and `completion`, so every call reports through a `ustore_outcome_t` whose `status` names the result and whose static `message` is never freed.
Statuses read `ustore_status_<name>_k`, from `ustore_status_success_k` through every failure.
Every call is a batch of `count` tasks, with each input array walked by its own stride in bytes, and a zero stride repeats one element for every task.
Keys are `ustore_key_t`, 16 bytes of a UUID compared bytewise, and a non-negative integer spells as its 128-bit big-endian bytes, as Python's `uuid.UUID(int=...)` does, so keys made from integers order as the integers.

The examples below share these handles:

```c
ustore_database_t database = NULL;
ustore_executor_t executor = NULL;
ustore_outcome_t outcome = {0};
ustore_arena_t arena = NULL;
```

### Executors, Blocking and Deferred Calls

An executor owns the threads that run calls and the scratch they use, and a `NULL` executor runs a call on the calling thread.
`ustore_executor_for_ram` spawns its threads, every core for zero, and holds up to `deferred_limit` deferred calls at once, 256 for zero.
`ustore_executor_on_pool` borrows a ForkUnion pool another library owns and claims it through `fu_pool_claim` around every batch, so the lender and every other library on that pool claim it around their own dispatches too.
A call blocks by default and returns with its outcome final, while `ustore_completion_deferred_k` returns once the call is submitted:

```c
ustore_executor_for_ram(&(ustore_executor_for_ram_t){.executor = &executor, .outcome = &outcome, .threads = 0});

ustore_outcome_t outcomes[4] = {{0}};
for (size_t i = 0; i != 4; ++i) {
    reads[i].executor = executor, reads[i].outcome = &outcomes[i];
    reads[i].completion = ustore_completion_deferred_k;
    ustore_read(&reads[i]);
}
for (size_t i = 0; i != 4; ++i) ustore_outcome_wait(&outcomes[i]);
```

A deferred call's status reads `ustore_status_deferred_k` until it finishes; the library then stores the message and the final status with release ordering and touches nothing the struct names after that, and the caller reads it with acquire ordering, by waiting or polling.
Calls on one executor start in submission order, and a blocking call runs alone: after every call submitted before it finishes, and before any submitted after it starts.
A blocking call on an executor runs on the calling thread, while deferred calls run in batches on the executor's threads.
`ustore_executor_drain` waits for every call submitted before it began, and sets whether the threads park or spin once idle.
A call is refused with `ustore_status_contended_k` rather than waited for when the executor's deferred table is full, when its arena or its transaction serves another call, or when it drains or blocks from a callback its own executor runs.
Freeing a database, transaction or snapshot waits until every deferred call naming it finishes, so a binding may drop its handle before those calls finish, but never from a callback of one of them.
A progress callback never waits on a call its own executor has not finished, and reads the status instead.
The C++ wrappers and the Rust and Python bindings choose the executor and the completion by receiver, as `database.on(executor)` and `database.deferred(executor)`, so blocking calls read as they would without executors; the Design sections of `rust/README.md` and `python/README.md` state the rules every binding keeps.
A call stops early only when its progress callback returns `ustore_cancel_k`.

### Opening the Database

```c
#include <ustore/ustore.h>

ustore_database_init_rocksdb(&(ustore_database_init_rocksdb_t){
    .database = &database,
    .outcome = &outcome,
    .directory = "/var/lib/ustore",
});
if (outcome.status != ustore_status_success_k)
    fprintf(stderr, "%s: %s\n", ustore_status_name(outcome.status), outcome.message);

ustore_arena_free(arena);
ustore_database_free(database);
ustore_executor_free(executor);
```

Each engine opens through its own call, `ustore_database_init_ram`, `_rocksdb`, `_leveldb` or `_nvme`, whose arguments hold what that engine can be asked, and a zero field keeps the engine's default.
Every freeing function accepts `NULL`.
`ustore_database_describe` fills a `ustore_database_description_t`, whose `capabilities` say whether the engine supports transactions, collections and snapshots.

### Collections

Every database has a main collection, `ustore_collection_main_k`, named by the nil UUID, which a `NULL` collections array addresses.
A collection is created with an optional name and the `ustore_modality_t` it holds, and addressed afterwards by the identifier the call answers, a UUIDv7 the catalog mints once and never reuses:

```c
ustore_collection_id_t movies = {0};
ustore_collection_create(&(ustore_collection_create_t){
    .database = database,
    .outcome = &outcome,
    .name = "movies",
    .modality = ustore_modality_docs_k,
    .collection = &movies,
});

ustore_collection_drop(&(ustore_collection_drop_t){
    .database = database,
    .outcome = &outcome,
    .collection = movies,
    .mode = ustore_drop_collection_k,
});
```

Each engine records names apart from every collection, so no identifier reaches them, and a name resolves in two phases: once through `ustore_collection_find`, which answers the identifier and the modality of every name of a batch at the head or at a snapshot, and by identifier from then on, as no other call carries one.
Collections are created and dropped only at the head, so their names are never read inside a transaction, and resolving one never joins the conflicts of the work that follows it.
A create records the name in the same step as the collection, refusing a name another collection carries even under a concurrent creator.
The main collection is never removed, so only `ustore_drop_pairs_k` and `ustore_drop_values_k` apply to it, and dropping any other collection whole forgets its name before the collection goes.
`ustore_collection_list` exports the identifier of every other collection and, where asked, its modality and its name, which is empty for a collection created without one.
A listing describes the database rather than a transaction, so it reads the head or a snapshot.

### Writes and Removals

For task `index`, `ustore_write` slices the written value out of its strided arrays like this:

```c
ustore_bytes_cptr_t chunk = *(ustore_bytes_cptr_t const*)((char const*)values + values_stride * index);
ustore_length_t start = starts ? *(ustore_length_t const*)((char const*)starts + starts_stride * index) : 0;
ustore_length_t length = *(ustore_length_t const*)((char const*)lengths + lengths_stride * index);
```

The value is `length` bytes starting at `chunk + start`.
Without `lengths`, each value ends where the next one starts, so `starts` then holds `count + 1` entries, the last one closing the final value.
A single write looks like this:

```c
ustore_key_t key = {.uuid.bytes[15] = 42};
ustore_bytes_cptr_t value = (ustore_bytes_cptr_t)"meaning of life";
ustore_length_t length = 15;
ustore_write(&(ustore_write_t){
    .database = database,
    .outcome = &outcome,
    .count = 1,
    .keys = &key,
    .values = &value,
    .lengths = &length,
});
```

Strides let the same batch come from an array of structures without copying it into separate arrays:

```c
typedef struct pair_t {
    ustore_key_t key;
    ustore_bytes_cptr_t value;
    ustore_length_t length;
} pair_t;

pair_t pairs[2] = {
    {{.uuid.bytes[15] = 42}, (ustore_bytes_cptr_t)"meaning of life", 15},
    {{.uuid.bytes[15] = 43}, (ustore_bytes_cptr_t)"is unknown", 10},
};
ustore_write(&(ustore_write_t){
    .database = database,
    .outcome = &outcome,
    .count = 2,
    .keys = &pairs[0].key,
    .keys_stride = sizeof(pair_t),
    .values = &pairs[0].value,
    .values_stride = sizeof(pair_t),
    .lengths = &pairs[0].length,
    .lengths_stride = sizeof(pair_t),
});
```

Removing keys is its own call, so no `NULL` value ever deletes:

```c
ustore_remove(&(ustore_remove_t){.database = database, .outcome = &outcome, .count = 1, .keys = &key});
```

A batch is atomic: if one task fails, none is applied.
`durability` chooses whether the call returns once the write is buffered or once it is flushed.

### Reads and Arenas

Outputs whose size the inputs bound land in the caller's spans, like `count + 1` offsets or a bitmap of presences.
Outputs only the call can size, like the values themselves, land in an arena: memory UStore allocates and the caller passes back to the next call, which rewinds and reuses it, growing only when the new outputs outsize everything it held.

```c
ustore_key_t key = {.uuid.bytes[15] = 42};
ustore_length_t offsets[2];
ustore_byte_t* values = NULL;
ustore_read(&(ustore_read_t){
    .database = database,
    .outcome = &outcome,
    .count = 1,
    .keys = &key,
    .offsets = offsets,
    .values = &values,
    .arena = &arena,
});
assert(offsets[1] - offsets[0] == 15 && memcmp(values + offsets[0], "meaning of life", 15) == 0);
```

Values are joined into one tape as Apache Arrow lays them out: `count + 1` offsets, the last one closing the final value, and the bytes end to end.
Every output is optional, so a read that leaves `.values` out exports only offsets, and `.presences` exports a bitmap of the keys that are present.
The outputs stay valid until the next call that takes `&arena`; to keep them past it, pass that call another arena.
An arena serves one call at a time, and a call whose inputs point into the arena it is about to rewind refuses with `ustore_status_invalid_argument_k`.

### Scans

`ustore_scan` exports up to `count_limits[index]` keys starting from `start_keys[index]`, or from the smallest key after it, and exports no values:

```c
ustore_key_t start = {0};
ustore_length_t limit = 100;
ustore_length_t offsets[2];
ustore_key_t found[100];
ustore_scan(&(ustore_scan_t){
    .database = database,
    .outcome = &outcome,
    .count = 1,
    .start_keys = &start,
    .count_limits = &limit,
    .offsets = offsets,
    .keys = found,
});
```

The next page starts from the key right after the last exported one, its 16 bytes incremented as one big-endian integer, and a `ustore_read` of the found keys fetches their values.
`ustore_scan_unordered_k` trades key order for throughput, `ustore_sample` draws keys uniformly at random, and `ustore_measure` bounds the cardinality, value bytes and disk usage between two keys from below and above.

### Transactions and Views

A transaction groups reads and writes across collections and modalities, and commits all of them or none.
Writes take the transaction itself, while reads take a view of it, which also chooses whether the read registers its keys so the commit fails if another writer changed them since:

```c
ustore_transaction_t transaction = NULL;
ustore_transaction_init(&(ustore_transaction_init_t){
    .transaction = &transaction,
    .database = database,
    .outcome = &outcome,
});

ustore_write(&(ustore_write_t){.database = database, .outcome = &outcome, .transaction = transaction, /* ... */});
ustore_read(&(ustore_read_t){
    .database = database, .outcome = &outcome,
    .view = ustore_transaction_view(transaction, ustore_watch_keys_k), /* ... */});

ustore_transaction_commit(&(ustore_transaction_commit_t){
    .database = database,
    .outcome = &outcome,
    .transaction = transaction,
});
ustore_transaction_free(transaction);
```

A failed commit keeps the contents of the transaction for a retry.
`ustore_transaction_rollback` withdraws a stage, releasing whatever it claimed, while keeping the reads and writes, so the transaction can stage again or be reset through `ustore_transaction_init`.
`ustore_transaction_stage` stages the transaction for a two-phase commit and can export its `read_stamp`, the `ustore_stamp_t` the transaction reads at and its commit validates against.
A commit can export its `commit_stamp`, its position in commit order, which every snapshot at or above it sees, and is never below the stage's `read_stamp`.

### Snapshots

A snapshot pins a point-in-time view of the whole database, and reads select it through `ustore_snapshot_view`, so a read can name a transaction or a snapshot but never both:

```c
ustore_snapshot_t snapshot = NULL;
ustore_snapshot_init(&(ustore_snapshot_init_t){
    .snapshot = &snapshot,
    .database = database,
    .outcome = &outcome,
});

ustore_read(&(ustore_read_t){
    .database = database, .outcome = &outcome, .view = ustore_snapshot_view(snapshot), /* ... */});

ustore_snapshot_free(snapshot);
```

`ustore_snapshot_init` can export its `read_stamp`, the stamp the snapshot reads at, which LevelDB, keeping its sequence numbers private, reports as zero.
A snapshot lives no longer than the process that took it, and holds its database open until freed, as a transaction does, so `ustore_database_free` closes the database only once the last of them is freed.
`ustore_snapshot_export` writes one out as a new database under a directory, which is how a view outlives the process.
TODO: persistent snapshots, recorded in the catalog under UUIDv7 identifiers and kept across restarts until dropped.

### Documents

A document collection stores JSON text, so reading a document back through `ustore_read` may not return the exact bytes written.
A field addresses a value inside a document, either as a top-level key such as `_id` or as an RFC 6901 JSON Pointer such as `/posts/0/text`.

- `ustore_docs_write` upserts, updates or inserts whole documents or fields, or applies RFC 6902 patches or RFC 7386 merge patches, as its `modification` selects; whole documents are removed with `ustore_remove`.
- `ustore_docs_read` reads whole documents or fields, as JSON or as values printed into text.
- `ustore_docs_gist` lists the unique JSON Pointers to the leaves of the requested documents.
- `ustore_docs_gather` reads many fields of many documents into typed columns, with validity, conversion and collision bitmaps per column.

```c
ustore_key_t key = {.uuid.bytes[15] = 1};
ustore_str_view_t document = "{\"person\": \"Alice\", \"age\": 27}";
ustore_length_t document_length = 30;
ustore_docs_write(&(ustore_docs_write_t){
    .database = database,
    .outcome = &outcome,
    .count = 1,
    .type = ustore_docs_field_json_k,
    .modification = ustore_docs_modify_upsert_k,
    .keys = &key,
    .values = (ustore_bytes_cptr_t const*)&document,
    .lengths = &document_length,
});

ustore_str_view_t field = "/person";
ustore_length_t offsets[2];
ustore_byte_t* person = NULL;
ustore_docs_read(&(ustore_docs_read_t){
    .database = database,
    .outcome = &outcome,
    .type = ustore_docs_field_str_k,
    .count = 1,
    .keys = &key,
    .fields = &field,
    .offsets = offsets,
    .values = &person,
    .arena = &arena,
});
```

### Graphs

A graph collection stores edges as triplets of a source, a target and an edge identifier, indexed from both ends.
The graph calls address a collection created with `ustore_modality_graph_k`, which the in-memory engine keeps apart from every collection of values.
Several edges between the same two vertices need distinct edge identifiers, and an edge without one gets `ustore_edge_id_default_k`.

- `ustore_graph_write_edges` adds edges together with their vertices.
- `ustore_graph_write_vertices` adds vertices without edges.
- `ustore_graph_read_edges` exports the degree and the edges of every requested vertex in the requested role.
- `ustore_graph_remove_edges` removes edges and keeps their vertices.
- `ustore_graph_remove_vertices` removes vertices together with all their edges.

```c
typedef struct triplet_t {
    ustore_key_t source;
    ustore_key_t target;
    ustore_key_t edge;
} triplet_t;

ustore_collection_id_t graph;
ustore_collection_create(&(ustore_collection_create_t){
    .database = database,
    .outcome = &outcome,
    .modality = ustore_modality_graph_k,
    .collection = &graph,
});

triplet_t triplets[2] = {
    {{.uuid.bytes[15] = 1}, {.uuid.bytes[15] = 2}, {.uuid.bytes[15] = 9}},
    {{.uuid.bytes[15] = 2}, {.uuid.bytes[15] = 3}, {.uuid.bytes[15] = 10}},
};
ustore_graph_write_edges(&(ustore_graph_write_edges_t){
    .database = database,
    .outcome = &outcome,
    .count = 2,
    .collections = &graph,
    .edges = &triplets[0].edge,
    .edges_stride = sizeof(triplet_t),
    .sources = &triplets[0].source,
    .sources_stride = sizeof(triplet_t),
    .targets = &triplets[0].target,
    .targets_stride = sizeof(triplet_t),
});

ustore_key_t vertex = {.uuid.bytes[15] = 2};
ustore_vertex_role_t role = ustore_vertex_role_any_k;
ustore_vertex_degree_t degree = 0;
ustore_key_t* edges = NULL;
ustore_graph_read_edges(&(ustore_graph_read_edges_t){
    .database = database,
    .outcome = &outcome,
    .count = 1,
    .collections = &graph,
    .vertices = &vertex,
    .roles = &role,
    .degrees_per_vertex = &degree,
    .edges_per_vertex = &edges,
    .arena = &arena,
});
```

Every found edge takes three keys of `edges`: its source, its target and its identifier.
Outgoing edges come first, sorted by target, then incoming edges, sorted by source, and a missing vertex has the degree `ustore_vertex_degree_missing_k`.
Roles matter only in directed graphs, where `ustore_vertex_role_source_k` and `ustore_vertex_role_target_k` select one direction.

### Pages

A pages collection stores ordered documents, edited by one verb per kind of edit, each taking only its own fields: `ustore_pages_import`, `_insert`, `_remove`, `_move`, `_splice`, `_set_attributes` and `_remove_attributes`.
`ustore_pages_read` reads documents back into an arena, and `ustore_pages_history` lists the edits that produced them.

## C++ Interface

### Conventions

Everything lives in `unum::ustore`, and `#include <ustore/ustore.hpp>` brings in all of it.
Fallible calls return a `status_t`, a `status_code_t` beside a static message, or an `expected` that holds either a value or the `status_t` explaining its absence; both are `[[nodiscard]]`.
Dereferencing an `expected<value_type>` yields the `value_type`, and `auto [value, status]` decomposes one whose value is default constructible.
`key_from_integer` spells a non-negative integer as a key, and `key_after` answers the key right after another.
Nothing copies values out of the arena, and nothing throws.

```cpp
using namespace unum::ustore;

auto [db, status] = database_t::open_ram();
if (status.failed())
    std::fprintf(stderr, "%s\n", status.message);
```

### Blobs

A collection handle is cheap to construct, and its `[]` operator returns a `blobs_ref` over one key or a batch of keys that reads and writes through the C interface:

```cpp
blobs_collection_t main = db.main();

status_t status = main[key_from_integer(42)].assign("purpose of life");
expected<value_view_t> value = main[key_from_integer(42)].value();
bool const matches = value && *value == "purpose of life";

status = main[{key_from_integer(43), key_from_integer(44)}].assign("same value");
auto present = main[{key_from_integer(43), key_from_integer(44)}].present();
auto lengths = main[{key_from_integer(43), key_from_integer(44)}].length();
status = main[key_from_integer(43)].remove();

std::array<ustore_key_t, 3> keys {key_from_integer(65), key_from_integer(66), key_from_integer(67)};
auto values = main[keys].value();
for (value_view_t found : *values)
    std::printf("%zu bytes\n", found.size());
```

A single key yields a `value_view_t`, a `bool` presence and a length, while a batch yields `embedded_blobs_t`, `bits_span_t` and a range of lengths.
`remove` removes the keys, while `clear_values` keeps them and empties their values.
`keys` and `pairs` range over a collection in key order from a `start_key` to an `end_key`, and `size_range` estimates how many keys it holds.

### Named Collections and Transactions

```cpp
expected<docs_collection_t> people = db.find_or_create<docs_collection_t>("people");

expected<transaction_t> transaction = db.transaction();
blobs_collection_t inside = transaction->main();
status = inside[key_from_integer(42)].assign("written inside a transaction");
status = transaction->commit();

expected<snapshot_t> snapshot = db.snapshot();
auto pinned = snapshot->main()[key_from_integer(42)].value();
```

A handle taken from a transaction or a snapshot reads and writes through it, and a transaction commits only when `commit` is called.

### Arenas

Every collection and transaction handle owns one arena, which all references produced from it share.
Threads sharing one handle, or reads interleaved with new requests, pass their own `arena_t` through `on`:

```cpp
arena_t arena;
auto lengths = main[{key_from_integer(43), key_from_integer(44)}].on(arena).length();
```

The exported ranges stay valid until the next call through the same arena.

### Documents

A `docs_collection_t` hands out `docs_ref` proxies, which read and write JSON through `ustore_docs_read` and `ustore_docs_write`:

```cpp
docs_collection_t people = db.main<docs_collection_t>();
status_t status = people[key_from_integer(1)].assign(R"({"person": "Alice", "age": 27, "height": 1})");
status = people[key_from_integer(2)].assign(R"({"person": "Bob", "age": "27", "weight": 2})");

expected<value_view_t> document = people[key_from_integer(1)].value();
expected<value_view_t> person = people[collection_key_field_t {key_from_integer(1), "/person"}].value(ustore_docs_field_str_k);

status = people[key_from_integer(1)].patch(R"([{"op": "replace", "path": "/age", "value": 28}])");
status = people[key_from_integer(2)].merge(R"({"weight": null})");
expected<joined_strs_t> paths = people[{key_from_integer(1), key_from_integer(2)}].gist();
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

std::array<ustore_key_t, 3> keys {key_from_integer(1), key_from_integer(2), key_from_integer(3)};
auto table = people[keys].gather(header);
auto ages = table->column<0>();
for (std::size_t row = 0; row != ages.size(); ++row)
    if (ages[row].valid)
        std::printf("%d\n", ages[row].value);
```

A header whose column types are known only at runtime views an array of `field_type_t`, and its columns are typed at the point of use:

```cpp
field_type_t const columns[] = {
    {"age", ustore_docs_field_i32_k},
    {"age", ustore_docs_field_str_k},
    {"person", ustore_docs_field_str_k},
};
auto dynamic = people[keys].gather(make_table_header(columns));
auto names = dynamic->column(2).as<std::string_view>();
```

Every cell carries `valid`, `converted` and `collides` flags beside its `value`, which is meaningful only when `valid`.

### Graphs

A `graph_collection_t` batches edges and vertices, and can be used from inside a transaction like any other handle:

```cpp
auto [graph, status] = db.create<graph_collection_t>("graph", ustore_modality_graph_k);
status = graph.write_edge(edge_t {key_from_integer(1), key_from_integer(2), key_from_integer(9)});

std::array<edge_t, 2> triplets {edge_t {key_from_integer(2), key_from_integer(3), key_from_integer(10)}, edge_t {key_from_integer(3), key_from_integer(4), key_from_integer(11)}};
status = graph.write_edges(edges(triplets));

expected<ustore_vertex_degree_t> degree = graph.degree(key_from_integer(2));
auto successors = graph.successors(key_from_integer(2));
auto between = graph.edges_between(key_from_integer(1), key_from_integer(2));
auto touching = graph.read_edges(key_from_integer(3), ustore_vertex_role_target_k);
```

`edges` streams every edge of the collection, and `vertex_stream` every vertex, both reading ahead in batches.
