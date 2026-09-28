# Tools

The tools run on the embedded C ABI of `include/ustore/db.h` and never see which engine is behind it, so the same commands drive the in-memory engine, RocksDB, LevelDB and NVMe, whichever `--engine` names.

## Command-Line Interface

`cli.cpp` builds into one executable, `ustore_cli`, when CMake is configured with `-DUSTORE_BUILD_TOOLS=1`.
It opens the database in the `--directory` it is given, runs one command and exits, with status 1 when the flags do not parse or the command fails.

```sh
cmake --preset release -DUSTORE_BUILD_TOOLS=1 -DUSTORE_WITH_ROCKSDB=1
cmake --build --preset release --target ustore_cli
build_release/build/bin/ustore_cli --engine rocksdb --directory /dev/shm/ustore \
    collection import --input data/twitter/tweets.ndjson --id-field id --memory-limit 1GiB --collection tweets
```

| Command                  | Action                                                                              |
| :----------------------- | :---------------------------------------------------------------------------------- |
| `collection create`      | Creates the collection named by `--collection`                                      |
| `collection drop`        | Drops the collection named by `--collection`                                        |
| `collection list`        | Prints the ID as canonical UUID text, the name and the modality of every collection |
| `collection import`      | Imports `--input` as documents keyed by `--id-field`, `--memory-limit` per batch    |
| `collection export`      | Exports documents into `--output`, `--memory-limit` per batch                       |
| `snapshot export <path>` | Takes a snapshot of the head and writes it to a path                                |

| Flag                    | Meaning                                                                |
| :---------------------- | :--------------------------------------------------------------------- |
| `--directory <path>`    | Database directory; required                                           |
| `--collection <name>`   | Collection a command acts on; the main collection when omitted         |
| `--input <file>`        | File a collection import reads                                         |
| `--output <file>`       | File a collection export writes                                        |
| `--id-field <field>`    | Field holding the keys of imported documents, integers or UUID text    |
| `--memory-limit <size>` | Bytes an import or export buffers per batch, such as `64MiB` or `1GiB` |

The file extension, `.ndjson` or `.csv`, selects the format, and `--help` prints the commands and the flags.

## Dataset Imports and Exports

`dataset.h` declares four C calls, each taking one argument struct in the style of the C ABI.

| Call                  | Action                                                                                       |
| :-------------------- | :------------------------------------------------------------------------------------------- |
| `ustore_docs_import`  | Every row of a file becomes a document keyed by `id_field`, optionally projected to `fields` |
| `ustore_docs_export`  | Every document of a collection becomes a `{"_id":key,"doc":document}` line or `_id,doc` row  |
| `ustore_graph_import` | Every row becomes an edge between its source and target fields, with an optional edge field  |
| `ustore_graph_export` | Every edge of a graph collection becomes a line or a row under the column names given        |

The `.ndjson` or `.csv` extension of `path` selects the format: NDJSON is parsed and written with yyjson, and CSV with StringZilla.
Fields are plain names or JSON pointers, and `max_batch_size` bounds the bytes buffered before each batched write.
A key in a file is a non-negative integer, read as the key of that integer, or a UUID in canonical text, and an export writes every key back the same way: as its integer when its first eight bytes are zero, and as quoted UUID text otherwise.

```c
ustore_error_t error = {0};
ustore_str_view_t const fields[] = {"id", "text"};
ustore_docs_import_t import = {
    .db = db,
    .error = &error,
    .collection = collection,
    .path = "data/twitter/tweets.ndjson",
    .max_batch_size = 64 * 1024 * 1024,
    .fields_count = 2,
    .fields = fields,
    .fields_stride = sizeof(ustore_str_view_t),
    .id_field = "id",
};
ustore_docs_import(&import);
```

## UStoreNVMe File Importer

`nvme_importer.cu` imports every file with one extension under a directory tree into a collection of a UStoreNVMe database, keyed by inode number, from several threads.
It takes its source directory, extension, size limit and data directories as flags, and `--threads` names the threads the run inserts on, as it does in the benchmarks.
It deletes the database's directory and every data directory before importing.
