"""The options a verb takes as keywords, and the views of a transaction that keep or drop its watch."""

import pytest
from capabilities import NEEDS_TRANSACTIONS

import ustore

pytestmark = NEEDS_TRANSACTIONS


@pytest.fixture
def database(open_database, isolation: ustore.Isolation | None) -> ustore.Database:
    """A database at one of the isolation levels the engine can be opened at, or the one it has."""
    return open_database(isolation)


def read_conflicts(isolation: ustore.Isolation | None, watch: ustore.Watch) -> bool:
    """Whether a transaction's read fails its commit once a concurrent writer touches the key."""
    # Strict serializability records every read; weaker levels record only what a view watches.
    return watch is ustore.Watch.KEYS or isolation is ustore.Isolation.STRICT_SERIALIZABLE


# region Watching


@pytest.mark.parametrize("watch", ustore.Watch)
def test_the_view_decides_the_watch(database, blobs, arena, isolation, watch: ustore.Watch):
    """A view overrides the transaction's own watch for exactly the calls made through it."""
    other = ustore.Watch.NONE if watch is ustore.Watch.KEYS else ustore.Watch.KEYS
    reader = database.transaction(other)
    try:
        (reader.watched() if watch is ustore.Watch.KEYS else reader.unwatched()).read([0], arena, collection=blobs)
        database.set(0, b"value", collection=blobs)
        reader.set(1, b"written", collection=blobs)
        if read_conflicts(isolation, watch):
            with pytest.raises(ustore.ConflictError):
                reader.commit()
        else:
            reader.commit()
    finally:
        reader.close()


def test_an_unwatched_read_leaves_the_rest_watching(database, blobs, arena, isolation):
    """Dropping the watch for one read leaves the reads around it recorded as before."""
    reader = database.transaction()
    try:
        reader.unwatched().read([0], arena, collection=blobs)
        reader.read([1], arena, collection=blobs)
        database.set(1, b"value", collection=blobs)
        with pytest.raises(ustore.ConflictError):
            reader.commit()
    finally:
        reader.close()


@pytest.mark.parametrize(
    "call",
    [
        lambda view: view.read([1], ustore.Arena(), collection="blobs"),
        lambda view: view.get(1, collection="blobs"),
        lambda view: view.contains(1, collection="blobs"),
        lambda view: view.scan(0, 4, ustore.Arena(), collection="blobs"),
        lambda view: view.scan_batch([0], 4, ustore.Arena(), collection="blobs"),
        lambda view: view.sample(2, ustore.Arena(), collection="blobs"),
        lambda view: view.sample_batch(1, 2, ustore.Arena(), collection="blobs"),
        lambda view: view.measure(0, 4, ustore.Arena(), collection="blobs"),
        lambda view: view.docs_read([1], ustore.Arena(), collection="docs"),
        lambda view: view.docs_gist([1], ustore.Arena(), collection="docs"),
        lambda view: view.docs_gather([1], ["v"], ustore.FieldType.I64, ustore.Arena(), collection="docs"),
        lambda view: view.graph_degrees([9], ustore.Arena(), collection="graph"),
        lambda view: view.write([1], [b"x"], collection="blobs"),
        lambda view: view.docs_write([2], '{"v": 2}', collection="docs"),
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
def test_every_verb_reaches_through_an_unwatched_view(database, blobs, docs, graph, call):
    """The watch rides on the view, so every verb a transaction reaches runs unwatched through it."""
    database.docs_write([1], '{"v": 1}', collection=docs)
    with database.transaction() as staged:
        call(staged.unwatched())


def test_only_a_transaction_watches(database):
    """Only a transaction has a conflict set, so a view of the database refuses to change its watch."""
    with ustore.Executor.for_ram(1) as executor, pytest.raises(TypeError):
        database.on(executor).unwatched()


# endregion Watching

# region Refused Keywords


@pytest.mark.parametrize(
    "keyword",
    ["shared_memory", "dont_discard_memory", "bulk", "snapshot", "watch", "executor", "flush"],
    ids=["shared-memory", "dont-discard-memory", "bulk", "snapshot", "watch", "executor", "flush"],
)
def test_keywords_outside_the_verb_are_refused(database, blobs, arena, keyword: str):
    """Receivers carry the view, the watch and the executor, and the arena's own options stay inside the ABI."""
    with database.transaction() as staged, pytest.raises(TypeError):
        staged.read([1], arena, **{keyword: True}, collection=blobs)


# endregion Refused Keywords

# region Enums


@pytest.mark.parametrize(
    "call",
    [
        lambda database: database.transaction(True),
        lambda database: database.set(1, b"v", durability=True, collection="blobs"),
        lambda database: database.collection_drop("", "pairs"),
    ],
    ids=["watch-bool", "durability-bool", "drop-mode-text"],
)
def test_modes_refuse_bools_and_text(database, blobs, call):
    """A bool would read as whichever member is valued one, and text no longer names a member."""
    with pytest.raises(TypeError):
        call(database)


def test_members_are_named_by_the_library():
    """Every member is named after the C spelling of its enumerator, upper-cased with dashes as underscores."""
    assert max(ustore.Status) is ustore.Status.CAPACITY_EXHAUSTED
    assert (ustore.Isolation.STRICT_SERIALIZABLE, ustore.Transport.LINUX_URING) == (0, 2)
    assert [engine.name for engine in ustore.Engine] == ["RAM", "ROCKSDB", "LEVELDB", "NVME"]
    assert ustore.version().count(".") == 2


def test_modes_accept_plain_integers(database, blobs):
    """A mode is any int a member is valued as, so the C enumerators pass as they are, and nothing else does."""
    database.set(1, b"v", durability=int(ustore.Durability.FLUSHED), collection=blobs)
    with database.transaction(int(ustore.Watch.NONE)) as staged:
        staged.set(1, b"w", collection=blobs)
    with pytest.raises(ValueError, match="Watch"):
        database.transaction(len(ustore.Watch))


# endregion Enums
