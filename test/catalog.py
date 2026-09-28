"""Collections over the C ABI: creating, naming, listing and dropping them."""

import pytest
from capabilities import NEEDS_COLLECTIONS

import ustore

pytestmark = NEEDS_COLLECTIONS


def address(database: ustore.Database, name: str, addressing: str) -> int | str:
    """The collection named `name`, spelled by name or by the identifier the name resolves to."""
    return name if addressing == "name" else database.collection_find(name)


ADDRESSINGS = ["name", "identifier"]


def test_create_answers_an_identifier(database):
    identifier = database.collection_create("people")
    assert isinstance(identifier, int)
    assert database.collection_find("people") == identifier


@pytest.mark.parametrize("addressing", ADDRESSINGS)
def test_verbs_address_a_collection_either_way(database, addressing: str):
    database.collection_create("people")
    collection = address(database, "people", addressing)
    database.set(1, b"named", collection=collection)
    database.write([2, 3], b"batch", collection=collection)
    assert database.get(1, collection="people") == b"named"
    assert [bytes(value) for value in database.read([2, 3], collection=collection)] == [b"batch"] * 2
    assert list(database.scan(0, 3, collection=collection)[0]) == [1, 2, 3]


@pytest.mark.parametrize("key", [0, 1, -1])
def test_collections_hold_their_keys_apart(database, key: int):
    first = database.collection_create("first")
    second = database.collection_create("second")
    database.set(key, b"in first", collection=first)
    database.set(key, b"in second", collection=second)

    assert database.get(key, collection=first) == b"in first"
    assert database.get(key, collection=second) == b"in second"
    assert not database.contains(key)


@pytest.mark.parametrize("main", [None, "", 0])
def test_the_main_collection_is_the_default(database, main: int | str | None):
    database.set(1, b"main")
    assert database.get(1, collection=main) == b"main"


def test_contains_reports_a_taken_name(database):
    assert not database.collection_contains("people")
    database.collection_create("people")
    assert database.collection_contains("people")


def test_the_empty_name_is_always_taken(database):
    assert database.collection_contains("")


@pytest.mark.parametrize("count", [1, 3, 20])
def test_list_answers_every_identifier(database, count: int):
    created = {database.collection_create(f"collection-{index}") for index in range(count)}
    assert created <= set(database.collection_list())


@pytest.mark.parametrize("names", [("cherry", "apple", "banana"), ("b", "a"), ("only",), ("naïve", "zebra")])
def test_names_come_in_the_order_of_their_text(database, names: tuple[str, ...]):
    for name in names:
        database.collection_create(name)
    assert database.collection_names() == tuple(sorted(names, key=str.encode))


@pytest.mark.parametrize("limit", [2, 3, 4])
def test_names_paginate(database, limit: int):
    """A page resumes from the last name it saw, which the next page repeats first."""
    names = ("apple", "banana", "cherry")
    for name in names:
        database.collection_create(name)

    walked = list(database.collection_names(limit=limit))
    while page := database.collection_names(after=walked[-1], limit=limit)[1:]:
        walked.extend(page)
    assert walked == list(names)


@pytest.mark.parametrize("addressing", ADDRESSINGS)
def test_drop_removes_the_collection(database, addressing: str):
    database.collection_create("people")
    database.set(1, b"named", collection="people")
    database.collection_drop(address(database, "people", addressing))
    if addressing == "name":
        assert not database.collection_contains("people")


def test_drop_frees_the_name_for_another_collection(database):
    database.collection_create("people")
    database.collection_drop("people")
    second = database.collection_create("people")
    assert database.collection_find("people") == second
    assert not database.contains(1, collection=second)


@pytest.mark.parametrize("mode", ["pairs", "values"])
def test_emptying_keeps_the_collection(database, mode: str):
    identifier = database.collection_create("people")
    database.set(1, b"named", collection=identifier)
    database.collection_drop(identifier, mode=mode)
    assert database.collection_contains("people")
    if mode == "pairs":
        assert not database.contains(1, collection=identifier)
    else:
        assert database.get(1, collection=identifier) in (None, b"")


def test_the_main_collection_can_be_cleared(database):
    database.set(1, b"main")
    database.collection_drop(0, mode="pairs")
    assert not database.contains(1)


@pytest.mark.parametrize(
    ("call", "failure"),
    [
        (lambda database: database.collection_create("people"), ustore.ExistsError),
        (lambda database: database.collection_find("nobody"), ustore.MissingError),
        (lambda database: database.get(1, collection="nobody"), ustore.MissingError),
        (lambda database: database.collection_create(""), ustore.ArgumentError),
        (lambda database: database.collection_drop("people", mode="everything"), ValueError),
        (lambda database: database.collection_create(42), TypeError),
        (lambda database: database.get(1, collection=1.5), TypeError),
    ],
    ids=["taken-name", "unknown-name", "unknown-name-in-a-verb", "empty-name", "drop-mode", "integer-name", "float"],
)
def test_refusals(database, call, failure: type):
    database.collection_create("people")
    with pytest.raises(failure):
        call(database)


def test_engine_reports_its_capabilities(database):
    assert isinstance(database.supports_transactions, bool)
    assert database.supports_collections


def test_control_describes_the_build(database, engine: str):
    described = database.control("info")
    assert isinstance(described, str)
    assert engine in described or "engine" in described
