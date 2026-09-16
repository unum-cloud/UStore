"""GPU-Accelerated Embedded Transactional Database for High-Throughput NVMe Devices."""

from ustore import lib as _extension
from ustore.lib import *

__version__ = _extension.__version__

del _extension
