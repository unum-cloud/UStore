# Tools

The tools run on the embedded C ABI of `include/ustore/db.h` and never see which engine is behind it, so the same commands drive the in-memory SmashTable engine, RocksDB and LevelDB, and UDisk once it builds.

## Command-Line Interface

`cli.cpp` builds into one executable per engine, `ustore_cli_<engine>`, when CMake is configured with `-DUSTORE_BUILD_TOOLS_CPP=1`.
It opens the database in the `--directory` it is given, runs one command and exits, with status 1 when the flags do not parse or the command fails.

```sh
cmake --preset release -DUSTORE_BUILD_TOOLS_CPP=1 -DUSTORE_USE_ROCKSDB=1
cmake --build --preset release --target ustore_cli_rocksdb
build_release/build/bin/ustore_cli_rocksdb --directory /dev/shm/ustore \
    collection import --input data/twitter/tweets.ndjson --id-field id --memory-limit 1GiB --collection tweets
```

| Command                  | Action                                                                           |
| :----------------------- | :------------------------------------------------------------------------------- |
| `collection create`      | Creates the collection named by `--collection`                                   |
| `collection drop`        | Drops the collection named by `--collection`                                     |
| `collection list`        | Prints the name of every collection                                              |
| `collection import`      | Imports `--input` as documents keyed by `--id-field`, `--memory-limit` per batch |
| `collection export`      | Exports documents into `--output`, `--memory-limit` per batch                    |
| `snapshot create`        | Takes a snapshot                                                                 |
| `snapshot list`          | Prints the existing snapshots                                                    |
| `snapshot export <path>` | Writes a snapshot to a path                                                      |
| `snapshot drop <id>`     | Drops a snapshot                                                                 |

| Flag                    | Meaning                                                                |
| :---------------------- | :--------------------------------------------------------------------- |
| `--directory <path>`    | Database directory; required                                           |
| `--collection <name>`   | Collection a command acts on; the main collection when omitted         |
| `--input <file>`        | File a collection import reads                                         |
| `--output <file>`       | File a collection export writes                                        |
| `--id-field <field>`    | Field holding the keys of imported documents                           |
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

## UDisk File Importer

`udisk_importer.cu` imports every file with one extension under a directory tree into a collection of a UDisk database, keyed by inode number, from several threads.
It takes its source directory, extension, size limit and data directories as flags, and `--threads` names the threads the run inserts on, as it does in the benchmarks.
It deletes the database's directory and every data directory before importing.
