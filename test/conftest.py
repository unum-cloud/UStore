"""Fixtures every Python suite shares: databases in a fresh directory, and the views verbs run through."""

import random
from collections.abc import Callable, Iterator
from pathlib import Path

import pytest
from base import SETTINGS
from capabilities import ENGINES, ISOLATIONS, SUPPORTS, open_engine

import ustore


def pytest_configure(config: pytest.Config) -> None:
    config.addinivalue_line("markers", "needs(capability): skips on the engines that lack the capability")


def pytest_report_header() -> list[str]:
    """The build this run exercises, printed where pytest prints its own header."""
    header = [f"- UStore: {ustore.__version__} from {ustore.lib.__file__}"]
    for engine in ENGINES:
        supported = ", ".join(name for name, present in SUPPORTS[engine].items() if present)
        header.append(f"- Engine {engine}: {supported or 'no transactions, collections or snapshots'}")
    header.append(f"- Seed: {SETTINGS.seed}")
    header.append(f"- Filter: {SETTINGS.filter or 'none'}")
    return header


def pytest_collection_modifyitems(config: pytest.Config, items: list[pytest.Item]) -> None:
    """Keeps only the tests whose node id `USTORE_FILTER` selects, on top of any `-k`."""
    deselected = [item for item in items if not SETTINGS.selects(item.nodeid)]
    if deselected:
        config.hook.pytest_deselected(items=deselected)
        items[:] = [item for item in items if SETTINGS.selects(item.nodeid)]


@pytest.fixture
def seed() -> int:
    """The seed of this run, 42 unless `USTORE_SEED` names another or is `random`."""
    return SETTINGS.seed


@pytest.fixture
def rng(seed: int) -> random.Random:
    """An RNG private to this test, so a neighbour's draws cannot shift this one's."""
    return random.Random(seed)


def pytest_generate_tests(metafunc: pytest.Metafunc) -> None:
    """Runs every case that opens a database once per engine, and at each isolation of an engine that takes one."""
    if "engine" not in metafunc.fixturenames:
        return
    needs = [mark.args[0] for mark in metafunc.definition.iter_markers("needs")]

    def configuration(engine: str, isolation: str | None = None):
        missing = [need for need in needs if not SUPPORTS[engine][need]]
        skipping = [pytest.mark.skip(reason=f"{engine} has no {' or '.join(missing)}")] if missing else []
        if "isolation" not in metafunc.fixturenames:
            return pytest.param(engine, marks=skipping, id=engine)
        return pytest.param(engine, isolation, marks=skipping, id=f"{engine}-{isolation}" if isolation else engine)

    if "isolation" in metafunc.fixturenames:
        configurations = [
            configuration(engine, isolation) for engine in ENGINES for isolation in ISOLATIONS.get(engine, (None,))
        ]
        metafunc.parametrize(("engine", "isolation"), configurations)
    else:
        metafunc.parametrize("engine", [configuration(engine) for engine in ENGINES])


@pytest.fixture
def open_database(tmp_path: Path, engine: str) -> Iterator[Callable[..., ustore.Database]]:
    """Opens databases of the engine in directories of their own, closing whichever the case left open."""
    opened: list[ustore.Database] = []

    def open_one(isolation: str | None = None) -> ustore.Database:
        directory = tmp_path / f"database-{len(opened)}"
        directory.mkdir()
        opened.append(open_engine(engine, directory, isolation))
        return opened[-1]

    yield open_one
    for database in opened:
        database.close()


@pytest.fixture
def database(open_database: Callable[..., ustore.Database]) -> ustore.Database:
    """A database at the engine's default isolation, closed once the case is done."""
    return open_database()


@pytest.fixture(params=["database", "transaction"])
def view(
    request: pytest.FixtureRequest, engine: str, database: ustore.Database
) -> Iterator[ustore.Database | ustore.Transaction]:
    """The database itself, or a transaction over it that commits once the case is done."""
    if request.param == "database":
        yield database
        return
    if not SUPPORTS[engine]["transactions"]:
        pytest.skip(f"{engine} has no transactions")
    with database.transaction() as staged:
        yield staged
