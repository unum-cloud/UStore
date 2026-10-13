"""The Python the website shows, line by line, on every engine.

The site opens `ustore.umem.Database()`, which is `Database.open_ram()` and its siblings here, as the
`database` fixture does. It decodes a poster with `.decode('png')`, while a blob reads here as bytes
that an image library decodes. Its `db.transition()` is a typo for `db.transaction()`.
"""

import pytest
from capabilities import NEEDS_TRANSACTIONS, key

import ustore

AVATAR, ALICE, BOB, NEW, IPHONE, GOOGLE_PIXEL, TITANIC = range(1, 8)
PNG = b"\x89PNG\r\n\x1a\n"


def test_description_is_a_document(database):
    database["movies"].docs[AVATAR] = {"title": "Avatar", "year": 2009}
    description = database["movies"].docs[AVATAR]
    assert description == {"title": "Avatar", "year": 2009}


def test_thumbnail_is_a_blob(database):
    database["posters"].blobs[AVATAR] = PNG
    thumbnail = database["posters"].blobs[AVATAR]
    assert thumbnail == PNG


def test_recommendations_are_edges(database):
    database["similar"].graph.add_edge(AVATAR, TITANIC)
    recommendations = database["similar"].graph.read_edges(AVATAR)
    assert [(source, target) for source, target, _ in recommendations[0]] == [(key(AVATAR), key(TITANIC))]


def test_images_are_sampled_values(database):
    database["pictures"].blobs[[AVATAR, TITANIC]] = [PNG, PNG]
    images = database["pictures"].sample(1000)
    assert images == {key(AVATAR): PNG, key(TITANIC): PNG}


@NEEDS_TRANSACTIONS
def test_the_transaction_writes_every_modality(database):
    pytest.importorskip("pandas")
    with database.transaction() as txn:
        txn["payments"].docs[NEW] = {"from": ALICE, "to": BOB, "amount": 10_000}
        txn["connections"].graph.add_edge(ALICE, BOB)
        txn["posters"].blobs[IPHONE] = PNG
        txn["products"].docs[IPHONE] = {"title": "iPhone", "price": 999}
        txn["relations"].graph.add_edge(IPHONE, GOOGLE_PIXEL)
        txn["availability"].table.loc[[IPHONE]].update({"quantity": 100})

    assert database["payments"].docs[NEW] == {"from": ALICE, "to": BOB, "amount": 10_000}
    assert database["connections"].graph.has_edge(ALICE, BOB)
    assert database["posters"].blobs[IPHONE] == PNG
    assert database["products"].docs[IPHONE] == {"title": "iPhone", "price": 999}
    assert database["relations"].graph.has_edge(IPHONE, GOOGLE_PIXEL)
    assert database["availability"].docs[IPHONE] == {"quantity": 100}


def test_the_database_opens_by_engine():
    with pytest.raises(TypeError, match="open_ram"):
        ustore.Database()
    with ustore.Database.open_ram() as db:
        assert isinstance(db["movies"], ustore.Collection)
