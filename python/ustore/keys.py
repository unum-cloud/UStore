"""Walks every key of a collection page by page, which the NetworkX and Pandas views both build on."""

from __future__ import annotations

from collections.abc import Iterable, Iterator
from operator import index
from uuid import UUID

from ustore.lib import Arena, Database, Snapshot, Transaction, View

type KeyLike = UUID | int | str | bytes

KEY_MIN = UUID(int=0)
KEY_MAX = UUID(int=2**128 - 1)
PAGE_KEYS = 1 << 16


def as_key(key: KeyLike) -> UUID:
    """The `uuid.UUID` any spelling of a key names, reading an integer as `uuid.UUID(int=n)` does."""
    if isinstance(key, UUID):
        return key
    if isinstance(key, str):
        return UUID(key)
    if isinstance(key, bytes | bytearray | memoryview):
        return UUID(bytes=bytes(key))
    return UUID(int=index(key))


def is_one_key(key: object) -> bool:
    """Whether `key` is one key rather than several, since text and bytes are iterable too."""
    return isinstance(key, str | bytes) or not isinstance(key, Iterable)


def scan_keys(
    view: Database | Transaction | Snapshot | View, collection: UUID | str | None = None, page: int = PAGE_KEYS
) -> Iterator[UUID]:
    """Yields every key of `collection` in ascending order through one cursor, holding one page of them at a time in
    one arena, made where the view's calls run, for the walk's whole life."""
    arena = Arena(view.executor if isinstance(view, View) else None)
    with view.cursor(collection=collection) as cursor:
        while found := cursor.next(page, arena):
            yield from found
