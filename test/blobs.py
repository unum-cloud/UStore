"""Binary values over the C ABI: single and batch access, scans, samples and measurements."""

import gc
from uuid import UUID

import numpy as np
import pytest
from capabilities import key, keys

import ustore

# Batches below, at and past the 16 values and 64 keys a verb lays out on the stack.
BATCH_SIZES = [
    0,
    1,
    15,
    16,
    17,
    63,
    64,
    65,
    1000,
]

# Empty, short, and longer than one page of the engine's memory.
VALUE_SHAPES = {
    "empty": b"",
    "short": b"x",
    "binary": bytes(range(256)),
    "paged": bytes(range(256)) * 4096,
}


def records(integers: list[int]) -> np.ndarray:
    """Integer keys as the 16-byte records a batch borrows without converting them."""
    return np.frombuffer(b"".join(uuid.bytes for uuid in keys(integers)), dtype="V16")


KEY_CONTAINERS = {
    "list": list,
    "tuple": tuple,
    "uuids": keys,
    "records": records,
    "rows": lambda integers: records(integers).view(np.uint8).reshape(-1, 16),
    "int64": lambda integers: np.array(integers, dtype=np.int64),
    "uint64": lambda integers: np.array(integers, dtype=np.uint64),
    "int32": lambda integers: np.array(integers, dtype=np.int32),
}


# region Single Access


@pytest.mark.parametrize("shape", VALUE_SHAPES)
def test_single_round_trip(view, shape: str):
    """A value reads back byte for byte, whatever its length."""
    value = VALUE_SHAPES[shape]
    view.set(3, value)
    assert view.contains(3)
    assert view.get(3) == value
    view.pop(3)
    assert not view.contains(3)
    assert view.get(3) is None


@pytest.mark.parametrize(("first", "second"), [(b"y", b"jjjjjjjj"), (b"jjjjjjjj", b"yy"), (b"", b"z"), (b"z", b"")])
def test_overwrite_changes_length(view, first: bytes, second: bytes):
    """An overwrite replaces the whole value, growing or shrinking it."""
    view.set(7, first)
    view.set(7, second)
    assert view.get(7) == second


def test_set_none_removes(view):
    view.set(9, b"here")
    view.set(9, None)
    assert not view.contains(9)


@pytest.mark.parametrize("boundary", [0, 2**63, 2**64 - 2])
def test_keys_span_the_lower_64_bits(view, boundary: int):
    """Every engine addresses the integer keys that fit 64 bits."""
    view.set(boundary, b"edge")
    assert view.get(boundary) == b"edge"


@pytest.mark.parametrize("boundary", [2**64, 2**128 - 2, UUID("0190b8f2-6c4e-7a31-9d4f-2b6c8e1a5f37")])
def test_keys_span_128_bits(view, boundary: int | UUID):
    """Every key but the one reserved for unknown keys addresses a value."""
    view.set(boundary, b"edge")
    assert view.get(boundary) == b"edge"


@pytest.mark.parametrize("boundary", [2**128, -1])
def test_rejects_an_integer_outside_128_bits(view, boundary: int):
    with pytest.raises(OverflowError):
        view.get(boundary)


@pytest.mark.parametrize(
    "spelling",
    [5, np.uint64(5), key(5), str(key(5)), key(5).bytes],
    ids=["integer", "numpy", "uuid", "text", "bytes"],
)
def test_every_spelling_of_a_key_addresses_one_value(view, spelling):
    view.set(spelling, b"v")
    assert view.get(5) == b"v"
    assert view.scan(0, 2)[0] == (key(5),)


# endregion Single Access

# region Batches


@pytest.mark.parametrize("count", BATCH_SIZES)
@pytest.mark.parametrize("container", KEY_CONTAINERS)
def test_batch_round_trip(view, count: int, container: str):
    """A batch reads back in the order it was asked for, borrowed as 16-byte keys or converted from integers."""
    keys = KEY_CONTAINERS[container](list(range(count)))
    values = [str(key).encode() * (key % 3 + 1) for key in range(count)]
    view.write(keys, values)
    found = view.read(keys)
    assert len(found) == count
    assert [bytes(value) for value in found] == values
    assert found.lengths == tuple(len(value) for value in values)


@pytest.mark.parametrize("missing", [[0], [1], [0, 2], [0, 1, 2]])
def test_batch_reports_missing_keys(view, missing: list[int]):
    """An absent key reads as None in its own position and nowhere else."""
    present = [key for key in range(3) if key not in missing]
    view.write(present, [b"v%d" % key for key in present])
    found = view.read([0, 1, 2])
    assert [value is None for value in found] == [key in missing for key in range(3)]
    assert found.lengths == tuple(None if key in missing else 2 for key in range(3))


@pytest.mark.parametrize("count", BATCH_SIZES)
def test_batch_broadcasts_one_value(view, count: int):
    view.write(list(range(count)), b"same")
    assert [bytes(value) for value in view.read(list(range(count)))] == [b"same"] * count


@pytest.mark.parametrize("count", BATCH_SIZES)
def test_batch_removes_every_key(view, count: int):
    keys = list(range(count))
    view.write(keys, b"value")
    view.write(keys, None)
    assert all(value is None for value in view.read(keys))


@pytest.mark.parametrize("removed", [0, 1])
def test_batch_removes_one_key_of_many(view, removed: int):
    view.write([30, 31], [b"a", b"b"])
    view.write([30, 31], [None, b"kept"] if removed == 0 else [b"kept", None])
    assert view.contains(30 + 1 - removed)
    assert not view.contains(30 + removed)


@pytest.mark.parametrize("keys", [[40], [40, 41], np.array([40, 41], dtype=np.int64)], ids=["one", "two", "numpy"])
def test_batch_pop(view, keys):
    view.write([40, 41], [b"a", b"b"])
    view.pop(keys)
    assert not any(view.contains(int(key)) for key in keys)


def test_pop_takes_a_bare_key(view):
    view.set(40, b"a")
    view.pop(40)
    assert not view.contains(40)


@pytest.mark.parametrize("values", [[b"a", b"b"], [b"a", b"b", b"c", b"d"]], ids=["short", "long"])
def test_rejects_mismatched_batch_widths(view, values: list[bytes]):
    with pytest.raises(ValueError):
        view.write([1, 2, 3], values)


# endregion Batches

# region Tapes


def test_values_are_borrowed_not_copied(view):
    view.set(1, b"borrowed")
    found = view.read([1])[0]
    assert isinstance(found, memoryview)
    assert found.readonly
    assert bytes(found) == b"borrowed"


def test_a_view_outlives_the_tape_that_exported_it(view):
    view.set(1, b"borrowed")
    held = view.read([1])[0]
    gc.collect()
    assert bytes(held) == b"borrowed"


def test_a_later_read_cannot_clobber_a_held_view(view):
    view.set(1, b"first")
    held = view.read([1])[0]
    view.set(2, b"second")
    assert bytes(view.read([2])[0]) == b"second"
    assert bytes(held) == b"first"


@pytest.mark.parametrize("tape_type", [ustore.Values, ustore.Keys, ustore.Columns, ustore.Edges])
def test_tape_types_are_named_but_not_constructible(tape_type: type):
    with pytest.raises(TypeError):
        tape_type()


def test_whole_tape_exports_a_buffer(view):
    view.write([1, 2], [b"ab", b"cd"])
    assert bytes(memoryview(view.read([1, 2]))) == b"abcd"


def test_keys_export_as_16_byte_records(view):
    view.write([5, 6], b"v")
    found = view.scan(0, 2)
    assert found[0] == (key(5), key(6))
    exported = memoryview(found)
    assert (exported.format, exported.shape) == ("B", (2, 16))
    assert exported.tobytes() == key(5).bytes + key(6).bytes


@pytest.mark.parametrize("index", [1, -2, 5])
def test_rejects_an_index_past_the_batch(view, index: int):
    view.set(1, b"a")
    with pytest.raises(IndexError):
        view.read([1])[index]


# endregion Tapes

# region Scans and Samples


@pytest.mark.parametrize(
    ("start", "count", "expected"),
    [(10, 6, [10, 20, 30, 40, 50, 60]), (20, 5, [20, 30, 40, 50, 60]), (30, 1, [30]), (40, 2, [40, 50]), (61, 5, [])],
)
def test_scan(view, start: int, count: int, expected: list[int]):
    view.write([10, 20, 30, 40, 50, 60], b"v")
    page = view.scan(start, count)
    assert list(page[0]) == keys(expected)
    assert page.counts == (len(expected),)


@pytest.mark.parametrize("page_size", [1, 2, 3, 7, 8])
def test_scan_paginates_from_the_last_key(view, page_size: int):
    view.write(list(range(1, 8)), b"v")
    walked: list[UUID] = []
    start = 0
    while page := list(view.scan(start, page_size)[0]):
        walked.extend(page)
        start = page[-1].int + 1
    assert walked == keys(range(1, 8))


@pytest.mark.parametrize("count", [0, 1, 3, 9, 20])
def test_sample(view, count: int):
    view.write(list(range(1, 10)), b"v")
    drawn = list(view.sample(count)[0])
    assert len(drawn) == len(set(drawn)) == min(count, 9)
    assert set(drawn) <= set(keys(range(1, 10)))


def test_measure(view):
    view.write([1, 2, 3], [b"a", b"bb", b"ccc"])
    bounds = view.measure(0, 100)
    assert set(bounds) == {
        "min_cardinality",
        "max_cardinality",
        "min_value_bytes",
        "max_value_bytes",
        "min_space_usage",
        "max_space_usage",
    }
    assert bounds["max_cardinality"] >= bounds["min_cardinality"]


# endregion Scans and Samples

# region Refusals


@pytest.mark.parametrize(
    ("call", "failure"),
    [
        (lambda view: view.get(1, colection=None), TypeError),
        (lambda view: view.set(1, 42), TypeError),
        (lambda view: view.set(1, "text"), TypeError),
        (lambda view: view.get("one"), ValueError),
        (lambda view: view.get(b"short"), ValueError),
        (lambda view: view.get(1.0), TypeError),
        (lambda view: view.read(1), TypeError),
        (lambda view: view.read(str(key(1))), TypeError),
        (lambda view: view.scan(0, -1), ValueError),
        (lambda view: view.get(), TypeError),
    ],
    ids=[
        "keyword",
        "integer-value",
        "string-value",
        "malformed-text-key",
        "short-bytes-key",
        "float-key",
        "bare-key-read",
        "text-key-read",
        "negative-count",
        "no-key",
    ],
)
def test_rejects_malformed_arguments(view, call, failure: type):
    with pytest.raises(failure):
        call(view)


def test_refuses_a_closed_database(open_database):
    closed = open_database()
    closed.close()
    with pytest.raises(ValueError):
        closed.get(1)


def test_closing_twice_is_safe(open_database):
    database = open_database()
    database.close()
    database.close()


# endregion Refusals
