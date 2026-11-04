"""Snapshots over the C ABI: capturing a point in time and reading it back through every read verb.

File: test/snapshots.py
Author: Ash Vardanian
Date: September 18, 2026
"""

import pytest
from capabilities import NEEDS_SNAPSHOTS, NEEDS_TRANSACTIONS, keys, location, open_engine

import ustore

pytestmark = NEEDS_SNAPSHOTS

READ_VERBS = {
    "get": lambda receiver, key: receiver.get(key, collection="blobs"),
    "read": lambda receiver, key: (
        None if (found := receiver.read([key], ustore.Arena(), collection="blobs")[0]) is None else bytes(found)
    ),
    "docs_read": lambda receiver, key: (
        None if (found := receiver.docs_read([key], ustore.Arena(), collection="docs")[0]) is None else bytes(found)
    ),
    "subscript": lambda receiver, key: receiver["blobs"].blobs[[key]][0],
}


def write(receiver, verb: str, key: int, value: bytes) -> None:
    """Writes `value` where the read verb `verb` finds it: a document for `docs_read`, a binary value otherwise."""
    if verb == "docs_read":
        receiver.docs_write([key], value, collection="docs")
    else:
        receiver.set(key, value, collection="blobs")


def test_a_snapshot_names_its_database(database):
    with database.snapshot() as snapshot:
        assert isinstance(snapshot, ustore.Snapshot)
        assert snapshot.database is database


@NEEDS_TRANSACTIONS
def test_a_snapshot_and_a_transaction_hold_their_database_open(database, blobs):
    """Closing the database first leaves its snapshot reading and its transaction committing."""
    database.set(1, b"before", collection=blobs)
    snapshot = database.snapshot()
    staged = database.transaction()
    database.close()
    with pytest.raises(ustore.UninitializedError):
        database.get(1, collection=blobs)
    staged.set(2, b"after", collection=blobs)
    staged.commit()
    assert snapshot.get(1, collection=blobs) == b"before" and snapshot.get(2, collection=blobs) is None
    staged.close()
    snapshot.close()
    snapshot.close()


@NEEDS_TRANSACTIONS
def test_a_snapshot_reads_at_or_above_the_commits_it_sees(database, blobs):
    """A commit is visible to every snapshot whose read stamp is at or above its commit stamp, and to none below."""
    with database.snapshot() as before:
        staged = database.transaction()
        staged.set(1, b"committed", collection=blobs)
        commit_stamp = staged.commit()
        staged.close()
        with database.snapshot() as after:
            assert before.read_stamp < commit_stamp <= after.read_stamp
            assert before.get(1, collection=blobs) is None and after.get(1, collection=blobs) == b"committed"


@pytest.mark.parametrize("verb", READ_VERBS)
def test_a_snapshot_keeps_the_view_it_captured(database, blobs, docs, verb: str):
    write(database, verb, 1, b'"before"')
    with database.snapshot() as snapshot:
        write(database, verb, 1, b'"after"')
        assert READ_VERBS[verb](database, 1) == b'"after"'
        assert READ_VERBS[verb](snapshot, 1) == b'"before"'


@pytest.mark.parametrize("verb", READ_VERBS)
def test_a_snapshot_hides_a_later_insert(database, blobs, docs, verb: str):
    with database.snapshot() as snapshot:
        write(database, verb, 1, b'"later"')
        assert READ_VERBS[verb](database, 1) is not None
        assert READ_VERBS[verb](snapshot, 1) is None
        assert not snapshot.contains(1, collection=blobs)


def test_batch_verbs_follow_a_snapshot(database, blobs, arena):
    database.write([1, 2], [b"a", b"bb"], collection=blobs)
    with database.snapshot() as snapshot:
        database.write([1, 2, 3], b"changed", collection=blobs)
        assert [bytes(value) for value in snapshot.read([1, 2], arena, collection=blobs)] == [b"a", b"bb"]
        assert list(snapshot.scan(0, 3, arena, collection=blobs)[0]) == keys([1, 2])
        assert snapshot["blobs"].blobs[1] == b"a"


def test_a_snapshot_only_reads(database, blobs):
    """A snapshot has no write verbs, and its collections and views refuse every write."""
    with database.snapshot() as snapshot, ustore.Executor.for_ram(1) as executor:
        assert not hasattr(snapshot, "write")
        with pytest.raises(TypeError):
            snapshot["blobs"].blobs[1] = b"refused"
        with pytest.raises(TypeError):
            snapshot.on(executor).set(1, b"refused", collection=blobs)
    assert not database.contains(1, collection=blobs)


def test_save_writes_a_database_that_opens(database, blobs, engine: str, tmp_path):
    database.set(1, b"saved", collection=blobs)
    directory = tmp_path / "saved"
    directory.mkdir()

    with database.snapshot() as snapshot:
        try:
            snapshot.save(location(engine, directory))
        except ustore.UnsupportedError:
            pytest.skip("The engine saves no snapshots")

    with open_engine(engine, directory) as reopened:
        assert reopened.get(1, collection=blobs) == b"saved"


def test_a_deferred_save_writes_a_database_that_opens(database, blobs, engine: str, tmp_path):
    database.set(1, b"saved", collection=blobs)
    directory = tmp_path / "saved"
    directory.mkdir()

    with database.snapshot() as snapshot, ustore.Executor.for_ram(1) as executor:
        saving = snapshot.deferred(executor).save(
            location(engine, directory), progress=lambda finished, total: finished <= total
        )
        try:
            assert saving.wait() is None
        except ustore.UnsupportedError:
            pytest.skip("The engine saves no snapshots")

    with open_engine(engine, directory) as reopened:
        assert reopened.get(1, collection=blobs) == b"saved"


def test_a_cancelled_save_leaves_nothing_behind(database, blobs, engine: str, tmp_path):
    """A save its callable cancels removes what it wrote, so a retry at the same path saves a copy that opens."""
    database.write(list(range(256)), b"v" * 256, collection=blobs)
    directory = tmp_path / "saved"
    directory.mkdir()
    with database.snapshot() as snapshot:
        with pytest.raises(ustore.CancelledError):
            snapshot.save(location(engine, directory), progress=lambda finished, total: False)
        snapshot.save(location(engine, directory))
    with open_engine(engine, directory) as reopened:
        assert reopened.get(255, collection=blobs) == b"v" * 256


def test_a_snapshot_keeps_its_collections_from_a_drop(database, engine: str):
    """A snapshot belongs to its holder, so the RAM and RocksDB engines drop no collection under it."""
    people = database.collection_create("people", modality=ustore.Modality.BLOBS)
    with database.snapshot():
        if engine in ("ram", "rocksdb"):
            with pytest.raises(ustore.InvalidArgumentError):
                database.collection_drop(people)
        else:
            database.collection_drop(people)


def test_save_needs_a_view_of_a_snapshot(database, tmp_path):
    with ustore.Executor.for_ram(1) as executor, pytest.raises(TypeError, match="snapshot"):
        database.deferred(executor).save(tmp_path)


def test_a_cursor_reads_its_snapshot_after_the_snapshot_closes(database, blobs, arena):
    """A cursor holds the snapshot it reads through, so its walk sees the pinned state whatever the head does."""
    database.write([1, 2], b"before", collection=blobs)
    snapshot = database.snapshot()
    cursor = snapshot.cursor(collection=blobs)
    snapshot.close()
    database.write([0, 3], b"after", collection=blobs)
    database.remove(1, collection=blobs)
    page = cursor.next(8, arena, values=True)
    assert list(page) == keys([1, 2])
    assert [bytes(value) for value in page.values] == [b"before", b"before"]
    cursor.close()


def test_a_closed_snapshot_is_refused(database, blobs):
    snapshot = database.snapshot()
    snapshot.close()
    with pytest.raises(ustore.UninitializedError):
        snapshot.get(1, collection=blobs)
    with pytest.raises(ustore.UninitializedError):
        snapshot.save("anywhere")
