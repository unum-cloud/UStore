"""Apache Arrow streams through the PyCapsule interface: collection exports, imports and ingests."""

from uuid import UUID

import pytest
from capabilities import key

import ustore

pyarrow = pytest.importorskip("pyarrow")


def write_posters(database: ustore.Database, count: int) -> list[tuple[UUID, bytes]]:
    """Writes `count` posters into the `posters` collection, answering them in key order."""
    rows = [(key(index), f"poster-{index}".encode()) for index in range(1, count + 1)]
    database.collection_create("posters", modality=ustore.Modality.BLOBS)
    database.write([row[0] for row in rows], [row[1] for row in rows], collection="posters")
    return rows


def table_of(rows: list[tuple[UUID, bytes]]) -> "pyarrow.Table":
    """The rows as a table of untagged 16-byte keys and binary values."""
    keys = pyarrow.array([row[0].bytes for row in rows], type=pyarrow.binary(16))
    values = pyarrow.array([row[1] for row in rows], type=pyarrow.binary())
    return pyarrow.table({"key": keys, "value": values})


def test_table_reads_what_was_written(database: ustore.Database) -> None:
    rows = write_posters(database, 3000)
    table = pyarrow.table(database["posters"])
    assert table.column_names == ["key", "value"]
    assert table.schema.metadata[b"ustore.modality"] == b"blobs"
    assert table.column("key").to_pylist() == [row[0] for row in rows]
    assert table.column("value").to_pylist() == [row[1] for row in rows]


def test_pyarrow_table_imports(database: ustore.Database) -> None:
    rows = [(key(index), bytes([index]) * index) for index in range(1, 101)]
    database.collection_create("copies", modality=ustore.Modality.BLOBS)
    assert database.import_arrow(table_of(rows), collection="copies") == len(rows)
    assert [database.get(row[0], collection="copies") for row in rows] == [row[1] for row in rows]


def test_exported_stream_imports_elsewhere(database: ustore.Database) -> None:
    rows = write_posters(database, 100)
    database.collection_create("copies", modality=ustore.Modality.BLOBS)
    assert database.import_arrow(database["posters"], collection="copies") == len(rows)
    assert pyarrow.table(database["copies"]).equals(pyarrow.table(database["posters"]))


def test_array_alone_imports(database: ustore.Database) -> None:
    rows = [(key(1), b"a"), (key(2), b"bb")]
    batch = table_of(rows).to_batches()[0]

    class ArrayOnly:
        """Exports one record batch through `__arrow_c_array__` and nothing else."""

        def __arrow_c_array__(self, requested_schema: object | None = None) -> tuple[object, object]:
            return batch.__arrow_c_array__(requested_schema)

    database.collection_create("copies", modality=ustore.Modality.BLOBS)
    assert database.import_arrow(ArrayOnly(), collection="copies") == 2
    assert [database.get(index, collection="copies") for index in (1, 2)] == [b"a", b"bb"]


def test_null_raises(database: ustore.Database) -> None:
    keys = pyarrow.array([key(1).bytes, key(2).bytes], type=pyarrow.binary(16))
    values = pyarrow.array([b"a", None], type=pyarrow.binary())
    database.collection_create("copies", modality=ustore.Modality.BLOBS)
    with pytest.raises(ustore.InvalidArgumentError):
        database.import_arrow(pyarrow.table({"key": keys, "value": values}), collection="copies")


@pytest.mark.parametrize("order", ["sorted", "unsorted"])
def test_ingest_lands(database: ustore.Database, engine: str, order: str) -> None:
    """An ingest lands every row as one commit, the last repeat of a key winning, which no older snapshot sees."""
    database.collection_create("copies", modality=ustore.Modality.BLOBS)
    rows = [(key(1), b"a"), (key(2), b"b"), (key(3), b"c")]
    if order == "unsorted":
        rows = [(key(3), b"c"), (key(1), b"stale"), (key(2), b"b"), (key(1), b"a")]
    if engine in ("rocksdb", "leveldb"):
        with pytest.raises(ustore.UnsupportedError):
            database.ingest_arrow(table_of(rows), collection="copies", order=order)
        return
    with database.snapshot() as before:
        assert database.ingest_arrow(table_of(rows), collection="copies", order=order) == len(rows)
        with database.snapshot() as after:
            assert before.read_stamp < after.read_stamp
            assert before.get(1, collection="copies") is None
    assert [database.get(index, collection="copies") for index in (1, 2, 3)] == [b"a", b"b", b"c"]
