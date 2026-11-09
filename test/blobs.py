"""Binary values over the C ABI: single and batch access, scans, samples, measurements and deferred calls.

File: test/blobs.py
Author: Ash Vardanian
Date: September 18, 2026
"""

import asyncio
import contextlib
import gc
import weakref
from uuid import UUID

import numpy as np
import pytest
from capabilities import BATCH_SIZES, key, keys, location

import ustore

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
def test_single_round_trip(view, blobs, shape: str):
    """A value reads back byte for byte, whatever its length."""
    value = VALUE_SHAPES[shape]
    view.set(3, value, collection=blobs)
    assert view.contains(3, collection=blobs)
    assert view.get(3, collection=blobs) == value
    view.remove(3, collection=blobs)
    assert not view.contains(3, collection=blobs)
    assert view.get(3, collection=blobs) is None


@pytest.mark.parametrize(("first", "second"), [(b"y", b"jjjjjjjj"), (b"jjjjjjjj", b"yy"), (b"", b"z"), (b"z", b"")])
def test_overwrite_changes_length(view, blobs, first: bytes, second: bytes):
    """An overwrite replaces the whole value, growing or shrinking it."""
    view.set(7, first, collection=blobs)
    view.set(7, second, collection=blobs)
    assert view.get(7, collection=blobs) == second


def test_set_refuses_none(view, blobs):
    """A None never removes, since `remove` does."""
    view.set(9, b"here", collection=blobs)
    with pytest.raises(TypeError, match="remove"):
        view.set(9, None, collection=blobs)
    assert view.get(9, collection=blobs) == b"here"


@pytest.mark.parametrize("boundary", [0, 2**63, 2**64 - 2])
def test_keys_span_the_lower_64_bits(view, blobs, boundary: int):
    """Every engine addresses the integer keys that fit 64 bits."""
    view.set(boundary, b"edge", collection=blobs)
    assert view.get(boundary, collection=blobs) == b"edge"


@pytest.mark.parametrize("boundary", [2**64, 2**128 - 2, UUID("0190b8f2-6c4e-7a31-9d4f-2b6c8e1a5f37")])
def test_keys_span_128_bits(view, blobs, boundary: int | UUID):
    """Every key but the one reserved for unknown keys addresses a value."""
    view.set(boundary, b"edge", collection=blobs)
    assert view.get(boundary, collection=blobs) == b"edge"


@pytest.mark.parametrize("boundary", [2**128, -1])
def test_rejects_an_integer_outside_128_bits(view, blobs, boundary: int):
    with pytest.raises(OverflowError):
        view.get(boundary, collection=blobs)


@pytest.mark.parametrize(
    "spelling",
    [5, np.uint64(5), key(5), str(key(5)), key(5).bytes],
    ids=["integer", "numpy", "uuid", "text", "bytes"],
)
def test_every_spelling_of_a_key_addresses_one_value(view, blobs, arena, spelling):
    view.set(spelling, b"v", collection=blobs)
    assert view.get(5, collection=blobs) == b"v"
    assert view.scan(0, 2, arena, collection=blobs)[0] == (key(5),)


# endregion Single Access

# region Batches


@pytest.mark.parametrize("count", BATCH_SIZES)
@pytest.mark.parametrize("container", KEY_CONTAINERS)
def test_batch_round_trip(view, blobs, arena, count: int, container: str):
    """A batch reads back in the order it was asked for, borrowed as 16-byte keys or converted from integers."""
    keys = KEY_CONTAINERS[container](list(range(count)))
    values = [str(key).encode() * (key % 3 + 1) for key in range(count)]
    view.write(keys, values, collection=blobs)
    found = view.read(keys, arena, collection=blobs)
    assert len(found) == count
    assert [bytes(value) for value in found] == values
    assert found.lengths == tuple(len(value) for value in values)


@pytest.mark.parametrize("missing", [[0], [1], [0, 2], [0, 1, 2]])
def test_batch_reports_missing_keys(view, blobs, arena, missing: list[int]):
    """An absent key reads as None in its own position and nowhere else."""
    present = [key for key in range(3) if key not in missing]
    view.write(present, [b"v%d" % key for key in present], collection=blobs)
    found = view.read([0, 1, 2], arena, collection=blobs)
    assert [value is None for value in found] == [key in missing for key in range(3)]
    assert found.lengths == tuple(None if key in missing else 2 for key in range(3))


@pytest.mark.parametrize("count", BATCH_SIZES)
def test_batch_broadcasts_one_value(view, blobs, arena, count: int):
    view.write(list(range(count)), b"same", collection=blobs)
    assert [bytes(value) for value in view.read(list(range(count)), arena, collection=blobs)] == [b"same"] * count


@pytest.mark.parametrize("count", BATCH_SIZES)
def test_batch_removes_every_key(view, blobs, arena, count: int):
    keys = list(range(count))
    view.write(keys, b"value", collection=blobs)
    view.remove(keys, collection=blobs)
    assert all(value is None for value in view.read(keys, arena, collection=blobs))


@pytest.mark.parametrize("values", [[None, b"kept"], [b"kept", None], None], ids=["first", "second", "whole"])
def test_none_in_a_batch_is_refused(view, blobs, arena, values):
    """A None inside a written batch is refused before anything is written, rather than read as a removal."""
    view.write([30, 31], [b"a", b"b"], collection=blobs)
    with pytest.raises(TypeError):
        view.write([30, 31], values, collection=blobs)
    assert [bytes(value) for value in view.read([30, 31], arena, collection=blobs)] == [b"a", b"b"]


@pytest.mark.parametrize("keys", [[40], [40, 41], np.array([40, 41], dtype=np.int64)], ids=["one", "two", "numpy"])
def test_batch_remove(view, blobs, keys):
    view.write([40, 41], [b"a", b"b"], collection=blobs)
    view.remove(keys, collection=blobs)
    assert not any(view.contains(int(key), collection=blobs) for key in keys)


def test_remove_takes_a_bare_key(view, blobs):
    view.set(40, b"a", collection=blobs)
    view.remove(40, collection=blobs)
    assert not view.contains(40, collection=blobs)


@pytest.mark.parametrize("values", [[b"a", b"b"], [b"a", b"b", b"c", b"d"]], ids=["short", "long"])
def test_rejects_mismatched_batch_widths(view, blobs, values: list[bytes]):
    with pytest.raises(ValueError):
        view.write([1, 2, 3], values, collection=blobs)


# endregion Batches

# region Tapes and Arenas


def test_values_are_borrowed_not_copied(view, blobs, arena):
    view.set(1, b"borrowed", collection=blobs)
    found = view.read([1], arena, collection=blobs)[0]
    assert isinstance(found, memoryview)
    assert found.readonly
    assert bytes(found) == b"borrowed"


def test_a_view_outlives_the_tape_that_exported_it(view, blobs, arena):
    view.set(1, b"borrowed", collection=blobs)
    held = view.read([1], arena, collection=blobs)[0]
    gc.collect()
    assert bytes(held) == b"borrowed"


def test_one_arena_serves_every_verb_in_turn(view, blobs, arena):
    """Each verb rewinds the arena it is handed, so one arena serves reads, scans and samples one after another."""
    view.write([1, 2, 3], [b"a", b"bb", b"ccc"], collection=blobs)
    assert [bytes(value) for value in view.read([1, 2], arena, collection=blobs)] == [b"a", b"bb"]
    assert view.scan(0, 3, arena, collection=blobs)[0] == tuple(keys([1, 2, 3]))
    assert set(view.sample(2, arena, collection=blobs)[0]) <= set(keys([1, 2, 3]))
    assert isinstance(view.measure(0, 100, arena, collection=blobs), ustore.Measurement)
    assert view.read([3], arena, collection=blobs).lengths == (3,)


def test_an_exported_buffer_pins_its_arena(view, blobs, arena):
    """A memoryview over an output pins its arena, as one over a bytearray pins it, until the view is released."""
    view.write([1, 2], [b"first", b"second"], collection=blobs)
    held = view.read([1], arena, collection=blobs)[0]
    with pytest.raises(ustore.ContendedError) as refusal:
        view.read([2], arena, collection=blobs)
    assert refusal.value.status is ustore.Status.CONTENDED
    assert bytes(view.read([2], ustore.Arena(), collection=blobs)[0]) == b"second"
    assert bytes(held) == b"first"
    held.release()
    assert bytes(view.read([2], arena, collection=blobs)[0]) == b"second"


def test_a_rewound_arena_leaves_its_results_stale(view, blobs, arena):
    """A result read after a later call rewound its arena raises rather than reading what that call wrote."""
    view.write([1, 2], [b"first", b"second"], collection=blobs)
    stale = view.read([1], arena, collection=blobs)
    scanned = view.scan(0, 2, arena, collection=blobs)
    for read in (lambda: stale[0], lambda: stale.lengths, lambda: memoryview(stale)):
        with pytest.raises(ustore.UninitializedError) as refusal:
            read()
        assert refusal.value.status is ustore.Status.UNINITIALIZED
    assert len(stale) == 1
    assert scanned[0] == tuple(keys([1, 2]))


@pytest.mark.parametrize("refused", [None, b"bytes", 42], ids=["none", "bytes", "integer"])
def test_a_verb_takes_an_arena(view, blobs, refused):
    with pytest.raises(TypeError, match="Arena"):
        view.read([1], refused, collection=blobs)


@pytest.mark.parametrize(
    "tape_type", [ustore.Values, ustore.Keys, ustore.Columns, ustore.Neighborhoods, ustore.Deferred]
)
def test_tape_types_are_named_but_not_constructible(tape_type: type):
    with pytest.raises(TypeError):
        tape_type()


def test_whole_tape_exports_a_buffer(view, blobs, arena):
    view.write([1, 2], [b"ab", b"cd"], collection=blobs)
    assert bytes(memoryview(view.read([1, 2], arena, collection=blobs))) == b"abcd"
    found = view.read([1, 2], arena, collection=blobs)
    assert found.__dlpack_device__() == (1, 0)
    assert np.from_dlpack(found).tobytes() == b"abcd"


@pytest.mark.parametrize("spelling", ["binary", "large_binary", "string", "large_string", "u32", "u64"])
def test_a_tape_writes_as_it_is(view, blobs, arena, spelling: str):
    """An Arrow array or a pair of NumPy buffers reaches the ABI as the u32 or u64 tape it already is."""
    values = [b"a", b"", b"ccc"]
    if spelling in ("u32", "u64"):
        offsets = np.cumsum([0, *map(len, values)], dtype=np.uint32 if spelling == "u32" else np.uint64)
        tape = (np.frombuffer(b"".join(values), dtype=np.uint8), offsets)
    else:
        pyarrow = pytest.importorskip("pyarrow")
        texts = [value.decode() for value in values] if "string" in spelling else values
        tape = pyarrow.array(texts, type=getattr(pyarrow, spelling)())
    view.write([1, 2, 3], tape, collection=blobs)
    assert [bytes(value) for value in view.read([1, 2, 3], arena, collection=blobs)] == values


def test_values_write_back_as_the_tape_they_are(view, blobs, arena):
    view.write([1, 2], [b"first", b""], collection=blobs)
    view.write([3, 4], view.read([1, 2], arena, collection=blobs), collection=blobs)
    assert [bytes(value) for value in view.read([3, 4], arena, collection=blobs)] == [b"first", b""]


@pytest.mark.parametrize(
    "tape",
    [
        (b"ab", np.array([0, 2], dtype=np.uint32)),
        (b"ab", np.array([0, 2, 1], dtype=np.uint32)),
        (b"ab", np.array([0, 1, 3], dtype=np.uint64)),
    ],
    ids=["one-value", "falling", "past-the-data"],
)
def test_a_malformed_tape_is_refused(view, blobs, tape):
    with pytest.raises(ustore.InvalidArgumentError):
        view.write([1, 2], tape, collection=blobs)


def test_an_arrow_null_is_refused(view, blobs):
    """A null has no bytes to write, and removing keys takes a verb of its own."""
    pyarrow = pytest.importorskip("pyarrow")
    with pytest.raises(ustore.InvalidArgumentError, match="null"):
        view.write([1, 2], pyarrow.array([b"a", None]), collection=blobs)


def test_keys_export_as_16_byte_records(view, blobs, arena):
    view.write([5, 6], b"v", collection=blobs)
    found = view.scan(0, 2, arena, collection=blobs)
    assert found[0] == (key(5), key(6))
    exported = memoryview(found)
    assert (exported.format, exported.shape) == ("B", (2, 16))
    assert exported.tobytes() == key(5).bytes + key(6).bytes


@pytest.mark.parametrize("index", [1, -2, 5])
def test_rejects_an_index_past_the_batch(view, blobs, arena, index: int):
    view.set(1, b"a", collection=blobs)
    with pytest.raises(IndexError):
        view.read([1], arena, collection=blobs)[index]


# endregion Tapes and Arenas

# region Scans and Samples


@pytest.mark.parametrize(
    ("start", "count", "expected"),
    [(10, 6, [10, 20, 30, 40, 50, 60]), (20, 5, [20, 30, 40, 50, 60]), (30, 1, [30]), (40, 2, [40, 50]), (61, 5, [])],
)
def test_scan(view, blobs, arena, start: int, count: int, expected: list[int]):
    view.write([10, 20, 30, 40, 50, 60], b"v", collection=blobs)
    page = view.scan(start, count, arena, collection=blobs)
    assert list(page[0]) == keys(expected)
    assert page.counts == (len(expected),)


@pytest.mark.parametrize("page_size", [1, 2, 3, 7, 8])
def test_scan_paginates_from_the_last_key(view, blobs, arena, page_size: int):
    view.write(list(range(1, 8)), b"v", collection=blobs)
    walked: list[UUID] = []
    start = 0
    while page := list(view.scan(start, page_size, arena, collection=blobs)[0]):
        walked.extend(page)
        start = page[-1].int + 1
    assert walked == keys(range(1, 8))


@pytest.mark.parametrize("count", [0, 1, 3, 9, 20])
def test_sample(view, blobs, arena, count: int):
    view.write(list(range(1, 10)), b"v", collection=blobs)
    drawn = list(view.sample(count, arena, collection=blobs)[0])
    assert len(drawn) == len(set(drawn)) == min(count, 9)
    assert set(drawn) <= set(keys(range(1, 10)))


def test_measure(view, blobs, arena):
    view.write([1, 2, 3], [b"a", b"bb", b"ccc"], collection=blobs)
    bounds = view.measure(0, 100, arena, collection=blobs)
    assert isinstance(bounds, ustore.Measurement)
    assert bounds.max_cardinality >= bounds.min_cardinality
    assert bounds.max_value_bytes >= bounds.min_value_bytes


def test_scan_bounds_and_directions(view, blobs, arena):
    view.write([10, 20, 30, 40], b"v", collection=blobs)
    descending = ustore.ScanDirection.DESCENDING
    assert list(view.scan(10, 8, arena, collection=blobs, end=30)[0]) == keys([10, 20])
    assert list(view.scan(None, 8, arena, collection=blobs, direction=descending)[0]) == keys([40, 30, 20, 10])
    assert list(view.scan(30, 8, arena, collection=blobs, end=10, direction=descending)[0]) == keys([30, 20])
    with view.cursor(collection=blobs, end=20, direction=descending) as cursor:
        assert list(cursor.next(8, arena)) == keys([40, 30])


def test_ranges_remove_and_values_read_in_slices(view, blobs, arena):
    view.write([1, 2, 3], [b"alpha", b"beta", b"gamma"], collection=blobs)
    sliced = view.read([1, 3], arena, collection=blobs, offset=1, limit=2)
    assert [bytes(value) for value in sliced] == [b"lp", b"am"]
    view.remove(1, ends=3, collection=blobs)
    assert list(view.scan(0, 8, arena, collection=blobs)[0]) == keys([3])


def test_change_feeds_yield_writes_then_removals(engine, database, blobs, arena):
    if engine == "leveldb":
        with pytest.raises(ustore.UnsupportedError):
            database.changes_cursor(collection=blobs)
        return
    with database.snapshot() as head:
        since = head.read_stamp
    with database.changes_cursor(since, collection=blobs) as feed:
        database.set(1, b"a", collection=blobs)
        database.remove(1, collection=blobs)
        changes = feed.next(8, arena, values=True)
        assert list(changes.keys) == keys([1, 1])
        assert changes.kinds == (ustore.ChangeKind.WRITE, ustore.ChangeKind.REMOVAL)
        assert changes.stamps[0] < changes.stamps[1]
        assert bytes(changes.values[0]) == b"a"
        assert not feed.next(8, arena).keys


def test_scan_patterns_predicates_and_scores(view, blobs, arena):
    """Patterns narrow a scan by value, weighted substrings score BM25, and a predicate admits keys page by page."""
    view.write([1, 2, 3, 4], [b"Apple", b"banana", b"cherry", b"avocado"], collection=blobs)
    matched = view.scan(0, 8, arena, collection=blobs, patterns=["^a"], sensitivity=ustore.CaseSensitivity.UNCASED)
    assert list(matched[0]) == keys([1, 4])
    scored = view.scan(
        0, 8, arena, collection=blobs, patterns=[b"an"], pattern_kind=ustore.PatternKind.SUBSTRINGS, weights=[2.0]
    )
    assert list(scored[0]) == keys([2])
    assert scored.scores == pytest.approx((2.0 * 2.0 * 2.2 / (2.0 + 1.2),))
    asked: list[int] = []
    admitted = view.scan(
        0,
        2,
        arena,
        collection=blobs,
        predicate=lambda page: asked.append(len(page)) or [key.int % 2 == 0 for key in page],
        predicate_cost=ustore.PredicateCost.EXPENSIVE,
    )
    assert list(admitted[0]) == keys([2, 4])
    assert asked == [2, 2]
    with pytest.raises(ustore.InvalidArgumentError):
        view.scan(0, 8, arena, collection=blobs, patterns=["("])


# endregion Scans and Samples

# region Cursors


@pytest.mark.parametrize("page_size", [1, 3, 10, 11])
def test_a_cursor_walks_a_collection_whole(view, blobs, arena, page_size: int):
    """Pages resume past their last key, carry the values when asked, and an empty one ends the walk."""
    stored = [bytes([index]) * index for index in range(10)]
    view.write(list(range(10)), stored, collection=blobs)
    walked, values = [], []
    with view.cursor(collection=blobs) as cursor:
        while page := cursor.next(page_size, arena, values=True):
            walked.extend(page)
            values.extend(bytes(value) for value in page.values)
        assert not cursor.next(page_size, arena)
    assert (walked, values) == (keys(range(10)), stored)


def test_a_cursor_resumes_past_its_last_key(database, blobs, arena):
    """A key written behind the walk never comes, one ahead of it does, and a short page ends the walk for good."""
    database.write([2, 4, 6], b"v", collection=blobs)
    with database.cursor(1, collection=blobs) as cursor:
        assert list(cursor.next(2, arena)) == keys([2, 4])
        database.write([3, 5], b"v", collection=blobs)
        assert list(cursor.next(3, arena)) == keys([5, 6])
        database.write([7], b"v", collection=blobs)
        assert not cursor.next(3, arena)


def test_a_page_exports_its_keys_and_pins_its_arena(database, blobs, arena):
    database.write([5, 6], b"v", collection=blobs)
    with database.cursor(collection=blobs) as cursor:
        page = cursor.next(2, arena)
        assert page.values is None
        exported = memoryview(page)
        assert (exported.format, exported.shape) == ("B", (2, 16))
        assert exported.tobytes() == key(5).bytes + key(6).bytes
        with pytest.raises(ustore.ContendedError):
            cursor.next(2, arena)
        exported.release()
        assert not cursor.next(2, arena)


def test_a_cursor_pulls_on_its_receivers_executor(database, blobs):
    """Every pull blocks on the executor the cursor was opened through, which no deferred view offers, and a closed
    executor or cursor refuses the next pull."""
    database.write([1, 2, 3], b"v", collection=blobs)
    with ustore.Executor.for_ram(2) as executor:
        cursor = database.on(executor).cursor(collection=blobs)
        assert list(cursor.next(2, ustore.Arena(executor))) == keys([1, 2])
        assert not hasattr(database.deferred(executor), "cursor")
    with pytest.raises(ustore.UninitializedError):
        cursor.next(2, ustore.Arena())
    cursor.close()
    cursor.close()
    with pytest.raises(ustore.UninitializedError):
        cursor.next(2, ustore.Arena())


# endregion Cursors

# region Executors and Deferred Calls


def outcomes_of(calls: list[ustore.Deferred]) -> list[ustore.Status]:
    """The status every call finished with, waiting for each in turn."""
    outcomes = []
    for call in calls:
        try:
            call.wait()
        except ustore.Error as failure:
            outcomes.append(failure.status)
        else:
            outcomes.append(call.status)
    return outcomes


def test_a_deferred_read_answers_once_waited(database, blobs):
    database.write([1, 2], [b"a", b"bb"], collection=blobs)
    with ustore.Executor.for_ram(2) as executor:
        reading = database.deferred(executor).read([1, 2, 3], ustore.Arena(executor), collection=blobs)
        assert isinstance(reading, ustore.Deferred)
        assert reading.status in (ustore.Status.DEFERRED, ustore.Status.SUCCESS)
        assert [None if value is None else bytes(value) for value in reading.wait()] == [b"a", b"bb", None]
        assert reading.status is ustore.Status.SUCCESS


def test_a_deferred_call_holds_its_arena(database, blobs):
    """A deferred call holds its arena until it is waited on, then its result views the arena as a blocking one does."""
    database.write([1, 2], [b"a", b"bb"], collection=blobs)
    with ustore.Executor.for_ram(1) as executor:
        arena = ustore.Arena(executor)
        reading = database.deferred(executor).read([1, 2], arena, collection=blobs)
        with pytest.raises(ustore.ContendedError):
            database.read([1], arena, collection=blobs)
        with pytest.raises(ustore.ContendedError):
            database.deferred(executor).scan(0, 2, arena, collection=blobs)
        values = reading.wait()
        assert [bytes(value) for value in values] == [b"a", b"bb"]
        assert database.on(executor).read([2], arena, collection=blobs).lengths == (2,)
        with pytest.raises(ustore.UninitializedError):
            values[0]


def test_shorthand_on_a_deferred_view_owns_its_arena(database, blobs, arena):
    """`get`, `contains` and a mapping view answer a Deferred of the copy they make, through an arena of their own,
    each on an executor of its own, since an executor runs one call at a time."""
    database.write([1, 2], [b"a", b"bb"], collection=blobs)
    with contextlib.ExitStack() as executors:
        later = [database.deferred(executors.enter_context(ustore.Executor.for_ram(1))) for _ in range(4)]
        getting, holding, subscripting = (
            later[0].get(1, collection=blobs),
            later[1].contains(2, collection=blobs),
            later[2]["blobs"].blobs[[1, 3]],
        )
        held = database.read([2], arena, collection=blobs)[0]
        missing = later[3]["blobs"].blobs[3]
        assert (getting.wait(), holding.wait(), subscripting.wait()) == (b"a", True, [b"a", None])
        assert later[0]["blobs"].blobs[2].wait() == b"bb"
        with pytest.raises(KeyError):
            missing.wait()
        assert bytes(held) == b"bb"


def test_await_under_asyncio(database, blobs, arena):
    """Gathered calls run at once, so each takes an executor of its own."""
    with contextlib.ExitStack() as executors:
        later = [database.deferred(executors.enter_context(ustore.Executor.for_ram(1))) for _ in range(4)]

        async def main() -> tuple[list[bytes], list[bytes | None]]:
            await later[0].write([1, 2], [b"a", b"bb"], collection=blobs)
            values = await later[0].read([1, 2], arena, collection=blobs)
            found = await asyncio.gather(*(later[key].get(key, collection=blobs) for key in range(4)))
            return [bytes(value) for value in values], found

        assert asyncio.run(main()) == ([b"a", b"bb"], [None, b"a", b"bb", None])


def test_await_raises_what_wait_raises(database, blobs):
    """Awaiting runs `wait()` on the loop's own executor, so a refusal reaches the task as it would the thread."""
    with ustore.Executor.for_ram(1) as executor:
        missing = database.deferred(executor)["blobs"].blobs[404]

        async def main() -> bytes:
            return await missing

        with pytest.raises(KeyError):
            asyncio.run(main())
        assert missing.status is ustore.Status.SUCCESS


def test_a_deferred_call_finishes_by_polling(database, blobs):
    """The executor's own thread serves a deferred call, so polling its status alone sees it final."""
    with ustore.Executor.for_ram(2) as executor:
        writing = database.deferred(executor).write([1], [b"a"], collection=blobs)
        while writing.status is ustore.Status.DEFERRED:
            pass
        on = database.on(executor)
        assert on.get(1, collection=blobs) == b"a"
        assert on["blobs"].blobs[1] == b"a"
        assert writing.wait() is None


def test_a_held_executor_refuses_the_next_call(database, blobs):
    """An executor serves one call at a time, so submissions finding it held raise once waited for."""
    with ustore.Executor.for_ram(1) as executor:
        later = database.deferred(executor)
        calls = [later.write(list(range(256)), b"v" * 1024, collection=blobs) for _ in range(64)]
        outcomes = outcomes_of(calls)
    assert ustore.Status.CONTENDED in outcomes
    assert set(outcomes) <= {ustore.Status.SUCCESS, ustore.Status.CONTENDED}


def test_a_drain_frees_the_executor_in_either_mood(database, blobs):
    """A drain only waits until no call holds the executor, so the next call on it runs, whichever mood it left."""
    with ustore.Executor.for_ram(2) as executor:
        for mood in (ustore.Mood.SPINNING, ustore.Mood.PARKED):
            writing = database.deferred(executor).write(list(range(256)), b"v" * 1024, collection=blobs)
            executor.drain(mood)
            assert database.on(executor).get(255, collection=blobs) == b"v" * 1024
            assert writing.wait() is None


def test_close_waits_for_deferred_calls(open_database):
    """Freeing a database waits in C for the deferred calls naming it, so closing one never refuses."""
    database = open_database()
    blobs = database.collection_create("blobs", modality=ustore.Modality.BLOBS)
    with ustore.Executor.for_ram(1) as executor:
        writing = database.deferred(executor).write(list(range(4096)), b"v" * 1024, collection=blobs)
        database.close()
        assert writing.status is ustore.Status.SUCCESS
        assert writing.wait() is None


def test_an_executor_serves_the_memory_domains_named(database, blobs, tmp_path):
    """Every host has NUMA node zero, an identifier no node carries is refused, and so is a null pool."""
    with ustore.Executor.for_ram(threads=1, memory_domains=[0]) as executor:
        database.on(executor).set(1, b"a", collection=blobs)
        assert database.deferred(executor).get(1, collection=blobs).wait() == b"a"
    with pytest.raises(ustore.InvalidArgumentError):
        ustore.Executor.for_ram(memory_domains=[1 << 20])
    with pytest.raises(ustore.InvalidArgumentError):
        ustore.Executor.on_pool(0)
    # Merges take a VRAM executor alone, and a build without NVMe refuses the open before reading it
    with (
        ustore.Executor.for_nvme(1, transport=ustore.Transport.BLOCKING, queue_depth=32) as executor,
        pytest.raises((ustore.DeviceError, ustore.UnsupportedError)),
    ):
        ustore.Database.open_nvme(location("nvme", tmp_path), compactions=executor)
    # A build without CUDA refuses every device, and one with CUDA refuses an ordinal no device carries
    with pytest.raises((ustore.UnsupportedError, ustore.DeviceError)):
        ustore.Executor.for_vram(1 << 20, gds=ustore.Gds.COMPATIBILITY)


def test_a_database_opens_on_an_executor(engine: str, tmp_path):
    """A constructor given an executor blocks on it, and one given a closed executor opens nothing."""
    with (
        ustore.Executor.for_ram(2) as executor,
        getattr(ustore.Database, f"open_{engine}")(location(engine, tmp_path), executor=executor) as database,
    ):
        blobs = database.collection_create("blobs", modality=ustore.Modality.BLOBS)
        database.set(1, b"a", collection=blobs)
    with pytest.raises(ustore.UninitializedError):
        getattr(ustore.Database, f"open_{engine}")(location(engine, tmp_path), executor=executor)
    with getattr(ustore.Database, f"open_{engine}")(location(engine, tmp_path)) as reopened:
        assert reopened.get(1, collection=blobs) == b"a"


def test_the_collector_frees_a_cycle_through_a_finished_deferred(database, blobs):
    """A cycle through what a final Deferred raised is garbage the collector frees, rather than a leak."""

    class Holder:
        call: ustore.Deferred[bytes]

    with ustore.Executor.for_ram(1) as executor:
        holder = Holder()
        holder.call = database.deferred(executor)["blobs"].blobs[404]
        with pytest.raises(KeyError) as raised:
            holder.call.wait()
        raised.value.holder = holder
        alive = weakref.ref(holder)
        del holder, raised
        gc.collect()
        assert alive() is None


def test_a_deferred_contains_answers_a_bool_once_waited(view, blobs, later):
    view.set(1, b"a", collection=blobs)
    assert later.contains(1, collection=blobs).wait() is True
    assert later.contains(2, collection=blobs).wait() is False


def test_a_deferred_view_names_the_verb_it_takes_instead(database, blobs, docs):
    """What cannot answer a Deferred raises TypeError naming the flat verb to call."""
    with ustore.Executor.for_ram(1) as executor:
        values, documents = database.deferred(executor)["blobs"], database.deferred(executor)["docs"]
        with pytest.raises(TypeError, match="write"):
            values.blobs[1] = b"a"
        with pytest.raises(TypeError, match="docs_write"):
            documents.docs[1] = {"a": 1}
        with pytest.raises(TypeError, match="remove"):
            del values.blobs[1]
        with pytest.raises(TypeError, match="contains"):
            _ = 1 in values.blobs
        with pytest.raises(TypeError, match="contains"):
            _ = 1 in documents.docs
        with pytest.raises(TypeError, match="docs_gather"):
            _ = documents.table


# endregion Executors and Deferred Calls

# region Refusals


@pytest.mark.parametrize(
    ("call", "failure"),
    [
        (lambda view: view.get(1, colection=None, collection="blobs"), TypeError),
        (lambda view: view.set(1, 42, collection="blobs"), TypeError),
        (lambda view: view.set(1, "text", collection="blobs"), TypeError),
        (lambda view: view.get("one", collection="blobs"), ValueError),
        (lambda view: view.get(b"short", collection="blobs"), ValueError),
        (lambda view: view.get(1.0, collection="blobs"), TypeError),
        (lambda view: view.read(1, ustore.Arena(), collection="blobs"), TypeError),
        (lambda view: view.read(str(key(1)), ustore.Arena(), collection="blobs"), TypeError),
        (lambda view: view.read([1], collection="blobs"), TypeError),
        (lambda view: view.scan(0, -1, ustore.Arena(), collection="blobs"), ValueError),
        (lambda view: view.get(collection="blobs"), TypeError),
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
        "no-arena",
        "negative-count",
        "no-key",
    ],
)
def test_rejects_malformed_arguments(view, blobs, call, failure: type):
    with pytest.raises(failure):
        call(view)


def test_refuses_a_closed_database(open_database):
    closed = open_database()
    blobs = closed.collection_create("blobs", modality=ustore.Modality.BLOBS)
    closed.close()
    with pytest.raises(ustore.UninitializedError):
        closed.get(1, collection=blobs)


def test_closing_twice_is_safe(open_database):
    database = open_database()
    database.close()
    database.close()


# endregion Refusals
