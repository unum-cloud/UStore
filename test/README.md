# Tests

These are UStore's suites, by what each checks.
How to build and run them, the settings they read and how to write a case are under [Test Suites](../CONTRIBUTING.md#test-suites) in `CONTRIBUTING.md`.

## Suites

- __Conformance__ — `abi/*.cpp`, `compilation.cpp` and `strings_collection.cpp` check the C ABI and its C++ wrappers, identically on every engine, and `c/test.c` calls the C ABI from plain C11 once per shape of its arguments.
- __Tools__ — `tools.cpp` checks dataset imports and exports, and the CLI against a real directory.
- __Components__ — `nvme/*.cpp` check the NVMe templates of `include/ustore/` below the C ABI, each on every transport the machine offers.
- __Stress__ — `stress_atomicity.cpp` and `stress_linearizability.cpp` check atomicity and linearizability under concurrent transactions.
- __Python__ — the Python suites check the `ustore.lib` extension and its graph and table views, parametrized over batch shapes, views and isolation levels, and gated on what the linked engine supports; NetworkX and pandas serve as oracles for the views, and `readme.py` runs the README Quickstarts verbatim.

### Conformance

The C ABI scenarios — collections, transactions, snapshots, scans, documents and graphs — are written once and compiled into one executable per suite, named `ustore_test_<suite>`, which CTest runs once per engine as `ustore_test_<suite>_<engine>`, such as `ustore_test_abi_blobs_rocksdb`.
Every engine answers the same questions, and the translation units build in parallel.
`compilation.cpp` calls every wrapper in each accepted form, so a broken call site fails to build.
The suites of `abi/` sit at one depth, one per modality or contract, and build as `ustore_test_abi_<suite>`, each of which CTest also runs on `ram` under snapshot isolation as `ustore_test_abi_<suite>_ram_snapshot`:

- `abi/database.cpp` — writes, reads, transactions and scans accept valid arguments and refuse malformed ones, and saves, compactions and their progress keep their contracts.
- `abi/executors.cpp` — where a call's outputs land in its arena, how deferred calls wait on each other and on frees, and the C++ receivers on an executor.
- `abi/cursors.cpp` — cursors walk whole collections, resume after writes and hold their snapshots, and predicates, patterns and filters narrow scans.
- `abi/blobs.cpp`, `abi/docs.cpp`, `abi/graph.cpp`, `abi/pages.cpp` — each modality through the C++ wrappers, and the pages calls' refusals.
- `abi/strings.cpp`, `abi/arrow.cpp` — the strings modality, which maps strings onto keys, and the Arrow columns of `arrow.h` read into views, exported from them and ingested.

`abi/fixture.hpp` holds what several of them share: the blocking write and read calls, a deferred scan held inside its progress callback, cursor pulls, the wrappers' round trips and the sample documents and edges.
Each suite asks the open database through `ustore_database_describe` what it supports, and a case needing a missing capability skips, naming it.
`c/test.c` builds twice, as `ustore_test_c` against `ustore::static` and `ustore_test_c_shared` against `ustore::shared`, so a function the headers declare but the shared library does not export fails to link.
Both build with `-Werror` wherever the compiler is not MSVC, so a header that only a C++ compiler accepts fails to build, and a shape the library misreads fails the run.

### Components

The engine suites share `nvme/fixture.hpp`: one shape per suite, one mount per transport, and a driver that runs every case on memory, on files through the blocking floor and through io_uring, and on a block device.
A transport the machine does not offer skips with its reason, and a transport the flags claim that then fails to mount fails the suite.
Every result line reads `<suite>.<transport>.<case>`.
CMake builds every suite below as `ustore_test_nvme_<suite>`, the Linux transports only on Linux.
The database transaction and stress suites link the device compactor when the build carries CUDA.
Every suite instantiates the templates with ForkUnion's standard atomic reference and a yielding waiter, as no suite picks a CPU class.
No suite reaching the harness compiles under nvcc, since nvcc's front end crashes instantiating StringZilla's string.
The compaction suite builds twice, as `ustore_test_nvme_compaction_host` with every test build and as `ustore_test_nvme_compaction` over the device compactor, which `nvme/compaction_device.cpp` instantiates for the suite's key slots.

- `nvme/ring.cpp`, `nvme/linux_uring_disk.cpp`, `nvme/linux_aio_disk.cpp`, `nvme/linux_spdk_disk.cpp` — the ring contract of `nvme/ring_cases.hpp` on the RAM and blocking transports, on io_uring with what only a kernel queue can get wrong, on Linux AIO, and on SPDK.
- `nvme/channels.cpp` — the registered arena, the blocks channels queue pair out of it, and the pipelines that keep a ring at depth.
- `nvme/volume.cpp` — the catalog of two slots, the crashes it survives, and the router above it.
- `nvme/catalog.cpp` — the catalog records and the schema blobs a database persists.
- `nvme/sorted_run.cpp` — one sorted run: round trips of every layout, truncation, lookups and read depth.
- `nvme/tree.cpp` — the LSM level and the tree: what a flush publishes, what a merge keeps, and what a cancelled compaction leaves.
- `nvme/collection.cpp` — one collection over a tree: the memory runs, the value cache, snapshots and scans.
- `nvme/single_collection_database.cpp`, `nvme/single_collection_database_transaction.cpp`, `nvme/single_collection_database_stress.cpp`, `nvme/single_collection_database_transaction_stress.cpp` — a database of one collection, its transactions, and both under concurrent load.
- `nvme/database.cpp`, `nvme/database_transaction.cpp`, `nvme/transaction_registry.cpp` — named collections over a router, the transactions across them, and the deltas a transaction files its writes in.
- `nvme/graph.cpp` — a graph over two databases keyed by 16-byte IDs: what a walk sees and what a commit shows.
- `nvme/compaction.cu` — host and GPU compaction against a reference merge and against each other.

### Stress

Stress suites are separate executables with a time bound, which CTest runs once per engine under the default bound, while longer runs pass their own `--time-limit`.
Atomicity batch-inserts intersecting key ranges from several threads in rounds, and checks after each round that every batch landed whole or not at all; on an engine without transactions its cases skip.
Linearizability replays concurrent write transactions in commit order and compares the result with the state the database holds; an engine without transactions reports it `skipped`.
`USTORE_TIME_LIMIT` bounds the whole run and is shared evenly by every configuration, each of which runs at least one round and at most `USTORE_CHECKPOINTS` rounds.

Build them with the `release` preset and run them under a memory cap:

```sh
cmake --preset release -DUSTORE_BUILD_TEST=1 -DUSTORE_WITH_ROCKSDB=1
cmake --build --preset release
systemd-run --user --scope -p MemoryMax=16G -p MemorySwapMax=0 -- \
    build_release/build/bin/ustore_stress_atomicity --engine rocksdb --directory /dev/shm/ustore-stress --time-limit 60s
```
