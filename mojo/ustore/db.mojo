"""Databases, snapshots, transactions, their receivers, collections, change feeds and ingests.

A `Database` owns its engine and is shared by every executor. The receiver picks where a verb runs
and how it completes: verbs on a database, transaction or snapshot run on the calling thread,
`handle.on(executor)` blocks on an executor, and `handle.deferred(executor)` submits there and
answers a `Deferred` at once. Each verb is one call all three route, written once in the verb
traits of this module and of the modality modules.

Reads see what their receiver sees: the head of a database, a transaction, or a snapshot. Writes
join the receiver's transaction, or land at the head when it has none, as a snapshot's receiver
has none.

File: mojo/ustore/db.mojo
Author: Ash Vardanian
Date: November 8, 2026
"""

from std.ffi import c_int, c_size_t
from std.memory import stack_allocation
from std.sys.info import size_of
from std.utils import StaticTuple

from ustore.blobs import BlobsDeferredVerbs, BlobsVerbs
from ustore.docs import DocsDeferredVerbs, DocsVerbs
from ustore.graph import GraphDeferredVerbs, GraphVerbs
from ustore.library import Library
from ustore.pages import PagesDeferredVerbs, PagesVerbs
from ustore.strings import StringsDeferredVerbs, StringsVerbs
from ustore.types import (
    Address,
    Answer,
    Arena,
    Arguments,
    BytesView,
    BytesViewArguments,
    Completion,
    CPUClass,
    Deferred,
    Error,
    Executor,
    NULL_ADDRESS,
    Outcome,
    Progress,
    StringsView,
    StringsViewArguments,
    Tape,
    Unwritten,
    Verb,
    _address_of,
    _anchor,
    _borrowed,
    _bit,
    _block,
    _created,
    _cross,
    _Crossing,
    _in_arena,
    _readable,
    _slot,
)

# region Identifiers


@fieldwise_init
struct UUID(Equatable, ImplicitlyCopyable, Writable):
    """Sixteen bytes naming one thing everywhere, ordered as their bytes, `ustore_uuid_t`."""

    var bytes: StaticTuple[UInt8, 16]
    """The bytes, an integer spelled big-endian."""

    def __init__(out self):
        """The nil UUID."""
        self.bytes = StaticTuple[UInt8, 16](fill=0)

    def __init__(out self, value: UInt64):
        """The UUID spelling an integer as its 128-bit big-endian bytes, so such UUIDs order as integers."""
        self.bytes = StaticTuple[UInt8, 16](fill=0)
        for position in range(8):
            self.bytes[15 - position] = UInt8((value >> UInt64(8 * position)) & 0xFF)

    def __eq__(self, other: Self) -> Bool:
        """Whether every byte matches."""
        for position in range(16):
            if self.bytes[position] != other.bytes[position]:
                return False
        return True

    def __ne__(self, other: Self) -> Bool:
        """Whether any byte differs."""
        return not self == other

    def write_to(self, mut writer: Some[Writer]):
        """Writes the canonical hyphenated lowercase hex spelling."""
        comptime digits = "0123456789abcdef"
        for position in range(16):
            if position == 4 or position == 6 or position == 8 or position == 10:
                writer.write("-")
            var byte = Int(self.bytes[position])
            writer.write(digits[byte=byte >> 4], digits[byte=byte & 15])


@fieldwise_init
struct Key(Equatable, ImplicitlyCopyable, Writable):
    """The identifier of a value within one collection, `ustore_key_t`."""

    var uuid: UUID
    """The sixteen bytes."""

    def __init__(out self):
        """The nil key."""
        self.uuid = UUID()

    def __init__(out self, value: UInt64):
        """The key spelling an integer, so such keys order as integers."""
        self.uuid = UUID(value)

    def __eq__(self, other: Self) -> Bool:
        """Whether every byte matches."""
        return self.uuid == other.uuid

    def __ne__(self, other: Self) -> Bool:
        """Whether any byte differs."""
        return self.uuid != other.uuid

    def write_to(self, mut writer: Some[Writer]):
        """Writes the UUID spelling."""
        writer.write(self.uuid)


@fieldwise_init
struct CollectionID(Answer, Defaultable, Equatable, ImplicitlyCopyable, Writable):
    """The identifier of a collection, a UUIDv7 its create mints once, `ustore_collection_id_t`."""

    comptime Written = CollectionID
    var uuid: UUID
    """The sixteen bytes; nil names no collection."""

    def __init__(out self):
        """The nil identifier, which names no collection."""
        self.uuid = UUID()

    def __init__(out self, arena: Address, generation: Int, written: CollectionID, count: Int):
        """Adopts the identifier a create wrote."""
        self.uuid = written.uuid

    def __eq__(self, other: Self) -> Bool:
        """Whether every byte matches."""
        return self.uuid == other.uuid

    def __ne__(self, other: Self) -> Bool:
        """Whether any byte differs."""
        return self.uuid != other.uuid

    def write_to(self, mut writer: Some[Writer]):
        """Writes the UUID spelling."""
        writer.write(self.uuid)


@fieldwise_init
struct Stamp(Answer, Defaultable, Equatable, ImplicitlyCopyable):
    """A position in the order commits apply in, `ustore_stamp_t`: where a commit landed, or the newest
    commit a transaction or a snapshot reads."""

    comptime Written = Stamp
    var value: UInt64
    """The position, growing with every commit."""

    def __init__(out self):
        """Stamp zero, which a failed call leaves."""
        self.value = 0

    def __init__(out self, arena: Address, generation: Int, written: Stamp, count: Int):
        """Adopts the stamp a call wrote."""
        self.value = written.value


# endregion Identifiers

# region Views


@fieldwise_init
struct StridedArguments(Defaultable, ImplicitlyCopyable, TrivialRegisterPassable):
    """`ustore_keys_view_t`, `ustore_collections_view_t` and every typed strided view, which share it."""

    var data: Address
    var stride: c_size_t

    def __init__(out self):
        """A zeroed view, which stands for the input's default."""
        self = Self(NULL_ADDRESS, 0)


@fieldwise_init
struct StridedView[T: Copyable, origin: ImmOrigin = ImmUntrackedOrigin](ImplicitlyCopyable, Sized):
    """A borrowed strided run of fixed-width elements, one per task, where a zero stride repeats the first.

    Parameters:
        T: The element, laid out as the ABI declares it.
        origin: The borrowed elements, which the view keeps alive for as long as it is used.
    """

    var data: Optional[Pointer[Self.T, Self.origin]]
    """The first element; absent for a zeroed view."""
    var stride: Int
    """Bytes between consecutive elements; zero repeats the first."""
    var count: Int
    """Elements the view holds: one for a repeated element, zero for a zeroed view."""

    def __init__(out self):
        """A zeroed view, which stands for the input's default."""
        self = Self(None, 0, 0)

    def __len__(self) -> Int:
        """The elements the view holds."""
        return self.count

    def tasks(self) -> Int:
        """The tasks the view can serve: its elements, minus one for a repeated element, which fits any batch."""
        return -1 if self.stride == 0 and self.data else self.count

    @staticmethod
    def of[items_origin: ImmOrigin, //](items: Span[Self.T, items_origin]) -> StridedView[Self.T, items_origin]:
        """Borrows elements packed one after another."""
        return StridedView[Self.T, items_origin](
            _borrowed[Self.T, items_origin](Int(items.unsafe_ptr())), size_of[Self.T](), len(items)
        )

    @staticmethod
    def repeating[item_origin: ImmOrigin, //](ref[item_origin] item: Self.T) -> StridedView[Self.T, item_origin]:
        """Borrows one element, repeated for every task."""
        return StridedView[Self.T, item_origin](_borrowed[Self.T, item_origin](Int(Pointer(to=item))), 0, 1)

    def arguments(self) -> StridedArguments:
        """The C layout, erased only inside a verb so the borrow lives through the call."""
        return StridedArguments(_address_of(self.data), c_size_t(self.stride))


comptime KeysView[origin: ImmOrigin = ImmUntrackedOrigin] = StridedView[Key, origin]
"""A strided run of keys, `ustore_keys_view_t`."""

comptime CollectionsView[origin: ImmOrigin = ImmUntrackedOrigin] = StridedView[CollectionID, origin]
"""A strided run of collections, `ustore_collections_view_t`."""

comptime LengthsView[origin: ImmOrigin = ImmUntrackedOrigin] = StridedView[UInt64, origin]
"""A strided run of lengths or positions, `ustore_lengths_view_t`."""


def _tasks(a: Int, b: Int = 0, c: Int = 0, d: Int = 0, e: Int = 0, f: Int = 0) -> Int:
    """The tasks of a call from its views' `tasks()`: the shortest packed view sets them, so the library
    never reads past one, and a repeated element alone makes one task."""
    var shortest = Int.MAX
    var repeated = False
    for tasks in [a, b, c, d, e, f]:
        if tasks > 0:
            shortest = min(shortest, tasks)
        repeated = repeated or tasks < 0
    if shortest != Int.MAX:
        return shortest
    return 1 if repeated else 0


# endregion Views

# region Enumerations


@fieldwise_init
struct Durability(Equatable, ImplicitlyCopyable, TrivialRegisterPassable):
    """How durable the writes a call publishes are once it finishes, `ustore_durability_t`."""

    var code: c_int
    """The raw `ustore_durability_t` value the ABI exchanges."""
    comptime BUFFERED = Self(0)
    """Visible, and on disk as the engine's buffers fill."""
    comptime FLUSHED = Self(1)
    """On disk."""


@fieldwise_init
struct Watch(Equatable, ImplicitlyCopyable, TrivialRegisterPassable):
    """Whether a transaction records the keys a call touches for conflicts, `ustore_watch_t`."""

    var code: c_int
    """The raw `ustore_watch_t` value the ABI exchanges."""
    comptime KEYS = Self(0)
    """Records the keys, so a concurrent change to any of them fails the commit."""
    comptime NONE = Self(1)
    """Records nothing."""


@fieldwise_init
struct ScanMode(Equatable, ImplicitlyCopyable, TrivialRegisterPassable):
    """In what order a scan hands its keys over, `ustore_scan_mode_t`."""

    var code: c_int
    """The raw `ustore_scan_mode_t` value the ABI exchanges."""
    comptime ORDERED = Self(0)
    """In order, each once, and only while present."""
    comptime UNORDERED = Self(1)
    """Possibly out of order, deleted or repeated, with no merge across partitions."""


@fieldwise_init
struct ScanDirection(Equatable, ImplicitlyCopyable, TrivialRegisterPassable):
    """Which way a scan, a cursor or a measure walks from its start key, `ustore_scan_direction_t`."""

    var code: c_int
    """The raw `ustore_scan_direction_t` value the ABI exchanges."""
    comptime ASCENDING = Self(0)
    """Up from the start, smallest key first."""
    comptime DESCENDING = Self(1)
    """Down from the start, largest key first."""


@fieldwise_init
struct DropMode(Equatable, ImplicitlyCopyable, TrivialRegisterPassable):
    """What dropping a collection removes, `ustore_drop_mode_t`."""

    var code: c_int
    """The raw `ustore_drop_mode_t` value the ABI exchanges."""
    comptime COLLECTION = Self(0)
    """The collection with its name and every pair."""
    comptime PAIRS = Self(1)
    """Every key and value, keeping the collection."""
    comptime VALUES = Self(2)
    """Every value, keeping the keys."""


@fieldwise_init
struct Isolation(Equatable, ImplicitlyCopyable, TrivialRegisterPassable):
    """How the transactions of the in-memory engine see each other's writes, `ustore_isolation_t`."""

    var code: c_int
    """The raw `ustore_isolation_t` value the ABI exchanges."""
    comptime STRICT_SERIALIZABLE = Self(0)
    """Every transaction appears to run alone at one moment."""
    comptime SNAPSHOT = Self(1)
    """Every transaction reads the state its start saw and conflicts only over what it writes."""


@fieldwise_init
struct Engine(Equatable, ImplicitlyCopyable, TrivialRegisterPassable):
    """The engine serving a database, `ustore_engine_t`."""

    var code: c_int
    """The raw `ustore_engine_t` value the ABI exchanges."""
    comptime RAM = Self(0)
    """The in-memory engine."""
    comptime ROCKSDB = Self(1)
    """The RocksDB engine."""
    comptime LEVELDB = Self(2)
    """The LevelDB engine."""
    comptime NVME = Self(3)
    """The NVMe engine."""


@fieldwise_init
struct Modality(Equatable, ImplicitlyCopyable, TrivialRegisterPassable):
    """What a collection holds, `ustore_modality_t`; zero names none."""

    var code: c_int
    """The raw `ustore_modality_t` value the ABI exchanges."""
    comptime NONE = Self(0)
    """No modality, as a name no collection carries finds."""
    comptime BLOBS = Self(1)
    """Binary values."""
    comptime DOCS = Self(2)
    """JSON documents."""
    comptime GRAPH = Self(3)
    """Vertices and their edges."""
    comptime STRINGS = Self(4)
    """Keys found by strings."""
    comptime PAGES = Self(5)
    """Ordered documents of elements."""


@fieldwise_init
struct ChangeKind(Equatable, ImplicitlyCopyable, TrivialRegisterPassable):
    """What one committed change did to its key, `ustore_change_kind_t`."""

    var code: c_int
    """The raw `ustore_change_kind_t` value the ABI exchanges."""
    comptime WRITE = Self(0)
    """Wrote the whole value."""
    comptime REMOVAL = Self(1)
    """Removed the key."""
    comptime CHANGE = Self(2)
    """Changed part of the value."""


@fieldwise_init
struct IngestOrder(Equatable, ImplicitlyCopyable, TrivialRegisterPassable):
    """In what order an ingest's rows arrive, `ustore_ingest_order_t`."""

    var code: c_int
    """The raw `ustore_ingest_order_t` value the ABI exchanges."""
    comptime SORTED = Self(0)
    """Keys strictly ascend across every append."""
    comptime UNSORTED = Self(1)
    """Keys arrive in any order and may repeat, the last row of a key winning."""


@fieldwise_init
struct Admission(Equatable, ImplicitlyCopyable, TrivialRegisterPassable):
    """Whether a predicate keeps a listed key or passes over it, `ustore_admission_t`."""

    var code: c_int
    """The raw `ustore_admission_t` value the ABI exchanges."""
    comptime REJECT = Self(0)
    """Pass over the key."""
    comptime ADMIT = Self(1)
    """Keep the key."""


@fieldwise_init
struct PredicateCost(Equatable, ImplicitlyCopyable, TrivialRegisterPassable):
    """How costly one predicate call is, which decides where in a scan it is asked, `ustore_predicate_cost_t`."""

    var code: c_int
    """The raw `ustore_predicate_cost_t` value the ABI exchanges."""
    comptime CHEAP = Self(0)
    """Asked once per page of listed keys, before patterns or filters read any value."""
    comptime EXPENSIVE = Self(1)
    """Asked once per page about the keys patterns and filters kept."""


# endregion Enumerations

# region Predicates


def _predicate[
    T: AnyType, handler: def(Span[Key, MutAnyOrigin], Span[Admission, MutAnyOrigin], Pointer[T, MutAnyOrigin]) thin -> None
](
    keys: Pointer[Key, MutAnyOrigin], admissions: Pointer[Admission, MutAnyOrigin], count: c_size_t, state: Pointer[T, MutAnyOrigin]
) abi("C"):
    """The C callback of one handler, instantiated per handler since a capturing closure cannot cross."""
    handler(
        Span[Key, MutAnyOrigin](unsafe_ptr=keys, length=Int(count)),
        Span[Admission, MutAnyOrigin](unsafe_ptr=admissions, length=Int(count)),
        state,
    )


@fieldwise_init
struct Predicate(Defaultable, ImplicitlyCopyable):
    """The caller's verdict on the keys a scan lists, `ustore_predicate_t`, asked on the calling side in
    pages of keys, never concurrently with itself within one call."""

    var callback: Address
    """The handler's C entry point; `NULL_ADDRESS` admits every key."""
    var state: Address
    """Forwarded into every call; it must outlive the scan."""
    var cost: PredicateCost
    """Where in a scan the handler is asked."""

    def __init__(out self):
        """No handler: every key is admitted."""
        self = Self(NULL_ADDRESS, NULL_ADDRESS, PredicateCost.CHEAP)

    @staticmethod
    def of[
        T: AnyType,
        origin: MutOrigin,
        //,
        handler: def(Span[Key, MutAnyOrigin], Span[Admission, MutAnyOrigin], Pointer[T, MutAnyOrigin]) thin -> None,
    ](state: Pointer[T, origin], cost: PredicateCost = PredicateCost.CHEAP) -> Self:
        """Asks `handler` about every page of listed keys, which writes one admission per key.

        Parameters:
            T: What `state` points at, inferred from it.
            origin: The state's origin, inferred with it.
            handler: Called with the page's keys, their admissions to write, and `state`.

        Args:
            state: Forwarded into every call; it must outlive the scan.
            cost: Where in a scan the handler is asked.
        """
        var callback = _predicate[T, handler]
        return Self(Pointer(to=callback).unsafe_bitcast[Int]()[], Int(state), cost)


# endregion Predicates

# region Scopes


@fieldwise_init
struct Scope(ImplicitlyCopyable):
    """What a receiver hands every call it routes: the engine, the database, the executor, and what reads
    see and writes join. Binding-only."""

    var library: Library
    """The loaded engine."""
    var database: Address
    """The `ustore_database_t`."""
    var executor: Address
    """The `ustore_executor_t` calls run on; null runs them on the calling thread."""
    var view: Address
    """The `ustore_view_t` reads see; null is the head."""
    var transaction: Address
    """The `ustore_transaction_t` writes join; null writes at the head."""
    var snapshot: Address
    """The `ustore_snapshot_t` listings read and saves write out; null is the head."""
    var watch: Watch
    """Whether the transaction records the keys writes touch."""


# endregion Scopes

# region Database Arguments


@fieldwise_init
struct DataDirectory[origin: ImmOrigin = ImmUntrackedOrigin](ImplicitlyCopyable):
    """A directory the data files may occupy, with the bytes they may take there, `ustore_data_directory_t`.

    Parameters:
        origin: The borrowed path, kept alive for as long as the entry is used.
    """

    var path: BytesView[Self.origin]
    """The directory, which must exist; on NVMe also a raw device from a base."""
    var max_bytes: c_size_t
    """Bytes the files may take there; zero is unlimited on RocksDB and the whole medium on NVMe."""


@fieldwise_init
struct DatabaseNvmeOptions(Defaultable, ImplicitlyCopyable):
    """The sizes an NVMe database opens with, where every zero keeps the engine's default. Binding-only."""

    var memory_limit: c_size_t
    """Bytes of host memory the engine may pin and cache."""
    var transaction_max_bytes: c_size_t
    """Bytes a transaction may buffer before it must commit."""
    var transaction_max_deltas: c_size_t
    """Deltas a transaction may buffer before it must commit."""
    var collection_concurrency_limit: c_size_t
    """Operations that may run on one collection at once."""
    var collection_cache_limit: c_size_t
    """Bytes of values each collection may cache."""
    var collection_memory_runs_max_bytes: c_size_t
    """Bytes of memory runs a collection holds before level 0 flushes."""
    var collection_memory_runs_max_deltas: c_size_t
    """Deltas of memory runs a collection holds before level 0 flushes."""
    var collection_level_enlarge_factor: c_size_t
    """How many times larger each level is than the one above it."""
    var collection_value_size: c_size_t
    """Bytes of every value of a collection whose values share one size, or zero."""
    var collection_value_max_size: c_size_t
    """Bytes of the largest value of a collection whose values vary in size."""
    var cpu_class: CPUClass
    """The class the database confines its waits, atomics and string kernels to; `UNKNOWN` derives."""

    def __init__(out self):
        """Every engine default."""
        self = Self(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, CPUClass.UNKNOWN)


@fieldwise_init
struct DatabaseInitRamArguments(ImplicitlyCopyable):
    """`ustore_database_init_ram_t`."""

    var database: Optional[Pointer[Address, MutUntrackedOrigin]]
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var isolation: Isolation
    var cpu_class: CPUClass


@fieldwise_init
struct DatabaseOpenRamArguments(ImplicitlyCopyable):
    """`ustore_database_open_ram_t`."""

    var database: Optional[Pointer[Address, MutUntrackedOrigin]]
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var directory: BytesViewArguments
    var isolation: Isolation
    var cpu_class: CPUClass


@fieldwise_init
struct DatabaseOpenRocksdbArguments(ImplicitlyCopyable):
    """`ustore_database_open_rocksdb_t`, and `ustore_database_view_rocksdb_t` with the same layout."""

    var database: Optional[Pointer[Address, MutUntrackedOrigin]]
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var directory: BytesViewArguments
    var data_directories: Address
    var data_directories_count: c_size_t


@fieldwise_init
struct DatabaseOpenLeveldbArguments(ImplicitlyCopyable):
    """`ustore_database_open_leveldb_t`."""

    var database: Optional[Pointer[Address, MutUntrackedOrigin]]
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var directory: BytesViewArguments


@fieldwise_init
struct DatabaseOpenNvmeArguments(ImplicitlyCopyable):
    """`ustore_database_open_nvme_t`."""

    var database: Optional[Pointer[Address, MutUntrackedOrigin]]
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var directory: BytesViewArguments
    var data_directories: Address
    var data_directories_count: c_size_t
    var compactions: Address
    var memory_limit: c_size_t
    var transaction_max_bytes: c_size_t
    var transaction_max_deltas: c_size_t
    var collection_concurrency_limit: c_size_t
    var collection_cache_limit: c_size_t
    var collection_memory_runs_max_bytes: c_size_t
    var collection_memory_runs_max_deltas: c_size_t
    var collection_level_enlarge_factor: c_size_t
    var collection_value_size: c_size_t
    var collection_value_max_size: c_size_t
    var cpu_class: CPUClass


@fieldwise_init
struct Capabilities(Equatable, ImplicitlyCopyable, TrivialRegisterPassable):
    """Capabilities an engine reports, `ustore_capabilities_t`, as flags."""

    var code: c_int
    """The raw flags."""
    comptime TRANSACTIONS = Self(1)
    """The engine supports transactions."""
    comptime SNAPSHOTS = Self(4)
    """The engine supports snapshots."""

    def contains(self, other: Self) -> Bool:
        """Whether every flag `other` names is set."""
        return self.code & other.code == other.code


@fieldwise_init
struct DatabaseDescription(Answer, Defaultable, ImplicitlyCopyable):
    """What an open database reports about itself in one call, `ustore_database_description_t`."""

    comptime Written = DatabaseDescription
    var capabilities: Capabilities
    """The capabilities of the engine."""
    var engine: Engine
    """The engine serving the database."""
    var isolation: Isolation
    """How transactions see each other."""
    var keys: c_size_t
    """Keys across every collection, the engine's estimate."""
    var memory_bytes: c_size_t
    """Bytes of memory the engine holds for the data."""
    var disk_bytes: c_size_t
    """Bytes the files of the database take on disk."""

    def __init__(out self):
        """Zeroes every field, which is what a failed report leaves."""
        self = Self(Capabilities(0), Engine.RAM, Isolation.STRICT_SERIALIZABLE, 0, 0, 0)

    def __init__(out self, arena: Address, generation: Int, written: DatabaseDescription, count: Int):
        """Adopts the description a call wrote."""
        self = written


@fieldwise_init
struct DatabaseDescribeArguments(Arguments):
    """`ustore_database_describe_t`."""

    comptime Written = DatabaseDescription
    comptime symbol: StaticString = "ustore_database_describe"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var description: Optional[Pointer[DatabaseDescription, MutUntrackedOrigin]]

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[DatabaseDescription, MutUntrackedOrigin], arena: Address):
        self = Self(scope.database, scope.executor, outcome, completion, written)

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().database_describe


@fieldwise_init
struct DatabaseClearArguments(Arguments):
    """`ustore_database_clear_t`."""

    comptime Written = Unwritten
    comptime symbol: StaticString = "ustore_database_clear"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var mode: DropMode

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[Unwritten, MutUntrackedOrigin], arena: Address):
        self = Self(scope.database, scope.executor, outcome, completion, self.mode)

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().database_clear


@fieldwise_init
struct DatabaseCompactArguments(Arguments):
    """`ustore_database_compact_t`."""

    comptime Written = Unwritten
    comptime symbol: StaticString = "ustore_database_compact"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var progress: Progress

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[Unwritten, MutUntrackedOrigin], arena: Address):
        self = Self(scope.database, scope.executor, outcome, completion, self.progress)

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().database_compact


@fieldwise_init
struct DatabaseFlushArguments(Arguments):
    """`ustore_database_flush_t`."""

    comptime Written = Unwritten
    comptime symbol: StaticString = "ustore_database_flush"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[Unwritten, MutUntrackedOrigin], arena: Address):
        self = Self(scope.database, scope.executor, outcome, completion)

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().database_flush


# endregion Database Arguments

# region Snapshot Arguments


@fieldwise_init
struct SnapshotInitArguments(ImplicitlyCopyable):
    """`ustore_snapshot_init_t`."""

    var snapshot: Optional[Pointer[Address, MutUntrackedOrigin]]
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var read_stamp: Optional[Pointer[Stamp, MutUntrackedOrigin]]


@fieldwise_init
struct SnapshotSaveArguments(Arguments):
    """`ustore_snapshot_save_t`."""

    comptime Written = Unwritten
    comptime symbol: StaticString = "ustore_snapshot_save"
    var snapshot: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var path: BytesViewArguments
    var progress: Progress

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[Unwritten, MutUntrackedOrigin], arena: Address):
        self = Self(scope.snapshot, scope.executor, outcome, completion, self.path, self.progress)

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().snapshot_save


# endregion Snapshot Arguments

# region Collection Arguments


@fieldwise_init
struct CollectionListWritten(Defaultable, ImplicitlyCopyable):
    """The outputs `ustore_collection_list` writes, at one stable address."""

    var collections: Address
    var modalities: Address
    var names: StringsViewArguments
    var written: c_size_t

    def __init__(out self):
        """Nothing written, which is what a failed call leaves."""
        self = Self(NULL_ADDRESS, NULL_ADDRESS, StringsViewArguments(), 0)


@fieldwise_init
struct CollectionListArguments(Arguments):
    """`ustore_collection_list_t`."""

    comptime Written = CollectionListWritten
    comptime symbol: StaticString = "ustore_collection_list"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var snapshot: Address
    var collections: Optional[Pointer[Address, MutUntrackedOrigin]]
    var modalities: Optional[Pointer[Address, MutUntrackedOrigin]]
    var names: Optional[Pointer[StringsViewArguments, MutUntrackedOrigin]]
    var written: Optional[Pointer[c_size_t, MutUntrackedOrigin]]
    var arena: Address

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[CollectionListWritten, MutUntrackedOrigin], arena: Address):
        self = Self(
            scope.database, scope.executor, outcome, completion, scope.snapshot,
            Pointer(to=written[].collections), Pointer(to=written[].modalities), Pointer(to=written[].names),
            Pointer(to=written[].written), arena,
        )

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().collection_list


@fieldwise_init
struct CollectionList[origin: MutOrigin](Answer, Sized):
    """The collections of a database, read in place from the arena a listing borrows.

    Parameters:
        origin: The arena the listing lives in, which this result borrows.
    """

    comptime Written = CollectionListWritten
    var arena: Pointer[Arena, Self.origin]
    """The arena holding the listing."""
    var generation: Int
    """The arena's call the listing came from."""
    var written: CollectionListWritten
    """The views the engine wrote."""

    def __init__(out self, arena: Address, generation: Int, written: CollectionListWritten, count: Int):
        self.arena = Pointer[Arena, Self.origin](unsafe_from_address=arena)
        self.generation = generation
        self.written = written

    def __len__(self) -> Int:
        """The collections listed."""
        return Int(self.written.written)

    def collections(ref self) raises Error -> Span[CollectionID, origin_of(self)]:
        """The identifiers of the collections."""
        _readable(self.arena, self.generation, "CollectionList.collections")
        return _in_arena[CollectionID, origin_of(self)](self.written.collections, len(self))

    def modalities(ref self) raises Error -> Span[Modality, origin_of(self)]:
        """What every collection holds."""
        _readable(self.arena, self.generation, "CollectionList.modalities")
        return _in_arena[Modality, origin_of(self)](self.written.modalities, len(self))

    def names(self) -> Tape[Self.origin]:
        """The names, in the order of the identifiers, empty for a collection without one."""
        return Tape[Self.origin](self.arena, self.generation, self.written.names, len(self))


@fieldwise_init
struct CollectionCreateArguments(Arguments):
    """`ustore_collection_create_t`."""

    comptime Written = CollectionID
    comptime symbol: StaticString = "ustore_collection_create"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var name: BytesViewArguments
    var modality: Modality
    var collection: Optional[Pointer[CollectionID, MutUntrackedOrigin]]

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[CollectionID, MutUntrackedOrigin], arena: Address):
        self = Self(scope.database, scope.executor, outcome, completion, self.name, self.modality, written)

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().collection_create


@fieldwise_init
struct CollectionFindWritten(Defaultable, ImplicitlyCopyable):
    """The outputs `ustore_collection_find` writes, at one stable address."""

    var presences: Address
    var collections: Address
    var modalities: Address

    def __init__(out self):
        """Nothing written, which is what a failed call leaves."""
        self = Self(NULL_ADDRESS, NULL_ADDRESS, NULL_ADDRESS)


@fieldwise_init
struct CollectionFindArguments(Arguments):
    """`ustore_collection_find_t`."""

    comptime Written = CollectionFindWritten
    comptime symbol: StaticString = "ustore_collection_find"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var snapshot: Address
    var count: c_size_t
    var names: StringsViewArguments
    var presences: Optional[Pointer[Address, MutUntrackedOrigin]]
    var collections: Optional[Pointer[Address, MutUntrackedOrigin]]
    var modalities: Optional[Pointer[Address, MutUntrackedOrigin]]
    var arena: Address

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[CollectionFindWritten, MutUntrackedOrigin], arena: Address):
        self = Self(
            scope.database, scope.executor, outcome, completion, scope.snapshot, self.count, self.names,
            Pointer(to=written[].presences), Pointer(to=written[].collections), Pointer(to=written[].modalities), arena,
        )

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().collection_find


@fieldwise_init
struct CollectionsFound[origin: MutOrigin](Answer, Sized):
    """The collections a batch of names address, read in place from the arena a find borrows.

    Parameters:
        origin: The arena the answers live in, which this result borrows.
    """

    comptime Written = CollectionFindWritten
    var arena: Pointer[Arena, Self.origin]
    """The arena holding the answers."""
    var generation: Int
    """The arena's call the answers came from."""
    var written: CollectionFindWritten
    """The views the engine wrote."""
    var count: Int
    """Names the call resolved."""

    def __init__(out self, arena: Address, generation: Int, written: CollectionFindWritten, count: Int):
        self.arena = Pointer[Arena, Self.origin](unsafe_from_address=arena)
        self.generation = generation
        self.written = written
        self.count = count

    def __len__(self) -> Int:
        """The names resolved."""
        return self.count

    def present(self, index: Int) raises Error -> Bool:
        """Whether a collection carries name `index`."""
        _readable(self.arena, self.generation, "CollectionsFound.present")
        return _bit(self.written.presences, index)

    def collections(ref self) raises Error -> Span[CollectionID, origin_of(self)]:
        """The identifier every name addresses, nil where none does."""
        _readable(self.arena, self.generation, "CollectionsFound.collections")
        return _in_arena[CollectionID, origin_of(self)](self.written.collections, self.count)

    def modalities(ref self) raises Error -> Span[Modality, origin_of(self)]:
        """What every addressed collection holds, `NONE` where no collection is."""
        _readable(self.arena, self.generation, "CollectionsFound.modalities")
        return _in_arena[Modality, origin_of(self)](self.written.modalities, self.count)


@fieldwise_init
struct CollectionDropArguments(Arguments):
    """`ustore_collection_drop_t`."""

    comptime Written = Unwritten
    comptime symbol: StaticString = "ustore_collection_drop"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var collection: CollectionID
    var mode: DropMode

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[Unwritten, MutUntrackedOrigin], arena: Address):
        self = Self(scope.database, scope.executor, outcome, completion, self.collection, self.mode)

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().collection_drop


# endregion Collection Arguments

# region Transaction Arguments


@fieldwise_init
struct Reset(Answer, Defaultable, ImplicitlyCopyable):
    """The handle a transaction reset reads and keeps, at one stable address; its result holds nothing."""

    comptime Written = Reset
    var transaction: Address
    """The transaction to reset, kept whatever the outcome."""

    def __init__(out self):
        """No handle."""
        self.transaction = NULL_ADDRESS

    def __init__(out self, arena: Address, generation: Int, written: Reset, count: Int):
        """Adopts the handle the reset kept."""
        self.transaction = written.transaction


@fieldwise_init
struct TransactionInitArguments(Arguments):
    """`ustore_transaction_init_t`, which begins a transaction or resets one in place."""

    comptime Written = Reset
    comptime symbol: StaticString = "ustore_transaction_init"
    var transaction: Optional[Pointer[Address, MutUntrackedOrigin]]
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[Reset, MutUntrackedOrigin], arena: Address):
        written[].transaction = scope.transaction
        self = Self(Pointer(to=written[].transaction), scope.database, scope.executor, outcome, completion)

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().transaction_init


@fieldwise_init
struct TransactionStageArguments(Arguments):
    """`ustore_transaction_stage_t`."""

    comptime Written = Stamp
    comptime symbol: StaticString = "ustore_transaction_stage"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var transaction: Address
    var read_stamp: Optional[Pointer[Stamp, MutUntrackedOrigin]]

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[Stamp, MutUntrackedOrigin], arena: Address):
        self = Self(scope.database, scope.executor, outcome, completion, scope.transaction, written)

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().transaction_stage


@fieldwise_init
struct TransactionCommitArguments(Arguments):
    """`ustore_transaction_commit_t`."""

    comptime Written = Stamp
    comptime symbol: StaticString = "ustore_transaction_commit"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var transaction: Address
    var commit_stamp: Optional[Pointer[Stamp, MutUntrackedOrigin]]
    var durability: Durability

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[Stamp, MutUntrackedOrigin], arena: Address):
        self = Self(scope.database, scope.executor, outcome, completion, scope.transaction, written, self.durability)

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().transaction_commit


@fieldwise_init
struct TransactionRollbackArguments(Arguments):
    """`ustore_transaction_rollback_t`."""

    comptime Written = Unwritten
    comptime symbol: StaticString = "ustore_transaction_rollback"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var transaction: Address

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[Unwritten, MutUntrackedOrigin], arena: Address):
        self = Self(scope.database, scope.executor, outcome, completion, scope.transaction)

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().transaction_rollback


# endregion Transaction Arguments

# region Change Arguments


@fieldwise_init
struct ChangesCursorInitArguments(ImplicitlyCopyable):
    """`ustore_changes_cursor_init_t`."""

    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var collection: CollectionID
    var since_stamp: UInt64
    var cursor: Optional[Pointer[Address, MutUntrackedOrigin]]


@fieldwise_init
struct ChangesWritten(Defaultable, ImplicitlyCopyable):
    """The outputs `ustore_changes_cursor_next` writes, at one stable address."""

    var count: c_size_t
    var keys: Address
    var stamps: Address
    var kinds: Address
    var values: StringsViewArguments

    def __init__(out self):
        """Nothing written, which is what a failed call leaves."""
        self = Self(0, NULL_ADDRESS, NULL_ADDRESS, NULL_ADDRESS, StringsViewArguments())


@fieldwise_init
struct ChangesCursorNextArguments(Arguments):
    """`ustore_changes_cursor_next_t`."""

    comptime Written = ChangesWritten
    comptime symbol: StaticString = "ustore_changes_cursor_next"
    var cursor: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var wanted: c_size_t
    var count: Optional[Pointer[c_size_t, MutUntrackedOrigin]]
    var keys: Optional[Pointer[Address, MutUntrackedOrigin]]
    var stamps: Optional[Pointer[Address, MutUntrackedOrigin]]
    var kinds: Optional[Pointer[Address, MutUntrackedOrigin]]
    var values: Optional[Pointer[StringsViewArguments, MutUntrackedOrigin]]
    var arena: Address
    var with_values: Bool

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[ChangesWritten, MutUntrackedOrigin], arena: Address):
        self = Self(
            self.cursor, scope.executor, outcome, completion, self.wanted, Pointer(to=written[].count),
            Pointer(to=written[].keys), Pointer(to=written[].stamps), Pointer(to=written[].kinds),
            _slot(self.with_values, Pointer(to=written[].values)), arena, self.with_values,
        )

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().changes_cursor_next


@fieldwise_init
struct Changes[origin: MutOrigin](Answer, Sized):
    """The changes one pull of a feed yielded, oldest first, read in place from the arena it borrows.

    Parameters:
        origin: The arena the changes live in, which this result borrows.
    """

    comptime Written = ChangesWritten
    var arena: Pointer[Arena, Self.origin]
    """The arena holding the changes."""
    var generation: Int
    """The arena's call the changes came from."""
    var written: ChangesWritten
    """The views the engine wrote."""

    def __init__(out self, arena: Address, generation: Int, written: ChangesWritten, count: Int):
        self.arena = Pointer[Arena, Self.origin](unsafe_from_address=arena)
        self.generation = generation
        self.written = written

    def __len__(self) -> Int:
        """The changes yielded, zero at the newest commit."""
        return Int(self.written.count)

    def keys(ref self) raises Error -> Span[Key, origin_of(self)]:
        """The key of every change."""
        _readable(self.arena, self.generation, "Changes.keys")
        return _in_arena[Key, origin_of(self)](self.written.keys, len(self))

    def stamps(ref self) raises Error -> Span[Stamp, origin_of(self)]:
        """The stamp every change was committed at."""
        _readable(self.arena, self.generation, "Changes.stamps")
        return _in_arena[Stamp, origin_of(self)](self.written.stamps, len(self))

    def kinds(ref self) raises Error -> Span[ChangeKind, origin_of(self)]:
        """What every change did."""
        _readable(self.arena, self.generation, "Changes.kinds")
        return _in_arena[ChangeKind, origin_of(self)](self.written.kinds, len(self))

    def values(self) -> Tape[Self.origin]:
        """The bytes every change carries, empty for a removal or when the pull skipped them."""
        return Tape[Self.origin](self.arena, self.generation, self.written.values, len(self))


struct ChangesCursor(Movable):
    """A feed of the changes committed to one collection after a stamp, oldest first, freed when it goes out
    of scope. It pins its stamp and holds its database open until then."""

    var library: Library
    """The loaded engine."""
    var address: Address
    """The `ustore_changes_cursor_t`."""

    def __init__(out self, *, library: Library, address: Address):
        """Adopts a feed the engine just opened."""
        self.library = library
        self.address = address

    def __deinit__(deinit self):
        """Frees the feed, releasing its stamp. Calls `ustore_changes_cursor_free`."""
        self.library.symbols().changes_cursor_free(self.address)
        _ = self.library^

    def next(self, executor: Executor, mut arena: Arena, wanted: Int, values: Bool = True) raises Error -> Changes[origin_of(arena)]:
        """Pulls up to `wanted` next changes, with their values when asked. Calls `ustore_changes_cursor_next`.

        Args:
            executor: The executor the pull runs on.
            arena: The arena to rewind and refill, which the result borrows.
            wanted: Most changes the pull yields.
            values: Whether the bytes every change carries come too.

        Raises:
            `Error` if the collection was dropped meanwhile.
        """
        return _block(
            Scope(self.library, NULL_ADDRESS, executor.address, NULL_ADDRESS, NULL_ADDRESS, NULL_ADDRESS, Watch.KEYS),
            _Crossing[ChangesCursorNextArguments, Changes[origin_of(arena)], origin_of(arena), ImmUntrackedOrigin](
                ChangesCursorNextArguments(
                    self.address, NULL_ADDRESS, None, Completion.BLOCKING, c_size_t(wanted), None, None, None, None, None,
                    NULL_ADDRESS, values,
                ),
                Pointer(to=arena),
                0,
            ),
        )


# endregion Change Arguments

# region Ingests


@fieldwise_init
struct IngestInitArguments(ImplicitlyCopyable):
    """`ustore_ingest_init_t`."""

    var ingest: Optional[Pointer[Address, MutUntrackedOrigin]]
    var database: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var collection: CollectionID
    var order: IngestOrder


@fieldwise_init
struct IngestAppendArguments(ImplicitlyCopyable):
    """`ustore_ingest_append_t`."""

    var ingest: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var count: c_size_t
    var keys: StridedArguments
    var values: StringsViewArguments
    var sources: StridedArguments
    var targets: StridedArguments
    var edges: StridedArguments


@fieldwise_init
struct IngestFinishArguments(ImplicitlyCopyable):
    """`ustore_ingest_finish_t`."""

    var ingest: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var durability: Durability
    var commit_stamp: Optional[Pointer[Stamp, MutUntrackedOrigin]]


struct Ingest(Movable):
    """An ingest under way into one collection, landing every row appended as one commit of its own, freed
    when it goes out of scope. Its calls run on the calling thread, one at a time."""

    var library: Library
    """The loaded engine."""
    var address: Address
    """The `ustore_ingest_t`."""

    def __init__(out self, *, library: Library, address: Address):
        """Adopts an ingest the engine just opened."""
        self.library = library
        self.address = address

    def __deinit__(deinit self):
        """Frees the ingest, dropping every row of an unfinished one. Calls `ustore_ingest_free`."""
        self.library.symbols().ingest_free(self.address)
        _ = self.library^

    def append[
        keys_origin: ImmOrigin, values_origin: ImmOrigin, sources_origin: ImmOrigin, targets_origin: ImmOrigin, edges_origin: ImmOrigin, //
    ](
        self,
        keys: KeysView[keys_origin] = KeysView(),
        values: StringsView[values_origin] = StringsView(),
        sources: KeysView[sources_origin] = KeysView(),
        targets: KeysView[targets_origin] = KeysView(),
        edges: KeysView[edges_origin] = KeysView(),
    ) raises Error:
        """Hands the ingest one batch of rows, copied before the call returns. Calls `ustore_ingest_append`.

        Blobs, documents and pages take `keys` and `values`, strings take their strings as `values` and
        the keys they map onto as `keys`, and edges take `sources`, `targets` and optionally `edges`.

        Raises:
            `Error` if a view the modality needs is missing, or a key breaks a sorted order.
        """
        var outcome = stack_allocation[1, Outcome]().unsafe_origin_cast[MutUntrackedOrigin]()
        outcome[] = Outcome()
        var count = _tasks(keys.tasks(), values.tasks(), sources.tasks(), targets.tasks(), edges.tasks())
        _cross(
            IngestAppendArguments(
                self.address, outcome, c_size_t(count), keys.arguments(), values.arguments(), sources.arguments(),
                targets.arguments(), edges.arguments(),
            ),
            self.library.symbols().ingest_append,
        )
        self.library._check(outcome.unsafe_origin_cast[MutAnyOrigin]()[], "ustore_ingest_append")

    def finish(self, durability: Durability = Durability.BUFFERED) raises Error -> Stamp:
        """Lands every row appended as one commit, answering its stamp. Calls `ustore_ingest_finish`.

        Raises:
            `Error` if the collection was dropped meanwhile.
        """
        var outcome = stack_allocation[1, Outcome]().unsafe_origin_cast[MutUntrackedOrigin]()
        outcome[] = Outcome()
        var stamp = stack_allocation[1, Stamp]().unsafe_origin_cast[MutUntrackedOrigin]()
        stamp[] = Stamp()
        _cross(IngestFinishArguments(self.address, outcome, durability, stamp), self.library.symbols().ingest_finish)
        self.library._check(outcome.unsafe_origin_cast[MutAnyOrigin]()[], "ustore_ingest_finish")
        return stamp.unsafe_origin_cast[MutAnyOrigin]()[]


# endregion Ingests

# region Handles


struct Database(BlobsVerbs, DatabaseVerbs, DocsVerbs, GraphVerbs, Movable, PagesVerbs, StringsVerbs):
    """An open database, closed when it goes out of scope while its data persists on disk.

    Verbs called on the database run on the calling thread and address its head; `db.on(executor)`
    runs the same verbs blocking on an executor, and `db.deferred(executor)` submits them there. A
    transaction, snapshot or cursor holds the database open until it is freed itself.
    """

    var library: Library
    """The loaded engine, held so its symbol table outlives every call made through the handle."""
    var address: Address
    """The opaque `ustore_database_t`."""

    def __init__(out self, *, library: Library, address: Address):
        """Adopts a handle the engine just opened."""
        self.library = library
        self.address = address

    @staticmethod
    def init_ram(
        executor: Executor, isolation: Isolation = Isolation.STRICT_SERIALIZABLE, cpu_class: CPUClass = CPUClass.UNKNOWN
    ) raises Error -> Self:
        """Starts an empty database of the in-memory engine, which persists nowhere. Calls `ustore_database_init_ram`.

        Args:
            executor: The executor the open runs on; it also lends its engine.
            isolation: How transactions see each other.
            cpu_class: The class the database confines its kernels to; `UNKNOWN` derives.

        Raises:
            `Error` if the class is not one the CPU runs.
        """
        var created = stack_allocation[1, Address]().unsafe_origin_cast[MutUntrackedOrigin]()
        created[] = NULL_ADDRESS
        var outcome = stack_allocation[1, Outcome]().unsafe_origin_cast[MutUntrackedOrigin]()
        outcome[] = Outcome()
        _cross(
            DatabaseInitRamArguments(created, executor.address, outcome, Completion.BLOCKING, isolation, cpu_class),
            executor.library.symbols().database_init_ram,
        )
        return Self(library=executor.library, address=_created(executor.library, outcome, created, "ustore_database_init_ram"))

    @staticmethod
    def open_ram[
        directory_origin: ImmOrigin, //
    ](
        executor: Executor,
        directory: StringSlice[directory_origin],
        isolation: Isolation = Isolation.STRICT_SERIALIZABLE,
        cpu_class: CPUClass = CPUClass.UNKNOWN,
    ) raises Error -> Self:
        """Serves a directory through the in-memory engine, loading its image and rewriting it on flushed
        commits and on close. Calls `ustore_database_open_ram`.

        Raises:
            `Error` with `NOT_FOUND` if the directory does not exist, or `MALFORMED` for a corrupt image.
        """
        var created = stack_allocation[1, Address]().unsafe_origin_cast[MutUntrackedOrigin]()
        created[] = NULL_ADDRESS
        var outcome = stack_allocation[1, Outcome]().unsafe_origin_cast[MutUntrackedOrigin]()
        outcome[] = Outcome()
        _cross(
            DatabaseOpenRamArguments(
                created, executor.address, outcome, Completion.BLOCKING, BytesView.of(directory).arguments(), isolation, cpu_class
            ),
            executor.library.symbols().database_open_ram,
        )
        return Self(library=executor.library, address=_created(executor.library, outcome, created, "ustore_database_open_ram"))

    @staticmethod
    def open_rocksdb[
        directory_origin: ImmOrigin, paths_origin: ImmOrigin, directories_origin: ImmOrigin, //
    ](
        executor: Executor,
        directory: StringSlice[directory_origin],
        data_directories: Span[DataDirectory[paths_origin], directories_origin] = Span[DataDirectory[paths_origin], directories_origin](),
    ) raises Error -> Self:
        """Serves a directory through the RocksDB engine, creating the database when it holds none.
        Calls `ustore_database_open_rocksdb`.

        Raises:
            `Error` with `UNSUPPORTED` in a build without the engine.
        """
        return Self._rocksdb(executor, BytesView.of(directory).arguments(), Int(data_directories.unsafe_ptr()), len(data_directories), False)

    @staticmethod
    def view_rocksdb[
        directory_origin: ImmOrigin, paths_origin: ImmOrigin, directories_origin: ImmOrigin, //
    ](
        executor: Executor,
        directory: StringSlice[directory_origin],
        data_directories: Span[DataDirectory[paths_origin], directories_origin] = Span[DataDirectory[paths_origin], directories_origin](),
    ) raises Error -> Self:
        """Serves a directory through the RocksDB engine, read-only. Calls `ustore_database_view_rocksdb`.

        Raises:
            `Error` with `UNSUPPORTED` in a build without the engine.
        """
        return Self._rocksdb(executor, BytesView.of(directory).arguments(), Int(data_directories.unsafe_ptr()), len(data_directories), True)

    @staticmethod
    def _rocksdb(executor: Executor, directory: BytesViewArguments, directories: Address, count: Int, read_only: Bool) raises Error -> Self:
        """The open and the read-only view share one layout, differing only in the verb."""
        var created = stack_allocation[1, Address]().unsafe_origin_cast[MutUntrackedOrigin]()
        created[] = NULL_ADDRESS
        var outcome = stack_allocation[1, Outcome]().unsafe_origin_cast[MutUntrackedOrigin]()
        outcome[] = Outcome()
        var arguments = DatabaseOpenRocksdbArguments(
            created, executor.address, outcome, Completion.BLOCKING, directory, directories if count else NULL_ADDRESS, c_size_t(count)
        )
        if read_only:
            _cross(arguments, executor.library.symbols().database_view_rocksdb)
            return Self(library=executor.library, address=_created(executor.library, outcome, created, "ustore_database_view_rocksdb"))
        _cross(arguments, executor.library.symbols().database_open_rocksdb)
        return Self(library=executor.library, address=_created(executor.library, outcome, created, "ustore_database_open_rocksdb"))

    @staticmethod
    def open_leveldb[directory_origin: ImmOrigin, //](executor: Executor, directory: StringSlice[directory_origin]) raises Error -> Self:
        """Serves a directory through the LevelDB engine, creating the database when it holds none.
        Calls `ustore_database_open_leveldb`.

        Raises:
            `Error` with `UNSUPPORTED` in a build without the engine.
        """
        var created = stack_allocation[1, Address]().unsafe_origin_cast[MutUntrackedOrigin]()
        created[] = NULL_ADDRESS
        var outcome = stack_allocation[1, Outcome]().unsafe_origin_cast[MutUntrackedOrigin]()
        outcome[] = Outcome()
        _cross(
            DatabaseOpenLeveldbArguments(created, executor.address, outcome, Completion.BLOCKING, BytesView.of(directory).arguments()),
            executor.library.symbols().database_open_leveldb,
        )
        return Self(library=executor.library, address=_created(executor.library, outcome, created, "ustore_database_open_leveldb"))

    @staticmethod
    def open_nvme[
        directory_origin: ImmOrigin, paths_origin: ImmOrigin, directories_origin: ImmOrigin, //
    ](
        executor: Executor,
        directory: StringSlice[directory_origin],
        options: DatabaseNvmeOptions = DatabaseNvmeOptions(),
        data_directories: Span[DataDirectory[paths_origin], directories_origin] = Span[DataDirectory[paths_origin], directories_origin](),
        compactions: Address = NULL_ADDRESS,
    ) raises Error -> Self:
        """Serves the database a manifest names through the NVMe engine, creating it when the manifest is
        absent. Calls `ustore_database_open_nvme`.

        Args:
            executor: The executor the open runs on, whose transport the database reads once.
            directory: The manifest, or the raw address of the database's first slot.
            options: The sizes the database opens with.
            data_directories: Further directories every run may land on.
            compactions: The address of a VRAM executor every merge runs on, read once; null merges on host threads.

        Raises:
            `Error` with `UNSUPPORTED` in a build without the engine.
        """
        var created = stack_allocation[1, Address]().unsafe_origin_cast[MutUntrackedOrigin]()
        created[] = NULL_ADDRESS
        var outcome = stack_allocation[1, Outcome]().unsafe_origin_cast[MutUntrackedOrigin]()
        outcome[] = Outcome()
        var count = len(data_directories)
        _cross(
            DatabaseOpenNvmeArguments(
                created, executor.address, outcome, Completion.BLOCKING, BytesView.of(directory).arguments(),
                Int(data_directories.unsafe_ptr()) if count else NULL_ADDRESS, c_size_t(count), compactions,
                options.memory_limit, options.transaction_max_bytes, options.transaction_max_deltas,
                options.collection_concurrency_limit, options.collection_cache_limit,
                options.collection_memory_runs_max_bytes, options.collection_memory_runs_max_deltas,
                options.collection_level_enlarge_factor, options.collection_value_size,
                options.collection_value_max_size, options.cpu_class,
            ),
            executor.library.symbols().database_open_nvme,
        )
        return Self(library=executor.library, address=_created(executor.library, outcome, created, "ustore_database_open_nvme"))

    def __deinit__(deinit self):
        """Closes the database once nothing holds it open. Calls `ustore_database_free`.

        The loader reference is surrendered only after the call, so Mojo cannot `dlclose` the engine
        between the load of the entry point and the branch to it.
        """
        self.library.symbols().database_free(self.address)
        _ = self.library^

    def _scope(self) -> Scope:
        """The head, on the calling thread."""
        return Scope(self.library, self.address, NULL_ADDRESS, NULL_ADDRESS, NULL_ADDRESS, NULL_ADDRESS, Watch.KEYS)

    def on(ref self, ref executor: Executor) -> On[ImmOrigin(origin_of(self, executor))]:
        """The same verbs, blocking on `executor` after every call submitted there before them. Binding-only."""
        var scope = self._scope()
        scope.executor = executor.address
        return On[ImmOrigin(origin_of(self, executor))](scope)

    def deferred(ref self, ref executor: Executor) -> DeferredOn[ImmOrigin(origin_of(self, executor))]:
        """The same verbs, each submitted to `executor` and answering a `Deferred` at once. Binding-only."""
        var scope = self._scope()
        scope.executor = executor.address
        return DeferredOn[ImmOrigin(origin_of(self, executor))](scope)

    def snapshot(self) raises Error -> Snapshot:
        """Pins a point-in-time state of the database until the snapshot is freed. Calls `ustore_snapshot_init`.

        Raises:
            `Error` with `UNSUPPORTED` on an engine without snapshots.
        """
        var created = stack_allocation[1, Address]().unsafe_origin_cast[MutUntrackedOrigin]()
        created[] = NULL_ADDRESS
        var outcome = stack_allocation[1, Outcome]().unsafe_origin_cast[MutUntrackedOrigin]()
        outcome[] = Outcome()
        var stamp = stack_allocation[1, Stamp]().unsafe_origin_cast[MutUntrackedOrigin]()
        stamp[] = Stamp()
        _cross(
            SnapshotInitArguments(created, self.address, NULL_ADDRESS, outcome, Completion.BLOCKING, stamp),
            self.library.symbols().snapshot_init,
        )
        var address = _created(self.library, outcome, created, "ustore_snapshot_init")
        return Snapshot(library=self.library, database=self.address, address=address, read_stamp=stamp.unsafe_origin_cast[MutAnyOrigin]()[])

    def transaction(self) raises Error -> Transaction:
        """Begins a transaction. Calls `ustore_transaction_init`.

        Raises:
            `Error` with `UNSUPPORTED` on an engine without transactions.
        """
        var created = stack_allocation[1, Address]().unsafe_origin_cast[MutUntrackedOrigin]()
        created[] = NULL_ADDRESS
        var outcome = stack_allocation[1, Outcome]().unsafe_origin_cast[MutUntrackedOrigin]()
        outcome[] = Outcome()
        _cross(
            TransactionInitArguments(created, self.address, NULL_ADDRESS, outcome, Completion.BLOCKING),
            self.library.symbols().transaction_init,
        )
        return Transaction(library=self.library, database=self.address, address=_created(self.library, outcome, created, "ustore_transaction_init"))

    def ingest(self, collection: CollectionID, order: IngestOrder = IngestOrder.SORTED) raises Error -> Ingest:
        """Opens an ingest into one collection. Calls `ustore_ingest_init`.

        Raises:
            `Error` with `UNSUPPORTED` where the engine ingests nothing into that collection's modality.
        """
        var created = stack_allocation[1, Address]().unsafe_origin_cast[MutUntrackedOrigin]()
        created[] = NULL_ADDRESS
        var outcome = stack_allocation[1, Outcome]().unsafe_origin_cast[MutUntrackedOrigin]()
        outcome[] = Outcome()
        _cross(IngestInitArguments(created, self.address, outcome, collection, order), self.library.symbols().ingest_init)
        return Ingest(library=self.library, address=_created(self.library, outcome, created, "ustore_ingest_init"))


struct Transaction(BlobsVerbs, DatabaseVerbs, DocsVerbs, GraphVerbs, Movable, PagesVerbs, StringsVerbs):
    """An optimistic transaction grouping operations across collections and modalities, freed when it goes
    out of scope. It holds its database open, and serves one call at a time.

    Verbs called on it run on the calling thread, read through it and write into it, recording the keys
    they touch; `txn.unwatched()` records nothing, and `txn.on(executor)` and `txn.deferred(executor)`
    route the same verbs to an executor.
    """

    var library: Library
    """The loaded engine."""
    var database: Address
    """The `ustore_database_t` it belongs to."""
    var address: Address
    """The opaque `ustore_transaction_t`."""

    def __init__(out self, *, library: Library, database: Address, address: Address):
        """Adopts a transaction the engine just began."""
        self.library = library
        self.database = database
        self.address = address

    def __deinit__(deinit self):
        """Resets and frees the transaction. Calls `ustore_transaction_free`."""
        self.library.symbols().transaction_free(self.address)
        _ = self.library^

    def _watching(self, watch: Watch, executor: Address) -> Scope:
        """The transaction's view, recording its keys or not, on `executor`. Calls `ustore_transaction_view`."""
        var view = self.library.symbols().transaction_view(self.address, watch.code)
        return Scope(self.library, self.database, executor, view, self.address, NULL_ADDRESS, watch)

    def _scope(self) -> Scope:
        """The transaction, recording its keys, on the calling thread."""
        return self._watching(Watch.KEYS, NULL_ADDRESS)

    def unwatched(ref self) -> On[ImmOrigin(origin_of(self))]:
        """The same verbs on the calling thread, recording nothing for conflicts. Binding-only."""
        return On[ImmOrigin(origin_of(self))](self._watching(Watch.NONE, NULL_ADDRESS))

    def on(ref self, ref executor: Executor, watch: Watch = Watch.KEYS) -> On[ImmOrigin(origin_of(self, executor))]:
        """The same verbs, blocking on `executor`. Binding-only."""
        return On[ImmOrigin(origin_of(self, executor))](self._watching(watch, executor.address))

    def deferred(ref self, ref executor: Executor, watch: Watch = Watch.KEYS) -> DeferredOn[ImmOrigin(origin_of(self, executor))]:
        """The same verbs, each submitted to `executor` and answering a `Deferred` at once. Binding-only."""
        return DeferredOn[ImmOrigin(origin_of(self, executor))](self._watching(watch, executor.address))


struct Snapshot(BlobsVerbs, DatabaseVerbs, DocsVerbs, GraphVerbs, Movable, PagesVerbs, StringsVerbs):
    """A point-in-time state of the whole database, released when it goes out of scope. It pins the
    versions its reads need and holds its database open.

    Verbs called on it read at its state on the calling thread; writes, carrying no transaction, land
    at the head. `snap.on(executor)` and `snap.deferred(executor)` route the same verbs to an executor.
    """

    var library: Library
    """The loaded engine."""
    var database: Address
    """The `ustore_database_t` it belongs to."""
    var address: Address
    """The opaque `ustore_snapshot_t`."""
    var read_stamp: Stamp
    """The stamp the snapshot reads at."""

    def __init__(out self, *, library: Library, database: Address, address: Address, read_stamp: Stamp):
        """Adopts a snapshot the engine just pinned."""
        self.library = library
        self.database = database
        self.address = address
        self.read_stamp = read_stamp

    def __deinit__(deinit self):
        """Releases the snapshot. Calls `ustore_snapshot_free`."""
        self.library.symbols().snapshot_free(self.address)
        _ = self.library^

    def _on(self, executor: Address) -> Scope:
        """The snapshot's view on `executor`. Calls `ustore_snapshot_view`."""
        var view = self.library.symbols().snapshot_view(self.address)
        return Scope(self.library, self.database, executor, view, NULL_ADDRESS, self.address, Watch.KEYS)

    def _scope(self) -> Scope:
        """The snapshot, on the calling thread."""
        return self._on(NULL_ADDRESS)

    def on(ref self, ref executor: Executor) -> On[ImmOrigin(origin_of(self, executor))]:
        """The same verbs, blocking on `executor`. Binding-only."""
        return On[ImmOrigin(origin_of(self, executor))](self._on(executor.address))

    def deferred(ref self, ref executor: Executor) -> DeferredOn[ImmOrigin(origin_of(self, executor))]:
        """The same verbs, each submitted to `executor` and answering a `Deferred` at once. Binding-only."""
        return DeferredOn[ImmOrigin(origin_of(self, executor))](self._on(executor.address))


@fieldwise_init
struct On[origin: ImmOrigin](BlobsVerbs, DatabaseVerbs, DocsVerbs, GraphVerbs, ImplicitlyCopyable, PagesVerbs, StringsVerbs):
    """A database, transaction or snapshot bound to the executor its verbs block on. Binding-only.

    Parameters:
        origin: The handle and the executor, which this receiver borrows.
    """

    var scope: Scope
    """What every verb addresses."""

    def _scope(self) -> Scope:
        """The bound handle and executor."""
        return self.scope


@fieldwise_init
struct DeferredOn[origin: ImmOrigin](
    BlobsDeferredVerbs, DatabaseDeferredVerbs, DocsDeferredVerbs, GraphDeferredVerbs, ImplicitlyCopyable, PagesDeferredVerbs, StringsDeferredVerbs
):
    """A database, transaction or snapshot bound to the executor its verbs are submitted to: every verb
    answers a `Deferred` at once, borrowing its arena and inputs until final. Binding-only.

    The verbs and their arguments are those of the blocking receivers.

    Parameters:
        origin: The handle and the executor, which this receiver borrows.
    """

    var scope: Scope
    """What every verb addresses."""

    def _scope(self) -> Scope:
        """The bound handle and executor."""
        return self.scope


# endregion Handles

# region Verbs


trait DatabaseVerbs:
    """The verbs of the database itself and of its transactions and snapshots, written once over the
    receiver's scope."""

    def _scope(self) -> Scope:
        """What the receiver addresses and the executor it runs on."""
        ...

    def describe(self) raises Error -> DatabaseDescription:
        """Reports the engine, what it supports and how much it holds. Calls `ustore_database_describe`."""
        return _block(
            self._scope(),
            _Crossing[DatabaseDescribeArguments, DatabaseDescription, MutUntrackedOrigin, ImmUntrackedOrigin](
                DatabaseDescribeArguments(NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, None), None, 0
            ),
        )

    def clear(self, mode: DropMode = DropMode.PAIRS) raises Error:
        """Empties every collection at once at the head. Calls `ustore_database_clear`.

        Raises:
            `Error` with `READ_ONLY` on a database opened read-only.
        """
        _ = _block(
            self._scope(),
            _Crossing[DatabaseClearArguments, Unwritten, MutUntrackedOrigin, ImmUntrackedOrigin](
                DatabaseClearArguments(NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, mode), None, 0
            ),
        )

    def compact(self, progress: Progress = Progress()) raises Error:
        """Reclaims the space overwritten and removed data still takes. Calls `ustore_database_compact`.

        Raises:
            `Error` with `CANCELLED` once `progress` stops it, keeping what it compacted.
        """
        _ = _block(
            self._scope(),
            _Crossing[DatabaseCompactArguments, Unwritten, MutUntrackedOrigin, ImmUntrackedOrigin](
                DatabaseCompactArguments(NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, progress), None, 0
            ),
        )

    def flush(self) raises Error:
        """Makes every commit that returned before this call durable. Calls `ustore_database_flush`."""
        _ = _block(
            self._scope(),
            _Crossing[DatabaseFlushArguments, Unwritten, MutUntrackedOrigin, ImmUntrackedOrigin](
                DatabaseFlushArguments(NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING), None, 0
            ),
        )

    def save[path_origin: ImmOrigin, //](self, path: StringSlice[path_origin], progress: Progress = Progress()) raises Error:
        """Writes the receiver's snapshot out as a new database of the same engine. Calls `ustore_snapshot_save`.

        Raises:
            `Error` with `UNINITIALIZED` on a receiver of no snapshot.
        """
        _ = _block(
            self._scope(),
            _Crossing[SnapshotSaveArguments, Unwritten, MutUntrackedOrigin, path_origin](
                SnapshotSaveArguments(NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, BytesView.of(path).arguments(), progress),
                None,
                0,
            ),
        )

    def collection_list(self, mut arena: Arena) raises Error -> CollectionList[origin_of(arena)]:
        """Lists the collections of the head, or of the receiver's snapshot. Calls `ustore_collection_list`."""
        return _block(
            self._scope(),
            _Crossing[CollectionListArguments, CollectionList[origin_of(arena)], origin_of(arena), ImmUntrackedOrigin](
                CollectionListArguments(NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS, None, None, None, None, NULL_ADDRESS),
                Pointer(to=arena),
                0,
            ),
        )

    def collection_create[name_origin: ImmOrigin, //](self, name: StringSlice[name_origin], modality: Modality) raises Error -> CollectionID:
        """Creates an empty collection, unnamed for an empty `name`. Calls `ustore_collection_create`.

        Raises:
            `Error` with `ALREADY_EXISTS` when another collection carries the name.
        """
        var view = BytesView.of(name).arguments() if name.byte_length() else BytesViewArguments.empty()
        return _block(
            self._scope(),
            _Crossing[CollectionCreateArguments, CollectionID, MutUntrackedOrigin, name_origin](
                CollectionCreateArguments(NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, view, modality, None), None, 0
            ),
        )

    def collection_find[names_origin: ImmOrigin, //](self, names: StringsView[names_origin], mut arena: Arena) raises Error -> CollectionsFound[origin_of(arena)]:
        """Reads the collections a batch of names address. Calls `ustore_collection_find`."""
        return _block(
            self._scope(),
            _Crossing[CollectionFindArguments, CollectionsFound[origin_of(arena)], origin_of(arena), names_origin](
                CollectionFindArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS, c_size_t(len(names)), names.arguments(),
                    None, None, None, NULL_ADDRESS,
                ),
                Pointer(to=arena),
                len(names),
            ),
        )

    def collection_drop(self, collection: CollectionID, mode: DropMode = DropMode.COLLECTION) raises Error:
        """Removes or clears a collection. Calls `ustore_collection_drop`.

        Raises:
            `Error` with `NOT_FOUND` when no collection carries the identifier.
        """
        _ = _block(
            self._scope(),
            _Crossing[CollectionDropArguments, Unwritten, MutUntrackedOrigin, ImmUntrackedOrigin](
                CollectionDropArguments(NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, collection, mode), None, 0
            ),
        )

    def reset(self) raises Error:
        """Resets the receiver's transaction onto a fresh snapshot, dropping its reads and writes.
        Calls `ustore_transaction_init`."""
        _ = _block(
            self._scope(),
            _Crossing[TransactionInitArguments, Reset, MutUntrackedOrigin, ImmUntrackedOrigin](
                TransactionInitArguments(None, NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING), None, 0
            ),
        )

    def stage(self) raises Error -> Stamp:
        """Stages the receiver's transaction, the first phase of a two-phase commit, answering its read stamp.
        Calls `ustore_transaction_stage`.

        Raises:
            `Error` with a conflict status when another commit got there first.
        """
        return _block(
            self._scope(),
            _Crossing[TransactionStageArguments, Stamp, MutUntrackedOrigin, ImmUntrackedOrigin](
                TransactionStageArguments(NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS, None), None, 0
            ),
        )

    def commit(self, durability: Durability = Durability.BUFFERED) raises Error -> Stamp:
        """Commits the receiver's transaction, answering its commit stamp. Calls `ustore_transaction_commit`.

        Raises:
            `Error` with a conflict status when another commit got there first.
        """
        return _block(
            self._scope(),
            _Crossing[TransactionCommitArguments, Stamp, MutUntrackedOrigin, ImmUntrackedOrigin](
                TransactionCommitArguments(NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS, None, durability), None, 0
            ),
        )

    def rollback(self) raises Error:
        """Withdraws a stage, keeping the reads and writes. Calls `ustore_transaction_rollback`."""
        _ = _block(
            self._scope(),
            _Crossing[TransactionRollbackArguments, Unwritten, MutUntrackedOrigin, ImmUntrackedOrigin](
                TransactionRollbackArguments(NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS), None, 0
            ),
        )

    def changes_cursor(self, collection: CollectionID, since: Stamp) raises Error -> ChangesCursor:
        """Opens a feed of one collection's changes committed after `since`, reading nothing yet.
        Calls `ustore_changes_cursor_init`.

        Raises:
            `Error` with `WRITE_CONFLICT` once the versions after `since` are gone.
        """
        var scope = self._scope()
        var created = stack_allocation[1, Address]().unsafe_origin_cast[MutUntrackedOrigin]()
        created[] = NULL_ADDRESS
        var outcome = stack_allocation[1, Outcome]().unsafe_origin_cast[MutUntrackedOrigin]()
        outcome[] = Outcome()
        _cross(
            ChangesCursorInitArguments(scope.database, scope.executor, outcome, Completion.BLOCKING, collection, since.value, created),
            scope.library.symbols().changes_cursor_init,
        )
        return ChangesCursor(library=scope.library, address=_created(scope.library, outcome, created, "ustore_changes_cursor_init"))


trait DatabaseDeferredVerbs:
    """The database verbs every deferred receiver offers, each answering a `Deferred` at once."""

    def _scope(self) -> Scope:
        """What the receiver addresses and the executor it submits to."""
        ...

    def describe(self) -> Deferred[_Crossing[DatabaseDescribeArguments, DatabaseDescription, MutUntrackedOrigin, ImmUntrackedOrigin]]:
        """Submits `DatabaseVerbs.describe`. Calls `ustore_database_describe`."""
        return {
            self._scope(),
            _Crossing[DatabaseDescribeArguments, DatabaseDescription, MutUntrackedOrigin, ImmUntrackedOrigin](
                DatabaseDescribeArguments(NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, None), None, 0
            ),
        }

    def clear(self, mode: DropMode = DropMode.PAIRS) -> Deferred[_Crossing[DatabaseClearArguments, Unwritten, MutUntrackedOrigin, ImmUntrackedOrigin]]:
        """Submits `DatabaseVerbs.clear`. Calls `ustore_database_clear`."""
        return {
            self._scope(),
            _Crossing[DatabaseClearArguments, Unwritten, MutUntrackedOrigin, ImmUntrackedOrigin](
                DatabaseClearArguments(NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, mode), None, 0
            ),
        }

    def compact(self, progress: Progress = Progress()) -> Deferred[_Crossing[DatabaseCompactArguments, Unwritten, MutUntrackedOrigin, ImmUntrackedOrigin]]:
        """Submits `DatabaseVerbs.compact`. Calls `ustore_database_compact`."""
        return {
            self._scope(),
            _Crossing[DatabaseCompactArguments, Unwritten, MutUntrackedOrigin, ImmUntrackedOrigin](
                DatabaseCompactArguments(NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, progress), None, 0
            ),
        }

    def flush(self) -> Deferred[_Crossing[DatabaseFlushArguments, Unwritten, MutUntrackedOrigin, ImmUntrackedOrigin]]:
        """Submits `DatabaseVerbs.flush`. Calls `ustore_database_flush`."""
        return {
            self._scope(),
            _Crossing[DatabaseFlushArguments, Unwritten, MutUntrackedOrigin, ImmUntrackedOrigin](
                DatabaseFlushArguments(NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED), None, 0
            ),
        }

    def save[path_origin: ImmOrigin, //](self, path: StringSlice[path_origin], progress: Progress = Progress()) -> Deferred[
        _Crossing[SnapshotSaveArguments, Unwritten, MutUntrackedOrigin, path_origin]
    ]:
        """Submits `DatabaseVerbs.save`. Calls `ustore_snapshot_save`."""
        return {
            self._scope(),
            _Crossing[SnapshotSaveArguments, Unwritten, MutUntrackedOrigin, path_origin](
                SnapshotSaveArguments(NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, BytesView.of(path).arguments(), progress),
                None,
                0,
            ),
        }

    def collection_list(self, mut arena: Arena) -> Deferred[
        _Crossing[CollectionListArguments, CollectionList[origin_of(arena)], origin_of(arena), ImmUntrackedOrigin]
    ]:
        """Submits `DatabaseVerbs.collection_list`. Calls `ustore_collection_list`."""
        return {
            self._scope(),
            _Crossing[CollectionListArguments, CollectionList[origin_of(arena)], origin_of(arena), ImmUntrackedOrigin](
                CollectionListArguments(NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS, None, None, None, None, NULL_ADDRESS),
                Pointer(to=arena),
                0,
            ),
        }

    def collection_create[name_origin: ImmOrigin, //](self, name: StringSlice[name_origin], modality: Modality) -> Deferred[
        _Crossing[CollectionCreateArguments, CollectionID, MutUntrackedOrigin, name_origin]
    ]:
        """Submits `DatabaseVerbs.collection_create`. Calls `ustore_collection_create`."""
        var view = BytesView.of(name).arguments() if name.byte_length() else BytesViewArguments.empty()
        return {
            self._scope(),
            _Crossing[CollectionCreateArguments, CollectionID, MutUntrackedOrigin, name_origin](
                CollectionCreateArguments(NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, view, modality, None), None, 0
            ),
        }

    def collection_find[names_origin: ImmOrigin, //](self, names: StringsView[names_origin], mut arena: Arena) -> Deferred[
        _Crossing[CollectionFindArguments, CollectionsFound[origin_of(arena)], origin_of(arena), names_origin]
    ]:
        """Submits `DatabaseVerbs.collection_find`. Calls `ustore_collection_find`."""
        return {
            self._scope(),
            _Crossing[CollectionFindArguments, CollectionsFound[origin_of(arena)], origin_of(arena), names_origin](
                CollectionFindArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS, c_size_t(len(names)), names.arguments(),
                    None, None, None, NULL_ADDRESS,
                ),
                Pointer(to=arena),
                len(names),
            ),
        }

    def collection_drop(self, collection: CollectionID, mode: DropMode = DropMode.COLLECTION) -> Deferred[
        _Crossing[CollectionDropArguments, Unwritten, MutUntrackedOrigin, ImmUntrackedOrigin]
    ]:
        """Submits `DatabaseVerbs.collection_drop`. Calls `ustore_collection_drop`."""
        return {
            self._scope(),
            _Crossing[CollectionDropArguments, Unwritten, MutUntrackedOrigin, ImmUntrackedOrigin](
                CollectionDropArguments(NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, collection, mode), None, 0
            ),
        }

    def reset(self) -> Deferred[_Crossing[TransactionInitArguments, Reset, MutUntrackedOrigin, ImmUntrackedOrigin]]:
        """Submits `DatabaseVerbs.reset`. Calls `ustore_transaction_init`."""
        return {
            self._scope(),
            _Crossing[TransactionInitArguments, Reset, MutUntrackedOrigin, ImmUntrackedOrigin](
                TransactionInitArguments(None, NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED), None, 0
            ),
        }

    def stage(self) -> Deferred[_Crossing[TransactionStageArguments, Stamp, MutUntrackedOrigin, ImmUntrackedOrigin]]:
        """Submits `DatabaseVerbs.stage`. Calls `ustore_transaction_stage`."""
        return {
            self._scope(),
            _Crossing[TransactionStageArguments, Stamp, MutUntrackedOrigin, ImmUntrackedOrigin](
                TransactionStageArguments(NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS, None), None, 0
            ),
        }

    def commit(self, durability: Durability = Durability.BUFFERED) -> Deferred[
        _Crossing[TransactionCommitArguments, Stamp, MutUntrackedOrigin, ImmUntrackedOrigin]
    ]:
        """Submits `DatabaseVerbs.commit`. Calls `ustore_transaction_commit`."""
        return {
            self._scope(),
            _Crossing[TransactionCommitArguments, Stamp, MutUntrackedOrigin, ImmUntrackedOrigin](
                TransactionCommitArguments(NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS, None, durability), None, 0
            ),
        }

    def rollback(self) -> Deferred[_Crossing[TransactionRollbackArguments, Unwritten, MutUntrackedOrigin, ImmUntrackedOrigin]]:
        """Submits `DatabaseVerbs.rollback`. Calls `ustore_transaction_rollback`."""
        return {
            self._scope(),
            _Crossing[TransactionRollbackArguments, Unwritten, MutUntrackedOrigin, ImmUntrackedOrigin](
                TransactionRollbackArguments(NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS), None, 0
            ),
        }


# endregion Verbs
