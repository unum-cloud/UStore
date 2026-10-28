"""Walks every key of a collection page by page, which the NetworkX and Pandas views both build on.

File: python/ustore/keys.py
Author: Ash Vardanian
Date: September 18, 2026
"""

from collections.abc import Iterable, Iterator
from operator import index
from uuid import UUID

from ustore.lib import Arena, Database, Modality, Snapshot, Transaction, View

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
    view: Database | Transaction | Snapshot | View,
    collection: UUID | str,
    page: int = PAGE_KEYS,
    modality: Modality = Modality.BLOBS,
) -> Iterator[UUID]:
    """Yields every key of `collection`, which holds `modality`, in ascending order through the scans of that modality, each page resuming past the last key of the one before it, holding one page at a time in one arena, made where the view's calls run, for the walk's whole life."""
    arena = Arena(view.executor if isinstance(view, View) else None)
    scan = {Modality.DOCS: view.docs_scan, Modality.GRAPH: view.graph_scan_vertices}.get(modality, view.scan)
    start = KEY_MIN
    while True:
        found = scan(start, page, arena, collection=collection)[0]
        yield from found
        if len(found) < page or found[-1] == KEY_MAX:
            return
        start = UUID(int=found[-1].int + 1)
