# Engines

Every engine implements the C ABI of `include/ustore/*.h` over its own storage, and links with `c/docs.cpp` and `c/graph.cpp` into one `ustore_embedded_<engine>` library, so documents and graphs work on each of them.
The engines fill three roles: SmashTable is the reference engine in memory, NVMe is the hyper-converged engine on NVMe drives and GPUs, and RocksDB and LevelDB bring third-party key-value stores behind the same calls.
Engines aim for the highest isolation SmashTable delivers, each reports the level its transactions actually deliver, and the conformance suite checks every advertised level against behaviour.

| Engine     | Directory         | Role                  | Builds today | Isolation                                    |
| :--------- | :---------------- | :-------------------- | :----------- | :------------------------------------------- |
| SmashTable | `src/smashtable/` | reference, in memory  | always       | `strict_serializable` or `snapshot`, at open |
| RocksDB    | `src/rocksdb/`    | third-party key-value | optional     | start snapshot, validated when staging       |
| LevelDB    | `src/leveldb/`    | third-party key-value | optional     | snapshot reads, no transactions              |
| NVMe       | `src/nvme/`       | hyper-converged       | v1 target    | optimistic, SmashTable's levels in v1        |

## SmashTable

`src/smashtable/` is the reference in-memory engine, and the one every other engine is checked against.
All collections and the catalog of their names share one stamp-based SmashTable `partitioned_store` ordered by collection and key, because collections appear at runtime while a `transaction_group` spans stores fixed at compile time.
One library serves both isolation levels: the `"isolation"` key picks one at open, each exported call visits the chosen instantiation once, and the `info` request of `ustore_database_control` reports it.

- `strict_serializable_k` validates every read and window, so `ustore_option_transaction_dont_watch_k` has nothing to switch off.
- `snapshot_k` validates only the reads a transaction watches, trading write skew for cheaper commits.

Snapshots map onto the versions the store already keeps.
A database opened with a directory persists as one binary image, rewritten whole on a flush and on close.

## RocksDB

`src/rocksdb/` keeps each named collection as a column family of a RocksDB `TransactionDB`, with fixed-width keys under a custom comparator.
Every write reaches RocksDB's log, which stays in memory until a flushed write or commit, a full buffer or a clean close writes it out, so a crash keeps a consistent prefix of the commits across every collection.
A transaction reads the snapshot it began from through its own buffered writes, and records the keys it watches unless a call passes `ustore_option_transaction_dont_watch_k`.
Nothing is locked until it stages, when RocksDB's pessimistic transaction locks and validates every watched read and buffered write and prepares them into the log, so conflicts surface at stage or commit as on SmashTable.
Scans are not validated, and a prepared transaction a crash leaves behind is rolled back when the database opens.
This is one design for durability and two-phase commits rather than the settled one, and a log kept above the engine, as a data-lake coordinator would own, remains open.

## LevelDB

`src/leveldb/` wraps one LevelDB database with snapshots, but without named collections or transactions: `ustore_transaction_init` fails with an error.
Its isolation ends at reading from a snapshot.

## NVMe

`src/nvme/` is the hyper-converged engine, and the one UStore's hardware paths serve.
A `database` holds `collection`s, each an `lsm_tree` of `lsm_level`s whose `lsm_sorted_run` files open with a `sorted_run_index` of `key_slot`s ahead of their `stored_value`s, with optimistic transactions, snapshots and backups.
Its graph database keeps adjacency in NVMe's own databases, as a fast path beside `c/graph.cpp`.
CMake refuses `USTORE_BUILD_NVME` until the sources in this directory build in tree, so everything below is a v1 target:

- __Kernel bypass__ — a transport chosen at mount reaches each data directory through io_uring on `O_DIRECT` files, io_uring on NVMe block devices, NVMe passthrough, or SPDK's user-space driver, and falls back to blocking positioned I/O on macOS, Windows and wherever io_uring is refused.
- __SIMD point lookups__ — sorted run indexes, micro-sets and ring buffers become SmashTable components with compile-time SIMD kits, one artifact carrying all of them and a database choosing one when it opens.
- __GPU bulk operations__ — compaction merges sorted runs on the CPU or on a CUDA GPU, runs on both Hopper and Blackwell, and produces the same output as CPU compaction.
- __GPUDirect Storage__ — sorted runs move between NVMe drives and GPU memory without staging in host memory.
- __Isolation__ — in-flight transactions reach SmashTable's levels once they run on SmashTable, after an analysis shows no loss of consistency or efficiency.

See [`nvme/README.md`](nvme/README.md) for its layers.

## Helpers

`src/helpers/` holds what several engines share:

- `config_loader.hpp` — the JSON database configuration on yyjson, until a schema-backed loader of intent keys replaces it.
- `linked_memory.hpp` and `linked_array.hpp` — the arena behind each C ABI call, and the non-throwing arrays grown inside it.
- `algorithm.hpp` and `full_scan.hpp` — STL algorithms over strided iterators, and a callback-driven full scan of a blob collection.
- `blocking_files.hpp` — positional file reads and writes, and directory queries, that block the calling thread and report a `status_t`.

All but the configuration loader give way to `std::ranges`, SmashTable walks and ForkUnion allocators.
Beside the engine directories, `src/executor.hpp` is the one owner of threads, topology and domain-placed memory over ForkUnion's C ABI, and nothing else calls `fu_*` directly, while `src/cuda.hpp` is the CUDA device layer GPU compaction builds on.

## Dependencies

| Dependency        | Used by                   | Role                                                              |
| :---------------- | :------------------------ | :---------------------------------------------------------------- |
| SmashTable        | SmashTable engine, NVMe   | Ordered and hash collections, transactions, snapshots, `expected` |
| StringZilla       | Every engine              | Strings                                                           |
| ForkUnion         | `executor.hpp`            | Threads, topology and domain-placed memory                        |
| NumKong           | NVMe's components         | SIMD kernels                                                      |
| yyjson            | Every engine              | JSON documents and configuration                                  |
| RocksDB           | RocksDB engine            | Persistent LSM trees with two-phase transactions                  |
| LevelDB           | LevelDB engine            | Persistent LSM tree with snapshots                                |
| fmt               | NVMe                      | Formatting logs and file names                                    |
| Thrust            | NVMe                      | Sorting and merging key slots on the host                         |
| CUDA and CUB      | NVMe, optional            | GPU compaction                                                    |
| GPUDirect Storage | NVMe, optional, v1 target | Reads and writes between NVMe drives and GPU memory               |
| liburing          | RocksDB engine, on Linux  | Batched SST reads through io_uring, falling back to `pread`       |
| SPDK              | NVMe, optional            | User-space NVMe access                                            |

NVMe's sources also include the `container`, `primitive`, `algo`, `utils` and `io` headers of a library that is not in this tree, and those give way to SmashTable, NumKong, StringZilla and ForkUnion.
