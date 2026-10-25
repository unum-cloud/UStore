# Benchmarks

UStore's speed claims rest on one hyper-converged machine, so this suite measures on one: its NVMe drives, its CPU cores and its GPUs, on Hopper and Blackwell boxes alike.
Comparisons against other databases live in a separate harness.
This suite answers _which engine configuration_ and _which component_: `ram`, RocksDB, LevelDB and NVMe behind the same C ABI, and NVMe's single-collection database against its own configurations.
Every binary echoes its flags, then prints its progress and one line per row to standard output, so a run is its own transcript.

## The Suite

| Source                               | What it measures                                                    |
| :----------------------------------- | :------------------------------------------------------------------ |
| `twitter.cpp`                        | Building documents and a graph from tweets, then sampling them      |
| `tabular_graph.cpp`                  | NDJSON or CSV imported as documents and a graph, then exported back |
| `louvain.cpp`                        | Louvain communities over a graph collection and over memory         |
| `docs_paths.cpp`                     | Document fields located by yyjson against StringZilla subdocuments  |
| `nvme_single_collection_database.cu` | Inserts, lookups, scans and deletions, per value width              |

The Twitter benchmark samples keys, parsed objects, one field, a four-column table and two-hop traversals.
`mixed.hpp` holds the transactional batch upserts of documents and graph edges that the Twitter benchmark issues.
`harness.hpp` holds the timed loop, the latency histograms, the result rows and the flags every benchmark shares.
The three engine benchmarks compile once each, as `ustore_bench_twitter`, `ustore_bench_tabular_graph` and `ustore_bench_louvain`, and open the engine their `--engine` names.
`ustore_bench_docs_paths` opens no engine: it resolves the fields of generated tweets in memory, through the same conversions a docs gather and a docs read make.

### Building

Every number comes from the `release` preset, which builds into `build_release`.
The `ram` engine is always built, and each other engine is switched on by its option, while NVMe stays off until its sources build.

```sh
cmake --preset release -DUSTORE_BUILD_BENCH=1 -DUSTORE_WITH_ROCKSDB=1 -DUSTORE_WITH_LEVELDB=1
cmake --build --preset release --target ustore_bench_twitter ustore_bench_tabular_graph
```

Executables land in `build_release/build/bin/`, and each opens the engine its `--engine` names.
Imports hold whole batches in memory, so every run goes under a cgroup cap rather than trusting the benchmark to refuse.

```sh
systemd-run --user --scope -p MemoryMax=16G -p MemorySwapMax=0 -- build_release/build/bin/ustore_bench_twitter --engine rocksdb ...
```

### Flags

Every engine benchmark takes its inputs as flags, reads no environment variable, and prints its flags with `--help`.
It echoes every flag as `- Name: value` before its rows, and a value that does not parse prints one `--flag="value" does not parse, expected <grammar>` line and exits with status 1.
`--threads` names the threads every benchmark of a binary runs on, and `--max-input-files-total` the files the whole run reads across those threads, so neither flag is a per-thread count.

| Flag                                  | Default               | Meaning                                             |
| :------------------------------------ | :-------------------- | :-------------------------------------------------- |
| `--engine`                            | `ram`                 | Engine to open: `ram`, `rocksdb`, `leveldb`, `nvme` |
| `--isolation`                         | `strict-serializable` | Isolation of `ram` transactions, or `snapshot`      |
| `--engine-transport`                  | `automatic`           | Transport of `nvme`, like `linux-uring`             |
| `--directory`                         | Required              | Database directory; Louvain runs in memory without  |
| `--dataset`                           | Generated             | Tweet dumps, input files or a SNAP edge list        |
| `--threads`                           | Half the cores        | Threads per row; `1` for tables, one for Louvain    |
| `--warmup`                            | `1s`                  | Untimed calls ahead of each sampled row             |
| `--time-limit`                        | `10s`                 | Timed calls of each sampled row                     |
| `--seed`                              | `42`                  | Seed of generated data and keys, or `random`        |
| `--filter`                            | None                  | Rows to run, by ECMAScript regex or else substring  |
| `--max-tweets`                        | `1000000`             | Tweets to generate without a dataset                |
| `--max-input-files-total`             | `1000`                | Input files read in total; `10` for tables          |
| `--connectivity`                      | `4`                   | Average mentions per generated tweet                |
| `--batch-sizes`                       | `32,64,128`           | Three ascending batch sizes; building uses the last |
| `--format`                            | `ndjson`              | Format of the input files and the exports, or `csv` |
| `--id-field`                          | Required              | Field holding each document's key, for tables       |
| `--source-field`                      | Required              | Integer field of the source vertex, for tables      |
| `--target-field`                      | Required              | Integer field of the target vertex, for tables      |
| `--edge-field`                        | Required              | Field holding the edge key, for tables              |
| `--memory-limit`                      | `1GB`                 | Bytes an import or export buffers per batch         |
| `--vertices`                          | `4096`                | Vertices of the generated graph                     |
| `--communities`                       | `16`                  | Communities planted in the generated graph          |
| `--inner-degree`                      | `8`                   | Edges a generated vertex draws in its community     |
| `--outer-degree`                      | `1`                   | Edges a generated vertex draws across communities   |
| `--max-edges`                         | Every edge            | Edges kept from the dataset                         |
| `--batch-size-upsert`                 | `1024`                | Edges per upsert batch                              |
| `--min-modularity-growth-per-billion` | `100`                 | Billionths of modularity growth a level must add    |
| `--max-levels`                        | `32`                  | Levels either run coarsens through                  |

### Output

A sampled row warms up every thread for `--warmup`, then times every call until `--time-limit`, with the deadlines shared by all threads.
A consuming row, like building a collection or importing files, visits each batch or file once, split into one contiguous share per thread.
Setup, like drawing random keys or laying out batches, runs before the clock starts.
Each row prints one line: the threads, items per second in decimal units, bytes per second in binary units, the p50, p99 and p99.9 latencies of one call, and the failed calls.

```text
docs_sample_objects/32                 2 threads  9.77 M items/s  20.79 GB/s  p50 5.6 us  p99 18.4 us  p99.9 29.7 us  0 failures
```

### Twitter

```sh
build_release/build/bin/ustore_bench_twitter --engine rocksdb \
    --dataset data/twitter --directory /dev/shm/ustore-twitter --threads 16 --max-input-files-total 100
```

### Tables and Graphs

```sh
build_release/build/bin/ustore_bench_tabular_graph --engine rocksdb \
    --dataset data/<name> --format csv --directory /dev/shm/ustore-tabular \
    --id-field id --source-field source --target-field target --edge-field edge \
    --threads 4 --max-input-files-total 40
```

Every row becomes one document and one edge between its source and target, labeled by its edge field.
The imports divide the listed files into one contiguous share per thread, so `--max-input-files-total` never falls below `--threads`, and each export thread writes its own `<stem>-<thread>.<format>` file.

### NVMe Components

`nvme_single_collection_database.cu` reads its workloads from `nvme_single_collection_database.json`, and writes its databases and its log under the required `--directory`, its only flag.
No CMake target builds it yet.
Each entry names one value width from 1 byte upward, switches on with `enabled`, and sizes the population, the cache, the memory runs and the LSM levels.

### Louvain

`louvain.cpp` reads a SNAP edge list into a graph collection and detects communities twice: over the collection, whose every neighborhood read reaches the store, and over the same graph as an in-memory adjacency.
One level optimizer and one aggregator serve both graphs, so the gap between the two timings is the cost of reading the graph through UStore.
It prints the `modularity`, `communities` and `levels` of each run after its rows, and times the upsert that fills the collection as a row of its own.

```sh
build_release/build/bin/ustore_bench_louvain --engine rocksdb \
    --dataset data/snap-email/email-Eu-core.txt --directory /dev/shm/ustore-louvain
build_release/build/bin/ustore_bench_louvain --engine ram --vertices 4096 --communities 16
```

`louvain.py` times the same algorithm in Python three ways — NetworkX's `louvain_communities`, a baseline over a `networkx.Graph`, and that baseline over a `ustore.graph_view.Graph` — so it is the reference the C++ numbers are read against.

```sh
python bench/louvain.py --dataset data/snap-email/email-Eu-core.txt
python bench/louvain.py --vertices 4096 --communities 16
```

## Datasets

Datasets live on the shared filesystem under `/mnt/data/sources/` and are reached through `data/<name>` symlinks, lowercase with dashes and the scale suffix last.

```sh
ln -sfn /mnt/data/sources/twitter data/twitter
```

The Twitter benchmark reads dumps of the [`GET statuses/sample`][twitter-samples] stream as `.ndjson` files, and Twitter's terms forbid redistributing them, so every machine brings its own sample.
The tabular benchmark takes any directory of `.ndjson` or `.csv` files with a fixed schema and integer source and target columns.

[twitter-samples]: https://developer.twitter.com/en/docs/twitter-api/v1/tweets/sample-realtime/overview
