"""Collections over the C ABI: creating, naming, listing and dropping them.

File: test/catalog.py
Author: Ash Vardanian
Date: September 18, 2026
"""

from uuid import UUID

import pytest
from base import ENGINES, NEEDS_SNAPSHOTS, executor_for, keys

import ustore
from ustore import Modality


def address(database: ustore.Database, name: str, addressing: str) -> UUID | str:
    """The collection named `name`, spelled by name or by the identifier the name resolves to."""
    return name if addressing == "name" else database.collection_find(name, ustore.Arena())


ADDRESSINGS = ["name", "identifier"]


def test_create_answers_a_uuid(database, arena, engine: str):
    """A collection ID is a UUIDv7, but on NVMe, which still spells the number it keeps."""
    identifier = database.collection_create("people", modality=Modality.BLOBS)
    assert isinstance(identifier, UUID)
    assert identifier.version == 7 or engine == "nvme"
    assert database.collection_find("people", arena) == identifier


@pytest.mark.parametrize("addressing", ADDRESSINGS)
def test_verbs_address_a_collection_either_way(database, arena, addressing: str):
    """Every verb finds a collection by its name or by its identifier alike."""
    database.collection_create("people", modality=Modality.BLOBS)
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
    """One key in two collections holds two values."""
    first = database.collection_create("first", modality=Modality.BLOBS)
    second = database.collection_create("second", modality=Modality.BLOBS)
    database.set(key, b"in first", collection=first)
    database.set(key, b"in second", collection=second)

    assert database.get(key, collection=first) == b"in first"
    assert database.get(key, collection=second) == b"in second"


def test_a_fresh_database_lists_no_collections(database, arena):
    """No collection exists until one is created, and the system ones are never listed."""
    assert database.collection_list(arena) == ()
    assert database.collection_names(arena) == ()


@pytest.mark.parametrize("name", ["_x", "_names", "_collections"])
def test_names_starting_with_an_underscore_are_refused(database, arena, name: str):
    """Names starting with an underscore belong to the system collections and are refused."""
    with pytest.raises(ustore.InvalidArgumentError):
        database.collection_create(name, modality=Modality.BLOBS)
    assert not database.collection_contains(name)
    assert database.collection_list(arena) == ()


@pytest.mark.parametrize("missing", [None, "", UUID(int=0)])
def test_a_verb_names_a_created_collection(database, missing: UUID | str | None):
    """Without a main collection, a verb that names None is refused, and the empty name or the nil UUID finds none."""
    database.collection_create("people", modality=Modality.BLOBS)
    with pytest.raises(TypeError if missing is None else ustore.NotFoundError):
        database.set(1, b"nowhere", collection=missing)
    with pytest.raises(ustore.NotFoundError):
        database.get(1)


def test_contains_reports_a_taken_name(database):
    """A name is reported taken once a collection is created under it."""
    assert not database.collection_contains("people")
    database.collection_create("people", modality=Modality.BLOBS)
    assert database.collection_contains("people")


def test_the_empty_name_is_never_taken(database):
    """The empty name names no collection."""
    assert not database.collection_contains("")


@pytest.mark.parametrize("count", [1, 3, 20])
def test_list_answers_every_identifier(database, arena, count: int):
    """The listing holds the identifier of every created collection."""
    created = {database.collection_create(f"collection-{index}", modality=Modality.BLOBS) for index in range(count)}
    assert created <= set(database.collection_list(arena))


def test_a_deferred_find_answers_once_waited(database, engine: str):
    """A deferred find answers the identifier, or raises NotFoundError naming the name, once waited."""
    people = database.collection_create("people", modality=Modality.BLOBS)
    with executor_for(engine, 1) as executor:
        later, arena = database.deferred(executor), ustore.Arena(executor)
        assert later.collection_find("people", arena).wait() == people
        missing = later.collection_find("nobody", arena)
        with pytest.raises(ustore.NotFoundError, match="nobody"):
            missing.wait()


def test_a_deferred_list_answers_once_waited(database, engine: str):
    """A deferred listing holds a created collection once waited."""
    people = database.collection_create("people", modality=Modality.BLOBS)
    with executor_for(engine, 1) as executor:
        assert people in database.deferred(executor).collection_list(ustore.Arena(executor)).wait()


@NEEDS_SNAPSHOTS
def test_a_snapshot_finds_and_lists_the_collections_it_pinned(database, arena):
    """A snapshot finds and lists the collections it was taken over, and none created after it."""
    people = database.collection_create("people", modality=Modality.BLOBS)
    with database.snapshot() as snapshot:
        later = database.collection_create("later", modality=Modality.BLOBS)
        assert snapshot.collection_find("people", arena) == people
        assert people in snapshot.collection_list(arena)
        assert later not in snapshot.collection_list(arena)
        with pytest.raises(ustore.NotFoundError):
            snapshot.collection_find("later", arena)


@pytest.mark.parametrize("names", [("cherry", "apple", "banana"), ("b", "a"), ("only",), ("naïve", "zebra")])
def test_names_come_in_the_order_of_their_text(database, arena, names: tuple[str, ...]):
    """Names list in the bytewise order of their UTF-8 text."""
    for name in names:
        database.collection_create(name, modality=Modality.BLOBS)
    assert database.collection_names(arena) == tuple(sorted(names, key=str.encode))


@pytest.mark.parametrize("limit", [2, 3, 4])
def test_names_paginate(database, arena, limit: int):
    """A page resumes from the last name it saw, which the next page repeats first."""
    names = ("apple", "banana", "cherry")
    for name in names:
        database.collection_create(name, modality=Modality.BLOBS)

    walked = list(database.collection_names(arena, limit=limit))
    while page := database.collection_names(arena, after=walked[-1], limit=limit)[1:]:
        walked.extend(page)
    assert walked == list(names)


@pytest.mark.parametrize("addressing", ADDRESSINGS)
def test_drop_removes_the_collection(database, addressing: str):
    """Dropping a collection by its name or its identifier removes it."""
    database.collection_create("people", modality=Modality.BLOBS)
    database.set(1, b"named", collection="people")
    database.collection_drop(address(database, "people", addressing))
    if addressing == "name":
        assert not database.collection_contains("people")


def test_drop_frees_the_name_for_another_collection(database, arena):
    """A dropped name creates a new, empty collection with a new identifier."""
    database.collection_create("people", modality=Modality.BLOBS)
    database.collection_drop("people")
    second = database.collection_create("people", modality=Modality.BLOBS)
    assert database.collection_find("people", arena) == second
    assert not database.contains(1, collection=second)


@pytest.mark.parametrize("mode", [ustore.DropMode.PAIRS, ustore.DropMode.VALUES])
def test_emptying_keeps_the_collection(database, mode: ustore.DropMode):
    """Dropping the pairs or the values empties a collection and keeps its name."""
    identifier = database.collection_create("people", modality=Modality.BLOBS)
    database.set(1, b"named", collection=identifier)
    database.collection_drop(identifier, mode)
    assert database.collection_contains("people")
    if mode is ustore.DropMode.PAIRS:
        assert not database.contains(1, collection=identifier)
    else:
        assert database.get(1, collection=identifier) in (None, b"")


@pytest.mark.parametrize(
    ("call", "failure"),
    [
        (lambda database: database.collection_create("people", modality=Modality.BLOBS), ustore.AlreadyExistsError),
        (lambda database: database.collection_find("nobody", ustore.Arena()), ustore.NotFoundError),
        (lambda database: database.get(1, collection="nobody"), KeyError),
        (lambda database: database.collection_create("", modality=Modality.BLOBS), ustore.InvalidArgumentError),
        (lambda database: database.collection_create("others"), TypeError),
        (lambda database: database.collection_drop("people", len(ustore.DropMode)), ValueError),
        (lambda database: database.collection_create(42, modality=Modality.BLOBS), TypeError),
        (lambda database: database.get(1, collection=1.5), TypeError),
        (lambda database: database.get(1, collection=7), TypeError),
    ],
    ids=[
        "taken-name",
        "unknown-name",
        "unknown-name-in-a-verb",
        "empty-name",
        "no-modality",
        "drop-mode",
        "integer-name",
        "float",
        "integer-collection",
    ],
)
def test_refusals(database, call, failure: type):
    """Taken, unknown and empty names, a missing modality, and keys or collections of a wrong type raise."""
    database.collection_create("people", modality=Modality.BLOBS)
    with pytest.raises(failure):
        call(database)


def test_engine_reports_its_capabilities(database):
    """The capabilities read as bools, and none names collections, which every engine keeps."""
    assert isinstance(database.capabilities.transactions, bool)
    assert isinstance(database.capabilities.snapshots, bool)
    assert isinstance(database.capabilities.saves_snapshots, bool)
    assert not hasattr(database.capabilities, "collections")


def test_describe_names_the_engine(database, blobs, engine: str):
    """A description names the engine, repeats its capabilities, and counts the keys on RAM."""
    database.write([1, 2, 3], b"v", collection=blobs)
    described = database.describe()
    assert described.engine is ustore.Engine[engine.upper()]
    assert described.capabilities == database.capabilities
    assert isinstance(described.isolation, ustore.Isolation)
    assert described.keys == 3 or engine != "ram"


@pytest.mark.parametrize("mode", ustore.DropMode)
def test_clear_empties_every_collection(database, mode: ustore.DropMode):
    """Pairs go keeping the collections, collections go with their names, or values go keeping the keys."""
    people = database.collection_create("people", modality=Modality.BLOBS)
    places = database.collection_create("places", modality=Modality.BLOBS)
    database.set(1, b"first", collection=people)
    database.set(2, b"second", collection=places)
    database.clear(mode)
    kept = mode is not ustore.DropMode.COLLECTION
    assert database.collection_contains("people") == kept
    assert database.collection_contains("places") == kept
    if mode is ustore.DropMode.VALUES:
        assert database.get(1, collection=people) in (None, b"")
        assert database.get(2, collection=places) in (None, b"")
    elif mode is ustore.DropMode.PAIRS:
        assert not database.contains(1, collection=people)
        assert not database.contains(2, collection=places)
    else:
        assert database.collection_list(ustore.Arena()) == ()


def test_compact_keeps_the_data_unless_cancelled(database):
    """A compaction reports as it goes, and one its callable cancels stops between collections with the data intact."""
    database.collection_create("people", modality=Modality.BLOBS)
    database.collection_create("places", modality=Modality.BLOBS)
    database.write(list(range(64)), b"v" * 64, collection="places")
    database.write(list(range(64)), b"w" * 64, collection="people")
    reports: list[tuple[int, int]] = []
    database.compact(progress=lambda finished, total: reports.append((finished, total)))
    assert all(finished <= total for finished, total in reports)
    with pytest.raises(ustore.CancelledError):
        database.compact(progress=lambda finished, total: False)
    assert database.get(63, collection="places") == b"v" * 64
    assert database.get(63, collection="people") == b"w" * 64


def test_compact_plans_stay_live_until_finished(database, engine: str):
    """A plan a stop leaves live continues deferred to its end, finishes once, and a second finish is refused."""
    database.collection_create("people", modality=Modality.BLOBS)
    database.collection_create("places", modality=Modality.BLOBS)
    database.write(list(range(64)), b"v" * 64, collection="places")
    database.write(list(range(64)), b"w" * 64, collection="people")
    plan = ustore.CompactPlan()
    assert plan.phase is ustore.Phase.NONE
    with executor_for(engine, 1) as executor:
        with pytest.raises(ustore.CancelledError):
            database.on(executor).compact(progress=lambda finished, total: False, plan=plan)
        assert plan.phase is not ustore.Phase.DONE
        database.deferred(executor).compact(plan=plan).wait()
        assert plan.phase is ustore.Phase.DONE
        database.deferred(executor).compact_plan_finish(plan).wait()
        with pytest.raises(ustore.InvalidArgumentError):
            database.on(executor).compact_plan_finish(plan)
    assert database.get(63, collection="places") == b"v" * 64
    assert database.get(63, collection="people") == b"w" * 64


def test_flush_keeps_the_data_blocking_or_deferred(database, engine: str):
    """A flush, blocking or deferred, keeps what was written."""
    database.collection_create("people", modality=Modality.BLOBS)
    database.set(1, b"kept", collection="people")
    database.flush()
    with executor_for(engine, 1) as executor:
        database.deferred(executor).flush().wait()
    assert database.get(1, collection="people") == b"kept"


@pytest.mark.skipif("rocksdb" not in ENGINES, reason="built without USTORE_WITH_ROCKSDB")
def test_a_rocksdb_view_refuses_every_change(tmp_path):
    """Reads, snapshots and their saves serve, while every call that would change the database raises ReadOnlyError."""
    with ustore.Database.open_rocksdb(tmp_path) as database:
        database.collection_create("people", modality=Modality.BLOBS)
        database.set(1, b"kept", collection="people")
    refused = [
        lambda view: view.set(2, b"refused", collection="people"),
        lambda view: view.remove(1, collection="people"),
        lambda view: view.transaction(),
        lambda view: view.collection_create("others", modality=Modality.BLOBS),
        lambda view: view.collection_drop("people"),
        lambda view: view.clear(ustore.DropMode.PAIRS),
        lambda view: view.compact(),
        lambda view: view.flush(),
    ]
    with ustore.Database.view_rocksdb(tmp_path) as view:
        assert view.get(1, collection="people") == b"kept"
        assert view.collection_contains("people")
        for call in refused:
            with pytest.raises(ustore.ReadOnlyError) as raised:
                call(view)
            assert raised.value.status is ustore.Status.READ_ONLY
        with view.snapshot() as snapshot:
            snapshot.save(tmp_path / "saved")
    with ustore.Database.open_rocksdb(tmp_path / "saved") as saved:
        assert saved.get(1, collection="people") == b"kept"
