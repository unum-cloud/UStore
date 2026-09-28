"""Benchmarks importing NDJSON or CSV files as documents and as a graph, and exporting both back.

Every row becomes one document keyed by its id field and one edge between its integer source and
target fields, labeled by its edge field, which is what `tabular_graph.cpp` measures through the C++
importers. Here the files are read through pyarrow and written through the CPython bindings, so the
numbers include every conversion a Python pipeline pays for.

Run:
    python bench/tabular_graph.py --dataset data/<name> --format csv --directory /dev/shm/ustore-tabular \\
        --id-field id --source-field source --target-field target --edge-field edge --threads 4
"""

import argparse
import json
import time
from collections.abc import Callable, Iterator
from concurrent.futures import ThreadPoolExecutor
from itertools import batched
from pathlib import Path
from uuid import UUID

import numpy as np
import pyarrow
import pyarrow.csv
import pyarrow.json

import ustore
from ustore.keys import PAGE_KEYS, scan_keys

MEBIBYTE = 1 << 20


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dataset", type=Path, required=True, help="directory of input files")
    parser.add_argument("--format", choices=["ndjson", "csv"], default="ndjson", help="format of inputs and exports")
    parser.add_argument("--engine", choices=ustore.engines_compiled(), default="ram", help="engine to open")
    parser.add_argument("--directory", type=Path, required=True, help="directory of the exports and the database")
    parser.add_argument("--id-field", required=True, help="field holding each document's key")
    parser.add_argument("--source-field", required=True, help="integer field holding the source vertex")
    parser.add_argument("--target-field", required=True, help="integer field holding the target vertex")
    parser.add_argument("--edge-field", required=True, help="field holding the edge key")
    parser.add_argument("--threads", type=int, default=1, help="import threads")
    parser.add_argument("--max-input-files", type=int, default=10, help="files each import thread takes")
    parser.add_argument("--batch-rows", type=int, default=1 << 16, help="rows one write carries")
    return parser.parse_args()


def list_files(settings: argparse.Namespace) -> list[Path]:
    """Up to `max_input_files` files per thread with the extension of the requested format."""
    wanted = settings.max_input_files * settings.threads
    found = sorted(path for path in settings.dataset.iterdir() if path.suffix == f".{settings.format}")
    return found[:wanted]


def read_table(path: Path, file_format: str) -> pyarrow.Table:
    """One input file as a table whose nested fields are flattened into dotted columns."""
    table = pyarrow.json.read_json(path) if file_format == "ndjson" else pyarrow.csv.read_csv(path)
    while any(pyarrow.types.is_struct(field.type) for field in table.schema):
        table = table.flatten()
    return table


def column_name(field: str) -> str:
    """The flattened column a field names, whether a top-level key or a JSON Pointer."""
    return field.removeprefix("/").replace("/", ".")


def integer_column(table: pyarrow.Table, field: str) -> np.ndarray:
    return table.column(column_name(field)).to_numpy().astype(np.int64, copy=False)


def report(name: str, processed_bytes: int, duration: float) -> None:
    rate = processed_bytes / MEBIBYTE / duration if duration else float("inf")
    print(f"{name}: {rate:.2f} MiB/s, {processed_bytes / MEBIBYTE:.2f} MiB in {duration:.3f} s", flush=True)


def run_threads(files: list[Path], settings: argparse.Namespace, import_file: Callable[[Path], int]) -> int:
    """Hands every thread its own run of files and answers the bytes they imported together."""
    shards = [files[shard :: settings.threads] for shard in range(settings.threads)]
    with ThreadPoolExecutor(settings.threads) as pool:
        return sum(pool.map(lambda shard: sum(import_file(path) for path in shard), shards))


def clear_main(database: ustore.Database) -> None:
    database.collection_drop(0, mode="pairs")


# region Documents


def bench_docs_import(database: ustore.Database, files: list[Path], settings: argparse.Namespace) -> None:
    def import_file(path: Path) -> int:
        rows = read_table(path, settings.format).to_pylist()
        for batch in batched(rows, settings.batch_rows):
            database.docs_write(None, [json.dumps(row) for row in batch], id_field=settings.id_field)
        return path.stat().st_size

    start = time.perf_counter()
    imported = run_threads(files, settings, import_file)
    report(f"docs_import_{settings.format}", imported, time.perf_counter() - start)


def exported_documents(database: ustore.Database) -> Iterator[dict]:
    for page in batched(scan_keys(database), PAGE_KEYS):
        for document in database.docs_read(page):
            if document is not None:
                yield json.loads(bytes(document))


def bench_docs_export(database: ustore.Database, settings: argparse.Namespace) -> None:
    output = settings.directory / f"tabular_graph_docs.{settings.format}"
    start = time.perf_counter()
    if settings.format == "ndjson":
        with output.open("w") as file:
            file.writelines(json.dumps(document) + "\n" for document in exported_documents(database))
    else:
        pyarrow.csv.write_csv(pyarrow.Table.from_pylist(list(exported_documents(database))), output)
    duration = time.perf_counter() - start
    exported = output.stat().st_size
    output.unlink()
    report(f"docs_export_{settings.format}", exported, duration)
    clear_main(database)


# endregion Documents

# region Graphs


def bench_graph_import(database: ustore.Database, graph: UUID, files: list[Path], settings: argparse.Namespace) -> None:
    def import_file(path: Path) -> int:
        table = read_table(path, settings.format)
        sources = integer_column(table, settings.source_field)
        targets = integer_column(table, settings.target_field)
        edges = integer_column(table, settings.edge_field)
        for begin in range(0, len(sources), settings.batch_rows):
            end = begin + settings.batch_rows
            database.graph_upsert_edges(
                sources[begin:end], targets[begin:end], edges=edges[begin:end], collection=graph
            )
        return 0

    start = time.perf_counter()
    run_threads(files, settings, import_file)
    duration = time.perf_counter() - start
    # Every stored vertex is counted as three keys, as the C++ benchmark counts it.
    imported = sum(1 for _ in scan_keys(database, graph)) * 3 * 8
    report(f"graph_import_{settings.format}", imported, duration)


def bench_graph_export(database: ustore.Database, graph: UUID, settings: argparse.Namespace) -> None:
    output = settings.directory / f"tabular_graph_edges.{settings.format}"
    # Exported column names cannot hold the slashes of JSON Pointers.
    names = [field.replace("/", "_") for field in (settings.source_field, settings.target_field, settings.edge_field)]
    start = time.perf_counter()
    tapes = [
        np.asarray(database.graph_find_edges(page, role="source", collection=graph))
        for page in batched(scan_keys(database, graph), PAGE_KEYS)
    ]
    triples = np.concatenate(tapes) if tapes else np.empty((0, 3), dtype=np.int64)
    table = pyarrow.Table.from_arrays([pyarrow.array(triples[:, column]) for column in range(3)], names=names)
    if settings.format == "ndjson":
        with output.open("w") as file:
            file.writelines(json.dumps(row) + "\n" for row in table.to_pylist())
    else:
        pyarrow.csv.write_csv(table, output)
    duration = time.perf_counter() - start
    exported = output.stat().st_size
    output.unlink()
    report(f"graph_export_{settings.format}", exported, duration)
    database.collection_drop(graph)


# endregion Graphs


def main() -> None:
    settings = parse_arguments()
    files = list_files(settings)
    if not files:
        raise SystemExit(f"No .{settings.format} files in {settings.dataset}")
    print(f"Found {len(files)} .{settings.format} files", flush=True)
    settings.directory.mkdir(parents=True, exist_ok=True)

    with getattr(ustore.Database, f"open_{settings.engine}")(settings.directory) as database:
        bench_docs_import(database, files, settings)
        bench_docs_export(database, settings)
        graph = database.collection_create("graph", modality="graph")
        bench_graph_import(database, graph, files, settings)
        bench_graph_export(database, graph, settings)


if __name__ == "__main__":
    main()
