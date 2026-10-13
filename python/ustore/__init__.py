"""GPU-Accelerated Embedded Transactional Database for High-Throughput NVMe Devices."""

from enum import IntEnum

from ustore import lib as _extension

# Every C enum a caller meets becomes an `IntEnum` valued as its enumerators, kept on the extension that answers them.
for _name, _members in _extension._enumerators.items():
    setattr(_extension, _name, IntEnum(_name, _members, module=__name__))

from ustore.lib import *

__version__ = _extension.__version__

del IntEnum, _extension, _name, _members
