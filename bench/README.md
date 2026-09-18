# Benchmarks

UStore's speed claims rest on one hyper-converged machine, so this suite measures on one: its NVMe drives, its CPU cores and its GPUs, on Hopper and Blackwell boxes alike.
Comparisons against other databases live in a separate harness.
This suite answers _which engine configuration_ and _which component_: SmashTable, RocksDB, LevelDB and NVMe behind the same C ABI, and NVMe's databases, LSM trees, graph database and disk router against their own configurations.
Every binary prints a machine block and one block per measured phase to standard output and nothing else, so a run is its own transcript.

## The Suite

| Source                               | What it measures                                                    |
| :----------------------------------- | :------------------------------------------------------------------ |
| `twitter.cpp`                        | Building documents and a graph from tweets, then sampling them      |
| `tabular_graph.cpp`                  | NDJSON or CSV imported as documents and a graph, then exported back |
| `louvain.cpp`                        | Louvain communities over a graph collection and over memory         |
| `nvme/single_collection_database.cu` | Inserts, lookups, scans and deletions, per value width              |
| `nvme/lsm_tree.cu`                   | Inserts, lookups, selections and scans, per value width             |
| `nvme/graph_store.cpp`               | Vertex and edge writes and reads, with and without a transaction    |
| `nvme/disk_router.cpp`               | Random 4 KiB `O_DIRECT` reads over cores, disks and queue depths    |

The Twitter benchmark samples keys, parsed objects, one field, a four-column table and two-hop traversals.
`mixed.hpp` holds the transactional batch upserts of documents and graph edges that the Twitter benchmark issues.
The three engine benchmarks compile once per engine, as `ustore_bench_twitter_<engine>`, `ustore_bench_tabular_graph_<engine>` and `ustore_bench_louvain_<engine>`.

### Building

Every number comes from the `release` preset, which builds into `build_release`.
The SmashTable engine is always built, and each other engine is switched on by its option.

```sh
cmake --preset release -DUSTORE_BUILD_BENCH_CPP=1 -DUSTORE_USE_ROCKSDB=1 -DUSTORE_USE_LEVELDB=1
cmake --build --preset release --target ustore_bench_twitter_rocksdb ustore_bench_tabular_graph_rocksdb
```

Executables land in `build_release/build/bin/`.
Imports hold whole batches in memory, so every run goes under a cgroup cap rather than trusting the benchmark to refuse.

```sh
systemd-run --user --scope -p MemoryMax=16G -p MemorySwapMax=0 -- build_release/build/bin/ustore_bench_twitter_rocksdb ...
```

### Flags

Every engine benchmark takes its inputs as flags, reads no environment variable, and prints its flags with `--help`.
Flags starting with `--benchmark_` go to Google Benchmark, which reads them before the benchmark's own flags are parsed.
`--threads` names the threads every benchmark of a binary runs on, and `--max-input-files-total` the files the whole run reads across those threads, so neither flag is a per-thread count.

### Twitter

```sh
build_release/build/bin/ustore_bench_twitter_rocksdb \
    --dataset data/twitter --directory /dev/shm/ustore-twitter --threads 16 --max-input-files-total 100
```

| Flag                              | Meaning                                             | Default                |
| :-------------------------------- | :-------------------------------------------------- | :--------------------- |
| `--directory <path>`              | Directory the database lives in; required           | None                   |
| `--dataset <path>`                | Directory of `.ndjson` tweet dumps                  | Generated tweets       |
| `--threads <count>`               | Threads every benchmark runs on, indexing included  | Half the logical cores |
| `--max-tweets <count>`            | Tweets to generate without a dataset                | `1000000`              |
| `--max-input-files-total <count>` | `.ndjson` files the run maps in total               | `1000`                 |
| `--connectivity <count>`          | Average mentions per generated tweet                | `4`                    |
| `--duration <duration>`           | Minimal time each sampling benchmark runs           | `10s`                  |
| `--batch-sizes <counts>`          | Three ascending batch sizes; building uses the last | `32,64,128`            |

### Tables and Graphs

```sh
build_release/build/bin/ustore_bench_tabular_graph_rocksdb \
    --dataset data/<name> --format csv --directory /dev/shm/ustore-tabular \
    --id-field id --source-field source --target-field target --edge-field edge \
    --threads 4 --max-input-files-total 40
```

| Flag                              | Meaning                                                | Default          |
| :-------------------------------- | :----------------------------------------------------- | :--------------- |
| `--dataset <path>`                | Directory of input files; required                     | None             |
| `--format ndjson\|csv`            | Format of the input files, which the exports also take | `ndjson`         |
| `--directory <path>`              | Holds the exports and the database; required           | None             |
| `--config <path>`                 | JSON file configuring the database                     | In `--directory` |
| `--id-field <field>`              | Field holding each document's key; required            | None             |
| `--source-field <field>`          | Integer field holding the source vertex; required      | None             |
| `--target-field <field>`          | Integer field holding the target vertex; required      | None             |
| `--edge-field <field>`            | Field holding the edge key; required                   | None             |
| `--threads <count>`               | Threads every benchmark runs on                        | `1`              |
| `--max-input-files-total <count>` | Input files listed from the dataset in total           | `10`             |
| `--memory-limit <size>`           | Bytes an import or export buffers per batch            | `1GiB`           |

Every row becomes one document and one edge between its source and target, labeled by its edge field.
The imports divide the listed files into one contiguous share per thread, so `--max-input-files-total` never falls below `--threads`, and each export thread writes its own `<stem>-<thread>.<format>` file.

### NVMe Components

`single_collection_database.cu` and `lsm_tree.cu` read their workloads from `single_collection_database.json` and `lsm_tree.json`.
Each entry names one value width from 1 byte upward, switches on with `enabled`, and sizes the population, the cache, the write buffer and the LSM levels.
The region, city and graph benchmarks write under the `--directory` they are given.
`disk_router.cpp` reads the existing files of every directory listed in `--data-directories`, and without that flag generates its files under `--directory`.

### Louvain

`louvain.cpp` reads a SNAP edge list into a graph collection and detects communities twice: over the collection, whose every neighborhood read reaches the store, and over the same graph as an in-memory adjacency.
One level optimizer and one aggregator serve both graphs, so the gap between the two timings is the cost of reading the graph through UStore.
It reports `modularity`, `communities` and `levels` as counters, and the upsert that fills the collection as a benchmark of its own.

```sh
build_release/build/bin/ustore_bench_louvain_rocksdb \
    --dataset data/snap-email/email-Eu-core.txt --directory /dev/shm/ustore-louvain
build_release/build/bin/ustore_bench_louvain_ram --vertices 4096 --communities 16
```

| Flag                                          | Meaning                                                | Default    |
| :-------------------------------------------- | :----------------------------------------------------- | :--------- |
| `--dataset <path>`                            | SNAP edge list; a partition is generated without it    | Generated  |
| `--directory <path>`                          | Database directory; the graph is in memory without it  | In memory  |
| `--vertices <count>`                          | Vertices of the generated graph                        | `4096`     |
| `--communities <count>`                       | Communities planted in the generated graph             | `16`       |
| `--inner-degree <count>`                      | Edges each generated vertex draws inside its community | `8`        |
| `--outer-degree <count>`                      | Edges each generated vertex draws across communities   | `1`        |
| `--seed <count>`                              | Seed of the generated graph                            | `42`       |
| `--max-edges <count>`                         | Edges kept from the dataset                            | Every edge |
| `--upsert-batch-size <count>`                 | Edges per upsert batch                                 | `1024`     |
| `--min-modularity-growth-per-billion <count>` | Billionths of modularity growth a level must add       | `100`      |
| `--max-levels <count>`                        | Levels either run coarsens through                     | `32`       |

`louvain.py` times the same algorithm in Python three ways — NetworkX's `louvain_communities`, a baseline over a `networkx.Graph`, and that baseline over a `ustore.graph_view.Graph` — so it is the reference the C++ numbers are read against.

```sh
python bench/louvain.py --dataset data/snap-email/email-Eu-core.txt
python bench/louvain.py --vertices 1000 --communities 10
```

## Datasets

Datasets live on the shared filesystem under `/mnt/data/sources/` and are reached through `data/<name>` symlinks, lowercase with dashes and the scale suffix last.

```sh
ln -sfn /mnt/data/sources/twitter data/twitter
```

The Twitter benchmark reads dumps of the [`GET statuses/sample`][twitter-samples] stream as `.ndjson` files, and Twitter's terms forbid redistributing them, so every machine brings its own sample.
The tabular benchmark takes any directory of `.ndjson` or `.csv` files with a fixed schema and integer source and target columns.

[twitter-samples]: https://developer.twitter.com/en/docs/twitter-api/v1/tweets/sample-realtime/overview
