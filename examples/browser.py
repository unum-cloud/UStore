"""Streamlit app browsing three UStore collections side by side: documents, a graph and images.

Seeds each collection on first run, then renders whichever one you pick, so one app shows three
modalities over the same database: documents as JSON, the graph as its edge list, and images as
pictures decoded straight from the stored bytes.
The handle is cached with `st.cache_resource`, so Streamlit reuses one open database across reruns
rather than reopening it on every widget change.

Needs `streamlit`, `requests` and the `ustore` bindings.
Run with `streamlit run examples/browser.py`.
"""

import requests
import streamlit as st
import ustore.smashtable as ustore

DOCUMENTS_COLLECTION = "Docs"
NETWORK_COLLECTION = "Network"
IMAGES_COLLECTION = "Images"

FIRST_IMAGE_KEY = 50
REQUEST_TIMEOUT_SECONDS = 30

IMAGE_LINKS = [
    "https://upload.wikimedia.org/wikipedia/commons/thumb/8/87/Mount_Ararat_and_the_Yerevan_skyline_in_spring_from_the_Cascade.jpg/556px-Mount_Ararat_and_the_Yerevan_skyline_in_spring_from_the_Cascade.jpg",
    "https://upload.wikimedia.org/wikipedia/commons/thumb/3/31/Vartavar_2014_Yerevan_%285%29.jpg/202px-Vartavar_2014_Yerevan_%285%29.jpg",
    "https://upload.wikimedia.org/wikipedia/commons/thumb/6/6e/Cascade_of_Yerevan.JPG/168px-Cascade_of_Yerevan.JPG",
    "https://upload.wikimedia.org/wikipedia/commons/thumb/6/64/Yerevan_Tsitsernakaberd_Armenian_Genocide_Museum_Memorial_msu-2018-3009.jpg/170px-Yerevan_Tsitsernakaberd_Armenian_Genocide_Museum_Memorial_msu-2018-3009.jpg",
    "https://upload.wikimedia.org/wikipedia/commons/thumb/1/19/%C3%93pera%2C_Erev%C3%A1n%2C_Armenia%2C_2016-10-03%2C_DD_12.jpg/230px-%C3%93pera%2C_Erev%C3%A1n%2C_Armenia%2C_2016-10-03%2C_DD_12.jpg",
    "https://upload.wikimedia.org/wikipedia/commons/thumb/d/dc/Jerewan-Matenadaran-msu-wlm-2509.jpg/140px-Jerewan-Matenadaran-msu-wlm-2509.jpg",
]

LANDMARKS = {
    10: {"name": "Cascade", "district": "Kentron", "visitors": 1_200_000},
    11: {"name": "Matenadaran", "district": "Kentron", "visitors": 310_000},
    12: {"name": "Tsitsernakaberd", "district": "Shengavit", "visitors": 640_000},
}

NEIGHBORHOODS = [(10, 11), (11, 12), (12, 10)]


def seed_documents(documents) -> None:
    """Writes the landmark records, so the documents view has something structured to render."""
    if len(documents):
        return
    for key, landmark in LANDMARKS.items():
        documents[key] = landmark


def seed_network(graph) -> None:
    """Links the landmarks into a small ring, so the graph view has edges to list."""
    for source, target in NEIGHBORHOODS:
        graph.add_edge(source, target)


def seed_images(images) -> None:
    """Downloads each photograph once and stores the raw bytes under consecutive keys."""
    if len(images):
        return
    for offset, link in enumerate(IMAGE_LINKS):
        images[FIRST_IMAGE_KEY + offset] = requests.get(link, timeout=REQUEST_TIMEOUT_SECONDS).content


@st.cache_resource
def open_database():
    """Opens the database once per session and seeds every collection that is still empty."""
    database = ustore.DataBase()
    seed_documents(database[DOCUMENTS_COLLECTION].docs)
    seed_network(database[NETWORK_COLLECTION].graph)
    seed_images(database[IMAGES_COLLECTION])
    return database


def render_documents(database) -> None:
    """Renders every stored document as JSON, keyed by the document's own key."""
    for key, document in database[DOCUMENTS_COLLECTION].docs.items:
        st.write(f"Document {key}")
        st.json(document)


def render_network(database) -> None:
    """Renders the graph as its edge list, naming each endpoint from the documents collection."""
    documents = database[DOCUMENTS_COLLECTION].docs
    for source, target in database[NETWORK_COLLECTION].graph.edges:
        st.write(f"{documents[source]['name']} → {documents[target]['name']}")


def render_images(database) -> None:
    """Renders every stored image, decoded by Streamlit from the bytes the collection holds."""
    for key, image in database[IMAGES_COLLECTION].items:
        st.image(image, caption=f"Image {key}", width="content")


RENDERERS = {
    DOCUMENTS_COLLECTION: render_documents,
    NETWORK_COLLECTION: render_network,
    IMAGES_COLLECTION: render_images,
}

selected = st.selectbox("Which collection would you like to view?", tuple(RENDERERS))
st.write("You selected:", selected)
RENDERERS[selected](open_database())
