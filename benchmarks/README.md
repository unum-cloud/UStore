# UKV Benchmarks

## [UCSB][ucsb]

For low-level binary workloads, we have a separate project called UCSB.
It covers LMDB, LevelDB, RocksDB, WiredTiger, Redis, MongoDB, and our Key-Value Stores.

* [Results][ucsb-1] for 1 TB collections.
* [Results][ucsb-10] for 10 TB collections.

As of December 2022, typical results can look like:

| Operation           | RocksDB | UDisk |
| :------------------ | :-----: | :---: |
| Scan                |  17 M   | 17 M  |
| **Batch Read**      |  650 K  | 4.5 M |
| Read                |  420 K  |  1 M  |
| Read & Update, 1:1  |  64 K   | 173 K |
| Read & Upsert, 19:1 |  128 K  | 270 K |
| **Batch Upsert**    |  57 K   | 260 K |
| Remove              |  420 K  | 874 K |

## Twitter

Twitter benchmark operated on real-world sample of Tweets obtained via [Twitter Stream API][twitter-samples].
It is frequently used across the industry, but the current "User Agreement" bans publicly sharing such datasets.

We took over 1 TB of tweets packed consecutively into `.ndjson` files and simulated constructing collections of Documents, Graphs, and Paths.
You can obtain results for your hardware and your sample of Tweets using the following command.

```sh
cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DUKV_BUILD_BENCHMARKS=1 .. \
    && make benchmark_twitter_ukv_embedded_umem \
    && ./build/bin/benchmark_twitter_ukv_embedded_umem
```

We manually repeated this same benchmark for a few other DBMS brands.
We tried to keep the RAM usage identical and limited the number of clients to 16 processes on the same machine on which the server was running.
The following results should be taken with the grain of salt and aren't as accurate as [UCSB](#ucsb) but they might be relevant for general perception.

For document-like workloads:

|                                | Insertion | Retrieval |
| :----------------------------- | :-------: | :-------: |
| Yugabyte                       |    2 K    |   82 K    |
| MongoDB                        |   18 K    |   210 K   |
|                                |           |           |
| UKV on RocksDB                 |   22 K    |   742 K   |
| UKV on RocksDB with Flight API |   23 K    |   282 K   |
| UKV on UMem                    |   350 K   |   4.8 M   |
| UKV on UMem with Flight API    |   206 K   |   371 K   |

For graphs, we show the number of edges per second handled by different queries:

|                                | Insertion | Two-hop Retrieval |
| :----------------------------- | :-------: | :---------------: |
| Neo4J                          |    2 K    |       1.5 K       |
|                                |           |                   |
| UKV on RocksDB                 |   241 K   |       3.7 M       |
| UKV on RocksDB with Flight API |   119 K   |      1.48 M       |
| UKV on UMem                    |   201 K   |       21 M        |
| UKV on UMem with Flight API    |   163 K   |       17 M        |

For MongoDB, we also tried the official <code class="docutils literal notranslate"><a href="https://www.mongodb.com/docs/database-tools/mongoimport/" class="pre">mongoimport</a></code> tool, which supports both `.csv` and `.ndjson`.
The results are mixed compared to a multi-process setup.
For Neo4J, significantly better results are possible if you are doing initialization with a pre-processed `.csv` file, but the same applies to UKV.

## Tables and Graphs

Generalizing the Twitter benchmark, we wrote a Python and a C++ benchmark for tabular datasets.
Unlike the previous benchmark, the schema must be fixed, but you can input `.json`, `.csv`, and `.parquet` files.

```sh
cmake -DCMAKE_BUILD_TYPE=Release -DUKV_BUILD_BENCHMARKS=1 .. && make benchmark_tabular_graph_ukv_embedded_umem && ./build/bin/benchmark_tabular_graph_ukv_embedded_umem
```

For Python:

```sh
python benchmarks/tabular_graph.py
```

We manually repeated this same benchmark for several other DBMS brands and multiple publicly available datasets.
Below are the results.

> Coming soon!

[ucsb-10]: https://unum.cloud/post/2022-03-22-ucsb
[ucsb-1]: https://unum.cloud/post/2021-11-25-ycsb
[ucsb]: https://github.com/unum-cloud/ucsb
[twitter-samples]: https://developer.twitter.com/en/docs/twitter-api/v1/tweets/sample-realtime/overview