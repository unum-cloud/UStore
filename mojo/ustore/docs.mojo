"""Collections of JSON documents: writes, reads, gists, gathers into columns, removes, scans and cursors.

A field addresses a value inside a document: a top-level key such as "_id", or a JSON Pointer such as
"/user/followers_count", while an empty field addresses the whole document. Reads and writes take
one field per task, and a gather reads every field of every document into typed columns.

File: mojo/ustore/docs.mojo
Author: Ash Vardanian
Date: November 8, 2026
"""

from std.ffi import c_int, c_size_t
from std.memory import stack_allocation
from std.sys.info import size_of

from ustore.blobs import (
    CursorNextArguments,
    KeyRuns,
    KeyRunsWritten,
    MeasureArguments,
    Measures,
    Pulled,
    SampleArguments,
    Values,
    ValuesWritten,
    _measure,
)
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

# region Enumerations


@fieldwise_init
struct FieldType(Equatable, ImplicitlyCopyable, TrivialRegisterPassable):
    """The type of a document, or of a value extracted from one, `ustore_docs_field_type_t`."""

    var code: c_int
    """The raw `ustore_docs_field_type_t` value the ABI exchanges."""
    comptime JSON = Self(0)
    """JSON text."""
    comptime NULL = Self(10)
    """The JSON null."""
    comptime BOOL = Self(11)
    """A boolean."""
    comptime UUID = Self(12)
    """A 128-bit UUID."""
    comptime F16 = Self(40)
    """A 16-bit float."""
    comptime F32 = Self(41)
    """A 32-bit float."""
    comptime F64 = Self(42)
    """A 64-bit float."""
    comptime I8 = Self(20)
    """A signed 8-bit integer."""
    comptime I16 = Self(21)
    """A signed 16-bit integer."""
    comptime I32 = Self(22)
    """A signed 32-bit integer."""
    comptime I64 = Self(23)
    """A signed 64-bit integer."""
    comptime U8 = Self(30)
    """An unsigned 8-bit integer."""
    comptime U16 = Self(31)
    """An unsigned 16-bit integer."""
    comptime U32 = Self(32)
    """An unsigned 32-bit integer."""
    comptime U64 = Self(33)
    """An unsigned 64-bit integer."""
    comptime BINARY = Self(50)
    """The text of a value without a null terminator."""
    comptime STRING = Self(51)
    """A value printed as text."""


@fieldwise_init
struct Modification(Equatable, ImplicitlyCopyable, TrivialRegisterPassable):
    """The modification a documents write applies, `ustore_docs_modification_t`."""

    var code: c_int
    """The raw `ustore_docs_modification_t` value the ABI exchanges."""
    comptime UPSERT = Self(0)
    """Insert or replace."""
    comptime UPDATE = Self(1)
    """Replace an existing document or field."""
    comptime INSERT = Self(2)
    """Insert a document or field that does not exist yet."""
    comptime PATCH = Self(3)
    """Apply an RFC 6902 patch."""
    comptime MERGE = Self(4)
    """Apply an RFC 7386 merge."""


# endregion Enumerations

# region Results


@fieldwise_init
struct GistWritten(Defaultable, ImplicitlyCopyable):
    """The paths and count a gist writes, at one stable address."""

    var fields: StringsViewArguments
    var written: c_size_t

    def __init__(out self):
        """Nothing written, which is what a failed call leaves."""
        self = Self(StringsViewArguments(), 0)


@fieldwise_init
struct Gist[origin: MutOrigin](Answer, Sized):
    """The unique paths to the leaves of the requested documents, read in place from the arena a gist borrows.

    Parameters:
        origin: The arena the paths live in, which this result borrows.
    """

    comptime Written = GistWritten
    var arena: Pointer[Arena, Self.origin]
    """The arena holding the paths."""
    var generation: Int
    """The arena's call the paths came from."""
    var written: GistWritten
    """The views the engine wrote."""

    def __init__(out self, arena: Address, generation: Int, written: GistWritten, count: Int):
        self.arena = Pointer[Arena, Self.origin](unsafe_from_address=arena)
        self.generation = generation
        self.written = written

    def __len__(self) -> Int:
        """The unique paths."""
        return Int(self.written.written)

    def fields(self) -> Tape[Self.origin]:
        """The paths as JSON Pointers."""
        return Tape[Self.origin](self.arena, self.generation, self.written.fields, len(self))


@fieldwise_init
struct ColumnViewArguments(ImplicitlyCopyable):
    """`ustore_docs_column_view_t`, laid out as the ABI declares it."""

    var validities: Address
    var conversions: Address
    var collisions: Address
    var scalars: Address
    var strings: StringsViewArguments


@fieldwise_init
struct ColumnsWritten(Defaultable, ImplicitlyCopyable):
    """The columns a gather writes and how many fields it asked for, at one stable address."""

    var columns: Address
    var fields_count: Int

    def __init__(out self):
        """Nothing written, which is what a failed call leaves."""
        self = Self(NULL_ADDRESS, 0)


@fieldwise_init
struct Column[origin: MutOrigin](ImplicitlyCopyable, Sized):
    """One gathered field over every document, as an Apache Arrow array of its type.

    Parameters:
        origin: The arena the column lives in, which this result borrows.
    """

    var arena: Pointer[Arena, Self.origin]
    """The arena holding the column."""
    var generation: Int
    """The arena's call the column came from."""
    var view: ColumnViewArguments
    """The bitmaps, scalars and strings the engine wrote."""
    var count: Int
    """Rows, one per document."""

    def __len__(self) -> Int:
        """The rows, one per document."""
        return self.count

    def valid(self, row: Int) raises Error -> Bool:
        """Whether the row's value was found and converted."""
        _readable(self.arena, self.generation, "Column.valid")
        return _bit(self.view.validities, row)

    def converted(self, row: Int) raises Error -> Bool:
        """Whether the conversion changed the row's semantics, as parsing a string into an integer does."""
        _readable(self.arena, self.generation, "Column.converted")
        return _bit(self.view.conversions, row)

    def collides(self, row: Int) raises Error -> Bool:
        """Whether a value of an inconvertible type sits under the field in that row."""
        _readable(self.arena, self.generation, "Column.collides")
        return _bit(self.view.collisions, row)

    def scalars[dtype: DType](ref self) raises Error -> Span[Scalar[dtype], origin_of(self)]:
        """The scalars of a scalar column, read as `dtype`, which must match the requested type's width."""
        _readable(self.arena, self.generation, "Column.scalars")
        return _in_arena[Scalar[dtype], origin_of(self)](self.view.scalars, self.count)

    def strings(self) -> Tape[Self.origin]:
        """The strings of a string column."""
        return Tape[Self.origin](self.arena, self.generation, self.view.strings, self.count)


@fieldwise_init
struct Columns[origin: MutOrigin](Answer, Sized):
    """Every gathered field of every document, one column per field, read in place from the arena a gather borrows.

    Parameters:
        origin: The arena the columns live in, which this result borrows.
    """

    comptime Written = ColumnsWritten
    var arena: Pointer[Arena, Self.origin]
    """The arena holding the columns."""
    var generation: Int
    """The arena's call the columns came from."""
    var written: ColumnsWritten
    """The views the engine wrote."""
    var count: Int
    """Rows, one per document."""

    def __init__(out self, arena: Address, generation: Int, written: ColumnsWritten, count: Int):
        self.arena = Pointer[Arena, Self.origin](unsafe_from_address=arena)
        self.generation = generation
        self.written = written
        self.count = count

    def __len__(self) -> Int:
        """The columns, one per field."""
        return self.written.fields_count

    def column(self, field: Int) raises Error -> Column[Self.origin]:
        """The column of field `field`."""
        _readable(self.arena, self.generation, "Columns.column")
        var view = Pointer[ColumnViewArguments, ImmUntrackedOrigin](
            unsafe_from_address=self.written.columns + field * size_of[ColumnViewArguments]()
        )[]
        return Column[Self.origin](self.arena, self.generation, view, self.count)


# endregion Results

# region Configs


struct DocsWriteConfig[fields_origin: ImmOrigin = ImmUntrackedOrigin, id_origin: ImmOrigin = ImmUntrackedOrigin](ImplicitlyCopyable):
    """Where inside the documents a write lands, how its values read, and how it applies.

    Parameters:
        fields_origin: The borrowed fields.
        id_origin: The borrowed name of the key field.
    """

    var fields: StringsView[Self.fields_origin]
    """Fields to modify inside the documents, one per task; zeroed modifies whole documents."""
    var id_field: BytesView[Self.id_origin]
    """Field holding every document's key when no keys are given, such as "_id"."""
    var type: FieldType
    """Format of the written values."""
    var modification: Modification
    """Modification applied to every document."""
    var durability: Durability
    """How durable the writes are once the call finishes at the head."""

    def __init__(
        out self,
        *,
        fields: StringsView[Self.fields_origin] = StringsView[Self.fields_origin](),
        id_field: BytesView[Self.id_origin] = BytesView[Self.id_origin](),
        type: FieldType = FieldType.JSON,
        modification: Modification = Modification.UPSERT,
        durability: Durability = Durability.BUFFERED,
    ):
        """Names only the knobs that differ from the defaults."""
        self.fields = fields
        self.id_field = id_field
        self.type = type
        self.modification = modification
        self.durability = durability


struct DocsReadConfig[fields_origin: ImmOrigin = ImmUntrackedOrigin](ImplicitlyCopyable):
    """Where inside the documents a read looks, and what it exports.

    Parameters:
        fields_origin: The borrowed fields.
    """

    var fields: StringsView[Self.fields_origin]
    """Fields to read inside the documents, one per task; zeroed reads whole documents."""
    var outputs_type: FieldType
    """Format the values are exported in, `JSON` or `STRING`."""
    var values: Bool
    """Whether the bytes come too, or only presences and lengths."""

    def __init__(
        out self,
        *,
        fields: StringsView[Self.fields_origin] = StringsView[Self.fields_origin](),
        outputs_type: FieldType = FieldType.JSON,
        values: Bool = True,
    ):
        """Names only the knobs that differ from the defaults."""
        self.fields = fields
        self.outputs_type = outputs_type
        self.values = values


struct DocsScanConfig[end_origin: ImmOrigin = ImmUntrackedOrigin, filter_origin: ImmOrigin = ImmUntrackedOrigin](ImplicitlyCopyable):
    """The bounds, order and filter of a scan over document keys.

    Parameters:
        end_origin: The borrowed end keys.
        filter_origin: The borrowed filter.
    """

    var end_keys: KeysView[Self.end_origin]
    """Key every scan stops before, exclusive; zeroed bounds nothing."""
    var direction: ScanDirection
    """Ascending or descending from the start keys."""
    var mode: ScanMode
    """In what order the keys come."""
    var filter: BytesView[Self.filter_origin]
    """A JSONPath, or a bare root filter such as "?@.age>30", every listed document matches; zeroed lists every key."""
    var predicate: Predicate
    """The caller's verdict on the listed keys."""
    var progress: Progress
    """Reports the scans finished, and stops the call where it says to."""

    def __init__(
        out self,
        *,
        end_keys: KeysView[Self.end_origin] = KeysView[Self.end_origin](),
        direction: ScanDirection = ScanDirection.ASCENDING,
        mode: ScanMode = ScanMode.ORDERED,
        filter: BytesView[Self.filter_origin] = BytesView[Self.filter_origin](),
        predicate: Predicate = Predicate(),
        progress: Progress = Progress(),
    ):
        """Names only the knobs that differ from the defaults."""
        self.end_keys = end_keys
        self.direction = direction
        self.mode = mode
        self.filter = filter
        self.predicate = predicate
        self.progress = progress


struct DocsCursorConfig[filter_origin: ImmOrigin = ImmUntrackedOrigin](ImplicitlyCopyable):
    """The bound, order and filter of a walk over one collection, the filter copied when the walk opens.

    Parameters:
        filter_origin: The borrowed filter.
    """

    var end_key: Optional[Key]
    """Key the walk stops before, exclusive; absent bounds nothing."""
    var direction: ScanDirection
    """Ascending or descending from the start key."""
    var filter: BytesView[Self.filter_origin]
    """A JSONPath or bare root filter every document the walk hands out matches; zeroed walks every key."""
    var predicate: Predicate
    """The caller's verdict on the keys of every page."""

    def __init__(
        out self,
        *,
        end_key: Optional[Key] = None,
        direction: ScanDirection = ScanDirection.ASCENDING,
        filter: BytesView[Self.filter_origin] = BytesView[Self.filter_origin](),
        predicate: Predicate = Predicate(),
    ):
        """Names only the knobs that differ from the defaults."""
        self.end_key = end_key
        self.direction = direction
        self.filter = filter
        self.predicate = predicate


# endregion Configs

# region Arguments


@fieldwise_init
struct DocsWriteArguments(Arguments):
    """`ustore_docs_write_t`."""

    comptime Written = Unwritten
    comptime symbol: StaticString = "ustore_docs_write"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var transaction: Address
    var count: c_size_t
    var collections: StridedArguments
    var keys: StridedArguments
    var id_field: BytesViewArguments
    var fields: StringsViewArguments
    var values: StringsViewArguments
    var type: FieldType
    var modification: Modification
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
        return library.symbols().docs_write


@fieldwise_init
struct DocsReadArguments(Arguments):
    """`ustore_docs_read_t`."""

    comptime Written = ValuesWritten
    comptime symbol: StaticString = "ustore_docs_read"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var view: Address
    var count: c_size_t
    var collections: StridedArguments
    var keys: StridedArguments
    var fields: StringsViewArguments
    var presences: Optional[Pointer[Address, MutUntrackedOrigin]]
    var lengths: Optional[Pointer[Address, MutUntrackedOrigin]]
    var values: Optional[Pointer[StringsViewArguments, MutUntrackedOrigin]]
    var arena: Address
    var outputs_type: FieldType
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
        return library.symbols().docs_read


@fieldwise_init
struct DocsGistArguments(Arguments):
    """`ustore_docs_gist_t`."""

    comptime Written = GistWritten
    comptime symbol: StaticString = "ustore_docs_gist"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var view: Address
    var count: c_size_t
    var collections: StridedArguments
    var keys: StridedArguments
    var fields: Optional[Pointer[StringsViewArguments, MutUntrackedOrigin]]
    var written: Optional[Pointer[c_size_t, MutUntrackedOrigin]]
    var arena: Address

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[GistWritten, MutUntrackedOrigin], arena: Address):
        self.database = scope.database
        self.executor = scope.executor
        self.outcome = outcome
        self.completion = completion
        self.view = scope.view
        self.fields = Pointer(to=written[].fields)
        self.written = Pointer(to=written[].written)
        self.arena = arena

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().docs_gist


@fieldwise_init
struct DocsGatherArguments(Arguments):
    """`ustore_docs_gather_t`."""

    comptime Written = ColumnsWritten
    comptime symbol: StaticString = "ustore_docs_gather"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var view: Address
    var count: c_size_t
    var collections: StridedArguments
    var keys: StridedArguments
    var fields_count: c_size_t
    var fields: StringsViewArguments
    var columns: Optional[Pointer[Address, MutUntrackedOrigin]]
    var arena: Address
    var outputs_types: Address

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[ColumnsWritten, MutUntrackedOrigin], arena: Address):
        self.database = scope.database
        self.executor = scope.executor
        self.outcome = outcome
        self.completion = completion
        self.view = scope.view
        written[].fields_count = Int(self.fields_count)
        self.columns = Pointer(to=written[].columns)
        self.arena = arena

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().docs_gather


@fieldwise_init
struct DocsRemoveArguments(Arguments):
    """`ustore_docs_remove_t`."""

    comptime Written = Unwritten
    comptime symbol: StaticString = "ustore_docs_remove"
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
        return library.symbols().docs_remove


@fieldwise_init
struct FilteredScanArguments[name: StaticString](Arguments):
    """`ustore_docs_scan_t`, and `ustore_pages_scan_t` with the same layout."""

    comptime Written = KeyRunsWritten
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
    var filter: BytesViewArguments
    var predicate: Predicate
    var offsets: Optional[Pointer[Address, MutUntrackedOrigin]]
    var keys: Optional[Pointer[Address, MutUntrackedOrigin]]
    var arena: Address
    var wanted: c_size_t
    var mode: ScanMode
    var progress: Progress

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
        comptime if Self.name == "ustore_pages_scan":
            return rebind[Verb[Self]](library.symbols().pages_scan)
        else:
            return rebind[Verb[Self]](library.symbols().docs_scan)


@fieldwise_init
struct FilteredCursorInitArguments(ImplicitlyCopyable):
    """`ustore_docs_cursor_init_t`, and `ustore_pages_cursor_init_t` with the same layout."""

    var cursor: Optional[Pointer[Address, MutUntrackedOrigin]]
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var view: Address
    var collection: CollectionID
    var start_key: Key
    var end_key: Address
    var direction: ScanDirection
    var filter: BytesViewArguments
    var predicate: Predicate


def _filtered_scan[
    name: StaticString
](
    count: Int, collections: StridedArguments, start_keys: StridedArguments, end_keys: StridedArguments, direction: ScanDirection,
    filter: BytesViewArguments, predicate: Predicate, wanted: Int, mode: ScanMode, progress: Progress,
) -> FilteredScanArguments[name]:
    """A filtered scan's arguments with its inputs set, the outputs left for `bind`."""
    return FilteredScanArguments[name](
        NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS, c_size_t(count), collections, start_keys, end_keys,
        direction, filter, predicate, None, None, NULL_ADDRESS, c_size_t(wanted), mode, progress,
    )


def _filtered_cursor(scope: Scope, collection: CollectionID, start_key: Key, end_key: Optional[Key], direction: ScanDirection, filter: BytesViewArguments, predicate: Predicate, verb: Verb[FilteredCursorInitArguments], detail: StaticString) raises Error -> Address:
    """Opens a documents or pages walk, answering its handle."""
    var created = stack_allocation[1, Address]().unsafe_origin_cast[MutUntrackedOrigin]()
    created[] = NULL_ADDRESS
    var outcome = stack_allocation[1, Outcome]().unsafe_origin_cast[MutUntrackedOrigin]()
    outcome[] = Outcome()
    var end = stack_allocation[1, Key]().unsafe_origin_cast[MutUntrackedOrigin]()
    var bounded = False
    if case .Some(key) = end_key:
        end[] = key
        bounded = True
    _cross(
        FilteredCursorInitArguments(
            created, scope.database, scope.executor, outcome, scope.view, collection, start_key,
            Int(end) if bounded else NULL_ADDRESS, direction, filter, predicate,
        ),
        verb,
    )
    return _created(scope.library, outcome, created, detail)


# endregion Arguments

# region Cursors


struct DocsCursor(Movable):
    """A walk over the document keys of one collection, every pull resuming past the last key the pull before
    it handed out, freed when it goes out of scope. It holds its database open, and the transaction or
    snapshot it reads through."""

    var library: Library
    """The loaded engine."""
    var database: Address
    """The `ustore_database_t` it walks."""
    var address: Address
    """The `ustore_docs_cursor_t`."""

    def __init__(out self, *, library: Library, database: Address, address: Address):
        """Adopts a cursor the engine just opened."""
        self.library = library
        self.database = database
        self.address = address

    def __deinit__(deinit self):
        """Frees the cursor and the holds it kept. Calls `ustore_docs_cursor_free`."""
        self.library.symbols().docs_cursor_free(self.address)
        _ = self.library^

    def next(
        self, executor: Executor, mut arena: Arena, wanted: Int, values: Bool = True, progress: Progress = Progress()
    ) raises Error -> Pulled[origin_of(arena)]:
        """Pulls the next page of up to `wanted` keys, with the whole documents when asked. Calls `ustore_docs_cursor_next`.

        Raises:
            `Error` with `CANCELLED` when `progress` stops the pull, which leaves the cursor where it was.
        """
        return _block(
            Scope(self.library, self.database, executor.address, NULL_ADDRESS, NULL_ADDRESS, NULL_ADDRESS, Watch.KEYS),
            _Crossing[CursorNextArguments["ustore_docs_cursor_next"], Pulled[origin_of(arena)], origin_of(arena), ImmUntrackedOrigin](
                CursorNextArguments["ustore_docs_cursor_next"](
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, self.address, None, None, None, NULL_ADDRESS,
                    c_size_t(wanted), progress, values,
                ),
                Pointer(to=arena),
                0,
            ),
        )


# endregion Cursors

# region Verbs


trait DocsVerbs:
    """The documents verbs every blocking receiver offers, written once over the receiver's scope."""

    def _scope(self) -> Scope:
        """What the receiver addresses and the executor it runs on."""
        ...

    def docs_write[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, values_origin: ImmOrigin, fields_origin: ImmOrigin, id_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        values: StringsView[values_origin],
        config: DocsWriteConfig[fields_origin, id_origin] = DocsWriteConfig(),
    ) raises Error:
        """Writes whole documents or fields inside them; zeroed `keys` read every key from `config.id_field`.
        Calls `ustore_docs_write`.

        Raises:
            `Error` with `MALFORMED` for a value that is no JSON, or `NOT_FOUND` for a missing collection.
        """
        _ = _block(
            self._scope(),
            _Crossing[DocsWriteArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[values_origin](), _anchor[fields_origin](), _anchor[id_origin]()))](
                DocsWriteArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), keys.tasks(), values.tasks(), config.fields.tasks())), collections.arguments(),
                    keys.arguments(), config.id_field.arguments(), config.fields.arguments(), values.arguments(), config.type,
                    config.modification, config.durability, 0,
                ),
                None,
                0,
            ),
        )

    def docs_read[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, fields_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        mut arena: Arena,
        config: DocsReadConfig[fields_origin] = DocsReadConfig(),
    ) raises Error -> Values[origin_of(arena)]:
        """Reads whole documents or fields inside them. Calls `ustore_docs_read`."""
        var count = _tasks(collections.tasks(), keys.tasks(), config.fields.tasks())
        return _block(
            self._scope(),
            _Crossing[DocsReadArguments, Values[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[fields_origin]()))](
                DocsReadArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    keys.arguments(), config.fields.arguments(), None, None, None, NULL_ADDRESS, config.outputs_type, config.values,
                ),
                Pointer(to=arena),
                count,
            ),
        )

    def docs_gist[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, //
    ](self, collections: CollectionsView[collections_origin], keys: KeysView[keys_origin], mut arena: Arena) raises Error -> Gist[origin_of(arena)]:
        """Lists the unique paths to the leaves of the requested documents. Calls `ustore_docs_gist`."""
        var count = _tasks(collections.tasks(), keys.tasks())
        return _block(
            self._scope(),
            _Crossing[DocsGistArguments, Gist[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin]()))](
                DocsGistArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    keys.arguments(), None, None, NULL_ADDRESS,
                ),
                Pointer(to=arena),
                count,
            ),
        )

    def docs_gather[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, fields_origin: ImmOrigin, types_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        fields: StringsView[fields_origin],
        types: Span[FieldType, types_origin],
        mut arena: Arena,
    ) raises Error -> Columns[origin_of(arena)]:
        """Reads, type-checks and converts every field of every document into one column per field, each
        converted into its entry of `types`. Calls `ustore_docs_gather`."""
        var count = _tasks(collections.tasks(), keys.tasks())
        return _block(
            self._scope(),
            _Crossing[DocsGatherArguments, Columns[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[fields_origin](), _anchor[types_origin]()))](
                DocsGatherArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    keys.arguments(), c_size_t(len(fields)), fields.arguments(), None, NULL_ADDRESS, Int(types.unsafe_ptr()),
                ),
                Pointer(to=arena),
                count,
            ),
        )

    def docs_remove[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, end_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        end_keys: KeysView[end_origin] = KeysView(),
        durability: Durability = Durability.BUFFERED,
    ) raises Error:
        """Removes a batch of whole documents, or every document of a range per task when `end_keys` are given.
        Calls `ustore_docs_remove`."""
        _ = _block(
            self._scope(),
            _Crossing[DocsRemoveArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[end_origin]()))](
                DocsRemoveArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), keys.tasks(), end_keys.tasks())), collections.arguments(), keys.arguments(),
                    end_keys.arguments(), durability, 0,
                ),
                None,
                0,
            ),
        )

    def docs_scan[
        collections_origin: ImmOrigin, start_origin: ImmOrigin, end_origin: ImmOrigin, filter_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        start_keys: KeysView[start_origin],
        wanted: Int,
        mut arena: Arena,
        config: DocsScanConfig[end_origin, filter_origin] = DocsScanConfig(),
    ) raises Error -> KeyRuns[origin_of(arena)]:
        """Lists up to `wanted` document keys from a start key in every collection. Calls `ustore_docs_scan`.

        Raises:
            `Error` with `INVALID_ARGUMENT` for a filter that does not compile.
        """
        var count = _tasks(collections.tasks(), start_keys.tasks(), config.end_keys.tasks())
        return _block(
            self._scope(),
            _Crossing[FilteredScanArguments["ustore_docs_scan"], KeyRuns[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[start_origin](), _anchor[end_origin](), _anchor[filter_origin]()))](
                _filtered_scan["ustore_docs_scan"](
                    count, collections.arguments(), start_keys.arguments(), config.end_keys.arguments(), config.direction,
                    config.filter.arguments(), config.predicate, wanted, config.mode, config.progress,
                ),
                Pointer(to=arena),
                count,
            ),
        )

    def docs_cursor[
        filter_origin: ImmOrigin, //
    ](self, collection: CollectionID, start_key: Key, config: DocsCursorConfig[filter_origin] = DocsCursorConfig()) raises Error -> DocsCursor:
        """Opens a walk over the document keys of one collection, compiling its filter once and reading nothing
        until its first pull. Calls `ustore_docs_cursor_init`."""
        var scope = self._scope()
        var address = _filtered_cursor(
            scope, collection, start_key, config.end_key, config.direction, config.filter.arguments(), config.predicate,
            scope.library.symbols().docs_cursor_init, "ustore_docs_cursor_init",
        )
        return DocsCursor(library=scope.library, database=scope.database, address=address)

    def docs_sample[
        collections_origin: ImmOrigin, //
    ](self, collections: CollectionsView[collections_origin], wanted: Int, mut arena: Arena) raises Error -> KeyRuns[origin_of(arena)]:
        """Draws up to `wanted` document keys uniformly at random from every collection. Calls `ustore_docs_sample`."""
        var count = len(collections)
        return _block(
            self._scope(),
            _Crossing[SampleArguments["ustore_docs_sample"], KeyRuns[origin_of(arena)], origin_of(arena), collections_origin](
                SampleArguments["ustore_docs_sample"](
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    None, None, NULL_ADDRESS, c_size_t(wanted),
                ),
                Pointer(to=arena),
                count,
            ),
        )

    def docs_measure[
        collections_origin: ImmOrigin, start_origin: ImmOrigin, end_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        start_keys: KeysView[start_origin],
        end_keys: KeysView[end_origin],
        mut arena: Arena,
        direction: ScanDirection = ScanDirection.ASCENDING,
    ) raises Error -> Measures[origin_of(arena)]:
        """Estimates the documents and the space inside a batch of key ranges. Calls `ustore_docs_measure`."""
        var count = _tasks(collections.tasks(), start_keys.tasks(), end_keys.tasks())
        return _block(
            self._scope(),
            _Crossing[MeasureArguments["ustore_docs_measure"], Measures[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[start_origin](), _anchor[end_origin]()))](
                _measure["ustore_docs_measure"](count, collections.arguments(), start_keys.arguments(), end_keys.arguments(), direction),
                Pointer(to=arena),
                count,
            ),
        )


trait DocsDeferredVerbs:
    """The documents verbs every deferred receiver offers, each answering a `Deferred` at once; the
    arguments are those of `DocsVerbs`."""

    def _scope(self) -> Scope:
        """What the receiver addresses and the executor it submits to."""
        ...

    def docs_write[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, values_origin: ImmOrigin, fields_origin: ImmOrigin, id_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        values: StringsView[values_origin],
        config: DocsWriteConfig[fields_origin, id_origin] = DocsWriteConfig(),
    ) -> Deferred[
        _Crossing[DocsWriteArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[values_origin](), _anchor[fields_origin](), _anchor[id_origin]()))]
    ]:
        """Submits `DocsVerbs.docs_write`. Calls `ustore_docs_write`."""
        return {
            self._scope(),
            _Crossing[DocsWriteArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[values_origin](), _anchor[fields_origin](), _anchor[id_origin]()))](
                DocsWriteArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), keys.tasks(), values.tasks(), config.fields.tasks())), collections.arguments(),
                    keys.arguments(), config.id_field.arguments(), config.fields.arguments(), values.arguments(), config.type,
                    config.modification, config.durability, 0,
                ),
                None,
                0,
            ),
        }

    def docs_read[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, fields_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        mut arena: Arena,
        config: DocsReadConfig[fields_origin] = DocsReadConfig(),
    ) -> Deferred[
        _Crossing[DocsReadArguments, Values[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[fields_origin]()))]
    ]:
        """Submits `DocsVerbs.docs_read`. Calls `ustore_docs_read`."""
        var count = _tasks(collections.tasks(), keys.tasks(), config.fields.tasks())
        return {
            self._scope(),
            _Crossing[DocsReadArguments, Values[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[fields_origin]()))](
                DocsReadArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    keys.arguments(), config.fields.arguments(), None, None, None, NULL_ADDRESS, config.outputs_type, config.values,
                ),
                Pointer(to=arena),
                count,
            ),
        }

    def docs_gist[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, //
    ](self, collections: CollectionsView[collections_origin], keys: KeysView[keys_origin], mut arena: Arena) -> Deferred[
        _Crossing[DocsGistArguments, Gist[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin]()))]
    ]:
        """Submits `DocsVerbs.docs_gist`. Calls `ustore_docs_gist`."""
        var count = _tasks(collections.tasks(), keys.tasks())
        return {
            self._scope(),
            _Crossing[DocsGistArguments, Gist[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin]()))](
                DocsGistArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    keys.arguments(), None, None, NULL_ADDRESS,
                ),
                Pointer(to=arena),
                count,
            ),
        }

    def docs_gather[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, fields_origin: ImmOrigin, types_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        fields: StringsView[fields_origin],
        types: Span[FieldType, types_origin],
        mut arena: Arena,
    ) -> Deferred[
        _Crossing[DocsGatherArguments, Columns[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[fields_origin](), _anchor[types_origin]()))]
    ]:
        """Submits `DocsVerbs.docs_gather`. Calls `ustore_docs_gather`."""
        var count = _tasks(collections.tasks(), keys.tasks())
        return {
            self._scope(),
            _Crossing[DocsGatherArguments, Columns[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[fields_origin](), _anchor[types_origin]()))](
                DocsGatherArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    keys.arguments(), c_size_t(len(fields)), fields.arguments(), None, NULL_ADDRESS, Int(types.unsafe_ptr()),
                ),
                Pointer(to=arena),
                count,
            ),
        }

    def docs_remove[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, end_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        end_keys: KeysView[end_origin] = KeysView(),
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[
        _Crossing[DocsRemoveArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[end_origin]()))]
    ]:
        """Submits `DocsVerbs.docs_remove`. Calls `ustore_docs_remove`."""
        return {
            self._scope(),
            _Crossing[DocsRemoveArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[end_origin]()))](
                DocsRemoveArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), keys.tasks(), end_keys.tasks())), collections.arguments(), keys.arguments(),
                    end_keys.arguments(), durability, 0,
                ),
                None,
                0,
            ),
        }

    def docs_scan[
        collections_origin: ImmOrigin, start_origin: ImmOrigin, end_origin: ImmOrigin, filter_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        start_keys: KeysView[start_origin],
        wanted: Int,
        mut arena: Arena,
        config: DocsScanConfig[end_origin, filter_origin] = DocsScanConfig(),
    ) -> Deferred[
        _Crossing[FilteredScanArguments["ustore_docs_scan"], KeyRuns[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[start_origin](), _anchor[end_origin](), _anchor[filter_origin]()))]
    ]:
        """Submits `DocsVerbs.docs_scan`. Calls `ustore_docs_scan`."""
        var count = _tasks(collections.tasks(), start_keys.tasks(), config.end_keys.tasks())
        return {
            self._scope(),
            _Crossing[FilteredScanArguments["ustore_docs_scan"], KeyRuns[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[start_origin](), _anchor[end_origin](), _anchor[filter_origin]()))](
                _filtered_scan["ustore_docs_scan"](
                    count, collections.arguments(), start_keys.arguments(), config.end_keys.arguments(), config.direction,
                    config.filter.arguments(), config.predicate, wanted, config.mode, config.progress,
                ),
                Pointer(to=arena),
                count,
            ),
        }

    def docs_sample[
        collections_origin: ImmOrigin, //
    ](self, collections: CollectionsView[collections_origin], wanted: Int, mut arena: Arena) -> Deferred[
        _Crossing[SampleArguments["ustore_docs_sample"], KeyRuns[origin_of(arena)], origin_of(arena), collections_origin]
    ]:
        """Submits `DocsVerbs.docs_sample`. Calls `ustore_docs_sample`."""
        var count = len(collections)
        return {
            self._scope(),
            _Crossing[SampleArguments["ustore_docs_sample"], KeyRuns[origin_of(arena)], origin_of(arena), collections_origin](
                SampleArguments["ustore_docs_sample"](
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    None, None, NULL_ADDRESS, c_size_t(wanted),
                ),
                Pointer(to=arena),
                count,
            ),
        }

    def docs_measure[
        collections_origin: ImmOrigin, start_origin: ImmOrigin, end_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        start_keys: KeysView[start_origin],
        end_keys: KeysView[end_origin],
        mut arena: Arena,
        direction: ScanDirection = ScanDirection.ASCENDING,
    ) -> Deferred[
        _Crossing[MeasureArguments["ustore_docs_measure"], Measures[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[start_origin](), _anchor[end_origin]()))]
    ]:
        """Submits `DocsVerbs.docs_measure`. Calls `ustore_docs_measure`."""
        var count = _tasks(collections.tasks(), start_keys.tasks(), end_keys.tasks())
        return {
            self._scope(),
            _Crossing[MeasureArguments["ustore_docs_measure"], Measures[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[start_origin](), _anchor[end_origin]()))](
                _measure["ustore_docs_measure"](count, collections.arguments(), start_keys.arguments(), end_keys.arguments(), direction),
                Pointer(to=arena),
                count,
            ),
        }


# endregion Verbs
