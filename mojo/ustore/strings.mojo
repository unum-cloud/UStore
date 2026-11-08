"""Collections of keys found by strings: writes, removes, reads, prefix scans, cursors, samples and measures.

A strings collection maps every string onto one key, so a strings verb reads and writes keys where a
blobs verb reads and writes bytes, and scans walk strings in the order of their text. Strings arrive
and leave as Apache Arrow tapes.

File: mojo/ustore/strings.mojo
Author: Ash Vardanian
Date: November 8, 2026
"""

from std.ffi import c_int, c_size_t
from std.memory import stack_allocation

from ustore.blobs import Measures, MeasuresWritten
from ustore.db import (
    CollectionID,
    CollectionsView,
    Durability,
    Key,
    KeysView,
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
    BytesView,
    BytesViewArguments,
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
struct StringRunsWritten(Defaultable, ImplicitlyCopyable):
    """The offsets, strings and scores a strings scan or sample writes, at one stable address."""

    var offsets: Address
    var strings: StringsViewArguments
    var scores: Address
    var with_scores: Bool

    def __init__(out self):
        """Nothing written, which is what a failed call leaves."""
        self = Self(NULL_ADDRESS, StringsViewArguments(), NULL_ADDRESS, False)


@fieldwise_init
struct StringRuns[origin: MutOrigin](Answer, Sized):
    """The strings a scan or a sample listed into the arena it borrows, run after run, one run per task.

    Parameters:
        origin: The arena the strings live in, which this result borrows.
    """

    comptime Written = StringRunsWritten
    var arena: Pointer[Arena, Self.origin]
    """The arena holding the strings."""
    var generation: Int
    """The arena's call the strings came from."""
    var written: StringRunsWritten
    """The views the engine wrote."""
    var count: Int
    """Runs, one per task."""

    def __init__(out self, arena: Address, generation: Int, written: StringRunsWritten, count: Int):
        self.arena = Pointer[Arena, Self.origin](unsafe_from_address=arena)
        self.generation = generation
        self.written = written
        self.count = count

    def __len__(self) -> Int:
        """The runs, one per task."""
        return self.count

    def offsets(ref self) raises Error -> Span[UInt64, origin_of(self)]:
        """The runs' boundaries in strings, one more of them than runs."""
        _readable(self.arena, self.generation, "StringRuns.offsets")
        return _in_arena[UInt64, origin_of(self)](self.written.offsets, self.count + 1)

    def strings(self) raises Error -> Tape[Self.origin]:
        """Every run's strings end to end; run `i` spans entries `offsets()[i]` up to `offsets()[i + 1]`."""
        var offsets = self.offsets()
        return Tape[Self.origin](self.arena, self.generation, self.written.strings, Int(offsets[self.count]) if self.count else 0)

    def scores(ref self) raises Error -> Span[Float32, origin_of(self)]:
        """One score per listed string, empty unless weighted patterns asked for them."""
        var offsets = self.offsets()
        return _in_arena[Float32, origin_of(self)](self.written.scores, Int(offsets[self.count]) if self.count else 0)


@fieldwise_init
struct StringKeysWritten(Defaultable, ImplicitlyCopyable):
    """The presences and keys a strings read writes, at one stable address."""

    var presences: Address
    var values: Address

    def __init__(out self):
        """Nothing written, which is what a failed call leaves."""
        self = Self(NULL_ADDRESS, NULL_ADDRESS)


@fieldwise_init
struct StringKeys[origin: MutOrigin](Answer, Sized):
    """The keys a batch of strings map onto, read in place from the arena a strings read borrows.

    Parameters:
        origin: The arena the keys live in, which this result borrows.
    """

    comptime Written = StringKeysWritten
    var arena: Pointer[Arena, Self.origin]
    """The arena holding the keys."""
    var generation: Int
    """The arena's call the keys came from."""
    var written: StringKeysWritten
    """The views the engine wrote."""
    var count: Int
    """Strings the read carried."""

    def __init__(out self, arena: Address, generation: Int, written: StringKeysWritten, count: Int):
        self.arena = Pointer[Arena, Self.origin](unsafe_from_address=arena)
        self.generation = generation
        self.written = written
        self.count = count

    def __len__(self) -> Int:
        """The strings the read carried."""
        return self.count

    def present(self, index: Int) raises Error -> Bool:
        """Whether string `index` is present."""
        _readable(self.arena, self.generation, "StringKeys.present")
        return _bit(self.written.presences, index)

    def keys(ref self) raises Error -> Span[Key, origin_of(self)]:
        """The key every string maps onto, nil for a missing one."""
        _readable(self.arena, self.generation, "StringKeys.keys")
        return _in_arena[Key, origin_of(self)](self.written.values, self.count)


@fieldwise_init
struct StringsPulledWritten(Defaultable, ImplicitlyCopyable):
    """The strings, keys and count one strings cursor pull writes, at one stable address."""

    var strings: StringsViewArguments
    var values: Address
    var written: c_size_t
    var with_values: Bool

    def __init__(out self):
        """Nothing written, which is what a failed call leaves."""
        self = Self(StringsViewArguments(), NULL_ADDRESS, 0, False)


@fieldwise_init
struct StringsPulled[origin: MutOrigin](Answer, Sized):
    """One page of a strings cursor's walk, read in place from the arena the pull borrows.

    Parameters:
        origin: The arena the page lives in, which this result borrows.
    """

    comptime Written = StringsPulledWritten
    var arena: Pointer[Arena, Self.origin]
    """The arena holding the page."""
    var generation: Int
    """The arena's call the page came from."""
    var written: StringsPulledWritten
    """The views the engine wrote."""

    def __init__(out self, arena: Address, generation: Int, written: StringsPulledWritten, count: Int):
        self.arena = Pointer[Arena, Self.origin](unsafe_from_address=arena)
        self.generation = generation
        self.written = written

    def __len__(self) -> Int:
        """The strings pulled, zero once the walk is exhausted."""
        return Int(self.written.written)

    def strings(self) -> Tape[Self.origin]:
        """The strings of the page in the order of their text."""
        return Tape[Self.origin](self.arena, self.generation, self.written.strings, len(self))

    def keys(ref self) raises Error -> Span[Key, origin_of(self)]:
        """The keys of the page's strings, empty when the pull skipped them."""
        _readable(self.arena, self.generation, "StringsPulled.keys")
        return _in_arena[Key, origin_of(self)](self.written.values, len(self))


# endregion Results

# region Configs


struct StringsScanConfig[
    start_origin: ImmOrigin = ImmUntrackedOrigin, end_origin: ImmOrigin = ImmUntrackedOrigin, patterns_origin: ImmOrigin = ImmUntrackedOrigin
](ImplicitlyCopyable):
    """The bounds, order and filters of a prefix scan.

    Parameters:
        start_origin: The borrowed start strings.
        end_origin: The borrowed end strings.
        patterns_origin: The borrowed patterns.
    """

    var start_strings: StringsView[Self.start_origin]
    """String every scan starts from, inclusive; zeroed starts at the first string in the direction."""
    var end_strings: StringsView[Self.end_origin]
    """String every scan stops before, exclusive; empty or zeroed bounds nothing."""
    var direction: ScanDirection
    """Ascending or descending from the start string."""
    var mode: ScanMode
    """In what order the strings come."""
    var patterns: Patterns[Self.patterns_origin]
    """Patterns every listed string matches; empty lists every string."""
    var predicate: Predicate
    """The caller's verdict on the keys the listed strings map onto."""
    var progress: Progress
    """Reports the scans finished, and stops the call where it says to."""
    var scores: Bool
    """Whether one score per listed string comes too, when the patterns carry weights."""

    def __init__(
        out self,
        *,
        start_strings: StringsView[Self.start_origin] = StringsView[Self.start_origin](),
        end_strings: StringsView[Self.end_origin] = StringsView[Self.end_origin](),
        direction: ScanDirection = ScanDirection.ASCENDING,
        mode: ScanMode = ScanMode.ORDERED,
        patterns: Patterns[Self.patterns_origin] = Patterns[Self.patterns_origin](),
        predicate: Predicate = Predicate(),
        progress: Progress = Progress(),
        scores: Bool = False,
    ):
        """Names only the knobs that differ from the defaults."""
        self.start_strings = start_strings
        self.end_strings = end_strings
        self.direction = direction
        self.mode = mode
        self.patterns = patterns
        self.predicate = predicate
        self.progress = progress
        self.scores = scores


struct StringsCursorConfig[
    start_origin: ImmOrigin = ImmUntrackedOrigin, end_origin: ImmOrigin = ImmUntrackedOrigin, patterns_origin: ImmOrigin = ImmUntrackedOrigin
](ImplicitlyCopyable):
    """The bounds, order and filters of a walk over one prefix, every string copied when the walk opens.

    Parameters:
        start_origin: The borrowed start string.
        end_origin: The borrowed end string.
        patterns_origin: The borrowed patterns.
    """

    var start_string: BytesView[Self.start_origin]
    """String the walk starts from, inclusive; zeroed starts at the first string of the prefix."""
    var end_string: BytesView[Self.end_origin]
    """String the walk stops before, exclusive; zeroed bounds nothing."""
    var direction: ScanDirection
    """Ascending or descending from the start string."""
    var patterns: Patterns[Self.patterns_origin]
    """Patterns every string the walk hands out matches; empty walks every string."""
    var predicate: Predicate
    """The caller's verdict on the keys of the strings of every page."""

    def __init__(
        out self,
        *,
        start_string: BytesView[Self.start_origin] = BytesView[Self.start_origin](),
        end_string: BytesView[Self.end_origin] = BytesView[Self.end_origin](),
        direction: ScanDirection = ScanDirection.ASCENDING,
        patterns: Patterns[Self.patterns_origin] = Patterns[Self.patterns_origin](),
        predicate: Predicate = Predicate(),
    ):
        """Names only the knobs that differ from the defaults."""
        self.start_string = start_string
        self.end_string = end_string
        self.direction = direction
        self.patterns = patterns
        self.predicate = predicate


# endregion Configs

# region Arguments


@fieldwise_init
struct StringsWriteArguments(Arguments):
    """`ustore_strings_write_t`."""

    comptime Written = Unwritten
    comptime symbol: StaticString = "ustore_strings_write"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var transaction: Address
    var count: c_size_t
    var collections: StridedArguments
    var strings: StringsViewArguments
    var values: StridedArguments
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
        return library.symbols().strings_write


@fieldwise_init
struct StringsRemoveArguments(Arguments):
    """`ustore_strings_remove_t`."""

    comptime Written = Unwritten
    comptime symbol: StaticString = "ustore_strings_remove"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var transaction: Address
    var count: c_size_t
    var collections: StridedArguments
    var strings: StringsViewArguments
    var end_strings: StringsViewArguments
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
        return library.symbols().strings_remove


@fieldwise_init
struct StringsReadArguments(Arguments):
    """`ustore_strings_read_t`."""

    comptime Written = StringKeysWritten
    comptime symbol: StaticString = "ustore_strings_read"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var view: Address
    var count: c_size_t
    var collections: StridedArguments
    var strings: StringsViewArguments
    var presences: Optional[Pointer[Address, MutUntrackedOrigin]]
    var values: Optional[Pointer[Address, MutUntrackedOrigin]]
    var arena: Address

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[StringKeysWritten, MutUntrackedOrigin], arena: Address):
        self.database = scope.database
        self.executor = scope.executor
        self.outcome = outcome
        self.completion = completion
        self.view = scope.view
        self.presences = Pointer(to=written[].presences)
        self.values = Pointer(to=written[].values)
        self.arena = arena

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().strings_read


@fieldwise_init
struct StringsScanArguments(Arguments):
    """`ustore_strings_scan_t`."""

    comptime Written = StringRunsWritten
    comptime symbol: StaticString = "ustore_strings_scan"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var view: Address
    var count: c_size_t
    var collections: StridedArguments
    var prefixes: StringsViewArguments
    var start_strings: StringsViewArguments
    var end_strings: StringsViewArguments
    var direction: ScanDirection
    var patterns: PatternsArguments
    var predicate: Predicate
    var offsets: Optional[Pointer[Address, MutUntrackedOrigin]]
    var strings: Optional[Pointer[StringsViewArguments, MutUntrackedOrigin]]
    var scores: Optional[Pointer[Address, MutUntrackedOrigin]]
    var arena: Address
    var wanted: c_size_t
    var mode: ScanMode
    var progress: Progress
    var with_scores: Bool

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[StringRunsWritten, MutUntrackedOrigin], arena: Address):
        self.database = scope.database
        self.executor = scope.executor
        self.outcome = outcome
        self.completion = completion
        self.view = scope.view
        self.offsets = Pointer(to=written[].offsets)
        self.strings = Pointer(to=written[].strings)
        self.scores = _slot(self.with_scores, Pointer(to=written[].scores))
        self.arena = arena

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().strings_scan


@fieldwise_init
struct StringsCursorInitArguments(ImplicitlyCopyable):
    """`ustore_strings_cursor_init_t`."""

    var cursor: Optional[Pointer[Address, MutUntrackedOrigin]]
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var view: Address
    var collection: CollectionID
    var prefix: BytesViewArguments
    var start_string: BytesViewArguments
    var end_string: BytesViewArguments
    var direction: ScanDirection
    var patterns: PatternsArguments
    var predicate: Predicate


@fieldwise_init
struct StringsCursorNextArguments(Arguments):
    """`ustore_strings_cursor_next_t`."""

    comptime Written = StringsPulledWritten
    comptime symbol: StaticString = "ustore_strings_cursor_next"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var cursor: Address
    var strings: Optional[Pointer[StringsViewArguments, MutUntrackedOrigin]]
    var values: Optional[Pointer[Address, MutUntrackedOrigin]]
    var written: Optional[Pointer[c_size_t, MutUntrackedOrigin]]
    var arena: Address
    var wanted: c_size_t
    var progress: Progress
    var with_values: Bool

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[StringsPulledWritten, MutUntrackedOrigin], arena: Address):
        self.database = scope.database
        self.executor = scope.executor
        self.outcome = outcome
        self.completion = completion
        self.strings = Pointer(to=written[].strings)
        self.values = _slot(self.with_values, Pointer(to=written[].values))
        self.written = Pointer(to=written[].written)
        self.arena = arena

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().strings_cursor_next


@fieldwise_init
struct StringsSampleArguments(Arguments):
    """`ustore_strings_sample_t`."""

    comptime Written = StringRunsWritten
    comptime symbol: StaticString = "ustore_strings_sample"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var view: Address
    var count: c_size_t
    var collections: StridedArguments
    var offsets: Optional[Pointer[Address, MutUntrackedOrigin]]
    var strings: Optional[Pointer[StringsViewArguments, MutUntrackedOrigin]]
    var arena: Address
    var wanted: c_size_t

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[StringRunsWritten, MutUntrackedOrigin], arena: Address):
        self.database = scope.database
        self.executor = scope.executor
        self.outcome = outcome
        self.completion = completion
        self.view = scope.view
        self.offsets = Pointer(to=written[].offsets)
        self.strings = Pointer(to=written[].strings)
        self.arena = arena

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().strings_sample


@fieldwise_init
struct StringsMeasureArguments(Arguments):
    """`ustore_strings_measure_t`."""

    comptime Written = MeasuresWritten
    comptime symbol: StaticString = "ustore_strings_measure"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var view: Address
    var count: c_size_t
    var collections: StridedArguments
    var start_strings: StringsViewArguments
    var end_strings: StringsViewArguments
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
        return library.symbols().strings_measure


# endregion Arguments

# region Cursors


struct StringsCursor(Movable):
    """A walk over the strings of one prefix, every pull resuming past the last string the pull before it
    handed out, freed when it goes out of scope. It holds its database open, and the transaction or
    snapshot it reads through."""

    var library: Library
    """The loaded engine."""
    var database: Address
    """The `ustore_database_t` it walks."""
    var address: Address
    """The `ustore_strings_cursor_t`."""

    def __init__(out self, *, library: Library, database: Address, address: Address):
        """Adopts a cursor the engine just opened."""
        self.library = library
        self.database = database
        self.address = address

    def __deinit__(deinit self):
        """Frees the cursor and the holds it kept. Calls `ustore_strings_cursor_free`."""
        self.library.symbols().strings_cursor_free(self.address)
        _ = self.library^

    def next(
        self, executor: Executor, mut arena: Arena, wanted: Int, values: Bool = True, progress: Progress = Progress()
    ) raises Error -> StringsPulled[origin_of(arena)]:
        """Pulls the next page of up to `wanted` strings, with their keys when asked. Calls `ustore_strings_cursor_next`.

        Raises:
            `Error` with `CANCELLED` when `progress` stops the pull, which leaves the cursor where it was.
        """
        return _block(
            Scope(self.library, self.database, executor.address, NULL_ADDRESS, NULL_ADDRESS, NULL_ADDRESS, Watch.KEYS),
            _Crossing[StringsCursorNextArguments, StringsPulled[origin_of(arena)], origin_of(arena), ImmUntrackedOrigin](
                StringsCursorNextArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, self.address, None, None, None, NULL_ADDRESS,
                    c_size_t(wanted), progress, values,
                ),
                Pointer(to=arena),
                0,
            ),
        )


# endregion Cursors

# region Verbs


trait StringsVerbs:
    """The strings verbs every blocking receiver offers, written once over the receiver's scope."""

    def _scope(self) -> Scope:
        """What the receiver addresses and the executor it runs on."""
        ...

    def strings_write[
        collections_origin: ImmOrigin, strings_origin: ImmOrigin, values_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        strings: StringsView[strings_origin],
        values: KeysView[values_origin],
        durability: Durability = Durability.BUFFERED,
    ) raises Error:
        """Writes a batch of strings and the keys they map onto. Calls `ustore_strings_write`.

        Raises:
            `Error` with `NOT_FOUND` for a missing collection, or a conflict status inside a transaction.
        """
        _ = _block(
            self._scope(),
            _Crossing[StringsWriteArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[strings_origin](), _anchor[values_origin]()))](
                StringsWriteArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), strings.tasks(), values.tasks())), collections.arguments(), strings.arguments(),
                    values.arguments(), durability, 0,
                ),
                None,
                0,
            ),
        )

    def strings_remove[
        collections_origin: ImmOrigin, strings_origin: ImmOrigin, end_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        strings: StringsView[strings_origin],
        end_strings: StringsView[end_origin] = StringsView(),
        durability: Durability = Durability.BUFFERED,
    ) raises Error:
        """Removes a batch of strings with their keys, or every string of a range per task when `end_strings`
        are given. Calls `ustore_strings_remove`."""
        _ = _block(
            self._scope(),
            _Crossing[StringsRemoveArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[strings_origin](), _anchor[end_origin]()))](
                StringsRemoveArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), strings.tasks(), end_strings.tasks())), collections.arguments(), strings.arguments(),
                    end_strings.arguments(), durability, 0,
                ),
                None,
                0,
            ),
        )

    def strings_read[
        collections_origin: ImmOrigin, strings_origin: ImmOrigin, //
    ](self, collections: CollectionsView[collections_origin], strings: StringsView[strings_origin], mut arena: Arena) raises Error -> StringKeys[
        origin_of(arena)
    ]:
        """Reads the keys a batch of strings map onto, and their presence. Calls `ustore_strings_read`."""
        var count = _tasks(collections.tasks(), strings.tasks())
        return _block(
            self._scope(),
            _Crossing[StringsReadArguments, StringKeys[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[strings_origin]()))](
                StringsReadArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    strings.arguments(), None, None, NULL_ADDRESS,
                ),
                Pointer(to=arena),
                count,
            ),
        )

    def strings_scan[
        collections_origin: ImmOrigin, prefixes_origin: ImmOrigin, start_origin: ImmOrigin, end_origin: ImmOrigin, patterns_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        prefixes: StringsView[prefixes_origin],
        wanted: Int,
        mut arena: Arena,
        config: StringsScanConfig[start_origin, end_origin, patterns_origin] = StringsScanConfig(),
    ) raises Error -> StringRuns[origin_of(arena)]:
        """Lists up to `wanted` strings under every prefix, in the order of their text. Calls `ustore_strings_scan`.

        Raises:
            `Error` with `CANCELLED` once `config.progress` stops it.
        """
        var count = _tasks(collections.tasks(), prefixes.tasks(), config.start_strings.tasks(), config.end_strings.tasks())
        return _block(
            self._scope(),
            _Crossing[StringsScanArguments, StringRuns[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[prefixes_origin](), _anchor[start_origin](), _anchor[end_origin](), _anchor[patterns_origin]()))](
                StringsScanArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    prefixes.arguments(), config.start_strings.arguments(), config.end_strings.arguments(), config.direction,
                    config.patterns.arguments(), config.predicate, None, None, None, NULL_ADDRESS, c_size_t(wanted), config.mode,
                    config.progress, config.scores,
                ),
                Pointer(to=arena),
                count,
            ),
        )

    def strings_cursor[
        prefix_origin: ImmOrigin, start_origin: ImmOrigin, end_origin: ImmOrigin, patterns_origin: ImmOrigin, //
    ](
        self,
        collection: CollectionID,
        prefix: StringSlice[prefix_origin],
        config: StringsCursorConfig[start_origin, end_origin, patterns_origin] = StringsCursorConfig(),
    ) raises Error -> StringsCursor:
        """Opens a walk over the strings of one prefix, empty for every string, reading nothing until its first
        pull. Calls `ustore_strings_cursor_init`.

        Raises:
            `Error` with `INVALID_ARGUMENT` for patterns that do not compile.
        """
        var scope = self._scope()
        var created = stack_allocation[1, Address]().unsafe_origin_cast[MutUntrackedOrigin]()
        created[] = NULL_ADDRESS
        var outcome = stack_allocation[1, Outcome]().unsafe_origin_cast[MutUntrackedOrigin]()
        outcome[] = Outcome()
        var prefixed = BytesView.of(prefix).arguments() if prefix.byte_length() else BytesViewArguments.empty()
        _cross(
            StringsCursorInitArguments(
                created, scope.database, scope.executor, outcome, scope.view, collection, prefixed,
                config.start_string.arguments(), config.end_string.arguments(), config.direction, config.patterns.arguments(),
                config.predicate,
            ),
            scope.library.symbols().strings_cursor_init,
        )
        return StringsCursor(library=scope.library, database=scope.database, address=_created(scope.library, outcome, created, "ustore_strings_cursor_init"))

    def strings_sample[
        collections_origin: ImmOrigin, //
    ](self, collections: CollectionsView[collections_origin], wanted: Int, mut arena: Arena) raises Error -> StringRuns[origin_of(arena)]:
        """Draws up to `wanted` strings uniformly at random from every collection. Calls `ustore_strings_sample`."""
        var count = len(collections)
        return _block(
            self._scope(),
            _Crossing[StringsSampleArguments, StringRuns[origin_of(arena)], origin_of(arena), collections_origin](
                StringsSampleArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    None, None, NULL_ADDRESS, c_size_t(wanted),
                ),
                Pointer(to=arena),
                count,
            ),
        )

    def strings_measure[
        collections_origin: ImmOrigin, start_origin: ImmOrigin, end_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        start_strings: StringsView[start_origin],
        end_strings: StringsView[end_origin],
        mut arena: Arena,
        direction: ScanDirection = ScanDirection.ASCENDING,
    ) raises Error -> Measures[origin_of(arena)]:
        """Estimates the strings and the space inside a batch of string ranges. Calls `ustore_strings_measure`."""
        var count = _tasks(collections.tasks(), start_strings.tasks(), end_strings.tasks())
        return _block(
            self._scope(),
            _Crossing[StringsMeasureArguments, Measures[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[start_origin](), _anchor[end_origin]()))](
                StringsMeasureArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    start_strings.arguments(), end_strings.arguments(), direction, None, None, None, None, None, None, NULL_ADDRESS,
                ),
                Pointer(to=arena),
                count,
            ),
        )


trait StringsDeferredVerbs:
    """The strings verbs every deferred receiver offers, each answering a `Deferred` at once; the arguments
    are those of `StringsVerbs`."""

    def _scope(self) -> Scope:
        """What the receiver addresses and the executor it submits to."""
        ...

    def strings_write[
        collections_origin: ImmOrigin, strings_origin: ImmOrigin, values_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        strings: StringsView[strings_origin],
        values: KeysView[values_origin],
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[
        _Crossing[StringsWriteArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[strings_origin](), _anchor[values_origin]()))]
    ]:
        """Submits `StringsVerbs.strings_write`. Calls `ustore_strings_write`."""
        return {
            self._scope(),
            _Crossing[StringsWriteArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[strings_origin](), _anchor[values_origin]()))](
                StringsWriteArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), strings.tasks(), values.tasks())), collections.arguments(), strings.arguments(),
                    values.arguments(), durability, 0,
                ),
                None,
                0,
            ),
        }

    def strings_remove[
        collections_origin: ImmOrigin, strings_origin: ImmOrigin, end_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        strings: StringsView[strings_origin],
        end_strings: StringsView[end_origin] = StringsView(),
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[
        _Crossing[StringsRemoveArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[strings_origin](), _anchor[end_origin]()))]
    ]:
        """Submits `StringsVerbs.strings_remove`. Calls `ustore_strings_remove`."""
        return {
            self._scope(),
            _Crossing[StringsRemoveArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[strings_origin](), _anchor[end_origin]()))](
                StringsRemoveArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), strings.tasks(), end_strings.tasks())), collections.arguments(), strings.arguments(),
                    end_strings.arguments(), durability, 0,
                ),
                None,
                0,
            ),
        }

    def strings_read[
        collections_origin: ImmOrigin, strings_origin: ImmOrigin, //
    ](self, collections: CollectionsView[collections_origin], strings: StringsView[strings_origin], mut arena: Arena) -> Deferred[
        _Crossing[StringsReadArguments, StringKeys[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[strings_origin]()))]
    ]:
        """Submits `StringsVerbs.strings_read`. Calls `ustore_strings_read`."""
        var count = _tasks(collections.tasks(), strings.tasks())
        return {
            self._scope(),
            _Crossing[StringsReadArguments, StringKeys[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[strings_origin]()))](
                StringsReadArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    strings.arguments(), None, None, NULL_ADDRESS,
                ),
                Pointer(to=arena),
                count,
            ),
        }

    def strings_scan[
        collections_origin: ImmOrigin, prefixes_origin: ImmOrigin, start_origin: ImmOrigin, end_origin: ImmOrigin, patterns_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        prefixes: StringsView[prefixes_origin],
        wanted: Int,
        mut arena: Arena,
        config: StringsScanConfig[start_origin, end_origin, patterns_origin] = StringsScanConfig(),
    ) -> Deferred[
        _Crossing[StringsScanArguments, StringRuns[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[prefixes_origin](), _anchor[start_origin](), _anchor[end_origin](), _anchor[patterns_origin]()))]
    ]:
        """Submits `StringsVerbs.strings_scan`. Calls `ustore_strings_scan`."""
        var count = _tasks(collections.tasks(), prefixes.tasks(), config.start_strings.tasks(), config.end_strings.tasks())
        return {
            self._scope(),
            _Crossing[StringsScanArguments, StringRuns[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[prefixes_origin](), _anchor[start_origin](), _anchor[end_origin](), _anchor[patterns_origin]()))](
                StringsScanArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    prefixes.arguments(), config.start_strings.arguments(), config.end_strings.arguments(), config.direction,
                    config.patterns.arguments(), config.predicate, None, None, None, NULL_ADDRESS, c_size_t(wanted), config.mode,
                    config.progress, config.scores,
                ),
                Pointer(to=arena),
                count,
            ),
        }

    def strings_sample[
        collections_origin: ImmOrigin, //
    ](self, collections: CollectionsView[collections_origin], wanted: Int, mut arena: Arena) -> Deferred[
        _Crossing[StringsSampleArguments, StringRuns[origin_of(arena)], origin_of(arena), collections_origin]
    ]:
        """Submits `StringsVerbs.strings_sample`. Calls `ustore_strings_sample`."""
        var count = len(collections)
        return {
            self._scope(),
            _Crossing[StringsSampleArguments, StringRuns[origin_of(arena)], origin_of(arena), collections_origin](
                StringsSampleArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    None, None, NULL_ADDRESS, c_size_t(wanted),
                ),
                Pointer(to=arena),
                count,
            ),
        }

    def strings_measure[
        collections_origin: ImmOrigin, start_origin: ImmOrigin, end_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        start_strings: StringsView[start_origin],
        end_strings: StringsView[end_origin],
        mut arena: Arena,
        direction: ScanDirection = ScanDirection.ASCENDING,
    ) -> Deferred[
        _Crossing[StringsMeasureArguments, Measures[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[start_origin](), _anchor[end_origin]()))]
    ]:
        """Submits `StringsVerbs.strings_measure`. Calls `ustore_strings_measure`."""
        var count = _tasks(collections.tasks(), start_strings.tasks(), end_strings.tasks())
        return {
            self._scope(),
            _Crossing[StringsMeasureArguments, Measures[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[start_origin](), _anchor[end_origin]()))](
                StringsMeasureArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    start_strings.arguments(), end_strings.arguments(), direction, None, None, None, None, None, None, NULL_ADDRESS,
                ),
                Pointer(to=arena),
                count,
            ),
        }


# endregion Verbs
