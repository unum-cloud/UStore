# UStore Templates

`include/ustore/` holds the first-party classes and templates UStore's own engines are built from, header-only, for advanced users who build their own low-level stores.
The public SDKs stay in `c/ustore/*.h` and `cpp/ustore/*.hpp`, and `src/` keeps the glue of the C ABI and the explicit instantiations.
Every subfolder `ustore/<dir>/` opens `unum::ustore::<dir>`, and every header is spelled `<ustore/<dir>/...>` in the tree and once installed.

## Stability

These headers carry no stability promise.
Their names, layouts and signatures change with the engines that instantiate them, in any release, and only the C ABI of `c/ustore/*.h` is versioned.
Pin a commit, not a version, when building on them.

## Folders

- `utilities/`, in `unum::ustore::utilities`:
  - `types.hpp`: `status_t`, `status_code_t` and `expected`, UUIDs, keys and collection IDs with their orders, hashes and text, edges, value views, byte views and the serialization over them, owned text and integer rounding.
    `cpp/ustore/status.hpp` and `cpp/ustore/types.hpp` re-export it all into `unum::ustore`.
  - `ram_buffers.hpp`: the growing arena behind `ustore_arena_t` and every call's scratch, the `arena_allocator` SmashTable's containers take, the aligned and budgeted allocators of unbuffered I/O with the buffers they grow, and `growing_strings`, a column of strings exported as a tape.
  - `ram_device.hpp`: `topology_t`, `executor_t`, `lent_pool_t` and `pool_view_t` over ForkUnion's C ABI.
  - `vram_device.hpp`: the vendor-neutral face of the GPU residency, every vendor's driver, the device listing, and one executor spawned over any vendor.
  - `vram_nvidia_device.hpp`: CUDA devices, streams, events and buffers from the host.
  - `vram_amd_device.hpp`: AMD devices from the host, the HIP runtime opened at first use, the device listing, modules, events and the executor.
  - `vram_matching.hpp` and `vram_matching.cuh`: the device matching kernels of the library's bundles as one executor's device resolved them, and the kernels themselves.
  - `vram_scan.cuh`: the device scan steps every kernel compacts with, a block-wide scan and a chain of blocks.
  - `tapes.hpp` and `tapes.cuh`: the strings a tape is assembled from, the segments a device gathers them in, and the host twin of the device kernels.
  - `blocking_files.hpp`: whole-file mappings, paths joined into one buffer, directories created and removed, and shared libraries opened by name.
  - `algorithm.hpp`: searches over sorted batches, reservoir sampling over ordered iterators, and inserts and erases in trivially copyable buffers.
  - `folds.hpp`: `fold_concept`, which folds the base of a key with the deltas newer than it, and `last_write_fold_t`, where the newest delta wins.
  - `nvme_ring.hpp`: completion counters, disk and file identifiers, alignment, extents, and the submission and completion ring every NVMe transport exposes.
    It reaches no StringZilla, so CUDA translation units include it.
  - `nvme_disks.hpp`: the ring machinery every disk shares, the volume in memory, and the blocking floor of positioned reads and writes.
  - `nvme_linux_uring_disk.hpp`, `nvme_linux_aio_disk.hpp`, `nvme_linux_cufile_disk.hpp` and `nvme_linux_spdk_disk.hpp`: the io_uring, Linux AIO, cuFile and SPDK transports.
  - `nvme_channels.hpp`: the pinned buffer pool, the channels checking a queue pair out with its blocks, and the pipelines holding a ring at depth.
  - `nvme_catalog.hpp`: the catalog of one volume, its checksum, blob kinds and names, on-disk layouts and the index resolving a name to an extent, and `schema_copies`, the two schema copies and the user blob a database keeps beside its runs.
  - `nvme_volume.hpp`: the extent allocator, volumes and the router placing blobs across them.
  - `nvme_mounts.hpp`: the mount of one database's storage, the transport it probes, the disk erased over the file transports, the volumes it opens and the shared resources over it.
- `blobs/`, in `unum::ustore::blobs`:
  - `ram_store.hpp`: `ordered_store`, the multi-version SmashTable store every RAM collection is, `values_by_key_store` of a blobs collection, the owned texts and trees of documents and pages, and the status translation every store shares.
  - `nvme_key_slot.hpp`: identifiers and value layouts of the LSM trees, stored keys and values, key slots and the sorted run index over them.
  - `nvme_sorted_run.hpp`: one immutable sorted run on a volume.
  - `nvme_compaction.hpp`: the rules of a merge, the host compactor and the sections readers a device compactor reads through.
  - `vram_nvidia_compaction.hpp` and `vram_nvidia_compaction.cuh`: the host face of the device compactor, and its kernels, which compile into the library's bundles.
  - `nvme_tree.hpp`: configurations and schemas, LSM levels, the merge of one level into the next, and the level set readers see.
  - `nvme_published_runs.hpp`: `commit_ledger`, `stamp_pin` and `pinned_snapshot`, the stamps and the sequencer every NVMe store commits through, `maintenance_claim`, the claim a database's maintenance runs under, and `published_runs`, the tree, memory runs and published view every NVMe store owns, with the commits, merges, flushes, compactions and truncations that move its runs.
  - `nvme_store.hpp`: `nvme_store`, the store of one collection, memory runs and a value cache in front of a tree, with its snapshots and `nvme_store_cursor`.
  - `nvme_transactions.hpp`: `transaction_deltas` and the read sets one transaction carries, `transaction_registry`, which orders and conflict-checks the transactions of one store, and `transaction_core`, the lifecycle, stamps and watches every transaction is made of.
  - `nvme_database.hpp`: databases of named collections, with their snapshots and optimistic transactions over the shared registry and core.
  - `nvme_single_collection_database.hpp`: single-collection databases, with their snapshots and transactions over the same registry and core.
- `graph/`, in `unum::ustore::graph`:
  - `half_edges.hpp`: `half_edge_t`, the entry a graph collection files every vertex and both ends of every edge under, with its order and hash.
  - `ram_store.hpp`: `half_edges_store`, the RAM store of a graph collection, exporting degrees and edges from it, and staging edge and vertex inserts and removals.
  - `vertex_values.hpp`: the dense codec keeping every vertex as one value of its degrees and sorted neighborships.
  - `adjacency_fold.hpp`: labeled edges and their half-edges, the 48-byte half-edge records a vertex value and its change deltas share, `adjacency_fold_t`, and the reads merging both.
  - `vram_nvidia_adjacency_fold.hpp` and `vram_nvidia_adjacency_fold.cuh`: the host face of the device fold `vram_nvidia_adjacency_fold_t`, and the kernels and device composition behind it, which compile into the library's bundles.
  - `nvme_store.hpp`: `nvme_store`, the NVMe store of a graph collection, one store keyed by vertex and folded by `adjacency_fold_t`.
- `docs/`, in `unum::ustore::docs`:
  - `ram_store.hpp`: `documents_by_key_store`, the RAM store of a documents collection.
  - `json_paths.hpp`: fields translated into chains of JSONPath prefixes and resolved for a batch of documents in one StringZilla subdocuments pass, into the same values yyjson's lookup finds, or handed back to yyjson where the pass cannot read a batch as yyjson does.
  - `json_values.hpp`: JSON documents parsed with yyjson into an arena, and the conversions between their values and the scalars, strings and UUIDs of a gathered column.
  - `json_patch.hpp`: RFC 6902 patches, RFC 7386 merges, and the field-level inserts, updates and upserts of a document.
  - `json_fold.hpp`: `json_fold_t`, which replays deltas of one modification of one field each as `ustore_docs_write` reads, modifies and writes back today, and composes them without a base where that is sound.
- `pages/`, in `unum::ustore::pages`:
  - `ram_store.hpp`: `pages_by_key_store`, the RAM store of a pages collection.
  - `element_tree.hpp`: a page as an ordered tree of keyed elements, its dense serialized form, and a view addressing subtrees in place.
  - `page_edits.hpp`: the edits of a page and their serialized chains.
  - `page_fold.hpp`: `page_fold_t`, which applies chains of edits in commit order into a page's current shape, and composes them without a base where that is sound.
  - `markup.hpp`: XML and text import and render, and XPath reads through StringZilla's subdocuments engine.
- `strings/`, in `unum::ustore::strings`:
  - `string_keys.hpp`: `string_key_t`, the owned key of a strings collection and of a collection's name, with its order and hash, and the 16-byte heads that search strings: `head_of` and `tail_of`.
  - `ram_store.hpp`: `keys_by_string_store`, the RAM store of a strings collection and of the names system collection.
  - `nvme_memory_run.hpp`: the memory runs of strings, keyed by whole strings on a tape and searched by their heads, and the cursor over a view of them.
  - `nvme_store.hpp`: `nvme_store`, the NVMe store of a strings collection, whose runs keep each string's head in a cell and the bytes past it in the run's tails, and `nvme_transaction`, the strings one database transaction writes and watches, joined to it as a share.

## Building

Every template that owns a shared word or waits on one takes the atomic reference and the waiter as template parameters, wrapping the waiter in SmashTable's parking policy itself, and names no CPU class, instruction or ForkUnion `preferred_*` alias; `src/` picks both per CPU class.
CMake exports the headers as the `ustore::header` interface target, which carries the include paths of `c/` and `include/` and links the header targets of ForkUnion, SmashTable and StringZilla, plus the include paths of yyjson and fmt, in the build tree.
An install copies `include/ustore/` beside `c/ustore/` and `cpp/ustore/`, but none of the siblings' headers, so a consumer of the installed package brings each sibling's own package, as the C++ SDK already asks.
Headers that call ForkUnion's C ABI, such as `ram_buffers.hpp` and `ram_device.hpp`, also need `forkunion::static` or `ustore::static` at link time.
`docs/json_paths.hpp` calls StringZilla's dispatched kernels, which its header-only build answers with `sz_missing_library_k`, so it resolves nothing without `stringzilla::static` or `ustore::static`, and its callers fall back to yyjson.
