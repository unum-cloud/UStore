# UStoreNVMe

UStoreNVMe is UStore's NVMe-focused LSM engine, with `io_uring`, Linux AIO, SPDK and blocking transports, GPU compaction and GPUDirect Storage, and SIMD row kits.
Every collection is a log-structured merge tree of immutable sorted runs, each carrying a saved S+ tree of its keys that every lookup descends in place.
Keys are fixed-width stored keys, `ustore_key_t` at the C ABI, and values are opaque bytes whose width a collection fixes at compile time.
The C ABI in `variants.hpp` backs each UStore database with one `database` and each collection with one `nvme_store` under the same UUID.
Every call is a template on the CPU class, whose atomic reference and waiter instantiate the `database`, and `src/nvme/variants_<class>.cpp` compiles one class each; the open in `engine.cpp` takes the newest class its ceiling fits, ending on the serial floor, and every handle it hands out carries that class's table.
It mounts on files through whichever file transport its executor names, reserves the `_collections` and `_names` system collections, and creates every other collection with a variable-width value layout so an empty value keeps its key.
Every write of a caller commits through a transaction of the registry, while both system collections change straight in the store under a claim of their own, as no caller's transaction reaches them.
A stage checks a transaction and hands the database's claim back at once, so the commit checks it again; a held claim would stall every other call.

## Data Layout

A type is named after what it holds and what it does, from the whole database down to one value:

| Type                         | What it is                                                                         |
| :--------------------------- | :--------------------------------------------------------------------------------- |
| `database`                   | A named database of collections that share one schema and one set of transactions  |
| `single_collection_database` | A named database of one collection, with its own transactions and snapshots        |
| `nvme_store`                 | The store of one collection: memory runs and a value cache in front of an LSM tree |
| `lsm_tree`                   | One LSM tree of levels, with the compaction that republishes them                  |
| `lsm_level`                  | One level of sorted runs                                                           |
| `lsm_sorted_run`             | One immutable file: a header, the sorted run index, value offsets, then values     |
| `key_slot`                   | A stored key, beside its value only where the value sits inline                    |
| `stored_value`               | One value                                                                          |

The types are templates of `include/ustore/`, which `variants.hpp` instantiates once per CPU class, in `unum::ustore::blobs` for the LSM layers and `unum::ustore::utilities` for the I/O kits beneath them:

- `utilities/nvme_ring.hpp` — completion counters, disk and file identifiers, extents and the ring.
- `utilities/nvme_disks.hpp` — the disk base, the memory disk and the blocking floor.
- `utilities/nvme_linux_uring_disk.hpp`, `nvme_linux_aio_disk.hpp`, `nvme_linux_spdk_disk.hpp` — the Linux transports.
- `utilities/nvme_channels.hpp` — the buffer pool, the I/O channels and the pipelines.
- `utilities/nvme_slots.hpp` — the 1 GB slots of a raw medium, the owner block opening each held slot, and the slot record.
- `utilities/nvme_manifest.hpp` — the `<database>.ustore.json` manifest, printed and parsed with yyjson.
- `utilities/nvme_volume.hpp` — data directory addresses, the slot allocator, and the directory and slotted volumes.
- `utilities/nvme_mounts.hpp` — the transport probe, the erased files disk, the mount and the shared resources.
- `blobs/nvme_key_slot.hpp` — identifiers, value layouts, stored keys and values, key slots and the sorted run index.
- `blobs/nvme_sorted_run.hpp` — `lsm_sorted_run`.
- `blobs/nvme_compaction.hpp`, `vram_nvidia_compaction.hpp` and `vram_nvidia_compaction.cuh` — the host compactor, the host face of the device one, and its kernels.
- `blobs/nvme_tree.hpp` — configurations and schemas, the merger and `lsm_tree`; `blobs/nvme_lsm_level.hpp` holds `lsm_level`.
- `blobs/nvme_store.hpp` — `nvme_store` and its snapshot; `blobs/nvme_value_cache.hpp` holds the value cache and `blobs/nvme_store_cursor.hpp` holds `nvme_store_cursor`.
- `blobs/nvme_transactions.hpp` — `transaction_deltas`, the read sets, `transaction_registry` and `transaction_core`, which both databases compose.
- `blobs/nvme_database.hpp` and `nvme_single_collection_database.hpp` — both databases, their snapshots and transactions over the shared registry and core.
- `graph/adjacency_fold.hpp`, `vram_nvidia_adjacency_fold.hpp` and `nvme_store.hpp` — the graph store below, and the host face of its device fold.

A run keeps its key slots in sorted order beside a saved S+ tree of their keys, and the merger hands the compactor each run's cells.

Levels differ by depth:

- Level 0, `L0`, receives one sorted run per flush of a collection's memory runs, so its sorted runs overlap and a scan merges them by smallest key.
- Intermediate levels keep their sorted runs sorted, and still hold remove deltas, whose kind shadows older values beneath them.
- The bottom level is sorted too, and compaction into it drops remove deltas for good.

A range removal lands as one range tombstone beside the run's deltas, never as a removal per key, as RocksDB's `DeleteRange` and ScyllaDB's range tombstones do.
A tombstone hides every older version of the keys from its start up to its exclusive end: in memory runs by stamp, and in the levels by position, a tombstone hiding the versions of deeper levels, of older runs of level 0 and of its own run stamped below it, since merges raise version stamps to the floor and tombstones keep theirs.
A read checks the tombstones of the runs above the version it found, a scan skips the keys they hide, and a run's key bounds cover its tombstones, so the runs of one level stay disjoint and a merge picks every run a tombstone reaches.
A merge drops the versions a tombstone hides from every reader, keeping those a pinned reader still reads, cuts the tombstone at the bounds of the runs it writes, and drops it at the bottom once no reader reads below it; a merge carrying tombstones runs on the CPU.

Level 0 admits `level_enlarge_factor` runs of one flush each, and every deeper level `level_enlarge_factor` times the bytes of the one above, so no level size is configured on its own.
Level 0 is due for compaction at that many sorted runs or at that many flushes' worth of memory-run bytes, whichever comes first.
A tree refuses to mount with a factor outside `[level_min_enlarge_factor_k, level_max_enlarge_factor_k]`, the bounds a collection's configuration is validated against.
The engine compacts after every write that leaves a collection due, on the thread of that write.

### Value Layouts

`value_layout_t` fixes a collection's sorted run layout at compile time, and `resolve_value_layout` picks the narrowest one for a value width:

- `inline_12_bytes_k` and `inline_44_bytes_k` keep values of up to 12 or 44 bytes inside a 32 or 64-byte key slot, so the sorted run has no stored values section.
- `fixed_width_k` stores wider values of one width in the stored values section, found by stride from the key slot's sorted position.
- `variable_width_k` stores values of any width up to the collection's maximum, found through a `u32` value offsets column right before the values, and is what a width of zero resolves to.

Every section of a sorted run starts on a 4,096-byte page, and a cell holds the 16-byte key alone for both layouts that store values apart, so a row of 16 cells is 256 bytes.
The value offsets stay in memory like the tails do, so a point read still costs one I/O, and the values of any range of positions read as one tape of offsets and bytes, which is an Arrow strings column and a StringZilla tape as it lands.

A `database` mixes collections of different value layouts, and its collection scanner scans any of them.

### Stamps and snapshots

Every commit draws one stamp from the database's `commit_ledger_t`, over SmashTable's `commit_order_t`, and a graph's two databases share one ledger.
A memory run carries the stamp of the commit that wrote each of its deltas, and every sorted run carries a column of its key slots' stamps as an `immutable_packed_array` trailer, frame-of-reference coded from a floor no reader can tell apart, so a cold run spends no bit on them.
The trailer's second column holds every key slot's `delta_kind_t`, which tells a write, a removal and a fold's change apart, so no value offset or width is a sentinel, and a run of writes alone spends no bit on it.
A run may hold several versions of one key, newest first, and the run cutter never splits them.

A reader pins a stamp in the ledger and reads the newest version at or below it.
A merge of memory runs keeps a version it replaces only while some pin falls between the two or the replacing commit is newer than the stamp published when the merge began, so a hot key nobody reads keeps one older version at most.
A flush and a merge keep one version per interval between pinned stamps, every version above the durable horizon, and the newest below it, which with no pin is the newest version alone, as before stamps.
A reader the memory budget cannot keep history for is refused at its next read with `write_conflict_k`, and the writer never is.

`pin_stamp` still holds the level set each flush published, for the callers inside one call that walk the runs themselves.

### Crash atomicity

The manifest carries a generation and the durable stamp D: every commit at or below D is wholly in the runs it names.
D is the least stamp below the oldest commit any collection holds in memory runs alone, or the published stamp while no collection holds one.
A mount reads the manifest in force, resumes the stamps at D and merges down every level holding a version above D, dropping those versions, so a crash loses a suffix of commits and never half of one.
A graph truncates both of its databases at the lesser of their two promises.

Backup, move and sorted-run export are not implemented.
They belong on the run files and the manifest of `nvme_volume.hpp`.

## Transactions

Single-collection database transactions span one collection, database transactions span the collections of a database, and both are optimistic with a two-phase commit:

- `stage` checks the read set for conflicts and flushes the memory runs when the changes would not fit beside them, and a failure keeps the changes so they can be logged.
- `commit` stages unless already staged, then applies the changes on the write path the stage took, which is the database's own claim and one channel.
- `rollback` drops the changes, and `renew` restarts the transaction with its changes kept, which only a transaction without reads may do.

A transaction pins its start stamp and reads at it, its own writes first.
The read set records each watched stored key with the stamp of its watch, a written key keeps its watch beside its write, and a scan records the range it walked.
Staging fails with `write_conflict_k` when a commit stamped after a watch began wrote a watched key or any key of a watched range, which the memory runs and then the runs whose newest stamp is newer answer exactly, with no fingerprint and no false conflict.
A watch follows one of four schedules:

| Schedule                      | When it checks                                                  |
| :---------------------------- | :-------------------------------------------------------------- |
| `never_k`                     | Never, so collisions are ignored                                |
| `now_k`                       | At the moment of the call                                       |
| `on_commit_k`                 | Between the transaction's start and its commit, and the default |
| `on_commit_since_operation_k` | Between the operation and the commit                            |

The C ABI watches every read and write on `on_commit_k` when a call asks for `ustore_watch_keys_k`, or on `never_k` for `ustore_watch_none_k`, and a read of a missing key watches its absence.
A scan watches the keys it hands out and the range it covered, so a phantom conflicts too, and an ordered walk inside a transaction merges the transaction's own writes over what the store held at its start.
A write outside every transaction draws a stamp of its own after its room is reserved, so a watcher sees it.
Commits are ordered by the stamps the ledger issues, which keeps a transaction to one node.

## The C ABI

`variants.hpp` fills every verb table of `src/engine.hpp` the RAM engine fills over one `database`, its graph, docs and pages collections through stores of their own, each refusing collections of any other modality.
`USTORE_WITH_NVME` adds the engine to the `ustore` library and `nvme` to the engines every suite runs with `--engine`.

- An open names a directory, which becomes the metadata directory; a data directory it does not name defaults to `data` beneath it, and a capacity it does not name defaults to one gibibyte.
- The executor of `ustore_executor_for_nvme` names the transport, the channels and their buffers, and the VRAM executor compactions run on, read once at the open; any other executor and none keep the defaults.
- The `_collections` and `_names` system collections live under two UUIDs no UUIDv7 spells, which every call refuses as unknown, as it does the nil UUID; every other collection carries the UUIDv7 its create minted.
- A read measures a key and copies its value into the call's scratch, measuring again when the value changed its width in between.
- A scan, a sample and a measurement walk one collection's runs and memory runs under the database's claim, binary-searching each run for the start key, and merge a transaction's own writes over them; a descending scan walks its range ascending and keeps the last keys of the page.
- A range removal of blobs, documents, whole pages, graph vertices or strings is one range tombstone per range in the commit, a range of strings bounded by whole strings and reaching past the last one for an empty end.
- A snapshot pins a stamp, holds no level set and stalls no writer; reads, scans and collection lookups through it answer at that stamp, a drop leaves the runs of a dropped collection until the last snapshot goes, and a save writes a database of this engine with every collection as the snapshot reads it.
- With keys watched, committed transactions are strictly serializable in stamp order; with none watched they read one snapshot and the last writer wins; the description reports `ustore_isolation_snapshot_k`.
- A write that leaves a level due compacts it on the writing thread, and `ustore_database_compact` flushes every collection before merging what is due.

The engine mounts on files through the transport its executor names or the probe settles on: `io_uring` where the kernel allows it, Linux AIO where it refuses a ring, and the blocking floor elsewhere.
Only `ram` is refused, since the database the ABI names keeps its volumes in files.

## Compactions

- Flushing a collection's memory runs writes one L0 sorted run from key slots held in RAM.
- Compaction is a call rather than a background loop: `compact` takes two channels, merges the shallowest level that is over its bounds into the level below, and writes new sorted runs.
- A sorted level with nothing to merge beneath it descends one level whole, and compacting the bottom level appends a new level below it.
- A tree keeps two level sets and stores which of them is live, so a publication builds the next one beside the one readers hold and waits only on the readers of the set it retires.

Compaction merges sorted key slots and plans how their stored values migrate into the new sorted runs, and `compaction_kind_t` names the merge: overlapping or sorted runs, into an intermediate or the bottom level, where remove deltas are dropped.
The host and GPU compactors share one comparator, survival rule, sorted run split and placement, so their outputs match field by field.
The GPU compactor launches three kernels of the library's bundle per merge, ranking, packing and placing, links no CUDA runtime, CUB or Thrust, takes its devices, streams and buffers from `include/ustore/utilities/vram_nvidia_device.hpp`, and is chosen once a device is present and a merge exceeds `cuda_compaction_threshold_k`, 900,000 key slots.
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
- __`linux_spdk_k`__ — `linux_spdk_disk_t`, the user-space NVMe driver behind `USTORE_WITH_LINUX_SPDK`.
  Its probe reads `/proc/meminfo` and the driver binding first, because `rte_eal_init` can otherwise call `exit` on the whole process.
  The environment comes up once per process and is never torn down, and a forked child must not touch it, since DPDK is not fork-safe.
  Multi-process DPDK, with a primary and secondaries, is out of scope.
  Queue pairs belong to the disk, which holds a controller share for its whole life. An unmount releases the channels' queue pairs first, and the disk's `release` then frees the rest before it gives back its namespaces and its share, so no queue pair outlives its controller.
- __`memory_k`__ — `memory_disk_t`, volumes in memory for tests and benchmarks.
- __`automatic_k`__ — io_uring where the kernel allows it, Linux AIO where it refuses a ring, the blocking floor where it offers neither.
  What the mount settled on is reported back rather than inferred.

Above the transport, `nvme_volume.hpp` places runs across every data directory of a database, each on the medium with the most free bytes, and commits the manifest that names them.
A database is the manifest `<database>.ustore.json` plus one self-describing sorted run per `.ustore` file, which a mount takes back level by level; a run the manifest in force does not name is a crash leftover, freed at mount.
Every publication costs two flushes, however many runs it writes and consumes: the new runs and the manifest copy are flushed first, and the commit second.

- __Directories.__ `basic_directory_volume` keeps each run as `<database>-<run id>.ustore`, its own registered medium, and commits through `replace_file_durably`: it writes `<database>.ustore.json.tmp`, flushes it with `fdatasync`, or `F_FULLFSYNC` on macOS and `FlushFileBuffers` on Windows, renames it over the manifest, and syncs the directory. Consumed runs are unlinked once the manifest that stopped naming them is durable. The manifest's directory is data directory 0, and the manifest lists files by `path` and `medium`.
- __Raw media.__ `basic_slotted_volume` cuts block devices and SPDK namespaces into 1 GiB slots from a base address, as in `/dev/nvme0n1@64GiB`, and the manifest lists runs by `medium` and `offset`. Runs under 512 MiB are packed into a shared slot, and longer ones take a slot of their own.

On raw media every slot a database holds opens with a `slot_owner_block_t` in its first logical block:

- __Owner block.__ The `USTORESL` magic, the format version, the slot's data directory, and the database's UUID, minted at its first open. A block of all zeros is a free slot, and anything else is foreign and skipped without being identified, so two databases and other tools share one medium.
- __Manifest slot.__ The slot at data directory 0's base keeps two JSON regions, and its owner block also names the generation, offset and length of the copy in force. A commit writes the idle region, flushes every medium, then rewrites that one block and flushes again, so a crash before the second flush leaves the old generation in force.
- __Mount.__ One logical block is read per slot. Slots this database holds that the manifest does not name are zeroed and freed, zero blocks are freed lowest first, and a slot naming another data directory index refuses the mount as reordered media.
- __Release.__ A slot left empty has its owner block zeroed and its payload deallocated at the next publication, with a punched hole, `BLKDISCARD` or Dataset Management.

Every I/O buffer comes from one registered arena, because a range can be pinned only once — io_uring answers `EBUSY` on re-registration and a growing arena cannot be pinned at all.
In `nvme_channels.hpp`, the buffer pool carves that arena into fixed blocks, each I/O channel gives a queue pair its own pools and polling room, and a pipeline holds the ring at its depth by refilling as completions retire rather than submitting a batch and waiting for all of it.

Depth comes from queue pairs rather than threads.
A dispatch blocks until all its threads return and admits one caller at a time, and ForkUnion bans nested parallelism, so a batch fanned out inside a poll would both serialize two owners and fork a pool that is already forked.

## Graph Store

`include/ustore/graph/nvme_store.hpp` keeps a graph collection as one collection of the engine's database, keyed by vertex, beside the blobs stores and under the same ledger:

- A vertex's value is its half-edges, 48-byte records sorted by neighbor, edge and role, as `adjacency_fold.hpp` lays them out.
- An edge write appends one change delta per endpoint rather than rewriting either, so two transactions adding edges to one vertex both commit.
- Reads, memory merges, flushes and compactions fold the deltas into the value through the fold path every store shares, and a merge holding a change runs on the CPU.
- A removed vertex is a removal of its key, and its neighbors take a change removing the half-edges they filed of it.
- A removed range of vertices is one range tombstone, and only the neighbors outside the range take those changes.

Every store serves the modalities its fold folds, so a graph of variable-width values lands in this one, and a graph of any other width is refused, as no store keeps it.
`src/nvme/graph.hpp` serves the graph calls of the C ABI over it, batching the vertices of a run of tasks on one collection into one select.
A write watches what it depends on: a vertex it creates, a vertex a removal of edges changes, and the adjacency a vertex removal reads.

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
