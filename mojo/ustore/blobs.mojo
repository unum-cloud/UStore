"""Collections of binary values: writes, removes, reads, scans, cursors, samples and measures.

Every verb takes a batch, so a single operation is a batch of one. Keys arrive as a strided run and
values as a tape, so a tape a read wrote is written back as it is. Every output whose length scales
with the call lands in the `Arena` the verb takes, and the result borrows it in turn. The results
here also serve the documents, graph and pages verbs, which list keys and read values the same way.

File: mojo/ustore/blobs.mojo
Author: Ash Vardanian
Date: November 8, 2026
"""

from std.ffi import c_int, c_size_t
from std.memory import stack_allocation

from ustore.db import (
    CollectionID,
    CollectionsView,
    Durability,
    Key,
    KeysView,
    LengthsView,
    Predicate,
    ScanDirection,
    ScanMode,
    Scope,
    StridedArguments,
    Watch,
    _tasks,
)
from ustore.library import Library
from ustore.types import (
    Address,
    Answer,
    Arena,
    Arguments,
    Completion,
    Deferred,
    Error,
    Executor,
    NULL_ADDRESS,
    Outcome,
    Patterns,
    PatternsArguments,
    Progress,
    StringsView,
    StringsViewArguments,
    Tape,
    Unwritten,
    Verb,
    _anchor,
    _bit,
    _block,
    _created,
    _cross,
    _Crossing,
    _in_arena,
    _readable,
    _slot,
)

# region Results


@fieldwise_init
struct ValuesWritten(Defaultable, ImplicitlyCopyable):
    """The presences, lengths and values a read writes, at one stable address."""

    var presences: Address
    var lengths: Address
    var values: StringsViewArguments
    var with_values: Bool

    def __init__(out self):
        """Nothing written, which is what a failed call leaves."""
        self = Self(NULL_ADDRESS, NULL_ADDRESS, StringsViewArguments(), True)


@fieldwise_init
struct Values[origin: MutOrigin](Answer, Sized):
    """What a read wrote into the arena it borrows: an Apache Arrow binary array of one value per task,
    a missing key reading as an unset presence bit and an empty value.

    Parameters:
        origin: The arena the values live in, which this result borrows.
    """

    comptime Written = ValuesWritten
    var arena: Pointer[Arena, Self.origin]
    """The arena holding the values."""
    var generation: Int
    """The arena's call the values came from."""
    var written: ValuesWritten
    """The views the engine wrote."""
    var count: Int
    """Tasks the read carried."""

    def __init__(out self, arena: Address, generation: Int, written: ValuesWritten, count: Int):
        self.arena = Pointer[Arena, Self.origin](unsafe_from_address=arena)
        self.generation = generation
        self.written = written
        self.count = count

    def __len__(self) -> Int:
        """The tasks the read carried."""
        return self.count

    def present(self, index: Int) raises Error -> Bool:
        """Whether task `index` found its value."""
        _readable(self.arena, self.generation, "Values.present")
        return _bit(self.written.presences, index)

    def lengths(ref self) raises Error -> Span[UInt64, origin_of(self)]:
        """The length of every value."""
        _readable(self.arena, self.generation, "Values.lengths")
        return _in_arena[UInt64, origin_of(self)](self.written.lengths, self.count)

    def values(self) -> Tape[Self.origin]:
        """The values, empty for a missing key or when the read skipped the bytes."""
        return Tape[Self.origin](self.arena, self.generation, self.written.values, self.count)


@fieldwise_init
struct KeyRunsWritten(Defaultable, ImplicitlyCopyable):
    """The offsets, keys and scores a scan or a sample writes, at one stable address."""

    var offsets: Address
    var keys: Address
    var scores: Address
    var with_scores: Bool

    def __init__(out self):
        """Nothing written, which is what a failed call leaves."""
        self = Self(NULL_ADDRESS, NULL_ADDRESS, NULL_ADDRESS, False)


@fieldwise_init
struct KeyRuns[origin: MutOrigin](Answer, Sized):
    """The keys a scan or a sample listed into the arena it borrows, run after run, one run per task.

    Parameters:
        origin: The arena the keys live in, which this result borrows.
    """

    comptime Written = KeyRunsWritten
    var arena: Pointer[Arena, Self.origin]
    """The arena holding the keys."""
    var generation: Int
    """The arena's call the keys came from."""
    var written: KeyRunsWritten
    """The views the engine wrote."""
    var count: Int
    """Runs, one per task."""

    def __init__(out self, arena: Address, generation: Int, written: KeyRunsWritten, count: Int):
        self.arena = Pointer[Arena, Self.origin](unsafe_from_address=arena)
        self.generation = generation
        self.written = written
        self.count = count

    def __len__(self) -> Int:
        """The runs, one per task."""
        return self.count

    def offsets(ref self) raises Error -> Span[UInt64, origin_of(self)]:
        """The runs' boundaries in keys, one more of them than runs."""
        _readable(self.arena, self.generation, "KeyRuns.offsets")
        return _in_arena[UInt64, origin_of(self)](self.written.offsets, self.count + 1)

    def keys(ref self) raises Error -> Span[Key, origin_of(self)]:
        """Every run's keys end to end."""
        var offsets = self.offsets()
        return _in_arena[Key, origin_of(self)](self.written.keys, Int(offsets[self.count]) if self.count else 0)

    def run(ref self, index: Int) raises Error -> Span[Key, origin_of(self)]:
        """The keys of task `index`."""
        var offsets = self.offsets()
        var first = Int(offsets[index])
        return _in_arena[Key, origin_of(self)](self.written.keys + first * 16, Int(offsets[index + 1]) - first)

    def scores(ref self) raises Error -> Span[Float32, origin_of(self)]:
        """One score per listed key, empty unless weighted patterns asked for them."""
        var offsets = self.offsets()
        return _in_arena[Float32, origin_of(self)](self.written.scores, Int(offsets[self.count]) if self.count else 0)


@fieldwise_init
struct MeasuresWritten(Defaultable, ImplicitlyCopyable):
    """The six bound arrays a measure writes, at one stable address."""

    var min_cardinalities: Address
    var max_cardinalities: Address
    var min_value_bytes: Address
    var max_value_bytes: Address
    var min_space_usages: Address
    var max_space_usages: Address

    def __init__(out self):
        """Nothing written, which is what a failed call leaves."""
        self = Self(NULL_ADDRESS, NULL_ADDRESS, NULL_ADDRESS, NULL_ADDRESS, NULL_ADDRESS, NULL_ADDRESS)


@fieldwise_init
struct Measures[origin: MutOrigin](Answer, Sized):
    """The bounds a measure estimated into the arena it borrows, one per range.

    Parameters:
        origin: The arena the bounds live in, which this result borrows.
    """

    comptime Written = MeasuresWritten
    var arena: Pointer[Arena, Self.origin]
    """The arena holding the bounds."""
    var generation: Int
    """The arena's call the bounds came from."""
    var written: MeasuresWritten
    """The views the engine wrote."""
    var count: Int
    """Ranges measured."""

    def __init__(out self, arena: Address, generation: Int, written: MeasuresWritten, count: Int):
        self.arena = Pointer[Arena, Self.origin](unsafe_from_address=arena)
        self.generation = generation
        self.written = written
        self.count = count

    def __len__(self) -> Int:
        """The ranges measured."""
        return self.count

    def _bounds(ref self, address: Address) raises Error -> Span[UInt64, origin_of(self)]:
        _readable(self.arena, self.generation, "Measures")
        return _in_arena[UInt64, origin_of(self)](address, self.count)

    def min_cardinalities(ref self) raises Error -> Span[UInt64, origin_of(self)]:
        """The lower bound on the entries in every range."""
        return self._bounds(self.written.min_cardinalities)

    def max_cardinalities(ref self) raises Error -> Span[UInt64, origin_of(self)]:
        """The upper bound on the entries in every range."""
        return self._bounds(self.written.max_cardinalities)

    def min_value_bytes(ref self) raises Error -> Span[UInt64, origin_of(self)]:
        """The lower bound on the bytes of values in every range."""
        return self._bounds(self.written.min_value_bytes)

    def max_value_bytes(ref self) raises Error -> Span[UInt64, origin_of(self)]:
        """The upper bound on the bytes of values in every range."""
        return self._bounds(self.written.max_value_bytes)

    def min_space_usages(ref self) raises Error -> Span[UInt64, origin_of(self)]:
        """The lower bound on the disk space every range takes."""
        return self._bounds(self.written.min_space_usages)

    def max_space_usages(ref self) raises Error -> Span[UInt64, origin_of(self)]:
        """The upper bound on the disk space every range takes."""
        return self._bounds(self.written.max_space_usages)


@fieldwise_init
struct PulledWritten(Defaultable, ImplicitlyCopyable):
    """The keys, values and count one cursor pull writes, at one stable address."""

    var keys: Address
    var values: StringsViewArguments
    var written: c_size_t
    var with_values: Bool

    def __init__(out self):
        """Nothing written, which is what a failed call leaves."""
        self = Self(NULL_ADDRESS, StringsViewArguments(), 0, False)


@fieldwise_init
struct Pulled[origin: MutOrigin](Answer, Sized):
    """One page of a cursor's walk, read in place from the arena the pull borrows.

    Parameters:
        origin: The arena the page lives in, which this result borrows.
    """

    comptime Written = PulledWritten
    var arena: Pointer[Arena, Self.origin]
    """The arena holding the page."""
    var generation: Int
    """The arena's call the page came from."""
    var written: PulledWritten
    """The views the engine wrote."""

    def __init__(out self, arena: Address, generation: Int, written: PulledWritten, count: Int):
        self.arena = Pointer[Arena, Self.origin](unsafe_from_address=arena)
        self.generation = generation
        self.written = written

    def __len__(self) -> Int:
        """The keys pulled, zero once the walk is exhausted."""
        return Int(self.written.written)

    def keys(ref self) raises Error -> Span[Key, origin_of(self)]:
        """The keys of the page in their order."""
        _readable(self.arena, self.generation, "Pulled.keys")
        return _in_arena[Key, origin_of(self)](self.written.keys, len(self))

    def values(self) -> Tape[Self.origin]:
        """Their values, empty when the pull skipped them."""
        return Tape[Self.origin](self.arena, self.generation, self.written.values, len(self))


# endregion Results

# region Configs


struct BlobsReadConfig[offsets_origin: ImmOrigin = ImmUntrackedOrigin, limits_origin: ImmOrigin = ImmUntrackedOrigin](
    ImplicitlyCopyable
):
    """The ranges a read takes of each value, and whether it copies the bytes.

    Parameters:
        offsets_origin: The borrowed start offsets.
        limits_origin: The borrowed byte limits.
    """

    var offsets: LengthsView[Self.offsets_origin]
    """Byte offset into each value the read starts at; zeroed reads whole values."""
    var limits: LengthsView[Self.limits_origin]
    """Bytes to read of each value from its offset; zeroed reads the rest."""
    var values: Bool
    """Whether the bytes come too, or only presences and lengths."""

    def __init__(
        out self,
        *,
        offsets: LengthsView[Self.offsets_origin] = LengthsView[Self.offsets_origin](),
        limits: LengthsView[Self.limits_origin] = LengthsView[Self.limits_origin](),
        values: Bool = True,
    ):
        """Names only the knobs that differ from the defaults."""
        self.offsets = offsets
        self.limits = limits
        self.values = values


struct BlobsScanConfig[end_origin: ImmOrigin = ImmUntrackedOrigin, patterns_origin: ImmOrigin = ImmUntrackedOrigin](
    ImplicitlyCopyable
):
    """The bounds, order and filters of a scan over keys.

    Parameters:
        end_origin: The borrowed end keys.
        patterns_origin: The borrowed patterns.
    """

    var end_keys: KeysView[Self.end_origin]
    """Key every scan stops before, exclusive; zeroed bounds nothing."""
    var direction: ScanDirection
    """Ascending or descending from the start keys."""
    var mode: ScanMode
    """In what order the keys come."""
    var patterns: Patterns[Self.patterns_origin]
    """Patterns the value of every listed key matches; empty lists every key."""
    var predicate: Predicate
    """The caller's verdict on the listed keys."""
    var progress: Progress
    """Reports the scans finished, and stops the call where it says to."""
    var scores: Bool
    """Whether one score per listed key comes too, when the patterns carry weights."""

    def __init__(
        out self,
        *,
        end_keys: KeysView[Self.end_origin] = KeysView[Self.end_origin](),
        direction: ScanDirection = ScanDirection.ASCENDING,
        mode: ScanMode = ScanMode.ORDERED,
        patterns: Patterns[Self.patterns_origin] = Patterns[Self.patterns_origin](),
        predicate: Predicate = Predicate(),
        progress: Progress = Progress(),
        scores: Bool = False,
    ):
        """Names only the knobs that differ from the defaults."""
        self.end_keys = end_keys
        self.direction = direction
        self.mode = mode
        self.patterns = patterns
        self.predicate = predicate
        self.progress = progress
        self.scores = scores


struct BlobsCursorConfig[patterns_origin: ImmOrigin = ImmUntrackedOrigin](ImplicitlyCopyable):
    """The bound, order and filters of a walk over one collection.

    Parameters:
        patterns_origin: The borrowed patterns, compiled once when the walk opens.
    """

    var end_key: Optional[Key]
    """Key the walk stops before, exclusive; absent bounds nothing."""
    var direction: ScanDirection
    """Ascending or descending from the start key."""
    var patterns: Patterns[Self.patterns_origin]
    """Patterns every value the walk hands out matches; empty walks every key."""
    var predicate: Predicate
    """The caller's verdict on the keys of every page."""

    def __init__(
        out self,
        *,
        end_key: Optional[Key] = None,
        direction: ScanDirection = ScanDirection.ASCENDING,
        patterns: Patterns[Self.patterns_origin] = Patterns[Self.patterns_origin](),
        predicate: Predicate = Predicate(),
    ):
        """Names only the knobs that differ from the defaults."""
        self.end_key = end_key
        self.direction = direction
        self.patterns = patterns
        self.predicate = predicate


# endregion Configs

# region Arguments


@fieldwise_init
struct BlobsWriteArguments(Arguments):
    """`ustore_blobs_write_t`."""

    comptime Written = Unwritten
    comptime symbol: StaticString = "ustore_blobs_write"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var transaction: Address
    var count: c_size_t
    var collections: StridedArguments
    var keys: StridedArguments
    var values: StringsViewArguments
    var durability: Durability
    var watch: c_int

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[Unwritten, MutUntrackedOrigin], arena: Address):
        self.database = scope.database
        self.executor = scope.executor
        self.outcome = outcome
        self.completion = completion
        self.transaction = scope.transaction
        self.watch = scope.watch.code

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().blobs_write


@fieldwise_init
struct BlobsRemoveArguments(Arguments):
    """`ustore_blobs_remove_t`."""

    comptime Written = Unwritten
    comptime symbol: StaticString = "ustore_blobs_remove"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var transaction: Address
    var count: c_size_t
    var collections: StridedArguments
    var keys: StridedArguments
    var end_keys: StridedArguments
    var durability: Durability
    var watch: c_int

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[Unwritten, MutUntrackedOrigin], arena: Address):
        self.database = scope.database
        self.executor = scope.executor
        self.outcome = outcome
        self.completion = completion
        self.transaction = scope.transaction
        self.watch = scope.watch.code

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().blobs_remove


@fieldwise_init
struct BlobsReadArguments(Arguments):
    """`ustore_blobs_read_t`."""

    comptime Written = ValuesWritten
    comptime symbol: StaticString = "ustore_blobs_read"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var view: Address
    var count: c_size_t
    var collections: StridedArguments
    var keys: StridedArguments
    var offsets: StridedArguments
    var limits: StridedArguments
    var presences: Optional[Pointer[Address, MutUntrackedOrigin]]
    var lengths: Optional[Pointer[Address, MutUntrackedOrigin]]
    var values: Optional[Pointer[StringsViewArguments, MutUntrackedOrigin]]
    var arena: Address
    var with_values: Bool

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[ValuesWritten, MutUntrackedOrigin], arena: Address):
        self.database = scope.database
        self.executor = scope.executor
        self.outcome = outcome
        self.completion = completion
        self.view = scope.view
        self.presences = Pointer(to=written[].presences)
        self.lengths = Pointer(to=written[].lengths)
        self.values = _slot(self.with_values, Pointer(to=written[].values))
        self.arena = arena

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().blobs_read


@fieldwise_init
struct BlobsScanArguments(Arguments):
    """`ustore_blobs_scan_t`."""

    comptime Written = KeyRunsWritten
    comptime symbol: StaticString = "ustore_blobs_scan"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var view: Address
    var count: c_size_t
    var collections: StridedArguments
    var start_keys: StridedArguments
    var end_keys: StridedArguments
    var direction: ScanDirection
    var patterns: PatternsArguments
    var predicate: Predicate
    var offsets: Optional[Pointer[Address, MutUntrackedOrigin]]
    var keys: Optional[Pointer[Address, MutUntrackedOrigin]]
    var scores: Optional[Pointer[Address, MutUntrackedOrigin]]
    var arena: Address
    var wanted: c_size_t
    var mode: ScanMode
    var progress: Progress
    var with_scores: Bool

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[KeyRunsWritten, MutUntrackedOrigin], arena: Address):
        self.database = scope.database
        self.executor = scope.executor
        self.outcome = outcome
        self.completion = completion
        self.view = scope.view
        self.offsets = Pointer(to=written[].offsets)
        self.keys = Pointer(to=written[].keys)
        self.scores = _slot(self.with_scores, Pointer(to=written[].scores))
        self.arena = arena

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().blobs_scan


@fieldwise_init
struct BlobsCursorInitArguments(ImplicitlyCopyable):
    """`ustore_blobs_cursor_init_t`."""

    var cursor: Optional[Pointer[Address, MutUntrackedOrigin]]
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var view: Address
    var collection: CollectionID
    var start_key: Key
    var end_key: Address
    var direction: ScanDirection
    var patterns: PatternsArguments
    var predicate: Predicate


@fieldwise_init
struct CursorNextArguments[name: StaticString](Arguments):
    """`ustore_blobs_cursor_next_t`, and `ustore_docs_cursor_next_t` with the same layout."""

    comptime Written = PulledWritten
    comptime symbol: StaticString = Self.name
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var cursor: Address
    var keys: Optional[Pointer[Address, MutUntrackedOrigin]]
    var values: Optional[Pointer[StringsViewArguments, MutUntrackedOrigin]]
    var written: Optional[Pointer[c_size_t, MutUntrackedOrigin]]
    var arena: Address
    var wanted: c_size_t
    var progress: Progress
    var with_values: Bool

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[PulledWritten, MutUntrackedOrigin], arena: Address):
        self.database = scope.database
        self.executor = scope.executor
        self.outcome = outcome
        self.completion = completion
        self.keys = Pointer(to=written[].keys)
        self.values = _slot(self.with_values, Pointer(to=written[].values))
        self.written = Pointer(to=written[].written)
        self.arena = arena

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        comptime if Self.name == "ustore_docs_cursor_next":
            return rebind[Verb[Self]](library.symbols().docs_cursor_next)
        else:
            return rebind[Verb[Self]](library.symbols().blobs_cursor_next)


@fieldwise_init
struct SampleArguments[name: StaticString](Arguments):
    """`ustore_blobs_sample_t`, and the documents, graph vertices and pages samples with the same layout."""

    comptime Written = KeyRunsWritten
    comptime symbol: StaticString = Self.name
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var view: Address
    var count: c_size_t
    var collections: StridedArguments
    var offsets: Optional[Pointer[Address, MutUntrackedOrigin]]
    var keys: Optional[Pointer[Address, MutUntrackedOrigin]]
    var arena: Address
    var wanted: c_size_t

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[KeyRunsWritten, MutUntrackedOrigin], arena: Address):
        self.database = scope.database
        self.executor = scope.executor
        self.outcome = outcome
        self.completion = completion
        self.view = scope.view
        self.offsets = Pointer(to=written[].offsets)
        self.keys = Pointer(to=written[].keys)
        self.arena = arena

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        comptime if Self.name == "ustore_docs_sample":
            return rebind[Verb[Self]](library.symbols().docs_sample)
        elif Self.name == "ustore_graph_sample_vertices":
            return rebind[Verb[Self]](library.symbols().graph_sample_vertices)
        elif Self.name == "ustore_pages_sample":
            return rebind[Verb[Self]](library.symbols().pages_sample)
        else:
            return rebind[Verb[Self]](library.symbols().blobs_sample)


@fieldwise_init
struct MeasureArguments[name: StaticString](Arguments):
    """`ustore_blobs_measure_t`, and the documents, graph vertices and pages measures with the same layout."""

    comptime Written = MeasuresWritten
    comptime symbol: StaticString = Self.name
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var view: Address
    var count: c_size_t
    var collections: StridedArguments
    var start_keys: StridedArguments
    var end_keys: StridedArguments
    var direction: ScanDirection
    var min_cardinalities: Optional[Pointer[Address, MutUntrackedOrigin]]
    var max_cardinalities: Optional[Pointer[Address, MutUntrackedOrigin]]
    var min_value_bytes: Optional[Pointer[Address, MutUntrackedOrigin]]
    var max_value_bytes: Optional[Pointer[Address, MutUntrackedOrigin]]
    var min_space_usages: Optional[Pointer[Address, MutUntrackedOrigin]]
    var max_space_usages: Optional[Pointer[Address, MutUntrackedOrigin]]
    var arena: Address

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[MeasuresWritten, MutUntrackedOrigin], arena: Address):
        self.database = scope.database
        self.executor = scope.executor
        self.outcome = outcome
        self.completion = completion
        self.view = scope.view
        self.min_cardinalities = Pointer(to=written[].min_cardinalities)
        self.max_cardinalities = Pointer(to=written[].max_cardinalities)
        self.min_value_bytes = Pointer(to=written[].min_value_bytes)
        self.max_value_bytes = Pointer(to=written[].max_value_bytes)
        self.min_space_usages = Pointer(to=written[].min_space_usages)
        self.max_space_usages = Pointer(to=written[].max_space_usages)
        self.arena = arena

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        comptime if Self.name == "ustore_docs_measure":
            return rebind[Verb[Self]](library.symbols().docs_measure)
        elif Self.name == "ustore_graph_measure_vertices":
            return rebind[Verb[Self]](library.symbols().graph_measure_vertices)
        elif Self.name == "ustore_pages_measure":
            return rebind[Verb[Self]](library.symbols().pages_measure)
        else:
            return rebind[Verb[Self]](library.symbols().blobs_measure)


def _measure[
    name: StaticString
](count: Int, collections: StridedArguments, start_keys: StridedArguments, end_keys: StridedArguments, direction: ScanDirection) -> MeasureArguments[name]:
    """A measure's arguments with its inputs set, the outputs left for `bind`."""
    return MeasureArguments[name](
        NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS, c_size_t(count), collections, start_keys, end_keys,
        direction, None, None, None, None, None, None, NULL_ADDRESS,
    )


# endregion Arguments

# region Cursors


struct BlobsCursor(Movable):
    """A walk over the keys of one collection, every pull resuming past the last key the pull before it
    handed out, freed when it goes out of scope. It holds its database open, and the transaction or
    snapshot it reads through."""

    var library: Library
    """The loaded engine."""
    var database: Address
    """The `ustore_database_t` it walks."""
    var address: Address
    """The `ustore_blobs_cursor_t`."""

    def __init__(out self, *, library: Library, database: Address, address: Address):
        """Adopts a cursor the engine just opened."""
        self.library = library
        self.database = database
        self.address = address

    def __deinit__(deinit self):
        """Frees the cursor and the holds it kept. Calls `ustore_blobs_cursor_free`."""
        self.library.symbols().blobs_cursor_free(self.address)
        _ = self.library^

    def next(
        self, executor: Executor, mut arena: Arena, wanted: Int, values: Bool = True, progress: Progress = Progress()
    ) raises Error -> Pulled[origin_of(arena)]:
        """Pulls the next page of up to `wanted` keys, with their values when asked. Calls `ustore_blobs_cursor_next`.

        Args:
            executor: The executor the pull runs on, of the kind the walk was opened for.
            arena: The arena to rewind and refill, which the result borrows.
            wanted: Most keys the page holds; a page of fewer exhausts the walk.
            values: Whether the values of the page's keys come too.
            progress: Reports the pull, and may cancel it before the cursor advances.

        Raises:
            `Error` with `CANCELLED` when `progress` stops the pull, which leaves the cursor where it was.
        """
        return _block(
            Scope(self.library, self.database, executor.address, NULL_ADDRESS, NULL_ADDRESS, NULL_ADDRESS, Watch.KEYS),
            _Crossing[CursorNextArguments["ustore_blobs_cursor_next"], Pulled[origin_of(arena)], origin_of(arena), ImmUntrackedOrigin](
                CursorNextArguments["ustore_blobs_cursor_next"](
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, self.address, None, None, None, NULL_ADDRESS,
                    c_size_t(wanted), progress, values,
                ),
                Pointer(to=arena),
                0,
            ),
        )


# endregion Cursors

# region Verbs


trait BlobsVerbs:
    """The binary-value verbs every blocking receiver offers, written once over the receiver's scope."""

    def _scope(self) -> Scope:
        """What the receiver addresses and the executor it runs on."""
        ...

    def write[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, values_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        values: StringsView[values_origin],
        durability: Durability = Durability.BUFFERED,
    ) raises Error:
        """Writes a batch of values, one per key; the longest view sets the batch. Calls `ustore_blobs_write`.

        Raises:
            `Error` with `NOT_FOUND` for a missing collection, or a conflict status inside a transaction.
        """
        _ = _block(
            self._scope(),
            _Crossing[BlobsWriteArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[values_origin]()))](
                BlobsWriteArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), keys.tasks(), values.tasks())), collections.arguments(), keys.arguments(),
                    values.arguments(), durability, 0,
                ),
                None,
                0,
            ),
        )

    def remove[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, end_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        end_keys: KeysView[end_origin] = KeysView(),
        durability: Durability = Durability.BUFFERED,
    ) raises Error:
        """Removes a batch of keys with their values, or every key of a range per task when `end_keys` are
        given. Calls `ustore_blobs_remove`."""
        _ = _block(
            self._scope(),
            _Crossing[BlobsRemoveArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[end_origin]()))](
                BlobsRemoveArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), keys.tasks(), end_keys.tasks())), collections.arguments(), keys.arguments(),
                    end_keys.arguments(), durability, 0,
                ),
                None,
                0,
            ),
        )

    def read[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, offsets_origin: ImmOrigin, limits_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        mut arena: Arena,
        config: BlobsReadConfig[offsets_origin, limits_origin] = BlobsReadConfig(),
    ) raises Error -> Values[origin_of(arena)]:
        """Reads a batch of values, their lengths and their presence. Calls `ustore_blobs_read`.

        Raises:
            `Error` with `NOT_FOUND` for a missing collection.
        """
        var count = _tasks(collections.tasks(), keys.tasks(), config.offsets.tasks(), config.limits.tasks())
        return _block(
            self._scope(),
            _Crossing[BlobsReadArguments, Values[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[offsets_origin](), _anchor[limits_origin]()))](
                BlobsReadArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    keys.arguments(), config.offsets.arguments(), config.limits.arguments(), None, None, None, NULL_ADDRESS,
                    config.values,
                ),
                Pointer(to=arena),
                count,
            ),
        )

    def scan[
        collections_origin: ImmOrigin, start_origin: ImmOrigin, end_origin: ImmOrigin, patterns_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        start_keys: KeysView[start_origin],
        wanted: Int,
        mut arena: Arena,
        config: BlobsScanConfig[end_origin, patterns_origin] = BlobsScanConfig(),
    ) raises Error -> KeyRuns[origin_of(arena)]:
        """Lists up to `wanted` keys of a range in every collection, from its start key towards its end key.
        Calls `ustore_blobs_scan`.

        Raises:
            `Error` with `CANCELLED` once `config.progress` stops it, or `INVALID_ARGUMENT` for patterns that
            do not compile.
        """
        var count = _tasks(collections.tasks(), start_keys.tasks(), config.end_keys.tasks())
        return _block(
            self._scope(),
            _Crossing[BlobsScanArguments, KeyRuns[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[start_origin](), _anchor[end_origin](), _anchor[patterns_origin]()))](
                BlobsScanArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    start_keys.arguments(), config.end_keys.arguments(), config.direction, config.patterns.arguments(),
                    config.predicate, None, None, None, NULL_ADDRESS, c_size_t(wanted), config.mode, config.progress, config.scores,
                ),
                Pointer(to=arena),
                count,
            ),
        )

    def cursor[
        patterns_origin: ImmOrigin, //
    ](
        self, collection: CollectionID, start_key: Key, config: BlobsCursorConfig[patterns_origin] = BlobsCursorConfig()
    ) raises Error -> BlobsCursor:
        """Opens a walk over the keys of one collection, reading nothing until its first pull, of the kind
        the receiver's executor computes on. Calls `ustore_blobs_cursor_init`.

        Raises:
            `Error` with `INVALID_ARGUMENT` for patterns that do not compile.
        """
        var scope = self._scope()
        var created = stack_allocation[1, Address]().unsafe_origin_cast[MutUntrackedOrigin]()
        created[] = NULL_ADDRESS
        var outcome = stack_allocation[1, Outcome]().unsafe_origin_cast[MutUntrackedOrigin]()
        outcome[] = Outcome()
        var end = stack_allocation[1, Key]().unsafe_origin_cast[MutUntrackedOrigin]()
        var bounded = False
        if case .Some(key) = config.end_key:
            end[] = key
            bounded = True
        _cross(
            BlobsCursorInitArguments(
                created, scope.database, scope.executor, outcome, scope.view, collection, start_key,
                Int(end) if bounded else NULL_ADDRESS, config.direction, config.patterns.arguments(), config.predicate,
            ),
            scope.library.symbols().blobs_cursor_init,
        )
        return BlobsCursor(library=scope.library, database=scope.database, address=_created(scope.library, outcome, created, "ustore_blobs_cursor_init"))

    def sample[
        collections_origin: ImmOrigin, //
    ](self, collections: CollectionsView[collections_origin], wanted: Int, mut arena: Arena) raises Error -> KeyRuns[origin_of(arena)]:
        """Draws up to `wanted` keys uniformly at random from every collection. Calls `ustore_blobs_sample`."""
        var count = len(collections)
        return _block(
            self._scope(),
            _Crossing[SampleArguments["ustore_blobs_sample"], KeyRuns[origin_of(arena)], origin_of(arena), collections_origin](
                SampleArguments["ustore_blobs_sample"](
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    None, None, NULL_ADDRESS, c_size_t(wanted),
                ),
                Pointer(to=arena),
                count,
            ),
        )

    def measure[
        collections_origin: ImmOrigin, start_origin: ImmOrigin, end_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        start_keys: KeysView[start_origin],
        end_keys: KeysView[end_origin],
        mut arena: Arena,
        direction: ScanDirection = ScanDirection.ASCENDING,
    ) raises Error -> Measures[origin_of(arena)]:
        """Estimates the entries and the space inside a batch of key ranges. Calls `ustore_blobs_measure`."""
        var count = _tasks(collections.tasks(), start_keys.tasks(), end_keys.tasks())
        return _block(
            self._scope(),
            _Crossing[MeasureArguments["ustore_blobs_measure"], Measures[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[start_origin](), _anchor[end_origin]()))](
                _measure["ustore_blobs_measure"](count, collections.arguments(), start_keys.arguments(), end_keys.arguments(), direction),
                Pointer(to=arena),
                count,
            ),
        )


trait BlobsDeferredVerbs:
    """The binary-value verbs every deferred receiver offers, each answering a `Deferred` at once; the
    arguments are those of `BlobsVerbs`."""

    def _scope(self) -> Scope:
        """What the receiver addresses and the executor it submits to."""
        ...

    def write[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, values_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        values: StringsView[values_origin],
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[
        _Crossing[BlobsWriteArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[values_origin]()))]
    ]:
        """Submits `BlobsVerbs.write`. Calls `ustore_blobs_write`."""
        return {
            self._scope(),
            _Crossing[BlobsWriteArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[values_origin]()))](
                BlobsWriteArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), keys.tasks(), values.tasks())), collections.arguments(), keys.arguments(),
                    values.arguments(), durability, 0,
                ),
                None,
                0,
            ),
        }

    def remove[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, end_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        end_keys: KeysView[end_origin] = KeysView(),
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[
        _Crossing[BlobsRemoveArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[end_origin]()))]
    ]:
        """Submits `BlobsVerbs.remove`. Calls `ustore_blobs_remove`."""
        return {
            self._scope(),
            _Crossing[BlobsRemoveArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[end_origin]()))](
                BlobsRemoveArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), keys.tasks(), end_keys.tasks())), collections.arguments(), keys.arguments(),
                    end_keys.arguments(), durability, 0,
                ),
                None,
                0,
            ),
        }

    def read[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, offsets_origin: ImmOrigin, limits_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        mut arena: Arena,
        config: BlobsReadConfig[offsets_origin, limits_origin] = BlobsReadConfig(),
    ) -> Deferred[
        _Crossing[BlobsReadArguments, Values[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[offsets_origin](), _anchor[limits_origin]()))]
    ]:
        """Submits `BlobsVerbs.read`. Calls `ustore_blobs_read`."""
        var count = _tasks(collections.tasks(), keys.tasks(), config.offsets.tasks(), config.limits.tasks())
        return {
            self._scope(),
            _Crossing[BlobsReadArguments, Values[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[offsets_origin](), _anchor[limits_origin]()))](
                BlobsReadArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    keys.arguments(), config.offsets.arguments(), config.limits.arguments(), None, None, None, NULL_ADDRESS,
                    config.values,
                ),
                Pointer(to=arena),
                count,
            ),
        }

    def scan[
        collections_origin: ImmOrigin, start_origin: ImmOrigin, end_origin: ImmOrigin, patterns_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        start_keys: KeysView[start_origin],
        wanted: Int,
        mut arena: Arena,
        config: BlobsScanConfig[end_origin, patterns_origin] = BlobsScanConfig(),
    ) -> Deferred[
        _Crossing[BlobsScanArguments, KeyRuns[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[start_origin](), _anchor[end_origin](), _anchor[patterns_origin]()))]
    ]:
        """Submits `BlobsVerbs.scan`. Calls `ustore_blobs_scan`."""
        var count = _tasks(collections.tasks(), start_keys.tasks(), config.end_keys.tasks())
        return {
            self._scope(),
            _Crossing[BlobsScanArguments, KeyRuns[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[start_origin](), _anchor[end_origin](), _anchor[patterns_origin]()))](
                BlobsScanArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    start_keys.arguments(), config.end_keys.arguments(), config.direction, config.patterns.arguments(),
                    config.predicate, None, None, None, NULL_ADDRESS, c_size_t(wanted), config.mode, config.progress, config.scores,
                ),
                Pointer(to=arena),
                count,
            ),
        }

    def sample[
        collections_origin: ImmOrigin, //
    ](self, collections: CollectionsView[collections_origin], wanted: Int, mut arena: Arena) -> Deferred[
        _Crossing[SampleArguments["ustore_blobs_sample"], KeyRuns[origin_of(arena)], origin_of(arena), collections_origin]
    ]:
        """Submits `BlobsVerbs.sample`. Calls `ustore_blobs_sample`."""
        var count = len(collections)
        return {
            self._scope(),
            _Crossing[SampleArguments["ustore_blobs_sample"], KeyRuns[origin_of(arena)], origin_of(arena), collections_origin](
                SampleArguments["ustore_blobs_sample"](
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    None, None, NULL_ADDRESS, c_size_t(wanted),
                ),
                Pointer(to=arena),
                count,
            ),
        }

    def measure[
        collections_origin: ImmOrigin, start_origin: ImmOrigin, end_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        start_keys: KeysView[start_origin],
        end_keys: KeysView[end_origin],
        mut arena: Arena,
        direction: ScanDirection = ScanDirection.ASCENDING,
    ) -> Deferred[
        _Crossing[MeasureArguments["ustore_blobs_measure"], Measures[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[start_origin](), _anchor[end_origin]()))]
    ]:
        """Submits `BlobsVerbs.measure`. Calls `ustore_blobs_measure`."""
        var count = _tasks(collections.tasks(), start_keys.tasks(), end_keys.tasks())
        return {
            self._scope(),
            _Crossing[MeasureArguments["ustore_blobs_measure"], Measures[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[start_origin](), _anchor[end_origin]()))](
                _measure["ustore_blobs_measure"](count, collections.arguments(), start_keys.arguments(), end_keys.arguments(), direction),
                Pointer(to=arena),
                count,
            ),
        }


# endregion Verbs
