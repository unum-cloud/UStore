"""Streamlit app browsing three UStore collections side by side: documents, a graph and images.

Seeds each collection on first run, then renders whichever one you pick, so one app shows three
modalities over the same database: documents as JSON, the graph as its edge list, and images as
pictures decoded straight from the stored bytes.
The handle is cached with `st.cache_resource`, so Streamlit reuses one open database across reruns
rather than reopening it on every widget change.

Needs `streamlit`, `requests` and the `ustore` bindings.
Run with `streamlit run examples/browser.py`.

File: examples/browser.py
Author: Ash Vardanian
Date: September 16, 2026
"""

import json
from typing import NamedTuple

import requests
import streamlit as st

import ustore
from ustore.graph_view import DiGraph
from ustore.keys import KEY_MIN, scan_keys

DOCUMENTS_COLLECTION = "Docs"
NETWORK_COLLECTION = "Network"
IMAGES_COLLECTION = "Images"

FIRST_IMAGE_KEY = 50
REQUEST_TIMEOUT_SECONDS = 30
USER_AGENT = "ustore-examples/1.0 (https://github.com/unum-cloud/UStore)"

# Wikimedia refuses an anonymous request for a `thumb/` path, so every photograph is scaled through
# `Special:FilePath`, which serves one and redirects to it.
IMAGE_WIDTH_PIXELS = 320
IMAGE_FILES = [
    "Mount_Ararat_and_the_Yerevan_skyline_in_spring_from_the_Cascade.jpg",
    "Vartavar_2014_Yerevan_%285%29.jpg",
    "Cascade_of_Yerevan.JPG",
    "Yerevan_Tsitsernakaberd_Armenian_Genocide_Museum_Memorial_msu-2018-3009.jpg",
    "%C3%93pera%2C_Erev%C3%A1n%2C_Armenia%2C_2016-10-03%2C_DD_12.jpg",
    "Jerewan-Matenadaran-msu-wlm-2509.jpg",
]
IMAGE_LINKS = [
    f"https://commons.wikimedia.org/wiki/Special:FilePath/{name}?width={IMAGE_WIDTH_PIXELS}" for name in IMAGE_FILES
]

LANDMARKS = {
    10: {"name": "Cascade", "district": "Kentron", "visitors": 1_200_000},
    11: {"name": "Matenadaran", "district": "Kentron", "visitors": 310_000},
    12: {"name": "Tsitsernakaberd", "district": "Shengavit", "visitors": 640_000},
}

NEIGHBORHOODS = [(10, 11), (11, 12), (12, 10)]


class Store(NamedTuple):
    """The open database beside the graph view and the two collections rendered from their raw bytes."""

    database: ustore.Database
    documents: int
    network: DiGraph
    images: int


def seeded(database: ustore.Database, collection: int) -> bool:
    """Whether a collection already holds a pair, so a rerun never seeds it twice."""
    return len(database.scan(KEY_MIN, 1, ustore.Arena(), collection=collection)[0]) > 0


def seed_documents(store: Store) -> None:
    """Writes the landmark records, so the documents view has something structured to render."""
    if seeded(store.database, store.documents):
        return
    documents = [json.dumps(landmark) for landmark in LANDMARKS.values()]
    store.database.docs_write(list(LANDMARKS), documents, collection=store.documents)


def seed_network(store: Store) -> None:
    """Links the landmarks into a small ring, so the graph view has edges to list."""
    sources = [source for source, _ in NEIGHBORHOODS]
    targets = [target for _, target in NEIGHBORHOODS]
    store.network.add_edges_from_arrays(sources, targets)


def seed_images(store: Store) -> None:
    """Downloads each photograph once and stores the raw bytes under consecutive keys."""
    if seeded(store.database, store.images):
        return
    keys = [FIRST_IMAGE_KEY + offset for offset in range(len(IMAGE_LINKS))]
    downloaded = [
        requests.get(link, timeout=REQUEST_TIMEOUT_SECONDS, headers={"User-Agent": USER_AGENT}).content
        for link in IMAGE_LINKS
    ]
    store.database.write(keys, downloaded, collection=store.images)


@st.cache_resource
def open_store() -> Store:
    """Opens the database once per session and seeds every collection that is still empty."""
    database = ustore.Database.open_ram()
    store = Store(
        database=database,
        documents=database.collection_create(DOCUMENTS_COLLECTION, modality=ustore.Modality.DOCS),
        network=DiGraph(database, NETWORK_COLLECTION),
        images=database.collection_create(IMAGES_COLLECTION, modality=ustore.Modality.BLOBS),
    )
    seed_documents(store)
    seed_network(store)
    seed_images(store)
    return store


def landmark_names(store: Store, keys: list[int]) -> list[str]:
    """The name of every landmark, read as one batch out of the documents collection."""
    found = store.database.docs_read(
        keys, ustore.Arena(), field="name", outputs_type=ustore.FieldType.STRING, collection=store.documents
    )
    return ["" if name is None else bytes(name).decode() for name in found]


def render_documents(store: Store) -> None:
    """Renders every stored document as JSON, keyed by the document's own key."""
    keys = list(scan_keys(store.database, store.documents))
    documents = store.database.docs_read(keys, ustore.Arena(), collection=store.documents)
    for key, document in zip(keys, documents, strict=True):
        st.write(f"Document {key}")
        st.json(bytes(document).decode())


def render_network(store: Store) -> None:
    """Renders the graph as its edge list, naming each endpoint from the documents collection."""
    edges = list(store.network.edges())
    sources = landmark_names(store, [source for source, _ in edges])
    targets = landmark_names(store, [target for _, target in edges])
    for source, target in zip(sources, targets, strict=True):
        st.write(f"{source} → {target}")


def render_images(store: Store) -> None:
    """Renders every stored image, decoded by Streamlit from the bytes the collection holds."""
    keys = list(scan_keys(store.database, store.images))
    for key, image in zip(keys, store.database.read(keys, ustore.Arena(), collection=store.images), strict=True):
        st.image(bytes(image), caption=f"Image {key}", width="content")


RENDERERS = {
    DOCUMENTS_COLLECTION: render_documents,
    NETWORK_COLLECTION: render_network,
    IMAGES_COLLECTION: render_images,
}

selected = st.selectbox("Which collection would you like to view?", tuple(RENDERERS))
st.write("You selected:", selected)
RENDERERS[selected](open_store())
