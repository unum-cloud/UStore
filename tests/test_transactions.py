"""Transactions over the C ABI: commits, rollbacks, conflicts and staging, at every isolation the engine offers."""

import pytest
import ustore
from ustore_capabilities import ENGINE, ISOLATIONS, NEEDS_TRANSACTIONS

pytestmark = NEEDS_TRANSACTIONS

BATCH_SIZES = [1, 17, 100]


@pytest.fixture(params=ISOLATIONS, ids=lambda isolation: isolation or "default")
def isolation(request: pytest.FixtureRequest) -> str | None:
    return request.param


@pytest.fixture
def database(open_database, isolation: str | None) -> ustore.Database:
    """A database at one of the isolation levels the engine can be opened at."""
    return open_database(isolation)


def read_conflicts(isolation: str | None, watch: bool) -> bool:
    """Whether a transaction's read fails its commit once a concurrent writer touches the key."""
    # Strict serializability records every read; weaker levels record only what a transaction watches.
    return watch or isolation == "strict_serializable"


# region Publishing


@pytest.mark.parametrize("count", BATCH_SIZES)
@pytest.mark.parametrize("flush", [False, True])
def test_commit_publishes_every_write(database, count: int, flush: bool):
    staged = database.transaction()
    for key in range(count):
        staged.set(key, str(key).encode())
    staged.commit(flush=flush)
    staged.close()
    assert [database.get(key) for key in range(count)] == [str(key).encode() for key in range(count)]


@pytest.mark.parametrize("count", BATCH_SIZES)
def test_commit_publishes_every_removal(database, count: int):
    database.write(list(range(count)), b"value")
    with database.transaction() as staged:
        staged.pop(list(range(count)))
    assert not any(database.contains(key) for key in range(count))


@pytest.mark.parametrize("verb", ["set", "write"])
def test_staged_work_is_invisible_until_it_commits(database, verb: str):
    staged = database.transaction()
    if verb == "set":
        staged.set(1, b"pending")
    else:
        staged.write([1], [b"pending"])
    assert not database.contains(1)
    assert staged.contains(1)
    assert staged.get(1) == b"pending"

    staged.commit()
    staged.close()
    assert database.get(1) == b"pending"


def test_rollback_drops_the_staged_work(database):
    staged = database.transaction()
    staged.set(1, b"pending")
    staged.rollback()
    staged.commit()
    staged.close()
    assert not database.contains(1)


def test_an_exception_leaves_the_work_unpublished(database):
    class Interrupted(Exception):
        """Raised inside the body to stop it short of a commit."""

    with pytest.raises(Interrupted), database.transaction() as staged:
        staged.set(1, b"pending")
        raise Interrupted

    assert not database.contains(1)


@pytest.mark.xfail(
    ENGINE == "rocksdb", raises=ustore.UnsupportedError, reason="RocksDB's optimistic transactions do not stage"
)
def test_stage_answers_a_sequence_number(database):
    staged = database.transaction()
    try:
        staged.set(1, b"pending")
        assert isinstance(staged.stage(), int)
        staged.commit()
    finally:
        staged.close()
    assert database.get(1) == b"pending"


def test_batch_verbs_reach_through_a_transaction(database):
    with database.transaction() as staged:
        staged.write([1, 2, 3], [b"a", b"bb", b"ccc"])
        assert [bytes(value) for value in staged.read([1, 2, 3])] == [b"a", b"bb", b"ccc"]
        assert list(staged.scan(0, 3)[0]) == [1, 2, 3]


# endregion Publishing

# region Conflicts


@pytest.mark.parametrize("stride", [1, 2])
def test_transactions_writing_apart_both_commit(database, stride: int):
    first = database.transaction()
    second = database.transaction()
    for key in range(0, 100, 2):
        first.set(key * stride, str(key).encode())
    for key in range(1, 100, 2):
        second.set(key * stride, str(key).encode())

    first.commit()
    second.commit()
    first.close()
    second.close()
    assert all(database.get(key * stride) == str(key).encode() for key in range(100))


@pytest.mark.parametrize("watch", [True, False])
@pytest.mark.parametrize("read_verb", ["get", "contains", "read"])
def test_a_read_conflicts_with_a_concurrent_write_when_recorded(database, isolation, watch: bool, read_verb: str):
    reader = database.transaction(watch=watch)
    try:
        if read_verb == "read":
            reader.read([0])
        else:
            getattr(reader, read_verb)(0)
        database.set(0, b"value")
        if read_conflicts(isolation, watch):
            with pytest.raises(ustore.ConflictError):
                reader.commit()
        else:
            reader.commit()
    finally:
        reader.close()


def test_a_write_conflicts_with_a_concurrent_write(database):
    first = database.transaction()
    second = database.transaction()
    try:
        first.set(0, b"first")
        second.set(0, b"second")
        first.commit()
        with pytest.raises(ustore.ConflictError):
            second.commit()
    finally:
        first.close()
        second.close()
    assert database.get(0) == b"first"


# endregion Conflicts

# region Handles


def test_a_transaction_names_its_database(database):
    with database.transaction() as staged:
        assert staged.database is database


@pytest.mark.parametrize("verb", ["get", "contains"])
def test_a_transaction_refuses_a_snapshot(database, verb: str):
    with database.transaction() as staged, pytest.raises(ustore.ArgumentError):
        getattr(staged, verb)(1, snapshot=1)


def test_a_transaction_cannot_be_built_by_name():
    with pytest.raises(TypeError):
        ustore.Transaction()


def test_a_database_waits_for_its_transactions(database):
    staged = database.transaction()
    with pytest.raises(ustore.ArgumentError):
        database.close()
    staged.close()


@pytest.mark.parametrize("verb", ["set", "commit", "stage", "rollback"])
def test_a_closed_transaction_refuses_work(database, verb: str):
    staged = database.transaction()
    staged.close()
    with pytest.raises(ValueError):
        if verb == "set":
            staged.set(1, b"late")
        else:
            getattr(staged, verb)()


def test_closing_a_transaction_twice_is_safe(database):
    staged = database.transaction()
    staged.close()
    staged.close()


# endregion Handles
