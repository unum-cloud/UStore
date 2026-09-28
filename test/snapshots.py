"""Snapshots over the C ABI: capturing a point in time and reading it back through every read verb."""

import pytest
from capabilities import NEEDS_SNAPSHOTS, keys, open_engine

import ustore

pytestmark = NEEDS_SNAPSHOTS

READ_VERBS = {
    "get": lambda database, key, snapshot: database.get(key, snapshot=snapshot),
    "read": lambda database, key, snapshot: (
        None if (found := database.read([key], snapshot=snapshot)[0]) is None else bytes(found)
    ),
    "docs_read": lambda database, key, snapshot: (
        None if (found := database.docs_read([key], snapshot=snapshot)[0]) is None else bytes(found)
    ),
}


def test_a_snapshot_names_its_database(database):
    with database.snapshot() as snapshot:
        assert isinstance(snapshot, ustore.Snapshot)
        assert snapshot.database is database


def test_a_database_waits_for_its_snapshots(database):
    snapshot = database.snapshot()
    with pytest.raises(ustore.ArgumentError):
        database.close()
    snapshot.close()
    snapshot.close()


@pytest.mark.parametrize("verb", READ_VERBS)
def test_a_snapshot_keeps_the_view_it_captured(database, verb: str):
    database.set(1, b'"before"')
    with database.snapshot() as snapshot:
        database.set(1, b'"after"')
        assert READ_VERBS[verb](database, 1, None) == b'"after"'
        assert READ_VERBS[verb](database, 1, snapshot) == b'"before"'


@pytest.mark.parametrize("verb", READ_VERBS)
def test_a_snapshot_hides_a_later_insert(database, verb: str):
    with database.snapshot() as snapshot:
        database.set(1, b'"later"')
        assert READ_VERBS[verb](database, 1, None) is not None
        assert READ_VERBS[verb](database, 1, snapshot) is None
        assert not database.contains(1, snapshot=snapshot)


def test_batch_verbs_follow_a_snapshot(database):
    database.write([1, 2], [b"a", b"bb"])
    with database.snapshot() as snapshot:
        database.write([1, 2, 3], b"changed")
        assert [bytes(value) for value in database.read([1, 2], snapshot=snapshot)] == [b"a", b"bb"]
        assert list(database.scan(0, 3, snapshot=snapshot)[0]) == keys([1, 2])


def test_export_writes_a_database_that_opens(database, engine: str, tmp_path):
    database.set(1, b"exported")
    directory = tmp_path / "exported"
    directory.mkdir()

    with database.snapshot() as snapshot:
        try:
            snapshot.export(directory)
        except ustore.UnsupportedError:
            pytest.skip("The engine exports no snapshots")

    with open_engine(engine, directory) as reopened:
        assert reopened.get(1) == b"exported"


def test_a_closed_snapshot_is_refused(database):
    snapshot = database.snapshot()
    snapshot.close()
    with pytest.raises(ValueError):
        database.get(1, snapshot=snapshot)
    with pytest.raises(ValueError):
        snapshot.export("anywhere")


def test_a_snapshot_of_another_database_is_refused(database, open_database):
    other = open_database()
    with other.snapshot() as foreign, pytest.raises(ustore.ArgumentError):
        database.get(1, snapshot=foreign)


def test_an_identifier_is_no_snapshot(database):
    with pytest.raises(TypeError):
        database.get(1, snapshot=1)
