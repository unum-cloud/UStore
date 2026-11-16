"""GPU-Accelerated Multi-Modal Transactional Database Engine Built around Kernel Bypass.

File: python/ustore/__init__.py
Author: Ash Vardanian
Date: July 25, 2023
"""

import asyncio
from enum import IntEnum

from ustore import lib as _extension

# Every C enum a caller meets becomes an `IntEnum` valued as its enumerators, kept on the extension that answers them.
for _name, _members in _extension._enumerators.items():
    setattr(_extension, _name, IntEnum(_name, _members, module=__name__))

from ustore.lib import *

__version__ = _extension.__version__


async def _settled(deferred: Deferred) -> object:
    """Answers `deferred` once final, parking no thread, as `Deferred.__await__` drives it.

    The status is polled between sleeps that grow from 1 µs to 1 ms.
    """
    delay = 1e-6
    while deferred.status == Status.DEFERRED:
        await asyncio.sleep(delay)
        delay = min(delay * 2, 1e-3)
    return deferred.wait()


del IntEnum, _extension, _name, _members
