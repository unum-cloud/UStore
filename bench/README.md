# Benchmarks

UStore's speed claims rest on one hyper-converged machine, so this suite measures on one: its NVMe drives, its CPU cores and its GPUs, on Hopper and Blackwell boxes alike.
Comparisons against other databases live in a separate harness.
This suite answers _which engine configuration_ and _which component_: SmashTable, RocksDB, LevelDB and UDisk behind the same C ABI, and UDisk's databases, LSM trees, graph database and disk router against their own configurations.
Every binary prints a machine block and one block per measured phase to standard output and nothing else, so a run is its own transcript.

## The Suite

| Source                                | What it measures                                                    |
| :------------------------------------ | :------------------------------------------------------------------ |
| `twitter.cpp`                         | Building documents and a graph from tweets, then sampling them      |
| `tabular_graph.cpp`                   | NDJSON or CSV imported as documents and a graph, then exported back |
| `udisk/single_collection_database.cu` | Inserts, lookups, scans and deletions, per value width              |
| `udisk/lsm_tree.cu`                   | Inserts, lookups, selections and scans, per value width             |
| `udisk/graph_store.cpp`               | Vertex and edge writes and reads, with and without a transaction    |
| `udisk/disk_router.cpp`               | Random 4 KiB `O_DIRECT` reads over cores, disks and queue depths    |

The Twitter benchmark samples keys, parsed objects, one field, a four-column table and two-hop traversals.
`mixed.hpp` holds the transactional batch upserts of documents and graph edges that the Twitter benchmark issues.
The two engine benchmarks compile once per engine, as `bench_twitter_ustore_embedded_<engine>` and `bench_tabular_graph_ustore_embedded_<engine>`.

### Building

Every number comes from the `release` preset, which builds into `build_release`.
The SmashTable engine is always built, and each other engine is switched on by its option.

```sh
cmake --preset release -DUSTORE_BUILD_BENCH_CPP=1 -DUSTORE_USE_ROCKSDB=1 -DUSTORE_USE_LEVELDB=1
cmake --build --preset release --target bench_twitter_ustore_embedded_rocksdb bench_tabular_graph_ustore_embedded_rocksdb
```

Executables land in `build_release/build/bin/`.
Imports hold whole batches in memory, so every run goes under a cgroup cap rather than trusting the benchmark to refuse.

```sh
systemd-run --user --scope -p MemoryMax=16G -p MemorySwapMax=0 -- build_release/build/bin/bench_twitter_ustore_embedded_rocksdb ...
```

### Flags

Both engine benchmarks take their inputs as flags, read no environment variable, and print their flags with `--help`.
Flags starting with `--benchmark_` go to Google Benchmark, which reads them before the benchmark's own flags are parsed.

### Twitter

```sh
build_release/build/bin/bench_twitter_ustore_embedded_rocksdb \
    --dataset data/twitter --directory /dev/shm/ustore-twitter --threads 16 --max-input-files 100
```

| Flag                        | Meaning                                                       | Default                |
| :-------------------------- | :------------------------------------------------------------ | :--------------------- |
| `--directory <path>`        | Directory the database lives in; required                     | None                   |
| `--dataset <path>`          | Directory of `.ndjson` tweet dumps                            | Generated tweets       |
| `--threads <count>`         | Threads per benchmark, which also index the dataset files     | Half the logical cores |
| `--max-tweets <count>`      | Tweets to generate without a dataset                          | `1000000`              |
| `--max-input-files <count>` | `.ndjson` files to map from the dataset                       | `1000`                 |
| `--connectivity <count>`    | Average mentions per generated tweet                          | `4`                    |
| `--duration <duration>`     | Minimal time each sampling benchmark runs                     | `10s`                  |
| `--batch-sizes <counts>`    | Three sampling batch sizes, ascending; building uses the last | `32,64,128`            |

### Tables and Graphs

```sh
build_release/build/bin/bench_tabular_graph_ustore_embedded_rocksdb \
    --dataset data/<name> --format csv --directory /dev/shm/ustore-tabular \
    --id-field id --source-field source --target-field target --edge-field edge \
    --threads 4 --max-input-files 10
```

| Flag                        | Meaning                                                          | Default          |
| :-------------------------- | :--------------------------------------------------------------- | :--------------- |
| `--dataset <path>`          | Directory of input files; required                               | None             |
| `--format ndjson\|csv`      | Format of the input files, which the exports also take           | `ndjson`         |
| `--directory <path>`        | Holds the exports, and the database without `--config`; required | None             |
| `--config <path>`           | JSON file configuring the database                               | In `--directory` |
| `--id-field <field>`        | Field holding each document's key; required                      | None             |
| `--source-field <field>`    | Integer field holding the source vertex; required                | None             |
| `--target-field <field>`    | Integer field holding the target vertex; required                | None             |
| `--edge-field <field>`      | Field holding the edge key; required                             | None             |
| `--threads <count>`         | Import threads                                                   | `1`              |
| `--max-input-files <count>` | Files each import thread takes                                   | `10`             |

Every row becomes one document and one edge between its source and target, labeled by its edge field.

### UDisk Components

`single_collection_database.cu` and `lsm_tree.cu` read their workloads from `single_collection_database.json` and `lsm_tree.json`.
Each entry names one value width from 1 byte upward, switches on with `enabled`, and sizes the population, the cache, the write buffer and the LSM levels.
The region, city and graph benchmarks write under the `--directory` they are given.
`disk_router.cpp` reads the existing files of every directory listed in `--data-directories`, and without that flag generates its files under `--directory`.

### Louvain

`louvain.py` times Louvain community detection three ways on SNAP graphs: NetworkX's `louvain_communities`, its own pure-Python `best_partition` over a UStore graph, and the C++ `community_louvain` from `louvain.cpp`.
It reads the unpacked edge lists from `tmp/`, and runs on the pre-v1 Python API, as does the `community_louvain` binding it calls.
`louvain.cpp` has no `main` and no CMake target: it is the algorithm alone, over the C++ graph wrappers.

| Dataset  | NetworkX | UStore Python | UStore C++ |
| :------- | :------- | :------------ | :--------- |
| Email    | 0.150 s  | 0.125 s       | 0.013 s    |
| Facebook | 0.670 s  | 0.582 s       | 0.065 s    |
| Amazon   | 36.72 s  | 46.27 s       | 11.40 s    |
| YouTube  | 161.2 s  | 107.6 s       | 77.51 s    |
| Orkut    | 10155 s  | 7453 s        | 3689 s     |

## Datasets

Datasets live on the shared filesystem under `/mnt/data/sources/` and are reached through `data/<name>` symlinks, lowercase with dashes and the scale suffix last.

```sh
ln -sfn /mnt/data/sources/twitter data/twitter
```

The Twitter benchmark reads dumps of the [`GET statuses/sample`][twitter-samples] stream as `.ndjson` files, and Twitter's terms forbid redistributing them, so every machine brings its own sample.
The tabular benchmark takes any directory of `.ndjson` or `.csv` files with a fixed schema and integer source and target columns.

[twitter-samples]: https://developer.twitter.com/en/docs/twitter-api/v1/tweets/sample-realtime/overview
