"""Settings, engine capabilities and key spellings every Python suite shares, read once at import.

The extension holds every engine its build enabled, so each case runs once per engine, and a case
marked as needing a capability skips on the engines that lack it. Suites write integer keys and
expect them back as the `uuid.UUID` every key is answered as, which `key` and `keys` spell.

File: test/base.py
Author: Ash Vardanian
Date: October 1, 2026
"""

import importlib.util
import os
import random
import tempfile
from collections.abc import Iterable
from dataclasses import dataclass
from typing import NoReturn
from uuid import UUID

import pytest

import ustore

# region Shared Settings
_UINT64_MASK = 2**64 - 1


def env_text(variable: str) -> str | None:
    """The stripped text of the environment `variable`, or None when unset or blank."""
    text = os.environ.get(variable, "").strip()
    return text or None


def exit_unparsed(variable: str, text: str, expected: str) -> NoReturn:
    """Stops the session on a setting that does not parse, naming it as the C and C++ suites do."""
    raise SystemExit(f'{variable}="{text}" does not parse, expected {expected}')


def env_seed(variable: str) -> int:
    """An unsigned 64-bit seed: 42 when unset, drawn afresh for `random`."""
    text = env_text(variable)
    if text is None:
        return 42
    if text == "random":
        return random.SystemRandom().getrandbits(64)
    if not text.isdigit() or int(text) > _UINT64_MASK:
        exit_unparsed(variable, text, "an unsigned integer or random")
    return int(text)


def env_real(variable: str, fallback: float) -> float:
    """A positive real, `fallback` when unset."""
    text = env_text(variable)
    if text is None:
        return fallback
    try:
        value = float(text)
    except ValueError:
        value = 0.0
    if not value > 0 or value == float("inf"):
        exit_unparsed(variable, text, "a positive number like 0.1 or 10")
    return value


def env_count(variable: str, fallback: int) -> int:
    """A whole number, `fallback` when unset."""
    text = env_text(variable)
    if text is None:
        return fallback
    if not text.isdigit():
        exit_unparsed(variable, text, "a whole number")
    return int(text)


def env_flag(variable: str, fallback: bool = False) -> bool:
    """A switch, `fallback` when unset: 0, false, off and no turn it off."""
    text = env_text(variable)
    if text is None:
        return fallback
    return text not in {"0", "false", "off", "no"}


def mix(value: int) -> int:
    """SplitMix64's finalizer, a bijection that spreads every input bit over all 64 output bits."""
    value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & _UINT64_MASK
    value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & _UINT64_MASK
    return value ^ (value >> 31)


def stream_key(seed: int, name: str, index: int = 0) -> int:
    """The `index`-th stream of the family `name` under `seed`, identical in C, C++, Rust and Mojo."""
    hashed = 0xCBF29CE484222325
    for byte in name.encode():
        hashed = ((hashed ^ byte) * 0x100000001B3) & _UINT64_MASK
    return mix((mix(seed ^ hashed) + index) & _UINT64_MASK)


# endregion Shared Settings


@dataclass(frozen=True)
class Settings:
    """Every `USTORE_*` variable the Python suites read, parsed once at import."""

    seed: int
    scale: float


SETTINGS = Settings(seed=env_seed("USTORE_SEED"), scale=env_real("USTORE_SCALE", 1.0))

BACKEND_AXES = {"engine", "isolation"}
"""The axes the seed and the case cap leave out, so every engine and isolation draws the same inputs."""

MAX_CASES_PER_TEST = 112
"""The most cases one test function may expand to beside its engines, today's largest, so a new axis cannot
multiply a matrix past it unnoticed.
"""

ENGINES = ustore.engines_compiled()

ISOLATIONS = {"ram": tuple(ustore.Isolation)}
"""The isolations each engine opens at: only the in-memory engine takes one, every other has the one level it has."""

BATCH_SIZES = [0, 1, 16, 17, 64, 65, 1000]
"""Batches below, at and past the 16 values and 64 keys a verb lays out on the stack."""

pyarrow_available = importlib.util.find_spec("pyarrow") is not None
"""Whether PyArrow is installed, which the test group leaves out on Windows on Arm."""

pandas_available = importlib.util.find_spec("pandas") is not None
"""Whether pandas is installed."""


def key(integer: int) -> UUID:
    """The `uuid.UUID` an integer key comes back as."""
    return UUID(int=integer)


def keys(integers: Iterable[int]) -> list[UUID]:
    """The `uuid.UUID` every integer key comes back as, in order."""
    return [UUID(int=integer) for integer in integers]


def location(engine: str, directory: str | os.PathLike[str]) -> str | os.PathLike[str]:
    """What `engine` opens or saves a database in `directory` by: the NVMe manifest there, or `directory` itself."""
    return os.path.join(directory, "ustore.ustore.json") if engine == "nvme" else directory


def open_engine(
    engine: str, directory: str | os.PathLike[str], isolation: ustore.Isolation | None = None
) -> ustore.Database:
    """Opens `engine` in `directory`, at `isolation` when the engine takes one."""
    if isolation is not None:
        return ustore.Database.open_ram(directory, isolation=isolation)
    return getattr(ustore.Database, f"open_{engine}")(location(engine, directory))


def executor_for(engine: str, threads: int, memory_domains: list[int] | None = None) -> ustore.Executor:
    """An executor of `threads` threads of the kind `engine` admits: NVMe queues on the NVMe engine, whose databases
    refuse a RAM executor, and host threads on every other."""
    if engine == "nvme":
        return ustore.Executor.for_nvme(threads, memory_domains=memory_domains)
    return ustore.Executor.for_ram(threads, memory_domains=memory_domains)


def serves_pages(database: ustore.Database) -> bool:
    """Whether the engine of `database` serves pages, which an engine may refuse as unsupported."""
    try:
        collection = database.collection_create("pages", modality=ustore.Modality.PAGES)
        database.pages_import(key(1), b"<probe/>", collection=collection)
    except ustore.UnsupportedError:
        return False
    return True


def probe(engine: str) -> dict[str, bool]:
    """What `engine` supports, read from a database opened in a scratch directory."""
    with tempfile.TemporaryDirectory() as directory, open_engine(engine, directory) as database:
        capabilities = database.capabilities
        return {
            "transactions": capabilities.transactions,
            "snapshots": capabilities.snapshots,
            "saved_snapshots": capabilities.saves_snapshots,
            "pages": serves_pages(database),
        }


SUPPORTS = {engine: probe(engine) for engine in ENGINES}

NEEDS_TRANSACTIONS = pytest.mark.needs("transactions")
NEEDS_SNAPSHOTS = pytest.mark.needs("snapshots")
NEEDS_SAVED_SNAPSHOTS = pytest.mark.needs("saved_snapshots")
NEEDS_PAGES = pytest.mark.needs("pages")
