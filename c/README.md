# UStore for C

`c/` is the public C SDK of UStore, and its only interface: every engine and every binding meets here.
The `*.h` headers declare a C11 binary interface that every engine implements — the in-memory `ram` reference engine, the RocksDB and LevelDB key-value stores, and the NVMe engine as a v1 target — so any language with a C FFI can drive the database.
Every call takes one argument struct and reports through an outcome, so the same verb blocks or defers:

- `ustore_database_t`, `ustore_transaction_t` and `ustore_snapshot_t`: the head, a group of changes, a point in time.
- `ustore_collection_id_t`: a collection of blobs, strings, documents, graphs or pages.
- `ustore_executor_t` and `ustore_outcome_t`: the threads a call runs on, and its status.
- `ustore_arena_t`: where every output whose length scales with the call lands.

## Installation

UStore builds with CMake into `ustore_static`, linked as `ustore::static`, and `ustore_shared`, linked as `ustore::shared`.
Each holds the in-memory engine and every engine the build enables through `USTORE_WITH_ROCKSDB`, `USTORE_WITH_LEVELDB` and `USTORE_WITH_NVME`, and each engine is its own object, so a program links only the engines it opens.
Configured with `USTORE_INSTALL`, the install writes both libraries, the headers and a CMake package that `find_package(ustore)` reads, and `pkg-config --cflags --libs ustore` serves builds without CMake.

```cmake
FetchContent_Declare(ustore GIT_REPOSITORY https://github.com/unum-cloud/UStore.git GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(ustore)
target_link_libraries(your_target PRIVATE ustore::static)
```

From a checkout, this builds the library and runs the C suite, which compiles the headers as plain C11:

```sh
cmake -B build_release -D CMAKE_BUILD_TYPE=Release -D USTORE_BUILD_TEST=1
cmake --build build_release --target ustore_test_c && ctest --test-dir build_release -R '^ustore_test_c$'
```

## Quickstart

The program opens a database in memory, writes a blob, a document and an edge in one transaction, and reads all three back.
Every argument struct is set by designators, so a call names only what it uses, and every omitted field is zero.

```c
#include <assert.h>
#include <string.h>

#include <ustore/ustore.h>

int main(void) {
    ustore_outcome_t outcome = {0};
    ustore_database_t db = NULL;
    ustore_arena_t arena = NULL;
    ustore_transaction_t txn = NULL;
    ustore_database_init_ram(&(ustore_database_init_ram_t){.database = &db, .outcome = &outcome});
    ustore_arena_init(&(ustore_arena_init_t){.arena = &arena, .outcome = &outcome});
    ustore_collection_id_t posters = {0}, movies = {0}, similar = {0};
    ustore_collection_create(&(ustore_collection_create_t){
        .database = db, .outcome = &outcome, .modality = ustore_modality_blobs_k, .collection = &posters});
    ustore_collection_create(&(ustore_collection_create_t){
        .database = db, .outcome = &outcome, .modality = ustore_modality_docs_k, .collection = &movies});
    ustore_collection_create(&(ustore_collection_create_t){
        .database = db, .outcome = &outcome, .modality = ustore_modality_graph_k, .collection = &similar});

    // One transaction writes a blob, a document and an edge, and commits all three or none
    ustore_key_t const avatar = {.uuid.bytes[15] = 1}, titanic = {.uuid.bytes[15] = 2};
    char const poster[] = "\x89PNG", movie[] = "{\"title\":\"Avatar\",\"year\":2009}";
    uint32_t const poster_offsets[2] = {0, sizeof(poster) - 1}, movie_offsets[2] = {0, sizeof(movie) - 1};
    ustore_transaction_init(&(ustore_transaction_init_t){.transaction = &txn, .database = db, .outcome = &outcome});
    ustore_blobs_write(&(ustore_blobs_write_t){
        .database = db, .outcome = &outcome, .transaction = txn, .count = 1, .collections = {&posters, 0},
        .keys = {&avatar, 0},
        .values = {.layout = ustore_strings_u32_tape_k, .data.u32_tape = {(uint8_t const*)poster, poster_offsets}}});
    ustore_docs_write(&(ustore_docs_write_t){
        .database = db, .outcome = &outcome, .transaction = txn, .count = 1, .collections = {&movies, 0},
        .keys = {&avatar, 0},
        .values = {.layout = ustore_strings_u32_tape_k, .data.u32_tape = {(uint8_t const*)movie, movie_offsets}}});
    ustore_graph_write_edges(&(ustore_graph_write_edges_t){
        .database = db, .outcome = &outcome, .transaction = txn, .count = 1, .collections = {&similar, 0},
        .sources = {&avatar, 0}, .targets = {&titanic, 0}});
    ustore_transaction_commit(&(ustore_transaction_commit_t){.database = db, .outcome = &outcome, .transaction = txn});
    assert(outcome.status == ustore_status_success_k);

    // Reads at the head see the commit: the poster, the title and the one edge leaving Avatar
    ustore_strings_view_t read = {0};
    ustore_blobs_read(&(ustore_blobs_read_t){.database = db, .outcome = &outcome, .count = 1,
                                             .collections = {&posters, 0}, .keys = {&avatar, 0}, .values = &read,
                                             .arena = arena});
    assert(read.data.u32_tape.offsets[1] == 4 && memcmp(read.data.u32_tape.data, poster, 4) == 0);
    uint32_t const title_offsets[2] = {0, 5};
    ustore_docs_read(&(ustore_docs_read_t){
        .database = db, .outcome = &outcome, .count = 1, .collections = {&movies, 0}, .keys = {&avatar, 0},
        .fields = {.layout = ustore_strings_u32_tape_k, .data.u32_tape = {(uint8_t const*)"title", title_offsets}},
        .values = &read, .arena = arena, .outputs_type = ustore_docs_field_type_string_k});
    assert(strcmp((char const*)read.data.u32_tape.data, "Avatar") == 0);
    ustore_vertex_degree_t const* degrees = NULL;
    ustore_length_t const* edges_offsets = NULL;
    ustore_key_t const* edges = NULL;
    ustore_graph_read_edges(&(ustore_graph_read_edges_t){
        .database = db, .outcome = &outcome, .count = 1, .collections = {&similar, 0}, .vertices = {&avatar, 0},
        .degrees_per_vertex = &degrees, .edges_offsets = &edges_offsets, .edges_per_vertex = &edges, .arena = arena});
    assert(degrees[0] == 1 && memcmp(&edges[1], &titanic, sizeof(titanic)) == 0);
    ustore_transaction_free(txn);
    ustore_arena_free(arena);
    ustore_database_free(db);
    return 0;
}
```

Each read rewinds the arena, so the poster's view goes stale the moment the title's read starts.

## Collections

A database holds no collection until one is created, and every call names the collections it addresses.
A collection is created with an optional name and the `ustore_modality_t` it holds, and addressed afterwards by the identifier its create answers, a UUIDv7 minted once and never reused.
A name resolves once, through `ustore_collection_find`, which answers the identifier and modality of every name of a batch, and by identifier from then on.
Names starting with `_` are refused, as the engines keep their own `_names` and `_collections` there, which no listing shows.
Collections are created and dropped only at the head, so resolving a name never joins the conflicts of a transaction.
`ustore_collection_list` exports every identifier and, where asked, its modality and name, and `ustore_collection_drop` with `ustore_drop_collection_k` forgets a collection's name before the collection goes.

```c
char const name[] = "movies";
ustore_collection_id_t movies = {0};
ustore_collection_create(&(ustore_collection_create_t){
    .database = database,
    .outcome = &outcome,
    .name = {(uint8_t const*)name, sizeof(name) - 1},
    .modality = ustore_modality_docs_k,
    .collection = &movies,
});
```

Every verb serves only collections of its own modality, and answers `ustore_status_invalid_argument_k` for a collection holding another.

## Reads and Writes

`ustore_blobs_write` takes its keys as a strided run and its values as a `ustore_strings_view_t`, whose `layout` is an Apache Arrow `binary` tape of 32-bit offsets, a `large_binary` tape of 64-bit offsets, or an accessor callback over any other shape.
A batch is atomic, so if one task fails none is applied, and `durability` chooses whether the call returns once the write is buffered or once it is flushed.
Removing keys is its own call, `ustore_blobs_remove`, so an empty value is still a value, and its `end_keys` remove ranges in the same atomic batch.
A read lands its values in the arena as one tape, which writes back without a copy, and `.lengths`, `.presences`, `offsets` and `limits` read lengths, presence bits or slices.

```c
ustore_key_t key = {.uuid.bytes[15] = 42};
char const value[] = "meaning of life";
uint32_t const offsets[2] = {0, 15};
ustore_keys_view_t const keys = {&key, sizeof(ustore_key_t)};
ustore_blobs_write(&(ustore_blobs_write_t){
    .database = database, .outcome = &outcome, .count = 1, .collections = {&values, 0}, .keys = keys,
    .values = {.layout = ustore_strings_u32_tape_k, .data.u32_tape = {(uint8_t const*)value, offsets}},
});

ustore_octet_t const* presences = NULL;
ustore_strings_view_t read = {0};
ustore_blobs_read(&(ustore_blobs_read_t){
    .database = database, .outcome = &outcome, .count = 1, .collections = {&values, 0}, .keys = keys,
    .presences = &presences, .values = &read, .arena = arena,
});
assert(read.data.u32_tape.offsets[1] - read.data.u32_tape.offsets[0] == 15);
```

The offsets are 32 bits wide while the bytes fit in 4 GiB and 64 bits beyond, so a reader checks `layout` once per call, and a missing key reads as an empty value.

### Strings

A strings collection, created with `ustore_modality_strings_k`, keys its values by variable-length strings kept in the order of their bytes.
Its strings and values both arrive as `ustore_strings_view_t`, so `ustore_strings_write`, `_remove`, `_read`, `_scan` and `_cursor_*` mirror the blob calls with strings in place of keys, and a scan lists up to `wanted` strings under every prefix of a batch.

## Scans

`ustore_blobs_scan` exports up to `wanted` keys per task, starting from `start_keys[index]` or the smallest key after it, and exports no values.
Start keys with `NULL` data start every scan at the first key in its `direction`, and `end_keys` stop it before a key.
`ustore_scan_ascending_k` walks `[start, end)` smallest first and `ustore_scan_descending_k` walks `(end, start]` largest first.
The next ascending page starts right after the last exported key, its 16 bytes incremented as one big-endian integer.
`ustore_blobs_sample` draws up to `wanted` keys uniformly at random, and `ustore_blobs_measure` bounds the cardinality, value bytes and disk usage of a range from below and above; every other modality samples and measures through its own `_sample` and `_measure`.

```c
ustore_key_t start = {0};
ustore_length_t const* offsets = NULL;
ustore_key_t const* found = NULL;
ustore_blobs_scan(&(ustore_blobs_scan_t){
    .database = database,
    .outcome = &outcome,
    .count = 1,
    .collections = {&values, 0},
    .start_keys = {&start, sizeof(ustore_key_t)},
    .offsets = &offsets,
    .keys = &found,
    .arena = arena,
    .wanted = 100,
});
```

### Cursors

A cursor walks the keys of one collection in pages, every pull resuming past the last key the pull before it handed out, and reads their values in the same call when `values` asks for them.
`ustore_blobs_cursor_init` reads nothing, and its `view`, `start_key`, `end_key` and `direction` choose what the walk sees and where it runs.
A page shorter than `wanted` exhausts the walk for good, a pull that fails or is cancelled leaves the cursor where it was, and a second pull while one runs finishes `ustore_status_contended_k`.
The cursor holds its database, and the transaction or snapshot it reads through, until `ustore_blobs_cursor_free`.

```c
ustore_blobs_cursor_t cursor = NULL;
ustore_blobs_cursor_init(&(ustore_blobs_cursor_init_t){
    .cursor = &cursor, .database = database, .outcome = &outcome, .collection = values});
ustore_size_t const wanted = 256;
ustore_size_t written = wanted;
ustore_key_t const* keys = NULL;
ustore_strings_view_t pairs = {0};
while (outcome.status == ustore_status_success_k && written == wanted)
    ustore_blobs_cursor_next(&(ustore_blobs_cursor_next_t){
        .database = database, .outcome = &outcome, .cursor = cursor, .keys = &keys, .values = &pairs,
        .written = &written, .arena = arena, .wanted = wanted});
ustore_blobs_cursor_free(cursor);
```

`ustore_changes_cursor_init` opens a feed of the changes committed to one collection after `since_stamp`, oldest first, pinning that stamp as a snapshot does.
Every pull writes the key, the commit stamp and the `ustore_change_kind_t` of every change, and a range removal comes as the removal of every key it removed.
Versions already dropped refuse the init with `ustore_status_write_conflict_k`, and LevelDB, which keeps no stamps, with `ustore_status_unsupported_k`.

### Progress and Cancellation

Scans, cursor pulls, saves and compactions take an optional `progress` callback and the `progress_state` handed to it, counting finished tasks for a scan, one unit for a pull, bytes for a save, and bytes or collections for a compaction.
A callback never runs concurrently with itself, never after the call is final, and never sees `finished` decrease.
Returning `ustore_cancel_k` is the only way to stop a call, which answers `ustore_status_cancelled_k`: outputs stay `NULL`, a cancelled save removes what it wrote, and a cancelled compaction keeps what it compacted.

```c
static ustore_continuation_t within_budget(ustore_size_t finished, ustore_size_t total, void* state) {
    ustore_size_t const budget = *(ustore_size_t const*)state;
    fprintf(stderr, "saved %llu of %llu bytes\n", (unsigned long long)finished, (unsigned long long)total);
    return finished <= budget ? ustore_continue_k : ustore_cancel_k;
}
```

## Transactions

A transaction groups reads and writes across collections and modalities, and commits all of them or none.
Writes take the transaction itself, while reads take a view of it, which chooses whether the read registers its keys so the commit fails if another writer changed them since.
`ustore_transaction_init` begins a transaction into a `NULL` handle, and resets one of the same database in place.
A failed commit keeps the contents for a retry.
`ustore_transaction_stage` stages it for a two-phase commit and can export its `read_stamp`, the stamp it reads at and its commit validates against, while `ustore_transaction_rollback` withdraws a stage and keeps the reads and writes.
A commit can export its `commit_stamp`, its position in commit order, which every snapshot at or above it sees.

```c
ustore_transaction_t transaction = NULL;
ustore_transaction_init(&(ustore_transaction_init_t){
    .transaction = &transaction, .database = database, .outcome = &outcome});
ustore_blobs_write(&(ustore_blobs_write_t){.database = database, .outcome = &outcome, .transaction = transaction, /* ... */});
ustore_blobs_read(&(ustore_blobs_read_t){
    .database = database, .outcome = &outcome,
    .view = ustore_transaction_view(transaction, ustore_watch_keys_k), /* ... */});
ustore_stamp_t commit_stamp = 0;
ustore_transaction_commit(&(ustore_transaction_commit_t){
    .database = database, .outcome = &outcome, .transaction = transaction, .commit_stamp = &commit_stamp});
ustore_transaction_free(transaction);
```

Conflicts finish the commit with `ustore_status_write_conflict_k`, `_read_conflict_k` or `_phantom_conflict_k`, and transactions never block one another.

## Snapshots

A snapshot pins a point-in-time view of the whole database, and reads select it through `ustore_snapshot_view`, so a read names a transaction or a snapshot but never both.
`ustore_snapshot_init` can export its `read_stamp`, which LevelDB, keeping its sequence numbers private, reports as zero.
A snapshot holds its database open until freed, as a transaction does, so `ustore_database_free` closes the database once the last of them is freed.
`ustore_snapshot_save` writes one out as a new database of the same engine, holding exactly the collections and names the snapshot records, so a save is how a view outlives the process.
RocksDB and LevelDB create its `path`, the `ram` engine writes its image into the directory `path` names, and NVMe takes `path` as the manifest of the new database.

```c
ustore_snapshot_t snapshot = NULL;
ustore_snapshot_init(&(ustore_snapshot_init_t){.snapshot = &snapshot, .database = database, .outcome = &outcome});
ustore_blobs_read(&(ustore_blobs_read_t){
    .database = database, .outcome = &outcome, .view = ustore_snapshot_view(snapshot), /* ... */});

ustore_size_t budget = 1ull << 30;
char const path[] = "/var/backups/ustore";
ustore_snapshot_save(&(ustore_snapshot_save_t){
    .snapshot = snapshot, .outcome = &outcome, .path = {(uint8_t const*)path, sizeof(path) - 1},
    .progress = within_budget, .progress_state = &budget});
ustore_snapshot_free(snapshot);
```

## Documents

A document collection stores JSON, so reading a document back may not return the exact bytes written.
A field addresses a value inside a document, as a top-level key such as `_id` or an RFC 6901 JSON Pointer such as `/posts/0/text`, while a zeroed `fields` view addresses the whole document.

- `ustore_docs_write` upserts, updates or inserts documents or fields, or applies RFC 6902 patches or RFC 7386 merge patches, as its `modification` selects.
- `ustore_docs_read` reads documents or fields, as JSON or as values printed into text.
- `ustore_docs_gist` lists the unique JSON Pointers to the leaves of the requested documents.
- `ustore_docs_gather` reads many fields of many documents into typed columns, with validity, conversion and collision bitmaps per column.

```c
char const field[] = "/person";
uint32_t const field_offsets[2] = {0, sizeof(field) - 1};
ustore_strings_view_t person = {0};
ustore_docs_read(&(ustore_docs_read_t){
    .database = database,
    .outcome = &outcome,
    .count = 1,
    .collections = {&movies, 0},
    .keys = {&key, sizeof(ustore_key_t)},
    .fields = {.layout = ustore_strings_u32_tape_k, .data.u32_tape = {(uint8_t const*)field, field_offsets}},
    .values = &person,
    .arena = arena,
    .outputs_type = ustore_docs_field_type_string_k,
});
```

`ustore_docs_remove`, `_scan`, `_sample` and `_measure` work on document keys as the blob calls do on theirs.

### Pages

A pages collection stores ordered documents, edited by one verb per kind of edit, each taking only its own fields: `ustore_pages_import`, `_insert`, `_remove`, `_move`, `_splice`, `_set_attributes` and `_remove_attributes`.
`ustore_pages_read` reads pages back into an arena, importing and rendering XML and plain text, while HTML, Markdown and the JSON tree answer `ustore_status_unsupported_k`.
An edit of a missing page answers `ustore_status_not_found_k`, and an edit watches the page it rewrites, except on the NVMe engine, which keeps the edits themselves, so two transactions editing one page both commit.

## Graphs

A graph collection, created with `ustore_modality_graph_k`, stores edges as triplets of a source, a target and an edge identifier, indexed from both ends.
Several edges between the same two vertices need distinct identifiers, and an edge without one gets `ustore_edge_id_default_k`.
`ustore_graph_write_edges` adds edges with their vertices, `_write_vertices` adds vertices alone, `_remove_edges` keeps the vertices, and `_remove_vertices` removes them with their edges.
`ustore_graph_read_edges` exports the degree and edges of every vertex in the requested role, three keys per edge: source, target and identifier.
The edges of vertex `i` span from `edges_offsets[i]` to `edges_offsets[i + 1]`, outgoing ones first, sorted by target, then incoming ones, sorted by source, and a missing vertex has the degree `ustore_vertex_degree_missing_k`.

```c
ustore_key_t vertex = {.uuid.bytes[15] = 2};
ustore_vertex_role_t const role = ustore_vertex_role_any_k;
ustore_vertex_degree_t const* degrees = NULL;
ustore_length_t const* edges_offsets = NULL;
ustore_key_t const* edges = NULL;
ustore_graph_read_edges(&(ustore_graph_read_edges_t){
    .database = database,
    .outcome = &outcome,
    .count = 1,
    .collections = {&graph, 0},
    .vertices = {&vertex, sizeof(ustore_key_t)},
    .roles = {&role, 0},
    .degrees_per_vertex = &degrees,
    .edges_offsets = &edges_offsets,
    .edges_per_vertex = &edges,
    .arena = arena,
});
```

Roles matter only in directed graphs, and roles with `NULL` data stand for `ustore_vertex_role_any_k`.

## Receivers & Deferred Calls

The receiver of a call is what its argument struct puts before its inputs: the view it reads, the executor it runs on, and its `completion`.
A call blocks by default and returns with its outcome final, while `ustore_completion_deferred_k` returns once the call is submitted, with the same struct.
Submission copies the argument struct, so the caller may reuse it at once, while everything it points to, the outcome, inputs, outputs and arena, stays borrowed until the call is final.
A deferred call's status reads `ustore_status_deferred_k` until it finishes; the library then stores the message and final status with release ordering and touches nothing the struct points to after that.
The caller reads the status with acquire ordering, polling through `ustore_outcome_status` or waiting through `ustore_outcome_wait`, and never loads the field itself.
A call ends in one order: it releases its arena, cursor, transaction and its counts on the handles it names, then the executor, and stores its status last, so a caller seeing it final may submit again or free at once.
Freeing a database, transaction, snapshot, cursor or arena waits until every call naming it ends, so a binding may drop its handle early, but never from a callback of one of those calls.

```c
ustore_outcome_t outcomes[2] = {{0}};
for (size_t batch = 0; batch != 2; ++batch) { // one call per executor at a time, so two executors overlap two reads
    reads[batch].executor = executors[batch], reads[batch].outcome = &outcomes[batch], reads[batch].arena = arenas[batch];
    reads[batch].completion = ustore_completion_deferred_k;
    ustore_blobs_read(&reads[batch]);
}
while (ustore_outcome_status(&outcomes[0]) == ustore_status_deferred_k ||
       ustore_outcome_status(&outcomes[1]) == ustore_status_deferred_k)
    do_other_work(); // polling alone finishes both
```

The [C++](https://github.com/unum-cloud/UStore/blob/main/cpp/README.md#receivers--deferred-calls), Python, Rust and Mojo bindings choose the executor and the completion by receiver, as `database.on(executor)` and `database.deferred(executor)`, so blocking calls read as they would without executors.

## Executors

An executor owns the threads that run calls and the scratch they use, and a `NULL` executor runs a call on the calling thread.
It serves one call at a time, and nothing queues: a call on an executor serving another, from any thread, finishes at once with `ustore_status_contended_k`, as does a call whose arena, transaction or cursor serves another call.
A blocking call on an executor runs on the calling thread, a deferred one is delegated to the executor's first thread, which serves it in the caller's place, and every fork splits the call's batch over the other threads.

- `ustore_executor_for_ram` spawns caller-exclusive threads near the NUMA nodes its `ram` options name, every node the process may allocate on for `NULL`, where zero threads take 7/8 of the cores those nodes carry.
- `ustore_executor_on_pool` borrows a ForkUnion pool another library owns, which must outlive the executor, and refuses a dispatch over its lender's with `ustore_status_contended_k`.
- `ustore_executor_for_nvme` spawns the threads of the NVMe engine, and its `nvme` options tell every database opened on it how to move bytes.
- `ustore_executor_for_vram` queues every call on one stream of a CUDA device, and serves the reads and scans an engine computes there, refusing changes with `ustore_status_unsupported_k`.

```c
int32_t const memory_domains[1] = {0};
ustore_executor_for_ram(&(ustore_executor_for_ram_t){
    .executor = &executor,
    .outcome = &outcome,
    .ram = {memory_domains, 1},
    .cpu_class_ceiling = ustore_cpu_class_unknown_k,
});
ustore_executor_drain(&(ustore_executor_drain_t){.executor = executor, .outcome = &outcome, .mood = ustore_mood_parked_k});
ustore_executor_free(executor);
```

`cpu_class_ceiling` caps the CPU class the threads confine their waits and atomics to, intersected with what the machine runs, and `ustore_executor_drain` waits until no call holds the executor, then parks or spins its threads.

### Inclusive and Exclusive Executors

A pool is caller-inclusive when the thread that spawned or lent it counts as one of its threads, and caller-exclusive when it only dispatches and waits.
An inclusive pool needs its caller to run anything, so a deferred call on it runs at submission and is final when it returns.
An exclusive pool, which every executor UStore spawns is, delegates a deferred call to its own first thread.
So one manager thread in `int main()` can overlap a USearch RAM executor, a UStore NVMe executor and a UForm VRAM executor, giving each one deferred call and polling the three statuses, with no thread of its own per engine.

## Batches and Arenas

Every call is a batch of `count` tasks, and each input takes one of three shapes:

- __One run of bytes__, a `ustore_bytes_view_t` of a pointer and a length, carries a path or a name, never null-terminated by contract.
- __A strided run__ of fixed-width elements, such as a `ustore_keys_view_t` of a pointer and a stride in bytes, carries one element per task, and a zero stride repeats the first element for every task.
- __A ragged batch__, a `ustore_strings_view_t`, carries one string per task as an Apache Arrow tape of 32-bit or 64-bit offsets, or through an accessor callback.

Keys are `ustore_key_t`, 16 bytes compared bytewise, and a non-negative integer spells as its 128-bit big-endian bytes, so keys made from integers order as the integers.
Every output whose length scales with the call lands packed in the arena the call is given, and only fixed-size reports, such as descriptions, stamps and created handles, stay caller fields.
`ustore_arena_init` makes an arena where its executor computes, or on host pages for a `NULL` executor, and every call given it rewinds and refills it, growing only when the new outputs outsize everything it held, so a steady loop allocates once.

```c
ustore_arena_t arena = NULL;
ustore_arena_init(&(ustore_arena_init_t){.arena = &arena, .outcome = &outcome, .executor = executor});
ustore_arena_description_t where = {0};
ustore_arena_describe(&(ustore_arena_describe_t){.arena = arena, .outcome = &outcome, .description = &where});
assert(where.residency == ustore_residency_ram_k);
ustore_arena_free(arena); // waits until every deferred call naming the arena is final
```

The outputs stay valid until the next call given that arena; to keep them past it, pass that call another arena.
A `NULL` arena is refused with `ustore_status_invalid_argument_k`, an arena serves one call at a time, and a call that fails, is cancelled or has nothing to report leaves its outputs `NULL`.
An accessor runs on the thread that runs the call, in blocks of up to 64 indices, never concurrently with itself and never after the call is final.

## Errors

Every call reports through a `ustore_outcome_t` whose `status` names the result and whose static `message` is never freed.
Statuses read `ustore_status_<name>_k`, and `ustore_status_name` and `ustore_status_from_name` spell them both ways, like `not-found`.
A call refuses in one order, invalid arguments first, then read-only, unsupported and contended, and a refusal changes nothing.
Every closed set a binding spells has a pair of functions, such as `ustore_isolation_name` answering `strict-serializable`, or `unrecognized` for a value no enumerator carries, and `ustore_isolation_from_name` reading it back.

```c
ustore_database_open_rocksdb(&(ustore_database_open_rocksdb_t){
    .database = &database,
    .outcome = &outcome,
    .directory = {(uint8_t const*)directory, sizeof(directory) - 1},
});
if (outcome.status != ustore_status_success_k)
    fprintf(stderr, "%s: %s\n", ustore_status_name(outcome.status), outcome.message);
```

The opens of an engine the build lacks answer `ustore_status_unsupported_k`, so a program links against every build, and every freeing function accepts `NULL`.
`ustore_version_name` spells the version of the library the program runs against, like `1.0.0`.

## Platform Notes

### Engines

Each engine opens through its own call, whose arguments hold what that engine can be asked, and a zero field keeps its default:

- `ustore_database_init_ram` starts an empty database in memory alone, while `ustore_database_open_ram` serves a directory as one image, loading it at open and rewriting it whole on a flushed commit and on close.
- `ustore_database_open_rocksdb` and `ustore_database_open_leveldb` serve a directory in place, creating the database when it holds none.
- `ustore_database_open_nvme` serves the database its manifest names, `<database>.ustore.json` in an existing directory or the raw address of its first slot, such as `/dev/nvme0n1@64GiB`.
- `ustore_database_view_rocksdb` serves an existing RocksDB directory read-only, refusing every change with `ustore_status_read_only_k`, while snapshots and their saves still serve.

The RAM and NVMe opens take a `cpu_class_ceiling`, the newest CPU class the database's kernels may run as, intersected with the machine, and the description reports the class picked as `cpu_class`.

### Maintenance

`ustore_database_describe` reports the engine, its capabilities, its isolation, and its estimates of the keys and bytes it holds, zero where it keeps none.
`ustore_database_clear` empties every collection as its `mode` names: `ustore_drop_pairs_k` keeps the collections and names, `ustore_drop_collection_k` drops every collection but the main one, and `ustore_drop_values_k` keeps the keys.
`ustore_database_compact` reclaims the space overwritten and removed data takes, through the phases its engine runs; with a `plan`, a cancelled compaction stays open, the next call continues from the unit it stopped at, and `ustore_database_compact_plan_finish` frees it.

### Design

- __This is the C ABI itself.__
  A verb `ustore_<noun>_<verb>` takes a `ustore_<noun>_<verb>_t` set by designator, so a call names only what it changes and the struct grows without breaking source.
- __The completion chooses, the verb stays.__
  A `NULL` executor runs a call on the calling thread, an executor blocks on it, and `ustore_completion_deferred_k` submits it, all with the same struct, reporting through the same outcome.
- __Three shapes in, three out.__
  Inputs are runs of bytes, strided runs or Arrow tapes, and outputs are packed arrays, tapes or packed keys split by offsets, so Arrow arrays and a runtime's own objects pass without copies.
- __No life of its own.__
  Threads live only in executors, each serving one call at a time, arenas are lent and rewound, and only a progress callback stops a running call.
