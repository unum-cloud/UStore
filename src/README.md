# Engines

Every engine implements the C ABI of `c/ustore/*.h` over its own storage, and joins the router's sources in the one source list both the `ustore_static` archive and the `ustore_shared` library build from, so documents and graphs work on each of them.
`src/lib.hpp` is the router every exported call goes through, and each public header has one source implementing it: `types.cpp`, `db.cpp`, `blobs.cpp`, `strings.cpp`, `docs.cpp`, `graph.cpp` and `pages.cpp`.
Those sources only route: each engine includes the layers it serves a modality through.
`docs_layer.hpp` and `graph_layer.hpp` keep documents and graphs as read-modify-writes over the codecs of `include/ustore/docs/` and `include/ustore/graph/`, through the unchecked whole-value verbs of the one engine instantiating their `*_over_blobs` templates.
`pages_layer.hpp` keeps pages the same way, each as one value under its page key: its current shape, the dense element tree of `include/ustore/pages/`.
The document and page paths take the reads and writes of an engine as an object, so `ram` runs them over its own stores.
Every verb serves only collections of its own modality: each engine's resolver refuses any other with the one status of `collection_holds_another_modality`, and its public blobs verbs wrap the unchecked ones in that check.
The reusable templates the engines instantiate live in `include/ustore/`, which [its own guide](../include/ustore/README.md) describes.
The engines fill three roles: `ram` is the reference engine in memory, NVMe is the hyper-converged engine on NVMe drives and GPUs, and RocksDB and LevelDB bring third-party key-value stores behind the same calls.
Engines aim for the highest isolation `ram` delivers, each reports the level its transactions actually deliver, and the conformance suite checks every advertised level against behaviour.

| Engine  | Directory              | Role                  | Builds today | Isolation                              |
| :------ | :--------------------- | :-------------------- | :----------- | :------------------------------------- |
| `ram`   | `src/ram/`             | reference, in memory  | always       | `strict_serializable` or `snapshot`    |
| RocksDB | `src_oracles/rocksdb/` | third-party key-value | optional     | start snapshot, validated when staging |
| LevelDB | `src_oracles/leveldb/` | third-party key-value | optional     | snapshot reads, no transactions        |
| NVMe    | `src/nvme/`            | hyper-converged       | v1 target    | start snapshot, validated by stamps    |

## RAM

`src/ram/` is the reference in-memory engine, and the one every other engine is checked against.
Every collection keeps a stamp-based SmashTable `partitioned_store` of its own, of the kind its modality picks: UUID keys mapped onto values for blobs, onto JSON text for documents and onto dense element trees for pages, strings mapped onto values for strings, and the half-edges of a graph, so no stored key carries its collection.
`src/ram/docs.hpp` and `src/ram/pages.hpp` edit documents and pages in place, reading, modifying and writing them back inside one transaction, the caller's or one of the call's own retried on conflicts.
The `_collections` system collection maps the UUID of every collection onto its modality, its name and its store, and the `_names` system collection maps every name onto its collection's UUID.
Every store is built into one commit order, and a `dynamic_transaction_group` joins whichever stores a transaction reaches, so it publishes under one stamp and a snapshot reads all of them at it.
A stage reports the group's `snapshot()` as its read stamp and a commit its `commit_stamp()`, never the `generation()` that only tags the group's uncommitted versions.
Dropping a collection retires its store at the drop's stamp, and the store is freed once no reader holds an older one.
One library serves both isolation levels: the `isolation` field of `ustore_database_init_ram_t` or `ustore_database_open_ram_t` picks one at open, each exported call visits the chosen instantiation once, and `ustore_database_describe` reports it.

- `strict_serializable_k` validates every read and window, so `ustore_watch_none_k` has nothing to switch off.
- `snapshot_k` validates only the reads a transaction watches, trading write skew for cheaper commits.

Snapshots map onto the versions the store already keeps.
`ustore_database_init_ram` starts a database in memory alone, while one `ustore_database_open_ram` serves persists in its directory as one binary image, rewritten whole on a flushed commit and on close.
A compaction vacuums one collection at a time, reporting one unit per collection, then frees the retired stores and rewrites the image, and a save writes the snapshot's collections as an image into the directory it names, removing `ram.image.partial` when cancelled.

## RocksDB

`src_oracles/rocksdb/` keeps each collection as a column family of a RocksDB `TransactionDB`, named after its UUID: UUID keys and strings alike under the bytewise comparator, and the `_names` and `_collections` system collections in families of their own that no identifier reaches, while the `default` family RocksDB requires stores nothing.
Every write reaches RocksDB's log, which stays in memory until a flushed write or commit, a full buffer or a clean close writes it out, so a crash keeps a consistent prefix of the commits across every collection.
A transaction reads the snapshot its first read or write took, through its own buffered writes, and records the keys it watches unless a call passes `ustore_watch_none_k`.
Nothing is locked until it stages, when RocksDB's pessimistic transaction locks and validates every watched read and buffered write and prepares them into the log, so conflicts surface at stage or commit as on `ram`.
A stage reports its snapshot's sequence number as the read stamp, taking that snapshot then if the transaction never read or wrote.
A commit reports the sequence number it was written at, and a snapshot the one it pins.
Scans are not validated, and a prepared transaction a crash leaves behind is rolled back when the database opens.
This is one design for durability and two-phase commits rather than the settled one, and a log kept above the engine, as a data-lake coordinator would own, remains open.

`ustore_database_view_rocksdb` opens a directory through `DB::OpenForReadOnly` instead, with the data directories the database was opened with, since RocksDB finds table files placed outside the directory only through them.
The router refuses every call that would change such a database with `ustore_status_read_only_k` before the engine sees it.
A snapshot records the handle of every family it sees, and a family dropped while a snapshot is open stays retired until the last snapshot is freed, so a save copies exactly the snapshot's collections and names.
A compaction flushes and compacts one family at a time, reporting the bytes of its memtable and table files.

## LevelDB

`src_oracles/leveldb/` wraps one LevelDB database with snapshots and collections, but without transactions: `ustore_transaction_init` answers `ustore_status_unsupported_k`.
LevelDB keeps one keyspace in bytewise order, so every stored key leads with the UUID of its collection, keeping each collection one contiguous run, and the collection's modality shapes the rest of the key.
The `_collections` and `_names` system collections are two more collections under reserved UUIDs:

```c
struct pairs       { uint8_t collection[16]; uint8_t key[16]; };    // blobs, documents, graphs and pages
struct strings     { uint8_t collection[16]; char string[]; };      // strings collections
struct collections { uint8_t collection[16]; uint8_t listed[16]; }; // all 0xFF, valued by the modality and the name
struct names       { uint8_t collection[16]; char name[]; };        // all 0xFF but a last 0xFE, valued by UUID
```

Every collection a call names resolves through `_collections` first, so no call reaches either system collection.
A create writes the collections entry and the name in one synced batch.
A drop removes the entry and the name first, synced, and then the collection's run, so a crash leaves an unlisted run rather than a listed empty collection.
Its isolation stops at reading from a snapshot, whose read stamp reads zero, as LevelDB keeps sequence numbers private.
A description reports zero keys, since LevelDB keeps no count, and a compaction compacts one collection's run at a time, in key order.
A save copies the snapshot into a new database a memtable-sized batch at a time, and destroys that database when cancelled.

## NVMe

`src/nvme/` is the hyper-converged engine, and the one UStore's hardware paths serve.
A `database` holds collections, each in an `nvme_store` over an `lsm_tree` of `lsm_level`s whose `lsm_sorted_run` files open with a `sorted_run_index` of `key_slot`s ahead of their `stored_value`s, with optimistic transactions.
`src/nvme/variants.hpp` serves every slot of `engine_t` the RAM engine serves, its snapshots as stamps pinned in the database's commit ledger, once per CPU class of `src/lib.hpp`, and `src/nvme/engine.cpp` opens a database with the newest class its ceiling fits, the running CPU's or the pinned `cpu_class`'s.
Every collection of the C ABI is one collection of the database under its UUID, keyed by the 16 bytes of `ustore_key_t`.
Its `_collections` system collection maps every collection onto its name, the database's own schema recording its modality, and a strings collection and the names live in the strings store of `include/ustore/strings/nvme_store.hpp`, keyed by whole strings, whose writes and watches join a transaction as a share of it.
A graph collection lives in the graph store of `include/ustore/graph/nvme_store.hpp`, one key per vertex folded from half-edge change deltas, and `src/nvme/graph.hpp` serves the graph calls of the table over it.
A docs collection lives in the docs store of `include/ustore/docs/nvme_store.hpp`, whose field writes, patches and merges append `json_fold_t` change deltas through `src/nvme/docs.hpp`.
A pages collection lives in the pages store of `include/ustore/pages/nvme_store.hpp`, whose edits append `page_fold_t` change deltas that the `*_edits` calls of `src/pages_layer.hpp` encode and `src/nvme/pages.hpp` commits.
Reads of documents and pages take the read paths of `src/docs_layer.hpp` and `src/pages_layer.hpp` over the engine's own reads of those stores, which fold the change deltas.
`USTORE_WITH_NVME` is off by default, and the Python package turns it on; what follows is a v1 target, with SPDK mounts, GPU compaction and GPUDirect Storage reads supported today:

- __Kernel bypass__ — a transport chosen at mount reaches each data directory through io_uring on `O_DIRECT` files, io_uring on NVMe block devices, NVMe passthrough, or SPDK's user-space driver, and falls back to blocking positioned I/O on macOS, Windows and wherever io_uring is refused.
- __SIMD point lookups__ — sorted run indexes, micro-sets and ring buffers become SmashTable components with compile-time SIMD kits, one artifact carrying all of them and a database choosing one when it opens.
- __GPU bulk operations__ — compaction merges sorted runs on the CPU or on a CUDA GPU, runs on both Hopper and Blackwell, and produces the same output as CPU compaction.
- __GPUDirect Storage__ — compaction reads sorted runs from NVMe drives into GPU memory without staging in host memory.
- __Isolation__ — NVMe's transactions reach SmashTable's levels once they run on SmashTable, after an analysis shows no loss of consistency or efficiency.

See [`nvme/README.md`](nvme/README.md) for its layers.

## Utilities

`src/utilities/` holds what several engines share and what reads the C ABI's own types, in `unum::ustore::utilities`, beside the generic utilities of `include/ustore/utilities/` in the same namespace:

- `ram_buffers.hpp` in `include/ustore/utilities/` and `outputs.hpp` here — the arena behind each C ABI call and behind the scratch it works in, the `arena_allocator` SmashTable's containers grow inside it, the `growing_strings` column a ragged output grows in, and the output columns every engine writes into the arena, each named for its verb: the values of a read, the keys of a scan or a sample, the strings of a strings scan, the bounds of a measurement, the collections a listing exports and those a name addresses.
  Whatever grows while a call runs grows in scratch, and each output is copied into the arena once at its final length, so the arena holds no growth slack and no partial copies.
  An arena's first block spans its first allocation rounded up to a page, and every later block at least doubles the newest, so the one-off arena of a small call takes one page.
- `algorithm.hpp` in `include/ustore/utilities/` — the search over sorted batches that the documents and graphs share, the reservoir sampling of keys over the iterators of RocksDB and LevelDB, and the inserts and erases in buffers of trivially copyable elements.
- `blocking_files.hpp` in `include/ustore/utilities/` — whole-file mappings and directory queries, that block the calling thread and report a `status_t`.
- `progress.hpp` — `progress_t`, the one reporter of a call's progress, which runs its callback on one thread at a time, so the counts it sees never decrease, and carries a cancellation to every thread of the call.

`batches.hpp` holds `strings_arg_t`, which reads a ragged input in any of its three layouts once the router has gathered what an accessor writes, beside the other batched arguments every engine takes and the checks every engine runs on them.
`ram_device.hpp` and the arenas of `ram_buffers.hpp` own threads, topology and domain-placed memory over ForkUnion's C ABI, and nothing else calls `fu_*` directly, while `vram_nvidia_device.hpp` is the CUDA device layer GPU compaction builds on.

## Executors

`include/ustore/utilities/ram_device.hpp` holds pools by type: an `executor_t` is spawned and deleted here and is the only kind parked when idle, a `lent_pool_t` is borrowed from another library and never deleted or parked, and both dispatch through a `pool_view_t`.
Every `ustore_executor_t` that `src/types.cpp` builds holds one of them, one scratch per pool thread plus one for the call holding it, and that `holding_call_t`.
Its `device` variant is the kind tag: nothing beyond the pool on RAM, the rings' options on NVMe, and on VRAM a `cuda_executor_t` holding the device ordinal, the one stream every call queues on, made or borrowed, and the scratch limit.
A VRAM executor runs one caller-exclusive host thread, which serves its deferred calls, and every device verb table an engine leaves to `unsupported_vram_tables_k` answers `ustore_status_unsupported_k` until its device tower lands.
`ustore_executor_for_ram` spawns its `executor_t` over a `topology_t`, which owns a `fu_topology_t`: `fu_pool_spawn` over every memory domain, `fu_pool_spawn_near_memory_domain` near one of several, and the whole machine for several but not all, since ForkUnion has no spawn over a subset.
Zero threads take 7/8 of the cores of the compute domains local to the named memory domains, rounded down and at least one.
`ustore_executor_on_pool` builds a `lent_pool_t` through `lent_pool_t::borrow`, which refuses a null pool or one holding no threads.
At creation, a `for_threads` dispatch places each pool thread's scratch on its local memory domain when the options name it, and on the first named domain otherwise, and the call's scratch and every arena the executor makes live where thread 0's does, which the executor keeps as its one piece of placement state; a lent pool refusing that dispatch refuses the creation.

`dispatch` in `src/lib.hpp` takes the executor's `holding_call.claim` with one exchange, and a second call from any thread finishes `ustore_status_contended_k`, so nothing queues and no thread drives the executor.
The holding call records its outcome, the arena, cursor and transaction claims the router took, and raises the named-call counts of its database, of the transaction, snapshot or cursor it names and of its arena.
A blocking call, and every call on a caller-inclusive pool, is served on the calling thread through `call_view`, the call's own scratch at the index past every pool thread's.
A deferred call on an owned caller-exclusive pool is copied into `holding_call.delegated_arguments`, a `std::variant` of every struct that names an executor, and dispatched with `fu_pool_unsafe_for_threads`: pool thread 0 serves it in the caller's place and ends it, while the other threads return at once.
Thread 0 may end the call before the submitter returns, so the dispatch's token stays local to the submitter, and the next call, a drain and a free each call `fu_pool_unsafe_join` with a zero token, which waits for whatever dispatch is in flight: at most the last instructions of the previous call on thread 0.
A lent pool never delegates, since its lender keeps its dispatches and joins, so a deferred call on it runs at submission.
Inside a call, every `for_threads`, `for_n` and `for_n_dynamic` on the executor's pool forks the other threads, thread 0 running its slice in place, so a verb splits its batch the same way whether its caller or thread 0 serves it.
`end_call` ends every call in one order: the call's claims, its named-call counts, the executor claim, then the message and the status with release ordering, reading nothing of the executor after the claim, so a caller seeing the status final may submit again or free the executor at once.
An engine's `ustore_database_init_ram`, `ustore_database_open_*` and `ustore_database_view_rocksdb` name their open as the template argument of `route_init`, which routes it as any other call, its copy the caller's argument struct and its serving verb the one instantiated for that open, since no handle names the engine yet.
`holding_call.body_thread` names the thread serving the call's body, so a drain from a callback on that thread answers `ustore_status_contended_k` instead of waiting on itself.
Each scratch holds whatever a call needs only while it runs, such as the documents the docs layer parses or the edges a graph read stages, rewound before use and kept at its high-water mark until the executor is freed, so the call's arena holds only its outputs.
The router gathers the views an accessor writes into the call's scratch, in blocks of up to 64 indices on the thread that serves the call, so an engine reads every ragged input as a tape or as gathered views.
A call without an executor works in a scratch of its own on the calling thread, whose first page lies on the stack and whose larger blocks are freed once the call is final, so a small call allocates nothing and nothing keeps state behind the caller's back.
An arena, made by `ustore_arena_init` on host pages or on the device of its executor, every transaction handle and every cursor carry a claim word, which the router takes before a call and releases once the engine returns, so a second call on any of them is refused as contended.
The router refuses a `NULL` arena and an arena living where the call's executor does not compute, serves every call through the verb table of its executor's home, `engine_t::host` or `engine_t::vram_nvidia`, leaves every output of a call `NULL` or zero until the engine writes it, and again after a failure or a cancellation, and refuses to reset a transaction through a database it does not belong to.
Every handle carries an `access`, and the router refuses every call naming a transaction, collection creates and drops, clears and compactions with `ustore_status_read_only_k` on a database `ustore_database_view_rocksdb` opened.
Every handle, arenas included, counts the executor calls naming it in `named_calls`, raised at the call's claim and lowered at its end, and each free waits for zero, so no binding keeps counts of its own.
A transaction, a snapshot and a cursor each hold their database open, and a cursor also holds the transaction or snapshot it reads through, so every free drops one hold and the last one frees the handle through its engine.
`ustore_executor_drain` waits until the claim is free, then parks an owned pool for `ustore_mood_parked_k`, and `ustore_executor_free` waits the same way before freeing the scratch.

Cursors live in `src/lib.hpp`, `src/blobs.cpp` and `src/strings.cpp` with no engine slot.
A pull calls the engine's scan slot into the call's scratch, then its read slot into the caller's arena when values are asked for, and copies the keys or the strings into the arena, so the arena holds only outputs.
A blobs cursor resumes at `key_after` of its last key, and a strings cursor at its last string followed by a zero byte, the smallest string after it.
A page shorter than `wanted`, or one ending at the largest key, sets the cursor's `exhausted` flag, after which every pull writes zero without reading.

## Dependencies

| Dependency        | Used by                  | Role                                                              |
| :---------------- | :----------------------- | :---------------------------------------------------------------- |
| SmashTable        | `ram` engine, NVMe       | Ordered and hash collections, transactions, snapshots, `expected` |
| StringZilla       | Every engine             | Strings                                                           |
| ForkUnion         | `ram_device.hpp`         | Threads, topology and domain-placed memory                        |
| yyjson            | Every engine             | JSON documents and configuration                                  |
| RocksDB           | RocksDB engine           | Persistent LSM trees with two-phase transactions                  |
| LevelDB           | LevelDB engine           | Persistent LSM tree with snapshots                                |
| fmt               | RocksDB, LevelDB, NVMe   | Formatting responses, logs and file names                         |
| CUDA toolkit      | NVMe, optional           | Compiling the GPU compaction kernels into the NVIDIA bundle       |
| GPUDirect Storage | NVMe, optional           | Reads sorted runs from NVMe drives into GPU memory                |
| liburing          | RocksDB engine, on Linux | Batched SST reads through io_uring, falling back to `pread`       |
| SPDK              | NVMe, optional           | User-space NVMe access                                            |
