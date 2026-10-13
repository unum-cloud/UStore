# Tests

UStore's suites carry no framework: every check in `test/harness.hpp` returns its failure count, counts roll up through each case to `main`, and the process exit code is the result.
A suite prints one `passed`, `FAILED` or `skipped` line per case and a `total` line last.

```cpp
case_result_t database_scan(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    failures += expect_status(database.drop("missing"), status_code_t::not_found_k);
    if (!database.supports_snapshots()) return case_result_t::skipped("the engine keeps no snapshots", failures);
    return failures;
}

int main(int argc, char** argv) {
    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options); status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();
    auto const test_directory = directory_t::make(options.directory);
    if (!test_directory) return 1;
    config_t const config {options.engine, test_directory->path()};
    report(totals, "database.scan", database_scan, config);
    return suite_exit_code(totals);
}
```

`expect` checks a condition, `expect_eq` and `expect_near` print both values when they differ, and `expect_status` prints the expected and the actual status with its message.
`report` prints the result and rolls it into the totals, and `suite_exit_code` prints `total N passed, M skipped, K failed, seed S` and answers the exit code: 1 on a failure, 3 on a skip under `--no-skips`.
A skip names the reason a case did not run, so a suite passing on nothing is visible in its summary.

## Suites

- __Conformance__ — `units.cpp`, `api.cpp`, `compilation.cpp`, `strings.cpp` and `strings_collection.cpp` check the C ABI and its C++ wrappers, identically on every engine, and `c/test.c` calls the C ABI from plain C11 once per shape of its arguments.
- __Tools__ — `tools.cpp` checks dataset imports and exports, and the CLI against a real directory.
- __Components__ — `nvme_*.cpp` and `nvme_*.cu` check the NVMe engine's parts below the C ABI, each on every transport the machine offers.
- __Stress__ — `stress_atomicity.cpp` and `stress_linearizability.cpp` check atomicity and linearizability under concurrent transactions.
- __Python__ — the Python suites check the `ustore.lib` extension and its graph and table views, parametrized over batch shapes, views and isolation levels, and gated on what the linked engine supports; NetworkX and pandas serve as oracles for the views.

### Conformance

The C ABI scenarios — collections, transactions, snapshots, scans, documents and graphs — are written once and compiled into one executable per suite, named `ustore_test_<suite>`, which CTest runs once per engine as `ustore_test_<suite>_<engine>`, such as `ustore_test_units_rocksdb`.
Every engine answers the same questions, and the translation units build in parallel.
`units.cpp` drives blobs, documents and graphs through the C++ wrappers, `api.cpp` checks that the C ABI accepts valid argument combinations and rejects malformed ones, and `compilation.cpp` calls every wrapper in each accepted form, so a broken call site fails to build.
Each suite asks the open database through `ustore_database_describe` whether it supports named collections, transactions and snapshots, and a case needing a missing capability returns without checks.
`c/test.c` builds twice, as `ustore_test_c` against `ustore::static` and `ustore_test_c_shared` against `ustore::shared`, so a function the headers declare but the shared library does not export fails to link.
Both build with `-Werror` wherever the compiler is not MSVC, so a header that only a C++ compiler accepts fails to build, and a shape the library misreads fails the run.

### Components

The engine suites share `nvme_fixture.hpp`: one shape per suite, one mount per transport, and a driver that runs every case on memory, on files through the blocking floor and through io_uring, and on a block device.
A transport the machine does not offer skips with its reason, and a transport the flags claim that then fails to mount fails the suite.
Every result line reads `<suite>.<transport>.<case>`.
CMake builds the `.cpp` suites below as `ustore_test_nvme_<suite>`, the Linux transports only on Linux, and the host half of `nvme_compaction.cu` with every test build, while `nvme_write_buffer.cpp` and the other `.cu` suites have no CMake target yet.

- `nvme_ring.cpp`, `nvme_linux_uring.cpp`, `nvme_linux_aio.cpp`, `nvme_linux_spdk.cpp` — the ring contract of `nvme_ring_cases.hpp` on the RAM and blocking transports, on io_uring with what only a kernel queue can get wrong, on Linux AIO, and on SPDK.
- `nvme_channels.cpp` — the registered arena, the blocks channels queue pair out of it, and the pipelines that keep a ring at depth.
- `nvme_volume.cpp` — the catalog of two slots, the crashes it survives, and the router above it.
- `nvme_catalog.cpp` — the catalog records and the schema blobs a database persists.
- `nvme_sorted_run.cpp` — one sorted run: round trips of every layout, truncation, lookups and read depth.
- `nvme_lsm.cpp` — the LSM level and the tree: what a flush publishes, what a merge keeps, and what a cancelled compaction leaves.
- `nvme_collection.cpp` — one collection over a tree: the write buffer, the value cache, snapshots and scans.
- `nvme_single_collection_database.cpp`, `nvme_single_collection_database_transaction.cu`, `nvme_single_collection_database_stress.cu`, `nvme_single_collection_database_transaction_stress.cu` — a database of one collection, its transactions, and both under concurrent load.
- `nvme_database.cpp`, `nvme_database_transaction.cu`, `nvme_transactions.cpp` — named collections over a router, and the transactions across them.
- `nvme_graph.cpp` — a graph over two databases: what a walk sees and what a commit shows.
- `nvme_compaction.cu` — host and GPU compaction against a reference merge and against each other.

## Building

The `ram` engine is always built, and each other engine is switched on by its option.
`tools.cpp` builds only when the CLI does, since it drives the CLI of the same engine.

```sh
cmake --preset debug -DUSTORE_BUILD_TEST=1 -DUSTORE_BUILD_TOOLS=1 -DUSTORE_WITH_ROCKSDB=1 -DUSTORE_WITH_LEVELDB=1
cmake --build --preset debug
```

The presets are `check`, `debug`, `dev`, `profile`, `release`, `relwithdebinfo`, `trace` and `tsan`, each building into `build_<preset>`, and sanitizers come with the `debug` and `tsan` presets rather than with an option.
Executables land in `build_debug/build/bin/`.

## Running

### Flags

Every suite takes its inputs as flags and reads no environment variable, and `--help` prints the flags it accepts.
A suite echoes every flag as `- Name: value` before its cases, and a value that does not parse prints one `--flag="value" does not parse, expected <grammar>` line and exits with status 1.
The engine, isolation and transport flags matter only to the engine suites.

| Flag                        | Default               | Meaning                                                        |
| :-------------------------- | :-------------------- | :------------------------------------------------------------- |
| `--directory <path>`        | None                  | Parent of the unique directory the suite works in              |
| `--device <path>`           | None                  | Block device the `device` transport opens, skipped when absent |
| `--spdk-address <address>`  | None                  | PCIe address of the device SPDK binds, skipped when absent     |
| `--transports <names>`      | Every transport       | Transports to mount, like `memory,linux-uring`                 |
| `--engine <name>`           | `ram`                 | Engine to open: `ram`, `rocksdb`, `leveldb` or `nvme`          |
| `--isolation <name>`        | `strict-serializable` | Isolation of `ram` transactions, or `snapshot`                 |
| `--engine-transport <name>` | `automatic`           | Transport of `nvme`, like `linux-uring`                        |
| `--threads <count>`         | Every core            | Threads the concurrent cases fan out over                      |
| `--seed <number>`           | `42`                  | The seed every random draw descends from, or `random`          |
| `--no-skips`                | Off                   | Exit with code 3 when any case skipped                         |
| `--filter <regex>`          | Every case            | Cases to run, by label, like `^lsm\.ram\.flush`                |

`tools.cpp` also takes:

| Flag           | Default | Meaning                                                           |
| :------------- | :------ | :---------------------------------------------------------------- |
| `--cli <path>` | None    | The CLI executable the CLI round trip drives, skipped when absent |

`nvme_linux_spdk.cpp` also takes:

| Flag                   | Default   | Meaning                                                        |
| :--------------------- | :-------- | :------------------------------------------------------------- |
| `--spdk-root <path>`   | None      | SPDK tree with a finished build, whose target the suite starts |
| `--spdk-serial <text>` | The rig's | Serial the controller must identify as                         |

`nvme_write_buffer.cpp` also takes:

| Flag                         | Default | Meaning                                            |
| :--------------------------- | :------ | :------------------------------------------------- |
| `--stored-values <count>`    | `1000`  | Stored values every case writes into the cache     |
| `--empty-value-step <count>` | `50`    | Every this many stored values is empty             |
| `--value-max-size <size>`    | `10000` | Largest stored value the cache admits, like `10KB` |

`nvme_single_collection_database_transaction.cu` and `nvme_database_transaction.cu` also take:

| Flag                                | Default | Meaning                                                            |
| :---------------------------------- | :------ | :----------------------------------------------------------------- |
| `--population <count>`              | `10000` | Stored values every case inserts into each collection              |
| `--empty-stored-value-step <count>` | `500`   | Every this many stored values is empty in the variable-width cases |

`nvme_single_collection_database_transaction_stress.cu` also takes:

| Flag                   | Default | Meaning                          |
| :--------------------- | :------ | :------------------------------- |
| `--population <count>` | `10000` | Keys every case writes and reads |

`nvme_compaction.cu` also takes:

| Flag                       | Default  | Meaning                                                       |
| :------------------------- | :------- | :------------------------------------------------------------ |
| `--large-universe <count>` | `600000` | Random keys of the large cases, which cross the GPU threshold |

With `--directory`, a suite creates a unique `ustore-XXXXXX` directory under the path, runs every case inside it, and removes it with its contents at exit, so parallel runs never share a database.
Without it, the engine runs on its default configuration, and the cases that need persistence or files report `skipped`.
Without `--cli`, the tools suite reports its CLI case `skipped`.
Without `--device`, the engine suites skip the `device` transport, and `scripts/rigs/nvmet_loop.sh up` hands out a RAM-backed NVMe namespace to pass to it.
A failing case prints the command that reruns it alone, with its `--seed` and a `--filter` anchored on its label, and a filter that matches no case fails the suite rather than passing an empty one.
CTest passes `--directory` as `test_data` inside the build directory, the engine suites get `--engine`, and the tools suite additionally gets `--cli` with the CLI it drives on that engine.

```sh
build_debug/build/bin/ustore_test_units --engine rocksdb --directory /dev/shm/ustore
build_debug/build/bin/ustore_test_tools --engine rocksdb --directory /dev/shm/ustore --cli build_debug/build/bin/ustore_cli
```

### Stress

Stress suites are separate executables with a time bound, which CTest runs once per engine under the default bound, while longer runs pass their own `--time-limit`.
Atomicity batch-inserts intersecting key ranges from several threads in rounds, and checks after each round that every batch landed whole or not at all; on an engine without transactions its cases pass without checks.
Linearizability replays concurrent write transactions in commit order and compares the result with the state the database holds; an engine without transactions reports it `skipped`.
`--time-limit` bounds the whole run and is shared evenly by every configuration, each of which runs at least one round and at most `--checkpoints` rounds.

Both suites take these flags:

| Flag                      | Default | Meaning                            |
| :------------------------ | :------ | :--------------------------------- |
| `--time-limit <duration>` | `10s`   | Bound of the whole run             |
| `--checkpoints <count>`   | `1000`  | Most rounds one configuration runs |

Atomicity also takes:

| Flag                            | Default      | Meaning                                               |
| :------------------------------ | :----------- | :---------------------------------------------------- |
| `--threads <counts>`            | `4,8,13,16`  | Thread counts, each run against every other list      |
| `--batch-sizes <counts>`        | `10,100,150` | Consecutive keys one transaction changes              |
| `--keys <count>`                | `100000`     | Keys one round writes, split into batches             |
| `--deletes-periodicity <count>` | `3`          | One batch in this many is erased by the deleting case |

Linearizability also takes:

| Flag                       | Default              | Meaning                                                      |
| :------------------------- | :------------------- | :----------------------------------------------------------- |
| `--threads <counts>`       | `2,3,4,5,6,7,8,9,10` | Thread counts, each run against every other list             |
| `--transactions <counts>`  | Listed below         | Transactions every thread commits between two checkpoints    |
| `--writes-per-key <count>` | `4`                  | Mean writes one key receives per round, sizing the key range |

`--transactions` defaults to `1,2,3,4,5,6,7,8,9,10,20,50,100`.

Build them with the `release` preset and run them under a memory cap:

```sh
cmake --preset release -DUSTORE_BUILD_TEST=1 -DUSTORE_WITH_ROCKSDB=1
cmake --build --preset release
systemd-run --user --scope -p MemoryMax=16G -p MemorySwapMax=0 -- \
    build_release/build/bin/ustore_stress_atomicity --engine rocksdb --directory /dev/shm/ustore-stress --time-limit 60s
```
