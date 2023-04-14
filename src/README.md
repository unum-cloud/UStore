# Implementation Details

## Architecture

Every supported Key-Value Store has it's own wrapper:

- `engine_ucset.cpp` for [unum-cloud/ucset](github.com/unum-cloud/ucset) in-memory AVL trees,
- `engine_leveldb.cpp` for [google/leveldb](github.com/google/leveldb) persistent B trees,
- `engine_rocksdb.cpp` for [facebook/rocksdb](github.com/facebook/rocksdb) persistent LSM trees.

Every server protocol has it's own implementation file:

- `flight_server.cpp` for Apache Arrow Flight RPC server,
- `rest_server.cpp` for now **deprecated** REST server,
- `json_rpc_server.c` for the minimalistic JSON-RPC server.

Some protocols, come with pre-built wrappers:

- `flight_client.cpp` for Apache Arrow Flight RPC client.

On top of the binary layer, every structured modality has it's own serialization:

- `modality_docs.cpp` for JSON, BSON and MessagePack documents,
- `modality_graph.cpp` for Directed Multi-Graphs,
- `modality_vectors.cpp` for Approximate Vector Search,
- `modality_paths.cpp` for String and Path-like keys.

## Dependencies

All implementations of all modalities try to avoid dynamic memory allocations.
Every call uses its own arena, until he needs to grow beyond it.
Largely for that reason we chose to use the following libraries for the implementation of logic across different modalities:

- For Documents:
  - `yyjson`: to update the state of documents.
  - `simdjson`: to sample fields from immutable documents.
  - `mongo-c-driver`: to support BSON inputs.
  - `zlib` for document compression. 🔜
- For Paths:
  - `pcre2`: to JIT Regular Expressions.
- For Graphs:
  - `turbopfor` for graph compression. 🔜

More broadly:

- `jemalloc` for faster NUMA-aware allocations.
- `arrow` for shared memory columnar representations.
- `arrow_flight` for gRPC implementation.
- `fmt` for string formatting of gRPC requests.
