"""The options a verb takes as keywords, chiefly the watch one call keeps or drops inside a transaction."""

import pytest
from capabilities import NEEDS_SNAPSHOTS, NEEDS_TRANSACTIONS

import ustore

pytestmark = NEEDS_TRANSACTIONS


@pytest.fixture
def database(open_database, isolation: str | None) -> ustore.Database:
    """A database at one of the isolation levels the engine can be opened at, or the one it has."""
    return open_database(isolation)


def read_conflicts(isolation: str | None, watch: bool) -> bool:
    """Whether a transaction's read fails its commit once a concurrent writer touches the key."""
    # Strict serializability records every read; weaker levels record only what a call watches.
    return watch or isolation == "strict-serializable"


# region Watching


@pytest.mark.parametrize("watch", [True, False])
def test_the_call_decides_the_watch(database, isolation, watch: bool):
    """The keyword overrides the transaction's own watch for exactly the call it sits on."""
    reader = database.transaction(watch=not watch)
    try:
        reader.read([0], watch=watch)
        database.set(0, b"value")
        reader.set(1, b"written")
        if read_conflicts(isolation, watch):
            with pytest.raises(ustore.ConflictError):
                reader.commit()
        else:
            reader.commit()
    finally:
        reader.close()


def test_an_unwatched_read_leaves_the_rest_watching(database, isolation):
    """Dropping the watch for one read leaves the reads around it recorded as before."""
    reader = database.transaction()
    try:
        reader.read([0], watch=False)
        reader.read([1])
        database.set(1, b"value")
        with pytest.raises(ustore.ConflictError):
            reader.commit()
    finally:
        reader.close()


@pytest.mark.parametrize(
    "call",
    [
        lambda view: view.read([1], watch=False),
        lambda view: view.get(1, watch=False),
        lambda view: view.contains(1, watch=False),
        lambda view: view.scan(0, 4, watch=False),
        lambda view: view.scan_batch([0], [4], watch=False),
        lambda view: view.sample(2, watch=False),
        lambda view: view.sample_batch([2], watch=False),
        lambda view: view.measure(0, 4, watch=False),
        lambda view: view.docs_read([1], watch=False),
        lambda view: view.docs_gist([1], watch=False),
        lambda view: view.docs_gather([1], ["v"], "i64", watch=False),
        lambda view: view.graph_degrees([9], watch=False),
        lambda view: view.write([1], [b"x"], watch=False),
        lambda view: view.docs_write([2], '{"v": 2}', watch=False),
    ],
    ids=[
        "read",
        "get",
        "contains",
        "scan",
        "scan-batch",
        "sample",
        "sample-batch",
        "measure",
        "docs-read",
        "docs-gist",
        "docs-gather",
        "graph-degrees",
        "write",
        "docs-write",
    ],
)
def test_every_verb_takes_the_watch(database, call):
    """The watch is a shared keyword, so every verb a transaction reaches accepts it."""
    database.docs_write([1], '{"v": 1}')
    with database.transaction() as staged:
        call(staged)


def test_watch_refused_outside_a_transaction(database):
    """Only a transaction has a conflict set, so the database itself refuses the keyword."""
    with pytest.raises(ustore.ArgumentError):
        database.read([1], watch=False)


def test_watch_reads_a_truth_value(database):
    """The keyword is read as a truth value, as `flush` beside it is."""
    with database.transaction() as staged:
        staged.read([1], watch=0)
        staged.read([1], watch=1)


# endregion Watching

# region Refused Combinations


@NEEDS_SNAPSHOTS
def test_snapshot_refused_inside_a_transaction(database):
    """A call addresses one view, so a transaction and a snapshot are never both named."""
    identifier = database.snapshot_create()
    with database.transaction() as staged, pytest.raises(ustore.ArgumentError):
        staged.read([1], snapshot=identifier)
    database.snapshot_drop(identifier)


@pytest.mark.parametrize(
    "keyword",
    ["shared_memory", "dont_discard_memory", "bulk"],
    ids=["shared-memory", "dont-discard-memory", "bulk"],
)
def test_arena_options_are_not_reachable(database, keyword: str):
    """The options steering the arena's own memory stay inside the ABI, since no result exposes it."""
    with pytest.raises(TypeError):
        database.read([1], **{keyword: True})


# endregion Refused Combinations
