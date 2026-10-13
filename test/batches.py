"""Batch scans and samples: many tasks and one `wanted` in one call, against the single form beside them, and the
progress a scan reports."""

import threading

import numpy as np
import pytest
from capabilities import keys

import ustore

KEYS = list(range(1, 33))


@pytest.fixture
def populated(view):
    """A view holding one short value per key of `KEYS`."""
    view.write(KEYS, [bytes([key % 251]) for key in KEYS])
    return view


def runs(found) -> list[list[int]]:
    """Every task's run of keys, as lists of the integers they spell."""
    return [[key.int for key in found[task]] for task in range(len(found))]


# region Batch Scans


def test_scan_batch_matches_the_single_form(populated, arena):
    """Every scan answers what the same scan alone would have, each held to the one `wanted`."""
    starts = [1, 9, 25]
    batched = populated.scan_batch(starts, 4, arena)
    assert batched.counts == (4, 4, 4)
    assert runs(batched) == [runs(populated.scan(start, 4, arena))[0] for start in starts]


def test_scan_batch_takes_a_buffer_of_starts(populated, arena):
    """Starts arrive through the buffer protocol, as every other batch of keys does."""
    found = populated.scan_batch(np.array([2, 10], dtype=np.int64), 2, arena)
    assert runs(found) == [[2, 3], [10, 11]]


def test_scan_batch_past_the_last_key(populated, arena):
    """A scan starting past every key answers an empty run rather than a failure."""
    found = populated.scan_batch([30, 1000], 8, arena)
    assert runs(found) == [[30, 31, 32], []]


def test_scan_batch_of_no_scans(populated, arena):
    """An empty batch answers an empty tape, which no ABI call was made for."""
    found = populated.scan_batch([], 4, arena)
    assert len(found) == 0
    assert found.counts == ()


def test_scan_batch_repeats_a_start(populated, arena):
    """Nothing makes the scans distinct, so a repeated range answers twice."""
    found = populated.scan_batch([4, 4], 3, arena)
    assert runs(found) == [[4, 5, 6], [4, 5, 6]]


def test_a_deferred_scan_batch_answers_once_waited(populated, later, arena):
    found = later.scan_batch([1, 9], 3, arena).wait()
    assert runs(found) == runs(populated.scan_batch([1, 9], 3, ustore.Arena()))
    assert len(later.scan_batch([], 3, arena).wait()) == 0


def test_an_exported_tape_pins_its_arena(populated, arena):
    """An exported tape outlives the calls after it, which another arena serves while this one is pinned."""
    exported = memoryview(populated.scan_batch([1, 9], 2, arena))
    with pytest.raises(ustore.ContendedError):
        populated.scan(1, 100, arena)
    populated.scan(1, 100, ustore.Arena())
    assert exported.tobytes() == b"".join(key.bytes for key in keys([1, 2, 9, 10]))


@pytest.mark.parametrize(
    ("call", "failure"),
    [
        (lambda view: view.scan_batch([1], 2**64, ustore.Arena()), ValueError),
        (lambda view: view.scan_batch([1], -1, ustore.Arena()), ValueError),
        (lambda view: view.scan_batch([1], [4], ustore.Arena()), TypeError),
        (lambda view: view.scan_batch([1], "four", ustore.Arena()), TypeError),
        (lambda view: view.scan_batch([1], 1, ustore.Arena(), start=0), TypeError),
    ],
    ids=["huge-wanted", "negative-wanted", "listed-wanted", "text-wanted", "unknown-keyword"],
)
def test_scan_batch_refusals(populated, call, failure):
    """A batch whose `wanted` is not one count is refused."""
    with pytest.raises(failure):
        call(populated)


# endregion Batch Scans

# region Batch Samples


def test_sample_batch_draws_one_run_per_sample(populated, arena):
    """Every sample draws its own keys, never more than `wanted`, and all of them are stored."""
    found = populated.sample_batch(3, 4, arena)
    assert len(found) == 3
    for task in range(3):
        drawn = list(found[task])
        assert len(drawn) <= 4
        assert set(drawn) <= set(keys(KEYS))


def test_sample_batch_of_no_samples(populated, arena):
    """An empty batch answers an empty tape, as an empty scan does."""
    found = populated.sample_batch(0, 4, arena)
    assert len(found) == 0


def test_a_deferred_sample_batch_answers_once_waited(populated, later, arena):
    found = later.sample_batch(2, 3, arena).wait()
    assert len(found) == 2
    assert set(found[0]) | set(found[1]) <= set(keys(KEYS))


def test_sample_batch_of_an_empty_collection(view, arena):
    """Nothing to draw from answers empty runs rather than a failure."""
    found = view.sample_batch(2, 3, arena)
    assert runs(found) == [[], []]


@pytest.mark.parametrize(
    ("call", "failure"),
    [
        (lambda view: view.sample_batch([4], 2, ustore.Arena()), TypeError),
        (lambda view: view.sample_batch(-2, 2, ustore.Arena()), ValueError),
        (lambda view: view.sample_batch(2, 2**64, ustore.Arena()), ValueError),
        (lambda view: view.sample_batch(2, ustore.Arena()), TypeError),
    ],
    ids=["listed-count", "negative-count", "huge-wanted", "no-wanted"],
)
def test_sample_batch_refusals(populated, call, failure):
    """A sample batch takes a count of samples and one `wanted`, and nothing that is not one."""
    with pytest.raises(failure):
        call(populated)


# endregion Batch Samples

# region Long Batches


@pytest.mark.parametrize("scans", [1, 63, 64, 65, 200])
def test_scan_batch_outgrows_the_stack(populated, arena, scans: int):
    """Batches below, at and past the starts one batch lays out on the stack all answer the same."""
    found = populated.scan_batch([1] * scans, 2, arena)
    assert len(found) == scans
    assert runs(found) == [[1, 2]] * scans


# endregion Long Batches

# region Progress


def test_progress_counts_every_scan(populated, arena):
    """Reports never go back, and an answer of None carries on to the last one, which finishes every scan."""
    reports: list[tuple[int, int]] = []
    found = populated.scan_batch([1, 9, 17, 25], 2, arena, progress=lambda *report: reports.append(report))
    assert runs(found) == [[1, 2], [9, 10], [17, 18], [25, 26]]
    assert reports[-1] == (4, 4)
    assert [finished for finished, _ in reports] == sorted(finished for finished, _ in reports)


@pytest.mark.parametrize("answer", [False, 0, ""], ids=["false", "zero", "empty"])
def test_a_falsy_progress_cancels(populated, arena, answer: object):
    """Any falsy answer but None stops the scans, which publish nothing, and leave the arena to the next call."""
    with pytest.raises(ustore.CancelledError):
        populated.scan_batch([1, 9, 17], 2, arena, progress=lambda finished, total: answer)
    assert runs(populated.scan_batch([1, 9], 2, arena, progress=lambda finished, total: True)) == [[1, 2], [9, 10]]


def test_a_raising_progress_raises_from_the_call(populated, arena):
    """An exception inside the callable cancels the call and reaches its caller in place of CancelledError."""
    with pytest.raises(ZeroDivisionError):
        populated.scan(1, 2, arena, progress=lambda finished, total: finished / 0)


def test_progress_runs_on_the_executors_threads(populated):
    """A deferred call reports from the executor's threads, where the callable attaches the interpreter itself, and
    its exception re-raises from `wait`."""
    reporters: set[int] = set()

    def report(finished: int, total: int) -> None:
        reporters.add(threading.get_ident())

    with ustore.Executor.for_ram(2) as executor:
        later, arena = populated.deferred(executor), ustore.Arena(executor)
        assert runs(later.scan_batch(KEYS, 1, arena, progress=report).wait()) == [[key] for key in KEYS]
        failing = later.scan_batch(KEYS, 1, arena, progress=lambda finished, total: finished / 0)
        with pytest.raises(ZeroDivisionError):
            failing.wait()
    assert reporters and threading.get_ident() not in reporters


def test_progress_must_be_callable(populated, arena):
    with pytest.raises(TypeError, match="progress"):
        populated.scan_batch([1], 2, arena, progress=5)


# endregion Progress
