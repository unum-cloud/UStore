"""What each engine the extension carries supports, probed once so every suite gates on it at collection.

The extension carries every engine its build enabled, so each case runs once per engine, and a case
marked as needing a capability skips on the engines that lack it. Suites write integer keys and
expect them back as the `uuid.UUID` every key is answered as, which `key` and `keys` spell.
"""

import os
import tempfile
from collections.abc import Iterable
from uuid import UUID

import pytest

import ustore

ENGINES = ustore.engines_compiled()

# Only the in-memory engine takes an isolation; every other engine opens at the one level it has.
ISOLATIONS = {"ram": ("strict-serializable", "snapshot")}


def open_engine(engine: str, directory: str | os.PathLike[str], isolation: str | None = None) -> ustore.Database:
    """Opens `engine` in `directory`, at `isolation` when the engine takes one."""
    if isolation is not None:
        return ustore.Database.open_ram(directory, isolation=isolation)
    return getattr(ustore.Database, f"open_{engine}")(directory)


def probe(engine: str) -> dict[str, bool]:
    """What `engine` supports, read from a database opened in a scratch directory."""
    with tempfile.TemporaryDirectory() as directory, open_engine(engine, directory) as database:
        return {
            "transactions": database.supports_transactions,
            "collections": database.supports_collections,
            "snapshots": database.supports_snapshots,
        }


SUPPORTS = {engine: probe(engine) for engine in ENGINES}


def key(integer: int) -> UUID:
    """The `uuid.UUID` an integer key comes back as."""
    return UUID(int=integer)


def keys(integers: Iterable[int]) -> list[UUID]:
    """The `uuid.UUID` every integer key comes back as, in order."""
    return [UUID(int=integer) for integer in integers]


NEEDS_TRANSACTIONS = pytest.mark.needs("transactions")
NEEDS_COLLECTIONS = pytest.mark.needs("collections")
NEEDS_SNAPSHOTS = pytest.mark.needs("snapshots")
