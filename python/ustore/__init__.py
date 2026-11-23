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

    While the status is deferred, it is re-polled on the next turn of the running loop through a
    future `loop.call_soon` resolves.
    """
    loop = asyncio.get_running_loop()
    while deferred.status == Status.DEFERRED:
        turn = loop.create_future()
        loop.call_soon(turn.set_result, None)
        await turn
    return deferred.wait()


del IntEnum, _extension, _name, _members
