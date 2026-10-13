# UStore for C and C++

`include/ustore/` is the public interface of UStore, and the only one: every engine and every binding meets here.
The `*.h` headers declare a C11 binary interface that every engine implements — the in-memory `ram` reference engine, the RocksDB and LevelDB key-value stores, and the NVMe engine as a v1 target — so any language with a C FFI can drive the database.
The `*.hpp` headers beside them wrap the same calls in C++ with RAII handles, batched references and typed ranges, and add no ABI of their own.

| Header      | Declares                                                                                               |
| :---------- | :----------------------------------------------------------------------------------------------------- |
| `ustore.h`  | Every C header below                                                                                   |
| `types.h`   | Statuses, outcomes, completions, progress callbacks, executors, arenas, string batches and the version |
| `db.h`      | Databases, collections, snapshots, transactions, views, strided runs of keys and enum spellings        |
| `blobs.h`   | Binary values: `ustore_blobs_write`, `_remove`, `_read`, `_scan`, `_sample`, `_measure` and cursors    |
| `strings.h` | String-keyed values: `ustore_strings_write`, `_remove`, `_read`, `_scan` and cursors                   |
| `docs.h`    | JSON documents: `ustore_docs_write`, `ustore_docs_read`, `ustore_docs_gist`, `ustore_docs_gather`      |
| `graph.h`   | Graph edges and vertices                                                                               |
| `pages.h`   | Ordered documents: one verb per edit, `ustore_pages_read` and `ustore_pages_history`                   |

| Header                   | Declares                                                           |
| :----------------------- | :----------------------------------------------------------------- |
| `ustore.hpp`             | Every C++ header below                                             |
| `db.hpp`                 | `database_t`, `transaction_t`, `snapshot_t` and receivers          |
| `blobs_collection.hpp`   | Binary collections and their deferred twins                        |
| `blobs_ref.hpp`          | Batched references to binary values                                |
| `blobs_range.hpp`        | Cursors over keys, the streams built on them, and key ranges       |
| `docs_collection.hpp`    | Document collections and their deferred twins                      |
| `docs_ref.hpp`           | Batched references to documents                                    |
| `docs_table.hpp`         | Gathered tables                                                    |
| `graph_collection.hpp`   | Graph collections and their deferred twins                         |
| `graph_stream.hpp`       | Adjacency streams                                                  |
| `strings_collection.hpp` | String-keyed collections, cursors, prefix walks and deferred twins |
| `status.hpp`             | `status_code_t`, `status_t`, `expected` and `deferred`             |
| `types.hpp`              | Value views, arenas, stamps, receivers and executors               |
| `ranges.hpp`             | Strided ranges, argument adapters, read views and tapes            |

## Installation

UStore builds with CMake into two libraries from one source list, each holding the in-memory engine and every engine the build enables through `USTORE_WITH_ROCKSDB`, `USTORE_WITH_LEVELDB` and `USTORE_BUILD_NVME`:

- __The archive__, `ustore_static`, linked as `ustore::static` or its alias `ustore::ustore`, carries the objects of the small libraries every build needs: ForkUnion, StringZilla, yyjson and fmt.
  The storage engines' own libraries, RocksDB, LevelDB and liburing, stay archives of their own, installed beside it as `ustore::rocksdb`, `ustore::leveldb` and `ustore::uring`, which `ustore::static` links, so a CMake consumer still names one target.
  A build without CMake links those archives after `libustore_static`, or takes the shared library instead.
- __The shared library__, `ustore_shared`, linked as `ustore::shared`, links them privately and exports only the functions the headers mark `USTORE_EXPORT`, which its consumers import on Windows through the `USTORE_SHARED` definition the target hands them.

Each engine is its own object of the archive, so a program links only the engines it opens.
`USTORE_BUILD_NVME` stays off until the NVMe sources are ported to this interface.

```cmake
FetchContent_Declare(
    ustore
    GIT_REPOSITORY https://github.com/unum-cloud/UStore.git
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(ustore)
target_link_libraries(your_target PRIVATE ustore::static)
```

Configured with `USTORE_INSTALL`, the install writes both libraries, the headers and a CMake package, which `find_package` reads:

```cmake
find_package(ustore 1.0 REQUIRED)
target_link_libraries(your_target PRIVATE ustore::shared)
```

## C Interface

### Conventions

Every call takes a single pointer to an argument struct named after it with a `_t` suffix, so arguments are named and every omitted one is zero.
Every struct opens with its handle, then `executor`, `outcome` and `completion`, so every call reports through a `ustore_outcome_t` whose `status` names the result and whose static `message` is never freed.
Statuses read `ustore_status_<name>_k`, from `ustore_status_success_k` through every failure.
Every closed set is a C enum, and each one a binding spells has a pair of functions: `<prefix>_<enum>_name`, such as `ustore_isolation_name`, answers a static lowercase spelling like `strict-serializable`, or `unrecognized` for a value no enumerator carries, and `<prefix>_<enum>_from_name` reads a `ustore_bytes_view_t` back, answering the enum's zero enumerator for a name it does not know.
`ustore_status_from_name` alone answers `ustore_status_invalid_argument_k` for an unknown name, and `ustore_version_name` spells the version of the library the program runs against, like `1.0.0`.
Every call is a batch of `count` tasks, and each of its inputs takes one of three shapes:

- __One run of bytes__, a `ustore_bytes_view_t` of a pointer and a length, carries a path or a name, never null-terminated by contract.
- __A strided run__ of fixed-width elements, such as a `ustore_keys_view_t` of a pointer and a stride in bytes, carries one element per task, and a zero stride repeats the first element for every task.
- __A ragged batch__, a `ustore_strings_view_t`, carries one string per task as an Apache Arrow tape of 32-bit or 64-bit offsets, or through an accessor callback over any other shape.

A zeroed view of any shape stands for the input's default.
Every output whose length scales with the call lands packed in the arena the call is given, so no output carries a capacity or a stride, and ragged bytes come back as a tape.
Keys are `ustore_key_t`, 16 bytes of a UUID compared bytewise, and a non-negative integer spells as its 128-bit big-endian bytes, as Python's `uuid.UUID(int=...)` does, so keys made from integers order as the integers.

The examples below share these handles, the arena made once for calls on the calling thread:

```c
ustore_database_t database = NULL;
ustore_executor_t executor = NULL;
ustore_outcome_t outcome = {0};
ustore_arena_t arena = NULL;
ustore_arena_init(&(ustore_arena_init_t){.arena = &arena, .outcome = &outcome});
```

### Executors, Blocking and Deferred Calls

An executor owns the threads that run calls and the scratch they use, and a `NULL` executor runs a call on the calling thread.
Its kind must match the residency of every database it drives, and a mismatch reports `ustore_status_device_k`.
`ustore_executor_for_ram` spawns its threads and holds up to `deferred_limit` deferred calls at once, 256 for zero.
Its `ram` options name the memory domains it serves, NUMA nodes as the operating system numbers them, where `NULL` derives every domain the process may allocate on and an unknown one is refused with `ustore_status_invalid_argument_k`.
Each thread keeps its scratch on its local domain when the options name it, and on the first named domain otherwise, as does every arena the executor makes.
Zero threads take 7/8 of the cores local to those domains, rounded down and at least one, leaving the rest to the driver thread, to blocking calls on their callers' threads, and to the background flushes and compactions of RocksDB and LevelDB.
ForkUnion spawns the threads near every domain or near one alone, so naming several domains but not all spreads them over the whole machine.
`ustore_executor_on_pool` borrows a ForkUnion pool another library owns, which must outlive the executor, and places its scratch and arenas by the same `ram` options; a dispatch on a pool that runs another is refused, so a batch finding the pool running its lender's dispatch runs on the executor's own driver thread instead of forking over the lender.
`ustore_executor_for_nvme` is declared for the NVMe engine, and answers `ustore_status_unsupported_k` until its sources build.
A call blocks by default and returns with its outcome final, while `ustore_completion_deferred_k` returns once the call is submitted:

```c
int32_t const memory_domains[1] = {0};
ustore_executor_for_ram(&(ustore_executor_for_ram_t){
    .executor = &executor,
    .outcome = &outcome,
    .ram = {memory_domains, 1},
});

ustore_outcome_t outcomes[4] = {{0}};
for (size_t batch = 0; batch != 4; ++batch) {
    reads[batch].executor = executor, reads[batch].outcome = &outcomes[batch], reads[batch].arena = arenas[batch];
    reads[batch].completion = ustore_completion_deferred_k;
    ustore_blobs_read(&reads[batch]);
}
for (size_t batch = 0; batch != 4; ++batch) ustore_outcome_wait(&outcomes[batch]);
```

Submission copies the argument struct, so the caller may reuse or drop it at once, while everything it points to, the outcome, inputs, outputs and arena alike, stays borrowed until the call is final.
A deferred call's status reads `ustore_status_deferred_k` until it finishes; the library then stores the message and the final status with release ordering and touches nothing the struct points to after that.
The caller reads the status with acquire ordering, polling through `ustore_outcome_status` or waiting through `ustore_outcome_wait`, and never loads the field itself.
Calls on one executor start in submission order, and a blocking call runs alone: after every call submitted before it finishes, and before any submitted after it starts.
A blocking call on an executor runs on the calling thread, while deferred calls run in batches on the executor's threads.
`ustore_executor_drain` waits for every call submitted before it began, and sets whether the threads park or spin once idle.
A call is refused with `ustore_status_contended_k` rather than waited for when the executor's deferred table is full, when its arena, its transaction or its cursor serves another call, or when it drains or blocks from a callback its own executor runs.
Freeing a database, transaction, snapshot, cursor or arena waits until every deferred call naming it finishes, so a binding may drop its handle before those calls finish, but never from a callback of one of them.
A progress callback never waits on a call its own executor has not finished, and polls the status instead.
The C++ wrappers and the Rust and Python bindings choose the executor and the completion by receiver, as `database.on(executor)` and `database.deferred(executor)`, so blocking calls read as they would without executors; the Design sections of `rust/README.md` and `python/README.md` state the rules every binding keeps.
A call stops early only when its progress callback returns `ustore_cancel_k`, as [Progress and Cancellation](#progress-and-cancellation) describes.

### Opening the Database

```c
#include <ustore/ustore.h>

char const directory[] = "/var/lib/ustore";
ustore_database_open_rocksdb(&(ustore_database_open_rocksdb_t){
    .database = &database,
    .outcome = &outcome,
    .directory = {(uint8_t const*)directory, sizeof(directory) - 1},
});
if (outcome.status != ustore_status_success_k)
    fprintf(stderr, "%s: %s\n", ustore_status_name(outcome.status), outcome.message);

ustore_arena_free(arena);
ustore_database_free(database);
ustore_executor_free(executor);
```

Each engine opens through its own calls, whose arguments hold what that engine can be asked, and a zero field keeps the engine's default:

- `ustore_database_init_ram` starts an empty database in memory alone, while `ustore_database_open_ram` serves a directory as one image, loading it at open and rewriting it whole on a flushed commit and on close.
- `ustore_database_open_rocksdb`, `ustore_database_open_leveldb` and `ustore_database_open_nvme` serve a directory in place, and RocksDB and LevelDB create the database when the directory holds none.
- `ustore_database_view_rocksdb` serves an existing RocksDB directory read-only, refusing every call that would change it with `ustore_status_read_only_k`: writes, removes, transactions, collection creates and drops, clears and compactions, while snapshots and their saves still serve.

The opens of an engine the build lacks answer `ustore_status_unsupported_k`, so a program links against every build.
Every freeing function accepts `NULL`.

### Describing, Clearing and Compacting

`ustore_database_describe` reports the engine of an open database, what it supports and how much it holds, while `ustore_database_clear` and `ustore_database_compact` work on every collection at once:

```c
ustore_database_description_t description = {0};
ustore_database_describe(&(ustore_database_describe_t){
    .database = database,
    .outcome = &outcome,
    .description = &description,
});
printf("%s holds %llu keys in %llu bytes on disk\n", ustore_engine_name(description.engine),
       (unsigned long long)description.keys, (unsigned long long)description.disk_bytes);

ustore_database_clear(&(ustore_database_clear_t){.database = database, .outcome = &outcome, .mode = ustore_drop_pairs_k});
ustore_database_compact(&(ustore_database_compact_t){.database = database, .outcome = &outcome});
```

A description's `capabilities` say whether the engine supports transactions, collections and snapshots, `engine` names the engine, and `isolation` says how its transactions see each other: as the `ram` engine was opened, and `ustore_isolation_snapshot_k` elsewhere.
`keys`, `memory_bytes` and `disk_bytes` are the engine's estimates of the keys across every collection and of the bytes the data takes, and zero where it keeps none, as LevelDB keeps no count of its keys.
`ustore_database_clear` empties every collection as its `mode` names:

- `ustore_drop_pairs_k` removes every pair, keeping the collections and their names;
- `ustore_drop_collection_k` drops every collection but the main one with its name, and empties the main one;
- `ustore_drop_values_k` empties every value, keeping the keys.

The `ram` and RocksDB engines refuse to drop the collections while a snapshot is open, with `ustore_status_invalid_argument_k`, since the snapshot belongs to its holder.
`ustore_database_compact` reclaims the space overwritten and removed data still takes, one collection at a time: RocksDB and LevelDB flush and compact their trees, and the `ram` engine frees the versions no reader needs and rewrites its image.

### Collections

Every database has a main collection, `ustore_collection_main_k`, named by the nil UUID, which a `collections` view with `NULL` data addresses.
A collection is created with an optional name and the `ustore_modality_t` it holds, and addressed afterwards by the identifier the call answers, a UUIDv7 the catalog mints once and never reuses:

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
`ustore_collection_list` exports the identifier of every other collection and, where asked, its modality and its name, as a tape where a collection created without one has an empty name.
A listing describes the database rather than a transaction, so it reads the head or a snapshot.

### Writes and Removals

`ustore_blobs_write` takes its keys as a `ustore_keys_view_t` and its values as a `ustore_strings_view_t`, whose `layout` names how the values lie:

- `ustore_strings_u32_tape_k` is Apache Arrow's `binary`: the bytes end to end, and `count + 1` offsets of 32 bits, where value `index` spans from `offsets[index]` up to `offsets[index + 1]`.
- `ustore_strings_u64_tape_k` is Arrow's `large_binary`: the same with 64-bit offsets.
- `ustore_strings_accessor_k` holds a `ustore_strings_get_t` callback and the `handle` it reads, and the callback writes the view of every value whose index it is asked for.

A single write passes a tape of one value:

```c
ustore_key_t key = {.uuid.bytes[15] = 42};
char const value[] = "meaning of life";
uint32_t const offsets[2] = {0, 15};
ustore_blobs_write(&(ustore_blobs_write_t){
    .database = database,
    .outcome = &outcome,
    .count = 1,
    .keys = {&key, sizeof(ustore_key_t)},
    .values = {.layout = ustore_strings_u32_tape_k, .data.u32_tape = {(uint8_t const*)value, offsets}},
});
```

A batch held in an array of structures needs no copy into separate arrays, since an accessor can read the values out of the records:

```c
typedef struct pair_t {
    ustore_key_t key;
    char const* text;
} pair_t;

static void get_texts(void const* handle, ustore_size_t const* indices, ustore_size_t count,
                      ustore_bytes_view_t* views) {
    pair_t const* pairs = (pair_t const*)handle;
    for (ustore_size_t index = 0; index != count; ++index) {
        char const* text = pairs[indices[index]].text;
        views[index] = (ustore_bytes_view_t){(uint8_t const*)text, strlen(text)};
    }
}
```

The write then takes the keys as a strided run over the same records:

```c
pair_t const pairs[2] = {
    {{.uuid.bytes[15] = 42}, "meaning of life"},
    {{.uuid.bytes[15] = 43}, "is unknown"},
};
ustore_blobs_write(&(ustore_blobs_write_t){
    .database = database,
    .outcome = &outcome,
    .count = 2,
    .keys = {&pairs[0].key, sizeof(pair_t)},
    .values = {.layout = ustore_strings_accessor_k, .data.accessor = {get_texts, pairs}},
});
```

The accessor runs on the thread that runs the call, in blocks of up to 64 indices, never concurrently with itself and never after the call is final.
An accessor writing the same view for every index writes one value under every key, as a zero stride repeats one key.
Removing keys is its own call, so no value ever deletes, and an empty value is still a value:

```c
ustore_blobs_remove(&(ustore_blobs_remove_t){.database = database, .outcome = &outcome, .count = 1, .keys = {&key, 0}});
```

A batch is atomic: if one task fails, none is applied.
`durability` chooses whether the call returns once the write is buffered or once it is flushed.

### Reads and Arenas

Every output whose length scales with the call, the values, their lengths and the bitmap of presences alike, lands in an arena.
`ustore_arena_init` makes one where its executor computes, or on host pages for a `NULL` executor, and every call given it rewinds it and refills it, growing only when the new outputs outsize everything it held, so a steady loop allocates once.
Only fixed-size reports, such as descriptions, stamps, the `written` count of a listing and created handles, stay caller fields, and a call that fails zeroes the scalars among them and leaves the handles `NULL`.

```c
ustore_key_t key = {.uuid.bytes[15] = 42};
ustore_octet_t const* presences = NULL;
ustore_strings_view_t values = {0};
ustore_blobs_read(&(ustore_blobs_read_t){
    .database = database,
    .outcome = &outcome,
    .count = 1,
    .keys = {&key, sizeof(ustore_key_t)},
    .presences = &presences,
    .values = &values,
    .arena = arena,
});
uint32_t const* offsets = values.data.u32_tape.offsets;
assert(values.layout == ustore_strings_u32_tape_k && offsets[1] - offsets[0] == 15);
assert(memcmp(values.data.u32_tape.data + offsets[0], "meaning of life", 15) == 0);
```

Values are joined into one tape as Apache Arrow lays them out: `count + 1` offsets, the last one closing the final value, and the bytes end to end.
The offsets are 32 bits wide while the bytes fit in 4 GiB and 64 bits beyond, so a reader checks `layout` once per call, and a tape a read wrote is written back as it is, without a copy.
Every output is optional: `.lengths` exports the length of every value without its bytes, `.presences` a bitmap of the keys that are present, and a missing key reads as an empty value.
The outputs stay valid until the next call given `arena`; to keep them past it, pass that call another arena.
A `NULL` arena is refused with `ustore_status_invalid_argument_k`, an arena serves one call at a time, and a call that fails, is cancelled or has nothing to report leaves its outputs `NULL`.
`ustore_arena_free` waits until every deferred call naming the arena is final.

### Scans

`ustore_blobs_scan` exports up to `wanted` keys per task, starting from `start_keys[index]`, or from the smallest key after it, and exports no values:

```c
ustore_key_t start = {0};
ustore_length_t const* offsets = NULL;
ustore_key_t const* found = NULL;
ustore_blobs_scan(&(ustore_blobs_scan_t){
    .database = database,
    .outcome = &outcome,
    .count = 1,
    .start_keys = {&start, sizeof(ustore_key_t)},
    .offsets = &offsets,
    .keys = &found,
    .arena = arena,
    .wanted = 100,
});
```

Start keys with `NULL` data start every scan at the first key.
The next page starts from the key right after the last exported one, its 16 bytes incremented as one big-endian integer, and a `ustore_blobs_read` of the found keys fetches their values.
`ustore_blobs_sample` draws up to `wanted` keys uniformly at random, and `ustore_blobs_measure` bounds the cardinality, value bytes and disk usage between two keys from below and above.
`ustore_scan_unordered_k` is reserved for scans that may hand keys over out of order, and no engine acts on it yet.

### Cursors

A cursor walks the keys of one collection in pages, every pull resuming past the last key the pull before it handed out, and reads their values in the same call when `values` asks for them:

```c
ustore_blobs_cursor_t cursor = NULL;
ustore_blobs_cursor_init(&(ustore_blobs_cursor_init_t){
    .cursor = &cursor,
    .database = database,
    .outcome = &outcome,
    .collection = ustore_collection_main_k,
});

ustore_size_t const wanted = 256;
ustore_size_t written = wanted, walked = 0;
ustore_key_t const* keys = NULL;
ustore_strings_view_t values = {0};
while (outcome.status == ustore_status_success_k && written == wanted) {
    ustore_blobs_cursor_next(&(ustore_blobs_cursor_next_t){
        .database = database,
        .outcome = &outcome,
        .cursor = cursor,
        .keys = &keys,
        .values = &values,
        .written = &written,
        .arena = arena,
        .wanted = wanted,
    });
    walked += written;
}
ustore_blobs_cursor_free(cursor);
```

`ustore_blobs_cursor_init` reads nothing, and its `view` and `start_key` choose what the walk sees and where it starts, the head and the first key when zeroed.
A cursor keeps only the key its next pull starts at, so a key written behind it is never handed out and one written ahead of it is, while `ustore_blobs_scan` stays the batched listing of many ranges.
A page shorter than `wanted` exhausts the walk for good: every later pull writes zero keys without reading, even after writes ahead of where it stopped.
A pull that fails or is cancelled leaves the cursor where it was, and a pull at the head lists and reads at two moments, so a key removed between them reads as an empty value.
The cursor holds its database open, and the transaction or snapshot it reads through, until `ustore_blobs_cursor_free`, so the caller may free those handles first.
It serves one pull at a time, and a second pull naming it finishes `ustore_status_contended_k`.
`ustore_strings_cursor_init` opens the same walk over the strings of one `prefix` from an optional `start_string`, copying both, and its pulls write the strings and their values as tapes.

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

ustore_blobs_write(&(ustore_blobs_write_t){.database = database, .outcome = &outcome, .transaction = transaction, /* ... */});
ustore_blobs_read(&(ustore_blobs_read_t){
    .database = database, .outcome = &outcome,
    .view = ustore_transaction_view(transaction, ustore_watch_keys_k), /* ... */});

ustore_transaction_commit(&(ustore_transaction_commit_t){
    .database = database,
    .outcome = &outcome,
    .transaction = transaction,
});
ustore_transaction_free(transaction);
```

`ustore_transaction_init` begins a new transaction into a `NULL` handle, left `NULL` on failure, and resets a transaction of the same database in place, keeping its handle whatever the outcome, so a reset that fails leaves it fit only to be freed or initialized again.
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

ustore_blobs_read(&(ustore_blobs_read_t){
    .database = database, .outcome = &outcome, .view = ustore_snapshot_view(snapshot), /* ... */});

ustore_snapshot_free(snapshot);
```

`ustore_snapshot_init` can export its `read_stamp`, the stamp the snapshot reads at, which LevelDB, keeping its sequence numbers private, reports as zero.
A snapshot lives no longer than the process that took it, and holds its database open until freed, as a transaction does, so `ustore_database_free` closes the database only once the last of them is freed.
`ustore_snapshot_save` writes one out as a new database of the same engine, holding exactly the collections and names the snapshot records, which that engine's open then serves, so a save is how a view outlives the process.
RocksDB and LevelDB create its `path`, while the `ram` engine writes its image into the directory `path` names.
Persistent snapshots, recorded in the catalog under UUIDv7 identifiers and kept across restarts until dropped, are planned.

### Progress and Cancellation

Scans, cursor pulls, saves and compactions take an optional `progress` callback and the `progress_state` handed to it.
Each reports in the units its verb consumes: finished tasks for a scan, one unit for a pull, bytes of keys and values for a save, and bytes compacted where the engine measures them, as RocksDB does, or collections elsewhere.
A callback never runs concurrently with itself, however many threads do the work, never runs after the call is final, and never sees `finished` decrease:

```c
static ustore_continuation_t within_budget(ustore_size_t finished, ustore_size_t total, void* state) {
    ustore_size_t const budget = *(ustore_size_t const*)state;
    fprintf(stderr, "saved %llu of %llu bytes\n", (unsigned long long)finished, (unsigned long long)total);
    return finished <= budget ? ustore_continue_k : ustore_cancel_k;
}
```

```c
ustore_size_t budget = 1ull << 30;
char const path[] = "/var/backups/ustore";
ustore_snapshot_save(&(ustore_snapshot_save_t){
    .snapshot = snapshot,
    .outcome = &outcome,
    .path = {(uint8_t const*)path, sizeof(path) - 1},
    .progress = within_budget,
    .progress_state = &budget,
});
```

Returning `ustore_cancel_k` is the only way to stop a call, which then answers `ustore_status_cancelled_k`:

- scan and pull outputs stay `NULL`, and a cancelled pull leaves its cursor where it was;
- a cancelled save removes what it wrote, as a failed one does;
- a cancelled compaction stops between collections, keeping what it compacted.

The callback runs on the thread that runs the call, the caller's for a blocking call and one of the executor's for a deferred one, so a binding's callback enters its language's runtime there.

### Documents

A document collection stores JSON text, so reading a document back through `ustore_blobs_read` may not return the exact bytes written.
A field addresses a value inside a document, either as a top-level key such as `_id` or as an RFC 6901 JSON Pointer such as `/posts/0/text`, while an empty field or a zeroed `fields` view addresses the whole document.

- `ustore_docs_write` upserts, updates or inserts whole documents or fields, or applies RFC 6902 patches or RFC 7386 merge patches, as its `modification` selects; whole documents are removed with `ustore_blobs_remove`.
- `ustore_docs_read` reads whole documents or fields, as JSON or as values printed into text.
- `ustore_docs_gist` lists the unique JSON Pointers to the leaves of the requested documents.
- `ustore_docs_gather` reads many fields of many documents into typed columns, with validity, conversion and collision bitmaps per column.

```c
ustore_key_t key = {.uuid.bytes[15] = 1};
char const document[] = "{\"person\": \"Alice\", \"age\": 27}";
uint32_t const document_offsets[2] = {0, sizeof(document) - 1};
ustore_docs_write(&(ustore_docs_write_t){
    .database = database,
    .outcome = &outcome,
    .count = 1,
    .keys = {&key, sizeof(ustore_key_t)},
    .values = {.layout = ustore_strings_u32_tape_k, .data.u32_tape = {(uint8_t const*)document, document_offsets}},
    .type = ustore_docs_field_type_json_k,
    .modification = ustore_docs_modify_upsert_k,
});

char const field[] = "/person";
uint32_t const field_offsets[2] = {0, sizeof(field) - 1};
ustore_strings_view_t person = {0};
ustore_docs_read(&(ustore_docs_read_t){
    .database = database,
    .outcome = &outcome,
    .count = 1,
    .keys = {&key, sizeof(ustore_key_t)},
    .fields = {.layout = ustore_strings_u32_tape_k, .data.u32_tape = {(uint8_t const*)field, field_offsets}},
    .values = &person,
    .arena = arena,
    .outputs_type = ustore_docs_field_type_str_k,
});
```

A gather takes `fields_count` fields as one strings view and the type of every column in `outputs_types`, and answers one `ustore_docs_column_view_t` per field in the arena.
A column holds the validity, conversion and collision bitmaps of its rows beside either the scalars or a tape of the strings.

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

ustore_collection_id_t graph = {0};
ustore_collection_create(&(ustore_collection_create_t){
    .database = database,
    .outcome = &outcome,
    .modality = ustore_modality_graph_k,
    .collection = &graph,
});

triplet_t const triplets[2] = {
    {{.uuid.bytes[15] = 1}, {.uuid.bytes[15] = 2}, {.uuid.bytes[15] = 9}},
    {{.uuid.bytes[15] = 2}, {.uuid.bytes[15] = 3}, {.uuid.bytes[15] = 10}},
};
ustore_graph_write_edges(&(ustore_graph_write_edges_t){
    .database = database,
    .outcome = &outcome,
    .count = 2,
    .collections = {&graph, 0},
    .edges = {&triplets[0].edge, sizeof(triplet_t)},
    .sources = {&triplets[0].source, sizeof(triplet_t)},
    .targets = {&triplets[0].target, sizeof(triplet_t)},
});

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

Every found edge takes three keys of `edges`: its source, its target and its identifier.
The edges of vertex `i` span from edge `edges_offsets[i]` up to edge `edges_offsets[i + 1]`, so a missing vertex spans none, and `edges_offsets` is required whenever `edges_per_vertex` is asked for.
Outgoing edges come first, sorted by target, then incoming edges, sorted by source, and a missing vertex has the degree `ustore_vertex_degree_missing_k`.
Roles matter only in directed graphs, where `ustore_vertex_role_source_k` and `ustore_vertex_role_target_k` select one direction, and roles with `NULL` data stand for `ustore_vertex_role_any_k`.

### Strings

A strings collection, created with `ustore_modality_strings_k`, keys its values by variable-length strings, kept in the order of their bytes.
Its strings and their values both arrive as `ustore_strings_view_t`, so `ustore_strings_write`, `_remove`, `_read`, `_scan` and `_cursor_*` mirror the blob calls with strings in place of keys.
A scan lists up to `wanted` strings under every prefix as one tape, split by task through `count + 1` offsets, and resumes from `start_strings`, while a strings cursor walks one prefix as a blobs cursor walks keys.

### Pages

A pages collection stores ordered documents, edited by one verb per kind of edit, each taking only its own fields: `ustore_pages_import`, `_insert`, `_remove`, `_move`, `_splice`, `_set_attributes` and `_remove_attributes`.
`ustore_pages_read` reads documents back into an arena, and `ustore_pages_history` lists the edits that produced them.
No engine implements these calls yet, so each of them answers `ustore_status_unsupported_k`.

## C++ Interface

### Conventions

Everything lives in `unum::ustore`, and `#include <ustore/ustore.hpp>` brings in all of it.
Fallible calls return a `status_t`, a `status_code_t` beside a static message, or an `expected` that holds either a value or the `status_t` explaining its absence; both are `[[nodiscard]]`.
Dereferencing an `expected<value_type>` yields the `value_type`, and `auto [value, status]` decomposes one whose value is default constructible.
`key_from_integer` spells a non-negative integer as a key, and `key_after` answers the key right after another.
Nothing copies values out of the arena, and nothing throws.

```cpp
using namespace unum::ustore;

auto [db, status] = database_t::init_ram();
if (status.failed())
    std::fprintf(stderr, "%s\n", status.message);
```

`database_t::init_ram`, `open_ram`, `open_rocksdb`, `view_rocksdb`, `open_leveldb` and `open_nvme` wrap the C opens of the same names, taking their argument structs as options.

### Blobs

A collection handle is cheap to construct and holds no state, and its `[]` operator returns a `blobs_ref` over one key or a batch of keys that reads and writes through the C interface:

```cpp
blobs_collection_t main = db.main();

status_t status = main[key_from_integer(42)].assign("purpose of life");
expected<owned_value_t> value = main[key_from_integer(42)].value();
bool const matches = value && *value == "purpose of life";

status = main[{key_from_integer(43), key_from_integer(44)}].assign("same value");
expected<bool> present = main[key_from_integer(43)].present();
status = main[key_from_integer(43)].remove();

auto [arena, arena_status] = arena_t::make();
std::array<ustore_key_t, 3> keys {key_from_integer(65), key_from_integer(66), key_from_integer(67)};
expected<values_view_t> values = main.read(keys, arena);
for (value_view_t found : *values)
    std::printf("%zu bytes\n", found.size());
```

Read without an arena, `value`, `present` and `length` are shorthand: a single key answers an `owned_value_t`, a `bool` or a length, and a batch an `owned<values_view_t>`, an owned `bits_view_t` or owned `lengths_view_t`.
Given an `arena_t&`, as `main.read(keys, arena)` or `main[keys].value(arena)`, `.present(arena)` and `.length(arena)` are, the same reads answer plain views into that arena.
`remove` removes the keys, while `clear_values` keeps them and empties their values.
`keys` and `pairs` range over a collection in key order from a `start_key` to an `end_key`, and `size_range` estimates how many keys it holds.
Each stream is shorthand for a range-based loop, which has no place for an arena, so it pulls pages from a cursor into one arena it owns for its lifetime.

### Arenas

Shorthand and verbs trade convenience for reuse, as in every binding:

- __Shorthand makes a one-off arena.__
  `main[key].value()` calls `ustore_arena_init`, reads into that arena, and hands it to its answer, which frees it on destruction, so nothing is copied and no handle keeps state.
  A scalar answer, such as a presence or a degree, is copied out and the arena freed at once.
  The price is one arena per call, so a loop reads through a verb instead.
- __Verbs reuse a caller's arena.__
  `arena_t::make(executor)` places one where its executor computes, or on host pages for calls on the calling thread, and a loop passing it to every call allocates once.
  Every call rewinds it, so its views last until the next call given that arena; to keep them longer, pass that call another arena.

```cpp
auto [arena, arena_status] = arena_t::make(executor);
for (std::span<ustore_key_t const> batch : batches) {
    auto [values, read_status] = db.on(executor).main().read(batch, arena);
    if (read_status.failed()) break;
    consume(values); // valid until the next call given `arena`
}
```

An `owned_value_t` derives from the `value_view_t` it holds, so it compares and converts like one, and `view()` slices that plain view off.
Collections, transactions and snapshots hold no arena, so threads share one handle, each passing its own arena.

Walks split the same way: the `keys`, `pairs` and `prefixed` streams own one arena each, while `blobs_cursor_t` and `strings_cursor_t` take the caller's on every pull, so a loop reuses one arena it keeps:

```cpp
auto [cursor, cursor_status] = blobs_cursor_t::make(db.receiver(), ustore_collection_main_k);
std::size_t const wanted = 256;
for (std::size_t pulled = wanted; cursor_status.succeeded() && pulled == wanted;) {
    auto [page, page_status] = cursor.next_pairs(wanted, arena);
    if (page_status.failed()) break;
    consume(page.values); // valid until the next call given `arena`
    pulled = page.keys.size();
}
```

`make` opens the walk at the view its receiver reads and from an optional start key, `next` pulls keys alone and `next_pairs` keys with their values, and a page shorter than `wanted` exhausts the walk for good.
`strings_cursor_t::make` takes a prefix and an optional start string, and its `next` pulls strings in the order of their text.

### Named Collections and Transactions

```cpp
expected<docs_collection_t> people = db.find_or_create<docs_collection_t>("people");

expected<transaction_t> transaction = db.transaction();
blobs_collection_t inside = transaction->main();
status = inside[key_from_integer(42)].assign("written inside a transaction");
expected<commit_stamp_t> committed = transaction->commit();

expected<snapshot_t> snapshot = db.snapshot();
auto pinned = snapshot->main()[key_from_integer(42)].value();
```

Each collection type names the modality it holds as `modality_k`, so `create` records it, `find` refuses a collection holding another with `status_code_t::invalid_argument_k`, and `find_or_create` creates only when no collection carries the name.
A handle taken from a transaction reads and writes through it, and a transaction commits only when `commit` is called, answering the commit's position in commit order.
A snapshot hands out the read-only twin of every collection, such as `docs_read_collection_t` for `find<docs_collection_t>`, so nothing writes through it, and `save(path)` writes it out as a new database.

### Describing, Clearing and Compacting

`describe`, `clear` and `compact` wrap the C calls of the same names, and a save or a compaction takes an optional progress callable:

```cpp
expected<ustore_database_description_t> description = db.describe();
if (description)
    std::printf("%s holds %llu keys\n", ustore_engine_name(description->engine),
                (unsigned long long)description->keys);
status = db.clear(ustore_drop_pairs_k);

ustore_size_t const budget = 1ull << 30;
auto within_budget = [&](ustore_size_t finished, ustore_size_t total) noexcept {
    std::fprintf(stderr, "saved %llu of %llu bytes\n", (unsigned long long)finished, (unsigned long long)total);
    return finished <= budget ? ustore_continue_k : ustore_cancel_k;
};
status = snapshot->save("/var/backups/ustore", within_budget);
status = db.compact();
```

`clear` empties every collection as its `ustore_drop_mode_t` names, so `ustore_drop_collection_k` drops every named collection, which the `ram` and RocksDB engines refuse while a snapshot is open.
A progress callable answers `ustore_continue_k` or `ustore_cancel_k` and is `noexcept`, so one that may throw does not compile, and the call borrows it until it is final, so a temporary does not compile either.

### Documents

A `docs_collection_t` hands out `docs_ref` proxies, which read and write JSON through `ustore_docs_read` and `ustore_docs_write`:

```cpp
docs_collection_t people = db.main<docs_collection_t>();
status_t status = people[key_from_integer(1)].assign(R"({"person": "Alice", "age": 27, "height": 1})");
status = people[key_from_integer(2)].assign(R"({"person": "Bob", "age": "27", "weight": 2})");

expected<owned_value_t> document = people[key_from_integer(1)].value();
expected<owned_value_t> person = people[collection_key_field_t {key_from_integer(1), "/person"}].value(ustore_docs_field_type_str_k);

status = people[key_from_integer(1)].patch(R"([{"op": "replace", "path": "/age", "value": 28}])");
status = people[key_from_integer(2)].merge(R"({"weight": null})");
expected<values_view_t> documents = people[{key_from_integer(1), key_from_integer(2)}].value(arena);
expected<sz::strings_view_t> paths = people[{key_from_integer(1), key_from_integer(2)}].gist(arena);
```

Field access through a proxy is shorthand like any other read without an arena, while `gist` and `gather` export into the arena they are given.
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
auto table = people[keys].gather(header, arena);
auto ages = table->column<0>();
for (std::size_t row = 0; row != ages.size(); ++row)
    if (ages[row].valid)
        std::printf("%d\n", ages[row].value);
```

A header whose column types are known only at runtime views an array of `field_type_t`, and its columns are typed at the point of use:

```cpp
field_type_t const columns[] = {
    {"age", ustore_docs_field_type_i32_k},
    {"age", ustore_docs_field_type_str_k},
    {"person", ustore_docs_field_type_str_k},
};
auto dynamic = people[keys].gather(make_table_header(columns), arena);
auto names = dynamic->column(2).as<std::string_view>();
```

A table is a view: one `ustore_docs_column_view_t` per column lives in the arena, while its index and header view the keys and the header the gather was given.
Every cell carries `valid`, `converted` and `collides` flags beside its `value`, which is meaningful only when `valid`.

### Graphs

A `graph_collection_t` batches edges and vertices, and can be used from inside a transaction like any other handle:

```cpp
auto [graph, status] = db.create<graph_collection_t>("graph");
status = graph.write_edge(edge_t {key_from_integer(1), key_from_integer(2), key_from_integer(9)});

std::array<edge_t, 2> triplets {edge_t {key_from_integer(2), key_from_integer(3), key_from_integer(10)}, edge_t {key_from_integer(3), key_from_integer(4), key_from_integer(11)}};
status = graph.write_edges(edges(triplets));

expected<ustore_vertex_degree_t> degree = graph.degree(key_from_integer(2));
expected<bool> present = graph.present(key_from_integer(2));
auto successors = graph.successors(key_from_integer(2), arena);
auto between = graph.edges_between(key_from_integer(1), key_from_integer(2), arena);
auto touching = graph.read_edges(key_from_integer(3), arena, ustore_vertex_role_target_k);

std::array<ustore_key_t, 3> vertices {key_from_integer(1), key_from_integer(7), key_from_integer(2)};
expected<edges_view_t> leaving = graph.read_edges(strided(vertices).immutable(), arena);
edges_view_t of_missing = leaving->of_vertex(1); // empty, as vertex 7 is missing
```

`degree` and `present` of one vertex are shorthand, copying a scalar out of a one-off arena, while edges, neighbors and the degrees of a batch land in the arena the call is given.
The edges of a batch come in the order of its vertices, and their `offsets` split them by vertex, so `of_vertex(i)` views the edges of vertex `i` and a missing vertex has none.
`edges` streams every edge of the collection, and `vertex_stream` every vertex, both reading ahead in batches.

### Strings

A `strings_collection_t` keys arbitrary bytes by variable-length strings, kept in the order of their text:

```cpp
auto [words, status] = db.create<strings_collection_t>("words");
status = words.assign("apple", "red");
status = words.assign("apricot", "orange");

expected<owned_value_t> apple = words.value("apple");
std::array<value_view_t, 2> const asked {"apple", "cherry"};
expected<values_view_t> found = words.read(asked, arena);
expected<sz::strings_view_t> page = words.prefixed_page("ap", 16, arena);
expected<std::size_t> count = words.size("ap");
```

`value` and `present` of one string are shorthand, while `read` and `prefixed_page` export into the arena they are given.
`prefixed` and `prefix_begin` stream every string of a prefix through one arena of their own, pulling pages from a `strings_cursor_t`.

### Receivers and Deferred Calls

The receiver chooses where a call runs, and the verb stays.
`db.on(executor)`, `transaction->on(executor)` and `snapshot->on(executor)` hand out the same collections, proxies and operators, whose calls block on the executor:

```cpp
auto [executor, executor_status] = executor_handle_t::for_ram({.threads = 4});
blobs_collection_t on_executor = db.on(executor).main();
status = on_executor[key_from_integer(42)].assign("purpose of life");
```

`deferred(executor)` hands out the deferred twin of every collection, whose verbs mirror the C calls and answer a `deferred<verb>` once submitted.
`main<docs_collection_t>()` and `find<graph_collection_t>(name)` pick the twin by modality, read-only at a snapshot, while the deferred database's `[]` takes a binary collection by the identifier `create` or `find` resolved, so addressing it never blocks.
`status()` polls without blocking, `std::move(call).wait()` answers what the call produced, and a `deferred` dropped unwaited waits in its destructor:

```cpp
deferred_database_t later = db.deferred(executor);
auto [arena, arena_status] = arena_t::make(executor);
std::array<ustore_key_t, 2> const keys {key_from_integer(1), key_from_integer(2)};
std::array<sz::string_view_t, 2> const texts {R"({"person": "Alice"})", R"({"person": "Bob"})"};

sz::expected<values_tape_t> values = sz::strings_view_t(texts).gather();
if (!values) return;
deferred<blobs_write_t> writing = later.main().write(keys, std::move(values.value));
status = std::move(writing).wait();
deferred<blobs_read_t> reading = later.main().read(keys, arena);
expected<values_view_t> read = std::move(reading).wait();
for (value_view_t found : *read)
    std::printf("%zu bytes\n", found.size());
```

Documents are written and read whole or by field, under any modification, and a gather answers a table together with the rows and columns it views outside the arena:

```cpp
deferred_docs_collection_t people = later.main<docs_collection_t>();
sz::expected<values_tape_t> documents = sz::strings_view_t(texts).gather();
if (!documents) return;
status = people.write(keys, std::move(documents.value), {}, ustore_docs_modify_merge_k).wait();

std::array<sz::string_view_t, 1> const columns {"person"};
sz::expected<values_tape_t> fields = sz::strings_view_t(columns).gather();
if (!fields) return;
auto gathered = people.gather(keys, std::move(fields.value), std::array {ustore_docs_field_type_str_k}, arena).wait();
auto names = gathered->table.column(0).as<std::string_view>();
```

Graphs read edges and degrees for one role of every vertex, and write and remove edges and vertices:

```cpp
deferred_graph_collection_t graph = *later.find<graph_collection_t>("graph");
std::array<edge_t, 2> const chain {edge_t {key_from_integer(1), key_from_integer(2)}, edge_t {key_from_integer(2), key_from_integer(3)}};
status = graph.write_edges(chain).wait();
expected<edges_view_t> leaving = graph.read_edges(keys, arena, ustore_vertex_role_source_k).wait();
expected<std::span<ustore_vertex_degree_t const>> degrees = graph.degrees(keys, arena).wait();
```

Strings are written and read in batches, and a scan answers a page of strings for every prefix:

```cpp
deferred_strings_collection_t words = *later.find<strings_collection_t>("words");
std::array<sz::string_view_t, 2> const spellings {"apple", "apricot"};
sz::expected<values_tape_t> strings = sz::strings_view_t(spellings).gather();
sz::expected<values_tape_t> meanings = sz::strings_view_t(texts).gather();
sz::expected<values_tape_t> prefixes = sz::strings_view_t(std::span {spellings}.first(1)).gather();
if (!strings || !meanings || !prefixes) return;
status = words.write(std::move(strings.value), std::move(meanings.value)).wait();
expected<strings_scan_t::finished_t> scanned = words.scan(std::move(prefixes.value), 16, arena).wait();
```

A transaction defers its own verbs too, `reset` among them:

```cpp
deferred_transaction_t deferred_transaction = transaction->deferred(executor);
status = deferred_transaction.main().remove(keys).wait();
expected<read_stamp_t> staged = deferred_transaction.stage().wait();
expected<commit_stamp_t> stamp = deferred_transaction.commit().wait();
```

The deferred database also creates, drops and lists collections, and describes, clears and compacts itself, while the deferred snapshot saves itself through `save`.
Opening blocks instead, as every constructor does: `init_ram`, `open_ram`, `open_rocksdb`, `view_rocksdb` and `open_leveldb` take the executor to block on as their last argument, while `open_nvme` lends the engine the executor its options name until close:

```cpp
expected<database_t> opened = database_t::open_rocksdb({.directory = bytes_of("/var/lib/ustore")}, executor);
```

Either way the database runs its own calls on the calling thread until a receiver names an executor.

- __Deferred inputs are owned, arenas borrowed.__
  Keys, vertices, edges and column types come as a `std::array` or an `st::basic_vector`, and values, strings and fields as a `values_tape_t`, StringZilla's `sz::strings_t` that `sz::strings_view_t::gather` fills, so a borrowed span or view does not compile.
  The arena comes as an `arena_t&`, borrowed until the call is final, which is safe because freeing an arena waits for the deferred calls naming it, while a temporary arena does not compile.
- __Submission copies the arguments.__
  A `deferred<verb>` keeps one heap block that never moves, holding the outcome, the owned inputs and the outputs the call writes through, while the C argument structure dies on the stack it was filled on.
  `status()` polls through `ustore_outcome_status`, and `wait()` and the destructor wait.
- __Answers view the arena.__
  A finished read answers the views a blocking one does, valid until the next call given that arena, and a finished gather owns its rows, columns and field names beside a table whose cells stay in the arena.
- __A transaction serves one call at a time.__
  Wait on one deferred call through it before submitting the next, which otherwise finishes at once with `status_code_t::contended_k`.
- __The wrappers have no life of their own.__
  They start no threads, keep no counts and hold no hidden arena: freeing a database, transaction or snapshot waits in C for the deferred calls naming it.

## Design

- __This is the C ABI itself.__
  Every name is the one `include/ustore/` declares: a verb `ustore_<noun>_<verb>` takes a `ustore_<noun>_<verb>_t`, and closed sets are C enums.
- __The completion chooses, the verb stays.__
  A `NULL` executor runs a call on the calling thread, an executor with the default completion blocks on it, and `ustore_completion_deferred_k` submits it, all with the same argument struct.
- __Deferred calls report through the same outcome.__
  `ustore_outcome_status` polls and `ustore_outcome_wait` blocks, both with the acquire ordering the final store pairs with.
- __No life of its own.__
  Threads live only in executors and deferred calls only in each executor's deferred table, and every `*_free` waits for the calls naming its handle first.
- __Three shapes in, three out.__
  An input is one run of bytes, a strided run of fixed-width elements, or a ragged batch of strings in an Arrow tape or behind an accessor, and an output is a packed array, a tape of bytes, or packed keys split by offsets.
  So Arrow arrays pass as they are, and a runtime's own objects pass through an accessor, without copies on either side.
- __Arenas are lent and reused, never shared.__
  Every output whose length scales with the call lands in an arena the caller passes, and each call rewinds it, so the previous call's views go stale the moment the next one starts.
- __Nothing costs anything until it is used.__
  Blocking calls take no slot of the deferred table, and an arena allocates only when a call writes into it.
- __One shape for every call.__
  Each verb takes one struct set by designator, so a call names only what it changes and the struct grows without breaking source.
- __Failures are statuses.__
  Every failure is a status in the call's outcome beside a static message, `ustore_status_name` and `ustore_status_from_name` spell statuses both ways, and an engine without a call answers `ustore_status_unsupported_k` rather than missing from the build.
- __Closed sets spell themselves.__
  Every enum a binding names has its `_name` and `_from_name` functions, so a binding builds its own enums from them and keeps no table to drift.
- __Walks hold, scans paginate.__
  A cursor holds its database and its view and resumes past its last key, while a scan holds nothing and starts wherever its caller says, so neither keeps an iterator open over concurrent state.
- __Only progress stops a call.__
  A callback returning `ustore_cancel_k` is the one way to stop a running call, so every other call runs to a final status, and a cancelled one leaves its outputs `NULL` and its cursor where it was.
