"""Crashes: what a database keeps when its process dies without closing it, reopened in the same directory.

Each case runs its writes in a child process that leaves through `os._exit`, which skips every destructor and flush
the way a killed process does, on every platform.
"""

import subprocess
import sys
import textwrap
from pathlib import Path

from capabilities import NEEDS_COLLECTIONS, NEEDS_TRANSACTIONS, config

import ustore


def crash_after(directory: Path, body: str) -> None:
    """Runs `body` against a database opened in `directory`, then exits the process without closing it."""
    opening = "import os, sys, ustore\ndatabase = ustore.Database(config=sys.argv[1])\n"
    script = opening + textwrap.dedent(body) + "\nos._exit(0)\n"
    subprocess.run([sys.executable, "-c", script, config(directory)], check=True)


def test_a_flushed_write_survives_a_crash(tmp_path: Path):
    crash_after(tmp_path, 'database.set(1, b"kept", flush=True)')
    with ustore.Database(config=config(tmp_path)) as database:
        assert database.get(1) == b"kept"


@NEEDS_TRANSACTIONS
def test_a_flushed_commit_survives_a_crash(tmp_path: Path):
    crash_after(
        tmp_path,
        """
        transaction = database.transaction()
        transaction.set(1, b"kept")
        transaction.commit(flush=True)
        """,
    )
    with ustore.Database(config=config(tmp_path)) as database:
        assert database.get(1) == b"kept"


@NEEDS_TRANSACTIONS
@NEEDS_COLLECTIONS
def test_a_crash_keeps_whole_transactions(tmp_path: Path):
    crash_after(
        tmp_path,
        """
        left = database.collection_create("left")
        right = database.collection_create("right")
        database.set(0, b"named", flush=True)
        for key in range(64):
            with database.transaction() as transaction:
                transaction.set(key, b"x" * 4096, collection=left)
                transaction.set(key, b"x" * 4096, collection=right)
        """,
    )
    with ustore.Database(config=config(tmp_path)) as database:
        left = [length is not None for length in database.read(list(range(64)), collection="left").lengths]
        right = [length is not None for length in database.read(list(range(64)), collection="right").lengths]
        assert left == right


@NEEDS_TRANSACTIONS
def test_a_crash_drops_a_staged_transaction(tmp_path: Path):
    crash_after(
        tmp_path,
        """
        transaction = database.transaction()
        transaction.set(1, b"staged")
        transaction.stage()
        database.set(2, b"after", flush=True)
        """,
    )
    with ustore.Database(config=config(tmp_path)) as database:
        assert database.get(1) is None
        assert database.get(2) == b"after"
        with database.transaction() as transaction:
            transaction.set(1, b"free")
        assert database.get(1) == b"free"
