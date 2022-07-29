# Universal Key-Values

![Universal Key Values by Unum](UKV.png)

Imagine having a standardized cross-lingual interface for all your things "Data":

* Storing binary blobs
* Building up graphs & indexes
* Querying structured documents
* Handling JSON, [BSON](https://www.mongodb.com/json-and-bson), [MsgPack](https://msgpack.org/index.html)
* [JSON-Pointers](https://datatracker.ietf.org/doc/html/rfc6901) & [Field-level Patches](https://datatracker.ietf.org/doc/html/rfc6902), no custom Query Languages
* [ACID](https://en.wikipedia.org/wiki/ACID) transactions across tables, docs & graphs
* Familiar high-level [drivers](#frontends) for tabular & graph analytics
* [Apache Arrow](https://arrow.apache.org/) exports, [Flight RPC](https://arrow.apache.org/docs/format/Flight.html) and [DataFusion SQL](https://github.com/apache/arrow-datafusion) support
* Packing Tensors for [PyTorch](https://pytorch.org/) and [TensorFlow](tensorflow.org)

UKV does just that, abstracting away the implementation from the user.
In under 10K LOC you get a reference implementation in C++, support for any classical backend, and bindings for [Python](#python), [GoLang](#golang), [Java](#java).

Common use-cases of UKV would be:

* Python, GoLang, Java and other high-level bindnigs for RocksDB and LevelDB.
* Performant embedded store in the foundation of your in-house storage solution.
* Document store, that is simpler and faster than putting JSONs in MongoDB or Postgres.
* Graph database, with the feel of NetworkX, speed of GunRock and scale of Hadoop.
* Low-latency media storage for games, CDNs and ML/BI pipelines.

## Backends

Backends differ in their functionality and purposes.
The underlying embedded key value stores include:

| Name    |      OSes       | ACID  | Collections | Persistent | Safe Reads |
| :------ | :-------------: | :---: | :---------: | :--------: | :--------: |
| STL     | POSIX + Windows |   ✅   |      ✅      |     ❌      |     ✅      |
| LevelDB | POSIX + Windows |   ❌   |      ❌      |     ✅      |     ❌      |
| RocksDB | POSIX + Windows |   ✅   |      ✅      |     ✅      |     ❌      |
| UnumDB  |      Linux      |   ✅   |      ✅      |     ✅      |     ✅      |

The STL backend originally served educational purposes, yet, with a proper web-server implementation, is comparable to other in-memory stores like Redis, MemCached or ETCD.
LevelDB is Key-Value stored designed at Google and extensively adopted across the industry.
RocksDB originally forked LevelDB to extend its functionality with transactions, collections, and higher performance.

## Frontends

Currently, at Proof-of-Concept stage, we support only the essential functionality in select programming languages.

| Name            | Transact | Batches | Collections | Docs  | Graphs | Zero-Copy | Extras         |
| :-------------- | :------: | :-----: | :---------: | :---: | :----: | :-------: | :------------- |
| C++             |    ✅     |    ✅    |      ✅      |   ✅   |   ✅    |     ✅     |                |
| Python          |    ✅     |    ❌    |      ✅      |   ❌   |   ✅    |     ✅     | Tensor Packing |
| Java            |    ✅     |    ❌    |      ❌      |   ❌   |   ❌    |     ❌     |                |
| GoLang          |    ❌     |    ❌    |      ❌      |   ❌   |   ❌    |     ✅     |                |
| TODO: C#        |    ❌     |    ❌    |      ❌      |   ❌   |   ❌    |     ✅     |                |
| TODO: REST API  |    ✅     |    ✅    |      ✅      |   ✅   |   ❌    |     ✅     |                |
| TODO: Arrow RPC |    ✅     |    ✅    |      ✅      |   ✅   |   ❌    |     ✅     | Tensor Packing |
| TODO: Wolfram   |    ❌     |    ❌    |      ✅      |   ❌   |   ✅    |     ✅     | Tensor Packing |

## Assumptions and Limitations

* Keys are constant length native integer types. High-performance solutions are impossible with variable size keys. 64-bit unsigned integers are currently chosen as the smallest native numeric type, that can address modern datasets.
* Values are serialized into variable-length byte strings.
* Iterators and enumerators often come with certain relevance, consistency or performance tradeoffs or aren't supported at all. Check the specs of exact backend.
* Transactions are ACI(D) by-default, meaning that:
  * Atomicity is guaranteed,
  * Consistency is implemented in the strongest form - tracking all key and metadata lookups by default,
  * Isolation is guaranteed, but may be implemented differently, depending on backend - in-memory systems generally prefer "locking" over "multi-versioning".
  * Durability doesn't apply to in-memory systems, but even in persistent stores its often disabled to be implemented in higher layers of infrastructure.

## Development

To build and test any set of bindings:

1. Build (`cmake . && make`) or download the prebuilt `libukv.a`,
2. Call `./language/run.sh` in your terminal.

### Python

Current implementation relies on [PyBind11](https://github.com/pybind/pybind11).
It's feature-rich, but not very performant, supporting:

* Named Collections
* ACID Transactions
* Single & Batch Operations
* Tensors support via [Buffer Protocol](https://docs.python.org/3/c-api/buffer.html)
* [NetworkX](https://networkx.org)-like interface for Graphs
* [Pandas](https://pandas.pydata.org)-like interface for Document collections

Using it can be as easy as:

```python
import ukv.stl as ukv
# import ukv.level as ukv
# import ukv.rocks as ukv
# import ukv.lerner as ukv

db = ukv.DataBase()
db[42] = 'purpose of life'.encode()
db['sub-collection'][0] = db[42]
del db[42]
assert len(db['sub-collection'][0]) == 15
```

All familiar Pythonic stuff!

### Rust

Rust implementation is designed to support:

* Named Collections
* ACID Transactions
* Single & Batch Operations
* [Apache DataFusion](https://arrow.apache.org/datafusion/) `TableProvider` for SQL

Using it should be, again, familiar, as it mimics [`std::collections`](https://doc.rust-lang.org/std/collections/hash_map/struct.HashMap.html):

```rust
let mut db = DataBase::new();
if db.contains_key(&42) {
    db.remove(&42);
    db.insert(43, "New Meaning".to_string());
}
for (key, value) in &db {
    println!("{key}: \"{value}\"");
}
db.clear();
```

### Java

These bindings are implemented via [Java Native Interface](https://docs.oracle.com/javase/8/docs/technotes/guides/jni/spec/jniTOC.html).
This interface is more performant than Python, but is not feature complete yet.
It mimics native `HashMap` and `Dictionary` classes, but has no support for batch operations yet.

```java
DataBase db = new DataBase("");
db.put(42, "purpose of life".getBytes());
assert db.get(42) == "purpose of life".getBytes() : "Big surprise";
db.close();
```

All `get` requests cause memory allocations in Java Runtime and export data into native Java types.
Most `set` requests will simply cast and forward values without additional copies.
Aside from opening and closing this class is **thread-safe** for higher interop with other Java-based tools.

Implementation follows the ["best practices" defined by IBM](https://developer.ibm.com/articles/j-jni/). It was tested with:

* JVM
* *[GraalVM](https://www.graalvm.org/22.1/reference-manual/native-image/JNI/)*

### GoLang

GoLang bindings are implemented using [cGo](https://pkg.go.dev/cmd/cgo).
The language lacks operator and function overloads, so we can't mimic native collections.
Instead we mimic the interfaces of most commonly used ORMs.

```go
db := DataBase{}
db.Reconnect()
db.Set(42, &[]byte{4, 2})
db.Get(42)
```

Implementation-wise, GoLang variant performs `memcpy`s on essentially every call.
As GoLang has no exceptions in the classical OOP sense, most functions return multiple values, error being the last one in each pack.
Batch lookup operations are implemented via channels sending slices, to avoid reallocations.

### JavaScript

* Node.js
* V8
* Deno
* [`bun:ffi`](https://twitter.com/jarredsumner/status/1521527222514774017)

### RESTful API & Clients

We implement a REST server using `Boost.Beast` and the underlying `Boost.Asio`, as the go-to Web-Dev libraries in C++.
To test the REST API, `./src/run_rest.sh` and then cURL into it:

```sh
curl -X PUT \
  -H "Accept: Application/json" \
  -H "Content-Type: application/octet-stream" \
  0.0.0.0/8080/one/42?col=sub \
  -d 'purpose of life'

curl -i \
  -H "Accept: application/octet-stream" \
  0.0.0.0/8080/one/42?col=sub
```

The [`OneAPI` specification](/openapi.yaml) documentation is in-development.

## FAQ

<details>
<summary>Why not use LevelDB interface (which was also adopted by RocksDB)?</summary>
1. Dynamic polymorphism
2. Dependancy on STL
3. No support for custom allocators
</details>

<details>
<summary>Why mix Docs and Graphs?</summary>
Modern Relational databases try to handle flexible-schema documents, but do it poorly.
Similarly, they hardly scale, when the number of "relations" is high.
So users are left with no good multi-purpose solutions.
Having collections of both kinds would solve that.
</details>

<details>
<summary>Why not adapt MQL or Cypher?</summary>
Mongo Query Language and Cypher by Neo4J are widely adopted, but are both vendor-specific.
Futhermore, as for core functionality, using text-based protocols in 2022 is inefficient.
CRUD operations are implemented in all binary interfaces and for document-level patches well standardized JSON-Pointer, JSON-Patch and JSON-MergePAth RFCs have been implemented.
</details>
