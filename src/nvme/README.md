# UStoreNVMe

UStoreNVMe is UStore's NVMe-focused LSM engine, with `io_uring`, Linux AIO, SPDK and blocking transports, GPU compaction and GPUDirect Storage, and SIMD row kits.
Every collection is a log-structured merge tree of immutable sorted runs, each opening with a memory-mapped B-tree of its keys.
Keys are fixed-width stored keys, `ustore_key_t` at the C ABI, and values are opaque bytes whose width a collection fixes at compile time.
The C ABI in `engine.cpp` backs each UStore database with one `database` and each collection with one `collection` under the same UUID.
It mounts on files through whichever file transport its executor names, reserves collections for `ustore_collection_main_k`, the catalog and the names, and creates every other collection with a variable-width value layout so an empty value keeps its key.
Every write of a caller commits through a transaction of the registry, while the catalog and the names change straight in the store under a claim of their own, as no caller's transaction reaches them.
A stage checks a transaction and hands the database's claim back at once, so the commit checks it again; a held claim would stall every other call.

## Data Layout

A type is named after what it holds and what it does, from the whole database down to one value:

| Type                         | What it is                                                                         |
| :--------------------------- | :--------------------------------------------------------------------------------- |
| `database`                   | A named database of collections that share one catalog and one set of transactions |
| `single_collection_database` | A named database of one collection, with its own transactions and snapshots        |
| `collection`                 | One collection: a write buffer and a value cache in front of an LSM tree           |
| `lsm_tree`                   | One LSM tree of levels, with the compaction that republishes them                  |
| `lsm_level`                  | One level of sorted runs                                                           |
| `lsm_sorted_run`             | One immutable file: a header, the sorted run index, then the stored values         |
| `key_slot`                   | A stored key with the size and location of its stored value                        |
| `stored_value`               | One value                                                                          |

The types are templates of `include/ustore/`, which `engine.cpp` instantiates, in `unum::ustore::blobs` for the LSM layers and `unum::ustore::utilities` for the I/O kits beneath them:

- `utilities/nvme_ring.hpp` — allocators, completion counters, identifiers, extents and the ring.
- `utilities/nvme_disks.hpp` — the disk base, the memory disk and the blocking floor.
- `utilities/nvme_linux_uring_disk.hpp`, `nvme_linux_aio_disk.hpp`, `nvme_linux_spdk_disk.hpp` — the Linux transports.
- `utilities/nvme_channels.hpp` — the buffer pool, the I/O channels and the pipelines.
- `utilities/nvme_volume.hpp` — text, catalogs, volumes, the router, the mount and the shared resources.
- `blobs/nvme_key_slot.hpp` — identifiers, value layouts, stored keys and values, key slots and the sorted run index.
- `blobs/nvme_sorted_run.hpp` — `lsm_sorted_run`.
- `blobs/nvme_compaction.hpp` and `nvme_compaction.cuh` — the host compactor, and the kernels of the device one.
- `blobs/nvme_tree.hpp` — configurations and schemas, `lsm_level`, its merger and `lsm_tree`.
- `blobs/nvme_write_buffer.hpp` — `write_buffer`.
- `blobs/nvme_collection.hpp` — the value cache, `collection`, its snapshot and its scanner.
- `blobs/nvme_database.hpp` — both databases, their snapshots, transactions, registries and read sets.
- `graph/edge_block.hpp`, `vertex_registry.hpp` and `nvme_graph.hpp` — the graph fast path below.

The sorted run index is a B-tree of the run's key slots, and the merger hands the compactor each run's index.

Levels differ by depth:

- Level 0, `L0`, receives one sorted run per flushed write buffer, so its sorted runs overlap and a scan merges them by smallest key.
- Intermediate levels keep their sorted runs sorted, and still hold tombstones: removals, whose size of zero shadows older values beneath them.
- The bottom level is sorted too, and compaction into it drops tombstones for good.

Level 0 is due for compaction at 4 sorted runs or at its byte budget, whichever comes first, and every deeper level's budget grows by `level_enlarge_factor`.
The engine compacts after every write that leaves a collection due, on the thread of that write.

### Value Layouts

`value_layout_t` fixes a collection's sorted run layout at compile time, and `resolve_value_layout` picks the narrowest one for a value width:

- `inline_1_byte_k` through `inline_64_bytes_k` keep values of up to 64 bytes inside the key slot, so the sorted run has no stored values section.
- `fixed_width_k` stores wider values of one width in the stored values section.
- `variable_width_k` stores values of any width up to the collection's maximum, and is what a width of zero resolves to.

A `database` mixes collections of different value layouts, and its collection scanner scans any of them.

### Snapshots

A snapshot is a held object rather than a named one: `take_snapshot` flushes every write buffer and counts a reader into the level set each flush published, so nothing that set names can be retired while the snapshot lives.
Taking one copies no data and moves no byte.
There are no snapshot identifiers and no snapshot persistence, so a snapshot lives exactly as long as the object holding it.

A hold is what a publication waits on, so a flush, a compaction or a second snapshot taken while one is still alive on the same thread never returns.
An ordered walk therefore takes a snapshot, reads it and drops it inside the call that needs it, and a caller that mutates what it walked collects the keys first and lets the snapshot go before the first write.

Backup, move and sorted-run export are not implemented.
They belong on the blob surface of `nvme_volume.hpp` rather than on the disk router they were written against, so the files that carried them were removed instead of adapted.

## Transactions

Single-collection database transactions span one collection, database transactions span the collections of a database, and both are optimistic with a two-phase commit:

- `stage` checks the read set for conflicts and flushes the write buffer when the changes would not fit, and a failure keeps the changes so they can be logged.
- `commit` stages unless already staged, then applies the changes on the write path the stage took, which is the database's own claim and one channel.
- `rollback` drops the changes, and `renew` restarts the transaction with its changes kept, which only a transaction without reads may do.

The read set records each watched stored key with the stamp and purpose of its watch, and the transaction registry keeps the keys of every commit made while an older transaction still runs.
Staging fails with `write_conflict_k` when a commit made after a watch began wrote the watched key.
A watch follows one of four schedules:

| Schedule                      | When it checks                                                  |
| :---------------------------- | :-------------------------------------------------------------- |
| `never_k`                     | Never, so collisions are ignored                                |
| `now_k`                       | At the moment of the call                                       |
| `on_commit_k`                 | Between the transaction's start and its commit, and the default |
| `on_commit_since_operation_k` | Between the operation and the commit                            |

The C ABI watches every read and write on `on_commit_k` when a call asks for `ustore_watch_keys_k`, or on `never_k` for `ustore_watch_none_k`, and a read of a missing key watches its absence.
Watches cover keys, not ranges, so a scan watches the keys it hands out and reports no phantom, and an ordered walk inside a transaction merges the transaction's own writes over what the store has committed.
A write that reached no registry would land where no watcher could see it, so a batch the C ABI takes outside a caller's transaction commits through a transaction of its own, sized to that batch.
Commits are ordered by the stamps the registry issues, which keeps a transaction to one node.

## The C ABI

`engine.cpp` serves every slot of `src/engine.hpp`'s `engine_t` the RAM engine serves over one `database`, and `src/docs.cpp` and `src/graph.cpp` layer documents and graphs on its blobs, exactly as they do for LevelDB.
`USTORE_BUILD_NVME` adds the engine to the `ustore` library and `nvme` to the engines every suite runs with `--engine`.

- An open names a directory, which becomes the metadata directory; a data directory it does not name defaults to `data` beneath it, and a capacity it does not name defaults to one gibibyte.
- The executor of `ustore_executor_for_nvme` names the transport, the channels and their buffers, and the VRAM executor compactions run on, read once at the open; any other executor and none keep the defaults.
- `ustore_collection_main_k` is the nil UUID, and the catalog and the names live under two more UUIDs no UUIDv7 spells, which every call refuses as unknown; every other collection carries the UUIDv7 its create minted.
- A read measures a key and copies its value into the call's scratch, measuring again when the value changed its width in between.
- A scan, a sample and a measurement walk one collection's runs and write buffer under the database's claim, binary-searching each run for the start key, and merge a transaction's own writes over them.
- Snapshots answer `ustore_status_unsupported_k`, and the description claims no `ustore_capabilities_snapshots_k`, since a level set held across calls would stall every publication behind it.
- A write that leaves a level due compacts it on the writing thread, and `ustore_database_compact` flushes every collection before merging what is due.

The engine mounts on files through the transport its executor names or the probe settles on: `io_uring` where the kernel allows it, Linux AIO where it refuses a ring, and the blocking floor elsewhere.
Only `ram` is refused, since the database the ABI names keeps its volumes in files.

## Compactions

- Flushing a write buffer writes one L0 sorted run from key slots held in RAM.
- Compaction is a call rather than a background loop: `compact` takes two channels, merges the shallowest level that is over its bounds into the level below, and writes new sorted runs.
- A sorted level with nothing to merge beneath it descends one level whole, and compacting the bottom level appends a new level below it.
- A tree keeps two level sets and stores which of them is live, so a publication builds the next one beside the one readers hold and waits only on the readers of the set it retires.

Compaction merges sorted key slots and plans how their stored values migrate into the new sorted runs, and `compaction_kind_t` names the merge: overlapping or sorted runs, into an intermediate or the bottom level, where tombstones are dropped.
The host and GPU compactors share one comparator, survival rule, sorted run split and placement, so their outputs match field by field.
The GPU compactor runs on CUB from CCCL 3.4.2 with no Thrust, takes its devices, streams and buffers from `include/ustore/utilities/vram_nvidia_device.hpp`, and is chosen once a device is present and a merge exceeds `gpu_compaction_threshold_k`, 900,000 key slots.
GPU compaction is part of v1 and runs on both Hopper and Blackwell.

## Storage I/O

A disk pairs a __volume__, which maps names to bytes, with a __transport__, which moves them.
`nvme_ring.hpp` is the contract every transport meets: a thread takes a queue pair, registers the memory it will read into and the target it will read from, submits a batch and polls its own completions.
Nothing blocks, no ordering is promised between two submitted requests, and durability is written as submit, poll, flush, poll.
The ring is exposed rather than hidden behind a blocking read because a blocking read reaches the depth of one thread, while a namespace wants thousands of commands outstanding.

`io_transport_t` in the configuration asks for a transport, and a mount probes rather than trusting a version number, since a kernel with io_uring still refuses it under Docker's seccomp profile:

- __`blocking_k`__ — `blocking_files_disk_t`, positioned reads and writes on the calling thread.
  Every platform has it, and it is where two refused probes fall back to.
- __`linux_uring_k`__ — `linux_uring_files_disk_t`, one `io_uring` per queue pair through raw `io_uring_setup`, `io_uring_register` and `io_uring_enter`.
  No liburing, and Linux 6.12 or newer.
  Every ring is charged the whole arena against `RLIMIT_MEMLOCK`, so a ring the cap refuses addresses the arena by pointer instead of by registered slot, and the mount stays on `io_uring`.
- __`linux_aio_k`__ — `linux_aio_files_disk_t`, one AIO context per queue pair through raw `io_setup`, `io_submit`, `io_getevents` and `io_destroy`, with no libaio.
  Docker's default seccomp profile, gVisor and kernels below 6.12 keep it where they refuse io_uring.
  It registers nothing, so the kernel pins each request's pages, and it links nothing, so a chained request waits in its seat until the poll that sees its predecessor transfer in full submits it.
  A flush is `IOCB_CMD_FDSYNC`, or `fdatasync` where a kernel older than 4.18 refuses that.
  One thread at depth 256 reads about 500 K random 4 KB blocks a second on a 6.8 kernel, where `pread` reads 14 K.
- __`linux_spdk_k`__ — `linux_spdk_disk_t`, the user-space NVMe driver behind `USTORE_WITH_SPDK`.
  Its probe reads `/proc/meminfo` and the driver binding first, because `rte_eal_init` can otherwise call `exit` on the whole process.
- __`memory_k`__ — `memory_disk_t`, volumes in memory for tests and benchmarks.
- __`automatic_k`__ — io_uring where the kernel allows it, Linux AIO where it refuses a ring, the blocking floor where it offers neither.
  What the mount settled on is reported back rather than inferred.

Above the transport, `nvme_volume.hpp` resolves a name to an extent.
A volume without a filesystem under it keeps its catalog in two slots of the volume itself: an image is written whole into the older slot, flushed, and from that instant it is the newer one, while a torn image fails the checksum its header carries and a mount reads the other slot.
The extent allocator rebuilds the free map from the catalog at mount, which is what makes an extent missing from the catalog free after a crash.
The volume router places a blob on the volume with the most free space that still fits.

Every I/O buffer comes from one registered arena, because a range can be pinned only once — io_uring answers `EBUSY` on re-registration and a growing arena cannot be pinned at all.
In `nvme_channels.hpp`, the buffer pool carves that arena into fixed blocks, each I/O channel gives a queue pair its own pools and polling room, and a pipeline holds the ring at its depth by refilling as completions retire rather than submitting a batch and waiting for all of it.

Depth comes from queue pairs rather than threads.
A dispatch blocks until all its threads return and admits one caller at a time, and ForkUnion bans nested parallelism, so a batch fanned out inside a poll would both serialize two owners and fork a pool that is already forked.

## Graph Fast Path

`include/ustore/graph/nvme_graph.hpp` stores graphs of labeled, weighted, directed or undirected edges in two single-collection databases of variable-width values:

- The vertices database, keyed by `id_entity_t`, the 16-byte `ustore_key_t` of the C ABI, holds each vertex's `vertex_t` of identifier, weight and label, its payload, and its registry.
- The half-edges database, keyed by `id_chunk_t`, the entity paired with a block's first neighbor, holds the sorted edge blocks of that vertex.

An edge, `labeled_edge_t`, carries its identifier, its `first` and `second` vertices, weight, label and direction.
Both endpoints store it as a `labeled_half_edge_t`: the edge's identifier, weight and label, the other vertex, and the role, which is source, target or undirected member.
A vertex's half-edges split into edge blocks of 16 to 512 half-edges, 128 at best, with every half-edge to one neighbor in the same block, so a small update rewrites one block rather than the whole adjacency list.
The `vertex_registry_t` lists each block's first and last neighbor and a quantized size class, so small inserts and removals rarely rewrite it.
Batched calls read registries and edge blocks once and write the changed vertices and edge blocks together, and looking up many vertex pairs in one call can cut IOPS tenfold.

`graph_store_t` opens both databases under the directory its caller passes and destroys their data when it closes, so a graph does not yet outlive its process.
`graph_store.hpp` serves the graph calls of the C ABI over its access, and `engine.cpp` builds them without listing them in its table, as the store's transactions are not the engine's; nothing includes the delta-baseline coder in `delta_coding.hpp`.
The fast path is headed for `graph.hpp`, storing adjacency directly in UStoreNVMe's files, while `c/graph.cpp` stays the modality every engine shares.

## Alternatives

- __WiscKey__ separates keys from values to cut write amplification, and [Badger](https://dgraph.io/blog/post/badger-over-rocksdb-in-dgraph/) applies the same idea in Go.
- __EvenDB__ optimizes compactions.
- [__XOR filters__](https://lemire.me/blog/2019/12/19/xor-filters-faster-and-smaller-than-bloom-filters/) replace Bloom filters with smaller and faster probabilistic filters.
- [__libmdbx__](https://github.com/erthink/libmdbx) evolves LMDB's memory-mapped B+ tree.
- [__Open-channel SSDs__](https://en.wikipedia.org/wiki/Open-channel_SSD) keep the logical-to-physical table in host memory and maintain it on the host CPU, so at a 4 KB average write a 1 TB drive takes almost [3 GB of RAM](https://www.osslab.com.tw/wp-content/uploads/2018/02/ioMemory_VSL_3.2.15_Release_Notes_2017-11-07.pdf).
- __Key-value SSDs__ such as [Samsung's](https://blocksandfiles.com/2019/09/05/samsungs-potentially-groundbreaking-keyvalue-ssd/) and [KAML](https://cseweb.ucsd.edu/~y7jin/papers/KAML17_Yanqin.pdf) move the index into the drive behind `Get` and `Put`, and object drives extend that command set.

## Reading List

- LSM trees:
  - [Log-Structured Merge Trees](http://www.benstopford.com/2015/02/14/log-structured-merge-trees/)
  - [Write Optimization: Myths, Comparison, Clarifications](https://www.percona.com/blog/2011/10/04/write-optimization-myths-comparison-clarifications-part-2/)
  - [Indexing SST Files for Better Lookup Performance](https://github.com/facebook/rocksdb/wiki/Indexing-SST-Files-for-Better-Lookup-Performance)
  - [How We Keep Track of Live SST Files](https://github.com/facebook/rocksdb/wiki/How-we-keep-track-of-live-SST-files)
- Beyond LSM trees:
  - [Cache-Oblivious Lookahead Arrays](https://stackoverflow.com/questions/6079890/cache-oblivious-lookahead-array)
  - [Fractal Trees Compared with LSM Trees](http://highscalability.com/blog/2014/8/6/tokutek-white-paper-a-comparison-of-log-structured-merge-lsm.html)
- NVMe:
  - [The Flash Translation Layer](https://en.wikipedia.org/wiki/Flash_memory_controller#Flash_Translation_Layer_%28FTL%29_and_Mapping)
  - [SPDK's NVMe Hello World](https://github.com/spdk/spdk/blob/master/examples/nvme/hello_world/hello_world.c)
  - [PyNVMe](https://pynvme.readthedocs.io/): [introduction](https://pynvme.readthedocs.io/intro.html), [VSCode integration](https://pynvme.readthedocs.io/vscode.html), [features](https://pynvme.readthedocs.io/features.html#multiple-controllers)
- Zoned storage:
  - [What Is Zoned Storage and the Zoned Storage Initiative?](https://blog.westerndigital.com/what-is-zoned-storage-initiative/)
  - [Enabling the Zettabyte Age](https://blog.westerndigital.com/storage-architectures-zettabyte-age/)
  - [NVMe Zoned Namespaces](https://zonedstorage.io/introduction/zns/)
- Persistent memory:
  - [Persistent Memory Development Kit](https://pmem.io/pmdk/)
  - [NVM Library Overview](https://pmem.io/2014/09/01/nvm-library-overview.html)
  - [An Introduction to pmemobj](https://pmem.io/2015/06/13/accessing-pmem.html)
  - [C++ Bindings and Containers for libpmemobj](https://github.com/pmem/libpmemobj-cpp)
