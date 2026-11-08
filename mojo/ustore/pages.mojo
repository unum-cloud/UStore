"""Collections of pages: ordered trees of elements, edited in place by many authors and read back as markup.

A page is an ordered tree of elements addressed by identity, where every element carries a key, a
tag, attributes and text, and the order of siblings is the content. Edits are positional and small,
one verb each: import whole pages, insert elements after a sibling, remove or move a subtree, splice
bytes inside a text run, or set or remove an attribute. The page key doubles as the root's key.

File: mojo/ustore/pages.mojo
Author: Ash Vardanian
Date: November 8, 2026
"""

from std.ffi import c_int, c_size_t
from std.memory import stack_allocation

from ustore.blobs import KeyRuns, MeasureArguments, Measures, Pulled, PulledWritten, SampleArguments, Values, ValuesWritten, _measure
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
from ustore.docs import FilteredScanArguments, _filtered_cursor, _filtered_scan
from ustore.library import Library
from ustore.types import (
    Address,
    Answer,
    Arena,
    Arguments,
    BytesView,
    Completion,
    Deferred,
    Error,
    Executor,
    NULL_ADDRESS,
    Outcome,
    Progress,
    StringsView,
    StringsViewArguments,
    Unwritten,
    Verb,
    _anchor,
    _block,
    _Crossing,
    _in_arena,
    _readable,
    _slot,
)

# region Vocabulary


@fieldwise_init
struct PageEncoding(Equatable, ImplicitlyCopyable, TrivialRegisterPassable):
    """The encoding a page or a fragment is imported from or rendered into, `ustore_page_encoding_t`."""

    var code: c_int
    """The raw `ustore_page_encoding_t` value the ABI exchanges."""
    comptime HTML = Self(0)
    """HTML, not yet imported or rendered, answering `UNSUPPORTED`."""
    comptime XML = Self(1)
    """XML, with every element and attribute kept as written."""
    comptime MARKDOWN = Self(2)
    """Markdown, not yet imported or rendered, answering `UNSUPPORTED`."""
    comptime TEXT = Self(3)
    """The text of every run in document order, with structure dropped."""
    comptime JSON = Self(4)
    """The element tree as JSON, not yet imported or rendered, answering `UNSUPPORTED`."""


@fieldwise_init
struct EffectiveKeysWritten(Defaultable, ImplicitlyCopyable):
    """The keys an insert gave its elements, at one stable address."""

    var keys: Address

    def __init__(out self):
        """Nothing written, which is what a failed call leaves."""
        self.keys = NULL_ADDRESS


@fieldwise_init
struct EffectiveKeys[origin: MutOrigin](Answer, Sized):
    """The key every inserted element ended up with, minted or given, read in place from the arena an
    insert borrows.

    Parameters:
        origin: The arena the keys live in, which this result borrows.
    """

    comptime Written = EffectiveKeysWritten
    var arena: Pointer[Arena, Self.origin]
    """The arena holding the keys."""
    var generation: Int
    """The arena's call the keys came from."""
    var written: EffectiveKeysWritten
    """The views the engine wrote."""
    var count: Int
    """Insertions the call carried."""

    def __init__(out self, arena: Address, generation: Int, written: EffectiveKeysWritten, count: Int):
        self.arena = Pointer[Arena, Self.origin](unsafe_from_address=arena)
        self.generation = generation
        self.written = written
        self.count = count

    def __len__(self) -> Int:
        """The insertions the call carried."""
        return self.count

    def keys(ref self) raises Error -> Span[Key, origin_of(self)]:
        """The key of every inserted element."""
        _readable(self.arena, self.generation, "EffectiveKeys.keys")
        return _in_arena[Key, origin_of(self)](self.written.keys, self.count)


# endregion Vocabulary

# region Configs


struct PagesInsertConfig[
    elements_origin: ImmOrigin = ImmUntrackedOrigin, parents_origin: ImmOrigin = ImmUntrackedOrigin, anchors_origin: ImmOrigin = ImmUntrackedOrigin
](ImplicitlyCopyable):
    """Where inserted fragments land, the keys they take, and how they read.

    Parameters:
        elements_origin: The borrowed element keys.
        parents_origin: The borrowed parents.
        anchors_origin: The borrowed anchors.
    """

    var elements: KeysView[Self.elements_origin]
    """Keys to give the inserted elements; nil or zeroed mints one."""
    var parents: KeysView[Self.parents_origin]
    """Parents the elements land under; nil stands for the root."""
    var anchors: KeysView[Self.anchors_origin]
    """Siblings after which the elements land; nil stands for the front."""
    var encoding: PageEncoding
    """Encoding of the fragments."""
    var durability: Durability
    """How durable the edits are once the call finishes at the head."""

    def __init__(
        out self,
        *,
        elements: KeysView[Self.elements_origin] = KeysView[Self.elements_origin](),
        parents: KeysView[Self.parents_origin] = KeysView[Self.parents_origin](),
        anchors: KeysView[Self.anchors_origin] = KeysView[Self.anchors_origin](),
        encoding: PageEncoding = PageEncoding.XML,
        durability: Durability = Durability.BUFFERED,
    ):
        """Names only the knobs that differ from the defaults."""
        self.elements = elements
        self.parents = parents
        self.anchors = anchors
        self.encoding = encoding
        self.durability = durability


struct PagesReadConfig[elements_origin: ImmOrigin = ImmUntrackedOrigin](ImplicitlyCopyable):
    """Which subtrees a read renders, and into what.

    Parameters:
        elements_origin: The borrowed element keys.
    """

    var elements: KeysView[Self.elements_origin]
    """Elements to render with their subtrees; zeroed or nil renders whole pages."""
    var outputs_encoding: PageEncoding
    """Encoding the renderings are converted into."""
    var values: Bool
    """Whether the renderings come too, or only presences and lengths."""

    def __init__(
        out self,
        *,
        elements: KeysView[Self.elements_origin] = KeysView[Self.elements_origin](),
        outputs_encoding: PageEncoding = PageEncoding.XML,
        values: Bool = True,
    ):
        """Names only the knobs that differ from the defaults."""
        self.elements = elements
        self.outputs_encoding = outputs_encoding
        self.values = values


struct PagesScanConfig[end_origin: ImmOrigin = ImmUntrackedOrigin, filter_origin: ImmOrigin = ImmUntrackedOrigin](ImplicitlyCopyable):
    """The bounds, order and filter of a scan over page keys.

    Parameters:
        end_origin: The borrowed end keys.
        filter_origin: The borrowed filter.
    """

    var end_keys: KeysView[Self.end_origin]
    """Key every scan stops before, exclusive; zeroed bounds nothing."""
    var direction: ScanDirection
    """Ascending or descending from the start keys."""
    var mode: ScanMode
    """In what order the page keys come."""
    var filter: BytesView[Self.filter_origin]
    """A path in StringZilla's XPath subset every listed page holds a node of; zeroed lists every page."""
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


struct PagesCursorConfig[filter_origin: ImmOrigin = ImmUntrackedOrigin](ImplicitlyCopyable):
    """The bound, order and filter of a walk over one collection's pages, the filter copied when it opens.

    Parameters:
        filter_origin: The borrowed filter.
    """

    var end_key: Optional[Key]
    """Key the walk stops before, exclusive; absent bounds nothing."""
    var direction: ScanDirection
    """Ascending or descending from the start key."""
    var filter: BytesView[Self.filter_origin]
    """A path in StringZilla's XPath subset every page the walk hands out holds a node of; zeroed walks every page."""
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
struct PagesImportArguments(Arguments):
    """`ustore_pages_import_t`."""

    comptime Written = Unwritten
    comptime symbol: StaticString = "ustore_pages_import"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var transaction: Address
    var count: c_size_t
    var collections: StridedArguments
    var keys: StridedArguments
    var values: StringsViewArguments
    var encoding: PageEncoding
    var durability: Durability
    var watch: c_int
    var progress: Progress

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[Unwritten, MutUntrackedOrigin], arena: Address):
        self.database = scope.database
        self.executor = scope.executor
        self.outcome = outcome
        self.completion = completion
        self.transaction = scope.transaction
        self.watch = scope.watch.code

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().pages_import


@fieldwise_init
struct PagesInsertArguments(Arguments):
    """`ustore_pages_insert_t`."""

    comptime Written = EffectiveKeysWritten
    comptime symbol: StaticString = "ustore_pages_insert"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var transaction: Address
    var count: c_size_t
    var collections: StridedArguments
    var keys: StridedArguments
    var elements: StridedArguments
    var parents: StridedArguments
    var anchors: StridedArguments
    var values: StringsViewArguments
    var encoding: PageEncoding
    var effective_keys: Optional[Pointer[Address, MutUntrackedOrigin]]
    var arena: Address
    var durability: Durability
    var watch: c_int

    def bind(mut self, scope: Scope, completion: Completion, outcome: Pointer[Outcome, MutUntrackedOrigin], written: Pointer[EffectiveKeysWritten, MutUntrackedOrigin], arena: Address):
        self.database = scope.database
        self.executor = scope.executor
        self.outcome = outcome
        self.completion = completion
        self.transaction = scope.transaction
        self.effective_keys = Pointer(to=written[].keys)
        self.arena = arena
        self.watch = scope.watch.code

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        return library.symbols().pages_insert


@fieldwise_init
struct PagesRemoveArguments(Arguments):
    """`ustore_pages_remove_t`."""

    comptime Written = Unwritten
    comptime symbol: StaticString = "ustore_pages_remove"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var transaction: Address
    var count: c_size_t
    var collections: StridedArguments
    var keys: StridedArguments
    var end_keys: StridedArguments
    var elements: StridedArguments
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
        return library.symbols().pages_remove


@fieldwise_init
struct PagesMoveArguments(Arguments):
    """`ustore_pages_move_t`."""

    comptime Written = Unwritten
    comptime symbol: StaticString = "ustore_pages_move"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var transaction: Address
    var count: c_size_t
    var collections: StridedArguments
    var keys: StridedArguments
    var elements: StridedArguments
    var parents: StridedArguments
    var anchors: StridedArguments
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
        return library.symbols().pages_move


@fieldwise_init
struct PagesSpliceArguments(Arguments):
    """`ustore_pages_splice_t`."""

    comptime Written = Unwritten
    comptime symbol: StaticString = "ustore_pages_splice"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var transaction: Address
    var count: c_size_t
    var collections: StridedArguments
    var keys: StridedArguments
    var elements: StridedArguments
    var text_starts: StridedArguments
    var text_lengths: StridedArguments
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
        return library.symbols().pages_splice


@fieldwise_init
struct PagesSetAttributesArguments(Arguments):
    """`ustore_pages_set_attributes_t`."""

    comptime Written = Unwritten
    comptime symbol: StaticString = "ustore_pages_set_attributes"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var transaction: Address
    var count: c_size_t
    var collections: StridedArguments
    var keys: StridedArguments
    var elements: StridedArguments
    var fields: StringsViewArguments
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
        return library.symbols().pages_set_attributes


@fieldwise_init
struct PagesRemoveAttributesArguments(Arguments):
    """`ustore_pages_remove_attributes_t`."""

    comptime Written = Unwritten
    comptime symbol: StaticString = "ustore_pages_remove_attributes"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var transaction: Address
    var count: c_size_t
    var collections: StridedArguments
    var keys: StridedArguments
    var elements: StridedArguments
    var fields: StringsViewArguments
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
        return library.symbols().pages_remove_attributes


@fieldwise_init
struct PagesReadArguments(Arguments):
    """`ustore_pages_read_t`."""

    comptime Written = ValuesWritten
    comptime symbol: StaticString = "ustore_pages_read"
    var database: Address
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var completion: Completion
    var view: Address
    var count: c_size_t
    var collections: StridedArguments
    var keys: StridedArguments
    var elements: StridedArguments
    var presences: Optional[Pointer[Address, MutUntrackedOrigin]]
    var lengths: Optional[Pointer[Address, MutUntrackedOrigin]]
    var values: Optional[Pointer[StringsViewArguments, MutUntrackedOrigin]]
    var arena: Address
    var outputs_encoding: PageEncoding
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
        return library.symbols().pages_read


@fieldwise_init
struct PagesCursorNextArguments(Arguments):
    """`ustore_pages_cursor_next_t`."""

    comptime Written = PulledWritten
    comptime symbol: StaticString = "ustore_pages_cursor_next"
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
    var outputs_encoding: PageEncoding
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
        return library.symbols().pages_cursor_next


# endregion Arguments

# region Cursors


struct PagesCursor(Movable):
    """A walk over the page keys of one collection, every pull resuming past the last key the pull before it
    handed out, freed when it goes out of scope. It holds its database open, and the transaction or
    snapshot it reads through."""

    var library: Library
    """The loaded engine."""
    var database: Address
    """The `ustore_database_t` it walks."""
    var address: Address
    """The `ustore_pages_cursor_t`."""

    def __init__(out self, *, library: Library, database: Address, address: Address):
        """Adopts a cursor the engine just opened."""
        self.library = library
        self.database = database
        self.address = address

    def __deinit__(deinit self):
        """Frees the cursor and the holds it kept. Calls `ustore_pages_cursor_free`."""
        self.library.symbols().pages_cursor_free(self.address)
        _ = self.library^

    def next(
        self,
        executor: Executor,
        mut arena: Arena,
        wanted: Int,
        values: Bool = True,
        outputs_encoding: PageEncoding = PageEncoding.XML,
        progress: Progress = Progress(),
    ) raises Error -> Pulled[origin_of(arena)]:
        """Pulls the next page of up to `wanted` keys, with the whole pages rendered when asked.
        Calls `ustore_pages_cursor_next`.

        Raises:
            `Error` with `CANCELLED` when `progress` stops the pull, which leaves the cursor where it was.
        """
        return _block(
            Scope(self.library, self.database, executor.address, NULL_ADDRESS, NULL_ADDRESS, NULL_ADDRESS, Watch.KEYS),
            _Crossing[PagesCursorNextArguments, Pulled[origin_of(arena)], origin_of(arena), ImmUntrackedOrigin](
                PagesCursorNextArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, self.address, None, None, None, NULL_ADDRESS,
                    c_size_t(wanted), outputs_encoding, progress, values,
                ),
                Pointer(to=arena),
                0,
            ),
        )


# endregion Cursors

# region Verbs


trait PagesVerbs:
    """The pages verbs every blocking receiver offers, written once over the receiver's scope."""

    def _scope(self) -> Scope:
        """What the receiver addresses and the executor it runs on."""
        ...

    def pages_import[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, values_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        values: StringsView[values_origin],
        encoding: PageEncoding = PageEncoding.XML,
        durability: Durability = Durability.BUFFERED,
        progress: Progress = Progress(),
    ) raises Error:
        """Replaces whole pages with parsed markup. Calls `ustore_pages_import`.

        Raises:
            `Error` with `MALFORMED` for markup that does not parse.
        """
        _ = _block(
            self._scope(),
            _Crossing[PagesImportArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[values_origin]()))](
                PagesImportArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), keys.tasks(), values.tasks())), collections.arguments(), keys.arguments(),
                    values.arguments(), encoding, durability, 0, progress,
                ),
                None,
                0,
            ),
        )

    def pages_insert[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, values_origin: ImmOrigin, elements_origin: ImmOrigin, parents_origin: ImmOrigin, anchors_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        values: StringsView[values_origin],
        mut arena: Arena,
        config: PagesInsertConfig[elements_origin, parents_origin, anchors_origin] = PagesInsertConfig(),
    ) raises Error -> EffectiveKeys[origin_of(arena)]:
        """Parses fragments into elements under parents after anchors, answering the key every element ended
        up with. Calls `ustore_pages_insert`."""
        var count = _tasks(collections.tasks(), keys.tasks(), values.tasks(), config.elements.tasks(), config.parents.tasks(), config.anchors.tasks())
        return _block(
            self._scope(),
            _Crossing[PagesInsertArguments, EffectiveKeys[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[values_origin](), _anchor[elements_origin](), _anchor[parents_origin](), _anchor[anchors_origin]()))](
                PagesInsertArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    keys.arguments(), config.elements.arguments(), config.parents.arguments(), config.anchors.arguments(),
                    values.arguments(), config.encoding, None, NULL_ADDRESS, config.durability, 0,
                ),
                Pointer(to=arena),
                count,
            ),
        )

    def pages_remove[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, elements_origin: ImmOrigin, end_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        elements: KeysView[elements_origin] = KeysView(),
        end_keys: KeysView[end_origin] = KeysView(),
        durability: Durability = Durability.BUFFERED,
    ) raises Error:
        """Removes elements with their subtrees, or every page of a range per task when `end_keys` are given.
        Calls `ustore_pages_remove`."""
        _ = _block(
            self._scope(),
            _Crossing[PagesRemoveArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[elements_origin](), _anchor[end_origin]()))](
                PagesRemoveArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), keys.tasks(), elements.tasks(), end_keys.tasks())), collections.arguments(),
                    keys.arguments(), end_keys.arguments(), elements.arguments(), durability, 0,
                ),
                None,
                0,
            ),
        )

    def pages_move[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, elements_origin: ImmOrigin, parents_origin: ImmOrigin, anchors_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        elements: KeysView[elements_origin],
        parents: KeysView[parents_origin],
        anchors: KeysView[anchors_origin],
        durability: Durability = Durability.BUFFERED,
    ) raises Error:
        """Moves elements with their subtrees under parents after anchors. Calls `ustore_pages_move`."""
        _ = _block(
            self._scope(),
            _Crossing[PagesMoveArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[elements_origin](), _anchor[parents_origin](), _anchor[anchors_origin]()))](
                PagesMoveArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), keys.tasks(), elements.tasks(), parents.tasks(), anchors.tasks())), collections.arguments(),
                    keys.arguments(), elements.arguments(), parents.arguments(), anchors.arguments(), durability, 0,
                ),
                None,
                0,
            ),
        )

    def pages_splice[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, elements_origin: ImmOrigin, starts_origin: ImmOrigin, lengths_origin: ImmOrigin, values_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        elements: KeysView[elements_origin],
        text_starts: LengthsView[starts_origin],
        text_lengths: LengthsView[lengths_origin],
        values: StringsView[values_origin],
        durability: Durability = Durability.BUFFERED,
    ) raises Error:
        """Replaces runs of text inside elements with new bytes. Calls `ustore_pages_splice`."""
        _ = _block(
            self._scope(),
            _Crossing[PagesSpliceArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[elements_origin](), _anchor[starts_origin](), _anchor[lengths_origin](), _anchor[values_origin]()))](
                PagesSpliceArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), keys.tasks(), elements.tasks(), text_starts.tasks(), text_lengths.tasks(), values.tasks())), collections.arguments(),
                    keys.arguments(), elements.arguments(), text_starts.arguments(), text_lengths.arguments(), values.arguments(),
                    durability, 0,
                ),
                None,
                0,
            ),
        )

    def pages_set_attributes[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, elements_origin: ImmOrigin, fields_origin: ImmOrigin, values_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        elements: KeysView[elements_origin],
        fields: StringsView[fields_origin],
        values: StringsView[values_origin],
        durability: Durability = Durability.BUFFERED,
    ) raises Error:
        """Sets one attribute of an element per task. Calls `ustore_pages_set_attributes`."""
        _ = _block(
            self._scope(),
            _Crossing[PagesSetAttributesArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[elements_origin](), _anchor[fields_origin](), _anchor[values_origin]()))](
                PagesSetAttributesArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), keys.tasks(), elements.tasks(), fields.tasks(), values.tasks())), collections.arguments(),
                    keys.arguments(), elements.arguments(), fields.arguments(), values.arguments(), durability, 0,
                ),
                None,
                0,
            ),
        )

    def pages_remove_attributes[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, elements_origin: ImmOrigin, fields_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        elements: KeysView[elements_origin],
        fields: StringsView[fields_origin],
        durability: Durability = Durability.BUFFERED,
    ) raises Error:
        """Removes one attribute of an element per task. Calls `ustore_pages_remove_attributes`."""
        _ = _block(
            self._scope(),
            _Crossing[PagesRemoveAttributesArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[elements_origin](), _anchor[fields_origin]()))](
                PagesRemoveAttributesArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), keys.tasks(), elements.tasks(), fields.tasks())), collections.arguments(),
                    keys.arguments(), elements.arguments(), fields.arguments(), durability, 0,
                ),
                None,
                0,
            ),
        )

    def pages_read[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, elements_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        mut arena: Arena,
        config: PagesReadConfig[elements_origin] = PagesReadConfig(),
    ) raises Error -> Values[origin_of(arena)]:
        """Renders whole pages, or chosen elements with their subtrees, as markup. Calls `ustore_pages_read`."""
        var count = _tasks(collections.tasks(), keys.tasks(), config.elements.tasks())
        return _block(
            self._scope(),
            _Crossing[PagesReadArguments, Values[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[elements_origin]()))](
                PagesReadArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    keys.arguments(), config.elements.arguments(), None, None, None, NULL_ADDRESS, config.outputs_encoding, config.values,
                ),
                Pointer(to=arena),
                count,
            ),
        )

    def pages_scan[
        collections_origin: ImmOrigin, start_origin: ImmOrigin, end_origin: ImmOrigin, filter_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        start_keys: KeysView[start_origin],
        wanted: Int,
        mut arena: Arena,
        config: PagesScanConfig[end_origin, filter_origin] = PagesScanConfig(),
    ) raises Error -> KeyRuns[origin_of(arena)]:
        """Lists up to `wanted` page keys from a start key in every collection. Calls `ustore_pages_scan`."""
        var count = _tasks(collections.tasks(), start_keys.tasks(), config.end_keys.tasks())
        return _block(
            self._scope(),
            _Crossing[FilteredScanArguments["ustore_pages_scan"], KeyRuns[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[start_origin](), _anchor[end_origin](), _anchor[filter_origin]()))](
                _filtered_scan["ustore_pages_scan"](
                    count, collections.arguments(), start_keys.arguments(), config.end_keys.arguments(), config.direction,
                    config.filter.arguments(), config.predicate, wanted, config.mode, config.progress,
                ),
                Pointer(to=arena),
                count,
            ),
        )

    def pages_cursor[
        filter_origin: ImmOrigin, //
    ](self, collection: CollectionID, start_key: Key, config: PagesCursorConfig[filter_origin] = PagesCursorConfig()) raises Error -> PagesCursor:
        """Opens a walk over the page keys of one collection, compiling its filter once and reading nothing
        until its first pull. Calls `ustore_pages_cursor_init`."""
        var scope = self._scope()
        var address = _filtered_cursor(
            scope, collection, start_key, config.end_key, config.direction, config.filter.arguments(), config.predicate,
            scope.library.symbols().pages_cursor_init, "ustore_pages_cursor_init",
        )
        return PagesCursor(library=scope.library, database=scope.database, address=address)

    def pages_sample[
        collections_origin: ImmOrigin, //
    ](self, collections: CollectionsView[collections_origin], wanted: Int, mut arena: Arena) raises Error -> KeyRuns[origin_of(arena)]:
        """Draws up to `wanted` page keys uniformly at random from every collection. Calls `ustore_pages_sample`."""
        var count = len(collections)
        return _block(
            self._scope(),
            _Crossing[SampleArguments["ustore_pages_sample"], KeyRuns[origin_of(arena)], origin_of(arena), collections_origin](
                SampleArguments["ustore_pages_sample"](
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.BLOCKING, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    None, None, NULL_ADDRESS, c_size_t(wanted),
                ),
                Pointer(to=arena),
                count,
            ),
        )

    def pages_measure[
        collections_origin: ImmOrigin, start_origin: ImmOrigin, end_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        start_keys: KeysView[start_origin],
        end_keys: KeysView[end_origin],
        mut arena: Arena,
        direction: ScanDirection = ScanDirection.ASCENDING,
    ) raises Error -> Measures[origin_of(arena)]:
        """Estimates the pages and the space inside a batch of key ranges. Calls `ustore_pages_measure`."""
        var count = _tasks(collections.tasks(), start_keys.tasks(), end_keys.tasks())
        return _block(
            self._scope(),
            _Crossing[MeasureArguments["ustore_pages_measure"], Measures[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[start_origin](), _anchor[end_origin]()))](
                _measure["ustore_pages_measure"](count, collections.arguments(), start_keys.arguments(), end_keys.arguments(), direction),
                Pointer(to=arena),
                count,
            ),
        )


trait PagesDeferredVerbs:
    """The pages verbs every deferred receiver offers, each answering a `Deferred` at once; the arguments
    are those of `PagesVerbs`."""

    def _scope(self) -> Scope:
        """What the receiver addresses and the executor it submits to."""
        ...

    def pages_import[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, values_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        values: StringsView[values_origin],
        encoding: PageEncoding = PageEncoding.XML,
        durability: Durability = Durability.BUFFERED,
        progress: Progress = Progress(),
    ) -> Deferred[
        _Crossing[PagesImportArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[values_origin]()))]
    ]:
        """Submits `PagesVerbs.pages_import`. Calls `ustore_pages_import`."""
        return {
            self._scope(),
            _Crossing[PagesImportArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[values_origin]()))](
                PagesImportArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), keys.tasks(), values.tasks())), collections.arguments(), keys.arguments(),
                    values.arguments(), encoding, durability, 0, progress,
                ),
                None,
                0,
            ),
        }

    def pages_insert[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, values_origin: ImmOrigin, elements_origin: ImmOrigin, parents_origin: ImmOrigin, anchors_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        values: StringsView[values_origin],
        mut arena: Arena,
        config: PagesInsertConfig[elements_origin, parents_origin, anchors_origin] = PagesInsertConfig(),
    ) -> Deferred[
        _Crossing[PagesInsertArguments, EffectiveKeys[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[values_origin](), _anchor[elements_origin](), _anchor[parents_origin](), _anchor[anchors_origin]()))]
    ]:
        """Submits `PagesVerbs.pages_insert`. Calls `ustore_pages_insert`."""
        var count = _tasks(collections.tasks(), keys.tasks(), values.tasks(), config.elements.tasks(), config.parents.tasks(), config.anchors.tasks())
        return {
            self._scope(),
            _Crossing[PagesInsertArguments, EffectiveKeys[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[values_origin](), _anchor[elements_origin](), _anchor[parents_origin](), _anchor[anchors_origin]()))](
                PagesInsertArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    keys.arguments(), config.elements.arguments(), config.parents.arguments(), config.anchors.arguments(),
                    values.arguments(), config.encoding, None, NULL_ADDRESS, config.durability, 0,
                ),
                Pointer(to=arena),
                count,
            ),
        }

    def pages_remove[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, elements_origin: ImmOrigin, end_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        elements: KeysView[elements_origin] = KeysView(),
        end_keys: KeysView[end_origin] = KeysView(),
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[
        _Crossing[PagesRemoveArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[elements_origin](), _anchor[end_origin]()))]
    ]:
        """Submits `PagesVerbs.pages_remove`. Calls `ustore_pages_remove`."""
        return {
            self._scope(),
            _Crossing[PagesRemoveArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[elements_origin](), _anchor[end_origin]()))](
                PagesRemoveArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), keys.tasks(), elements.tasks(), end_keys.tasks())), collections.arguments(),
                    keys.arguments(), end_keys.arguments(), elements.arguments(), durability, 0,
                ),
                None,
                0,
            ),
        }

    def pages_move[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, elements_origin: ImmOrigin, parents_origin: ImmOrigin, anchors_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        elements: KeysView[elements_origin],
        parents: KeysView[parents_origin],
        anchors: KeysView[anchors_origin],
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[
        _Crossing[PagesMoveArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[elements_origin](), _anchor[parents_origin](), _anchor[anchors_origin]()))]
    ]:
        """Submits `PagesVerbs.pages_move`. Calls `ustore_pages_move`."""
        return {
            self._scope(),
            _Crossing[PagesMoveArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[elements_origin](), _anchor[parents_origin](), _anchor[anchors_origin]()))](
                PagesMoveArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), keys.tasks(), elements.tasks(), parents.tasks(), anchors.tasks())), collections.arguments(),
                    keys.arguments(), elements.arguments(), parents.arguments(), anchors.arguments(), durability, 0,
                ),
                None,
                0,
            ),
        }

    def pages_splice[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, elements_origin: ImmOrigin, starts_origin: ImmOrigin, lengths_origin: ImmOrigin, values_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        elements: KeysView[elements_origin],
        text_starts: LengthsView[starts_origin],
        text_lengths: LengthsView[lengths_origin],
        values: StringsView[values_origin],
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[
        _Crossing[PagesSpliceArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[elements_origin](), _anchor[starts_origin](), _anchor[lengths_origin](), _anchor[values_origin]()))]
    ]:
        """Submits `PagesVerbs.pages_splice`. Calls `ustore_pages_splice`."""
        return {
            self._scope(),
            _Crossing[PagesSpliceArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[elements_origin](), _anchor[starts_origin](), _anchor[lengths_origin](), _anchor[values_origin]()))](
                PagesSpliceArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), keys.tasks(), elements.tasks(), text_starts.tasks(), text_lengths.tasks(), values.tasks())), collections.arguments(),
                    keys.arguments(), elements.arguments(), text_starts.arguments(), text_lengths.arguments(), values.arguments(),
                    durability, 0,
                ),
                None,
                0,
            ),
        }

    def pages_set_attributes[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, elements_origin: ImmOrigin, fields_origin: ImmOrigin, values_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        elements: KeysView[elements_origin],
        fields: StringsView[fields_origin],
        values: StringsView[values_origin],
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[
        _Crossing[PagesSetAttributesArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[elements_origin](), _anchor[fields_origin](), _anchor[values_origin]()))]
    ]:
        """Submits `PagesVerbs.pages_set_attributes`. Calls `ustore_pages_set_attributes`."""
        return {
            self._scope(),
            _Crossing[PagesSetAttributesArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[elements_origin](), _anchor[fields_origin](), _anchor[values_origin]()))](
                PagesSetAttributesArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), keys.tasks(), elements.tasks(), fields.tasks(), values.tasks())), collections.arguments(),
                    keys.arguments(), elements.arguments(), fields.arguments(), values.arguments(), durability, 0,
                ),
                None,
                0,
            ),
        }

    def pages_remove_attributes[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, elements_origin: ImmOrigin, fields_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        elements: KeysView[elements_origin],
        fields: StringsView[fields_origin],
        durability: Durability = Durability.BUFFERED,
    ) -> Deferred[
        _Crossing[PagesRemoveAttributesArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[elements_origin](), _anchor[fields_origin]()))]
    ]:
        """Submits `PagesVerbs.pages_remove_attributes`. Calls `ustore_pages_remove_attributes`."""
        return {
            self._scope(),
            _Crossing[PagesRemoveAttributesArguments, Unwritten, MutUntrackedOrigin, ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[elements_origin](), _anchor[fields_origin]()))](
                PagesRemoveAttributesArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS,
                    c_size_t(_tasks(collections.tasks(), keys.tasks(), elements.tasks(), fields.tasks())), collections.arguments(),
                    keys.arguments(), elements.arguments(), fields.arguments(), durability, 0,
                ),
                None,
                0,
            ),
        }

    def pages_read[
        collections_origin: ImmOrigin, keys_origin: ImmOrigin, elements_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        keys: KeysView[keys_origin],
        mut arena: Arena,
        config: PagesReadConfig[elements_origin] = PagesReadConfig(),
    ) -> Deferred[
        _Crossing[PagesReadArguments, Values[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[elements_origin]()))]
    ]:
        """Submits `PagesVerbs.pages_read`. Calls `ustore_pages_read`."""
        var count = _tasks(collections.tasks(), keys.tasks(), config.elements.tasks())
        return {
            self._scope(),
            _Crossing[PagesReadArguments, Values[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[keys_origin](), _anchor[elements_origin]()))](
                PagesReadArguments(
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    keys.arguments(), config.elements.arguments(), None, None, None, NULL_ADDRESS, config.outputs_encoding, config.values,
                ),
                Pointer(to=arena),
                count,
            ),
        }

    def pages_scan[
        collections_origin: ImmOrigin, start_origin: ImmOrigin, end_origin: ImmOrigin, filter_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        start_keys: KeysView[start_origin],
        wanted: Int,
        mut arena: Arena,
        config: PagesScanConfig[end_origin, filter_origin] = PagesScanConfig(),
    ) -> Deferred[
        _Crossing[FilteredScanArguments["ustore_pages_scan"], KeyRuns[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[start_origin](), _anchor[end_origin](), _anchor[filter_origin]()))]
    ]:
        """Submits `PagesVerbs.pages_scan`. Calls `ustore_pages_scan`."""
        var count = _tasks(collections.tasks(), start_keys.tasks(), config.end_keys.tasks())
        return {
            self._scope(),
            _Crossing[FilteredScanArguments["ustore_pages_scan"], KeyRuns[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[start_origin](), _anchor[end_origin](), _anchor[filter_origin]()))](
                _filtered_scan["ustore_pages_scan"](
                    count, collections.arguments(), start_keys.arguments(), config.end_keys.arguments(), config.direction,
                    config.filter.arguments(), config.predicate, wanted, config.mode, config.progress,
                ),
                Pointer(to=arena),
                count,
            ),
        }

    def pages_sample[
        collections_origin: ImmOrigin, //
    ](self, collections: CollectionsView[collections_origin], wanted: Int, mut arena: Arena) -> Deferred[
        _Crossing[SampleArguments["ustore_pages_sample"], KeyRuns[origin_of(arena)], origin_of(arena), collections_origin]
    ]:
        """Submits `PagesVerbs.pages_sample`. Calls `ustore_pages_sample`."""
        var count = len(collections)
        return {
            self._scope(),
            _Crossing[SampleArguments["ustore_pages_sample"], KeyRuns[origin_of(arena)], origin_of(arena), collections_origin](
                SampleArguments["ustore_pages_sample"](
                    NULL_ADDRESS, NULL_ADDRESS, None, Completion.DEFERRED, NULL_ADDRESS, c_size_t(count), collections.arguments(),
                    None, None, NULL_ADDRESS, c_size_t(wanted),
                ),
                Pointer(to=arena),
                count,
            ),
        }

    def pages_measure[
        collections_origin: ImmOrigin, start_origin: ImmOrigin, end_origin: ImmOrigin, //
    ](
        self,
        collections: CollectionsView[collections_origin],
        start_keys: KeysView[start_origin],
        end_keys: KeysView[end_origin],
        mut arena: Arena,
        direction: ScanDirection = ScanDirection.ASCENDING,
    ) -> Deferred[
        _Crossing[MeasureArguments["ustore_pages_measure"], Measures[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[start_origin](), _anchor[end_origin]()))]
    ]:
        """Submits `PagesVerbs.pages_measure`. Calls `ustore_pages_measure`."""
        var count = _tasks(collections.tasks(), start_keys.tasks(), end_keys.tasks())
        return {
            self._scope(),
            _Crossing[MeasureArguments["ustore_pages_measure"], Measures[origin_of(arena)], origin_of(arena), ImmOrigin(origin_of(_anchor[collections_origin](), _anchor[start_origin](), _anchor[end_origin]()))](
                _measure["ustore_pages_measure"](count, collections.arguments(), start_keys.arguments(), end_keys.arguments(), direction),
                Pointer(to=arena),
                count,
            ),
        }


# endregion Verbs
