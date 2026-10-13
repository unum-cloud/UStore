"""Collections over the C ABI: creating, naming, listing and dropping them."""

from uuid import UUID

import pytest
from capabilities import NEEDS_COLLECTIONS, NEEDS_SNAPSHOTS, keys

import ustore

pytestmark = NEEDS_COLLECTIONS


def address(database: ustore.Database, name: str, addressing: str) -> UUID | str:
    """The collection named `name`, spelled by name or by the identifier the name resolves to."""
    return name if addressing == "name" else database.collection_find(name, ustore.Arena())


ADDRESSINGS = ["name", "identifier"]


def test_create_answers_a_uuid(database, arena, engine: str):
    """A collection ID is a UUIDv7, but on NVMe, which still spells the number it keeps."""
    identifier = database.collection_create("people")
    assert isinstance(identifier, UUID)
    assert identifier.version == 7 or engine == "nvme"
    assert database.collection_find("people", arena) == identifier


@pytest.mark.parametrize("addressing", ADDRESSINGS)
def test_verbs_address_a_collection_either_way(database, arena, addressing: str):
    database.collection_create("people")
    collection = address(database, "people", addressing)
    database.set(1, b"named", collection=collection)
    database.write([2, 3], b"batch", collection=collection)
    assert database.get(1, collection="people") == b"named"
    assert [bytes(value) for value in database.read([2, 3], arena, collection=collection)] == [b"batch"] * 2
    assert list(database.scan(0, 3, arena, collection=collection)[0]) == keys([1, 2, 3])
    with database.cursor(2, collection=collection) as cursor:
        assert list(cursor.next(3, arena)) == keys([2, 3])


@pytest.mark.parametrize("key", [0, 1, 2**64 - 2])
def test_collections_hold_their_keys_apart(database, key: int):
    first = database.collection_create("first")
    second = database.collection_create("second")
    database.set(key, b"in first", collection=first)
    database.set(key, b"in second", collection=second)

    assert database.get(key, collection=first) == b"in first"
    assert database.get(key, collection=second) == b"in second"
    assert not database.contains(key)


@pytest.mark.parametrize("main", [None, "", UUID(int=0)])
def test_the_main_collection_is_the_default(database, main: UUID | str | None):
    database.set(1, b"main")
    assert database.get(1, collection=main) == b"main"


def test_contains_reports_a_taken_name(database):
    assert not database.collection_contains("people")
    database.collection_create("people")
    assert database.collection_contains("people")


def test_the_empty_name_is_always_taken(database):
    assert database.collection_contains("")


@pytest.mark.parametrize("count", [1, 3, 20])
def test_list_answers_every_identifier(database, arena, count: int):
    created = {database.collection_create(f"collection-{index}") for index in range(count)}
    assert created <= set(database.collection_list(arena))


def test_a_deferred_find_answers_once_waited(database):
    people = database.collection_create("people")
    with ustore.Executor.for_ram(1) as executor:
        later, arena = database.deferred(executor), ustore.Arena(executor)
        assert later.collection_find("people", arena).wait() == people
        assert later.collection_find("", arena).wait() == UUID(int=0)
        missing = later.collection_find("nobody", arena)
        with pytest.raises(ustore.NotFoundError, match="nobody"):
            missing.wait()


def test_a_deferred_list_answers_once_waited(database):
    people = database.collection_create("people")
    with ustore.Executor.for_ram(1) as executor:
        assert people in database.deferred(executor).collection_list(ustore.Arena(executor)).wait()


@NEEDS_SNAPSHOTS
def test_a_snapshot_finds_and_lists_the_collections_it_pinned(database, arena):
    people = database.collection_create("people")
    with database.snapshot() as snapshot:
        later = database.collection_create("later")
        assert snapshot.collection_find("people", arena) == people
        assert people in snapshot.collection_list(arena)
        assert later not in snapshot.collection_list(arena)
        with pytest.raises(ustore.NotFoundError):
            snapshot.collection_find("later", arena)


@pytest.mark.parametrize("names", [("cherry", "apple", "banana"), ("b", "a"), ("only",), ("naïve", "zebra")])
def test_names_come_in_the_order_of_their_text(database, arena, names: tuple[str, ...]):
    for name in names:
        database.collection_create(name)
    assert database.collection_names(arena) == tuple(sorted(names, key=str.encode))


@pytest.mark.parametrize("limit", [2, 3, 4])
def test_names_paginate(database, arena, limit: int):
    """A page resumes from the last name it saw, which the next page repeats first."""
    names = ("apple", "banana", "cherry")
    for name in names:
        database.collection_create(name)

    walked = list(database.collection_names(arena, limit=limit))
    while page := database.collection_names(arena, after=walked[-1], limit=limit)[1:]:
        walked.extend(page)
    assert walked == list(names)


@pytest.mark.parametrize("addressing", ADDRESSINGS)
def test_drop_removes_the_collection(database, addressing: str):
    database.collection_create("people")
    database.set(1, b"named", collection="people")
    database.collection_drop(address(database, "people", addressing))
    if addressing == "name":
        assert not database.collection_contains("people")


def test_drop_frees_the_name_for_another_collection(database, arena):
    database.collection_create("people")
    database.collection_drop("people")
    second = database.collection_create("people")
    assert database.collection_find("people", arena) == second
    assert not database.contains(1, collection=second)


@pytest.mark.parametrize("mode", [ustore.DropMode.PAIRS, ustore.DropMode.VALUES])
def test_emptying_keeps_the_collection(database, mode: ustore.DropMode):
    identifier = database.collection_create("people")
    database.set(1, b"named", collection=identifier)
    database.collection_drop(identifier, mode)
    assert database.collection_contains("people")
    if mode is ustore.DropMode.PAIRS:
        assert not database.contains(1, collection=identifier)
    else:
        assert database.get(1, collection=identifier) in (None, b"")


def test_the_main_collection_can_be_cleared(database):
    database.set(1, b"main")
    database.collection_drop(UUID(int=0), mode=ustore.DropMode.PAIRS)
    assert not database.contains(1)


@pytest.mark.parametrize(
    ("call", "failure"),
    [
        (lambda database: database.collection_create("people"), ustore.AlreadyExistsError),
        (lambda database: database.collection_find("nobody", ustore.Arena()), ustore.NotFoundError),
        (lambda database: database.get(1, collection="nobody"), KeyError),
        (lambda database: database.collection_create(""), ustore.InvalidArgumentError),
        (lambda database: database.collection_drop("people", len(ustore.DropMode)), ValueError),
        (lambda database: database.collection_create(42), TypeError),
        (lambda database: database.get(1, collection=1.5), TypeError),
        (lambda database: database.get(1, collection=7), TypeError),
    ],
    ids=[
        "taken-name",
        "unknown-name",
        "unknown-name-in-a-verb",
        "empty-name",
        "drop-mode",
        "integer-name",
        "float",
        "integer-collection",
    ],
)
def test_refusals(database, call, failure: type):
    database.collection_create("people")
    with pytest.raises(failure):
        call(database)


def test_engine_reports_its_capabilities(database):
    assert isinstance(database.capabilities.transactions, bool)
    assert database.capabilities.collections


def test_describe_names_the_engine(database, engine: str):
    database.write([1, 2, 3], b"v")
    described = database.describe()
    assert described.engine is ustore.Engine[engine.upper()]
    assert described.capabilities == database.capabilities
    assert isinstance(described.isolation, ustore.Isolation)
    assert described.keys == 3 or engine != "ram"


@pytest.mark.parametrize("mode", ustore.DropMode)
def test_clear_empties_every_collection(database, mode: ustore.DropMode):
    """Pairs go keeping the collections, collections go with their names, or values go keeping the keys."""
    people = database.collection_create("people")
    database.set(1, b"main")
    database.set(2, b"named", collection=people)
    database.clear(mode)
    assert database.collection_contains("people") == (mode is not ustore.DropMode.COLLECTION)
    if mode is ustore.DropMode.VALUES:
        assert database.get(1) in (None, b"") and database.get(2, collection=people) in (None, b"")
    else:
        assert not database.contains(1)


def test_compact_keeps_the_data_unless_cancelled(database):
    """A compaction reports as it goes, and one its callable cancels stops between collections with the data intact."""
    database.collection_create("people")
    database.write(list(range(64)), b"v" * 64)
    database.write(list(range(64)), b"w" * 64, collection="people")
    reports: list[tuple[int, int]] = []
    database.compact(progress=lambda finished, total: reports.append((finished, total)))
    assert all(finished <= total for finished, total in reports)
    with pytest.raises(ustore.CancelledError):
        database.compact(progress=lambda finished, total: False)
    assert database.get(63) == b"v" * 64 and database.get(63, collection="people") == b"w" * 64


@pytest.mark.skipif("rocksdb" not in ustore.engines_compiled(), reason="This build carries no RocksDB engine")
def test_a_rocksdb_view_refuses_every_change(tmp_path):
    """Reads, snapshots and their saves serve, while every call that would change the database raises ReadOnlyError."""
    with ustore.Database.open_rocksdb(tmp_path) as database:
        database.collection_create("people")
        database.set(1, b"kept")
    refused = [
        lambda view: view.set(2, b"refused"),
        lambda view: view.remove(1),
        lambda view: view.transaction(),
        lambda view: view.collection_create("others"),
        lambda view: view.collection_drop("people"),
        lambda view: view.clear(ustore.DropMode.PAIRS),
        lambda view: view.compact(),
    ]
    with ustore.Database.view_rocksdb(tmp_path) as view:
        assert view.get(1) == b"kept" and view.collection_contains("people")
        for call in refused:
            with pytest.raises(ustore.ReadOnlyError) as raised:
                call(view)
            assert raised.value.status is ustore.Status.READ_ONLY
        with view.snapshot() as snapshot:
            snapshot.save(tmp_path / "saved")
    with ustore.Database.open_rocksdb(tmp_path / "saved") as saved:
        assert saved.get(1) == b"kept"
