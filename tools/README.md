# Tools

The tools run on the embedded C ABI of `c/ustore/db.h` and never see which engine is behind it, so the same commands drive the in-memory engine, RocksDB and LevelDB, and NVMe once its sources build, whichever `--engine` names.

## Command-Line Interface

`cli.cpp` builds into one executable, `ustore_cli`, when CMake is configured with `-DUSTORE_BUILD_TOOLS=1`.
It opens the database in the `--directory` it is given, runs one command and exits, with status 1 when the flags do not parse or the command fails.
Status lines go to standard error, so an export to `--output -` leaves standard output carrying only Arrow bytes.

```sh
cmake --preset release -DUSTORE_BUILD_TOOLS=1 -DUSTORE_WITH_ROCKSDB=1
cmake --build --preset release --target ustore_cli
build_release/build/bin/ustore_cli --engine rocksdb --directory /dev/shm/ustore \
    collection import --input data/twitter/tweets.ndjson --id-field id --memory-limit 1GB --collection tweets
```

Arrow streams and files carry every modality, and `-` reads standard input or writes standard output, so object stores are reached through their own tools:

```sh
aws s3 cp s3://bucket/posters.arrows - | ustore_cli --engine nvme --directory /mnt/db \
    collection import --input - --collection posters --modality blobs
ustore_cli --engine nvme --directory /mnt/db collection export --collection posters --output - | aws s3 cp - s3://bucket/posters.arrows
rclone cat r2:bucket/similar.arrow | ustore_cli --directory /mnt/db collection import --input - --collection similar
ustore_cli --directory /mnt/db collection export --collection movies --output movies.arrow --alignment 4096
```

| Command                 | Action                                                                                  |
| :---------------------- | :-------------------------------------------------------------------------------------- |
| `collection create`     | Creates the collection named by `--collection`, of `--modality` or documents            |
| `collection drop`       | Drops the collection named by `--collection`                                            |
| `collection list`       | Prints the ID as canonical UUID text, the name and the modality of every collection     |
| `collection import`     | Imports Arrow `--input`, or documents keyed by `--id-field` from NDJSON or CSV          |
| `collection export`     | Exports into Arrow `--output`, or documents into NDJSON or CSV                          |
| `snapshot save <path>`  | Takes a snapshot of the head and saves it to a path as a new database                   |
| `snapshot export <dir>` | Writes every named collection of one snapshot into an empty directory as `<name>.arrow` |
| `snapshot import <dir>` | Recreates every collection of an exported directory, on any engine                      |

| Flag                    | Meaning                                                               |
| :---------------------- | :-------------------------------------------------------------------- |
| `--directory <path>`    | Database directory; required                                          |
| `--collection <name>`   | Collection a command acts on; required                                |
| `--input <file>`        | File a collection import reads, or `-` for standard input             |
| `--output <file>`       | File a collection export writes, or `-` for standard output           |
| `--id-field <field>`    | Field holding the keys of imported documents, integers or UUID text   |
| `--memory-limit <size>` | Bytes an NDJSON or CSV import or export buffers per batch, like `1GB` |
| `--modality <name>`     | What a created collection holds: blobs, docs, graph, strings, pages   |
| `--format <name>`       | `automatic`, `ndjson`, `csv`, `arrows` for streams, `arrow` for files |
| `--ingest <order>`      | Lands an Arrow import as one commit of `sorted` or `unsorted` keys    |
| `--alignment <bytes>`   | Where an Arrow export starts every buffer: 64 or 4096                 |
| `--batch-rows <count>`  | Most rows per batch of an Arrow export                                |

The file extension selects the format unless `--format` names one: `.arrows` is an Arrow stream, `.arrow` and `.feather` Arrow files, and `.ndjson` and `.csv` documents.
Standard input is an Arrow file when it starts with the file's magic, and a stream otherwise; standard output is a stream unless `--format arrow` asks for a file.
An Arrow import creates a missing collection, holding the `--modality` given, else the schema's `ustore.modality`, else what its columns spell.
Without `--ingest` it writes a batch at a time through the writes of the modality; with it, every batch is appended to one ingest of `c/ustore/db.h`, which lands the stream as one commit, an engine that ingests nothing refuses as unsupported, and a refused ingest drops the collection it created.
`--help` prints the commands and the flags.

`snapshot save` writes the engine's own image, which keeps internal forms such as half-edges and page trees but loads only on the engine that wrote it.
`snapshot export` is the portable form: it takes one snapshot, so every collection reads at the same stamp, and writes one Arrow file per named collection, whose schema metadata carries `ustore.modality` and `ustore.collection`.
A name that cannot be a file name, or clashes with one already written, takes the collection's UUID as its file name instead, while the schema keeps the name, and unnamed collections are skipped.
It refuses a directory that is not empty, so an export never mixes with an older one.
`snapshot import` creates each collection under the name its schema carries and lands its file as one sorted ingest where the engine ingests, since exports write keys in order, and in batches otherwise.
Both take any path, so a bucket mounted through `mountpoint-s3` works as a directory:

```sh
ustore_cli --engine ram --directory /dev/shm/db snapshot export /mnt/s3/backups/2026-10-29
ustore_cli --engine nvme --directory /mnt/db snapshot import /mnt/s3/backups/2026-10-29
```

## Dataset Imports and Exports

`dataset.h` declares six C calls, each taking one argument struct in the style of the C ABI.

| Call                    | Action                                                                                     |
| :---------------------- | :----------------------------------------------------------------------------------------- |
| `ustore_dataset_import` | An Arrow stream or file of any modality lands in a named collection, created when missing  |
| `ustore_dataset_export` | A collection of any modality becomes an Arrow stream or file, its buffers written in place |
| `ustore_docs_import`    | Every row becomes a document keyed by `id_field`, optionally projected to `fields`         |
| `ustore_docs_export`    | Every document becomes a `{"_id":key,"doc":document}` line or an `_id,doc` row             |
| `ustore_graph_import`   | Every row becomes an edge between its source and target fields, with an optional edge      |
| `ustore_graph_export`   | Every edge of a graph becomes a line or a row under the column names given                 |

The Arrow calls read and write through the header-only codec of `tools/arrow_ipc.hpp`, with no Arrow library, and move columns in and out of the views of the C ABI through the converters and exporters of `c/ustore/arrow.h`.
An import maps a file whole and reads standard input, `-`, or anything that does not map, in exactly the byte counts each message names, so pipes work.
An export hands every buffer of a batch to `writev` in place, padded so each starts at a multiple of `alignment` at its absolute position, and never seeks, so standard output takes files too.
Keys are `fixed_size_binary(16)` tagged `arrow.uuid`, byte strings are `binary`, and the schema metadata names the modality: blobs and docs as `key` and `value`, pages the same with `ustore.page_encoding=xml` on the value, strings as a binary `key` and a uuid `value`, and graphs as `source`, `target` and `edge`.

The `.ndjson` or `.csv` extension of `path` selects the format of the other four calls: NDJSON is parsed and written with yyjson, and CSV with StringZilla.
Paths and the ID field are `ustore_bytes_view_t`, fields are one `ustore_strings_view_t` of `fields_count` plain names or JSON pointers, and `max_batch_size` bounds the bytes buffered before each batched write.
Imports take the `durability` of their writes, and exports the `view` they read through, where a `NULL` view reads the head.
Exports also take an `arena` made through `ustore_arena_init`, which every read rewinds, while a `NULL` arena has the export make a one-off arena and free it once the export is final.
A key in a file is a non-negative integer, read as the key of that integer, or a UUID in canonical text, and an export writes every key back the same way: as its integer when its first eight bytes are zero, and as quoted UUID text otherwise.

```c
ustore_outcome_t outcome = {0};
char const path[] = "data/twitter/tweets.ndjson";
char const fields[] = "idtext";
uint32_t const fields_offsets[3] = {0, 2, 6};
ustore_docs_import_t import = {
    .database = database,
    .outcome = &outcome,
    .collection = collection,
    .path = {(uint8_t const*)path, sizeof(path) - 1},
    .max_batch_size = 64 * 1024 * 1024,
    .fields_count = 2,
    .fields = {.layout = ustore_strings_u32_tape_k, .data.u32_tape = {(uint8_t const*)fields, fields_offsets}},
    .id_field = {(uint8_t const*)"id", 2},
};
ustore_docs_import(&import);
```
