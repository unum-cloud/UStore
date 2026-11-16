"""The Python the website shows, line by line, on every engine, and the binding Quickstarts.

The site opens `ustore.umem.Database()`, which is `Database.open_ram()` and its siblings here, as the
`database` fixture does. It decodes a poster with `.decode('png')`, while a blob reads here as bytes
that an image library decodes. Its `db.transition()` is a typo for `db.transaction()`.

The Quickstarts of `python/README.md` and `mojo/README.md` run verbatim, the first fenced block
under `## Quickstart` of each, and each test asserts on what its program prints. The Python block
imports the `ustore` the suite imports, and the Mojo block finds its package through `-I mojo` and
the engine through the loader path, which the test points at the library beside the extension.

File: test/website.py
Author: Ash Vardanian
Date: October 7, 2026
"""

import os
import shutil
import subprocess
from pathlib import Path

import pytest
from capabilities import NEEDS_TRANSACTIONS, key

import ustore

ROOT = Path(__file__).resolve().parents[1]
"""The repository root, holding every binding's README."""

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


def quickstart(binding: str, language: str) -> str:
    """The first `language` block under `## Quickstart` of `binding/README.md`."""
    text = (ROOT / binding / "README.md").read_text()
    section = text.split("\n## Quickstart\n", 1)[1].split("\n## ", 1)[0]
    return section.split(f"```{language}\n", 1)[1].split("\n```", 1)[0]


def test_quickstart(tmp_path, monkeypatch, capsys):
    monkeypatch.chdir(tmp_path)
    exec(compile(quickstart("python", "python"), "python/README.md", "exec"), {})  # noqa: S102
    assert capsys.readouterr().out == "RAM\n"


def test_mojo_quickstart(tmp_path):
    mojo = shutil.which("mojo")
    library = Path(ustore.lib.__file__).resolve().parent
    if not mojo or not any(library.glob("libustore_shared.*")):
        pytest.skip("needs `mojo` on the path and the shared library beside the extension")
    environment = dict(os.environ)
    environment["LD_LIBRARY_PATH"] = os.pathsep.join(filter(None, [str(library), environment.get("LD_LIBRARY_PATH")]))
    environment["DYLD_LIBRARY_PATH"] = environment["LD_LIBRARY_PATH"]
    # Pixi's activation sets this, which a bare `mojo` needs to find `std`
    environment.setdefault("MODULAR_HOME", str(Path(mojo).resolve().parents[1] / "share" / "max"))
    program = tmp_path / "quickstart.mojo"
    program.write_text(quickstart("mojo", "mojo"))
    ran = subprocess.run(
        [mojo, "run", "-I", str(ROOT / "mojo"), str(program)],
        cwd=tmp_path,
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )
    assert ran.returncode == 0, ran.stderr
    assert ran.stdout.splitlines()[-1] == "True 2009 1"
