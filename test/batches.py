"""Batch scans and samples: many starts and limits in one call, against the single form beside them."""

import numpy
import pytest

KEYS = list(range(1, 33))


@pytest.fixture
def populated(view):
    """A view holding one short value per key of `KEYS`."""
    view.write(KEYS, [bytes([key % 251]) for key in KEYS])
    return view


def runs(found) -> list[list[int]]:
    """Every request's run of keys, as lists."""
    return [list(found[request]) for request in range(len(found))]


# region Batch Scans


def test_scan_batch_matches_the_single_form(populated):
    """Every request answers what the same scan alone would have."""
    starts = [1, 9, 25]
    counts = [4, 4, 4]
    batched = runs(populated.scan_batch(starts, counts))
    alone = [list(populated.scan(start, count)[0]) for start, count in zip(starts, counts, strict=True)]
    assert batched == alone


def test_scan_batch_honours_differing_limits(populated):
    """Limits differ per request, so one call resumes several ranges at exactly what each needs."""
    found = populated.scan_batch([1, 5, 20], [1, 3, 7])
    assert found.counts == (1, 3, 7)
    assert runs(found) == [[1], [5, 6, 7], list(range(20, 27))]


def test_scan_batch_takes_a_buffer_of_starts(populated):
    """Starts arrive through the buffer protocol, as every other batch of keys does."""
    found = populated.scan_batch(numpy.array([2, 10], dtype=numpy.int64), [2, 2])
    assert runs(found) == [[2, 3], [10, 11]]


def test_scan_batch_past_the_last_key(populated):
    """A request starting past every key answers an empty run rather than a failure."""
    found = populated.scan_batch([30, 1000], [8, 8])
    assert runs(found) == [[30, 31, 32], []]


def test_scan_batch_of_no_requests(populated):
    """An empty batch answers an empty tape, which no ABI call was made for."""
    found = populated.scan_batch([], [])
    assert len(found) == 0
    assert found.counts == ()


def test_scan_batch_repeats_a_start(populated):
    """Nothing makes the requests distinct, so a repeated range answers twice."""
    found = populated.scan_batch([4, 4], [3, 3])
    assert runs(found) == [[4, 5, 6], [4, 5, 6]]


def test_scan_batch_holds_its_arena(populated):
    """A run outlives the call after it, since the view keeps the arena alive."""
    found = populated.scan_batch([1, 9], [2, 2])
    first = found[0]
    populated.scan(1, 100)
    assert list(first) == [1, 2]


@pytest.mark.parametrize(
    ("call", "failure"),
    [
        (lambda view: view.scan_batch([1, 2], [1]), ValueError),
        (lambda view: view.scan_batch([1], [1, 2]), ValueError),
        (lambda view: view.scan_batch([1], [2**33]), ValueError),
        (lambda view: view.scan_batch([1], [-1]), ValueError),
        (lambda view: view.scan_batch([1], 4), TypeError),
        (lambda view: view.scan_batch([1], ["four"]), TypeError),
        (lambda view: view.scan_batch([1], [1], start=0), TypeError),
    ],
    ids=["few-limits", "many-limits", "huge-limit", "negative-limit", "bare-limit", "text-limit", "unknown-keyword"],
)
def test_scan_batch_refusals(populated, call, failure):
    """A batch whose starts and limits disagree, or whose limits are not counts, is refused."""
    with pytest.raises(failure):
        call(populated)


# endregion Batch Scans

# region Batch Samples


def test_sample_batch_draws_one_run_per_limit(populated):
    """Every request draws its own keys, never more than its limit, and all of them are stored."""
    found = populated.sample_batch([1, 4, 7])
    assert len(found) == 3
    for request, limit in enumerate([1, 4, 7]):
        drawn = list(found[request])
        assert len(drawn) <= limit
        assert set(drawn) <= set(KEYS)


def test_sample_batch_of_no_requests(populated):
    """An empty batch answers an empty tape, as an empty scan does."""
    found = populated.sample_batch([])
    assert len(found) == 0


def test_sample_batch_of_an_empty_collection(view):
    """Nothing to draw from answers empty runs rather than a failure."""
    found = view.sample_batch([3, 3])
    assert runs(found) == [[], []]


@pytest.mark.parametrize(
    ("call", "failure"),
    [
        (lambda view: view.sample_batch(4), TypeError),
        (lambda view: view.sample_batch([-2]), ValueError),
        (lambda view: view.sample_batch([2**33]), ValueError),
    ],
    ids=["bare-limit", "negative-limit", "huge-limit"],
)
def test_sample_batch_refusals(populated, call, failure):
    """A sample batch takes a sequence of counts, and nothing that is not one."""
    with pytest.raises(failure):
        call(populated)


# endregion Batch Samples

# region Long Batches


@pytest.mark.parametrize("requests", [1, 63, 64, 65, 200])
def test_scan_batch_outgrows_the_stack(populated, requests: int):
    """Batches below, at and past the limits one batch lays out on the stack all answer the same."""
    found = populated.scan_batch([1] * requests, [2] * requests)
    assert len(found) == requests
    assert runs(found) == [[1, 2]] * requests


@pytest.mark.parametrize("requests", [1, 64, 65, 200])
def test_sample_batch_outgrows_the_stack(populated, requests: int):
    """A sample batch reaches for the heap at the same width a scan batch does."""
    found = populated.sample_batch([1] * requests)
    assert len(found) == requests


# endregion Long Batches
