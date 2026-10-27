# UStore for C

`c/` is the public C SDK of UStore, and its only interface: every engine and every binding meets here.
The `*.h` headers declare a C11 binary interface that every engine implements — the in-memory `ram` reference engine, the RocksDB and LevelDB key-value stores, and the NVMe engine as a v1 target — so any language with a C FFI can drive the database.
UStore's sources split into four folders:

- `c/`: this SDK, the C ABI in `c/ustore/*.h`, `test.c`, which compiles it as plain C11 against both libraries, and this guide.
- `cpp/`: the C++ wrappers of the same calls in `cpp/ustore/*.hpp`, in `unum::ustore`, with [their own guide](https://github.com/unum-cloud/UStore/blob/main/cpp/README.md).
- `include/`: reserved for the first-party classes and templates that advanced users may reuse to build their own low-level stores, each subfolder `ustore/<dir>/` opening `unum::ustore::<dir>`.
- `src/`: UStore's own instantiations of those templates, the engines behind this C ABI.

`c/ustore/` holds one header per part of the interface:

- `ustore.h`: every C header below.
- `types.h`: statuses, outcomes, completions, progress callbacks, executors, arenas, string batches and the version.
- `db.h`: databases, collections, snapshots, transactions, views, strided runs of keys and enum spellings.
- `blobs.h`: binary values, through `ustore_blobs_write`, `_remove`, `_read`, `_scan`, `_sample`, `_measure` and cursors.
- `strings.h`: string-keyed values, through `ustore_strings_write`, `_remove`, `_read`, `_scan` and cursors.
- `docs.h`: JSON documents, through `ustore_docs_write`, `_read`, `_gist`, `_gather`, `_remove` and `_scan`.
- `graph.h`: graph edges and vertices, and the scans of the vertices.
- `pages.h`: ordered documents, through one verb per edit, `ustore_pages_read` and `ustore_pages_scan`.

Every verb serves only collections of its own modality, and answers `ustore_status_invalid_argument_k` for a collection holding another.

## Installation

UStore builds with CMake into two libraries from one source list, each holding the in-memory engine and every engine the build enables through `USTORE_WITH_ROCKSDB`, `USTORE_WITH_LEVELDB` and `USTORE_WITH_NVME`:

- __The archive__, `ustore_static`, linked as `ustore::static` or its alias `ustore::ustore`, carries the objects of the small libraries every build needs: ForkUnion, StringZilla, yyjson and fmt.
  The storage engines' own libraries, RocksDB, LevelDB and liburing, stay archives of their own, installed beside it as `ustore::rocksdb`, `ustore::leveldb` and `ustore::uring`, which `ustore::static` links, so a CMake consumer still names one target.
  A build without CMake links those archives after `libustore_static`, or takes the shared library instead.
- __The shared library__, `ustore_shared`, linked as `ustore::shared`, links them privately and exports only the functions the headers mark `USTORE_EXPORT`, which its consumers import on Windows through the `USTORE_SHARED` definition the target hands them.

Each engine is its own object of the archive, so a program links only the engines it opens.
`USTORE_WITH_NVME` stays off until the NVMe sources are ported to this interface.

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

`c/` and `cpp/` are both include roots, so every header is spelled `<ustore/...>` in the tree and once installed, where the headers of both land side by side in one `ustore/` folder.

## Conventions

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

## Executors, Blocking and Deferred Calls

An executor owns the threads that run calls and the scratch they use, and a `NULL` executor runs a call on the calling thread.
Its kind picks where calls compute: a RAM or NVMe executor serves every engine on host threads, and a VRAM executor serves the reads and scans an engine computes on its one device, refusing changes, the control plane and every call no device tower serves yet with `ustore_status_unsupported_k`.
`ustore_executor_for_ram` spawns its threads and holds up to `deferred_limit` deferred calls at once, 256 for zero.
Its `ram` options name the memory domains it serves, NUMA nodes as the operating system numbers them, where `NULL` derives every domain the process may allocate on and an unknown one is refused with `ustore_status_invalid_argument_k`.
Each thread keeps its scratch on its local domain when the options name it, and on the first named domain otherwise, as does every arena the executor makes.
Zero threads take 7/8 of the cores local to those domains, rounded down and at least one, leaving the rest to the driver thread, to blocking calls on their callers' threads, and to the background flushes and compactions of RocksDB and LevelDB.
ForkUnion spawns the threads near every domain or near one alone, so naming several domains but not all spreads them over the whole machine.
`ustore_executor_on_pool` borrows a ForkUnion pool another library owns, which must outlive the executor, and places its scratch and arenas by the same `ram` options; a dispatch on a pool that runs another is refused, so a batch finding the pool running its lender's dispatch runs on the executor's own driver thread instead of forking over the lender.
`ustore_executor_for_nvme` spawns the threads of the NVMe engine, and its `nvme` options tell every database `ustore_database_open_nvme` opens on it how to move bytes: the transport, the channels and their buffers, read once at the open.
It may borrow an executor of `ustore_executor_for_vram`, whose CUDA device and scratch limit the compactions take, and which answers `ustore_status_unsupported_k` in a library built without CUDA.
That executor queues every call on one stream of its device, its own or one the caller lends through `stream`, so one host thread keeps the device fed.
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
[The C++ wrappers](https://github.com/unum-cloud/UStore/blob/main/cpp/README.md#receivers-and-deferred-calls) and the Rust and Python bindings choose the executor and the completion by receiver, as `database.on(executor)` and `database.deferred(executor)`, so blocking calls read as they would without executors; the Design sections of `rust/README.md` and `python/README.md` state the rules every binding keeps.
A call stops early only when its progress callback returns `ustore_cancel_k`, as [Progress and Cancellation](#progress-and-cancellation) describes.

## Opening the Database

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

## Describing, Clearing and Compacting

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

## Collections

A database holds no collection until one is created, and every call names the collections it addresses, refusing a `collections` view with `NULL` data with `ustore_status_invalid_argument_k`.
A collection is created with an optional name and the `ustore_modality_t` it holds, and addressed afterwards by the identifier the call answers, a UUIDv7 its create mints once and never reuses:

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

ustore_collection_id_t values = {0};
ustore_collection_create(&(ustore_collection_create_t){
    .database = database,
    .outcome = &outcome,
    .modality = ustore_modality_blobs_k,
    .collection = &values,
});
```

The examples below write binary values into `values` and documents into `movies`.

Each engine records names in the `_names` system collection and every collection's modality and name in `_collections`, which no identifier reaches and no listing shows, so a name starting with `_` is refused, and a name resolves in two phases: once through `ustore_collection_find`, which answers the identifier and the modality of every name of a batch at the head or at a snapshot, and by identifier from then on, as no other call carries one.
Collections are created and dropped only at the head, so their names are never read inside a transaction, and resolving one never joins the conflicts of the work that follows it.
A create records the name in the same step as the collection, refusing a name another collection carries even under a concurrent creator.
A name no collection carries resolves to the nil UUID with its presence bit cleared, and `ustore_collection_drop` with `ustore_drop_collection_k` forgets a collection's name before the collection goes.
`ustore_collection_list` exports the identifier of every collection and, where asked, its modality and its name, as a tape where a collection created without one has an empty name.
A listing describes the database rather than a transaction, so it reads the head or a snapshot.

## Writes and Removals

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
    .collections = {&values, 0},
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
    .collections = {&values, 0},
    .keys = {&pairs[0].key, sizeof(pair_t)},
    .values = {.layout = ustore_strings_accessor_k, .data.accessor = {get_texts, pairs}},
});
```

The accessor runs on the thread that runs the call, in blocks of up to 64 indices, never concurrently with itself and never after the call is final.
An accessor writing the same view for every index writes one value under every key, as a zero stride repeats one key.
Removing keys is its own call, so no value ever deletes, and an empty value is still a value:

```c
ustore_blobs_remove(&(ustore_blobs_remove_t){
    .database = database, .outcome = &outcome, .count = 1, .collections = {&values, 0}, .keys = {&key, 0}});
```

A batch is atomic: if one task fails, none is applied.
`durability` chooses whether the call returns once the write is buffered or once it is flushed.

## Reads and Arenas

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
    .collections = {&values, 0},
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

## Scans

`ustore_blobs_scan` exports up to `wanted` keys per task, starting from `start_keys[index]`, or from the smallest key after it, and exports no values:

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

Start keys with `NULL` data start every scan at the first key.
The next page starts from the key right after the last exported one, its 16 bytes incremented as one big-endian integer, and a `ustore_blobs_read` of the found keys fetches their values.
`ustore_blobs_sample` draws up to `wanted` keys uniformly at random, and `ustore_blobs_measure` bounds the cardinality, value bytes and disk usage between two keys from below and above.
`ustore_scan_unordered_k` is reserved for scans that may hand keys over out of order, and no engine acts on it yet.

## Cursors

A cursor walks the keys of one collection in pages, every pull resuming past the last key the pull before it handed out, and reads their values in the same call when `values` asks for them:

```c
ustore_blobs_cursor_t cursor = NULL;
ustore_blobs_cursor_init(&(ustore_blobs_cursor_init_t){
    .cursor = &cursor,
    .database = database,
    .outcome = &outcome,
    .collection = values,
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

## Transactions and Views

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

## Snapshots

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
Persistent snapshots, recorded in a system collection under UUIDv7 identifiers and kept across restarts until dropped, are planned.

## Progress and Cancellation

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

## Documents

A document collection stores JSON text, so reading a document back through `ustore_blobs_read` may not return the exact bytes written.
A field addresses a value inside a document, either as a top-level key such as `_id` or as an RFC 6901 JSON Pointer such as `/posts/0/text`, while an empty field or a zeroed `fields` view addresses the whole document.

- `ustore_docs_write` upserts, updates or inserts whole documents or fields, or applies RFC 6902 patches or RFC 7386 merge patches, as its `modification` selects.
- `ustore_docs_read` reads whole documents or fields, as JSON or as values printed into text.
- `ustore_docs_gist` lists the unique JSON Pointers to the leaves of the requested documents.
- `ustore_docs_gather` reads many fields of many documents into typed columns, with validity, conversion and collision bitmaps per column.
- `ustore_docs_remove` removes whole documents, and `ustore_docs_scan` lists their keys as `ustore_blobs_scan` lists the keys of blobs.
- `ustore_docs_sample` draws document keys at random as `ustore_blobs_sample` draws the keys of blobs.

```c
ustore_key_t key = {.uuid.bytes[15] = 1};
char const document[] = "{\"person\": \"Alice\", \"age\": 27}";
uint32_t const document_offsets[2] = {0, sizeof(document) - 1};
ustore_docs_write(&(ustore_docs_write_t){
    .database = database,
    .outcome = &outcome,
    .count = 1,
    .collections = {&movies, 0},
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
    .collections = {&movies, 0},
    .keys = {&key, sizeof(ustore_key_t)},
    .fields = {.layout = ustore_strings_u32_tape_k, .data.u32_tape = {(uint8_t const*)field, field_offsets}},
    .values = &person,
    .arena = arena,
    .outputs_type = ustore_docs_field_type_str_k,
});
```

A gather takes `fields_count` fields as one strings view and the type of every column in `outputs_types`, and answers one `ustore_docs_column_view_t` per field in the arena.
A column holds the validity, conversion and collision bitmaps of its rows beside either the scalars or a tape of the strings.

## Graphs

A graph collection stores edges as triplets of a source, a target and an edge identifier, indexed from both ends.
The graph calls address a collection created with `ustore_modality_graph_k`, which the in-memory engine keeps apart from every collection of values.
Several edges between the same two vertices need distinct edge identifiers, and an edge without one gets `ustore_edge_id_default_k`.

- `ustore_graph_write_edges` adds edges together with their vertices.
- `ustore_graph_write_vertices` adds vertices without edges.
- `ustore_graph_read_edges` exports the degree and the edges of every requested vertex in the requested role.
- `ustore_graph_remove_edges` removes edges and keeps their vertices.
- `ustore_graph_remove_vertices` removes vertices together with all their edges.
- `ustore_graph_scan_vertices` lists the vertices, those without edges included, as `ustore_blobs_scan` lists keys; a missing vertex reads with the degree `ustore_vertex_degree_missing_k`.

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

## Strings

A strings collection, created with `ustore_modality_strings_k`, keys its values by variable-length strings, kept in the order of their bytes.
Its strings and their values both arrive as `ustore_strings_view_t`, so `ustore_strings_write`, `_remove`, `_read`, `_scan` and `_cursor_*` mirror the blob calls with strings in place of keys.
A scan lists up to `wanted` strings under every prefix as one tape, split by task through `count + 1` offsets, and resumes from `start_strings`, while a strings cursor walks one prefix as a blobs cursor walks keys.

## Pages

A pages collection stores ordered documents, edited by one verb per kind of edit, each taking only its own fields: `ustore_pages_import`, `_insert`, `_remove`, `_move`, `_splice`, `_set_attributes` and `_remove_attributes`.
`ustore_pages_read` reads documents back into an arena, and `ustore_pages_scan` lists the keys of the pages.
A page stores only its current shape, so no call lists the edits that produced it.
Every engine serves them, importing and rendering XML and plain text, while HTML, Markdown and the JSON tree answer `ustore_status_unsupported_k`.
An edit watches the page it rewrites, except on a pages collection of the NVMe engine, which keeps the edits themselves, so two transactions editing one page both commit.
An edit of a missing page answers `ustore_status_not_found_k`, an edit of a missing element inside a page does nothing, and a Nil element removes the whole page.

## Design

- __This is the C ABI itself.__
  Every name is the one `c/ustore/` declares: a verb `ustore_<noun>_<verb>` takes a `ustore_<noun>_<verb>_t`, and closed sets are C enums.
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
