<h1 align="center">UStore</h1>
<h3 align="center">
GPU-Accelerated Multi-Modal Transactional Database Engine<br/>
Built around Kernel Bypass
</h3>
<br/>

<p align="center">
<a href="https://discord.gg/4mxGrenbNt"><img height="25" src="https://github.com/unum-cloud/.github/raw/main/assets/discord.svg" alt="Discord"></a>
&nbsp;&nbsp;&nbsp;
<a href="https://www.linkedin.com/company/unum-cloud/"><img height="25" src="https://github.com/unum-cloud/.github/raw/main/assets/linkedin.svg" alt="LinkedIn"></a>
&nbsp;&nbsp;&nbsp;
<a href="https://twitter.com/unum_cloud"><img height="25" src="https://github.com/unum-cloud/.github/raw/main/assets/twitter.svg" alt="Twitter"></a>
&nbsp;&nbsp;&nbsp;
<a href="https://unum.cloud/post"><img height="25" src="https://github.com/unum-cloud/.github/raw/main/assets/blog.svg" alt="Blog"></a>
&nbsp;&nbsp;&nbsp;
<a href="https://github.com/unum-cloud/UStore"><img height="25" src="https://github.com/unum-cloud/.github/raw/main/assets/github.svg" alt="GitHub"></a>
</p>

<p align="center">
Blobs • Strings • Documents • Graphs • Pages
<br/>
<a href="https://github.com/unum-cloud/UStore/blob/main/c/README.md">C</a> •
<a href="https://github.com/unum-cloud/UStore/blob/main/cpp/README.md">C++</a> •
<a href="https://github.com/unum-cloud/UStore/blob/main/python/README.md">Python</a> •
<a href="https://github.com/unum-cloud/UStore/blob/main/rust/README.md">Rust</a> •
<a href="https://github.com/unum-cloud/UStore/blob/main/mojo/README.md">Mojo</a>
<br/>
Linux • macOS • Windows
<br/>
x86 • Arm • RISC-V • POWER • CUDA • ROCm
</p>

---

UStore is an embedded transactional database engine that keeps blobs, documents, graphs, strings and markup pages in collections addressed by 128-bit UUID keys, with transactions spanning collections behind one C ABI.
Its NVMe engine reaches drives through kernel bypass with io_uring and SPDK, lays immutable sorted runs into 1 GB slots of raw drives or into files under JSON manifests, searches them with SIMD S+ trees, and merges them on GPUs that read through GPUDirect Storage.
It links into the process that uses it, with no server to deploy, and the in-memory `ram` reference engine and the RocksDB and LevelDB engines serve the same calls.

- __Transactions across collections__ — blobs, documents and graphs stage into one transaction and commit together.
- __One C ABI over every engine__ — the same calls run on the NVMe engine, the in-memory `ram` reference engine and the RocksDB and LevelDB key-value stores.
- __Isolation chosen at open__ — the `ram` engine runs at `strict_serializable` or `snapshot`, picked by the `isolation` field of `ustore_database_init_ram_t`.
- __Documents without a query language__ — fields are addressed by JSON Pointer and modified by JSON Patch and JSON MergePatch.
- __Graphs as a modality__ — directed multigraphs with identified edges, stored in the same database as the documents they relate.
- __Batches with strides__ — every call takes arrays of keys, values and collections, laid out as structures of arrays or arrays of structures.
- __Sampling as a primitive__ — `ustore_blobs_sample` draws random keys from a collection, next to read, write and scan.

## Performance

Items per second from `ustore_bench_twitter` over 100,000 generated tweets, indexed as 200,000 documents and 500,000 edges.
Every row runs 16 threads pinned to the 8 P-cores of an Intel Core i9-13950HX, and the disk engines share one PCIe 4 NVMe SSD under ext4.

| Engine      | Blob writes | Blob reads | Doc writes | Doc reads | Graph writes | Two-hop walks |
| :---------- | ----------: | ---------: | ---------: | --------: | -----------: | ------------: |
| UStore RAM  |       765 k |     5.25 M |      502 k |    12.3 M |        107 k |        4.64 M |
| UStore NVMe |      99.4 k |     67.5 k |     19.7 k |     562 k |       35.3 k |         139 k |
| RocksDB     |       242 k |      880 k |      279 k |     946 k |        641 k |        30.3 M |
| LevelDB     |      17.9 k |      427 k |          … |         … |            … |             … |

Blob reads fetch one key per call, while every other column moves batches of 128 keys, and writes commit as transactions of one batch each.
Graph batches that conflict under 16 writers fail without a retry, from 1,102 on `ram` to 2,572 on RocksDB, so the walks cover graphs of different sizes.
The LevelDB engine served the blob rows alone in this run.

```sh
cmake --preset release -D USTORE_WITH_NVME=ON
cmake --build --preset release --target ustore_bench_twitter
build_release/build/bin/ustore_bench_twitter --engine nvme --directory /mnt/nvme/ustore --max-tweets 100000 --threads 16
```

The [benchmark guide](https://github.com/unum-cloud/UStore/blob/main/bench/README.md) covers the other workloads, compaction on the GPU, and the unit economics.

## Quickstart

UStore builds with CMake against StringZilla, ForkUnion and SmashTable, pulled as git submodules.

```cmake
FetchContent_Declare(ustore GIT_REPOSITORY https://github.com/unum-cloud/UStore.git)
FetchContent_MakeAvailable(ustore)
target_link_libraries(my_app PRIVATE ustore::cpp)
```

`ustore::cpp` carries the C++ SDK, the C headers of `ustore::abi` and `ustore::static`, an archive that links the storage engines' archives installed beside it.
`ustore::shared` exports the C ABI alone, `ustore::templates` names the header-only templates, and `-DUSTORE_INSTALL=ON` installs them all with a CMake package for `find_package(ustore)`.
The program opens a database in memory, writes a blob, a document and an edge in one transaction, and reads them back, as every binding's Quickstart does.

```cpp
#include <cassert>

#include <ustore/ustore.hpp>

using namespace unum::ustore;

int main() {
    auto [db, open_status] = database_t::init_ram();
    auto [posters, posters_status] = db.create<blobs_collection_t>("posters");
    auto [movies, movies_status] = db.create<docs_collection_t>("movies");
    auto [similar, similar_status] = db.create<graph_collection_t>("similar");
    assert(open_status.succeeded() && posters_status.succeeded() && movies_status.succeeded() &&
           similar_status.succeeded());

    // One transaction writes a blob, a document and an edge, and commits all three or none
    ustore_key_t const avatar = key_from_integer(1), titanic = key_from_integer(2);
    auto [transaction, transaction_status] = db.transaction();
    transaction_t const& inside = transaction;
    status_t const poster = inside["posters"].value.at(avatar).assign("\x89PNG");
    status_t const movie = inside.find<docs_collection_t>("movies").value.at(avatar).assign(
        R"({"title":"Avatar","year":2009})");
    status_t const edge = inside.find<graph_collection_t>("similar").value.write_edge(edge_t {avatar, titanic});
    auto [committed, commit_status] = inside.commit();
    assert(transaction_status.succeeded() && poster.succeeded() && movie.succeeded() && edge.succeeded() &&
           commit_status.succeeded());

    // Reads at the head see the commit: the poster, the title and the one edge leaving Avatar
    auto [stored, stored_status] = posters[avatar].value();
    auto [title, title_status] = movies[collection_key_field_t {avatar, "title"}].value(ustore_docs_field_type_string_k);
    auto [degree, degree_status] = similar.degree(avatar, ustore_vertex_role_source_k);
    assert(stored_status.succeeded() && stored == "\x89PNG" && title_status.succeeded() && title == "Avatar" &&
           degree_status.succeeded() && degree == 1);
}
```

A persistent engine opens through its own constructor, which takes the directory, and the rest of the code stays the same.

```cpp
auto [db, status] = database_t::open_rocksdb({.directory = bytes_of("/var/lib/ustore/")});
```

> `open_ram` persists the in-memory engine in a directory as one image, and `view_rocksdb` serves a RocksDB directory read-only, refusing every change with `status_code_t::read_only_k`.
> Every fallible C++ call returns a `status_t`, an `expected`, or a `deferred` call whose `wait` answers one, and every C call reports failure through its `ustore_outcome_t` argument, a status beside a static message.
> A deferred C call returns once submitted, and its outcome is polled through `ustore_outcome_status` and waited on through `ustore_outcome_wait`.
> Calls run on the calling thread until a receiver names an executor, which by default spawns a thread for 7/8 of the cores local to the memory domains it serves, and keeps its scratch and its arenas on those domains.
> Scans, cursor pulls, snapshot saves and compactions report their advance to an optional progress callback, which is the only way to cancel a call.

The C ABI and its argument structs are described in [`c/README.md`](https://github.com/unum-cloud/UStore/blob/main/c/README.md), and the C++ wrappers in [`cpp/README.md`](https://github.com/unum-cloud/UStore/blob/main/cpp/README.md).

## Architecture

| Layer      | What it is                                                             | Where                               |
| :--------- | :--------------------------------------------------------------------- | :---------------------------------- |
| Bindings   | C++, Python, Rust and Mojo, each forwarding rather than reimplementing | `cpp/`, `python/`, `rust/`, `mojo/` |
| C ABI      | One argument struct per call, 128-bit UUID keys                        | `c/ustore/*.h`                      |
| Modalities | Documents and graphs, written once over any engine                     | `src/*.cpp`                         |
| Engines    | `ram` and `nvme` are ours, `rocksdb` and `leveldb` are checked against | `src/`, `src_oracles/`              |
| Hardware   | NVMe drives, GPUs and CPU cores, in one box                            | —                                   |

- __The C ABI__ in `c/ustore/*.h` is the only interface: every call takes one argument struct, and every binding forwards to it.
- __The modalities__ in `src/docs_layer.hpp` and `src/graph_layer.hpp` implement documents and graphs once, on top of whichever engine the library links, while the `ram` engine serves graphs on half-edges of its own and every engine keys its strings collections itself.
- __The `ram` engine__ is the reference: ordered multi-version stores in memory on SmashTable containers, persisted as one binary image when it opens with a directory.
- __NVMe__ is the engine the hardware paths serve: each collection is an `lsm_tree` of `lsm_level`s of `lsm_sorted_run`s, point lookups search SmashTable's SIMD row kits, compactions merge sorted runs on the CPU or a GPU, GPUDirect Storage reads runs from drives into GPU memory for compaction, and a transport chosen at mount reaches the drive through io_uring, SPDK or blocking positioned I/O.
- __RocksDB and LevelDB__ keep their own storage formats, so an application moves between them and the other engines without changing a call.

## Why Hyper-Converged

Disaggregated storage puts a network between every query and its data, and a server full of PCIe 5.0 NVMe drives reads faster than the links in front of it carry.
Compaction then turns into a remote copy, and a GPU that could merge sorted runs waits on bytes crossing the network twice.
UStore argues for the opposite design: storage, compute and accelerators in one box, each device reached through the most direct path its platform offers.
Kernel bypass removes the system calls and page-cache copies between the drive and the engine, GPUDirect Storage reads data from drives into GPU memory without staging it in host memory, and SIMD kits keep point lookups on the CPU, where a single lookup is too small to ship anywhere.
Scale-out stays above the database, where a data-lake layer composes several such boxes through two-phase commits and each box keeps its own drives and accelerators.

## Blobs

A collection maps UUID keys to binary values, and a database holds any number of them, each under its own name.
`find_or_create` opens one by name, creating it on first use.

```cpp
auto [archive, archive_status] = db.find_or_create("archive");
status_t status = archive[key_from_integer(7)].assign("poster bytes");
auto lengths = archive[{key_from_integer(1), key_from_integer(2), key_from_integer(7)}].length();
status = archive[key_from_integer(7)].remove();
```

A value sits inside the same transaction as its metadata, so a media file and the document describing it commit or fail together.
A cursor walks a collection in pages, reading the values of each page in the same call, and resumes past the last key it handed out, so it sees keys written ahead of it and never those behind.

Sampling is how a training loop shuffles a dataset whose metadata does not fit in RAM.
`ustore_blobs_sample` returns a random subset of a collection's keys, and the reads that follow fetch only those.

## Documents

A document collection stores JSON, parsed and modified with yyjson.
A field is either a top-level key or an RFC 6901 JSON Pointer starting with a slash.

```cpp
auto [users, users_status] = db.find_or_create<docs_collection_t>("users");
status_t status = users[key_from_integer(1)].assign(R"({"name": "Alice", "follows": [2, 3]})");

auto name = users[collection_key_field_t {key_from_integer(1), "/name"}].value();
status = users[key_from_integer(1)].patch(R"([{"op": "add", "path": "/age", "value": 24}])");
status = users[key_from_integer(1)].merge(R"({"follows": null})");
```

Modifications follow open RFCs rather than a proprietary query language, and each maps onto one `ustore_docs_modification_t` mode of `ustore_docs_write`:

- [JSON Pointer, RFC 6901][pointer] addresses nested fields.
- [JSON Patch, RFC 6902][patch] applies field-level updates.
- [JSON MergePatch, RFC 7386][merge-patch] applies document-level updates.

`insert`, `update` and `upsert` differ only in whether the document must be absent, present, or either.
`gist` lists the unique paths to the leaves of a set of documents, and `gather` reads chosen fields of many documents into typed columns.

[pointer]: https://datatracker.ietf.org/doc/html/rfc6901
[patch]: https://datatracker.ietf.org/doc/html/rfc6902
[merge-patch]: https://datatracker.ietf.org/doc/html/rfc7386

## Graphs

A graph collection stores a directed multigraph, where every edge carries a source, a target and its own identifier.

```cpp
auto [friends, friends_status] = db.find_or_create<graph_collection_t>("friends");
status_t status = friends.write_edge(edge_t {key_from_integer(1), key_from_integer(2), key_from_integer(100)});
status = friends.write_edge(edge_t {key_from_integer(2), key_from_integer(3), key_from_integer(101)});

auto [arena, arena_status] = arena_t::make();
auto neighbors = friends.neighbors(key_from_integer(2), arena);
auto degree = friends.degree(key_from_integer(2), ustore_vertex_role_source_k);
auto present = friends.present(key_from_integer(3));
```

The neighbors land in the arena the call is given, while `degree` and `present` of one vertex copy a scalar out of a one-off arena.
Upserting an edge upserts its vertices, removing an edge keeps them, and removing a vertex removes every edge that touches it.
The C ABI behind these calls is six functions: `ustore_graph_read_edges`, `ustore_graph_write_edges`, `ustore_graph_remove_edges`, `ustore_graph_write_vertices`, `ustore_graph_remove_vertices` and `ustore_graph_scan_vertices`.

Documents and graphs share one key type, so a user's profile and a user's vertex carry the same key, in one box, inside one transaction.

## Transactions

A transaction reads and writes through the same collection types as the database.

```cpp
auto [transaction, transaction_status] = db.transaction();
auto [users, users_status] = transaction.find<docs_collection_t>("users");
auto [friends, friends_status] = transaction.find<graph_collection_t>("friends");

status_t status = users[key_from_integer(4)].assign(R"({"name": "Dana"})");
status = friends.write_edge(edge_t {key_from_integer(1), key_from_integer(4), key_from_integer(102)});

if (auto [commit_stamp, commit_status] = transaction.commit(); commit_status.failed())
    return 1;
```

### Atomicity

A commit applies every staged write or none of them.

### Consistency & Isolation

A transaction watches the keys it reads and writes, and a commit fails when another transaction committed a colliding write in between.
`ustore_watch_none_k` turns that collision detection off for one call, trading the guarantee for cheaper commits, and changes nothing on a `ram` database at `strict_serializable`, which validates every read.

A snapshot is an opaque handle pinning a consistent view of the whole database for reads, and `db.snapshot()` returns an `expected<snapshot_t>` that reads through it and frees it when it goes.
`snapshot.save(path)` writes exactly what the snapshot sees out as a new database of the same engine.
Spin models in [`verification/`](https://github.com/unum-cloud/UStore/tree/main/verification) check the ring's completion credit, the one-issuer rule that moves a queue pair between threads, and the write, flush, record and sync order behind durability.

### Durability

Commits do not flush by default, since a database built on top of a key-value store usually keeps its own write-ahead log.
Durability at commit is one option away.

```c
ustore_transaction_commit_t commit = {
    .database = database,
    .outcome = &outcome,
    .transaction = transaction,
    .durability = ustore_durability_flushed_k,
};
ustore_transaction_commit(&commit);
```

A `ram` database without a directory has no durability to offer.

## Engines

Every engine implements the same C ABI of `c/ustore/db.h`, `c/ustore/blobs.h` and `c/ustore/strings.h`, and documents and graphs run on top of any of them.

| Engine    | Role                  | Storage                        | Transactions | Snapshots | Collections |
| :-------- | :-------------------- | :----------------------------- | :----------- | :-------- | :---------- |
| `ram`     | reference, in memory  | memory, with an optional image | yes          | yes       | yes         |
| `nvme`    | hyper-converged       | LSM trees on NVMe or files     | yes          | yes       | yes         |
| `rocksdb` | baseline, third-party | LSM tree on a filesystem       | yes          | yes       | yes         |
| `leveldb` | baseline, third-party | LSM tree on a filesystem       | no           | yes       | yes         |

Each carries a limit worth knowing before choosing it.
The `ram` engine rewrites its whole image on a flush and keeps no log, so a crash loses everything written since the last one.
NVMe keeps no write-ahead log, so a crash loses the commits its memory runs held, a suffix of them and never half of one.
RocksDB takes a transaction's snapshot at its first read or write rather than when it began, and stages by locking and validating every watched read and buffered write against that snapshot, then preparing the writes into its log.
LevelDB has no transactions.

The engines build into one library, offered as the `ustore_static` archive and the `ustore_shared` library, which holds `ram` and every engine enabled through `USTORE_WITH_NVME`, `USTORE_WITH_ROCKSDB` and `USTORE_WITH_LEVELDB`.

An engine reports itself through `ustore_database_describe`: its name, what it supports, its isolation, and its estimates of the keys, memory and disk the data takes.
The C++ `database_t` exposes the capability bits as `supports_transactions`, `supports_snapshots` and `supports_collections`.
`ustore_database_clear` empties every collection at once, and `ustore_database_compact` reclaims the space overwritten and removed data still takes.
NVMe builds behind `USTORE_WITH_NVME`, which is off by default and on in the presets that test it.
It runs on every platform through blocking positioned I/O, mounts io_uring on Linux where the kernel and the seccomp profile allow it, and merges on the GPU when built with `USTORE_CUDA_ARCH`; [`src/nvme/README.md`](https://github.com/unum-cloud/UStore/blob/main/src/nvme/README.md) describes its layers.
Each io_uring ring registers its buffers where `RLIMIT_MEMLOCK` allows and addresses them by pointer otherwise, staying on io_uring either way.
A database on NVMe is a `<database>.ustore.json` manifest and one `.ustore` file per sorted run, on filesystems or in 1 GB slots of raw drives, as [`docs/format.md`](https://github.com/unum-cloud/UStore/blob/main/docs/format.md) describes.

## Keys & Values

Keys are 128-bit UUIDs in `c/ustore/db.h`, compared bytewise, and `key_from_integer` spells a non-negative integer as its big-endian bytes, so keys made from integers order as the integers.
Collections are named by UUIDv7 identifiers the catalog mints once and never reuses, and the main collection by the nil UUID.
Fixed-width keys compare as machine words, while variable-length keys force character-wise comparisons and force keys and values to be joined on disk.
Map strings onto keys once, at the edge of the application, and address documents, graphs and blobs by the resulting identifiers.

Value lengths are `ustore_length_t`, a 64-bit unsigned integer, and a batch of values is one `ustore_strings_view_t`: an Apache Arrow tape of 32-bit offsets as in `binary`, of 64-bit offsets as in `large_binary`, or an accessor callback over any other layout.
A call writes its tapes with 32-bit offsets while the bytes fit in 4 GB and 64-bit beyond, so neither one value nor one batch stops at 4 GB.

## Multiversioned and Monoversioned Builds

UStore compiles in one of two shapes, picked by `USTORE_MULTIVERSIONED`.

- __Multiversioned builds are for distribution.__
  One binary runs well across diverse machines, because each CPU and each GPU picks its own class at runtime.
  This is the default, and the shape of the Python wheels and the Rust crate.
- __Monoversioned builds are for throughput.__
  For common hardware and data-center deployments, the binary carries only the best code for one machine.
  These are the leanest binaries: they start fast and take little memory and disk.
  Outside that machine they refuse cleanly with `ustore_status_unsupported_k`, never crashing and never falling back to slower code in silence.
- __The switches.__
  `USTORE_MULTIVERSIONED=OFF` picks the second shape and `USTORE_MARCH` names its CPU floor.
  The `xeon4_hopper`, `xeon6_blackwell`, `grace_hopper`, `vera_rubin` and `venice_mi450` presets spell whole machines.

A monoversioned build compiles the single `monoversioned` CPU class in place of the ladder of named ones.
It needs no monoversioned GPU class, as a preset's one GPU architecture already matches a named class, and CMake builds only the class units carrying code for it.

## Compatibility

Builds target the baseline instruction set of each architecture, so one artifact runs on any CPU of it.
The C headers are C11, checked by `c/test.c` under every platform's compiler against both libraries, while the C++ headers and the engines need C++20, and the CPython core is C11, with Pandas and NetworkX adapters on top.

| Platform       | C ABI and `ram` | RocksDB | LevelDB | NVMe     | Python | Rust |
| :------------- | :-------------: | :-----: | :-----: | :------- | :----: | :--: |
| Linux x86-64   |       ✅        |   ✅    |   ✅    | local    |   ✅   |  ✅  |
| Linux AArch64  |       ✅        |   ✅    |   ✅    | untested |   ✅   |  ✅  |
| macOS arm64    |       ✅        |   ✅    |   ✅    | untested |   ✅   |  ✅  |
| Windows x86-64 |       ✅        |   ✅    |   ✅    | untested |   ✅   |  ✅  |

Every column but NVMe is exercised by CI on all four platforms: `.github/workflows/prerelease.yml` builds and tests the C ABI and each engine, the Python extension and the Rust crate.
The NVMe column records the suites in [`test/`](https://github.com/unum-cloud/UStore/tree/main/test) on two Linux x86-64 machines rather than CI, which does not build NVMe; the blocking transport compiles for the other platforms and has not been exercised there, io_uring needs Linux 6.12 or newer, and the SPDK transport passes the shared ring cases over NVMe/TCP and is mounted by the engine.
GPU compaction targets NVIDIA Hopper and Blackwell alike through the CUDA driver API, AMD CDNA through HIP's module API and Apple GPUs through Metal, the last two untested on hardware; it launches three kernels per merge without linking any of those runtimes, and its output must match CPU compaction on the same runs.
The engines and the layout of `src/` are described in [`src/README.md`](https://github.com/unum-cloud/UStore/blob/main/src/README.md).

### Why Not RocksDB

LevelDB and RocksDB expose C++ interfaces built on standard library strings and containers.
A caller cannot bring its own strings, arena or allocator, and an allocation failure surfaces as an exception, which a database under memory pressure hits routinely.
Keys are variable-length strings, and the classes rely on virtual calls and inheritance, so the interface is neither ABI-stable nor portable to other languages.

UStore puts a C ABI in front of the same engines instead.
Each call takes one argument struct, so optional arguments keep their defaults and the argument order stops mattering.
Outputs are allocated from an arena the caller owns and reuses across calls.

## Repository Layout

```bash
ustore/
├── c/                the C SDK: the C ABI in ustore/*.h and its plain C11 test
├── cpp/              the C++ wrappers of the C ABI in ustore/*.hpp
├── src/              the router, documents and graphs, over any engine
│   ├── ram/          the in-memory engine
│   └── nvme/         the on-disk engine
├── src_oracles/
│   ├── rocksdb/      the RocksDB baseline
│   └── leveldb/      the LevelDB baseline
├── python/           the CPython extension and the package around it
├── rust/             the crate over the C ABI
├── tools/            the command line interface, dataset import and export
├── test/             suites, one file per topic
├── bench/            benchmarks, one file per topic
├── verification/     models of the concurrency and durability protocols
└── examples/         one script per modality
```

Conventions, hooks and the commit format are described in [`CONTRIBUTING.md`](https://github.com/unum-cloud/UStore/blob/main/CONTRIBUTING.md).

## Citation

If UStore helps your work, please cite it:

```bibtex
@software{Vardanian_UStore,
  author = {Vardanian, Ash},
  title = {{UStore: GPU-Accelerated Multi-Modal Transactional Database Engine Built around Kernel Bypass}},
  doi = {10.5281/zenodo.7537042},
  url = {https://github.com/unum-cloud/UStore},
  license = {Apache-2.0}
}
```

## License

Licensed under the Apache License, Version 2.0.
See `LICENSE` for details.
