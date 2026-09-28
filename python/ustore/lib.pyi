"""Type stubs for the `ustore.lib` extension.

Stub files are never executed, so PEP 688's `Buffer` is read by any checker regardless of the
interpreter running the tests. The package floor is 3.12.
"""

from collections.abc import Sequence
from os import PathLike
from types import TracebackType
from typing import Literal, Self

type Buffer = memoryview | bytes | bytearray
type CollectionLike = int | str | None
type KeysLike = Sequence[int] | Buffer
type ValueLike = Buffer | None
type DocumentLike = str | Buffer | None
type FieldType = Literal[
    "json", "str", "bytes", "null", "bool", "f64", "f32", "i64", "i32", "i16", "i8", "u64", "u32", "u16", "u8"
]
type GatherType = Literal[
    "bool", "uuid", "f64", "f32", "f16", "i64", "i32", "i16", "i8", "u64", "u32", "u16", "u8", "str", "bytes"
]
type Modification = Literal["upsert", "update", "insert", "patch", "merge"]
type Role = Literal["source", "target", "any"]
type PathLikeOrStr = str | bytes | PathLike[str]
type DataDirectory = PathLikeOrStr | tuple[PathLikeOrStr, int]
type Isolation = Literal["strict-serializable", "snapshot"]
type Transport = Literal["automatic", "blocking", "linux-uring", "linux-aio", "linux-spdk"]
type Modality = Literal["any", "blobs", "docs", "graph", "strings", "pages"]

__version__: str
default_edge_id: int

def engines_compiled() -> tuple[str, ...]:
    """The engines this build carries: some of "ram", "rocksdb", "leveldb" and "nvme", in that order."""

# region: Failures

class Error(RuntimeError):
    """Every failure the engine reports, carrying the status shared with the C ABI."""

    status: str

class ArgumentError(Error):
    """An argument is null, contradicts another one, or names a handle that was never opened."""

class UnsupportedError(Error):
    """The engine or this build does not implement what the call asks for."""

class MissingError(Error):
    """A snapshot, collection or name the call addresses does not exist."""

class ExistsError(Error):
    """A snapshot or collection the call creates already exists."""

class ConflictError(Error):
    """A concurrent writer touched what this transaction read, wrote or scanned."""

class MalformedError(Error):
    """Stored or supplied data could not be decoded."""

class CapacityError(Error):
    """An allocation could not be satisfied, or a design ceiling was reached."""

class StorageError(Error):
    """A file, directory or device could not be reached, or the engine failed outright."""

# endregion: Failures

# region: Result Tapes

class Values:
    """The values one batch read exported, read straight out of the arena holding them."""

    lengths: tuple[int | None, ...]

    def __len__(self) -> int: ...
    def __getitem__(self, task: int) -> memoryview | None: ...
    def __buffer__(self, flags: int) -> memoryview: ...

class Keys:
    """The keys one batch scan or sample found, read straight out of the arena holding them."""

    counts: tuple[int, ...]

    def __len__(self) -> int: ...
    def __getitem__(self, task: int) -> memoryview: ...
    def __buffer__(self, flags: int) -> memoryview: ...

class Columns:
    """The typed columns one gather exported, read straight out of the arena holding them."""

    types: tuple[str, ...]
    rows: int

    def __len__(self) -> int: ...
    def __getitem__(self, field: int) -> memoryview | tuple[str | bytes | None, ...]: ...
    def validity(self, field: int) -> memoryview:
        """The bitmap of documents whose value was found and converted."""

    def conversion(self, field: int) -> memoryview:
        """The bitmap of values whose conversion changed their meaning."""

    def collision(self, field: int) -> memoryview:
        """The bitmap of values of a type no conversion reaches."""

class Edges:
    """The edges one batch lookup found, read straight out of the arena holding them."""

    degrees: tuple[int | None, ...]

    def __len__(self) -> int: ...
    def __getitem__(self, vertex: int) -> memoryview | None: ...
    def __buffer__(self, flags: int) -> memoryview: ...

# endregion: Result Tapes

# region: Handles

class Database:
    """An embedded database, opened through one class method per engine."""

    supports_transactions: bool
    supports_collections: bool
    supports_snapshots: bool

    @classmethod
    def open_ram(cls, directory: PathLikeOrStr | None = None, *, isolation: Isolation = "strict-serializable") -> Self:
        """An in-memory database, persisted as one image when it names a directory."""

    @classmethod
    def open_rocksdb(cls, directory: PathLikeOrStr, *, data_directories: Sequence[DataDirectory] = ()) -> Self:
        """A database on RocksDB, whose table files may spread over data directories."""

    @classmethod
    def open_leveldb(cls, directory: PathLikeOrStr) -> Self:
        """A database on LevelDB."""

    @classmethod
    def open_nvme(
        cls,
        directory: PathLikeOrStr,
        *,
        data_directories: Sequence[DataDirectory] = (),
        transport: Transport = "automatic",
        gpu_devices: Sequence[int] = (),
        memory_limit: int = 0,
        threads: int = 0,
        gpu_memory_limit: int = 0,
        shared_io_channels: int = 0,
        queue_depth: int = 0,
        request_buffer_bytes: int = 0,
        request_buffers_count: int = 0,
        stream_buffer_bytes: int = 0,
        stream_buffers_count: int = 0,
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
        """A database on the NVMe engine, where every zero size keeps the engine's default."""
    def __enter__(self) -> Self: ...
    def __exit__(
        self,
        kind: type[BaseException] | None = None,
        value: BaseException | None = None,
        traceback: TracebackType | None = None,
    ) -> None: ...
    def close(self) -> None:
        """Closes the database; every transaction it opened must be closed first."""

    def control(self, request: str) -> str:
        """Sends a free-form request to the engine, such as 'clear', 'compact' or 'usage'."""

    def collection_create(self, name: str, *, modality: Modality = "any") -> int:
        """Creates an empty collection and records the name for it."""

    def collection_find(self, name: str) -> int:
        """The identifier the name addresses."""

    def collection_contains(self, name: str) -> bool:
        """Whether a collection carries the name."""

    def collection_list(self) -> tuple[int, ...]:
        """The identifiers of every collection but the main one."""

    def collection_names(self, *, after: str | None = None, limit: int = 1024) -> tuple[str, ...]:
        """One page of the names recorded for collections, in the order of their text."""

    def collection_drop(self, collection: int | str, *, mode: str = "collection") -> None:
        """Removes a collection, or empties its pairs or its values."""

    def snapshot_create(self) -> int:
        """Captures a point-in-time view of the whole database."""

    def snapshot_list(self) -> tuple[int, ...]:
        """The identifiers of every snapshot."""

    def snapshot_drop(self, snapshot: int) -> None:
        """Releases a snapshot."""

    def snapshot_export(self, snapshot: int, directory: str | bytes | PathLike[str]) -> None:
        """Writes a snapshot out as a new database."""

    def transaction(self, *, watch: bool = True) -> Transaction:
        """Begins a transaction over this database."""

    def read(self, keys: KeysLike, *, collection: CollectionLike = None, snapshot: int = 0) -> Values: ...
    def write(
        self,
        keys: KeysLike,
        values: Sequence[ValueLike] | ValueLike,
        *,
        collection: CollectionLike = None,
        flush: bool = False,
    ) -> None: ...
    def scan(self, start: int, count: int, *, collection: CollectionLike = None, snapshot: int = 0) -> Keys: ...
    def scan_batch(
        self, starts: KeysLike, counts: Sequence[int], *, collection: CollectionLike = None, snapshot: int = 0
    ) -> Keys: ...
    def sample(self, count: int, *, collection: CollectionLike = None, snapshot: int = 0) -> Keys: ...
    def sample_batch(self, counts: Sequence[int], *, collection: CollectionLike = None, snapshot: int = 0) -> Keys: ...
    def measure(
        self, start: int, end: int, *, collection: CollectionLike = None, snapshot: int = 0
    ) -> dict[str, int]: ...
    def get(self, key: int, *, collection: CollectionLike = None, snapshot: int = 0) -> bytes | None: ...
    def set(self, key: int, value: ValueLike, *, collection: CollectionLike = None, flush: bool = False) -> None: ...
    def pop(self, keys: int | KeysLike, *, collection: CollectionLike = None, flush: bool = False) -> None: ...
    def contains(self, key: int, *, collection: CollectionLike = None, snapshot: int = 0) -> bool: ...
    def docs_write(
        self,
        keys: KeysLike | None,
        documents: Sequence[DocumentLike] | DocumentLike,
        *,
        collection: CollectionLike = None,
        field: str | None = None,
        modification: Modification = "upsert",
        type: FieldType = "json",
        id_field: str | None = None,
        flush: bool = False,
    ) -> None: ...
    def docs_read(
        self,
        keys: KeysLike,
        *,
        collection: CollectionLike = None,
        snapshot: int = 0,
        field: str | None = None,
        type: Literal["json", "str"] = "json",
    ) -> Values: ...
    def docs_gist(self, keys: KeysLike, *, collection: CollectionLike = None, snapshot: int = 0) -> tuple[str, ...]: ...
    def docs_gather(
        self,
        keys: KeysLike,
        fields: Sequence[str],
        types: GatherType | Sequence[GatherType],
        *,
        collection: CollectionLike = None,
        snapshot: int = 0,
    ) -> Columns: ...
    def graph_upsert_edges(
        self,
        sources: KeysLike,
        targets: KeysLike,
        *,
        edges: KeysLike | None = None,
        collection: CollectionLike = None,
        flush: bool = False,
    ) -> None: ...
    def graph_remove_edges(
        self,
        sources: KeysLike,
        targets: KeysLike,
        *,
        edges: KeysLike | None = None,
        collection: CollectionLike = None,
        flush: bool = False,
    ) -> None: ...
    def graph_find_edges(
        self, vertices: KeysLike, *, role: Role = "any", collection: CollectionLike = None, snapshot: int = 0
    ) -> Edges: ...
    def graph_degrees(
        self, vertices: KeysLike, *, role: Role = "any", collection: CollectionLike = None, snapshot: int = 0
    ) -> tuple[int | None, ...]: ...
    def graph_upsert_vertices(
        self, vertices: KeysLike, *, collection: CollectionLike = None, flush: bool = False
    ) -> None: ...
    def graph_remove_vertices(
        self, vertices: KeysLike, *, role: Role = "any", collection: CollectionLike = None, flush: bool = False
    ) -> None: ...

class Transaction:
    """An optimistic transaction grouping operations across collections and modalities."""

    database: Database

    def __enter__(self) -> Self: ...
    def __exit__(
        self,
        kind: type[BaseException] | None = None,
        value: BaseException | None = None,
        traceback: TracebackType | None = None,
    ) -> None: ...
    def commit(self, *, flush: bool = False) -> None:
        """Applies every staged change, or none of them."""

    def stage(self) -> int:
        """Runs the first phase of a two-phase commit."""

    def rollback(self) -> None:
        """Drops every staged change and starts a new generation on the same handle."""

    def close(self) -> None:
        """Drops whatever is still staged and releases the handle."""

    def read(self, keys: KeysLike, *, collection: CollectionLike = None, watch: bool = True) -> Values: ...
    def write(
        self,
        keys: KeysLike,
        values: Sequence[ValueLike] | ValueLike,
        *,
        collection: CollectionLike = None,
        flush: bool = False,
        watch: bool = True,
    ) -> None: ...
    def scan(self, start: int, count: int, *, collection: CollectionLike = None, watch: bool = True) -> Keys: ...
    def scan_batch(
        self, starts: KeysLike, counts: Sequence[int], *, collection: CollectionLike = None, watch: bool = True
    ) -> Keys: ...
    def sample(self, count: int, *, collection: CollectionLike = None, watch: bool = True) -> Keys: ...
    def sample_batch(self, counts: Sequence[int], *, collection: CollectionLike = None, watch: bool = True) -> Keys: ...
    def measure(
        self, start: int, end: int, *, collection: CollectionLike = None, watch: bool = True
    ) -> dict[str, int]: ...
    def get(self, key: int, *, collection: CollectionLike = None, watch: bool = True) -> bytes | None: ...
    def set(
        self, key: int, value: ValueLike, *, collection: CollectionLike = None, flush: bool = False, watch: bool = True
    ) -> None: ...
    def pop(
        self, keys: int | KeysLike, *, collection: CollectionLike = None, flush: bool = False, watch: bool = True
    ) -> None: ...
    def contains(self, key: int, *, collection: CollectionLike = None, watch: bool = True) -> bool: ...
    def docs_write(
        self,
        keys: KeysLike | None,
        documents: Sequence[DocumentLike] | DocumentLike,
        *,
        collection: CollectionLike = None,
        field: str | None = None,
        modification: Modification = "upsert",
        type: FieldType = "json",
        id_field: str | None = None,
        flush: bool = False,
        watch: bool = True,
    ) -> None: ...
    def docs_read(
        self,
        keys: KeysLike,
        *,
        collection: CollectionLike = None,
        field: str | None = None,
        type: Literal["json", "str"] = "json",
        watch: bool = True,
    ) -> Values: ...
    def docs_gist(
        self, keys: KeysLike, *, collection: CollectionLike = None, watch: bool = True
    ) -> tuple[str, ...]: ...
    def docs_gather(
        self,
        keys: KeysLike,
        fields: Sequence[str],
        types: GatherType | Sequence[GatherType],
        *,
        collection: CollectionLike = None,
        watch: bool = True,
    ) -> Columns: ...
    def graph_upsert_edges(
        self,
        sources: KeysLike,
        targets: KeysLike,
        *,
        edges: KeysLike | None = None,
        collection: CollectionLike = None,
        flush: bool = False,
        watch: bool = True,
    ) -> None: ...
    def graph_remove_edges(
        self,
        sources: KeysLike,
        targets: KeysLike,
        *,
        edges: KeysLike | None = None,
        collection: CollectionLike = None,
        flush: bool = False,
        watch: bool = True,
    ) -> None: ...
    def graph_find_edges(
        self, vertices: KeysLike, *, role: Role = "any", collection: CollectionLike = None, watch: bool = True
    ) -> Edges: ...
    def graph_degrees(
        self, vertices: KeysLike, *, role: Role = "any", collection: CollectionLike = None, watch: bool = True
    ) -> tuple[int | None, ...]: ...
    def graph_upsert_vertices(
        self, vertices: KeysLike, *, collection: CollectionLike = None, flush: bool = False, watch: bool = True
    ) -> None: ...
    def graph_remove_vertices(
        self,
        vertices: KeysLike,
        *,
        role: Role = "any",
        collection: CollectionLike = None,
        flush: bool = False,
        watch: bool = True,
    ) -> None: ...

# endregion: Handles
