# Engines

Every engine implements the C ABI of `c/ustore/*.h` over its own storage, and joins the router's sources in the one source list both the `ustore_static` archive and the `ustore_shared` library build from, so documents and graphs work on each of them.
`src/lib.hpp` is the router every exported call goes through, and each public header has one source implementing it: `types.cpp`, `db.cpp`, `blobs.cpp`, `strings.cpp`, `docs.cpp`, `graph.cpp` and `pages.cpp`.
`docs.cpp` and `graph.cpp` also keep the documents and graphs any engine serves through its own reads and writes, over the codecs of `include/ustore/docs/` and `include/ustore/graph/`.
The reusable templates the engines instantiate live in `include/ustore/`, which [its own guide](../include/ustore/README.md) describes.
The engines fill three roles: `ram` is the reference engine in memory, NVMe is the hyper-converged engine on NVMe drives and GPUs, and RocksDB and LevelDB bring third-party key-value stores behind the same calls.
Engines aim for the highest isolation `ram` delivers, each reports the level its transactions actually deliver, and the conformance suite checks every advertised level against behaviour.

| Engine  | Directory              | Role                  | Builds today | Isolation                              |
| :------ | :--------------------- | :-------------------- | :----------- | :------------------------------------- |
| `ram`   | `src/ram/`             | reference, in memory  | always       | `strict_serializable` or `snapshot`    |
| RocksDB | `src_oracles/rocksdb/` | third-party key-value | optional     | start snapshot, validated when staging |
| LevelDB | `src_oracles/leveldb/` | third-party key-value | optional     | snapshot reads, no transactions        |
| NVMe    | `src/nvme/`            | hyper-converged       | v1 target    | optimistic, `ram`'s levels in v1       |

## RAM

`src/ram/` is the reference in-memory engine, and the one every other engine is checked against.
Every collection keeps a stamp-based SmashTable `partitioned_store` of its own, of the kind its modality picks: UUID keys mapped onto values for blobs, documents and anything else, strings mapped onto values for strings, and the half-edges of a graph, so no stored key carries its collection.
A catalog store maps the UUID of every collection, the main one under the nil UUID, onto its modality, its name and its store, and a names store maps every name onto its collection's UUID.
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

`src_oracles/rocksdb/` keeps each collection as a column family of a RocksDB `TransactionDB`, named `<modality>.<UUID>` so an open recovers both: UUID keys and strings alike under the bytewise comparator, the main collection in the default family, and the names of collections in a family of their own that no identifier reaches.
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
The catalog and the names are two more collections under reserved UUIDs:

```c
struct pairs   { uint8_t collection[16]; uint8_t key[16]; };    // blobs, documents and graphs, the main one under nil
struct strings { uint8_t collection[16]; char string[]; };      // strings collections
struct catalog { uint8_t collection[16]; uint8_t listed[16]; }; // all 0xFF, valued by the modality and the name
struct names   { uint8_t collection[16]; char name[]; };        // all 0xFF but a last 0xFE, valued by UUID and modality
```

Every collection a call names resolves through the catalog first, so no call reaches the catalog or the names.
A create writes the catalog entry and the name in one synced batch.
A drop removes the entry and the name first, synced, and then the collection's run, so a crash leaves an unlisted run rather than a listed empty collection.
Its isolation stops at reading from a snapshot, whose read stamp reads zero, as LevelDB keeps sequence numbers private.
A description reports zero keys, since LevelDB keeps no count, and a compaction compacts one collection's run at a time, in key order.
A save copies the snapshot into a new database a memtable-sized batch at a time, and destroys that database when cancelled.

## NVMe

`src/nvme/` is the hyper-converged engine, and the one UStore's hardware paths serve.
A `database` holds `collection`s, each an `lsm_tree` of `lsm_level`s whose `lsm_sorted_run` files open with a `sorted_run_index` of `key_slot`s ahead of their `stored_value`s, with optimistic transactions, snapshots and backups.
Its graph database keeps adjacency in NVMe's own databases, as a fast path beside `src/graph.cpp`.
Its strings and the names of its collections live in `src/nvme/strings.cpp`, as sorted buckets under integer keys in collections no identifier a caller holds reaches, until the engine keys strings itself.
It keeps 64-bit keys inside for now, so it takes only keys below 2⁶⁴, refusing the rest with `ustore_status_unsupported_k`, and names a collection by the UUID spelling its catalog number rather than a UUIDv7.
`USTORE_BUILD_NVME` is off by default, as the sources in this directory are not yet ported to the current C ABI and do not build, so everything below is a v1 target:

- __Kernel bypass__ — a transport chosen at mount reaches each data directory through io_uring on `O_DIRECT` files, io_uring on NVMe block devices, NVMe passthrough, or SPDK's user-space driver, and falls back to blocking positioned I/O on macOS, Windows and wherever io_uring is refused.
- __SIMD point lookups__ — sorted run indexes, micro-sets and ring buffers become SmashTable components with compile-time SIMD kits, one artifact carrying all of them and a database choosing one when it opens.
- __GPU bulk operations__ — compaction merges sorted runs on the CPU or on a CUDA GPU, runs on both Hopper and Blackwell, and produces the same output as CPU compaction.
- __GPUDirect Storage__ — sorted runs move between NVMe drives and GPU memory without staging in host memory.
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
Every `ustore_executor_t` that `src/types.cpp` builds holds one of them, a one-thread driver, and a deferred table of `deferred_limit` slots in a ring.
`ustore_executor_for_ram` spawns its `executor_t` over a `topology_t`, which owns a `fu_topology_t`: `fu_pool_spawn` over every memory domain, `fu_pool_spawn_near_memory_domain` near one of several, and the whole machine for several but not all, since ForkUnion has no spawn over a subset.
Zero threads take 7/8 of the cores of the compute domains local to the named memory domains, rounded down and at least one.
`ustore_executor_on_pool` builds a `lent_pool_t` through `lent_pool_t::borrow`, which refuses a null pool or one holding no threads.
At creation, a `for_threads` dispatch places each pool thread's scratch on its local memory domain when the options name it, and on the first named domain otherwise, while the blocking runner's scratch and every arena the executor makes live on the first named domain, which the executor keeps as its one piece of placement state.
The driver hands each run of ready slots with consecutive tickets to the pool as one blocking `for_n_dynamic`, and parks an owned pool once the table empties.
ForkUnion's dispatch claims the pool itself and refuses with `fu_contended_k`, running nothing, while the pool runs another dispatch, so a lent pool running its lender's dispatch, or any refused dispatch, leaves the batch to the driver, which runs it on its own thread.
A slot holds a copy of the caller's argument struct in a `std::variant` of every struct that names an executor, so nothing reads the caller's struct after submission, while everything the copy points to stays borrowed until final.
An engine's `ustore_database_init_ram`, `ustore_database_open_*` and `ustore_database_view_rocksdb` hand their open to `route_init`, which routes it as any other call, its copy carrying the open, since no handle names the engine yet.
Each task of a batch leaves its status in a driver-owned result, and once the dispatch returns the driver frees the batch's table positions before it finishes any outcome, so a caller that sees a call final may submit again at once.
One compare-and-swap on a 64-bit word takes a call's ticket in its low half and, for a deferred call, its table position in the high half, so both orders agree.
A blocking call takes a ticket and no slot, waits until every earlier ticket is final, and runs on the calling thread.
Each pool thread and the blocking caller record themselves while they run a call, so a drain or a blocking call from a callback of the same executor answers `ustore_status_contended_k` instead of waiting on itself.
Each of them also keeps a scratch for whatever a call needs only while it runs, such as the documents the docs layer parses or the edges a graph read stages, rewound before every call and kept at its high-water mark until the executor is freed, so the call's arena holds only its outputs.
The router gathers the views an accessor writes into that scratch, in blocks of up to 64 indices on the thread that runs the call, so an engine reads every ragged input as a tape or as gathered views.
A call without an executor works in a scratch of its own on the calling thread, whose first page lies on the stack and whose larger blocks are freed once the call is final, so a small call allocates nothing and nothing keeps state behind the caller's back.
An arena, made by `ustore_arena_init` with the residency of its executor, every transaction handle and every cursor carry a claim word, which the router takes before a call and releases once the engine returns, so a second call on any of them is refused as contended.
The router refuses a `NULL` arena, and a database or an arena of another residency than the call's executor, leaves every output of a call `NULL` or zero until the engine writes it, and again after a failure or a cancellation, and refuses to reset a transaction through a database it does not belong to.
Every handle carries an `access`, and the router refuses every call naming a transaction, collection creates and drops, clears and compactions with `ustore_status_read_only_k` on a database `ustore_database_view_rocksdb` opened.
Every handle, arenas included, also counts its unfinished deferred calls: a submission raises the count of its database, of the transaction, snapshot or cursor it names and of its arena, the driver lowers them once the outcome is final, and each free waits for zero, so no binding keeps counts of its own.
A transaction, a snapshot and a cursor each hold their database open, and a cursor also holds the transaction or snapshot it reads through, so every free drops one hold and the last one frees the handle through its engine.
In a build without OS threads there is no driver, and every call runs at submission.

Cursors live in `src/lib.hpp`, `src/blobs.cpp` and `src/strings.cpp` with no engine slot.
A pull calls the engine's scan slot into the call's scratch, then its read slot into the caller's arena when values are asked for, and copies the keys or the strings into the arena, so the arena holds only outputs.
A blobs cursor resumes at `key_after` of its last key, and a strings cursor at its last string followed by a zero byte, the smallest string after it.
A page shorter than `wanted`, or one ending at the largest key, sets the cursor's `exhausted` flag, after which every pull writes zero without reading.

## Dependencies

| Dependency        | Used by                   | Role                                                              |
| :---------------- | :------------------------ | :---------------------------------------------------------------- |
| SmashTable        | `ram` engine, NVMe        | Ordered and hash collections, transactions, snapshots, `expected` |
| StringZilla       | Every engine              | Strings                                                           |
| ForkUnion         | `ram_device.hpp`          | Threads, topology and domain-placed memory                        |
| yyjson            | Every engine              | JSON documents and configuration                                  |
| RocksDB           | RocksDB engine            | Persistent LSM trees with two-phase transactions                  |
| LevelDB           | LevelDB engine            | Persistent LSM tree with snapshots                                |
| fmt               | RocksDB, LevelDB, NVMe    | Formatting responses, logs and file names                         |
| Thrust            | NVMe                      | Sorting and merging key slots on the host                         |
| CUDA and CUB      | NVMe, optional            | GPU compaction                                                    |
| GPUDirect Storage | NVMe, optional, v1 target | Reads and writes between NVMe drives and GPU memory               |
| liburing          | RocksDB engine, on Linux  | Batched SST reads through io_uring, falling back to `pread`       |
| SPDK              | NVMe, optional            | User-space NVMe access                                            |
