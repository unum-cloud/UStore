"""Fixtures every Python suite shares: databases in a fresh directory, and the views verbs run through.

File: test/conftest.py
Author: Ash Vardanian
Date: September 18, 2026
"""

import collections
import enum
import random
import sys
from collections.abc import Callable, Iterator
from pathlib import Path
from uuid import UUID

import pytest
from base import BACKEND_AXES, ENGINES, ISOLATIONS, MAX_CASES_PER_TEST, SETTINGS, SUPPORTS, open_engine, stream_key

import ustore


def pytest_configure(config: pytest.Config) -> None:
    """Registers the engine marks, which `--strict-markers` otherwise refuses."""
    config.addinivalue_line("markers", "needs(capability): skips on the engines that lack the capability")
    config.addinivalue_line("markers", "any_engine: runs on the first engine alone, as its engine is not under test")


def pytest_report_header() -> list[str]:
    """The build this run exercises, printed where pytest prints its own header."""
    header = [f"UStore {ustore.__version__} from {ustore.lib.__file__}"]
    for engine in ENGINES:
        supported = ", ".join(name for name, present in SUPPORTS[engine].items() if present)
        header.append(f"- Engine {engine}: {supported or 'none of the capabilities probed'}")
    header.append(f"- Seed: {SETTINGS.seed}, scale: {SETTINGS.scale:g}")
    return header


# region Shared Pytest
def pytest_collection_modifyitems(config: pytest.Config, items: list[pytest.Item]) -> None:
    """Refuses a test whose cases outgrow MAX_CASES_PER_TEST."""
    variants: dict[tuple[str, str], set[str]] = collections.defaultdict(set)
    for item in items:
        callspec = getattr(item, "callspec", None)
        parameters = dict(callspec.params) if callspec is not None else {}
        parameters.pop("__pytest_repeat_step_number", None)
        kept = {key: value for key, value in parameters.items() if key not in BACKEND_AXES}
        variants[(item.module.__name__, getattr(item, "originalname", item.name))].add(repr(kept))
    oversized = [
        f"{module}::{name} has {len(cases)}"
        for (module, name), cases in variants.items()
        if len(cases) > MAX_CASES_PER_TEST
    ]
    if oversized:
        raise pytest.UsageError(f"over MAX_CASES_PER_TEST={MAX_CASES_PER_TEST}: " + "; ".join(oversized))


def pytest_make_parametrize_id(config: pytest.Config, val: object, argname: str) -> str | None:
    """Names enum parameters as the code spells them, `Backend.LINUX_URING618`, so an id greps to its source."""
    return f"{type(val).__name__}.{val.name}" if isinstance(val, enum.Enum) else None


@pytest.fixture
def seed(request: pytest.FixtureRequest) -> int:
    """Share inputs across backends while keeping other parameters and repeat steps distinct."""
    callspec = getattr(request.node, "callspec", None)
    parameters = dict(callspec.params) if callspec is not None else {}
    repeat_step = parameters.pop("__pytest_repeat_step_number", 0)
    name = request.node.originalname or request.node.name
    name += repr({key: value for key, value in parameters.items() if key not in BACKEND_AXES})
    return stream_key(SETTINGS.seed, name, repeat_step)


@pytest.fixture
def rng(seed: int) -> random.Random:
    """A generator private to this test, so a neighbour's draws cannot shift this one's."""
    return random.Random(seed)


@pytest.fixture
def unraisable(monkeypatch: pytest.MonkeyPatch) -> list[object]:
    """Collects what the interpreter reports as unraisable, like a finalizer's exception, for the test to check."""
    seen: list[object] = []
    monkeypatch.setattr(sys, "unraisablehook", seen.append)
    return seen


# endregion Shared Pytest


def pytest_generate_tests(metafunc: pytest.Metafunc) -> None:
    """Runs every case that opens a database once per engine, and at each isolation of an engine that takes one,
    or on the first engine alone for a case marked `any_engine`."""
    if "engine" not in metafunc.fixturenames:
        return
    needs = [mark.args[0] for mark in metafunc.definition.iter_markers("needs")]

    def configuration(engine: str, isolation: ustore.Isolation | None = None):
        missing = [need for need in needs if not SUPPORTS[engine][need]]
        skipping = [pytest.mark.skip(reason=f"needs {' and '.join(missing)}")] if missing else []
        if "isolation" not in metafunc.fixturenames:
            return pytest.param(engine, marks=skipping, id=engine)
        # The first member is valued zero, so only None stands for no isolation
        named = engine if isolation is None else f"{engine}-{isolation.name.lower()}"
        return pytest.param(engine, isolation, marks=skipping, id=named)

    if "isolation" in metafunc.fixturenames:
        argnames = ("engine", "isolation")
        configurations = [
            configuration(engine, isolation) for engine in ENGINES for isolation in ISOLATIONS.get(engine, (None,))
        ]
    else:
        argnames = "engine"
        configurations = [configuration(engine) for engine in ENGINES]
    if metafunc.definition.get_closest_marker("any_engine"):
        configurations = configurations[:1]
    metafunc.parametrize(argnames, configurations)


@pytest.fixture
def open_database(tmp_path: Path, engine: str) -> Iterator[Callable[..., ustore.Database]]:
    """Opens databases of the engine in directories of their own, closing whichever the case left open."""
    opened: list[ustore.Database] = []

    def open_one(isolation: ustore.Isolation | None = None) -> ustore.Database:
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


@pytest.fixture
def blobs(database: ustore.Database) -> UUID:
    """A collection of binary values, since no collection exists until created."""
    return database.collection_create("blobs", modality=ustore.Modality.BLOBS)


@pytest.fixture
def docs(database: ustore.Database) -> UUID:
    """A collection of JSON documents."""
    return database.collection_create("docs", modality=ustore.Modality.DOCS)


@pytest.fixture
def graph(database: ustore.Database) -> UUID:
    """A graph collection."""
    return database.collection_create("graph", modality=ustore.Modality.GRAPH)


@pytest.fixture(params=["database", "transaction"])
def view(
    request: pytest.FixtureRequest, engine: str, database: ustore.Database
) -> Iterator[ustore.Database | ustore.Transaction]:
    """The database itself, or a transaction over it that commits once the case is done."""
    if request.param == "database":
        yield database
        return
    if not SUPPORTS[engine]["transactions"]:
        pytest.skip("needs transactions")
    with database.transaction() as staged:
        yield staged


@pytest.fixture
def arena() -> ustore.Arena:
    """Host memory for the outputs of the verbs a case calls, which every call rewinds."""
    return ustore.Arena()


@pytest.fixture
def later(view: ustore.Database | ustore.Transaction) -> Iterator[ustore.DeferredOn]:
    """A deferred view of `view`, whose executor waits for every call it ran once the case is done."""
    with ustore.Executor.for_ram(2) as executor:
        yield view.deferred(executor)
