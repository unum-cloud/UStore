# UStore File Format

A UStore database on NVMe is a checkpoint that is always live: one JSON manifest names the database and every sorted run, and each run is a self-describing binary file.
The same bytes live on a filesystem as files, on a raw drive or SPDK namespace inside 1 GB slots, and on object storage as the synced directory.
Every integer is little-endian, and every section of a run starts on a 4096-byte boundary, so 512-byte and 4096-byte drive formats both read it unbuffered.

## Sorted Runs

A run is one immutable sorted run of one collection's LSM tree, at most 1 GiB less 4 KiB.
On a filesystem it is the file `<database>-<run id>.ustore`, where the run ID is the UUID its header carries as `content_id`.
On raw media it is an extent inside a slot, which the manifest locates by offset.

```c
struct sorted_run {                       // every section starts on a 4096-byte boundary
    sorted_run_header_t header;           // 120 bytes
    key_slot cells[n];                    // sorted order, inline values only
    splus_tree keys;                      // saved immutable_splus_tree over the n keys
    u32 value_offsets[n + 1];             // variable-width values only; [0] = 0, [n] = stored_values_bytes
    byte_t values[stored_values_bytes];   // sorted order, fixed-width ones found by stride
    byte_t stamps[stamps_bytes];          // trailer: the saved stamps column
    byte_t kinds[kinds_bytes];            // the saved kinds column right after it
    range_tombstone ranges[t];            // t = range_tombstones_count; start and end keys, ascending by start
    u64 range_stamps[t];                  // the raw stamp of each range tombstone
};

struct strings_run {                      // every section starts on a 4096-byte boundary
    sorted_run_header_t header;           // 120 bytes
    splus_tree heads;                     // saved immutable_splus_tree<key128_t> over the n heads
    uuid_t values[n];                     // Arrow fixed_size_binary(16), in sorted order
    u32 key_offsets[n + 1];               // Arrow string column of the whole keys
    char keys[key_tape_bytes];
    byte_t stamps[stamps_bytes];          // trailer: the saved stamps column
    byte_t kinds[kinds_bytes];            // the saved kinds column right after it
    head_layers_header_t layers_header;   // the layers, layers_bytes in all
    head_layer_t layers[layers_count];    // by first position, then by depth
    splus_tree layer_heads[layers_count];
    u64 range_stamps[t];                  // t = range_tombstones_count, ascending by start
    u32 range_offsets[2 * t + 1];         // tombstone i spans [2i, 2i + 1) and [2i + 1, 2i + 2) of the tape
    char range_tape[];                    // start and end strings, an empty end reaching past the last string
};
```

The header carries everything a reader needs to locate every other section, so a run lifted out of its database still describes itself.

| Offset | Bytes | Field                    | Meaning                                                 |
| -----: | ----: | :----------------------- | :------------------------------------------------------ |
|      0 |     8 | `magic`                  | `USTORERN`                                              |
|      8 |     4 | `format_version`         | 1                                                       |
|     12 |     4 | `key_slot_bytes`         | bytes of one cell, refusing a run of another key shape  |
|     16 |     8 | `key_slots_count`        | key slots the run holds                                 |
|     24 |     8 | `stored_values_bytes`    | bytes of the values section before its padding          |
|     32 |     4 | `value_max_size`         | largest value the writing collection admits             |
|     36 |     2 | `branching`              | keys in one row of the saved tree                       |
|     38 |     2 | `value_layout`           | how values are stored: inline, by stride or by offsets  |
|     40 |    16 | `content_id`             | the run ID, which also names the file                   |
|     56 |     8 | `stamps_bytes`           | bytes of the saved stamps column                        |
|     64 |     8 | `stamps_floor`           | the stamp every stamp of the run was raised to at least |
|     72 |     8 | `newest_stamp`           | the newest stamp any cell carries                       |
|     80 |     8 | `kinds_bytes`            | bytes of the saved kinds column                         |
|     88 |     4 | `key_tape_bytes`         | bytes of every key of a run of strings whole            |
|     92 |     4 | `layers_bytes`           | bytes of the layers section of a run of strings         |
|     96 |    16 | `collection`             | the UUID of the collection that wrote the run           |
|    112 |     4 | `range_tombstones_count` | range tombstones at the end of the run                  |
|    116 |     4 | `range_tombstones_bytes` | bytes of that section before its padding                |

A cell holds a fixed-width key and nothing of its value, which the cell's sorted position locates.
A lookup descends the saved tree of the keys, whose rank is the cell's position.
A cell that is its key alone, for fixed-width and variable-width values, is not saved, as the tree's leaf rows hold every key; a reader or a device compactor takes keys from those rows, 16-byte keys stored as a column of high words and a column of low words per row.
A run of strings files every key under its 16-byte head, its first 15 bytes and its length, and keeps the keys whole on the key tape.
A tie of equal heads wider than one row of the tree gets a layer over the next 15 bytes of its keys, nested as deep as they keep tying, so a lookup compares at most one row of whole keys.
Each cell's kind tells a removal, an empty value and a fold's change apart, so no length or offset carries a sentinel.

A range tombstone is what one range removal leaves: every key from its start up to its exclusive end, hidden from a reader at or past its stamp for every version older than it.
Its stamp stays raw, never raised to the floor, and a version it hides is older by position: one of a deeper level, of an older run of level zero, or of its own run stamped below it.
The run's key bounds cover its tombstones, so a level's runs stay disjoint, and a run may hold tombstones and no key slot at all.
A merge drops the versions a tombstone hides from every reader, cuts the tombstone at the bounds of the runs it writes, and drops it once it reaches the bottom with no reader below it.

## Manifest

The manifest is `<database>.ustore.json`, written last as the commit record of every flush and every merge step.
A run the manifest does not name is a crash leftover, removed at the next mount.

```json
{"__metadata__": {"format": "ustore", "version": "1"},
 "database": "molecules", "id": "0193f1c0-…", "generation": 42, "durable_stamp": 918273,
 "collections": [{"id": "…", "name": "smiles", "store": 0, "value_size": 0, "value_max_size": 1048576,
                  "level_enlarge_factor": 4, "cache_limit": 0,
                  "memory_runs_max_bytes": 16777216, "memory_runs_max_deltas": 65536}],
 "files": [{"path": "molecules-0193f1c2-….ustore", "bytes": 3211264, "collection": 0, "level": 0, "stamp": 0,
            "sha256": "9f86d081…"},
           {"path": "molecules-0193f1c3-….ustore", "bytes": 1832}]}
```

The top-level keys:

- `__metadata__`: the format name and version, both strings.
- `database`: the name the database is opened by, which prefixes every run file.
- `id`: the database's UUID, minted at its first open and carried by every slot it holds.
- `generation`: how many manifests the database has published, this one included.
- `durable_stamp`: every commit at or below this stamp is wholly inside the files named, and a mount drops every one above.
- `collections`: every collection with what a mount needs to open it again: its UUID, name, store tag, value widths, level growth factor, cache and memory-run bounds.
- `files`: every run, plus the caller's user data as the one entry without a collection.

A file entry names its bytes in one of two ways, and one manifest never mixes them:

- `path`, on a filesystem: the run file's name, relative to the data directory its `medium` names, the manifest's own directory when `medium` is absent.
- `medium` and `offset`, on raw media: the data directory's index and the run's first byte inside that medium.

Either way it carries `bytes`, and a run also carries its `collection` index into `collections`, its `level`, zero being the level flushes land on, and the `stamp` an ingest assigned a run written at stamp zero.
A saved snapshot's manifest also gives every file a `sha256`, 64 lowercase hex digits over its bytes, hashed while the save copies them.
A mount hashes every file whose entry names one before anything reads it, and refuses a mismatch with `ustore_status_malformed_k`.
The manifests the database publishes after that mount name no digests, so only the first open of a restored database hashes its runs; every later save hashes them again while copying.

A publication costs two flushes, however many runs it writes and consumes.
On a filesystem the new run files are flushed, then `<database>.ustore.json.tmp` is written and flushed, renamed over the manifest, and the directory is synced.
Runs the new manifest stopped naming are unlinked only after that.
On raw media the order is the one under Manifest Slot.

## Raw Slots

A raw block device or SPDK namespace holds no filesystem, so a database claims it slot by slot.
Slot `i` of a data directory spans the 1 GiB from `base + i × 1 GiB`, where the base is the address's `@` offset, as in `/dev/nvme0n1@64GiB`.
The first logical block of every slot is its owner block, and runs start 4 KiB into the slot, past it on either block size.

```c
struct slot_owner_block_t {      // first logical block of every slot a database holds
    char   magic[8];             //  0 "USTORESL"
    u32    format_version;       //  8 1
    u32    medium;               // 12 the slot's data directory, so reordered drives are refused
    uuid_t database;             // 16 the database's `id`
    u64    manifest_generation;  // 32 manifest slot only: the copy in force
    u64    manifest_offset;      // 40 manifest slot only: where that copy begins
    u64    manifest_bytes;       // 48 manifest slot only: its length, also ending at its first zero byte
};                               // 56 bytes
```

An owner block of all zeros means the slot is free.
A block holding `USTORESL` with this database's ID means the slot is held.
Anything else is foreign, and a mount skips it without trying to identify it, so two databases and other software can share a drive.
A mount reads one block per slot, frees every held slot the manifest does not name, and refuses a drive whose blocks name another position among the data directories.

Runs of 512 MiB and above take a slot of their own.
Shorter runs are appended into one open packed slot, which is released once none of its runs is live.
A released slot's owner block is zeroed, and the slot is then handed back to the drive: `BLKDISCARD` on a block device, a Dataset Management deallocation on an SPDK namespace.

## Manifest Slot

The slot at data directory 0's base holds the manifest, so an address like `/dev/nvme0n1@64GiB` names the database and finding its manifest takes one read.
Past the owner block it keeps two JSON regions of equal size, and the owner block points at the one in force.

A publication runs in two steps:

1. Write the owner blocks of every slot taken or freed, the new runs, and the JSON into the idle region, then flush every medium.
2. Rewrite the manifest slot's owner block with the next generation, the idle region's offset and the JSON's length, then flush.

The owner block is shorter than one logical block and NVMe writes at least one block atomically, so the second step either lands whole or leaves the previous generation in force.
A crash between the two steps loses only the copy that was never pointed at.

## Recovery

Every run names its collection and its own ID, so a database can be rebuilt without its manifest.
`ustore_cli recover` scans the data directories' `.ustore` files, or, on raw media, every run of every slot the database holds, walking a packed slot's runs back to back from its payload offset, puts each run on level 0 of the collection its header names, and writes a fresh manifest.
Per-key stamps then pick the versions, so overlapping runs from an interrupted merge resolve to the newest write.
The collections themselves come from the `_collections` system collection, UUID `ffffffff-ffff-ffff-ffff-fffffffffffd`.
Under every collection's UUID it keeps a packed 48-byte record, the store tag, value sizes, level factor, cache and memory-run bounds, followed by the collection's name.
Recovery reads the newest version of every key across `_collections`' runs, skips removals, and records each listed collection with those fields.
`_collections` and `_names` (`ffffffff-ffff-ffff-ffff-fffffffffffe`) take the bounds every open creates them with.
A run whose collection `_collections` lists as dropped, or does not list at all, is left out.
`ustore_cli manifest` prints the manifest a database opens by: a `.ustore.json` path, a directory with `--database`, or the copy a raw address's manifest slot points at.
