"""Type stubs for the `ustore.lib` extension.

Stub files are never executed, so PEP 688's `Buffer` is read by any checker regardless of the
interpreter running the tests. The package floor is 3.12.

File: python/ustore/lib.pyi
Author: Ash Vardanian
Date: September 16, 2026
"""

from collections.abc import Callable, Generator, Iterator, Sequence
from enum import IntEnum
from os import PathLike
from types import GenericAlias, TracebackType
from typing import Any, Literal, Protocol, Self, final, overload, type_check_only
from uuid import UUID

from _typeshed import structseq

from ustore.graph_view import DiGraph
from ustore.table_view import Table

@type_check_only
class _ArrowArray(Protocol):
    """Anything exporting an Arrow array through the C data interface, such as a `pyarrow.Array` or `Values`."""

    def __arrow_c_array__(self, requested_schema: object | None = None) -> tuple[object, object]: ...

@type_check_only
class _ArrowStream(Protocol):
    """Anything exporting an Arrow stream through the C stream interface, such as a `pyarrow.Table`."""

    def __arrow_c_stream__(self, requested_schema: object | None = None) -> object: ...

type ArrowSource = _ArrowStream | _ArrowArray
"""An Arrow stream whose batches a call takes over, or one array, read as a stream of one batch."""
type IngestOrder = Literal["sorted", "unsorted"]
"""Whether an ingest's keys strictly ascend across its stream, or arrive in any order with the last repeat winning."""
type Buffer = memoryview | bytes | bytearray
type CollectionLike = UUID | str
type KeyLike = UUID | int | str | bytes
type KeysLike = Sequence[KeyLike] | Buffer
type ValueLike = Buffer
type TapeLike = _ArrowArray | tuple[Buffer, Buffer]
"""An Arrow `binary`, `large_binary`, `string` or `large_string` array, or a `(data, offsets)` pair of buffers whose
offsets are unsigned 32-bit or 64-bit integers, one more than the values, which a write passes without a copy."""
type JSONValue = dict[str, JSONValue] | list[JSONValue] | tuple[JSONValue, ...] | str | int | float | bool | None
type DocumentLike = str | Buffer | dict[str, JSONValue] | list[JSONValue] | int | float | bool
type TextsLike = Sequence[str | Buffer] | str | Buffer | TapeLike
"""One `str` or bytes-like text per task, a tape of them, or one for every task, as markup and names reach pages."""
type LengthsLike = Sequence[int] | int
type PathLikeOrStr = str | bytes | PathLike[str]
type DataDirectory = PathLikeOrStr | tuple[PathLikeOrStr, int]
type Progress = Callable[[int, int], bool | None]
"""Called with the units a call finished and the units it finishes in all, from whichever thread runs it but never two
at once; None or a truthy answer carries on, any other falsy one cancels, and a raise cancels and re-raises."""
type Predicate = Callable[[tuple[UUID, ...]], Sequence[object]]
"""Called with the keys one page of a scan or a pull listed, from whichever thread runs it, answering one truthy item
per key kept; a raise rejects the rest and re-raises once the call is final."""

__version__: str
edge_id_default: UUID

def version() -> str:
    """The version of the library this module runs against, like "1.0.0". Calls `ustore_version_name`."""

def engines_compiled() -> tuple[str, ...]:
    """The engines this build carries: some of "ram", "rocksdb", "leveldb" and "nvme", in that order."""

def gpu_devices() -> tuple[GPUDevice, ...]:
    """Every GPU this process can drive, NVIDIA devices first, then AMD, each at the ordinal `Executor.for_vram` names
    it by; empty on a machine without GPUs or their runtimes. Calls `ustore_gpu_devices`."""

# region: Statuses and Modes

class Status(IntEnum):
    """What went wrong, valued as `ustore_status_t`: `Error.status` and `Deferred.status`."""

    SUCCESS = 0
    DEFERRED = 1
    OUT_OF_MEMORY = 2
    READ_ONLY = 6
    IO = 7
    DEVICE = 8
    MALFORMED = 9
    INTERNAL = 10
    UNINITIALIZED = 12
    CONTENDED = 14
    CANCELLED = 15
    UNSUPPORTED = 16
    INVALID_ARGUMENT = 17
    NOT_FOUND = 18
    ALREADY_EXISTS = 19
    WRITE_CONFLICT = 20
    READ_CONFLICT = 21
    PHANTOM_CONFLICT = 22
    CAPACITY_EXHAUSTED = 23

class Mood(IntEnum):
    """What an executor's threads do once it next goes idle, valued as `ustore_mood_t`."""

    PARKED = 0
    SPINNING = 1

class Watch(IntEnum):
    """Whether a transaction records the keys a call touches, valued as `ustore_watch_t`."""

    KEYS = 0
    NONE = 1

class Durability(IntEnum):
    """How durable the writes a call publishes are once it finishes, valued as `ustore_durability_t`."""

    BUFFERED = 0
    FLUSHED = 1

class DropMode(IntEnum):
    """What dropping a collection removes, valued as `ustore_drop_mode_t`."""

    COLLECTION = 0
    PAIRS = 1
    VALUES = 2

class ScanMode(IntEnum):
    """In what order a scan hands its keys over, valued as `ustore_scan_mode_t`."""

    ORDERED = 0
    UNORDERED = 1

class ScanDirection(IntEnum):
    """Which way a scan, a cursor or a measurement walks its range from the start key."""

    ASCENDING = 0
    DESCENDING = 1

class Modality(IntEnum):
    """What a collection holds, valued as `ustore_modality_t`."""

    BLOBS = 1
    DOCS = 2
    GRAPH = 3
    STRINGS = 4
    PAGES = 5

class Isolation(IntEnum):
    """How the transactions of the in-memory engine see each other's writes, valued as `ustore_isolation_t`."""

    STRICT_SERIALIZABLE = 0
    SNAPSHOT = 1

class CPUClass(IntEnum):
    """The CPU generation a database confines its waits, atomics and string kernels to, valued as
    `ustore_cpu_class_t`."""

    AUTOMATIC = 0
    SERIAL = 1
    INTEL_HASWELL = 2
    INTEL_SIERRA = 3
    INTEL_SKYLAKE = 4
    INTEL_ICELAKE = 5
    AWS_G2 = 6
    APPLE_M2 = 7
    AWS_G5 = 8
    ALIBABA_C930 = 9
    MONOVERSIONED = 10

class Transport(IntEnum):
    """How the NVMe engine moves bytes between its files and memory, valued as `ustore_transport_t`."""

    AUTOMATIC = 0
    BLOCKING = 1
    LINUX_URING = 2
    LINUX_AIO = 3
    LINUX_SPDK = 4

class PatternKind(IntEnum):
    """What the patterns of a scan or a cursor are matched as, valued as `ustore_pattern_kind_t`."""

    REGEX = 0
    SUBSTRINGS = 1

class CaseSensitivity(IntEnum):
    """Whether patterns compare bytes as they are or after Unicode case folding, valued as
    `ustore_case_sensitivity_t`."""

    CASED = 0
    UNCASED = 1

class PredicateCost(IntEnum):
    """Where a scan asks its predicate: before patterns and filters read values, or last, valued as
    `ustore_predicate_cost_t`."""

    CHEAP = 0
    EXPENSIVE = 1

class Gds(IntEnum):
    """How an executor of `Executor.for_vram` moves bytes between the drives and its device, valued as
    `ustore_gds_t`."""

    AUTOMATIC = 0
    DIRECT = 1
    COMPATIBILITY = 2

class GPUVendor(IntEnum):
    """The vendor whose runtime drives a GPU, valued as `ustore_gpu_vendor_t`."""

    NVIDIA = 1
    AMD = 2

class GPUClass(IntEnum):
    """The generation of a GPU, valued as `ustore_gpu_class_t`."""

    NVIDIA_AMPERE = 1
    NVIDIA_ADA = 2
    NVIDIA_HOPPER = 3
    NVIDIA_BLACKWELL = 4
    NVIDIA_BLACKWELLRTX = 5
    AMD_CDNA3 = 6
    AMD_CDNA4 = 7
    AMD_CDNA5 = 8

class Engine(IntEnum):
    """The engine serving a database, as `describe()` reports it, valued as `ustore_engine_t`."""

    RAM = 0
    ROCKSDB = 1
    LEVELDB = 2
    NVME = 3

class Role(IntEnum):
    """The role a vertex plays in an edge, valued as `ustore_vertex_role_t`."""

    UNKNOWN = 0
    SOURCE = 1
    TARGET = 2
    ANY = 3

class FieldType(IntEnum):
    """How a document or a field is spelled, or what a gather converts it into, valued as `ustore_docs_field_type_t`."""

    JSON = 0
    NULL = 10
    BOOL = 11
    UUID = 12
    I8 = 20
    I16 = 21
    I32 = 22
    I64 = 23
    U8 = 30
    U16 = 31
    U32 = 32
    U64 = 33
    F16 = 40
    F32 = 41
    F64 = 42
    BINARY = 50
    STRING = 51

class Modification(IntEnum):
    """What a document write applies, valued as `ustore_docs_modification_t`."""

    UPSERT = 0
    UPDATE = 1
    INSERT = 2
    PATCH = 3
    MERGE = 4

class PageEncoding(IntEnum):
    """How a page or a fragment is spelled on import or rendered on read, valued as `ustore_page_encoding_t`."""

    HTML = 0
    XML = 1
    MARKDOWN = 2
    TEXT = 3
    JSON = 4

class ChangeKind(IntEnum):
    """What one committed change did to its key."""

    WRITE = 0
    REMOVAL = 1
    CHANGE = 2

# endregion: Statuses and Modes

# region: Failures

class Error(RuntimeError):
    """Every failure the engine reports, one subclass per status shared with the C ABI."""

    status: Status
    """The status, such as `Status.WRITE_CONFLICT` or `Status.OUT_OF_MEMORY`."""

class OutOfMemoryError(Error, MemoryError):
    """An allocation could not be satisfied."""

class InvalidArgumentError(Error, ValueError):
    """An argument is null, contradicts another one, or carries a choice the call does not accept, like a collection
    of another modality."""

class UninitializedError(Error):
    """The call needs a handle that was never opened or is closed, or reads a result whose arena a later call
    rewound."""

class UnsupportedError(Error, NotImplementedError):
    """The engine or this build does not implement what the call asks for."""

class InputOutputError(Error, OSError):
    """A file or directory could not be opened, read, written, or closed."""

class MalformedError(Error, ValueError):
    """Stored or supplied data could not be decoded, like a corrupted file or an invalid document."""

class NotFoundError(Error, KeyError):
    """A collection, a directory, or a document a modification needs does not exist."""

class AlreadyExistsError(Error, ValueError):
    """A collection name or a document the call creates is already taken, or a raw medium's first slot is not free."""

class ConflictError(Error):
    """A concurrent writer touched what this transaction wrote, read or scanned: the one base a retry loop catches."""

class WriteConflictError(ConflictError):
    """A concurrent writer changed a key this transaction wrote; retry the transaction."""

class ReadConflictError(ConflictError):
    """A concurrent writer changed a key this transaction read; retry the transaction."""

class PhantomConflictError(ConflictError):
    """A concurrent writer inserted into a range this transaction scanned; retry the transaction."""

class CapacityExhaustedError(Error, OverflowError):
    """A hard design ceiling was reached."""

class DeviceError(Error):
    """A device could not be selected or claimed, or the executor, database and arena of a call live apart."""

class InternalError(Error):
    """The engine failed in a way no other status names, which points at a defect."""

class ContendedError(Error):
    """The call was refused rather than waited for: a full deferred table, an arena serving another call or exporting
    a buffer, or a transaction serving another call; retry it once that one finishes or the buffer is released."""

class CancelledError(Error):
    """A progress callback stopped the call, which unwound and published nothing."""

class ReadOnlyError(Error):
    """The call would change a database opened read-only, as `Database.view_rocksdb` opens one."""

# endregion: Failures

# region: Arenas and Result Tapes

class Arena:
    """The memory every output whose length scales with a call lands in, rewound by the next call handed it.

    Every verb whose outputs scale with the call takes one as its last positional argument and answers results
    viewing it. A buffer exported over its outputs pins it, so the next call naming it raises `ContendedError` until
    the buffer is released, and a result read after a later call rewound it raises `UninitializedError`. A deferred
    call holds it until it is waited on or dropped.
    """

    def __init__(self, executor: Executor | None = None, /) -> None:
        """Makes an arena where `executor` computes, or host pages for calls on the calling thread without one."""

class Values:
    """The values one batch read or cursor pull wrote into an `Arena`, viewing it until it rewinds, and a tape a write
    takes back."""

    lengths: tuple[int | None, ...]
    """The length of every value, derived from the offsets, with None where the key was absent."""

    def __len__(self) -> int: ...
    def __getitem__(self, task: int) -> memoryview | None: ...
    def __buffer__(self, flags: int) -> memoryview: ...
    def __arrow_c_array__(self, requested_schema: object | None = None) -> tuple[object, object]:
        """The schema and array PyCapsules of the batch as Arrow `binary`, or `large_binary` once its bytes outgrow
        32-bit offsets: the presence bits, offsets and bytes the read wrote, exported in place."""

class Keys:
    """The keys one batch scan or sample wrote into an `Arena`, viewing it until it rewinds."""

    counts: tuple[int, ...]
    scores: tuple[float, ...] | None

    def __len__(self) -> int: ...
    def __getitem__(self, task: int) -> tuple[UUID, ...]: ...
    def __buffer__(self, flags: int) -> memoryview: ...

@final
class Page:
    """The keys one pull of a `Cursor` wrote into an `Arena`, viewing it until it rewinds; empty, so falsy, once the
    walk ends."""

    values: Values | None
    """The values of the keys, every one present, when the pull asked for them, and None otherwise."""

    def __len__(self) -> int: ...
    def __getitem__(self, key: int) -> UUID: ...
    def __iter__(self) -> Iterator[UUID]: ...
    def __buffer__(self, flags: int) -> memoryview:
        """The keys as an array of shape (keys, 16) bytes, pinning the arena until released."""

@final
class Cursor:
    """A walk over the keys of one collection, which every pull resumes past the last key the one before it handed
    out, so a key written behind the walk never comes and one written ahead of it does. A page shorter than `wanted`
    exhausts the walk for good, whatever is written since."""

    def next(self, wanted: int, arena: Arena, /, *, values: bool = False, progress: Progress | None = None) -> Page:
        """The next page of up to `wanted` keys in `arena`, with their values when asked, blocking on the executor the
        cursor was opened through. Calls `ustore_blobs_cursor_next`."""

    def close(self) -> None:
        """Frees the walk with the holds it kept on its database and the transaction or snapshot it reads through."""

    def __enter__(self) -> Self: ...
    def __exit__(
        self,
        kind: type[BaseException] | None = None,
        value: BaseException | None = None,
        traceback: TracebackType | None = None,
    ) -> None: ...

@final
class ChangesCursor:
    """A feed of the changes committed to one collection after a stamp, oldest first, which pins that stamp and
    resumes every pull past the last change the pull before it handed out."""

    def next(self, wanted: int, arena: Arena, /, *, values: bool = False) -> Changes:
        """The next up to `wanted` changes in `arena`, with the bytes every one carries when asked; an empty pull
        reached the newest commit."""

    def close(self) -> None:
        """Frees the feed, releasing the stamp it pinned and the hold it kept on its database."""

    def __enter__(self) -> Self: ...
    def __exit__(
        self,
        kind: type[BaseException] | None = None,
        value: BaseException | None = None,
        traceback: TracebackType | None = None,
    ) -> None: ...

class Columns:
    """The typed columns one gather wrote into an `Arena`, viewing it until it rewinds."""

    types: tuple[FieldType, ...]
    rows: int

    def __len__(self) -> int: ...
    def __getitem__(self, field: int) -> memoryview | tuple[str | bytes | None, ...]: ...
    def validities(self, field: int) -> memoryview:
        """The bitmap of documents whose value was found and converted."""

    def conversions(self, field: int) -> memoryview:
        """The bitmap of values whose conversion changed their meaning."""

    def collisions(self, field: int) -> memoryview:
        """The bitmap of values of a type no conversion reaches."""

class Neighborhoods:
    """The edges one batch read found around its vertices, viewing its `Arena` until it rewinds."""

    degrees: tuple[int | None, ...]

    def __len__(self) -> int: ...
    def __getitem__(self, vertex: int) -> tuple[tuple[UUID, UUID, UUID], ...] | None: ...
    def __buffer__(self, flags: int) -> memoryview: ...

# endregion: Arenas and Result Tapes

# region: Collections

class Collection:
    """A collection a receiver addresses by name, resolved on first use and created by its first write.

    The mapping views and `sample` copy what they read out of a one-off arena each. Through a deferred view,
    `blobs[key]`, `docs[key]`, `docs_json[key]`, `scan`, `sample_keys` and the reads of `graph` that take one call
    answer a `Deferred` of what they answer here.
    """

    name: str
    id: UUID
    modality: Modality
    blobs: BlobsView
    docs: DocsView
    docs_json: DocsJSONView
    graph: DiGraph
    table: Table

    def sample(self, wanted: int, /) -> dict[UUID, bytes | None]:
        """Up to `wanted` keys drawn at random onto copies of their values, through one-off arenas."""

    def sample_keys(self, wanted: int, arena: Arena, /) -> Keys:
        """Up to `wanted` keys drawn at random."""

    def scan(self, start: KeyLike, wanted: int, arena: Arena, /) -> Keys:
        """Up to `wanted` keys from `start` upwards."""

    def __arrow_c_stream__(self, requested_schema: object | None = None) -> object:
        """The whole collection as one Arrow stream through the receiver's view, keys as `arrow.uuid` and values as
        binary, so `pyarrow.table(db['posters'])` reads it in place."""

class BlobsView:
    """The binary values of a collection: one key reads a copy as bytes, a batch a list of them, through one-off
    arenas."""

    @overload
    def __getitem__(self, key: KeyLike) -> bytes: ...
    @overload
    def __getitem__(self, key: KeysLike) -> list[bytes | None]: ...
    def __setitem__(self, key: KeyLike | KeysLike, value: Sequence[ValueLike] | ValueLike | TapeLike) -> None: ...
    def __delitem__(self, key: KeyLike | KeysLike) -> None: ...
    def __contains__(self, key: KeyLike) -> bool: ...

class DocsView:
    """The JSON documents of a collection as Python objects, serialized and parsed in C."""

    @overload
    def __getitem__(self, key: KeyLike) -> JSONValue: ...
    @overload
    def __getitem__(self, key: KeysLike) -> list[JSONValue]: ...
    def __setitem__(self, key: KeyLike | KeysLike, document: JSONValue | Sequence[JSONValue]) -> None: ...
    def __delitem__(self, key: KeyLike | KeysLike) -> None: ...
    def __contains__(self, key: KeyLike) -> bool: ...

class DocsJSONView:
    """The JSON documents of a collection as their text: one key reads a copy as bytes, a batch a list of them."""

    @overload
    def __getitem__(self, key: KeyLike) -> bytes: ...
    @overload
    def __getitem__(self, key: KeysLike) -> list[bytes | None]: ...
    def __setitem__(self, key: KeyLike | KeysLike, text: Sequence[DocumentLike] | DocumentLike) -> None: ...
    def __delitem__(self, key: KeyLike | KeysLike) -> None: ...
    def __contains__(self, key: KeyLike) -> bool: ...

# endregion: Collections

# region: Executors and Deferred Calls

@final
class Executor:
    """The threads that run calls, shared by any number of databases."""

    @classmethod
    def for_ram(cls, threads: int = 0, *, memory_domains: Sequence[int] | None = None, deferred_limit: int = 0) -> Self:
        """Host threads for the RAM, RocksDB and LevelDB engines near `memory_domains`, every NUMA node for None, where
        zero threads take 7/8 of the cores those nodes carry. Calls `ustore_executor_for_ram`."""

    @classmethod
    def on_pool(cls, pool: int, /, *, memory_domains: Sequence[int] | None = None, deferred_limit: int = 0) -> Self:
        """The threads of a ForkUnion `fu_pool_t` at address `pool`, which another library runs and which must outlive
        the executor. Calls `ustore_executor_on_pool`."""

    @classmethod
    def for_nvme(
        cls,
        threads: int = 0,
        *,
        transport: Transport = Transport.AUTOMATIC,
        deferred_limit: int = 0,
        shared_io_channels: int = 0,
        queue_depth: int = 0,
        request_buffer_bytes: int = 0,
        request_buffers_count: int = 0,
        stream_buffer_bytes: int = 0,
        stream_buffers_count: int = 0,
    ) -> Self:
        """Threads each driving an I/O ring for the NVMe engine, telling every database `Database.open_nvme` opens on
        it how to move bytes, where every zero size keeps the engine's default. Calls `ustore_executor_for_nvme`."""

    @classmethod
    def for_vram(
        cls,
        device: int,
        *,
        scratch_limit: int = 0,
        gds: Gds = Gds.AUTOMATIC,
        deferred_limit: int = 0,
    ) -> Self:
        """One stream on `device`, a `GPUDevice.ordinal` of either vendor, every call queues on, fed by one host thread,
        which `Database.open_nvme` takes as its `compactions` on an NVIDIA device, refused with `DeviceError` for an
        ordinal no device carries; zero `scratch_limit` lets scratch grow as calls ask. Calls
        `ustore_executor_for_vram`."""

    def drain(self, mood: Mood = Mood.PARKED) -> None:
        """Waits for every call submitted before the drain began, setting what the threads do once idle."""

    def close(self) -> None:
        """Waits for every submitted call, then frees the threads."""
    def __enter__(self) -> Self: ...
    def __exit__(
        self,
        kind: type[BaseException] | None = None,
        value: BaseException | None = None,
        traceback: TracebackType | None = None,
    ) -> None: ...

@final
class Deferred[T]:
    """A call a deferred view submitted, which answers once it finishes, holding its arena until then."""

    def __class_getitem__(cls, item: Any, /) -> GenericAlias:
        """Names what `wait()` answers, as `Deferred[Values]`."""

    @property
    def status(self) -> Status:
        """The status of the call, `Status.DEFERRED` until it finishes, polled through `ustore_outcome_status`."""

    def wait(self) -> T:
        """Blocks the thread until the call finishes, answering or raising what the blocking verb would."""

    def __await__(self) -> Generator[Any, None, T]:
        """Runs `wait()` on the running event loop's default executor, suspending the task meanwhile."""

# endregion: Executors and Deferred Calls

# region: Handles

@final
class Capabilities(structseq[bool], tuple[bool, bool]):
    """What an engine supports, as `Database.capabilities` reports it."""

    @property
    def transactions(self) -> bool:
        """Whether the engine groups operations into transactions."""

    @property
    def snapshots(self) -> bool:
        """Whether the engine pins point-in-time states."""

@final
class Changes(
    structseq[Any], tuple[Page, Values | None, tuple[int, ...], tuple[ChangeKind, ...]]
):
    """The changes one pull of a ChangesCursor answers, oldest first."""

    @property
    def keys(self) -> Page:
        """The Page of the key of every change, oldest change first."""

    @property
    def values(self) -> Values | None:
        """The Values of the bytes every change carries when the pull asked for them, or None."""

    @property
    def stamps(self) -> tuple[int, ...]:
        """The stamp every change was committed at."""

    @property
    def kinds(self) -> tuple[ChangeKind, ...]:
        """What every change did."""

@final
class Measurement(structseq[int], tuple[int, int, int, int, int, int]):
    """The bounds on what a key range holds, as `measure()` estimates them without walking it."""

    @property
    def min_cardinality(self) -> int:
        """The fewest entries the range may hold."""

    @property
    def max_cardinality(self) -> int:
        """The most entries the range may hold."""

    @property
    def min_value_bytes(self) -> int:
        """The fewest bytes the values in the range may take."""

    @property
    def max_value_bytes(self) -> int:
        """The most bytes the values in the range may take."""

    @property
    def min_space_usage(self) -> int:
        """The fewest bytes the range may take on disk."""

    @property
    def max_space_usage(self) -> int:
        """The most bytes the range may take on disk."""

@final
class GPUDevice(structseq[Any], tuple[int, GPUVendor, int, GPUClass, int, int, str]):
    """One GPU this process can drive, as `gpu_devices()` lists it."""

    @property
    def ordinal(self) -> int:
        """The ordinal `Executor.for_vram` names the device by, its position in `gpu_devices()`."""

    @property
    def vendor(self) -> GPUVendor:
        """The vendor whose runtime drives the device."""

    @property
    def vendor_ordinal(self) -> int:
        """The device's ordinal in its vendor's runtime, as `nvidia-smi` or `rocm-smi` list it."""

    @property
    def gpu_class(self) -> GPUClass:
        """The class of the device."""

    @property
    def compute_units(self) -> int:
        """Streaming multiprocessors or compute units."""

    @property
    def memory_bytes(self) -> int:
        """Device memory in bytes."""

    @property
    def name(self) -> str:
        """The vendor's own name for the device."""

@final
class Description(structseq[Any], tuple[Capabilities, Engine, Isolation, int, int, int]):
    """What an open database reports about itself, as `describe()` answers it, every size the engine's estimate."""

    @property
    def capabilities(self) -> Capabilities:
        """What the engine supports."""

    @property
    def engine(self) -> Engine:
        """The engine serving the database."""

    @property
    def isolation(self) -> Isolation:
        """How transactions see each other: what the RAM engine was opened with, and `SNAPSHOT` elsewhere."""

    @property
    def keys(self) -> int:
        """Keys across every collection."""

    @property
    def memory_bytes(self) -> int:
        """Bytes of memory the engine holds for the data, or zero where it keeps no estimate."""

    @property
    def disk_bytes(self) -> int:
        """Bytes the files of the database take on disk."""

@type_check_only
class _Reads:
    """The read verbs every blocking receiver answers, and the views it derives."""

    def __getitem__(self, name: str) -> Collection:
        """The collection `name` addresses."""

    def read(
        self,
        keys: KeysLike,
        arena: Arena,
        /,
        *,
        collection: CollectionLike,
        offset: int = 0,
        limit: int | None = None,
    ) -> Values: ...
    def get(self, key: KeyLike, *, collection: CollectionLike) -> bytes | None:
        """Copies one value out of a one-off arena, which `read` with an arena of yours avoids."""

    def contains(self, key: KeyLike, *, collection: CollectionLike) -> bool:
        """Reads one presence into a one-off arena."""

    def cursor(
        self,
        start: KeyLike | None = None,
        /,
        *,
        collection: CollectionLike,
        end: KeyLike | None = None,
        direction: ScanDirection = ScanDirection.ASCENDING,
        patterns: Sequence[str | bytes] | None = None,
        pattern_kind: PatternKind = PatternKind.REGEX,
        sensitivity: CaseSensitivity = CaseSensitivity.CASED,
        predicate: Predicate | None = None,
        predicate_cost: PredicateCost = PredicateCost.CHEAP,
    ) -> Cursor:
        """A walk over the keys of one collection from `start`, reading nothing until its first pull, whose every pull
        blocks as this receiver's calls do. Calls `ustore_blobs_cursor_init`."""

    def docs_cursor(
        self,
        start: KeyLike | None = None,
        /,
        *,
        collection: CollectionLike,
        end: KeyLike | None = None,
        direction: ScanDirection = ScanDirection.ASCENDING,
        filter: str | bytes | None = None,
        predicate: Predicate | None = None,
        predicate_cost: PredicateCost = PredicateCost.CHEAP,
    ) -> Cursor:
        """A walk over documents, keeping those `filter` matches. Calls `ustore_docs_cursor_init`."""

    def pages_cursor(
        self,
        start: KeyLike | None = None,
        /,
        *,
        collection: CollectionLike,
        end: KeyLike | None = None,
        direction: ScanDirection = ScanDirection.ASCENDING,
        filter: str | bytes | None = None,
        predicate: Predicate | None = None,
        predicate_cost: PredicateCost = PredicateCost.CHEAP,
        outputs_encoding: PageEncoding = PageEncoding.XML,
    ) -> Cursor:
        """A walk over pages, keeping those holding a node of the XPath `filter`, whose pulls render whole pages into
        `outputs_encoding` when asked. Calls `ustore_pages_cursor_init`."""

    def graph_vertices_cursor(
        self,
        start: KeyLike | None = None,
        /,
        *,
        collection: CollectionLike,
        end: KeyLike | None = None,
        direction: ScanDirection = ScanDirection.ASCENDING,
        predicate: Predicate | None = None,
        predicate_cost: PredicateCost = PredicateCost.CHEAP,
    ) -> Cursor:
        """A walk over the vertices of one graph collection. Calls `ustore_graph_vertices_cursor_init`."""

    def scan(
        self,
        start: KeyLike | None,
        wanted: int,
        arena: Arena,
        /,
        *,
        collection: CollectionLike,
        end: KeyLike | None = None,
        direction: ScanDirection = ScanDirection.ASCENDING,
        mode: ScanMode = ScanMode.ORDERED,
        progress: Progress | None = None,
        patterns: Sequence[str | bytes] | None = None,
        pattern_kind: PatternKind = PatternKind.REGEX,
        sensitivity: CaseSensitivity = CaseSensitivity.CASED,
        weights: Sequence[float] | None = None,
        predicate: Predicate | None = None,
        predicate_cost: PredicateCost = PredicateCost.CHEAP,
    ) -> Keys: ...
    def scan_batch(
        self,
        starts: KeysLike,
        wanted: int,
        arena: Arena,
        /,
        *,
        collection: CollectionLike,
        ends: KeysLike | None = None,
        direction: ScanDirection = ScanDirection.ASCENDING,
        mode: ScanMode = ScanMode.ORDERED,
        progress: Progress | None = None,
        patterns: Sequence[str | bytes] | None = None,
        pattern_kind: PatternKind = PatternKind.REGEX,
        sensitivity: CaseSensitivity = CaseSensitivity.CASED,
        weights: Sequence[float] | None = None,
        predicate: Predicate | None = None,
        predicate_cost: PredicateCost = PredicateCost.CHEAP,
    ) -> Keys:
        """One scan per start, reporting the scans finished to `progress`."""
    def sample(self, wanted: int, arena: Arena, /, *, collection: CollectionLike) -> Keys: ...
    def sample_batch(self, count: int, wanted: int, arena: Arena, /, *, collection: CollectionLike) -> Keys:
        """`count` samples of up to `wanted` keys each, all drawn from the one collection named."""
    def measure(
        self,
        start: KeyLike | None,
        end: KeyLike | None,
        arena: Arena,
        /,
        *,
        collection: CollectionLike,
        direction: ScanDirection = ScanDirection.ASCENDING,
    ) -> Measurement: ...
    def docs_read(
        self,
        keys: KeysLike,
        arena: Arena,
        /,
        *,
        collection: CollectionLike,
        field: str | None = None,
        outputs_type: FieldType = FieldType.JSON,
    ) -> Values: ...
    def docs_gist(self, keys: KeysLike, arena: Arena, /, *, collection: CollectionLike) -> tuple[str, ...]: ...
    def docs_gather(
        self,
        keys: KeysLike,
        fields: Sequence[str],
        outputs_types: FieldType | Sequence[FieldType],
        arena: Arena,
        /,
        *,
        collection: CollectionLike,
    ) -> Columns: ...
    def docs_scan(
        self,
        start: KeyLike | None,
        wanted: int,
        arena: Arena,
        /,
        *,
        collection: CollectionLike,
        end: KeyLike | None = None,
        direction: ScanDirection = ScanDirection.ASCENDING,
        mode: ScanMode = ScanMode.ORDERED,
        progress: Progress | None = None,
        filter: str | bytes | None = None,
        predicate: Predicate | None = None,
        predicate_cost: PredicateCost = PredicateCost.CHEAP,
    ) -> Keys: ...
    def docs_sample(self, wanted: int, arena: Arena, /, *, collection: CollectionLike) -> Keys:
        """Up to `wanted` document keys drawn at random, which a `docs_read` or a `docs_gather` follows."""
    def docs_measure(
        self,
        start: KeyLike | None,
        end: KeyLike | None,
        arena: Arena,
        /,
        *,
        collection: CollectionLike,
        direction: ScanDirection = ScanDirection.ASCENDING,
    ) -> Measurement:
        """Bounds on what a range of document keys holds, as `measure` reads them for blobs."""
    def pages_read(
        self,
        keys: KeyLike | KeysLike,
        arena: Arena,
        /,
        *,
        collection: CollectionLike,
        elements: KeyLike | KeysLike | None = None,
        outputs_encoding: PageEncoding = PageEncoding.XML,
    ) -> Values:
        """Renders whole pages, or the subtrees of `elements`, one per task, None where a page or an element is
        absent."""
    def pages_scan(
        self,
        start: KeyLike | None,
        wanted: int,
        arena: Arena,
        /,
        *,
        collection: CollectionLike,
        end: KeyLike | None = None,
        direction: ScanDirection = ScanDirection.ASCENDING,
        mode: ScanMode = ScanMode.ORDERED,
        progress: Progress | None = None,
        filter: str | bytes | None = None,
        predicate: Predicate | None = None,
        predicate_cost: PredicateCost = PredicateCost.CHEAP,
    ) -> Keys:
        """Up to `wanted` page keys from `start`, keeping those holding a node of the XPath `filter`."""
    def pages_sample(self, wanted: int, arena: Arena, /, *, collection: CollectionLike) -> Keys:
        """Up to `wanted` page keys drawn at random, which a `pages_read` follows."""
    def pages_measure(
        self,
        start: KeyLike | None,
        end: KeyLike | None,
        arena: Arena,
        /,
        *,
        collection: CollectionLike,
        direction: ScanDirection = ScanDirection.ASCENDING,
    ) -> Measurement:
        """Bounds on what a range of page keys holds, as `measure` reads them for blobs."""
    def graph_read_edges(
        self, vertices: KeysLike, arena: Arena, /, *, role: Role = Role.ANY, collection: CollectionLike
    ) -> Neighborhoods: ...
    def graph_degrees(
        self, vertices: KeysLike, arena: Arena, /, *, role: Role = Role.ANY, collection: CollectionLike
    ) -> tuple[int | None, ...]: ...
    def graph_contains(self, vertex: KeyLike, /, *, collection: CollectionLike) -> bool:
        """Whether the vertex is present, read as its degree through a one-off arena."""
    def graph_scan_vertices(
        self,
        start: KeyLike | None,
        wanted: int,
        arena: Arena,
        /,
        *,
        collection: CollectionLike,
        end: KeyLike | None = None,
        direction: ScanDirection = ScanDirection.ASCENDING,
        mode: ScanMode = ScanMode.ORDERED,
        progress: Progress | None = None,
        predicate: Predicate | None = None,
        predicate_cost: PredicateCost = PredicateCost.CHEAP,
    ) -> Keys: ...
    def graph_sample_vertices(self, wanted: int, arena: Arena, /, *, collection: CollectionLike) -> Keys:
        """Up to `wanted` vertices drawn at random, those without edges included."""
    def graph_measure_vertices(
        self,
        start: KeyLike | None,
        end: KeyLike | None,
        arena: Arena,
        /,
        *,
        collection: CollectionLike,
        direction: ScanDirection = ScanDirection.ASCENDING,
    ) -> Measurement:
        """Bounds on what a range of vertices holds, as `measure` reads them for blobs."""

@type_check_only
class _Receivers:
    """The views a database, a transaction, a snapshot or a view derives."""

    def on(self, executor: Executor) -> View:
        """A view whose calls block on `executor`, after every call submitted to it before."""

    def deferred(self, executor: Executor) -> DeferredView:
        """A view whose calls `executor` runs while each verb answers a `Deferred` at once."""

@type_check_only
class _Writes(_Reads):
    """The write verbs every blocking receiver but a snapshot answers."""

    def write(
        self,
        keys: KeysLike,
        values: Sequence[ValueLike] | ValueLike | TapeLike,
        *,
        collection: CollectionLike,
        durability: Durability = Durability.BUFFERED,
    ) -> None: ...
    def set(
        self,
        key: KeyLike,
        value: ValueLike,
        *,
        collection: CollectionLike,
        durability: Durability = Durability.BUFFERED,
    ) -> None: ...
    def remove(
        self,
        keys: KeyLike | KeysLike,
        *,
        collection: CollectionLike,
        ends: KeyLike | KeysLike | None = None,
        durability: Durability = Durability.BUFFERED,
    ) -> None:
        """Removes keys with their values, or every key of a range up to its key in `ends`."""
    def docs_write(
        self,
        keys: KeysLike | None,
        documents: Sequence[DocumentLike] | DocumentLike | TapeLike,
        *,
        collection: CollectionLike,
        field: str | None = None,
        modification: Modification = Modification.UPSERT,
        type: FieldType = FieldType.JSON,
        id_field: str | None = None,
        durability: Durability = Durability.BUFFERED,
    ) -> None: ...
    def docs_remove(
        self,
        keys: KeyLike | KeysLike,
        *,
        collection: CollectionLike,
        ends: KeyLike | KeysLike | None = None,
        durability: Durability = Durability.BUFFERED,
    ) -> None:
        """Removes whole documents, or every document of a range up to its key in `ends`."""
    def pages_import(
        self,
        keys: KeyLike | KeysLike,
        markup: TextsLike,
        *,
        collection: CollectionLike,
        encoding: PageEncoding = PageEncoding.XML,
        durability: Durability = Durability.BUFFERED,
        progress: Progress | None = None,
    ) -> None:
        """Replaces whole pages with parsed markup, minting a key for every element."""
    def import_arrow(
        self, source: ArrowSource, *, collection: CollectionLike, durability: Durability | None = None
    ) -> int:
        """Writes every batch of an Arrow stream through the writes of the collection's modality, answering the rows
        written."""
    def pages_insert(
        self,
        keys: KeyLike | KeysLike,
        fragments: TextsLike,
        *,
        collection: CollectionLike,
        elements: KeyLike | KeysLike | None = None,
        parents: KeyLike | KeysLike | None = None,
        anchors: KeyLike | KeysLike | None = None,
        encoding: PageEncoding = PageEncoding.XML,
        durability: Durability = Durability.BUFFERED,
    ) -> tuple[UUID, ...]:
        """Parses fragments into elements under `parents` after `anchors`, the root and the front where None, answering
        the key every element ended up with, given in `elements` or minted."""
    def pages_remove(
        self,
        keys: KeyLike | KeysLike,
        elements: KeyLike | KeysLike | None = None,
        *,
        collection: CollectionLike,
        ends: KeyLike | KeysLike | None = None,
        durability: Durability = Durability.BUFFERED,
    ) -> None:
        """Removes elements with their subtrees, where the page key or no elements removes the whole page, and every
        page of a range up to its key in `ends`."""
    def pages_move(
        self,
        keys: KeyLike | KeysLike,
        elements: KeyLike | KeysLike,
        *,
        collection: CollectionLike,
        parents: KeyLike | KeysLike | None = None,
        anchors: KeyLike | KeysLike | None = None,
        durability: Durability = Durability.BUFFERED,
    ) -> None:
        """Moves elements with their subtrees under `parents` after `anchors`, keeping their keys."""
    def pages_splice(
        self,
        keys: KeyLike | KeysLike,
        elements: KeyLike | KeysLike,
        starts: LengthsLike,
        lengths: LengthsLike,
        texts: TextsLike,
        *,
        collection: CollectionLike,
        durability: Durability = Durability.BUFFERED,
    ) -> None:
        """Replaces `lengths` bytes of the text of every element from `starts` on with `texts`."""
    def pages_set_attributes(
        self,
        keys: KeyLike | KeysLike,
        elements: KeyLike | KeysLike,
        fields: TextsLike,
        values: TextsLike,
        *,
        collection: CollectionLike,
        durability: Durability = Durability.BUFFERED,
    ) -> None:
        """Sets one attribute of an element per task, adding the ones it lacks."""
    def pages_remove_attributes(
        self,
        keys: KeyLike | KeysLike,
        elements: KeyLike | KeysLike,
        fields: TextsLike,
        *,
        collection: CollectionLike,
        durability: Durability = Durability.BUFFERED,
    ) -> None:
        """Removes one attribute of an element per task."""
    def graph_write_edges(
        self,
        sources: KeysLike,
        targets: KeysLike,
        *,
        edges: KeysLike | None = None,
        collection: CollectionLike,
        durability: Durability = Durability.BUFFERED,
    ) -> None: ...
    def graph_remove_edges(
        self,
        sources: KeysLike,
        targets: KeysLike,
        *,
        edges: KeysLike | None = None,
        collection: CollectionLike,
        durability: Durability = Durability.BUFFERED,
    ) -> None: ...
    def graph_write_vertices(
        self, vertices: KeysLike, *, collection: CollectionLike, durability: Durability = Durability.BUFFERED
    ) -> None: ...
    def graph_remove_vertices(
        self,
        vertices: KeysLike,
        *,
        role: Role = Role.ANY,
        collection: CollectionLike,
        ends: KeysLike | None = None,
        durability: Durability = Durability.BUFFERED,
    ) -> None: ...

class Database(_Writes, _Receivers):
    """An embedded database, opened through one class method per engine."""

    capabilities: Capabilities

    def ingest_arrow(
        self,
        source: ArrowSource,
        *,
        collection: CollectionLike,
        order: IngestOrder = "sorted",
        durability: Durability | None = None,
    ) -> int:
        """Lands every batch of an Arrow stream as one commit of its own at the head, answering the rows landed."""

    @classmethod
    def open_ram(
        cls,
        directory: PathLikeOrStr | None = None,
        *,
        isolation: Isolation = Isolation.STRICT_SERIALIZABLE,
        executor: Executor | None = None,
        cpu_class: CPUClass = CPUClass.AUTOMATIC,
    ) -> Self:
        """An in-memory database, persisted as one image when it names a directory, opened blocking on `executor`
        and confined to `cpu_class`."""

    @classmethod
    def open_rocksdb(
        cls,
        directory: PathLikeOrStr,
        *,
        data_directories: Sequence[DataDirectory] = (),
        executor: Executor | None = None,
    ) -> Self:
        """A database on RocksDB, whose table files may spread over data directories, opened blocking on `executor`."""

    @classmethod
    def view_rocksdb(
        cls,
        directory: PathLikeOrStr,
        *,
        data_directories: Sequence[DataDirectory] = (),
        executor: Executor | None = None,
    ) -> Self:
        """A read-only database on RocksDB, refusing every change with `ReadOnlyError` while snapshots and their saves
        still serve, opened blocking on `executor`."""

    @classmethod
    def open_leveldb(cls, directory: PathLikeOrStr, *, executor: Executor | None = None) -> Self:
        """A database on LevelDB, opened blocking on `executor`."""

    @classmethod
    def open_nvme(
        cls,
        manifest: PathLikeOrStr,
        *,
        executor: Executor | None = None,
        data_directories: Sequence[DataDirectory] = (),
        compactions: Executor | None = None,
        cpu_class: CPUClass = CPUClass.AUTOMATIC,
        memory_limit: int = 0,
        transaction_max_bytes: int = 0,
        transaction_max_deltas: int = 0,
        collection_concurrency_limit: int = 0,
        collection_cache_limit: int = 0,
        collection_memory_runs_max_bytes: int = 0,
        collection_memory_runs_max_deltas: int = 0,
        collection_level_enlarge_factor: int = 0,
        collection_value_size: int = 0,
        collection_value_max_size: int = 0,
    ) -> Self:
        """A database on the NVMe engine, named by its `manifest`, `<database>.ustore.json` in an existing directory or
        the raw address of its first slot, where every zero size keeps the engine's default, opened blocking on
        `executor`, which tells it how to move bytes when `Executor.for_nvme` made it, merging on the device of
        `compactions` when `Executor.for_vram` made it."""
    def __enter__(self) -> Self: ...
    def __exit__(
        self,
        kind: type[BaseException] | None = None,
        value: BaseException | None = None,
        traceback: TracebackType | None = None,
    ) -> None: ...
    def close(self) -> None:
        """Closes the database once every deferred call on it finished, while its open transactions and snapshots
        keep working, each holding the database open until it closes too."""

    def clear(self, mode: DropMode, /) -> None:
        """Empties every collection at once: its pairs, every collection with its name, or its values."""

    def compact(self, *, progress: Progress | None = None) -> None:
        """Reclaims the space overwritten and removed data takes, one collection at a time, reporting bytes where the
        engine measures them and collections elsewhere."""

    def describe(self) -> Description:
        """The engine, its capabilities and isolation, and the keys, memory and disk it holds."""

    def collection_create(self, name: str, *, modality: Modality) -> UUID:
        """Creates an empty collection holding `modality` and records the name for it, which may not start with `_`."""

    def collection_find(self, name: str, arena: Arena, /) -> UUID:
        """The collection ID the name addresses."""

    def collection_contains(self, name: str) -> bool:
        """Whether a collection carries the name, read into a one-off arena."""

    def collection_list(self, arena: Arena, /) -> tuple[UUID, ...]:
        """The IDs of every collection, never a system one."""

    def collection_names(self, arena: Arena, /, *, after: str | None = None, limit: int = 1024) -> tuple[str, ...]:
        """One page of the names recorded for collections, in the order of their text."""

    def collection_drop(self, collection: UUID | str, mode: DropMode = DropMode.COLLECTION) -> None:
        """Removes a collection, or empties its pairs or its values."""

    def snapshot(self) -> Snapshot:
        """Pins a point-in-time state of the whole database, which the snapshot's read verbs see."""

    def changes_cursor(self, since: int = 0, /, *, collection: CollectionLike) -> ChangesCursor:
        """A feed of the changes committed to `collection` after the commit stamp `since`, oldest first."""

    def transaction(self, watch: Watch = Watch.KEYS) -> Transaction:
        """Begins a transaction over this database, recording the keys its verbs touch unless `Watch.NONE`."""

class Snapshot(_Reads, _Receivers):
    """A point-in-time state of the whole database, whose read verbs see what it pinned."""

    database: Database
    read_stamp: int
    """The stamp the snapshot reads at: it sees every commit whose commit stamp is at or below it, and none above."""

    def __enter__(self) -> Self: ...
    def __exit__(
        self,
        kind: type[BaseException] | None = None,
        value: BaseException | None = None,
        traceback: TracebackType | None = None,
    ) -> None: ...
    def save(self, path: PathLikeOrStr, /, *, progress: Progress | None = None) -> None:
        """Writes the snapshot out as a new database of the same engine, holding exactly the collections it records,
        reporting the bytes of keys and values saved; a cancelled save removes what it wrote."""

    def collection_find(self, name: str, arena: Arena, /) -> UUID:
        """The collection ID the name addresses at this snapshot."""

    def collection_list(self, arena: Arena, /) -> tuple[UUID, ...]:
        """The IDs of every collection at this snapshot, never a system one."""

    def close(self) -> None:
        """Releases the snapshot once its deferred calls finished, unpinning the versions only it kept."""

class Transaction(_Writes, _Receivers):
    """An optimistic transaction grouping operations across collections and modalities, one call at a time."""

    database: Database

    def __enter__(self) -> Self: ...
    def __exit__(
        self,
        kind: type[BaseException] | None = None,
        value: BaseException | None = None,
        traceback: TracebackType | None = None,
    ) -> None: ...
    def watched(self) -> View:
        """A view whose calls record the keys they touch for conflicts."""

    def unwatched(self) -> View:
        """A view whose calls record nothing, so they never fail the commit."""

    def commit(self, durability: Durability = Durability.BUFFERED) -> int:
        """Applies every staged change, or none of them, answering the commit stamp: the commit's position in commit
        order, which every snapshot whose read stamp is at or above it sees."""

    def stage(self) -> int:
        """Runs the first phase of a two-phase commit, answering the read stamp: the stamp the transaction reads at
        and its commit validates against, no greater than the commit stamp that follows."""

    def rollback(self) -> None:
        """Withdraws a stage, keeping the reads and writes, so the transaction can stage or commit again."""

    def reset(self) -> None:
        """Drops every read and write and starts a new generation on the same handle."""

    def close(self) -> None:
        """Drops whatever is still staged and releases the handle, once its deferred calls finished."""

class View(_Writes, _Receivers):
    """What a database, transaction or snapshot addresses, with the executor its calls block on."""

    database: Database
    executor: Executor | None
    """The executor the view's calls run on, where an arena for them belongs, or None for the calling thread."""

    def watched(self) -> View:
        """A view of the same transaction whose calls record the keys they touch for conflicts."""

    def unwatched(self) -> View:
        """A view of the same transaction whose calls record nothing."""

    def ingest_arrow(
        self,
        source: ArrowSource,
        *,
        collection: CollectionLike,
        order: IngestOrder = "sorted",
        durability: Durability | None = None,
    ) -> int:
        """Lands every batch of an Arrow stream as one commit of its own at the head, answering the rows landed; a view of a transaction raises TypeError."""

    def commit(self, durability: Durability = Durability.BUFFERED) -> int:
        """Commits the transaction on the executor, answering the commit stamp."""

    def stage(self) -> int:
        """Stages the transaction on the executor, answering the read stamp."""

    def save(self, path: PathLikeOrStr, /, *, progress: Progress | None = None) -> None:
        """Saves the snapshot on the executor, refusing a view of anything else."""

    def clear(self, mode: DropMode, /) -> None:
        """Clears the database on the executor, refusing a view of a transaction or a snapshot."""

    def compact(self, *, progress: Progress | None = None) -> None:
        """Compacts the database on the executor, refusing a view of a transaction or a snapshot."""

    def describe(self) -> Description:
        """Describes the database on the executor, refusing a view of a transaction or a snapshot."""

    def collection_find(self, name: str, arena: Arena, /) -> UUID:
        """The collection ID the name addresses, at the snapshot of a snapshot and at the head otherwise."""

    def collection_list(self, arena: Arena, /) -> tuple[UUID, ...]:
        """The IDs of every collection, at the snapshot of a snapshot and at the head otherwise."""

class DeferredView:
    """What a database, transaction or snapshot addresses, with the executor its calls go to, each verb answering a
    `Deferred` at once. Every pull of a cursor blocks, so it opens none."""

    database: Database
    executor: Executor | None
    """The executor the view's calls go to, where an arena for them belongs."""

    def __getitem__(self, name: str) -> Collection:
        """The collection `name` addresses, whose `blobs[key]` and `docs[key]` answer a `Deferred`."""

    def on(self, executor: Executor) -> View:
        """on(executor) answers a View whose calls block on the executor.

        Every call runs after the ones submitted to the executor before it finishes, so a read
        through it sees what earlier deferred writes wrote.

        Args:
          executor (Executor): The threads that run the calls.
        """

    def deferred(self, executor: Executor) -> DeferredView:
        """deferred(executor) answers a DeferredView whose calls the executor runs meanwhile.

        Every verb of it submits its call and answers a Deferred at once.

        Args:
          executor (Executor): The threads that run the calls.
        """

    def watched(self) -> DeferredView:
        """watched() answers a view of the transaction whose calls record the keys they touch for conflicts."""

    def unwatched(self) -> DeferredView:
        """unwatched() answers a view of the transaction whose calls record nothing, so they never fail its commit."""

    def read(
        self,
        keys: KeysLike,
        arena: Arena,
        /,
        *,
        collection: CollectionLike,
        offset: int = 0,
        limit: int | None = None,
    ) -> Deferred[Values]:
        """read(keys, arena, *, collection, offset=0, limit=None) answers a Values tape.

        Reads a batch of binary values in one call, which no sequence of single reads
        matches for throughput. Nothing is copied: the tape sits in the arena, and indexing
        the answer gives a memoryview over the stored bytes, or None for an absent key.

        Args:
          keys (Sequence[KeyLike] | buffer): The keys to read, as a sequence, a contiguous
            buffer of 16-byte keys, which is read in place, or one of 64-bit integers.
          arena (Arena): Where the tape lands, rewound by this call.
          collection (UUID | str): The collection, by identifier or by name.
          offset (int, optional): Byte of every value the read starts at, where an offset
            past the end reads empty.
          limit (int, optional): Most bytes of every value to read from the offset, or None
            for the rest of it.
        Returns:
          Values: One entry per key, in the order they were asked for.
        Raises:
          ContendedError: The arena serves another call or exports a buffer.
          Error: Whatever the engine reported.
        """

    def get(self, key: KeyLike, *, collection: CollectionLike) -> Deferred[bytes | None]:
        """The `Deferred` of a copy of one value, owning the one-off arena it reads into."""

    def contains(self, key: KeyLike, *, collection: CollectionLike) -> Deferred[bool]:
        """The `Deferred` of one presence, owning the one-off arena it reads into."""

    def scan(
        self,
        start: KeyLike | None,
        wanted: int,
        arena: Arena,
        /,
        *,
        collection: CollectionLike,
        end: KeyLike | None = None,
        direction: ScanDirection = ScanDirection.ASCENDING,
        mode: ScanMode = ScanMode.ORDERED,
        progress: Progress | None = None,
        patterns: Sequence[str | bytes] | None = None,
        pattern_kind: PatternKind = PatternKind.REGEX,
        sensitivity: CaseSensitivity = CaseSensitivity.CASED,
        weights: Sequence[float] | None = None,
        predicate: Predicate | None = None,
        predicate_cost: PredicateCost = PredicateCost.CHEAP,
    ) -> Deferred[Keys]:
        """scan(start, wanted, arena, *, collection, end=None, direction=ScanDirection.ASCENDING,
        mode=ScanMode.ORDERED, progress=None) answers a Keys tape.

        Lists up to `wanted` keys from `start` towards `end`. Values are not exported, so a
        read follows the scan, or a `cursor()` walks a collection with its values instead.

        Args:
          start (KeyLike | None): The first key the scan may return, or None for the first
            key in its direction.
          wanted (int): Most keys the scan returns.
          arena (Arena): Where the keys land, rewound by this call.
          collection (UUID | str): The collection, by identifier or by name.
          end (KeyLike, optional): The key the scan stops before, or None for no bound.
          direction (ScanDirection, optional): `ScanDirection.DESCENDING` walks down from
            `start`, largest key first.
          mode (ScanMode, optional): `ScanMode.UNORDERED` lets keys come out of order,
            removed or repeated, to speed up the read after the scan; no engine uses it yet.
          progress (Callable[[int, int], bool | None], optional): Called with the scans
            finished and the scans in all; a falsy answer other than None cancels the call.
        Returns:
          Keys: One run of keys, in the order of the direction.
        Raises:
          CancelledError: The progress callable stopped the scan.
        """

    def scan_batch(
        self,
        starts: KeysLike,
        wanted: int,
        arena: Arena,
        /,
        *,
        collection: CollectionLike,
        ends: KeysLike | None = None,
        direction: ScanDirection = ScanDirection.ASCENDING,
        mode: ScanMode = ScanMode.ORDERED,
        progress: Progress | None = None,
        patterns: Sequence[str | bytes] | None = None,
        pattern_kind: PatternKind = PatternKind.REGEX,
        sensitivity: CaseSensitivity = CaseSensitivity.CASED,
        weights: Sequence[float] | None = None,
        predicate: Predicate | None = None,
        predicate_cost: PredicateCost = PredicateCost.CHEAP,
    ) -> Deferred[Keys]:
        """The `Deferred` of one scan per start, whose `progress` runs on the executor's threads."""
    def sample(self, wanted: int, arena: Arena, /, *, collection: CollectionLike) -> Deferred[Keys]:
        """sample(wanted, arena, *, collection) answers a Keys tape.

        Draws keys uniformly at random from one collection, which is how a workload picks
        entries it has no list of.

        Args:
          wanted (int): Most keys to draw.
          arena (Arena): Where the keys land, rewound by this call.
          collection (UUID | str): The collection, by identifier or by name.
        Returns:
          Keys: One run of keys, in no particular order.
        """

    def sample_batch(self, count: int, wanted: int, arena: Arena, /, *, collection: CollectionLike) -> Deferred[Keys]:
        """sample_batch(count, wanted, arena, *, collection) answers a Keys tape.

        Draws `count` independent samples in one call, all from the one collection named,
        which is how a workload builds several random batches without paying for a call each.

        Args:
          count (int): Samples to draw.
          wanted (int): Most keys every sample draws.
          arena (Arena): Where the keys land, rewound by this call.
          collection (UUID | str): The one collection every sample draws from, by
            identifier or by name.
        Returns:
          Keys: One run of keys per sample, in no particular order.
        """

    def measure(
        self,
        start: KeyLike | None,
        end: KeyLike | None,
        arena: Arena,
        /,
        *,
        collection: CollectionLike,
        direction: ScanDirection = ScanDirection.ASCENDING,
    ) -> Deferred[Measurement]:
        """measure(start, end, arena, *, collection, direction=ScanDirection.ASCENDING)
        answers a Measurement.

        Estimates what a key range holds without walking it, so the answers are bounds
        rather than counts.

        Args:
          start (KeyLike | None): First key of the range, or None for the first key in the
            direction.
          end (KeyLike | None): The key the range stops before, or None for no bound.
          arena (Arena): Where the bounds land before they are copied out, rewound by this
            call.
          collection (UUID | str): The collection, by identifier or by name.
          direction (ScanDirection, optional): `ScanDirection.DESCENDING` ranges down from
            `start`.
        Returns:
          Measurement: The lower and upper bounds on the entries, the bytes of their values,
          and the space they take on disk.
        """

    def docs_read(
        self,
        keys: KeysLike,
        arena: Arena,
        /,
        *,
        collection: CollectionLike,
        field: str | None = None,
        outputs_type: FieldType = FieldType.JSON,
    ) -> Deferred[Values]:
        """docs_read(keys, arena, *, collection, field=None, outputs_type=FieldType.JSON)
        answers a Values tape.

        Reads whole documents, or one field of each, as JSON text or as printed strings. Whole
        documents read as JSON pass straight through from the engine, so nothing is parsed.

        Args:
          keys (Sequence[KeyLike] | buffer): The keys to read.
          arena (Arena): Where the tape lands, rewound by this call.
          collection (UUID | str): The collection, by identifier or by name.
          field (str, optional): A top-level key or a JSON Pointer to read inside every
            document; None reads whole documents.
          outputs_type (FieldType, optional): `FieldType.JSON` for JSON text, or
            `FieldType.STRING` to print scalars as text.
        Returns:
          Values: One entry per key, None where the document or the field is absent.
        """

    def docs_gist(self, keys: KeysLike, arena: Arena, /, *, collection: CollectionLike) -> Deferred[tuple[str, ...]]:
        """docs_gist(keys, arena, *, collection) answers a tuple of JSON Pointers.

        Lists every distinct path to a leaf across the requested documents, in the order they
        were first met, which is how a table learns the columns its documents hold.

        Args:
          keys (Sequence[KeyLike] | buffer): The documents to inspect.
          arena (Arena): Where the paths land before they are copied out, rewound by this
            call.
          collection (UUID | str): The collection, by identifier or by name.
        Returns:
          tuple[str, ...]: One JSON Pointer per leaf, such as '/user/name'.
        """

    def docs_gather(
        self,
        keys: KeysLike,
        fields: Sequence[str],
        outputs_types: FieldType | Sequence[FieldType],
        arena: Arena,
        /,
        *,
        collection: CollectionLike,
    ) -> Deferred[Columns]:
        """docs_gather(keys, fields, outputs_types, arena, *, collection) answers Columns.

        Reads every field of every document into typed columns, converting each value on a
        best-effort basis and recording what was found, converted, or could not be.

        Args:
          keys (Sequence[KeyLike] | buffer): The documents, the rows of every column.
          fields (Sequence[str]): Top-level keys or JSON Pointers, the columns of the result.
          outputs_types (FieldType | Sequence[FieldType]): One type per field, or one for
            every field: a fixed-width scalar from `FieldType.BOOL` to `FieldType.U8`,
            `FieldType.UUID`, `FieldType.STRING` or `FieldType.BINARY`. A `FieldType.F16`
            column exports as a half-precision view NumPy reads as float16, and a
            `FieldType.UUID` column as sixteen bytes per document.
          arena (Arena): Where the columns land, rewound by this call.
          collection (UUID | str): The collection, by identifier or by name.
        Returns:
          Columns: One column per field, each holding one entry per document.
        """

    def docs_scan(
        self,
        start: KeyLike | None,
        wanted: int,
        arena: Arena,
        /,
        *,
        collection: CollectionLike,
        end: KeyLike | None = None,
        direction: ScanDirection = ScanDirection.ASCENDING,
        mode: ScanMode = ScanMode.ORDERED,
        progress: Progress | None = None,
        filter: str | bytes | None = None,
        predicate: Predicate | None = None,
        predicate_cost: PredicateCost = PredicateCost.CHEAP,
    ) -> Deferred[Keys]:
        """docs_scan(start, wanted, arena, *, collection, end=None,
        direction=ScanDirection.ASCENDING, mode=ScanMode.ORDERED, progress=None)
        answers a Keys tape.

        Lists up to `wanted` document keys from `start` towards `end`, as `scan` lists the
        keys of blobs. Documents are not exported, so a `docs_read` follows the scan.

        Args:
          start (KeyLike | None): The first key the scan may return, or None for the first
            key in its direction.
          wanted (int): Most keys the scan returns.
          arena (Arena): Where the keys land, rewound by this call.
          collection (UUID | str): The docs collection, by identifier or by name.
          end (KeyLike, optional): The key the scan stops before, or None for no bound.
          direction (ScanDirection, optional): Which way the scan walks, as `scan` takes it.
          mode (ScanMode, optional): In what order the keys come, as `scan` takes it.
          progress (Callable[[int, int], bool | None], optional): Called with the scans
            finished and the scans in all; a falsy answer other than None cancels the call.
        Returns:
          Keys: One run of keys, in the order of the direction.
        Raises:
          CancelledError: The progress callable stopped the scan.
        """

    def docs_sample(self, wanted: int, arena: Arena, /, *, collection: CollectionLike) -> Deferred[Keys]:
        """docs_sample(wanted, arena, *, collection) answers a Keys tape.

        Draws document keys uniformly at random from one collection, as `sample` draws the keys
        of blobs. Documents are not exported, so a `docs_read` or a `docs_gather` follows.

        Args:
          wanted (int): Most keys to draw.
          arena (Arena): Where the keys land, rewound by this call.
          collection (UUID | str): The docs collection, by identifier or by name.
        Returns:
          Keys: One run of keys, in no particular order.
        """

    def docs_measure(
        self,
        start: KeyLike | None,
        end: KeyLike | None,
        arena: Arena,
        /,
        *,
        collection: CollectionLike,
        direction: ScanDirection = ScanDirection.ASCENDING,
    ) -> Deferred[Measurement]:
        """The `Deferred` of bounds on what a range of document keys holds."""

    def pages_read(
        self,
        keys: KeyLike | KeysLike,
        arena: Arena,
        /,
        *,
        collection: CollectionLike,
        elements: KeyLike | KeysLike | None = None,
        outputs_encoding: PageEncoding = PageEncoding.XML,
    ) -> Deferred[Values]:
        """Renders whole pages, or the subtrees of `elements`, one per task, None where a page or an element is
        absent."""

    def pages_scan(
        self,
        start: KeyLike | None,
        wanted: int,
        arena: Arena,
        /,
        *,
        collection: CollectionLike,
        end: KeyLike | None = None,
        direction: ScanDirection = ScanDirection.ASCENDING,
        mode: ScanMode = ScanMode.ORDERED,
        progress: Progress | None = None,
        filter: str | bytes | None = None,
        predicate: Predicate | None = None,
        predicate_cost: PredicateCost = PredicateCost.CHEAP,
    ) -> Deferred[Keys]:
        """Up to `wanted` page keys from `start`, keeping those holding a node of the XPath `filter`."""

    def pages_sample(self, wanted: int, arena: Arena, /, *, collection: CollectionLike) -> Deferred[Keys]:
        """The `Deferred` of up to `wanted` page keys drawn at random."""

    def pages_measure(
        self,
        start: KeyLike | None,
        end: KeyLike | None,
        arena: Arena,
        /,
        *,
        collection: CollectionLike,
        direction: ScanDirection = ScanDirection.ASCENDING,
    ) -> Deferred[Measurement]:
        """The `Deferred` of bounds on what a range of page keys holds."""

    def graph_read_edges(
        self, vertices: KeysLike, arena: Arena, /, *, role: Role = Role.ANY, collection: CollectionLike
    ) -> Deferred[Neighborhoods]:
        """graph_read_edges(vertices, arena, *, role=Role.ANY, collection) answers Neighborhoods.

        Reads every edge a batch of vertices takes part in. The edges of one vertex come sorted
        by neighbour; with both roles the outgoing ones precede the incoming ones.

        Args:
          vertices (Sequence[KeyLike] | buffer): The vertices whose edges are read.
          arena (Arena): Where the degrees and the edges land, rewound by this call.
          role (Role, optional): `Role.SOURCE` for the edges a vertex leaves by, `Role.TARGET`
            for those it is entered by, or `Role.ANY` for both, which is how an undirected
            graph asks.
          collection (UUID | str): The graph collection, by identifier or by name.
        Returns:
          Neighborhoods: One run of (source, target, edge) triples per vertex, None where it is
          absent.
        """

    def graph_degrees(
        self, vertices: KeysLike, arena: Arena, /, *, role: Role = Role.ANY, collection: CollectionLike
    ) -> Deferred[tuple[int | None, ...]]:
        """graph_degrees(vertices, arena, *, role=Role.ANY, collection) answers a tuple.

        Counts the edges of a batch of vertices without exporting them.

        Args:
          vertices (Sequence[KeyLike] | buffer): The vertices whose edges are counted.
          arena (Arena): Where the degrees land before they are copied out, rewound by this
            call.
          role (Role, optional): The role of every vertex, as `graph_read_edges` takes it.
          collection (UUID | str): The graph collection, by identifier or by name.
        Returns:
          tuple[int | None, ...]: The degree of every vertex, None where it is absent.
        """

    def graph_contains(self, vertex: KeyLike, /, *, collection: CollectionLike) -> Deferred[bool]:
        """graph_contains(vertex, *, collection) answers a bool.

        Whether a vertex is present, read as its degree through a one-off arena, which a
        loop avoids with `graph_degrees` and an arena of its own.

        Args:
          vertex (KeyLike): The vertex to look for.
          collection (UUID | str): The graph collection, by identifier or by name.
        Returns:
          bool: Whether the vertex is present.
        """

    def graph_scan_vertices(
        self,
        start: KeyLike | None,
        wanted: int,
        arena: Arena,
        /,
        *,
        collection: CollectionLike,
        end: KeyLike | None = None,
        direction: ScanDirection = ScanDirection.ASCENDING,
        mode: ScanMode = ScanMode.ORDERED,
        progress: Progress | None = None,
        predicate: Predicate | None = None,
        predicate_cost: PredicateCost = PredicateCost.CHEAP,
    ) -> Deferred[Keys]:
        """graph_scan_vertices(start, wanted, arena, *, collection, end=None,
        direction=ScanDirection.ASCENDING, mode=ScanMode.ORDERED, progress=None)
        answers a Keys tape.

        Lists up to `wanted` vertices from `start` towards `end`, those without edges included.
        Edges are not exported, so a `graph_read_edges` follows the scan.

        Args:
          start (KeyLike | None): The first vertex the scan may return, or None for the first
            vertex in its direction.
          wanted (int): Most vertices the scan returns.
          arena (Arena): Where the vertices land, rewound by this call.
          collection (UUID | str): The graph collection, by identifier or by name.
          end (KeyLike, optional): The vertex the scan stops before, or None for no bound.
          direction (ScanDirection, optional): Which way the scan walks, as `scan` takes it.
          mode (ScanMode, optional): In what order the vertices come, as `scan` takes it.
          progress (Callable[[int, int], bool | None], optional): Called with the scans
            finished and the scans in all; a falsy answer other than None cancels the call.
        Returns:
          Keys: One run of vertices, in the order of the direction.
        Raises:
          CancelledError: The progress callable stopped the scan.
        """

    def graph_sample_vertices(self, wanted: int, arena: Arena, /, *, collection: CollectionLike) -> Deferred[Keys]:
        """The `Deferred` of up to `wanted` vertices drawn at random."""

    def graph_measure_vertices(
        self,
        start: KeyLike | None,
        end: KeyLike | None,
        arena: Arena,
        /,
        *,
        collection: CollectionLike,
        direction: ScanDirection = ScanDirection.ASCENDING,
    ) -> Deferred[Measurement]:
        """The `Deferred` of bounds on what a range of vertices holds."""

    def collection_find(self, name: str, arena: Arena, /) -> Deferred[UUID]:
        """collection_find(name, arena) answers the identifier the name addresses.

        Reads the name at the snapshot of a snapshot, and at the head otherwise, since a
        transaction never creates or drops collections.

        Args:
          name (str): The name to resolve.
          arena (Arena): Where the lookup lands before it is copied out, rewound by this
            call.
        Returns:
          UUID: The identifier addressing the collection.
        Raises:
          NotFoundError: No collection carries the name.
        """

    def collection_list(self, arena: Arena, /) -> Deferred[tuple[UUID, ...]]:
        """collection_list(arena) answers the identifiers of every collection.

        Lists the collections at the snapshot of a snapshot, and at the head otherwise.
        System collections are never listed, and collection_names answers the names.

        Args:
          arena (Arena): Where the listing lands before it is copied out, rewound by this
            call.
        Returns:
          tuple[UUID, ...]: One identifier per collection.
        """

    def save(self, path: PathLikeOrStr, /, *, progress: Progress | None = None) -> Deferred[None]:
        """save(path, *, progress=None) answers None.

        Writes the snapshot out as a new database of the same engine, holding exactly the
        collections and names the snapshot records, which that engine's open then serves.
        RocksDB and LevelDB create the directory, the RAM engine writes its image into the
        directory the path names, and NVMe takes the path as the new database's manifest.
        Calls `ustore_snapshot_save`.

        Args:
          path (str | bytes | PathLike): The directory the saved database lands in, or its
            manifest on NVMe.
          progress (Callable[[int, int], bool | None], optional): Called with the bytes of the
            keys and values saved and the bytes in all. A falsy answer other than None stops
            the save, which removes what it wrote.
        Raises:
          CancelledError: The progress callable stopped the save.
          UnsupportedError: The engine saves no snapshots.
        """

    def clear(self, mode: DropMode, /) -> Deferred[None]:
        """clear(mode) answers None.

        Empties every collection at once at the head, as `collection_drop` empties one. Calls
        `ustore_database_clear`.

        Args:
          mode (DropMode): `DropMode.PAIRS` removes every key and value and keeps the
            collections, `DropMode.COLLECTION` drops every collection with its name, and
            `DropMode.VALUES` empties every value and keeps the keys.
        Raises:
          InvalidArgumentError: The RAM and RocksDB engines drop no collection while a
            snapshot is open, since the snapshot belongs to its holder.
          ReadOnlyError: The database was opened read-only.
        """

    def compact(self, *, progress: Progress | None = None) -> Deferred[None]:
        """compact(*, progress=None) answers None.

        Reclaims the space overwritten and removed data still takes, one collection at a time:
        RocksDB and LevelDB flush and compact their trees, and the RAM engine frees the
        versions no reader needs and rewrites its image. Calls `ustore_database_compact`.

        Args:
          progress (Callable[[int, int], bool | None], optional): Called with the units
            compacted and the units in all, bytes where the engine measures them, as RocksDB
            does, and collections elsewhere. A falsy answer other than None stops between
            collections, keeping what was compacted.
        Raises:
          CancelledError: The progress callable stopped the compaction.
          ReadOnlyError: The database was opened read-only.
        """

    def describe(self) -> Deferred[Description]:
        """describe() answers a Description.

        Reports the engine serving the database, what it supports, its isolation, and the
        keys, memory and disk it holds, every size the engine's estimate and zero where it
        keeps none. Calls `ustore_database_describe`.

        Returns:
          Description: The `capabilities`, `engine`, `isolation`, `keys`, `memory_bytes` and
          `disk_bytes` of the database.
        """

    def write(
        self,
        keys: KeysLike,
        values: Sequence[ValueLike] | ValueLike | TapeLike,
        *,
        collection: CollectionLike,
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[None]:
        """write(keys, values, *, collection, durability=Durability.BUFFERED) answers None.

        Upserts a batch of binary values in one call. Every value is borrowed for the
        length of the call rather than copied, so a memory-mapped buffer reaches the engine
        untouched.

        Args:
          keys (Sequence[KeyLike] | buffer): The keys to write.
          values (Sequence[bytes-like] | bytes-like | tape): One value per key, a tape of
            them, or a single bytes-like object broadcast to every key. A tape is an Arrow
            `binary`, `large_binary`, `string` or `large_string` array, such as `Values`, or
            a `(data, offsets)` tuple of buffers with unsigned 32-bit or 64-bit offsets, and
            passes without a copy. None is refused, since `remove` removes keys.
          collection (UUID | str): The collection, by identifier or by name.
          durability (Durability, optional): `Durability.FLUSHED` reaches the drive before
            returning.
        Raises:
          TypeError: A value is None or exports no buffer.
          Error: Whatever the engine reported.
        """

    def set(
        self,
        key: KeyLike,
        value: ValueLike,
        *,
        collection: CollectionLike,
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[None]:
        """set(key, value, *, collection, durability=Durability.BUFFERED) answers None.

        Upserts one binary value.

        Args:
          key (KeyLike): The key to write.
          value (bytes-like): The bytes to store, borrowed rather than copied.
          collection (UUID | str): The collection, by identifier or by name.
          durability (Durability, optional): `Durability.FLUSHED` reaches the drive before
            returning.
        Raises:
          TypeError: The value is None or exports no buffer.
        """

    def remove(
        self,
        keys: KeyLike | KeysLike,
        *,
        collection: CollectionLike,
        ends: KeyLike | KeysLike | None = None,
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[None]:
        """remove(keys, *, collection, ends=None, durability=Durability.BUFFERED) answers None.

        Removes a batch of keys with their values, or one key given on its own, while
        `docs_remove` removes documents. With `ends`, every key starts a range reaching up to
        its end, and every key of the range goes, all in one atomic call.

        Args:
          keys (KeyLike | Sequence[KeyLike] | buffer): The keys to remove, or the first key
            of every range.
          collection (UUID | str): The collection, by identifier or by name.
          ends (KeyLike | Sequence[KeyLike] | buffer, optional): The key every range stops
            before, one per key.
          durability (Durability, optional): `Durability.FLUSHED` reaches the drive before
            returning.
        """

    def docs_write(
        self,
        keys: KeysLike | None,
        documents: Sequence[DocumentLike] | DocumentLike | TapeLike,
        *,
        collection: CollectionLike,
        field: str | None = None,
        modification: Modification = Modification.UPSERT,
        type: FieldType = FieldType.JSON,
        id_field: str | None = None,
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[None]:
        """docs_write(keys, documents, *, collection, field=None, modification=Modification.UPSERT,
                   type=FieldType.JSON, id_field=None, durability=Durability.BUFFERED) answers None.

        Writes a batch of JSON documents, or one field inside each of them. Every document is
        borrowed for the length of the call rather than copied.

        Args:
          keys (Sequence[KeyLike] | buffer | None): The keys to write; None reads every key from
            `id_field` of its document.
          documents (Sequence[str | bytes-like | object] | str | bytes-like | dict | tape): One
            document per key, a tape of them, or one document broadcast to every key. Text,
            bytes and a tape, as `write` takes one, are JSON already, while a dict, a list or a
            number inside a sequence is serialized. None is refused: `remove` removes whole
            documents, and a merge with a JSON null removes a field. Under a fixed-width `type`,
            a contiguous buffer of one scalar per key is a column, which reaches the store
            without being printed as JSON and parsed back.
          collection (UUID | str): The collection, by identifier or by name.
          field (str, optional): A top-level key or a JSON Pointer such as '/user/name' to
            write inside every document; None writes whole documents.
          modification (Modification, optional): An upsert, update or insert, an RFC 6902
            `Modification.PATCH` or an RFC 7386 `Modification.MERGE`.
          type (FieldType, optional): How every document is spelled: `FieldType.JSON`,
            `FieldType.STRING` for a plain string, `FieldType.NULL`, or the raw bytes of
            `FieldType.BOOL`, `FieldType.F64`, `FieldType.F32`, `FieldType.I64` through
            `FieldType.U8`.
          id_field (str, optional): The field holding every key when `keys` is None.
          durability (Durability, optional): `Durability.FLUSHED` reaches the drive before
            returning.
        Raises:
          TypeError: A document is None.
          MalformedError: A document does not parse, or its `id_field` holds no integer.
          NotFoundError: A field is written inside a document that does not exist.
          AlreadyExistsError: An insert names a document that already exists.
        """

    def docs_remove(
        self,
        keys: KeyLike | KeysLike,
        *,
        collection: CollectionLike,
        ends: KeyLike | KeysLike | None = None,
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[None]:
        """docs_remove(keys, *, collection, ends=None, durability=Durability.BUFFERED)
        answers None.

        Removes a batch of whole documents, or one given on its own; a patch or a merge removes
        fields inside them instead. With `ends`, every key starts a range of documents removed
        up to its end, as `remove` takes them.

        Args:
          keys (KeyLike | Sequence[KeyLike] | buffer): The keys of the documents to remove, or
            the first key of every range.
          collection (UUID | str): The docs collection, by identifier or by name.
          ends (KeyLike | Sequence[KeyLike] | buffer, optional): The key every range stops
            before, one per key.
          durability (Durability, optional): `Durability.FLUSHED` reaches the drive before
            returning.
        """

    def pages_import(
        self,
        keys: KeyLike | KeysLike,
        markup: TextsLike,
        *,
        collection: CollectionLike,
        encoding: PageEncoding = PageEncoding.XML,
        durability: Durability = Durability.BUFFERED,
        progress: Progress | None = None,
    ) -> Deferred[None]:
        """Replaces whole pages with parsed markup, minting a key for every element."""
    def import_arrow(
        self, source: ArrowSource, *, collection: CollectionLike, durability: Durability | None = None
    ) -> Deferred[int]:
        """Writes every batch of an Arrow stream through the writes of the collection's modality."""
    def ingest_arrow(
        self,
        source: ArrowSource,
        *,
        collection: CollectionLike,
        order: IngestOrder = "sorted",
        durability: Durability | None = None,
    ) -> Deferred[int]:
        """Lands every batch of an Arrow stream as one commit of its own at the head, refused through a transaction."""
    def pages_insert(
        self,
        keys: KeyLike | KeysLike,
        fragments: TextsLike,
        *,
        collection: CollectionLike,
        elements: KeyLike | KeysLike | None = None,
        parents: KeyLike | KeysLike | None = None,
        anchors: KeyLike | KeysLike | None = None,
        encoding: PageEncoding = PageEncoding.XML,
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[tuple[UUID, ...]]:
        """Parses fragments into elements under `parents` after `anchors`, the root and the front where None, answering
        the key every element ended up with, given in `elements` or minted."""
    def pages_remove(
        self,
        keys: KeyLike | KeysLike,
        elements: KeyLike | KeysLike | None = None,
        *,
        collection: CollectionLike,
        ends: KeyLike | KeysLike | None = None,
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[None]:
        """Removes elements with their subtrees, where the page key or no elements removes the whole page, and every
        page of a range up to its key in `ends`."""
    def pages_move(
        self,
        keys: KeyLike | KeysLike,
        elements: KeyLike | KeysLike,
        *,
        collection: CollectionLike,
        parents: KeyLike | KeysLike | None = None,
        anchors: KeyLike | KeysLike | None = None,
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[None]:
        """Moves elements with their subtrees under `parents` after `anchors`, keeping their keys."""
    def pages_splice(
        self,
        keys: KeyLike | KeysLike,
        elements: KeyLike | KeysLike,
        starts: LengthsLike,
        lengths: LengthsLike,
        texts: TextsLike,
        *,
        collection: CollectionLike,
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[None]:
        """Replaces `lengths` bytes of the text of every element from `starts` on with `texts`."""
    def pages_set_attributes(
        self,
        keys: KeyLike | KeysLike,
        elements: KeyLike | KeysLike,
        fields: TextsLike,
        values: TextsLike,
        *,
        collection: CollectionLike,
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[None]:
        """Sets one attribute of an element per task, adding the ones it lacks."""
    def pages_remove_attributes(
        self,
        keys: KeyLike | KeysLike,
        elements: KeyLike | KeysLike,
        fields: TextsLike,
        *,
        collection: CollectionLike,
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[None]:
        """Removes one attribute of an element per task."""

    def graph_write_edges(
        self,
        sources: KeysLike,
        targets: KeysLike,
        *,
        edges: KeysLike | None = None,
        collection: CollectionLike,
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[None]:
        """graph_write_edges(sources, targets, *, edges=None, collection,
                          durability=Durability.BUFFERED) answers None.

        Inserts a batch of edges, creating the vertices they connect. An edge already present
        is kept, so writing the same batch twice changes nothing.

        Args:
          sources (Sequence[KeyLike] | buffer): The vertex every edge leaves.
          targets (Sequence[KeyLike] | buffer): The vertex every edge enters.
          edges (Sequence[KeyLike] | buffer, optional): The identifier of every edge, which tells
            apart several edges between one pair; None gives every edge `edge_id_default`.
          collection (UUID | str): The graph collection, by identifier or by name.
          durability (Durability, optional): `Durability.FLUSHED` reaches the drive before
            returning.
        """

    def graph_remove_edges(
        self,
        sources: KeysLike,
        targets: KeysLike,
        *,
        edges: KeysLike | None = None,
        collection: CollectionLike,
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[None]:
        """graph_remove_edges(sources, targets, *, edges=None, collection,
                           durability=Durability.BUFFERED) answers None.

        Removes a batch of edges and keeps the vertices they connected, even once isolated.

        Args:
          sources (Sequence[KeyLike] | buffer): The vertex every edge leaves.
          targets (Sequence[KeyLike] | buffer): The vertex every edge enters.
          edges (Sequence[KeyLike] | buffer, optional): The identifier of every edge; None removes
            the edges that were given `edge_id_default`.
          collection (UUID | str): The graph collection, by identifier or by name.
          durability (Durability, optional): `Durability.FLUSHED` reaches the drive before
            returning.
        """

    def graph_write_vertices(
        self, vertices: KeysLike, *, collection: CollectionLike, durability: Durability = Durability.BUFFERED
    ) -> Deferred[None]:
        """graph_write_vertices(vertices, *, collection, durability=Durability.BUFFERED)
        answers None.

        Inserts a batch of vertices without edges, keeping those already present.

        Args:
          vertices (Sequence[KeyLike] | buffer): The vertices to insert.
          collection (UUID | str): The graph collection, by identifier or name.
          durability (Durability, optional): `Durability.FLUSHED` reaches the drive before
            returning.
        """

    def graph_remove_vertices(
        self,
        vertices: KeysLike,
        *,
        role: Role = Role.ANY,
        collection: CollectionLike,
        ends: KeysLike | None = None,
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[None]:
        """graph_remove_vertices(vertices, *, role=Role.ANY, collection, ends=None,
                              durability=Durability.BUFFERED) answers None.

        Removes a batch of vertices together with every edge they take part in. With `ends`,
        every vertex starts a range removed up to its end, as `remove` takes them.

        Args:
          vertices (Sequence[KeyLike] | buffer): The vertices to remove, or the first vertex of
            every range.
          ends (Sequence[KeyLike] | buffer, optional): The vertex every range stops before, one
            per vertex.
          role (Role, optional): The role every vertex plays, which only a joining graph between
            two collections needs; `Role.ANY` otherwise.
          collection (UUID | str): The graph collection, by identifier or by name.
          durability (Durability, optional): `Durability.FLUSHED` reaches the drive before
            returning.
        """

    def commit(self, durability: Durability = Durability.BUFFERED) -> Deferred[int]:
        """commit(durability=Durability.BUFFERED) answers the commit stamp.

        Applies every staged change, or none of them. The contents are kept whatever the
        outcome, so a rejected transaction can be inspected or replayed.

        Args:
          durability (Durability, optional): `Durability.FLUSHED` reaches the drive before
            returning.
        Returns:
          int: The commit stamp, this commit's position in commit order, which every snapshot
          whose read stamp is at or above it sees.
        Raises:
          ConflictError: A concurrent writer touched what this transaction watched.
        """

    def stage(self) -> Deferred[int]:
        """stage() answers the read stamp.

        Runs the first phase of a two-phase commit, after which a commit publishes the
        work. Staging reserves no position in commit order.

        Returns:
          int: The read stamp, the stamp the transaction reads at and its commit validates
          against, which is no greater than the commit stamp the commit then answers.
        """

# endregion: Handles
