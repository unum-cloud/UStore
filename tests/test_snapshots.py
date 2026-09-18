"""Snapshots over the C ABI: capturing a point in time and reading it back through every read verb."""

import pytest
import ustore
from ustore_capabilities import NEEDS_SNAPSHOTS

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


def test_create_answers_an_identifier(database):
    identifier = database.snapshot_create()
    assert isinstance(identifier, int)
    database.snapshot_drop(identifier)


@pytest.mark.parametrize("count", [1, 3])
def test_a_snapshot_is_listed_until_it_is_dropped(database, count: int):
    identifiers = [database.snapshot_create() for _ in range(count)]
    assert set(identifiers) <= set(database.snapshot_list())
    for identifier in identifiers:
        database.snapshot_drop(identifier)
    assert not set(identifiers) & set(database.snapshot_list())


@pytest.mark.parametrize("verb", READ_VERBS)
def test_a_snapshot_keeps_the_view_it_captured(database, verb: str):
    database.set(1, b'"before"')
    identifier = database.snapshot_create()
    database.set(1, b'"after"')

    assert READ_VERBS[verb](database, 1, 0) == b'"after"'
    assert READ_VERBS[verb](database, 1, identifier) == b'"before"'
    database.snapshot_drop(identifier)


@pytest.mark.parametrize("verb", READ_VERBS)
def test_a_snapshot_hides_a_later_insert(database, verb: str):
    identifier = database.snapshot_create()
    database.set(1, b'"later"')

    assert READ_VERBS[verb](database, 1, 0) is not None
    assert READ_VERBS[verb](database, 1, identifier) is None
    assert not database.contains(1, snapshot=identifier)
    database.snapshot_drop(identifier)


def test_batch_verbs_follow_a_snapshot(database):
    database.write([1, 2], [b"a", b"bb"])
    identifier = database.snapshot_create()
    database.write([1, 2, 3], b"changed")

    assert [bytes(value) for value in database.read([1, 2], snapshot=identifier)] == [b"a", b"bb"]
    assert list(database.scan(0, 3, snapshot=identifier)[0]) == [1, 2]
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


@pytest.mark.parametrize(
    "call",
    [
        lambda database: database.snapshot_drop(9999),
        lambda database: database.get(1, snapshot=9999),
    ],
    ids=["drop", "read"],
)
def test_an_unknown_snapshot_is_refused(database, call):
    with pytest.raises(ustore.Error):
        call(database)
