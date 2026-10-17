"""Type stubs for the `ustore.lib` extension.

Stub files are never executed, so PEP 688's `Buffer` is read by any checker regardless of the
interpreter running the tests. The package floor is 3.12.
"""

from collections.abc import Callable, Generator, Iterator, Sequence
from enum import IntEnum
from os import PathLike
from types import GenericAlias, TracebackType
from typing import Any, Protocol, Self, final, overload, type_check_only
from uuid import UUID

from _typeshed import structseq

from ustore.graph_view import DiGraph
from ustore.table_view import Table

@type_check_only
class _ArrowArray(Protocol):
    """Anything exporting an Arrow array through the C data interface, such as a `pyarrow.Array` or `Values`."""

    def __arrow_c_array__(self, requested_schema: object | None = None) -> tuple[object, object]: ...

type Buffer = memoryview | bytes | bytearray
type CollectionLike = UUID | str | None
type KeyLike = UUID | int | str | bytes
type KeysLike = Sequence[KeyLike] | Buffer
type ValueLike = Buffer
type TapeLike = _ArrowArray | tuple[Buffer, Buffer]
"""An Arrow `binary`, `large_binary`, `string` or `large_string` array, or a `(data, offsets)` pair of buffers whose
offsets are unsigned 32-bit or 64-bit integers, one more than the values, which a write passes without a copy."""
type JSONValue = dict[str, JSONValue] | list[JSONValue] | tuple[JSONValue, ...] | str | int | float | bool | None
type DocumentLike = str | Buffer | dict[str, JSONValue] | list[JSONValue] | int | float | bool
type PathLikeOrStr = str | bytes | PathLike[str]
type DataDirectory = PathLikeOrStr | tuple[PathLikeOrStr, int]
type Progress = Callable[[int, int], bool | None]
"""Called with the units a call finished and the units it finishes in all, from whichever thread runs it but never two
at once; None or a truthy answer carries on, any other falsy one cancels, and a raise cancels and re-raises."""

__version__: str
edge_id_default: UUID

def version() -> str:
    """The version of the library this module runs against, like "1.0.0". Calls `ustore_version_name`."""

def engines_compiled() -> tuple[str, ...]:
    """The engines this build carries: some of "ram", "rocksdb", "leveldb" and "nvme", in that order."""

# region: Statuses and Modes

class Status(IntEnum):
    """What went wrong, valued as `ustore_status_t`: `Error.status` and `Deferred.status`."""

    SUCCESS = 0
    DEFERRED = 1
    OUT_OF_MEMORY = 2
    INVALID_ARGUMENT = 3
    UNINITIALIZED = 4
    UNSUPPORTED = 5
    IO = 6
    MALFORMED = 7
    NOT_FOUND = 8
    ALREADY_EXISTS = 9
    WRITE_CONFLICT = 10
    READ_CONFLICT = 11
    PHANTOM_CONFLICT = 12
    CAPACITY_EXHAUSTED = 13
    DEVICE = 14
    INTERNAL = 15
    CONTENDED = 16
    CANCELLED = 17
    READ_ONLY = 18

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

class Modality(IntEnum):
    """What a collection holds, valued as `ustore_modality_t`."""

    ANY = 0
    BLOBS = 1
    DOCS = 2
    GRAPH = 3
    STRINGS = 4
    PAGES = 5

class Isolation(IntEnum):
    """How the transactions of the in-memory engine see each other's writes, valued as `ustore_isolation_t`."""

    STRICT_SERIALIZABLE = 0
    SNAPSHOT = 1

class Transport(IntEnum):
    """How the NVMe engine moves bytes between its files and memory, valued as `ustore_transport_t`."""

    AUTOMATIC = 0
    BLOCKING = 1
    LINUX_URING = 2
    LINUX_AIO = 3
    LINUX_SPDK = 4

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
    BIN = 50
    STR = 51

class Modification(IntEnum):
    """What a document write applies, valued as `ustore_docs_modification_t`."""

    UPSERT = 0
    UPDATE = 1
    INSERT = 2
    PATCH = 3
    MERGE = 4

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
    """A collection name or a document the call creates is already taken."""

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
        vram: Executor | None = None,
        deferred_limit: int = 0,
        shared_io_channels: int = 0,
        queue_depth: int = 0,
        request_buffer_bytes: int = 0,
        request_buffers_count: int = 0,
        stream_buffer_bytes: int = 0,
        stream_buffers_count: int = 0,
    ) -> Self:
        """Threads each driving an I/O ring for the NVMe engine, telling every database `Database.open_nvme` opens on
        it how to move bytes, where every zero size keeps the engine's default, and compacting on the devices of
        `vram`, which it keeps until it closes. Calls `ustore_executor_for_nvme`."""

    @classmethod
    def for_vram(cls, devices: Sequence[int] | None = None, *, scratch_limit: int = 0, deferred_limit: int = 0) -> Self:
        """Host threads driving the CUDA `devices`, every visible one for None, which an NVMe executor compacts on,
        refused with `UnsupportedError` in a build without CUDA. Calls `ustore_executor_for_vram`."""

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
class Capabilities(structseq[bool], tuple[bool, bool, bool]):
    """What an engine supports, as `Database.capabilities` reports it."""

    @property
    def transactions(self) -> bool:
        """Whether the engine groups operations into transactions."""

    @property
    def collections(self) -> bool:
        """Whether the engine holds collections beyond the main one."""

    @property
    def snapshots(self) -> bool:
        """Whether the engine pins point-in-time states."""

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
        """Keys across every collection, the main one included."""

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
        """The collection `name` addresses, where the empty name is the main collection."""

    def read(self, keys: KeysLike, arena: Arena, /, *, collection: CollectionLike = None) -> Values: ...
    def get(self, key: KeyLike, *, collection: CollectionLike = None) -> bytes | None:
        """Copies one value out of a one-off arena, which `read` with an arena of yours avoids."""

    def contains(self, key: KeyLike, *, collection: CollectionLike = None) -> bool:
        """Reads one presence into a one-off arena."""

    def cursor(self, start: KeyLike = 0, /, *, collection: CollectionLike = None) -> Cursor:
        """A walk over the keys of one collection from `start`, reading nothing until its first pull, whose every pull
        blocks as this receiver's calls do. Calls `ustore_blobs_cursor_init`."""

    def scan(
        self,
        start: KeyLike,
        wanted: int,
        arena: Arena,
        /,
        *,
        collection: CollectionLike = None,
        mode: ScanMode = ScanMode.ORDERED,
        progress: Progress | None = None,
    ) -> Keys: ...
    def scan_batch(
        self,
        starts: KeysLike,
        wanted: int,
        arena: Arena,
        /,
        *,
        collection: CollectionLike = None,
        mode: ScanMode = ScanMode.ORDERED,
        progress: Progress | None = None,
    ) -> Keys:
        """One scan per start, reporting the scans finished to `progress`."""
    def sample(self, wanted: int, arena: Arena, /, *, collection: CollectionLike = None) -> Keys: ...
    def sample_batch(self, count: int, wanted: int, arena: Arena, /, *, collection: CollectionLike = None) -> Keys:
        """`count` samples of up to `wanted` keys each, all drawn from the one collection named."""
    def measure(
        self, start: KeyLike, end: KeyLike, arena: Arena, /, *, collection: CollectionLike = None
    ) -> Measurement: ...
    def docs_read(
        self,
        keys: KeysLike,
        arena: Arena,
        /,
        *,
        collection: CollectionLike = None,
        field: str | None = None,
        outputs_type: FieldType = FieldType.JSON,
    ) -> Values: ...
    def docs_gist(self, keys: KeysLike, arena: Arena, /, *, collection: CollectionLike = None) -> tuple[str, ...]: ...
    def docs_gather(
        self,
        keys: KeysLike,
        fields: Sequence[str],
        outputs_types: FieldType | Sequence[FieldType],
        arena: Arena,
        /,
        *,
        collection: CollectionLike = None,
    ) -> Columns: ...
    def graph_read_edges(
        self, vertices: KeysLike, arena: Arena, /, *, role: Role = Role.ANY, collection: CollectionLike = None
    ) -> Neighborhoods: ...
    def graph_degrees(
        self, vertices: KeysLike, arena: Arena, /, *, role: Role = Role.ANY, collection: CollectionLike = None
    ) -> tuple[int | None, ...]: ...

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
        collection: CollectionLike = None,
        durability: Durability = Durability.BUFFERED,
    ) -> None: ...
    def set(
        self,
        key: KeyLike,
        value: ValueLike,
        *,
        collection: CollectionLike = None,
        durability: Durability = Durability.BUFFERED,
    ) -> None: ...
    def remove(
        self,
        keys: KeyLike | KeysLike,
        *,
        collection: CollectionLike = None,
        durability: Durability = Durability.BUFFERED,
    ) -> None:
        """Removes keys with their values or documents."""
    def docs_write(
        self,
        keys: KeysLike | None,
        documents: Sequence[DocumentLike] | DocumentLike | TapeLike,
        *,
        collection: CollectionLike = None,
        field: str | None = None,
        modification: Modification = Modification.UPSERT,
        type: FieldType = FieldType.JSON,
        id_field: str | None = None,
        durability: Durability = Durability.BUFFERED,
    ) -> None: ...
    def graph_write_edges(
        self,
        sources: KeysLike,
        targets: KeysLike,
        *,
        edges: KeysLike | None = None,
        collection: CollectionLike = None,
        durability: Durability = Durability.BUFFERED,
    ) -> None: ...
    def graph_remove_edges(
        self,
        sources: KeysLike,
        targets: KeysLike,
        *,
        edges: KeysLike | None = None,
        collection: CollectionLike = None,
        durability: Durability = Durability.BUFFERED,
    ) -> None: ...
    def graph_write_vertices(
        self, vertices: KeysLike, *, collection: CollectionLike = None, durability: Durability = Durability.BUFFERED
    ) -> None: ...
    def graph_remove_vertices(
        self,
        vertices: KeysLike,
        *,
        role: Role = Role.ANY,
        collection: CollectionLike = None,
        durability: Durability = Durability.BUFFERED,
    ) -> None: ...

class Database(_Writes, _Receivers):
    """An embedded database, opened through one class method per engine."""

    capabilities: Capabilities

    @classmethod
    def open_ram(
        cls,
        directory: PathLikeOrStr | None = None,
        *,
        isolation: Isolation = Isolation.STRICT_SERIALIZABLE,
        executor: Executor | None = None,
    ) -> Self:
        """An in-memory database, persisted as one image when it names a directory, opened blocking on `executor`."""

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
        directory: PathLikeOrStr,
        *,
        executor: Executor | None = None,
        data_directories: Sequence[DataDirectory] = (),
        memory_limit: int = 0,
        transaction_max_bytes: int = 0,
        transaction_max_elements: int = 0,
        collection_concurrency_limit: int = 0,
        collection_cache_limit: int = 0,
        collection_write_buffer_max_bytes: int = 0,
        collection_write_buffer_max_elements: int = 0,
        collection_first_level_max_bytes: int = 0,
        collection_level_enlarge_factor: int = 0,
        collection_value_size: int = 0,
        collection_value_max_size: int = 0,
    ) -> Self:
        """A database on the NVMe engine, where every zero size keeps the engine's default, opened blocking on
        `executor`, which tells it how to move bytes when `Executor.for_nvme` made it."""
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
        """Empties every collection at once: its pairs, every collection but the main one, or its values."""

    def compact(self, *, progress: Progress | None = None) -> None:
        """Reclaims the space overwritten and removed data takes, one collection at a time, reporting bytes where the
        engine measures them and collections elsewhere."""

    def describe(self) -> Description:
        """The engine, its capabilities and isolation, and the keys, memory and disk it holds."""

    def collection_create(self, name: str, *, modality: Modality = Modality.ANY) -> UUID:
        """Creates an empty collection and records the name for it."""

    def collection_find(self, name: str, arena: Arena, /) -> UUID:
        """The collection ID the name addresses."""

    def collection_contains(self, name: str) -> bool:
        """Whether a collection carries the name, read into a one-off arena."""

    def collection_list(self, arena: Arena, /) -> tuple[UUID, ...]:
        """The IDs of every collection but the main one."""

    def collection_names(self, arena: Arena, /, *, after: str | None = None, limit: int = 1024) -> tuple[str, ...]:
        """One page of the names recorded for collections, in the order of their text."""

    def collection_drop(self, collection: UUID | str, mode: DropMode = DropMode.COLLECTION) -> None:
        """Removes a collection, or empties its pairs or its values."""

    def snapshot(self) -> Snapshot:
        """Pins a point-in-time state of the whole database, which the snapshot's read verbs see."""

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
        """The IDs of every collection but the main one at this snapshot."""

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
        """The IDs of every collection but the main one, at the snapshot of a snapshot and at the head otherwise."""

class DeferredView:
    """What a database, transaction or snapshot addresses, with the executor its calls go to, each verb answering a
    `Deferred` at once. Every pull of a cursor blocks, so it opens none."""

    database: Database
    executor: Executor | None
    """The executor the view's calls go to, where an arena for them belongs."""

    def __getitem__(self, name: str) -> Collection:
        """The collection `name` addresses, whose `blobs[key]` and `docs[key]` answer a `Deferred`."""

    def on(self, executor: Executor) -> View: ...
    def deferred(self, executor: Executor) -> DeferredView: ...
    def watched(self) -> DeferredView: ...
    def unwatched(self) -> DeferredView: ...
    def read(self, keys: KeysLike, arena: Arena, /, *, collection: CollectionLike = None) -> Deferred[Values]: ...
    def get(self, key: KeyLike, *, collection: CollectionLike = None) -> Deferred[bytes | None]:
        """The `Deferred` of a copy of one value, owning the one-off arena it reads into."""

    def contains(self, key: KeyLike, *, collection: CollectionLike = None) -> Deferred[bool]:
        """The `Deferred` of one presence, owning the one-off arena it reads into."""

    def scan(
        self,
        start: KeyLike,
        wanted: int,
        arena: Arena,
        /,
        *,
        collection: CollectionLike = None,
        mode: ScanMode = ScanMode.ORDERED,
        progress: Progress | None = None,
    ) -> Deferred[Keys]: ...
    def scan_batch(
        self,
        starts: KeysLike,
        wanted: int,
        arena: Arena,
        /,
        *,
        collection: CollectionLike = None,
        mode: ScanMode = ScanMode.ORDERED,
        progress: Progress | None = None,
    ) -> Deferred[Keys]:
        """The `Deferred` of one scan per start, whose `progress` runs on the executor's threads."""
    def sample(self, wanted: int, arena: Arena, /, *, collection: CollectionLike = None) -> Deferred[Keys]: ...
    def sample_batch(
        self, count: int, wanted: int, arena: Arena, /, *, collection: CollectionLike = None
    ) -> Deferred[Keys]: ...
    def measure(
        self, start: KeyLike, end: KeyLike, arena: Arena, /, *, collection: CollectionLike = None
    ) -> Deferred[Measurement]: ...
    def docs_read(
        self,
        keys: KeysLike,
        arena: Arena,
        /,
        *,
        collection: CollectionLike = None,
        field: str | None = None,
        outputs_type: FieldType = FieldType.JSON,
    ) -> Deferred[Values]: ...
    def docs_gist(
        self, keys: KeysLike, arena: Arena, /, *, collection: CollectionLike = None
    ) -> Deferred[tuple[str, ...]]: ...
    def docs_gather(
        self,
        keys: KeysLike,
        fields: Sequence[str],
        outputs_types: FieldType | Sequence[FieldType],
        arena: Arena,
        /,
        *,
        collection: CollectionLike = None,
    ) -> Deferred[Columns]: ...
    def graph_read_edges(
        self, vertices: KeysLike, arena: Arena, /, *, role: Role = Role.ANY, collection: CollectionLike = None
    ) -> Deferred[Neighborhoods]: ...
    def graph_degrees(
        self, vertices: KeysLike, arena: Arena, /, *, role: Role = Role.ANY, collection: CollectionLike = None
    ) -> Deferred[tuple[int | None, ...]]: ...
    def collection_find(self, name: str, arena: Arena, /) -> Deferred[UUID]: ...
    def collection_list(self, arena: Arena, /) -> Deferred[tuple[UUID, ...]]: ...
    def save(self, path: PathLikeOrStr, /, *, progress: Progress | None = None) -> Deferred[None]: ...
    def clear(self, mode: DropMode, /) -> Deferred[None]: ...
    def compact(self, *, progress: Progress | None = None) -> Deferred[None]: ...
    def describe(self) -> Deferred[Description]: ...
    def write(
        self,
        keys: KeysLike,
        values: Sequence[ValueLike] | ValueLike | TapeLike,
        *,
        collection: CollectionLike = None,
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[None]: ...
    def set(
        self,
        key: KeyLike,
        value: ValueLike,
        *,
        collection: CollectionLike = None,
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[None]: ...
    def remove(
        self,
        keys: KeyLike | KeysLike,
        *,
        collection: CollectionLike = None,
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[None]: ...
    def docs_write(
        self,
        keys: KeysLike | None,
        documents: Sequence[DocumentLike] | DocumentLike | TapeLike,
        *,
        collection: CollectionLike = None,
        field: str | None = None,
        modification: Modification = Modification.UPSERT,
        type: FieldType = FieldType.JSON,
        id_field: str | None = None,
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[None]: ...
    def graph_write_edges(
        self,
        sources: KeysLike,
        targets: KeysLike,
        *,
        edges: KeysLike | None = None,
        collection: CollectionLike = None,
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[None]: ...
    def graph_remove_edges(
        self,
        sources: KeysLike,
        targets: KeysLike,
        *,
        edges: KeysLike | None = None,
        collection: CollectionLike = None,
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[None]: ...
    def graph_write_vertices(
        self, vertices: KeysLike, *, collection: CollectionLike = None, durability: Durability = Durability.BUFFERED
    ) -> Deferred[None]: ...
    def graph_remove_vertices(
        self,
        vertices: KeysLike,
        *,
        role: Role = Role.ANY,
        collection: CollectionLike = None,
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[None]: ...
    def commit(self, durability: Durability = Durability.BUFFERED) -> Deferred[int]: ...
    def stage(self) -> Deferred[int]: ...

# endregion: Handles
