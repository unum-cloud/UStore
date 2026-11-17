"""The Python the website shows, line by line, on every engine, and the Quickstarts of the binding READMEs.

The site opens `ustore.umem.Database()`, which is `Database.open_ram()` and its siblings here, as the
`database` fixture does. It decodes a poster with `.decode('png')`, while a blob reads here as bytes
that an image library decodes. Its `db.transition()` is a typo for `db.transaction()`.

The Quickstarts of `python/README.md` and `mojo/README.md` run verbatim, the first fenced block
under `## Quickstart` of each, and each test asserts on what its program prints. The Python block
imports the `ustore` the suite imports, and the Mojo block finds its package through `-I mojo` and
the engine through the loader path, which the test points at the library beside the extension.

File: test/readme.py
Author: Ash Vardanian
Date: October 7, 2026
"""

import os
import shutil
import subprocess
from pathlib import Path

import pytest
from base import NEEDS_TRANSACTIONS, key, pandas_available

import ustore

ROOT = Path(__file__).resolve().parents[1]
"""The repository root, holding every binding's README."""

MOJO = shutil.which("mojo")
LIBRARY = Path(ustore.lib.__file__).resolve().parent
"""Where the extension sits, beside the shared library the Mojo package loads."""

AVATAR, ALICE, BOB, NEW, IPHONE, GOOGLE_PIXEL, TITANIC = range(1, 8)
PNG = b"\x89PNG\r\n\x1a\n"


def test_description_is_a_document(database):
    """A movie's description is a document, read back as the object written."""
    database["movies"].docs[AVATAR] = {"title": "Avatar", "year": 2009}
    description = database["movies"].docs[AVATAR]
    assert description == {"title": "Avatar", "year": 2009}


def test_thumbnail_is_a_blob(database):
    """A movie's thumbnail is a binary value, read back byte for byte."""
    database["posters"].blobs[AVATAR] = PNG
    thumbnail = database["posters"].blobs[AVATAR]
    assert thumbnail == PNG


def test_recommendations_are_edges(database):
    """Recommendations are graph edges, read back by their source."""
    database["similar"].graph.add_edge(AVATAR, TITANIC)
    recommendations = database["similar"].graph.read_edges(AVATAR)
    assert [(source, target) for source, target, _ in recommendations[0]] == [(key(AVATAR), key(TITANIC))]


def test_images_are_sampled_values(database):
    """Sampling a collection of images answers every image by its key."""
    database["pictures"].blobs[[AVATAR, TITANIC]] = [PNG, PNG]
    images = database["pictures"].sample(1000)
    assert images == {key(AVATAR): PNG, key(TITANIC): PNG}


@NEEDS_TRANSACTIONS
@pytest.mark.skipif(not pandas_available, reason="pandas is not installed")
def test_the_transaction_writes_every_modality(database):
    """One transaction writes documents, edges, values and table rows across collections."""
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
    """A database opens through an engine's constructor alone, never the bare class."""
    with pytest.raises(TypeError, match="open_ram"):
        ustore.Database()
    with ustore.Database.open_ram() as db:
        assert isinstance(db["movies"], ustore.Collection)


def quickstart(binding: str, language: str) -> str:
    """The first `language` block under `## Quickstart` of `binding/README.md`."""
    text = (ROOT / binding / "README.md").read_text()
    section = text.split("\n## Quickstart\n", 1)[1].split("\n## ", 1)[0]
    return section.split(f"```{language}\n", 1)[1].split("\n```", 1)[0]


def test_readme_quickstart(tmp_path, monkeypatch, capsys):
    """The Python Quickstart runs verbatim and prints the engine it opened."""
    monkeypatch.chdir(tmp_path)
    exec(compile(quickstart("python", "python"), "python/README.md", "exec"), {})  # noqa: S102
    assert capsys.readouterr().out == "RAM\n"


@pytest.mark.skipif(
    not MOJO or not any(LIBRARY.glob("libustore_shared.*")),
    reason="needs mojo on the path and the shared library beside the extension",
)
def test_readme_mojo_mirror(tmp_path):
    """The Mojo Quickstart runs verbatim against the library beside the extension and prints what it read."""
    environment = dict(os.environ)
    environment["LD_LIBRARY_PATH"] = os.pathsep.join(filter(None, [str(LIBRARY), environment.get("LD_LIBRARY_PATH")]))
    environment["DYLD_LIBRARY_PATH"] = environment["LD_LIBRARY_PATH"]
    # Pixi's activation sets this, which a bare `mojo` needs to find `std`
    environment.setdefault("MODULAR_HOME", str(Path(MOJO).resolve().parents[1] / "share" / "max"))
    program = tmp_path / "quickstart.mojo"
    program.write_text(quickstart("mojo", "mojo"))
    ran = subprocess.run(
        [MOJO, "run", "-I", str(ROOT / "mojo"), str(program)],
        cwd=tmp_path,
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )
    assert ran.returncode == 0, ran.stderr
    assert ran.stdout.splitlines()[-1] == "True 2009 1"
