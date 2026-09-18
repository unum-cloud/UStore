"""Fixtures every Python suite shares: databases in a fresh directory, and the views verbs run through."""

from collections.abc import Callable, Iterator
from pathlib import Path

import pytest
import ustore
from ustore_capabilities import (
    ENGINE,
    NEEDS_TRANSACTIONS,
    SUPPORTS_COLLECTIONS,
    SUPPORTS_SNAPSHOTS,
    SUPPORTS_TRANSACTIONS,
    config,
)


def pytest_report_header() -> list[str]:
    """The build this run exercises, printed where pytest prints its own header."""
    return [
        f"ustore: {ustore.__version__} from {ustore.lib.__file__}",
        f"engine: {ENGINE}",
        f"transactions: {SUPPORTS_TRANSACTIONS}, collections: {SUPPORTS_COLLECTIONS}",
        f"snapshots: {SUPPORTS_SNAPSHOTS}",
    ]


@pytest.fixture
def open_database(tmp_path: Path) -> Iterator[Callable[..., ustore.Database]]:
    """Opens databases in directories of their own, closing whichever the case left open."""
    opened: list[ustore.Database] = []

    def open_one(isolation: str | None = None) -> ustore.Database:
        directory = tmp_path / f"database-{len(opened)}"
        directory.mkdir()
        opened.append(ustore.Database(config=config(directory, isolation)))
        return opened[-1]

    yield open_one
    for database in opened:
        database.close()


@pytest.fixture
def database(open_database: Callable[..., ustore.Database]) -> ustore.Database:
    """A database at the engine's default isolation, closed once the case is done."""
    return open_database()


@pytest.fixture(params=["database", pytest.param("transaction", marks=NEEDS_TRANSACTIONS)])
def view(request: pytest.FixtureRequest, database: ustore.Database) -> Iterator[ustore.Database | ustore.Transaction]:
    """The database itself, or a transaction over it that commits once the case is done."""
    if request.param == "database":
        yield database
        return
    with database.transaction() as staged:
        yield staged
