# Reference Implementation Details

UKV is an open standard.
Anyone can contribute to the interface, or suggest an implementation.
Unum maintains following implementations:

1. Fully Open Source Reference Implementation
2. Proprietary Hardware-Accelerated Implementation

Even the reference design is expected to be faster than the DBMS you are using today.
Below are somewhat more detailed descriptions of the first.

## Dependencies

All implementations of all modalities try to avoid dynamic memory allocations.
Every call uses its own arena, until he needs to grow beyond it.
Largely for that reason we chose to use the following libraries for the implementation of logic across different modalities:

* For Documents:
  * `yyjson`: to update the state of documents.
  * `simdjson`: to sample fields from immutable documents.
  * `mongo-c-driver`: to support BSON inputs.
  * `zlib` for document compression.
* For Paths:
  * `pcre2`: to JIT Regular Expressions.
* For Graphs:
  * `turbopfor`: for graph compression.

More broadly:

* `jemalloc`: for faster NUMA-aware allocations.
* `arrow`: for shared memory columnar representations.
* `arrow_flight`: for gRPC implementation.
* `fmt`: for string formatting or gRPC requests.

## Embedded Implementations

### UMem

In-memory backend with disk serialization.

* Depends on [unum-cloud/ucset](github.com/unum-cloud/ucset).
* Supports snapshots, transactions and named collections.

Is by far the fastest of all backends, but with the lowest Durability.

### LevelDB

The simplest modern persistent backend with limited functionality.

* Depends on [google/leveldb](github.com/google/leveldb).
* Supports snapshots, but not transactions or named collections.

### RocksDB

The industry-standard persistent backend.

* Depends on [facebook/rocksdb](github.com/facebook/rocksdb).
* Supports snapshots, transactions and named collections.

### UDisk

Our proprietary Key-Value Store, available on demand.

## Standalone Servers

Any of the above "Embedded Implementations" can be wrapped into a server.

### Arrow Flight RPC Server & Client

We currently use Apache Arrow Flight RPC as the primary client-server communication protocol due to its extensive support across the compute ecosystem.
This makes it easy for the external frameworks to send and gather info from UKV and underlying databases even without explicitly implementing UKV function calls.

### RESTful API

We implement a REST server using `Boost.Beast` and the underlying `Boost.Asio`, as the go-to Web-Dev libraries in C++.

## Contributing

