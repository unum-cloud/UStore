# UStore Benchmarks

It is always best to implement an application-specific benchmark, as every use case is different.
The second best is to estimate a few common use cases.
Here is our list:

- [UCSB]() for binary layer.
- 


For more advanced modality-specific workloads, we have the following benchmarks provided in this repo:

- **Twitter**. It takes the `.ndjson` dump of their <code class="docutils literal notranslate"><a href="https://developer.twitter.com/en/docs/twitter-api/v1/tweets/sample-realtime/overview" class="pre">GET statuses/sample</a></code> API and imports it into the Documents collection. We then measure random-gathers' speed at document-level, field-level, and multi-field tabular exports. We also construct a graph from the same data in a separate collection. And evaluate Graph construction time and traversals from random starting points.
- **Tabular**. Similar to the previous benchmark, but generalizes it to arbitrary datasets with some additional context. It supports Parquet and CSV input files. 🔜
- **Vector**. Given a memory-mapped file with a big matrix, builds an Approximate Nearest Neighbors Search index from the rows of that matrix. Evaluates both construction and query time. 🔜

We are working hard to prepare a comprehensive overview of different parts of UStore compared to industry-standard tools.
On both our hardware and most common instances across public clouds.


## [UCSB][ucsb]

For low-level binary workloads, we have a separate project called UCSB.
It covers LMDB, LevelDB, RocksDB, WiredTiger, Redis, MongoDB, and our Key-Value Stores.
It doesn't depend on UStore and uses native interfaces of all the engines to put everyone in the same shoes.

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
    -DUSTORE_BUILD_BENCHMARKS=1 .. \
    && make bench_twitter_ustore_embedded_ucset \
    && ./build/bin/bench_twitter_ustore_embedded_ucset
```

We manually repeated this same benchmark for a few other DBMS brands.
We tried to keep the RAM usage identical and limited the number of clients to 16 processes on the same machine on which the server was running.
The following results should be taken with the grain of salt and aren't as accurate as [UCSB](#ucsb) but they might be relevant for general perception.

For document-like workloads:

|                                   | Insertion | Retrieval |
| :-------------------------------- | :-------: | :-------: |
| Yugabyte                          |    2 K    |   82 K    |
| MongoDB                           |   18 K    |   210 K   |
|                                   |           |           |
| UStore on RocksDB                 |   22 K    |   742 K   |
| UStore on RocksDB with Flight API |   23 K    |   282 K   |
| UStore on UCSet                   |   350 K   |   4.8 M   |
| UStore on UCSet with Flight API   |   206 K   |   371 K   |

For graphs, we show the number of edges per second handled by different queries:

|                                   | Insertion | Two-hop Retrieval |
| :-------------------------------- | :-------: | :---------------: |
| Neo4J                             |    2 K    |       1.5 K       |
|                                   |           |                   |
| UStore on RocksDB                 |   241 K   |       3.7 M       |
| UStore on RocksDB with Flight API |   119 K   |      1.48 M       |
| UStore on UCSet                   |   201 K   |       21 M        |
| UStore on UCSet with Flight API   |   163 K   |       17 M        |

For MongoDB, we also tried the official <code class="docutils literal notranslate"><a href="https://www.mongodb.com/docs/database-tools/mongoimport/" class="pre">mongoimport</a></code> tool, which supports both `.csv` and `.ndjson`.
The results are mixed compared to a multi-process setup.
For Neo4J, significantly better results are possible if you are doing initialization with a pre-processed `.csv` file, but the same applies to UStore.

## Tables and Graphs

Generalizing the Twitter benchmark, we wrote a Python and a C++ benchmark for tabular datasets.
Unlike the previous benchmark, the schema must be fixed, but you can input `.json`, `.csv`, and `.parquet` files.

```sh
cmake -DCMAKE_BUILD_TYPE=Release -DUSTORE_BUILD_BENCHMARKS=1 .. && make benchmark_tabular_graph_ustore_embedded_ucset && ./build/bin/benchmark_tabular_graph_ustore_embedded_ucset
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