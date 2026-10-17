"""Binary values over the C ABI: single and batch access, scans, samples, measurements and deferred calls."""

import asyncio
import gc
import weakref
from uuid import UUID

import numpy as np
import pytest
from capabilities import BATCH_SIZES, key, keys

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
def test_single_round_trip(view, shape: str):
    """A value reads back byte for byte, whatever its length."""
    value = VALUE_SHAPES[shape]
    view.set(3, value)
    assert view.contains(3)
    assert view.get(3) == value
    view.remove(3)
    assert not view.contains(3)
    assert view.get(3) is None


@pytest.mark.parametrize(("first", "second"), [(b"y", b"jjjjjjjj"), (b"jjjjjjjj", b"yy"), (b"", b"z"), (b"z", b"")])
def test_overwrite_changes_length(view, first: bytes, second: bytes):
    """An overwrite replaces the whole value, growing or shrinking it."""
    view.set(7, first)
    view.set(7, second)
    assert view.get(7) == second


def test_set_refuses_none(view):
    """A None never removes, since `remove` does."""
    view.set(9, b"here")
    with pytest.raises(TypeError, match="remove"):
        view.set(9, None)
    assert view.get(9) == b"here"


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
def test_every_spelling_of_a_key_addresses_one_value(view, arena, spelling):
    view.set(spelling, b"v")
    assert view.get(5) == b"v"
    assert view.scan(0, 2, arena)[0] == (key(5),)


# endregion Single Access

# region Batches


@pytest.mark.parametrize("count", BATCH_SIZES)
@pytest.mark.parametrize("container", KEY_CONTAINERS)
def test_batch_round_trip(view, arena, count: int, container: str):
    """A batch reads back in the order it was asked for, borrowed as 16-byte keys or converted from integers."""
    keys = KEY_CONTAINERS[container](list(range(count)))
    values = [str(key).encode() * (key % 3 + 1) for key in range(count)]
    view.write(keys, values)
    found = view.read(keys, arena)
    assert len(found) == count
    assert [bytes(value) for value in found] == values
    assert found.lengths == tuple(len(value) for value in values)


@pytest.mark.parametrize("missing", [[0], [1], [0, 2], [0, 1, 2]])
def test_batch_reports_missing_keys(view, arena, missing: list[int]):
    """An absent key reads as None in its own position and nowhere else."""
    present = [key for key in range(3) if key not in missing]
    view.write(present, [b"v%d" % key for key in present])
    found = view.read([0, 1, 2], arena)
    assert [value is None for value in found] == [key in missing for key in range(3)]
    assert found.lengths == tuple(None if key in missing else 2 for key in range(3))


@pytest.mark.parametrize("count", BATCH_SIZES)
def test_batch_broadcasts_one_value(view, arena, count: int):
    view.write(list(range(count)), b"same")
    assert [bytes(value) for value in view.read(list(range(count)), arena)] == [b"same"] * count


@pytest.mark.parametrize("count", BATCH_SIZES)
def test_batch_removes_every_key(view, arena, count: int):
    keys = list(range(count))
    view.write(keys, b"value")
    view.remove(keys)
    assert all(value is None for value in view.read(keys, arena))


@pytest.mark.parametrize("values", [[None, b"kept"], [b"kept", None], None], ids=["first", "second", "whole"])
def test_none_in_a_batch_is_refused(view, arena, values):
    """A None inside a written batch is refused before anything is written, rather than read as a removal."""
    view.write([30, 31], [b"a", b"b"])
    with pytest.raises(TypeError):
        view.write([30, 31], values)
    assert [bytes(value) for value in view.read([30, 31], arena)] == [b"a", b"b"]


@pytest.mark.parametrize("keys", [[40], [40, 41], np.array([40, 41], dtype=np.int64)], ids=["one", "two", "numpy"])
def test_batch_remove(view, keys):
    view.write([40, 41], [b"a", b"b"])
    view.remove(keys)
    assert not any(view.contains(int(key)) for key in keys)


def test_remove_takes_a_bare_key(view):
    view.set(40, b"a")
    view.remove(40)
    assert not view.contains(40)


@pytest.mark.parametrize("values", [[b"a", b"b"], [b"a", b"b", b"c", b"d"]], ids=["short", "long"])
def test_rejects_mismatched_batch_widths(view, values: list[bytes]):
    with pytest.raises(ValueError):
        view.write([1, 2, 3], values)


# endregion Batches

# region Tapes and Arenas


def test_values_are_borrowed_not_copied(view, arena):
    view.set(1, b"borrowed")
    found = view.read([1], arena)[0]
    assert isinstance(found, memoryview)
    assert found.readonly
    assert bytes(found) == b"borrowed"


def test_a_view_outlives_the_tape_that_exported_it(view, arena):
    view.set(1, b"borrowed")
    held = view.read([1], arena)[0]
    gc.collect()
    assert bytes(held) == b"borrowed"


def test_one_arena_serves_every_verb_in_turn(view, arena):
    """Each verb rewinds the arena it is handed, so one arena serves reads, scans and samples one after another."""
    view.write([1, 2, 3], [b"a", b"bb", b"ccc"])
    assert [bytes(value) for value in view.read([1, 2], arena)] == [b"a", b"bb"]
    assert view.scan(0, 3, arena)[0] == tuple(keys([1, 2, 3]))
    assert set(view.sample(2, arena)[0]) <= set(keys([1, 2, 3]))
    assert isinstance(view.measure(0, 100, arena), ustore.Measurement)
    assert view.read([3], arena).lengths == (3,)


def test_an_exported_buffer_pins_its_arena(view, arena):
    """A memoryview over an output pins its arena, as one over a bytearray pins it, until the view is released."""
    view.write([1, 2], [b"first", b"second"])
    held = view.read([1], arena)[0]
    with pytest.raises(ustore.ContendedError) as refusal:
        view.read([2], arena)
    assert refusal.value.status is ustore.Status.CONTENDED
    assert bytes(view.read([2], ustore.Arena())[0]) == b"second"
    assert bytes(held) == b"first"
    held.release()
    assert bytes(view.read([2], arena)[0]) == b"second"


def test_a_rewound_arena_leaves_its_results_stale(view, arena):
    """A result read after a later call rewound its arena raises rather than reading what that call wrote."""
    view.write([1, 2], [b"first", b"second"])
    stale = view.read([1], arena)
    scanned = view.scan(0, 2, arena)
    for read in (lambda: stale[0], lambda: stale.lengths, lambda: memoryview(stale)):
        with pytest.raises(ustore.UninitializedError) as refusal:
            read()
        assert refusal.value.status is ustore.Status.UNINITIALIZED
    assert len(stale) == 1
    assert scanned[0] == tuple(keys([1, 2]))


@pytest.mark.parametrize("refused", [None, b"bytes", 42], ids=["none", "bytes", "integer"])
def test_a_verb_takes_an_arena(view, refused):
    with pytest.raises(TypeError, match="Arena"):
        view.read([1], refused)


@pytest.mark.parametrize(
    "tape_type", [ustore.Values, ustore.Keys, ustore.Columns, ustore.Neighborhoods, ustore.Deferred]
)
def test_tape_types_are_named_but_not_constructible(tape_type: type):
    with pytest.raises(TypeError):
        tape_type()


def test_whole_tape_exports_a_buffer(view, arena):
    view.write([1, 2], [b"ab", b"cd"])
    assert bytes(memoryview(view.read([1, 2], arena))) == b"abcd"


@pytest.mark.parametrize("spelling", ["binary", "large_binary", "string", "large_string", "u32", "u64"])
def test_a_tape_writes_as_it_is(view, arena, spelling: str):
    """An Arrow array or a pair of NumPy buffers reaches the ABI as the u32 or u64 tape it already is."""
    values = [b"a", b"", b"ccc"]
    if spelling in ("u32", "u64"):
        offsets = np.cumsum([0, *map(len, values)], dtype=np.uint32 if spelling == "u32" else np.uint64)
        tape = (np.frombuffer(b"".join(values), dtype=np.uint8), offsets)
    else:
        pyarrow = pytest.importorskip("pyarrow")
        texts = [value.decode() for value in values] if "string" in spelling else values
        tape = pyarrow.array(texts, type=getattr(pyarrow, spelling)())
    view.write([1, 2, 3], tape)
    assert [bytes(value) for value in view.read([1, 2, 3], arena)] == values


def test_values_write_back_as_the_tape_they_are(view, arena):
    view.write([1, 2], [b"first", b""])
    view.write([3, 4], view.read([1, 2], arena))
    assert [bytes(value) for value in view.read([3, 4], arena)] == [b"first", b""]


@pytest.mark.parametrize(
    "tape",
    [
        (b"ab", np.array([0, 2], dtype=np.uint32)),
        (b"ab", np.array([0, 2, 1], dtype=np.uint32)),
        (b"ab", np.array([0, 1, 3], dtype=np.uint64)),
    ],
    ids=["one-value", "falling", "past-the-data"],
)
def test_a_malformed_tape_is_refused(view, tape):
    with pytest.raises(ustore.InvalidArgumentError):
        view.write([1, 2], tape)


def test_an_arrow_null_is_refused(view):
    """A null has no bytes to write, and removing keys takes a verb of its own."""
    pyarrow = pytest.importorskip("pyarrow")
    with pytest.raises(ustore.InvalidArgumentError, match="null"):
        view.write([1, 2], pyarrow.array([b"a", None]))


def test_keys_export_as_16_byte_records(view, arena):
    view.write([5, 6], b"v")
    found = view.scan(0, 2, arena)
    assert found[0] == (key(5), key(6))
    exported = memoryview(found)
    assert (exported.format, exported.shape) == ("B", (2, 16))
    assert exported.tobytes() == key(5).bytes + key(6).bytes


@pytest.mark.parametrize("index", [1, -2, 5])
def test_rejects_an_index_past_the_batch(view, arena, index: int):
    view.set(1, b"a")
    with pytest.raises(IndexError):
        view.read([1], arena)[index]


# endregion Tapes and Arenas

# region Scans and Samples


@pytest.mark.parametrize(
    ("start", "count", "expected"),
    [(10, 6, [10, 20, 30, 40, 50, 60]), (20, 5, [20, 30, 40, 50, 60]), (30, 1, [30]), (40, 2, [40, 50]), (61, 5, [])],
)
def test_scan(view, arena, start: int, count: int, expected: list[int]):
    view.write([10, 20, 30, 40, 50, 60], b"v")
    page = view.scan(start, count, arena)
    assert list(page[0]) == keys(expected)
    assert page.counts == (len(expected),)


@pytest.mark.parametrize("page_size", [1, 2, 3, 7, 8])
def test_scan_paginates_from_the_last_key(view, arena, page_size: int):
    view.write(list(range(1, 8)), b"v")
    walked: list[UUID] = []
    start = 0
    while page := list(view.scan(start, page_size, arena)[0]):
        walked.extend(page)
        start = page[-1].int + 1
    assert walked == keys(range(1, 8))


@pytest.mark.parametrize("count", [0, 1, 3, 9, 20])
def test_sample(view, arena, count: int):
    view.write(list(range(1, 10)), b"v")
    drawn = list(view.sample(count, arena)[0])
    assert len(drawn) == len(set(drawn)) == min(count, 9)
    assert set(drawn) <= set(keys(range(1, 10)))


def test_measure(view, arena):
    view.write([1, 2, 3], [b"a", b"bb", b"ccc"])
    bounds = view.measure(0, 100, arena)
    assert isinstance(bounds, ustore.Measurement)
    assert bounds.max_cardinality >= bounds.min_cardinality
    assert bounds.max_value_bytes >= bounds.min_value_bytes


# endregion Scans and Samples

# region Cursors


@pytest.mark.parametrize("page_size", [1, 3, 10, 11])
def test_a_cursor_walks_a_collection_whole(view, arena, page_size: int):
    """Pages resume past their last key, carry the values when asked, and an empty one ends the walk."""
    stored = [bytes([index]) * index for index in range(10)]
    view.write(list(range(10)), stored)
    walked, values = [], []
    with view.cursor() as cursor:
        while page := cursor.next(page_size, arena, values=True):
            walked.extend(page)
            values.extend(bytes(value) for value in page.values)
        assert not cursor.next(page_size, arena)
    assert (walked, values) == (keys(range(10)), stored)


def test_a_cursor_resumes_past_its_last_key(database, arena):
    """A key written behind the walk never comes, one ahead of it does, and a short page ends the walk for good."""
    database.write([2, 4, 6], b"v")
    with database.cursor(1) as cursor:
        assert list(cursor.next(2, arena)) == keys([2, 4])
        database.write([3, 5], b"v")
        assert list(cursor.next(3, arena)) == keys([5, 6])
        database.write([7], b"v")
        assert not cursor.next(3, arena)


def test_a_page_exports_its_keys_and_pins_its_arena(database, arena):
    database.write([5, 6], b"v")
    with database.cursor() as cursor:
        page = cursor.next(2, arena)
        assert page.values is None
        exported = memoryview(page)
        assert (exported.format, exported.shape) == ("B", (2, 16))
        assert exported.tobytes() == key(5).bytes + key(6).bytes
        with pytest.raises(ustore.ContendedError):
            cursor.next(2, arena)
        exported.release()
        assert not cursor.next(2, arena)


def test_a_cursor_pulls_on_its_receivers_executor(database):
    """Every pull blocks on the executor the cursor was opened through, which no deferred view offers, and a closed
    executor or cursor refuses the next pull."""
    database.write([1, 2, 3], b"v")
    with ustore.Executor.for_ram(2) as executor:
        cursor = database.on(executor).cursor()
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


def test_a_deferred_read_answers_once_waited(database):
    database.write([1, 2], [b"a", b"bb"])
    with ustore.Executor.for_ram(2) as executor:
        reading = database.deferred(executor).read([1, 2, 3], ustore.Arena(executor))
        assert isinstance(reading, ustore.Deferred)
        assert reading.status in (ustore.Status.DEFERRED, ustore.Status.SUCCESS)
        assert [None if value is None else bytes(value) for value in reading.wait()] == [b"a", b"bb", None]
        assert reading.status is ustore.Status.SUCCESS


def test_a_deferred_call_holds_its_arena(database):
    """A deferred call holds its arena until it is waited on, then its result views the arena as a blocking one does."""
    database.write([1, 2], [b"a", b"bb"])
    with ustore.Executor.for_ram(1) as executor:
        arena = ustore.Arena(executor)
        reading = database.deferred(executor).read([1, 2], arena)
        with pytest.raises(ustore.ContendedError):
            database.read([1], arena)
        with pytest.raises(ustore.ContendedError):
            database.deferred(executor).scan(0, 2, arena)
        values = reading.wait()
        assert [bytes(value) for value in values] == [b"a", b"bb"]
        assert database.on(executor).read([2], arena).lengths == (2,)
        with pytest.raises(ustore.UninitializedError):
            values[0]


def test_shorthand_on_a_deferred_view_owns_its_arena(database, arena):
    """`get`, `contains` and a mapping view answer a Deferred of the copy they make, through an arena of their own."""
    database.write([1, 2], [b"a", b"bb"])
    with ustore.Executor.for_ram(2) as executor:
        later = database.deferred(executor)
        getting, holding, subscripting = later.get(1), later.contains(2), later[""].blobs[[1, 3]]
        held = database.read([2], arena)[0]
        missing = later[""].blobs[3]
        assert (getting.wait(), holding.wait(), subscripting.wait()) == (b"a", True, [b"a", None])
        assert later[""].blobs[2].wait() == b"bb"
        with pytest.raises(KeyError):
            missing.wait()
        assert bytes(held) == b"bb"


def test_await_under_asyncio(database, arena):
    with ustore.Executor.for_ram(2) as executor:
        later = database.deferred(executor)

        async def main() -> tuple[list[bytes], list[bytes | None]]:
            await later.write([1, 2], [b"a", b"bb"])
            values = await later.read([1, 2], arena)
            found = await asyncio.gather(*(later.get(key) for key in range(4)))
            return [bytes(value) for value in values], found

        assert asyncio.run(main()) == ([b"a", b"bb"], [None, b"a", b"bb", None])


def test_await_raises_what_wait_raises(database):
    """Awaiting runs `wait()` on the loop's own executor, so a refusal reaches the task as it would the thread."""
    with ustore.Executor.for_ram(1) as executor:
        missing = database.deferred(executor)[""].blobs[404]

        async def main() -> bytes:
            return await missing

        with pytest.raises(KeyError):
            asyncio.run(main())
        assert missing.status is ustore.Status.SUCCESS


def test_a_view_blocks_after_earlier_deferred_calls(database):
    with ustore.Executor.for_ram(2) as executor:
        writing = database.deferred(executor).write([1], [b"a"])
        on = database.on(executor)
        assert on.get(1) == b"a"
        assert on[""].blobs[1] == b"a"
        assert writing.wait() is None


def test_a_full_deferred_table_refuses_the_next_call(database):
    """A table of one call refuses the submissions that find it full, which raise once waited for."""
    with ustore.Executor.for_ram(1, deferred_limit=1) as executor:
        later = database.deferred(executor)
        calls = [later.write(list(range(256)), b"v" * 1024) for _ in range(64)]
        outcomes = outcomes_of(calls)
    assert ustore.Status.CONTENDED in outcomes
    assert set(outcomes) <= {ustore.Status.SUCCESS, ustore.Status.CONTENDED}


def test_close_waits_for_deferred_calls(open_database):
    """Freeing a database waits in C for the deferred calls naming it, so closing one never refuses."""
    database = open_database()
    with ustore.Executor.for_ram(1) as executor:
        writing = database.deferred(executor).write(list(range(4096)), b"v" * 1024)
        database.close()
        assert writing.status is ustore.Status.SUCCESS
        assert writing.wait() is None


def test_an_executor_serves_the_memory_domains_named(database):
    """Every host has NUMA node zero, an identifier no node carries is refused, and so is a null pool."""
    with ustore.Executor.for_ram(threads=1, memory_domains=[0], deferred_limit=4) as executor:
        database.on(executor).set(1, b"a")
        assert database.deferred(executor).get(1).wait() == b"a"
    with pytest.raises(ustore.InvalidArgumentError):
        ustore.Executor.for_ram(memory_domains=[1 << 20])
    with pytest.raises(ustore.InvalidArgumentError):
        ustore.Executor.on_pool(0)
    with (
        ustore.Executor.for_nvme(1, transport=ustore.Transport.BLOCKING, queue_depth=32) as executor,
        pytest.raises(ustore.DeviceError),
    ):
        ustore.Executor.for_nvme(vram=executor)
    # A build without CUDA refuses every device, and one with CUDA refuses an ordinal no device carries
    with pytest.raises((ustore.UnsupportedError, ustore.DeviceError)):
        ustore.Executor.for_vram([1 << 20])


def test_a_database_opens_on_an_executor(engine: str, tmp_path):
    """A constructor given an executor blocks on it, and one given a closed executor opens nothing."""
    with (
        ustore.Executor.for_ram(2) as executor,
        getattr(ustore.Database, f"open_{engine}")(tmp_path, executor=executor) as database,
    ):
        database.set(1, b"a")
    with pytest.raises(ustore.UninitializedError):
        getattr(ustore.Database, f"open_{engine}")(tmp_path, executor=executor)
    with getattr(ustore.Database, f"open_{engine}")(tmp_path) as reopened:
        assert reopened.get(1) == b"a"


def test_the_collector_frees_a_cycle_through_a_finished_deferred(database):
    """A cycle through what a final Deferred raised is garbage the collector frees, rather than a leak."""

    class Holder:
        call: ustore.Deferred[bytes]

    with ustore.Executor.for_ram(1) as executor:
        holder = Holder()
        holder.call = database.deferred(executor)[""].blobs[404]
        with pytest.raises(KeyError) as raised:
            holder.call.wait()
        raised.value.holder = holder
        alive = weakref.ref(holder)
        del holder, raised
        gc.collect()
        assert alive() is None


def test_a_deferred_contains_answers_a_bool_once_waited(view, later):
    view.set(1, b"a")
    assert later.contains(1).wait() is True
    assert later.contains(2).wait() is False


def test_a_deferred_view_names_the_verb_it_takes_instead(database):
    """What cannot answer a Deferred raises TypeError naming the flat verb to call."""
    with ustore.Executor.for_ram(1) as executor:
        collection = database.deferred(executor)[""]
        with pytest.raises(TypeError, match="write"):
            collection.blobs[1] = b"a"
        with pytest.raises(TypeError, match="docs_write"):
            collection.docs[1] = {"a": 1}
        with pytest.raises(TypeError, match="remove"):
            del collection.blobs[1]
        with pytest.raises(TypeError, match="contains"):
            _ = 1 in collection.blobs
        with pytest.raises(TypeError, match="contains"):
            _ = 1 in collection.docs
        with pytest.raises(TypeError, match="docs_gather"):
            _ = collection.table


# endregion Executors and Deferred Calls

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
        (lambda view: view.read(1, ustore.Arena()), TypeError),
        (lambda view: view.read(str(key(1)), ustore.Arena()), TypeError),
        (lambda view: view.read([1]), TypeError),
        (lambda view: view.scan(0, -1, ustore.Arena()), ValueError),
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
        "no-arena",
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
    with pytest.raises(ustore.UninitializedError):
        closed.get(1)


def test_closing_twice_is_safe(open_database):
    database = open_database()
    database.close()
    database.close()


# endregion Refusals
