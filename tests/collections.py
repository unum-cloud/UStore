"""Collections over the C11 core: creating, naming, listing and dropping them."""

import pytest

ustore = pytest.importorskip("ustore", reason="The `ustore.lib` extension is not built into this environment")


@pytest.fixture
def database():
    """An in-memory database, closed once the case that asked for it is done."""
    with ustore.Database() as opened:
        if not opened.supports_collections:
            pytest.skip("The engine holds only the main collection")
        yield opened


def test_create_answers_an_identifier(database):
    identifier = database.collection_create("people")
    assert isinstance(identifier, int)
    assert database.collection_find("people") == identifier


def test_names_resolve_to_the_same_collection(database):
    database.collection_create("people")
    database.set(1, b"named", collection="people")
    assert database.get(1, collection=database.collection_find("people")) == b"named"


def test_collections_hold_their_keys_apart(database):
    first = database.collection_create("first")
    second = database.collection_create("second")
    database.set(1, b"in first", collection=first)
    database.set(1, b"in second", collection=second)

    assert database.get(1, collection=first) == b"in first"
    assert database.get(1, collection=second) == b"in second"
    assert not database.contains(1)


def test_the_main_collection_is_the_default(database):
    database.set(1, b"main")
    assert database.get(1, collection=None) == b"main"
    assert database.get(1, collection="") == b"main"


def test_contains_reports_a_taken_name(database):
    assert not database.collection_contains("people")
    database.collection_create("people")
    assert database.collection_contains("people")


def test_the_empty_name_is_always_taken(database):
    assert database.collection_contains("")


def test_list_answers_every_identifier(database):
    created = {database.collection_create(name) for name in ("first", "second", "third")}
    assert created <= set(database.collection_list())


def test_names_come_in_the_order_of_their_text(database):
    for name in ("cherry", "apple", "banana"):
        database.collection_create(name)
    assert database.collection_names() == ("apple", "banana", "cherry")


def test_names_paginate(database):
    for name in ("apple", "banana", "cherry"):
        database.collection_create(name)

    page = database.collection_names(limit=2)
    assert page == ("apple", "banana")
    assert database.collection_names(after=page[-1], limit=2) == ("banana", "cherry")


def test_drop_removes_the_collection_and_its_name(database):
    database.collection_create("people")
    database.set(1, b"named", collection="people")
    database.collection_drop("people")
    assert not database.collection_contains("people")


def test_drop_frees_the_name_for_another_collection(database):
    first = database.collection_create("people")
    database.collection_drop("people")
    second = database.collection_create("people")
    assert database.collection_find("people") == second
    assert first != second or not database.contains(1, collection=second)


def test_drop_pairs_keeps_the_collection(database):
    identifier = database.collection_create("people")
    database.set(1, b"named", collection=identifier)
    database.collection_drop(identifier, mode="pairs")
    assert not database.contains(1, collection=identifier)
    assert database.collection_contains("people")


def test_the_main_collection_can_be_cleared(database):
    database.set(1, b"main")
    database.collection_drop(0, mode="pairs")
    assert not database.contains(1)


def test_creating_a_taken_name_is_refused(database):
    database.collection_create("people")
    with pytest.raises(ustore.ExistsError):
        database.collection_create("people")


def test_finding_an_unknown_name_is_refused(database):
    with pytest.raises(ustore.MissingError):
        database.collection_find("nobody")


def test_naming_the_main_collection_is_refused(database):
    with pytest.raises(ustore.ArgumentError):
        database.collection_create("")


def test_rejects_an_unknown_drop_mode(database):
    database.collection_create("people")
    with pytest.raises(ValueError):
        database.collection_drop("people", mode="everything")


def test_rejects_a_name_that_is_not_a_string(database):
    with pytest.raises(TypeError):
        database.collection_create(42)


def test_engine_reports_its_capabilities(database):
    assert isinstance(database.supports_transactions, bool)
    assert isinstance(database.supports_collections, bool)
    assert isinstance(database.supports_snapshots, bool)


def test_control_describes_the_build(database):
    described = database.control("info")
    assert isinstance(described, str)
    assert "engine" in described
