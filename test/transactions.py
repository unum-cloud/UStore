"""Transactions over the C ABI: commits, rollbacks, conflicts and staging, at every isolation the engine offers."""

import pytest
from capabilities import BATCH_SIZES, NEEDS_TRANSACTIONS, keys

import ustore

pytestmark = NEEDS_TRANSACTIONS


@pytest.fixture
def database(open_database, isolation: ustore.Isolation | None) -> ustore.Database:
    """A database at one of the isolation levels the engine can be opened at, or the one it has."""
    return open_database(isolation)


def read_conflicts(isolation: ustore.Isolation | None, watch: ustore.Watch) -> bool:
    """Whether a transaction's read fails its commit once a concurrent writer touches the key."""
    # Strict serializability records every read; weaker levels record only what a transaction watches.
    return watch is ustore.Watch.KEYS or isolation is ustore.Isolation.STRICT_SERIALIZABLE


# region Publishing


@pytest.mark.parametrize("count", BATCH_SIZES)
@pytest.mark.parametrize("durability", ustore.Durability)
def test_commit_publishes_every_write(database, blobs, count: int, durability: ustore.Durability):
    staged = database.transaction()
    for key in range(count):
        staged.set(key, str(key).encode(), collection=blobs)
    staged.commit(durability)
    staged.close()
    assert [database.get(key, collection=blobs) for key in range(count)] == [str(key).encode() for key in range(count)]


@pytest.mark.parametrize("count", BATCH_SIZES)
def test_commit_publishes_every_removal(database, blobs, count: int):
    database.write(list(range(count)), b"value", collection=blobs)
    with database.transaction() as staged:
        staged.remove(list(range(count)), collection=blobs)
    assert not any(database.contains(key, collection=blobs) for key in range(count))


@pytest.mark.parametrize("verb", ["set", "write"])
def test_staged_work_is_invisible_until_it_commits(database, blobs, verb: str):
    staged = database.transaction()
    if verb == "set":
        staged.set(1, b"staged", collection=blobs)
    else:
        staged.write([1], [b"staged"], collection=blobs)
    assert not database.contains(1, collection=blobs)
    assert staged.contains(1, collection=blobs)
    assert staged.get(1, collection=blobs) == b"staged"

    staged.commit()
    staged.close()
    assert database.get(1, collection=blobs) == b"staged"


def test_reset_drops_the_staged_work(database, blobs):
    staged = database.transaction()
    staged.set(1, b"staged", collection=blobs)
    staged.reset()
    staged.commit()
    staged.close()
    assert not database.contains(1, collection=blobs)


def test_rollback_withdraws_a_stage_and_keeps_the_writes(database, blobs):
    """A rollback only withdraws the stage, so the next commit still publishes what was written."""
    staged = database.transaction()
    try:
        staged.set(1, b"staged", collection=blobs)
        staged.stage()
        staged.rollback()
        assert not database.contains(1, collection=blobs)
        staged.commit()
    finally:
        staged.close()
    assert database.get(1, collection=blobs) == b"staged"


def test_an_exception_leaves_the_work_unpublished(database, blobs):
    class Interrupted(Exception):
        """Raised inside the body to stop it short of a commit."""

    with pytest.raises(Interrupted), database.transaction() as staged:
        staged.set(1, b"staged", collection=blobs)
        raise Interrupted

    assert not database.contains(1, collection=blobs)


def test_stage_reads_at_or_below_the_commit_stamp(database, blobs):
    """Staging reserves no commit position, so the read stamp it answers never passes the commit stamp after it."""
    staged = database.transaction()
    try:
        staged.set(1, b"staged", collection=blobs)
        read_stamp = staged.stage()
        commit_stamp = staged.commit()
        assert isinstance(read_stamp, int) and isinstance(commit_stamp, int)
        assert read_stamp <= commit_stamp
    finally:
        staged.close()
    assert database.get(1, collection=blobs) == b"staged"


def test_batch_verbs_reach_through_a_transaction(database, blobs, arena):
    with database.transaction() as staged:
        staged.write([1, 2, 3], [b"a", b"bb", b"ccc"], collection=blobs)
        assert [bytes(value) for value in staged.read([1, 2, 3], arena, collection=blobs)] == [b"a", b"bb", b"ccc"]
        assert list(staged.scan(0, 3, arena, collection=blobs)[0]) == keys([1, 2, 3])


# endregion Publishing

# region Conflicts


@pytest.mark.parametrize("stride", [1, 2])
def test_transactions_writing_apart_both_commit(database, blobs, stride: int):
    first = database.transaction()
    second = database.transaction()
    for key in range(0, 100, 2):
        first.set(key * stride, str(key).encode(), collection=blobs)
    for key in range(1, 100, 2):
        second.set(key * stride, str(key).encode(), collection=blobs)

    first.commit()
    second.commit()
    first.close()
    second.close()
    assert all(database.get(key * stride, collection=blobs) == str(key).encode() for key in range(100))


@pytest.mark.parametrize("watch", ustore.Watch)
@pytest.mark.parametrize("read_verb", ["get", "contains", "read"])
def test_a_read_conflicts_with_a_concurrent_write_when_recorded(
    database, blobs, arena, isolation, watch: ustore.Watch, read_verb: str
):
    reader = database.transaction(watch)
    try:
        if read_verb == "read":
            reader.read([0], arena, collection=blobs)
        else:
            getattr(reader, read_verb)(0, collection=blobs)
        database.set(0, b"value", collection=blobs)
        if read_conflicts(isolation, watch):
            with pytest.raises(ustore.ConflictError):
                reader.commit()
        else:
            reader.commit()
    finally:
        reader.close()


def test_a_write_conflicts_with_a_concurrent_write(database, blobs):
    first = database.transaction()
    second = database.transaction()
    try:
        first.set(0, b"first", collection=blobs)
        second.set(0, b"second", collection=blobs)
        first.commit()
        with pytest.raises(ustore.ConflictError):
            second.commit()
    finally:
        first.close()
        second.close()
    assert database.get(0, collection=blobs) == b"first"


# endregion Conflicts

# region Handles


def test_a_transaction_names_its_database(database):
    with database.transaction() as staged:
        assert staged.database is database


def test_a_transaction_cannot_be_built_by_name():
    with pytest.raises(TypeError):
        ustore.Transaction()


@pytest.mark.parametrize("verb", ["set", "commit", "stage", "rollback", "reset"])
def test_a_closed_transaction_refuses_work(database, blobs, verb: str):
    staged = database.transaction()
    staged.close()
    with pytest.raises(ustore.UninitializedError):
        if verb == "set":
            staged.set(1, b"late", collection=blobs)
        else:
            getattr(staged, verb)()


def test_closing_a_transaction_twice_is_safe(database):
    staged = database.transaction()
    staged.close()
    staged.close()


# endregion Handles

# region Deferred Calls


def test_deferred_calls_stage_and_commit(database, blobs):
    with ustore.Executor.for_ram(2) as executor:
        staged = database.transaction()
        try:
            later = staged.deferred(executor)
            assert later.write([1, 2], [b"a", b"bb"], collection=blobs).wait() is None
            assert later.get(2, collection=blobs).wait() == b"bb"
            assert isinstance(later.commit().wait(), int)
        finally:
            staged.close()
    assert database.get(2, collection=blobs) == b"bb"


def test_a_transaction_serves_one_call_at_a_time(database, blobs):
    """A call on a transaction while a deferred one holds it raises ContendedError, while closing it waits."""
    with ustore.Executor.for_ram(1) as executor:
        staged = database.transaction()
        writing = staged.deferred(executor).write(list(range(4096)), b"v" * 1024, collection=blobs)
        with pytest.raises(ustore.ContendedError):
            staged.get(1, collection=blobs)
        staged.close()
        assert writing.status is ustore.Status.SUCCESS
        assert writing.wait() is None


# endregion Deferred Calls
