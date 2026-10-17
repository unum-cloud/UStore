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
  - `types.hpp`: `status_t`, `status_code_t` and `expected`, UUIDs, keys and collection IDs with their orders, hashes and text, edges, value views, integer rounding, and `waiter_t`, the stall a spinning thread takes.
    `cpp/ustore/status.hpp` and `cpp/ustore/types.hpp` re-export it all into `unum::ustore`.
  - `ram_buffers.hpp`: the growing arena behind `ustore_arena_t` and every call's scratch, the `arena_allocator` SmashTable's containers take, and `growing_strings`, a column of strings exported as a tape.
  - `ram_device.hpp`: `topology_t`, `executor_t`, `lent_pool_t` and `pool_view_t` over ForkUnion's C ABI.
  - `vram_nvidia_device.hpp`: CUDA devices, streams, events and buffers from the host.
  - `blocking_files.hpp`: whole-file mappings and directory queries.
  - `algorithm.hpp`: searches over sorted batches, reservoir sampling over ordered iterators, and inserts and erases in trivially copyable buffers.
  - `folds.hpp`: `fold_concept`, which folds the base of a key with the deltas newer than it, and `last_write_fold`, where the newest record wins.
  - `nvme_ring.hpp`: aligned and budgeted allocators, completion counters, 128-bit identifiers, serialization, extents, and the submission and completion ring every NVMe transport exposes.
    It reaches no StringZilla, so CUDA translation units include it.
  - `nvme_disks.hpp`: the ring machinery every disk shares, the volume in memory, and the blocking floor of positioned reads and writes.
  - `nvme_linux_uring_disk.hpp`, `nvme_linux_aio_disk.hpp` and `nvme_linux_spdk_disk.hpp`: the io_uring, Linux AIO and SPDK transports.
  - `nvme_channels.hpp`: the pinned buffer pool, the channels checking a queue pair out with its blocks, and the pipelines holding a ring at depth.
  - `nvme_volume.hpp`: owned text, catalogs, volumes and the router placing blobs across them, and the mount of one database's storage with its shared resources.
- `blobs/`, in `unum::ustore::blobs`:
  - `ram_store.hpp`: the multi-version SmashTable stores of the RAM engine, one per collection, the catalog and the names over them, and `collections_transaction`, which publishes any number of them under one stamp.
  - `nvme_key_slot.hpp`: identifiers and value layouts of the LSM trees, stored keys and values, key slots and the sorted run index over them.
  - `nvme_sorted_run.hpp`: one immutable sorted run on a volume.
  - `nvme_compaction.hpp` and `nvme_compaction.cuh`: the host compactor and the device one, whose kernels only nvcc compiles.
  - `nvme_tree.hpp`: configurations and schemas, LSM levels, the merge of one level into the next, and the level set readers see.
  - `nvme_write_buffer.hpp`: the in-memory write buffer of stored values.
  - `nvme_collection.hpp`: one collection, a write buffer and a value cache in front of a tree, with its snapshots and scanner.
  - `nvme_database.hpp`: databases of named collections and single-collection databases, their snapshots, optimistic transactions, registries and read sets.
- `graph/`, in `unum::ustore::graph`:
  - `half_edges.hpp`: `half_edge_t`, the entry a graph collection files every vertex and both ends of every edge under, with its order and hash.
  - `ram_graph.hpp`: exporting degrees and edges from a store of half-edges, and staging edge and vertex inserts and removals.
  - `vertex_values.hpp`: the dense codec keeping every vertex as one value of its degrees and sorted neighborships.
  - `edge_block.hpp`: vertices, labeled edges and their half-edges under 16-byte IDs, and the edge blocks of 16 to 512 sorted half-edges.
  - `vertex_registry.hpp`: `vertex_registry_t`, the directory of a vertex's edge blocks.
  - `nvme_graph.hpp`: the graph over two NVMe single-collection databases, and the transaction spanning both.
- `docs/`, in `unum::ustore::docs`:
  - `json_paths.hpp`: fields translated into chains of JSONPath prefixes and resolved for a batch of documents in one StringZilla subdocuments pass, into the same values yyjson's lookup finds, or handed back to yyjson where the pass cannot read a batch as yyjson does.
  - `json_values.hpp`: JSON documents parsed with yyjson into an arena, and the conversions between their values and the scalars, strings and UUIDs of a gathered column.
  - `json_patch.hpp`: RFC 6902 patches, RFC 7386 merges, and the field-level inserts, updates and upserts of a document.
  - `json_fold.hpp`: `json_fold`, which replays deltas of one modification of one field each as `ustore_docs_write` reads, modifies and writes back today, and composes them without a base where that is sound.
- `pages/`, in `unum::ustore::pages`:
  - `element_tree.hpp`: a page as an ordered tree of keyed elements, its dense serialized form, and a view addressing subtrees in place.
  - `page_edits.hpp`: the edits of a page and their serialized chains.
  - `page_fold.hpp`: `page_fold`, which applies chains of edits in commit order into a page's current shape, and composes them without a base where that is sound.
  - `markup.hpp`: XML and text import and render, and XPath reads through StringZilla's subdocuments engine.
- `strings/`, in `unum::ustore::strings`:
  - `string_keys.hpp`: `string_key_t`, the owned key of a strings collection and of a collection's name, with its order and hash.

## Building

CMake exports the headers as the `ustore::header` interface target, which carries the include paths of `c/` and `include/` and links the header targets of ForkUnion, SmashTable and StringZilla, plus the include paths of yyjson and fmt, in the build tree.
An install copies `include/ustore/` beside `c/ustore/` and `cpp/ustore/`, but none of the siblings' headers, so a consumer of the installed package brings each sibling's own package, as the C++ SDK already asks.
Headers that call ForkUnion's C ABI, such as `ram_buffers.hpp` and `ram_device.hpp`, also need `forkunion::static` or `ustore::static` at link time.
`docs/json_paths.hpp` calls StringZilla's dispatched kernels, which its header-only build answers with `sz_missing_library_k`, so it resolves nothing without `stringzilla::static` or `ustore::static`, and its callers fall back to yyjson.
