# Tests

UStore's suites carry no framework: every check in `tests/test.hpp` returns its failure count, counts roll up through each case to `main`, and the process exit code is the verdict.
A suite prints one `passed`, `FAILED` or `skipped` line per case and a `total` line last.

```cpp
std::size_t database_scan(char const* directory) {
    std::size_t failures = 0;
    failures += expect(database.open(config.c_str()));
    failures += expect_eq(keys_found, 3u);
    return failures;
}

int main(int argc, char** argv) {
    std::size_t total = 0;
    total += report("database.scan", database_scan(directory));
    return report("total", total) ? 1 : 0;
}
```

`expect` checks a condition, `expect_eq` and `expect_near` print both values when they differ, and `report` prints the verdict and passes the count through.

## Suites

- __Conformance__ — `test_units.cpp`, `test_api.cpp` and `test_compilation.cpp` check the C ABI and its C++ wrappers, identically on every engine.
- __Tools__ — `test_tools.cpp` checks dataset imports and exports, and the CLI against a real directory.
- __Components__ — `tests/udisk/` checks UDisk's parts below the C ABI.
- __Stress__ — `stress_atomicity.cpp` and `stress_linearizability.cpp` check atomicity and linearizability under concurrent transactions.
- __Python__ — `*.py` check the pre-v1 Python API, and skip themselves until the Wave 4 bindings land.

### Conformance

The C ABI scenarios — collections, transactions, snapshots, scans, documents and graphs — are written once and compiled into one executable per suite and engine, named `<suite>_ustore_embedded_<engine>`, such as `test_units_ustore_embedded_rocksdb`.
Every engine answers the same questions, and the translation units build in parallel.
`test_units.cpp` drives blobs, documents and graphs through the C++ wrappers, `test_api.cpp` checks that the C ABI accepts valid argument combinations and rejects malformed ones, and `test_compilation.cpp` calls every wrapper in each accepted form, so a broken call site fails to build.
Each suite asks the open database through `ustore_get_metadata` whether it supports named collections, transactions and snapshots, and a case needing a missing capability returns without checks.

### Components

Only `compaction.cu` builds today, with `USTORE_BUILD_CUDA`; the other sources wait for the UDisk engine to build.

- `street.cpp`, `building.cpp` — streets of buildings: construction, lookups, scans, copies and moves.
- `write_cache.cpp` — adding, removing, merging, copying and moving citizens in the write cache.
- `city.cu`, `country.cu` — citizens in cities and countries, with exports, imports and compaction.
- `compaction.cu` — host and GPU compaction against a reference merge and against each other.
- `region.cu`, `planet.cu` — point and batch access, scans, snapshots, backups and transfers.
- `region_transaction.cu`, `planet_transaction.cu` — transactions over one region and across the countries of a planet.
- `region_stress.cu`, `region_transaction_stress.cu` — concurrent reads, writes and transactions on a region, replayed in commit order.
- `disk_ram.cpp`, `filesystem.cpp` — the RAM-backed disk and files written through the disk router.
- `graph_gdb*.cpp`, `graph_ukv_graph.cpp` — the persistent graph database and the graph C ABI above it.

## Building

The SmashTable engine is always built, and each other engine is switched on by its option.
`test_tools` builds only when the CLI does, since it drives the CLI of the same engine.

```sh
cmake --preset debug -DUSTORE_BUILD_TEST_CPP=1 -DUSTORE_BUILD_TOOLS_CPP=1 -DUSTORE_USE_ROCKSDB=1 -DUSTORE_USE_LEVELDB=1
cmake --build --preset debug
```

The presets are `check`, `debug`, `dev`, `profile`, `release`, `relwithdebinfo`, `trace` and `tsan`, each building into `build_<preset>`, and sanitizers come with the `debug` and `tsan` presets rather than with an option.
Executables land in `build_debug/build/bin/`.

## Running

### Flags

Every suite takes its inputs as flags and reads no environment variable, and `--help` prints the flags it accepts.

| Flag                 | Suites       | Meaning                                                                 |
| :------------------- | :----------- | :---------------------------------------------------------------------- |
| `--directory <path>` | Every suite  | Parent of the unique directory the suite works in, created when missing |
| `--cli <path>`       | `test_tools` | The CLI executable the CLI round trip drives                            |

With `--directory`, a suite creates a unique `ustore-XXXXXX` directory under the path, runs every case inside it, and removes it with its contents at exit, so parallel runs never share a database.
Without it, the engine runs on its default configuration, and the cases that need persistence or files report `skipped`.
Without `--cli`, `test_tools` reports its CLI case `skipped`.
CTest passes `--directory` as `test_data` inside the build directory, and `test_tools` additionally gets `--cli` with the CLI built for the same engine.

```sh
build_debug/build/bin/test_units_ustore_embedded_rocksdb --directory /dev/shm/ustore
build_debug/build/bin/test_tools_ustore_embedded_rocksdb --directory /dev/shm/ustore --cli build_debug/build/bin/ustore_cli_rocksdb
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
cmake --preset release -DUSTORE_BUILD_TEST_CPP=1 -DUSTORE_USE_ROCKSDB=1
cmake --build --preset release
systemd-run --user --scope -p MemoryMax=16G -p MemorySwapMax=0 -- \
    build_release/build/bin/stress_atomicity_ustore_embedded_rocksdb --directory /dev/shm/ustore-stress --duration 1min
```
