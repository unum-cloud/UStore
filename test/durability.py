"""Crashes: what a database keeps when its process dies without closing it, reopened in the same directory.

Each case runs its writes in a child process that leaves through `os._exit`, which skips every destructor and flush
the way a killed process does, on every platform.

File: test/durability.py
Author: Ash Vardanian
Date: September 27, 2026
"""

import subprocess
import sys
import textwrap
from pathlib import Path

from capabilities import NEEDS_TRANSACTIONS, location, open_engine

import ustore


def crash_after(engine: str, directory: Path, body: str) -> None:
    """Runs `body` against `engine` opened in `directory` with a `blobs` collection, then exits without closing it."""
    opening = textwrap.dedent(f"""
        import os, sys, ustore
        database = ustore.Database.open_{engine}(sys.argv[1])
        blobs = database.collection_create("blobs", modality=ustore.Modality.BLOBS)
    """)
    script = opening + textwrap.dedent(body) + "\nos._exit(0)\n"
    # A child that never exits fails its case instead of stalling the suite.
    subprocess.run([sys.executable, "-c", script, str(location(engine, directory))], check=True, timeout=120)


def test_a_flushed_write_survives_a_crash(engine: str, tmp_path: Path):
    crash_after(engine, tmp_path, 'database.set(1, b"kept", durability=ustore.Durability.FLUSHED, collection=blobs)')
    with open_engine(engine, tmp_path) as database:
        assert database.get(1, collection="blobs") == b"kept"


@NEEDS_TRANSACTIONS
def test_a_flushed_commit_survives_a_crash(engine: str, tmp_path: Path):
    crash_after(
        engine,
        tmp_path,
        """
        transaction = database.transaction()
        transaction.set(1, b"kept", collection=blobs)
        transaction.commit(ustore.Durability.FLUSHED)
        """,
    )
    with open_engine(engine, tmp_path) as database:
        assert database.get(1, collection="blobs") == b"kept"


@NEEDS_TRANSACTIONS
def test_a_crash_keeps_whole_transactions(engine: str, tmp_path: Path):
    crash_after(
        engine,
        tmp_path,
        """
        left = database.collection_create("left", modality=ustore.Modality.BLOBS)
        right = database.collection_create("right", modality=ustore.Modality.BLOBS)
        database.set(0, b"named", durability=ustore.Durability.FLUSHED, collection=blobs)
        for key in range(64):
            with database.transaction() as transaction:
                transaction.set(key, b"x" * 4096, collection=left)
                transaction.set(key, b"x" * 4096, collection=right)
        """,
    )
    with open_engine(engine, tmp_path) as database:
        arena = ustore.Arena()
        left = [length is not None for length in database.read(list(range(64)), arena, collection="left").lengths]
        right = [length is not None for length in database.read(list(range(64)), arena, collection="right").lengths]
        assert left == right


def test_a_crash_keeps_the_names_of_collections(engine: str, tmp_path: Path):
    crash_after(engine, tmp_path, 'database.collection_create("kept", modality=ustore.Modality.DOCS)')
    with open_engine(engine, tmp_path) as database:
        assert database.collection_contains("kept")


@NEEDS_TRANSACTIONS
def test_a_crash_drops_a_staged_transaction(engine: str, tmp_path: Path):
    crash_after(
        engine,
        tmp_path,
        """
        transaction = database.transaction()
        transaction.set(1, b"staged", collection=blobs)
        transaction.stage()
        database.set(2, b"after", durability=ustore.Durability.FLUSHED, collection=blobs)
        """,
    )
    with open_engine(engine, tmp_path) as database:
        assert database.get(1, collection="blobs") is None
        assert database.get(2, collection="blobs") == b"after"
        with database.transaction() as transaction:
            transaction.set(1, b"free", collection="blobs")
        assert database.get(1, collection="blobs") == b"free"
