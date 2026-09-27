# Tests

UStore's suites carry no framework: every check in `test/test.hpp` returns its failure count, counts roll up through each case to `main`, and the process exit code is the result.
A suite prints one `passed`, `FAILED` or `skipped` line per case and a `total` line last.

```cpp
case_result_t database_scan(char const* directory) {
    std::size_t failures = 0;
    failures += expect(database.open(config.c_str()));
    failures += expect_status(database.remove(missing), status_code_t::not_found_k);
    if (!database.supports_snapshots()) return case_result_t::skipped("the engine keeps no snapshots", failures);
    return failures;
}

int main(int argc, char** argv) {
    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options); status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();
    report(totals, "database.scan", database_scan(options.directory));
    return suite_exit_code(totals);
}
```

`expect` checks a condition, `expect_eq` and `expect_near` print both values when they differ, and `expect_status` prints the expected and the actual status with its message.
`report` prints the result and rolls it into the totals, and `suite_exit_code` prints `total N passed, M skipped, K failed, seed S` and answers the exit code: 1 on a failure, 3 on a skip under `--no-skips`.
A skip names the reason a case did not run, so a suite passing on nothing is visible in its summary.

## Suites

- __Conformance__ — `units.cpp`, `api.cpp`, `compilation.cpp`, `strings.cpp` and `strings_collection.cpp` check the C ABI and its C++ wrappers, identically on every engine.
- __Tools__ — `tools.cpp` checks dataset imports and exports, and the CLI against a real directory.
- __Components__ — `nvme_*.cpp` and `nvme_*.cu` check the NVMe engine's parts below the C ABI, each on every transport the machine offers.
- __Stress__ — `stress_atomicity.cpp` and `stress_linearizability.cpp` check atomicity and linearizability under concurrent transactions.
- __Python__ — the Python suites check the `ustore.lib` extension and its graph and table views, parametrized over batch shapes, views and isolation levels, and gated on what the linked engine supports; NetworkX and pandas serve as oracles for the views.

### Conformance

The C ABI scenarios — collections, transactions, snapshots, scans, documents and graphs — are written once and compiled into one executable per suite and engine, named `ustore_test_<suite>_<engine>`, such as `ustore_test_units_rocksdb`.
Every engine answers the same questions, and the translation units build in parallel.
`units.cpp` drives blobs, documents and graphs through the C++ wrappers, `api.cpp` checks that the C ABI accepts valid argument combinations and rejects malformed ones, and `compilation.cpp` calls every wrapper in each accepted form, so a broken call site fails to build.
Each suite asks the open database through `ustore_get_metadata` whether it supports named collections, transactions and snapshots, and a case needing a missing capability returns without checks.

### Components

The engine suites share `nvme_fixture.hpp`: one shape per suite, one mount per transport, and a driver that runs every case on memory, on files through the blocking floor and through io_uring, and on a block device.
A transport the machine does not offer skips with its reason, and a transport the flags claim that then fails to mount fails the suite.
Every result line reads `<suite>.<transport>.<case>`.

- `nvme_ring.cpp`, `nvme_uring.cpp`, `nvme_spdk.cpp` — the ring contract of `nvme_ring_cases.hpp` on the RAM and blocking transports, on io_uring with what only a kernel queue can get wrong, and on SPDK.
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

The SmashTable engine is always built, and each other engine is switched on by its option.
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

| Flag                       | Suites        | Meaning                                                                 |
| :------------------------- | :------------ | :---------------------------------------------------------------------- |
| `--directory <path>`       | Every suite   | Parent of the unique directory the suite works in, created when missing |
| `--device <path>`          | Engine suites | Block device the `device` transport opens, skipped when absent          |
| `--spdk-address <address>` | Engine suites | PCIe address of the NVMe device SPDK binds, skipped when absent         |
| `--transports <names>`     | Engine suites | Transports to mount, like `ram,files,uring`, every one when absent      |
| `--threads <count>`        | Every suite   | Threads the concurrent cases fan out over, every core when absent       |
| `--cli <path>`             | `tools.cpp`   | The CLI executable the CLI round trip drives                            |
| `--seed <number>`          | Every suite   | The seed every random draw descends from, drawn and printed when absent |
| `--no-skips`               | Every suite   | Exit with code 3 when any case skipped, for a rig that must be present  |
| `--filter <regex>`         | Every suite   | Run only the cases whose label it matches, like `^lsm\.ram\.flush`      |

With `--directory`, a suite creates a unique `ustore-XXXXXX` directory under the path, runs every case inside it, and removes it with its contents at exit, so parallel runs never share a database.
Without it, the engine runs on its default configuration, and the cases that need persistence or files report `skipped`.
Without `--cli`, the tools suite reports its CLI case `skipped`.
Without `--device`, the engine suites skip the `device` transport, and `scripts/rigs/nvmet_loop.sh up` hands out a RAM-backed NVMe namespace to pass to it.
A failing case prints the command that reruns it alone, with its `--seed` and a `--filter` anchored on its label, and a filter that matches no case fails the suite rather than passing an empty one.
CTest passes `--directory` as `test_data` inside the build directory, and the tools suite additionally gets `--cli` with the CLI built for the same engine.

```sh
build_debug/build/bin/ustore_test_units_rocksdb --directory /dev/shm/ustore
build_debug/build/bin/ustore_test_tools_rocksdb --directory /dev/shm/ustore --cli build_debug/build/bin/ustore_cli_rocksdb
```

### Stress

Stress suites are separate executables with a time bound, run on demand and in nightly CI rather than on every push.
Atomicity batch-inserts intersecting key ranges from several threads in rounds, and checks after each round that every batch landed whole or not at all; on an engine without transactions its cases pass without checks.
Linearizability replays concurrent write transactions in commit order and compares the result with the state the database holds; an engine without transactions reports it `skipped`.
`--duration` bounds the whole run and is shared evenly by every configuration, each of which runs at least one round and at most `--checkpoints` rounds.

Both suites take these flags:

| Flag                    | Meaning                            | Default |
| :---------------------- | :--------------------------------- | :------ |
| `--duration <duration>` | Bound of the whole run             | `10s`   |
| `--checkpoints <count>` | Most rounds one configuration runs | `1000`  |

Atomicity also takes:

| Flag                            | Meaning                                               | Default      |
| :------------------------------ | :---------------------------------------------------- | :----------- |
| `--threads <counts>`            | Thread counts, each run against every other list      | `4,8,13,16`  |
| `--batch-sizes <counts>`        | Consecutive keys one transaction changes              | `10,100,150` |
| `--keys <count>`                | Keys one round writes, split into batches             | `100000`     |
| `--deletes-periodicity <count>` | One batch in this many is erased by the deleting case | `3`          |

Linearizability also takes:

| Flag                       | Meaning                                                      | Default              |
| :------------------------- | :----------------------------------------------------------- | :------------------- |
| `--threads <counts>`       | Thread counts, each run against every other list             | `2,3,4,5,6,7,8,9,10` |
| `--transactions <counts>`  | Transactions every thread commits between two checkpoints    | Listed below         |
| `--writes-per-key <count>` | Mean writes one key receives per round, sizing the key range | `4`                  |

`--transactions` defaults to `1,2,3,4,5,6,7,8,9,10,20,50,100`.

Build them with the `release` preset and run them under a memory cap:

```sh
cmake --preset release -DUSTORE_BUILD_TEST=1 -DUSTORE_WITH_ROCKSDB=1
cmake --build --preset release
systemd-run --user --scope -p MemoryMax=16G -p MemorySwapMax=0 -- \
    build_release/build/bin/ustore_stress_atomicity_rocksdb --directory /dev/shm/ustore-stress --duration 1min
```
