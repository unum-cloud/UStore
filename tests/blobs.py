"""Binary values over the C11 core: single and batch access, scans, samples and measurements."""

import gc

import pytest

ustore = pytest.importorskip("ustore", reason="The `ustore.lib` extension is not built into this environment")


@pytest.fixture
def database():
    """An in-memory database, closed once the case that asked for it is done."""
    with ustore.Database() as opened:
        yield opened


def test_single_round_trip(database):
    database.set(3, b"x")
    database.set(4, b"y")
    assert database.contains(3)
    assert database.contains(4)
    assert database.get(3) == b"x"
    assert database.get(4) == b"y"

    database.pop(3)
    database.pop(4)
    assert not database.contains(3)
    assert not database.contains(4)
    assert database.get(3) is None


def test_overwrite_changes_length(database):
    database.set(7, b"y")
    assert database.get(7) == b"y"
    database.set(7, b"jjjjjjjj")
    assert database.get(7) == b"jjjjjjjj"
    database.set(7, b"yy")
    assert database.get(7) == b"yy"


def test_set_none_removes(database):
    database.set(9, b"here")
    database.set(9, None)
    assert not database.contains(9)


def test_batch_round_trip(database):
    database.write([1, 2, 3], [b"a", b"bb", b"ccc"])
    values = database.read([1, 2, 3])
    assert len(values) == 3
    assert [bytes(value) for value in values] == [b"a", b"bb", b"ccc"]
    assert values.lengths == (1, 2, 3)


def test_batch_reports_missing_keys(database):
    database.write([1], [b"only"])
    values = database.read([1, 404])
    assert bytes(values[0]) == b"only"
    assert values[1] is None
    assert values.lengths == (4, None)


def test_batch_broadcasts_one_value(database):
    database.write([10, 11, 12], b"same")
    assert [bytes(value) for value in database.read([10, 11, 12])] == [b"same"] * 3


def test_batch_removes_every_key(database):
    database.write([20, 21], [b"a", b"b"])
    database.write([20, 21], None)
    assert not database.contains(20)
    assert not database.contains(21)


def test_batch_removes_one_key_of_many(database):
    database.write([30, 31], [b"a", b"b"])
    database.write([30, 31], [None, b"kept"])
    assert not database.contains(30)
    assert database.get(31) == b"kept"


def test_batch_pop(database):
    database.write([40, 41], [b"a", b"b"])
    database.pop([40, 41])
    assert not database.contains(40)
    assert not database.contains(41)


def test_keys_arrive_through_the_buffer_protocol(database):
    numpy = pytest.importorskip("numpy")
    keys = numpy.array([50, 51, 52], dtype=numpy.int64)
    database.write(keys, [b"a", b"b", b"c"])
    assert [bytes(value) for value in database.read(keys)] == [b"a", b"b", b"c"]


def test_values_are_borrowed_not_copied(database):
    database.set(1, b"borrowed")
    values = database.read([1])
    view = values[0]
    assert isinstance(view, memoryview)
    assert bytes(view) == b"borrowed"


def test_a_view_outlives_the_tape_that_exported_it(database):
    database.set(1, b"borrowed")
    view = database.read([1])[0]
    gc.collect()
    assert bytes(view) == b"borrowed"


def test_a_later_read_cannot_clobber_a_held_view(database):
    database.set(1, b"first")
    held = database.read([1])[0]
    database.set(2, b"second")
    later = database.read([2])
    assert bytes(later[0]) == b"second"
    assert bytes(held) == b"first"


def test_the_tape_types_are_named_but_not_constructible(database):
    database.set(1, b"a")
    assert isinstance(database.read([1]), ustore.Values)
    assert isinstance(database.scan(0, 1), ustore.Keys)
    with pytest.raises(TypeError):
        ustore.Values()
    with pytest.raises(TypeError):
        ustore.Keys()


def test_whole_tape_exports_a_buffer(database):
    database.write([1, 2], [b"ab", b"cd"])
    values = database.read([1, 2])
    assert bytes(memoryview(values)) == b"abcd"


def test_scan(database):
    for key in (10, 20, 30, 40, 50, 60):
        database.set(key, b"a" * (key // 10))

    assert list(database.scan(10, 6)[0]) == [10, 20, 30, 40, 50, 60]
    assert list(database.scan(20, 5)[0]) == [20, 30, 40, 50, 60]
    assert list(database.scan(30, 1)[0]) == [30]
    assert list(database.scan(40, 2)[0]) == [40, 50]
    assert database.scan(10, 6).counts == (6,)


def test_scan_paginates_from_the_last_key(database):
    for key in range(1, 8):
        database.set(key, b"v")

    page = list(database.scan(0, 3)[0])
    assert page == [1, 2, 3]
    assert list(database.scan(page[-1] + 1, 3)[0]) == [4, 5, 6]


def test_sample(database):
    for key in range(1, 10):
        database.set(key, b"v")

    drawn = list(database.sample(3)[0])
    assert len(drawn) <= 3
    assert set(drawn) <= set(range(1, 10))


def test_measure(database):
    database.write([1, 2, 3], [b"a", b"bb", b"ccc"])
    bounds = database.measure(0, 100)
    assert set(bounds) == {
        "min_cardinality",
        "max_cardinality",
        "min_value_bytes",
        "max_value_bytes",
        "min_space_usage",
        "max_space_usage",
    }
    assert bounds["max_cardinality"] >= bounds["min_cardinality"]


def test_empty_batch(database):
    values = database.read([])
    assert len(values) == 0
    assert values.lengths == ()


def test_value_longer_than_one_page(database):
    long_value = bytes(range(256)) * 4096
    database.set(1, long_value)
    assert database.get(1) == long_value


def test_rejects_an_unknown_keyword(database):
    with pytest.raises(TypeError):
        database.get(1, colection=None)


def test_rejects_a_value_that_is_not_bytes(database):
    with pytest.raises(TypeError):
        database.set(1, 42)


def test_rejects_a_key_that_is_not_an_integer(database):
    with pytest.raises(TypeError):
        database.get("one")


def test_rejects_mismatched_batch_widths(database):
    with pytest.raises(ValueError):
        database.write([1, 2, 3], [b"a", b"b"])


def test_rejects_an_index_past_the_batch(database):
    database.set(1, b"a")
    values = database.read([1])
    with pytest.raises(IndexError):
        values[1]


def test_refuses_a_closed_database():
    closed = ustore.Database()
    closed.close()
    with pytest.raises(ValueError):
        closed.get(1)


def test_closing_twice_is_safe():
    database = ustore.Database()
    database.close()
    database.close()
