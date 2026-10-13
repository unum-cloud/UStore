"""Collections addressed by name: resolution at first use, creation by the first write, and the modality views."""

import gc
import json

import numpy as np
import pytest
from capabilities import NEEDS_TRANSACTIONS, key, keys

import ustore

NESTED = {"s": "naïve", "n": -3, "u": 2**63, "wide": 2**80, "f": 2.5, "t": True, "z": None, "o": {"a": [1, "two"]}}


def test_indexing_resolves_on_first_use(database):
    """Naming a missing collection costs nothing, while reading from it is refused."""
    posters = database["posters"]
    assert not database.collection_contains("posters")
    with pytest.raises(ustore.NotFoundError, match="posters"):
        posters.blobs[1]
    with pytest.raises(ustore.NotFoundError):
        assert 1 not in posters.docs


@pytest.mark.parametrize(
    ("member", "modality", "other"),
    [("blobs", ustore.Modality.BLOBS, "docs"), ("docs", ustore.Modality.DOCS, "blobs")],
)
def test_first_write_declares_the_modality(database, member: str, modality: ustore.Modality, other: str):
    written = b"bytes" if modality is ustore.Modality.BLOBS else {"k": 1}
    getattr(database["things"], member)[1] = written
    assert database["things"].modality is modality
    assert database["things"].id == database.collection_find("things", ustore.Arena())

    with pytest.raises(ustore.InvalidArgumentError, match=f"'things' holds {member}, not {other}"):
        getattr(database["things"], other)[1]


@pytest.mark.parametrize(("member", "modality"), [("graph", ustore.Modality.GRAPH), ("table", ustore.Modality.DOCS)])
def test_graph_and_table_declare_on_access(database, member: str, modality: ustore.Modality):
    getattr(database["declared"], member)
    assert database["declared"].modality is modality
    with pytest.raises(ustore.InvalidArgumentError, match="'declared' holds"):
        database["declared"].blobs[1] = b"refused"


def test_any_collection_accepts_every_view(database):
    """The binding lets every view through, while RAM's engine still keeps a graph out of an 'any' collection."""
    database.collection_create("mixed")
    mixed = database["mixed"]
    mixed.blobs[1] = b"bytes"
    mixed.docs[2] = {"k": 2}
    assert mixed.graph.graph_collection == mixed.id
    assert mixed.modality is ustore.Modality.ANY
    assert mixed.blobs[1] == b"bytes"
    assert mixed.docs[2] == {"k": 2}


def test_main_collection_is_the_empty_name(database):
    database.set(1, b"main")
    assert database[""].blobs[1] == b"main"


def test_docs_round_trip_python_objects(view):
    """Documents come back as the objects written, and text is a JSON string rather than JSON."""
    people = view["people"]
    people.docs[1] = NESTED
    people.docs[2] = '{"not": "parsed"}'
    people.docs[3] = [1, 2]
    assert people.docs[1] == NESTED
    assert people.docs[2] == '{"not": "parsed"}'
    assert people.docs[[3, 4]] == [[1, 2], None]
    assert json.loads(people.docs_json[1]) == NESTED


def test_docs_refuse_what_json_cannot_hold(database):
    with pytest.raises(TypeError, match="set"):
        database["people"].docs[1] = {"tags": {"a"}}
    with pytest.raises(TypeError, match="keys are strings"):
        database["people"].docs[1] = {1: "one"}


@pytest.mark.parametrize(
    ("member", "writer", "written"),
    [("blobs", "blobs", b"{}"), ("docs", "docs", {}), ("docs_json", "docs", {})],
    ids=["blobs", "docs", "docs_json"],
)
def test_missing_keys(database, member: str, writer: str, written: bytes | dict):
    """One missing key raises KeyError as a mapping does, while a batch answers None in its place."""
    getattr(database["things"], writer)[1] = written
    mapping = getattr(database["things"], member)
    with pytest.raises(KeyError):
        mapping[2]
    assert 1 in mapping and 2 not in mapping
    assert mapping[[2, 1]][0] is None


def test_blobs_copy_out_of_a_one_off_arena(database, arena):
    """A mapping view has no place for an arena, so it copies, while `read` with an arena of yours does not."""
    database["posters"].blobs[[1, 2]] = [b"first", b"second"]
    poster = database["posters"].blobs[1]
    batch = database["posters"].blobs[[1, 2]]
    gc.collect()
    assert poster == b"first" and batch == [b"first", b"second"]
    borrowed = database.read([1, 2], arena, collection="posters")
    assert isinstance(borrowed[0], memoryview) and [bytes(value) for value in borrowed] == batch


def test_deleting_removes_and_never_creates(database):
    with pytest.raises(ustore.NotFoundError):
        del database["posters"].blobs[1]
    assert not database.collection_contains("posters")
    database["posters"].blobs[1] = b"poster"
    del database["posters"].blobs[1]
    assert 1 not in database["posters"].blobs


@NEEDS_TRANSACTIONS
def test_transactions_stage_writes_and_create_at_the_head(database):
    """A collection created for a transaction is newer than its view, so the staged read reuses the one resolved."""
    with database.transaction() as staged:
        payments = staged["payments"]
        payments.docs[1] = {"amount": 10}
        assert database.collection_contains("payments")
        assert 1 not in database["payments"].docs
        assert payments.docs[1] == {"amount": 10}
    assert database["payments"].docs[1] == {"amount": 10}


def test_sample_answers_keys_onto_values(database, arena):
    stored = {key(1): b"one", key(2): b"two", key(3): b"three"}
    database["pictures"].blobs[list(stored)] = list(stored.values())
    assert database["pictures"].sample(1000) == stored
    assert set(database["pictures"].sample_keys(2, arena)[0]) <= set(stored)
    assert list(database["pictures"].scan(2, 5, arena)[0]) == keys([2, 3])


def test_docs_write_serializes_dicts(database):
    """A dict is one document for every key, not the sequence of its keys, and dicts inside a batch serialize too."""
    database.docs_write([1, 2], {"same": True})
    database.docs_write([3, 4], [{"n": 3}, '{"n": 4}'])
    assert database[""].docs[[1, 2, 3, 4]] == [{"same": True}, {"same": True}, {"n": 3}, {"n": 4}]


def test_values_export_as_arrow_binary(database, arena):
    """A small read writes 32-bit offsets, so it exports as Arrow `binary` over the arena's own bytes."""
    pyarrow = pytest.importorskip("pyarrow")
    database["posters"].blobs[[1, 2]] = [b"first", b""]
    values = database.read([1, 3, 2], arena, collection="posters")
    array = pyarrow.array(values)
    assert array.type == pyarrow.binary()
    assert array.to_pylist() == [b"first", None, b""]
    assert array.buffers()[2].address == np.frombuffer(values, dtype=np.uint8).ctypes.data
