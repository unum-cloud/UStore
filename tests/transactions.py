"""Transactions over the C11 core: commits, rollbacks, conflicts and staging."""

import pytest

ustore = pytest.importorskip("ustore", reason="The `ustore.lib` extension is not built into this environment")


@pytest.fixture
def database():
    """An in-memory database, closed once the case that asked for it is done."""
    with ustore.Database() as opened:
        if not opened.supports_transactions:
            pytest.skip("The engine has no transactions")
        yield opened


def test_commit_publishes_every_write(database):
    with database.transaction() as staged:
        for key in range(100):
            staged.set(key, str(key).encode())
    for key in range(100):
        assert database.get(key) == str(key).encode()


def test_commit_publishes_every_removal(database):
    for key in range(100):
        database.set(key, str(key).encode())

    with database.transaction() as staged:
        staged.pop(list(range(100)))
    for key in range(100):
        assert not database.contains(key)


def test_staged_work_is_invisible_until_it_commits(database):
    staged = database.transaction()
    staged.set(1, b"pending")
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


def test_two_transactions_writing_apart_both_commit(database):
    first = database.transaction()
    second = database.transaction()
    for key in range(0, 100, 2):
        first.set(key, str(key).encode())
    for key in range(1, 100, 2):
        second.set(key, str(key).encode())

    first.commit()
    second.commit()
    first.close()
    second.close()
    for key in range(100):
        assert database.get(key) == str(key).encode()


def test_a_watched_read_conflicts_with_a_concurrent_write(database):
    reader = database.transaction()
    reader.get(0)
    with database.transaction() as writer:
        writer.set(0, b"value")

    with pytest.raises(ustore.ConflictError):
        reader.commit()
    reader.close()


def test_a_transaction_that_watches_nothing_always_commits():
    # Dropping the watch is honoured under snapshot isolation; strict serializability records every read.
    with ustore.Database(config='{"version": "1.0", "isolation": "snapshot"}') as database:
        staged = database.transaction(watch=False)
        try:
            staged.get(0)
            database.set(0, b"value")
            staged.commit()
        finally:
            staged.close()


def test_stage_answers_a_sequence_number(database):
    staged = database.transaction()
    staged.set(1, b"pending")
    sequence_number = staged.stage()
    assert isinstance(sequence_number, int)
    staged.commit()
    staged.close()
    assert database.get(1) == b"pending"


def test_batch_verbs_reach_through_a_transaction(database):
    with database.transaction() as staged:
        staged.write([1, 2, 3], [b"a", b"bb", b"ccc"])
        values = staged.read([1, 2, 3])
        assert [bytes(value) for value in values] == [b"a", b"bb", b"ccc"]
        assert list(staged.scan(0, 3)[0]) == [1, 2, 3]


def test_a_transaction_names_its_database(database):
    with database.transaction() as staged:
        assert staged.database is database


def test_a_transaction_refuses_a_snapshot(database):
    with database.transaction() as staged, pytest.raises(ustore.ArgumentError):
        staged.get(1, snapshot=1)


def test_a_transaction_cannot_be_built_by_name():
    with pytest.raises(TypeError):
        ustore.Transaction()


def test_a_database_waits_for_its_transactions(database):
    staged = database.transaction()
    with pytest.raises(ustore.ArgumentError):
        database.close()
    staged.close()


def test_a_closed_transaction_refuses_work(database):
    staged = database.transaction()
    staged.close()
    with pytest.raises(ValueError):
        staged.set(1, b"late")


def test_closing_a_transaction_twice_is_safe(database):
    staged = database.transaction()
    staged.close()
    staged.close()
