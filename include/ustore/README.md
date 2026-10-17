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
- `blobs/`, in `unum::ustore::blobs`:
  - `ram_store.hpp`: the multi-version SmashTable stores of the RAM engine, one per collection, the catalog and the names over them, and `collections_transaction`, which publishes any number of them under one stamp.
- `graph/`, in `unum::ustore::graph`:
  - `half_edges.hpp`: `half_edge_t`, the entry a graph collection files every vertex and both ends of every edge under, with its order and hash.
  - `ram_graph.hpp`: exporting degrees and edges from a store of half-edges, and staging edge and vertex inserts and removals.
  - `vertex_values.hpp`: the dense codec keeping every vertex as one value of its degrees and sorted neighborships.
- `docs/`, in `unum::ustore::docs`:
  - `json_values.hpp`: JSON documents parsed with yyjson into an arena, and the conversions between their values and the scalars, strings and UUIDs of a gathered column.
  - `json_patch.hpp`: RFC 6902 patches, RFC 7386 merges, and the field-level inserts, updates and upserts of a document.
- `strings/`, in `unum::ustore::strings`:
  - `string_keys.hpp`: `string_key_t`, the owned key of a strings collection and of a collection's name, with its order and hash.

## Building

CMake exports the headers as the `ustore::header` interface target, which carries the include paths of `c/` and `include/` and links the header targets of ForkUnion, SmashTable and StringZilla, plus the include paths of yyjson and fmt, in the build tree.
An install copies `include/ustore/` beside `c/ustore/` and `cpp/ustore/`, but none of the siblings' headers, so a consumer of the installed package brings each sibling's own package, as the C++ SDK already asks.
Headers that call ForkUnion's C ABI, such as `ram_buffers.hpp` and `ram_device.hpp`, also need `forkunion::static` or `ustore::static` at link time.
