# UStore Benchmarks

UStore's speed claims rest on one hyper-converged machine, so this suite measures on one: its NVMe drives, its CPU cores and its GPUs, on Hopper and Blackwell boxes alike.
Comparisons against other databases live in a separate harness.
This suite answers _which engine configuration_ and _which component_: `ram`, RocksDB, LevelDB and NVMe behind the same C ABI, and NVMe's single-collection database against its own configurations.
Every binary echoes its flags, then prints its progress and one line per row to standard output, so a run is its own transcript.

## The Suite

| Source                                | What it measures                                                    |
| :------------------------------------ | :------------------------------------------------------------------ |
| `twitter.cpp`                         | Building documents and a graph from tweets, then sampling them      |
| `tabular_graph.cpp`                   | NDJSON or CSV imported as documents and a graph, then exported back |
| `louvain.cpp`                         | Louvain communities over a graph collection and over memory         |
| `docs_paths.cpp`                      | Document fields located by yyjson against StringZilla subdocuments  |
| `nvme_single_collection_database.cpp` | Inserts, finds, selects, scans and removals, per value layout       |
| `compaction.cpp`                      | One full compaction of the same level-0 runs, per engine and device |

The Twitter benchmark samples keys, parsed objects, one field, a four-column table and two-hop traversals.
It also stores every tweet's body as a plain value, then reads those one key at a time and walks them in pages.
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
| `--directories`                       | None                  | Data directories `nvme` and `rocksdb` fill          |
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

`nvme_single_collection_database.cpp` opens no engine: it drives NVMe's single-collection database over volumes of its own, and builds as `ustore_bench_nvme_single_collection_database` when `USTORE_WITH_NVME` is on.
It reads its workloads from a manifest like `nvme_single_collection_database.json`, whose every entry names one value layout — `12_bytes`, `44_bytes`, `fixed_width` or `variable_width` — switches on with `enabled`, and sizes the threads, the population, the cache, the memory runs and the LSM levels.
Every enabled workload opens one database over a fresh volume under `--directory`, fills it in one pass, samples finds, single and batched selects and a scan, removes every key, and deletes the volume.

| Flag            | Default     | Meaning                                                     |
| :-------------- | :---------- | :---------------------------------------------------------- |
| `--directory`   | Required    | Directory every workload's volume is created in             |
| `--workloads`   | Required    | JSON manifest of the workloads                              |
| `--transport`   | `automatic` | How the volumes are reached, or `memory` to skip the drives |
| `--volume-size` | `4GB`       | Bytes every workload's volume spans                         |

```sh
build_release/build/bin/ustore_bench_nvme_single_collection_database \
    --workloads bench/nvme_single_collection_database.json --directory /mnt/nvme/ustore-scdb
```

### Compaction

`compaction.cpp` times one full compaction of the same overlapping level-0 runs in RocksDB and in NVMe, the latter merging on host threads or on a GPU, and builds as `ustore_bench_compaction` when `USTORE_WITH_NVME` is on.
It writes random 16-byte keys with 100-byte values while no merge runs: RocksDB through `disable_auto_compactions=true` in the options file its next open loads, and NVMe by stopping half a memory run short of the level-0 run count that starts a merge.
The timed `ustore_database_compact` then flushes what memory holds and merges every run into one sorted level, after every file of the database is synced and dropped from the page cache.
Each row reports the CPU time of all threads and the bytes the process read and wrote during the call, and 100,000 sampled keys are read back afterwards.

| Flag            | Default    | Meaning                                                      |
| :-------------- | :--------- | :----------------------------------------------------------- |
| `--engine`      | `nvme`     | `nvme` or `rocksdb`                                          |
| `--compactor`   | `cpu`      | Where NVMe merges: `cpu`, or `gpu` through a CUDA executor   |
| `--directory`   | Required   | An empty directory the database fills                        |
| `--runs`        | `16`       | Level-0 runs the merge takes, and NVMe's level enlarge factor |
| `--run-records` | `400000`   | Records of every level-0 run                                 |
| `--value-size`  | `100`      | Bytes of every value                                         |
| `--write-limit` | `8GiB`     | Bytes the process may write before it stops                  |

```sh
build/bin/ustore_bench_compaction --engine rocksdb --directory /mnt/nvme/compaction-rocksdb
build/bin/ustore_bench_compaction --engine nvme --compactor gpu --directory /mnt/nvme/compaction-gpu
```

On an Intel Core i9-13950HX with an RTX 4060 Laptop GPU and one PCIe 4 SSD under ext4, cuFile in compatibility mode, 6.2M records of 116 bytes, 719 MB:

| Engine  | Compactor | Level-0 runs | Seconds |  GB/s | CPU seconds | Read bytes | Written bytes |
| :------ | :-------- | -----------: | ------: | ----: | ----------: | ---------: | ------------: |
| RocksDB | CPU       |           14 |    5.36 | 0.134 |        2.26 |     0.76 G |        0.80 G |
| NVMe    | CPU       |           16 |    3.93 | 0.183 |        3.94 |     0.74 G |        0.74 G |
| NVMe    | GPU       |           16 |    2.12 | 0.339 |        4.28 |     0.74 G |        0.74 G |

Each source run streams its values through a window of its own, so NVMe reads every byte it copies once, as RocksDB does.
The GPU ranks the keys and the host copies the values, which makes the device row 1.9 times faster than the host row and 2.5 times faster than RocksDB.

On the same machine's Crucial T710 at PCIe 4.0 x4, every run pinned to four P-cores with one thread each, as medians of three or more runs:

| Engine  | Compactor | Transport                         | Seconds |  GB/s | CPU seconds | Drive read commands |
| :------ | :-------- | :-------------------------------- | ------: | ----: | ----------: | ------------------: |
| RocksDB | CPU       | ext4, 1 subcompaction             |   2.705 | 0.266 |        1.80 |               13.8K |
| RocksDB | CPU       | ext4, 4 subcompactions            |   0.903 | 0.796 |        2.06 |               13.8K |
| NVMe    | CPU       | io_uring on the raw device        |   1.110 | 0.648 |        1.14 |              106.5K |
| NVMe    | CPU       | SPDK under vfio-pci               |   1.316 | 0.547 |        1.32 |              113.0K |
| NVMe    | GPU       | cuFile compatibility, raw device  |   0.915 | 0.786 |        1.82 |              106.6K |

RocksDB splits its manual compaction across four threads, while NVMe merges on one, so four subcompactions tie the GPU row and beat both CPU rows.
SPDK runs slower than io_uring here, because the merge is bound by its one thread, and SPDK caps every command at the controller's 512 KiB where the kernel splits larger blocks itself.
Read commands count the whole run, load included, from the drive's own counters; NVMe issues about 8 times more than RocksDB for the same bytes.

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

## Unit Economics

Each scale gets two tables on one machine, a row per engine: the measured rates, then what they cost at each price.
Prices are inputs, the rates come from the binaries above, and every dollar cell follows from the two.

### Price Inputs

The machine is an AWS `i8g.metal-24xl` in `us-west-2`: Graviton4, 96 cores, 768 GiB and six 3.75 TB NVMe drives.

- __On-demand:__ $8.2368 per hour.
- __1-year savings:__ $5.3405 per hour, an EC2 Instance Savings Plan with no upfront payment; a Compute Savings Plan costs $6.4078.
- __Spot:__ $2.31 per hour in `us-west-2a`.

With a price of p dollars per hour and a rate of r items or B bytes per second, both at 96 threads:

- __Lookups:__ $/1M lookups = p · 10⁶ / (3600 · r).
- __Writes:__ $/1B writes = p · 10⁹ / (3600 · r), once per durability.
- __Scans:__ $/TB scanned = p · 10¹² / (3600 · B), where B is the printed MB/s times 2²⁰, as rows print binary units.

### Workload Sources

Every column comes from one row of `ustore_bench_twitter` at `--threads 96`, and the lookups again at `--threads 1` with `--filter '^(construct_blobs|blobs_read)'`.

- __Lookups/s:__ `blobs_read/1`, one key's raw body per call.
- __Writes/s buffered:__ `construct_docs/<batch>`, transactional upserts whose commits stay buffered.
- __Writes/s flushed:__ `construct_docs_flushed/<batch>`, the same upserts committed with `ustore_durability_flushed_k` into a collection of their own.
- __Scan MB/s:__ `blobs_scan/<batch>`, pages of `<batch>` keys with their values pulled through a cursor from a sampled key.
- __Engines:__ `--engine ram`, `nvme`, `rocksdb` or `leveldb`, the NVMe one built with `USTORE_WITH_NVME=1` and given one mounted directory per drive through `--directories`.

### Generated Tweets

These tables measure the 1,000,000 tweets `ustore_bench_twitter` generates without a dataset.

| Engine      | Machine        | Lookups/s at 1 | Lookups/s at 96 | Writes/s buffered | Writes/s flushed | Scan MB/s |
| :---------- | :------------- | -------------: | --------------: | ----------------: | ---------------: | --------: |
| UStore RAM  | i8g.metal-24xl |              … |               … |                 … |                … |         … |
| UStore NVMe | i8g.metal-24xl |              … |               … |                 … |                … |         … |
| RocksDB     | i8g.metal-24xl |              … |               … |                 … |                … |         … |
| LevelDB     | i8g.metal-24xl |              … |               … |                 … |                … |         … |

| Engine      | Pricing        |    $/h | $/1M lookups | $/1B buffered writes | $/1B flushed writes | $/TB scanned |
| :---------- | :------------- | -----: | -----------: | -------------------: | ------------------: | -----------: |
| UStore RAM  | On-demand      | 8.2368 |            … |                    … |                   … |            … |
| UStore RAM  | 1-year savings | 5.3405 |            … |                    … |                   … |            … |
| UStore RAM  | Spot           |   2.31 |            … |                    … |                   … |            … |
| UStore NVMe | On-demand      | 8.2368 |            … |                    … |                   … |            … |
| UStore NVMe | 1-year savings | 5.3405 |            … |                    … |                   … |            … |
| UStore NVMe | Spot           |   2.31 |            … |                    … |                   … |            … |
| RocksDB     | On-demand      | 8.2368 |            … |                    … |                   … |            … |
| RocksDB     | 1-year savings | 5.3405 |            … |                    … |                   … |            … |
| RocksDB     | Spot           |   2.31 |            … |                    … |                   … |            … |
| LevelDB     | On-demand      | 8.2368 |            … |                    … |                   … |            … |
| LevelDB     | 1-year savings | 5.3405 |            … |                    … |                   … |            … |
| LevelDB     | Spot           |   2.31 |            … |                    … |                   … |            … |

## Datasets

Datasets live on the shared filesystem under `/mnt/data/sources/` and are reached through `data/<name>` symlinks, lowercase with dashes and the scale suffix last.

```sh
ln -sfn /mnt/data/sources/twitter data/twitter
```

The Twitter benchmark reads dumps of the [`GET statuses/sample`][twitter-samples] stream as `.ndjson` files, and Twitter's terms forbid redistributing them, so every machine brings its own sample.
The tabular benchmark takes any directory of `.ndjson` or `.csv` files with a fixed schema and integer source and target columns.

[twitter-samples]: https://developer.twitter.com/en/docs/twitter-api/v1/tweets/sample-realtime/overview
