"""Snapshots over the C11 core: capturing a point in time and reading it back."""

import pytest

ustore = pytest.importorskip("ustore", reason="The `ustore.lib` extension is not built into this environment")


@pytest.fixture
def database():
    """An in-memory database, closed once the case that asked for it is done."""
    with ustore.Database() as opened:
        if not opened.supports_snapshots:
            pytest.skip("The engine captures no snapshots")
        yield opened


def test_create_answers_an_identifier(database):
    identifier = database.snapshot_create()
    assert isinstance(identifier, int)
    database.snapshot_drop(identifier)


def test_a_snapshot_is_listed_until_it_is_dropped(database):
    identifier = database.snapshot_create()
    assert identifier in database.snapshot_list()
    database.snapshot_drop(identifier)
    assert identifier not in database.snapshot_list()


def test_a_snapshot_keeps_the_view_it_captured(database):
    database.set(1, b"before")
    identifier = database.snapshot_create()
    database.set(1, b"after")

    assert database.get(1) == b"after"
    assert database.get(1, snapshot=identifier) == b"before"
    database.snapshot_drop(identifier)


def test_a_snapshot_hides_a_later_insert(database):
    identifier = database.snapshot_create()
    database.set(1, b"later")

    assert database.contains(1)
    assert not database.contains(1, snapshot=identifier)
    database.snapshot_drop(identifier)


def test_batch_verbs_follow_a_snapshot(database):
    database.write([1, 2], [b"a", b"bb"])
    identifier = database.snapshot_create()
    database.write([1, 2], [b"changed", b"changed"])

    values = database.read([1, 2], snapshot=identifier)
    assert [bytes(value) for value in values] == [b"a", b"bb"]
    assert list(database.scan(0, 2, snapshot=identifier)[0]) == [1, 2]
    database.snapshot_drop(identifier)


def test_export_writes_a_database_that_opens(database, tmp_path):
    database.set(1, b"exported")
    identifier = database.snapshot_create()
    directory = tmp_path / "exported"
    directory.mkdir()

    try:
        database.snapshot_export(identifier, directory)
    except ustore.UnsupportedError:
        pytest.skip("The engine exports no snapshots")
    finally:
        database.snapshot_drop(identifier)

    with ustore.Database(directory) as reopened:
        assert reopened.get(1) == b"exported"


def test_dropping_an_unknown_snapshot_is_refused(database):
    with pytest.raises(ustore.Error):
        database.snapshot_drop(9999)


def test_reading_an_unknown_snapshot_is_refused(database):
    with pytest.raises(ustore.Error):
        database.get(1, snapshot=9999)
