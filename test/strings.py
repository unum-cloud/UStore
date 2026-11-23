"""Strings collections over the C ABI: writes, reads, scans, samples, measurements and removals.

File: test/strings.py
Author: Ash Vardanian
Date: November 6, 2026
"""

import pytest
from base import key

import ustore

WORDS = ["apple", "apricot", "banana"]


@pytest.fixture
def words(database: ustore.Database):
    """A strings collection mapping `apple`, `apricot` and `banana` onto the keys one, two and three."""
    collection = database.collection_create("words", modality=ustore.Modality.STRINGS)
    database.strings_write(WORDS, [1, 2, 3], collection=collection)
    return collection


def test_strings_read_back_their_keys_and_report_the_missing(words, view, arena):
    """Strings, as text or bytes, read back their keys, and missing or removed ones read as None."""
    assert view.strings_read(["banana", "cherry", b"apple"], arena, collection=words) == (key(3), None, key(1))
    view.strings_remove(["apple", "cherry"], collection=words)
    assert view.strings_read("apple", arena, collection=words) == (None,)
    assert view.strings_read(["apricot"], arena, collection=words) == (key(2),)


def test_strings_write_refuses_a_key_missing_for_a_string(database, words):
    """More strings than keys raise InvalidArgumentError."""
    with pytest.raises(ustore.InvalidArgumentError):
        database.strings_write(WORDS, [4, 5], collection=words)


def test_strings_scan_by_prefix_in_either_direction_and_resume_from_a_start(words, view, arena):
    """A strings scan narrows by prefix, runs either way, and bounds by a start and an end."""
    assert view.strings_scan("ap", 8, arena, collection=words) == ("apple", "apricot")
    assert view.strings_scan(None, 2, arena, collection=words, direction=ustore.ScanDirection.DESCENDING) == (
        "banana",
        "apricot",
    )
    assert view.strings_scan(None, 8, arena, collection=words, start="apricot") == ("apricot", "banana")
    assert view.strings_scan(None, 8, arena, collection=words, end="b") == ("apple", "apricot")
    assert view.strings_scan("zz", 8, arena, collection=words) == ()


def test_strings_sample_and_measure_cover_the_collection(words, view, arena):
    """A sample draws every string when asked for more, and a measurement bounds a range's count."""
    assert set(view.strings_sample(8, arena, collection=words)) == set(WORDS)
    bounds = view.strings_measure("a", "b", arena, collection=words)
    assert isinstance(bounds, ustore.Measurement)
    assert bounds.min_cardinality <= 2 <= bounds.max_cardinality


def test_strings_cross_a_deferred_view(later, words, arena):
    """Strings writes, reads and scans answer through a deferred view once waited."""
    later.strings_write(["cherry", "date"], [4, 5], collection=words).wait()
    assert later.strings_read(["date", "fig"], arena, collection=words).wait() == (key(5), None)
    assert later.strings_scan("d", 8, arena, collection=words).wait() == ("date",)
    assert later.strings_read("cherry", arena, collection=words).wait() == (key(4),)
