"""What the extension under test was built with, read once so every suite gates on it at collection.

The extension links one engine per build, so the engine, what it supports and the isolation levels
it can be opened at are properties of the build rather than axes a test can choose at runtime.
"""

import json
import os
import tempfile

import pytest

import ustore

ENGINE = ustore.engines_compiled()[0]

# Only SmashTable reads an "isolation" key; every other engine opens at the one level it has.
ISOLATIONS = ["strict_serializable", "snapshot"] if ENGINE == "ram" else [None]


def config(directory: str | os.PathLike[str], isolation: str | None = None) -> str:
    """The JSON configuration opening a database in `directory` at `isolation`."""
    options = {"version": "1.0", "directory": os.fspath(directory)}
    if isolation is not None:
        options["isolation"] = isolation
    return json.dumps(options)


# RocksDB and LevelDB refuse to open without a directory, so the probe always names one.
with tempfile.TemporaryDirectory() as probe_directory, ustore.Database(probe_directory) as probe:
    SUPPORTS_TRANSACTIONS = probe.supports_transactions
    SUPPORTS_COLLECTIONS = probe.supports_collections
    SUPPORTS_SNAPSHOTS = probe.supports_snapshots

NEEDS_TRANSACTIONS = pytest.mark.skipif(not SUPPORTS_TRANSACTIONS, reason=f"{ENGINE} has no transactions")
NEEDS_COLLECTIONS = pytest.mark.skipif(not SUPPORTS_COLLECTIONS, reason=f"{ENGINE} holds only the main collection")
NEEDS_SNAPSHOTS = pytest.mark.skipif(not SUPPORTS_SNAPSHOTS, reason=f"{ENGINE} captures no snapshots")
