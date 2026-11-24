"""Collections addressed by name: resolution at first use, lookups that only find, and the modality views.

File: test/collection.py
Author: Ash Vardanian
Date: October 7, 2026
"""

import gc
import json

import numpy as np
import pytest
from base import NEEDS_TRANSACTIONS, key, keys, pyarrow_available

import ustore

NESTED = {"s": "naïve", "n": -3, "u": 2**63, "wide": 2**80, "f": 2.5, "t": True, "z": None, "o": {"a": [1, "two"]}}


def test_indexing_resolves_on_first_use(database):
    """Naming a missing collection costs nothing, while reading from it is refused."""
    posters = database["posters"]
    assert not database.collection_contains("posters")
    with pytest.raises(ustore.NotFoundError):
        posters.blobs[1]
    with pytest.raises(ustore.NotFoundError):
        assert 1 not in posters.docs


@pytest.mark.parametrize(("member", "written"), [("blobs", b"bytes"), ("docs", {"k": 1}), ("docs_json", "{}")])
def test_a_write_to_a_missing_name_never_creates(database, member: str, written: bytes | dict | str):
    """A write through a view of a missing name raises NotFoundError, leaving creation to `collection_create`."""
    with pytest.raises(ustore.NotFoundError):
        getattr(database["things"], member)[1] = written
    assert not database.collection_contains("things")


@pytest.mark.parametrize("member", ["graph", "table"])
def test_graph_and_table_of_a_missing_name_raise(database, member: str):
    """Reaching the graph or table view of a missing name raises NotFoundError rather than creating it."""
    with pytest.raises(ustore.NotFoundError):
        getattr(database["declared"], member)
    assert not database.collection_contains("declared")


@pytest.mark.parametrize("modality", list(ustore.Modality))
def test_a_created_collection_keeps_its_modality(database, modality: ustore.Modality):
    """Every collection holds the one modality it was created with, so the views of the others are refused."""
    database.collection_create("declared", modality=modality)
    assert database["declared"].modality is modality
    if modality is not ustore.Modality.BLOBS:
        with pytest.raises(ustore.InvalidArgumentError):
            database["declared"].blobs[1] = b"refused"
    if modality is not ustore.Modality.DOCS:
        with pytest.raises(ustore.InvalidArgumentError):
            database["declared"].docs[1] = {"k": 1}


def test_the_empty_name_addresses_no_collection(database):
    """The empty name raises NotFoundError through a mapping view and a verb alike."""
    with pytest.raises(ustore.NotFoundError):
        database[""].blobs[1]
    with pytest.raises(ustore.NotFoundError):
        database.get(1, collection="")


def test_docs_round_trip_python_objects(docs, view):
    """Documents come back as the objects written, and text is a JSON string rather than JSON."""
    people = view["docs"]
    people.docs[1] = NESTED
    people.docs[2] = '{"not": "parsed"}'
    people.docs[3] = [1, 2]
    assert people.docs[1] == NESTED
    assert people.docs[2] == '{"not": "parsed"}'
    assert people.docs[[3, 4]] == [[1, 2], None]
    assert json.loads(people.docs_json[1]) == NESTED


def test_docs_refuse_what_json_cannot_hold(database, docs):
    """Sets and keys that are no strings raise TypeError, since JSON holds neither."""
    with pytest.raises(TypeError):
        database["docs"].docs[1] = {"tags": {"a"}}
    with pytest.raises(TypeError):
        database["docs"].docs[1] = {1: "one"}


@pytest.mark.parametrize(
    ("member", "writer", "written"),
    [("blobs", "blobs", b"{}"), ("docs", "docs", {}), ("docs_json", "docs", {})],
    ids=["blobs", "docs", "docs_json"],
)
def test_missing_keys(database, member: str, writer: str, written: bytes | dict):
    """One missing key raises KeyError as a mapping does, while a batch answers None in its place."""
    database.collection_create("things", modality=ustore.Modality.BLOBS if writer == "blobs" else ustore.Modality.DOCS)
    getattr(database["things"], writer)[1] = written
    mapping = getattr(database["things"], member)
    with pytest.raises(KeyError):
        mapping[2]
    assert 1 in mapping
    assert 2 not in mapping
    assert mapping[[2, 1]][0] is None


def test_blobs_copy_out_of_a_one_off_arena(database, blobs, arena):
    """A mapping view has no place for an arena, so it copies, while `read` with an arena of yours does not."""
    database["blobs"].blobs[[1, 2]] = [b"first", b"second"]
    poster = database["blobs"].blobs[1]
    batch = database["blobs"].blobs[[1, 2]]
    gc.collect()
    assert poster == b"first"
    assert batch == [b"first", b"second"]
    viewed = database.read([1, 2], arena, collection=blobs)
    assert isinstance(viewed[0], memoryview)
    assert [bytes(value) for value in viewed] == batch


def test_deleting_removes_and_never_creates(database):
    """Deleting from a missing collection raises without creating it, and deleting a key removes it."""
    with pytest.raises(ustore.NotFoundError):
        del database["posters"].blobs[1]
    assert not database.collection_contains("posters")
    database.collection_create("posters", modality=ustore.Modality.BLOBS)
    database["posters"].blobs[1] = b"poster"
    del database["posters"].blobs[1]
    assert 1 not in database["posters"].blobs


@NEEDS_TRANSACTIONS
def test_transactions_stage_writes_into_a_created_collection(database):
    """A write through a transaction's view stages there, unseen at the head until the commit publishes it."""
    database.collection_create("payments", modality=ustore.Modality.DOCS)
    with database.transaction() as staged:
        payments = staged["payments"]
        payments.docs[1] = {"amount": 10}
        assert 1 not in database["payments"].docs
        assert payments.docs[1] == {"amount": 10}
    assert database["payments"].docs[1] == {"amount": 10}


def test_sample_answers_keys_onto_values(database, blobs, arena):
    """A collection samples a dict of keys onto values, and its key sample and scan stay inside it."""
    stored = {key(1): b"one", key(2): b"two", key(3): b"three"}
    database["blobs"].blobs[list(stored)] = list(stored.values())
    assert database["blobs"].sample(1000) == stored
    assert set(database["blobs"].sample_keys(2, arena)[0]) <= set(stored)
    assert list(database["blobs"].scan(2, 5, arena)[0]) == keys([2, 3])


def test_docs_write_serializes_dicts(database, docs):
    """A dict is one document for every key, not the sequence of its keys, and dicts inside a batch serialize too."""
    database.docs_write([1, 2], {"same": True}, collection=docs)
    database.docs_write([3, 4], [{"n": 3}, '{"n": 4}'], collection=docs)
    assert database["docs"].docs[[1, 2, 3, 4]] == [{"same": True}, {"same": True}, {"n": 3}, {"n": 4}]


@pytest.mark.skipif(not pyarrow_available, reason="PyArrow is not installed")
def test_values_export_as_arrow_binary(database, blobs, arena):
    """A small read writes 32-bit offsets, so it exports as Arrow `binary` over the arena's own bytes."""
    import pyarrow

    database["blobs"].blobs[[1, 2]] = [b"first", b""]
    values = database.read([1, 3, 2], arena, collection=blobs)
    array = pyarrow.array(values)
    assert array.type == pyarrow.binary()
    assert array.to_pylist() == [b"first", None, b""]
    assert array.buffers()[2].address == np.frombuffer(values, dtype=np.uint8).ctypes.data
