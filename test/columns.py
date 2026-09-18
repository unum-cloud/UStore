"""Typed columns over the C ABI: scalar writes that skip JSON, and the half-precision and UUID gathers."""

import json
import struct

import numpy
import pytest
from ustore.table_view import WRITE_TYPES, Table

# A NumPy dtype, the type name a write spells it as, and the values a column carries.
COLUMN_DTYPES = {
    "f64": (numpy.float64, [1.5, -2.25, 3.75]),
    "f32": (numpy.float32, [1.5, -2.25, 3.75]),
    "i64": (numpy.int64, [-(2**40), 0, 2**40]),
    "i32": (numpy.int32, [-5, 0, 7]),
    "i16": (numpy.int16, [-5, 0, 7]),
    "i8": (numpy.int8, [-5, 0, 7]),
    "u64": (numpy.uint64, [0, 1, 2**40]),
    "u32": (numpy.uint32, [0, 1, 2**31]),
    "u16": (numpy.uint16, [0, 1, 65535]),
    "u8": (numpy.uint8, [0, 1, 255]),
    "bool": (numpy.bool_, [True, False, True]),
}

# The UUID the gather parses, and the sixteen bytes it decodes into.
UUID_TEXT = "12345678-1234-5678-1234-567812345678"
UUID_BYTES = bytes.fromhex(UUID_TEXT.replace("-", ""))


def bit(bitmap, index: int) -> bool:
    """One bit of an Arrow-style bitmap, addressed least significant bit first."""
    return bool(bitmap[index // 8] >> (index % 8) & 1)


def decoded(values) -> list:
    """Every document of a read, decoded from JSON, with None where the key was absent."""
    return [None if value is None else json.loads(bytes(value)) for value in values]


# region Scalar Column Writes


@pytest.mark.parametrize("type_name", list(COLUMN_DTYPES))
def test_column_write_round_trip(view, type_name: str):
    """A NumPy column of scalars reaches every document without being printed as JSON."""
    dtype, values = COLUMN_DTYPES[type_name]
    keys = [3, 4, 5]
    view.docs_write(keys, ["{}"] * len(keys))
    view.docs_write(keys, numpy.array(values, dtype=dtype), field="v", type=type_name)
    assert decoded(view.docs_read(keys)) == [{"v": value} for value in values]


def test_column_write_accepts_bytes(view):
    """A plain buffer of bytes carries no width of its own, so it splits by the field's."""
    keys = [1, 2, 3]
    view.docs_write(keys, ["{}"] * len(keys))
    view.docs_write(keys, struct.pack("<3i", 7, 8, 9), field="v", type="i32")
    assert decoded(view.docs_read(keys)) == [{"v": 7}, {"v": 8}, {"v": 9}]


def test_column_write_broadcasts_one_scalar(view):
    """One scalar the width of the field stands for every key, as one document does."""
    keys = [1, 2, 3]
    view.docs_write(keys, ["{}"] * len(keys))
    view.docs_write(keys, numpy.array([2.5], dtype=numpy.float64), field="v", type="f64")
    assert decoded(view.docs_read(keys)) == [{"v": 2.5}] * 3


def test_column_write_refuses_a_strided_view(view):
    """A column is borrowed rather than copied, so a slice with gaps in it is refused outright."""
    keys = [1, 2, 3]
    view.docs_write(keys, ["{}"] * len(keys))
    strided = numpy.arange(6, dtype=numpy.int64)[::2]
    with pytest.raises(TypeError):
        view.docs_write(keys, strided, field="v", type="i64")
    view.docs_write(keys, numpy.ascontiguousarray(strided), field="v", type="i64")
    assert decoded(view.docs_read(keys)) == [{"v": 0}, {"v": 2}, {"v": 4}]


def test_column_write_replaces_whole_documents(view):
    """Without a field the scalars replace the documents themselves, one per key."""
    keys = [1, 2]
    view.docs_write(keys, numpy.array([4.5, 5.5], dtype=numpy.float64), type="f64")
    assert decoded(view.docs_read(keys)) == [4.5, 5.5]


@pytest.mark.parametrize(
    ("call", "failure"),
    [
        (
            lambda view: view.docs_write([1, 2, 3], numpy.zeros(2, dtype=numpy.float64), field="v", type="f64"),
            ValueError,
        ),
        (
            lambda view: view.docs_write([1, 2, 3], numpy.zeros(3, dtype=numpy.float32), field="v", type="f64"),
            ValueError,
        ),
        (lambda view: view.docs_write([1, 2], b"\x00" * 3, field="v", type="i32"), ValueError),
        (lambda view: view.docs_write([1], numpy.zeros(1, dtype=numpy.float64), field="v", type="f16"), ValueError),
        (lambda view: view.docs_write([1], numpy.zeros(1, dtype=numpy.float64), field="v", type="uuid"), ValueError),
    ],
    ids=["short-column", "narrow-items", "ragged-bytes", "unwritable-f16", "unwritable-uuid"],
)
def test_column_write_refusals(view, call, failure):
    """A column whose values do not fit its type, and a type no write spells, are refused."""
    view.docs_write([1, 2, 3], ["{}"] * 3)
    with pytest.raises(failure):
        call(view)


# endregion Scalar Column Writes

# region Half Precision Gathers


def test_gather_half_precision(view):
    """An 'f16' column exports the 'e' format, which NumPy reads as float16 without a copy."""
    view.docs_write([0, 1], [json.dumps({"v": 1.5}), json.dumps({"v": -0.25})])
    columns = view.docs_gather([0, 1], ["v"], "f16")
    column = columns[0]
    assert column.format == "e"
    assert column.itemsize == 2
    assert numpy.asarray(column).dtype == numpy.float16
    assert numpy.asarray(column).tolist() == [1.5, -0.25]
    assert bit(columns.validity(0), 0) and bit(columns.validity(0), 1)


def test_gather_half_precision_as_bytes(view):
    """Without NumPy the same column reads as the bytes behind it, which struct unpacks."""
    view.docs_write([0], json.dumps({"v": 2.5}))
    column = view.docs_gather([0], ["v"], "f16")[0]
    assert struct.unpack("<e", bytes(column.cast("B"))) == (2.5,)


def test_gather_half_precision_overflows(view):
    """A magnitude past the largest finite half collides, and the cell is left at zero."""
    view.docs_write([0, 1], [json.dumps({"v": 1e9}), json.dumps({"v": 1.0})])
    columns = view.docs_gather([0, 1], ["v"], "f16")
    assert bit(columns.collision(0), 0)
    assert not bit(columns.collision(0), 1)
    assert numpy.asarray(columns[0])[1] == numpy.float16(1.0)


def test_gather_half_precision_converts(view):
    """An integer reaches a half column converted, which the conversion bitmap records."""
    view.docs_write([0, 1], [json.dumps({"v": 3}), json.dumps({"v": 0.1})])
    columns = view.docs_gather([0, 1], ["v"], "f16")
    assert bit(columns.conversion(0), 0)
    assert not bit(columns.conversion(0), 1)
    assert numpy.asarray(columns[0]).tolist() == [3.0, numpy.float16(0.1)]


# endregion Half Precision Gathers

# region UUID Gathers


def test_gather_uuid(view):
    """A 'uuid' column exports sixteen bytes per document, as a two-dimensional view."""
    view.docs_write([0], json.dumps({"v": UUID_TEXT}))
    columns = view.docs_gather([0], ["v"], "uuid")
    column = columns[0]
    assert column.shape == (1, 16)
    assert column.tolist() == [list(UUID_BYTES)]
    assert numpy.asarray(column)[0].tobytes() == UUID_BYTES
    assert bit(columns.validity(0), 0)


@pytest.mark.parametrize(
    "value",
    ["not-a-uuid", "12345678-1234-5678-1234-56781234567", 42, {"k": 1}],
    ids=["text", "short", "integer", "object"],
)
def test_gather_uuid_collides(view, value):
    """A value no UUID text spells collides, and the cell is left at sixteen zeroes."""
    view.docs_write([0], json.dumps({"v": value}))
    columns = view.docs_gather([0], ["v"], "uuid")
    assert bit(columns.collision(0), 0)
    assert not bit(columns.validity(0), 0)
    assert bytes(columns[0]) == bytes(16)


def test_gather_uuid_beside_a_scalar(view):
    """Columns of differing widths stand side by side, each naming the type it was gathered as."""
    view.docs_write([0], json.dumps({"id": UUID_TEXT, "n": 7}))
    columns = view.docs_gather([0], ["id", "n"], ["uuid", "i64"])
    assert columns.types == ("uuid", "i64")
    assert bytes(columns[0]) == UUID_BYTES
    assert numpy.asarray(columns[1]).tolist() == [7]


def test_gather_holds_its_arena(view):
    """A view outlives the result it came from, since the memoryview keeps the arena alive."""
    view.docs_write([0], json.dumps({"v": UUID_TEXT}))
    column = view.docs_gather([0], ["v"], "uuid")[0]
    view.docs_write([1], json.dumps({"v": UUID_TEXT}))
    assert bytes(column) == UUID_BYTES


@pytest.mark.parametrize("type_name", ["json", "null", "i128"])
def test_gather_refuses_unconvertible_types(view, type_name: str):
    """A gather names the types it converts into, and nothing outside that closed set."""
    view.docs_write([0], "{}")
    with pytest.raises(ValueError):
        view.docs_gather([0], ["v"], type_name)


def test_gather_of_no_documents(view):
    """An empty gather answers empty columns of the types it was asked for."""
    columns = view.docs_gather([], ["v"], "f16")
    assert columns.rows == 0
    assert len(columns[0]) == 0


# endregion UUID Gathers

# region Table Columns


def test_table_column_assignment_is_typed(view):
    """Assigning a NumPy column writes raw scalars, so the stored values keep their dtype exactly."""
    table = Table.from_records(view, [{"n": row} for row in range(4)])
    table["score"] = numpy.array([0.1, 0.25, 0.5, 0.75], dtype=numpy.float64)
    assert decoded(view.docs_read(table.index)) == [
        {"n": row, "score": score} for row, score in enumerate([0.1, 0.25, 0.5, 0.75])
    ]


@pytest.mark.parametrize("dtype", [numpy.int64, numpy.int32, numpy.uint16, numpy.float32, numpy.bool_])
def test_table_column_assignment_dtypes(view, dtype):
    """Every dtype a column writes as raw scalars round-trips through the table view."""
    table = Table.from_records(view, [{"n": row} for row in range(3)])
    table["v"] = numpy.array([1, 0, 1], dtype=dtype)
    gathered = view.docs_gather(table.index, ["v"], WRITE_TYPES[numpy.dtype(dtype).name])
    assert numpy.asarray(gathered[0]).tolist() == numpy.array([1, 0, 1], dtype=dtype).tolist()


def test_table_column_of_objects_still_writes_json(view):
    """A column of Python objects has no scalar width, so it is spelled as JSON as before."""
    table = Table.from_records(view, [{"n": row} for row in range(2)])
    table["tag"] = ["first", "second"]
    assert decoded(view.docs_read(table.index)) == [{"n": 0, "tag": "first"}, {"n": 1, "tag": "second"}]


def test_table_column_of_the_wrong_length(view):
    """A column shorter than the rows it would fill is refused before anything is written."""
    table = Table.from_records(view, [{"n": row} for row in range(3)])
    with pytest.raises(ValueError):
        table["v"] = numpy.zeros(2, dtype=numpy.float64)


# endregion Table Columns
