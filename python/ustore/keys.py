"""Walks every key of a collection page by page, which the NetworkX and Pandas views both build on."""

from __future__ import annotations

from collections.abc import Iterator

from ustore.lib import Database, Transaction

KEY_MIN = -(2**63)
KEY_MAX = 2**63 - 1
PAGE_KEYS = 1 << 16


def scan_keys(
    view: Database | Transaction, collection: int | str | None = None, page: int = PAGE_KEYS
) -> Iterator[int]:
    """Yields every key of `collection` in ascending order, holding one page of them at a time."""
    start = KEY_MIN
    while True:
        found = view.scan(start, page, collection=collection)[0]
        yield from found
        if len(found) < page or found[-1] == KEY_MAX:
            return
        start = found[-1] + 1
