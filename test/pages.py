"""Pages over the C ABI: imports, edits and renders on every engine that serves them.

The Python binding exposes no pages yet, so this suite calls `c/ustore/pages.h` through `ctypes` on the shared
library a CMake build leaves in `build*/build/lib`, or the one `USTORE_SHARED_LIBRARY` names, and skips without one.
It opens its own databases there, one per engine the extension carries, so one random edit stream runs on all of them
at once and every engine must render every page as the others do after every call.
"""

import ctypes
import os
import random
from ctypes import POINTER, Structure, Union, byref, c_char_p, c_int, c_uint32, c_uint64, c_void_p
from pathlib import Path
from uuid import UUID

import pytest
from capabilities import ENGINES

# region Library


def find_library() -> Path | None:
    """The shared library `USTORE_SHARED_LIBRARY` names, else the newest one a build next to the sources left."""
    named = os.environ.get("USTORE_SHARED_LIBRARY")
    if named:
        return Path(named)
    root = Path(__file__).resolve().parent.parent
    built = sorted(root.glob("build*/build/lib/libustore_shared.*"), key=lambda path: path.stat().st_mtime)
    return built[-1] if built else None


LIBRARY_PATH = find_library()
if LIBRARY_PATH is None or not LIBRARY_PATH.exists():
    pytest.skip("no shared library to call the pages ABI through", allow_module_level=True)
LIBRARY = ctypes.CDLL(str(LIBRARY_PATH))

HTML, XML, MARKDOWN, TEXT, JSON = 0, 1, 2, 3, 4
IMPORT, INSERT, REMOVE, MOVE, SPLICE, SET_ATTRIBUTES, REMOVE_ATTRIBUTES = range(7)
U32_TAPE, U64_TAPE = 0, 1
NIL = bytes(16)
PAGES_MODALITY = 5


class Outcome(Structure):
    _fields_ = [("status", c_int), ("message", c_char_p)]


class BytesView(Structure):
    _fields_ = [("data", c_void_p), ("length", c_uint64)]


LIBRARY.ustore_status_from_name.argtypes = [BytesView]
SUCCESS, INVALID_ARGUMENT, UNSUPPORTED, MALFORMED, NOT_FOUND = (
    LIBRARY.ustore_status_from_name(BytesView(ctypes.cast(name, c_void_p), len(name)))
    for name in (b"success", b"invalid-argument", b"unsupported", b"malformed", b"not-found")
)


class StridedView(Structure):
    """Every strided run of the ABI: keys, collections and lengths."""

    _fields_ = [("data", c_void_p), ("stride", c_uint64)]


class Tape(Structure):
    _fields_ = [("data", c_void_p), ("offsets", c_void_p)]


class StringsData(Union):
    _fields_ = [("tape", Tape)]


class StringsView(Structure):
    _fields_ = [("layout", c_int), ("data", StringsData)]


HEAD = [("database", c_void_p), ("executor", c_void_p), ("outcome", POINTER(Outcome)), ("completion", c_int)]
EDIT_HEAD = [*HEAD, ("transaction", c_void_p), ("count", c_uint64), ("collections", StridedView), ("keys", StridedView)]
EDIT_TAIL = [("durability", c_int), ("watch", c_int)]


def arguments(name: str, fields: list) -> type:
    return type(name, (Structure,), {"_fields_": fields})


Import = arguments(
    "Import",
    [
        *EDIT_HEAD,
        ("values", StringsView),
        ("encoding", c_int),
        *EDIT_TAIL,
        ("progress", c_void_p),
        ("progress_state", c_void_p),
    ],
)
Insert = arguments(
    "Insert",
    [
        *EDIT_HEAD,
        ("elements", StridedView),
        ("parents", StridedView),
        ("anchors", StridedView),
        ("values", StringsView),
        ("encoding", c_int),
        ("effective_keys", POINTER(c_void_p)),
        ("arena", c_void_p),
        *EDIT_TAIL,
    ],
)
Remove = arguments("Remove", [*EDIT_HEAD, ("elements", StridedView), *EDIT_TAIL])
Move = arguments(
    "Move", [*EDIT_HEAD, ("elements", StridedView), ("parents", StridedView), ("anchors", StridedView), *EDIT_TAIL]
)
Splice = arguments(
    "Splice",
    [
        *EDIT_HEAD,
        ("elements", StridedView),
        ("text_starts", StridedView),
        ("text_lengths", StridedView),
        ("values", StringsView),
        *EDIT_TAIL,
    ],
)
SetAttributes = arguments(
    "SetAttributes",
    [*EDIT_HEAD, ("elements", StridedView), ("fields", StringsView), ("values", StringsView), *EDIT_TAIL],
)
RemoveAttributes = arguments(
    "RemoveAttributes", [*EDIT_HEAD, ("elements", StridedView), ("fields", StringsView), *EDIT_TAIL]
)
Read = arguments(
    "Read",
    [
        *HEAD,
        ("view", c_void_p),
        ("count", c_uint64),
        ("collections", StridedView),
        ("keys", StridedView),
        ("elements", StridedView),
        ("presences", POINTER(c_void_p)),
        ("lengths", POINTER(c_void_p)),
        ("values", POINTER(StringsView)),
        ("arena", c_void_p),
        ("outputs_encoding", c_int),
    ],
)
InitRam = arguments("InitRam", [("database", POINTER(c_void_p)), *HEAD[1:], ("isolation", c_int)])
OpenLevelDB = arguments("OpenLevelDB", [("database", POINTER(c_void_p)), *HEAD[1:], ("directory", BytesView)])
OpenRocksDB = arguments(
    "OpenRocksDB",
    [
        ("database", POINTER(c_void_p)),
        *HEAD[1:],
        ("directory", BytesView),
        ("data_directories", c_void_p),
        ("count", c_uint64),
    ],
)
CreateCollection = arguments(
    "CreateCollection", [*HEAD, ("name", BytesView), ("modality", c_int), ("collection", c_void_p)]
)
InitArena = arguments(
    "InitArena", [("arena", POINTER(c_void_p)), ("executor", c_void_p), ("outcome", POINTER(Outcome))]
)

VERBS = {
    IMPORT: ("ustore_pages_import", Import),
    INSERT: ("ustore_pages_insert", Insert),
    REMOVE: ("ustore_pages_remove", Remove),
    MOVE: ("ustore_pages_move", Move),
    SPLICE: ("ustore_pages_splice", Splice),
    SET_ATTRIBUTES: ("ustore_pages_set_attributes", SetAttributes),
    REMOVE_ATTRIBUTES: ("ustore_pages_remove_attributes", RemoveAttributes),
}

# endregion

# region Client


class Strings:
    """A u64 tape over `strings`, which keeps its buffers alive while the view is in use."""

    def __init__(self, strings: list[bytes]):
        offsets = [0]
        for string in strings:
            offsets.append(offsets[-1] + len(string))
        self.data = ctypes.create_string_buffer(b"".join(strings) or b"\0")
        self.offsets = (c_uint64 * len(offsets))(*offsets)
        self.view = StringsView(
            U64_TAPE, StringsData(Tape(ctypes.cast(self.data, c_void_p), ctypes.cast(self.offsets, c_void_p)))
        )


def keys_buffer(keys: list[bytes]):
    return ctypes.create_string_buffer(b"".join(keys) or b"\0", 16 * max(len(keys), 1))


def tape_strings(view: StringsView, count: int) -> list[bytes]:
    """The `count` strings a call wrote into an output tape."""
    if count == 0 or not view.data.tape.offsets:
        return [b""] * count
    offset_type = c_uint32 if view.layout == U32_TAPE else c_uint64
    offsets = ctypes.cast(view.data.tape.offsets, POINTER(offset_type))
    # A tape of empty strings carries no bytes, so its data may be null
    data = view.data.tape.data or 0
    lengths = [offsets[index + 1] - offsets[index] for index in range(count)]
    return [ctypes.string_at(data + offsets[index], length) if length else b"" for index, length in enumerate(lengths)]


class Pages:
    """Blocking page calls on one pages collection of a database the shared library opened."""

    def __init__(self, engine: str, directory: Path):
        self.engine = engine
        self.database = c_void_p()
        outcome = Outcome()
        if engine == "ram":
            LIBRARY.ustore_database_init_ram(
                byref(InitRam(ctypes.pointer(self.database), None, ctypes.pointer(outcome), 0, 0))
            )
        else:
            path = str(directory).encode()
            self.directory = ctypes.create_string_buffer(path)
            view = BytesView(ctypes.cast(self.directory, c_void_p), len(path))
            if engine == "rocksdb":
                LIBRARY.ustore_database_open_rocksdb(
                    byref(OpenRocksDB(ctypes.pointer(self.database), None, ctypes.pointer(outcome), 0, view, None, 0))
                )
            elif engine == "leveldb":
                LIBRARY.ustore_database_open_leveldb(
                    byref(OpenLevelDB(ctypes.pointer(self.database), None, ctypes.pointer(outcome), 0, view))
                )
            else:
                outcome.status = UNSUPPORTED
        self.opened = outcome.status == SUCCESS
        self.arena = c_void_p()
        if not self.opened:
            return
        LIBRARY.ustore_arena_init(byref(InitArena(ctypes.pointer(self.arena), None, ctypes.pointer(outcome))))
        assert outcome.status == SUCCESS
        # No collection exists until created, so every call addresses this one
        self.name, self.collection = ctypes.create_string_buffer(b"pages"), ctypes.create_string_buffer(16)
        name = BytesView(ctypes.cast(self.name, c_void_p), len(b"pages"))
        create = CreateCollection(self.database, None, ctypes.pointer(outcome), 0, name, PAGES_MODALITY)
        create.collection = ctypes.cast(self.collection, c_void_p)
        LIBRARY.ustore_collection_create(byref(create))
        self.created = outcome.status

    def close(self):
        if self.opened:
            LIBRARY.ustore_arena_free(self.arena)
            LIBRARY.ustore_database_free(self.database)

    def serves_pages(self) -> bool:
        """Whether the database opened and its engine serves pages, which every engine with blobs does but NVMe."""
        if not self.opened or self.created == UNSUPPORTED:
            return False
        assert self.created == SUCCESS, self.created
        return self.call(IMPORT, key(1), [{"value": b"<probe/>"}])[0] != UNSUPPORTED

    def call(self, verb: int, page: bytes, tasks: list[dict], encoding: int = XML) -> tuple[int, list[bytes]]:
        """Runs `tasks` of `verb` on `page` in one call, answering the status and the effective keys of insertions."""
        name, structure = VERBS[verb]
        count = len(tasks)
        outcome = Outcome()
        held = []

        def strided(column: str, size: int = 16):
            values = [task.get(column, NIL if size == 16 else 0) for task in tasks]
            buffer = keys_buffer(values) if size == 16 else (c_uint64 * max(count, 1))(*values)
            held.append(buffer)
            return StridedView(ctypes.cast(buffer, c_void_p), size)

        def strings(column: str):
            tape = Strings([task.get(column, b"") for task in tasks])
            held.append(tape)
            return tape.view

        page_buffer = keys_buffer([page])
        call = structure(database=self.database, outcome=ctypes.pointer(outcome), count=count)
        call.collections = StridedView(ctypes.cast(self.collection, c_void_p), 0)
        call.keys = StridedView(ctypes.cast(page_buffer, c_void_p), 0)
        effective = c_void_p()
        if verb in (IMPORT, INSERT):
            call.values, call.encoding = strings("value"), encoding
        if verb != IMPORT:
            call.elements = strided("element")
        if verb in (INSERT, MOVE):
            call.parents, call.anchors = strided("parent"), strided("anchor")
        if verb == INSERT:
            call.effective_keys, call.arena = ctypes.pointer(effective), self.arena
        if verb == SPLICE:
            call.text_starts, call.text_lengths = strided("start", 8), strided("length", 8)
            call.values = strings("value")
        if verb in (SET_ATTRIBUTES, REMOVE_ATTRIBUTES):
            call.fields = strings("field")
        if verb == SET_ATTRIBUTES:
            call.values = strings("value")
        getattr(LIBRARY, name)(byref(call))
        landed = []
        if verb == INSERT and outcome.status == SUCCESS:
            landed = [ctypes.string_at(effective.value + 16 * index, 16) for index in range(count)]
        return outcome.status, landed

    def read(self, page: bytes, element: bytes = NIL, encoding: int = XML) -> tuple[int, bytes | None]:
        """The rendering of `element` of `page`, `None` where it is missing."""
        outcome = Outcome()
        page_buffer, element_buffer = keys_buffer([page]), keys_buffer([element])
        presences, values = c_void_p(), StringsView()
        call = Read(
            database=self.database,
            outcome=ctypes.pointer(outcome),
            count=1,
            arena=self.arena,
            outputs_encoding=encoding,
        )
        call.collections = StridedView(ctypes.cast(self.collection, c_void_p), 0)
        call.keys = StridedView(ctypes.cast(page_buffer, c_void_p), 0)
        call.elements = StridedView(ctypes.cast(element_buffer, c_void_p), 0)
        call.presences, call.values = ctypes.pointer(presences), ctypes.pointer(values)
        LIBRARY.ustore_pages_read(byref(call))
        if outcome.status != SUCCESS:
            return outcome.status, None
        present = ctypes.string_at(presences.value, 1)[0] & 1
        return SUCCESS, tape_strings(values, 1)[0] if present else None


@pytest.fixture
def pages(tmp_path: Path, engine: str):
    opened = Pages(engine, tmp_path)
    if not opened.serves_pages():
        opened.close()
        pytest.skip(f"the shared library serves no {engine} pages")
    yield opened
    opened.close()


def key(integer: int) -> bytes:
    """The 16 bytes of the integer key `integer`, as `uuid.UUID(int=integer)` spells them."""
    return UUID(int=integer).bytes


# endregion

# region Cases

PAGE = key(500)


def test_edit_and_render(pages: Pages):
    """Every verb lands where its header says, and renders back as XML and text, whole and by subtree."""
    assert pages.call(IMPORT, PAGE, [{"value": b'<doc><p id="1">one</p></doc>'}])[0] == SUCCESS
    status, landed = pages.call(INSERT, PAGE, [{"element": key(10), "value": b"<h1>title</h1>"}])
    assert status == SUCCESS and landed == [key(10)]
    assert pages.read(PAGE) == (SUCCESS, b'<h1>title</h1><doc><p id="1">one</p></doc>')
    status, minted = pages.call(INSERT, PAGE, [{"value": b"<i>x</i>", "parent": key(10), "anchor": NIL}])
    assert status == SUCCESS and minted[0] != NIL
    assert pages.read(PAGE, key(10)) == (SUCCESS, b"<h1>title<i>x</i></h1>")
    assert pages.call(SPLICE, PAGE, [{"element": key(10), "start": 0, "length": 5, "value": b"Head"}])[0] == SUCCESS
    assert pages.call(SET_ATTRIBUTES, PAGE, [{"element": key(10), "field": b"class", "value": b"a<b"}])[0] == SUCCESS
    assert pages.read(PAGE, key(10)) == (SUCCESS, b'<h1 class="a&lt;b">Head<i>x</i></h1>')
    assert pages.call(MOVE, PAGE, [{"element": minted[0], "parent": NIL, "anchor": NIL}])[0] == SUCCESS
    assert pages.call(REMOVE_ATTRIBUTES, PAGE, [{"element": key(10), "field": b"class"}])[0] == SUCCESS
    assert pages.read(PAGE)[1] == b'<i>x</i><h1>Head</h1><doc><p id="1">one</p></doc>'
    assert pages.read(PAGE, encoding=TEXT) == (SUCCESS, b"xHeadone")
    assert pages.call(REMOVE, PAGE, [{"element": key(10)}])[0] == SUCCESS
    assert pages.read(PAGE, key(10)) == (SUCCESS, None)
    assert pages.call(REMOVE, PAGE, [{"element": NIL}])[0] == SUCCESS
    assert pages.read(PAGE) == (SUCCESS, None)


def test_refusals(pages: Pages):
    """HTML, Markdown and JSON are unsupported, broken XML is malformed, and an edit of a missing page is not found."""
    for encoding in (HTML, MARKDOWN, JSON):
        assert pages.call(IMPORT, PAGE, [{"value": b"<a/>"}], encoding=encoding)[0] == UNSUPPORTED
        assert pages.read(key(1), encoding=encoding)[0] == UNSUPPORTED
    for markup in (b"<a>", b"<a>&nbsp;</a>", b"a & b", b'<a b="<"/>'):
        assert pages.call(IMPORT, PAGE, [{"value": markup}])[0] == MALFORMED
    assert pages.call(SPLICE, key(404), [{"element": key(404), "value": b"x"}])[0] == NOT_FOUND
    assert pages.call(IMPORT, PAGE, [{"value": b"a & b"}], encoding=TEXT)[0] == SUCCESS
    assert pages.read(PAGE) == (SUCCESS, b"a &amp; b")


class Stream:
    """A random stream of page calls, aimed mostly at the elements it gave keys to."""

    def __init__(self, rng: random.Random):
        self.rng = rng
        self.given: list[bytes] = []
        self.imported = False

    def text(self, longest: int) -> bytes:
        return bytes(self.rng.choice(b"abcxyz &<>\"'") for _ in range(self.rng.randint(0, longest)))

    def any_key(self) -> bytes:
        roll = self.rng.randrange(10)
        if roll == 0 or not self.given:
            return PAGE
        if roll == 1:
            return key(77)
        return self.rng.choice(self.given)

    def escaped(self, text: bytes) -> bytes:
        return text.replace(b"&", b"&amp;").replace(b"<", b"&lt;").replace(b">", b"&gt;")

    def fragment(self) -> bytes:
        name = self.rng.choice([b"p", b"row"])
        attribute = (
            b' id="' + self.escaped(self.text(3)).replace(b'"', b"&quot;") + b'"' if self.rng.randrange(2) else b""
        )
        return b"<" + name + attribute + b">" + self.escaped(self.text(4)) + b"</" + name + b">"

    def call(self) -> tuple[int, list[dict], int]:
        roll = self.rng.randrange(100)
        if roll < 4 or not self.imported:
            self.imported = True
            body = b"".join(self.fragment() for _ in range(self.rng.randrange(4)))
            return IMPORT, [{"value": b"<doc>" + body + b"</doc>"}], XML
        tasks = []
        for _ in range(self.rng.randint(1, 3)):
            if roll < 40:
                self.given.append(key(10_000 + len(self.given)))
                tasks.append(
                    {
                        "element": self.given[-1],
                        "parent": self.rng.choice([NIL, self.any_key()]),
                        "anchor": self.rng.choice([NIL, self.any_key()]),
                        "value": self.fragment(),
                    }
                )
            elif roll < 50:
                tasks.append({"element": self.any_key()})
            elif roll < 62:
                tasks.append(
                    {
                        "element": self.any_key(),
                        "parent": self.rng.choice([NIL, self.any_key()]),
                        "anchor": self.rng.choice([NIL, self.any_key()]),
                    }
                )
            elif roll < 80:
                tasks.append(
                    {
                        "element": self.any_key(),
                        "start": self.rng.randrange(6),
                        "length": self.rng.randrange(4),
                        "value": self.text(4),
                    }
                )
            elif roll < 93:
                tasks.append(
                    {"element": self.any_key(), "field": self.rng.choice([b"id", b"x"]), "value": self.text(3)}
                )
            else:
                tasks.append({"element": self.any_key(), "field": self.rng.choice([b"id", b"x"])})
        verb = (
            INSERT
            if roll < 40
            else REMOVE
            if roll < 50
            else MOVE
            if roll < 62
            else SPLICE
            if roll < 80
            else SET_ATTRIBUTES
            if roll < 93
            else REMOVE_ATTRIBUTES
        )
        return verb, tasks, XML


def test_random_streams_agree(tmp_path: Path, seed: int):
    """One random stream applied to every engine at once renders identically on all of them after every call."""
    engines = []
    for engine in ENGINES:
        directory = tmp_path / engine
        directory.mkdir()
        opened = Pages(engine, directory)
        if not opened.serves_pages():
            opened.close()
            continue
        engines.append(opened)
    if len(engines) < 2:
        pytest.skip("fewer than two engines serve pages")
    stream = Stream(random.Random(seed))
    try:
        for _ in range(300):
            verb, tasks, encoding = stream.call()
            statuses = {opened.engine: opened.call(verb, PAGE, tasks, encoding)[0] for opened in engines}
            assert set(statuses.values()) == {SUCCESS}, statuses
            renders = {opened.engine: opened.read(PAGE) for opened in engines}
            assert len(set(renders.values())) == 1, renders
            probed = stream.any_key()
            subtrees = {opened.engine: opened.read(PAGE, probed, TEXT) for opened in engines}
            assert len(set(subtrees.values())) == 1, subtrees
    finally:
        for opened in engines:
            opened.close()


# endregion
